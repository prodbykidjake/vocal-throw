"""Turns a probability into coaching.

Entries: BUY (side, limit, size), or WAIT with the reason and what would change the answer.
Open positions (scalp-first): SELL NOW, or HOLD with a concrete "SELL AT x¢" target, plus the live
cash-out value and P&L the way Kalshi's sell sheet shows them.
All contract prices are dollars (0..1); Kalshi quotes sub-cent prices (0.013), so display keeps one decimal.
"""
from __future__ import annotations

import math
import time
from dataclasses import dataclass, field
from statistics import NormalDist

from .config import TradingCfg
from .fees import FeeSchedule, fee_per_contract, taker_fee
from .model import Prediction, clamp
from .paths import TouchCurve, simulate
from .sizing import dollars_for

_ND = NormalDist()
SELL_RULES = ("zone", "rollover", "give_up")


def cents(x: float | None) -> str:
    if x is None:
        return "--"
    c = x * 100
    return f"{c:.1f}¢" if abs(c - round(c)) > 0.05 else f"{round(c):d}¢"


def pct(p: float | None) -> str:
    return "--" if p is None else f"{round(p * 100):d}%"


def pct5(p: float | None) -> str:
    """Probability rounded to 5% so the number on screen does not twitch every second."""
    return "--" if p is None else f"{5 * round(p * 20):d}%"


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
    target: float | None = None  # low end of the committed sell zone (from the plan, or set at entry)
    target_high: float | None = None  # high end of the sell zone
    zone_ts: float = 0.0  # when the zone was last (re)planned
    last_reason: str | None = None  # the rule SHOWN last second (in memory only): the latches key on it
    last_raw_reason: str | None = None  # the rule that FIRED last second, before the two-second debounce
    rollover_ref: float | None = None  # high bid when a rollover SELL was last shown (in memory): it re-arms only on a new high

    @property
    def cost(self) -> float:
        """Dollars at risk: what you paid for the shares plus the taker fee Kalshi charged on the buy."""
        return (self.amount if self.amount is not None else self.qty * self.avg_price) + self.entry_fee

    def as_dict(self) -> dict:
        return {"id": self.id, "ticker": self.ticker, "side": self.side, "qty": round(self.qty, 4),
                "avg_price": self.avg_price, "opened_ts": self.opened_ts, "amount": self.amount,
                "entry_fee": self.entry_fee, "cost": round(self.cost, 2), "high_bid": self.high_bid, "target": self.target,
                "target_high": self.target_high, "zone_ts": self.zone_ts}


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

    def _exit_plan(self, side: str, entry: float) -> str:
        return (f"Exit plan: once you report the buy the card shows SELL BETWEEN low–high (prices the simulated paths reach "
                f"before the close, with the chance of each); SELL NOW when the bid gets there. It only says SELL at a loss "
                f"when the chance of getting back to breakeven is ≤ {pct(self.cfg.give_up_prob)}; "
                f"hold to settlement only if {side} is ≥ {pct(self.cfg.hold_to_settle_prob)} with under 2 min left and selling is clearly worse.")

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
               seconds_elapsed: float | None, position: Position | None = None, now: float | None = None) -> Signal:
        if position is not None:
            return self._manage_position(pred, quotes, price, strike, seconds_left, position, now)
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
            trig_price += pred.basis_signed  # the model works in basis-adjusted space; show it in feed terms
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

        sized = dollars_for(p, cfg, ask=ask, fee=fee)
        amount, tier = sized if sized else (round(size * ask, 2), "lean")
        shares = amount / ask
        headline = f"BUY {side} · limit {cents(ask)} · ${amount:.0f} (≈ {shares:.0f} shares) · {tier}"
        details.append(f"Size: {tier} → ${amount:.0f} (unit ${cfg.unit_dollars:.0f}, max ${cfg.max_trade_dollars:.0f}); "
                       f"max loss ${amount:.2f}, win pays ${shares * (1 - ask):.2f} before fees.")
        details.append(self._exit_plan(side, ask))
        if self.fees.is_estimate:
            details.append("Fee is an estimate: this series uses a fee table the app cannot read.")
        return Signal("BUY", side, ask, size, edge, p, headline, details, ["edge"], pred.confidence, triggers)

    # ------------------------------------------------------------------ open position (scalp-first, forward-looking)
    def breakeven(self, pos: Position) -> float:
        """The bid at which cashing out returns exactly what you put in (entry fee and exit fee included)."""
        if pos.qty <= 0:
            return pos.avg_price
        b = pos.cost / pos.qty
        for _ in range(3):
            b = (pos.cost + taker_fee(b, pos.qty, self.fees)) / pos.qty
        return min(b, 0.99)

    def curve_for(self, pred: Prediction, side: str, price: float, strike: float | None, seconds_left: float | None,
                  quotes: Quotes | None = None, exclude_last_s: float | None = None) -> TouchCurve | None:
        """Simulated paths of the BID for `side` from here to the close. They start from where Kalshi's own odds
        put the price (the model's price only when there are no odds), minus half the spread, so "chance of
        reaching 35¢" is about the price you can actually sell at, not about the model's opinion of it."""
        cfg = self.cfg
        if strike is None or seconds_left is None or seconds_left <= 0 or pred.confidence in ("stale", "warming_up", "no_target"):
            return None
        s0 = pred.price_adj if pred.price_adj is not None else price
        if pred.implied_price is not None and pred.implied_price > 0:
            s0 = pred.implied_price
        offset = 0.01
        if quotes is not None and quotes.ask(side) is not None and quotes.bid(side) is not None:
            offset = clamp((quotes.ask(side) - quotes.bid(side)) / 2.0, 0.0, 0.05)
        sigma = pred.sigma_eff or pred.sigma
        return simulate(side, s0, strike, sigma, seconds_left, cfg.settle_lag_s, cfg.tie_adj, cfg.mc_paths,
                        exclude_last_s=cfg.zone_exclude_last_s if exclude_last_s is None else exclude_last_s, offset=offset)

    def zone_from(self, curve: TouchCurve, breakeven: float) -> tuple[float, float]:
        """SELL BETWEEN low–high: low = the price reached with `zone_low_prob` (never a loss), high = with `zone_high_prob`."""
        cfg = self.cfg
        low = round(max(curve.level(cfg.zone_low_prob), breakeven + 0.02), 2)
        low = min(low, max(0.93, round(breakeven + 0.01, 3)))  # capped, but never under breakeven
        high = round(max(curve.level(cfg.zone_high_prob), low + 0.05), 2)
        high = min(high, max(0.95, round(low + 0.02, 3)))
        return low, high

    def _manage_position(self, pred: Prediction, quotes: Quotes, price: float, strike: float | None,
                         seconds_left: float | None, pos: Position, now: float | None = None) -> Signal:
        cfg = self.cfg
        now = time.time() if now is None else now
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
        triggers: dict = {"zone": [pos.target, pos.target_high], "give_up_prob": cfg.give_up_prob}

        def scalp(action: str, reason: str, text: str, cash: float | None = None, pnl: float | None = None, **extra) -> dict:
            d = {"action": action, "reason": reason, "text": text, "target": pos.target, "target_high": pos.target_high,
                 "bid": bid, "cash_out": None if cash is None else round(cash, 2), "pnl": None if pnl is None else round(pnl, 2),
                 "p_side": round(p, 4)}
            d.update(extra)
            return d

        def hold(code: str, headline: str, sc: dict, edge: float | None = None) -> Signal:
            return Signal("HOLD", side, None, 0, edge, p, headline, details, [code], pred.confidence, triggers, sc)

        def sell(code: str, headline: str, sc: dict, edge: float | None = None) -> Signal:
            return Signal("SELL", side, bid, 0, edge, p, headline, details, [code], pred.confidence, triggers, sc)

        if seconds_left is not None and seconds_left <= 0:
            return hold("window_closed", "HOLD · window closed, waiting for settlement",
                        scalp("HOLD", "window_closed", "window closed; settlement decides"))
        if bid is None:
            return hold("no_quotes", "HOLD · no bid to sell into right now", scalp("HOLD", "no_quotes", "no fresh Kalshi bid"))

        cash = cash_out(qty, bid, self.fees)
        pnl = cash - pos.cost
        hold_val = p  # per share, expected settlement value by the model
        sell_val = cash / qty if qty > 0 else bid  # per share, after the sell fee
        breakeven = self.breakeven(pos)
        losing = bid < breakeven
        details.append(f"Cash out now ≈ ${cash:.2f} ({money(pnl)}); breakeven bid {cents(breakeven)}; by the model holding is worth "
                       f"≈ ${hold_val * qty:.2f} ({cents(hold_val)} vs {cents(sell_val)} per share after fee).")

        if pred.confidence in ("stale", "warming_up"):
            target = pos.target if pos.target else round(max(breakeven + 0.05, bid + 0.01), 2)
            details.append("Model can't judge right now (" + pred.confidence + "); target is price-based only.")
            return hold(pred.confidence, f"HOLD {side} · sell at {cents(target)} (model unavailable)",
                        scalp("SELL AT", pred.confidence, "model can't judge right now; price-based target only", cash, pnl, target=target))

        # --- the sell zone: committed on the position, re-planned at most every zone_refresh_s ---
        curve = self.curve_for(pred, side, price, strike, seconds_left, quotes)
        commit: tuple[float, float] | None = None
        p_low = p_high = p_recover = None
        low, high = pos.target, pos.target_high
        if low is not None and low < breakeven + 0.015:
            low = high = None  # a plan filled at a worse price, or a target from before zones existed: never a loss
        closing = curve is not None and curve.horizon_s <= 0  # no sellable time left before the last seconds
        if curve is not None:
            fresh_low, fresh_high = self.zone_from(curve, breakeven)
            if low is None or high is None:
                low = low if low else fresh_low  # a plan's target is kept; otherwise plan the zone now
                high = max(min(max(high if high else 0.0, fresh_high, low + 0.05), 0.95), round(low + 0.02, 3))
                commit = (low, high)
            elif now - pos.zone_ts >= cfg.zone_refresh_s or (closing and now - pos.zone_ts >= 5):
                # The zone never chases the price upward (that would move the goalposts every time the bid
                # climbed); it comes down only when the old low has become unlikely.
                if curve.touch(low) < cfg.zone_drop_prob and fresh_low <= low - 0.02:
                    commit = (fresh_low, min(max(fresh_high, fresh_low + 0.05), max(0.95, round(fresh_low + 0.02, 3))))
                    low, high = commit
            p_low, p_high = curve.touch(low), curve.touch(high)
        if losing:
            # chance of ever seeing breakeven again: paths to the very end of trading (the zone curve stops
            # 30 s early), and never below the chance of simply winning, since a win pays $1 > breakeven
            full = self.curve_for(pred, side, price, strike, seconds_left, quotes, exclude_last_s=0.0)
            p_recover = max(full.touch(breakeven) if full is not None else 0.0, p)
        if low is None or high is None:
            low = round(max(breakeven + 0.05, bid + 0.01), 2)
            high = max(min(0.95, round(low + 0.10, 2)), round(low + 0.02, 3))
        zone_txt = f"{cents(low)}–{cents(high)}"
        triggers["zone"] = [low, high]
        if curve is not None:
            details.append(f"Simulated {curve.horizon_s / 60:.0f} min of price paths: ~{pct5(p_low)} chance the {side} price reaches "
                           f"{cents(low)} before the close, ~{pct5(p_high)} chance of {cents(high)}"
                           + (f", ~{pct5(p_recover)} chance of getting back to breakeven ({cents(breakeven)}) or winning." if losing else "."))

        # --- which rule fires this second (the "raw" answer) ---
        # Every SELL NOW rule latches on the PREVIOUSLY SHOWN rule, so a 1¢ wobble back across its threshold
        # does not turn the box back to HOLD (that is the flicker the user hated).
        prev = pos.last_reason
        ride_p = cfg.hold_to_settle_prob + (-0.05 if prev == "ride_to_settle" else 0.03 if prev in SELL_RULES else 0.0)
        ride_gap = 0.0 if prev == "ride_to_settle" else 0.01
        can_ride = (cfg.settle_advice != "never" and seconds_left is not None and seconds_left <= 120
                    and p >= ride_p and hold_val >= sell_val + ride_gap)
        latched = prev == "zone" and bid >= max(low - max(0.02, 0.08 * low), breakeven + 0.005)  # never a loss
        in_zone = bid >= low - 0.002 or latched
        ran = pos.high_bid is not None and pos.high_bid >= pos.avg_price + 0.5 * (low - pos.avg_price)
        armed = pos.rollover_ref is None or (pos.high_bid is not None and pos.high_bid >= pos.rollover_ref + 0.03)
        rolled = False
        if pos.high_bid:
            rolled = (armed and bid <= pos.high_bid * 0.75) or (prev == "rollover" and bid <= pos.high_bid * 0.85)
        rollover = ran and pnl > 0 and bid > pos.avg_price and rolled
        hopeless = p_recover is not None and (p_recover <= cfg.give_up_prob or (prev == "give_up" and p_recover <= 2 * cfg.give_up_prob))
        min_cash = min(cfg.give_up_min_cash, 0.2 * pos.cost) * (0.5 if prev == "give_up" else 1.0)
        # vs the model's settlement value: a lottery ticket beats salvage when the bid is far under it; with
        # hysteresis so a 1¢ wobble does not flip salvage <-> lottery every second
        slack = cfg.edge_min * (2.0 if prev == "give_up" else 0.5 if prev == "lottery" else 1.0)
        salvage_ok = cash >= min_cash and sell_val >= hold_val - slack
        if can_ride:
            rule = "ride_to_settle"
        elif in_zone:
            rule = "zone"
        elif rollover:
            rule = "rollover"
        elif losing and hopeless:
            rule = "give_up" if salvage_ok else "lottery"
        else:
            rule = "hold"
        # --- debounce, done here so the shown card always carries this second's numbers ---
        # A SELL has to be the raw answer two seconds in a row before it shows; a shown SELL stays until the raw
        # answer has been something else two seconds in a row.
        prev_raw = pos.last_raw_reason
        shown = rule
        confirming = False
        if rule in SELL_RULES and prev not in SELL_RULES and prev_raw not in SELL_RULES:
            shown = prev if prev in ("ride_to_settle", "lottery") else "hold"  # first second: not yet
            confirming = True
        elif rule not in SELL_RULES and prev in SELL_RULES and prev_raw in SELL_RULES:
            shown = prev  # one more second before letting go
            # ... but never as a loss (zone/rollover) and never a salvage call on a position back in profit
            if (shown in ("zone", "rollover") and pnl <= 0) or (shown == "give_up" and not losing):
                shown = rule
        triggers["raw_rule"] = rule

        def out(sig: Signal) -> Signal:
            if commit and sig.scalp is not None:
                sig.scalp["commit_zone"] = [low, high]  # the engine persists this on the position
            return sig

        # --- render the shown rule with fresh numbers ---
        if shown == "ride_to_settle":
            return out(hold("ride_to_settle", f"HOLD to settlement · {side} {pct(p)} with {mmss(seconds_left)} left; selling would give up "
                            f"{cents(max(hold_val - sell_val, 0.0))} per share",
                            scalp("HOLD", "ride_to_settle", f"{side} {pct(p)} with {mmss(seconds_left)} left · selling gives up {cents(max(hold_val - sell_val, 0.0))} a share", cash, pnl),
                            hold_val - sell_val))
        if shown == "zone":
            if closing and not losing and (commit or prev == "zone"):
                # the zone came down to the bid because the clock ran out, not because the price got there
                why = f"last {mmss(seconds_left)} · take the profit before the close · {money(pnl)}"
            else:
                where = "top of" if bid >= high - 0.002 else "in"
                dipped = "" if bid >= low - 0.002 else (" (dipped a hair under it)" if latched else " (just left it)")
                why = f"{where} your sell zone {zone_txt}{dipped} · {money(pnl)}"
            return out(sell("zone", f"SELL NOW {side} at {cents(bid)} · {why}", scalp("SELL NOW", "zone", why, cash, pnl), sell_val - hold_val))
        if shown == "rollover":
            pos.rollover_ref = pos.high_bid  # shown, so the rule re-arms only after a new high (3¢ above this one)
            return out(sell("rollover", f"SELL NOW {side} at {cents(bid)} · lock in {money(pnl)}; bid rolled over from {cents(pos.high_bid)}",
                            scalp("SELL NOW", "rollover", f"bid rolled over from {cents(pos.high_bid)} · lock in {money(pnl)}", cash, pnl), sell_val - hold_val))
        if shown == "give_up":
            return out(sell("give_up", f"SELL NOW {side} at {cents(bid)} · only ~{pct5(p_recover)} chance of getting back to {cents(breakeven)}; "
                            f"salvage ${cash:.2f} ({money(pnl)})",
                            scalp("SELL NOW", "give_up", f"only ~{pct5(p_recover)} chance of getting back to {cents(breakeven)} · salvage ${cash:.2f}", cash, pnl,
                                  p_recover=None if p_recover is None else round(p_recover, 3), breakeven=round(breakeven, 3)), sell_val - hold_val))
        if shown == "lottery":
            details.append("Selling would return almost nothing; a lottery ticket is worth more than that.")
            return out(hold("lottery", f"HOLD {side} · ride it as a {pct(p)} lottery ticket (cash out is only ${cash:.2f})",
                            scalp("HOLD", "lottery", f"{pct(p)} lottery ticket · cash out is only ${cash:.2f}", cash, pnl), hold_val - sell_val))
        # hold for the zone, and say how likely it is (in the last 30 s the zone curve has no time left: no chances)
        text = f"now {cents(bid)}"
        chance = ""
        if closing:
            text += f" · last {mmss(seconds_left)} of trading"
        elif p_low is not None:
            text += f" · ~{pct5(p_low)} chance of {cents(low)} · ~{pct5(p_high)} chance of {cents(high)}"
            chance = f" · ~{pct5(p_low)} chance of {cents(low)}"
        if losing and p_recover is not None:
            text += f" · ~{pct5(p_recover)} chance of getting back to {cents(breakeven)} or winning"
        if confirming:
            text += " · confirming SELL NOW…"
            chance = " · confirming SELL NOW…"
        details.append(f"Will say SELL NOW when the bid enters the zone, if a profitable run rolls over hard, or if the chance of "
                       f"getting back to breakeven drops to {pct(cfg.give_up_prob)}.")
        sc = scalp("SELL BETWEEN", "hold", text, cash, pnl, target=low, target_high=high,
                   p_low=None if p_low is None else round(p_low, 3), p_high=None if p_high is None else round(p_high, 3),
                   p_recover=None if p_recover is None else round(p_recover, 3), breakeven=round(breakeven, 3))
        return out(hold("hold", f"HOLD {side} · sell between {zone_txt}{chance}", sc, hold_val - sell_val))
