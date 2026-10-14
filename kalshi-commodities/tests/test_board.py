"""The board: per-market scale, calendars, the shared rate limiter, and seven simulated markets end to end."""
import asyncio
import random
import time

import pytest

from cm15m.board import Board
from cm15m.config import Config
from cm15m.kalshi import RateLimiter
from cm15m.markets import KNOWN, Scale, decimals_of, spec_for_series
from cm15m.model import Model
from cm15m import volcal
from cm15m.desk import fmt_move, fmt_price


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


def test_moves_read_in_cents_or_dollars_by_price_level():
    gas, gold = Scale(decimals=5, price=3.3), Scale(decimals=2, price=4170.0)
    assert fmt_move(-0.00214, gas) == "0.214¢" and fmt_move(2.0, gold) == "$2.00"
    assert fmt_price(4173.29, 2) == "$4,173.29" and fmt_price(3.34148, 5) == "$3.34148"


def test_each_kind_of_market_has_its_own_report_calendar():
    by = {m.key: m for m in KNOWN}
    def et(y, mo, d, h, mi):
        import datetime as dt
        from cm15m import clock
        return dt.datetime(y, mo, d, h, mi, tzinfo=clock.ET).timestamp()
    thu_830 = (et(2026, 10, 15, 8, 15), et(2026, 10, 15, 8, 30))
    thu_1030 = (et(2026, 10, 15, 10, 15), et(2026, 10, 15, 10, 30))
    wed_1030 = (et(2026, 10, 14, 10, 15), et(2026, 10, 14, 10, 30))
    def names(spec, span):
        extra = volcal.parse_extra_events(list(spec.dated))
        return [n for _, n in volcal.events_between(span[0], span[1], extra, spec.weekly)]
    assert names(by["gold"], thu_830) == ["Jobless claims"] and names(by["wti"], thu_830) == []
    assert names(by["natgas"], thu_1030) == ["EIA gas storage"] and names(by["gold"], thu_1030) == []
    assert names(by["wti"], wed_1030) == ["EIA inventories"] and names(by["natgas"], wed_1030) == []
    assert names(by["silver"], (et(2026, 10, 28, 13, 45), et(2026, 10, 28, 14, 0))) == ["FOMC decision"]


def test_a_new_series_gets_a_generic_card():
    s = spec_for_series("KXALUMINIUM15M", "Aluminium 15 min")
    assert s.key == "aluminium" and s.name == "Aluminium" and s.kind == "metal"
    assert spec_for_series("KXGOLD15M").name == "Gold"


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


async def test_seven_simulated_markets_each_get_a_card_with_its_own_scale(tmp_path):
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
    assert by["natgas"]["scale"]["decimals"] == 5 and by["gold"]["scale"]["cents"] is False and by["wti"]["scale"]["cents"] is True
    # every market keeps its own database
    assert sorted(p.name for p in tmp_path.glob("*.db")) == sorted([f"{m.key}.db" for m in KNOWN] + ["edges.db"])
    assert all("edge" in m and "watch" in m["edge"] and "record" in m["edge"] for m in st["markets"])
    assert st["edge"]["wins"] + st["edge"]["losses"] >= 0
    board.set_muted("gold", True)
    assert Board(cfg).muted == {"gold"}  # remembered across restarts


async def test_wti_gas_and_gold_run_their_own_coach_engines(tmp_path):
    from cm15m.engines import CallsOnlyNotifier, EngineDesk
    from wti15m import __version__ as wti_version
    from wti15m.engine import Engine as WtiEngine
    from ng15m.engine import Engine as GasEngine
    from gold15m.engine import Engine as GoldEngine

    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.trading.min_warmup_minutes = 1
    board = Board(cfg, sim={"window_s": 120, "warmup_minutes": 10})
    await board.build()
    kinds = {d.spec.key: type(getattr(d, "engine", d)) for d in board.desks}
    assert kinds["wti"] is WtiEngine and kinds["natgas"] is GasEngine and kinds["gold"] is GoldEngine
    assert wti_version.startswith("0.12")  # the v12 line
    assert not isinstance(board.desk("silver"), EngineDesk)  # the other four use the board's own desks
    stop = asyncio.Event()
    task = asyncio.create_task(board.run(stop))
    await asyncio.sleep(4)
    stop.set()
    await task
    by = {m["key"]: m for m in board.state["markets"]}
    assert by["wti"]["engine"] == "WTI v12 engine" and by["wti"]["prediction"] and "meter" in by["wti"]["call"]
    assert by["natgas"]["engine"].startswith("gas") and by["gold"]["engine"].startswith("gold")
    assert board.desk("wti").detail()["chart"]["ticks"]
    # the engines' BUY/SELL and plan notifications stay quiet on the board; settlement calls go through unless muted
    sent = []
    class Inner:
        def notify(self, title, message, sound_name="Glass"):
            sent.append(title)
    n = CallsOnlyNotifier(Inner(), "wti", lambda k: False)
    n.notify("WTI 15m: BUY UP", "x"); n.notify("WTI 15m: settles UP", "y")
    CallsOnlyNotifier(Inner(), "wti", lambda k: True).notify("WTI 15m: settles DOWN", "z")
    assert sent == ["WTI 15m: settles UP"]
