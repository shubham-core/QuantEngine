#!/usr/bin/env python3
"""
QuantEngine Exchange API Server
================================
A high-performance mock exchange server that wraps the C++20 QuantEngine
matching engine with a Binance-compatible REST + WebSocket API.

Architecture
------------
- Single ``OptimizedMatchingEngine`` instance (C++20, zero-allocation arena).
- ``asyncio.Lock`` serialises all engine mutations — the C++ engine is not
  thread-safe, but the GIL is released inside ``submit_batch()`` so async
  tasks queue safely without blocking the event loop.
- All prices are integers in *ticks* (1 tick = 0.01 USD by convention).
  The ``TICK_SIZE`` constant converts between tick-integers and decimal strings
  in the JSON responses, matching the Binance "price" field format.
- A ``ConnectionManager`` broadcasts diff-depth and trade payloads to every
  connected WebSocket subscriber.

Usage
-----
    python server.py                          # starts on 0.0.0.0:8080
    python server.py --host 127.0.0.1 --port 9000 --tick-size 0.01
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
import time
from collections import deque
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from typing import Any, Deque, Dict, List, Optional, Set

try:
    import quantengine as qe
except ImportError as exc:
    print(
        "[ERROR] Could not import 'quantengine'. "
        "Build and install it first:\n"
        "  cmake --preset dev-release && cmake --build build/release -j$(nproc)\n"
        "  pip install .\n",
        file=sys.stderr,
    )
    raise SystemExit(1) from exc

try:
    from fastapi import FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
    from fastapi.responses import JSONResponse
    from pydantic import BaseModel, Field, field_validator
    import uvicorn
except ImportError as exc:
    print(
        "[ERROR] FastAPI/uvicorn not installed. Run:\n"
        "  pip install fastapi uvicorn websockets\n",
        file=sys.stderr,
    )
    raise SystemExit(1) from exc

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
log = logging.getLogger("quantengine.server")

# ---------------------------------------------------------------------------
# Global Configuration
# ---------------------------------------------------------------------------
SYMBOL = "BTCUSDT"
TICK_SIZE: float = 0.01          # 1 tick = $0.01
MAX_TRADE_HISTORY: int = 1_000   # circular buffer length
MAX_DEPTH_LEVELS: int = 500      # max book levels returned by /depth


# ---------------------------------------------------------------------------
# Price helpers
# ---------------------------------------------------------------------------

def ticks_to_str(ticks: int) -> str:
    """Convert integer ticks to Binance-style decimal price string."""
    return f"{ticks * TICK_SIZE:.8f}"


def str_to_ticks(price_str: str) -> int:
    """Parse a decimal price string into integer ticks (round half-up)."""
    return round(float(price_str) / TICK_SIZE)


def qty_to_str(qty: int) -> str:
    """Quantity is stored as whole lots; represent as decimal string."""
    return str(qty)


# ---------------------------------------------------------------------------
# Connection Manager (WebSocket broadcast)
# ---------------------------------------------------------------------------

class ConnectionManager:
    """Thread-safe (asyncio) WebSocket fan-out broadcaster."""

    def __init__(self) -> None:
        self._active: Set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def connect(self, ws: WebSocket) -> None:
        await ws.accept()
        async with self._lock:
            self._active.add(ws)
        log.info("WS client connected — total=%d", len(self._active))

    async def disconnect(self, ws: WebSocket) -> None:
        async with self._lock:
            self._active.discard(ws)
        log.info("WS client disconnected — total=%d", len(self._active))

    async def broadcast(self, payload: Dict[str, Any]) -> None:
        """Broadcast JSON payload to all connected clients."""
        message = json.dumps(payload)
        dead: List[WebSocket] = []
        async with self._lock:
            clients = list(self._active)
        for ws in clients:
            try:
                await ws.send_text(message)
            except Exception:
                dead.append(ws)
        if dead:
            async with self._lock:
                for ws in dead:
                    self._active.discard(ws)


# ---------------------------------------------------------------------------
# Exchange State
# ---------------------------------------------------------------------------

@dataclass
class ExchangeState:
    """Singleton holding engine + auxiliary market state."""
    engine: qe.OptimizedMatchingEngine = field(
        default_factory=lambda: qe.OptimizedMatchingEngine(65536)
    )
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    ws_manager: ConnectionManager = field(default_factory=ConnectionManager)

    # Next client-facing order ID (monotonically increasing)
    _next_order_id: int = 1

    # Map: client_order_id (str) -> internal_order_id (int)
    client_to_internal: Dict[str, int] = field(default_factory=dict)
    # Reverse map for cancel/status lookup
    internal_to_client: Dict[int, str] = field(default_factory=dict)
    # Track order side for response building
    order_side: Dict[int, str] = field(default_factory=dict)
    # Track original order price for response building
    order_price: Dict[int, int] = field(default_factory=dict)
    # Track original quantity
    order_qty: Dict[int, int] = field(default_factory=dict)
    # Track filled quantity per order
    order_filled: Dict[int, int] = field(default_factory=dict)
    # Track order status strings
    order_status: Dict[int, str] = field(default_factory=dict)

    # Trade history ring buffer
    trade_history: Deque[Dict[str, Any]] = field(
        default_factory=lambda: deque(maxlen=MAX_TRADE_HISTORY)
    )
    # Global event time (Unix ms)
    event_time: int = field(default_factory=lambda: int(time.time() * 1000))

    def allocate_order_id(self) -> int:
        oid = self._next_order_id
        self._next_order_id += 1
        return oid

    def record_client_order(
        self, client_id: str, internal_id: int, side: str, price: int, qty: int
    ) -> None:
        self.client_to_internal[client_id] = internal_id
        self.internal_to_client[internal_id] = client_id
        self.order_side[internal_id] = side
        self.order_price[internal_id] = price
        self.order_qty[internal_id] = qty
        self.order_filled[internal_id] = 0
        self.order_status[internal_id] = "NEW"


# Instantiated at startup via lifespan
_state: ExchangeState


# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------

@asynccontextmanager
async def lifespan(app: FastAPI):  # noqa: ANN001
    global _state
    _state = ExchangeState()
    log.info(
        "QuantEngine exchange started — engine capacity=65536, symbol=%s, tick=%.4f",
        SYMBOL,
        TICK_SIZE,
    )
    yield
    log.info("QuantEngine exchange shutting down.")


app = FastAPI(
    title="QuantEngine Mock Exchange",
    description="Binance-compatible REST+WebSocket API backed by C++20 QuantEngine",
    version="1.0.0",
    lifespan=lifespan,
)


# ---------------------------------------------------------------------------
# Pydantic request/response models (Binance-compatible schema)
# ---------------------------------------------------------------------------

class NewOrderRequest(BaseModel):
    symbol: str = Field(..., description="Trading pair symbol, e.g. BTCUSDT")
    side: str = Field(..., description="BUY or SELL")
    type: str = Field(default="LIMIT", description="Order type — only LIMIT supported")
    timeInForce: str = Field(default="GTC", description="GTC or IOC")
    quantity: str = Field(..., description="Order quantity as decimal string")
    price: str = Field(..., description="Limit price as decimal string")
    newClientOrderId: Optional[str] = Field(
        default=None, description="Unique client-assigned order ID"
    )

    @field_validator("side")
    @classmethod
    def validate_side(cls, v: str) -> str:
        if v.upper() not in ("BUY", "SELL"):
            raise ValueError("side must be BUY or SELL")
        return v.upper()

    @field_validator("type")
    @classmethod
    def validate_type(cls, v: str) -> str:
        if v.upper() != "LIMIT":
            raise ValueError("Only LIMIT orders are supported")
        return v.upper()

    @field_validator("quantity")
    @classmethod
    def validate_quantity(cls, v: str) -> str:
        if float(v) <= 0:
            raise ValueError("quantity must be positive")
        return v

    @field_validator("price")
    @classmethod
    def validate_price(cls, v: str) -> str:
        if float(v) <= 0:
            raise ValueError("price must be positive")
        return v


class CancelOrderRequest(BaseModel):
    symbol: str
    orderId: Optional[int] = None
    origClientOrderId: Optional[str] = None


# ---------------------------------------------------------------------------
# Helper: process ExecutionReport and broadcast
# ---------------------------------------------------------------------------

async def _process_report(
    report: qe.ExecutionReport,
    state: ExchangeState,
) -> None:
    """Update auxiliary state and broadcast trades to WebSocket subscribers."""
    internal_id = report.order_id

    # Accumulate fills
    state.order_filled[internal_id] = (
        state.order_filled.get(internal_id, 0) + report.filled_quantity
    )

    # Determine new status string
    if report.status == qe.OrderStatus.Filled:
        state.order_status[internal_id] = "FILLED"
    elif report.status == qe.OrderStatus.PartiallyFilled:
        state.order_status[internal_id] = "PARTIALLY_FILLED"
    elif report.status in (qe.OrderStatus.Cancelled, qe.OrderStatus.Rejected):
        state.order_status[internal_id] = (
            "CANCELED" if report.status == qe.OrderStatus.Cancelled else "REJECTED"
        )
    elif report.status == qe.OrderStatus.Resting:
        state.order_status[internal_id] = "NEW"

    # Also update maker orders participating in trades
    for trade in report.trades:
        maker_id = trade.maker_order_id
        if maker_id in state.order_qty:
            state.order_filled[maker_id] = (
                state.order_filled.get(maker_id, 0) + trade.quantity
            )
            if state.order_filled[maker_id] >= state.order_qty[maker_id]:
                state.order_status[maker_id] = "FILLED"
            else:
                state.order_status[maker_id] = "PARTIALLY_FILLED"

    # Build and broadcast trade events
    now_ms = int(time.time() * 1000)
    for trade in report.trades:
        trade_payload: Dict[str, Any] = {
            "e": "trade",
            "E": now_ms,
            "s": SYMBOL,
            "t": trade.trade_id,
            "p": ticks_to_str(trade.price),
            "q": qty_to_str(trade.quantity),
            "b": trade.maker_order_id if trade.maker_side == qe.Side.Buy else trade.taker_order_id,
            "a": trade.maker_order_id if trade.maker_side == qe.Side.Sell else trade.taker_order_id,
            "T": now_ms,
            "m": trade.maker_side == qe.Side.Buy,
            "M": True,
        }
        state.trade_history.append(trade_payload)
        await state.ws_manager.broadcast(trade_payload)

    # Broadcast depth update after every order
    await _broadcast_depth_update(state)


async def _broadcast_depth_update(state: ExchangeState) -> None:
    """Broadcast a diff-depth event (Binance @depth format) to all WS clients."""
    now_ms = int(time.time() * 1000)
    bids = state.engine.get_bids(20)
    asks = state.engine.get_asks(20)
    payload = {
        "e": "depthUpdate",
        "E": now_ms,
        "s": SYMBOL,
        "U": state.engine.current_sequence(),
        "u": state.engine.current_sequence(),
        "b": [[ticks_to_str(l.price), qty_to_str(l.total_quantity)] for l in bids],
        "a": [[ticks_to_str(l.price), qty_to_str(l.total_quantity)] for l in asks],
    }
    await state.ws_manager.broadcast(payload)


# ---------------------------------------------------------------------------
# REST Endpoints — Binance-compatible
# ---------------------------------------------------------------------------

@app.get("/api/v3/ping", tags=["System"])
async def ping() -> Dict[str, Any]:
    """Connectivity test — returns empty object like Binance."""
    return {}


@app.get("/api/v3/time", tags=["System"])
async def server_time() -> Dict[str, Any]:
    """Return server time in Unix milliseconds."""
    return {"serverTime": int(time.time() * 1000)}


@app.get("/api/v3/exchangeInfo", tags=["System"])
async def exchange_info() -> Dict[str, Any]:
    """Return minimal exchange metadata."""
    return {
        "timezone": "UTC",
        "serverTime": int(time.time() * 1000),
        "symbols": [
            {
                "symbol": SYMBOL,
                "status": "TRADING",
                "baseAsset": "BTC",
                "quoteAsset": "USDT",
                "filters": [
                    {"filterType": "PRICE_FILTER", "minPrice": "0.01", "tickSize": f"{TICK_SIZE:.8f}"},
                    {"filterType": "LOT_SIZE", "minQty": "1", "stepSize": "1"},
                ],
                "orderTypes": ["LIMIT"],
                "timeInForce": ["GTC", "IOC"],
            }
        ],
    }


@app.get("/api/v3/depth", tags=["Market Data"])
async def get_depth(
    symbol: str = Query(...),
    limit: int = Query(default=100, ge=1, le=MAX_DEPTH_LEVELS),
) -> Dict[str, Any]:
    """Return L2 order book snapshot (Binance /api/v3/depth schema)."""
    if symbol.upper() != SYMBOL:
        raise HTTPException(status_code=400, detail=f"Unknown symbol: {symbol}")

    async with _state.lock:
        bids = _state.engine.get_bids(limit)
        asks = _state.engine.get_asks(limit)
        seq = _state.engine.current_sequence()

    return {
        "lastUpdateId": seq,
        "bids": [[ticks_to_str(l.price), qty_to_str(l.total_quantity)] for l in bids],
        "asks": [[ticks_to_str(l.price), qty_to_str(l.total_quantity)] for l in asks],
    }


@app.get("/api/v3/trades", tags=["Market Data"])
async def get_recent_trades(
    symbol: str = Query(...),
    limit: int = Query(default=500, ge=1, le=1000),
) -> List[Dict[str, Any]]:
    """Return recent trade history (Binance /api/v3/trades schema)."""
    if symbol.upper() != SYMBOL:
        raise HTTPException(status_code=400, detail=f"Unknown symbol: {symbol}")
    trades = list(_state.trade_history)[-limit:]
    return [
        {
            "id": t["t"],
            "price": t["p"],
            "qty": t["q"],
            "quoteQty": f"{float(t['p']) * float(t['q']):.8f}",
            "time": t["T"],
            "isBuyerMaker": t["m"],
        }
        for t in trades
    ]


@app.get("/api/v3/ticker/bookTicker", tags=["Market Data"])
async def book_ticker(symbol: str = Query(...)) -> Dict[str, Any]:
    """Return best bid/ask (Binance /api/v3/ticker/bookTicker schema)."""
    if symbol.upper() != SYMBOL:
        raise HTTPException(status_code=400, detail=f"Unknown symbol: {symbol}")
    async with _state.lock:
        bid_p = _state.engine.best_bid_price()
        bid_q = _state.engine.best_bid_quantity()
        ask_p = _state.engine.best_ask_price()
        ask_q = _state.engine.best_ask_quantity()

    return {
        "symbol": SYMBOL,
        "bidPrice": ticks_to_str(bid_p) if bid_p is not None else "0.00000000",
        "bidQty": qty_to_str(bid_q) if bid_q is not None else "0",
        "askPrice": ticks_to_str(ask_p) if ask_p is not None else "0.00000000",
        "askQty": qty_to_str(ask_q) if ask_q is not None else "0",
    }


@app.get("/api/v3/ticker/24hr", tags=["Market Data"])
async def ticker_24hr(symbol: str = Query(...)) -> Dict[str, Any]:
    """Return 24-hour statistics (simplified)."""
    if symbol.upper() != SYMBOL:
        raise HTTPException(status_code=400, detail=f"Unknown symbol: {symbol}")
    async with _state.lock:
        bid_p = _state.engine.best_bid_price()
        ask_p = _state.engine.best_ask_price()

    mid_ticks = ((bid_p or 0) + (ask_p or 0)) // 2 or 1
    return {
        "symbol": SYMBOL,
        "priceChange": "0.00000000",
        "priceChangePercent": "0.000",
        "weightedAvgPrice": ticks_to_str(mid_ticks),
        "prevClosePrice": ticks_to_str(mid_ticks),
        "lastPrice": ticks_to_str(mid_ticks),
        "bidPrice": ticks_to_str(bid_p) if bid_p is not None else "0.00000000",
        "askPrice": ticks_to_str(ask_p) if ask_p is not None else "0.00000000",
        "openPrice": ticks_to_str(mid_ticks),
        "highPrice": ticks_to_str(mid_ticks),
        "lowPrice": ticks_to_str(mid_ticks),
        "volume": "0.00000000",
        "quoteVolume": "0.00000000",
        "openTime": int(time.time() * 1000) - 86_400_000,
        "closeTime": int(time.time() * 1000),
        "count": len(_state.trade_history),
    }


# ---------------------------------------------------------------------------
# Order Management Endpoints
# ---------------------------------------------------------------------------

@app.post("/api/v3/order", tags=["Trading"], status_code=200)
async def new_order(req: NewOrderRequest) -> Dict[str, Any]:
    """Place a new limit order (Binance POST /api/v3/order schema)."""
    if req.symbol.upper() != SYMBOL:
        raise HTTPException(status_code=400, detail=f"Unknown symbol: {req.symbol}")

    price_ticks = str_to_ticks(req.price)
    quantity = int(req.quantity)
    if price_ticks <= 0:
        raise HTTPException(status_code=400, detail="Invalid price")
    if quantity <= 0:
        raise HTTPException(status_code=400, detail="Invalid quantity")

    side_cpp = qe.Side.Buy if req.side == "BUY" else qe.Side.Sell

    async with _state.lock:
        internal_id = _state.allocate_order_id()
        client_id = req.newClientOrderId or str(internal_id)
        if client_id in _state.client_to_internal:
            raise HTTPException(
                status_code=400,
                detail=f"Duplicate clientOrderId: {client_id}",
            )

        _state.record_client_order(client_id, internal_id, req.side, price_ticks, quantity)
        report = _state.engine.submit_order(internal_id, side_cpp, price_ticks, quantity)
        await _process_report(report, _state)

    status_str = _state.order_status.get(internal_id, "NEW")
    filled = _state.order_filled.get(internal_id, 0)
    transact_time = int(time.time() * 1000)

    return {
        "symbol": SYMBOL,
        "orderId": internal_id,
        "orderListId": -1,
        "clientOrderId": client_id,
        "transactTime": transact_time,
        "price": req.price,
        "origQty": req.quantity,
        "executedQty": qty_to_str(filled),
        "cummulativeQuoteQty": f"{filled * price_ticks * TICK_SIZE:.8f}",
        "status": status_str,
        "timeInForce": req.timeInForce,
        "type": "LIMIT",
        "side": req.side,
        "fills": [
            {
                "price": ticks_to_str(t.price),
                "qty": qty_to_str(t.quantity),
                "commission": "0.00000000",
                "commissionAsset": "USDT",
                "tradeId": t.trade_id,
            }
            for t in report.trades
        ],
    }


@app.delete("/api/v3/order", tags=["Trading"])
async def cancel_order(
    symbol: str = Query(...),
    orderId: Optional[int] = Query(default=None),
    origClientOrderId: Optional[str] = Query(default=None),
) -> Dict[str, Any]:
    """Cancel a resting limit order (Binance DELETE /api/v3/order schema)."""
    if symbol.upper() != SYMBOL:
        raise HTTPException(status_code=400, detail=f"Unknown symbol: {symbol}")
    if orderId is None and origClientOrderId is None:
        raise HTTPException(status_code=400, detail="Provide orderId or origClientOrderId")

    async with _state.lock:
        if orderId is not None:
            internal_id = orderId
        else:
            internal_id = _state.client_to_internal.get(origClientOrderId)  # type: ignore[arg-type]
            if internal_id is None:
                raise HTTPException(
                    status_code=400, detail=f"Unknown clientOrderId: {origClientOrderId}"
                )

        if internal_id not in _state.order_status:
            raise HTTPException(status_code=400, detail=f"Unknown orderId: {internal_id}")
        if _state.order_status[internal_id] in ("FILLED", "CANCELLED", "CANCELED"):
            raise HTTPException(
                status_code=400,
                detail=f"Order {internal_id} is already {_state.order_status[internal_id]}",
            )

        report = _state.engine.cancel_order(internal_id)
        if report.status == qe.OrderStatus.Rejected:
            raise HTTPException(
                status_code=400,
                detail=f"Order {internal_id} cancel rejected",
            )
        await _process_report(report, _state)

    client_id = _state.internal_to_client.get(internal_id, str(internal_id))
    return {
        "symbol": SYMBOL,
        "origClientOrderId": client_id,
        "orderId": internal_id,
        "orderListId": -1,
        "clientOrderId": client_id,
        "price": ticks_to_str(_state.order_price.get(internal_id, 0)),
        "origQty": qty_to_str(_state.order_qty.get(internal_id, 0)),
        "executedQty": qty_to_str(_state.order_filled.get(internal_id, 0)),
        "cummulativeQuoteQty": "0.00000000",
        "status": "CANCELED",
        "timeInForce": "GTC",
        "type": "LIMIT",
        "side": _state.order_side.get(internal_id, "BUY"),
    }


@app.get("/api/v3/order", tags=["Trading"])
async def get_order(
    symbol: str = Query(...),
    orderId: Optional[int] = Query(default=None),
    origClientOrderId: Optional[str] = Query(default=None),
) -> Dict[str, Any]:
    """Query order status (Binance GET /api/v3/order schema)."""
    if symbol.upper() != SYMBOL:
        raise HTTPException(status_code=400, detail=f"Unknown symbol: {symbol}")
    if orderId is None and origClientOrderId is None:
        raise HTTPException(status_code=400, detail="Provide orderId or origClientOrderId")

    if orderId is not None:
        internal_id = orderId
    else:
        internal_id = _state.client_to_internal.get(origClientOrderId)  # type: ignore[arg-type]
        if internal_id is None:
            raise HTTPException(
                status_code=400, detail=f"Unknown clientOrderId: {origClientOrderId}"
            )

    if internal_id not in _state.order_status:
        raise HTTPException(status_code=400, detail=f"Unknown orderId: {internal_id}")

    client_id = _state.internal_to_client.get(internal_id, str(internal_id))
    filled = _state.order_filled.get(internal_id, 0)
    price = _state.order_price.get(internal_id, 0)
    qty = _state.order_qty.get(internal_id, 0)
    remaining = max(0, qty - filled)

    return {
        "symbol": SYMBOL,
        "orderId": internal_id,
        "orderListId": -1,
        "clientOrderId": client_id,
        "price": ticks_to_str(price),
        "origQty": qty_to_str(qty),
        "executedQty": qty_to_str(filled),
        "cummulativeQuoteQty": f"{filled * price * TICK_SIZE:.8f}",
        "status": _state.order_status.get(internal_id, "NEW"),
        "timeInForce": "GTC",
        "type": "LIMIT",
        "side": _state.order_side.get(internal_id, "BUY"),
        "time": int(time.time() * 1000),
        "updateTime": int(time.time() * 1000),
        "isWorking": remaining > 0,
        "origQuoteOrderQty": "0.00000000",
    }


@app.get("/api/v3/openOrders", tags=["Trading"])
async def open_orders(symbol: str = Query(...)) -> List[Dict[str, Any]]:
    """Return all resting orders for a symbol."""
    if symbol.upper() != SYMBOL:
        raise HTTPException(status_code=400, detail=f"Unknown symbol: {symbol}")

    result = []
    for internal_id, status in _state.order_status.items():
        if status in ("NEW", "PARTIALLY_FILLED"):
            client_id = _state.internal_to_client.get(internal_id, str(internal_id))
            filled = _state.order_filled.get(internal_id, 0)
            price = _state.order_price.get(internal_id, 0)
            qty = _state.order_qty.get(internal_id, 0)
            result.append(
                {
                    "symbol": SYMBOL,
                    "orderId": internal_id,
                    "clientOrderId": client_id,
                    "price": ticks_to_str(price),
                    "origQty": qty_to_str(qty),
                    "executedQty": qty_to_str(filled),
                    "status": status,
                    "timeInForce": "GTC",
                    "type": "LIMIT",
                    "side": _state.order_side.get(internal_id, "BUY"),
                    "time": int(time.time() * 1000),
                    "isWorking": True,
                }
            )
    return result


# ---------------------------------------------------------------------------
# Engine Diagnostics Endpoints (non-Binance, QuantEngine-specific)
# ---------------------------------------------------------------------------

@app.get("/qe/audit", tags=["Diagnostics"])
async def engine_audit() -> Dict[str, Any]:
    """Run the C++ InvariantAuditor on the live engine state."""
    async with _state.lock:
        ok, msg = _state.engine.audit()
        state_hash = _state.engine.canonical_hash()
        seq = _state.engine.current_sequence()
    return {
        "invariants_ok": ok,
        "message": msg or "All invariants satisfied.",
        "canonical_hash": f"0x{state_hash:016x}",
        "sequence_number": seq,
        "total_orders": _state.engine.total_orders(),
        "total_bid_volume": _state.engine.total_bid_volume(),
        "total_ask_volume": _state.engine.total_ask_volume(),
    }


@app.post("/qe/reset", tags=["Diagnostics"])
async def engine_reset() -> Dict[str, Any]:
    """Reset the matching engine to empty state (for testing)."""
    async with _state.lock:
        _state.engine.reset()
        _state.client_to_internal.clear()
        _state.internal_to_client.clear()
        _state.order_side.clear()
        _state.order_price.clear()
        _state.order_qty.clear()
        _state.order_filled.clear()
        _state.order_status.clear()
        _state.trade_history.clear()
        _state._next_order_id = 1
    log.warning("Engine reset to empty state.")
    return {"status": "reset", "message": "Engine cleared."}


# ---------------------------------------------------------------------------
# WebSocket Endpoint
# ---------------------------------------------------------------------------

@app.websocket("/ws/stream")
async def websocket_stream(websocket: WebSocket) -> None:
    """
    Real-time stream endpoint.

    Clients connect and receive:
    - ``depthUpdate`` — diff-depth after each order mutation
    - ``trade`` — execution record for each fill

    On connect, sends an immediate snapshot of the top 20 bid/ask levels.
    """
    await _state.ws_manager.connect(websocket)
    try:
        # Send initial snapshot
        async with _state.lock:
            bids = _state.engine.get_bids(20)
            asks = _state.engine.get_asks(20)
            seq = _state.engine.current_sequence()
        snapshot = {
            "e": "depthUpdate",
            "E": int(time.time() * 1000),
            "s": SYMBOL,
            "U": seq,
            "u": seq,
            "b": [[ticks_to_str(l.price), qty_to_str(l.total_quantity)] for l in bids],
            "a": [[ticks_to_str(l.price), qty_to_str(l.total_quantity)] for l in asks],
        }
        await websocket.send_text(json.dumps(snapshot))
        log.info("Sent initial depth snapshot to new WS client.")

        # Keep connection alive — server will push; client can send pings
        while True:
            try:
                msg = await asyncio.wait_for(websocket.receive_text(), timeout=30.0)
                # Echo back pings
                if msg.strip().lower() in ("ping", "{}"):
                    await websocket.send_text(json.dumps({"e": "pong"}))
            except asyncio.TimeoutError:
                # Send a keepalive pong
                await websocket.send_text(json.dumps({"e": "ping"}))
    except WebSocketDisconnect:
        pass
    finally:
        await _state.ws_manager.disconnect(websocket)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="QuantEngine Mock Exchange Server")
    parser.add_argument("--host", default="0.0.0.0", help="Bind host (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8080, help="Bind port (default: 8080)")
    parser.add_argument(
        "--tick-size",
        type=float,
        default=0.01,
        dest="tick_size",
        help="Price tick size in USD (default: 0.01)",
    )
    parser.add_argument("--reload", action="store_true", help="Enable auto-reload (dev mode)")
    return parser.parse_args()


if __name__ == "__main__":
    args = _parse_args()
    TICK_SIZE = args.tick_size
    log.info("Starting QuantEngine server on %s:%d (tick=%.4f)", args.host, args.port, TICK_SIZE)
    uvicorn.run(
        "server:app",
        host=args.host,
        port=args.port,
        reload=args.reload,
        log_level="info",
    )
