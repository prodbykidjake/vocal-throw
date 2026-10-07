import datetime as dt
import json
import pathlib

from wti15m.kalshi import Market, Series, parse_price, parse_strike, select_live_market

FIX = pathlib.Path(__file__).resolve().parents[1] / "fixtures"
UTC = dt.timezone.utc


def load(name):
    return json.loads((FIX / name).read_text())


def test_parse_price_prefers_dollar_strings():
    assert parse_price({"yes_bid": 48, "yes_bid_dollars": "0.4800"}, "yes_bid") == 0.48
    assert parse_price({"yes_bid": 48}, "yes_bid") == 0.48
    assert parse_price({"yes_bid_fp": "0.4850"}, "yes_bid") == 0.485
    assert parse_price({}, "yes_bid") is None


def test_parse_strike_sources():
    assert parse_strike({"floor_strike": 90.21}) == (90.21, "floor_strike")
    assert parse_strike({"floor_strike": "90.21"}) == (90.21, "floor_strike")
    assert parse_strike({"yes_sub_title": "$90.21 or above"}) == (90.21, "yes_sub_title")
    assert parse_strike({"title": "WTI up or down?"}) == (None, "none")


def test_market_from_fixture():
    m = Market.from_api(load("synthetic-markets-open.json")["markets"][0])
    assert m.ticker == "KXWTI15M-26OCT070745-45"
    assert m.volume == 9200 and m.open_interest == 4100
    assert m.strike == 90.21
    assert m.yes_bid == 0.48 and m.yes_ask == 0.49
    assert m.no_ask == 0.52
    assert m.is_open and not m.is_settled
    at = dt.datetime(2026, 10, 7, 11, 34, 34, tzinfo=UTC)
    assert m.is_live(at)
    assert abs(m.seconds_left(at) - 626) < 1e-6  # 10:26 on the screenshot
    assert m.spread == 0.01 and abs(m.yes_mid - 0.485) < 1e-9


def test_derives_missing_no_side():
    m = Market.from_api({"ticker": "X", "yes_bid_dollars": "0.4800", "yes_ask_dollars": "0.4900"})
    assert m.no_ask == 0.52 and m.no_bid == 0.51


def test_settled_market():
    m = Market.from_api(load("synthetic-market-settled.json")["market"])
    assert m.is_settled and m.result == "no" and m.status == "finalized"
    assert m.settle_value == 90.16


def test_settle_value_only_when_numeric():
    assert Market.from_api({"ticker": "X", "expiration_value": ""}).settle_value is None
    assert Market.from_api({"ticker": "X", "expiration_value": "yes"}).settle_value is None
    assert Market.from_api({"ticker": "X", "expiration_value": "88.83"}).settle_value == 88.83


def test_series_fixture():
    s = Series.from_api(load("synthetic-series.json"))
    assert s.ticker == "KXWTI15M" and s.fee_type == "quadratic" and s.fee_multiplier == 1.0
    assert s.settlement_sources[0]["name"] == "Pyth - WTI"


def test_select_live_market_prefers_current_window_then_next():
    base = dt.datetime(2026, 10, 7, 11, 0, tzinfo=UTC)

    def mk(ticker, open_min, close_min, status="open"):
        return Market.from_api({"ticker": ticker, "status": status,
                                "open_time": (base + dt.timedelta(minutes=open_min)).isoformat(),
                                "close_time": (base + dt.timedelta(minutes=close_min)).isoformat()})

    cur, nxt, old = mk("cur", 30, 45), mk("next", 45, 60), mk("old", 15, 30)
    at = base + dt.timedelta(minutes=34)
    assert select_live_market([nxt, old, cur], at).ticker == "cur"
    # between windows: pick the upcoming one
    assert select_live_market([nxt, old], base + dt.timedelta(minutes=44, seconds=59)).ticker == "next"
    assert select_live_market([old], at) is None
