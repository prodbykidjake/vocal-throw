"""Primary feed with automatic fallback: republishes the primary while it is fresh, else the fallback."""
from __future__ import annotations

import asyncio
import logging
import time

from .base import FeedHealth, PriceFeed, Tick

log = logging.getLogger(__name__)


class CompositeFeed(PriceFeed):
    name = "feed"

    def __init__(self, primary: PriceFeed, fallback: PriceFeed | None = None, stale_after_s: float = 8.0,
                 warmup_grace_s: float = 10.0):
        super().__init__()
        self.primary = primary
        self.fallback = fallback
        self.stale_after_s = stale_after_s
        self.warmup_grace_s = warmup_grace_s
        self.active = "primary"
        self._primary_warmed = False
        self._fallback_warm: list[tuple[float, float]] | None = None
        self._task: asyncio.Task | None = None
        primary.subscribe(self._from_primary)
        primary.subscribe_warmup(self._warm_primary)
        if fallback is not None:
            fallback.subscribe(self._from_fallback)
            fallback.subscribe_warmup(self._warm_fallback)

    # ------------------------------------------------------------------ routing
    def primary_fresh(self) -> bool:
        t = self.primary.latest()
        return t is not None and (time.time() - t.ts) <= self.stale_after_s

    def _from_primary(self, tick: Tick):
        if self.active != "primary":
            log.info("feed: back to %s", self.primary.name)
        self.active = "primary"
        self._publish(tick.ts, tick.price)

    def _from_fallback(self, tick: Tick):
        if self.primary_fresh():
            return
        if self.active != "fallback":
            log.warning("feed: %s stale/unavailable, using %s", self.primary.name, self.fallback.name)
        self.active = "fallback"
        self._publish(tick.ts, tick.price)

    def _warm_primary(self, closes):
        self._primary_warmed = True
        self._publish_warmup(closes)

    def _warm_fallback(self, closes):
        self._fallback_warm = closes
        if self._primary_warmed:
            return
        # used only if the primary has not warmed up within the grace period (see _warm_watch)

    async def _warm_watch(self):
        await asyncio.sleep(self.warmup_grace_s)
        if not self._primary_warmed and self._fallback_warm:
            log.warning("feed: %s did not warm up in %.0fs, seeding from %s", self.primary.name, self.warmup_grace_s,
                        self.fallback.name)
            self._publish_warmup(self._fallback_warm)

    # ------------------------------------------------------------------ lifecycle
    async def start(self):
        await self.primary.start()
        if self.fallback is not None:
            await self.fallback.start()
            self._task = asyncio.create_task(self._warm_watch(), name="feed-warm-watch")

    async def stop(self):
        if self._task:
            self._task.cancel()
        await self.primary.stop()
        if self.fallback is not None:
            await self.fallback.stop()

    # ------------------------------------------------------------------ introspection
    @property
    def active_feed(self) -> PriceFeed:
        return self.primary if (self.active == "primary" or self.fallback is None) else self.fallback

    @property
    def active_name(self) -> str:
        return self.active_feed.name

    def health(self) -> FeedHealth:
        base = super().health()
        act = self.active_feed
        base.name = act.name
        base.connected = act.connected
        base.mode = act.mode if self.active == "primary" else f"FALLBACK ({act.mode})"
        base.symbol = act.symbol
        if self.fallback is not None:
            fb = self.fallback if self.active == "primary" else self.primary
            base.symbol = f"{act.symbol or act.name} (standby: {fb.symbol or fb.name})"
        base.last_error = act.last_error or (self.primary.last_error if self.active != "primary" else None)
        return base
