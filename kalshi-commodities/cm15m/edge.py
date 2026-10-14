"""The EDGE signal: buy only when the model is clearly more confident than Kalshi's price.

Why: replaying the settlement call on two months of Kalshi history (Aug-Oct 2026; WTI, gold, silver, copper, natural
gas, palladium, platinum; second-by-second settlement prices) showed the calls are honest (they win about as often as
they say) but break-even to slightly losing when bought at the ask: Kalshi's price already knows what the model knows
by the time a call fires. What made money was buying only when the model's chance for its side stood clearly above
that side's ask at a minute mark. First found on WTI with a 6-point gap (+6.2c a contract on WTI's held-out days,
+2.2c to +6.4c on the six other markets, never tuned on them). v1.3 tightens it to an 8-point gap and a 75% minimum:
over all seven markets that won 84% instead of 79% and made +6.5c a contract instead of +4.5c (fills priced a full
minute later, fees included), profitable on every market, with about half as many signals.

The rule: at each whole minute from 3:00 to 13:00 into the window (checked in the first few seconds after the mark),
take the model's favoured side; if its chance is at least `min_p` and at least `gap` above that side's ask (Kalshi's
live book, fresh quotes, the live price from Kalshi's own series), signal BUY that side. At most one signal per
window; it is scored at settlement against the ask at the signal, with the taker fee of a 100-contract order.
"""
from __future__ import annotations

import datetime as dt
import math
import os
import sqlite3

SCHEMA = """
CREATE TABLE IF NOT EXISTS edges (market TEXT, ticker TEXT, ts REAL, minute INTEGER, side TEXT, p REAL, ask REAL,
  gap REAL, result TEXT, correct INTEGER, pnl REAL, settled_ts REAL, PRIMARY KEY (market, ticker));
"""


def taker_fee_per_contract(price: float, count: int = 100) -> float:
    return math.ceil(0.07 * count * price * (1 - price) * 100 - 1e-9) / 100 / count


def _ts(iso: str | None) -> float | None:
    if not iso:
        return None
    try:
        return dt.datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


class EdgeStore:
    def __init__(self, path: str):
        if path != ":memory:":
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript(SCHEMA)

    def add(self, market: str, ticker: str, ts: float, minute: int, side: str, p: float, ask: float, gap: float):
        self.conn.execute("INSERT OR IGNORE INTO edges (market, ticker, ts, minute, side, p, ask, gap) VALUES (?,?,?,?,?,?,?,?)",
                          (market, ticker, ts, minute, side, p, ask, gap))
        self.conn.commit()

    def get(self, market: str, ticker: str) -> dict | None:
        row = self.conn.execute("SELECT * FROM edges WHERE market=? AND ticker=?", (market, ticker)).fetchone()
        return dict(row) if row else None

    def open_edges(self, market: str) -> list[dict]:
        return [dict(r) for r in self.conn.execute("SELECT * FROM edges WHERE market=? AND result IS NULL", (market,))]

    def settle(self, market: str, ticker: str, up: bool, now: float):
        e = self.get(market, ticker)
        if not e or e["result"] is not None:
            return
        win = (e["side"] == "UP") == up
        pnl = (1.0 if win else 0.0) - e["ask"] - taker_fee_per_contract(e["ask"])
        self.conn.execute("UPDATE edges SET result=?, correct=?, pnl=?, settled_ts=? WHERE market=? AND ticker=?",
                          ("UP" if up else "DOWN", int(win), pnl, now, market, ticker))
        self.conn.commit()

    def record(self, market: str | None = None, recent: int = 12) -> dict:
        q = "SELECT * FROM edges WHERE result IS NOT NULL" + (" AND market=?" if market else "") + " ORDER BY ts DESC"
        rows = [dict(r) for r in self.conn.execute(q, (market,) if market else ())]
        wins = sum(r["correct"] for r in rows)
        return {"wins": wins, "losses": len(rows) - wins, "pnl": round(sum(r["pnl"] for r in rows), 2), "n": len(rows),
                "avg_ask": round(sum(r["ask"] for r in rows) / len(rows), 3) if rows else None,
                "recent": [{"ticker": r["ticker"], "side": r["side"], "correct": bool(r["correct"]), "p": r["p"], "ask": r["ask"]}
                           for r in rows[:recent]]}


class EdgeTracker:
    """One market's EDGE signal, fed the market's live state once a second."""

    def __init__(self, market: str, store: EdgeStore, gap: float = 0.08, min_p: float = 0.75, first_min: int = 3,
                 last_min: int = 13, check_window_s: float = 10.0):
        self.market, self.store = market, store
        self.gap, self.min_p, self.first_min, self.last_min = gap, min_p, first_min, last_min
        self.check_window_s = check_window_s
        self._checked: dict[str, set[int]] = {}
        self.watch: dict = {}

    def step(self, now: float, st: dict) -> dict | None:
        """Returns the signal the second it fires, else None. `st` is the desk's state dict."""
        m, pred = st.get("market") or {}, st.get("prediction") or {}
        ticker = m.get("ticker")
        open_ts, close_ts = _ts(m.get("open_time")), _ts(m.get("close_time"))
        if not ticker or open_ts is None or close_ts is None or pred.get("p_up") is None:
            self.watch = {}
            return None
        if ticker not in self._checked:
            self._checked = {ticker: set()}  # forget older windows
        p_up = pred["p_up"]
        side = "UP" if p_up >= 0.5 else "DOWN"
        p_side = p_up if side == "UP" else 1.0 - p_up
        yb, ya = m.get("yes_bid"), m.get("yes_ask")
        ask = ya if side == "UP" else (None if yb is None else 1.0 - yb)
        usable = (st.get("quotes_fresh") and ask is not None and 0 < ask < 1 and yb is not None and ya is not None
                  and ya - yb <= 0.30 and "FALLBACK" not in str((st.get("feed") or {}).get("mode", "")).upper()
                  and pred.get("confidence") not in ("warming_up", "stale", "no_target"))
        unit = max(1.0, (close_ts - open_ts) / 15.0)  # one minute of a 15-minute window (the demo's windows are shorter)
        elapsed = now - open_ts
        minute = int(elapsed // unit)
        gap = None if ask is None else p_side - ask
        existing = self.store.get(self.market, ticker)
        nxt = next((k for k in range(max(self.first_min, minute + 1), self.last_min + 1)), None)
        self.watch = {"side": side, "p": round(p_side, 3), "ask": ask, "gap": None if gap is None else round(gap, 3),
                      "need": self.gap, "usable": bool(usable), "signal": existing,
                      "next_check_in_s": None if nxt is None else max(0, round(open_ts + unit * nxt - now)),
                      "window_over": minute > self.last_min}
        if existing is not None or not (self.first_min <= minute <= self.last_min):
            return None
        if elapsed - unit * minute > min(self.check_window_s, unit / 2) or minute in self._checked[ticker]:
            return None  # only in the first seconds after each whole minute, once
        if not usable:
            return None  # try again next second while still inside this minute's check window
        self._checked[ticker].add(minute)
        if p_side < self.min_p - 1e-9 or gap < self.gap - 1e-9:  # an exact 8 points counts as 8 points
            return None
        self.store.add(self.market, ticker, now, minute, side, p_side, ask, gap)
        sig = self.store.get(self.market, ticker)
        self.watch["signal"] = sig
        return sig

    def settle_from(self, window_lookup, now: float):
        """Score open signals whose windows have settled (window_lookup(ticker) -> {'result': 'yes'|'no', ...})."""
        for e in self.store.open_edges(self.market):
            w = window_lookup(e["ticker"]) or {}
            if w.get("result") in ("yes", "no"):
                self.store.settle(self.market, e["ticker"], w["result"] == "yes", now)

    def state(self) -> dict:
        return {"watch": self.watch, "record": self.store.record(self.market),
                "rules": {"gap": self.gap, "min_p": self.min_p, "from_min": self.first_min, "to_min": self.last_min}}

