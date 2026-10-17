"""Kalshi history download for the 15-minute crypto markets (standalone, standard library only; never touches the
board's own databases).

Run from the kalshi-crypto folder:
    python3 tools/backfill.py --probe                     # ~20 requests for Bitcoin: answers the open questions, paste it
    python3 tools/backfill.py --series all --rps 5        # 60 days of all nine coins -> data/history-<SERIES>.zip each
    python3 tools/backfill.py --series KXETH15M           # one coin -> data/history-KXETH15M.db/.zip (safe to stop, re-run)

Pulls the free, unauthenticated Kalshi history for a 15-minute series into a separate SQLite file:
  * every settled window: target (floor_strike), settlement (expiration_value), result, open/close, volume
  * 1-minute Kalshi candlesticks per window: yes_bid / yes_ask OHLC, trade-price OHLC, volume, open interest
  * Kalshi's second-by-second chart series per window (live_data), if Kalshi serves it for crypto (--probe checks)
  * optional (--trades): every public trade per window

A copy of kalshi-wti/tools/backfill.py (endpoints and parameters from Kalshi's official Python SDK, in which every
endpoint used here has no auth settings), with second-by-second prices stored precisely enough for Dogecoin's
7-decimal targets and the oil-only extras (Hyperliquid, Pyth) left out.

Usage:  python3 tools/backfill.py [--series KXBTC15M|all] [--days 60] [--out data/history-<SERIES>.db] [--trades]
        python3 tools/backfill.py --probe [--series KXBTC15M]
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib

KALSHI = "https://api.elections.kalshi.com/trade-api/v2"
UA = {"Accept": "application/json", "User-Agent": "cx15m-backfill/1.0"}
CRYPTO_SERIES = ("KXBTC15M", "KXETH15M", "KXSOL15M", "KXXRP15M", "KXDOGE15M", "KXHYPE15M", "KXNEAR15M", "KXZEC15M",
                 "KXBNB15M")
BATCH = 50  # tickers per batch-candles call (API max 100)
CANDLE_CAP = 8000  # Kalshi refuses a batch when markets x minutes in its time span exceeds 10,000 (seen 2026-10-13)

SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS hist_windows (
  ticker TEXT PRIMARY KEY, series TEXT, event_ticker TEXT, open_ts REAL, close_ts REAL,
  target REAL, settle REAL, result TEXT, status TEXT, settlement_ts REAL, volume REAL, open_interest REAL,
  rules_primary TEXT, tier TEXT, raw TEXT);
CREATE INDEX IF NOT EXISTS hist_windows_close ON hist_windows(series, close_ts);
CREATE TABLE IF NOT EXISTS kalshi_candles (
  ticker TEXT, end_ts INTEGER,
  bid_o REAL, bid_h REAL, bid_l REAL, bid_c REAL, ask_o REAL, ask_h REAL, ask_l REAL, ask_c REAL,
  px_o REAL, px_h REAL, px_l REAL, px_c REAL, px_mean REAL, volume REAL, open_interest REAL,
  PRIMARY KEY (ticker, end_ts));
CREATE TABLE IF NOT EXISTS candles_done (ticker TEXT PRIMARY KEY, n INTEGER, fetched_ts REAL);
CREATE TABLE IF NOT EXISTS pyth_points (event_ticker TEXT, ts REAL, price REAL, PRIMARY KEY (event_ticker, ts));
CREATE TABLE IF NOT EXISTS livedata_done (event_ticker TEXT PRIMARY KEY, n_total INTEGER, n_in_window INTEGER, fetched_ts REAL);
CREATE TABLE IF NOT EXISTS price_paths (event_ticker TEXT PRIMARY KEY, n INTEGER, first_ts REAL, last_ts REAL, data BLOB);
CREATE TABLE IF NOT EXISTS trades (trade_id TEXT PRIMARY KEY, ticker TEXT, ts REAL, yes_price REAL, count REAL, taker_side TEXT);
CREATE TABLE IF NOT EXISTS trades_done (ticker TEXT PRIMARY KEY, n INTEGER, fetched_ts REAL);
CREATE TABLE IF NOT EXISTS pyth_candles (symbol TEXT, t INTEGER, o REAL, h REAL, l REAL, c REAL,
  PRIMARY KEY (symbol, t));
CREATE TABLE IF NOT EXISTS pyth_done (symbol TEXT, chunk_from INTEGER, n INTEGER, status TEXT, PRIMARY KEY (symbol, chunk_from));
CREATE TABLE IF NOT EXISTS hl_candles (coin TEXT, t_open INTEGER, t_close INTEGER, o REAL, h REAL, l REAL, c REAL, v REAL,
  PRIMARY KEY (coin, t_open));
"""


# ----------------------------------------------------------------------------- HTTP with pacing and retries

class HttpError(RuntimeError):
    def __init__(self, status: int, msg: str):
        super().__init__(msg)
        self.status = status


class Http:
    def __init__(self, rps: float = 8.0, timeout: float = 20.0, opener=None, sleep=time.sleep, clock=time.monotonic):
        self.min_gap = 1.0 / rps
        self.timeout = timeout
        self.opener = opener or self._urlopen  # (method, url, body) -> (status, headers, bytes)
        self.sleep, self.clock = sleep, clock
        self._last = float("-inf")
        self.count = 0
        self.retries = 0

    def _urlopen(self, method, url, body, headers=None):
        req = urllib.request.Request(url, data=body, method=method,
                                     headers={**UA, **({"Content-Type": "application/json"} if body else {}), **(headers or {})})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers or {}), e.read() if hasattr(e, "read") else b""

    def request(self, method: str, url: str, params: dict | None = None, body: dict | None = None, headers: dict | None = None):
        if params:
            url = f"{url}?{urllib.parse.urlencode({k: v for k, v in params.items() if v is not None})}"
        data = json.dumps(body).encode() if body is not None else None
        for attempt in range(7):
            wait = self.min_gap - (self.clock() - self._last)
            if wait > 0:
                self.sleep(wait)
            self._last = self.clock()
            self.count += 1
            try:
                status, rh, raw = (self.opener(method, url, data, headers) if headers else self.opener(method, url, data))
            except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
                status, rh, raw = 0, {}, str(exc).encode()
            if status == 200:
                return json.loads(raw or b"null")
            if status == 404:
                return None
            if status in (0, 429) or status >= 500:
                self.retries += 1
                retry_after = rh.get("Retry-After") or rh.get("retry-after")
                try:
                    delay = float(retry_after) if retry_after else min(30.0, 2.0 ** attempt)
                except ValueError:
                    delay = min(30.0, 2.0 ** attempt)
                self.sleep(delay)
                continue
            raise HttpError(status, f"{method} {url.split('?')[0]} -> HTTP {status}: {raw[:200]!r}")
        raise RuntimeError(f"{method} {url}: gave up after retries")

    def get(self, path: str, params: dict | None = None, base: str = KALSHI):
        return self.request("GET", f"{base}{path}", params)


# ----------------------------------------------------------------------------- parsing helpers

def fnum(v):
    if v in (None, ""):
        return None
    try:
        return float(str(v).replace(",", "").replace("$", ""))
    except (TypeError, ValueError):
        return None


def ts_of(v):
    """RFC3339 string or epoch (s or ms) -> epoch seconds."""
    if v in (None, ""):
        return None
    if isinstance(v, (int, float)):
        return v / 1000.0 if v > 1e11 else float(v)
    s = str(v).replace("Z", "+00:00")
    try:
        return dt.datetime.fromisoformat(s).timestamp()
    except ValueError:
        f = fnum(v)
        return None if f is None else (f / 1000.0 if f > 1e11 else f)


def dist(c: dict, name: str, field: str):
    """Candle sub-field, live shape (yes_bid.close_dollars) or archived shape (yes_bid.close)."""
    d = c.get(name) or {}
    return fnum(d.get(f"{field}_dollars", d.get(field)))


def candle_row(ticker: str, c: dict):
    return (ticker, int(c.get("end_period_ts")),
            dist(c, "yes_bid", "open"), dist(c, "yes_bid", "high"), dist(c, "yes_bid", "low"), dist(c, "yes_bid", "close"),
            dist(c, "yes_ask", "open"), dist(c, "yes_ask", "high"), dist(c, "yes_ask", "low"), dist(c, "yes_ask", "close"),
            dist(c, "price", "open"), dist(c, "price", "high"), dist(c, "price", "low"), dist(c, "price", "close"),
            dist(c, "price", "mean"), fnum(c.get("volume_fp", c.get("volume"))), fnum(c.get("open_interest_fp", c.get("open_interest"))))


def window_row(series: str, m: dict, tier: str):
    target = fnum(m.get("floor_strike"))
    return (m.get("ticker"), series, m.get("event_ticker"), ts_of(m.get("open_time")), ts_of(m.get("close_time")),
            target, fnum(m.get("expiration_value")), (m.get("result") or "").lower() or None, m.get("status"),
            ts_of(m.get("settlement_ts")), fnum(m.get("volume_fp", m.get("volume"))),
            fnum(m.get("open_interest_fp", m.get("open_interest"))), None, tier,
            json.dumps({k: v for k, v in m.items() if not k.startswith("rules")}))  # the rules text is in meta once


# ----------------------------------------------------------------------------- steps

def list_markets(http: Http, series: str, start_ts: float, cutoff_ts: float | None):
    """Settled markets closing at or after start_ts, live tier first, then the archive if start is before the cutoff."""
    out = []
    cursor = None
    while True:
        data = http.get("/markets", {"series_ticker": series, "status": "settled", "min_settled_ts": int(start_ts),
                                     "limit": 1000, "cursor": cursor}) or {}
        out += [(m, "live") for m in data.get("markets") or []]
        cursor = data.get("cursor")
        if not cursor:
            break
    if cutoff_ts is not None and start_ts < cutoff_ts:
        cursor = None
        while True:  # /historical/markets takes no time filter (filters are mutually exclusive): page until older than start
            data = http.get("/historical/markets", {"series_ticker": series, "limit": 1000, "cursor": cursor}) or {}
            page = data.get("markets") or []
            out += [(m, "archived") for m in page if (ts_of(m.get("close_time")) or 0) >= start_ts]
            cursor = data.get("cursor")
            closes = [ts_of(m.get("close_time")) or 0 for m in page]
            # UNCONFIRMED: archive order. Stop early only if the whole page is older than the start.
            if not cursor or (closes and max(closes) < start_ts):
                break
    seen, uniq = set(), []
    for m, tier in out:
        if m.get("ticker") and m["ticker"] not in seen:
            seen.add(m["ticker"])
            uniq.append((m, tier))
    return uniq


def fetch_candles(http: Http, db: sqlite3.Connection, series: str, log=print):
    done = {r[0] for r in db.execute("SELECT ticker FROM candles_done")}
    rows = db.execute("SELECT ticker, open_ts, close_ts, tier FROM hist_windows WHERE series=? ORDER BY close_ts", (series,)).fetchall()
    todo = [r for r in rows if r[0] not in done and r[1] and r[2]]
    live = [r for r in todo if r[3] == "live"]
    archived = [r for r in todo if r[3] != "live"]

    def store(ticker, candles):
        db.executemany("INSERT OR REPLACE INTO kalshi_candles VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                       [candle_row(ticker, c) for c in candles if c.get("end_period_ts") is not None])
        db.execute("INSERT OR REPLACE INTO candles_done VALUES (?,?,?)", (ticker, len(candles), time.time()))

    def single(t, o, c, archived_tier):
        params = {"start_ts": int(o) - 60, "end_ts": int(c) + 60, "period_interval": 1}
        path = f"/historical/markets/{t}/candlesticks" if archived_tier else f"/series/{series}/markets/{t}/candlesticks"
        try:
            data = http.get(path, params) or {}
        except HttpError as exc:  # one odd window must not stop the run; it is retried on the next run
            log(f"  candles: {t} skipped ({exc})")
            return
        store(t, data.get("candlesticks") or [])

    def minutes(batch):
        return (int(max(r[2] for r in batch)) + 60 - (int(min(r[1] for r in batch)) - 60)) // 60 + 1

    # Kalshi counts a batch as (markets x minutes between the earliest start and the latest end) and refuses more than
    # 10,000, so batches are built from neighbouring windows and kept well under that
    batches, cur = [], []
    for r in live:
        if cur and (len(cur) >= BATCH or (len(cur) + 1) * minutes(cur + [r]) > CANDLE_CAP):
            batches.append(cur)
            cur = []
        cur.append(r)
    if cur:
        batches.append(cur)
    n_done = 0
    for b_i, chunk in enumerate(batches):
        params = {"market_tickers": ",".join(r[0] for r in chunk), "start_ts": int(min(r[1] for r in chunk)) - 60,
                  "end_ts": int(max(r[2] for r in chunk)) + 60, "period_interval": 1}
        try:
            data = http.get("/markets/candlesticks", params) or {}
        except HttpError as exc:
            if exc.status != 400:
                raise
            data = None  # refused: one call per market instead
        got = {m.get("market_ticker"): m.get("candlesticks") or [] for m in (data or {}).get("markets") or []}
        total = sum(len(v) for v in got.values())
        if data is None or total >= 10000:  # refused, or hit the response cap: one call per market
            for t, o, c, _ in chunk:
                single(t, o, c, False)
        else:
            for t, o, c, _ in chunk:
                if t in got:
                    store(t, got[t])
                else:
                    single(t, o, c, False)
        db.commit()
        n_done += len(chunk)
        if len(batches) < 20 or n_done == len(live) or b_i % 20 == 0:
            log(f"  candles: {n_done}/{len(live)} windows")
    for j, (t, o, c, _) in enumerate(archived):
        single(t, o, c, True)
        if j % 100 == 0:
            db.commit()
            log(f"  candles: {j + 1}/{len(archived)} archived windows")
    db.commit()


PATH_SCALE = 100000  # prices stored to 1e-5 (the WTI tool's fixed scale; still read from older files)


def path_scale(price: float) -> int:
    """Integer steps per dollar: 1e-8 under $10 (Dogecoin's targets have 7 decimals), 1e-5 above."""
    return 100_000_000 if abs(price) < 10 else PATH_SCALE


def encode_path(pts: list[tuple[float, float]]) -> bytes:
    """Second-by-second points as zlib-compressed integer deltas (~1-2 KB per 15-minute window)."""
    scale = path_scale(pts[0][1])
    t = [round(x * 1000) for x, _ in pts]
    p = [round(y * scale) for _, y in pts]
    body = {"t0": t[0], "p0": p[0], "s": scale, "dt": [b - a for a, b in zip(t, t[1:])], "dp": [b - a for a, b in zip(p, p[1:])]}
    return zlib.compress(json.dumps(body, separators=(",", ":")).encode(), 9)


def decode_path(blob: bytes) -> list[tuple[float, float]]:
    d = json.loads(zlib.decompress(blob))
    scale = d.get("s", PATH_SCALE)
    t, p = [d["t0"]], [d["p0"]]
    for a in d["dt"]:
        t.append(t[-1] + a)
    for b in d["dp"]:
        p.append(p[-1] + b)
    return [(x / 1000.0, y / scale) for x, y in zip(t, p)]


def fetch_livedata(http: Http, db: sqlite3.Connection, series: str, log=print, stop_after_empty: int = 12):
    """Kalshi's second-by-second settlement price (the Pyth series) for each window, from /live_data/events/{event}.
    For a past event Kalshi returns the hour that ended at its close (probe of 2026-10-13: 3,600 points, still served
    for a week-old window), so one request covers that window and the three before it: windows are fetched newest
    first, each fetch fills every window it fully covers, and the run stops after `stop_after_empty` events in a row
    come back empty (the end of what Kalshi keeps)."""
    have = {r[0] for r in db.execute("SELECT event_ticker FROM price_paths")}
    rows = db.execute("SELECT DISTINCT event_ticker, open_ts, close_ts FROM hist_windows WHERE series=? AND event_ticker IS NOT NULL "
                      "AND open_ts IS NOT NULL AND close_ts IS NOT NULL ORDER BY close_ts DESC", (series,)).fetchall()
    todo = [r for r in rows if r[0] not in have]
    empty = fetched = 0
    oldest = None
    for k, (ev, o, c) in enumerate(todo):
        if ev in have:
            continue
        try:
            pts = parse_live(http.get(f"/live_data/events/{ev}", {"range": "1h"}))
        except HttpError as exc:
            log(f"  live prices: {ev} skipped ({exc})")
            continue
        fetched += 1
        if not pts:
            empty += 1
            db.execute("INSERT OR REPLACE INTO livedata_done VALUES (?,?,?,?)", (ev, 0, 0, time.time()))
            if empty >= stop_after_empty:
                log(f"  live prices: {empty} windows in a row came back empty -> Kalshi keeps them back to about {oldest}; stopping")
                break
            continue
        empty = 0
        lo, hi = pts[0][0], pts[-1][0]
        for ev2, o2, c2 in todo[k:]:  # this window and the older ones this hour of prices fully covers
            if c2 < lo:
                break
            if ev2 in have or not (lo <= o2 + 1 and hi >= c2 - 3):
                continue
            inside = [(t, p) for t, p in pts if o2 - 60 <= t <= c2 + 60]
            if len(inside) < 0.8 * (c2 - o2):
                continue
            db.execute("INSERT OR REPLACE INTO price_paths VALUES (?,?,?,?,?)",
                       (ev2, len(inside), inside[0][0], inside[-1][0], encode_path(inside)))
            have.add(ev2)
            oldest = dt.datetime.fromtimestamp(o2).strftime("%Y-%m-%d")
        db.execute("INSERT OR REPLACE INTO livedata_done VALUES (?,?,?,?)", (ev, len(pts), 0, time.time()))
        if fetched % 50 == 0:
            db.commit()
            log(f"  live prices: {len(have)}/{len(rows)} windows (back to {oldest}), {fetched} requests")
    db.commit()
    log(f"  live prices: {len(have)}/{len(rows)} windows have their second-by-second price ({fetched} requests)")


def parse_live(data) -> list[tuple[float, float]]:
    ld = (data or {}).get("live_data", data) if isinstance(data, dict) else {}
    details = ld.get("details", ld) if isinstance(ld, dict) else {}
    series = details.get("timeseries") if isinstance(details, dict) else None
    out = []
    for pt in series or []:
        t = ts_of(pt.get("t") if isinstance(pt, dict) else pt[0])
        v = fnum(pt.get("v") if isinstance(pt, dict) else pt[1])
        if t and v and v > 0:
            out.append((t, v))
    return sorted(out)


def fetch_trades(http: Http, db: sqlite3.Connection, series: str, log=print):
    done = {r[0] for r in db.execute("SELECT ticker FROM trades_done")}
    rows = db.execute("SELECT ticker, tier FROM hist_windows WHERE series=? ORDER BY close_ts", (series,)).fetchall()
    for k, (t, tier) in enumerate(r for r in rows if r[0] not in done):
        path = "/historical/trades" if tier != "live" else "/markets/trades"
        cursor, n = None, 0
        while True:
            data = http.get(path, {"ticker": t, "limit": 1000, "cursor": cursor}) or {}
            tr = data.get("trades") or []
            db.executemany("INSERT OR IGNORE INTO trades VALUES (?,?,?,?,?,?)",
                           [(x.get("trade_id"), t, ts_of(x.get("created_time")), fnum(x.get("yes_price_dollars", x.get("yes_price"))),
                             fnum(x.get("count_fp", x.get("count"))), x.get("taker_outcome_side") or x.get("taker_side")) for x in tr])
            n += len(tr)
            cursor = data.get("cursor")
            if not cursor or not tr:
                break
        db.execute("INSERT OR REPLACE INTO trades_done VALUES (?,?,?)", (t, n, time.time()))
        if k % 50 == 0:
            db.commit()
            log(f"  trades: {k + 1} windows")
    db.commit()


def chain_check(db: sqlite3.Connection, series: str) -> tuple[int, int]:
    rows = db.execute("SELECT open_ts, close_ts, target, settle FROM hist_windows WHERE series=? AND result IN ('yes','no') "
                      "ORDER BY close_ts", (series,)).fetchall()
    pairs = ok = 0
    for a, b in zip(rows, rows[1:]):
        if abs(b[0] - a[1]) < 1 and a[3] is not None and b[2] is not None:
            pairs += 1
            ok += abs(b[2] - a[3]) < 1e-9
    return pairs, ok


def run(args, http: Http | None = None, log=print) -> sqlite3.Connection:
    http = http or Http(args.rps)
    if os.path.dirname(args.out):
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
    db = sqlite3.connect(args.out)
    db.executescript(SCHEMA)
    series = args.series
    now = time.time() if args.now is None else args.now
    start = now - args.days * 86400
    s = http.get(f"/series/{series}") or {}
    db.execute("INSERT OR REPLACE INTO meta VALUES ('series:' || ?, ?)", (series, json.dumps(s.get("series", s))))
    cut = http.get("/historical/cutoff") or {}
    cutoff_ts = ts_of(cut.get("market_settled_ts"))
    db.execute("INSERT OR REPLACE INTO meta VALUES ('cutoff', ?)", (json.dumps(cut),))
    log(f"{series}: archive cutoff {cut.get('market_settled_ts')}; listing settled windows since {dt.datetime.fromtimestamp(start)}")
    ms = list_markets(http, series, start, cutoff_ts)
    db.executemany("INSERT OR REPLACE INTO hist_windows VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", [window_row(series, m, tier) for m, tier in ms])
    if ms:
        db.execute("INSERT OR REPLACE INTO meta VALUES ('rules_primary_sample', ?)", (ms[0][0].get("rules_primary") or "",))
    db.commit()
    pairs, ok = chain_check(db, series)
    db.execute("INSERT OR REPLACE INTO meta VALUES ('chain_check', ?)", (json.dumps({"pairs": pairs, "target_eq_prev_settle": ok}),))
    log(f"  {len(ms)} settled windows ({sum(t == 'archived' for _, t in ms)} archived); chain check {ok}/{pairs}")
    fetch_candles(http, db, series, log)
    if args.livedata:
        fetch_livedata(http, db, series, log)
    if args.trades:
        fetch_trades(http, db, series, log)
    db.execute("INSERT OR REPLACE INTO meta VALUES ('last_run', ?)", (json.dumps({"ts": now, "days": args.days, "requests": http.count,
                                                                                 "retries": http.retries}),))
    db.commit()
    log(f"done: {http.count} requests, {http.retries} retries")
    if getattr(args, "zip", True):
        db.commit()
        zpath = os.path.splitext(args.out)[0] + ".zip"
        with zipfile.ZipFile(zpath, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            z.write(args.out, arcname=os.path.basename(args.out))
        log(f"send this file: {os.path.abspath(zpath)} ({os.path.getsize(zpath) / 1e6:.1f} MB)")
    return db


def probe(args, http: Http | None = None, log=print):
    """Answer the open questions with a handful of requests before a long run."""
    http = http or Http(args.rps)
    cut = http.get("/historical/cutoff")
    log(f"cutoff: {cut}")
    page = (http.get("/markets", {"series_ticker": args.series, "status": "settled", "limit": 5}) or {}).get("markets") or []
    log(f"newest settled (live tier): {[m.get('ticker') for m in page]}")
    if page:
        m = page[0]
        log(f"  fields: floor_strike={m.get('floor_strike')} expiration_value={m.get('expiration_value')} result={m.get('result')} "
            f"settlement_ts={m.get('settlement_ts')} open={m.get('open_time')} close={m.get('close_time')}")
        o, c = ts_of(m.get("open_time")), ts_of(m.get("close_time"))
        cs = (http.get(f"/series/{args.series}/markets/{m['ticker']}/candlesticks",
                       {"start_ts": int(o) - 3600, "end_ts": int(c) + 3600, "period_interval": 1}) or {}).get("candlesticks") or []
        log(f"  1-minute candles returned for a 15-minute window (+-1 h asked): {len(cs)}; first/last end_ts "
            f"{cs[0].get('end_period_ts') if cs else None}/{cs[-1].get('end_period_ts') if cs else None}; sample {cs[-1] if cs else None}")
        for back_h in (0.5, 6, 48, 24 * 7):
            target_close = c - back_h * 3600
            older = (http.get("/markets", {"series_ticker": args.series, "status": "settled", "max_settled_ts": int(target_close + 120),
                                           "min_settled_ts": int(target_close - 900), "limit": 1}) or {}).get("markets") or []
            if not older:
                log(f"  live_data {back_h} h back: no market found")
                continue
            ev, oo, cc = older[0].get("event_ticker"), ts_of(older[0].get("open_time")), ts_of(older[0].get("close_time"))
            pts = parse_live(http.get(f"/live_data/events/{ev}", {"range": "15min"}))
            inside = [p for p in pts if oo - 60 <= p[0] <= cc + 60]
            log(f"  live_data for {ev} ({back_h} h old): {len(pts)} points, {len(inside)} inside its own window "
                f"(span {dt.datetime.fromtimestamp(pts[0][0]) if pts else None} .. {dt.datetime.fromtimestamp(pts[-1][0]) if pts else None})")
        # UNCONFIRMED: does a longer range hint serve a trailing day of the Pyth series (a free daily history pull)?
        for rng in ("1h", "1d", "1w"):
            pts = parse_live(http.get(f"/live_data/events/{m.get('event_ticker')}", {"range": rng}))
            span = (pts[-1][0] - pts[0][0]) / 3600 if len(pts) > 1 else 0
            log(f"  live_data range={rng} for {m.get('event_ticker')}: {len(pts)} points spanning {span:.2f} h")
    arch = (http.get("/historical/markets", {"series_ticker": args.series, "limit": 5}) or {}).get("markets") or []
    log(f"archive sample: {[(m.get('ticker'), m.get('close_time')) for m in arch]}")
    log(f"requests used: {http.count}")


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument("--days", type=float, default=60)
    p.add_argument("--series", default="KXBTC15M", help="a series ticker, several separated by commas, or all")
    p.add_argument("--out", default=None, help="default data/history-<SERIES>.db")
    p.add_argument("--rps", type=float, default=8.0)
    p.add_argument("--trades", action="store_true")
    p.add_argument("--no-livedata", dest="livedata", action="store_false",
                   help="skip the second-by-second chart series (on by default)")
    p.add_argument("--probe", action="store_true")
    p.add_argument("--now", type=float, default=None, help=argparse.SUPPRESS)
    args = p.parse_args(argv)
    names = CRYPTO_SERIES if args.series.strip().lower() == "all" else [x.strip().upper() for x in args.series.split(",") if x.strip()]
    if args.probe:
        args.series = names[0]
        probe(args)
        return 0
    out = args.out
    for name in names:
        args.series = name
        args.out = out if (out and len(names) == 1) else f"data/history-{name}.db"
        run(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
