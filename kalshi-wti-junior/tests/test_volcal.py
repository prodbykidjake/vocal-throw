import datetime as dt
import math
import random

import pytest

from wtijr import clock
from wtijr.config import TradingCfg
from wtijr.decision import DecisionEngine, Quotes
from wtijr.model import Model
from wtijr.volcal import VolProfile, VolRow, events_between, parse_extra_events, realized_sigma


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


def test_weekly_oil_reports_are_found_in_et():
    # Wednesday 2026-10-14: EIA at 10:30 ET falls in the 10:15-10:30 window (settles on the 10:30 candle) and the next
    eia = et(2026, 10, 14, 10, 30)
    assert [n for _, n in events_between(et(2026, 10, 14, 10, 15), et(2026, 10, 14, 10, 31))] == ["EIA inventories"]
    assert [n for _, n in events_between(et(2026, 10, 14, 10, 30), et(2026, 10, 14, 10, 46))] == ["EIA inventories"]
    assert events_between(et(2026, 10, 14, 10, 45), et(2026, 10, 14, 11, 1)) == []
    assert events_between(eia - 7200, eia - 3600) == []
    assert [n for _, n in events_between(et(2026, 10, 13, 16, 15), et(2026, 10, 13, 16, 31))] == ["API inventories"]
    assert [n for _, n in events_between(et(2026, 10, 16, 12, 45), et(2026, 10, 16, 13, 1))] == ["Baker Hughes rigs"]
    extra = parse_extra_events(["2026-11-27 11:00 EIA inventories (holiday)", "garbage"])
    assert len(extra) == 1 and events_between(et(2026, 11, 27, 10, 45), et(2026, 11, 27, 11, 1), extra)[0][1].startswith("EIA")


def test_profile_learns_a_busy_hour_and_shrinks_with_little_data():
    rows = []
    for day in range(10):
        base = et(2026, 10, 1 + day, 0)
        for hour in (3, 9):  # 3 am ET: forecast right; 9 am ET (the open): really moves 1.6x the forecast
            rows.append(VolRow(base + hour * 3600, 0.004, 0.004 * (1.6 if hour == 9 else 1.0)))
    prof = VolProfile(k=8)
    prof.fit(rows)
    m9, why9, n9 = prof.mult(et(2026, 10, 20, 9, 15))
    m3, _, _ = prof.mult(et(2026, 10, 20, 3, 15))
    assert n9 == 10 and 1.25 < m9 < 1.6 and "9 am" in why9  # most of the way to 1.6 after 10 windows, not all
    assert 0.95 < m3 < 1.15  # pulled slightly toward the overall level
    assert prof.error_fit < prof.error_raw  # the correction actually helps out of sample
    one = VolProfile(k=8)
    one.fit([VolRow(et(2026, 10, 1, 9), 0.004, 0.012)])  # a single 3x surprise moves the hour only a little
    assert one.mult(et(2026, 10, 2, 9))[0] < 1.3
    assert VolProfile().mult(et(2026, 10, 2, 9)) == (1.0, "no measured windows yet: using the live volatility as is", 0)


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


def test_study_period_and_report_block_entries():
    cfg = TradingCfg(min_warmup_minutes=0, study_s=180.0)
    eng = DecisionEngine(cfg)
    m = Model(min_warmup_s=0)
    for i in range(3600):
        m.vol.update(1000.0 + i, 70 + 0.01 * math.sin(i / 7))
    pred = m.predict(70.30, 70.00, 600, 0.6, 0.5)
    q = Quotes(0.58, 0.60, 0.40, 0.42)
    eng.context = {"study": {"moves_x": 1.4, "drift": 0.3, "odds_lo": 0.5, "odds_hi": 0.62}, "events": []}
    sig = eng.decide(pred, q, 70.30, 70.00, 600, 100)
    assert sig.action == "WAIT" and sig.reasons == ["studying"] and "1:20" in sig.headline
    assert any("1.4×" in d for d in sig.details) and any("50%–62%" in d for d in sig.details)
    eng.context = {"events": [{"name": "EIA inventories", "at": "10:30am ET"}]}
    sig = eng.decide(pred, q, 70.30, 70.00, 600, 300)
    assert sig.action == "WAIT" and sig.reasons == ["report"] and "EIA" in sig.headline
    cfg.event_entries = "allow"
    assert eng.decide(pred, q, 70.30, 70.00, 600, 300).reasons != ["report"]
