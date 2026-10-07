"""The 1 Hz brain: joins the Kalshi tracker, the price feed, the model and the decision engine;
records everything; learns at settlement; produces the state the dashboard shows."""
from __future__ import annotations

import asyncio
import logging
import time
from collections import deque

from . import clock
from .config import Config
from .decision import DecisionEngine, Position, Quotes, Signal, cents, pct
from .fees import FeeSchedule, fee_per_contract
from .feeds.base import PriceFeed, Tick
from .kalshi import Market, MarketTracker
from .model import Calibrator, Model, Prediction
from .notify import Notifier
from .store import Store

log = logging.getLogger(__name__)


class Engine:
    def __init__(self, cfg: Config, store: Store, kalshi_client, feed: PriceFeed, notifier: Notifier | None = None):
        self.cfg = cfg
        self.store = store
        self.feed = feed
        self.notifier = notifier or Notifier(cfg.notify.desktop, cfg.notify.sound)
        t = cfg.trading
        self.model = Model(calibrator=Calibrator.from_json(store.get_state("calibrator")), tie_adj=t.tie_adj,
                           min_warmup_s=t.min_warmup_minutes * 60, confident_margin=t.confident_margin,
                           stale_after_s=cfg.feed.stale_after_s)
        self.model.basis_error = _float(store.get_state("basis_error"), 0.0)
        self.model.prev_outcome = _float(store.get_state("prev_outcome"), 0.0)
        self.fees = FeeSchedule()
        self.decider = DecisionEngine(t, self.fees)
        self.tracker = MarketTracker(kalshi_client, cfg.kalshi.series_ticker, cfg.kalshi.poll_interval_s,
                                     on_event=self.on_market_event)
        self.position: Position | None = _position_from_row(store.get_open_position())
        self.events: deque[dict] = deque(maxlen=60)
        self.state: dict = {"status": "starting"}
        self.pred: Prediction | None = None
        self.signal: Signal | None = None
        self.started_ts = time.time()
        self._last_signal_key: str | None = None
        self._last_signal_log = 0.0
        self._last_snapshot = 0.0
        self._last_window_upsert = 0.0
        self._last_tick_store = 0.0
        self._paper_done: set[str] = set()
        self._close_capture: dict[str, dict] = {}
        self._stats_cache: tuple[float, dict] = (0.0, {})
        self.feed.subscribe(self.on_tick)
        self.feed.subscribe_warmup(self.on_warmup)

    # ------------------------------------------------------------------ feed callbacks
    def on_warmup(self, closes):
        self.model.vol.seed_from_closes(closes)
        self.log_event("feed", f"warm-up: {len(closes)} candles from {self.feed.name}")

    def on_tick(self, tick: Tick):
        self.model.vol.update(tick.ts, tick.price)

    # ------------------------------------------------------------------ market callbacks
    async def on_market_event(self, kind: str, m: Market):
        now = time.time()
        if kind == "quote":
            self.store.add_quote(now, m)
            if now - self._last_window_upsert > 30:
                self._last_window_upsert = now
                self.store.upsert_window(m, self.pred.p_final if self.pred else None)
            if self.tracker.series and (self.fees.fee_type != self.tracker.series.fee_type
                                        or self.fees.multiplier != self.tracker.series.fee_multiplier):
                self.fees = FeeSchedule.from_series(self.tracker.series.fee_type, self.tracker.series.fee_multiplier)
                self.decider.fees = self.fees
        elif kind == "window_open":
            self.store.upsert_window(m)
            self._last_signal_key = None
            self.log_event("window", f"new window {m.ticker}: target {('$%.2f' % m.strike) if m.strike else '?'}"
                                     f" (from {m.strike_source}), closes {clock.et_label(m.close_time)}")
        elif kind == "window_close":
            close_ts = m.close_time.timestamp() if m.close_time else now
            price = self.feed.buffer.price_at(close_ts) or (self.feed.latest().price if self.feed.latest() else None)
            self._close_capture[m.ticker] = {"feed_price": price, "p_market": m.yes_mid,
                                             "p_model": self.pred.p_final if self.pred else None, "strike": m.strike}
            self.store.upsert_window(m, self.pred.p_final if self.pred else None)
            self.log_event("window", f"window {m.ticker} closed; feed price at close "
                                     f"{('$%.2f' % price) if price else '?'} vs target {('$%.2f' % m.strike) if m.strike else '?'}")
        elif kind == "settled":
            self.handle_settlement(m)
        elif kind == "settlement_timeout":
            self.log_event("window", f"no settlement result for {m.ticker} after 20 min")

    # ------------------------------------------------------------------ settlement + learning
    def handle_settlement(self, m: Market):
        now = time.time()
        label = 1 if m.result == "yes" else 0
        cap = self._close_capture.pop(m.ticker, {})
        strike = m.strike if m.strike is not None else cap.get("strike")
        feed_px = cap.get("feed_price")
        feed_error = None
        if feed_px is not None and strike is not None:
            feed_said_up = feed_px >= strike - self.cfg.trading.tie_adj
            feed_error = 0.0 if feed_said_up == bool(label) else abs(feed_px - strike)
        self.store.label_snapshots(m.ticker, label)
        self.store.settle_window(m.ticker, m.result or "", feed_px, feed_error, now)
        # learn
        X, y, _ = self.store.training_rows(m.ticker)
        if len(X) >= 3:
            self.model.cal.fit_window(X, y)
            self.store.set_state("calibrator", self.model.cal.to_json())
        self.model.prev_outcome = 1.0 if label else -1.0
        self.store.set_state("prev_outcome", str(self.model.prev_outcome))
        self._update_basis_error()
        # paper trade
        paper = self.store.open_paper_trade_for(m.ticker)
        if paper:
            value = 1.0 if (paper["side"] == "UP") == bool(label) else 0.0
            self.store.close_paper_trade(paper["id"], now, value, 0.0, "settle")
        # user position
        if self.position and self.position.ticker == m.ticker:
            value = 1.0 if (self.position.side == "UP") == bool(label) else 0.0
            pnl = (value - self.position.avg_price) * self.position.qty
            self.store.close_position(self.position.id, now, value, pnl)
            self.notifier.notify("Window settled", f"{m.ticker}: {'UP' if label else 'DOWN'} · your {self.position.side} "
                                                   f"{'won' if value else 'lost'} ({'+' if pnl >= 0 else ''}{pnl:.2f})")
            self.position = None
        outcome = "UP" if label else "DOWN"
        p_model = cap.get("p_model")
        msg = f"{m.ticker} settled {outcome}"
        if p_model is not None:
            msg += f" · model had Up at {pct(p_model)} near the close"
        if feed_error:
            msg += f" · feed disagreed with settlement by ≥ ${feed_error:.2f}"
        self.log_event("settled", msg)

    def _update_basis_error(self):
        rows = self.store.recent_windows(200)
        errs = sorted(abs(r["feed_error"]) for r in rows if r.get("feed_error"))
        if not errs:
            basis = 0.0
        elif len(errs) < 4:
            basis = errs[-1]
        else:
            basis = errs[int(0.75 * (len(errs) - 1))]
        self.model.basis_error = basis
        self.store.set_state("basis_error", str(basis))

    # ------------------------------------------------------------------ user positions
    def open_position(self, side: str, qty: int, price: float, note: str = "") -> Position:
        m = self.tracker.current
        ticker = m.ticker if m else "unknown"
        if self.position:
            raise ValueError("a position is already open; close it first")
        side = side.upper()
        if side not in ("UP", "DOWN") or qty <= 0 or not (0 < price < 1):
            raise ValueError("side must be UP/DOWN, qty > 0, price between 0 and 1")
        now = time.time()
        pid = self.store.open_position(ticker, side, qty, price, now, note)
        self.position = Position(ticker, side, qty, price, now, pid)
        self.log_event("you", f"you bought {qty} {side} @ {cents(price)} on {ticker}")
        return self.position

    def close_position(self, price: float) -> dict:
        if not self.position:
            raise ValueError("no open position")
        pos = self.position
        pnl = (price - pos.avg_price) * pos.qty - fee_per_contract(price, pos.qty, self.fees) * pos.qty
        self.store.close_position(pos.id, time.time(), price, pnl)
        self.log_event("you", f"you sold {pos.qty} {pos.side} @ {cents(price)} → {'+' if pnl >= 0 else ''}{pnl:.2f} after fee")
        self.position = None
        return {"pnl": round(pnl, 2)}

    def cancel_position(self):
        if self.position:
            self.store.close_position(self.position.id, time.time(), self.position.avg_price, 0.0)
            self.log_event("you", "position entry removed")
            self.position = None

    # ------------------------------------------------------------------ main loop
    async def run(self, stop: asyncio.Event | None = None):
        stop = stop or asyncio.Event()
        await self.feed.start()
        tracker_task = asyncio.create_task(self.tracker.run(stop), name="tracker")
        try:
            while not stop.is_set():
                try:
                    self.step()
                except Exception:
                    log.exception("engine step failed")
                try:
                    await asyncio.wait_for(stop.wait(), timeout=1.0)
                except asyncio.TimeoutError:
                    pass
        finally:
            tracker_task.cancel()
            await self.feed.stop()

    def step(self):
        now = time.time()
        m = self.tracker.current
        tick = self.feed.latest()
        health = self.feed.health()
        if tick and now - self._last_tick_store >= 1.0:
            self._last_tick_store = now
            self.store.add_tick(tick.ts, self.feed.name, tick.price)
        pred = sig = None
        tau = elapsed = None
        if m is not None and tick is not None:
            at = clock.now()
            tau = m.seconds_left(at)
            elapsed = m.seconds_elapsed(at)
            buf = self.feed.buffer
            pred = self.model.predict(tick.price, m.strike, tau if tau is not None else 0.0, m.yes_mid, health.age_s,
                                      buf.price_at(now - 60), buf.price_at(now - 180), buf.price_at(now - 300))
            pos = self.position if (self.position and self.position.ticker == m.ticker) else None
            sig = self.decider.decide(pred, Quotes.from_market(m), tick.price, m.strike, tau, elapsed, pos)
            if tau is not None and tau > 0 and now - self._last_snapshot >= 15:
                self._last_snapshot = now
                self.store.add_snapshot(now, m.ticker, tau, tick.price, m.strike, pred)
            if sig.key != self._last_signal_key or now - self._last_signal_log >= 60:
                self.store.add_signal(now, m.ticker, sig, pred, tau)
                self._last_signal_log = now
                if sig.key != self._last_signal_key and sig.action in ("BUY", "SELL"):
                    self.notifier.notify(f"WTI 15m: {sig.action} {sig.side or ''}".strip(), sig.headline,
                                         "Glass" if sig.action == "BUY" else "Submarine")
                    self.log_event("signal", sig.headline)
                self._last_signal_key = sig.key
            self._paper_step(m, pred, sig, tick.price, tau, elapsed, now)
        self.pred = pred
        self.signal = sig
        self.state = self._build_state(now, m, tick, health, pred, sig, tau, elapsed)

    def _paper_step(self, m: Market, pred: Prediction, sig: Signal, price: float, tau, elapsed, now: float):
        paper = self.store.open_paper_trade_for(m.ticker)
        if paper is None:
            if sig.action == "BUY" and m.ticker not in self._paper_done and sig.price:
                fee = fee_per_contract(sig.price, sig.size, self.fees) * sig.size
                self.store.open_paper_trade(m.ticker, sig.side, now, sig.price, sig.size, fee, sig.confidence, sig.p_side or 0.5)
                self._paper_done.add(m.ticker)
                self.log_event("paper", f"paper BUY {sig.size} {sig.side} @ {cents(sig.price)}")
            return
        if tau is not None and tau <= 0:
            return  # settlement will close it
        pos = Position(m.ticker, paper["side"], paper["size"], paper["entry_price"], paper["entry_ts"], paper["id"])
        psig = self.decider.decide(pred, Quotes.from_market(m), price, m.strike, tau, elapsed, pos)
        if psig.action == "SELL" and psig.price:
            fee = fee_per_contract(psig.price, pos.qty, self.fees) * pos.qty
            self.store.close_paper_trade(paper["id"], now, psig.price, fee, psig.reasons[0] if psig.reasons else "sell")
            self.log_event("paper", f"paper SELL {pos.qty} {pos.side} @ {cents(psig.price)} ({psig.reasons[0] if psig.reasons else ''})")

    # ------------------------------------------------------------------ state for the UI
    def log_event(self, kind: str, text: str):
        self.events.appendleft({"ts": time.time(), "kind": kind, "text": text})
        log.info("[%s] %s", kind, text)

    def stats(self) -> dict:
        ts, cached = self._stats_cache
        if time.time() - ts > 20:
            data = self.store.stats()
            data["calibrator"] = {"n_windows": self.model.cal.n_windows, "n_samples": self.model.cal.n_samples,
                                  "shrink": round(self.model.cal.shrink, 3), "weights": self.model.cal.weights()}
            data["basis_error"] = self.model.basis_error
            self._stats_cache = (time.time(), data)
            return data
        return cached

    def _build_state(self, now, m, tick, health, pred, sig, tau, elapsed) -> dict:
        paper = self.store.open_paper_trade_for(m.ticker) if m else None
        return {
            "now": now,
            "status": "live" if (m and tick) else ("no_market" if tick else "no_feed"),
            "uptime_s": round(now - self.started_ts),
            "series": self.tracker.series.summary() if self.tracker.series else None,
            "fees": {"fee_type": self.fees.fee_type, "multiplier": self.fees.multiplier, "is_estimate": self.fees.is_estimate},
            "market": m.summary() if m else None,
            "seconds_left": None if tau is None else round(tau, 1),
            "seconds_elapsed": None if elapsed is None else round(elapsed, 1),
            "countdown": clock.fmt_countdown(tau),
            "tick": {"ts": tick.ts, "price": tick.price} if tick else None,
            "feed": health.as_dict(),
            "prediction": pred.as_dict() if pred else None,
            "signal": sig.as_dict() if sig else None,
            "position": self.position.as_dict() if self.position else None,
            "paper": paper,
            "tracker": {"last_error": self.tracker.last_error, "polls": self.tracker.poll_count,
                        "pending_settlements": list(self.tracker.pending.keys())},
            "events": list(self.events)[:25],
            "config": {"bankroll": self.cfg.trading.bankroll, "edge_min": self.cfg.trading.edge_min,
                       "min_warmup_minutes": self.cfg.trading.min_warmup_minutes, "feed": self.cfg.feed.source},
        }

    def chart(self, minutes: float = 20) -> dict:
        now = time.time()
        m = self.tracker.current
        since = now - minutes * 60
        if m and m.open_time:
            since = min(since, m.open_time.timestamp() - 300)
        ticks = [(t.ts, t.price) for t in self.feed.buffer.since(since)]
        if not ticks:
            ticks = self.store.ticks_since(since, self.feed.name)
        out = {"ticks": ticks, "strike": m.strike if m else None,
               "open_time": m.open_time.timestamp() if (m and m.open_time) else None,
               "close_time": m.close_time.timestamp() if (m and m.close_time) else None}
        if self.pred and m and m.close_time:
            out["cone"] = {"sigma": self.pred.sigma, "price": ticks[-1][1] if ticks else None}
        return out


def _float(value, default: float) -> float:
    try:
        return float(value) if value not in (None, "") else default
    except (TypeError, ValueError):
        return default


def _position_from_row(row: dict | None) -> Position | None:
    if not row:
        return None
    return Position(row["ticker"], row["side"], int(row["qty"]), float(row["avg_price"]), float(row["opened_ts"]), row["id"])
