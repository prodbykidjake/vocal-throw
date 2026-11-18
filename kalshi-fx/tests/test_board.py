"""The board: per-pair scale, the FX calendar, discovery, the shared rate limiter, and six simulated pairs end to end."""
import asyncio
import random
import time

import pytest

from fx15m.board import Board
from fx15m.config import Config
from fx15m.kalshi import RateLimiter
from fx15m.markets import KNOWN, Scale, decimals_of, spec_for_series
from fx15m.model import Model
from fx15m import volcal
from fx15m.desk import fmt_move, fmt_price


def test_decimals_and_tie_follow_how_kalshi_writes_the_target():
    assert decimals_of(3.34148) == 5 and decimals_of(4173.29) == 2 and decimals_of(60.707) == 3 and decimals_of(92.0) == 0
    s = Scale(decimals=2)
    s.observe_target(1693.04)   # a trailing zero is invisible in a float ...
    s.observe_target(1693.045)  # ... the board keeps the most decimals it has seen
    assert s.decimals == 3 and abs(s.tie_adj - 0.0005) < 1e-12
    assert abs(Scale(decimals=5).tie_adj - 0.000005) < 1e-12


@pytest.mark.parametrize("price,sigma", [(3.34, 0.0005), (92.0, 0.005), (4173.0, 0.15)])
def test_volatility_is_measured_at_every_price_level_never_clipped(price, sigma):
    sc = Scale()
    sc.observe_price(price)
    m = Model(min_warmup_s=0)
    m.vol.floor, m.vol.cap = sc.vol_floor(), sc.vol_cap()
    rng, px = random.Random(1), price
    for i in range(3600):
        px += rng.gauss(0, sigma)
        m.vol.update(1000.0 + i, px)
    assert 0.7 * sigma < m.vol.sigma() < 1.4 * sigma


def test_moves_read_in_pips():
    eur, jpy = Scale(decimals=5, price=1.0975), Scale(decimals=3, price=159.25)
    assert eur.pip == 0.0001 and jpy.pip == 0.01
    assert fmt_move(-0.00042, eur) == "4.2 pips" and fmt_move(0.135, jpy) == "13.5 pips"
    assert fmt_price(1.09752, 5) == "1.09752" and fmt_price(159.256, 3) == "159.256"


def test_fx_calendar_blocks_us_releases():
    by = {m.key: m for m in KNOWN}
    def et(y, mo, d, h, mi):
        import datetime as dt
        from fx15m import clock
        return dt.datetime(y, mo, d, h, mi, tzinfo=clock.ET).timestamp()
    def names(spec, span):
        extra = volcal.parse_extra_events(list(spec.dated))
        return [n for _, n in volcal.events_between(span[0], span[1], extra, spec.weekly)]
    eur = by["eurusd"]
    assert names(eur, (et(2026, 11, 19, 8, 15), et(2026, 11, 19, 8, 30))) == ["U.S. jobless claims"]  # a Thursday
    assert names(eur, (et(2026, 12, 4, 8, 15), et(2026, 12, 4, 8, 30))) == ["U.S. jobs report"]  # first Friday
    assert names(eur, (et(2026, 12, 11, 8, 15), et(2026, 12, 11, 8, 30))) == []  # second Friday
    assert names(by["usdjpy"], (et(2026, 12, 9, 13, 45), et(2026, 12, 9, 14, 0))) == ["FOMC decision"]
    assert names(by["gbpusd"], (et(2026, 12, 10, 8, 15), et(2026, 12, 10, 8, 30))) == ["CPI", "U.S. jobless claims"]  # a Thursday


def test_first_fridays():
    import datetime as dt
    from fx15m.markets import first_fridays
    assert first_fridays(dt.date(2026, 11, 1), 3) == ("2026-11-06 08:30 U.S. jobs report", "2026-12-04 08:30 U.S. jobs report",
                                                     "2027-01-01 08:30 U.S. jobs report")


def test_only_currency_pairs_get_cards():
    assert spec_for_series("KXEURUSD15M").name == "EUR/USD" and spec_for_series("KXUSDJPY15M").decimals == 3
    s = spec_for_series("KXEURGBP15M", "15-minute EURGBP")
    assert s.key == "eurgbp" and s.name == "EUR/GBP" and s.kind == "fx"
    assert spec_for_series("KX10YRRATE15M") is None and spec_for_series("KXINX15M") is None and spec_for_series("KXDJIA15M") is None


async def test_discovery_skips_tests_rates_indices_and_dormant_new_pairs():
    from fx15m.markets import discover
    class Client:
        async def list_series_raw(self, category):
            return [{"ticker": t} for t in ("KX30YRRATE15M", "KXDJIA15M", "KXUSDMXN15M", "KXGBPUSD15M", "KXGBPUSD15MTEST",
                                            "KXINX15M", "KXUSDJPY15M", "KXAUDUSD15M", "KXUSDCAD15M", "KXEURUSD15M",
                                            "KXNDQ15M", "KXEURGBP15M", "KXEURJPY15M", "KXEURUSD")]
        async def get_markets_raw(self, ticker, status=None):
            return [{"ticker": ticker + "-X"}] if ticker == "KXEURJPY15M" else []
    got = [m.series for m in await discover(Client())]
    assert got == ["KXEURUSD15M", "KXGBPUSD15M", "KXUSDJPY15M", "KXAUDUSD15M", "KXUSDCAD15M", "KXUSDMXN15M", "KXEURJPY15M"]


async def test_rate_limiter_spaces_requests():
    lim = RateLimiter(per_s=20)
    t0 = time.monotonic()
    for _ in range(6):
        await lim.acquire()
    assert time.monotonic() - t0 >= 0.24  # 6 requests at 20/s: at least 5 gaps of 50 ms
    lim.penalize(0.2)
    t1 = time.monotonic()
    await lim.acquire()
    assert time.monotonic() - t1 >= 0.15 and lim.throttled == 1


async def test_six_simulated_pairs_each_get_a_card_with_its_own_scale(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.trading.min_warmup_minutes = 1
    board = Board(cfg, sim={"window_s": 120, "warmup_minutes": 10})
    await board.build()
    stop = asyncio.Event()
    task = asyncio.create_task(board.run(stop))
    await asyncio.sleep(4)
    stop.set()
    await task
    st = board.state
    assert [m["key"] for m in st["markets"]] == [m.key for m in KNOWN]
    by = {m["key"]: m for m in st["markets"]}
    assert all(m["status"] == "live" and m["prediction"] for m in st["markets"])
    assert by["eurusd"]["scale"]["decimals"] == 5 and by["usdjpy"]["scale"]["decimals"] == 3
    assert by["eurusd"]["scale"]["pip"] == 0.0001 and by["usdjpy"]["scale"]["pip"] == 0.01
    # every market keeps its own database
    assert sorted(p.name for p in tmp_path.glob("*.db")) == sorted([f"{m.key}.db" for m in KNOWN] + ["edges.db", "pullouts.db"])
    assert all("edge" in m and "watch" in m["edge"] and "record" in m["edge"] for m in st["markets"])
    assert st["edge"]["wins"] + st["edge"]["losses"] >= 0
    # v1.8: each card's sparkline of the window's price, and the board's TODAY record
    assert all(isinstance(m.get("spark"), list) and 1 <= len(m["spark"]) <= board.SPARK_MAX for m in st["markets"])
    assert st["today"] == {"wins": 0, "losses": 0}
    board.set_muted("eurusd", True)
    assert Board(cfg).muted == {"eurusd"}  # remembered across restarts


def test_sparkline_samples_every_ten_seconds_and_restarts_with_each_window(tmp_path):
    import types
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    board = Board(cfg)
    d = types.SimpleNamespace(spec=types.SimpleNamespace(key="gold"), state={"market": {"ticker": "A"}, "tick": {"price": 1.0}})
    for i in range(200):
        d.state["tick"] = {"price": 1.0 + i}
        board._spark_step(d, 1000.0 + i)
    assert len(d.state["spark"]) == 20 and d.state["spark"][:2] == [1.0, 11.0]
    for i in range(3000):
        board._spark_step(d, 2000.0 + i)
    assert len(d.state["spark"]) == board.SPARK_MAX
    d.state["market"] = {"ticker": "B"}
    board._spark_step(d, 9000.0)
    assert len(d.state["spark"]) == 1
