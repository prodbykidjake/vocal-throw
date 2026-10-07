"""Turns a probability into coaching: BUY (side, limit, size), SELL, HOLD, or WAIT with the reason and
what would change the answer. All prices are contract prices in dollars (0..1)."""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from statistics import NormalDist

from .config import TradingCfg
from .fees import FeeSchedule, fee_per_contract
from .model import Prediction, clamp

_ND = NormalDist()


def cents(x: float | None) -> str:
    return "--" if x is None else f"{round(x * 100):d}¢"


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
    qty: int
    avg_price: float
    opened_ts: float
    id: int | None = None

    def as_dict(self) -> dict:
        return {"id": self.id, "ticker": self.ticker, "side": self.side, "qty": self.qty,
                "avg_price": self.avg_price, "opened_ts": self.opened_ts}


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

    @property
    def key(self) -> str:
        """Identity used to decide whether a signal is 'new' (for notifications)."""
        return f"{self.action}:{self.side or ''}:{','.join(self.reasons[:1])}"

    def as_dict(self) -> dict:
        return {"action": self.action, "side": self.side, "price": self.price, "size": self.size,
                "edge": None if self.edge is None else round(self.edge, 4),
                "p_side": None if self.p_side is None else round(self.p_side, 4),
                "headline": self.headline, "details": self.details, "reasons": self.reasons,
                "confidence": self.confidence, "triggers": self.triggers}


def p_for_side(pred: Prediction, side: str) -> float:
    return pred.p_final if side == "UP" else pred.p_down


def kelly_fraction(p: float, price: float) -> float:
    """Full-Kelly fraction of bankroll for a binary paying $1 bought at `price` with win prob p."""
    if price <= 0 or price >= 1:
        return 0.0
    return max(0.0, (p - price) / (1.0 - price))


def price_for_probability(target_p: float, strike: float, sigma: float, tau_s: float, tie_adj: float) -> float | None:
    """Underlying price at which the base model would give P(Up) = target_p."""
    if tau_s <= 0.5 or sigma <= 0:
        return None
    target_p = clamp(target_p, 0.001, 0.999)
    return strike - tie_adj + _ND.inv_cdf(target_p) * sigma * math.sqrt(tau_s)


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

    def _edge(self, p: float, ask: float, size: int) -> tuple[float, float]:
        fee = fee_per_contract(ask, size, self.fees)
        return p - ask - fee, fee

    def _exit_plan(self, side: str, entry: float) -> str:
        return (f"Exit plan: sell {side} if its bid reaches {cents(entry + self.cfg.profit_target)} and the edge is gone; "
                f"cut if the model's {side} chance drops under {pct(self.cfg.stop_prob)}; "
                f"hold to settlement if it is ≥ {pct(self.cfg.hold_to_settle_prob)} with under 2 min left.")

    def _situation(self, pred: Prediction, price: float, strike: float | None, seconds_left: float | None) -> str:
        if strike is None:
            return f"Price ${price:.2f}; target not published yet."
        gap = price - strike
        rel = "above" if gap > 0 else "below" if gap < 0 else "at"
        move = pred.expected_move
        return (f"Price ${price:.2f} is {abs(gap) * 100:.0f}¢ {rel} the ${strike:.2f} target with {mmss(seconds_left)} left; "
                f"typical move over the time left ≈ ±${move:.2f} (z = {pred.z:+.2f}).")

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
            return wait("no_quotes", "WAIT · no Kalshi quotes yet")
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
        best: dict | None = None
        for side in ("UP", "DOWN"):
            p = p_for_side(pred, side)
            ask = quotes.ask(side)
            if ask is None or ask <= 0 or ask >= 1:
                continue
            size = self._size(p, ask)
            edge, fee = self._edge(p, ask, size)
            cand = {"side": side, "p": p, "ask": ask, "size": size, "edge": edge, "fee": fee}
            if best is None or edge > best["edge"]:
                best = cand
        if best is None:
            return wait("no_quotes", "WAIT · no usable quotes")

        side, p, ask, size, edge, fee = (best[k] for k in ("side", "p", "ask", "size", "edge", "fee"))
        # What would make this a trade: the ask we would need, and the price move we would need.
        max_ask = round(p - cfg.edge_min - fee, 2)
        triggers: dict = {"side": side, "max_ask": max_ask, "current_ask": ask}
        needed_p = ask + cfg.edge_min + fee
        if side == "UP":
            trig_price = price_for_probability(needed_p, strike, pred.sigma, seconds_left, cfg.tie_adj)
        else:
            trig_price = price_for_probability(1 - needed_p, strike, pred.sigma, seconds_left, cfg.tie_adj)
        if trig_price is not None:
            triggers["price_needed"] = round(trig_price, 2)
        edge_line = (f"Best side: {side} at {cents(ask)} ask → model {pct(p)} − price − ~{cents(fee)} fee = "
                     f"edge {edge * 100:+.1f}¢ per contract (need {cfg.edge_min * 100:+.0f}¢).")
        details.append(edge_line)
        what_for = f"Would buy {side} if its ask were ≤ {cents(max_ask)} (now {cents(ask)})"
        if trig_price is not None and 0 < trig_price < 10_000:
            what_for += f", or if WTI moved to about ${trig_price:.2f} with the odds unchanged"
        what_for += "."

        if seconds_left < cfg.late_entry_s and abs(pred.z) < cfg.late_entry_min_z:
            return wait("too_late", f"WAIT · under {int(cfg.late_entry_s)} s left and the result is not lopsided enough",
                        [what_for], triggers)
        if edge < cfg.edge_min:
            if abs(pred.z) < 0.5:
                return wait("coin_flip", "WAIT · coin flip: the gap to target is inside normal noise", [what_for], triggers)
            if edge > 0:
                return wait("edge_too_small", f"WAIT · lean {side} {pct(p)}, but edge after fees is only {edge * 100:+.1f}¢",
                            [what_for], triggers)
            return wait("priced_in", f"WAIT · market already prices {side} at {cents(ask)} (model {pct(p)}); no edge",
                        [what_for], triggers)
        if pred.confidence == "coinflip":
            return wait("not_confident", "WAIT · edge exists on paper but the model is not confident", [what_for], triggers)

        stake = size * ask
        headline = f"BUY {side} · limit {cents(ask)} · {size} contract{'s' if size != 1 else ''} (${stake:.2f})"
        details.append(f"Size: ¼-Kelly on a ${cfg.bankroll:.0f} bankroll → {size} contracts; max loss ${stake:.2f}, "
                       f"win pays ${size * (1 - ask):.2f} before fees.")
        details.append(self._exit_plan(side, ask))
        if self.fees.is_estimate:
            details.append("Fee is an estimate: this series uses a fee table the app cannot read.")
        return Signal("BUY", side, ask, size, edge, p, headline, details, ["edge"], pred.confidence, triggers)

    # ------------------------------------------------------------------ open position
    def _manage_position(self, pred: Prediction, quotes: Quotes, price: float, strike: float | None,
                         seconds_left: float | None, pos: Position) -> Signal:
        cfg = self.cfg
        side = pos.side
        p = p_for_side(pred, side)
        bid = quotes.bid(side)
        details = [self._situation(pred, price, strike, seconds_left),
                   f"You hold {pos.qty} {side} @ {cents(pos.avg_price)}. Model gives {side} {pct(p)}"
                   + (f"; market bid {cents(bid)}" if bid is not None else "") + "."]
        if seconds_left is not None and seconds_left <= 0:
            return Signal("HOLD", side, None, pos.qty, None, p, "HOLD · window closed, waiting for settlement",
                          details, ["window_closed"], pred.confidence)
        if bid is None:
            return Signal("HOLD", side, None, pos.qty, None, p, "HOLD · no bid to sell into right now", details,
                          ["no_quotes"], pred.confidence)
        fee_sell = fee_per_contract(bid, pos.qty, self.fees)
        ev_hold = p  # expected settlement value per contract
        ev_sell = bid - fee_sell
        unreal = (bid - pos.avg_price) * pos.qty
        details.append(f"Per contract: hold is worth ≈ {cents(ev_hold)} (model), selling now nets ≈ {cents(ev_sell)} "
                       f"after fee. Unrealized: {money(unreal)}.")
        triggers = {"stop_prob": cfg.stop_prob, "take_profit_bid": round(pos.avg_price + cfg.profit_target, 2)}

        if pred.confidence in ("stale", "warming_up"):
            return Signal("HOLD", side, None, pos.qty, None, p, "HOLD · model can't judge right now (" + pred.confidence + ")",
                          details, [pred.confidence], pred.confidence, triggers)
        if seconds_left is not None and seconds_left <= 120 and p >= cfg.hold_to_settle_prob:
            return Signal("HOLD", side, None, pos.qty, ev_hold - ev_sell, p,
                          f"HOLD to settlement · {side} {pct(p)} with {mmss(seconds_left)} left; selling would give up "
                          f"{cents(ev_hold - ev_sell)} per contract", details, ["ride_to_settle"], pred.confidence, triggers)
        if p <= cfg.stop_prob:
            return Signal("SELL", side, bid, pos.qty, ev_sell - ev_hold, p,
                          f"SELL {side} at {cents(bid)} · model flipped against you ({side} {pct(p)} ≤ {pct(cfg.stop_prob)})",
                          details, ["stop"], pred.confidence, triggers)
        if bid - pos.avg_price >= cfg.profit_target and (p - bid - fee_sell) < cfg.edge_min / 2:
            return Signal("SELL", side, bid, pos.qty, ev_sell - ev_hold, p,
                          f"SELL {side} at {cents(bid)} · take profit {money(unreal)}; the edge is gone",
                          details, ["take_profit"], pred.confidence, triggers)
        if ev_sell > ev_hold + 0.03:
            return Signal("SELL", side, bid, pos.qty, ev_sell - ev_hold, p,
                          f"SELL {side} at {cents(bid)} · the market pays {cents(bid)} for something the model values at {cents(ev_hold)}",
                          details, ["overpriced"], pred.confidence, triggers)
        details.append(f"Will say SELL if the {side} bid ≥ {cents(triggers['take_profit_bid'])} with no edge left, "
                       f"or if {side} drops under {pct(cfg.stop_prob)}.")
        return Signal("HOLD", side, None, pos.qty, ev_hold - ev_sell, p,
                      f"HOLD {side} · {pct(p)} and the market agrees; nothing to do", details, ["hold"], pred.confidence, triggers)
