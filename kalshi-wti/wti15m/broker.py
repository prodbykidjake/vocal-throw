"""Your Kalshi account: signed requests, orders, fills, positions, balance.

Authentication (Kalshi trade-api v2): every request carries
    KALSHI-ACCESS-KEY        the API key id
    KALSHI-ACCESS-TIMESTAMP  milliseconds since the epoch
    KALSHI-ACCESS-SIGNATURE  base64( RSA-PSS-SHA256( timestamp + METHOD + "/trade-api/v2" + path ) )
with the PSS salt length equal to the digest length and the query string left out of the signed path.

Orders (v2, POST /portfolio/events/orders) are expressed on the YES price: `side` is "bid" (buy Yes / sell No) or
"ask" (sell Yes / buy No), `price` and `count` are fixed-point strings ("0.3100", "16.13"). The legacy endpoint
(/portfolio/orders: action buy/sell, side yes/no, yes_price_dollars, count_fp) is kept as a fallback.
Prices here are always in the SIDE's own terms (a DOWN contract at 31¢ is price 0.31); the mapping lives in one place.

Nothing in this module decides anything. The AutoTrader does.
"""
from __future__ import annotations

import base64
import logging
import math
import pathlib
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable
from urllib.parse import urlparse

import httpx

log = logging.getLogger(__name__)


class BrokerError(RuntimeError):
    def __init__(self, msg: str, status: int | None = None, body: str = ""):
        super().__init__(msg)
        self.status = status
        self.body = body


@dataclass
class OrderResult:
    order_id: str
    client_order_id: str
    status: str  # resting | executed | canceled | pending | unknown
    filled: float = 0.0  # contracts filled so far
    remaining: float = 0.0
    avg_price: float | None = None  # in the side's own terms, when known
    fees: float = 0.0  # dollars, when known
    raw: dict = field(default_factory=dict, repr=False)

    @property
    def done(self) -> bool:
        return self.status in ("executed", "canceled", "cancelled", "expired") or (self.remaining <= 1e-9 and self.filled > 0)


@dataclass
class AccountPosition:
    ticker: str
    side: str | None  # UP | DOWN | None (flat)
    qty: float  # contracts held (always >= 0)
    exposure: float  # dollars at risk (Kalshi's market_exposure)
    realized_pnl: float = 0.0
    fees_paid: float = 0.0
    resting_orders: int = 0

    @property
    def avg_price(self) -> float | None:
        return (self.exposure / self.qty) if self.qty > 0 and self.exposure > 0 else None


# ----------------------------------------------------------------------------- helpers

def _num(d: dict, *keys, default: float | None = None, cents_keys: tuple[str, ...] = ()) -> float | None:
    """First present numeric among keys; `cents_keys` are legacy integer-cent fields divided by 100."""
    for key in keys:
        value = d.get(key)
        if value in (None, ""):
            continue
        try:
            f = float(str(value).replace(",", ""))
        except (TypeError, ValueError):
            continue
        return f / 100.0 if key in cents_keys else f
    return default


def fmt_price(price: float, direction: str = "nearest") -> str:
    """Kalshi price string. Ticks: 0.1¢ under 5¢ (sub-penny band), 1¢ elsewhere. `direction` rounds a BUY
    limit up and a SELL limit down so the order is never rejected for sitting between ticks."""
    tick = 0.001 if price < 0.05 else 0.01
    units = price / tick
    if direction == "up":
        n = math.ceil(units - 1e-9)
    elif direction == "down":
        n = math.floor(units + 1e-9)
    else:
        n = round(units)
    p = max(tick, min(1.0 - tick, n * tick))
    return f"{p:.4f}"


def fmt_count(count: float, fractional: bool) -> str:
    if fractional:
        n = math.floor(count * 100 + 1e-9) / 100.0
        return f"{max(0.01, n):.2f}"
    return f"{max(1, int(math.floor(count + 1e-9)))}"


def new_client_order_id() -> str:
    return str(uuid.uuid4())


# ----------------------------------------------------------------------------- auth

class KalshiAuth:
    def __init__(self, key_id: str, private_key_path: str, prefix: str = "/trade-api/v2"):
        from cryptography.hazmat.primitives import serialization

        if not key_id or not private_key_path:
            raise BrokerError("auto trading needs [auto] api_key_id and private_key_path (or the KALSHI_* env vars)")
        path = pathlib.Path(private_key_path).expanduser()
        if not path.is_file():
            raise BrokerError(f"private key file not found: {path}")
        try:
            if path.stat().st_mode & 0o077:
                log.warning("private key %s is readable by other users on this Mac; run: chmod 600 %s", path, path)
        except OSError:
            pass
        self.key_id = key_id
        self.prefix = prefix.rstrip("/")
        self._key = serialization.load_pem_private_key(path.read_bytes(), password=None)

    def sign(self, timestamp_ms: str, method: str, path: str) -> str:
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import padding

        msg = f"{timestamp_ms}{method.upper()}{self.prefix}{path}".encode("utf-8")
        sig = self._key.sign(msg, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.DIGEST_LENGTH),
                             hashes.SHA256())
        return base64.b64encode(sig).decode("ascii")

    def headers(self, method: str, path: str) -> dict[str, str]:
        ts = str(int(time.time() * 1000))
        return {"KALSHI-ACCESS-KEY": self.key_id, "KALSHI-ACCESS-TIMESTAMP": ts,
                "KALSHI-ACCESS-SIGNATURE": self.sign(ts, method, path)}


# ----------------------------------------------------------------------------- side mapping

def to_yes_terms(side: str, action: str, price: float) -> tuple[str, float]:
    """(v2 book side, yes price) for buying/selling an UP or DOWN contract at `price` in its own terms.
    Buying Down at 31¢ is selling Yes at 69¢ on Kalshi's single book."""
    if side == "UP":
        return ("bid" if action == "buy" else "ask"), price
    return ("ask" if action == "buy" else "bid"), round(1.0 - price, 6)


def parse_order(d: dict, side: str | None = None) -> OrderResult:
    o = d.get("order", d) if isinstance(d, dict) else {}
    if not isinstance(o, dict):
        o = {}
    status = str(o.get("status") or "unknown").lower()
    filled = _num(o, "fill_count_fp", "fill_count", default=0.0) or 0.0
    remaining = _num(o, "remaining_count_fp", "remaining_count", default=0.0) or 0.0
    fees = (_num(o, "taker_fees_dollars", "taker_fees", default=0.0, cents_keys=("taker_fees",)) or 0.0) + \
           (_num(o, "maker_fees_dollars", "maker_fees", default=0.0, cents_keys=("maker_fees",)) or 0.0)
    avg = None
    cost = (_num(o, "taker_fill_cost_dollars", "taker_fill_cost", default=0.0, cents_keys=("taker_fill_cost",)) or 0.0) + \
           (_num(o, "maker_fill_cost_dollars", "maker_fill_cost", default=0.0, cents_keys=("maker_fill_cost",)) or 0.0)
    if filled > 0 and cost > 0:
        avg = cost / filled  # Kalshi reports the cost in the order's own side terms
    return OrderResult(str(o.get("order_id") or o.get("id") or ""), str(o.get("client_order_id") or ""), status, filled,
                       remaining, avg, fees, o)


def parse_positions(d: dict) -> list[AccountPosition]:
    out = []
    for p in (d.get("market_positions") or []) if isinstance(d, dict) else []:
        qty = _num(p, "position_fp", "position", default=0.0) or 0.0
        side = "UP" if qty > 1e-9 else "DOWN" if qty < -1e-9 else None
        out.append(AccountPosition(str(p.get("ticker") or ""), side, abs(qty),
                                   abs(_num(p, "market_exposure_dollars", "market_exposure", default=0.0, cents_keys=("market_exposure",)) or 0.0),
                                   _num(p, "realized_pnl_dollars", "realized_pnl", default=0.0, cents_keys=("realized_pnl",)) or 0.0,
                                   _num(p, "fees_paid_dollars", "fees_paid", default=0.0, cents_keys=("fees_paid",)) or 0.0,
                                   int(_num(p, "resting_orders_count", default=0) or 0)))
    return out


def parse_fills(d: dict, side: str) -> list[dict]:
    """Fills normalised to the side's own terms: [{count, price, fee, is_taker, order_id, ts}]."""
    out = []
    for f in (d.get("fills") or []) if isinstance(d, dict) else []:
        count = _num(f, "count_fp", "count", default=0.0) or 0.0
        yes_px = _num(f, "yes_price_dollars", "yes_price", cents_keys=("yes_price",))
        no_px = _num(f, "no_price_dollars", "no_price", cents_keys=("no_price",))
        if side == "UP":
            px = yes_px if yes_px is not None else (None if no_px is None else 1.0 - no_px)
        else:
            px = no_px if no_px is not None else (None if yes_px is None else 1.0 - yes_px)
        if count <= 0 or px is None:
            continue
        out.append({"count": count, "price": px, "fee": _num(f, "fee_dollars", "fee", "fee_cost", default=0.0, cents_keys=("fee", "fee_cost")) or 0.0,
                    "is_taker": bool(f.get("is_taker", True)), "order_id": str(f.get("order_id") or ""),
                    "ts": f.get("created_time"), "trade_id": str(f.get("trade_id") or "")})
    return out


# ----------------------------------------------------------------------------- live broker

class KalshiBroker:
    def __init__(self, base_url: str, auth: KalshiAuth, timeout: float = 10.0, order_api: str = "v2",
                 fractional: bool = True, client: httpx.AsyncClient | None = None):
        self.base_url = base_url.rstrip("/")
        self.auth = auth
        self.order_api = order_api
        self.fractional = fractional
        self._client = client or httpx.AsyncClient(timeout=timeout, headers={"Accept": "application/json",
                                                                              "User-Agent": "wti15m-coach/0.1"})
        self._owns = client is None
        self.name = "kalshi"
        self.last_error: str | None = None

    async def aclose(self):
        if self._owns:
            await self._client.aclose()

    async def _request(self, method: str, path: str, params: dict | None = None, body: dict | None = None) -> Any:
        url = f"{self.base_url}{path}"
        headers = self.auth.headers(method, path)
        for attempt in range(2):
            try:
                resp = await self._client.request(method, url, params=params, json=body, headers=headers)
            except httpx.HTTPError as exc:
                self.last_error = f"{method} {path}: {exc}"
                raise BrokerError(self.last_error) from exc
            if resp.status_code == 429 and attempt == 0:
                import asyncio
                await asyncio.sleep(1.0)
                headers = self.auth.headers(method, path)
                continue
            break
        if resp.status_code >= 300:
            text = resp.text[:300]
            self.last_error = f"{method} {path} -> HTTP {resp.status_code}: {text}"
            raise BrokerError(self.last_error, resp.status_code, text)
        self.last_error = None
        if not resp.content:
            return {}
        try:
            return resp.json()
        except ValueError as exc:
            raise BrokerError(f"{method} {path}: invalid JSON") from exc

    async def exchange_status(self) -> dict:
        return await self._request("GET", "/exchange/status")

    async def balance(self) -> float:
        d = await self._request("GET", "/portfolio/balance")
        bal = _num(d, "balance_dollars")
        if bal is None:
            bal = _num(d, "balance", default=0.0, cents_keys=("balance",))
        return float(bal or 0.0)

    async def positions(self, ticker: str | None = None) -> list[AccountPosition]:
        params: dict = {"limit": 200}
        if ticker:
            params["ticker"] = ticker
        d = await self._request("GET", "/portfolio/positions", params)
        return parse_positions(d)

    async def place(self, ticker: str, side: str, action: str, price: float, count: float, tif: str = "good_till_canceled",
                    ttl_s: float | None = None, client_order_id: str | None = None, reduce_only: bool = False) -> OrderResult:
        """Limit order to buy/sell `count` contracts of UP/DOWN at `price` (the side's own terms, dollars)."""
        if side not in ("UP", "DOWN") or action not in ("buy", "sell"):
            raise BrokerError(f"bad order {side} {action}")
        coid = client_order_id or new_client_order_id()
        count_s = fmt_count(count, self.fractional)
        if self.order_api == "legacy":
            body: dict = {"ticker": ticker, "client_order_id": coid, "action": action, "side": "yes" if side == "UP" else "no",
                          "type": "limit", "count_fp": count_s,
                          ("yes_price_dollars" if side == "UP" else "no_price_dollars"): fmt_price(price, "up" if action == "buy" else "down")}
            if tif in ("immediate_or_cancel", "fill_or_kill"):
                body["time_in_force"] = tif
            elif ttl_s:
                body["expiration_ts"] = int(time.time() + ttl_s)
            if reduce_only:
                body["sell_position_capped"] = True
            d = await self._request("POST", "/portfolio/orders", body=body)
            return parse_order(d, side)
        book_side, yes_px = to_yes_terms(side, action, price)
        # rounding on the yes grid: a buy must not end up cheaper than the ask it is meant to take
        direction = "up" if (action == "buy") == (side == "UP") else "down"
        body = {"ticker": ticker, "client_order_id": coid, "side": book_side, "count": count_s,
                "price": fmt_price(yes_px, direction), "time_in_force": tif}
        if tif == "good_till_canceled" and ttl_s:
            body["expiration_time"] = int(time.time() + ttl_s)
        if reduce_only:
            body["reduce_only"] = True
        try:
            d = await self._request("POST", "/portfolio/events/orders", body=body)
        except BrokerError as exc:
            if exc.status in (404, 405) and self.order_api == "v2":
                log.warning("v2 order endpoint unavailable (%s); switching to the legacy order API", exc)
                self.order_api = "legacy"
                return await self.place(ticker, side, action, price, count, tif, ttl_s, coid, reduce_only)
            raise
        res = parse_order(d, side)
        if not res.client_order_id:
            res.client_order_id = coid
        return res

    async def order(self, order_id: str, side: str | None = None) -> OrderResult:
        d = await self._request("GET", f"/portfolio/orders/{order_id}")
        return parse_order(d, side)

    async def cancel(self, order_id: str) -> OrderResult | None:
        paths = ((f"/portfolio/events/orders/{order_id}", f"/portfolio/orders/{order_id}") if self.order_api == "v2"
                 else (f"/portfolio/orders/{order_id}",))
        last: BrokerError | None = None
        for path in paths:
            try:
                d = await self._request("DELETE", path)
                return parse_order(d)
            except BrokerError as exc:
                last = exc  # try the other shape of the cancel endpoint before giving up
        if last is not None:
            raise last
        return None

    async def fills(self, ticker: str | None = None, order_id: str | None = None, side: str = "UP", limit: int = 100) -> list[dict]:
        params: dict = {"limit": limit}
        if ticker:
            params["ticker"] = ticker
        if order_id:
            params["order_id"] = order_id
        d = await self._request("GET", "/portfolio/fills", params)
        return parse_fills(d, side)


# ----------------------------------------------------------------------------- simulated broker (demo / tests)

class SimBroker:
    """Fills against the simulated book: a buy fills at the ask when the limit reaches it, a sell at the bid.
    Resting orders fill later when the book crosses them (checked on every poll)."""

    def __init__(self, quotes_fn: Callable[[], dict | None], balance: float = 500.0, fractional: bool = True,
                 fee_fn: Callable[[float, float], float] | None = None):
        self.quotes_fn = quotes_fn  # -> {"ticker", "yes_bid", "yes_ask", "no_bid", "no_ask"} or None
        self.cash = balance
        self.fractional = fractional
        self.fee_fn = fee_fn or (lambda price, count: math.ceil(0.07 * count * price * (1 - price) * 100) / 100.0)
        self.orders: dict[str, dict] = {}
        self.pos: dict[str, dict] = {}  # ticker -> {"side", "qty", "exposure"}
        self.fills_log: list[dict] = []
        self.name = "sim"
        self.last_error = None
        self.order_api = "sim"

    async def aclose(self):
        return None

    async def exchange_status(self) -> dict:
        return {"exchange_active": True, "trading_active": True}

    async def balance(self) -> float:
        return round(self.cash, 2)

    def _quote(self, ticker: str, side: str) -> tuple[float | None, float | None]:
        q = self.quotes_fn()
        if not q or q.get("ticker") != ticker:
            return None, None
        return (q.get("yes_bid"), q.get("yes_ask")) if side == "UP" else (q.get("no_bid"), q.get("no_ask"))

    def _try_fill(self, o: dict):
        if o["status"] != "resting":
            return
        bid, ask = self._quote(o["ticker"], o["side"])
        if o["action"] == "buy":
            if ask is None or ask > o["price"] + 1e-9:
                return
            px = ask
            cost = px * o["remaining"]
            fee = self.fee_fn(px, o["remaining"])
            self.cash -= cost + fee
            p = self.pos.setdefault(o["ticker"], {"side": o["side"], "qty": 0.0, "exposure": 0.0})
            if p["qty"] > 0 and p["side"] != o["side"]:
                raise BrokerError("sim: would flip the position")
            p["side"], p["qty"], p["exposure"] = o["side"], p["qty"] + o["remaining"], p["exposure"] + cost
        else:
            if bid is None or bid < o["price"] - 1e-9:
                return
            p = self.pos.get(o["ticker"])
            if not p or p["side"] != o["side"] or p["qty"] <= 0:
                o["status"] = "canceled"
                return
            qty = min(o["remaining"], p["qty"]) if o.get("reduce_only") else o["remaining"]
            px = bid
            fee = self.fee_fn(px, qty)
            self.cash += px * qty - fee
            p["exposure"] *= (p["qty"] - qty) / p["qty"] if p["qty"] > 0 else 0.0
            p["qty"] -= qty
            o["remaining"] = qty  # what fills below
        o["filled"] += o["remaining"]
        o["fees"] += fee
        o["avg_price"] = px
        self.fills_log.append({"order_id": o["order_id"], "ticker": o["ticker"], "side": o["side"], "count": o["remaining"],
                               "price": px, "fee": fee, "is_taker": True, "ts": time.time()})
        o["remaining"] = 0.0
        o["status"] = "executed"

    async def place(self, ticker, side, action, price, count, tif="good_till_canceled", ttl_s=None, client_order_id=None,
                    reduce_only=False) -> OrderResult:
        oid = f"sim-{len(self.orders) + 1}"
        count = float(fmt_count(count, self.fractional))
        o = {"order_id": oid, "client_order_id": client_order_id or new_client_order_id(), "ticker": ticker, "side": side,
             "action": action, "price": float(fmt_price(price, "up" if action == "buy" else "down")), "count": count,
             "remaining": count, "filled": 0.0, "fees": 0.0, "avg_price": None, "status": "resting",
             "expires": (time.time() + ttl_s) if ttl_s else None, "tif": tif, "reduce_only": reduce_only}
        self.orders[oid] = o
        self._try_fill(o)
        if o["status"] == "resting" and tif in ("immediate_or_cancel", "fill_or_kill"):
            o["status"] = "canceled"
        return self._result(o)

    def _result(self, o: dict) -> OrderResult:
        return OrderResult(o["order_id"], o["client_order_id"], o["status"], o["filled"], o["remaining"], o["avg_price"], o["fees"], dict(o))

    async def order(self, order_id: str, side: str | None = None) -> OrderResult:
        o = self.orders[order_id]
        if o["status"] == "resting" and o["expires"] and time.time() > o["expires"]:
            o["status"] = "canceled"
        self._try_fill(o)
        return self._result(o)

    async def cancel(self, order_id: str) -> OrderResult | None:
        o = self.orders.get(order_id)
        if o and o["status"] == "resting":
            o["status"] = "canceled"
        return self._result(o) if o else None

    async def positions(self, ticker: str | None = None) -> list[AccountPosition]:
        out = []
        for t, p in self.pos.items():
            if ticker and t != ticker:
                continue
            out.append(AccountPosition(t, p["side"] if p["qty"] > 1e-9 else None, p["qty"], p["exposure"]))
        return out

    async def fills(self, ticker=None, order_id=None, side="UP", limit=100) -> list[dict]:
        return [f for f in self.fills_log if (not ticker or f["ticker"] == ticker) and (not order_id or f["order_id"] == order_id)][-limit:]

    def settle(self, ticker: str, result_up: bool):
        """Pay out a settled window (the demo calls this when the simulated market settles)."""
        p = self.pos.pop(ticker, None)
        if p and p["qty"] > 0 and (p["side"] == "UP") == result_up:
            self.cash += p["qty"]
