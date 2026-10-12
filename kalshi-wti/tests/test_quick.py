import random

from wti15m.config import TradingCfg
from wti15m.decision import Quotes
from wti15m.fees import FeeSchedule
from wti15m.model import Model
from wti15m.quick import QuickScalper, first_passage
from wti15m.store import Store


def warmed(sigma=0.004, seed=3):
    m = Model(min_warmup_s=0)
    rng = random.Random(seed)
    px = 70.0
    for i in range(3600):
        px += rng.gauss(0, sigma)
        m.vol.update(1000.0 + i, px)
    return m


def test_first_passage_counts_target_before_stop():
    kw = dict(side="UP", strike=70.0, sigma=0.004, seconds_left=600, settle_lag_s=60, tie_adj=0.005,
              offset=0.005, horizon_s=120, converge_s=300)
    # no model edge (Kalshi's bid sits on the model's fair value): a closer target wins more often than a farther stop
    base = first_passage(s0=69.95, bid0=0.305, target=0.335, stop=0.255, **kw)
    assert base and abs(base["p_win"] + base["p_loss"] + base["p_none"] - 1) < 1e-9
    assert base["p_win"] > base["p_loss"] and abs(base["gap"]) < 0.03
    # Kalshi's bid 6c under the model's fair value: the catch-up adds to the win chance
    cheap = first_passage(s0=69.95, bid0=0.25, target=0.28, stop=0.20, **kw)
    assert cheap["gap"] < -0.04 and cheap["p_win"] > base["p_win"]
    # Kalshi's bid 6c over fair value: the fade works against the scalp
    rich = first_passage(s0=69.95, bid0=0.37, target=0.40, stop=0.32, **kw)
    assert rich["p_win"] < base["p_win"]
    assert first_passage(s0=69.95, bid0=0.3, target=0.33, stop=0.25, **{**kw, "seconds_left": 8}) is None


def test_calls_an_underpriced_cheap_side_and_refuses_an_overpriced_one():
    m = warmed()
    qs = QuickScalper(TradingCfg(), Store(":memory:"))
    fees = FeeSchedule()
    # the feed sits on the 70.00 target: the model gives Up ~52%, Kalshi sells it at 31c -> called
    pred = m.predict(70.00, 70.00, 660, 0.305, 0.5)
    opts = qs.evaluate(pred, Quotes(0.30, 0.31, 0.69, 0.70), 70.00, 70.00, 600, fees)
    up = next(o for o in opts if o["side"] == "UP")
    assert up["call"] and up["p_win"] >= 0.55 and up["ev"] > 0 and up["target"] >= 0.34 and up["gap"] < -0.15
    # a smaller disagreement (model ~40% vs 31c) does not survive the fees and the noise: no call, and it says why
    small = qs.evaluate(m.predict(69.97, 70.00, 660, 0.305, 0.5), Quotes(0.30, 0.31, 0.69, 0.70), 69.97, 70.00, 600, fees)
    up_s = next(o for o in small if o["side"] == "UP")
    assert not up_s["call"] and "after fees" in up_s["why_not"]
    assert up["maker_free"]  # plain 'quadratic' series: a resting sell pays no fee
    # same quotes, but the feed says 69.92: the model gives Up ~20%, Kalshi's 31c is rich
    pred2 = m.predict(69.92, 70.00, 660, 0.305, 0.5)
    up2 = next(o for o in qs.evaluate(pred2, Quotes(0.30, 0.31, 0.69, 0.70), 69.92, 70.00, 600, fees) if o["side"] == "UP")
    assert not up2["call"] and "model" in up2["why_not"]
    # an oil report in the window, or a confident settlement call the other way: no call
    blocked = qs.evaluate(pred, Quotes(0.30, 0.31, 0.69, 0.70), 70.00, 70.00, 600, fees, report=True)
    assert not next(o for o in blocked if o["side"] == "UP")["call"]
    against = qs.evaluate(pred, Quotes(0.30, 0.31, 0.69, 0.70), 70.00, 70.00, 600, fees,
                          settle_call={"side": "DOWN", "kind": "confident"})
    assert "settlement call" in next(o for o in against if o["side"] == "UP")["why_not"]
    # too late in the window, or nothing cheap: nothing at all
    assert qs.evaluate(pred, Quotes(0.30, 0.31, 0.69, 0.70), 70.00, 70.00, 60, fees) == []
    assert qs.evaluate(pred, Quotes(0.49, 0.51, 0.49, 0.51), 70.00, 70.00, 600, fees) == []


def test_every_call_is_scored_on_paper():
    m = warmed()
    store = Store(":memory:")
    qs = QuickScalper(TradingCfg(), store)
    fees = FeeSchedule()
    pred = m.predict(70.00, 70.00, 660, 0.305, 0.5)
    q = Quotes(0.30, 0.31, 0.69, 0.70)
    qs.step(1000.0, "W1", pred, q, 70.00, 70.00, 600, fees, False, None, show=True)
    assert qs.call and qs.call["side"] == "UP" and len(store.quick_open_rows("W1")) == 1
    qs.step(1001.0, "W1", pred, q, 70.00, 70.00, 599, fees, False, None, show=True)
    assert len(store.quick_open_rows("W1")) == 1  # one open paper scalp per side, not one per second
    target = qs.call["target"]
    qs.step(1030.0, "W1", pred, Quotes(target, target + 0.01, 1 - target - 0.01, 1 - target), 70.02, 70.00, 570, fees, False, None, show=True)
    rec = store.quick_record()
    assert (rec["wins"], rec["losses"]) == (1, 0) and rec["pnl"] > 0 and rec["by_reason"]["target"] == 1
    assert any("WIN" in e for e in qs.events)
    # a paper scalp that falls to its stop is a loss
    store.add_quick("W1", "DOWN", 1040.0, 0.30, 0.35, 0.25, 1160.0, 0.6, 0.1, 5.0)
    qs.step(1050.0, "W1", pred, Quotes(0.74, 0.75, 0.24, 0.26), 70.05, 70.00, 550, fees, False, None, show=False)
    rec = store.quick_record()
    assert (rec["wins"], rec["losses"]) == (1, 1) and rec["by_reason"]["stop"] == 1
    # no stop: sold at the bid when the time runs out (here at a loss)
    store.add_quick("W1", "DOWN", 1060.0, 0.30, 0.40, 0.0, 1080.0, 0.6, 0.1, 5.0)
    qs.step(1081.0, "W1", pred, Quotes(0.71, 0.72, 0.28, 0.29), 70.01, 70.00, 520, fees, False, None, show=False)
    assert store.quick_record()["by_reason"]["time"] == 1
    # a call still open when the window ends is scored at its last bid
    qs.step(1090.0, "W1", pred, q, 70.00, 70.00, 510, fees, False, None, show=True)
    assert store.quick_open_rows("W1")
    qs.step(1100.0, "W2", None, Quotes(), None, None, None, fees, False, None, show=False)
    assert store.quick_record()["by_reason"]["window ended"] == 1 and store.quick_open_rows("W1") == []
