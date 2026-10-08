"""Turns a probability into coaching.

Entries: BUY (side, limit, size), or WAIT with the reason and what would change the answer.
Open positions (scalp-first): SELL NOW, or HOLD with a concrete "SELL AT x¢" target, plus the live
cash-out value and P&L the way Kalshi's sell sheet shows them.
All contract prices are dollars (0..1); Kalshi quotes sub-cent prices (0.013), so display keeps one decimal.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from statistics import NormalDist

from .config import TradingCfg
from .fees import FeeSchedule, fee_per_contract, taker_fee
from .model import Prediction, clamp

_ND = NormalDist()


def cents(x: float | None) -> str:
    if x is None:
        return "--"
    c = x * 100
    return f"{c:.1f}¢" if abs(c - round(c)) > 0.05 else f"{round(c):d}¢"


def pct(p: float | None) -> str:
    return "--" if p is None else f"{round(p * 100):d}%"


def money(x: float | None) -> str:
    if x is None:
        return "--"
    sign = "-" if x < 0 else "+"
    return f"{sign}${abs(x):.2f}"


def mmss(seconds: float | None) -> str:
    if seconds is None:
        return "--:--"
    s = max(0, int(seconds))
    return f"{s // 60}:{s % 60:02d}"


def qty_text(q: float) -> str:
    return f"{q:.2f}".rstrip("0").rstrip(".") if q != int(q) else f"{int(q)}"


@dataclass
class Quotes:
    yes_bid: float | None = None
    yes_ask: float | None = None
    no_bid: float | None = None
    no_ask: float | None = None

    @classmethod
    def from_market(cls, m) -> "Quotes":
        return cls(m.yes_bid, m.yes_ask, m.no_bid, m.no_ask)

    @property
    def spread(self) -> float | None:
        if self.yes_bid is None or self.yes_ask is None:
            return None
        return round(self.yes_ask - self.yes_bid, 4)

    def ask(self, side: str) -> float | None:
        return self.yes_ask if side == "UP" else self.no_ask

    def bid(self, side: str) -> float | None:
        return self.yes_bid if side == "UP" else self.no_bid


@dataclass
class Position:
    ticker: str
    side: str  # UP | DOWN
    qty: float  # Kalshi sells fractional shares when you buy by dollar amount
    avg_price: float
    opened_ts: float
    id: int | None = None
    amount: float | None = None  # dollars paid (what you typed into Kalshi)
    entry_fee: float = 0.0
    high_bid: float | None = None  # best bid for this side since entry

    @property
    def cost(self) -> float:
        return self.amount if self.amount is not None else self.qty * self.avg_price + self.entry_fee

    def as_dict(self) -> dict:
        return {"id": self.id, "ticker": self.ticker, "side": self.side, "qty": round(self.qty, 4),
                "avg_price": self.avg_price, "opened_ts": self.opened_ts, "amount": self.amount,
                "entry_fee": self.entry_fee, "cost": round(self.cost, 2), "high_bid": self.high_bid}


@dataclass
class Signal:
    action: str  # BUY | SELL | HOLD | WAIT
    side: str | None = None
    price: float | None = None
    size: int = 0
    edge: float | None = None
    p_side: float | None = None
    headline: str = ""
    details: list[str] = field(default_factory=list)
    reasons: list[str] = field(default_factory=list)
    confidence: str = ""
    triggers: dict = field(default_factory=dict)
    scalp: dict | None = None  # live position box: action SELL NOW / SELL AT / HOLD, target, cash_out, pnl

    @property
    def key(self) -> str:
        """Identity used to decide whether a signal is 'new' (for notifications)."""
        return f"{self.action}:{self.side or ''}:{','.join(self.reasons[:1])}"

    def as_dict(self) -> dict:
        return {"action": self.action, "side": self.side, "price": self.price, "size": self.size,
                "edge": None if self.edge is None else round(self.edge, 4),
                "p_side": None if self.p_side is None else round(self.p_side, 4),
                "headline": self.headline, "details": self.details, "reasons": self.reasons,
                "confidence": self.confidence, "triggers": self.triggers, "scalp": self.scalp}


def p_for_side(pred: Prediction, side: str) -> float:
    return pred.p_final if side == "UP" else pred.p_down


def kelly_fraction(p: float, price: float) -> float:
    """Full-Kelly fraction of bankroll for a binary paying $1 bought at `price` with win prob p."""
    if price <= 0 or price >= 1:
        return 0.0
    return max(0.0, (p - price) / (1.0 - price))


def price_for_probability(target_p: float, strike: float, sigma: float, tau_s: float, tie_adj: float) -> float | None:
    """Underlying price at which the base model would give P(Up) = target_p (sigma = the one the model used)."""
    if tau_s <= 0.5 or sigma <= 0:
        return None
    target_p = clamp(target_p, 0.001, 0.999)
    return strike - tie_adj + _ND.inv_cdf(target_p) * sigma * math.sqrt(tau_s)


def cash_out(qty: float, bid: float, fees: FeeSchedule | None) -> float:
    """What selling `qty` shares at `bid` nets after Kalshi's taker fee."""
    return qty * bid - taker_fee(bid, qty, fees)


class DecisionEngine:
    def __init__(self, cfg: TradingCfg, fees: FeeSchedule | None = None):
        self.cfg = cfg
        self.fees = fees or FeeSchedule()

    # ------------------------------------------------------------------ helpers
    def _size(self, p: float, ask: float) -> int:
        fee1 = fee_per_contract(ask, 1, self.fees)
        f = kelly_fraction(p - fee1, ask) * self.cfg.kelly_fraction
        stake = min(f * self.cfg.bankroll, self.cfg.max_stake_fraction * self.cfg.bankroll)
        n = int(stake // ask) if ask > 0 else 0
        return max(1, min(n, self.cfg.max_contracts))

    def _edge(self, p: float, ask: float, size: float) -> tuple[float, float]:
        fee = fee_per_contract(ask, size, self.fees)
        return p - ask - fee, fee

    def _take_profit(self, entry: float) -> float:
        return round(min(entry + self.cfg.profit_target, 0.99), 3)

    def _exit_plan(self, side: str, entry: float) -> str:
        return (f"Exit plan: sell {side} around {cents(self._take_profit(entry))} if the edge is gone; "
                f"cut if the model's {side} chance drops under {pct(self.cfg.stop_prob)} and the bid is still worth it; "
                f"hold to settlement if it is ≥ {pct(self.cfg.hold_to_settle_prob)} with under 2 min left.")

    def _situation(self, pred: Prediction, price: float, strike: float | None, seconds_left: float | None) -> str:
        if strike is None:
            return f"Price ${price:.2f}; target not published yet."
        gap = price - strike
        rel = "above" if gap > 0 else "below" if gap < 0 else "at"
        move = pred.expected_move
        return (f"Price ${price:.2f} is {abs(gap) * 100:.0f}¢ {rel} the ${strike:.2f} target with {mmss(seconds_left)} of trading left; "
                f"typical move until the settlement candle closes ≈ ±${move:.2f} (z = {pred.z:+.2f}).")

    # ------------------------------------------------------------------ main entry
    def decide(self, pred: Prediction, quotes: Quotes, price: float, strike: float | None, seconds_left: float | None,
               seconds_elapsed: float | None, position: Position | None = None) -> Signal:
        if position is not None:
            return self._manage_position(pred, quotes, price, strike, seconds_left, position)
        return self._entry(pred, quotes, price, strike, seconds_left, seconds_elapsed)

    # ------------------------------------------------------------------ entries
    def _entry(self, pred: Prediction, quotes: Quotes, price: float, strike: float | None, seconds_left: float | None,
               seconds_elapsed: float | None) -> Signal:
        cfg = self.cfg
        lean_side = "UP" if pred.p_final >= 0.5 else "DOWN"
        lean_p = p_for_side(pred, lean_side)
        details = [self._situation(pred, price, strike, seconds_left)]
        model_line = (f"Model: {pct(pred.p_final)} Up / {pct(pred.p_down)} Down"
                      + (f" · market {pct(pred.p_market)} Up" if pred.p_market is not None else "")
                      + f" · confidence: {pred.confidence}")
        details.append(model_line)
        for note in pred.notes:
            if not note.startswith("warming up"):
                details.append(note)

        def wait(code: str, headline: str, extra: list[str] | None = None, triggers: dict | None = None) -> Signal:
            return Signal("WAIT", lean_side, None, 0, None, lean_p, headline, details + (extra or []), [code],
                          pred.confidence, triggers or {})

        if seconds_left is None:
            return wait("no_market", "WAIT · no live window right now")
        if seconds_left <= 0:
            return wait("window_closed", "WAIT · window closed, waiting for settlement and the next window")
        if pred.confidence == "no_target" or strike is None:
            return wait("no_target", "WAIT · target price not published yet")
        if pred.confidence == "warming_up":
            return wait("warming_up", "WAIT · warming up (need more price history to size the odds)", pred.notes)
        if pred.confidence == "stale":
            return wait("feed_stale", "WAIT · price feed is stale, not trading blind")
        if quotes.yes_ask is None or quotes.no_ask is None:
            return wait("no_quotes", "WAIT · no fresh Kalshi quotes")
        if seconds_elapsed is not None and seconds_elapsed < 0:
            return wait("not_open_yet", f"WAIT · next window opens in {mmss(-seconds_elapsed)}")
        if seconds_elapsed is not None and seconds_elapsed < cfg.no_entry_first_s:
            return wait("window_just_opened",
                        f"WAIT · window just opened ({mmss(seconds_elapsed)} in); let the target and odds settle",
                        [f"Entries open after {int(cfg.no_entry_first_s)} s."])
        spread = quotes.spread
        if spread is not None and spread > cfg.max_spread:
            return wait("spread_wide", f"WAIT · spread too wide ({cents(spread)}) to pay for an entry")

        # Evaluate both sides.
        cands: dict[str, dict] = {}
        for side in ("UP", "DOWN"):
            p = p_for_side(pred, side)
            ask = quotes.ask(side)
            if ask is None or ask <= 0 or ask >= 1:
                continue
            size = self._size(p, ask)
            edge, fee = self._edge(p, ask, size)
            cands[side] = {"side": side, "p": p, "ask": ask, "size": size, "edge": edge, "fee": fee}
        if not cands:
            return wait("no_quotes", "WAIT · no usable quotes")
        best = max(cands.values(), key=lambda c: c["edge"])
        # When nothing clears the bar, explain the side the model actually leans to (not a longshot's "less negative" edge).
        shown = best if best["edge"] >= cfg.edge_min else cands.get(lean_side, best)
        side, p, ask, size, edge, fee = (shown[k] for k in ("side", "p", "ask", "size", "edge", "fee"))
        sigma_used = pred.sigma_eff or pred.sigma
        max_ask = round(max(0.0, p - cfg.edge_min - fee), 3)
        triggers: dict = {"side": side, "max_ask": max_ask, "current_ask": ask}
        needed_p = ask + cfg.edge_min + fee
        if side == "UP":
            trig_price = price_for_probability(needed_p, strike, sigma_used, pred.tau_s, cfg.tie_adj)
        else:
            trig_price = price_for_probability(1 - needed_p, strike, sigma_used, pred.tau_s, cfg.tie_adj)
        if trig_price is not None:
            triggers["price_needed"] = round(trig_price, 2)
        details.append(f"Best side: {side} at {cents(ask)} ask → model {pct(p)} − price − ~{cents(fee)} fee = "
                       f"edge {edge * 100:+.1f}¢ per contract (need {cfg.edge_min * 100:+.0f}¢).")
        what_for = ""
        if max_ask > 0:
            what_for = f"Would buy {side} if its ask were ≤ {cents(max_ask)} (now {cents(ask)})"
            if trig_price is not None and 0 < trig_price < 10_000:
                what_for += f", or if WTI moved to about ${trig_price:.2f} with the odds unchanged"
            what_for += "."
        elif trig_price is not None and 0 < trig_price < 10_000:
            what_for = f"Would consider {side} only if WTI moved to about ${trig_price:.2f}."
        extra = [what_for] if what_for else []

        if seconds_left < cfg.late_entry_s and abs(pred.z) < cfg.late_entry_min_z:
            return wait("too_late", f"WAIT · under {int(cfg.late_entry_s)} s left and the result is not lopsided enough",
                        extra, triggers)
        if best["edge"] < cfg.edge_min:
            if abs(pred.z) < 0.5:
                return wait("coin_flip", "WAIT · coin flip: the gap to target is inside normal noise", extra, triggers)
            if edge > 0:
                return wait("edge_too_small", f"WAIT · lean {side} {pct(p)}, but edge after fees is only {edge * 100:+.1f}¢",
                            extra, triggers)
            return wait("priced_in", f"WAIT · market already prices {side} at {cents(ask)} (model {pct(p)}); no edge",
                        extra, triggers)
        if pred.confidence == "coinflip":
            return wait("not_confident", "WAIT · edge exists on paper but the model is not confident", extra, triggers)

        stake = size * ask
        headline = f"BUY {side} · limit {cents(ask)} · {size} contract{'s' if size != 1 else ''} (${stake:.2f})"
        details.append(f"Size: ¼-Kelly on a ${cfg.bankroll:.0f} bankroll → {size} contracts; max loss ${stake:.2f}, "
                       f"win pays ${size * (1 - ask):.2f} before fees.")
        details.append(self._exit_plan(side, ask))
        if self.fees.is_estimate:
            details.append("Fee is an estimate: this series uses a fee table the app cannot read.")
        return Signal("BUY", side, ask, size, edge, p, headline, details, ["edge"], pred.confidence, triggers)

    # ------------------------------------------------------------------ open position (scalp-first)
    def _manage_position(self, pred: Prediction, quotes: Quotes, price: float, strike: float | None,
                         seconds_left: float | None, pos: Position) -> Signal:
        cfg = self.cfg
        side = pos.side
        p = p_for_side(pred, side)
        bid = quotes.bid(side)
        qty = pos.qty
        details = [self._situation(pred, price, strike, seconds_left),
                   f"You hold {qty_text(qty)} {side} @ {cents(pos.avg_price)} (${pos.cost:.2f} in). Model gives {side} {pct(p)}"
                   + (f"; market bid {cents(bid)}" if bid is not None else "") + "."]
        for note in pred.notes:
            if not note.startswith("warming up"):
                details.append(note)
        tp = self._take_profit(pos.avg_price)
        triggers = {"stop_prob": cfg.stop_prob, "take_profit_bid": tp}

        def hold(code: str, headline: str, scalp: dict | None, edge: float | None = None) -> Signal:
            return Signal("HOLD", side, None, 0, edge, p, headline, details, [code], pred.confidence, triggers, scalp)

        if seconds_left is not None and seconds_left <= 0:
            return hold("window_closed", "HOLD · window closed, waiting for settlement",
                        {"action": "HOLD", "target": None, "bid": bid, "cash_out": None, "pnl": None, "reason": "window_closed"})
        if bid is None:
            return hold("no_quotes", "HOLD · no bid to sell into right now",
                        {"action": "HOLD", "target": None, "bid": None, "cash_out": None, "pnl": None, "reason": "no_quotes"})

        cash = cash_out(qty, bid, self.fees)
        pnl = cash - pos.cost
        hold_val = p  # per share, expected settlement value
        sell_val = cash / qty if qty > 0 else bid  # per share, after fee
        fee_pc = bid - sell_val
        ev_hold_total = p * qty
        details.append(f"Cash out now ≈ ${cash:.2f} ({money(pnl)}); by the model, holding is worth ≈ ${ev_hold_total:.2f} "
                       f"({cents(hold_val)} vs {cents(sell_val)} per share after fee).")
        if pos.avg_price <= 0.10:
            details.append(f"Longshot: bought at {cents(pos.avg_price)}. It only pays if WTI crosses the target; "
                           f"otherwise it decays toward 0 as the clock runs. Set a hard sell target and respect it.")

        def scalp(action: str, target: float | None, reason: str) -> dict:
            return {"action": action, "target": None if target is None else round(target, 3), "bid": bid,
                    "cash_out": round(cash, 2), "pnl": round(pnl, 2), "reason": reason, "p_side": round(p, 4)}

        def sell(code: str, headline: str) -> Signal:
            return Signal("SELL", side, bid, 0, sell_val - hold_val, p, headline, details, [code], pred.confidence,
                          triggers, scalp("SELL NOW", bid, code))

        if pred.confidence in ("stale", "warming_up"):
            # no model to judge by: give a price-action target only
            target = max(tp, bid + 0.005)
            details.append("Model can't judge right now (" + pred.confidence + "); target is price-based only.")
            return hold(pred.confidence, f"HOLD {side} · set a sell at {cents(target)} (model unavailable)",
                        scalp("SELL AT", target, pred.confidence))

        # 1. market pays more than the model thinks it is worth
        if sell_val >= hold_val + 0.03:
            return sell("overpriced", f"SELL NOW {side} at {cents(bid)} · market pays {cents(bid)} for something worth ≈ {cents(hold_val)}")
        # 2. nearly settled and strongly in your favour: don't pay to exit
        if seconds_left is not None and seconds_left <= 120 and p >= cfg.hold_to_settle_prob and hold_val >= sell_val:
            return hold("ride_to_settle", f"HOLD to settlement · {side} {pct(p)} with {mmss(seconds_left)} left; selling would give up "
                        f"{cents(hold_val - sell_val)} per share", scalp("HOLD", None, "ride_to_settle"), hold_val - sell_val)
        # 3. in profit and the bid rolled over hard from its high since entry
        if pos.high_bid and pnl > 0 and bid > pos.avg_price and bid <= pos.high_bid * 0.75:
            return sell("rollover", f"SELL NOW {side} at {cents(bid)} · lock in {money(pnl)}; bid rolled over from {cents(pos.high_bid)}")
        # 4. take-profit target reached and the edge is gone
        if bid >= tp and (p - bid - fee_pc) < cfg.edge_min / 2:
            return sell("take_profit", f"SELL NOW {side} at {cents(bid)} · target hit, {money(pnl)}, edge is gone")
        # 5. model flipped against you (only if selling still gets you something close to its value)
        if p <= cfg.stop_prob:
            if sell_val >= hold_val - cfg.edge_min or pred.disagree:
                return sell("stop", f"SELL NOW {side} at {cents(bid)} · model flipped against you ({side} {pct(p)} ≤ {pct(cfg.stop_prob)})"
                            + (" and the market agrees it's gone" if pred.disagree else ""))
            details.append(f"Model has flipped ({side} {pct(p)}), but the bid ({cents(bid)}) is far below even that value; "
                           f"selling would lock in more loss than holding is worth.")
            return hold("too_late_to_cut", f"HOLD {side} · too late to cut; ride it as a {pct(p)} lottery ticket",
                        scalp("HOLD", None, "too_late_to_cut"), hold_val - sell_val)
        # 6. otherwise: a concrete sell target where the market would have caught up to the model
        target = min(tp, hold_val - fee_pc - cfg.edge_min / 2)
        if pred.disagree:
            # the model and the market disagree a lot (feed probably off): don't promise a target far from the book
            target = min(target, bid + max(0.02, bid * 0.5))
            details.append("Target capped near the current bid because the model and the market disagree a lot right now.")
        target = clamp(target, bid + 0.005, 0.99)
        if target <= bid + 0.006:
            return sell("fair_exit", f"SELL NOW {side} at {cents(bid)} · bid is already at the model's fair exit ({money(pnl)})")
        gap_txt = "the market agrees with the model" if abs(sell_val - hold_val) <= 0.03 else \
            f"the bid ({cents(bid)}) is below the model's value ({cents(hold_val)})"
        details.append(f"Will say SELL NOW at ≈ {cents(target)}, if the bid rolls over hard while you're in profit, "
                       f"or if {side} drops under {pct(cfg.stop_prob)} with a bid still worth taking.")
        return hold("hold", f"HOLD {side} · set a sell at {cents(target)} · now {cents(bid)}, cash out {money(pnl)} · {gap_txt}",
                    scalp("SELL AT", target, "hold"), hold_val - sell_val)
