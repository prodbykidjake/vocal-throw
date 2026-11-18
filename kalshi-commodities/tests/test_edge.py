"""EDGE: buy only at a minute check (3:00-13:00) when the model is >= 75% and beats the ask by 8 points; one per window."""
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
    # minute 6 check: 85% vs 75c = +10 pts -> BUY UP
    sig = tr.step(OPEN + 361, state(0.85, 0.73, 0.75))
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
    tr.step(OPEN + 361, state(0.85, 0.73, 0.75, ticker="A"))
    tr.settle_from(lambda t: {"result": "yes"}, OPEN + 1000)
    r = tr.store.record("gold")
    assert r["wins"] == 1 and r["losses"] == 0
    assert abs(r["pnl"] - round(1 - 0.75 - taker_fee_per_contract(0.75), 2)) < 1e-9
    tr.step(OPEN + 900 + 361, {**state(0.18, 0.27, 0.29, ticker="B"), "market": {**state(0.18, 0.27, 0.29, ticker="B")["market"],
                                                                                  "open_time": iso(OPEN + 900), "close_time": iso(OPEN + 1800)}})
    tr.settle_from(lambda t: {"result": "yes"} if t == "B" else {"result": "yes"}, OPEN + 2000)  # DOWN lost
    r = tr.store.record("gold")
    assert (r["wins"], r["losses"]) == (1, 1)


def test_a_big_gap_is_not_enough_without_confidence():
    tr = tracker()
    assert tr.step(OPEN + 181, state(0.70, 0.58, 0.60)) is None  # +10 pts but only 70% sure: no signal
    assert tr.step(OPEN + 241, state(0.82, 0.73, 0.75)) is None  # 82% but only +7 pts: no signal
    assert tr.step(OPEN + 301, state(0.84, 0.74, 0.76))["side"] == "UP"  # 84% and +8 pts: signal


def test_an_old_config_with_the_v12_rule_gets_the_tighter_one(tmp_path):
    from cm15m import config
    path = tmp_path / "config.toml"
    path.write_text("[board]\nedge_gap = 0.06\nedge_min_p = 0.55\n")
    cfg = config.load(str(path))
    assert cfg.board.edge_gap == 0.08 and cfg.board.edge_min_p == 0.75
    path.write_text("[board]\nedge_gap = 0.10\n")  # a deliberate choice is kept
    assert config.load(str(path)).board.edge_gap == 0.10


def test_a_gap_of_12_or_more_points_shown_is_a_big_gap():
    from cm15m.edge import points, tier
    gaps = (None, 0.08, 0.1149, 0.115, 0.12, 0.1996, 0.31)
    assert [tier(g) for g in gaps] == ["normal", "normal", "normal", "big", "big", "big", "big"]
    assert [points(g) for g in (0.1149, 0.115, 0.1196, 0.1996)] == [11, 12, 12, 20]
    assert tier(0.87 - 0.75) == "big" and tier(0.86 - 0.75) == "normal"  # float sums land on the right side


def test_the_record_splits_by_gap_size_and_a_signal_carries_its_tier():
    s = EdgeStore(":memory:")
    for tk, gap, up in (("A", 0.09, True), ("B", 0.15, True), ("C", 0.16, False), ("D", 0.25, True)):
        s.add("gold", tk, 1000.0, 5, "UP", 0.80, 0.80 - gap, gap)
        s.settle("gold", tk, up, 2000.0)
    assert s.get("gold", "A")["tier"] == "normal" and s.get("gold", "D")["tier"] == "big"
    t = s.record()["tiers"]
    assert (t["normal"]["n"], t["big"]["wins"], t["big"]["losses"], t["big"]["n"]) == (1, 2, 1, 3)
    want = sum((1.0 if up else 0.0) - a - taker_fee_per_contract(a) for a, up in ((0.65, True), (0.64, False), (0.55, True)))
    assert abs(t["big"]["pnl"] - round(want, 2)) < 1e-9
