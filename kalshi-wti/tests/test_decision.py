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
    # model flipped: price now well above target -> Down prob small; bid 0.14 is far above the model's value -> SELL (overpriced)
    pred = m.predict(90.35, 90.21, 300, p_market=0.85, feed_age_s=0.3)
    sig = eng.decide(pred, Quotes(0.84, 0.86, 0.14, 0.16), 90.35, 90.21, 300, 600, pos)
    assert sig.action == "SELL" and sig.reasons[0] in ("overpriced", "stop")
    assert sig.scalp["action"] == "SELL NOW" and sig.scalp["pnl"] < 0
    # ride to settlement: 90 s left, far below target
    pred = m.predict(90.00, 90.21, 90, p_market=0.02, feed_age_s=0.3)
    sig = eng.decide(pred, Quotes(0.01, 0.03, 0.97, 0.99), 90.00, 90.21, 90, 810, pos)
    assert sig.action == "HOLD" and sig.reasons == ["ride_to_settle"]
    # in profit with the market behind the model: HOLD with a concrete SELL AT target above the bid
    pred = m.predict(90.12, 90.21, 500, p_market=0.30, feed_age_s=0.3)
    sig = eng.decide(pred, Quotes(0.29, 0.31, 0.69, 0.71), 90.12, 90.21, 500, 400, pos)
    assert sig.action in ("HOLD", "SELL")
    if sig.action == "HOLD":
        assert sig.scalp["action"] == "SELL AT" and sig.scalp["target"] > 0.69


def test_stop_does_not_sell_into_a_worthless_bid():
    m = warmed_model()
    eng = engine()
    pos = Position("T", "UP", 20, 0.55, 0.0)
    # UP now ~35% by the model but the bid is 5c: selling locks in far more loss than the position is worth
    pred = m.predict(90.17, 90.21, 300, p_market=0.06, feed_age_s=0.3)
    assert pred.p_final < 0.5
    sig = eng.decide(pred, Quotes(0.05, 0.07, 0.93, 0.95), 90.17, 90.21, 300, 600, pos)
    if pred.p_final <= eng.cfg.stop_prob:
        assert sig.action == "HOLD" and sig.reasons == ["too_late_to_cut"]


def test_rollover_lock_in():
    m = warmed_model()
    eng = engine()
    pos = Position("T", "UP", 100, 0.02, 0.0, amount=2.0, high_bid=0.10)
    pred = m.predict(90.24, 90.21, 400, p_market=0.07, feed_age_s=0.3)
    sig = eng.decide(pred, Quotes(0.06, 0.08, 0.92, 0.94), 90.24, 90.21, 400, 500, pos)
    # bid 0.06 is 40% off the 0.10 high while still in profit -> lock it in (unless the model says the market overpays, also a SELL)
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
