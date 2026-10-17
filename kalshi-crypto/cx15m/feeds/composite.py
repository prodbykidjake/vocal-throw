"""The exact index first, the exchanges matched to Kalshi's average as its backup.

While the index (kalshi_index.py) is fresh the board reads it; when it goes quiet for `stale_s` seconds the board
switches to the backup. The backup (calibrated.py) is the exchanges' price matched second by second to Kalshi's own
settlement average, nearly as good as the index, so calls go on while it is matched; only when it is not (no fresh
point of Kalshi's average, or no offset learned yet) does the card say FALLBACK, and no new settlement call is made
until a good price is back (desk.py, shadow.py, edge.py and pullout.py all honour that mode). Each source keeps its
own measured offset from the settlement values, since the desk stores the basis by the name of the source in use.
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

    def settle_avg(self, now: float | None = None, close_ts: float | None = None) -> float | None:
        """The settlement minute's average so far: from Kalshi's own average when the backup has it (checked against
        Kalshi's settlements), else from the index's own running average."""
        for f in (self.backup, self.primary):
            fn = getattr(f, "settle_avg", None)
            a = fn(now, close_ts) if fn else None
            if a is not None:
                return a
        return None

    @property
    def avg_source(self):
        """Kalshi's average, when the backup carries it (the desk measures volatility on it)."""
        return getattr(self.backup, "avg_source", None)

    def _backup_matched(self) -> bool:
        kal = getattr(self.backup, "kalshi_avg", None)
        return getattr(self.backup, "offset", None) is not None and kal is not None and kal() is not None

    def health(self) -> FeedHealth:
        h = super().health()
        if self.using_backup:
            b = self.backup.health()
            why = f"exact index down: {self.primary.last_error or self.primary.mode}"
            h.mode = f"backup: {b.mode} ({why})" if self._backup_matched() else f"FALLBACK: {b.mode} ({why})"
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
