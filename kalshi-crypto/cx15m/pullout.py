"""PULL OUT: the alarm that the locked settlement call has flipped and is now almost certainly wrong.

Carried over unchanged from the commodities board (cm15m), where these rules were chosen and checked; crypto keeps
its own live record of the alarm.

Why these rules (commodities): replaying the board's calls on two months of Kalshi history (16,988 calls over the seven markets,
3,699 of them lost; rules chosen on each market's first 60% of days, checked on the last 40%) asked when a call that
has gone the other way can be given up with almost no chance of it coming back. The model alone is over-confident in
the far tail (at a 1% reading it still came back about 1.3% of the time) and a single reading can blip; Kalshi's own
price is honest down there. So the alarm needs both:

  Rule A (Kalshi's quotes are usable): after the call, with at least 1:00 left, fire the first second the average of
    the model's chance for the called side and Kalshi's mid for that side is 3% or less. On v1.3's calls it was right
    (the call really lost) 98.1% of the time: 97.8% on the held-out days, at least 97% in every market. On v1.4's
    later, surer calls it fires much less often (about 1 call in 100 instead of 7: they lose less, and more of their
    losses turn in the last minute) and was right 162 times out of 166 (97.6%; 67 of 68 on the held-out days),
    replayed second by second through this code. A call that only slides to 35% still wins about 1 time in 4, which
    is why it must not flash there.
  Rule B (no usable Kalshi quote: no fresh book, no bid or ask on one side, or a spread over 30c): the model alone
    gives the called side 1% or less for 10 seconds in a row, with at least 1:00 left. 98.8% right on history.

Under 1:00 left there is no alarm: by then the losing side often has no bid to sell into. Once it fires the alarm
stays for the rest of the window (one per window) and is scored at settlement: right = the call lost. Selling at the
alarm gets the few cents the losing side still bids (about 2c at the minute's close on history, possibly more if sold
within seconds); it is a safety net against riding a dead call to zero, not a money maker.
"""
from __future__ import annotations

import os
import sqlite3

SCHEMA = """
CREATE TABLE IF NOT EXISTS pullouts (market TEXT, ticker TEXT, ts REAL, seconds_left REAL, side TEXT, rule TEXT,
  p_model REAL, p_kalshi REAL, score REAL, bid REAL, call_p REAL, result TEXT, correct INTEGER, settled_ts REAL,
  PRIMARY KEY (market, ticker));
"""
BAD_CONFIDENCE = ("warming_up", "stale", "no_target")


class PulloutStore:
    def __init__(self, path: str):
        if path != ":memory:":
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript(SCHEMA)

    def add(self, market: str, ticker: str, ts: float, seconds_left: float, side: str, rule: str, p_model: float,
            p_kalshi: float | None, score: float, bid: float | None, call_p: float | None):
        self.conn.execute("INSERT OR IGNORE INTO pullouts (market, ticker, ts, seconds_left, side, rule, p_model, p_kalshi, "
                          "score, bid, call_p) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                          (market, ticker, ts, seconds_left, side, rule, p_model, p_kalshi, score, bid, call_p))
        self.conn.commit()

    def get(self, market: str, ticker: str) -> dict | None:
        row = self.conn.execute("SELECT * FROM pullouts WHERE market=? AND ticker=?", (market, ticker)).fetchone()
        return dict(row) if row else None

    def latest(self, market: str) -> dict | None:
        row = self.conn.execute("SELECT * FROM pullouts WHERE market=? ORDER BY ts DESC LIMIT 1", (market,)).fetchone()
        return dict(row) if row else None

    def open_alarms(self, market: str) -> list[dict]:
        return [dict(r) for r in self.conn.execute("SELECT * FROM pullouts WHERE market=? AND result IS NULL", (market,))]

    def settle(self, market: str, ticker: str, up: bool, now: float):
        a = self.get(market, ticker)
        if not a or a["result"] is not None:
            return
        right = (a["side"] == "UP") != up  # the alarm was right when the call it warned about lost
        self.conn.execute("UPDATE pullouts SET result=?, correct=?, settled_ts=? WHERE market=? AND ticker=?",
                          ("UP" if up else "DOWN", int(right), now, market, ticker))
        self.conn.commit()

    def record(self, market: str | None = None, recent: int = 12) -> dict:
        q = "SELECT * FROM pullouts WHERE result IS NOT NULL" + (" AND market=?" if market else "") + " ORDER BY ts DESC"
        rows = [dict(r) for r in self.conn.execute(q, (market,) if market else ())]
        right = sum(r["correct"] for r in rows)
        return {"right": right, "wrong": len(rows) - right, "n": len(rows),
                "recent": [{"ticker": r["ticker"], "side": r["side"], "correct": bool(r["correct"]), "rule": r["rule"],
                            "seconds_left": r["seconds_left"], "bid": r["bid"]} for r in rows[:recent]]}


class PulloutTracker:
    """One market's PULL OUT alarm, fed the market's live state once a second (after its desk has stepped)."""

    def __init__(self, market: str, store: PulloutStore, level: float = 0.03, model_level: float = 0.01,
                 hold_s: float = 10.0, min_left_s: float = 60.0, max_spread: float = 0.30):
        self.market, self.store = market, store
        self.level, self.model_level, self.hold_s = level, model_level, hold_s
        self.min_left_s, self.max_spread = min_left_s, max_spread
        self._ticker: str | None = None
        self._alarm: dict | None = None  # this window's alarm, once fired (latched)
        self._low_since: float | None = None  # Rule B: since when the model alone has said <= model_level
        self._last: dict | None = store.latest(market)  # the most recent alarm (any window), to show how it ended
        self._now = 0.0
        self.watch: dict = {"state": "waiting"}

    LAST_SHOWN_S = 1800.0  # the card shows how the last alarm ended for half an hour (Kalshi scores it after the close)

    def step(self, now: float, st: dict) -> dict | None:
        """Returns the alarm the second it fires, else None. `st` is the desk's state dict."""
        m = st.get("market") or {}
        ticker = m.get("ticker")
        self._now = now
        if self._last is not None and self._last.get("result") is None:  # pick up its score once Kalshi settles
            self._last = self.store.get(self.market, self._last["ticker"]) or self._last
        if ticker != self._ticker:
            self._ticker, self._low_since = ticker, None
            self._alarm = self.store.get(self.market, ticker) if ticker else None
        if self._alarm is not None:
            if self._alarm.get("result") is None:  # pick up the settlement once it is scored
                self._alarm = self.store.get(self.market, ticker) or self._alarm
            self.watch = {"state": "fired", "alarm": self._alarm}
            return None
        call = (st.get("call") or {}).get("call")
        if not ticker or not call or call.get("ticker") != ticker or call.get("result") is not None:
            self._low_since = None
            self.watch = {"state": "waiting"}
            return None
        side = call["side"]
        left = st.get("seconds_left")
        pred = st.get("prediction") or {}
        p_up = pred.get("p_up")
        fallback = "FALLBACK" in str((st.get("feed") or {}).get("mode", "")).upper()
        if p_up is None or pred.get("confidence") in BAD_CONFIDENCE or fallback or left is None:
            self._low_since = None
            self.watch = {"state": "paused", "side": side, "call_p": call.get("p_side"),
                          "why": "on the backup price" if fallback else "waiting for a fresh price"}
            return None
        p_model = p_up if side == "UP" else 1.0 - p_up
        yb, ya = m.get("yes_bid"), m.get("yes_ask")
        mid_ok = (bool(st.get("quotes_fresh")) and yb is not None and ya is not None and 0.0 <= yb < ya <= 1.0
                  and ya - yb <= self.max_spread + 1e-9)
        p_kalshi = None
        if mid_ok:
            mid = (yb + ya) / 2.0
            p_kalshi = mid if side == "UP" else 1.0 - mid
        bid = yb if side == "UP" else (None if ya is None else 1.0 - ya)
        score = (p_model + p_kalshi) / 2.0 if p_kalshi is not None else p_model
        if p_kalshi is None and p_model <= self.model_level + 1e-9:
            if self._low_since is None:
                self._low_since = now
        else:
            self._low_since = None
        self.watch = {"state": "watching", "side": side, "call_p": call.get("p_side"), "p_model": round(p_model, 4),
                      "p_kalshi": None if p_kalshi is None else round(p_kalshi, 4), "score": round(score, 4),
                      "rule": "A" if p_kalshi is not None else "B", "level": self.level, "model_level": self.model_level,
                      "low_for_s": None if self._low_since is None else round(now - self._low_since),
                      "hold_s": self.hold_s, "too_late": left < self.min_left_s}
        if left < self.min_left_s:
            return None
        rule = None
        if p_kalshi is not None and score <= self.level + 1e-9:  # an exact 3% counts as 3%
            rule = "A"
        elif p_kalshi is None and self._low_since is not None and now - self._low_since >= self.hold_s:
            rule = "B"
        if rule is None:
            return None
        self.store.add(self.market, ticker, now, left, side, rule, p_model, p_kalshi, score,
                       None if bid is None or bid <= 0 else bid, call.get("p_side"))
        self._alarm = self._last = self.store.get(self.market, ticker)
        self.watch = {"state": "fired", "alarm": self._alarm}
        return self._alarm

    def settle_from(self, window_lookup, now: float):
        """Score open alarms whose windows have settled (window_lookup(ticker) -> {'result': 'yes'|'no', ...})."""
        for a in self.store.open_alarms(self.market):
            w = window_lookup(a["ticker"]) or {}
            if w.get("result") in ("yes", "no"):
                self.store.settle(self.market, a["ticker"], w["result"] == "yes", now)

    def state(self) -> dict:
        last = self._last if self._last and self._now - (self._last.get("ts") or 0) < self.LAST_SHOWN_S else None
        return {"watch": self.watch, "record": self.store.record(self.market), "last": last,
                "rules": {"level": self.level, "model_level": self.model_level, "hold_s": self.hold_s,
                          "min_left_s": self.min_left_s}}
