"""Full coach engines on the board: WTI runs the WTI coach's v12 engine, Natural Gas the gas coach's v1.0.1 engine and
Gold the gold coach's v1 engine, exactly as they run in their own apps (study period, learned volatility, settlement
timing, learner, settlement call and meter). The engines are copies inside this folder (packages `wti15m`, `ng15m`,
`gold15m`) with their own databases in this board's data folder: nothing is shared with the standalone apps.

On the board they share the board's rate limiter, and only their settlement calls notify (their signal-card BUY/SELL
and plan notifications stay quiet: the board is about the settlement call).
"""
from __future__ import annotations

import importlib
import logging
import os
import time

from .markets import MarketSpec, Scale

log = logging.getLogger(__name__)

# market key -> (package, label shown on the card)
ENGINES = {"wti": ("wti15m", "WTI v12 engine"), "natgas": ("ng15m", "gas v1.0.1 engine"), "gold": ("gold15m", "gold v1 engine")}


LOG_KINDS = {"call", "settled", "settle", "window", "feed", "config", "model", "recover"}


class CallsOnlyNotifier:
    """Passes on the engine's settlement-call notifications (unless the card is muted); drops BUY/SELL/plan ones."""

    def __init__(self, inner, key: str, is_muted):
        self.inner, self.key, self.is_muted = inner, key, is_muted

    def notify(self, title: str, message: str, sound_name: str = "Glass"):
        if "settles" in title and not self.is_muted(self.key):
            self.inner.notify(title, message, sound_name)


def _limited_client_class(pkg, limiter):
    base = importlib.import_module(f"{pkg}.kalshi").KalshiClient

    class LimitedClient(base):
        async def _get(self, path, params=None):
            await limiter.acquire()
            self.requests = getattr(self, "requests", 0) + 1
            return await super()._get(path, params)

    return LimitedClient


def _limited_live_class(pkg, limiter):
    base = importlib.import_module(f"{pkg}.feeds.kalshi_live").KalshiLiveFeed

    class LimitedLive(base):
        async def fetch_series(self, event_ticker, range_):
            await limiter.acquire()
            return await super().fetch_series(event_ticker, range_)

    return LimitedLive


class EngineDesk:
    """Wraps a coach Engine so the board can run and show it like its own desks."""

    def __init__(self, spec: MarketSpec, board_cfg, limiter, notifier, is_muted, sim: dict | None = None):
        pkg, self.engine_label = ENGINES[spec.key]
        self.spec = spec
        self.is_muted = is_muted
        config = importlib.import_module(f"{pkg}.config")
        engine_mod = importlib.import_module(f"{pkg}.engine")
        store_mod = importlib.import_module(f"{pkg}.store")
        cfg = config.Config()  # the engine's own defaults: exactly what it runs with in its own app
        cfg.storage.db_path = os.path.join(board_cfg.storage.data_dir, f"{spec.key}.db")
        cfg.notify.desktop, cfg.notify.sound = board_cfg.notify.desktop, board_cfg.notify.sound
        if sim is not None:  # the demo's short windows: the same clock scaling as the coaches' own demo
            t = cfg.trading
            window = sim.get("window_s", 900)
            t.min_warmup_minutes = board_cfg.trading.min_warmup_minutes
            t.no_entry_first_s = max(5, window // 9)
            t.study_s = max(5, window // 6)
            t.call_deadline_s = max(8, window // 6)
            t.call_join_min_s = window * 0.6
            t.call_min_left_s = 5
            t.call_hold_s = 3
            t.late_entry_s = max(5, window // 15)
        self.cfg = cfg
        store = store_mod.Store(cfg.storage.db_path)
        wrapped = CallsOnlyNotifier(notifier, spec.key, is_muted)
        if sim is not None:
            sim_feed = importlib.import_module(f"{pkg}.feeds.sim").SimFeed
            sim_kalshi = importlib.import_module(f"{pkg}.sim").SimKalshi
            feed = sim_feed(warmup_minutes=sim.get("warmup_minutes", 40))  # each package's sim is at its own scale
            client = sim_kalshi(feed, window_s=sim.get("window_s", 900), series_ticker=spec.series)
            self.engine = engine_mod.Engine(cfg, store, client, feed, wrapped)
        else:
            client = _limited_client_class(pkg, limiter)(cfg.kalshi.base_url, cfg.kalshi.timeout_s)
            composite = importlib.import_module(f"{pkg}.feeds.composite").CompositeFeed
            live_mod = importlib.import_module(f"{pkg}.feeds.kalshi_live")
            primary = _limited_live_class(pkg, limiter)(cfg.kalshi.base_url, poll_s=cfg.feed.kalshi_live_poll_s)
            fallback = None
            if cfg.feed.fallback == "hyperliquid":
                hl = importlib.import_module(f"{pkg}.feeds.hyperliquid").HyperliquidFeed
                fallback = hl(cfg.feed.hyperliquid_rest, cfg.feed.hyperliquid_ws, cfg.feed.hyperliquid_dex,
                              cfg.feed.hyperliquid_symbol, cfg.feed.warmup_candles_min)
            feed = composite(primary, fallback, cfg.feed.stale_after_s)
            self.engine = engine_mod.Engine(cfg, store, client, feed, wrapped)
            eng = self.engine
            primary.event_ticker_fn = lambda: (live_mod.event_ticker_for(eng.tracker.current.ticker, eng.tracker.current.event_ticker)
                                               if eng.tracker.current else None)
        self.client = client
        self.feed = self.engine.feed
        self.tracker = self.engine.tracker
        self.scale = Scale(decimals=spec.decimals)
        self.state: dict = {"key": spec.key, "status": "starting"}

    def step(self):
        self.engine.step()
        st = self.engine.state
        m, tick = st.get("market"), st.get("tick")
        if m and m.get("strike"):
            self.scale.observe_target(m["strike"])
        if tick:
            self.scale.observe_price(tick["price"])
        status = st.get("status") or "starting"
        self.state = {
            "key": self.spec.key, "name": self.spec.name, "series": self.spec.series, "badge": self.spec.badge,
            "color": self.spec.color, "kind": self.spec.kind, "status": status, "engine": self.engine_label,
            "scale": self.scale.as_dict(), "market": m, "quotes_fresh": st.get("quotes_fresh"),
            "seconds_left": st.get("seconds_left"), "countdown": st.get("countdown"), "tick": tick,
            "feed": st.get("feed") or {}, "prediction": st.get("prediction"), "vol": st.get("vol") or {},
            "call": st.get("call") or {}, "muted": self.is_muted(self.spec.key),
            "tracker_error": (st.get("tracker") or {}).get("last_error"),
        }

    def detail(self, minutes: float = 20) -> dict:
        e = self.engine
        return {"key": self.spec.key, "engine": self.engine_label, "chart": e.chart(minutes),
                "calls": e.store.call_record(60)["recent"], "stats": e.stats(),
                # the board is about the settlement call: leave out the engine's signal-card and plan lines
                "events": [ev for ev in e.events if ev.get("kind") in LOG_KINDS][:30]}

    @property
    def requests(self) -> int:
        return getattr(self.client, "requests", 0)

    async def start(self, stop):
        await self.engine.feed.start()
        await self.engine.tracker.run(stop)

    async def stop(self):
        try:
            await self.engine.feed.stop()
        except Exception:
            pass
