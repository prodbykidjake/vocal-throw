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
