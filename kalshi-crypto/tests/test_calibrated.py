"""The exchanges' price matched to Kalshi's own 60-second average (Kalshi's chart series for crypto IS the settlement
average: its point one second before the close is the settlement value, checked on 72 of 72 windows)."""
import math
import random

import pytest

from cx15m.feeds.base import PriceFeed, on_backup
from cx15m.feeds.calibrated import CalibratedFeed
from cx15m.feeds.composite import FallbackFeed


class Stub(PriceFeed):
    name = "stub"

    async def start(self):
        pass

    async def stop(self):
        pass


def world(seconds=400, offset=6.57, noise=0.8, seed=1, t0=None):
    """A random-walk index S, Kalshi's trailing 60-s average M (point t = mean of S over t-59..t), and the exchanges'
    median E = S - offset + noise."""
    import time
    rng = random.Random(seed)
    t0 = int(time.time()) - seconds if t0 is None else t0
    s, S = 80_000.0, {}
    for i in range(-60, seconds):
        s += rng.gauss(0, 6.4)
        S[t0 + i] = s
    M = {t: sum(S[t - k] for k in range(60)) / 60 for t in range(t0, t0 + seconds)}
    E = {t: S[t] - offset + rng.gauss(0, noise) for t in S}
    return t0, S, M, E


def test_the_offset_is_learned_from_kalshis_average_and_applied_to_every_print():
    t0, S, M, E = world()
    raw, avg = Stub(), Stub()
    f = CalibratedFeed(raw, avg)
    for t in range(t0 - 60, t0 + 400):
        raw._publish(t + 0.05, E[t])
        if t >= t0 and t % 2 == 0:  # Kalshi's points arrive every couple of seconds
            avg._publish(float(t), M[t])
    assert f.offset == pytest.approx(6.57, abs=0.5) and f.n > 150
    last = f.latest()
    assert last.price == pytest.approx(E[t0 + 399] + f.offset) and abs(last.price - S[t0 + 399]) < 3
    assert "matched to Kalshi's average (offset +$6." in f.health().mode


def test_one_glitch_cannot_drag_the_offset():
    t0, S, M, E = world(seed=2)
    raw, avg = Stub(), Stub()
    f = CalibratedFeed(raw, avg)
    for t in range(t0 - 60, t0 + 300):
        raw._publish(t + 0.05, E[t])
        if t >= t0:
            avg._publish(float(t), M[t] + (500.0 if t == t0 + 250 else 0.0))  # one wild point from Kalshi
    assert f.offset == pytest.approx(6.57, abs=0.6)


def test_the_settlement_minute_average_comes_from_kalshis_average_and_is_exact_at_the_end():
    t0, S, M, E = world(seconds=300, noise=0.0, offset=0.0)
    close = t0 + 240
    raw, avg = Stub(), Stub()
    f = CalibratedFeed(raw, avg)
    for t in range(t0 - 60, t0 + 300):
        raw._publish(float(t), E[t])
    f.offset = 0.0
    avg._publish(float(close - 1), M[close - 1])
    exact = sum(S[t] for t in range(close - 60, close)) / 60  # the settlement: prints close-60 .. close-1
    assert f.settle_avg(close - 1 + 0.3, close) == pytest.approx(exact, abs=1e-6)
    # 20 s into the minute: prints close-60 .. close-41 from Kalshi's point at close-41 minus the older prints
    raw, avg = Stub(), Stub()
    f = CalibratedFeed(raw, avg)
    for t in range(t0 - 60, close - 40):
        raw._publish(float(t), E[t])
    f.offset = 0.0
    avg._publish(float(close - 41), M[close - 41])
    so_far = sum(S[t] for t in range(close - 60, close - 40)) / 20
    assert f.settle_avg(close - 41 + 0.2, close) == pytest.approx(so_far, abs=1e-6)
    assert f.settle_avg(close - 90, close) is None and f.settle_avg(close + 1, close) is None


def test_a_matched_backup_keeps_calls_going_and_an_unmatched_one_blocks_them():
    import time
    primary, raw, avg = Stub(), Stub(), Stub()
    primary.name, primary.mode = "kalshi-index", "index down"
    backup = CalibratedFeed(raw, avg)
    f = FallbackFeed(primary, backup, stale_s=5)
    now = time.time()
    raw._publish(now - 1, 100.0)
    assert on_backup(f) and f.health().mode.startswith("FALLBACK")  # no offset learned yet: no new calls
    backup.offset = 0.01
    avg._publish(now - 2, 100.0)
    assert not on_backup(f) and f.health().mode.startswith("backup: ")  # matched: calls go on
