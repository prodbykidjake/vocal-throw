"""The board: one settlement-calling desk per 15-minute commodity, sharing one rate-limited Kalshi connection.

Each desk polls its own window list, order book and live price on a staggered 2-second rhythm, so seven markets cost
about 8 requests a second, under the 10/s the shared limiter allows (Kalshi's public tier is ~20/s, and the
standalone coaches need their share). One 1 Hz loop steps every desk and publishes the combined state.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import time

from .config import Config
from .desk import Desk
from .edge import EdgeStore, EdgeTracker
from .engines import ENGINES, EngineDesk
from .feeds.kalshi_live import KalshiLiveFeed, event_ticker_for
from .kalshi import KalshiClient, RateLimiter
from .markets import KNOWN, MarketSpec, discover
from .notify import Notifier
from .store import Store

log = logging.getLogger(__name__)


class Board:
    def __init__(self, cfg: Config, specs: list[MarketSpec] | None = None, client=None, sim: dict | None = None):
        """`sim` (demo/tests): {"window_s": 90, "warmup_minutes": 5} builds a simulated market per spec instead."""
        self.cfg = cfg
        self.sim = sim
        self.limiter = RateLimiter(cfg.kalshi.max_requests_per_s)
        self.client = client or (None if sim else KalshiClient(cfg.kalshi.base_url, cfg.kalshi.timeout_s, limiter=self.limiter))
        self.notifier = Notifier(cfg.notify.desktop, cfg.notify.sound)
        self.desks: list[Desk] = []
        self._specs = specs
        self._mute_path = os.path.join(cfg.storage.data_dir, "board.json")
        self.muted: set[str] = set(cfg.board.muted) | self._load_mutes()
        self.state: dict = {"status": "starting", "markets": []}
        self.started_ts = time.time()
        self.edge_store = EdgeStore(os.path.join(cfg.storage.data_dir, "edges.db"))
        self.edges: dict[str, EdgeTracker] = {}
        self._last_edge_settle = 0.0

    # ------------------------------------------------------------------ mutes (per card, remembered)
    def _load_mutes(self) -> set[str]:
        try:
            with open(self._mute_path, encoding="utf-8") as fh:
                return set(json.load(fh).get("muted", []))
        except (OSError, ValueError):
            return set()

    def set_muted(self, key: str, muted: bool):
        if key not in {d.spec.key for d in self.desks}:
            raise KeyError(key)
        (self.muted.add if muted else self.muted.discard)(key)
        os.makedirs(self.cfg.storage.data_dir, exist_ok=True)
        with open(self._mute_path, "w", encoding="utf-8") as fh:
            json.dump({"muted": sorted(self.muted)}, fh)

    # ------------------------------------------------------------------ desks
    async def build(self):
        specs = self._specs
        if specs is None:
            specs = await discover(self.client, self.cfg.kalshi.category) if self.client else list(KNOWN)
        wanted = [k.lower() for k in self.cfg.board.markets]
        if wanted:
            specs = [s for s in specs if s.key in wanted or s.series.lower() in wanted]
        for spec in specs:
            self.desks.append(self._make_desk(spec))
            self.edges[spec.key] = EdgeTracker(spec.key, self.edge_store, self.cfg.board.edge_gap, self.cfg.board.edge_min_p)
        log.info("board: %d markets: %s", len(self.desks), ", ".join(d.spec.name for d in self.desks))

    def _make_desk(self, spec: MarketSpec):
        if spec.key in ENGINES:  # WTI, Natural Gas and Gold run their coaches' own engines
            return EngineDesk(spec, self.cfg, self.limiter, self.notifier, self.is_muted, self.sim)
        store = Store(os.path.join(self.cfg.storage.data_dir, f"{spec.key}.db"))
        if self.sim is not None:
            from .feeds.sim import SimFeed
            from .sim import SimKalshi
            feed = SimFeed(start_price=spec.sim_price, sigma_per_sqrt_s=spec.sim_sigma,
                           warmup_minutes=self.sim.get("warmup_minutes", 40))
            client = SimKalshi(feed, window_s=self.sim.get("window_s", 900), series_ticker=spec.series,
                               decimals=spec.decimals, name=spec.name)
            return Desk(spec, self.cfg, store, client, feed, self.notifier, self.is_muted)
        feed = KalshiLiveFeed(self.cfg.kalshi.base_url, poll_s=self.cfg.kalshi.poll_s, client=self.client,
                              symbol=f"{spec.name} (Pyth via Kalshi)")
        desk = Desk(spec, self.cfg, store, self.client, feed, self.notifier, self.is_muted)
        feed.event_ticker_fn = lambda d=desk: (event_ticker_for(d.tracker.current.ticker, d.tracker.current.event_ticker)
                                               if d.tracker.current else None)
        return desk

    def is_muted(self, key: str) -> bool:
        return key in self.muted

    def desk(self, key: str) -> Desk | None:
        return next((d for d in self.desks if d.spec.key == key), None)

    # ------------------------------------------------------------------ run
    async def run(self, stop: asyncio.Event | None = None):
        stop = stop or asyncio.Event()
        if not self.desks:
            await self.build()
        tasks = []
        n = max(len(self.desks), 1)
        for i, d in enumerate(self.desks):
            # stagger the markets across the 2-second rhythm so the requests spread out evenly
            tasks.append(asyncio.create_task(self._start_desk(d, i * self.cfg.kalshi.poll_s / n, stop), name=f"desk-{d.spec.key}"))
        try:
            while not stop.is_set():
                self.step()
                try:
                    await asyncio.wait_for(stop.wait(), timeout=1.0)
                except asyncio.TimeoutError:
                    pass
        finally:
            for t in tasks:
                t.cancel()
            for d in self.desks:
                try:
                    await d.feed.stop()
                except Exception:
                    pass

    async def _start_desk(self, d, delay: float, stop: asyncio.Event):
        await asyncio.sleep(delay)
        if isinstance(d, EngineDesk):
            await d.start(stop)
            return
        await d.feed.start()
        await d.tracker.run(stop)

    def step(self):
        now = time.time()
        settle = now - self._last_edge_settle >= 10
        if settle:
            self._last_edge_settle = now
        for d in self.desks:
            try:
                d.step()
            except Exception:
                log.exception("desk %s step failed", d.spec.key)
            try:
                self._edge_step(d, now, settle)
            except Exception:
                log.exception("desk %s edge step failed", d.spec.key)
        self.state = self._build_state()

    def _edge_step(self, d, now: float, settle: bool):
        tr = self.edges.get(d.spec.key)
        if tr is None:
            tr = self.edges[d.spec.key] = EdgeTracker(d.spec.key, self.edge_store, self.cfg.board.edge_gap, self.cfg.board.edge_min_p)
        sig = tr.step(now, d.state)
        if sig and not self.is_muted(d.spec.key):
            ask_c = round(sig["ask"] * 100)
            self.notifier.notify(f"{d.spec.name.upper()} EDGE: BUY {sig['side']}",
                                 f"model {round(sig['p'] * 100)}% vs ask {ask_c}¢ (+{round(sig['gap'] * 100)} pts) · "
                                 f"buy at {ask_c}¢ or less, hold to settlement", "Glass")
        if settle:
            store = getattr(d, "store", None) or getattr(getattr(d, "engine", None), "store", None)
            if store is not None:
                tr.settle_from(store.window, now)
        d.state["edge"] = tr.state()

    def _build_state(self) -> dict:
        markets = [d.state for d in self.desks]
        total = {"wins": 0, "losses": 0, "market_expected": 0.0, "n_market": 0}
        lines = []
        for d in self.desks:
            r = (d.state.get("call") or {}).get("record") or {}
            for k in ("wins", "losses", "n_market"):
                total[k] += r.get(k, 0) or 0
            total["market_expected"] += r.get("market_expected", 0.0) or 0.0
            lines.append({"key": d.spec.key, "name": d.spec.name, "wins": r.get("wins", 0), "losses": r.get("losses", 0)})
        total["market_expected"] = round(total["market_expected"], 1)
        return {"now": time.time(), "status": "live", "uptime_s": round(time.time() - self.started_ts),
                "markets": markets, "record": total, "lines": lines, "edge": self.edge_store.record(recent=0),
                "requests": None if self.sim is not None else (getattr(self.client, "requests", 0) or 0)
                + sum(d.requests for d in self.desks if isinstance(d, EngineDesk)),
                "throttled": self.limiter.throttled}
