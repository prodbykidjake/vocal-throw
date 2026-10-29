"""Simulated Kalshi: same interface as KalshiClient, driven by a SimFeed. Used by `demo` mode and tests."""
from __future__ import annotations

import datetime as dt
import math
import random
import time

from . import clock
from .feeds.sim import SimFeed
from .kalshi import Market, Series
from .model import base_probability


class SimKalshi:
    def __init__(self, feed: SimFeed, window_s: int = 900, series_ticker: str = "KXWTI15M", settle_delay_s: float = 5.0,
                 spread: float = 0.03, seed: int | None = None, quote_lag_s: float = 20.0):
        self.feed = feed
        self.window_s = window_s
        self.series_ticker = series_ticker
        self.settle_delay_s = settle_delay_s
        self.spread = spread
        self.quote_lag_s = quote_lag_s  # the simulated book reprices from a slightly stale price, like a real book
        self.rng = random.Random(seed)
        self._strikes: dict[str, float] = {}
        self._settle_px: dict[str, float] = {}

    def _window_bounds(self, at: float) -> tuple[float, float]:
        start = math.floor(at / self.window_s) * self.window_s
        return start, start + self.window_s

    def _ticker(self, close_epoch: float) -> str:
        close = dt.datetime.fromtimestamp(close_epoch, clock.ET)
        return f"{self.series_ticker}-{close.strftime('%y%b%d%H%M').upper()}"

    def _build(self, open_epoch: float, close_epoch: float, at: float) -> dict:
        ticker = self._ticker(close_epoch)
        if ticker not in self._strikes:
            px = self.feed.buffer.price_at(open_epoch) or (self.feed.latest().price if self.feed.latest() else 90.0)
            self._strikes[ticker] = round(px, 2)
        strike = self._strikes[ticker]
        last = self.feed.latest()
        price = last.price if last else strike
        quote_price = self.feed.buffer.price_at(at - self.quote_lag_s) or price
        tau = max(0.0, close_epoch - at)
        status = "open" if at < close_epoch else "closed"
        result = ""
        if at >= close_epoch:
            if ticker not in self._settle_px:
                self._settle_px[ticker] = self.feed.buffer.price_at(close_epoch) or price
            if at >= close_epoch + self.settle_delay_s:
                status = "settled"
                result = "yes" if round(self._settle_px[ticker], 2) >= strike else "no"
        p, _ = base_probability(quote_price, strike, 0.005, max(tau, 1.0))
        noise = self.rng.gauss(0, 0.02)
        mid = min(0.99, max(0.01, p + noise))
        yes_bid = round(max(0.01, mid - self.spread / 2), 2)
        yes_ask = round(min(0.99, mid + self.spread / 2), 2)
        return {
            "ticker": ticker, "event_ticker": ticker, "status": status,
            "title": "WTI Oil price up or down? (simulated)", "yes_sub_title": f"${strike:.2f} or above",
            "open_time": dt.datetime.fromtimestamp(open_epoch, clock.UTC).isoformat(),
            "close_time": dt.datetime.fromtimestamp(close_epoch, clock.UTC).isoformat(),
            "floor_strike": strike,
            "yes_bid_dollars": f"{yes_bid:.4f}", "yes_ask_dollars": f"{yes_ask:.4f}",
            "no_bid_dollars": f"{1 - yes_ask:.4f}", "no_ask_dollars": f"{1 - yes_bid:.4f}",
            "last_price_dollars": f"{mid:.4f}", "volume": 1000, "open_interest": 500, "result": result,
            "rules_primary": "SIMULATED MARKET. Resolves Yes if the simulated price at close is at or above the target.",
        }

    async def get_series(self, series_ticker: str) -> Series:
        return Series.from_api({"series": {"ticker": series_ticker, "title": "WTI Oil 15 min (simulated)",
                                            "fee_type": "quadratic", "fee_multiplier": 1.0,
                                            "settlement_sources": [{"name": "simulated feed", "url": ""}]}})

    async def get_markets(self, series_ticker=None, status=None, **kw) -> list[Market]:
        at = time.time()
        start, end = self._window_bounds(at)
        return [Market.from_api(self._build(start, end, at))]

    async def get_orderbook(self, ticker: str, depth: int = 5) -> dict | None:
        m = await self.get_market(ticker)
        return {"yes_bid": m.yes_bid, "yes_ask": m.yes_ask, "no_bid": m.no_bid, "no_ask": m.no_ask, "yes_depth": 100.0, "no_depth": 100.0}

    async def get_market(self, ticker: str) -> Market:
        at = time.time()
        for close_epoch in (self._window_bounds(at)[1], self._window_bounds(at)[0]):
            if self._ticker(close_epoch) == ticker:
                return Market.from_api(self._build(close_epoch - self.window_s, close_epoch, at))
        # older window: rebuild from stored strike
        start, _ = self._window_bounds(at)
        for k in range(1, 50):
            close_epoch = start - (k - 1) * self.window_s
            if self._ticker(close_epoch) == ticker:
                return Market.from_api(self._build(close_epoch - self.window_s, close_epoch, at))
        raise KeyError(ticker)

    async def aclose(self):
        return None
