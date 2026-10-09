import base64
import json
import time

import httpx
import pytest

from wti15m.broker import (BrokerError, KalshiAuth, KalshiBroker, SimBroker, fmt_count, fmt_price, parse_fills, parse_order,
                           parse_positions, to_yes_terms)


def test_price_and_count_formatting():
    assert fmt_price(0.31, "up") == "0.3100"
    assert fmt_price(0.314, "up") == "0.3200"  # a buy limit rounds up to the next cent
    assert fmt_price(0.314, "down") == "0.3100"  # a sell limit rounds down
    assert fmt_price(0.0123, "up") == "0.0130"  # sub-penny band: 0.1 cent ticks
    assert fmt_price(0.0123, "down") == "0.0120"
    assert fmt_price(0.0, "down") == "0.0010" and fmt_price(1.2, "up") == "0.9900"
    assert fmt_count(133.3337, True) == "133.33" and fmt_count(0.001, True) == "0.01"
    assert fmt_count(133.9, False) == "133" and fmt_count(0.4, False) == "1"


def test_side_mapping_on_the_yes_book():
    assert to_yes_terms("UP", "buy", 0.31) == ("bid", 0.31)
    assert to_yes_terms("UP", "sell", 0.40) == ("ask", 0.40)
    assert to_yes_terms("DOWN", "buy", 0.31) == ("ask", 0.69)  # buying Down = selling Yes at 1 - price
    assert to_yes_terms("DOWN", "sell", 0.40) == ("bid", 0.60)


def test_parse_order_fixed_point_and_legacy():
    o = parse_order({"order": {"order_id": "abc", "client_order_id": "c1", "status": "executed", "fill_count_fp": "16.13",
                               "remaining_count_fp": "0.00", "taker_fill_cost_dollars": "4.8390", "taker_fees_dollars": "0.17"}})
    assert o.order_id == "abc" and o.status == "executed" and o.filled == 16.13 and o.remaining == 0.0 and o.done
    assert abs(o.avg_price - 0.30) < 1e-6 and o.fees == 0.17
    legacy = parse_order({"order": {"order_id": "x", "status": "resting", "fill_count": 5, "remaining_count": 5, "taker_fees": 7}})
    assert legacy.filled == 5 and legacy.remaining == 5 and not legacy.done and legacy.fees == 0.07
    v2 = parse_order({"order_id": "y", "status": "resting", "fill_count": "0.00", "remaining_count": "10.00"})
    assert v2.order_id == "y" and v2.remaining == 10.0 and v2.filled == 0.0


def test_parse_positions_and_fills():
    pos = parse_positions({"market_positions": [
        {"ticker": "T", "position_fp": "-16.13", "market_exposure_dollars": "4.84", "realized_pnl_dollars": "0", "fees_paid_dollars": "0.17"},
        {"ticker": "U", "position_fp": "0.00", "market_exposure_dollars": "0"}]})
    assert pos[0].side == "DOWN" and pos[0].qty == 16.13 and abs(pos[0].avg_price - 0.30) < 1e-3
    assert pos[1].side is None and pos[1].qty == 0
    fills = parse_fills({"fills": [{"order_id": "o", "count_fp": "10.00", "yes_price_dollars": "0.6900", "no_price_dollars": "0.3100",
                                    "is_taker": True, "fee_dollars": "0.15"}]}, "DOWN")
    assert fills[0]["price"] == 0.31 and fills[0]["count"] == 10.0 and fills[0]["fee"] == 0.15
    assert parse_fills({"fills": [{"count_fp": "1", "yes_price": 69}]}, "UP")[0]["price"] == 0.69


@pytest.fixture
def pem(tmp_path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    path = tmp_path / "k.pem"
    path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    return key, str(path)


def test_signature_covers_timestamp_method_and_full_path(pem):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding
    key, path = pem
    auth = KalshiAuth("key-id", path)
    h = auth.headers("POST", "/portfolio/events/orders")
    assert h["KALSHI-ACCESS-KEY"] == "key-id"
    ts = h["KALSHI-ACCESS-TIMESTAMP"]
    assert abs(int(ts) - time.time() * 1000) < 5000  # milliseconds
    msg = f"{ts}POST/trade-api/v2/portfolio/events/orders".encode()
    key.public_key().verify(base64.b64decode(h["KALSHI-ACCESS-SIGNATURE"]), msg,
                            padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.DIGEST_LENGTH), hashes.SHA256())
    with pytest.raises(BrokerError):
        KalshiAuth("", path)
    with pytest.raises(BrokerError):
        KalshiAuth("k", str(path) + ".missing")


@pytest.mark.asyncio
async def test_broker_sends_v2_orders_and_falls_back_to_legacy(pem):
    _, path = pem
    seen = []

    def handler(request: httpx.Request):
        seen.append((request.method, request.url.path, json.loads(request.content) if request.content else None,
                     request.headers.get("KALSHI-ACCESS-KEY")))
        if request.url.path.endswith("/portfolio/events/orders"):
            if handler.v2_ok:
                return httpx.Response(201, json={"order": {"order_id": "o1", "status": "resting", "fill_count": "0.00", "remaining_count": "16.13"}})
            return httpx.Response(404, json={"error": {"code": "not_found"}})
        if request.url.path.endswith("/portfolio/orders"):
            return httpx.Response(201, json={"order": {"order_id": "o2", "status": "executed", "fill_count_fp": "16.13", "remaining_count_fp": "0"}})
        if request.url.path.endswith("/portfolio/balance"):
            return httpx.Response(200, json={"balance": 12345})
        if request.url.path.endswith("/portfolio/orders/o1"):
            return httpx.Response(200, json={"order": {"order_id": "o1", "status": "canceled"}}) if request.method == "DELETE" else \
                httpx.Response(200, json={"order": {"order_id": "o1", "status": "resting", "fill_count_fp": "0", "remaining_count_fp": "16.13"}})
        if request.url.path.endswith("/portfolio/events/orders/o1"):
            return httpx.Response(405)
        return httpx.Response(500, text="boom")
    handler.v2_ok = True
    client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    b = KalshiBroker("https://api.elections.kalshi.com/trade-api/v2", KalshiAuth("kid", path), client=client)
    res = await b.place("KXWTI15M-X", "DOWN", "buy", 0.31, 16.13, "good_till_canceled", 20)
    method, p, body, key = seen[-1]
    assert (method, p, key) == ("POST", "/trade-api/v2/portfolio/events/orders", "kid")
    assert body["side"] == "ask" and body["price"] == "0.6900" and body["count"] == "16.13" and body["time_in_force"] == "good_till_canceled"
    assert body["expiration_time"] >= int(time.time()) + 19 and len(body["client_order_id"]) == 36
    assert res.order_id == "o1" and res.status == "resting" and res.remaining == 16.13
    sell = await b.place("KXWTI15M-X", "DOWN", "sell", 0.40, 16.13, "immediate_or_cancel", None, None, True)
    body = seen[-1][2]
    assert body["side"] == "bid" and body["price"] == "0.6000" and body["reduce_only"] is True and "expiration_time" not in body
    assert (await b.balance()) == 123.45
    got = await b.order("o1", "DOWN")
    assert got.status == "resting" and got.remaining == 16.13
    assert (await b.cancel("o1")).status == "canceled"  # v2 path 405 -> legacy cancel path
    # v2 gone: the broker switches to the legacy order API by itself
    handler.v2_ok = False
    res = await b.place("KXWTI15M-X", "UP", "buy", 0.31, 10, "good_till_canceled", 20)
    method, p, body, _ = seen[-1]
    assert p == "/trade-api/v2/portfolio/orders" and b.order_api == "legacy"
    assert body["action"] == "buy" and body["side"] == "yes" and body["yes_price_dollars"] == "0.3100" and body["count_fp"] == "10.00"
    assert res.order_id == "o2" and res.filled == 16.13
    with pytest.raises(BrokerError) as exc:
        await b.fills("T")
    assert exc.value.status == 500 and "boom" in str(exc.value)
    await b.aclose()


@pytest.mark.asyncio
async def test_sim_broker_fills_like_a_book():
    q = {"ticker": "T", "yes_bid": 0.60, "yes_ask": 0.63, "no_bid": 0.37, "no_ask": 0.40}
    b = SimBroker(lambda: q, balance=100.0)
    rest = await b.place("T", "DOWN", "buy", 0.38, 10, "good_till_canceled", 20)  # under the ask: rests
    assert rest.status == "resting" and rest.filled == 0
    q["no_ask"] = 0.38
    rest = await b.order(rest.order_id)
    assert rest.status == "executed" and rest.filled == 10 and rest.avg_price == 0.38
    pos = await b.positions("T")
    assert pos[0].side == "DOWN" and pos[0].qty == 10 and abs(pos[0].exposure - 3.8) < 1e-9
    assert (await b.balance()) < 100 - 3.8  # cost plus fee
    ioc = await b.place("T", "DOWN", "sell", 0.50, 10, "immediate_or_cancel", None, None, True)  # above the bid: nothing
    assert ioc.status == "canceled" and ioc.filled == 0
    sold = await b.place("T", "DOWN", "sell", 0.34, 10, "immediate_or_cancel", None, None, True)
    assert sold.status == "executed" and sold.filled == 10 and sold.avg_price == 0.37
    assert (await b.positions("T"))[0].qty == 0
    assert len(await b.fills("T")) == 2
