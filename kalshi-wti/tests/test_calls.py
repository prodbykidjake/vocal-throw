from wti15m.calls import CallTracker
from wti15m.config import TradingCfg
from wti15m.model import Prediction
from wti15m.store import Store


def pred(p_up, p_market=None, conf="lean", disagree=False):
    return Prediction(p_up, p_up, p_up, p_market, 0.0, 0.004, None, None, 600, 0.0, 0.1, conf, 0.0, disagree=disagree)


def tracker(**kw):
    return CallTracker(TradingCfg(**kw), Store(":memory:"))


def test_waits_out_the_study_then_calls_once_confident_and_locks():
    ct = tracker()
    t0 = 1000.0
    # the study period: never a call, even at 90%
    for i in range(170):
        assert ct.step(t0 + i, "W1", pred(0.9, 0.8), 890 - i, 10 + i, 180) is None
    assert ct.watch["status"] == "studying"
    # after the study: 65% is not enough, it keeps watching
    for i in range(30):
        assert ct.step(t0 + 200 + i, "W1", pred(0.65, 0.6), 690 - i, 210 + i, 180) is None
    # 75% must hold 10 s in a row
    made = None
    for i in range(12):
        made = ct.step(t0 + 300 + i, "W1", pred(0.75, 0.62), 600 - i, 300 + i, 180) or made
    assert made and made["side"] == "UP" and made["kind"] == "confident" and abs(made["p_market_side"] - 0.62) < 1e-9
    # locked: a flip to 80% Down changes nothing
    assert ct.step(t0 + 400, "W1", pred(0.2, 0.3), 500, 400, 180) is None and ct.call["side"] == "UP"


def test_a_wobble_restarts_the_confirmation_and_the_deadline_forces_the_lean():
    ct = tracker()
    for i in range(9):
        ct.step(100 + i, "W2", pred(0.25), 600 - i, 300 + i, 180)  # Down 75% for 9 s
    ct.step(110, "W2", pred(0.40), 590, 310, 180)  # back to 60%: confirmation restarts
    assert ct.call is None and ct.step(111, "W2", pred(0.25), 589, 311, 180) is None
    # never confident again; at 2:00 left it calls its lean (Down 58%)
    assert ct.step(500, "W2", pred(0.42), 125, 775, 180) is None
    made = ct.step(505, "W2", pred(0.42), 120, 780, 180)
    assert made["side"] == "DOWN" and made["kind"] == "late"


def test_no_free_wins_for_late_joins_or_disagreement():
    ct = tracker()
    assert ct.step(1, "W3", pred(0.97), 240, 660, 180) is None  # app first saw the window with 4 min left
    assert ct.step(200, "W3", pred(0.97), 100, 800, 180) is None and ct.call is None and "too late" in ct.watch["status"]
    ct2 = tracker()
    for i in range(20):  # 80% while the model and Kalshi disagree a lot: not a confident call
        ct2.step(i, "W4", pred(0.8, 0.3, disagree=True), 600 - i, 300 + i, 180)
    assert ct2.call is None


def test_record_counts_wins_losses_and_what_the_market_expected():
    ct = tracker()
    st = ct.store
    st.add_call("A", "UP", 0.75, 0.70, 1.0, 500, "confident")
    st.add_call("B", "DOWN", 0.72, 0.60, 2.0, 400, "confident")
    st.add_call("C", "UP", 0.55, 0.50, 3.0, 120, "late")
    ct.settle("A", True)  # Up: win
    ct.settle("B", True)  # Up: the Down call loses
    ct.settle("C", True)
    ct.settle("C", False)  # a second result for the same window is ignored
    rec = st.call_record()
    assert (rec["wins"], rec["losses"]) == (2, 1)
    assert rec["market_expected"] == round(0.70 + 0.60 + 0.50, 1) and rec["n_market"] == 3
    assert rec["by_kind"] == {"confident": {"wins": 1, "losses": 1}, "late": {"wins": 1, "losses": 0}}
    assert [r["ticker"] for r in rec["recent"]] == ["C", "B", "A"]
    # a restart reloads the window's call instead of making a second one
    ct2 = CallTracker(TradingCfg(), st)
    assert ct2.step(10, "A", pred(0.1), 500, 400, 180) is None and ct2.call["side"] == "UP"


def test_old_config_call_prob_follows_the_timing_fix(tmp_path):
    from wti15m import config

    p = tmp_path / "config.toml"
    p.write_text("[trading]\ncall_prob = 0.70\n")
    assert config.load(str(p)).trading.call_prob == 0.72
    p.write_text("[trading]\ncall_prob = 0.80\n")
    assert config.load(str(p)).trading.call_prob == 0.80


def test_no_confident_call_without_kalshis_odds():
    ct = CallTracker(TradingCfg(), Store(":memory:"))
    for i in range(30):  # 85% but no Kalshi quote: the disagreement check is blind, so no confident call
        ct.step(i, "W9", pred(0.85, None, conf="confident"), 600 - i, 300 + i, 180)
    assert ct.call is None


def test_meter_tracks_the_lean_and_its_recent_range_even_after_the_call():
    ct = CallTracker(TradingCfg(), Store(":memory:"))
    for i, p in enumerate([0.55, 0.62, 0.58, 0.66]):
        ct.step(100 + i, "M1", pred(p, 0.6), 700 - i, 100 + i, 180)
    m = ct.state()["meter"]
    assert m["p_up"] == 0.66 and m["lo"] == 0.55 and m["hi"] == 0.66 and m["line"] == 0.72 and m["market_up"] == 0.6
    ct.step(200, "M1", pred(0.40, 0.45), 600, 200, 180)
    assert ct.state()["meter"]["lo"] == 0.40  # 30-s band: older points dropped
    assert ct.state()["meter"]["hi"] == 0.40
    ct.step(201, "M2", pred(0.5, 0.5, conf="stale"), 899, 1, 180)
    assert ct.state()["meter"] is None


def test_no_call_on_the_backup_price_and_the_confirmation_restarts():
    # v12.4: on Oct 12 the backup feed sat ~13c low and turned a 70% DOWN reading into a 78% DOWN call that lost
    ct = tracker()
    for i in range(9):
        ct.step(100 + i, "W5", pred(0.22, 0.3), 600 - i, 300 + i, 180)  # Down 78% for 9 s on Kalshi's price
    for i in range(20):  # the price feed drops to the backup: no call however long it holds, and it says so
        assert ct.step(110 + i, "W5", pred(0.15, 0.3), 590 - i, 310 + i, 180, feed_ok=False) is None
    assert ct.call is None and ct.watch.get("feed_paused") and "confirming_s" not in ct.watch
    # back on Kalshi's price: the 10 s confirmation starts over
    assert ct.step(130, "W5", pred(0.22, 0.3), 570, 330, 180) is None
    assert ct.watch["confirming_s"] == 0 and not ct.watch.get("feed_paused")
    made = None
    for i in range(1, 11):
        made = ct.step(130 + i, "W5", pred(0.22, 0.3), 570 - i, 330 + i, 180, source="kalshi-live") or made
    assert made and made["side"] == "DOWN" and made["source"] == "kalshi-live" and made["version"]


def test_the_deadline_call_waits_for_kalshis_price():
    ct = tracker()
    assert ct.step(100, "W6", pred(0.42), 600, 300, 180) is None  # watching since 10:00 left, never confident
    assert ct.step(500, "W6", pred(0.42), 120, 780, 180, feed_ok=False) is None  # at the deadline, on the backup
    assert ct.step(510, "W6", pred(0.42), 110, 790, 180, feed_ok=False) is None and ct.call is None
    made = ct.step(515, "W6", pred(0.42), 105, 795, 180)  # Kalshi's price is back with 1:45 left: it calls now
    assert made["side"] == "DOWN" and made["kind"] == "late"
    ct2 = tracker()
    ct2.step(100, "W7", pred(0.42), 600, 300, 180)
    for i in range(70):  # the backup lasts past the last minute: no call at all this window
        ct2.step(600 + i, "W7", pred(0.42), 120 - i * 1.0, 780 + i, 180, feed_ok=False)
    assert ct2.call is None


def test_record_shows_profit_at_the_ask_and_splits_by_side_and_version():
    from wti15m.fees import taker_fee
    st = Store(":memory:")
    st.add_call("A", "UP", 0.75, 0.74, 1.0, 500, "confident", ask=0.75, bid=0.73)  # an old call: no version
    st.add_call("B", "DOWN", 0.75, 0.76, 2.0, 500, "confident", ask=0.77, bid=0.75, version="0.12.4", source="kalshi-live")
    st.add_call("C", "UP", 0.80, 0.80, 3.0, 400, "confident", ask=0.81, bid=0.79, version="0.12.4", source="kalshi-live")
    st.settle_call("A", "UP", 10.0)
    st.settle_call("B", "UP", 11.0)  # the DOWN call lost
    st.settle_call("C", "UP", 12.0)
    r = st.call_record()
    fee = lambda a: taker_fee(a, 100) / 100  # noqa: E731
    expect = (1 - 0.75 - fee(0.75)) + (-0.77 - fee(0.77)) + (1 - 0.81 - fee(0.81))
    assert r["n_pnl"] == 3 and abs(r["pnl"] - round(expect, 2)) < 1e-9
    assert r["by_side"]["UP"]["wins"] == 2 and r["by_side"]["DOWN"]["losses"] == 1
    assert abs(r["by_side"]["UP"]["market_expected"] - 1.5) < 1e-9
    assert r["by_version"]["before 0.12.4"]["wins"] == 1 and r["by_version"]["0.12.4"]["losses"] == 1
