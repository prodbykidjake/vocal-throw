"""Price feed interface + tick buffer shared by every underlying-price source."""
from __future__ import annotations

import abc
import bisect
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable


@dataclass(frozen=True)
class Tick:
    ts: float  # epoch seconds
    price: float


@dataclass
class FeedHealth:
    name: str
    connected: bool
    age_s: float | None
    seconds_seen: float
    mode: str = ""
    symbol: str | None = None
    last_error: str | None = None
    ticks: int = 0

    def is_stale(self, stale_after_s: float) -> bool:
        return self.age_s is None or self.age_s > stale_after_s

    def as_dict(self) -> dict:
        return {
            "name": self.name, "connected": self.connected, "age_s": self.age_s,
            "seconds_seen": self.seconds_seen, "mode": self.mode, "symbol": self.symbol,
            "last_error": self.last_error, "ticks": self.ticks,
        }


class TickBuffer:
    """Keeps at most one tick per second for `max_seconds`, with time-indexed lookups."""

    def __init__(self, max_seconds: float = 6 * 3600):
        self.max_seconds = max_seconds
        self._ts: deque[float] = deque()
        self._px: deque[float] = deque()
        self.count = 0

    def add(self, ts: float, price: float) -> bool:
        """Accept a tick. Returns False (and ignores it) for bad prices, future timestamps and out-of-order ticks."""
        if price <= 0 or ts > time.time() + 2.0:
            return False  # bad price, or a timestamp in the future (e.g. an in-progress candle's end time)
        if self._ts and ts < self._ts[-1]:
            return False  # out of order (e.g. a backfill older than what we already have)
        self.count += 1
        if self._ts and ts - self._ts[-1] < 1.0:
            self._px[-1] = price  # same second: keep the latest print
            return True
        self._ts.append(ts)
        self._px.append(price)
        cutoff = ts - self.max_seconds
        while self._ts and self._ts[0] < cutoff:
            self._ts.popleft()
            self._px.popleft()
        return True

    def items(self) -> list[tuple[float, float]]:
        return list(zip(self._ts, self._px))

    def __len__(self) -> int:
        return len(self._ts)

    def latest(self) -> Tick | None:
        return Tick(self._ts[-1], self._px[-1]) if self._ts else None

    def first_ts(self) -> float | None:
        return self._ts[0] if self._ts else None

    def seconds_seen(self) -> float:
        return (self._ts[-1] - self._ts[0]) if len(self._ts) >= 2 else 0.0

    def price_at(self, ts: float) -> float | None:
        """Last price at or before `ts` (None if the buffer does not reach back that far)."""
        if not self._ts or ts < self._ts[0]:
            return None
        idx = bisect.bisect_right(self._ts, ts) - 1
        return self._px[idx] if idx >= 0 else None

    def last_at_or_before(self, ts: float) -> Tick | None:
        if not self._ts or ts < self._ts[0]:
            return None
        idx = bisect.bisect_right(self._ts, ts) - 1
        return Tick(self._ts[idx], self._px[idx]) if idx >= 0 else None

    def price_near(self, ts: float, window_s: float = 5.0) -> float | None:
        """Last price at or before `ts`, only if a tick exists within `window_s` before it."""
        tick = self.last_at_or_before(ts)
        return tick.price if tick is not None and ts - tick.ts <= window_s else None

    def since(self, ts: float) -> list[Tick]:
        if not self._ts:
            return []
        idx = bisect.bisect_left(self._ts, ts)
        return [Tick(t, p) for t, p in zip(list(self._ts)[idx:], list(self._px)[idx:])]

    def closes(self, step_s: float, since_ts: float | None = None) -> list[tuple[float, float]]:
        """Downsample to one close per `step_s` bucket (bucket end time, last price)."""
        out: list[tuple[float, float]] = []
        bucket = None
        for t, p in zip(self._ts, self._px):
            if since_ts is not None and t < since_ts:
                continue
            b = int(t // step_s)
            if bucket is None or b != bucket:
                out.append(((b + 1) * step_s, p))
                bucket = b
            else:
                out[-1] = (out[-1][0], p)
        return out


class PriceFeed(abc.ABC):
    name = "feed"

    def __init__(self):
        self.buffer = TickBuffer()
        self._subscribers: list[Callable[[Tick], None]] = []
        self._warmup_subscribers: list[Callable[[list[tuple[float, float]]], None]] = []
        self.connected = False
        self.mode = "init"
        self.symbol: str | None = None
        self.last_error: str | None = None

    def subscribe(self, fn: Callable[[Tick], None]):
        self._subscribers.append(fn)

    def subscribe_warmup(self, fn: Callable[[list[tuple[float, float]]], None]):
        self._warmup_subscribers.append(fn)

    @property
    def active_name(self) -> str:
        """Name of the source actually producing ticks (a composite feed overrides this)."""
        return self.name

    def _publish(self, ts: float, price: float):
        if not self.buffer.add(ts, price):
            return  # rejected (stale/future/bad): subscribers must not see it either
        tick = Tick(ts, price)
        for fn in self._subscribers:
            try:
                fn(tick)
            except Exception:  # a bad subscriber must not kill the feed
                import logging
                logging.getLogger(__name__).exception("tick subscriber failed")

    def _notify_warmup(self, closes: list[tuple[float, float]]):
        for fn in self._warmup_subscribers:
            try:
                fn(closes)
            except Exception:
                import logging
                logging.getLogger(__name__).exception("warmup subscriber failed")

    def _publish_warmup(self, closes: list[tuple[float, float]]):
        for ts, px in closes:
            self.buffer.add(ts, px)
        self._notify_warmup(closes)

    def latest(self) -> Tick | None:
        return self.buffer.latest()

    def health(self) -> FeedHealth:
        last = self.buffer.latest()
        return FeedHealth(
            name=self.name, connected=self.connected,
            age_s=(time.time() - last.ts) if last else None,
            seconds_seen=self.buffer.seconds_seen(), mode=self.mode, symbol=self.symbol,
            last_error=self.last_error, ticks=self.buffer.count,
        )

    @abc.abstractmethod
    async def start(self): ...

    @abc.abstractmethod
    async def stop(self): ...
