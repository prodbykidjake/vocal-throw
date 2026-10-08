import datetime as dt
import json
import pathlib

import pytest

from wti15m.kalshi import Market, MarketTracker, Series

FIX = pathlib.Path(__file__).resolve().parents[1] / "fixtures"
UTC = dt.timezone.utc


class FakeClient:
    def __init__(self):
        self.markets: list[dict] = []
        self.single: dict[str, dict] = {}
        self.series_calls = 0

    async def get_series(self, ticker):
        self.series_calls += 1
        return Series.from_api(json.loads((FIX / "synthetic-series.json").read_text()))

    async def get_markets(self, series_ticker=None, status=None, **kw):
        return [Market.from_api(d) for d in self.markets]

    async def get_market(self, ticker):
        return Market.from_api(self.single[ticker])


def mk(ticker, open_s, close_s, **extra):
    now = dt.datetime.now(UTC)
    d = {"ticker": ticker, "status": "open", "floor_strike": 90.21,
         "open_time": (now + dt.timedelta(seconds=open_s)).isoformat(),
         "close_time": (now + dt.timedelta(seconds=close_s)).isoformat(),
         "yes_bid_dollars": "0.4800", "yes_ask_dollars": "0.4900"}
    d.update(extra)
    return d


@pytest.mark.asyncio
async def test_tracker_rollover_and_settlement():
    client = FakeClient()
    events = []
    tracker = MarketTracker(client, "KXWTI15M", on_event=lambda k, m: events.append((k, m.ticker)))
    client.markets = [mk("A", -60, 60)]
    client.single["A"] = mk("A", -60, 60)
    await tracker.poll_once()
    assert tracker.series is not None and tracker.current.ticker == "A"
    assert events[:2] == [("window_open", "A"), ("quote", "A")]
    # A closes, B is live
    client.markets = [mk("B", -1, 899)]
    await tracker.poll_once()
    kinds = [e[0] for e in events]
    assert ("window_close", "A") in events and ("window_open", "B") in events
    assert "A" in tracker.pending
    # settlement for A arrives
    client.single["A"] = mk("A", -1000, -100, status="settled", result="yes")
    tracker._settle_checked.clear()
    await tracker.poll_once()
    assert ("settled", "A") in events and "A" not in tracker.pending
    assert tracker.current.ticker == "B"


@pytest.mark.asyncio
async def test_tracker_keeps_current_on_transient_empty_response():
    client = FakeClient()
    tracker = MarketTracker(client, "KXWTI15M")
    client.markets = [mk("A", -60, 60)]
    await tracker.poll_once()
    assert tracker.quotes_fresh(10)
    client.markets = []
    await tracker.poll_once()
    assert tracker.current is not None and tracker.current.ticker == "A"
    # the stale fallback must not count as a fresh quote
    tracker.last_quote_ts -= 20
    assert not tracker.quotes_fresh(10)
