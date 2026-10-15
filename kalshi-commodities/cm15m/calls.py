"""The settlement call: one Up-or-Down prediction per window, made once and never changed, scored at settlement.

It is not a trade. It answers "where will this window settle?", takes as long as it needs, and keeps an honest
record:

  * no call during the study period (the first minutes of the window);
  * a CONFIDENT call as soon as the model gives one side at least `call_prob` and keeps it for `call_hold_s`
    seconds, while the model and Kalshi's odds are not far apart;
  * otherwise a LATE call with whatever the model leans to at `call_deadline_s` before the close (unless `late_call`
    is off: then a window that never gets sure enough gets no call at all);
  * no call for a window the app first saw with under `call_join_min_s` to go, and never in the last `call_min_left_s`
    (late joins and last-second calls would be near-free wins).

With `call_blend_market` above 0 the call reads a blend of the model and Kalshi's mid (in log-odds, Kalshi's weight =
`call_blend_market`) instead of the model alone; it falls back to the model when Kalshi's quotes are not fresh.

The record shows wins and losses next to the number of wins Kalshi's own odds expected at the moments of the calls,
so it is clear whether the calls beat the market or just agree with it. With `rules_tag` set, a second record counts
only the calls made since those rules first ran.
"""
from __future__ import annotations

import math
import time
from collections import deque

from .config import TradingCfg
from .quotes import Quotes
from .model import Prediction


METER_TRAIL_S = 30.0  # the meter's faint band = where the model's chance moved over this many seconds


def blend_up(p_model: float, p_market: float | None, w_market: float) -> float:
    """The Up chance the call reads: the model's, or a log-odds blend with Kalshi's mid (Kalshi's weight w_market)."""
    if not w_market or p_market is None:
        return p_model

    def logit(p: float) -> float:
        p = min(0.999, max(0.001, p))
        return math.log(p / (1.0 - p))

    x = (1.0 - w_market) * logit(p_model) + w_market * logit(p_market)
    return 1.0 / (1.0 + math.exp(-x))


def cfg_rules(cfg) -> str | None:
    return cfg.rules_tag or None


class CallTracker:
    def __init__(self, cfg: TradingCfg, store):
        self.cfg = cfg
        self.store = store
        self.rules_since: float | None = None  # when the current rules (rules_tag) first ran on this market
        tag = cfg.rules_tag
        if tag:
            key = f"rules_since:{tag}"
            try:
                self.rules_since = float(store.get_state(key))
            except (TypeError, ValueError):
                self.rules_since = time.time()
                store.set_state(key, str(self.rules_since))
        self._ticker: str | None = None
        self.call: dict | None = None  # the current window's call, once made
        self._hold: tuple[str, float] | None = None  # (side, since): a confident side being confirmed
        self.watch: dict = {}  # what it is looking at before the call (for the card)
        self._first_left: float | None = None  # seconds left when the app first saw this window
        self._refused: str | None = None  # why a call that reached the bar was refused this window (for the card)
        self._quotes = Quotes()  # Kalshi's prices this second, recorded with a call
        self._trail: deque[tuple[float, float]] = deque()  # (ts, model's Up chance) over the last METER_TRAIL_S, for the meter
        self.meter: dict | None = None  # the confidence meter under the card: where the model stands right now

    # ------------------------------------------------------------------ per second
    def step(self, now: float, ticker: str | None, pred: Prediction | None, seconds_left: float | None,
             seconds_elapsed: float | None, study_s: float, quotes: Quotes | None = None) -> dict | None:
        """Returns the call the second it is made, else None."""
        cfg = self.cfg
        late_call = cfg.late_call
        self._quotes = quotes if quotes is not None else Quotes()
        if ticker != self._ticker:
            self._ticker = ticker
            self._hold = None
            self.watch = {}
            self._first_left = None
            self._refused = None
            self.call = self.store.call_for(ticker) if ticker else None
            self._trail.clear()
        self._update_meter(now, pred, seconds_left)
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
            if self._first_left < cfg.call_join_min_s:
                status = "no call this window (joined it too late to call it fairly)"
            elif late_call:
                status = "no call this window (too close to the close)"
            elif self._refused:
                status = f"no call this window ({self._refused})"
            else:
                status = f"no call this window (never {round(cfg.call_prob * 100)}% sure for {round(cfg.call_hold_s)}s)"
            self.watch = {"status": status}
            return None
        p_up = self.p_for_call(pred)
        side = "UP" if p_up >= 0.5 else "DOWN"
        p_side = p_up if side == "UP" else 1.0 - p_up
        self.watch = {"status": "studying" if (seconds_elapsed is not None and seconds_elapsed < study_s) else "watching",
                      "lean": side, "p": round(p_side, 3),
                      "deadline_in_s": max(0, round(seconds_left - cfg.call_deadline_s)) if late_call else None,
                      "closes_in_s": None if late_call else max(0, round(seconds_left - cfg.call_min_left_s)),
                      "study_left_s": None if seconds_elapsed is None else max(0, round(study_s - seconds_elapsed))}
        if seconds_elapsed is not None and seconds_elapsed < study_s:
            self._hold = None
            return None
        if p_side >= cfg.call_prob and (pred.disagree or pred.p_market is None):
            # sure enough, but no confident call while Kalshi's odds are far off or missing: remember why
            self._refused = "engine and Kalshi too far apart" if pred.disagree else "no fresh Kalshi odds"
        if p_side >= cfg.call_prob and not pred.disagree and pred.p_market is not None:
            if self._hold is None or self._hold[0] != side:
                self._hold = (side, now)
            self.watch["confirming_s"] = round(now - self._hold[1])
            if now - self._hold[1] >= cfg.call_hold_s:
                return self._make(now, ticker, pred, side, p_side, seconds_left, "confident")
            return None
        self._hold = None
        if late_call and seconds_left <= cfg.call_deadline_s:
            return self._make(now, ticker, pred, side, p_side, seconds_left, "late")
        return None

    def p_for_call(self, pred: Prediction) -> float:
        """The Up chance the call goes by: the model's, or its blend with Kalshi's mid (see blend_up)."""
        return blend_up(pred.p_final, pred.p_market, self.cfg.call_blend_market)

    def _update_meter(self, now: float, pred: Prediction | None, seconds_left: float | None):
        """The Up chance the call goes by this second plus its range over the last half minute. It only shows what the
        model sees; the call itself still follows its rules (and, once made, never changes)."""
        if pred is None or seconds_left is None or seconds_left <= 0 or pred.confidence in ("warming_up", "stale", "no_target"):
            self.meter = None
            return
        p_up = self.p_for_call(pred)
        self._trail.append((now, p_up))
        while self._trail and now - self._trail[0][0] > METER_TRAIL_S:
            self._trail.popleft()
        ups = [p for _, p in self._trail]
        self.meter = {"p_up": round(p_up, 3), "lo": round(min(ups), 3), "hi": round(max(ups), 3),
                      "line": self.cfg.call_prob, "market_up": None if pred.p_market is None else round(pred.p_market, 3),
                      "disagree": pred.disagree,
                      "model_up": round(pred.p_final, 3) if p_up != pred.p_final else None}

    def _make(self, now: float, ticker: str, pred: Prediction, side: str, p_side: float, seconds_left: float, kind: str) -> dict:
        pm = None
        if pred.p_market is not None:
            pm = pred.p_market if side == "UP" else 1.0 - pred.p_market
        q = self._quotes
        self.store.add_call(ticker, side, p_side, pm, now, seconds_left, kind, q.ask(side), q.bid(side), cfg_rules(self.cfg))
        self.call = self.store.call_for(ticker)
        self._hold = None
        return self.call

    # ------------------------------------------------------------------ scoring
    def settle(self, ticker: str, label_up: bool):
        self.store.settle_call(ticker, "UP" if label_up else "DOWN", time.time())
        if self.call is not None and self.call.get("ticker") == ticker:
            self.call = self.store.call_for(ticker)

    def state(self) -> dict:
        cfg = self.cfg
        out = {"call": self.call, "watch": self.watch, "meter": self.meter, "record": self.store.call_record(),
               "rules": {"prob": cfg.call_prob, "hold_s": cfg.call_hold_s, "deadline_s": cfg.call_deadline_s,
                         "late_call": cfg.late_call, "study_s": cfg.study_s,
                         "min_left_s": cfg.call_min_left_s, "blend_market": cfg.call_blend_market,
                         "tag": cfg.rules_tag}}
        if self.rules_since is not None:  # only the calls these rules made (a call row carries the rules it was made under)
            out["record_rules"] = {**self.store.call_record(0, rules=cfg.rules_tag), "since": self.rules_since}
        return out
