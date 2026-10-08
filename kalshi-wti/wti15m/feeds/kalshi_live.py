"""Kalshi's own live price series for an event: the Pyth PYTHOIL value Kalshi draws as "Now" and settles on.

GET https://api.elections.kalshi.com/trade-api/v2/live_data/events/{event_ticker}?range=15min
  -> {"live_data": {"type": ..., "details": {"timeseries": [{"t": <epoch ms>, "v": <price>}, ...]},
      "default_range": "15min", "range_options": ["15min", "1h"]}}
Unauthenticated, about one point per second. The shape above comes from Kalshi's docs mirror; the parser is
tolerant (t in ms or s, v as number or string, list pairs) and `python -m wti15m probe` dumps the raw response.
"""
from __future__ import annotations

import asyncio
import logging
import time
from typing import Callable

import httpx

from .base import PriceFeed

log = logging.getLogger(__name__)


def parse_series(data) -> list[tuple[float, float]]:
    """(epoch seconds, price) pairs, sorted, from a live_data response (or just its details)."""
    if not isinstance(data, dict):
        return []
    ld = data.get("live_data", data)
    details = ld.get("details", ld) if isinstance(ld, dict) else {}
    series = None
    for container in (details, ld, data):
        if isinstance(container, dict) and isinstance(container.get("timeseries"), list):
            series = container["timeseries"]
            break
    out: list[tuple[float, float]] = []
    for pt in series or []:
        try:
            if isinstance(pt, dict):
                t = pt.get("t", pt.get("ts", pt.get("time", pt.get("timestamp"))))
                v = pt.get("v", pt.get("value", pt.get("price", pt.get("close"))))
            else:
                t, v = pt[0], pt[1]
            t = float(t)
            v = float(str(v).replace(",", ""))
        except (TypeError, ValueError, IndexError):
            continue
        if t > 1e11:  # milliseconds
            t /= 1000.0
        if v > 0:
            out.append((t, v))
    out.sort()
    return out


def event_ticker_for(market_ticker: str, event_ticker: str | None = None) -> str | None:
    if event_ticker:
        return event_ticker
    if not market_ticker:
        return None
    head, sep, tail = market_ticker.rpartition("-")
    return head if sep and tail.isdigit() and len(tail) <= 3 else market_ticker


class KalshiLiveFeed(PriceFeed):
    name = "kalshi-live"

    def __init__(self, base_url: str, event_ticker_fn: Callable[[], str | None] | None = None,
                 http: httpx.AsyncClient | None = None, poll_s: float = 1.0, warmup_range: str = "1h",
                 live_range: str = "15min"):
        super().__init__()
        self.base_url = base_url.rstrip("/")
        self.event_ticker_fn = event_ticker_fn or (lambda: None)
        self._http = http or httpx.AsyncClient(timeout=10.0, headers={"Accept": "application/json",
                                                                       "User-Agent": "wti15m-coach/0.1"})
        self.poll_s = poll_s
        self.warmup_range = warmup_range
        self.live_range = live_range
        self.symbol = "PYTHOIL via Kalshi"
        self.mode = "init"
        self.errors = 0
        self.last_t = 0.0  # newest point published (epoch s)
        self._warmed_events: set[str] = set()
        self._last_event: str | None = None
        self._last_event_seen = 0.0
        self._task: asyncio.Task | None = None
        self._stop = asyncio.Event()

    # ------------------------------------------------------------------ HTTP
    async def fetch_series(self, event_ticker: str, range_: str) -> list[tuple[float, float]]:
        url = f"{self.base_url}/live_data/events/{event_ticker}"
        resp = await self._http.get(url, params={"range": range_})
        if resp.status_code != 200:
            raise RuntimeError(f"live_data {event_ticker} -> HTTP {resp.status_code}: {resp.text[:120]}")
        return parse_series(resp.json())

    def current_event(self) -> str | None:
        ev = None
        try:
            ev = self.event_ticker_fn()
        except Exception:
            ev = None
        now = time.time()
        if ev:
            self._last_event, self._last_event_seen = ev, now
            return ev
        if self._last_event and now - self._last_event_seen < 90:
            return self._last_event  # between windows: keep reading the last event's series a bit longer
        return None

    # ------------------------------------------------------------------ one poll (testable)
    async def poll_once(self) -> int:
        """Fetch the series for the current event and publish new points. Returns the number published."""
        ev = self.current_event()
        if ev is None:
            self.mode = "waiting for a window"
            return 0
        try:
            if ev not in self._warmed_events:
                pts = await self.fetch_series(ev, self.warmup_range)
                self._warmed_events.add(ev)
                if pts:
                    history = [p for p in pts if p[0] > self.last_t]
                    for ts, px in history[:-1]:
                        self.buffer.add(ts, px)
                    # seed volatility with 5-second closes (the live path buckets ticks the same way)
                    closes, bucket = [], None
                    for ts, px in history:
                        b = int(ts // 5)
                        if b != bucket:
                            closes.append((ts, px))
                            bucket = b
                        else:
                            closes[-1] = (ts, px)
                    for fn in self._warmup_subscribers:
                        try:
                            fn(closes)
                        except Exception:
                            log.exception("warmup subscriber failed")
                    if history:
                        self.last_t = history[-1][0]
                        self._publish(history[-1][0], history[-1][1])
                    log.info("kalshi-live: warmed up %s with %d points", ev, len(history))
                self.mode = "kalshi-live"
                self.connected = True
                self.errors = 0
                self.last_error = None
                return 1 if pts else 0
            pts = await self.fetch_series(ev, self.live_range)
            new = [p for p in pts if p[0] > self.last_t]
            for ts, px in new:
                self._publish(ts, px)
            if new:
                self.last_t = new[-1][0]
            self.mode = "kalshi-live"
            self.connected = True
            self.errors = 0
            self.last_error = None
            return len(new)
        except Exception as exc:
            self.errors += 1
            self.last_error = str(exc)[:160]
            self.connected = self.errors < 5
            if self.errors in (1, 10, 100):
                log.warning("kalshi-live poll failed (%d): %s", self.errors, exc)
            if self.errors >= 5:
                self.mode = "error"
            return 0

    # ------------------------------------------------------------------ lifecycle
    async def start(self):
        self._stop.clear()
        self._task = asyncio.create_task(self._run(), name="kalshi-live-feed")

    async def stop(self):
        self._stop.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
        await self._http.aclose()

    async def _run(self):
        while not self._stop.is_set():
            try:
                await self.poll_once()
            except asyncio.CancelledError:
                raise
            except Exception:
                log.exception("kalshi-live loop error")
            await asyncio.sleep(self.poll_s if self.errors < 5 else min(30.0, 2.0 * self.errors))
