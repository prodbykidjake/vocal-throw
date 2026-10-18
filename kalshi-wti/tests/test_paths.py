import math
import time

import numpy as np

from wti15m.model import base_probability
from wti15m.paths import TouchCurve, fair_price, norm_cdf, simulate


def test_norm_cdf_matches_math_erf():
    xs = np.linspace(-6, 6, 241)
    exact = np.array([0.5 * (1 + math.erf(x / math.sqrt(2))) for x in xs])
    assert np.max(np.abs(norm_cdf(xs) - exact)) < 2e-7


def test_fair_price_matches_the_model_and_down_is_the_complement():
    p, _ = base_probability(90.25, 90.21, 0.005, 540, 0.005)
    assert abs(fair_price("UP", 90.25, 90.21, 0.005, 540, 0.005) - p) < 1e-6
    assert abs(fair_price("DOWN", 90.25, 90.21, 0.005, 540, 0.005) - (1 - p)) < 1e-6
    assert fair_price("UP", 90.25, 90.21, 0.005, 0.1, 0.005) == 1.0


def test_touch_curve_is_monotone_consistent_and_deterministic():
    c = simulate("DOWN", 90.25, 90.21, 0.005, 480, 60)
    assert c is not None and 0 < c.start < 1 and c.horizon_s > 0
    assert c.touch(c.start) == 1.0 and c.touch(0.999) < 0.6
    levels = [0.3, 0.4, 0.5, 0.6, 0.8]
    probs = [c.touch(lv) for lv in levels]
    assert probs == sorted(probs, reverse=True)
    # level(prob) and touch(level) agree
    for q in (0.5, 0.2):
        lv = c.level(q)
        assert abs(c.touch(lv) - q) < 0.03
    assert c.level(0.2) > c.level(0.5) >= c.start
    c2 = simulate("DOWN", 90.25, 90.21, 0.005, 480, 60)
    assert np.array_equal(c.maxima, c2.maxima)  # fixed seed: no jitter between seconds


def test_more_time_and_more_vol_reach_higher():
    short = simulate("UP", 90.18, 90.21, 0.005, 120, 60)
    long_ = simulate("UP", 90.18, 90.21, 0.005, 600, 60)
    wild = simulate("UP", 90.18, 90.21, 0.012, 600, 60)
    assert long_.level(0.5) > short.level(0.5)
    assert wild.level(0.5) > long_.level(0.5)


def test_offset_describes_the_bid_not_the_mid():
    mid = simulate("UP", 90.18, 90.21, 0.005, 300, 60)
    bid = simulate("UP", 90.18, 90.21, 0.005, 300, 60, offset=0.01)
    assert abs((mid.start - bid.start) - 0.01) < 1e-9
    assert bid.touch(0.4) <= mid.touch(0.4)


def test_no_time_left_is_a_flat_curve_and_bad_inputs_are_none():
    c = simulate("UP", 90.18, 90.21, 0.005, 20, 60, exclude_last_s=30)
    assert c.horizon_s == 0.0 and c.level(0.5) == c.start and c.touch(c.start + 0.01) == 0.0
    assert simulate("UP", 90.18, None, 0.005, 300, 60) is None
    assert simulate("UP", 90.18, 90.21, 0.0, 300, 60) is None
    assert simulate("UP", 90.18, 90.21, 0.005, None, 60) is None


def test_simulation_is_fast_enough_for_once_a_second():
    t = time.perf_counter()
    for _ in range(5):
        simulate("UP", 90.18, 90.21, 0.005, 880, 60)
    assert (time.perf_counter() - t) / 5 < 0.25
