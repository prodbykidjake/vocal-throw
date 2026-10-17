"""The exact index first, the exchanges' median as its backup.

While the index (kalshi_index.py) is fresh the board reads it; when it goes quiet for `stale_s` seconds the board
switches to the exchanges' median (exchanges.py) and says FALLBACK, and no new settlement call is made until the index
is back (desk.py, shadow.py, edge.py and pullout.py all honour that mode). Each source keeps its own measured offset
from the settlement values, since the desk stores the basis by the name of the source actually in use.
"""
from __future__ import annotations

import time

from .base import FeedHealth, PriceFeed, Tick


class FallbackFeed(PriceFeed):
    def __init__(self, primary: PriceFeed, backup: PriceFeed, stale_s: float = 5.0):
        super().__init__()
        self.primary, self.backup = primary, backup
        self.stale_s = stale_s
        self.name = primary.name
        self.symbol = primary.symbol
        primary.subscribe(self._on_primary)
        backup.subscribe(self._on_backup)
        backup.subscribe_warmup(self._on_warmup)  # the exchanges' candles seed the volatility
        primary.subscribe_warmup(self._on_warmup)

    def _primary_fresh(self, now: float | None = None) -> bool:
        last = self.primary.latest()
        return last is not None and (time.time() if now is None else now) - last.ts <= self.stale_s

    @property
    def using_backup(self) -> bool:
        return not self._primary_fresh()

    @property
    def active_name(self) -> str:
        return self.backup.name if self.using_backup else self.primary.name

    def _on_primary(self, tick: Tick):
        self._publish(tick.ts, tick.price)

    def _on_backup(self, tick: Tick):
        if self.using_backup:
            self._publish(tick.ts, tick.price)

    def _on_warmup(self, closes):
        self._publish_warmup(closes)

    def settle_avg(self, now: float | None = None) -> float | None:
        fn = getattr(self.primary, "settle_avg", None)
        return fn(now) if fn and not self.using_backup else None

    def health(self) -> FeedHealth:
        h = super().health()
        if self.using_backup:
            b = self.backup.health()
            h.mode = f"FALLBACK: {b.mode} (exact index down: {self.primary.last_error or self.primary.mode})"
            h.symbol, h.last_error = b.symbol, self.primary.last_error
            h.connected = b.connected
        else:
            h.mode, h.symbol, h.connected = self.primary.mode, self.primary.symbol, True
        return h

    async def start(self):
        await self.backup.start()
        await self.primary.start()

    async def stop(self):
        for f in (self.primary, self.backup):
            try:
                await f.stop()
            except Exception:
                pass
