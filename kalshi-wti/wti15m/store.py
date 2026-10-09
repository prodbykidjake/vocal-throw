"""SQLite persistence: windows, ticks, quotes, model snapshots, signals, paper trades, positions, settlements."""
from __future__ import annotations

import json
import os
import sqlite3
import time
from typing import Any

import numpy as np

SCHEMA = """
CREATE TABLE IF NOT EXISTS windows (
  ticker TEXT PRIMARY KEY, event_ticker TEXT, title TEXT, open_time REAL, close_time REAL,
  strike REAL, strike_source TEXT, status TEXT, result TEXT, settled_ts REAL,
  feed_price_at_close REAL, feed_error REAL, p_market_at_close REAL, p_model_at_close REAL,
  rules_primary TEXT, created_ts REAL, updated_ts REAL,
  settle_price REAL, feed_error_lag0 REAL, feed_error_lag60 REAL, feed_source TEXT
);
CREATE TABLE IF NOT EXISTS ticks (ts REAL, source TEXT, price REAL);
CREATE INDEX IF NOT EXISTS ticks_ts ON ticks(ts);
CREATE TABLE IF NOT EXISTS quotes (ts REAL, ticker TEXT, yes_bid REAL, yes_ask REAL, no_bid REAL, no_ask REAL,
  last_price REAL, volume INTEGER, open_interest INTEGER);
CREATE INDEX IF NOT EXISTS quotes_ts ON quotes(ts);
CREATE TABLE IF NOT EXISTS snapshots (id INTEGER PRIMARY KEY, ts REAL, ticker TEXT, seconds_left REAL, price REAL,
  strike REAL, sigma REAL, z REAL, p_base REAL, p_market REAL, p_final REAL, confidence TEXT, features TEXT,
  label INTEGER);
CREATE INDEX IF NOT EXISTS snapshots_ticker ON snapshots(ticker);
CREATE TABLE IF NOT EXISTS signals (id INTEGER PRIMARY KEY, ts REAL, ticker TEXT, action TEXT, side TEXT, price REAL,
  size INTEGER, edge REAL, p_side REAL, p_market REAL, confidence TEXT, headline TEXT, reasons TEXT, seconds_left REAL);
CREATE INDEX IF NOT EXISTS signals_ticker ON signals(ticker);
CREATE TABLE IF NOT EXISTS paper_trades (id INTEGER PRIMARY KEY, ticker TEXT, side TEXT, entry_ts REAL, entry_price REAL,
  size INTEGER, entry_fee REAL, exit_ts REAL, exit_price REAL, exit_fee REAL, exit_reason TEXT, pnl REAL,
  confidence TEXT, p_side REAL);
CREATE TABLE IF NOT EXISTS positions (id INTEGER PRIMARY KEY, ticker TEXT, side TEXT, qty REAL, avg_price REAL,
  opened_ts REAL, closed_ts REAL, exit_price REAL, pnl REAL, note TEXT, amount REAL, entry_fee REAL, high_bid REAL);
CREATE TABLE IF NOT EXISTS model_state (key TEXT PRIMARY KEY, value TEXT, updated_ts REAL);
CREATE TABLE IF NOT EXISTS plans (id INTEGER PRIMARY KEY, ticker TEXT, side TEXT, limit_price REAL, amount REAL, shares REAL,
  target REAL, p_at_plan REAL, tier TEXT, created_ts REAL, ended_ts REAL, status TEXT, status_text TEXT,
  hit_target INTEGER, best_bid REAL, hypo_pnl REAL, expected_profit REAL);
"""


class Store:
    def __init__(self, path: str):
        self.path = path
        if path != ":memory:":
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        self.conn = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA synchronous=NORMAL")
        self.conn.executescript(SCHEMA)
        self._migrate()

    def _migrate(self):
        """Add columns introduced after a database was first created."""
        have = {row[1] for row in self.conn.execute("PRAGMA table_info(windows)")}
        for col, typ in (("settle_price", "REAL"), ("feed_error_lag0", "REAL"), ("feed_error_lag60", "REAL"),
                         ("feed_source", "TEXT")):
            if col not in have:
                self.conn.execute(f"ALTER TABLE windows ADD COLUMN {col} {typ}")
        have = {row[1] for row in self.conn.execute("PRAGMA table_info(positions)")}
        for col, typ in (("amount", "REAL"), ("entry_fee", "REAL"), ("high_bid", "REAL"), ("target", "REAL"),
                         ("target_high", "REAL"), ("zone_ts", "REAL")):
            if col not in have:
                self.conn.execute(f"ALTER TABLE positions ADD COLUMN {col} {typ}")
        have = {row[1] for row in self.conn.execute("PRAGMA table_info(plans)")}
        for col, typ in (("target_high", "REAL"), ("p_target", "REAL")):
            if col not in have:
                self.conn.execute(f"ALTER TABLE plans ADD COLUMN {col} {typ}")

    def mark_status(self, ticker: str, status: str):
        self.conn.execute("UPDATE windows SET status=?, updated_ts=? WHERE ticker=? AND result IS NULL",
                          (status, time.time(), ticker))

    def close(self):
        self.conn.close()

    # ------------------------------------------------------------------ windows
    def upsert_window(self, m, p_model: float | None = None):
        now = time.time()
        self.conn.execute(
            """INSERT INTO windows (ticker, event_ticker, title, open_time, close_time, strike, strike_source, status,
                                    result, rules_primary, created_ts, updated_ts, p_market_at_close, p_model_at_close)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)
               ON CONFLICT(ticker) DO UPDATE SET status=excluded.status, result=COALESCE(excluded.result, windows.result),
                 strike=COALESCE(excluded.strike, windows.strike), strike_source=excluded.strike_source,
                 updated_ts=excluded.updated_ts, p_market_at_close=excluded.p_market_at_close,
                 p_model_at_close=COALESCE(excluded.p_model_at_close, windows.p_model_at_close)""",
            (m.ticker, m.event_ticker, m.title, _ts(m.open_time), _ts(m.close_time), m.strike, m.strike_source, m.status,
             m.result, m.rules_primary, now, now, m.yes_mid, p_model))

    def settle_window(self, ticker: str, result: str, feed_price_at_close: float | None, feed_error: float | None,
                      settled_ts: float | None = None, settle_price: float | None = None):
        self.conn.execute(
            """UPDATE windows SET result=?, status='settled', settled_ts=?,
                 feed_price_at_close=COALESCE(?, feed_price_at_close), feed_error=COALESCE(?, feed_error),
                 settle_price=COALESCE(?, settle_price), updated_ts=?
               WHERE ticker=?""",
            (result, settled_ts or time.time(), feed_price_at_close, feed_error, settle_price, time.time(), ticker))

    def set_settle_price(self, ticker: str, settle_price: float, feed_price_at_close: float | None,
                         err_lag0: float | None, err_lag60: float | None, err_chosen: float | None,
                         feed_source: str | None = None):
        """Record the settlement price learned from the next window's target, plus feed errors (signed, dollars)
        and which feed produced them."""
        self.conn.execute(
            """UPDATE windows SET settle_price=?, feed_price_at_close=COALESCE(?, feed_price_at_close),
                 feed_error_lag0=?, feed_error_lag60=?, feed_error=?, feed_source=COALESCE(?, feed_source), updated_ts=?
               WHERE ticker=?""",
            (settle_price, feed_price_at_close, err_lag0, err_lag60, err_chosen, feed_source, time.time(), ticker))

    def feed_errors(self, feed_source: str, limit: int = 200) -> list[float]:
        """Signed feed − settlement errors (newest first) measured with the given feed."""
        rows = self.conn.execute(
            "SELECT feed_error FROM windows WHERE feed_error IS NOT NULL AND feed_source=? ORDER BY close_time DESC LIMIT ?",
            (feed_source, limit)).fetchall()
        return [r[0] for r in rows]

    def feed_error_pairs(self, feed_source: str, limit: int = 200) -> list[tuple[float, float]]:
        """(error at the close, error one candle later), newest first, where both were measured."""
        rows = self.conn.execute(
            "SELECT feed_error_lag0, feed_error_lag60 FROM windows WHERE feed_error_lag0 IS NOT NULL AND feed_error_lag60 IS NOT NULL "
            "AND feed_source=? ORDER BY close_time DESC LIMIT ?", (feed_source, limit)).fetchall()
        return [(r[0], r[1]) for r in rows]

    def settled_window_count(self) -> int:
        row = self.conn.execute("SELECT COUNT(*) FROM windows WHERE result IN ('yes','no')").fetchone()
        return int(row[0] or 0)

    def prune(self, older_than_s: float = 3 * 86400):
        """Drop raw ticks/quotes older than a few days; everything the app needs later lives in windows/snapshots."""
        cutoff = time.time() - older_than_s
        self.conn.execute("DELETE FROM ticks WHERE ts < ?", (cutoff,))
        self.conn.execute("DELETE FROM quotes WHERE ts < ?", (cutoff,))

    def window(self, ticker: str) -> dict | None:
        row = self.conn.execute("SELECT * FROM windows WHERE ticker=?", (ticker,)).fetchone()
        return dict(row) if row else None

    def recent_windows(self, limit: int = 50) -> list[dict]:
        rows = self.conn.execute(
            """SELECT w.*,
                 (SELECT COUNT(*) FROM signals s WHERE s.ticker=w.ticker AND s.action='BUY') AS buy_signals,
                 (SELECT SUM(pnl) FROM paper_trades p WHERE p.ticker=w.ticker) AS paper_pnl,
                 (SELECT p_final FROM snapshots sn WHERE sn.ticker=w.ticker AND sn.seconds_left BETWEEN 540 AND 660
                    ORDER BY sn.ts DESC LIMIT 1) AS p_at_10min,
                 (SELECT p_final FROM snapshots sn WHERE sn.ticker=w.ticker AND sn.seconds_left BETWEEN 120 AND 240
                    ORDER BY sn.ts DESC LIMIT 1) AS p_at_3min
               FROM windows w ORDER BY w.close_time DESC LIMIT ?""", (limit,)).fetchall()
        return [dict(r) for r in rows]

    # ------------------------------------------------------------------ streams
    def add_tick(self, ts: float, source: str, price: float):
        self.conn.execute("INSERT INTO ticks (ts, source, price) VALUES (?,?,?)", (ts, source, price))

    def add_ticks(self, rows: list[tuple[float, str, float]]):
        self.conn.executemany("INSERT INTO ticks (ts, source, price) VALUES (?,?,?)", rows)

    def last_tick_before(self, ts: float, source: str, window_s: float = 5.0) -> float | None:
        row = self.conn.execute("SELECT price FROM ticks WHERE source=? AND ts<=? AND ts>=? ORDER BY ts DESC LIMIT 1",
                                (source, ts, ts - window_s)).fetchone()
        return row[0] if row else None

    def ticks_since(self, ts: float, source: str | None = None, limit: int = 20000) -> list[tuple[float, float]]:
        if source:
            rows = self.conn.execute("SELECT ts, price FROM ticks WHERE ts>=? AND source=? ORDER BY ts LIMIT ?",
                                     (ts, source, limit)).fetchall()
        else:
            rows = self.conn.execute("SELECT ts, price FROM ticks WHERE ts>=? ORDER BY ts LIMIT ?", (ts, limit)).fetchall()
        return [(r[0], r[1]) for r in rows]

    def add_quote(self, ts: float, m):
        self.conn.execute(
            "INSERT INTO quotes (ts, ticker, yes_bid, yes_ask, no_bid, no_ask, last_price, volume, open_interest) VALUES (?,?,?,?,?,?,?,?,?)",
            (ts, m.ticker, m.yes_bid, m.yes_ask, m.no_bid, m.no_ask, m.last_price, m.volume, m.open_interest))

    def quotes_for(self, ticker: str) -> list[dict]:
        rows = self.conn.execute("SELECT * FROM quotes WHERE ticker=? ORDER BY ts", (ticker,)).fetchall()
        return [dict(r) for r in rows]

    # ------------------------------------------------------------------ snapshots / learning
    def add_snapshot(self, ts: float, ticker: str, seconds_left: float, price: float, strike: float | None, pred) -> int:
        cur = self.conn.execute(
            """INSERT INTO snapshots (ts, ticker, seconds_left, price, strike, sigma, z, p_base, p_market, p_final, confidence, features)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?)""",
            (ts, ticker, seconds_left, price, strike, pred.sigma, pred.z, pred.p_base, pred.p_market, pred.p_final,
             pred.confidence, json.dumps(pred.features)))
        return int(cur.lastrowid)

    def label_snapshots(self, ticker: str, label: int):
        self.conn.execute("UPDATE snapshots SET label=? WHERE ticker=?", (label, ticker))

    def training_rows(self, ticker: str | None = None) -> tuple[np.ndarray, np.ndarray, list[dict]]:
        if ticker:
            rows = self.conn.execute("SELECT * FROM snapshots WHERE ticker=? AND label IS NOT NULL ORDER BY ts", (ticker,)).fetchall()
        else:
            rows = self.conn.execute("SELECT * FROM snapshots WHERE label IS NOT NULL ORDER BY ts").fetchall()
        X, y, meta = [], [], []
        for r in rows:
            try:
                feats = json.loads(r["features"] or "[]")
            except ValueError:
                continue
            if not feats:
                continue
            X.append(feats)
            y.append(float(r["label"]))
            meta.append(dict(r))
        return np.array(X, dtype=float), np.array(y, dtype=float), meta

    def labeled_tickers(self) -> list[str]:
        rows = self.conn.execute("SELECT ticker FROM snapshots WHERE label IS NOT NULL GROUP BY ticker ORDER BY MIN(ts)").fetchall()
        return [r[0] for r in rows]

    def training_rows_recent(self, n_windows: int = 50) -> tuple[np.ndarray, np.ndarray, int]:
        """Labeled snapshots of the last `n_windows` settled windows (pooled batch for the calibrator)."""
        rows = self.conn.execute(
            """SELECT ticker FROM windows WHERE result IN ('yes','no') ORDER BY close_time DESC LIMIT ?""", (n_windows,)).fetchall()
        tickers = [r[0] for r in rows]
        if not tickers:
            return np.zeros((0, 9)), np.zeros(0), 0
        marks = ",".join("?" * len(tickers))
        snaps = self.conn.execute(f"SELECT features, label, ticker FROM snapshots WHERE label IS NOT NULL AND ticker IN ({marks})",
                                  tickers).fetchall()
        X, y, seen = [], [], set()
        for r in snaps:
            try:
                feats = json.loads(r["features"] or "[]")
            except ValueError:
                continue
            if feats:
                X.append(feats)
                y.append(float(r["label"]))
                seen.add(r["ticker"])
        return np.array(X, dtype=float), np.array(y, dtype=float), len(seen)

    # ------------------------------------------------------------------ signals / paper
    def add_signal(self, ts: float, ticker: str, sig, pred, seconds_left: float | None):
        self.conn.execute(
            """INSERT INTO signals (ts, ticker, action, side, price, size, edge, p_side, p_market, confidence, headline, reasons, seconds_left)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (ts, ticker, sig.action, sig.side, sig.price, sig.size, sig.edge, sig.p_side, pred.p_market, sig.confidence,
             sig.headline, json.dumps(sig.reasons), seconds_left))

    def signals_for(self, ticker: str) -> list[dict]:
        rows = self.conn.execute("SELECT * FROM signals WHERE ticker=? ORDER BY ts", (ticker,)).fetchall()
        return [dict(r) for r in rows]

    def open_paper_trade(self, ticker: str, side: str, ts: float, price: float, size: int, fee: float,
                         confidence: str, p_side: float) -> int:
        cur = self.conn.execute(
            """INSERT INTO paper_trades (ticker, side, entry_ts, entry_price, size, entry_fee, confidence, p_side)
               VALUES (?,?,?,?,?,?,?,?)""", (ticker, side, ts, price, size, fee, confidence, p_side))
        return int(cur.lastrowid)

    def open_paper_trade_for(self, ticker: str) -> dict | None:
        row = self.conn.execute("SELECT * FROM paper_trades WHERE ticker=? AND exit_ts IS NULL ORDER BY id DESC LIMIT 1",
                                (ticker,)).fetchone()
        return dict(row) if row else None

    def close_paper_trade(self, trade_id: int, ts: float, price: float, fee: float, reason: str):
        row = self.conn.execute("SELECT * FROM paper_trades WHERE id=?", (trade_id,)).fetchone()
        if not row:
            return
        pnl = (price - row["entry_price"]) * row["size"] - row["entry_fee"] - fee
        self.conn.execute("UPDATE paper_trades SET exit_ts=?, exit_price=?, exit_fee=?, exit_reason=?, pnl=? WHERE id=?",
                          (ts, price, fee, reason, pnl, trade_id))

    def paper_trades(self, limit: int = 200) -> list[dict]:
        rows = self.conn.execute("SELECT * FROM paper_trades ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        return [dict(r) for r in rows]

    # ------------------------------------------------------------------ user positions
    def open_position(self, ticker: str, side: str, qty: float, price: float, ts: float, note: str = "",
                      amount: float | None = None, entry_fee: float = 0.0, high_bid: float | None = None) -> int:
        cur = self.conn.execute(
            "INSERT INTO positions (ticker, side, qty, avg_price, opened_ts, note, amount, entry_fee, high_bid) VALUES (?,?,?,?,?,?,?,?,?)",
            (ticker, side, qty, price, ts, note, amount, entry_fee, high_bid))
        return int(cur.lastrowid)

    def update_position_high(self, pos_id: int, high_bid: float):
        self.conn.execute("UPDATE positions SET high_bid=? WHERE id=?", (high_bid, pos_id))

    def update_position_target(self, pos_id: int, target: float):
        self.conn.execute("UPDATE positions SET target=? WHERE id=?", (target, pos_id))

    def update_position_zone(self, pos_id: int, low: float, high: float, ts: float):
        self.conn.execute("UPDATE positions SET target=?, target_high=?, zone_ts=? WHERE id=?", (low, high, ts, pos_id))

    def recent_amounts(self, limit: int = 10) -> list[float]:
        rows = self.conn.execute("SELECT amount FROM positions WHERE amount IS NOT NULL ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        return [float(r[0]) for r in rows]

    # ------------------------------------------------------------------ plans
    def add_plan(self, plan) -> int:
        cur = self.conn.execute(
            """INSERT INTO plans (ticker, side, limit_price, amount, shares, target, p_at_plan, tier, created_ts, status, status_text,
                                  expected_profit, target_high, p_target)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (plan.ticker, plan.side, plan.limit, plan.amount, plan.shares, plan.target, plan.p_at_plan, plan.tier,
             plan.created_ts, plan.status, plan.status_text, plan.expected_profit, plan.target_high, plan.p_target))
        return int(cur.lastrowid)

    def end_plan(self, plan_id: int, status: str, status_text: str, ended_ts: float, hit_target: bool | None,
                 best_bid: float | None, hypo_pnl: float | None):
        self.conn.execute("UPDATE plans SET status=?, status_text=?, ended_ts=?, hit_target=?, best_bid=?, hypo_pnl=? WHERE id=?",
                          (status, status_text, ended_ts, None if hit_target is None else int(hit_target), best_bid, hypo_pnl, plan_id))

    def open_plans(self) -> list[dict]:
        return [dict(r) for r in self.conn.execute("SELECT * FROM plans WHERE status='open' ORDER BY id").fetchall()]

    def plans(self, limit: int = 100) -> list[dict]:
        return [dict(r) for r in self.conn.execute("SELECT * FROM plans ORDER BY id DESC LIMIT ?", (limit,)).fetchall()]

    def plan_stats(self) -> dict:
        rows = self.conn.execute("SELECT tier, status, hit_target, hypo_pnl FROM plans WHERE status != 'open'").fetchall()
        out: dict = {"n": len(rows), "by_tier": {}}
        for r in rows:
            t = out["by_tier"].setdefault(r["tier"] or "?", {"n": 0, "hit": 0, "pnl": 0.0})
            t["n"] += 1
            t["hit"] += 1 if r["hit_target"] else 0
            t["pnl"] += r["hypo_pnl"] or 0.0
        for t in out["by_tier"].values():
            t["hit_rate"] = round(t["hit"] / t["n"], 3) if t["n"] else None
            t["pnl"] = round(t["pnl"], 2)
        out["hit"] = sum(1 for r in rows if r["hit_target"])
        out["pnl"] = round(sum((r["hypo_pnl"] or 0.0) for r in rows), 2)
        return out

    def delete_position(self, pos_id: int):
        self.conn.execute("DELETE FROM positions WHERE id=?", (pos_id,))

    def get_open_position(self) -> dict | None:
        row = self.conn.execute("SELECT * FROM positions WHERE closed_ts IS NULL ORDER BY id DESC LIMIT 1").fetchone()
        return dict(row) if row else None

    def close_position(self, pos_id: int, ts: float, exit_price: float, pnl: float):
        self.conn.execute("UPDATE positions SET closed_ts=?, exit_price=?, pnl=? WHERE id=?", (ts, exit_price, pnl, pos_id))

    def positions(self, limit: int = 100) -> list[dict]:
        rows = self.conn.execute("SELECT * FROM positions ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        return [dict(r) for r in rows]

    # ------------------------------------------------------------------ model state
    def get_state(self, key: str) -> str | None:
        row = self.conn.execute("SELECT value FROM model_state WHERE key=?", (key,)).fetchone()
        return row[0] if row else None

    def set_state(self, key: str, value: str):
        self.conn.execute("INSERT INTO model_state (key, value, updated_ts) VALUES (?,?,?) "
                          "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_ts=excluded.updated_ts",
                          (key, value, time.time()))

    # ------------------------------------------------------------------ stats
    def stats(self) -> dict[str, Any]:
        out: dict[str, Any] = {}
        row = self.conn.execute("SELECT COUNT(*) AS n, SUM(result='yes') AS ups FROM windows WHERE result IN ('yes','no')").fetchone()
        out["settled_windows"] = int(row["n"] or 0)
        out["up_share"] = (row["ups"] / row["n"]) if row["n"] else None
        buckets = {"10min": (540, 660), "5min": (240, 360), "2min": (90, 150), "1min": (30, 89)}
        brier: dict[str, Any] = {}
        for name, (lo, hi) in buckets.items():
            rows = self.conn.execute(
                """SELECT p_final, p_market, label FROM snapshots WHERE label IS NOT NULL AND seconds_left BETWEEN ? AND ?""",
                (lo, hi)).fetchall()
            if not rows:
                continue
            n = len(rows)
            bm = sum((r["p_final"] - r["label"]) ** 2 for r in rows) / n
            mk = [r for r in rows if r["p_market"] is not None]
            bk = sum((r["p_market"] - r["label"]) ** 2 for r in mk) / len(mk) if mk else None
            brier[name] = {"n": n, "model": round(bm, 4), "market": None if bk is None else round(bk, 4)}
        out["brier"] = brier
        # calibration: predicted bucket vs realized, at all labeled snapshots with seconds_left >= 60
        rows = self.conn.execute("SELECT p_final, label FROM snapshots WHERE label IS NOT NULL AND seconds_left >= 60").fetchall()
        cal: dict[str, dict] = {}
        for r in rows:
            b = min(9, int(r["p_final"] * 10))
            c = cal.setdefault(f"{b * 10}-{b * 10 + 10}%", {"n": 0, "up": 0})
            c["n"] += 1
            c["up"] += int(r["label"])
        out["calibration"] = {k: {"n": v["n"], "realized_up": round(v["up"] / v["n"], 3)} for k, v in sorted(cal.items(), key=lambda kv: int(kv[0].split("-")[0]))}
        # paper P&L and hit rate by confidence
        rows = self.conn.execute("SELECT confidence, pnl, p_side FROM paper_trades WHERE exit_ts IS NOT NULL").fetchall()
        by_conf: dict[str, dict] = {}
        total = 0.0
        wins = 0
        for r in rows:
            total += r["pnl"] or 0.0
            wins += 1 if (r["pnl"] or 0) > 0 else 0
            c = by_conf.setdefault(r["confidence"] or "?", {"n": 0, "wins": 0, "pnl": 0.0})
            c["n"] += 1
            c["wins"] += 1 if (r["pnl"] or 0) > 0 else 0
            c["pnl"] += r["pnl"] or 0.0
        out["paper"] = {"trades": len(rows), "wins": wins, "pnl": round(total, 2),
                        "by_confidence": {k: {"n": v["n"], "hit_rate": round(v["wins"] / v["n"], 3), "pnl": round(v["pnl"], 2)} for k, v in by_conf.items()}}
        rows = self.conn.execute("SELECT pnl FROM positions WHERE closed_ts IS NOT NULL").fetchall()
        out["real"] = {"trades": len(rows), "pnl": round(sum((r["pnl"] or 0.0) for r in rows), 2)}
        rows = self.conn.execute("SELECT feed_error FROM windows WHERE feed_error IS NOT NULL").fetchall()
        errs = [abs(r[0]) for r in rows]
        out["feed_error"] = {"n": len(errs), "mean_abs": round(sum(errs) / len(errs), 4) if errs else None,
                             "max_abs": round(max(errs), 4) if errs else None}
        rows = self.conn.execute("SELECT feed_error_lag0, feed_error_lag60 FROM windows "
                                 "WHERE feed_error_lag0 IS NOT NULL AND feed_error_lag60 IS NOT NULL").fetchall()
        if rows:
            out["feed_error"]["n_lag"] = len(rows)
            out["feed_error"]["lag0_mean_abs"] = round(sum(abs(r[0]) for r in rows) / len(rows), 4)
            out["feed_error"]["lag60_mean_abs"] = round(sum(abs(r[1]) for r in rows) / len(rows), 4)
        return out


def _ts(value) -> float | None:
    return None if value is None else value.timestamp()
