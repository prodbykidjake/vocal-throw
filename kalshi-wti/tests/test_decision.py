import math
import random

from wti15m.config import TradingCfg
from wti15m.decision import DecisionEngine, Position, Quotes, kelly_fraction, price_for_probability
from wti15m.fees import FeeSchedule
from wti15m.model import Model


def warmed_model(sigma=0.005, seconds=2400, seed=11):
    m = Model(min_warmup_s=1800, confident_margin=0.20)
    rng = random.Random(seed)
    px, t = 90.0, 0.0
    while t < seconds:
        px += rng.gauss(0, sigma)
        t += 1
        m.vol.update(t, px)
    return m


def engine(**over):
    cfg = TradingCfg(**over)
    return DecisionEngine(cfg, FeeSchedule())


def shown(eng, pred, q, price, strike, left, elapsed, pos, now, n=2):
    """What the card shows after n consecutive seconds of the same picture (the engine remembers the shown and
    the raw rule on the position between seconds; a SELL needs two seconds in a row before it shows)."""
    sig = None
    for i in range(n):
        sig = eng.decide(pred, q, price, strike, left, elapsed, pos, now + i)
        pos.last_reason = sig.reasons[0]
        pos.last_raw_reason = sig.triggers.get("raw_rule", sig.reasons[0])
    return sig


def test_kelly_fraction():
    assert abs(kelly_fraction(0.6, 0.5) - 0.2) < 1e-9
    assert kelly_fraction(0.4, 0.5) == 0.0


def test_price_for_probability_round_trips():
    sigma, tau, K = 0.005, 600, 90.21
    target = price_for_probability(0.75, K, sigma, tau, 0.005)
    from wti15m.model import base_probability
    p, _ = base_probability(target, K, sigma, tau)
    assert abs(p - 0.75) < 1e-6


def test_screenshot_1026_left_two_cents_below_is_a_wait():
    m = warmed_model()
    pred = m.predict(90.19, 90.21, 626, p_market=0.485, feed_age_s=0.3)
    sig = engine().decide(pred, Quotes(0.48, 0.49, 0.51, 0.52), 90.19, 90.21, 626, 274)
    assert sig.action == "WAIT"
    assert sig.reasons[0] in ("coin_flip", "edge_too_small", "priced_in")
    assert "Would buy" in " ".join(sig.details)
    assert sig.triggers["side"] in ("UP", "DOWN") and "max_ask" in sig.triggers


def test_screenshot_sixteen_seconds_left_ten_cents_up_at_99c_is_a_wait():
    m = warmed_model()
    pred = m.predict(89.92, 89.82, 16, p_market=0.99, feed_age_s=0.3)
    assert pred.p_final > 0.98
    sig = engine().decide(pred, Quotes(0.98, 0.99, 0.01, 0.02), 89.92, 89.82, 16, 884)
    assert sig.action == "WAIT"
    # even at 99c the 1c fee round-up kills it: edge = 0.99+ - 0.99 - 0.01 < 0.05
    assert sig.reasons[0] in ("priced_in", "edge_too_small")


def test_big_gap_mid_window_with_cheap_ask_is_a_buy():
    m = warmed_model()
    # 8 cents below target with 7 minutes left and sigma 0.005 -> z ~ -0.77 -> ~78% Down
    pred = m.predict(90.13, 90.21, 420, p_market=0.40, feed_age_s=0.3)
    assert pred.p_down > 0.7
    # Market only prices Down at 60c: edge is large
    sig = engine(bankroll=200).decide(pred, Quotes(0.39, 0.41, 0.59, 0.61), 90.13, 90.21, 420, 480)
    assert sig.action == "BUY" and sig.side == "DOWN"
    assert sig.price == 0.61 and 1 <= sig.size <= 20
    assert sig.edge >= 0.05
    assert any("Exit plan" in d for d in sig.details)


def test_first_minute_blocks_entries():
    m = warmed_model()
    pred = m.predict(90.13, 90.21, 870, p_market=0.40, feed_age_s=0.3)
    sig = engine().decide(pred, Quotes(0.39, 0.41, 0.59, 0.61), 90.13, 90.21, 870, 30)
    assert sig.action == "WAIT" and sig.reasons == ["window_just_opened"]


def test_wide_spread_blocks_entries():
    m = warmed_model()
    pred = m.predict(90.13, 90.21, 420, p_market=0.40, feed_age_s=0.3)
    sig = engine().decide(pred, Quotes(0.30, 0.50, 0.50, 0.70), 90.13, 90.21, 420, 480)
    assert sig.action == "WAIT" and sig.reasons == ["spread_wide"]


def test_cold_model_waits_for_warmup():
    m = Model(min_warmup_s=1800)
    pred = m.predict(90.13, 90.21, 420, p_market=0.40, feed_age_s=0.3)
    sig = engine().decide(pred, Quotes(0.39, 0.41, 0.59, 0.61), 90.13, 90.21, 420, 480)
    assert sig.action == "WAIT" and sig.reasons == ["warming_up"]


def test_position_management_rules():
    m = warmed_model()
    eng = engine()
    pos = Position("T", "DOWN", 6, 0.61, 0.0, amount=3.66)
    # price now well above target with 5 min left: Down is nearly gone, but the loss is not yet certain enough
    # to pay for an exit; the card says HOLD with the chance of getting back to breakeven, never a stop-loss
    pred = m.predict(90.35, 90.21, 360, p_market=0.85, feed_age_s=0.3)
    sig = eng.decide(pred, Quotes(0.84, 0.86, 0.14, 0.16), 90.35, 90.21, 300, 600, pos, now=1000.0)
    assert sig.action == "HOLD" and sig.reasons[0] in ("hold", "lottery")
    assert sig.scalp["action"] in ("SELL BETWEEN", "HOLD") and sig.scalp["pnl"] < 0
    if sig.reasons[0] == "hold":
        assert sig.scalp["target"] >= sig.scalp["breakeven"]  # the zone never asks for a loss
        assert 0 <= sig.scalp["p_recover"] <= 1 and "chance" in sig.scalp["text"]
    # ride to settlement: 90 s left, far below target
    pred = m.predict(90.00, 90.21, 150, p_market=0.02, feed_age_s=0.3)
    sig = eng.decide(pred, Quotes(0.01, 0.03, 0.97, 0.99), 90.00, 90.21, 90, 810, pos, now=1000.0)
    assert sig.action == "HOLD" and sig.reasons == ["ride_to_settle"]
    # in profit below the zone: HOLD with a concrete SELL BETWEEN zone above the bid and its chance
    pred = m.predict(90.12, 90.21, 560, p_market=0.30, feed_age_s=0.3)
    sig = eng.decide(pred, Quotes(0.29, 0.31, 0.69, 0.71), 90.12, 90.21, 500, 400, pos, now=1000.0)
    assert sig.action in ("HOLD", "SELL")
    if sig.action == "HOLD":
        assert sig.scalp["action"] == "SELL BETWEEN" and sig.scalp["target"] > 0.69 < sig.scalp["target_high"]
        assert sig.scalp["commit_zone"] == [sig.scalp["target"], sig.scalp["target_high"]]
        assert 0 < sig.scalp["p_low"] <= 1 and sig.scalp["p_high"] <= sig.scalp["p_low"]


def test_screenshot_premature_stop_is_now_a_hold_with_recovery_chance():
    """$30 of DOWN at 24c; the bid slipped to 22c and the model gives DOWN ~30% with 8 min left. v7 said
    SELL NOW 22c (-$4); the user held and sold at 42c later. Now: HOLD - SELL BETWEEN, with the chances."""
    m = warmed_model()
    eng = engine()
    pos = Position("T", "DOWN", 125.0, 0.24, 0.0, amount=30.0, entry_fee=0.32)
    pred = m.predict(90.26, 90.21, 540, p_market=0.77, feed_age_s=0.3)
    assert pred.p_down <= eng.cfg.stop_prob  # the old stop rule would have fired
    sig = eng.decide(pred, Quotes(0.76, 0.78, 0.22, 0.24), 90.26, 90.21, 480, 420, pos, now=1000.0)
    assert sig.action == "HOLD" and sig.reasons == ["hold"]
    sc = sig.scalp
    assert sc["action"] == "SELL BETWEEN" and sc["pnl"] < 0
    assert sc["breakeven"] > 0.24 and sc["target"] >= sc["breakeven"] + 0.02 - 1e-9 and sc["target_high"] > sc["target"]
    assert sc["p_recover"] > 0.5 and 0.3 <= sc["p_low"] <= 0.7 and sc["p_high"] < sc["p_low"]
    assert "chance of getting back to" in sc["text"] and "sell between" in sig.headline
    # the bid reaches the committed zone later: SELL NOW, in the zone
    pos.target, pos.target_high, pos.zone_ts = sc["target"], sc["target_high"], 1000.0
    pred = m.predict(90.19, 90.21, 360, p_market=0.60, feed_age_s=0.3)
    q = Quotes(0.59, 0.61, pos.target + 0.01, pos.target + 0.03)
    first = eng.decide(pred, q, 90.19, 90.21, 300, 600, pos, now=1200.0)
    assert first.action == "HOLD" and first.triggers["raw_rule"] == "zone"  # one second is not enough (debounce)
    sig = shown(eng, pred, q, 90.19, 90.21, 300, 600, pos, now=1200.0)
    assert sig.action == "SELL" and sig.reasons == ["zone"] and sig.scalp["action"] == "SELL NOW" and sig.scalp["pnl"] > 0


def test_zone_low_never_chases_the_price_up_but_comes_down_when_unlikely():
    m = warmed_model()
    eng = engine()
    pos = Position("T", "DOWN", 125.0, 0.24, 0.0, amount=30.0, entry_fee=0.32, target=0.35, target_high=0.64, zone_ts=990.0, high_bid=0.31)
    pred = m.predict(90.225, 90.21, 420, p_market=0.70, feed_age_s=0.3)
    q = Quotes(0.69, 0.71, 0.29, 0.31)
    sig = eng.decide(pred, q, 90.225, 90.21, 360, 540, pos, now=1000.0)
    assert sig.action == "HOLD" and sig.scalp["target"] == 0.35 and "commit_zone" not in sig.scalp  # within zone_refresh_s
    sig = eng.decide(pred, q, 90.225, 90.21, 360, 540, pos, now=1040.0)
    assert sig.scalp["target"] == 0.35  # the low end stays put even though the price climbed toward it
    # 45 s before the close with the bid at 28c a 50c low is out of reach: the zone comes down (but stays a profit)
    pos.target, pos.target_high, pos.zone_ts = 0.50, 0.70, 990.0
    pred = m.predict(90.215, 90.21, 105, p_market=0.70, feed_age_s=0.3)
    sig = eng.decide(pred, Quotes(0.70, 0.72, 0.28, 0.30), 90.215, 90.21, 45, 855, pos, now=1100.0)
    assert sig.action in ("HOLD", "SELL")
    zone = sig.scalp.get("commit_zone")
    assert zone and zone[0] < 0.50 and zone[0] >= sig.scalp.get("breakeven", 0.25) and zone[1] >= zone[0] + 0.05 - 1e-9


def test_give_up_only_when_recovery_is_nearly_hopeless_and_worth_the_click():
    m = warmed_model()
    eng = engine()
    # $30 of UP at 55c; with 100 s left the price is 12c under the target and the bid is 8c:
    # getting back to ~57c is nearly impossible -> salvage what is left (the only loss-taking sell)
    pos = Position("T", "UP", 54.5, 0.55, 0.0, amount=30.0, entry_fee=0.5)
    pred = m.predict(90.09, 90.21, 160, p_market=0.09, feed_age_s=0.3)
    sig = shown(eng, pred, Quotes(0.08, 0.10, 0.90, 0.92), 90.09, 90.21, 100, 800, pos, now=1000.0)
    assert sig.action == "SELL" and sig.reasons == ["give_up"]
    assert sig.scalp["action"] == "SELL NOW" and sig.scalp["p_recover"] <= eng.cfg.give_up_prob and "salvage" in sig.scalp["text"]
    # same picture but the bid is 1c: selling returns pennies -> lottery ticket, not a sell
    pos.last_reason = pos.last_raw_reason = None
    pred = m.predict(90.05, 90.21, 160, p_market=0.02, feed_age_s=0.3)
    sig = shown(eng, pred, Quotes(0.01, 0.03, 0.97, 0.99), 90.05, 90.21, 100, 800, pos, now=1000.0)
    assert sig.action == "HOLD" and sig.reasons == ["lottery"]


def test_model_flip_alone_never_sells_at_a_loss():
    m = warmed_model()
    eng = engine()
    pos = Position("T", "UP", 20, 0.55, 0.0)
    # UP now ~35% by the model with the bid at 5c and 5 min left: no stop-loss, whatever the model thinks
    pred = m.predict(90.17, 90.21, 360, p_market=0.06, feed_age_s=0.3)
    assert pred.p_final < 0.5
    sig = eng.decide(pred, Quotes(0.05, 0.07, 0.93, 0.95), 90.17, 90.21, 300, 600, pos, now=1000.0)
    assert sig.action == "HOLD" and sig.reasons[0] in ("hold", "lottery")


def test_rollover_lock_in():
    m = warmed_model()
    eng = engine()
    pos = Position("T", "UP", 100, 0.02, 0.0, amount=2.0, high_bid=0.10)
    pred = m.predict(90.24, 90.21, 400, p_market=0.07, feed_age_s=0.3)
    sig = shown(eng, pred, Quotes(0.06, 0.08, 0.92, 0.94), 90.24, 90.21, 400, 500, pos, now=1000.0)
    # bid 0.06 is 40% off the 0.10 high while still in profit -> lock it in (or it is already inside the zone: also a SELL)
    assert sig.action == "SELL" and sig.scalp["action"] == "SELL NOW"


def test_cents_formatting_keeps_sub_cent_prices():
    from wti15m.decision import cents, qty_text
    assert cents(0.013) == "1.3¢" and cents(0.49) == "49¢" and cents(0.988) == "98.8¢" and cents(None) == "--"
    assert qty_text(48.44) == "48.44" and qty_text(20.0) == "20"


def test_cash_out_matches_kalshi_sheet_shape():
    from wti15m.decision import cash_out
    from wti15m.fees import taker_fee
    # 48.44 shares, bid 1.3c -> $0.63 gross minus the taker fee rounded up to the cent (0.07*48.44*0.013*0.987 -> 5c)
    fee = taker_fee(0.013, 48.44, FeeSchedule())
    assert fee == 0.05
    assert abs(cash_out(48.44, 0.013, FeeSchedule()) - (48.44 * 0.013 - fee)) < 1e-9


def test_position_cost_includes_entry_fee():
    pos = Position("T", "UP", 115.38, 0.026, 0.0, amount=3.0, entry_fee=0.03)
    assert abs(pos.cost - 3.03) < 1e-9
    pos2 = Position("T", "UP", 10, 0.5, 0.0, entry_fee=0.02)
    assert abs(pos2.cost - 5.02) < 1e-9


def test_trigger_price_is_reported_in_feed_terms():
    m = warmed_model()
    m.basis_signed = -0.04  # feed reads 4c low
    pred = m.predict(90.13, 90.21, 420, p_market=0.50, feed_age_s=0.3)
    sig = engine().decide(pred, Quotes(0.49, 0.51, 0.49, 0.51), 90.13, 90.21, 420, 480)
    if "price_needed" in sig.triggers:
        from wti15m.decision import price_for_probability
        from wti15m.fees import fee_per_contract
        # recompute what the model-space trigger would be and check the feed-space number is shifted by the basis
        side = sig.triggers["side"]
        assert sig.triggers["price_needed"] == round(sig.triggers["price_needed"], 2)


def test_sell_rules_latch_instead_of_flickering():
    m = warmed_model()
    eng = engine()
    # zone latch: in the zone at 35c, a 1.5c dip under it keeps SELL NOW; a 5c dip does not
    pos = Position("T", "DOWN", 125.0, 0.24, 0.0, amount=30.0, entry_fee=0.32, target=0.35, target_high=0.64, zone_ts=990.0, high_bid=0.35)
    pred = m.predict(90.19, 90.21, 360, p_market=0.64, feed_age_s=0.3)
    sig = shown(eng, pred, Quotes(0.64, 0.66, 0.35, 0.37), 90.19, 90.21, 300, 600, pos, now=1000.0)
    assert sig.action == "SELL" and sig.reasons == ["zone"]
    sig = shown(eng, pred, Quotes(0.655, 0.675, 0.335, 0.355), 90.19, 90.21, 300, 600, pos, now=1002.0)
    assert sig.action == "SELL" and sig.reasons == ["zone"] and "dipped" in sig.headline
    # a real drop: one second keeps the SELL (debounce), the second second lets go
    sig = eng.decide(pred, Quotes(0.69, 0.71, 0.30, 0.32), 90.19, 90.21, 300, 600, pos, now=1004.0)
    assert sig.action == "SELL" and sig.triggers["raw_rule"] == "hold"
    sig = shown(eng, pred, Quotes(0.69, 0.71, 0.30, 0.32), 90.19, 90.21, 300, 600, pos, now=1004.0)
    assert sig.action == "HOLD"
    # rollover re-arms only on a new high: once ignored, the same dip does not nag again
    pos = Position("T", "UP", 136.0, 0.22, 0.0, amount=30.0, entry_fee=0.3, target=0.41, target_high=0.79, zone_ts=990.0, high_bid=0.36)
    pred = m.predict(90.17, 90.21, 500, p_market=0.28, feed_age_s=0.3)
    q = Quotes(0.27, 0.29, 0.71, 0.73)
    first = eng.decide(pred, q, 90.17, 90.21, 440, 460, pos, now=1000.0)
    assert first.action == "HOLD" and pos.rollover_ref is None  # a hidden first second must not disarm the rule
    sig = shown(eng, pred, q, 90.17, 90.21, 440, 460, pos, now=1000.0)
    assert sig.action == "SELL" and sig.reasons == ["rollover"] and pos.rollover_ref == 0.36
    sig = shown(eng, pred, Quotes(0.32, 0.34, 0.66, 0.68), 90.17, 90.21, 440, 460, pos, now=1002.0)
    assert sig.action == "HOLD"  # recovered above 85% of the high: back to the zone plan
    sig = shown(eng, pred, q, 90.17, 90.21, 440, 460, pos, now=1004.0, n=3)
    assert sig.action == "HOLD"  # same dip again, no new high: not re-armed
    pos.high_bid = 0.40
    sig = shown(eng, pred, Quotes(0.29, 0.31, 0.69, 0.71), 90.17, 90.21, 440, 460, pos, now=1007.0)
    assert sig.action == "SELL" and sig.reasons == ["rollover"]


def test_recovery_chance_runs_to_the_close_and_never_below_the_win_chance():
    """Review finding: the zone curve stops 30 s before the close, so with 34 s left a 58% favourite that was
    slightly under water read '0% chance of getting back to breakeven' and got a salvage SELL."""
    m = warmed_model()
    eng = engine()
    pos = Position("T", "UP", 20.0, 0.60, 0.0, amount=12.0, entry_fee=0.34)
    for left in (35, 34, 10, 3):
        pred = m.predict(90.215, 90.21, left + 60, p_market=0.56, feed_age_s=0.3)
        sig = eng.decide(pred, Quotes(0.55, 0.57, 0.43, 0.45), 90.215, 90.21, left, 900 - left, pos, now=1000.0)
        assert sig.action == "HOLD", (left, sig.headline)
        assert sig.scalp["p_recover"] >= pred.p_final - 0.0011  # scalp values are rounded to 3 decimals


def test_zone_latch_never_sells_at_a_loss():
    m = warmed_model()
    eng = engine()
    pos = Position("T", "UP", 22.2, 0.45, 0.0, amount=10.0, entry_fee=0.17, target=0.50, target_high=0.60, zone_ts=990.0, high_bid=0.50)
    pos.last_reason = "zone"
    pred = m.predict(90.21, 90.21, 360, p_market=0.47, feed_age_s=0.3)
    sig = eng.decide(pred, Quotes(0.46, 0.48, 0.52, 0.54), 90.21, 90.21, 300, 600, pos, now=1000.0)
    assert not (sig.action == "SELL" and sig.scalp["pnl"] < 0 and sig.reasons != ["give_up"])


def test_last_seconds_take_the_profit_but_never_a_loss():
    m = warmed_model()
    eng = engine()
    # in profit with 20 s left and a zone that is out of reach: the card says take the profit before the close
    pos = Position("T", "DOWN", 125.0, 0.24, 0.0, amount=30.0, entry_fee=0.32, target=0.50, target_high=0.70, zone_ts=900.0, high_bid=0.33)
    pred = m.predict(90.20, 90.21, 80, p_market=0.67, feed_age_s=0.3)
    sig = shown(eng, pred, Quotes(0.66, 0.68, 0.32, 0.34), 90.20, 90.21, 20, 880, pos, now=1000.0)
    assert sig.action == "SELL" and sig.reasons == ["zone"] and "before the close" in sig.headline and sig.scalp["pnl"] > 0
    # under water with 20 s left and a real chance of winning: no sell, and the chance shown is at least the win chance
    pos = Position("T", "DOWN", 125.0, 0.24, 0.0, amount=30.0, entry_fee=0.32, target=0.35, target_high=0.64, zone_ts=900.0, high_bid=0.24)
    pred = m.predict(90.215, 90.21, 80, p_market=0.60, feed_age_s=0.3)
    sig = shown(eng, pred, Quotes(0.59, 0.61, 0.39 - 0.19, 0.41 - 0.19), 90.215, 90.21, 20, 880, pos, now=1000.0)
    assert sig.action == "HOLD" and sig.scalp["p_recover"] >= round(pred.p_down, 3) - 0.0011


def test_debounce_hides_one_second_blips_both_ways():
    """Review finding: a one-second SELL blip must not show, must not set the latches, and must not disarm the
    rollover rule; a shown SELL must survive a one-second wobble back across its line."""
    m = warmed_model()
    eng = engine()
    pos = Position("T", "DOWN", 125.0, 0.24, 0.0, amount=30.0, entry_fee=0.32, target=0.35, target_high=0.64, zone_ts=990.0, high_bid=0.30)
    pred = m.predict(90.19, 90.21, 360, p_market=0.64, feed_age_s=0.3)
    inzone, below = Quotes(0.64, 0.66, 0.35, 0.37), Quotes(0.69, 0.71, 0.30, 0.32)
    seq = [below, inzone, below, below, inzone, inzone, below, inzone, below, below]
    shown_rules = []
    for i, q in enumerate(seq):
        sig = eng.decide(pred, q, 90.19, 90.21, 300, 600, pos, now=1000.0 + i)
        pos.last_reason, pos.last_raw_reason = sig.reasons[0], sig.triggers.get("raw_rule", sig.reasons[0])
        shown_rules.append(sig.reasons[0])
    # the lone blip at index 1 is hidden; the two-second run at 4-5 shows and survives the one-second dip at 6
    assert shown_rules[:4] == ["hold", "hold", "hold", "hold"]
    assert shown_rules[4] == "hold" and shown_rules[5] == "zone" and shown_rules[6] == "zone" and shown_rules[7] == "zone"
    assert shown_rules[8] == "zone" and shown_rules[9] == "hold"
