"""The exchanges' price, matched second by second to Kalshi's own settlement average (no key needed).

What Kalshi serves: for a crypto window, GET /live_data/events/{event} (the chart on the market page) is the CF
Benchmarks index's running 60-second average, one point a second: the point at second t is the average of the index's
prints over the 60 seconds ending at t, and the window settles on the point one second before the close (rounded to the
target's decimals; the event's own series shows that value at the close). Checked on 72 of 72 settled windows across
six coins, 2026-10-17. Its one-second changes are 98% correlated with each other, as a 60-second average's are.

So the average itself is exact and free, but it lags the index by about half a minute, and the model needs the index's
price now (the settlement depends on where the price goes from here). The exchanges' median midpoint is that live
price up to a steady offset (measured the same day: 0.5-9.5 basis points by coin, e.g. Bitcoin +$6.57, steady to within
0.1-0.5 bp). This feed prints the exchanges' price corrected by that offset, learned continuously by comparing each
new point of Kalshi's average with the same 60 seconds of exchange prices. Inside the settlement minute it also gives
the average of the prints already in, from Kalshi's own average, exact at the last second.
"""
from __future__ import annotations

import math
import time

from .base import FeedHealth, PriceFeed, Tick, second_average

MATCH_WAIT_S = 300.0  # after start-up: "FALLBACK" (no new calls) until matched, for at most this long
HALFLIFE_S = 90.0  # the offset follows Kalshi's average with this half-life (each point is a fresh 60-s comparison)
MIN_SHARE = 0.8  # a comparison needs exchange prints in 80% of the 60 seconds
STALE_AVG_S = 30.0  # Kalshi's average older than this: keep the last offset, say so


def money(x: float) -> str:
    a = abs(x)
    digits = 2 if a >= 1 else 4 if a >= 0.01 else 6
    return f"{'+' if x >= 0 else '-'}${a:,.{digits}f}"


class CalibratedFeed(PriceFeed):
    """`raw` (the exchanges' median) + offset learned against `avg` (Kalshi's running 60-second average)."""
    name = "exchanges+kalshi"

    def __init__(self, raw: PriceFeed, avg: PriceFeed, window_s: float = 60.0, halflife_s: float = HALFLIFE_S):
        super().__init__()
        self.raw, self.avg = raw, avg
        self.window_s = window_s
        self.halflife_s = halflife_s
        self.offset: float | None = None
        self.offset_sd = 0.0  # spread of the comparisons around the offset (typical size, EWMA)
        self.n = 0  # comparisons so far
        self._last_cmp_ts: float | None = None
        self._started: float | None = None
        self.symbol = raw.symbol
        raw.subscribe(self._on_raw)
        raw.subscribe_warmup(self._publish_warmup)  # the exchanges' candles seed the volatility, as before
        avg.subscribe(self._on_avg)

    # ------------------------------------------------------------------ calibration
    def _on_raw(self, tick: Tick):
        self._publish(tick.ts, tick.price + (self.offset or 0.0))

    def _on_avg(self, tick: Tick):
        """A new point of Kalshi's average at second t: compare it with the exchanges' prints over the same 60 s."""
        t = float(round(tick.ts))
        if self._last_cmp_ts is not None and t <= self._last_cmp_ts:
            return
        ex = second_average(self.raw.buffer.items(), t - self.window_s + 1, t + 1, min_share=MIN_SHARE)
        if ex is None:
            return
        r = tick.price - ex
        if self.offset is None:
            self.offset = r
        else:
            dt = t - self._last_cmp_ts if self._last_cmp_ts is not None else 1.0
            a = 1.0 - 0.5 ** (max(dt, 1.0) / self.halflife_s)
            dev = r - self.offset
            # one glitch (a frozen exchange, a gap) moves the offset by at most 4 typical spreads
            lim = 4.0 * max(self.offset_sd, 1e-6 * abs(tick.price))
            self.offset += a * max(-lim, min(lim, dev)) if self.n >= 20 else a * dev
            self.offset_sd = math.sqrt((1 - a) * self.offset_sd ** 2 + a * dev * dev) if self.n else abs(dev)
        self.n += 1
        self._last_cmp_ts = t

    def kalshi_avg(self, now: float | None = None) -> tuple[float, float] | None:
        """(ts, value) of Kalshi's latest average point, if fresh."""
        last = self.avg.latest()
        now = time.time() if now is None else now
        return (last.ts, last.price) if last and now - last.ts <= STALE_AVG_S else None

    def settle_avg(self, now: float | None = None, close_ts: float | None = None) -> float | None:
        """Inside the settlement minute: the average of the index's prints since it began, from Kalshi's own average
        (its latest point covers the last 60 s; the prints from before the minute that it still holds are taken off
        using the calibrated exchange prices). Exact at the last second, when nothing from before is left."""
        now = time.time() if now is None else now
        if close_ts is None or not close_ts - self.window_s <= now < close_ts:
            return None
        k = self.kalshi_avg(now)
        if k is None:
            return None
        t, m = float(round(k[0])), k[1]
        start = close_ts - self.window_s  # the first print of the settlement average
        if t < start or t > math.floor(now) + 2:
            return None  # not inside the minute yet, or a point stamped ahead of this computer's clock
        done = int(t - start) + 1  # prints in so far: start .. t
        old_lo, old_hi = t - self.window_s + 1, start  # prints Kalshi's point still holds from before the minute
        old = 0.0
        if old_hi > old_lo:
            avg_old = second_average(self.buffer.items(), old_lo, old_hi, min_share=0.5)
            if avg_old is None:
                return None
            old = avg_old * (old_hi - old_lo)
        a = (self.window_s * m - old) / done
        # the seconds since Kalshi's latest point (it is polled every couple of seconds) are in too: the live prints
        last = min(math.floor(now), close_ts - 1)
        if last > t:
            recent = second_average(self.buffer.items(), t + 1, last + 1, min_share=0.5)
            if recent is not None:
                a = (done * a + (last - t) * recent) / (done + last - t)
        return a

    # ------------------------------------------------------------------ health
    def health(self) -> FeedHealth:
        h = super().health()
        r = self.raw.health()
        h.connected, h.last_error = r.connected, r.last_error
        k = self.kalshi_avg()
        if self.offset is None and time.time() - (self._started or time.time()) < MATCH_WAIT_S:
            # the first minute or so: the exchanges' price can sit several basis points off Kalshi's index, enough
            # to fake an EDGE, so no new calls until it is matched (shadow.py, edge.py and pullout.py read this too)
            h.mode = f"FALLBACK: {r.mode}, matching to Kalshi's average…"
        elif self.offset is None:
            h.mode = f"{r.mode} (Kalshi's average unavailable: not matched)"
        elif k is None:
            h.mode = f"{r.mode}, offset {money(self.offset)} (Kalshi's average not updating)"
        else:
            h.mode = f"{r.mode} matched to Kalshi's average (offset {money(self.offset)})"
        h.symbol = r.symbol
        return h

    async def start(self):
        self._started = time.time()
        await self.raw.start()
        await self.avg.start()

    async def stop(self):
        for f in (self.raw, self.avg):
            try:
                await f.stop()
            except Exception:
                pass
