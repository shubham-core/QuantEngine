"""
tests/python/test_server.py
===========================
Integration tests for the FastAPI exchange server (server.py).

Uses FastAPI's built-in TestClient (httpx-based, sync) and
``pytest-asyncio`` for the WebSocket tests.

Run with:
    pytest tests/python/test_server.py -v
"""

from __future__ import annotations

import json
import threading
import time
from typing import Any, Dict

import pytest

# Guard: skip entire module if FastAPI or the quantengine extension is missing
pytest.importorskip("fastapi")
pytest.importorskip("quantengine")

from fastapi.testclient import TestClient  # noqa: E402

# Import the app *after* guard so pytest collects tests only when deps exist
from server import app, SYMBOL, ticks_to_str, str_to_ticks  # noqa: E402


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture()
def client():
    """Yield a TestClient with a fresh engine state for each test."""
    with TestClient(app) as c:
        # Reset engine to blank state before each test
        c.post("/qe/reset")
        yield c


# ---------------------------------------------------------------------------
# System endpoints
# ---------------------------------------------------------------------------

class TestSystemEndpoints:
    def test_ping(self, client: TestClient) -> None:
        r = client.get("/api/v3/ping")
        assert r.status_code == 200
        assert r.json() == {}

    def test_server_time(self, client: TestClient) -> None:
        r = client.get("/api/v3/time")
        assert r.status_code == 200
        data = r.json()
        assert "serverTime" in data
        assert abs(data["serverTime"] - int(time.time() * 1000)) < 5000

    def test_exchange_info(self, client: TestClient) -> None:
        r = client.get("/api/v3/exchangeInfo")
        assert r.status_code == 200
        info = r.json()
        assert info["symbols"][0]["symbol"] == SYMBOL
        assert "LIMIT" in info["symbols"][0]["orderTypes"]


# ---------------------------------------------------------------------------
# Market data endpoints (empty book)
# ---------------------------------------------------------------------------

class TestMarketDataEmpty:
    def test_depth_empty_book(self, client: TestClient) -> None:
        r = client.get(f"/api/v3/depth?symbol={SYMBOL}")
        assert r.status_code == 200
        data = r.json()
        assert data["bids"] == []
        assert data["asks"] == []

    def test_depth_unknown_symbol(self, client: TestClient) -> None:
        r = client.get("/api/v3/depth?symbol=ETHUSDT")
        assert r.status_code == 400

    def test_trades_empty(self, client: TestClient) -> None:
        r = client.get(f"/api/v3/trades?symbol={SYMBOL}")
        assert r.status_code == 200
        assert r.json() == []

    def test_book_ticker_empty(self, client: TestClient) -> None:
        r = client.get(f"/api/v3/ticker/bookTicker?symbol={SYMBOL}")
        assert r.status_code == 200
        data = r.json()
        assert data["bidPrice"] == "0.00000000"
        assert data["askPrice"] == "0.00000000"

    def test_open_orders_empty(self, client: TestClient) -> None:
        r = client.get(f"/api/v3/openOrders?symbol={SYMBOL}")
        assert r.status_code == 200
        assert r.json() == []


# ---------------------------------------------------------------------------
# Order placement
# ---------------------------------------------------------------------------

class TestOrderPlacement:
    def _place(
        self,
        client: TestClient,
        side: str = "BUY",
        price: str = "50000.00",
        qty: str = "1",
        cid: str | None = None,
    ) -> Dict[str, Any]:
        payload: Dict[str, Any] = {
            "symbol": SYMBOL,
            "side": side,
            "type": "LIMIT",
            "timeInForce": "GTC",
            "quantity": qty,
            "price": price,
        }
        if cid:
            payload["newClientOrderId"] = cid
        r = client.post("/api/v3/order", json=payload)
        assert r.status_code == 200, r.text
        return r.json()

    def test_place_bid(self, client: TestClient) -> None:
        resp = self._place(client, "BUY", "50000.00", "5")
        assert resp["status"] in ("NEW", "PARTIALLY_FILLED", "FILLED")
        assert resp["side"] == "BUY"
        assert resp["symbol"] == SYMBOL
        assert resp["orderId"] >= 1

    def test_place_ask(self, client: TestClient) -> None:
        resp = self._place(client, "SELL", "51000.00", "3")
        assert resp["side"] == "SELL"

    def test_place_crossing_order_fills(self, client: TestClient) -> None:
        """A sell order crossing the best bid must produce a trade."""
        self._place(client, "BUY", "50000.00", "10", cid="bid1")
        resp = self._place(client, "SELL", "49000.00", "5", cid="ask1")
        # Aggressive sell crosses the buy -> should be filled
        assert resp["status"] in ("FILLED", "PARTIALLY_FILLED")
        assert int(resp["executedQty"]) >= 5

    def test_depth_after_resting_orders(self, client: TestClient) -> None:
        self._place(client, "BUY", "50000.00", "10")
        self._place(client, "SELL", "51000.00", "7")
        r = client.get(f"/api/v3/depth?symbol={SYMBOL}&limit=5")
        data = r.json()
        assert len(data["bids"]) == 1
        assert len(data["asks"]) == 1
        assert data["bids"][0][0] == "50000.00000000"
        assert data["asks"][0][0] == "51000.00000000"

    def test_duplicate_client_order_id_rejected(self, client: TestClient) -> None:
        self._place(client, "BUY", "50000.00", "5", cid="dup_id")
        r = client.post(
            "/api/v3/order",
            json={
                "symbol": SYMBOL,
                "side": "BUY",
                "type": "LIMIT",
                "timeInForce": "GTC",
                "quantity": "5",
                "price": "50000.00",
                "newClientOrderId": "dup_id",
            },
        )
        assert r.status_code == 400

    def test_invalid_side_rejected(self, client: TestClient) -> None:
        r = client.post(
            "/api/v3/order",
            json={
                "symbol": SYMBOL,
                "side": "LONG",  # invalid
                "type": "LIMIT",
                "timeInForce": "GTC",
                "quantity": "5",
                "price": "50000.00",
            },
        )
        assert r.status_code == 422  # FastAPI validation error

    def test_zero_quantity_rejected(self, client: TestClient) -> None:
        r = client.post(
            "/api/v3/order",
            json={
                "symbol": SYMBOL,
                "side": "BUY",
                "type": "LIMIT",
                "timeInForce": "GTC",
                "quantity": "0",
                "price": "50000.00",
            },
        )
        assert r.status_code == 422

    def test_negative_price_rejected(self, client: TestClient) -> None:
        r = client.post(
            "/api/v3/order",
            json={
                "symbol": SYMBOL,
                "side": "BUY",
                "type": "LIMIT",
                "timeInForce": "GTC",
                "quantity": "5",
                "price": "-1.00",
            },
        )
        assert r.status_code == 422

    def test_market_order_type_rejected(self, client: TestClient) -> None:
        r = client.post(
            "/api/v3/order",
            json={
                "symbol": SYMBOL,
                "side": "BUY",
                "type": "MARKET",
                "timeInForce": "GTC",
                "quantity": "5",
                "price": "50000.00",
            },
        )
        assert r.status_code == 422


# ---------------------------------------------------------------------------
# Order query and cancel
# ---------------------------------------------------------------------------

class TestOrderLifecycle:
    def _place(self, client: TestClient, **kwargs: Any) -> Dict[str, Any]:
        defaults = {
            "symbol": SYMBOL,
            "side": "BUY",
            "type": "LIMIT",
            "timeInForce": "GTC",
            "quantity": "5",
            "price": "50000.00",
        }
        defaults.update(kwargs)
        r = client.post("/api/v3/order", json=defaults)
        assert r.status_code == 200
        return r.json()

    def test_query_by_order_id(self, client: TestClient) -> None:
        resp = self._place(client)
        oid = resp["orderId"]
        r = client.get(f"/api/v3/order?symbol={SYMBOL}&orderId={oid}")
        assert r.status_code == 200
        data = r.json()
        assert data["orderId"] == oid

    def test_query_by_client_id(self, client: TestClient) -> None:
        resp = self._place(client, newClientOrderId="my_order_1")
        r = client.get(f"/api/v3/order?symbol={SYMBOL}&origClientOrderId=my_order_1")
        assert r.status_code == 200
        assert r.json()["clientOrderId"] == "my_order_1"

    def test_cancel_resting_order(self, client: TestClient) -> None:
        resp = self._place(client, newClientOrderId="cancel_me")
        oid = resp["orderId"]
        assert resp["status"] == "NEW"

        r = client.delete(f"/api/v3/order?symbol={SYMBOL}&orderId={oid}")
        assert r.status_code == 200
        assert r.json()["status"] == "CANCELED"

    def test_cancel_unknown_order_rejected(self, client: TestClient) -> None:
        r = client.delete(f"/api/v3/order?symbol={SYMBOL}&orderId=99999")
        assert r.status_code == 400

    def test_cancel_filled_order_rejected(self, client: TestClient) -> None:
        """Cancelling an already-filled order should return 400."""
        # Place a bid and immediately cross it with a sell
        self._place(client, side="BUY", price="50000.00", quantity="5",
                    newClientOrderId="bid_fill")
        ask = self._place(client, side="SELL", price="49000.00", quantity="5",
                          newClientOrderId="ask_fill")
        bid_id = client.get(
            f"/api/v3/order?symbol={SYMBOL}&origClientOrderId=bid_fill"
        ).json()["orderId"]
        # The bid should now be FILLED
        r = client.delete(f"/api/v3/order?symbol={SYMBOL}&orderId={bid_id}")
        # Filled order cancel should return 400
        assert r.status_code == 400

    def test_open_orders_list(self, client: TestClient) -> None:
        self._place(client, newClientOrderId="open1")
        self._place(client, price="49000.00", newClientOrderId="open2")
        r = client.get(f"/api/v3/openOrders?symbol={SYMBOL}")
        assert r.status_code == 200
        ids = {o["clientOrderId"] for o in r.json()}
        assert "open1" in ids
        assert "open2" in ids


# ---------------------------------------------------------------------------
# Trade history
# ---------------------------------------------------------------------------

class TestTradeHistory:
    def test_trades_populated_after_fill(self, client: TestClient) -> None:
        client.post(
            "/api/v3/order",
            json={
                "symbol": SYMBOL, "side": "BUY", "type": "LIMIT",
                "timeInForce": "GTC", "quantity": "10", "price": "50000.00",
            },
        )
        client.post(
            "/api/v3/order",
            json={
                "symbol": SYMBOL, "side": "SELL", "type": "LIMIT",
                "timeInForce": "GTC", "quantity": "5", "price": "49000.00",
            },
        )
        r = client.get(f"/api/v3/trades?symbol={SYMBOL}")
        assert r.status_code == 200
        trades = r.json()
        assert len(trades) >= 1
        assert "price" in trades[0]
        assert "qty" in trades[0]


# ---------------------------------------------------------------------------
# Diagnostics
# ---------------------------------------------------------------------------

class TestDiagnostics:
    def test_audit_empty_engine(self, client: TestClient) -> None:
        r = client.get("/qe/audit")
        assert r.status_code == 200
        data = r.json()
        assert data["invariants_ok"] is True
        assert "canonical_hash" in data

    def test_audit_after_orders(self, client: TestClient) -> None:
        client.post(
            "/api/v3/order",
            json={
                "symbol": SYMBOL, "side": "BUY", "type": "LIMIT",
                "timeInForce": "GTC", "quantity": "5", "price": "50000.00",
            },
        )
        r = client.get("/qe/audit")
        assert r.json()["invariants_ok"] is True

    def test_reset_clears_state(self, client: TestClient) -> None:
        client.post(
            "/api/v3/order",
            json={
                "symbol": SYMBOL, "side": "BUY", "type": "LIMIT",
                "timeInForce": "GTC", "quantity": "5", "price": "50000.00",
            },
        )
        client.post("/qe/reset")
        r = client.get(f"/api/v3/depth?symbol={SYMBOL}")
        data = r.json()
        assert data["bids"] == []
        assert data["asks"] == []


# ---------------------------------------------------------------------------
# Price conversion helpers
# ---------------------------------------------------------------------------

class TestPriceHelpers:
    def test_ticks_to_str_roundtrip(self) -> None:
        assert ticks_to_str(5000000) == "50000.00000000"
        assert ticks_to_str(1) == "0.01000000"

    def test_str_to_ticks_roundtrip(self) -> None:
        assert str_to_ticks("50000.00") == 5_000_000
        assert str_to_ticks("0.01") == 1

    def test_extreme_price(self) -> None:
        assert str_to_ticks("999999.99") == 99_999_999


# ---------------------------------------------------------------------------
# WebSocket stream tests
# ---------------------------------------------------------------------------

class TestWebSocket:
    def test_ws_initial_snapshot(self, client: TestClient) -> None:
        with client.websocket_connect("/ws/stream") as ws:
            data = ws.receive_json()
            assert data["e"] == "depthUpdate"
            assert data["s"] == SYMBOL
            assert "b" in data
            assert "a" in data

    def test_ws_ping_pong(self, client: TestClient) -> None:
        with client.websocket_connect("/ws/stream") as ws:
            _ = ws.receive_json()  # initial snapshot
            ws.send_text("ping")
            resp = ws.receive_json()
            assert resp.get("e") == "pong"
