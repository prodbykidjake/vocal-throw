"""The settlement call: one Up-or-Down prediction per window, made once and never changed, scored at settlement.

It is not a trade. It answers "where will this window settle?", takes as long as it needs, and keeps an honest
record:

  * no call during the study period (the first minutes of the window);
  * a CONFIDENT call as soon as the model gives one side at least `call_prob` and keeps it for `call_hold_s`
    seconds, while the model and Kalshi's odds are not far apart;
  * otherwise a LATE call with whatever the model leans to at `call_deadline_s` before the close;
  * no call for a window the app first saw with under `call_join_min_s` to go, and never in the last `call_min_left_s`
    (late joins and last-second calls would be near-free wins).

The record shows wins and losses next to the number of wins Kalshi's own odds expected at the moments of the calls,
so it is clear whether the calls beat the market or just agree with it.

v12: being right is not the same as making money. A call that wins 80% of the time loses money bought at 85¢, so the
card also says what the call is worth paying ("worth buying up to"), records Kalshi's price at the moment of the call,
and keeps a second record: what buying every call at that price would have made after fees.
"""
from __future__ import annotations

import time

from .config import TradingCfg
from .decision import Quotes
from .fees import FeeSchedule, taker_fee
from .model import Prediction


def worth_up_to(p_side: float | None, stake: float, margin: float, fees: FeeSchedule | None = None) -> float | None:
    """The highest whole-cent price at which buying `stake` dollars of a side the model gives `p_side` still leaves
    `margin` of expected value per contract after the taker fee. None when no price does."""
    if p_side is None:
        return None
    for c in range(99, 0, -1):
        price = c / 100.0
        qty = stake / price
        if p_side - price - taker_fee(price, qty, fees) / qty >= margin - 1e-9:
            return price
    return None


class CallTracker:
    def __init__(self, cfg: TradingCfg, store):
        self.cfg = cfg
        self.store = store
        self._ticker: str | None = None
        self.call: dict | None = None  # the current window's call, once made
        self._hold: tuple[str, float] | None = None  # (side, since): a confident side being confirmed
        self.watch: dict = {}  # what it is looking at before the call (for the card)
        self._first_left: float | None = None  # seconds left when the app first saw this window
        self.fees = FeeSchedule()  # the engine keeps this in step with the series' fee schedule
        self._quotes = Quotes()
        self._pred: Prediction | None = None

    # ------------------------------------------------------------------ per second
    def step(self, now: float, ticker: str | None, pred: Prediction | None, seconds_left: float | None,
             seconds_elapsed: float | None, study_s: float, quotes: Quotes | None = None) -> dict | None:
        """Returns the call the second it is made, else None."""
        cfg = self.cfg
        self._quotes = quotes if quotes is not None else Quotes()
        self._pred = pred
        if ticker != self._ticker:
            self._ticker = ticker
            self._hold = None
            self.watch = {}
            self._first_left = None
            self.call = self.store.call_for(ticker) if ticker else None
        if ticker is None or self.call is not None:
            return None
        if pred is None or seconds_left is None or seconds_left <= 0 or pred.confidence in ("warming_up", "stale", "no_target"):
            self._hold = None
            self.watch = {"status": "waiting for data" if pred is None or pred.confidence != "warming_up" else "warming up"}
            return None
        if self._first_left is None:
            self._first_left = seconds_left
        if self._first_left < cfg.call_join_min_s or seconds_left < cfg.call_min_left_s:
            self._hold = None
            self.watch = {"status": "no call this window (joined it too late to call it fairly)" if self._first_left < cfg.call_join_min_s
                          else "no call this window (too close to the close)"}
            return None
        side = "UP" if pred.p_final >= 0.5 else "DOWN"
        p_side = pred.p_final if side == "UP" else 1.0 - pred.p_final
        self.watch = {"status": "studying" if (seconds_elapsed is not None and seconds_elapsed < study_s) else "watching",
                      "lean": side, "p": round(p_side, 3),
                      "deadline_in_s": max(0, round(seconds_left - cfg.call_deadline_s)),
                      "study_left_s": None if seconds_elapsed is None else max(0, round(study_s - seconds_elapsed))}
        if seconds_elapsed is not None and seconds_elapsed < study_s:
            self._hold = None
            return None
        if p_side >= cfg.call_prob and not pred.disagree:
            if self._hold is None or self._hold[0] != side:
                self._hold = (side, now)
            self.watch["confirming_s"] = round(now - self._hold[1])
            if now - self._hold[1] >= cfg.call_hold_s:
                return self._make(now, ticker, pred, side, p_side, seconds_left, "confident")
            return None
        self._hold = None
        if seconds_left <= cfg.call_deadline_s:
            return self._make(now, ticker, pred, side, p_side, seconds_left, "late")
        return None

    def _make(self, now: float, ticker: str, pred: Prediction, side: str, p_side: float, seconds_left: float, kind: str) -> dict:
        pm = None
        if pred.p_market is not None:
            pm = pred.p_market if side == "UP" else 1.0 - pred.p_market
        q = self._quotes
        self.store.add_call(ticker, side, p_side, pm, now, seconds_left, kind, q.ask(side), q.bid(side), self.max_price(p_side))
        self.call = self.store.call_for(ticker)
        self._hold = None
        return self.call

    def max_price(self, p_side: float | None) -> float | None:
        return worth_up_to(p_side, self.cfg.unit_dollars, self.cfg.call_buy_margin, self.fees)

    def backfill_prices(self):
        """Calls made before v12 have no recorded price: take Kalshi's quote at that second from the stored quotes."""
        for row in self.store.calls_missing_price():
            q = self.store.quote_near(row["ticker"], row["ts"])
            if q is None:
                continue
            up = row["side"] == "UP"
            ask, bid = (q["yes_ask"], q["yes_bid"]) if up else (q["no_ask"], q["no_bid"])
            if ask is not None:
                self.store.set_call_price(row["ticker"], ask, bid, self.max_price(row["p_side"]))

    def guide(self) -> dict | None:
        """While the window is open: what the locked call is worth paying now (the side never changes; the price does)."""
        call, pred = self.call, self._pred
        if call is None or call.get("result") is not None or pred is None or pred.confidence in ("stale", "no_target"):
            return None
        side = call["side"]
        p_now = pred.p_final if side == "UP" else 1.0 - pred.p_final
        ask = self._quotes.ask(side)
        top = self.max_price(p_now)
        return {"p_now": round(p_now, 3), "ask": ask, "max_price": top,
                "worth_it": ask is not None and top is not None and ask <= top + 1e-9}

    # ------------------------------------------------------------------ scoring
    def settle(self, ticker: str, label_up: bool):
        self.store.settle_call(ticker, "UP" if label_up else "DOWN", time.time())
        if self.call is not None and self.call.get("ticker") == ticker:
            self.call = self.store.call_for(ticker)

    def state(self) -> dict:
        return {"call": self.call, "watch": self.watch, "guide": self.guide(),
                "record": self.store.call_record(stake=self.cfg.unit_dollars, fees=self.fees),
                "rules": {"prob": self.cfg.call_prob, "hold_s": self.cfg.call_hold_s, "deadline_s": self.cfg.call_deadline_s,
                          "margin": self.cfg.call_buy_margin, "stake": self.cfg.unit_dollars}}
