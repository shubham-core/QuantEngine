# ⚡ QuantEngine

<p align="center">
  <a href="assets/quantengine-demo.gif">
    <img src="assets/quantengine-demo.gif" alt="QuantEngine Deterministic Order Book and L3 Historical Replay Demo" width="850px" />
  </a>
  <br>
  <em>Live Demo: Zero-allocation intrusive doubly-linked arena matching hot path, L3 replay, and FNV-1a binary state hashing.</em>
</p>

> **A deterministic, ultra-low-latency C++20 Limit Order Book matching engine featuring a 65,536-slot intrusive arena pool, historical L3 market replay, and Binance-compatible REST/WebSocket endpoints.**

<div align="center">

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg?logo=cplusplus)](https://en.cppreference.com/w/cpp/20)
[![Python](https://img.shields.io/badge/Python-3.10%2B-blue.svg?logo=python)](https://python.org)
[![Tests](https://img.shields.io/badge/tests-248%20C%2B%2B%20%7C%2063%20Python%20passing-brightgreen.svg)](#)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL%20v3-red.svg)](LICENSE)
[![CI](https://github.com/shubham-core/QuantEngine/actions/workflows/ci.yml/badge.svg)](https://github.com/shubham-core/QuantEngine/actions)

[Quickstart](#hands-on-quickstart-2-minute-walkthrough) · [Architecture](#architecture) · [Getting Started](#getting-started) · [Exchange API](#exchange-api-server) · [L3 Historical Replay](#l3-historical-replay) · [Benchmarks](#benchmarks) · [Commercial License](#commercial-licensing)

</div>

---

### 🏛️ Architecture Pipeline

```
┌────────────────────────────────────────────────────────┐
│ Inbound Orders / Historical L3 Tick Replay Stream      │
│ (ADD / CANCEL / EXECUTE / MODIFY)                      │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│ 65,536-Slot Intrusive Doubly-Linked Arena Pool         │
│ (Pre-Allocated Node Index Array • Zero Heap Allocs)   │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│ Deterministic Price-Level BTree & FIFO Queue Engine    │
│ (Sub-Microsecond Price Priority Matching Hot Path)     │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│ 64-Bit FNV-1a Canonical Binary State Hasher            │
│ (Bit-for-Bit Cross-Engine State Verification Invariant)│
└───────────────────────────┬────────────────────────────┘
                            │
         ┌──────────────────┴──────────────────┐
         ▼                                     ▼
┌──────────────────────────────┐   ┌──────────────────────────────┐
│ pybind11 High-Throughput     │   │ Binance-Compatible REST &    │
│ Python Strategy Bindings     │   │ WebSocket Server (FastAPI)   │
└──────────────────────────────┘   └──────────────────────────────┘
```
---

## Why Determinism Matters

Most backtesting frameworks treat order execution as a black box. They assume fills happen at the quoted price, ignore queue position, and cannot reproduce a historical book state bit-for-bit.

QuantEngine takes a different approach:

- **Zero-allocation hot path.** The `OptimizedOrderBook` uses a 65,536-slot arena (`OrderPool`) with 32-bit intrusive doubly-linked lists and a flat hash map for price levels. The matching loop does not touch the heap.
- **Canonical binary state hash.** After every event, a 64-bit FNV-1a hash over the serialised book state produces a single integer. Two engines that process the same command log **must** produce the same hash — this is enforced by a cross-engine invariant test on every CI push.
- **L3 event-time replay.** The historical replay subsystem reconstructs the full exchange book from raw L3 tick data (ADD / CANCEL / EXECUTE / MODIFY), tracks FIFO queue position for each resting order, and simulates virtual latency for your strategy's observation, decision, and submission paths.
- **Full exchange API.** The `server.py` FastAPI layer wraps the C++ engine in a Binance-compatible REST + WebSocket server. Point your existing trading bot at `localhost:8080` and it will not know the difference.

---

## Hands-On Quickstart (2-Minute Walkthrough)

Experience the full exchange lifecycle from your terminal in under two minutes:

### 1. Boot the Exchange Server (Terminal 1)
```bash
PYTHONPATH=build/release/python python3 server.py --host 127.0.0.1 --port 8080
```

### 2. Inspect Depth & Place Orders (Terminal 2)
```bash
# Ping connectivity
curl -s http://127.0.0.1:8080/api/v3/ping

# Check empty book
curl -s "http://127.0.0.1:8080/api/v3/depth?symbol=BTCUSDT"

# Place a resting limit BUY order (5 BTC @ $50,000.00)
curl -s -X POST http://127.0.0.1:8080/api/v3/order \
  -H "Content-Type: application/json" \
  -d '{"symbol":"BTCUSDT","side":"BUY","type":"LIMIT","timeInForce":"GTC","quantity":"5","price":"50000.00","newClientOrderId":"my_bid"}'

# Cross the book with an aggressive SELL order (2 BTC @ $49,000.00)
curl -s -X POST http://127.0.0.1:8080/api/v3/order \
  -H "Content-Type: application/json" \
  -d '{"symbol":"BTCUSDT","side":"SELL","type":"LIMIT","timeInForce":"GTC","quantity":"2","price":"49000.00","newClientOrderId":"my_ask"}'

# View the executed trade on the public tape
curl -s "http://127.0.0.1:8080/api/v3/trades?symbol=BTCUSDT"

# Query original order status (shows PARTIALLY_FILLED, 3 BTC resting)
curl -s "http://127.0.0.1:8080/api/v3/order?symbol=BTCUSDT&origClientOrderId=my_bid"

# Cancel resting liquidity
curl -s -X DELETE "http://127.0.0.1:8080/api/v3/order?symbol=BTCUSDT&orderId=1"

# Audit C++ internal memory pool invariants and state hash
curl -s http://127.0.0.1:8080/qe/audit
```

---

## Architecture

### Core Matching Engine

```mermaid
flowchart LR
    subgraph C++20 Core
        OC[OrderCommand\nCreateOrderCommand\nCancelOrderCommand\nModifyOrderCommand]
        GME[GenericMatchingEngine\ntemplate BookType]
        OOB[OptimizedOrderBook\nArena Pool · Intrusive Lists\nFlat HashMap]
        ROB[ReferenceOrderBook\nstd::map · std::list\nGround Truth Oracle]
        PORT[Portfolio\nAVCO Accounting\nRealised P&L]
    end

    OC --> GME
    GME --> OOB
    GME --> ROB
    GME -->|ExecutionReport + Trades| PORT
```

### L3 Historical Replay Pipeline

```mermaid
flowchart LR
    CSV[L3 CSV Tick Data\nADD · CANCEL\nEXECUTE · MODIFY]
    PARSER[L3CsvParser\nCanonical Events]
    BOOK[HistoricalL3Book\nPrice/Time Priority\nRemaining Quantities]
    QUEUE[FifoQueueModel\nQueue-Ahead Tracking\nDeterministic Position]
    CLOCK[EventClock\nVirtual Nanoseconds]
    LATENCY[LatencyModel\nFeed · Decision\nEntry · Response]
    GATEWAY[ReplayGateway\nExecution Simulator]
    STRATEGY[Strategy\nObservation → Signal]
    PORTFOLIO[Portfolio\nP&L · AVCO]
    HASH[Canonical Hash\n64-bit FNV-1a]

    CSV --> PARSER
    PARSER --> BOOK
    BOOK --> QUEUE
    QUEUE --> CLOCK
    CLOCK --> LATENCY
    LATENCY --> GATEWAY
    GATEWAY --> STRATEGY
    STRATEGY -->|Orders| GATEWAY
    GATEWAY --> PORTFOLIO
    PORTFOLIO --> HASH
```

### Exchange API Layer

```mermaid
flowchart TD
    BOT[Trading Bot\nExisting Strategy]
    REST["REST API\nPOST /api/v3/order\nDELETE /api/v3/order\nGET /api/v3/depth"]
    WS[WebSocket\n/ws/stream\ndepthUpdate · trade]
    FASTAPI[FastAPI / uvicorn\nasyncio.Lock]
    ENGINE[OptimizedMatchingEngine\nC++20 · Zero-Alloc]

    BOT -->|HTTP| REST
    BOT -->|WS| WS
    REST --> FASTAPI
    WS --> FASTAPI
    FASTAPI -->|GIL released| ENGINE
```

---

## Getting Started

### Prerequisites

| Tool | Minimum version |
|------|----------------|
| GCC or Clang | 13+ (C++20 required) |
| CMake | 3.28+ |
| Ninja | 1.11+ |
| Python | 3.9+ |

```bash
# Ubuntu 24.04
sudo apt-get install build-essential cmake ninja-build python3-dev python3-pip

# macOS (Homebrew)
brew install cmake ninja
```

### Clone and Build (C++)

```bash
git clone --recurse-submodules https://github.com/shubham-dataeng/QuantEngine.git
cd QuantEngine

# Release build (maximum optimisation)
cmake --preset dev-release
cmake --build build/release -j$(nproc)

# Sanitizer build (AddressSanitizer + UndefinedBehaviourSanitizer)
cmake --preset dev-sanitizer
cmake --build build/dev-sanitizer -j$(nproc)
```

### Run C++ Tests

```bash
# All 248 tests (release)
./build/release/tests/quantengine_tests

# With sanitizers (ASan/UBSan clean)
./build/dev-sanitizer/tests/quantengine_tests
```

Expected output:
```
[==========] 248 tests from 40 test suites ran. (119 ms total)
[  PASSED  ] 248 tests.
```

### Run Python Tests (32 Server + 31 Engine Tests)

```bash
PYTHONPATH=build/release/python python3 -m pytest tests/python/ -v
```

Expected output:
```
======================== 63 passed, 1 warning in 0.73s =========================
```

### Install Python Bindings

```bash
pip install .                      # core bindings only
pip install ".[server]"            # + FastAPI exchange server
pip install ".[all]"               # + tests + replay
```

### Quick Python Example

```python
import quantengine as qe

engine = qe.OptimizedMatchingEngine()

# Place resting bid (price in ticks: 50,000.00 USD = 5,000,000 ticks)
r = engine.submit_order(1, qe.Side.Buy, 5_000_000, 10)
print(r.status)   # OrderStatus.Resting

# Aggressive sell crosses the book
r = engine.submit_order(2, qe.Side.Sell, 4_999_900, 5)
print(r.status)          # OrderStatus.Filled
print(len(r.trades))     # 1

# Deterministic canonical state hash
print(f"0x{engine.canonical_hash():016x}")
```

---

## Exchange API Server

The `server.py` module boots a **Binance-compatible mock exchange** powered by the C++ engine. Point any standard trading bot at it with zero code changes.

### Start the Server

```bash
# Direct run using built bindings
PYTHONPATH=build/release/python python3 server.py --host 0.0.0.0 --port 8080 --tick-size 0.01

# Or with Docker
docker compose up --build
```

### REST Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/v3/ping` | Connectivity check |
| GET | `/api/v3/time` | Server time (Unix ms) |
| GET | `/api/v3/exchangeInfo` | Symbol and filter metadata |
| GET | `/api/v3/depth?symbol=BTCUSDT&limit=20` | L2 order book snapshot |
| GET | `/api/v3/trades?symbol=BTCUSDT` | Recent trade history |
| GET | `/api/v3/ticker/bookTicker?symbol=BTCUSDT` | Best bid/ask |
| POST | `/api/v3/order` | Place new limit order |
| DELETE | `/api/v3/order?orderId=…` | Cancel a resting order |
| GET | `/api/v3/order?orderId=…` | Query order status |
| GET | `/api/v3/openOrders?symbol=BTCUSDT` | List all resting orders |
| GET | `/qe/audit` | C++ invariant audit + canonical hash |
| POST | `/qe/reset` | Reset engine to empty state |

### Example: Place and Cancel an Order

```bash
# Place a bid
curl -s -X POST http://localhost:8080/api/v3/order \
  -H "Content-Type: application/json" \
  -d '{
    "symbol": "BTCUSDT",
    "side": "BUY",
    "type": "LIMIT",
    "timeInForce": "GTC",
    "quantity": "5",
    "price": "50000.00",
    "newClientOrderId": "my_bot_order_1"
  }'
```

Response (Binance schema):
```json
{
  "symbol": "BTCUSDT",
  "orderId": 1,
  "clientOrderId": "my_bot_order_1",
  "transactTime": 1727890123456,
  "price": "50000.00",
  "origQty": "5",
  "executedQty": "0",
  "status": "NEW",
  "timeInForce": "GTC",
  "type": "LIMIT",
  "side": "BUY",
  "fills": []
}
```

```bash
# Cancel it
curl -s -X DELETE "http://localhost:8080/api/v3/order?symbol=BTCUSDT&orderId=1"
```

### WebSocket Stream

```bash
# Subscribe to real-time depth updates and trade stream
wscat -c ws://localhost:8080/ws/stream
```

On connection you receive an immediate book snapshot, then live `depthUpdate` and `trade` events after every order mutation — matching the [Binance diff-depth stream](https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams) format.

---

## L3 Historical Replay

### Native C++ CLI Tool

```bash
# Basic replay
./build/release/quantengine_cli replay-l3 tests/fixtures/l3_sample.csv

# With Market Making strategy simulation and virtual wire latency
./build/release/quantengine_cli replay-l3 tests/fixtures/l3_sample.csv \
    --simulate-mm \
    --latency-feed 1000 \
    --latency-entry 2000 \
    --latency-resp 1000
```

Authentic CLI Output:
```
=========================================================================
            HISTORICAL L3 REPLAY & EXECUTION SIMULATION REPORT           
=========================================================================
 Feed Source:          tests/fixtures/l3_sample.csv
 Events Processed:     13
 Parse Errors Skipped: 0
 Replay Wall Time:     0.03 ms (0.46 Mops/sec)
 Virtual Event Span:   1200 ns (0.00 ms)
-------------------------------------------------------------------------
 EVENT BREAKDOWN:
   OrderAdded (A):        6
   OrderExecuted (E):     3
   OrderCancelled (C):    1
   OrderReplaced (R):     1
   TradeMessage (T):      2
-------------------------------------------------------------------------
 FINAL BOOK RECONSTRUCTION:
   Resting Orders:        4
   Total Bid Volume:      225
   Total Ask Volume:      125
   Best Bid Price (Qty):  28000 (200)
   Best Ask Price (Qty):  15050 (75)
   Inside Market Spread:  -12950 ticks

+-------------------------------------------------------------------------+
|                        ORDER BOOK DEPTH LADDER                          |
+--------------------+-----------------+-----------------+----------------+
|   Bid Orders (Vol) |       Bid Price |       Ask Price | Ask Orders(Vol)|
+--------------------+-----------------+-----------------+----------------+
|            1 (200) |           28000 |           15050 |         1 (75) |
|             1 (25) |           15000 |           15075 |         1 (50) |
+--------------------+-----------------+-----------------+----------------+

-------------------------------------------------------------------------
 VIRTUAL LATENCY CONFIGURATION:
   Feed Latency:          1000 ns
   Entry Wire Latency:    2000 ns
   Response Wire Latency: 1000 ns
   Round-Trip Latency:    3000 ns
-------------------------------------------------------------------------
 PORTFOLIO & QUEUE SIMULATION:
   Simulated Orders:      4
   Active Resting:        2
   Portfolio Realized P&L:0 ticks
   Open Orders Tracked:   2
-------------------------------------------------------------------------
 DETERMINISTIC STATE & VERIFICATION:
   Invariant Audit:       PASSED (ZERO ANOMALIES)
   Canonical 64-bit Hash: 0x9d887bd855316601
=========================================================================
```

### Python Replay Utility

```bash
PYTHONPATH=build/release/python python3 historical_replay.py \
    --input tests/fixtures/l3_sample.csv \
    --output replay_result.json \
    --tick-size 0.01 \
    --symbol BTCUSDT
```

Output JSON contains throughput metrics, periodic depth snapshots, the canonical hash, and invariant verification.

### L3 Canonical Message Format

```csv
# Canonical QuantEngine L3 Feed Format
A,1000000000,1001,AAPL,15000,100,B
A,1000000100,1002,AAPL,15000,50,B
E,1000000400,1001,AAPL,75,5001
C,1000000600,1002,AAPL,50
R,1000000700,1003,1005,AAPL,15025,100,S
```

| Type Code | Name | Format Layout | Description |
|-----------|------|---------------|-------------|
| `A` | **OrderAdded** | `A,ts_ns,order_id,symbol,price_ticks,quantity,side` | New limit order inserted into the book |
| `E` | **OrderExecuted** | `E,ts_ns,order_id,symbol,executed_qty,match_number` | Execution against resting liquidity |
| `C` | **OrderCancelled** | `C,ts_ns,order_id,symbol,cancelled_qty` | Cancellation / withdrawal of resting volume |
| `R` | **OrderReplaced** | `R,ts_ns,old_id,new_id,symbol,new_price,new_qty,side` | Order modification / replacement |
| `T` | **TradeMessage** | `T,ts_ns,match_number,symbol,price,qty,aggressor_side,cond` | Public tape trade report |

---

## Benchmarks

All benchmarks run on: **AMD Ryzen 7 7840HS, GCC 13.3.0, `-O3 -march=native`**, Release build, isolated core.

### Matching Engine

| Scenario | Throughput | Mean Latency | p99 Latency |
|----------|-----------|-------------|------------|
| Balanced book (50% cross) | **11.79 Mops/s** | 84.8 ns | 281 ns |
| Crossing orders (full fill) | **15.14 Mops/s** | 66.0 ns | 218 ns |
| Cancel-heavy workload | **13.41 Mops/s** | 74.6 ns | 244 ns |

### L3 Replay Pipeline

| Stage | Events | Throughput |
|-------|--------|-----------|
| CSV parsing | 1M lines | **14.51 Mlines/s** |
| Book reconstruction | 100k events | **7.08 Mevents/s** |
| Full pipeline (book + queue + simulator) | 100k events | **7.15 Mevents/s** |
| Full pipeline (1M events) | 1M events | **7.22 Mevents/s** |

### Memory

| Component | Allocation strategy | Heap calls (steady state) |
|-----------|-------------------|--------------------------|
| `OptimizedOrderBook` | Arena pool (65,536 slots × 64 B) | **Zero** |
| `ReferenceOrderBook` | `std::map` + `std::list` | O(log N) per insert |
| L3 event vector | `std::vector` (pre-reserved) | Zero after initial reserve |

---

## Local HFT Bot Backtesting

QuantEngine is designed to be the fastest path from a tick data CSV to an honest fill simulation:

```
┌─────────────────────────────────────────────────────┐
│  1. Acquire L3 tick data (e.g. Tardis.dev CSV)      │
│  2. Run historical_replay.py to seed the engine     │
│  3. Point your bot at server.py (REST + WebSocket)  │
│  4. Compare canonical hashes across runs for        │
│     determinism verification                        │
└─────────────────────────────────────────────────────┘
```

**Determinism guarantee:** Two runs of `historical_replay.py` on the same CSV with the same strategy produce the exact same canonical hash. This is enforced by the engine's `ReplayEngine::verify_determinism()` cross-engine test.

### Setting Up for Algo Testing

```bash
# 1. Start the mock exchange
PYTHONPATH=build/release/python python3 server.py --tick-size 0.01 &

# 2. Pump historical data through the engine via the replay CLI
./build/release/quantengine_cli replay-l3 tests/fixtures/l3_sample.csv

# 3. Connect your trading bot
# python my_bot.py --host 127.0.0.1 --port 8080
# (uses the standard Binance SDK — no modification needed)

# 4. Verify determinism
python3 -c "
import quantengine as qe
result1 = qe.ReplayEngine.replay_optimized(commands)
result2 = qe.ReplayEngine.replay_optimized(commands)
assert result1.final_hash == result2.final_hash, 'Non-determinism detected!'
print(f'Hash: 0x{result1.final_hash:016x} — determinism verified.')
"
```

---

## Simulation Limitations

> [!IMPORTANT]
> QuantEngine produces a **deterministic simulation**, not a live market. The following factors are **not modelled** and will cause simulation results to deviate from live trading outcomes. Understand these before making real capital decisions.

### What Is Not Modelled

| Limitation | Impact | Severity |
|-----------|--------|----------|
| **Hidden/iceberg liquidity** | Displayed depth underestimates true available liquidity; fills may be faster in reality | High |
| **Off-exchange internalization** | Market makers route retail flow internally; aggressive orders may fill at mid without touching the displayed book | High |
| **Last-look and quote-stuffing** | Liquidity providers can withdraw quotes after seeing a taker order; fills may reject in live | Medium |
| **Adverse selection bias** | If you are consistently filling, you are trading against better-informed participants; simulation does not capture post-fill mean reversion | Medium |
| **Fee structure** | Maker/taker rebates and tier discounts are not applied to P&L | Medium |
| **Network jitter** | The latency model uses deterministic values; real-world variance (tail latency, packet loss) is not simulated | Low |
| **Corporate actions / circuit breakers** | Halts, limit-up/down, and volatility auctions are not modelled | Low |
| **Cross-venue arbitrage** | The engine models a single venue; multi-leg strategies across exchanges cannot be accurately evaluated | Low |

### Appropriate Use Cases

✅ **Good for:**
- Testing order routing and order management logic
- Verifying that your bot correctly handles partial fills, cancels, and rejects
- Measuring relative performance of strategy variants in controlled conditions
- Stress-testing latency and throughput of your execution infrastructure

❌ **Not suitable for:**
- Estimating absolute P&L or Sharpe ratio of live strategies
- Optimising fill rate on passive orders (queue position estimates are approximate)
- Any strategy that relies on dark pool, internalised, or OTC liquidity

---

## Docker Deployment

```bash
# Build and start the exchange in one command
docker compose up --build

# Run a historical replay job
docker compose --profile replay up replay

# Check exchange health
curl http://localhost:8080/api/v3/ping
```

The multi-stage `Dockerfile` uses `ubuntu:24.04` to compile the C++ engine and `python:3.12-slim` as the runtime — the final image is approximately 180 MB.

---

## Commercial Licensing

QuantEngine is dual-licensed:

| License | Use Case | Cost |
|---------|----------|------|
| **AGPLv3** (this repo) | Open-source projects, academic research, personal use | Free |
| **Commercial License** | Proprietary trading systems, prop shops, hedge funds, SaaS products that do not wish to open-source their modifications | Contact us |

The AGPL requires that any software that incorporates or communicates with QuantEngine over a network must also be released under the AGPL. If this is incompatible with your business model, a commercial license removes this obligation.

**To acquire a commercial license:**
- Email: **commercial@quantengine.io**
- Subject: `Commercial License Enquiry — [Your Company]`
- Include: intended use case, expected order volume, number of seats

---

## Development

### CMake Presets

| Preset | Description | Output directory |
|--------|-------------|-----------------|
| `dev-release` | `-O3 -march=native`, no sanitizers | `build/release/` |
| `dev-sanitizer` | `-O1 -fsanitize=address,undefined` | `build/dev-sanitizer/` |
| `dev-debug` | `-O0 -g`, full symbols | `build/debug/` |

### Running Tests

```bash
# C++ Test Binary (all 248 tests)
./build/release/tests/quantengine_tests

# CTest Suite (release)
ctest --preset dev-release --output-on-failure

# Python Tests (all 63 tests)
PYTHONPATH=build/release/python python3 -m pytest tests/python -v
```

### Code Formatting

```bash
# Reformat all C++ sources (requires clang-format 17+)
find include src tests benchmarks bindings -name '*.hpp' -o -name '*.cpp' \
  | xargs clang-format -i
```

---

## Project Milestones

- [x] Phase 1 — Core matching engine: `GenericMatchingEngine`, `OptimizedOrderBook`, `ReferenceOrderBook`, invariant auditor, canonical state hash
- [x] Phase 2 — L3 canonical events: `L3Message`, `IL3MarketDataSource`, `L3CsvParser`, adversarial tests
- [x] Phase 3 — Historical book & queue model: `HistoricalL3Book`, `FifoQueueModel`, `QueuePositionTracker`, differential tests
- [x] Phase 4 — Virtual time & execution: `EventClock`, `LatencyModel`, `VirtualTimeline`, `ReplayGateway`, Portfolio integration
- [x] Phase 5 — CLI, benchmarks, strategy experiment: `quantengine_cli replay-l3`, 14+ Mlines/s parsing, 7+ Mevents/s reconstruction
- [x] Phase 6 — Code polish & documentation: clang-format, 248/248 tests, zero-warning build under `-Werror`
- [x] **Phase 7 — Exchange API & commercialisation:** FastAPI REST+WebSocket server, cross-platform wheel CI, Docker, `historical_replay.py`, AGPLv3 dual-licensing

---

## License

```
QuantEngine — High-Performance C++20 Deterministic Limit Order Matching Engine
Copyright (C) 2024 QuantEngine Authors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program. If not, see <https://www.gnu.org/licenses/>.

For commercial licensing, contact: commercial@quantengine.io
```
