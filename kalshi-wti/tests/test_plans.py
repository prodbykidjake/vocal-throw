import time

from wti15m.config import TradingCfg
from wti15m.decision import Quotes, Signal
from wti15m.model import Prediction
from wti15m.plans import PlanTracker, scalp_target
from wti15m.sizing import dollars_for, tier_for, unit_dollars


def pred(p_up: float, disagree: bool = False) -> Prediction:
    return Prediction(p_up, p_up, p_up, None, 0.0, 0.005, 0.005, 0.005, 480, 0.0, 0.1, "confident", 0.0, [], [], 0.005,
                      None, 0.0, None, disagree)


def buy(side: str, ask: float) -> Signal:
    return Signal("BUY", side, ask, 10, 0.1, 0.8, f"BUY {side}", ["d1", "d2"], ["edge"], "confident")


def test_tiers_and_dollars():
    cfg = TradingCfg(unit_dollars=10, max_trade_dollars=50)
    assert tier_for(0.95) == ("near-certain", 3.0) and tier_for(0.7) == ("confident", 1.0) and tier_for(0.5) is None
    assert dollars_for(0.95, cfg) == (30.0, "near-certain")
    assert dollars_for(0.85, cfg) == (20.0, "strong")
    assert dollars_for(0.6, cfg) == (5.0, "lean")
    cfg.max_trade_dollars = 25
    assert dollars_for(0.95, cfg) == (25.0, "near-certain")
    cfg.learn_unit = True
    assert unit_dollars(cfg, [3, 90, 20, 15, 12]) == 15  # median of what you actually trade
    assert unit_dollars(cfg, [3]) == 10  # too few samples: configured unit


def test_scalp_target_has_room():
    cfg = TradingCfg()
    t = scalp_target(0.9, 0.20, 0.004, cfg)
    assert 0.6 < t <= 0.95


def test_plan_lifecycle_confirm_hold_miss_cooldown():
    cfg = TradingCfg(unit_dollars=10, max_trade_dollars=50, confirm_s=3, min_hold_s=20, miss_margin_cents=4, miss_seconds=10, cooldown_s=30)
    tr = PlanTracker(cfg)
    t0 = time.time()
    q = Quotes(0.19, 0.20, 0.79, 0.80)  # Up ask 20c
    p = pred(0.92)
    # a BUY signal must persist confirm_s before it becomes a plan
    assert tr.update(t0, "W1", p, q, 90.0, 90.0, 480, buy("UP", 0.20), False) is None
    assert tr.update(t0 + 1, "W1", p, q, 90.0, 90.0, 479, buy("UP", 0.20), False) is None
    ev = tr.update(t0 + 3.5, "W1", p, q, 90.0, 90.0, 477, buy("UP", 0.20), False)
    assert ev and ev.kind == "created"
    plan = tr.plan
    assert plan.side == "UP" and plan.limit == 0.205 and plan.amount == 30.0 and plan.tier == "near-certain"
    assert plan.target > plan.limit + 0.08 and plan.expected_profit > 0
    disp = tr.display(buy("UP", 0.20), p, t0 + 4)
    assert disp.action == "BUY" and "sell at" in disp.headline and disp.triggers["amount"] == 30.0
    # the model wobbles a little: within min_hold nothing changes
    assert tr.update(t0 + 5, "W1", pred(0.75), q, 90.0, 90.0, 475, buy("UP", 0.21), False) is None
    assert tr.plan is plan
    # ask runs away: 4c past the limit for 10 s = missed
    far = Quotes(0.25, 0.26, 0.73, 0.74)
    assert tr.update(t0 + 6, "W1", p, far, 90.0, 90.0, 474, None, False) is None
    assert "past the" in tr.plan.status_text
    ev = tr.update(t0 + 17, "W1", p, far, 90.0, 90.0, 463, None, False)
    assert ev and ev.kind == "missed" and tr.plan is None and tr.cooldown_until > t0 + 17
    # cooldown blocks new plans and the display says so
    assert tr.update(t0 + 20, "W1", p, q, 90.0, 90.0, 460, buy("UP", 0.20), False) is None
    disp = tr.display(buy("UP", 0.20), p, t0 + 20)
    assert disp.action == "WAIT" and "analyzing again" in disp.headline
    # after the cooldown a new setup can form again
    assert tr.update(t0 + 48, "W1", p, q, 90.0, 90.0, 432, buy("UP", 0.20), False) is None
    ev = tr.update(t0 + 52, "W1", p, q, 90.0, 90.0, 428, buy("UP", 0.20), False)
    assert ev and ev.kind == "created"


def test_plan_cancels_on_hard_invalidation_and_expires_on_new_window():
    cfg = TradingCfg(confirm_s=0, min_hold_s=20)
    tr = PlanTracker(cfg)
    t0 = time.time()
    q = Quotes(0.29, 0.30, 0.69, 0.70)
    tr.update(t0, "W1", pred(0.85), q, 90.0, 90.0, 480, buy("UP", 0.30), False)
    ev = tr.update(t0 + 0.5, "W1", pred(0.85), q, 90.0, 90.0, 480, buy("UP", 0.30), False)
    assert ev and ev.kind == "created"
    # model collapses below the stop: cancelled even inside min_hold
    ev = tr.update(t0 + 2, "W1", pred(0.30), q, 90.0, 90.0, 478, None, False)
    assert ev and ev.kind == "cancelled" and "thesis broke" in ev.text
    tr.cooldown_until = 0
    tr.update(t0 + 3, "W1", pred(0.85), q, 90.0, 90.0, 477, buy("UP", 0.30), False)
    ev = tr.update(t0 + 3.5, "W1", pred(0.85), q, 90.0, 90.0, 477, buy("UP", 0.30), False)
    assert ev and ev.kind == "created"
    # market disagrees strongly: cancelled
    ev = tr.update(t0 + 4, "W1", pred(0.85, disagree=True), q, 90.0, 90.0, 476, None, False)
    assert ev and ev.kind == "cancelled"
    tr.cooldown_until = 0
    tr.update(t0 + 5, "W1", pred(0.85), q, 90.0, 90.0, 475, buy("UP", 0.30), False)
    tr.update(t0 + 5.5, "W1", pred(0.85), q, 90.0, 90.0, 475, buy("UP", 0.30), False)
    assert tr.plan is not None
    ev = tr.update(t0 + 6, "W2", pred(0.85), q, 90.0, 90.0, 900, None, False)
    assert ev and ev.kind == "expired" and tr.plan is None
    # a filled plan ends as filled and carries its target
    tr.cooldown_until = 0
    tr.update(t0 + 7, "W2", pred(0.85), q, 90.0, 90.0, 899, buy("UP", 0.30), False)
    tr.update(t0 + 7.5, "W2", pred(0.85), q, 90.0, 90.0, 899, buy("UP", 0.30), False)
    target = tr.plan.target
    ev = tr.mark_filled(t0 + 8)
    assert ev.kind == "filled" and ev.plan.target == target and tr.cooldown_until == 0


def test_no_plan_without_scalp_room():
    cfg = TradingCfg(confirm_s=0, min_scalp_cents=8)
    tr = PlanTracker(cfg)
    t0 = time.time()
    q = Quotes(0.90, 0.91, 0.09, 0.10)  # Up already at 91c: nowhere to scalp to
    tr.update(t0, "W1", pred(0.95), q, 90.0, 90.0, 480, buy("UP", 0.91), False)
    assert tr.update(t0 + 0.5, "W1", pred(0.95), q, 90.0, 90.0, 480, buy("UP", 0.91), False) is None
    assert tr.plan is None
