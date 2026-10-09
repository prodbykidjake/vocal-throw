import time

from wti15m.config import TradingCfg
from wti15m.decision import Quotes, Signal
from wti15m.model import Prediction
from wti15m.plans import PlanTracker, hard_stop, scalp_target
from wti15m.sizing import dollars_for, tier_for, unit_dollars


def pred(p_up: float, disagree: bool = False) -> Prediction:
    return Prediction(p_up, p_up, p_up, None, 0.0, 0.005, 0.005, 0.005, 480, 0.0, 0.1, "confident", 0.0, [], [], 0.005,
                      None, 0.0, None, disagree)


def live_pred(price: float, strike: float, seconds_left: float, p_market: float) -> Prediction:
    """A prediction with the fields the path simulation needs (price_adj, implied price, sigma)."""
    from wti15m.model import Model
    import random
    m = Model(min_warmup_s=0)
    rng = random.Random(3)
    px, t = strike, 0.0
    while t < 2400:
        px += rng.gauss(0, 0.005)
        t += 1
        m.vol.update(t, px)
    return m.predict(price, strike, seconds_left + 60, p_market, 0.3)


def buy(side: str, ask: float) -> Signal:
    return Signal("BUY", side, ask, 10, 0.1, 0.8, f"BUY {side}", ["d1", "d2"], ["edge"], "confident")


def test_tiers_and_dollars():
    cfg = TradingCfg(unit_dollars=10, max_trade_dollars=50)
    assert tier_for(0.95) == ("near-certain", 3.0) and tier_for(0.7) == ("confident", 1.0) and tier_for(0.5) is None
    assert dollars_for(0.95, cfg) == (30.0, "near-certain")
    assert dollars_for(0.85, cfg) == (20.0, "strong")
    assert dollars_for(0.6, cfg) == (5.0, "lean")
    # below the lowest tier a cheap contract with edge is a longshot: half a unit, a full unit when the model's
    # chance is at least double the cost
    assert dollars_for(0.32, cfg) is None and tier_for(0.32) is None
    assert tier_for(0.32, ask=0.14, fee=0.01) == ("longshot", 1.0)
    assert tier_for(0.25, ask=0.14, fee=0.01) == ("longshot", 0.5)
    assert tier_for(0.18, ask=0.14, fee=0.01) is None  # edge 3c < edge_min
    assert dollars_for(0.32, cfg, ask=0.14, fee=0.01) == (10.0, "longshot")
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
    # market disagrees strongly while the model got MORE confident: that is the scalp, not a broken thesis
    assert tr.update(t0 + 4, "W1", pred(0.90, disagree=True), q, 90.0, 90.0, 476, None, False) is None
    # a 1-point dip is noise, not a weakening: still no cancel
    assert tr.update(t0 + 4.2, "W1", pred(0.84, disagree=True), q, 90.0, 90.0, 476, None, False) is None
    # market disagrees strongly and the model has clearly weakened since the plan: cancelled, and the text says why
    ev = tr.update(t0 + 4.5, "W1", pred(0.78, disagree=True), q, 90.0, 90.0, 476, None, False)
    assert ev and ev.kind == "cancelled" and "market disagrees" in ev.text
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


def test_hard_stop_scales_with_the_plan_probability():
    cfg = TradingCfg(stop_prob=0.35)
    assert hard_stop(0.85, cfg) == 0.35
    assert abs(hard_stop(0.32, cfg) - 0.192) < 1e-9


def test_longshot_plan_forms_with_a_reachable_target_and_chance():
    """Screenshot: Up 32% by the model, 14c ask, ten minutes left. v7 showed 'setup forming 3/3 s' forever."""
    cfg = TradingCfg(unit_dollars=10, max_trade_dollars=50, confirm_s=0)
    tr = PlanTracker(cfg)
    t0 = time.time()
    p = live_pred(90.16, 90.21, 600, p_market=0.15)
    p.p_final = 0.32  # the model's view (learner included); p_down follows
    q = Quotes(0.13, 0.14, 0.86, 0.87)
    sig = buy("UP", 0.14)
    tr.update(t0, "W1", p, q, 90.16, 90.21, 600, sig, False)
    ev = tr.update(t0 + 0.5, "W1", p, q, 90.16, 90.21, 600, sig, False)
    assert ev and ev.kind == "created", tr.no_plan_reason
    plan = tr.plan
    assert plan.tier == "longshot" and plan.amount == 10.0 and plan.limit == 0.145
    assert plan.target >= plan.limit + 0.08 and plan.target_high > plan.target
    assert plan.p_target is not None and plan.p_target >= cfg.plan_min_chance
    assert "chance" in tr.headline(plan) and "sell at" in tr.headline(plan)
    # a 34% model reading does not kill a 32% plan; 15% does
    assert tr.update(t0 + 1, "W1", pred(0.34), q, 90.16, 90.21, 599, None, False) is None
    ev = tr.update(t0 + 2, "W1", pred(0.15), q, 90.16, 90.21, 598, None, False)
    assert ev and ev.kind == "cancelled"


def test_card_says_why_a_confirmed_setup_is_not_a_plan():
    cfg = TradingCfg(confirm_s=0, min_scalp_cents=8)
    tr = PlanTracker(cfg)
    t0 = time.time()
    q = Quotes(0.90, 0.91, 0.09, 0.10)  # Up already at 91c: nowhere to scalp to
    tr.update(t0, "W1", pred(0.95), q, 90.0, 90.0, 480, buy("UP", 0.91), False)
    assert tr.update(t0 + 0.5, "W1", pred(0.95), q, 90.0, 90.0, 480, buy("UP", 0.91), False) is None
    assert tr.plan is None and tr.no_plan_reason and "room" in tr.no_plan_reason
    disp = tr.display(buy("UP", 0.91), pred(0.95), t0 + 1)
    assert disp.action == "WAIT" and disp.reasons == ["no_plan"] and "room" in disp.headline


def test_candidate_survives_a_short_gap_without_a_buy_signal():
    """The edge hovers at the threshold: BUY, WAIT, BUY, WAIT... The candidate (and the 'setup forming' card)
    must not reset on every WAIT second, and the plan still forms once the setup has been around confirm_s."""
    cfg = TradingCfg(unit_dollars=10, confirm_s=3)
    tr = PlanTracker(cfg)
    t0 = time.time()
    q = Quotes(0.19, 0.20, 0.79, 0.80)
    p = pred(0.92)
    wait = Signal("WAIT", "UP", None, 0, None, 0.5, "WAIT · no edge", ["d1", "d2"], ["priced_in"], "lean")
    assert tr.update(t0, "W1", p, q, 90.0, 90.0, 480, buy("UP", 0.20), False) is None
    assert tr.update(t0 + 1, "W1", p, q, 90.0, 90.0, 479, wait, False) is None
    disp = tr.display(wait, p, t0 + 1)
    assert disp.reasons == ["forming"] and "UP" in disp.headline  # still forming, not the plain WAIT text
    assert tr.update(t0 + 2, "W1", p, q, 90.0, 90.0, 478, buy("UP", 0.20), False) is None
    ev = tr.update(t0 + 3.5, "W1", p, q, 90.0, 90.0, 477, buy("UP", 0.20), False)
    assert ev and ev.kind == "created"
    # a long gap (more than confirm_s without a BUY) does reset it
    tr2 = PlanTracker(cfg)
    assert tr2.update(t0, "W1", p, q, 90.0, 90.0, 480, buy("UP", 0.20), False) is None
    assert tr2.update(t0 + 4, "W1", p, q, 90.0, 90.0, 476, wait, False) is None
    assert tr2._candidate is None and tr2.display(wait, p, t0 + 4) is wait


def test_no_plan_while_model_and_market_disagree():
    cfg = TradingCfg(confirm_s=0)
    tr = PlanTracker(cfg)
    t0 = time.time()
    q = Quotes(0.29, 0.30, 0.69, 0.70)
    tr.update(t0, "W1", pred(0.92, disagree=True), q, 90.0, 90.0, 480, buy("UP", 0.30), False)
    assert tr.update(t0 + 0.5, "W1", pred(0.92, disagree=True), q, 90.0, 90.0, 480, buy("UP", 0.30), False) is None
    assert tr.plan is None and "disagree" in tr.no_plan_reason
    disp = tr.display(buy("UP", 0.30), pred(0.92, disagree=True), t0 + 1)
    assert disp.reasons == ["no_plan"] and "disagree" in disp.headline


def test_refusal_text_is_calm_and_a_refused_setup_does_not_count_down_again():
    """Review finding: the refusal text rewrote itself every second with that second's cents, and each time the
    edge re-crossed the threshold the card ran another 'setup forming 0/3' for a setup it had just refused."""
    cfg = TradingCfg(confirm_s=3, min_scalp_cents=8)
    tr = PlanTracker(cfg)
    t0 = time.time()
    wait = Signal("WAIT", "UP", None, 0, None, 0.5, "WAIT · no edge", ["d1", "d2"], ["priced_in"], "lean")
    texts = set()
    for i in range(8):  # ask wobbling 3-4c, model ~10-12%: edge, but never 8c of room
        ask = 0.03 if i % 2 else 0.04
        p = pred(0.10 + 0.005 * (i % 3))
        tr.update(t0 + i, "W1", p, Quotes(ask - 0.01, ask, 1 - ask - 0.01, 1 - ask), 90.0, 90.0, 480 - i, buy("UP", ask), False)
        d = tr.display(buy("UP", ask), p, t0 + i)
        if d.reasons == ["no_plan"]:
            texts.add(d.headline)
    assert len(texts) == 1, texts  # one wording, held, not a new number every second
    # the signal drops to WAIT for longer than the grace, then the same setup comes back: no new countdown
    for i in range(8, 13):
        tr.update(t0 + i, "W1", pred(0.10), Quotes(0.03, 0.04, 0.95, 0.96), 90.0, 90.0, 480 - i, wait, False)
    assert tr._candidate is None
    tr.update(t0 + 13, "W1", pred(0.10), Quotes(0.03, 0.04, 0.95, 0.96), 90.0, 90.0, 467, buy("UP", 0.04), False)
    d = tr.display(buy("UP", 0.04), pred(0.10), t0 + 13)
    assert d.reasons == ["no_plan"] and "forming" not in d.headline
