"""One coin's settlement caller: the commodities board's desk (cm15m) with crypto's settlement rule.

Each desk follows one 15-minute series: the live window (MarketTracker), a live price for the coin (the exchanges'
median, or Kalshi's exact index with an API key), the model (settle on the average of the last minute's 60 one-second
index prints, learned volatility by weekday/weekend hour and report, learner without trend features), the locked
settlement call with its meter, and what it learns at settlement: the feed's own closing-minute average against the
settlement value, the window's real volatility, and the call's result. Each desk has its own database.
"""
from __future__ import annotations

import datetime as dt
import logging
import math
import statistics
import time
from collections import deque

from . import clock, volcal
from .calls import CallTracker
from .config import Config
from .fees import FeeSchedule
from .feeds.base import PRINT_MAX_AGE_S, PriceFeed, Tick, on_backup, second_average
from .kalshi import Market, MarketTracker
from .markets import MarketSpec, Scale
from .model import Calibrator, Model, Prediction
from .notify import Notifier
from .quotes import Quotes
from .store import Store

log = logging.getLogger(__name__)

QUOTE_MAX_AGE_S = 10.0
def fmt_price(x: float | None, decimals: int) -> str:
    return "?" if x is None else f"${x:,.{max(decimals, 2)}f}"


def fmt_move(d: float | None, scale: Scale) -> str:
    """A gap or move in the market's natural unit: cents for the small coins, dollars for the big ones."""
    if d is None:
        return "?"
    if scale.moves_in_cents():
        digits = max(0, scale.decimals - 2)
        return f"{abs(d) * 100:.{digits}f}¢"
    return f"${abs(d):,.2f}"


class Desk:
    def __init__(self, spec: MarketSpec, cfg: Config, store: Store, client, feed: PriceFeed,
                 notifier: Notifier | None = None, is_muted=lambda key: False):
        self.spec = spec
        self.cfg = cfg
        self.store = store
        self.feed = feed
        self.notifier = notifier or Notifier(False, False)
        self.is_muted = is_muted
        t = cfg.trading
        self.scale = Scale(decimals=spec.decimals)
        for row in store.recent_windows(40):  # targets seen before: the decimals Kalshi writes them with
            self.scale.observe_target(row.get("strike"))
        self.model = Model(calibrator=Calibrator.from_json(store.get_state("calibrator")), tie_adj=self.scale.tie_adj,
                           min_warmup_s=t.min_warmup_minutes * 60, confident_margin=t.confident_margin,
                           stale_after_s=cfg.feed.stale_after_s, settle_avg_s=t.settle_avg_s)
        self._apply_scale()
        self.fees = FeeSchedule()
        self.tracker = MarketTracker(client, spec.series, cfg.kalshi.poll_s, on_event=self.on_market_event)
        self.tracker.discovery_interval_s = cfg.kalshi.discovery_s
        self.events: deque[dict] = deque(maxlen=40)
        self.pred: Prediction | None = None
        self.state: dict = {"key": spec.key, "status": "starting"}
        self._basis_source: str | None = None
        self._last_snapshot = 0.0
        self._last_window_upsert = 0.0
        self._last_quote_store = 0.0
        self._last_stored_tick_ts: float | None = None
        self._last_prune = time.time()
        self._close_capture: dict[str, dict] = {}
        self._window_strikes: dict[int, float] = {}
        self._forecast_done: set[str] = set()
        self._vol_info: dict = {}
        self._stats_cache: tuple[float, dict] = (0.0, {})
        self.vol_profile = volcal.VolProfile(t.vol_profile_k, t.event_prior)
        self._extra = volcal.parse_extra_events(list(spec.dated) + list(cfg.board.extra_events.get(spec.key, [])))
        self.calls = CallTracker(t, store)
        self.feed.subscribe(self.on_tick)
        self.feed.subscribe_warmup(self.on_warmup)
        self._recover()
        self._load_basis(self.feed.active_name)
        self._refit_vol()
        for row in store.unsettled_calls():  # windows that settled while the board was off
            self.calls.settle(row["ticker"], row["result"] == "yes")

    # ------------------------------------------------------------------ scale
    def _apply_scale(self):
        self.model.tie_adj = self.scale.tie_adj
        if self.scale.price:
            self.model.vol.floor = self.scale.vol_floor()
            self.model.vol.cap = self.scale.vol_cap()

    def log_event(self, kind: str, text: str):
        self.events.appendleft({"ts": time.time(), "kind": kind, "text": text})
        log.info("[%s] [%s] %s", self.spec.key, kind, text)

    # ------------------------------------------------------------------ restart recovery
    def _recover(self):
        now = time.time()
        for row in self.store.recent_windows(80):
            open_ts, close_ts = row.get("open_time"), row.get("close_time")
            if row.get("strike") and open_ts:
                self._window_strikes[int(round(open_ts))] = row["strike"]
            if not close_ts or close_ts > now or now - close_ts > 90 * 60 or row.get("result") in ("yes", "no"):
                continue
            m = Market(ticker=row["ticker"], status=row.get("status") or "closed", strike=row.get("strike"),
                       strike_source=row.get("strike_source") or "none",
                       open_time=dt.datetime.fromtimestamp(open_ts, clock.UTC) if open_ts else None,
                       close_time=dt.datetime.fromtimestamp(close_ts, clock.UTC))
            self.tracker.pending[m.ticker] = (m, close_ts)
            source = row.get("feed_source") or self.feed.active_name
            feed_px = row.get("feed_price_at_close")
            if feed_px is None:
                feed_px = self._feed_avg(close_ts, source)
            self._close_capture[m.ticker] = {
                "feed_price": feed_px, "p_market": row.get("p_market_at_close"), "close_ts": close_ts,
                "p_model": row.get("p_model_at_close"), "strike": row.get("strike"), "source": source,
                "settle_price": row.get("settle_price"), "err_lag0": row.get("feed_error_lag0"),
                "err_lag60": row.get("feed_error_lag60")}

    # ------------------------------------------------------------------ feed callbacks
    def on_warmup(self, closes):
        if closes:
            self.scale.observe_price(closes[-1][1])
            self._apply_scale()
        self.model.vol.seed_from_closes(closes)

    def on_tick(self, tick: Tick):
        if self.scale.price is None or abs(tick.price / self.scale.price - 1) > 0.2:
            self.scale.observe_price(tick.price)
            self._apply_scale()
        self.model.vol.update(tick.ts, tick.price)

    # ------------------------------------------------------------------ market callbacks
    def _note_strike(self, m: Market | None):
        if m is not None and m.strike is not None and m.strike > 0 and m.open_time is not None:
            self._window_strikes[int(round(m.open_time.timestamp()))] = m.strike
            if len(self._window_strikes) > 500:
                for key in sorted(self._window_strikes)[:-200]:
                    del self._window_strikes[key]

    def _feed_price_near(self, ts: float, window_s: float = 5.0, source: str | None = None) -> float | None:
        price = self.feed.buffer.price_near(ts, window_s)
        if price is None:
            price = self.store.last_tick_before(ts, source or self.feed.active_name, window_s)
        return price

    def _feed_avg(self, end_ts: float, source: str | None = None) -> float | None:
        """Our feed's version of the settlement value: the average of its one-second prints over the settlement minute
        ending at `end_ts` (from memory, or from the database after a restart)."""
        w = self.cfg.trading.settle_avg_s
        if w <= 0:
            return self._feed_price_near(end_ts, source=source)
        start = end_ts - w
        ticks = [(t.ts, t.price) for t in self.feed.buffer.since(start - PRINT_MAX_AGE_S) if t.ts <= end_ts]
        avg = second_average(ticks, start, end_ts)
        if avg is None:
            stored = self.store.ticks_between(start - PRINT_MAX_AGE_S, end_ts, source or self.feed.active_name)
            avg = second_average(stored, start, end_ts)
        return avg

    def _avg_sofar(self, close_ts: float, now: float) -> float | None:
        """Inside the settlement minute: the average of the prints already in (None before it, or without prints)."""
        w = self.cfg.trading.settle_avg_s
        if w <= 0 or not close_ts - w <= now < close_ts:
            return None
        exact = getattr(self.feed, "settle_avg", None)  # Kalshi's own running average, with the exact index
        if exact is not None and exact(now):
            return exact(now)
        start = close_ts - w
        ticks = [(t.ts, t.price) for t in self.feed.buffer.since(start - PRINT_MAX_AGE_S)]
        return second_average(ticks, start, math.floor(now) + 1.0, min_share=0.0)

    async def on_market_event(self, kind: str, m: Market):
        now = time.time()
        self._note_strike(m)
        if kind == "quote":
            if now - self._last_quote_store >= 5:
                self._last_quote_store = now
                self.store.add_quote(now, m)
            if now - self._last_window_upsert > 30:
                self._last_window_upsert = now
                self.store.upsert_window(m, self.pred.p_final if self.pred else None)
            s = self.tracker.series
            if s and (self.fees.fee_type != s.fee_type or self.fees.multiplier != s.fee_multiplier):
                self.fees = FeeSchedule.from_series(s.fee_type, s.fee_multiplier)
        elif kind == "window_open":
            self.scale.observe_target(m.strike)
            self._apply_scale()
            self.store.upsert_window(m)
            self.log_event("window", f"new window: target {fmt_price(m.strike, self.scale.decimals)}, closes {clock.et_label(m.close_time)}")
        elif kind == "window_close":
            close_ts = m.close_time.timestamp() if m.close_time else now
            if m.open_time is not None and self._measure_window(m.ticker, m.open_time.timestamp(), close_ts, self.feed.active_name):
                self._refit_vol()
            price = self._feed_avg(close_ts)  # the feed's own closing-minute average, compared with the settlement
            self._close_capture[m.ticker] = {"feed_price": price, "p_market": m.yes_mid, "close_ts": close_ts,
                                             "p_model": self.pred.p_final if self.pred else None, "strike": m.strike,
                                             "source": self.feed.active_name,
                                             "settle_price": None, "err_lag0": None, "err_lag60": None}
            self.store.upsert_window(m, self.pred.p_final if self.pred else None)
            self.store.mark_status(m.ticker, "closed")
        elif kind == "settled":
            self.handle_settlement(m)
        elif kind == "settlement_timeout":
            self.store.mark_status(m.ticker, "unsettled")

    # ------------------------------------------------------------------ settlement + learning
    def _resolve_closed_windows(self, now: float):
        """The next window's target IS the previous window's settlement value: measure the feed's error exactly, for
        its closing-minute average (what the model uses) and for its single price at the close (the check that the
        averaging matters: the average should be the closer of the two)."""
        self._note_strike(self.tracker.current)
        lag = self.cfg.trading.settle_lag_s
        later = 5.0
        for ticker, cap in list(self._close_capture.items()):
            close_ts = cap.get("close_ts", 0)
            if now - close_ts > 90 * 60:
                del self._close_capture[ticker]
                continue
            if cap.get("settle_price") is not None:
                continue
            settle = self._window_strikes.get(int(round(close_ts)))
            if settle is None or settle <= 0:
                continue
            latest = self.feed.latest()
            wait = max(lag, later)
            if latest is None or (latest.ts < close_ts + wait and now < close_ts + wait + 30):
                continue
            source = cap.get("source") or self.feed.active_name
            p0 = self._feed_avg(close_ts + lag, source=source)
            p_last = self._feed_price_near(close_ts + lag, source=source)
            err0 = None if p0 is None else round(p0 - settle, 10)
            err_lag = None if p_last is None else round(p_last - settle, 10)  # the last price alone, for comparison
            chosen = err0
            cap.update({"settle_price": settle, "err_lag0": err0, "err_lag60": err_lag})
            self.store.set_settle_price(ticker, settle, p0, err0, err_lag, chosen, source)
            if chosen is not None:
                self._update_basis_error(source)

    def handle_settlement(self, m: Market):
        now = time.time()
        label = 1 if m.result == "yes" else 0
        cap = self._close_capture.get(m.ticker, {})
        feed_px = cap.get("feed_price")
        settle = m.settle_value if m.settle_value is not None else cap.get("settle_price")
        feed_error = cap.get("err_lag0")
        if feed_error is None and settle is not None and feed_px is not None:
            feed_error = round(feed_px - settle, 10)
        self.store.label_snapshots(m.ticker, label)
        self.store.settle_window(m.ticker, m.result or "", feed_px, feed_error, now, settle)
        X, y, n_fit = self.store.training_rows_recent(50)
        if len(X) >= 20 and len(set(y.tolist())) == 2:
            self.model.cal.fit_pooled(X, y, n_fit)
            self.store.set_state("calibrator", self.model.cal.to_json())
        call = self.store.call_for(m.ticker)
        outcome = "UP" if label else "DOWN"
        if call and call.get("result") is None:
            self.calls.settle(m.ticker, bool(label))
            won = call["side"] == outcome
            rec = self.store.call_record()
            text = f"call {call['side']} {'WON ✓' if won else 'lost ✗'} · record {rec['wins']}-{rec['losses']}"
            self.log_event("call", f"{m.ticker} settled {outcome}: {text}")
        else:
            self.log_event("settled", f"{m.ticker} settled {outcome}")

    def _load_basis(self, source: str):
        if source == self._basis_source:
            return
        self._basis_source = source
        if self.store.get_state(f"basis_n:{source}") is None:
            if self.store.feed_errors(source, 1):
                self._update_basis_error(source)
            else:
                self.store.set_state(f"basis_signed:{source}", "0")
        self.model.basis_error = _float(self.store.get_state(f"basis_error:{source}"), 0.0)
        self.model.basis_signed = _float(self.store.get_state(f"basis_signed:{source}"), 0.0)

    def _update_basis_error(self, source: str):
        """The feed's typical error at settlement (75th percentile of |error|) widens the odds a little; a consistent
        signed bias (>= basis_min_windows windows, >= basis_min_t standard errors) shifts the feed."""
        t = self.cfg.trading
        signed = self.store.feed_errors(source, 200)
        errs = sorted(abs(e) for e in signed)
        basis = 0.0 if not errs else errs[-1] if len(errs) < 4 else errs[int(0.75 * (len(errs) - 1))]
        recent = list(reversed(signed[:12]))
        bias = 0.0
        if recent:
            bias = recent[0]
            for e in recent[1:]:
                bias = 0.7 * bias + 0.3 * e
            bias *= len(recent) / (len(recent) + 1.0)
        n = len(recent)
        t_stat = 0.0
        if n >= 2:
            sd = statistics.stdev(recent)
            mean = statistics.fmean(recent)
            t_stat = abs(mean) / (sd / math.sqrt(n)) if sd > 0 else (float("inf") if abs(mean) > 0 else 0.0)
        consistent = n >= t.basis_min_windows and t_stat >= t.basis_min_t and abs(bias) > self.scale.match_tol
        applied = round(bias, 6) if consistent else 0.0
        self.store.set_state(f"basis_error:{source}", str(basis))
        self.store.set_state(f"basis_signed:{source}", str(applied))
        self.store.set_state(f"basis_raw:{source}", str(round(bias, 6)))
        self.store.set_state(f"basis_n:{source}", str(n))
        self.store.set_state(f"basis_consistent:{source}", "1" if consistent else "0")
        if source == self._basis_source:
            self.model.basis_error = basis
            self.model.basis_signed = applied

    # ------------------------------------------------------------------ learned volatility
    def _study_len(self) -> float:
        return self.cfg.trading.study_s

    def _window_events(self, open_ts: float, close_ts: float) -> list[tuple[float, str]]:
        return volcal.events_between(open_ts, close_ts + self.cfg.trading.settle_lag_s, self._extra, self.spec.weekly)

    def _refit_vol(self):
        rows = [volcal.VolRow(r["open_time"], r["sigma_forecast"], r["sigma_realized"], r["event"] or "")
                for r in self.store.vol_rows() if r.get("open_time")]
        self.vol_profile.fit(rows)

    def _measure_window(self, ticker: str, open_ts: float, close_ts: float, source: str) -> bool:
        start = open_ts + self._study_len()
        ticks = [(tk.ts, tk.price) for tk in self.feed.buffer.since(start) if tk.ts <= close_ts]
        if len(ticks) < 20:
            ticks = self.store.ticks_between(start, close_ts, source)
        sigma, n = volcal.realized_sigma(ticks, start, close_ts)
        if sigma is None:
            return False
        self.store.set_window_realized(ticker, sigma, n)
        return True

    def _vol_context(self, m: Market, now: float, elapsed) -> list[tuple[float, str]]:
        open_ts = m.open_time.timestamp() if m.open_time else now
        close_ts = m.close_time.timestamp() if m.close_time else now
        events = self._window_events(open_ts, close_ts)
        if self.cfg.trading.vol_profile:
            mult, note, n = self.vol_profile.mult(open_ts, events)
        else:
            mult, note, n = 1.0, "learned correction off", 0
        self.model.vol_mult, self.model.vol_note = mult, note
        study = self._study_len()
        self._vol_info = {"mult": round(mult, 3), "note": note, "n": n,
                          "events": [{"name": name, "at": clock.et_label(dt.datetime.fromtimestamp(ts, clock.UTC))} for ts, name in events],
                          "studying": elapsed is not None and elapsed < study}
        return events

    def _record_forecast(self, m: Market, pred: Prediction, now: float, elapsed, tau, events):
        study = self._study_len()
        if (elapsed is not None and elapsed >= study and m.ticker not in self._forecast_done and elapsed <= study + 120
                and tau and tau > 0 and pred.sigma_raw > 0 and pred.confidence not in ("warming_up", "stale")):
            self._forecast_done.add(m.ticker)
            self.store.set_window_forecast(m.ticker, pred.sigma_raw, events[0][1] if events else "")

    # ------------------------------------------------------------------ per second
    def step(self):
        now = time.time()
        m = self.tracker.current
        tick = self.feed.latest()
        health = self.feed.health()
        self._load_basis(self.feed.active_name)
        if tick and tick.ts != self._last_stored_tick_ts:
            self._last_stored_tick_ts = tick.ts
            self.store.add_tick(tick.ts, self.feed.active_name, tick.price)
        if now - self._last_prune > 600:
            self._last_prune = now
            self.store.prune()
        quotes_fresh = self.tracker.quotes_fresh(QUOTE_MAX_AGE_S, now)
        self._resolve_closed_windows(now)
        pred = None
        tau = elapsed = None
        events: list = []
        if m is not None and tick is not None:
            at = clock.now()
            tau = m.seconds_left(at)
            elapsed = m.seconds_elapsed(at)
            buf = self.feed.buffer
            events = self._vol_context(m, now, elapsed)
            close_ts = m.close_time.timestamp() if m.close_time else now + (tau or 0.0)
            pred = self.model.predict(tick.price, m.strike, (tau or 0.0) + self.cfg.trading.settle_lag_s,
                                      m.yes_mid if quotes_fresh else None, health.age_s,
                                      buf.price_at(now - 60), buf.price_at(now - 180), buf.price_at(now - 300),
                                      avg_sofar=self._avg_sofar(close_ts + self.cfg.trading.settle_lag_s, now))
            quotes = Quotes.from_market(m) if quotes_fresh else Quotes()
            backup = on_backup(self.feed)
            blocked = (bool(events) and self.cfg.trading.event_entries == "block") or backup
            made = self.calls.step(now, m.ticker, None if blocked and self.calls.call is None else pred,
                                   tau, elapsed, self._study_len(), quotes)
            if blocked and self.calls.call is None:
                self.calls.watch = {"status": self._blocked_text(events, backup)}
                self.calls._update_meter(now, pred, tau)  # still show what the model sees
            if made:
                self._announce(made)
            self._record_forecast(m, pred, now, elapsed, tau, events)
            if tau is not None and tau > 0 and now - self._last_snapshot >= 15:
                self._last_snapshot = now
                self.store.add_snapshot(now, m.ticker, tau, tick.price, m.strike, pred)
        else:
            self.calls.step(now, m.ticker if m else None, None, None, None, self._study_len())
        self.pred = pred
        self.state = self._build_state(now, m, tick, health, pred, tau, quotes_fresh)

    @staticmethod
    def _blocked_text(events: list, backup: bool) -> str:
        if backup:
            return "no new call while the exact index is down (reading the exchanges' backup price)"
        name = events[0][1]
        if name == "Kalshi maintenance":
            return "no call this window (Kalshi's weekly maintenance, Thursday 3:00-5:00 AM ET)"
        return f"no call this window ({name} at {clock.et_label(dt.datetime.fromtimestamp(events[0][0], clock.UTC))})"

    def _announce(self, made: dict):
        when = f"{int(made['seconds_left'] // 60)}:{int(made['seconds_left'] % 60):02d} left"
        text = f"{self.spec.name}: settles {made['side']} · model {round(made['p_side'] * 100)}% ({made['kind']}, {when})"
        self.log_event("call", text)
        if not self.is_muted(self.spec.key):
            self.notifier.notify(f"{self.spec.name.upper()} settles {made['side']}", text, "Glass")

    # ------------------------------------------------------------------ state for the board
    def _build_state(self, now, m, tick, health, pred, tau, quotes_fresh) -> dict:
        status = "live" if (m and tick) else ("no_market" if tick else ("no_series" if self._series_missing() else "no_feed"))
        return {
            "key": self.spec.key, "name": self.spec.name, "series": self.spec.series, "badge": self.spec.badge,
            "color": self.spec.color, "kind": self.spec.kind, "status": status,
            "scale": self.scale.as_dict(),
            "market": m.summary() if m else None,
            "quotes_fresh": quotes_fresh,
            "seconds_left": None if tau is None else round(tau, 1),
            "countdown": clock.fmt_countdown(tau),
            "tick": {"ts": tick.ts, "price": tick.price} if tick else None,
            "feed": {"age_s": health.age_s, "mode": health.mode, "last_error": health.last_error},
            "prediction": pred.as_dict() if pred else None,
            "vol": self._vol_info,
            "call": self.calls.state(),
            "muted": self.is_muted(self.spec.key),
            "tracker_error": self.tracker.last_error,
        }

    def _series_missing(self) -> bool:
        err = self.tracker.last_error or ""
        return "404" in err

    def detail(self, minutes: float = 20) -> dict:
        """Everything the detail view shows for this market: chart, call history, stats, log."""
        now = time.time()
        m = self.tracker.current
        since = now - minutes * 60
        if m and m.open_time:
            since = min(since, m.open_time.timestamp() - 300)
        ticks = [(t.ts, t.price) for t in self.feed.buffer.since(since)] or self.store.ticks_since(since, self.feed.active_name)
        return {"key": self.spec.key,
                "chart": {"ticks": ticks, "strike": m.strike if m else None,
                          "open_time": m.open_time.timestamp() if (m and m.open_time) else None,
                          "close_time": m.close_time.timestamp() if (m and m.close_time) else None},
                "calls": self.store.call_record(60)["recent"],
                "stats": self.stats(), "events": list(self.events)[:30]}

    def stats(self) -> dict:
        ts, cached = self._stats_cache
        if time.time() - ts > 20:
            data = self.store.stats(self.scale.match_tol)
            src = self._basis_source or ""
            data["settle_lag_s"] = self.cfg.trading.settle_lag_s
            data["basis"] = {"source": src, "applied": self.model.basis_signed, "error": self.model.basis_error,
                             "n": int(_float(self.store.get_state(f"basis_n:{src}"), 0.0))}
            data["vol_profile"] = self.vol_profile.summary()
            data["scale"] = self.scale.as_dict()
            self._stats_cache = (time.time(), data)
            return data
        return cached


def _float(value, default: float) -> float:
    try:
        return float(value) if value not in (None, "") else default
    except (TypeError, ValueError):
        return default
