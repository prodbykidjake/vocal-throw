"""Places the coach's calls on your Kalshi account, no button presses.

Runs once a second after the engine's step (Engine.run awaits `tick`). One thing at a time:

  no position, no order  -> a plan on the signal card (valid, ask at or a hair over the limit) or a quick-scalp
                            call becomes ONE buy order at the card's limit, for the card's dollars, capped by the
                            risk limits; it rests for buy_ttl_s and is cancelled when unfilled, when the plan
                            dies, or when the window ends
  order open             -> poll it; fills open / add to the engine's position at the real fill price and fee
  position open          -> when the position card says SELL NOW, one immediate-or-cancel, reduce-only sell
                            down to bid − sell_slip_cents; partial fills take part of the position off
  every sync_s           -> balance and your Kalshi position on the live window; a position you opened in the
                            Kalshi app is adopted, one you sold there is closed here

Hard limits (config [auto]): per order, per window, per day, daily realized loss, losing trades in a row. A manual
PAUSE stops new buys (sells still run); STOP cancels the open order and pauses. `dry_run` (the default) runs the
same code against a simulated fill engine on the real quotes, so the log shows exactly what live would have done.
"""
from __future__ import annotations

import datetime as dt
import logging
import time

from . import clock
from .broker import AccountPosition, BrokerError, OrderResult
from .config import Config
from .decision import Quotes, cents

log = logging.getLogger(__name__)

QUICK_TTL_S = 8.0
RETRY_COOLDOWN_S = 10.0
SELL_RETRY_S = 2.0
ADOPT_GRACE_S = 15.0  # a position must be this old before "Kalshi shows none" means you sold it by hand


class AutoTrader:
    def __init__(self, cfg: Config, broker, store, engine, mode: str | None = None):
        self.cfg = cfg
        self.a = cfg.auto
        self.broker = broker
        self.store = store
        self.engine = engine
        self.mode = mode or ("dry_run" if cfg.auto.dry_run else "live")  # dry_run | live | sim
        self.paused = False
        self.pause_reason = ""
        self.blocked = ""  # why no buy right now (limits), recomputed every tick
        self.order: dict | None = None
        self.balance: float | None = None
        self.kalshi_pos: AccountPosition | None = None
        self.synced_ts = 0.0
        self.sync_error: str | None = None
        self.last_text = "auto trading armed" if self.mode != "live" else "LIVE: auto trading armed"
        self.last_ts = time.time()
        self.attempted: set[str] = set()
        self.cooldown_until = 0.0
        self.last_sell_try = 0.0
        self.last_fill_ts = 0.0
        self.errors = 0
        self.day_bought = 0.0
        self.day_pnl = 0.0
        self.losses_row = 0
        self._losses_ack = 0  # losses-in-a-row count you pressed RESUME on: the stop re-arms only after another loss
        self.window_bought = 0.0
        self._day_key = self._today()

    # ------------------------------------------------------------------ small helpers
    @staticmethod
    def _today() -> str:
        return dt.datetime.now(clock.ET).date().isoformat()

    @staticmethod
    def _day_start() -> float:
        d = dt.datetime.now(clock.ET)
        return d.replace(hour=0, minute=0, second=0, microsecond=0).timestamp()

    def say(self, text: str, kind: str = "auto"):
        self.last_text, self.last_ts = text, time.time()
        self.engine.log_event(kind, text)

    def pause(self, reason: str):
        self.paused, self.pause_reason = True, reason
        self.say(f"auto trading PAUSED: {reason}")

    def resume(self):
        self.paused, self.pause_reason = False, ""
        self._losses_ack = self.losses_row
        self.say("auto trading resumed")

    async def stop_all(self):
        if self.order:
            try:
                await self.broker.cancel(self.order["order_id"], self.order["ticker"])
            except BrokerError as exc:
                self.say(f"cancel failed: {exc}")
        self.pause("stopped by you")

    def _quotes(self):
        m = self.engine.tracker.current
        return (m, Quotes.from_market(m)) if m is not None else (None, Quotes())

    # ------------------------------------------------------------------ per-second tick
    async def tick(self, now: float):
        if not self.a.enabled:
            return
        m, quotes = self._quotes()
        ticker = m.ticker if m else None
        await self._sync(now, ticker)
        if self.order is not None:
            await self._track_order(now, ticker, quotes)
            return
        pos = self.engine.position
        if pos is not None and (ticker is None or pos.ticker != ticker):
            return  # waiting for settlement
        if m is None:
            self.blocked = "no live window"
            return
        if pos is not None:
            await self._maybe_sell(now, m, quotes, pos)
        else:
            await self._maybe_buy(now, m, quotes)

    # ------------------------------------------------------------------ account sync
    async def _sync(self, now: float, ticker: str | None):
        if now - self.synced_ts < self.a.sync_s:
            return
        self.synced_ts = now
        if self._day_key != self._today():
            self._day_key = self._today()
            self.attempted.clear()
        day0 = self._day_start()
        self.day_bought = self.store.bought_since(day0, mode=self.mode)
        pnls = self.store.closed_pnls_since(day0)
        self.day_pnl = round(sum(pnls), 2)
        self.losses_row = 0
        for p in self.store.last_closed_pnls(self.a.max_losses_in_a_row + 1):
            if p < 0:
                self.losses_row += 1
            else:
                break
        self.window_bought = self.store.bought_since(day0, ticker, self.mode) if ticker else 0.0
        try:
            self.balance = await self.broker.balance()
            positions = await self.broker.positions(ticker) if ticker else []
            self.sync_error = None
        except BrokerError as exc:
            self.sync_error = str(exc)
            if exc.status in (401, 403) and not self.paused:
                self.pause(f"Kalshi rejected the API key (HTTP {exc.status}); check [auto] api_key_id / private_key_path")
            return
        except Exception as exc:  # the trader must never kill the engine loop
            self.sync_error = f"{type(exc).__name__}: {exc}"
            log.exception("account sync failed")
            return
        kp = next((p for p in positions if p.ticker == ticker), None)
        self.kalshi_pos = kp
        if ticker is None or self.order is not None:
            return
        m, quotes = self._quotes()
        pos = self.engine.position
        mine = pos is not None and pos.ticker == ticker
        if kp is not None and kp.qty > 0.009 and kp.side and not mine and pos is None:
            # bought in the Kalshi app (or a fill we lost track of): adopt it so the sell rules apply
            price = kp.avg_price or quotes.ask(kp.side)
            if price:
                try:
                    self.engine.open_position(kp.side, kp.exposure or kp.qty * price, price, "synced from Kalshi", entry_fee=0.0)
                    self.attempted.add(f"adopt:{ticker}")
                    self.say(f"adopted your {kp.qty:.2f} {kp.side} @ {cents(price)} from Kalshi")
                except ValueError as exc:
                    self.say(f"could not adopt the Kalshi position: {exc}")
        elif mine and (kp is None or kp.qty <= 0.009) and now - pos.opened_ts > ADOPT_GRACE_S and now - self.last_fill_ts > ADOPT_GRACE_S:
            bid = quotes.bid(pos.side)
            try:
                res = self.engine.close_position(bid)
                self.say(f"Kalshi shows no {pos.side} on {ticker} any more: recorded as sold by you at {cents(bid)} ({res['pnl']:+.2f})")
            except ValueError as exc:
                self.say(f"position vanished on Kalshi but could not record it: {exc}")

    # ------------------------------------------------------------------ buys
    def _gate(self, now: float, amount: float, seconds_left: float | None) -> str | None:
        a = self.a
        if self.paused:
            return f"paused: {self.pause_reason}"
        if now < self.cooldown_until:
            return "cooling down after the last order"
        if self.sync_error:
            return f"account sync failing: {self.sync_error[:80]}"
        if now - self.synced_ts > 3 * a.sync_s:
            return "account not synced recently"
        if self.kalshi_pos is not None and self.kalshi_pos.qty > 0.009:
            return "Kalshi already shows a position on this window"
        if seconds_left is None or seconds_left <= self.cfg.trading.late_entry_s:
            return "too late in the window"
        if not self.engine.state.get("quotes_fresh"):
            return "Kalshi quotes stale"
        if self.day_pnl <= -abs(a.max_day_loss):
            return f"daily loss limit hit ({self.day_pnl:+.2f} today)"
        if self.losses_row >= a.max_losses_in_a_row and self.losses_row > self._losses_ack:
            return f"{self.losses_row} losses in a row: press RESUME to keep going"
        if self.day_bought + amount > a.max_day_dollars + 1e-9:
            return f"daily buy limit (${a.max_day_dollars:.0f}) reached"
        if self.window_bought + amount > a.max_window_dollars + 1e-9:
            return f"window buy limit (${a.max_window_dollars:.0f}) reached"
        if self.balance is not None and self.balance < 1.5:
            return f"balance ${self.balance:.2f} is too small to trade"
        return None

    async def _maybe_buy(self, now: float, m, quotes: Quotes):
        eng = self.engine
        tau = m.seconds_left()
        cand = None
        plan = eng.plans.plan
        if self.a.take_plans and plan is not None and plan.ticker == m.ticker and plan.status == "open":
            key = f"plan:{plan.id}"
            ask = quotes.ask(plan.side)
            if key not in self.attempted and ask is not None and ask <= plan.limit + 0.0051:
                cand = {"key": key, "side": plan.side, "price": max(plan.limit, ask), "amount": plan.amount, "ttl": self.a.buy_ttl_s,
                        "reason": "plan", "target": plan.target, "target_high": plan.target_high,
                        "text": f"plan {plan.side} up to {cents(plan.limit)}"}
        if cand is None and self.a.take_quick:
            call = eng._quick.get("call")
            if call:
                key = f"quick:{m.ticker}:{call['side']}:{int(eng._quick.get('since', 0))}"
                if key not in self.attempted:
                    cand = {"key": key, "side": call["side"], "price": call["limit"], "amount": float(call["amount"]), "ttl": QUICK_TTL_S,
                            "reason": "quick", "target": call["target"], "target_high": None,
                            "text": f"quick scalp {call['side']} @ {cents(call['ask'])} → {cents(call['target'])}+"}
        if cand is None:
            self.blocked = ""
            return
        amount = min(cand["amount"], self.a.max_order_dollars, self.cfg.trading.max_trade_dollars)
        if self.balance is not None:
            amount = min(amount, round(self.balance - 0.5, 2))  # a plan bigger than the account is cut to what is there
        why = self._gate(now, amount, tau)
        if why:
            self.blocked = why
            if why.startswith(("daily", "window", "balance", "paused", f"{self.losses_row} losses")):
                self.attempted.add(cand["key"])  # a limit is not going to lift this second; do not re-check every tick
            return
        self.blocked = ""
        self.attempted.add(cand["key"])
        price = cand["price"]
        count = amount / price
        if count < (0.01 if self.a.fractional else 1):
            self.say(f"skip {cand['text']}: ${amount:.2f} buys too little")
            return
        coid = None
        try:
            res = await self.broker.place(m.ticker, cand["side"], "buy", price, count, "good_till_canceled", cand["ttl"])
        except BrokerError as exc:
            self.errors += 1
            self.cooldown_until = now + RETRY_COOLDOWN_S
            body = (exc.body or str(exc)).lower()
            if exc.status == 400 and self.a.fractional and ("count" in body or "fraction" in body):
                self.a.fractional = False
                self.broker.fractional = False
                self.say(f"Kalshi rejected a fractional count; switching to whole contracts ({exc})")
            elif exc.status in (401, 403):
                self.pause(f"Kalshi rejected the API key (HTTP {exc.status})")
            else:
                self.say(f"BUY failed: {exc}")
            self.store.add_order(now, m.ticker, cand["side"], "buy", price, count, amount, "", coid or "", "error", cand["reason"],
                                 str(exc)[:200], self.mode)
            return
        row = self.store.add_order(now, m.ticker, cand["side"], "buy", price, count, amount, res.order_id, res.client_order_id,
                                   res.status, cand["reason"], cand["text"], self.mode)
        self.order = {"row": row, "order_id": res.order_id, "ticker": m.ticker, "side": cand["side"], "action": "buy", "price": price,
                      "count": count, "amount": amount, "placed": now, "ttl": cand["ttl"], "reason": cand["reason"],
                      "target": cand["target"], "target_high": cand["target_high"], "filled": 0.0, "cost": 0.0, "fees": 0.0,
                      "cancel_sent": False, "plan_id": plan.id if (plan is not None and cand["reason"] == "plan") else None}
        self.say(f"{'LIVE ' if self.mode == 'live' else ''}BUY {cand['side']} · ${amount:.2f} ({count:.2f} shares) up to {cents(price)} · {cand['text']}", "order")
        await self._apply(res, now, quotes)

    # ------------------------------------------------------------------ sells
    async def _maybe_sell(self, now: float, m, quotes: Quotes, pos):
        sig = self.engine.signal
        if sig is None or sig.action != "SELL" or now - self.last_sell_try < SELL_RETRY_S:
            return
        if not self.engine.state.get("quotes_fresh"):
            return
        bid = quotes.bid(pos.side)
        tau = m.seconds_left()
        if bid is None or tau is None or tau <= 1.0:
            return
        self.last_sell_try = now
        price = max(0.01, bid - self.a.sell_slip_cents / 100.0)
        qty = pos.qty
        if self.kalshi_pos is not None and self.kalshi_pos.side == pos.side and self.kalshi_pos.qty > 0:
            qty = min(qty, self.kalshi_pos.qty)
        reason = sig.reasons[0] if sig.reasons else "sell"
        try:
            res = await self.broker.place(m.ticker, pos.side, "sell", price, qty, "immediate_or_cancel", None, None, True)
        except BrokerError as exc:
            self.errors += 1
            self.say(f"SELL failed: {exc}")
            self.store.add_order(now, m.ticker, pos.side, "sell", price, qty, qty * bid, "", "", "error", reason, str(exc)[:200], self.mode)
            return
        row = self.store.add_order(now, m.ticker, pos.side, "sell", price, qty, qty * bid, res.order_id, res.client_order_id, res.status,
                                   reason, sig.headline[:120], self.mode)
        self.order = {"row": row, "order_id": res.order_id, "ticker": m.ticker, "side": pos.side, "action": "sell", "price": price,
                      "count": qty, "amount": qty * bid, "placed": now, "ttl": 3.0, "reason": reason, "filled": 0.0, "cost": 0.0,
                      "fees": 0.0, "cancel_sent": False, "plan_id": None}
        self.say(f"{'LIVE ' if self.mode == 'live' else ''}SELL {pos.side} · {qty:.2f} shares at {cents(bid)} (limit {cents(price)}) · {reason}", "order")
        await self._apply(res, now, quotes)

    # ------------------------------------------------------------------ order tracking
    async def _track_order(self, now: float, ticker: str | None, quotes: Quotes):
        o = self.order
        if o is None:
            return
        eng = self.engine
        plan_dead = o["plan_id"] is not None and (eng.plans.plan is None or eng.plans.plan.id != o["plan_id"])
        expired = now - o["placed"] > o["ttl"]
        if not o["cancel_sent"] and (expired or ticker != o["ticker"] or (o["action"] == "buy" and plan_dead)):
            o["cancel_sent"] = True
            try:
                await self.broker.cancel(o["order_id"], o["ticker"])
            except BrokerError as exc:
                self.say(f"cancel failed: {exc}")
        try:
            res = await self.broker.order(o["order_id"], o["side"])
        except BrokerError as exc:
            self.errors += 1
            if now - o["placed"] > o["ttl"] + 30:
                self.say(f"lost track of order {o['order_id']}: {exc}; dropping it")
                self.store.update_order(o["row"], "unknown", o["filled"], None, o["fees"], str(exc)[:200])
                self.order = None
                self.synced_ts = 0.0
            return
        await self._apply(res, now, quotes)

    async def _apply(self, res: OrderResult, now: float, quotes: Quotes):
        """Book new fills into the engine's position; close out the order when it is done."""
        o = self.order
        if o is None:
            return
        eng = self.engine
        new = res.filled - o["filled"]
        if new > 1e-9:
            # the fill price, in the side's own terms: from the fills ledger (it states the Yes AND the No price of
            # every fill, so a Down fill cannot be mistaken for its Yes mirror), else the broker's running average,
            # else the limit
            cost_now = None
            try:
                fills = await self.broker.fills(o["ticker"], o["order_id"], o["side"])
                qty = sum(f["count"] for f in fills)
                if qty >= res.filled - 1e-6 and qty > 0:
                    cost_now = sum(f["count"] * f["price"] for f in fills) * (res.filled / qty)
            except Exception as exc:  # the ledger is a nicety; never let it stall the fill
                log.warning("fills lookup failed for %s: %s", o["order_id"], exc)
            if cost_now is None and res.avg_price is not None:
                cost_now = res.avg_price * res.filled
            if cost_now is not None:
                px = (cost_now - o["cost"]) / new
                o["cost"] = cost_now
            else:
                px = o["price"]
                o["cost"] += px * new
            if not (0 < px < 1):
                px = o["price"]  # a nonsense ledger value must not become the cost basis
            fee_new = max(0.0, res.fees - o["fees"])
            o["fees"] = res.fees
            o["filled"] = res.filled
            self.last_fill_ts = now
            if o["action"] == "buy":
                if eng.position is None:
                    eng.open_position(o["side"], px * new, px, f"auto {o['reason']}", o["target"], entry_fee=fee_new)
                    self.say(f"FILLED: bought {new:.2f} {o['side']} @ {cents(px)} (${px * new:.2f}, fee ${fee_new:.2f})", "fill")
                else:
                    eng.add_to_position(new, px, fee_new)
                    self.say(f"FILLED more: +{new:.2f} {o['side']} @ {cents(px)}", "fill")
            else:
                out = eng.reduce_position(new, px, fee_new)
                self.say(f"FILLED: sold {new:.2f} {o['side']} @ {cents(px)} → ${px * new - fee_new:.2f}"
                         + (f" · position closed {out['pnl']:+.2f}" if out.get("closed") else f" · {out['qty_left']:.2f} left"), "fill")
            self.window_bought = self.store.bought_since(self._day_start(), o["ticker"], self.mode) if o["action"] == "buy" else self.window_bought
        status = res.status
        done = res.done or (o["cancel_sent"] and status in ("canceled", "cancelled", "expired", "executed"))
        if o["action"] == "sell" and status not in ("resting", "pending"):
            done = True
        if o["cancel_sent"] and now - o["placed"] > o["ttl"] + 15:
            done = True  # the exchange never confirmed the cancel; stop waiting (its expiration time has passed too)
        self.store.update_order(o["row"], status, o["filled"], (o["cost"] / o["filled"]) if o["filled"] > 0 else None, o["fees"])
        if done:
            if o["filled"] <= 1e-9:
                self.say(f"{o['action'].upper()} {o['side']} not filled ({status})")
                self.cooldown_until = now + (3.0 if o["action"] == "buy" else 0.0)
            self.order = None
            self.synced_ts = 0.0  # re-read the account on the next tick

    # ------------------------------------------------------------------ state for the UI
    def as_dict(self, now: float) -> dict:
        o = self.order
        kp = self.kalshi_pos
        return {
            "mode": self.mode, "paused": self.paused, "pause_reason": self.pause_reason, "blocked": self.blocked,
            "balance": self.balance, "sync_age_s": None if not self.synced_ts else round(now - self.synced_ts, 1),
            "sync_error": self.sync_error, "errors": self.errors, "api": getattr(self.broker, "order_api", ""),
            "kalshi_position": None if kp is None or kp.qty <= 0 else {"side": kp.side, "qty": round(kp.qty, 2), "exposure": round(kp.exposure, 2)},
            "order": None if o is None else {"action": o["action"], "side": o["side"], "price": o["price"], "count": round(o["count"], 2),
                                             "amount": round(o["amount"], 2), "filled": round(o["filled"], 2), "age_s": round(now - o["placed"], 1),
                                             "reason": o["reason"]},
            "day": {"bought": round(self.day_bought, 2), "pnl": self.day_pnl, "losses_row": self.losses_row,
                    "max_dollars": self.a.max_day_dollars, "max_loss": self.a.max_day_loss},
            "window_bought": round(self.window_bought, 2),
            "limits": {"order": self.a.max_order_dollars, "window": self.a.max_window_dollars, "day": self.a.max_day_dollars,
                       "day_loss": self.a.max_day_loss, "losses_row": self.a.max_losses_in_a_row, "take_plans": self.a.take_plans,
                       "take_quick": self.a.take_quick},
            "last_text": self.last_text, "last_ts": self.last_ts,
            "orders": self.store.orders(8),
        }
