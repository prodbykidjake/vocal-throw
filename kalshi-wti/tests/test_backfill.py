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

    def __init__(self):
        rnd = random.Random(3)
        px = 90.0
        self.W = []
        for i in range(12):
            o = T0 + 900 * i
            target = round(px, 2)
            px += rnd.gauss(0, 0.08)
            settle = round(px, 2)
            self.W.append((f"KXWTI15M-W{i:02d}", f"KXWTI15M-E{i:02d}", o, o + 900, target, settle, "yes" if settle >= target else "no"))
        self.cutoff = self.W[3][3] + 1
        self.calls = []
        self.fail_next_429 = 1

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
            return ok({"markets": [{"market_ticker": t, "candlesticks": self.candles(t, int(q["start_ts"]), int(q["end_ts"]), False)}
                                   for t in tks]})
        if path.startswith("/series/") and path.endswith("/candlesticks"):
            t = path.split("/")[4]
            return ok({"candlesticks": self.candles(t, int(q["start_ts"]), int(q["end_ts"]), False)})
        if path.startswith("/historical/markets/") and path.endswith("/candlesticks"):
            t = path.split("/")[3]
            return ok({"candlesticks": self.candles(t, int(q["start_ts"]), int(q["end_ts"]), True)})
        if path.startswith("/live_data/events/"):
            return ok({"live_data": {"details": {"timeseries": []}}})
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
