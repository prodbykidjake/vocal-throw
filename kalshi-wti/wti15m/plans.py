"""Committed trade plans.

The per-second analysis is noisy. A scalper wants one call: side, the most to pay, how many dollars, where to
sell, and for that call to stay on screen until it is filled, missed or dead. PlanTracker turns the stream of
entry signals into exactly that:

  ANALYZING --(setup persists confirm_s)--> PLAN --> FILLED   (you reported a buy)
                                                 --> MISSED   (ask ran miss_margin past the limit for miss_seconds)
                                                 --> CANCELLED (thesis broke: model collapsed, market disagrees)
                                                 --> EXPIRED  (window over)
  MISSED/CANCELLED --> cooldown_s --> ANALYZING
Within min_hold_s only a hard invalidation can cancel a plan.

The sell target comes from simulated price paths (see paths.py): the model's fair exit, lowered to the price
that has at least `plan_min_chance` of being reached before the close. Cheap contracts below the lowest
confidence tier still become plans as 'longshots' when the edge after fees is there (a 32% chance at 14¢).
"""
from __future__ import annotations

import time
from dataclasses import dataclass

from .config import TradingCfg
from .decision import DecisionEngine, Quotes, Signal, cents, money, p_for_side, pct5
from .fees import FeeSchedule, taker_fee
from .model import Prediction, clamp
from .sizing import dollars_for


@dataclass
class Plan:
    ticker: str
    side: str
    limit: float  # the most to pay per share
    amount: float  # dollars
    shares: float
    target: float  # sell here (low end of the zone once filled)
    p_at_plan: float
    tier: str
    created_ts: float
    strike: float | None = None
    status: str = "open"  # open | filled | missed | cancelled | expired
    status_text: str = ""
    ended_ts: float | None = None
    expected_profit: float = 0.0
    id: int | None = None
    hit_target: bool | None = None
    best_bid: float | None = None
    target_high: float | None = None  # high end of the zone once filled
    p_target: float | None = None  # simulated chance the price reaches the target before the close

    @property
    def age_s(self) -> float:
        return time.time() - self.created_ts

    def as_dict(self) -> dict:
        return {"id": self.id, "ticker": self.ticker, "side": self.side, "limit": self.limit, "amount": self.amount,
                "shares": round(self.shares, 2), "target": self.target, "p_at_plan": round(self.p_at_plan, 4),
                "tier": self.tier, "created_ts": self.created_ts, "status": self.status, "status_text": self.status_text,
                "ended_ts": self.ended_ts, "expected_profit": round(self.expected_profit, 2), "strike": self.strike,
                "hit_target": self.hit_target, "best_bid": self.best_bid, "target_high": self.target_high,
                "p_target": None if self.p_target is None else round(self.p_target, 3)}


@dataclass
class PlanEvent:
    kind: str  # created | missed | cancelled | expired | filled
    plan: Plan
    text: str


def scalp_target(p_side: float, limit: float, fee_pc: float, cfg: TradingCfg) -> float:
    """Where the market would have caught up to the model, less the fee and half the edge buffer."""
    return clamp(min(0.95, p_side - fee_pc - cfg.edge_min / 2), limit + 0.005, 0.95)


def hard_stop(p_at_plan: float, cfg: TradingCfg) -> float:
    """The model chance below which a plan is dead: the configured stop, or 60% of what it was at plan time
    for longshots (a 32% call is not broken at 34%)."""
    return min(cfg.stop_prob, 0.6 * p_at_plan)


class PlanTracker:
    def __init__(self, cfg: TradingCfg, fees: FeeSchedule | None = None, recent_amounts_fn=None,
                 decider: DecisionEngine | None = None):
        self.cfg = cfg
        self.fees = fees or FeeSchedule()
        self.recent_amounts_fn = recent_amounts_fn or (lambda: [])
        self.decider = decider or DecisionEngine(cfg, self.fees)
        self.plan: Plan | None = None
        self.last: Plan | None = None  # most recent ended plan (for the cooldown message)
        self.cooldown_until = 0.0
        self._candidate: tuple[str, float] | None = None  # (side, first seen ts)
        self._miss_since: float | None = None
        self._ticker: str | None = None
        self.no_plan_reason: str | None = None  # why a confirmed setup did not become a plan (shown on the card)

    # ------------------------------------------------------------------ helpers
    def _end(self, now: float, status: str, text: str) -> PlanEvent:
        plan = self.plan
        plan.status, plan.status_text, plan.ended_ts = status, text, now
        self.plan = None
        self.last = plan
        self._miss_since = None
        self._candidate = None
        if status in ("missed", "cancelled"):
            self.cooldown_until = now + self.cfg.cooldown_s
        return PlanEvent(status, plan, text)

    def mark_filled(self, now: float | None = None) -> PlanEvent | None:
        if self.plan is None:
            return None
        return self._end(now or time.time(), "filled", "you bought it")

    def _build(self, now: float, ticker: str, side: str, pred: Prediction, quotes: Quotes, price: float | None,
               strike, seconds_left: float | None) -> Plan | None:
        cfg = self.cfg
        ask = quotes.ask(side)
        if ask is None or ask <= 0 or ask >= 0.97:
            self.no_plan_reason = "no usable ask"
            return None
        p = p_for_side(pred, side)
        fee1 = taker_fee(ask, 1, self.fees)
        sized = dollars_for(p, cfg, self.recent_amounts_fn(), ask=ask, fee=fee1)
        if sized is None:
            self.no_plan_reason = f"{side} {round(p * 100)}% at {cents(ask)} is not enough edge after fees"
            return None
        amount, tier = sized
        limit = round(ask + 0.005, 3)
        shares = amount / limit
        fee_in = taker_fee(limit, shares, self.fees)
        fee_pc = fee_in / shares if shares > 0 else 0.0
        target = round(scalp_target(p, limit, fee_pc, cfg), 3)
        breakeven = (limit * shares + fee_in + taker_fee(target, shares, self.fees)) / shares if shares > 0 else limit
        p_target = None
        target_high = None
        curve = self.decider.curve_for(pred, side, price if price is not None else strike, strike, seconds_left, quotes)
        if curve is not None:
            p_target = curve.touch(target)
            if p_target < cfg.plan_min_chance:
                # the model's fair exit is a stretch from here: aim where the paths actually get to
                target = round(min(target, max(curve.level(max(cfg.plan_min_chance, 0.4)), breakeven + 0.02)), 3)
                p_target = curve.touch(target)
            target_high = round(min(0.95, max(curve.level(cfg.zone_high_prob), target + 0.05)), 2)
        else:
            target_high = round(min(0.95, target + 0.10), 2)
        min_scalp = max(cfg.min_scalp_cents / 100.0, 0.3 * limit)
        if target - limit < min_scalp:
            self.no_plan_reason = (f"{side} at {cents(ask)} has edge but not enough room to scalp "
                                   f"(sell target {cents(target)} is under {cents(limit + min_scalp)})")
            return None
        if p_target is not None and p_target < cfg.plan_min_chance:
            self.no_plan_reason = f"{side} at {cents(ask)}: only ~{pct5(p_target)} chance of reaching {cents(target)}"
            return None
        self.no_plan_reason = None
        profit = shares * (target - limit) - fee_in - taker_fee(target, shares, self.fees)
        return Plan(ticker, side, limit, amount, shares, target, p, tier, now, strike, expected_profit=profit,
                    target_high=target_high, p_target=p_target)

    # ------------------------------------------------------------------ per-second update
    def update(self, now: float, ticker: str | None, pred: Prediction | None, quotes: Quotes, price: float | None,
               strike, seconds_left: float | None, entry_signal: Signal | None, position_open: bool) -> PlanEvent | None:
        if ticker != self._ticker:
            self._ticker = ticker
            self._candidate = None
            self._miss_since = None
            self.no_plan_reason = None
            if self.plan is not None:
                return self._end(now, "expired", "window changed")
        if self.plan is not None:
            return self._check_open(now, pred, quotes, price, seconds_left)
        if position_open or pred is None or entry_signal is None or ticker is None:
            self._candidate = None
            self.no_plan_reason = None
            return None
        if now < self.cooldown_until:
            self._candidate = None
            return None
        if entry_signal.action != "BUY" or entry_signal.side is None:
            self._candidate = None
            self.no_plan_reason = None
            return None
        side = entry_signal.side
        if self._candidate is None or self._candidate[0] != side:
            self._candidate = (side, now)
            self.no_plan_reason = None
            return None
        if now - self._candidate[1] < self.cfg.confirm_s:
            return None
        plan = self._build(now, ticker, side, pred, quotes, price, strike, seconds_left)
        if plan is None:
            return None
        self.plan = plan
        self._candidate = None
        plan.status_text = f"valid: ask {cents(quotes.ask(side))} ≤ {cents(plan.limit)} · buy now"
        return PlanEvent("created", plan, self.headline(plan))

    def _check_open(self, now: float, pred: Prediction | None, quotes: Quotes, price, seconds_left) -> PlanEvent | None:
        plan = self.plan
        if seconds_left is not None and seconds_left <= 0:
            return self._end(now, "expired", "window closed")
        ask, bid = quotes.ask(plan.side), quotes.bid(plan.side)
        if bid is not None:
            plan.best_bid = bid if plan.best_bid is None else max(plan.best_bid, bid)
            if bid >= plan.target:
                plan.hit_target = True
        if pred is not None:
            p = p_for_side(pred, plan.side)
            if p <= hard_stop(plan.p_at_plan, self.cfg) or pred.disagree:
                return self._end(now, "cancelled", f"thesis broke: {plan.side} is {round(p * 100)}% by the model now")
            if plan.age_s >= self.cfg.min_hold_s and p <= plan.p_at_plan - 0.20:
                return self._end(now, "cancelled", f"model cooled off ({round(plan.p_at_plan * 100)}% → {round(p * 100)}%)")
        if ask is None:
            plan.status_text = "no fresh quotes; plan stands"
            return None
        margin = self.cfg.miss_margin_cents / 100.0
        if ask > plan.limit + margin:
            if self._miss_since is None:
                self._miss_since = now
            held = now - self._miss_since
            if held >= self.cfg.miss_seconds:
                return self._end(now, "missed", f"missed: ask ran to {cents(ask)}, {cents(ask - plan.limit)} past the limit")
            plan.status_text = f"ask {cents(ask)} is past the {cents(plan.limit)} limit ({int(self.cfg.miss_seconds - held)} s before it counts as missed)"
            return None
        self._miss_since = None
        if ask <= plan.limit:
            plan.status_text = f"valid: ask {cents(ask)} ≤ {cents(plan.limit)} · buy now"
        else:
            plan.status_text = f"ask {cents(ask)} is a hair above the {cents(plan.limit)} limit; still fine"
        return None

    # ------------------------------------------------------------------ text
    def headline(self, plan: Plan) -> str:
        chance = f" (~{pct5(plan.p_target)} chance)" if plan.p_target is not None else ""
        return (f"BUY {plan.side} · up to {cents(plan.limit)} · ${plan.amount:.0f} (≈ {plan.shares:.0f} shares) · "
                f"sell at {cents(plan.target)}{chance} · ≈ {money(plan.expected_profit)} if it gets there")

    def display(self, entry_signal: Signal | None, pred: Prediction | None, now: float) -> Signal | None:
        """What the card shows: the open plan, the cooldown, a forming setup, or the plain analysis."""
        if self.plan is not None:
            plan = self.plan
            base = entry_signal.details[:2] if entry_signal else []
            zone = f"{cents(plan.target)}–{cents(plan.target_high)}" if plan.target_high else cents(plan.target)
            details = base + [
                f"Plan since {time.strftime('%H:%M:%S', time.localtime(plan.created_ts))} · {plan.tier} ({round(plan.p_at_plan * 100)}% at the time) · {plan.status_text}",
                f"If filled: sell zone {zone} for ≈ {money(plan.expected_profit)} at the low end; it will say missed if the ask runs "
                f"{self.cfg.miss_margin_cents:.0f}¢ past the limit for {self.cfg.miss_seconds:.0f} s, or cancelled if the thesis breaks.",
            ]
            return Signal("BUY", plan.side, plan.limit, plan.shares, None, plan.p_at_plan, self.headline(plan), details,
                          ["plan"], pred.confidence if pred else "",
                          {"amount": plan.amount, "target": plan.target, "target_high": plan.target_high})
        if now < self.cooldown_until and self.last is not None:
            left = int(self.cooldown_until - now)
            details = (entry_signal.details[:2] if entry_signal else []) + [
                f"Last plan ({self.last.side} up to {cents(self.last.limit)}) ended: {self.last.status_text}."]
            return Signal("WAIT", self.last.side, None, 0, None, None,
                          f"WAIT · {self.last.status} · analyzing again in {left} s", details, [self.last.status],
                          pred.confidence if pred else "")
        if entry_signal is not None and entry_signal.action == "BUY":
            since = self._candidate[1] if self._candidate else now
            held = max(0.0, now - since)
            if self.no_plan_reason and held >= self.cfg.confirm_s:
                return Signal("WAIT", entry_signal.side, None, 0, entry_signal.edge, entry_signal.p_side,
                              f"WAIT · {self.no_plan_reason}", entry_signal.details[:3], ["no_plan"],
                              entry_signal.confidence, entry_signal.triggers)
            return Signal("WAIT", entry_signal.side, None, 0, entry_signal.edge, entry_signal.p_side,
                          f"WAIT · setup forming: {entry_signal.side} near {cents(entry_signal.price)} (confirming {min(held, self.cfg.confirm_s):.0f}/{self.cfg.confirm_s:.0f} s)",
                          entry_signal.details[:3], ["forming"], entry_signal.confidence, entry_signal.triggers)
        return entry_signal
