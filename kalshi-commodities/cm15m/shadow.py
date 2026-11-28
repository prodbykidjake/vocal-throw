"""All three strictness modes on the same windows, recorded quietly (board v1.5).

The board shows one settlement call per card, in the mode chosen on the page (Standard, Strict or Very strict; see
cm15m/tuning.py). To make that choice on live results rather than on history, every market also runs each mode's
call on the very same seconds into its own small database (data/shadow/<market>-<mode>.db), never shown as the call
and never notified. The cards show the three records side by side.
"""
from __future__ import annotations

import copy
import os
import time

from . import clock, tuning
from .calls import CallTracker
from .quotes import Quotes
from .store import Store

QUOTE_MAX_AGE_S = 10.0


def on_backup(src) -> bool:
    """True while a desk reads its backup price (only the WTI engine has one): no new call then, as on the card."""
    feed = getattr(src, "feed", None)
    try:
        return "FALLBACK" in str(feed.health().mode).upper()
    except Exception:  # a desk or test double without a feed health
        return False


def desk_inputs(d, now: float):
    """(ticker, prediction, seconds left, seconds elapsed, quotes, no new call: report window or backup price?)
    for either kind of desk."""
    src = getattr(d, "engine", None) or d
    m = src.tracker.current
    pred = getattr(src, "pred", None)
    if m is None:
        return None, None, None, None, Quotes(), False
    at = clock.now()
    tau, elapsed = m.seconds_left(at), m.seconds_elapsed(at)
    fresh = src.tracker.quotes_fresh(QUOTE_MAX_AGE_S, now)
    quotes = Quotes.from_market(m) if fresh else Quotes()
    events = (getattr(src, "_vol_info", None) or {}).get("events") or []
    return m.ticker, pred, tau, elapsed, quotes, bool(events) or on_backup(src)


class ShadowCalls:
    """One market's call in one mode, run on the same seconds as the live card."""

    def __init__(self, key: str, mode: str, base_trading, data_dir: str, overrides: dict | None = None,
                 window_s: float | None = None):
        self.key, self.mode = key, tuning.mode_of(mode)
        t = copy.copy(base_trading)
        tuning.apply(t, tuning.for_market(key, overrides, self.mode), window_s, tuning.tag(self.mode))
        self.cfg = t
        self.store = Store(os.path.join(data_dir, "shadow", f"{key}-{self.mode}.db"))
        self.calls = CallTracker(t, self.store)

    def step(self, d, now: float | None = None):
        now = time.time() if now is None else now
        ticker, pred, tau, elapsed, quotes, blocked = desk_inputs(d, now)
        if blocked and self.calls.call is None:  # no new call in a report window or on the backup price, as on the card
            pred = None
        self.calls.step(now, ticker, pred, tau, elapsed, self.cfg.study_s, quotes)

    def settle_from(self, window_lookup):
        for row in self.store.conn.execute("SELECT ticker FROM calls WHERE result IS NULL").fetchall():
            w = window_lookup(row[0]) or {}
            if w.get("result") in ("yes", "no"):
                self.calls.settle(row[0], w["result"] == "yes")

    def state(self) -> dict:
        r = self.store.call_record(0, rules=self.cfg.rules_tag)
        return {"mode": self.mode, "wins": r["wins"], "losses": r["losses"], "since": self.calls.rules_since,
                "bar": self.cfg.call_prob, "call": self.calls.call}
