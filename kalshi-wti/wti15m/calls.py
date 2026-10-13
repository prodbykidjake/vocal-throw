"""The settlement call: one Up-or-Down prediction per window, made once and never changed, scored at settlement.

It is not a trade. It answers "where will this window settle?", takes as long as it needs, and keeps an honest
record:

  * no call during the study period (the first minutes of the window);
  * a CONFIDENT call as soon as the model gives one side at least `call_prob` and keeps it for `call_hold_s`
    seconds, while the model and Kalshi's odds are not far apart;
  * otherwise a LATE call with whatever the model leans to at `call_deadline_s` before the close;
  * no call for a window the app first saw with under `call_join_min_s` to go, and never in the last `call_min_left_s`
    (late joins and last-second calls would be near-free wins);
  * no call while the price comes from the backup feed (or is older than `call_feed_max_age_s`): the backup sat 3-17c
    off the price Kalshi settles on, and on Oct 12 it turned a 70% DOWN reading into a 78% DOWN call that lost. The
    hold restarts and a deadline call waits until Kalshi's own price is back.

The record shows wins and losses next to the number of wins Kalshi's own odds expected at the moments of the calls,
so it is clear whether the calls beat the market or just agree with it.
"""
from __future__ import annotations

import time
from collections import deque

from . import __version__
from .config import TradingCfg
from .decision import Quotes
from .model import Prediction


METER_TRAIL_S = 30.0  # the meter's faint band = where the model's chance moved over this many seconds


class CallTracker:
    def __init__(self, cfg: TradingCfg, store):
        self.cfg = cfg
        self.store = store
        self._ticker: str | None = None
        self.call: dict | None = None  # the current window's call, once made
        self._hold: tuple[str, float] | None = None  # (side, since): a confident side being confirmed
        self.watch: dict = {}  # what it is looking at before the call (for the card)
        self._first_left: float | None = None  # seconds left when the app first saw this window
        self._quotes = Quotes()  # Kalshi's prices this second, recorded with a call
        self._source: str | None = None  # the price feed this second, recorded with a call
        self._trail: deque[tuple[float, float]] = deque()  # (ts, model's Up chance) over the last METER_TRAIL_S, for the meter
        self.meter: dict | None = None  # the confidence meter under the card: where the model stands right now

    # ------------------------------------------------------------------ per second
    def step(self, now: float, ticker: str | None, pred: Prediction | None, seconds_left: float | None,
             seconds_elapsed: float | None, study_s: float, quotes: Quotes | None = None, feed_ok: bool = True,
             source: str | None = None) -> dict | None:
        """Returns the call the second it is made, else None. `feed_ok` is False while the price is the backup feed or
        too old: then no call is made and a confirmation in progress starts over."""
        cfg = self.cfg
        self._quotes = quotes if quotes is not None else Quotes()
        self._source = source
        if ticker != self._ticker:
            self._ticker = ticker
            self._hold = None
            self.watch = {}
            self._first_left = None
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
        if not feed_ok:
            self._hold = None
            self.watch["feed_paused"] = True
            return None
        if p_side >= cfg.call_prob and not pred.disagree and pred.p_market is not None:
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

    def _update_meter(self, now: float, pred: Prediction | None, seconds_left: float | None):
        """The model's Up chance this second plus its range over the last half minute. It only shows what the model
        sees; the call itself still follows its rules (and, once made, never changes)."""
        if pred is None or seconds_left is None or seconds_left <= 0 or pred.confidence in ("warming_up", "stale", "no_target"):
            self.meter = None
            return
        self._trail.append((now, pred.p_final))
        while self._trail and now - self._trail[0][0] > METER_TRAIL_S:
            self._trail.popleft()
        ups = [p for _, p in self._trail]
        self.meter = {"p_up": round(pred.p_final, 3), "lo": round(min(ups), 3), "hi": round(max(ups), 3),
                      "line": self.cfg.call_prob, "market_up": None if pred.p_market is None else round(pred.p_market, 3),
                      "disagree": pred.disagree}

    def _make(self, now: float, ticker: str, pred: Prediction, side: str, p_side: float, seconds_left: float, kind: str) -> dict:
        pm = None
        if pred.p_market is not None:
            pm = pred.p_market if side == "UP" else 1.0 - pred.p_market
        q = self._quotes
        self.store.add_call(ticker, side, p_side, pm, now, seconds_left, kind, q.ask(side), q.bid(side),
                            version=__version__, source=self._source)
        self.call = self.store.call_for(ticker)
        self._hold = None
        return self.call

    # ------------------------------------------------------------------ scoring
    def settle(self, ticker: str, label_up: bool):
        self.store.settle_call(ticker, "UP" if label_up else "DOWN", time.time())
        if self.call is not None and self.call.get("ticker") == ticker:
            self.call = self.store.call_for(ticker)

    def state(self) -> dict:
        return {"call": self.call, "watch": self.watch, "meter": self.meter, "record": self.store.call_record(),
                "rules": {"prob": self.cfg.call_prob, "hold_s": self.cfg.call_hold_s, "deadline_s": self.cfg.call_deadline_s}}
