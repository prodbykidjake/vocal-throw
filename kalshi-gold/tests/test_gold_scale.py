"""Gold trades near $4,000 and moves dollars per window: volatility, odds and text must work at that scale."""
import random

from gold15m.config import Config
from gold15m.decision import DecisionEngine, Quotes
from gold15m.model import Model, VolEstimator


def warmed(sigma=0.15, price=4000.0, seed=7):
    m = Model(min_warmup_s=0)
    rng = random.Random(seed)
    for i in range(3600):
        price += rng.gauss(0, sigma)
        m.vol.update(1000.0 + i, price)
    return m, price


def test_volatility_is_measured_at_gold_scale_not_clipped_by_an_oil_cap():
    m, _ = warmed()
    assert 0.10 < m.vol.sigma() < 0.22  # true 0.15 $ per second-root
    assert VolEstimator().cap >= 1.0


def test_odds_and_text_at_gold_scale():
    m, px = warmed()
    strike = round(px - 2.0, 2)  # $2 above the target with 5 minutes left: about 0.8 standard deviations
    pred = m.predict(px, strike, 300, 0.75, 0.3)
    assert 0.65 < pred.p_final < 0.9
    sig = DecisionEngine(Config().trading).decide(pred, Quotes(0.74, 0.76, 0.24, 0.26), px, strike, 300, 600)
    text = " ".join([sig.headline] + list(sig.details))
    assert f"${strike:,.2f}" in text and "$2.00 above" in text and "¢ above" not in text
