"""The 1 Hz brain: joins the Kalshi tracker, the price feed, the model and the decision engine;
records everything; learns at settlement; produces the state the dashboard shows."""
from __future__ import annotations

import asyncio
import datetime as dt
import logging
import math
import statistics
import time
from collections import deque

from . import clock, volcal
from .config import Config
from .decision import DecisionEngine, Position, Quotes, Signal, cash_out, cents, pct, pct5, qty_text
from .fees import FeeSchedule, fee_per_contract, taker_fee
from .feeds.base import PriceFeed, Tick
from .kalshi import Market, MarketTracker
from .model import Calibrator, Model, Prediction
from .notify import Notifier
from .calls import CallTracker
from .plans import PlanEvent, PlanTracker
from .store import Store

log = logging.getLogger(__name__)

QUOTE_MAX_AGE_S = 10.0


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
        self.model.prev_outcome = _float(store.get_state("prev_outcome"), 0.0)
        self._basis_source: str | None = None  # feed whose measured basis is loaded into the model
        self._last_prune = time.time()
        self.fees = FeeSchedule()
        self.decider = DecisionEngine(t, self.fees)
        self.tracker = MarketTracker(kalshi_client, cfg.kalshi.series_ticker, cfg.kalshi.poll_interval_s,
                                     on_event=self.on_market_event)
        self.position: Position | None = _position_from_row(store.get_open_position())
        self.plans = PlanTracker(t, self.fees, recent_amounts_fn=lambda: store.recent_amounts(10), decider=self.decider)
        self._paper_pos: Position | None = None  # the paper trade's Position, kept across seconds so its latches work
        self._wait_sig: Signal | None = None  # WAIT text is held for a few seconds so the card reads calmly
        self._wait_since = 0.0
        for row in store.open_plans():  # plans from a previous run cannot be judged any more
            store.end_plan(row["id"], "expired", "app restarted", time.time(), None, None, None)
        self.events: deque[dict] = deque(maxlen=60)
        self.state: dict = {"status": "starting"}
        self.pred: Prediction | None = None
        self.signal: Signal | None = None
        self.started_ts = time.time()
        self._last_signal_key: str | None = None
        self._pending_key: str | None = None  # a new BUY/SELL must hold for 2 consecutive seconds before it notifies
        self._pending_count = 0
        self._last_notified: dict[str, float] = {}  # action -> epoch of last notification (30 s cooldown)
        self._last_snapshot = 0.0
        self._last_window_upsert = 0.0
        self._last_stored_tick_ts: float | None = None
        self._paper_done: set[str] = set()
        self._close_capture: dict[str, dict] = {}
        self._window_strikes: dict[int, float] = {}  # open_time (epoch s, rounded) -> target; targets double as prior settles
        self._stats_cache: tuple[float, dict] = (0.0, {})
        self.feed.subscribe(self.on_tick)
        self.feed.subscribe_warmup(self.on_warmup)
        for note in cfg.notes:
            self.log_event("config", note)
        self._migrate_settle_timing()
        self._load_basis(self.feed.active_name)
        self._recover()
        # v10: learned volatility by ET hour / report, from every window's forecast vs what really happened
        self.vol_profile = volcal.VolProfile(t.vol_profile_k, t.event_prior)
        self._extra_events = volcal.parse_extra_events(t.extra_events)
        self._forecast_done: set[str] = set()
        self._study: dict = {}  # this window's study readings: odds range, moves so far
        self._vol_info: dict = {}
        self._backfill_vol(time.time())
        self._refit_vol()
        # v10: the settlement call (one locked Up/Down prediction per window) and its record
        self.calls = CallTracker(t, store)
        for row in store.unsettled_calls():  # windows that settled while the app was off
            self.calls.settle(row["ticker"], row["result"] == "yes")

    # ------------------------------------------------------------------ per-feed basis
    def _migrate_settle_timing(self):
        """The feed's error vs settlement is measured at the close and one candle later; the one that counts is the
        moment the app settles on (settle_lag_s). When that changes (v12: a minute later -> at the close), every stored
        error and each feed's basis is re-derived, so the old moment's noise (several cents of price movement in
        the minute after the close) stops widening the odds and shifting the feed."""
        at_close = self.cfg.trading.settle_lag_s <= 0
        want = "close" if at_close else "candle"
        if self.store.get_state("settle_timing") == want:
            return
        changed = self.store.rechoose_feed_errors(at_close)
        for source in self.store.feed_sources():
            self.store.set_state(f"basis_n:{source}", None)  # _load_basis re-derives it (and its gate) from the errors
        if self.store.settled_window_count() > 0:
            # the learner's main input (the base odds) was recorded with the other timing: start it over from the
            # base model and let it learn again from windows recorded from now on
            self.model.cal = Calibrator()
            self.store.set_state("calibrator", self.model.cal.to_json())
            self.store.set_state("learner_since", str(time.time()))
            self.log_event("model", "learner restarted: its past inputs were recorded with the old settlement timing")
        self.store.set_state("settle_timing", want)
        if changed:
            self.log_event("feed", f"settlement timing: {'at the close' if at_close else 'one candle after the close'}; "
                                   f"{changed} stored feed errors re-measured at that moment")

    def _load_basis(self, source: str):
        """The feed's measured bias vs settlement is per source: Hyperliquid's 6¢ must never be applied to Kalshi's."""
        if source == self._basis_source:
            return
        self._basis_source = source
        if self.store.get_state(f"basis_n:{source}") is None:
            # a database from before the consistency gate: re-derive the shift from the stored errors (and gate it)
            if self.store.feed_errors(source, 1):
                self._update_basis_error(source)
            else:
                self.store.set_state(f"basis_signed:{source}", "0")
        self.model.basis_error = _float(self.store.get_state(f"basis_error:{source}"), 0.0)
        self.model.basis_signed = _float(self.store.get_state(f"basis_signed:{source}"), 0.0)

    # ------------------------------------------------------------------ restart recovery
    def _recover(self):
        """After a restart: remember recent targets, re-queue closed-but-unsettled windows for settlement polling,
        rebuild their close captures from the database, and settle a restored position whose window already resolved."""
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
                feed_px = self.store.last_tick_before(close_ts, source)
            self._close_capture[m.ticker] = {
                "feed_price": feed_px, "p_market": row.get("p_market_at_close"), "close_ts": close_ts,
                "p_model": row.get("p_model_at_close"), "strike": row.get("strike"), "source": source,
                "settle_price": row.get("settle_price"), "err_lag0": row.get("feed_error_lag0"),
                "err_lag60": row.get("feed_error_lag60")}
            self.log_event("recover", f"re-queued {m.ticker} for settlement")
        if self.position:
            w = self.store.window(self.position.ticker)
            if w and w.get("result") in ("yes", "no"):
                self._settle_position(1 if w["result"] == "yes" else 0, now, notify=False)

    # ------------------------------------------------------------------ feed callbacks
    def on_warmup(self, closes):
        self.model.vol.seed_from_closes(closes)
        self.log_event("feed", f"warm-up: {len(closes)} candles from {self.feed.name}")

    def on_tick(self, tick: Tick):
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

    async def on_market_event(self, kind: str, m: Market):
        now = time.time()
        self._note_strike(m)
        if kind == "quote":
            self.store.add_quote(now, m)
            if now - self._last_window_upsert > 30:
                self._last_window_upsert = now
                self.store.upsert_window(m, self.pred.p_final if self.pred else None)
            if self.tracker.series and (self.fees.fee_type != self.tracker.series.fee_type
                                        or self.fees.multiplier != self.tracker.series.fee_multiplier):
                self.fees = FeeSchedule.from_series(self.tracker.series.fee_type, self.tracker.series.fee_multiplier)
                self.decider.fees = self.fees
                self.plans.fees = self.fees
        elif kind == "window_open":
            self.store.upsert_window(m)
            self._last_signal_key = None
            self.log_event("window", f"new window {m.ticker}: target {('$%.2f' % m.strike) if m.strike else '?'}"
                                     f" (from {m.strike_source}), closes {clock.et_label(m.close_time)}")
        elif kind == "window_close":
            close_ts = m.close_time.timestamp() if m.close_time else now
            if m.open_time is not None and self._measure_window(m.ticker, m.open_time.timestamp(), close_ts, self.feed.active_name):
                self._refit_vol()
            price = self._feed_price_near(close_ts)
            self._close_capture[m.ticker] = {"feed_price": price, "p_market": m.yes_mid, "close_ts": close_ts,
                                             "p_model": self.pred.p_final if self.pred else None, "strike": m.strike,
                                             "source": self.feed.active_name,
                                             "settle_price": None, "err_lag0": None, "err_lag60": None}
            self.store.upsert_window(m, self.pred.p_final if self.pred else None)
            self.store.mark_status(m.ticker, "closed")
            self.log_event("window", f"window {m.ticker} closed; feed price at close "
                                     f"{('$%.2f' % price) if price else '?'} vs target {('$%.2f' % m.strike) if m.strike else '?'}")
        elif kind == "settled":
            self.handle_settlement(m)
        elif kind == "settlement_timeout":
            self.store.mark_status(m.ticker, "unsettled")
            self.log_event("window", f"no settlement result for {m.ticker} after 90 min")

    # ------------------------------------------------------------------ settlement + learning
    def _resolve_closed_windows(self, now: float):
        """The next window's target IS the previous window's settlement price (same 1-minute Pyth candle).
        Use it to measure the feed's error exactly, at the close and one candle later. Order-independent:
        works whether Kalshi's result, the next target, or the feed data arrives first."""
        self._note_strike(self.tracker.current)
        lag = self.cfg.trading.settle_lag_s
        later = 60.0  # the price is checked at the close AND one candle later, whichever the app settles on
        for ticker, cap in list(self._close_capture.items()):
            close_ts = cap.get("close_ts", 0)
            if now - close_ts > 90 * 60:
                del self._close_capture[ticker]  # nothing more will arrive for this window
                continue
            if cap.get("settle_price") is not None:
                continue
            settle = self._window_strikes.get(int(round(close_ts)))
            if settle is None or settle <= 0:
                continue  # next window's target not published yet
            latest = self.feed.latest()
            wait = max(lag, later)
            if latest is None or (latest.ts < close_ts + wait and now < close_ts + wait + 30):
                continue  # wait until the feed covers both moments (or give up after 30 s grace)
            source = cap.get("source") or self.feed.active_name
            p0 = self._feed_price_near(close_ts, source=source)
            p_lag = self._feed_price_near(close_ts + later - 1, source=source)
            err0 = None if p0 is None else round(p0 - settle, 4)
            err_lag = None if p_lag is None else round(p_lag - settle, 4)
            chosen = err_lag if lag > 0 else err0
            cap.update({"settle_price": settle, "err_lag0": err0, "err_lag60": err_lag})
            self.store.set_settle_price(ticker, settle, p0, err0, err_lag, chosen, source)
            msg = f"{ticker} settlement price ${settle:.2f} (= next target)"
            if p0 is not None:
                msg += f"; feed said ${p0:.2f} at the close"
            if p_lag is not None:
                msg += f", ${p_lag:.2f} at the candle close"
            if p0 is None and p_lag is None:
                msg += "; no feed data around the close (not counted in the feed error)"
            self.log_event("settle", msg)
            if chosen is not None:
                self._update_basis_error(source)

    def handle_settlement(self, m: Market):
        now = time.time()
        label = 1 if m.result == "yes" else 0
        cap = self._close_capture.get(m.ticker, {})  # kept until resolved; _resolve_closed_windows expires it
        strike = m.strike if m.strike is not None else cap.get("strike")
        feed_px = cap.get("feed_price")
        settle = m.settle_value if m.settle_value is not None else cap.get("settle_price")
        feed_error = cap.get("err_lag60") if self.cfg.trading.settle_lag_s > 0 else cap.get("err_lag0")
        if feed_error is None and settle is not None and feed_px is not None and self.cfg.trading.settle_lag_s <= 0:
            feed_error = round(feed_px - settle, 4)
        direction_ok = None
        if feed_px is not None and strike is not None:
            direction_ok = (feed_px >= strike - self.cfg.trading.tie_adj) == bool(label)
        self.store.label_snapshots(m.ticker, label)
        self.store.settle_window(m.ticker, m.result or "", feed_px, feed_error, now, settle)
        # learn on a pooled batch of recent windows (both outcomes present) instead of one window at a time;
        # the learner's weight grows with every settled window since its features last changed meaning
        since = _float(self.store.get_state("learner_since"), 0.0)
        X, y, n_fit = self.store.training_rows_recent(50, since)
        if len(X) >= 20 and len(set(y.tolist())) == 2:
            self.model.cal.fit_pooled(X, y, n_fit)  # its weight counts the windows it learned from (at most 50)
            self.store.set_state("calibrator", self.model.cal.to_json())
        self.model.prev_outcome = 1.0 if label else -1.0
        self.store.set_state("prev_outcome", str(self.model.prev_outcome))
        # paper trade
        paper = self.store.open_paper_trade_for(m.ticker)
        if paper:
            value = 1.0 if (paper["side"] == "UP") == bool(label) else 0.0
            self.store.close_paper_trade(paper["id"], now, value, 0.0, "settle")
        # user position
        if self.position and self.position.ticker == m.ticker:
            self._settle_position(label, now)
        # the settlement call
        call = self.store.call_for(m.ticker)
        if call and call.get("result") is None:
            self.calls.settle(m.ticker, bool(label))
            won = call["side"] == ("UP" if label else "DOWN")
            rec = self.store.call_record()
            self.log_event("call", f"settlement call {call['side']} on {m.ticker}: {'WIN' if won else 'LOSS'} "
                                   f"· record {rec['wins']}-{rec['losses']}")
        outcome = "UP" if label else "DOWN"
        p_model = cap.get("p_model")
        msg = f"{m.ticker} settled {outcome}"
        if p_model is not None:
            msg += f" · model had Up at {pct(p_model)} near the close"
        if settle is not None:
            msg += f" · settled at ${settle:.2f}"
        if feed_error:
            msg += f" · feed off by ${feed_error:+.2f}"
        elif direction_ok is False:
            msg += " · feed was on the wrong side of the target at the close"
        self.log_event("settled", msg)

    def _settle_position(self, label: int, now: float, notify: bool = True):
        pos = self.position
        if pos is None:
            return
        value = 1.0 if (pos.side == "UP") == bool(label) else 0.0
        pnl = pos.qty * value - pos.cost + pos.realized
        self.store.close_position(pos.id, now, value, pnl)
        text = f"{pos.ticker}: settled {'UP' if label else 'DOWN'} · your {pos.side} {'won' if value else 'lost'} ({'+' if pnl >= 0 else ''}{pnl:.2f})"
        if notify:
            self.notifier.notify("Window settled", text)
        self.log_event("you", text)
        self.position = None

    def _update_basis_error(self, source: str):
        """From measured feed − settlement errors of ONE feed source: the typical size (75th percentile of |err|
        over the last 200 windows; max if < 4) widens the model's uncertainty, and the recent signed bias (EWMA
        over the last 12 windows, shrunk by n/(n+1)) shifts that feed's price before it is compared with the target.
        The shift is only APPLIED once it is consistent: at least basis_min_windows windows and |bias| at least
        basis_min_t standard errors from zero; a 2¢ wobble after three windows must not move every call by 2¢."""
        t = self.cfg.trading
        signed = self.store.feed_errors(source, 200)  # newest first
        errs = sorted(abs(e) for e in signed)
        if not errs:
            basis = 0.0
        elif len(errs) < 4:
            basis = errs[-1]
        else:
            basis = errs[int(0.75 * (len(errs) - 1))]
        recent = list(reversed(signed[:12]))  # oldest -> newest
        bias = 0.0
        if recent:
            bias = recent[0]
            for e in recent[1:]:
                bias = 0.7 * bias + 0.3 * e
            bias *= len(recent) / (len(recent) + 1.0)  # one sample counts half, three count 3/4, ...
        bias = round(bias, 4)
        n = len(recent)
        t_stat = 0.0
        if n >= 2:
            sd = statistics.stdev(recent)
            mean = statistics.fmean(recent)
            t_stat = abs(mean) / (sd / math.sqrt(n)) if sd > 0 else (float("inf") if abs(mean) > 0 else 0.0)
        consistent = n >= t.basis_min_windows and t_stat >= t.basis_min_t
        applied = bias if consistent else 0.0
        self.store.set_state(f"basis_error:{source}", str(basis))
        self.store.set_state(f"basis_signed:{source}", str(applied))
        self.store.set_state(f"basis_raw:{source}", str(bias))
        self.store.set_state(f"basis_n:{source}", str(n))
        self.store.set_state(f"basis_consistent:{source}", "1" if consistent else "0")
        if source == self._basis_source:
            self.model.basis_error = basis
            self.model.basis_signed = applied
        pairs = self.store.feed_error_pairs(source, 50)
        lag_txt = ""
        if len(pairs) >= 3:
            med0 = statistics.median(abs(a) for a, _ in pairs)
            med60 = statistics.median(abs(b) for _, b in pairs)
            lag_txt = f" · |error| median at the close {med0 * 100:.1f}¢ vs one candle later {med60 * 100:.1f}¢ ({len(pairs)} windows)"
        self.log_event("feed", f"{source} vs settlement: bias {bias * 100:+.1f}¢ over {n} windows "
                               f"({'applied' if consistent else 'not applied yet: ' + ('too few windows' if n < t.basis_min_windows else 'not consistent')})"
                               + lag_txt)

    # ------------------------------------------------------------------ user positions
    def open_position(self, side: str, amount: float, price: float | None = None, note: str = "",
                      target: float | None = None) -> Position:
        """Record what you bought in the Kalshi app: side, dollars spent, and the price (defaults to the live ask).
        `target`, when given, becomes the low end of the sell zone."""
        m = self.tracker.current
        if self.position:
            raise ValueError("a position is already open; sell or remove it first")
        if m is None:
            raise ValueError("no live window to buy into right now")
        side = side.upper()
        if side not in ("UP", "DOWN"):
            raise ValueError("side must be UP or DOWN")
        if amount is None or amount <= 0:
            raise ValueError("amount must be the dollars you spent (> 0)")
        quotes = Quotes.from_market(m)
        if price is None:
            price = quotes.ask(side)
            if price is None:
                raise ValueError("no live ask price; enter the price you paid")
        if not (0 < price < 1):
            raise ValueError("price must be between 0 and 1 dollars (e.g. 0.026 for 2.6¢)")
        qty = amount / price
        entry_fee = taker_fee(price, qty, self.fees)
        now = time.time()
        high = quotes.bid(side)
        own_target = target
        target = target_high = None
        plan = self.plans.plan
        if plan is not None and plan.side == side and plan.ticker == m.ticker:
            target, target_high = plan.target, plan.target_high
            event = self.plans.mark_filled(now)
            if event and plan.id:
                self.store.end_plan(plan.id, "filled", "you bought it", now, plan.hit_target, plan.best_bid, None)
        if target is None and own_target is not None and 0 < own_target < 1:
            target, target_high = own_target, None
        pid = self.store.open_position(m.ticker, side, qty, price, now, note, amount, entry_fee, high)
        zone_ts = 0.0
        if target is not None:
            # the plan's zone was sized for its limit; a fill at a worse price must not put the low under breakeven
            be = self.decider.breakeven(Position(m.ticker, side, qty, price, now, pid, amount, entry_fee, high))
            target = max(target, round(be + 0.02, 2))
            target_high = max(target_high if target_high else min(0.95, target + 0.10), round(target + 0.05, 2))
            target_high = min(target_high, max(0.95, round(target + 0.02, 3)))
            zone_ts = now
            self.store.update_position_zone(pid, target, target_high, zone_ts)
        self.position = Position(m.ticker, side, qty, price, now, pid, amount, entry_fee, high, target, target_high, zone_ts)
        self.log_event("you", f"you bought ${amount:.2f} of {side} @ {cents(price)} = {qty_text(qty)} shares on {m.ticker}"
                              + (f" · sell zone {cents(target)}–{cents(target_high)}" if target else ""))
        return self.position

    def close_position(self, price: float | None = None) -> dict:
        """Record that you sold (cashed out). Price defaults to the live bid for your side."""
        pos = self.position
        if not pos:
            raise ValueError("no open position")
        if price is None:
            m = self.tracker.current
            if m is None or m.ticker != pos.ticker:
                raise ValueError("that window is over; enter the price you sold at (or wait for settlement)")
            price = Quotes.from_market(m).bid(pos.side)
            if price is None:
                raise ValueError("no live bid; enter the price you sold at")
        if not (0 <= price <= 1):
            raise ValueError("price must be between 0 and 1 dollars")
        proceeds = cash_out(pos.qty, price, self.fees)
        pnl = proceeds - pos.cost + pos.realized
        self.store.close_position(pos.id, time.time(), price, pnl)
        self.log_event("you", f"you sold {qty_text(pos.qty)} {pos.side} @ {cents(price)} → cash out ${proceeds:.2f}, "
                              f"{'+' if pnl >= 0 else ''}{pnl:.2f}")
        self.position = None
        return {"pnl": round(pnl, 2), "cash_out": round(proceeds, 2)}

    def cancel_position(self):
        if self.position:
            self.store.delete_position(self.position.id)
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
        self._load_basis(self.feed.active_name)
        if tick and tick.ts != self._last_stored_tick_ts:
            self._last_stored_tick_ts = tick.ts
            self.store.add_tick(tick.ts, self.feed.active_name, tick.price)
        if now - self._last_prune > 600:
            self._last_prune = now
            self.store.prune()
        pred = sig = None
        tau = elapsed = None
        quotes_fresh = self.tracker.quotes_fresh(QUOTE_MAX_AGE_S, now)
        self._resolve_closed_windows(now)
        if m is not None and tick is not None:
            at = clock.now()
            tau = m.seconds_left(at)
            elapsed = m.seconds_elapsed(at)
            buf = self.feed.buffer
            # horizon = trading time left + the settlement candle (settles on that candle's close)
            tau_eff = (tau if tau is not None else 0.0) + self.cfg.trading.settle_lag_s
            self._vol_context(m, now, elapsed, quotes_fresh)
            pred = self.model.predict(tick.price, m.strike, tau_eff, m.yes_mid if quotes_fresh else None, health.age_s,
                                      buf.price_at(now - 60), buf.price_at(now - 180), buf.price_at(now - 300))
            quotes = Quotes.from_market(m) if quotes_fresh else Quotes()
            pos = self.position if (self.position and self.position.ticker == m.ticker) else None
            if pos is not None and quotes_fresh:
                bid = quotes.bid(pos.side)
                if bid is not None and (pos.high_bid is None or bid > pos.high_bid):
                    pos.high_bid = bid
                    self.store.update_position_high(pos.id, bid)
            self._study_step(m, pred, tick.price, now, elapsed, tau)
            # on the board (v1.4): no new settlement call in a window that holds a scheduled report, like the board's
            # own desks (on two months of history the windows opening at a release won only 64%)
            events = self._vol_info.get("events") or []
            t = self.cfg.trading
            blocked = bool(events) and t.call_block_events and t.event_entries == "block" and self.calls.call is None
            made = self.calls.step(now, m.ticker, None if blocked else pred, tau, elapsed, self._study_len(), quotes)
            if blocked:
                self.calls.watch = {"status": f"no call this window ({events[0]['name']} at {events[0]['at']})"}
                self.calls._update_meter(now, pred, tau)  # still show what the model sees
            if made:
                when = f"{int(made['seconds_left'] // 60)}:{int(made['seconds_left'] % 60):02d} left"
                text = (f"settlement call: {made['side']} on {m.ticker} · model {round(made['p_side'] * 100)}% "
                        f"({made['kind']}, {when})")
                self.log_event("call", text)
                self.notifier.notify(f"WTI 15m: settles {made['side']}", text, "Glass")
            raw = self.decider.decide(pred, quotes, tick.price, m.strike, tau, elapsed, pos, now)
            if tau is not None and tau > 0 and now - self._last_snapshot >= 15:
                self._last_snapshot = now
                self.store.add_snapshot(now, m.ticker, tau, tick.price, m.strike, pred)
            if pos is None:
                event = self.plans.update(now, m.ticker, pred, quotes, tick.price, m.strike, tau, raw, False)
                if event:
                    self._on_plan_event(event, now, quotes)
                sig = self._calm_wait(self.plans.display(raw, pred, now) or raw, now)
            else:
                self.plans.update(now, m.ticker, pred, quotes, tick.price, m.strike, tau, None, True)
                sig = self._stable_position_signal(raw, pos, now)
            if sig.key != self._last_signal_key:
                self.store.add_signal(now, m.ticker, sig, pred, tau)
                self.log_event("signal", sig.headline)
                self._last_signal_key = sig.key
            self._maybe_notify(sig, now)
            self._paper_step(m, pred, tick.price, tau, elapsed, now, quotes)
        else:
            self.calls.step(now, m.ticker if m else None, None, None, None, self._study_len())
            event = self.plans.update(now, m.ticker if m else None, None, Quotes(), None, None, None, None, self.position is not None)
            if event:
                self._on_plan_event(event, now, Quotes())
        self.pred = pred
        self.signal = sig
        self.state = self._build_state(now, m, tick, health, pred, sig, tau, elapsed, quotes_fresh)

    # ------------------------------------------------------------------ v10: study period + learned volatility
    def _study_len(self) -> float:
        t = self.cfg.trading
        return max(t.no_entry_first_s, t.study_s)

    def _window_events(self, open_ts: float, close_ts: float) -> list[tuple[float, str]]:
        # the settlement candle closes settle_lag_s after trading stops: a report then still moves the result
        return volcal.events_between(open_ts, close_ts + self.cfg.trading.settle_lag_s, self._extra_events)

    def _refit_vol(self):
        rows = [volcal.VolRow(r["open_time"], r["sigma_forecast"], r["sigma_realized"], r["event"] or "")
                for r in self.store.vol_rows() if r.get("open_time")]
        self.vol_profile.fit(rows)

    def _measure_window(self, ticker: str, open_ts: float, close_ts: float, source: str) -> bool:
        """After a window closes: how much the price really moved after the study period (σ per √s)."""
        start = open_ts + self._study_len()
        ticks = [(tk.ts, tk.price) for tk in self.feed.buffer.since(start) if tk.ts <= close_ts]
        if len(ticks) < 20:
            ticks = self.store.ticks_between(start, close_ts, source)
        sigma, n = volcal.realized_sigma(ticks, start, close_ts)
        if sigma is None:
            return False
        self.store.set_window_realized(ticker, sigma, n)
        return True

    def _backfill_vol(self, now: float):
        """Windows recorded before v10 (or while the app was off at a close): rebuild forecast and realized σ from
        the 15-second snapshots and the stored ticks, as far back as the ticks go (3 days)."""
        study = self._study_len()
        for row in self.store.windows_missing_vol(now - 3 * 86400 + 3600, now - 60):
            open_ts, close_ts = row.get("open_time"), row.get("close_time")
            if not open_ts or not close_ts:
                continue
            if not row.get("sigma_forecast"):
                left = (close_ts - open_ts) - study
                fc = self.store.snapshot_sigma_near(row["ticker"], left - 25, left + 25)
                if not fc:
                    continue
                events = self._window_events(open_ts, close_ts)
                self.store.set_window_forecast(row["ticker"], fc, events[0][1] if events else "")
            source = self.feed.active_name
            for src in (source, "kalshi-live", "hyperliquid", "sim"):
                ticks = self.store.ticks_between(open_ts + study, close_ts, src)
                sigma, n = volcal.realized_sigma(ticks, open_ts + study, close_ts)
                if sigma is not None:
                    self.store.set_window_realized(row["ticker"], sigma, n)
                    break

    def _vol_context(self, m: Market, now: float, elapsed, quotes_fresh: bool):
        """Before predicting: the learned multiplier for this window's hour (or report), and what the decider needs
        to know (study period left, reports inside the window)."""
        t = self.cfg.trading
        open_ts = m.open_time.timestamp() if m.open_time else now
        close_ts = m.close_time.timestamp() if m.close_time else now
        events = self._window_events(open_ts, close_ts)
        if t.vol_profile:
            mult, note, n = self.vol_profile.mult(open_ts, events)
        else:
            mult, note, n = 1.0, "learned correction off", 0
        self.model.vol_mult, self.model.vol_note = mult, note
        st = self._study
        if st.get("ticker") != m.ticker:
            st.clear()
            st.update({"ticker": m.ticker, "odds_lo": None, "odds_hi": None})
            self._forecast_done.discard(m.ticker)
        study = self._study_len()
        if quotes_fresh and m.yes_mid is not None and elapsed is not None and 0 <= elapsed <= study:
            st["odds_lo"] = m.yes_mid if st["odds_lo"] is None else min(st["odds_lo"], m.yes_mid)
            st["odds_hi"] = m.yes_mid if st["odds_hi"] is None else max(st["odds_hi"], m.yes_mid)
        ev = [{"name": name, "at": clock.et_label(dt.datetime.fromtimestamp(ts, clock.UTC))} for ts, name in events]
        self.decider.context = {"events": ev, "study": st, "study_s": study}
        self._vol_info = {"mult": round(mult, 3), "note": note, "n": n, "events": ev, "study_s": study,
                          "studying": elapsed is not None and elapsed < study,
                          "study_left_s": None if elapsed is None else max(0, round(study - elapsed)),
                          "study": {k: v for k, v in st.items() if k != "ticker"}}

    def _study_step(self, m: Market, pred: Prediction, price: float, now: float, elapsed, tau):
        """During the study period: what this window is doing; at its end: record the forecast to learn from."""
        st = self._study
        if m.open_time is None or elapsed is None:
            return
        open_ts = m.open_time.timestamp()
        study = self._study_len()
        if elapsed <= study + 1:
            ticks = [(tk.ts, tk.price) for tk in self.feed.buffer.since(open_ts)]
            so_far, n = volcal.realized_sigma(ticks, open_ts, now, min_returns=6)
            if so_far and pred.sigma > 0:
                st["moves_x"] = round(so_far / pred.sigma, 2)  # vs the corrected forecast
            if m.strike:
                st["drift"] = round(price - m.strike, 3)
            st["sigma_per_window"] = round(pred.sigma * math.sqrt(max(tau or 0, 0) + self.cfg.trading.settle_lag_s), 3)
        if (elapsed >= study and m.ticker not in self._forecast_done and elapsed <= study + 120 and tau and tau > 0
                and pred.sigma_raw > 0 and pred.confidence not in ("warming_up", "stale")):
            self._forecast_done.add(m.ticker)
            events = self._window_events(open_ts, m.close_time.timestamp() if m.close_time else now)
            self.store.set_window_forecast(m.ticker, pred.sigma_raw, events[0][1] if events else "")

    def _calm_wait(self, sig: Signal, now: float, hold_s: float = 4.0) -> Signal:
        """Between plans the analysis text can flip every second; keep a WAIT headline for `hold_s` unless the
        action changes or a plan/cooldown message arrives."""
        if sig.action != "WAIT" or sig.reasons[:1] in (["plan"], ["forming"], ["missed"], ["cancelled"], ["expired"]):
            self._wait_sig, self._wait_since = None, 0.0
            return sig
        if self._wait_sig is not None and now - self._wait_since < hold_s and self._wait_sig.reasons != sig.reasons:
            return self._wait_sig
        if self._wait_sig is None or self._wait_sig.reasons != sig.reasons:
            self._wait_since = now
        self._wait_sig = sig
        return sig

    def _stable_position_signal(self, raw: Signal, pos: Position, now: float) -> Signal:
        """Persist the sell zone the decider committed (set once, then re-planned at most every zone_refresh_s)
        and remember what was shown: the decider's latches and its two-second debounce key on the rule shown
        last second and on the rule that fired last second."""
        self._remember(pos, raw, now)
        return raw

    def _remember(self, pos: Position, sig: Signal, now: float, paper: bool = False):
        sc = sig.scalp or {}
        zone = sc.get("commit_zone")
        if zone and len(zone) == 2:
            pos.target, pos.target_high, pos.zone_ts = float(zone[0]), float(zone[1]), now
            if paper:
                self.store.set_state(f"paper_zone:{pos.ticker}", f"{pos.target},{pos.target_high},{now}")
            else:
                self.store.update_position_zone(pos.id, pos.target, pos.target_high, now)
                self.log_event("zone", f"sell zone for your {pos.side}: {cents(pos.target)}–{cents(pos.target_high)}")
        pos.last_reason = sig.reasons[0] if sig.reasons else None
        pos.last_raw_reason = sig.triggers.get("raw_rule", pos.last_reason)

    def _on_plan_event(self, event: PlanEvent, now: float, quotes: Quotes):
        plan = event.plan
        if event.kind == "created":
            plan.id = self.store.add_plan(plan)
            self.log_event("plan", event.text)
            if now - self._last_notified.get("PLAN", 0.0) >= 30:
                self._last_notified["PLAN"] = now
                self.notifier.notify(f"WTI 15m: PLAN {plan.side}", event.text, "Glass")
            # paper: assume a fill at the limit
            fee = taker_fee(plan.limit, plan.shares, self.fees)
            if self.store.open_paper_trade_for(plan.ticker) is None:
                self.store.open_paper_trade(plan.ticker, plan.side, now, plan.limit, plan.shares, fee, plan.tier, plan.p_at_plan)
                self.store.set_state(f"paper_zone:{plan.ticker}",
                                     f"{plan.target},{plan.target_high or min(0.95, plan.target + 0.10)},{now}")
            return
        hypo = None
        if event.kind in ("cancelled", "expired"):
            exit_px = plan.target if plan.hit_target else quotes.bid(plan.side)
            if exit_px is not None:
                hypo = plan.shares * (exit_px - plan.limit) - taker_fee(plan.limit, plan.shares, self.fees) - taker_fee(exit_px, plan.shares, self.fees)
        if plan.id:
            self.store.end_plan(plan.id, event.kind, event.text, now, plan.hit_target, plan.best_bid, hypo)
        self.log_event("plan", f"plan {plan.side} up to {cents(plan.limit)}: {event.kind} · {event.text}")
        if event.kind in ("missed", "cancelled") and now - self._last_notified.get("PLAN_END", 0.0) >= 30:
            self._last_notified["PLAN_END"] = now
            self.notifier.notify(f"WTI 15m: plan {event.kind}", event.text, "Submarine")

    def _maybe_notify(self, sig: Signal, now: float):
        """Desktop notification at most once per 30 s per action. A SELL is already debounced by the decider
        (two seconds in a row before it shows), so it notifies the second it appears; a BUY must hold 2 s."""
        if sig.action not in ("BUY", "SELL"):
            self._pending_key, self._pending_count = None, 0
            return
        if sig.key == self._pending_key:
            self._pending_count += 1
        else:
            self._pending_key, self._pending_count = sig.key, 1
        needed = 1 if sig.action == "SELL" else 2
        if self._pending_count == needed and now - self._last_notified.get(sig.action, 0.0) >= 30:
            self._last_notified[sig.action] = now
            self.notifier.notify(f"WTI 15m: {sig.action} {sig.side or ''}".strip(), sig.headline,
                                 "Glass" if sig.action == "BUY" else "Submarine")

    def _paper_step(self, m: Market, pred: Prediction, price: float, tau, elapsed, now: float, quotes: Quotes):
        """Paper trades are opened when a plan is created (fill assumed at the limit) and managed like a position."""
        paper = self.store.open_paper_trade_for(m.ticker)
        if paper is None:
            self._paper_pos = None
            return
        if tau is not None and tau <= 0:
            return  # settlement will close it
        pos = self._paper_pos
        if pos is None or pos.id != paper["id"]:
            low = high = None
            zone_ts = 0.0
            raw_zone = self.store.get_state(f"paper_zone:{m.ticker}")
            if raw_zone:
                parts = raw_zone.split(",")
                if len(parts) == 3:
                    low, high, zone_ts = _float(parts[0], 0.0) or None, _float(parts[1], 0.0) or None, _float(parts[2], 0.0)
            pos = Position(m.ticker, paper["side"], float(paper["size"]), paper["entry_price"], paper["entry_ts"], paper["id"],
                           amount=paper["entry_price"] * paper["size"], entry_fee=paper["entry_fee"] or 0.0, target=low,
                           target_high=high, zone_ts=zone_ts)
            self._paper_pos = pos
        bid = quotes.bid(pos.side)
        if bid is not None and (pos.high_bid is None or bid > pos.high_bid):
            pos.high_bid = bid
        psig = self.decider.decide(pred, quotes, price, m.strike, tau, elapsed, pos, now)
        self._remember(pos, psig, now, paper=True)  # same zone, latches and two-second debounce as your card
        if psig.action == "SELL" and psig.price is not None:
            fee = taker_fee(psig.price, pos.qty, self.fees)
            self.store.close_paper_trade(paper["id"], now, psig.price, fee, psig.reasons[0] if psig.reasons else "sell")
            self.log_event("paper", f"paper SELL {qty_text(pos.qty)} {pos.side} @ {cents(psig.price)} ({psig.reasons[0] if psig.reasons else ''})")
            self._paper_pos = None

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
            data["settle_lag_s"] = self.cfg.trading.settle_lag_s
            src = self._basis_source or ""
            data["basis"] = {"source": src, "applied": self.model.basis_signed,
                             "raw": _float(self.store.get_state(f"basis_raw:{src}"), 0.0),
                             "n": int(_float(self.store.get_state(f"basis_n:{src}"), 0.0)),
                             "consistent": self.store.get_state(f"basis_consistent:{src}") == "1",
                             "min_windows": self.cfg.trading.basis_min_windows}
            data["plans"] = self.store.plan_stats()
            data["vol_profile"] = self.vol_profile.summary()
            self._stats_cache = (time.time(), data)
            return data
        return cached

    def _position_state(self, m: Market | None, sig: Signal | None, quotes_fresh: bool) -> dict | None:
        pos = self.position
        if pos is None:
            return None
        d = pos.as_dict()
        live: dict = {"bid": None, "cash_out": None, "pnl": None, "awaiting_settlement": False, "quotes_fresh": quotes_fresh}
        if m is not None and m.ticker == pos.ticker:
            bid = Quotes.from_market(m).bid(pos.side) if quotes_fresh else None
            if bid is not None:
                cash = cash_out(pos.qty, bid, self.fees)
                live.update({"bid": bid, "cash_out": round(cash, 2), "pnl": round(cash - pos.cost, 2)})
        else:
            live["awaiting_settlement"] = True
        d["live"] = live
        d["scalp"] = sig.scalp if (sig is not None and sig.scalp) else None
        return d

    def _build_state(self, now, m, tick, health, pred, sig, tau, elapsed, quotes_fresh) -> dict:
        paper = self.store.open_paper_trade_for(m.ticker) if m else None
        return {
            "now": now,
            "status": "live" if (m and tick) else ("no_market" if tick else "no_feed"),
            "uptime_s": round(now - self.started_ts),
            "series": self.tracker.series.summary() if self.tracker.series else None,
            "fees": {"fee_type": self.fees.fee_type, "multiplier": self.fees.multiplier, "is_estimate": self.fees.is_estimate},
            "market": m.summary() if m else None,
            "quotes_fresh": quotes_fresh,
            "seconds_left": None if tau is None else round(tau, 1),
            "seconds_elapsed": None if elapsed is None else round(elapsed, 1),
            "countdown": clock.fmt_countdown(tau),
            "tick": {"ts": tick.ts, "price": tick.price} if tick else None,
            "feed": health.as_dict(),
            "prediction": pred.as_dict() if pred else None,
            "signal": sig.as_dict() if sig else None,
            "position": self._position_state(m, sig, quotes_fresh),
            "plan": self.plans.plan.as_dict() if self.plans.plan else None,
            "plan_cooldown_s": max(0, round(self.plans.cooldown_until - now)) if self.plans.plan is None else 0,
            "last_plan": self.plans.last.as_dict() if self.plans.last else None,
            "paper": paper,
            "tracker": {"last_error": self.tracker.last_error, "polls": self.tracker.poll_count,
                        "pending_settlements": list(self.tracker.pending.keys()),
                        "quote_source": self.tracker.quote_source, "orderbook_errors": self.tracker.orderbook_errors,
                        "quote_age_s": None if self.tracker.last_quote_ts is None else round(now - self.tracker.last_quote_ts, 1)},
            "events": list(self.events)[:25],
            "vol": self._vol_info,
            "call": self.calls.state(),
            "config": {"bankroll": self.cfg.trading.bankroll, "edge_min": self.cfg.trading.edge_min,
                       "min_warmup_minutes": self.cfg.trading.min_warmup_minutes, "feed": self.cfg.feed.source,
                       "settle_lag_s": self.cfg.trading.settle_lag_s, "unit_dollars": self.cfg.trading.unit_dollars,
                       "max_trade_dollars": self.cfg.trading.max_trade_dollars,
                       "tradingview_symbol": self.cfg.feed.tradingview_symbol, "study_s": self._study_len()},
        }

    def chart(self, minutes: float = 20) -> dict:
        now = time.time()
        m = self.tracker.current
        since = now - minutes * 60
        if m and m.open_time:
            since = min(since, m.open_time.timestamp() - 300)
        ticks = [(t.ts, t.price) for t in self.feed.buffer.since(since)]
        if not ticks:
            ticks = self.store.ticks_since(since, self.feed.active_name)
        return {"ticks": ticks, "strike": m.strike if m else None,
                "open_time": m.open_time.timestamp() if (m and m.open_time) else None,
                "close_time": m.close_time.timestamp() if (m and m.close_time) else None,
                "source": self.feed.active_name}


def _float(value, default: float) -> float:
    try:
        return float(value) if value not in (None, "") else default
    except (TypeError, ValueError):
        return default


def _position_from_row(row: dict | None) -> Position | None:
    if not row:
        return None
    return Position(row["ticker"], row["side"], float(row["qty"]), float(row["avg_price"]), float(row["opened_ts"]), row["id"],
                    row.get("amount"), float(row.get("entry_fee") or 0.0), row.get("high_bid"), row.get("target"),
                    row.get("target_high"), float(row.get("zone_ts") or 0.0), realized=float(row.get("realized") or 0.0))
