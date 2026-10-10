"""Kalshi public REST client (no API key needed for market data) + live-window tracking.

Base: https://api.elections.kalshi.com/trade-api/v2
Endpoints used:
  GET /series/{series_ticker}                       -> settlement_sources, fee_type, fee_multiplier
  GET /markets?series_ticker=KXWTI15M&status=open    -> the window(s) currently trading
  GET /markets/{ticker}                              -> one market (used to pick up `result` after close)

Prices: since March 2026 Kalshi returns fixed-point dollar strings ("0.4900") in *_dollars / *_fp
fields next to legacy integer cents. We prefer the dollar strings and fall back to cents.
"""
from __future__ import annotations

import asyncio
import datetime as dt
import inspect
import logging
import re
from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable

import httpx

from . import clock

log = logging.getLogger(__name__)

OPEN_STATUSES = {"open", "active"}
SETTLED_STATUSES = {"settled", "finalized", "determined"}

_STRIKE_RE = re.compile(r"\$?\s*(\d{1,4}(?:,\d{3})*\.\d{1,4})")


class KalshiError(RuntimeError):
    pass


# ----------------------------------------------------------------------------- parsing

def parse_price(d: dict, name: str) -> float | None:
    """Return a 0..1 dollar price for e.g. 'yes_bid', trying *_dollars/*_fp strings, then cents."""
    for key in (f"{name}_dollars", f"{name}_fp"):
        value = d.get(key)
        if value not in (None, ""):
            try:
                return float(value)
            except (TypeError, ValueError):
                pass
    value = d.get(name)
    if value in (None, ""):
        return None
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value / 100.0
    if isinstance(value, float):
        return value / 100.0 if value > 1.0 else value
    try:
        f = float(value)
    except (TypeError, ValueError):
        return None
    return f / 100.0 if f > 1.0 else f


def parse_strike(d: dict) -> tuple[float | None, str]:
    """The window's target price. Prefer floor_strike; fall back to text fields."""
    for key in ("floor_strike", "cap_strike"):
        value = d.get(key)
        if value not in (None, ""):
            try:
                f = float(value)
            except (TypeError, ValueError):
                continue
            if f > 0:  # 0 / negative = not published yet
                return f, key
    custom = d.get("custom_strike")
    if isinstance(custom, dict):
        for value in custom.values():
            try:
                f = float(value)
            except (TypeError, ValueError):
                continue
            if f > 0:
                return f, "custom_strike"
    for key in ("yes_sub_title", "subtitle", "title", "no_sub_title"):
        match = _STRIKE_RE.search(str(d.get(key) or ""))
        if match:
            f = float(match.group(1).replace(",", ""))
            if f > 0:
                return f, key
    return None, "none"


def _float_or_none(value) -> float | None:
    if value in (None, ""):
        return None
    try:
        return float(str(value).replace(",", "").replace("$", ""))
    except (TypeError, ValueError):
        return None


def _best_level(levels, in_cents: bool) -> tuple[float | None, float]:
    """Best (highest) resting bid price and total quantity from an orderbook side."""
    best_px, depth = None, 0.0
    for lvl in levels or []:
        try:
            if isinstance(lvl, dict):
                px = lvl.get("price_dollars", lvl.get("price"))
                qty = lvl.get("quantity_fp", lvl.get("quantity", lvl.get("count", 0)))
            else:
                px = lvl[0]
                qty = lvl[1] if len(lvl) > 1 else 0
            px = float(px)
            qty = float(qty or 0)
        except (TypeError, ValueError, IndexError):
            continue
        if in_cents and px >= 1.0:
            px = px / 100.0
        if px <= 0 or px >= 1:
            continue
        depth += qty
        if best_px is None or px > best_px:
            best_px = px
    return best_px, depth


def parse_orderbook(d: dict) -> dict | None:
    """Top of book from GET /markets/{ticker}/orderbook.

    Kalshi returns resting BIDS per side under orderbook_fp.yes_dollars / no_dollars as
    [["0.4200", "13.00"], ...] (legacy: orderbook.yes/no with [cents, count]). There is no ask ladder:
    "a bid for yes at X is an ask for no at 1 - X", so yes_ask = 1 - best no bid and no_ask = 1 - best yes bid.
    """
    if not isinstance(d, dict):
        return None
    # 2026 shape: "orderbook_fp" (fixed-point quantities) is canonical; "orderbook" is the legacy block.
    ob = d.get("orderbook_fp") or d.get("orderbook") or d
    if not isinstance(ob, dict):
        return None
    sides = {}
    for side in ("yes", "no"):
        levels, in_cents = None, False
        for key in (f"{side}_dollars", f"{side}_fp"):
            if ob.get(key) is not None:
                levels = ob.get(key)
                break
        if levels is None and ob.get(side) is not None:
            levels, in_cents = ob.get(side), True
        sides[side] = _best_level(levels, in_cents)
    yes_bid, yes_depth = sides["yes"]
    no_bid, no_depth = sides["no"]
    if yes_bid is None and no_bid is None:
        return None
    return {
        "yes_bid": yes_bid,
        "yes_ask": None if no_bid is None else round(1.0 - no_bid, 4),
        "no_bid": no_bid,
        "no_ask": None if yes_bid is None else round(1.0 - yes_bid, 4),
        "yes_depth": yes_depth,
        "no_depth": no_depth,
    }


def _int(d: dict, *keys) -> int | None:
    for key in keys:
        value = d.get(key)
        if value in (None, ""):
            continue
        try:
            return int(float(value))
        except (TypeError, ValueError):
            continue
    return None


@dataclass
class Market:
    ticker: str
    event_ticker: str = ""
    title: str = ""
    subtitle: str = ""
    status: str = ""
    open_time: dt.datetime | None = None
    close_time: dt.datetime | None = None
    expiration_time: dt.datetime | None = None
    strike: float | None = None
    strike_source: str = "none"
    yes_bid: float | None = None
    yes_ask: float | None = None
    no_bid: float | None = None
    no_ask: float | None = None
    last_price: float | None = None
    volume: int | None = None
    open_interest: int | None = None
    exchange_index: int | None = None  # the exchange shard the market trades on (orders spend that shard's cash)
    result: str | None = None  # "yes" | "no" | None
    settle_value: float | None = None  # numeric expiration_value once settled (the settlement price)
    rules_primary: str = ""
    quote_source: str = "list"  # list | orderbook
    quote_ts: float | None = None
    raw: dict = field(default_factory=dict, repr=False)

    def apply_quotes(self, book: dict, ts: float):
        """Overwrite bid/ask with a fresh top of book from the orderbook endpoint."""
        if book.get("yes_bid") is not None:
            self.yes_bid = book["yes_bid"]
        if book.get("yes_ask") is not None:
            self.yes_ask = book["yes_ask"]
        if book.get("no_bid") is not None:
            self.no_bid = book["no_bid"]
        if book.get("no_ask") is not None:
            self.no_ask = book["no_ask"]
        self.quote_source = "orderbook"
        self.quote_ts = ts

    @classmethod
    def from_api(cls, d: dict) -> "Market":
        strike, source = parse_strike(d)
        result = str(d.get("result") or "").lower() or None
        if result not in ("yes", "no"):
            result = None
        yes_bid = parse_price(d, "yes_bid")
        yes_ask = parse_price(d, "yes_ask")
        no_bid = parse_price(d, "no_bid")
        no_ask = parse_price(d, "no_ask")
        # Derive the missing side of the book from the other (Kalshi books are mirror images).
        if no_ask is None and yes_bid is not None:
            no_ask = round(1.0 - yes_bid, 4)
        if no_bid is None and yes_ask is not None:
            no_bid = round(1.0 - yes_ask, 4)
        if yes_ask is None and no_bid is not None:
            yes_ask = round(1.0 - no_bid, 4)
        if yes_bid is None and no_ask is not None:
            yes_bid = round(1.0 - no_ask, 4)
        return cls(
            ticker=str(d.get("ticker") or ""),
            event_ticker=str(d.get("event_ticker") or ""),
            title=str(d.get("title") or ""),
            subtitle=str(d.get("yes_sub_title") or d.get("subtitle") or ""),
            status=str(d.get("status") or "").lower(),
            open_time=clock.parse_time(d.get("open_time")),
            close_time=clock.parse_time(d.get("close_time")),
            expiration_time=clock.parse_time(d.get("expiration_time") or d.get("expected_expiration_time")),
            strike=strike,
            strike_source=source,
            yes_bid=yes_bid,
            yes_ask=yes_ask,
            no_bid=no_bid,
            no_ask=no_ask,
            last_price=parse_price(d, "last_price"),
            volume=_int(d, "volume_fp", "volume"),
            open_interest=_int(d, "open_interest_fp", "open_interest"),
            exchange_index=_int(d, "exchange_index"),
            result=result,
            settle_value=_float_or_none(d.get("expiration_value")),
            rules_primary=str(d.get("rules_primary") or ""),
            raw=d,
        )

    # --- derived -------------------------------------------------------------------------
    @property
    def is_open(self) -> bool:
        return self.status in OPEN_STATUSES

    @property
    def is_settled(self) -> bool:
        return self.result in ("yes", "no") or self.status in SETTLED_STATUSES

    def is_live(self, at: dt.datetime | None = None) -> bool:
        at = at or clock.now()
        return bool(self.open_time and self.close_time and self.open_time <= at < self.close_time)

    def seconds_left(self, at: dt.datetime | None = None) -> float | None:
        return clock.seconds_until(self.close_time, at)

    def seconds_elapsed(self, at: dt.datetime | None = None) -> float | None:
        if self.open_time is None:
            return None
        return ((at or clock.now()) - self.open_time).total_seconds()

    @property
    def yes_mid(self) -> float | None:
        if self.yes_bid is not None and self.yes_ask is not None:
            return (self.yes_bid + self.yes_ask) / 2
        return self.last_price

    @property
    def spread(self) -> float | None:
        if self.yes_bid is not None and self.yes_ask is not None:
            return round(self.yes_ask - self.yes_bid, 4)
        return None

    def summary(self) -> dict:
        return {
            "ticker": self.ticker,
            "status": self.status,
            "open_time": self.open_time.isoformat() if self.open_time else None,
            "close_time": self.close_time.isoformat() if self.close_time else None,
            "close_label": clock.et_label(self.close_time),
            "strike": self.strike,
            "strike_source": self.strike_source,
            "yes_bid": self.yes_bid,
            "yes_ask": self.yes_ask,
            "no_bid": self.no_bid,
            "no_ask": self.no_ask,
            "last_price": self.last_price,
            "volume": self.volume,
            "open_interest": self.open_interest,
            "result": self.result,
            "settle_value": self.settle_value,
            "quote_source": self.quote_source,
        }


@dataclass
class Series:
    ticker: str
    title: str = ""
    fee_type: str = "quadratic"
    fee_multiplier: float = 1.0
    settlement_sources: list[dict] = field(default_factory=list)
    raw: dict = field(default_factory=dict, repr=False)

    @classmethod
    def from_api(cls, d: dict) -> "Series":
        d = d.get("series", d) if isinstance(d, dict) else {}
        try:
            mult = float(d.get("fee_multiplier")) if d.get("fee_multiplier") not in (None, "") else 1.0
        except (TypeError, ValueError):
            mult = 1.0
        sources = d.get("settlement_sources") or []
        if not isinstance(sources, list):
            sources = [sources]
        return cls(
            ticker=str(d.get("ticker") or ""),
            title=str(d.get("title") or ""),
            fee_type=str(d.get("fee_type") or "quadratic"),
            fee_multiplier=mult,
            settlement_sources=[s if isinstance(s, dict) else {"name": str(s)} for s in sources],
            raw=d,
        )

    def summary(self) -> dict:
        return {
            "ticker": self.ticker,
            "title": self.title,
            "fee_type": self.fee_type,
            "fee_multiplier": self.fee_multiplier,
            "settlement_sources": self.settlement_sources,
        }


def select_live_market(markets: list[Market], at: dt.datetime | None = None) -> Market | None:
    """The window trading right now (open_time <= now < close_time, soonest close), else the next one."""
    at = at or clock.now()
    candidates = [m for m in markets if m.is_live(at) and not m.is_settled]
    if candidates:
        return min(candidates, key=lambda m: m.close_time)
    upcoming = [m for m in markets if m.close_time and m.close_time > at and not m.is_settled]
    if upcoming:
        return min(upcoming, key=lambda m: m.close_time)
    return None


# ----------------------------------------------------------------------------- client

class KalshiClient:
    def __init__(self, base_url: str, timeout: float = 10.0, client: httpx.AsyncClient | None = None):
        self.base_url = base_url.rstrip("/")
        self._client = client or httpx.AsyncClient(timeout=timeout, headers={"Accept": "application/json",
                                                                              "User-Agent": "wti15m-coach/0.1"})
        self._owns_client = client is None

    async def aclose(self):
        if self._owns_client:
            await self._client.aclose()

    async def _get(self, path: str, params: dict | None = None) -> Any:
        url = f"{self.base_url}{path}"
        try:
            resp = await self._client.get(url, params=params)
        except httpx.HTTPError as exc:
            raise KalshiError(f"GET {path}: {exc}") from exc
        if resp.status_code != 200:
            raise KalshiError(f"GET {path} -> HTTP {resp.status_code}: {resp.text[:200]}")
        try:
            return resp.json()
        except ValueError as exc:
            raise KalshiError(f"GET {path}: invalid JSON") from exc

    async def get_series_raw(self, series_ticker: str) -> dict:
        return await self._get(f"/series/{series_ticker}")

    async def get_series(self, series_ticker: str) -> Series:
        return Series.from_api(await self.get_series_raw(series_ticker))

    async def get_markets_raw(self, series_ticker: str | None = None, status: str | None = None,
                              event_ticker: str | None = None, limit: int = 100, max_pages: int = 3) -> list[dict]:
        params: dict[str, Any] = {"limit": limit}
        if series_ticker:
            params["series_ticker"] = series_ticker
        if status:
            params["status"] = status
        if event_ticker:
            params["event_ticker"] = event_ticker
        out: list[dict] = []
        cursor = None
        for _ in range(max_pages):
            if cursor:
                params["cursor"] = cursor
            data = await self._get("/markets", params)
            out.extend(data.get("markets") or [])
            cursor = data.get("cursor")
            if not cursor:
                break
        return out

    async def get_markets(self, series_ticker: str | None = None, status: str | None = None, **kw) -> list[Market]:
        return [Market.from_api(d) for d in await self.get_markets_raw(series_ticker, status, **kw)]

    async def get_market_raw(self, ticker: str) -> dict:
        data = await self._get(f"/markets/{ticker}")
        return data.get("market", data)

    async def get_live_data_raw(self, event_ticker: str, range_: str = "15min") -> dict:
        """Kalshi's own live price series for an event (the Pyth value it settles on)."""
        return await self._get(f"/live_data/events/{event_ticker}", {"range": range_})

    async def get_orderbook_raw(self, ticker: str, depth: int = 5) -> dict:
        return await self._get(f"/markets/{ticker}/orderbook", {"depth": depth})

    async def get_orderbook(self, ticker: str, depth: int = 5) -> dict | None:
        return parse_orderbook(await self.get_orderbook_raw(ticker, depth))

    async def get_market(self, ticker: str) -> Market:
        return Market.from_api(await self.get_market_raw(ticker))


# ----------------------------------------------------------------------------- tracker

EventHandler = Callable[[str, Market], None | Awaitable[None]]


class MarketTracker:
    """Polls Kalshi, keeps `current` pointed at the live window, and reports settlements.

    Events (kind, market): "quote" every poll with fresh prices, "window_open" when a new window
    becomes current, "window_close" when it stops being current, "settled" once `result` is known.
    """

    def __init__(self, client, series_ticker: str, poll_interval: float = 1.0,
                 on_event: EventHandler | None = None, settlement_poll_s: float = 5.0,
                 settlement_timeout_s: float = 90 * 60):
        self.client = client
        self.series_ticker = series_ticker
        self.poll_interval = poll_interval
        self.on_event = on_event
        self.settlement_poll_s = settlement_poll_s
        self.settlement_timeout_s = settlement_timeout_s
        self.series: Series | None = None
        self.current: Market | None = None
        self.pending: dict[str, tuple[Market, float]] = {}  # ticker -> (market, closed_at_epoch)
        self.last_error: str | None = None
        self.last_poll: float | None = None
        self.last_quote_ts: float | None = None  # last poll that actually returned fresh prices
        self.quote_source: str = "list"
        self.discovery_interval_s = 5.0  # how often to re-read the market list while a window is live
        self._last_discovery = 0.0
        self.orderbook_errors = 0
        self.poll_count = 0
        self._settle_checked: dict[str, float] = {}

    async def _emit(self, kind: str, market: Market):
        if self.on_event is None:
            return
        try:
            result = self.on_event(kind, market)
            if inspect.isawaitable(result):
                await result
        except Exception:  # handlers must not kill the poll loop
            log.exception("event handler failed for %s", kind)

    async def poll_once(self):
        at = clock.now()
        if self.series is None:
            try:
                self.series = await self.client.get_series(self.series_ticker)
                log.info("series %s: fee_type=%s multiplier=%s settlement=%s", self.series.ticker,
                         self.series.fee_type, self.series.fee_multiplier, self.series.settlement_sources)
            except KalshiError as exc:
                self.last_error = str(exc)
                log.warning("series fetch failed: %s", exc)
        now_ts = at.timestamp()
        self.last_poll = now_ts
        self.poll_count += 1
        # The market LIST is only needed to find the window (every few seconds); live prices come from the
        # orderbook endpoint every poll, because the list's bid/ask can lag the real book.
        have_live = self.current is not None and self.current.is_live(at)
        discover = (not have_live) or (now_ts - self._last_discovery >= self.discovery_interval_s)
        markets: list[Market] = []
        listed = False
        if discover:
            self._last_discovery = now_ts
            try:
                markets = await self.client.get_markets(self.series_ticker, status="open")
                self.last_error = None
                listed = True
            except KalshiError as exc:
                self.last_error = str(exc)
                log.warning("markets fetch failed: %s", exc)

        live = select_live_market(markets, at) if listed else None
        stale = False
        if live is None and self.current is not None and self.current.is_live(at):
            live = self.current  # not re-listed this poll (or the list failed): keep the window
            stale = True
        if live is not None and (self.current is None or live.ticker != self.current.ticker):
            if self.current is not None:
                self.pending[self.current.ticker] = (self.current, at.timestamp())
                await self._emit("window_close", self.current)
            self.current = live
            await self._emit("window_open", live)
        elif live is not None:
            self.current = live
        elif self.current is not None and not self.current.is_live(at):
            self.pending[self.current.ticker] = (self.current, at.timestamp())
            closed = self.current
            self.current = None
            await self._emit("window_close", closed)

        fresh = (not stale) and listed
        if self.current is not None and live is not None and self.current.is_live(at):
            book = await self._fetch_book(self.current.ticker)
            if book:
                self.current.apply_quotes(book, now_ts)
                self.quote_source = "orderbook"
                fresh = True
            elif fresh:
                self.quote_source = "list"
        if self.current is not None and live is not None and fresh:
            self.last_quote_ts = now_ts
            await self._emit("quote", self.current)

        if self.pending:
            await self._check_settlements(now_ts)

    async def _fetch_book(self, ticker: str) -> dict | None:
        getter = getattr(self.client, "get_orderbook", None)
        if getter is None:
            return None
        try:
            book = await getter(ticker)
            self.orderbook_errors = 0
            return book
        except Exception as exc:  # the list quotes remain as a fallback
            self.orderbook_errors += 1
            if self.orderbook_errors in (1, 10, 100):
                log.warning("orderbook fetch failed (%d): %s", self.orderbook_errors, exc)
            return None

    def quotes_fresh(self, max_age_s: float = 10.0, at: float | None = None) -> bool:
        return self.last_quote_ts is not None and ((at or clock.now().timestamp()) - self.last_quote_ts) <= max_age_s

    async def _check_settlements(self, now_epoch: float):
        for ticker, (market, closed_at) in list(self.pending.items()):
            # every 5 s for the first 20 minutes, then once a minute until the timeout
            cadence = self.settlement_poll_s if now_epoch - closed_at < 20 * 60 else 60.0
            if now_epoch - self._settle_checked.get(ticker, 0.0) < cadence:
                continue
            self._settle_checked[ticker] = now_epoch
            try:
                fresh = await self.client.get_market(ticker)
            except Exception as exc:  # KalshiError, network, or a market that vanished
                log.warning("settlement check %s failed: %s", ticker, exc)
                continue
            if fresh.result in ("yes", "no"):
                fresh.strike = fresh.strike if fresh.strike is not None else market.strike
                del self.pending[ticker]
                self._settle_checked.pop(ticker, None)
                await self._emit("settled", fresh)
            elif now_epoch - closed_at > self.settlement_timeout_s:
                log.warning("gave up waiting for settlement of %s", ticker)
                del self.pending[ticker]
                self._settle_checked.pop(ticker, None)
                await self._emit("settlement_timeout", fresh)

    async def run(self, stop: asyncio.Event | None = None):
        stop = stop or asyncio.Event()
        while not stop.is_set():
            try:
                await self.poll_once()
            except Exception:  # never die
                log.exception("poll failed")
            interval = self.poll_interval if self.current is not None else max(self.poll_interval, 2.0)
            if self.last_error:
                interval = max(interval, 5.0)
            try:
                await asyncio.wait_for(stop.wait(), timeout=interval)
            except asyncio.TimeoutError:
                pass
