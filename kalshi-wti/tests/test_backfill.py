"""tools/backfill.py against a fake Kalshi API (synthetic windows; no network)."""
import datetime as dt
import importlib.util
import json
import pathlib
import random
import urllib.parse

spec = importlib.util.spec_from_file_location("backfill", pathlib.Path(__file__).resolve().parents[1] / "tools" / "backfill.py")
bf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bf)

T0 = 1_791_000_000 // 900 * 900


def iso(ts):
    return dt.datetime.fromtimestamp(ts, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class FakeKalshi:
    """12 chained 15-minute windows (target = previous settlement); the first 4 sit in the archive tier."""

    def __init__(self, n=12, gap_every=0):
        rnd = random.Random(3)
        px = 90.0
        self.W = []
        o = T0 - 900
        for i in range(n):
            o += 900 + (3600 if gap_every and i and i % gap_every == 0 else 0)  # the daily trading break
            target = round(px, 2)
            px += rnd.gauss(0, 0.08)
            settle = round(px, 2)
            self.W.append((f"KXWTI15M-W{i:02d}", f"KXWTI15M-E{i:02d}", o, o + 900, target, settle, "yes" if settle >= target else "no"))
        self.cutoff = self.W[3][3] + 1
        self.calls = []
        self.fail_next_429 = 1
        self.live_since = 0

    def price(self, t):
        return round(90 + 0.3 * ((t * 7919) % 1000) / 1000 + (t - T0) * 1e-5, 5)

    def market(self, w, archived):
        tk, ev, o, cl, k, s, r = w
        m = {"ticker": tk, "event_ticker": ev, "open_time": iso(o), "close_time": iso(cl), "status": "finalized",
             "result": r, "floor_strike": k, "expiration_value": f"{s:.2f}", "settlement_ts": iso(cl + 30)}
        m.update({"volume": "100.00"} if archived else {"volume_fp": "100.00"})
        return m

    def candles(self, tk, start, end, archived):
        w = next(x for x in self.W if x[0] == tk)
        sfx = "" if archived else "_dollars"
        out = []
        for m_end in range(int(w[2]) + 60, int(w[3]) + 1, 60):
            if start <= m_end <= end:
                d = {f"open{sfx}": "0.4800", f"high{sfx}": "0.5200", f"low{sfx}": "0.4700", f"close{sfx}": "0.5000"}
                out.append({"end_period_ts": m_end, "yes_bid": d, "yes_ask": d, "price": {f"close{sfx}": None}})
        return out

    def __call__(self, method, url, body, headers=None):
        u = urllib.parse.urlparse(url)
        path = u.path.replace("/trade-api/v2", "")
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        self.calls.append(path)
        ok = lambda obj: (200, {}, json.dumps(obj).encode())  # noqa: E731
        if path == "/markets" and self.fail_next_429:
            self.fail_next_429 -= 1
            return 429, {"Retry-After": "1"}, b"slow down"
        if path.startswith("/series/") and path.count("/") == 2:
            return ok({"series": {"ticker": "KXWTI15M"}})
        if path == "/historical/cutoff":
            return ok({"market_settled_ts": iso(self.cutoff)})
        if path == "/markets":
            lo = float(q.get("min_settled_ts", 0))
            ms = [self.market(w, False) for w in reversed(self.W) if w[3] >= self.cutoff and w[3] + 30 >= lo]
            start, lim = int(q.get("cursor") or 0), min(int(q.get("limit", 100)), 5)  # force pagination
            return ok({"markets": ms[start:start + lim], "cursor": str(start + lim) if start + lim < len(ms) else ""})
        if path == "/historical/markets":
            return ok({"markets": [self.market(w, True) for w in reversed(self.W) if w[3] < self.cutoff], "cursor": ""})
        if path == "/markets/candlesticks":
            tks = q["market_tickers"].split(",")
            asked = len(tks) * ((int(q["end_ts"]) - int(q["start_ts"])) // 60 + 1)
            if asked > 10000:  # Kalshi's real rule (seen 2026-10-13)
                return 400, {}, json.dumps({"error": {"code": "bad_request", "details": f"requested candlesticks across all markets: {asked}, max candlesticks: 10000"}}).encode()
            return ok({"markets": [{"market_ticker": t, "candlesticks": self.candles(t, int(q["start_ts"]), int(q["end_ts"]), False)}
                                   for t in tks]})
        if path.startswith("/series/") and path.endswith("/candlesticks"):
            t = path.split("/")[4]
            return ok({"candlesticks": self.candles(t, int(q["start_ts"]), int(q["end_ts"]), False)})
        if path.startswith("/historical/markets/") and path.endswith("/candlesticks"):
            t = path.split("/")[3]
            return ok({"candlesticks": self.candles(t, int(q["start_ts"]), int(q["end_ts"]), True)})
        if path.startswith("/live_data/events/"):
            ev = path.split("/")[3]
            w = next(x for x in self.W if x[1] == ev)
            if w[3] <= self.live_since:  # older than what Kalshi keeps
                return ok({"live_data": {"details": {"timeseries": []}}})
            # a past event serves the hour that ended at its close, one point a second (ms timestamps)
            pts = [{"t": int(t * 1000), "v": self.price(t)} for t in range(int(w[3]) - 3600, int(w[3]))]
            return ok({"live_data": {"type": "commodity", "details": {"timeseries": pts}}})
        return 404, {}, b"not found"


class Args:
    pyth = False
    pyth_key = None
    days = 1
    series = "KXWTI15M"
    rps = 1000.0
    trades = False
    livedata = False
    hyperliquid = False
    zip = False


def test_backfill_stores_every_window_its_candles_and_resumes(tmp_path):
    fake = FakeKalshi()
    args = Args()
    args.out = str(tmp_path / "sub" / "history.db")
    args.now = fake.W[-1][3] + 600
    slept = []
    db = bf.run(args, bf.Http(rps=1000.0, opener=fake, sleep=slept.append), log=lambda *a: None)
    assert db.execute("SELECT COUNT(*), SUM(tier='archived') FROM hist_windows").fetchone() == (12, 4)
    assert bf.chain_check(db, "KXWTI15M") == (11, 11)
    assert db.execute("SELECT target, settle, result FROM hist_windows WHERE ticker='KXWTI15M-W05'").fetchone() == fake.W[5][4:7]
    per = dict(db.execute("SELECT ticker, COUNT(*) FROM kalshi_candles GROUP BY ticker").fetchall())
    assert len(per) == 12 and set(per.values()) == {15}
    assert 1.0 in slept  # honoured Retry-After on the 429
    fake.calls.clear()  # a second run fetches no candles again
    bf.run(args, bf.Http(rps=1000.0, opener=fake, sleep=lambda s: None), log=lambda *a: None)
    assert not [c for c in fake.calls if "candlesticks" in c]


def test_pacing_stays_under_the_rate():
    t = [0.0]

    def sleep(s):
        t[0] += s

    http = bf.Http(rps=8.0, opener=lambda m, u, b: (200, {}, b"{}"), sleep=sleep, clock=lambda: t[0])
    for _ in range(9):
        http.get("/x")
    assert abs(t[0] - 1.0) < 1e-9


def test_probe_runs_and_reports():
    fake = FakeKalshi()
    fake.fail_next_429 = 0
    lines = []
    bf.probe(Args(), bf.Http(rps=1000.0, opener=fake, sleep=lambda s: None), log=lines.append)
    assert any("cutoff" in x for x in lines) and any("requests used" in x for x in lines)


def test_second_by_second_prices_cover_four_windows_per_request_and_stop_when_kalshi_has_none(tmp_path):
    fake = FakeKalshi()
    fake.fail_next_429 = 0
    args = Args()
    args.out, args.livedata, args.zip = str(tmp_path / "history.db"), True, True
    args.now = fake.W[-1][3] + 600
    lines = []
    db = bf.run(args, bf.Http(rps=1000.0, opener=fake, sleep=lambda s: None), log=lines.append)
    got = dict(db.execute("SELECT event_ticker, data FROM price_paths").fetchall())
    assert set(got) == {w[1] for w in fake.W}
    assert sum(1 for c in fake.calls if c.startswith("/live_data/")) == 3  # each hour of prices covers four windows
    w = fake.W[7]
    path = bf.decode_path(got[w[1]])
    assert path[0][0] == w[2] - 60 and path[-1][0] == w[3] - 1 and len(path) == 960  # the hour ends at the close
    assert all(abs(p - fake.price(t)) < 1e-9 for t, p in path)
    assert (tmp_path / "history.zip").exists() and any("send this file" in x for x in lines)

    # Kalshi keeps the series only for recent windows: older events come back empty and the run stops after a few
    fake2 = FakeKalshi()
    fake2.fail_next_429 = 0
    fake2.live_since = fake2.W[7][3]
    args2 = Args()
    args2.out, args2.now = str(tmp_path / "h2.db"), fake2.W[-1][3] + 600
    db2 = bf.run(args2, bf.Http(rps=1000.0, opener=fake2, sleep=lambda s: None), log=lambda *a: None)
    fake2.calls.clear()
    bf.fetch_livedata(bf.Http(rps=1000.0, opener=fake2, sleep=lambda s: None), db2, "KXWTI15M", log=lambda *a: None,
                      stop_after_empty=3)
    assert {r[0] for r in db2.execute("SELECT event_ticker FROM price_paths")} == {w[1] for w in fake2.W[8:]}
    assert sum(1 for c in fake2.calls if c.startswith("/live_data/")) == 1 + 3


def test_candle_batches_stay_under_kalshis_cap_over_weeks_of_windows(tmp_path):
    fake = FakeKalshi(n=400, gap_every=92)  # ~4 days of windows with daily breaks: a 50-window batch would be refused
    fake.fail_next_429 = 0
    fake.cutoff = 0  # everything in the live tier
    args = Args()
    args.out, args.now, args.days = str(tmp_path / "h.db"), fake.W[-1][3] + 600, 10
    db = bf.run(args, bf.Http(rps=1000.0, opener=fake, sleep=lambda s: None), log=lambda *a: None)
    assert db.execute("SELECT COUNT(*) FROM candles_done").fetchone()[0] == 400
    assert db.execute("SELECT COUNT(DISTINCT ticker) FROM kalshi_candles").fetchone()[0] == 400
    batch_calls = sum(1 for c in fake.calls if c == "/markets/candlesticks")
    singles = sum(1 for c in fake.calls if c.startswith("/series/") and c.endswith("/candlesticks"))
    assert singles == 0 and batch_calls <= 25
