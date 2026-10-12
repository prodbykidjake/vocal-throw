"""Natural gas trades near $3 with three-decimal targets: the model, the tie rule and the text must work at that scale."""
import random

from ng15m.config import Config
from ng15m.decision import DecisionEngine, Quotes
from ng15m.model import Model, VolEstimator, base_probability


def warmed(sigma=0.0005, price=3.2, seed=5):
    m = Model(min_warmup_s=0)
    rng = random.Random(seed)
    for i in range(3600):
        price += rng.gauss(0, sigma)
        m.vol.update(1000.0 + i, price)
    return m, price


def test_volatility_is_measured_at_gas_scale_not_clamped_to_an_oil_floor():
    m, _ = warmed()
    s = m.vol.sigma()  # true per-step sigma 0.0005 per second
    assert 0.0003 < s < 0.0008
    assert VolEstimator().floor < 0.0001


def test_tie_is_half_a_tenth_of_a_cent():
    # a three-decimal target: a price 0.4 thousandths under it still rounds to it, and a tie pays Up
    p_at, _ = base_probability(3.2496, 3.250, 0.0005, 0.5 + 1e-9)
    assert p_at > 0.5
    p_below, _ = base_probability(3.2494, 3.250, 0.0005, 0.6)
    assert p_below < 0.5


def test_odds_and_text_at_gas_scale():
    m, px = warmed()
    strike = round(px - 0.006, 3)  # 0.6¢ above the target with 5 minutes left
    pred = m.predict(px, strike, 300, 0.7, 0.3)
    assert 0.6 < pred.p_final < 0.95
    sig = DecisionEngine(Config().trading).decide(pred, Quotes(0.69, 0.71, 0.29, 0.31), px, strike, 300, 600)
    text = " ".join([sig.headline] + list(sig.details))
    assert f"${strike:.3f}" in text and "0.6¢ above" in text
