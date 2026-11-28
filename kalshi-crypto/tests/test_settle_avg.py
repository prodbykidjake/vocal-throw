"""Crypto settles on the average of the index's last 60 one-second prints: the model's maths, checked against simulated
paths, and the desk's own closing-minute average that it compares with each settlement value."""
import math
import random

import pytest

from cx15m.feeds.base import second_average
from cx15m.model import Model, average_inputs, average_probability


def test_effective_time_outside_and_inside_the_settlement_minute():
    assert average_inputs(100.0, 600, 60) == (100.0, 560.0)  # τ - 60 + 60/3
    assert average_inputs(100.0, 60, 60) == (100.0, 20.0)
    mean, t = average_inputs(100.0, 30, 60, avg_sofar=98.0)  # half the prints locked at 98, half still to come at ~100
    assert mean == pytest.approx(99.0) and t == pytest.approx((30 / 60) ** 2 * 30 / 3)
    mean, t = average_inputs(100.0, 0, 60, avg_sofar=98.0)
    assert mean == pytest.approx(98.0) and t == 0.0
    assert average_inputs(100.0, 30, 0) == (100.0, 30.0)  # window 0 = the single price at the close
    assert average_inputs(100.0, 30, 60) == (100.0, pytest.approx(2.5))  # no prints known: assume the live price


def _paths(rng, n, sigma, tau, known=None):
    """Monte Carlo: the average of one print a second over the last 60 s of a random walk, tau seconds from now."""
    out = []
    for _ in range(n):
        px, prints = 0.0, list(known or [])
        for s in range(int(tau), 0, -1):  # s = seconds left before this step
            px += rng.gauss(0, sigma)
            if s <= 60:
                prints.append(px)
        out.append(sum(prints[-60:]) / 60.0)
    return out


@pytest.mark.parametrize("tau,gap", [(240, 4.0), (90, 2.0), (45, 0.5)])
def test_model_odds_match_simulated_settlement_averages(tau, gap):
    rng, sigma = random.Random(7), 0.3
    if tau < 60:
        known = [rng.gauss(gap, 0.5) for _ in range(60 - tau)]  # the prints already in, near the live price
        avg = sum(known) / len(known)
        sims = _paths(rng, 4000, sigma, tau, known)
        mean, t_eff = average_inputs(0.0, tau, 60, avg)
    else:
        sims = _paths(rng, 4000, sigma, tau)
        mean, t_eff = average_inputs(0.0, tau, 60)
    strike = -gap  # live price is `gap` above the target
    empirical = sum(1 for s in sims if s >= strike) / len(sims)
    p, _ = average_probability(mean, strike, sigma, t_eff, True, 0.0)
    assert abs(p - empirical) < 0.03
    # the old single-price model would be clearly further off in the last minutes
    single = 0.5 * (1 + math.erf((0.0 - strike) / (sigma * math.sqrt(tau)) / math.sqrt(2)))
    if tau <= 90:
        assert abs(single - empirical) > abs(p - empirical)


def test_model_inside_the_last_minute_leans_on_the_locked_prints():
    m = Model(min_warmup_s=0)
    m.vol.var_slow = m.vol.var_fast = 6.4 ** 2
    m.vol.first_ts, m.vol.last_ts, m.vol.floor, m.vol.cap = 0.0, 10_000.0, 0.1, 100.0
    # the live price is $40 below the target, but 50 of the 60 prints are already in at $30 above it
    p = m.predict(80_016.35, 80_056.35, 10, None, 1.0, avg_sofar=80_086.35)
    assert p.p_final > 0.9 and p.settle_mean == pytest.approx((50 * 80_086.35 + 10 * 80_016.35) / 60)
    assert p.avg_sofar == 80_086.35 and p.tau_eff < 0.1
    # outside the minute the locked prints are ignored (none exist yet)
    early = m.predict(80_016.35, 80_056.35, 120, None, 1.0, avg_sofar=80_086.35)
    assert early.avg_sofar is None and early.p_final < 0.5 and early.tau_eff == pytest.approx(80.0)
    # at the close the average decides, even a second before
    last = m.predict(80_100.0, 80_056.35, 0.2, None, 1.0, avg_sofar=80_050.0)
    assert last.p_base == 0.0 and last.p_final < 0.001


def test_basis_widens_the_odds_without_blowing_up_near_the_close():
    m = Model(min_warmup_s=0)
    m.vol.var_slow = m.vol.var_fast = 0.01
    m.vol.first_ts, m.vol.last_ts, m.vol.floor, m.vol.cap = 0.0, 10_000.0, 1e-4, 1.0
    m.basis_error = 0.2
    p = m.predict(100.3, 100.0, 5, None, 1.0, avg_sofar=100.3)
    # spread = sqrt(σ²·τ_eff + basis²) ≈ basis: 0.3 above the target with a 0.2 basis is ~93%, not certainty
    assert 0.85 < p.p_base < 0.97 and p.expected_move == pytest.approx(math.sqrt(0.01 * p.tau_eff + 0.04), rel=1e-6)


def test_second_average_takes_one_print_a_second():
    ticks = [(100.0 + i, 10.0 + i) for i in range(60)]  # 100..159, price 10..69
    assert second_average(ticks, 100.0, 160.0) == pytest.approx(39.5)
    # gaps: each second takes the last price at or before it
    sparse = [(100.0, 1.0), (130.0, 3.0)]
    # seconds 100-110 read 1.0, 111-129 have nothing fresh (over 10 s old), 130-139 read 3.0
    assert second_average(sparse, 100.0, 140.0) == pytest.approx((11 * 1.0 + 10 * 3.0) / 21)
    assert second_average(sparse, 100.0, 160.0) is None  # under half the seconds have a print
    assert second_average([], 0.0, 60.0) is None and second_average(ticks, 200.0, 260.0, min_share=0.0) is None


async def test_simulated_market_settles_on_the_closing_average_and_chains_targets(tmp_path):
    import time

    from cx15m.feeds.sim import SimFeed
    from cx15m.sim import SimKalshi

    feed = SimFeed(start_price=100.0, sigma_per_sqrt_s=0.05, warmup_minutes=0)
    now = math.floor(time.time())
    close = math.floor(now / 60) * 60  # the last 1-minute window closed here (tickers name the minute)
    for i in range(200, -1, -1):  # 100 everywhere, except 105 over the 10 seconds before that close
        feed.buffer.add(now - i, 105.0 if close - 10 <= now - i < close else 100.0)
    sk = SimKalshi(feed, window_s=60, settle_avg_s=10, settle_delay_s=0.0, decimals=2, series_ticker="KXBTC15M")
    m = await sk.get_market(sk._ticker(close))
    assert m.status == "settled" and m.settle_value == pytest.approx(105.0)  # the last 10 s averaged, not the last price
    nxt = await sk.get_market(sk._ticker(close + 60))
    assert nxt.strike == pytest.approx(105.0)  # the next window's target is that settlement value


def test_desk_measures_its_closing_minute_average_against_the_settlement(tmp_path):
    import asyncio
    import time

    from cx15m.board import Board
    from cx15m.config import Config

    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.board.markets = ["btc"]
    board = Board(cfg, sim={"window_s": 900, "warmup_minutes": 0})
    asyncio.run(board.build())
    d = board.desk("btc")
    assert d.cfg.trading.settle_avg_s == 60 and d.model.settle_avg_s == 60
    close = float(int(time.time()))
    for i in range(120, 0, -1):  # 80,000 until the last minute, then 80,060 for all of it
        d.feed.buffer.add(close - i, 80_060.0 if i <= 60 else 80_000.0)
    assert d._feed_avg(close) == pytest.approx(80_060.0)
    # 30 s into the settlement minute: the prints so far (none of the old price)
    assert d._avg_sofar(close + 30, close) == pytest.approx(80_060.0)
    assert d._avg_sofar(close + 120, close) is None  # not inside the minute yet


def test_volatility_is_measured_on_kalshis_average_like_the_index_itself():
    import numpy as np

    from cx15m.model import AvgVolEstimator
    from cx15m.volcal import realized_sigma_avg

    rng = np.random.default_rng(7)
    S = 80_000 + np.cumsum(rng.normal(0, 2.0, 30000))  # a price, always positive
    cs = np.r_[0, np.cumsum(S)]
    M = (cs[60:] - cs[:-60]) / 60  # Kalshi's average: point i = mean of the 60 prints ending there
    v = AvgVolEstimator(floor=0.01, cap=100)
    pts = [(float(i), float(m)) for i, m in enumerate(M)]
    est = []
    for t, m in pts:
        v.update(t, m)
        if t > 3000 and t % 300 == 0:
            est.append(v.sigma_slow())
    assert abs(np.mean(est) - 2.0) < 0.12  # the index's own sigma per sqrt(second), unbiased within noise
    real, n = realized_sigma_avg(pts, 1000.0, 29000.0)
    assert abs(real - 2.0) < 0.06 and n > 5000
    v.update(5.0, 100.0)  # an old point (a second warm-up) is ignored
    assert v.last_ts == pts[-1][0]


def test_desks_on_the_matched_feed_measure_volatility_on_kalshis_average(tmp_path):
    from cx15m.board import Board
    from cx15m.config import Config
    from cx15m.markets import KNOWN
    from cx15m.model import AvgVolEstimator
    from cx15m.store import Store
    from cx15m.desk import Desk

    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    board = Board(cfg, client=object())
    feed = board._make_feed(KNOWN[0])
    d = Desk(KNOWN[0], board._desk_cfg("btc"), Store(":memory:"), object(), feed)
    assert isinstance(d.model.vol, AvgVolEstimator) and d.avg_feed is feed.avg
    import time
    now = time.time()
    feed.avg._publish(now - 40, 80000.0)
    feed.avg._publish(now - 5, 80010.0)
    assert d.model.vol.last_ts == now - 5  # Kalshi's average points reach the estimator
    feed.raw._publish(now - 1, 79990.0)
    assert d.model.vol.last_ts == now - 5  # the exchanges' price does not
