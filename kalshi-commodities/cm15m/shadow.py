"""The v1.4 shadow: what the previous rules would have called on the same windows, recorded quietly.

v1.5's per-market rules were chosen on two months of history. The fair live test is head to head: every market also
runs v1.4's settlement call (its per-market bar held for 10 s, its mix of engine and Kalshi) on the very same
seconds, into its own small database (data/shadow/<market>.db), never shown as the call and never notified. The card shows
both records, so after a few days it is plain whether v1.5 is really better live.
"""
from __future__ import annotations

import copy
import os
import time

from . import clock, tuning
from .calls import CallTracker
from .quotes import Quotes
from .store import Store

SHADOW_VERSION = "1.4"
QUOTE_MAX_AGE_S = 10.0


def desk_inputs(d, now: float):
    """(ticker, prediction, seconds left, seconds elapsed, quotes, report window?) for either kind of desk."""
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
    return m.ticker, pred, tau, elapsed, quotes, bool(events)


class ShadowCalls:
    def __init__(self, key: str, base_trading, data_dir: str, window_s: float | None = None):
        self.key = key
        t = copy.copy(base_trading)
        table, default = tuning.VERSIONS[SHADOW_VERSION]
        tuning.apply(t, table.get(key, default), window_s, "v" + SHADOW_VERSION)
        self.cfg = t
        self.store = Store(os.path.join(data_dir, "shadow", f"{key}.db"))
        self.calls = CallTracker(t, self.store)

    def step(self, d, now: float | None = None):
        now = time.time() if now is None else now
        ticker, pred, tau, elapsed, quotes, blocked = desk_inputs(d, now)
        if blocked and self.calls.call is None:  # v1.4 made no new call in a report window either
            pred = None
        self.calls.step(now, ticker, pred, tau, elapsed, self.cfg.study_s, quotes)

    def settle_from(self, window_lookup):
        for row in self.store.conn.execute("SELECT ticker FROM calls WHERE result IS NULL").fetchall():
            w = window_lookup(row[0]) or {}
            if w.get("result") in ("yes", "no"):
                self.calls.settle(row[0], w["result"] == "yes")

    def state(self) -> dict:
        r = self.store.call_record(0, rules="v" + SHADOW_VERSION)
        return {"version": SHADOW_VERSION, "wins": r["wins"], "losses": r["losses"], "since": self.calls.rules_since,
                "call": self.calls.call}
