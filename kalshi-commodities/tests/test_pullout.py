"""PULL OUT: flashes only when the engine and Kalshi together put the called side at 3% or less (or, with no usable
Kalshi quote, the engine alone at 1% or less for 10 s), with at least 1:00 left; latched per window; scored at settlement."""
import types

from cm15m.board import Board
from cm15m.config import Config
from cm15m.pullout import PulloutStore, PulloutTracker


def state(p_up, yes_bid, yes_ask, side="UP", left=200.0, ticker="W1", call_ticker=None, fresh=True, mode="kalshi live",
          conf="confident", result=None, called=True):
    call = {"ticker": call_ticker or ticker, "side": side, "p_side": 0.88, "result": result} if called else None
    return {"market": {"ticker": ticker, "yes_bid": yes_bid, "yes_ask": yes_ask}, "seconds_left": left,
            "prediction": {"p_up": p_up, "confidence": conf}, "quotes_fresh": fresh, "feed": {"mode": mode},
            "call": {"call": call}}


def tracker(**kw):
    return PulloutTracker("gold", PulloutStore(":memory:"), **kw)


def test_rule_a_fires_at_three_percent_and_not_a_hair_above():
    tr = tracker()
    # Up called; engine 2.0% Up, Kalshi mid 4.1% -> average 3.05%: not yet
    assert tr.step(1000, state(0.020, 0.03, 0.052)) is None
    assert tr.watch["state"] == "watching" and tr.watch["rule"] == "A"
    # engine 2% and Kalshi 4% -> exactly 3%: PULL OUT
    a = tr.step(1001, state(0.020, 0.03, 0.05))
    assert a and a["side"] == "UP" and a["rule"] == "A" and abs(a["score"] - 0.03) < 1e-9
    assert abs(a["bid"] - 0.03) < 1e-9 and abs(a["p_kalshi"] - 0.04) < 1e-9 and a["seconds_left"] == 200.0


def test_a_strong_call_sliding_to_35_percent_does_not_flash():
    tr = tracker()
    # Jake's gold example: called UP 87%, now UP 35% / DOWN 65% with 6:13 left. It still wins about 1 time in 4.
    for i in range(30):
        assert tr.step(1000 + i, state(0.35, 0.33, 0.36, left=373 - i)) is None
    assert tr.watch["state"] == "watching" and tr.store.record()["n"] == 0


def test_down_call_reads_the_no_side_and_nothing_in_the_last_minute():
    tr = tracker()
    # DOWN called; Up now 98% (Down 2%), Kalshi yes 96/98 -> Down mid 3%: average 2.5% ... but only 59 s left
    assert tr.step(1000, state(0.98, 0.96, 0.98, side="DOWN", left=59.0)) is None
    assert tr.watch["too_late"]
    a = tr.step(1001, state(0.98, 0.96, 0.98, side="DOWN", left=60.0, ticker="W2"))
    assert a and a["side"] == "DOWN" and abs(a["p_model"] - 0.02) < 1e-9 and abs(a["p_kalshi"] - 0.03) < 1e-9
    assert abs(a["bid"] - 0.02) < 1e-9  # the NO bid = 1 - yes ask


def test_kalshi_still_on_the_call_vetoes_the_engine():
    tr = tracker()
    # the engine says 0.5% but Kalshi still has the called side at 84%: no alarm (a real case on history)
    for i in range(30):
        assert tr.step(1000 + i, state(0.005, 0.83, 0.85)) is None


def test_rule_b_needs_ten_seconds_at_one_percent_when_kalshi_has_no_usable_quote():
    tr = tracker()
    for i in range(10):  # stale quotes: the engine alone at 0.8% for 0..9 s
        assert tr.step(1000 + i, state(0.008, None, None, fresh=False)) is None
    assert tr.watch["rule"] == "B" and tr.watch["low_for_s"] == 9
    a = tr.step(1010, state(0.008, None, None, fresh=False))
    assert a and a["rule"] == "B" and a["p_kalshi"] is None and a["bid"] is None
    # a wobble above 1% restarts the count; a 40c-wide book counts as no quote
    tr2 = tracker()
    for i in range(8):
        assert tr2.step(1000 + i, state(0.008, 0.01, 0.41)) is None
    assert tr2.step(1008, state(0.02, 0.01, 0.41)) is None
    for i in range(10):
        assert tr2.step(1009 + i, state(0.009, 0.01, 0.41)) is None
    assert tr2.step(1019, state(0.009, 0.01, 0.41))["rule"] == "B"


def test_no_alarm_without_a_call_on_this_window_or_on_a_bad_price():
    tr = tracker()
    assert tr.step(1000, state(0.01, 0.01, 0.03, called=False)) is None and tr.watch["state"] == "waiting"
    assert tr.step(1001, state(0.01, 0.01, 0.03, call_ticker="OLD")) is None and tr.watch["state"] == "waiting"
    assert tr.step(1002, state(0.01, 0.01, 0.03, mode="FALLBACK (hyperliquid)")) is None and tr.watch["state"] == "paused"
    assert tr.step(1003, state(0.01, 0.01, 0.03, conf="stale")) is None and tr.watch["state"] == "paused"


def test_latched_once_per_window_then_scored_at_settlement():
    tr = tracker()
    assert tr.step(1000, state(0.01, 0.01, 0.03, ticker="A"))
    # the price comes back: the alarm stays (one per window), and never fires twice
    assert tr.step(1001, state(0.60, 0.55, 0.60, ticker="A")) is None and tr.watch["state"] == "fired"
    assert tr.step(1002, state(0.01, 0.01, 0.03, ticker="A")) is None
    # the call lost (settled DOWN): the alarm was right
    tr.settle_from(lambda t: {"result": "no"}, 2000)
    tr.step(2001, state(0.5, 0.4, 0.6, ticker="A"))
    assert tr.watch["alarm"]["correct"] == 1
    # next window: a fresh alarm; this time the call wins and the alarm was wrong
    assert tr.step(3000, state(0.01, 0.01, 0.03, ticker="B"))
    tr.settle_from(lambda t: {"result": "yes"} if t == "B" else {"result": "no"}, 4000)
    r = tr.store.record("gold")
    assert r["right"] == 1 and r["wrong"] == 1 and r["n"] == 2


def test_board_notifies_even_when_the_card_is_muted(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    board = Board(cfg)
    sent = []
    board.notifier = types.SimpleNamespace(notify=lambda title, msg, sound="Glass": sent.append((title, msg, sound)))
    board.muted = {"gold"}
    desk = types.SimpleNamespace(spec=types.SimpleNamespace(key="gold", name="Gold"),
                                 state=state(0.01, 0.01, 0.03, left=125.0),
                                 store=types.SimpleNamespace(window=lambda t: None))
    board._pullout_step(desk, 1000.0, False)
    assert sent and sent[0][0] == "GOLD: PULL OUT of UP" and "2:05 left" in sent[0][1] and "bid ~1¢" in sent[0][1]
    assert desk.state["pullout"]["watch"]["state"] == "fired"
    board._pullout_step(desk, 1001.0, False)
    assert len(sent) == 1  # once per window
