import datetime as dt
import math
import random

import pytest

from cx15m import clock
from cx15m.config import TradingCfg
from cx15m.model import Model
from cx15m.volcal import VolProfile, VolRow, events_between, parse_extra_events, realized_sigma


def et(y, mo, d, h, mi=0):
    return dt.datetime(y, mo, d, h, mi, tzinfo=clock.ET).timestamp()


def test_realized_sigma_recovers_a_random_walk():
    rng = random.Random(1)
    sigma = 0.004  # dollars per sqrt(second)
    ticks, px = [], 70.0
    for i in range(720):  # one tick a second for 12 minutes
        px += rng.gauss(0, sigma)
        ticks.append((1000.0 + i, px))
    est, n = realized_sigma(ticks, 1000.0, 1720.0)
    assert n > 100 and est == pytest.approx(sigma, rel=0.2)
    assert realized_sigma(ticks[:20], 1000.0, 1720.0) == (None, 3)  # too few returns: no number


def test_maintenance_and_dated_reports_are_found_in_et():
    from cx15m.markets import KNOWN
    weekly = KNOWN[0].weekly  # Kalshi's Thursday maintenance marks
    assert [n for _, n in events_between(et(2026, 10, 15, 3, 0), et(2026, 10, 15, 3, 15), None, weekly)] == ["Kalshi maintenance"]
    assert events_between(et(2026, 10, 15, 5, 0), et(2026, 10, 15, 5, 15), None, weekly) == []
    assert events_between(et(2026, 10, 14, 10, 15), et(2026, 10, 14, 10, 31)) == []  # no weekly marks by default
    extra = parse_extra_events(["2026-11-06 08:30 Jobs report", "garbage"])
    assert len(extra) == 1 and events_between(et(2026, 11, 6, 8, 15), et(2026, 11, 6, 8, 31), extra)[0][1] == "Jobs report"


def test_profile_learns_a_busy_hour_and_shrinks_with_little_data():
    rows = []
    days = [d for d in range(1, 16) if dt.date(2026, 10, d).weekday() < 5][:10]  # ten weekdays
    for day in days:
        base = et(2026, 10, day, 0)
        for hour in (3, 9):  # 3 am ET: forecast right; 9 am ET (U.S. open): really moves 1.6x the forecast
            rows.append(VolRow(base + hour * 3600, 0.004, 0.004 * (1.6 if hour == 9 else 1.0)))
    prof = VolProfile(k=8)
    prof.fit(rows)
    m9, why9, n9 = prof.mult(et(2026, 10, 20, 9, 15))
    m3, _, _ = prof.mult(et(2026, 10, 20, 3, 15))
    assert n9 == 10 and 1.25 < m9 < 1.6 and "weekday 9 am" in why9  # most of the way to 1.6 after 10 windows, not all
    assert 0.95 < m3 < 1.15  # pulled slightly toward the overall level
    assert prof.error_fit < prof.error_raw  # the correction actually helps out of sample
    one = VolProfile(k=8)
    one.fit([VolRow(et(2026, 10, 1, 9), 0.004, 0.012)])  # a single 3x surprise moves the hour only a little
    assert one.mult(et(2026, 10, 2, 9))[0] < 1.3
    assert VolProfile().mult(et(2026, 10, 2, 9)) == (1.0, "no measured windows yet: using the live volatility as is", 0)


def test_weekend_hours_are_learned_apart_from_weekdays():
    rows = []
    for day in range(1, 29):  # four weeks: weekends really move 0.6x the live estimate, weekdays as forecast
        weekend = dt.date(2026, 10, day).weekday() >= 5
        for hour in (2, 14):
            rows.append(VolRow(et(2026, 10, day, hour), 0.004, 0.004 * (0.6 if weekend else 1.0)))
    prof = VolProfile(k=8)
    prof.fit(rows)
    sat, why, n = prof.mult(et(2026, 10, 31, 14, 0))  # a Saturday
    tue, _, _ = prof.mult(et(2026, 10, 27, 14, 0))
    assert n == 8 and sat < 0.8 and "weekend 2 pm" in why and 0.9 < tue < 1.05
    # a weekend hour never seen falls back to the weekend level, not to "no correction"
    new_hour, _, n0 = prof.mult(et(2026, 10, 31, 20, 0))
    assert n0 == 0 and new_hour < 0.85
    assert prof.summary()["weekend"] < 0.85 and "weekend 14" in prof.summary()["hours"]


def test_report_windows_use_their_own_multiplier_with_a_prior():
    prof = VolProfile(k=8, event_prior=1.5)
    prof.fit([])
    m, why, n = prof.mult(et(2026, 10, 14, 10, 15), [(et(2026, 10, 14, 10, 30), "EIA inventories")])
    assert m == pytest.approx(1.5) and n == 0 and "EIA" in why
    prof.fit([VolRow(et(2026, 10, d, 10, 15), 0.004, 0.012, "EIA inventories") for d in range(1, 9)])
    m2, _, n2 = prof.mult(et(2026, 10, 14, 10, 15), [(0, "EIA inventories")])
    assert n2 == 8 and 1.5 < m2 < 3.0


def test_model_applies_the_multiplier_to_the_odds_not_to_the_learner_features():
    m = Model(min_warmup_s=0)
    rng = random.Random(2)
    px = 70.0
    for i in range(3600):
        px += rng.gauss(0, 0.004)
        m.vol.update(1000.0 + i, px)
    plain = m.predict(70.20, 70.00, 600, None, 0.5)
    m.vol_mult, m.vol_note = 2.0, "test hour moves x2.00"
    wide = m.predict(70.20, 70.00, 600, None, 0.5)
    assert wide.sigma == pytest.approx(2 * plain.sigma) and wide.sigma_raw == pytest.approx(plain.sigma_raw)
    assert 0.5 < wide.p_base < plain.p_base  # more expected movement: a 20c lead is worth less
    assert wide.features[3:6] == plain.features[3:6]  # momentum features stay on the raw scale
    assert any("volatility" in n for n in wide.notes)

