"""Simulated WTI feed (dollar random walk, sigma in $/√s) for demos and tests when the real APIs are unreachable."""
from __future__ import annotations

import asyncio
import math
import random
import time

from .base import PriceFeed


class SimFeed(PriceFeed):
    name = "sim"

    def __init__(self, start_price: float = 90.0, sigma_per_sqrt_s: float = 0.005, tick_s: float = 0.5,
                 warmup_minutes: int = 60, seed: int | None = None, speed: float = 1.0):
        super().__init__()
        self.price = start_price
        self.sigma = sigma_per_sqrt_s
        self.tick_s = tick_s
        self.warmup_minutes = warmup_minutes
        self.rng = random.Random(seed)
        self.speed = speed
        self.symbol = "SIM:WTI"
        self._task: asyncio.Task | None = None
        self._stop = asyncio.Event()

    def _step(self, dt_s: float):
        self.price += self.rng.gauss(0.0, self.sigma * math.sqrt(dt_s))
        return self.price

    async def start(self):
        now = time.time()
        closes = []
        px = self.price
        # build a backwards-consistent warm-up history
        path = []
        for i in range(self.warmup_minutes):
            path.append(px)
            px += self.rng.gauss(0.0, self.sigma * math.sqrt(60))
        path.reverse()
        for i, p in enumerate(path):
            closes.append((now - (self.warmup_minutes - i) * 60.0, p))
        self.price = path[-1]
        self._publish_warmup(closes)
        self.connected = True
        self.mode = "simulated"
        self._stop.clear()
        self._task = asyncio.create_task(self._run(), name="sim-feed")

    async def stop(self):
        self._stop.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass

    async def _run(self):
        while not self._stop.is_set():
            await asyncio.sleep(self.tick_s / self.speed)
            self._publish(time.time(), self._step(self.tick_s))
