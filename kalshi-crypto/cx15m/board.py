"""The board: one settlement-calling desk per 15-minute crypto market, sharing one rate-limited Kalshi connection.

Each desk polls its own order book every 2 seconds, its window list every 10 and Kalshi's 60-second average every 5,
staggered, so nine markets cost about 7 requests a second, under the 10/s the shared limiter allows (Kalshi's public
tier is ~20/s per connection, and the commodities board on the same computer uses about 8). The coins' prices come from one shared connection per exchange (feeds/exchanges.py), or from
Kalshi's exact index with an API key (feeds/kalshi_index.py). One 1 Hz loop steps every desk and publishes the
combined state.
"""
from __future__ import annotations

import asyncio
import copy
import dataclasses
import json
import logging
import os
import time

from . import tuning
from .config import Config
from .desk import Desk
from .edge import EdgeStore, EdgeTracker, points as edge_points
from .feeds.calibrated import CalibratedFeed
from .feeds.composite import FallbackFeed
from .feeds.exchanges import ExchangeFeed, ExchangeHub
from .feeds.kalshi_index import KalshiIndexFeed, KalshiIndexHub, has_key, key_settings
from .feeds.kalshi_live import KalshiLiveFeed, event_ticker_for
from .kalshi import KalshiClient, RateLimiter
from .markets import KNOWN, MarketSpec, discover
from .notify import Notifier
from .pullout import PulloutStore, PulloutTracker
from .shadow import ShadowCalls
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
        saved = self._load_saved()
        self.muted: set[str] = set(cfg.board.muted) | set(saved.get("muted", []))
        self.mode: str = tuning.mode_of(saved.get("mode", cfg.board.call_mode))  # the settlement calls' strictness
        self.state: dict = {"status": "starting", "markets": []}
        self.started_ts = time.time()
        self.edge_store = EdgeStore(os.path.join(cfg.storage.data_dir, "edges.db"))
        self.edges: dict[str, EdgeTracker] = {}
        self._last_edge_settle = 0.0
        self.pullout_store = PulloutStore(os.path.join(cfg.storage.data_dir, "pullouts.db"))
        self.pullouts: dict[str, PulloutTracker] = {}
        self.shadows: dict[str, dict[str, ShadowCalls]] = {}  # every mode on the same windows (cx15m/shadow.py)
        self.exchange_hub: ExchangeHub | None = None  # one connection per exchange for every coin
        self.index_hub: KalshiIndexHub | None = None  # the exact index (with a Kalshi API key)
        self._source: str | None = None  # the price source, worked out once (feed_source)

    # ------------------------------------------------------------------ mutes (per card, remembered)
    def _load_saved(self) -> dict:
        try:
            with open(self._mute_path, encoding="utf-8") as fh:
                data = json.load(fh)
                return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def _save(self):
        os.makedirs(self.cfg.storage.data_dir, exist_ok=True)
        with open(self._mute_path, "w", encoding="utf-8") as fh:
            json.dump({"muted": sorted(self.muted), "mode": self.mode}, fh)

    def set_muted(self, key: str, muted: bool):
        if key not in {d.spec.key for d in self.desks}:
            raise KeyError(key)
        (self.muted.add if muted else self.muted.discard)(key)
        self._save()

    def set_mode(self, mode: str):
        """Switch every card's settlement call to Standard / Strict / Very strict (remembered). A call already made
        in the current window stays as it is; the next one follows the new mode."""
        if not tuning.enabled(self.cfg.tuning):
            raise ValueError("the strictness switch is off ([tuning] enabled = false in config.toml)")
        if str(mode).strip().lower().replace("-", "_").replace(" ", "_") not in tuning.MODES:
            raise ValueError("mode must be standard, strict or very_strict")
        self.mode = tuning.mode_of(mode)
        window_s = None if self.sim is None else self.sim.get("window_s", 900)
        for d in self.desks:
            src = getattr(d, "engine", None) or d
            tuning.apply(src.cfg.trading, tuning.for_market(d.spec.key, self.cfg.tuning, self.mode), window_s,
                         tuning.tag(self.mode))
            src.calls.refresh_rules()
        self._save()

    # ------------------------------------------------------------------ desks
    async def build(self):
        specs = self._specs
        if specs is None:
            specs = await discover(self.client, self.cfg.kalshi.category) if self.client else list(KNOWN)
        wanted = [k.lower() for k in self.cfg.board.markets]
        if wanted:
            specs = [s for s in specs if s.key in wanted or s.series.lower() in wanted]
        tuning.check(self.cfg.tuning, [s.key for s in specs])
        for spec in specs:
            self.desks.append(self._make_desk(spec))
            self.edges[spec.key] = EdgeTracker(spec.key, self.edge_store, self.cfg.board.edge_gap, self.cfg.board.edge_min_p)
            self.pullouts[spec.key] = self._make_pullout(spec.key)
            if tuning.enabled(self.cfg.tuning):
                self.shadows[spec.key] = {
                    mode: ShadowCalls(spec.key, mode, self.cfg.trading, self.cfg.storage.data_dir, self.cfg.tuning,
                                      None if self.sim is None else self.sim.get("window_s", 900))
                    for mode in tuning.MODES}
        log.info("board: %d markets: %s", len(self.desks), ", ".join(d.spec.name for d in self.desks))

    def _make_desk(self, spec: MarketSpec):
        store = Store(os.path.join(self.cfg.storage.data_dir, f"{spec.key}.db"))
        cfg = self._desk_cfg(spec.key)
        if self.sim is not None:
            from .feeds.sim import SimFeed
            from .sim import SimKalshi
            feed = SimFeed(start_price=spec.sim_price, sigma_per_sqrt_s=spec.sim_sigma,
                           warmup_minutes=self.sim.get("warmup_minutes", 40))
            client = SimKalshi(feed, window_s=self.sim.get("window_s", 900), series_ticker=spec.series,
                               decimals=spec.decimals, name=spec.name, settle_avg_s=cfg.trading.settle_avg_s)
            return Desk(spec, cfg, store, client, feed, self.notifier, self.is_muted)
        feed = self._make_feed(spec)
        desk = Desk(spec, cfg, store, self.client, feed, self.notifier, self.is_muted)
        for live in _kalshi_series_feeds(feed):  # Kalshi's average follows the desk's current window
            live.event_ticker_fn = lambda d=desk: (event_ticker_for(d.tracker.current.ticker, d.tracker.current.event_ticker)
                                                   if d.tracker.current else None)
        return desk

    def feed_source(self) -> str:
        """The live price each card reads ([feed] source, see config.example.toml), worked out once:
        kalshi-index (with an API key) | calibrated (the exchanges matched to Kalshi's own average) | exchanges."""
        if self._source is not None:
            return self._source
        src = str(self.cfg.feed.source or "auto").strip().lower()
        if src == "kalshi-live":
            src = "calibrated"  # older name: Kalshi's chart series is the 60-second average, so it calibrates
        if src not in ("auto", "exchanges", "kalshi-index", "calibrated"):
            log.warning("config: [feed] source = %r is not auto/calibrated/exchanges/kalshi-index; using auto", src)
            src = "auto"
        if src == "kalshi-index" and not has_key(self.cfg.kalshi):
            log.warning("config: [feed] source = kalshi-index needs [kalshi] api_key_id and private_key_path; "
                        "using the exchanges matched to Kalshi's average")
            src = "calibrated"
        if src == "auto":
            src = "kalshi-index" if has_key(self.cfg.kalshi) else "calibrated"
        self._source = src
        return src

    def _make_feed(self, spec: MarketSpec):
        src = self.feed_source()
        if self.exchange_hub is None:
            self.exchange_hub = ExchangeHub(list(self.cfg.feed.exchanges), self.cfg.feed.quote_max_age_s)
        exchanges = ExchangeFeed(self.exchange_hub, spec.badge, spec.name)
        if src == "exchanges":
            return exchanges
        average = KalshiLiveFeed(self.cfg.kalshi.base_url, poll_s=self.cfg.kalshi.average_poll_s, client=self.client,
                                 symbol=f"{spec.name}: Kalshi's 60-second average")
        calibrated = CalibratedFeed(exchanges, average)
        if src == "calibrated" or not spec.index:
            return calibrated
        if self.index_hub is None:
            key_id, path = key_settings(self.cfg.kalshi)
            self.index_hub = KalshiIndexHub(self.cfg.kalshi.ws_url, key_id, path)
        return FallbackFeed(KalshiIndexFeed(self.index_hub, spec.index, spec.name), calibrated)

    def _make_pullout(self, key: str) -> PulloutTracker:
        tr = PulloutTracker(key, self.pullout_store)
        if self.sim is not None:  # the demo's short windows: the last-minute cut-off and Rule B's hold shrink with them
            frac = self.sim.get("window_s", 900) / 900.0
            tr.min_left_s, tr.hold_s = max(5.0, 60.0 * frac), max(2.0, 10.0 * frac)
        return tr

    def _desk_cfg(self, key: str) -> Config:
        """A desk's own copy of the settings, with its market's settlement-call rules (cx15m/tuning.py)."""
        t = copy.copy(self.cfg.trading)
        if tuning.enabled(self.cfg.tuning):
            tuning.apply(t, tuning.for_market(key, self.cfg.tuning, self.mode),
                         None if self.sim is None else self.sim.get("window_s", 900), tuning.tag(self.mode))
        if self.sim is not None:  # the demo's short windows settle on a proportionally short closing average
            t.settle_avg_s = max(3.0, round(t.settle_avg_s * self.sim.get("window_s", 900) / 900.0))
        return dataclasses.replace(self.cfg, trading=t)

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
            try:
                self._pullout_step(d, now, settle)
            except Exception:
                log.exception("desk %s pull-out step failed", d.spec.key)
            try:
                self._shadow_step(d, now, settle)
            except Exception:
                log.exception("desk %s shadow step failed", d.spec.key)
            try:
                self._early_step(d)
            except Exception:
                log.exception("desk %s early record failed", d.spec.key)
        self.state = self._build_state()

    def _edge_step(self, d, now: float, settle: bool):
        tr = self.edges.get(d.spec.key)
        if tr is None:
            tr = self.edges[d.spec.key] = EdgeTracker(d.spec.key, self.edge_store, self.cfg.board.edge_gap, self.cfg.board.edge_min_p)
        if self.edge_mode() == "off":
            d.state["edge"] = {"off": True}
            return
        sig = tr.step(now, d.state)
        if sig and self.edge_mode() == "on" and not self.is_muted(d.spec.key):
            ask_c = round(sig["ask"] * 100)
            big = ""
            if sig.get("tier") == "big":  # v1.6: say how big-gap signals have done on this board so far
                b = self.edge_store.record(recent=0)["tiers"]["big"]
                big = (" · BIG GAP: Kalshi's price disagrees a lot, keep your normal size" +
                       (f" (big gaps so far {b['wins']}-{b['losses']}, {100 * b['pnl'] / b['n']:+.1f}¢ a signal)" if b["n"] else ""))
            self.notifier.notify(f"{d.spec.name.upper()} EDGE: BUY {sig['side']}",
                                 f"model {round(sig['p'] * 100)}% vs ask {ask_c}¢ (+{edge_points(sig['gap'])} pts) · "
                                 f"buy at {ask_c}¢ or less, hold to settlement{big}", "Glass")
        if settle:
            store = self._window_store(d)
            if store is not None:
                tr.settle_from(store.window, now)
        d.state["edge"] = tr.state()

    def _pullout_step(self, d, now: float, settle: bool):
        tr = self.pullouts.get(d.spec.key)
        if tr is None:
            tr = self.pullouts[d.spec.key] = self._make_pullout(d.spec.key)
        if settle:  # score before stepping, so a card shows its alarm's result as soon as it is known
            store = self._window_store(d)
            if store is not None:
                tr.settle_from(store.window, now)
        alarm = tr.step(now, d.state)
        if alarm:
            left = alarm["seconds_left"]
            bid = "" if alarm["bid"] is None else f" (bid ~{round(alarm['bid'] * 100)}¢)"
            kalshi = "" if alarm["p_kalshi"] is None else f" · Kalshi {_pct(alarm['p_kalshi'])}"
            # urgent and rare (about 7 calls in 100): it notifies even when the card's call notifications are muted
            self.notifier.notify(f"{d.spec.name.upper()}: PULL OUT of {alarm['side']}",
                                 f"the {alarm['side']} call has flipped: engine {_pct(alarm['p_model'])}{kalshi} with "
                                 f"{int(left // 60)}:{int(left % 60):02d} left · sell your {alarm['side']} now{bid}", "Basso")
        d.state["pullout"] = tr.state()

    def edge_mode(self) -> str:
        """on | watch | off ([board] edge): on crypto EDGE is watched, not alerted, until it proves itself live."""
        mode = str(self.cfg.board.edge or "watch").strip().lower()
        return mode if mode in ("on", "watch", "off") else "watch"

    def early_left_s(self) -> float:
        """Seconds left from which a call counts as EARLY (6:30; the demo's short windows scale it like every clock)."""
        return tuning.EARLY_LEFT_S * (1.0 if self.sim is None else self.sim.get("window_s", 900) / 900.0)

    def _early_step(self, d):
        """v1.6: the record of the calls made EARLY under the desk's current rules (its rules tag; none = untagged)."""
        store = self._window_store(d)
        src = getattr(d, "engine", None) or d
        if store is None or getattr(src, "cfg", None) is None:
            return
        tag = src.cfg.trading.rules_tag or ""
        q = ("SELECT COUNT(*), COALESCE(SUM(correct), 0) FROM calls WHERE result IS NOT NULL AND seconds_left >= ? AND "
             + ("rules = ?" if tag else "rules IS NULL"))
        n, wins = store.conn.execute(q, (self.early_left_s(), tag) if tag else (self.early_left_s(),)).fetchone()
        d.state["early"] = {"wins": int(wins), "losses": int(n) - int(wins), "tag": tag}

    def _shadow_step(self, d, now: float, settle: bool):
        shadows = self.shadows.get(d.spec.key)
        if not shadows:
            return
        store = self._window_store(d) if settle else None
        for sh in shadows.values():
            sh.step(d, now)
            if store is not None:
                sh.settle_from(store.window)
        d.state["modes"] = {mode: sh.state() for mode, sh in shadows.items()}

    @staticmethod
    def _window_store(d):
        return getattr(d, "store", None) or getattr(getattr(d, "engine", None), "store", None)

    def _build_state(self) -> dict:
        markets = [d.state for d in self.desks]
        total = {"wins": 0, "losses": 0, "market_expected": 0.0, "n_market": 0}
        rules_total = {"wins": 0, "losses": 0, "since": None, "tag": tuning.tag(self.mode)}  # calls made in the current mode only
        modes_total = {mode: {"wins": 0, "losses": 0} for mode in tuning.MODES} if self.shadows else {}
        lines = []
        for d in self.desks:
            c = d.state.get("call") or {}
            r = c.get("record") or {}
            for k in ("wins", "losses", "n_market"):
                total[k] += r.get(k, 0) or 0
            total["market_expected"] += r.get("market_expected", 0.0) or 0.0
            rr = c.get("record_rules")
            if rr:
                rules_total["wins"] += rr.get("wins", 0) or 0
                rules_total["losses"] += rr.get("losses", 0) or 0
                since = rr.get("since")
                if since is not None and (rules_total["since"] is None or since < rules_total["since"]):
                    rules_total["since"] = since
            for mode, rec in (d.state.get("modes") or {}).items():
                if mode in modes_total:
                    modes_total[mode]["wins"] += rec.get("wins", 0) or 0
                    modes_total[mode]["losses"] += rec.get("losses", 0) or 0
            lines.append({"key": d.spec.key, "name": d.spec.name, "wins": r.get("wins", 0), "losses": r.get("losses", 0)})
        total["market_expected"] = round(total["market_expected"], 1)
        return {"now": time.time(), "status": "live", "uptime_s": round(time.time() - self.started_ts),
                "markets": markets, "record": total, "lines": lines, "edge": self.edge_store.record(recent=0),
                "pullout": self.pullout_store.record(recent=0), "record_rules": rules_total, "modes": modes_total,
                "mode": self.mode, "mode_labels": tuning.MODE_LABELS, "modes_on": tuning.enabled(self.cfg.tuning),
                "early_left_s": self.early_left_s(), "edge_mode": self.edge_mode(),
                "requests": None if self.sim is not None else (getattr(self.client, "requests", 0) or 0),
                "throttled": self.limiter.throttled, "feed_source": None if self.sim is not None else self.feed_source()}


def _kalshi_series_feeds(feed):
    """Every KalshiLiveFeed inside a (possibly composite) feed: they need the desk's current event."""
    if isinstance(feed, KalshiLiveFeed):
        yield feed
    for attr in ("primary", "backup", "raw", "avg"):
        inner = getattr(feed, attr, None)
        if inner is not None and inner is not feed:
            yield from _kalshi_series_feeds(inner)


def _pct(p: float | None) -> str:
    return "?" if p is None else f"{round(p * 100)}%"
