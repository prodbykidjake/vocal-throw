"""EDGE: buy only at a minute check (3:00-13:00) when the model beats the ask by the margin; one per window; scored."""
import datetime as dt

from cm15m.edge import EdgeStore, EdgeTracker, taker_fee_per_contract

OPEN = 1_800_000_000.0


def iso(ts):
    return dt.datetime.fromtimestamp(ts, dt.timezone.utc).isoformat()


def state(p_up, yes_bid, yes_ask, ticker="W1", fresh=True, mode="kalshi live", conf="confident"):
    return {"market": {"ticker": ticker, "open_time": iso(OPEN), "close_time": iso(OPEN + 900), "yes_bid": yes_bid, "yes_ask": yes_ask},
            "prediction": {"p_up": p_up, "confidence": conf}, "quotes_fresh": fresh, "feed": {"mode": mode}}


def tracker():
    return EdgeTracker("gold", EdgeStore(":memory:"))


def test_fires_only_at_a_minute_check_when_the_model_beats_the_ask_by_six_points():
    tr = tracker()
    # minute 2: too early, however big the gap
    assert tr.step(OPEN + 125, state(0.90, 0.70, 0.72)) is None
    # minute 4, 30 s after the check: not a check moment
    assert tr.step(OPEN + 270, state(0.90, 0.70, 0.72)) is None
    # minute 5 check: model 78% Up vs ask 74c = +4 pts: not enough
    assert tr.step(OPEN + 301, state(0.78, 0.72, 0.74)) is None
    # still minute 5 a few seconds later: already checked this minute, no second look
    assert tr.step(OPEN + 304, state(0.90, 0.72, 0.74)) is None
    # minute 6 check: 82% vs 75c = +7 pts -> BUY UP
    sig = tr.step(OPEN + 361, state(0.82, 0.73, 0.75))
    assert sig and sig["side"] == "UP" and sig["minute"] == 6 and abs(sig["ask"] - 0.75) < 1e-9
    # one per window
    assert tr.step(OPEN + 421, state(0.95, 0.73, 0.75)) is None and tr.watch["signal"]["minute"] == 6


def test_down_side_uses_the_no_ask_and_no_signal_on_a_bad_feed_or_stale_quotes():
    tr = tracker()
    assert tr.step(OPEN + 181, state(0.18, 0.27, 0.29, fresh=False)) is None  # stale quotes: wait ...
    assert tr.step(OPEN + 183, state(0.18, 0.27, 0.29, mode="FALLBACK (hyperliquid)")) is None  # ... backup price: wait
    sig = tr.step(OPEN + 185, state(0.18, 0.27, 0.29))  # Down 82% vs NO ask 1 - 0.27 = 73c: +9 pts
    assert sig["side"] == "DOWN" and abs(sig["ask"] - 0.73) < 1e-9 and abs(sig["gap"] - 0.09) < 1e-9
    tr2 = tracker()
    assert tr2.step(OPEN + 181, state(0.30, 0.20, 0.60)) is None  # a 40c-wide book is not a price


def test_after_minute_thirteen_nothing_and_the_window_says_so():
    tr = tracker()
    assert tr.step(OPEN + 14 * 60 + 1, state(0.95, 0.70, 0.72)) is None
    assert tr.watch["window_over"]


def test_settles_with_the_fee_and_keeps_a_record():
    tr = tracker()
    tr.step(OPEN + 361, state(0.82, 0.73, 0.75, ticker="A"))
    tr.settle_from(lambda t: {"result": "yes"}, OPEN + 1000)
    r = tr.store.record("gold")
    assert r["wins"] == 1 and r["losses"] == 0
    assert abs(r["pnl"] - round(1 - 0.75 - taker_fee_per_contract(0.75), 2)) < 1e-9
    tr.step(OPEN + 900 + 361, {**state(0.18, 0.27, 0.29, ticker="B"), "market": {**state(0.18, 0.27, 0.29, ticker="B")["market"],
                                                                                  "open_time": iso(OPEN + 900), "close_time": iso(OPEN + 1800)}})
    tr.settle_from(lambda t: {"result": "yes"} if t == "B" else {"result": "yes"}, OPEN + 2000)  # DOWN lost
    r = tr.store.record("gold")
    assert (r["wins"], r["losses"]) == (1, 1)
