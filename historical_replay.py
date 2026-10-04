#!/usr/bin/env python3
"""
historical_replay.py
====================
Pumps standard L3 CSV tick data through the C++20 QuantEngine matching
engine to simulate historical market conditions at maximum speed.

Supported CSV format (same as tests/fixtures/l3_sample.csv)
------------------------------------------------------------
    timestamp_ns,event_type,order_id,side,price,quantity

    - timestamp_ns : int   — nanosecond exchange timestamp
    - event_type   : str   — ADD | CANCEL | EXECUTE | MODIFY
    - order_id     : int   — unique order identifier
    - side         : str   — B (buy) or S (sell)   [ignored for CANCEL]
    - price        : float — decimal price          [tick-converted internally]
    - quantity     : int   — lot quantity           [0 for CANCEL]

Usage
-----
    python historical_replay.py --input data.csv --output result.json

    # Pipe multiple files and limit depth snapshots:
    python historical_replay.py \\
        --input data/*.csv \\
        --output result.json \\
        --tick-size 0.01 \\
        --snapshot-interval 10000 \\
        --symbol BTCUSDT

Output
------
JSON file with:
    {
        "symbol": "BTCUSDT",
        "tick_size": 0.01,
        "events_processed": 1000000,
        "trades_generated": 5432,
        "duration_sec": 0.712,
        "throughput_mevents_sec": 14.04,
        "final_canonical_hash": "0x3f9a4b2c1d8e7f60",
        "final_best_bid": "49999.99000000",
        "final_best_ask": "50000.01000000",
        "snapshots": [...]    // periodic depth snapshots
    }

Exit Codes
----------
    0 — success
    1 — input parse error or engine invariant violation
    2 — missing dependency
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import logging
import sys
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Dict, Iterator, List, Optional, Tuple

try:
    import quantengine as qe
except ImportError as exc:
    print(
        "[ERROR] 'quantengine' not found. Build and install it:\n"
        "  cmake --preset dev-release && cmake --build build/release -j$(nproc)\n"
        "  pip install .\n",
        file=sys.stderr,
    )
    sys.exit(2)

log = logging.getLogger("quantengine.replay")

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

TICK_SIZE: float = 0.01  # overridden by --tick-size


def price_to_ticks(price_str: str) -> int:
    return round(float(price_str) / TICK_SIZE)


def ticks_to_decimal(ticks: int) -> str:
    return f"{ticks * TICK_SIZE:.8f}"


# ---------------------------------------------------------------------------
# Data Structures
# ---------------------------------------------------------------------------

@dataclass
class ReplayStats:
    symbol: str = "UNKNOWN"
    tick_size: float = TICK_SIZE
    input_files: List[str] = field(default_factory=list)
    events_processed: int = 0
    events_skipped: int = 0
    trades_generated: int = 0
    duration_sec: float = 0.0
    throughput_mevents_sec: float = 0.0
    final_canonical_hash: str = ""
    final_best_bid: Optional[str] = None
    final_best_ask: Optional[str] = None
    final_total_orders: int = 0
    invariants_ok: bool = True
    invariant_message: str = ""
    snapshots: List[Dict[str, Any]] = field(default_factory=list)

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


@dataclass
class L3Row:
    timestamp_ns: int
    event_type: str   # ADD | CANCEL | EXECUTE | MODIFY
    order_id: int
    side: Optional[str]     # B | S | None
    price_ticks: int
    quantity: int


# ---------------------------------------------------------------------------
# CSV Parser
# ---------------------------------------------------------------------------

def _parse_line(raw_line: str, lineno: int) -> Optional[L3Row]:
    line = raw_line.strip()
    if not line or line.startswith("#"):
        return None

    parts = [p.strip() for p in line.split(",")]
    if not parts:
        return None

    type_code = parts[0].upper()

    # 1. Canonical QuantEngine L3 format (A, E, C, R, T)
    if type_code == "A" and len(parts) >= 7:
        # A,ts_ns,order_id,symbol,price_ticks,quantity,side
        ts = int(parts[1])
        oid = int(parts[2])
        p_ticks = int(parts[4])
        qty = int(parts[5])
        side = "B" if parts[6].upper() in ("B", "BUY") else "S"
        return L3Row(timestamp_ns=ts, event_type="ADD", order_id=oid, side=side, price_ticks=p_ticks, quantity=qty)

    elif type_code == "E" and len(parts) >= 5:
        # E,ts_ns,order_id,symbol,executed_qty[,match_number]
        ts = int(parts[1])
        oid = int(parts[2])
        qty = int(parts[4])
        return L3Row(timestamp_ns=ts, event_type="EXECUTE", order_id=oid, side=None, price_ticks=0, quantity=qty)

    elif type_code == "C" and len(parts) >= 5:
        # C,ts_ns,order_id,symbol,cancelled_qty
        ts = int(parts[1])
        oid = int(parts[2])
        qty = int(parts[4])
        return L3Row(timestamp_ns=ts, event_type="CANCEL", order_id=oid, side=None, price_ticks=0, quantity=qty)

    elif type_code == "R" and len(parts) >= 8:
        # R,ts_ns,old_id,new_id,symbol,new_price,new_qty,side
        ts = int(parts[1])
        old_id = int(parts[2])
        new_price = int(parts[5])
        new_qty = int(parts[6])
        side = "B" if parts[7].upper() in ("B", "BUY") else "S"
        return L3Row(timestamp_ns=ts, event_type="MODIFY", order_id=old_id, side=side, price_ticks=new_price, quantity=new_qty)

    elif type_code == "T":
        # Trade reporting message — skip matching engine mutation
        return None

    # 2. Generic format: event_type / timestamp_ns
    if type_code in ("ADD", "CANCEL", "EXECUTE", "MODIFY"):
        # e.g., event_type,ts,order_id,side,price,qty
        if len(parts) >= 6:
            ts = int(parts[1])
            oid = int(parts[2])
            side = parts[3].upper() if parts[3] else None
            p_ticks = price_to_ticks(parts[4]) if parts[4] else 0
            qty = int(parts[5])
            return L3Row(timestamp_ns=ts, event_type=type_code, order_id=oid, side=side, price_ticks=p_ticks, quantity=qty)

    # 3. Numeric timestamp first: ts,event_type,order_id,side,price,quantity
    try:
        ts = int(parts[0])
        ev_type = parts[1].upper()
        if ev_type in ("ADD", "CANCEL", "EXECUTE", "MODIFY") and len(parts) >= 6:
            oid = int(parts[2])
            side = parts[3].upper() if parts[3] else None
            p_ticks = price_to_ticks(parts[4]) if parts[4] else 0
            qty = int(parts[5])
            return L3Row(timestamp_ns=ts, event_type=ev_type, order_id=oid, side=side, price_ticks=p_ticks, quantity=qty)
    except (ValueError, IndexError):
        pass

    return None


def iter_rows(paths: List[str]) -> Iterator[Tuple[str, L3Row]]:
    """Yield (source_file, L3Row) from all CSV files, in order."""
    for path in paths:
        lineno = 0
        try:
            with open(path, "r", encoding="utf-8") as fh:
                for line in fh:
                    lineno += 1
                    try:
                        row = _parse_line(line, lineno)
                        if row is not None:
                            yield (path, row)
                    except Exception as exc:
                        log.debug("Line %d: parse error — %s", lineno, exc)
        except FileNotFoundError:
            log.error("File not found: %s", path)
            raise


# ---------------------------------------------------------------------------
# Engine adapter
# ---------------------------------------------------------------------------

def _side_to_cpp(side: Optional[str]) -> qe.Side:
    if side == "B":
        return qe.Side.Buy
    return qe.Side.Sell


class ReplayEngine:
    """
    Stateful adapter that drives an ``OptimizedMatchingEngine`` from L3 events.

    Tracks which order IDs are currently live (resting) so that CANCEL and
    EXECUTE events can be forwarded to the engine correctly.
    """

    def __init__(self) -> None:
        self.engine = qe.OptimizedMatchingEngine(65536)
        self._live_orders: set[int] = set()
        self.total_trades: int = 0

    def apply(self, row: L3Row) -> int:
        """Apply one L3 event. Returns the number of fills generated."""
        fills = 0

        if row.event_type == "ADD":
            if row.order_id in self._live_orders:
                # Duplicate ADD — treat as no-op to stay deterministic
                return 0
            if row.price_ticks <= 0 or row.quantity <= 0:
                return 0
            if row.side not in ("B", "S"):
                return 0
            report = self.engine.submit_order(
                row.order_id,
                _side_to_cpp(row.side),
                row.price_ticks,
                row.quantity,
            )
            if report.status not in (qe.OrderStatus.Rejected,):
                self._live_orders.add(row.order_id)
            fills = len(report.trades)

        elif row.event_type in ("CANCEL", "EXECUTE"):
            if row.order_id not in self._live_orders:
                # Already gone (fully filled earlier) — skip silently
                return 0
            report = self.engine.cancel_order(row.order_id)
            if report.status != qe.OrderStatus.Rejected:
                self._live_orders.discard(row.order_id)

        elif row.event_type == "MODIFY":
            if row.order_id not in self._live_orders:
                return 0
            if row.price_ticks <= 0 or row.quantity <= 0:
                return 0
            report = self.engine.modify_order(row.order_id, row.price_ticks, row.quantity)
            if report.status == qe.OrderStatus.Rejected:
                self._live_orders.discard(row.order_id)
            fills = len(report.trades)

        self.total_trades += fills
        return fills

    def snapshot(self, depth: int = 5) -> Dict[str, Any]:
        """Return a current depth snapshot dict."""
        bids = self.engine.get_bids(depth)
        asks = self.engine.get_asks(depth)
        return {
            "bids": [[ticks_to_decimal(l.price), l.total_quantity] for l in bids],
            "asks": [[ticks_to_decimal(l.price), l.total_quantity] for l in asks],
            "sequence": self.engine.current_sequence(),
        }

    def audit(self) -> Tuple[bool, str]:
        return self.engine.audit()

    def canonical_hash(self) -> int:
        return self.engine.canonical_hash()

    def stats(self) -> Dict[str, Any]:
        bid_p = self.engine.best_bid_price()
        ask_p = self.engine.best_ask_price()
        return {
            "total_orders": self.engine.total_orders(),
            "best_bid": ticks_to_decimal(bid_p) if bid_p is not None else None,
            "best_ask": ticks_to_decimal(ask_p) if ask_p is not None else None,
        }


# ---------------------------------------------------------------------------
# Replay runner
# ---------------------------------------------------------------------------

def run_replay(
    input_paths: List[str],
    symbol: str,
    snapshot_interval: int,
) -> ReplayStats:
    """
    Pump all events from *input_paths* through the engine and return stats.
    """
    global TICK_SIZE
    stats = ReplayStats(symbol=symbol, tick_size=TICK_SIZE, input_files=input_paths)

    engine = ReplayEngine()
    events_processed = 0
    events_skipped = 0
    last_snapshot_at = 0

    t_start = time.perf_counter()

    for _path, row in iter_rows(input_paths):
        fills = engine.apply(row)
        if fills >= 0:
            events_processed += 1
        else:
            events_skipped += 1

        # Periodic depth snapshots
        if snapshot_interval > 0 and events_processed - last_snapshot_at >= snapshot_interval:
            snap = engine.snapshot(depth=5)
            snap["event_index"] = events_processed
            stats.snapshots.append(snap)
            last_snapshot_at = events_processed

    t_end = time.perf_counter()
    elapsed = t_end - t_start

    # Final audit
    ok, msg = engine.audit()
    eng_stats = engine.stats()

    stats.events_processed = events_processed
    stats.events_skipped = events_skipped
    stats.trades_generated = engine.total_trades
    stats.duration_sec = round(elapsed, 6)
    stats.throughput_mevents_sec = round(events_processed / elapsed / 1e6, 3) if elapsed > 0 else 0.0
    stats.final_canonical_hash = f"0x{engine.canonical_hash():016x}"
    stats.final_best_bid = eng_stats["best_bid"]
    stats.final_best_ask = eng_stats["best_ask"]
    stats.final_total_orders = eng_stats["total_orders"]
    stats.invariants_ok = ok
    stats.invariant_message = msg or "All invariants satisfied."

    return stats


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="QuantEngine historical L3 CSV replay utility",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--input",
        nargs="+",
        required=True,
        help="Input L3 CSV file(s). Supports glob patterns (e.g. data/*.csv).",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output JSON file for replay statistics.",
    )
    parser.add_argument(
        "--tick-size",
        type=float,
        default=0.01,
        dest="tick_size",
        help="Price tick size in USD (default: 0.01).",
    )
    parser.add_argument(
        "--symbol",
        default="BTCUSDT",
        help="Instrument symbol for metadata (default: BTCUSDT).",
    )
    parser.add_argument(
        "--snapshot-interval",
        type=int,
        default=10_000,
        dest="snapshot_interval",
        help="Record a depth snapshot every N events (0 = disabled, default: 10000).",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Enable debug-level logging.",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
    )

    global TICK_SIZE
    TICK_SIZE = args.tick_size

    # Expand globs
    input_paths: List[str] = []
    for pattern in args.input:
        expanded = sorted(glob.glob(pattern, recursive=True))
        if not expanded:
            # Treat as literal path
            expanded = [pattern]
        input_paths.extend(expanded)

    if not input_paths:
        log.error("No input files found.")
        return 1

    log.info(
        "Starting replay — %d file(s), symbol=%s, tick=%.4f",
        len(input_paths),
        args.symbol,
        TICK_SIZE,
    )

    try:
        stats = run_replay(
            input_paths=input_paths,
            symbol=args.symbol,
            snapshot_interval=args.snapshot_interval,
        )
    except ParseError as exc:
        log.error("Parse error: %s", exc)
        return 1

    # Write output JSON
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(stats.to_dict(), fh, indent=2)

    # Print summary to stdout
    print(
        f"\n{'='*60}\n"
        f"  QuantEngine Historical Replay — Summary\n"
        f"{'='*60}\n"
        f"  Symbol            : {stats.symbol}\n"
        f"  Input files       : {len(input_paths)}\n"
        f"  Events processed  : {stats.events_processed:,}\n"
        f"  Events skipped    : {stats.events_skipped:,}\n"
        f"  Trades generated  : {stats.trades_generated:,}\n"
        f"  Duration          : {stats.duration_sec:.3f}s\n"
        f"  Throughput        : {stats.throughput_mevents_sec:.2f} Mevents/s\n"
        f"  Final hash        : {stats.final_canonical_hash}\n"
        f"  Best bid          : {stats.final_best_bid or 'none'}\n"
        f"  Best ask          : {stats.final_best_ask or 'none'}\n"
        f"  Resting orders    : {stats.final_total_orders}\n"
        f"  Invariants OK     : {stats.invariants_ok}\n"
        f"  Output            : {output_path.resolve()}\n"
        f"{'='*60}\n"
    )

    if not stats.invariants_ok:
        log.error("Invariant violation: %s", stats.invariant_message)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
