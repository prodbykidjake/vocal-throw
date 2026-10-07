import math
import random

import numpy as np

from wti15m.model import Calibrator, DIM, Model, VolEstimator, base_probability, momentum_z, norm_cdf


def brownian(n_seconds, sigma, start=90.0, seed=1, step=1.0):
    rng = random.Random(seed)
    px, t, out = start, 0.0, []
    for _ in range(int(n_seconds / step)):
        px += rng.gauss(0, sigma * math.sqrt(step))
        t += step
        out.append((t, px))
    return out


def test_norm_cdf():
    assert abs(norm_cdf(0) - 0.5) < 1e-12
    assert abs(norm_cdf(1.96) - 0.975) < 1e-3


def test_base_probability_at_target_is_slightly_above_half_because_ties_pay_up():
    p, z = base_probability(90.21, 90.21, 0.005, 600)
    assert 0.5 < p < 0.52


def test_base_probability_screenshot_case_is_a_coin_flip():
    # 10:26 left, price 2c below target, sigma about $0.15 per 15 min -> 0.005/sqrt(s)
    p, z = base_probability(90.19, 90.21, 0.005, 626)
    assert 0.40 < p < 0.50 and -0.2 < z < 0


def test_base_probability_last_seconds_large_gap():
    p, _ = base_probability(89.92, 89.82, 0.005, 16)
    assert p > 0.99
    p0, _ = base_probability(89.80, 89.82, 0.005, 0.2)
    assert p0 == 0.0


def test_vol_estimator_recovers_sigma_from_ticks():
    sigma = 0.004
    est = VolEstimator()
    for t, px in brownian(3600, sigma, seed=3):
        est.update(t, px)
    assert est.n_updates > 500
    assert 0.6 * sigma < est.sigma() < 1.5 * sigma
    assert est.seconds_seen > 3500


def test_vol_estimator_seed_from_minute_closes():
    sigma = 0.004
    est = VolEstimator()
    path = brownian(3600, sigma, seed=5)
    closes = [(t, p) for t, p in path if int(t) % 60 == 0]
    est.seed_from_closes(closes)
    assert est.n_updates == len(closes) - 1
    assert 0.5 * sigma < est.sigma() < 2.0 * sigma


def test_momentum_z_sign():
    assert momentum_z(90.3, 90.0, 0.005, 60) > 0
    assert momentum_z(89.7, 90.0, 0.005, 60) < 0
    assert momentum_z(90.0, None, 0.005, 60) == 0.0


def test_calibrator_defers_to_base_with_no_data():
    cal = Calibrator()
    x = np.zeros(DIM)
    x[0] = 1.0
    x[1] = 1.2
    p_final, _ = cal.predict(0.75, x)
    assert abs(p_final - 0.75) < 1e-9
    assert cal.shrink == 0.0


def test_calibrator_learns_that_market_is_informative():
    rng = np.random.default_rng(0)
    cal = Calibrator(shrink_n=5)
    for _ in range(60):
        X, y = [], []
        for _ in range(40):
            x = np.zeros(DIM)
            x[0] = 1.0
            true_logit = rng.normal(0, 1.5)
            x[1] = rng.normal(0, 0.3)  # base model is noise in this synthetic world
            x[2] = true_logit  # market carries the truth
            X.append(x)
            y.append(1.0 if rng.random() < 1 / (1 + math.exp(-true_logit)) else 0.0)
        cal.fit_window(np.array(X), np.array(y))
    assert cal.w[2] > 0.5  # learned to trust the market feature
    assert cal.shrink > 0.9
    text = cal.to_json()
    again = Calibrator.from_json(text)
    assert np.allclose(again.w, cal.w) and again.n_windows == 60


def test_model_predict_confidence_labels():
    m = Model(min_warmup_s=600, confident_margin=0.2)
    for t, px in brownian(1200, 0.005, seed=7):
        m.vol.update(t, px)
    pred = m.predict(price=90.19, strike=90.21, tau_s=626, p_market=0.49, feed_age_s=0.5)
    assert pred.confidence in ("coinflip", "lean")
    pred2 = m.predict(price=89.92, strike=89.82, tau_s=16, p_market=0.99, feed_age_s=0.5)
    assert pred2.confidence == "confident" and pred2.p_final > 0.95
    stale = m.predict(price=89.92, strike=89.82, tau_s=16, p_market=0.99, feed_age_s=10)
    assert stale.confidence == "stale"
    cold = Model(min_warmup_s=1800)
    assert cold.predict(90.0, 90.0, 600, None, 0.1).confidence == "warming_up"
