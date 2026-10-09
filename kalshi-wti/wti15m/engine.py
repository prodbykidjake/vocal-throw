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

from . import clock
from .config import Config
from .decision import DecisionEngine, Position, Quotes, Signal, cash_out, cents, pct, qty_text
from .fees import FeeSchedule, fee_per_contract, taker_fee
from .feeds.base import PriceFeed, Tick
from .kalshi import Market, MarketTracker
from .model import Calibrator, Model, Prediction
from .notify import Notifier
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
        self._sell_streak = 0
        self._last_hold_sig: Signal | None = None
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
        self._load_basis(self.feed.active_name)
        self._recover()

    # ------------------------------------------------------------------ per-feed basis
    def _load_basis(self, source: str):
        """The feed's measured bias vs settlement is per source: Hyperliquid's 6¢ must never be applied to Kalshi's."""
        if source == self._basis_source:
            return
        self._basis_source = source
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
            if latest is None or (latest.ts < close_ts + lag and now < close_ts + lag + 30):
                continue  # wait until the feed covers the settlement candle (or give up after 30 s grace)
            source = cap.get("source") or self.feed.active_name
            p0 = self._feed_price_near(close_ts, source=source)
            p_lag = self._feed_price_near(close_ts + max(lag - 1, 0), source=source)
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
        # the learner's weight grows with ALL settled windows, not just the pooled batch
        X, y, _ = self.store.training_rows_recent(50)
        if len(X) >= 20 and len(set(y.tolist())) == 2:
            self.model.cal.fit_pooled(X, y, self.store.settled_window_count())
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
        pnl = pos.qty * value - pos.cost
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
        se = statistics.pstdev(recent) / math.sqrt(n) if n >= 2 else float("inf")
        consistent = n >= t.basis_min_windows and abs(bias) >= t.basis_min_t * se
        applied = bias if consistent else 0.0
        self.store.set_state(f"basis_error:{source}", str(basis))
        self.store.set_state(f"basis_signed:{source}", str(applied))
        self.store.set_state(f"basis_raw:{source}", str(bias))
        self.store.set_state(f"basis_n:{source}", str(n))
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
    def open_position(self, side: str, amount: float, price: float | None = None, note: str = "") -> Position:
        """Record what you bought in the Kalshi app: side, dollars spent, and the price (defaults to the live ask)."""
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
        target = target_high = None
        plan = self.plans.plan
        if plan is not None and plan.side == side and plan.ticker == m.ticker:
            target, target_high = plan.target, plan.target_high
            event = self.plans.mark_filled(now)
            if event and plan.id:
                self.store.end_plan(plan.id, "filled", "you bought it", now, plan.hit_target, plan.best_bid, None)
        pid = self.store.open_position(m.ticker, side, qty, price, now, note, amount, entry_fee, high)
        zone_ts = 0.0
        if target is not None:
            zone_ts = now
            self.store.update_position_zone(pid, target, target_high if target_high else min(0.95, target + 0.10), zone_ts)
        self.position = Position(m.ticker, side, qty, price, now, pid, amount, entry_fee, high, target, target_high, zone_ts)
        self._sell_streak = 0
        self._last_hold_sig = None
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
        pnl = proceeds - pos.cost
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
            pred = self.model.predict(tick.price, m.strike, tau_eff, m.yes_mid if quotes_fresh else None, health.age_s,
                                      buf.price_at(now - 60), buf.price_at(now - 180), buf.price_at(now - 300))
            quotes = Quotes.from_market(m) if quotes_fresh else Quotes()
            pos = self.position if (self.position and self.position.ticker == m.ticker) else None
            if pos is not None and quotes_fresh:
                bid = quotes.bid(pos.side)
                if bid is not None and (pos.high_bid is None or bid > pos.high_bid):
                    pos.high_bid = bid
                    self.store.update_position_high(pos.id, bid)
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
            event = self.plans.update(now, m.ticker if m else None, None, Quotes(), None, None, None, None, self.position is not None)
            if event:
                self._on_plan_event(event, now, Quotes())
        self.pred = pred
        self.signal = sig
        self.state = self._build_state(now, m, tick, health, pred, sig, tau, elapsed, quotes_fresh)

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
        and require SELL NOW to hold for 2 consecutive seconds."""
        sc = raw.scalp or {}
        zone = sc.get("commit_zone")
        if zone and len(zone) == 2:
            pos.target, pos.target_high, pos.zone_ts = float(zone[0]), float(zone[1]), now
            self.store.update_position_zone(pos.id, pos.target, pos.target_high, now)
            self.log_event("zone", f"sell zone for your {pos.side}: {cents(pos.target)}–{cents(pos.target_high)}")
        pos.last_reason = raw.reasons[0] if raw.reasons else None  # the decider's latch for next second
        if raw.action == "SELL":
            self._sell_streak += 1
            if self._sell_streak < 2 and self._last_hold_sig is not None:
                return self._last_hold_sig
            return raw
        self._sell_streak = 0
        self._last_hold_sig = raw
        return raw

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
        """Notify on BUY/SELL only once the call has held for 2 consecutive seconds, at most once per 30 s per action.
        The on-screen box still flips instantly; this only de-spams the sound/notification."""
        if sig.action not in ("BUY", "SELL"):
            self._pending_key, self._pending_count = None, 0
            return
        if sig.key == self._pending_key:
            self._pending_count += 1
        else:
            self._pending_key, self._pending_count = sig.key, 1
        if self._pending_count == 2 and now - self._last_notified.get(sig.action, 0.0) >= 30:
            self._last_notified[sig.action] = now
            self.notifier.notify(f"WTI 15m: {sig.action} {sig.side or ''}".strip(), sig.headline,
                                 "Glass" if sig.action == "BUY" else "Submarine")

    def _paper_step(self, m: Market, pred: Prediction, price: float, tau, elapsed, now: float, quotes: Quotes):
        """Paper trades are opened when a plan is created (fill assumed at the limit) and managed like a position."""
        paper = self.store.open_paper_trade_for(m.ticker)
        if paper is None:
            return
        if tau is not None and tau <= 0:
            return  # settlement will close it
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
        psig = self.decider.decide(pred, quotes, price, m.strike, tau, elapsed, pos, now)
        zone = (psig.scalp or {}).get("commit_zone")
        if zone and len(zone) == 2:
            self.store.set_state(f"paper_zone:{m.ticker}", f"{zone[0]},{zone[1]},{now}")
        if psig.action == "SELL" and psig.price is not None:
            fee = taker_fee(psig.price, pos.qty, self.fees)
            self.store.close_paper_trade(paper["id"], now, psig.price, fee, psig.reasons[0] if psig.reasons else "sell")
            self.log_event("paper", f"paper SELL {qty_text(pos.qty)} {pos.side} @ {cents(psig.price)} ({psig.reasons[0] if psig.reasons else ''})")

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
            src = self._basis_source or ""
            data["basis"] = {"source": src, "applied": self.model.basis_signed,
                             "raw": _float(self.store.get_state(f"basis_raw:{src}"), 0.0),
                             "n": int(_float(self.store.get_state(f"basis_n:{src}"), 0.0))}
            data["plans"] = self.store.plan_stats()
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
            "config": {"bankroll": self.cfg.trading.bankroll, "edge_min": self.cfg.trading.edge_min,
                       "min_warmup_minutes": self.cfg.trading.min_warmup_minutes, "feed": self.cfg.feed.source,
                       "settle_lag_s": self.cfg.trading.settle_lag_s, "unit_dollars": self.cfg.trading.unit_dollars,
                       "max_trade_dollars": self.cfg.trading.max_trade_dollars},
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
                    row.get("target_high"), float(row.get("zone_ts") or 0.0))
