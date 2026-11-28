import asyncio
import time

import pytest

from wti15m.feeds.base import PriceFeed
from wti15m.feeds.composite import CompositeFeed
from wti15m.feeds.kalshi_live import KalshiLiveFeed, event_ticker_for, parse_series


def test_parse_series_shapes():
    now_ms = int(time.time() * 1000)
    data = {"live_data": {"type": "x", "details": {"timeseries": [{"t": now_ms - 2000, "v": 88.9}, {"t": now_ms - 1000, "v": "88.91"}]},
                          "default_range": "15min"}}
    pts = parse_series(data)
    assert len(pts) == 2 and abs(pts[0][0] - (now_ms - 2000) / 1000) < 1e-6 and pts[1][1] == 88.91
    assert parse_series({"timeseries": [[1700000000, 90.0], [1700000001, "90.1"]]}) == [(1700000000.0, 90.0), (1700000001.0, 90.1)]
    assert parse_series({"live_data": {"details": {"timeseries": []}}}) == []
    assert parse_series({"live_data": {"details": {"timeseries": [{"t": "bad", "v": 1}]}}}) == []
    assert parse_series(None) == []


def test_event_ticker_for():
    assert event_ticker_for("KXWTI15M-26OCT072030-30", "KXWTI15M-26OCT072030") == "KXWTI15M-26OCT072030"
    assert event_ticker_for("KXWTI15M-26OCT072030-30", "") == "KXWTI15M-26OCT072030"
    assert event_ticker_for("KXWTI15M-26OCT072030", None) == "KXWTI15M-26OCT072030"
    assert event_ticker_for("", None) is None


class FakeResp:
    def __init__(self, data, status=200):
        self._data, self.status_code, self.text = data, status, "x"

    def json(self):
        return self._data


class FakeHttp:
    def __init__(self):
        self.calls = []
        self.series = []
        self.fail = False

    async def get(self, url, params=None):
        self.calls.append((url, params))
        if self.fail:
            return FakeResp({}, 500)
        return FakeResp({"live_data": {"details": {"timeseries": [{"t": int(t * 1000), "v": v} for t, v in self.series]}}})

    async def aclose(self):
        pass


@pytest.mark.asyncio
async def test_kalshi_live_feed_warms_up_then_publishes_only_new_points():
    http = FakeHttp()
    now = time.time()
    http.series = [(now - 120 + i, 88.80 + i * 0.001) for i in range(118)]
    feed = KalshiLiveFeed("https://example/trade-api/v2", lambda: "KXWTI15M-26OCT072030", http=http)
    warm = []
    ticks = []
    feed.subscribe_warmup(lambda closes: warm.append(len(closes)))
    feed.subscribe(lambda t: ticks.append(t))
    await feed.poll_once()  # warm-up request (range=1h)
    assert http.calls[0][1] == {"range": "1h"} and "live_data/events/KXWTI15M-26OCT072030" in http.calls[0][0]
    assert warm and warm[0] > 10 and len(ticks) == 1  # history goes into the buffer, latest point is published live
    assert feed.latest().price == http.series[-1][1] and feed.mode == "kalshi-live"
    # next poll: two new points
    http.series += [(now - 1, 88.95), (now, 88.96)]
    n = await feed.poll_once()
    assert n == 2 and http.calls[-1][1] == {"range": "15min"} and feed.latest().price == 88.96
    # same data again: nothing new
    assert await feed.poll_once() == 0
    # errors are counted, never raised
    http.fail = True
    assert await feed.poll_once() == 0 and feed.errors == 1 and "HTTP 500" in feed.last_error
    # no event: waits
    feed.event_ticker_fn = lambda: None
    feed._last_event_seen = 0
    assert await feed.poll_once() == 0 and feed.mode.startswith("waiting")


class ManualFeed(PriceFeed):
    def __init__(self, name):
        super().__init__()
        self.name = name
        self.symbol = name.upper()

    async def start(self):
        self.connected = True

    async def stop(self):
        pass

    def push(self, ts, price):
        self._publish(ts, price)


@pytest.mark.asyncio
async def test_composite_switches_to_fallback_when_primary_is_stale():
    primary, fallback = ManualFeed("kalshi-live"), ManualFeed("hyperliquid")
    feed = CompositeFeed(primary, fallback, stale_after_s=5.0, warmup_grace_s=0.05)
    got = []
    feed.subscribe(lambda t: got.append((feed.active, t.price)))
    await feed.start()
    now = time.time()
    primary.push(now - 1, 88.90)
    fallback.push(now - 1, 88.84)  # ignored: primary is fresh
    assert got == [("primary", 88.90)] and feed.health().name == "kalshi-live"
    primary.buffer._ts[-1] = now - 30  # primary goes quiet
    fallback.push(now, 88.85)
    assert got[-1] == ("fallback", 88.85) and feed.active == "fallback" and feed.health().mode.startswith("FALLBACK")
    primary.push(now + 1, 88.91)
    assert got[-1] == ("primary", 88.91) and feed.active_name == "kalshi-live"
    # fallback warm-up is only used if the primary never warms
    fallback._publish_warmup([(now - 600, 88.0), (now - 300, 88.5)])
    await asyncio.sleep(0.1)
    assert feed.buffer.first_ts() is not None
    await feed.stop()


@pytest.mark.asyncio
async def test_composite_ignores_primary_backfill_older_than_fallback():
    primary, fallback = ManualFeed("kalshi-live"), ManualFeed("hyperliquid")
    feed = CompositeFeed(primary, fallback, stale_after_s=5.0, warmup_grace_s=0.05)
    got = []
    feed.subscribe(lambda t: got.append((feed.active, t.ts, t.price)))
    await feed.start()
    now = time.time()
    primary.push(now - 40, 88.90)
    primary.buffer._ts[-1] = now - 40
    fallback.push(now - 2, 88.84)  # primary stale -> fallback active
    assert feed.active == "fallback"
    primary.push(now - 20, 88.91)  # a late backfill point older than the fallback's tick: ignored
    assert feed.active == "fallback" and got[-1][2] == 88.84
    primary.push(now, 88.92)  # genuinely new: back to primary
    assert feed.active == "primary" and got[-1][2] == 88.92
    await feed.stop()


@pytest.mark.asyncio
async def test_composite_rebuilds_history_when_primary_warms_up_late():
    primary, fallback = ManualFeed("kalshi-live"), ManualFeed("hyperliquid")
    feed = CompositeFeed(primary, fallback, stale_after_s=5.0, warmup_grace_s=0.05)
    seeds = []
    feed.subscribe_warmup(lambda c: seeds.append(len(c)))
    await feed.start()
    now = time.time()
    fallback.push(now - 1, 88.84)
    assert feed.buffer.first_ts() is not None and feed.active == "fallback"
    primary._publish_warmup([(now - 600 + i * 5, 88.5 + i * 0.001) for i in range(100)])
    assert seeds == [100]
    assert feed.buffer.first_ts() < now - 500  # history now sits in front of the fallback tick
    assert feed.buffer.latest().price == 88.84
    await feed.stop()


@pytest.mark.asyncio
async def test_kalshi_live_retries_warmup_when_series_is_empty():
    http = FakeHttp()
    http.series = []
    feed = KalshiLiveFeed("https://example/trade-api/v2", lambda: "EV", http=http)
    await feed.poll_once()
    assert "EV" not in feed._warmed_events and http.calls[-1][1] == {"range": "1h"}
    now = time.time()
    http.series = [(now - 60 + i, 90.0) for i in range(59)]
    await feed.poll_once()
    assert "EV" in feed._warmed_events and feed.latest() is not None
