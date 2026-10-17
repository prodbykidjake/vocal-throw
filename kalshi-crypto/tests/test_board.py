"""The board: per-coin scale, calendars, the shared rate limiter, and nine simulated coins end to end."""
import asyncio
import random
import time

import pytest

from cx15m.board import Board
from cx15m.config import Config
from cx15m.kalshi import RateLimiter
from cx15m.markets import KNOWN, Scale, decimals_of, spec_for_series
from cx15m.model import Model
from cx15m import volcal
from cx15m.desk import fmt_move, fmt_price


def test_decimals_and_tie_follow_how_kalshi_writes_the_target():
    assert decimals_of(0.0885266) == 7 and decimals_of(80056.35) == 2 and decimals_of(103.7542) == 4 and decimals_of(92.0) == 0
    s = Scale(decimals=2)
    s.observe_target(709.8)    # a trailing zero is invisible in a float ...
    s.observe_target(709.825)  # ... the board keeps the most decimals it has seen
    assert s.decimals == 3 and abs(s.tie_adj - 0.0005) < 1e-12
    assert abs(Scale(decimals=7).tie_adj - 0.00000005) < 1e-15
    by = {m.key: m for m in KNOWN}
    assert [by[k].decimals for k in ("btc", "eth", "bnb")] == [2, 2, 2] and by["doge"].decimals == 7
    assert all(by[k].decimals == 4 for k in ("sol", "xrp", "hype", "near", "zec"))
    assert by["btc"].index == "BRTI" and by["eth"].index == "ETHUSD_RTI" and all(m.index for m in KNOWN)


@pytest.mark.parametrize("price,sigma", [(0.0885, 0.0000134), (1.5, 0.00022), (103.75, 0.014), (80056.0, 6.4)])
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


def test_moves_read_in_cents_or_dollars_per_coin():
    by = {m.key: m for m in KNOWN}
    doge = Scale(decimals=7, price=0.0885, cents=by["doge"].cents)
    btc = Scale(decimals=2, price=80056.0, cents=by["btc"].cents)
    zec = Scale(decimals=4, price=815.0, cents=by["zec"].cents)
    assert fmt_move(-0.0000214, doge) == "0.00214¢" and fmt_move(25.0, btc) == "$25.00" and fmt_move(1.2, zec) == "$1.20"
    assert fmt_price(80056.35, 2) == "$80,056.35" and fmt_price(0.0885266, 7) == "$0.0885266"


def _et(y, mo, d, h, mi):
    import datetime as dt
    from cx15m import clock
    return dt.datetime(y, mo, d, h, mi, tzinfo=clock.ET).timestamp()


def test_thursday_maintenance_and_us_reports_block_their_windows():
    btc = {m.key: m for m in KNOWN}["btc"]
    def names(span):
        extra = volcal.parse_extra_events(list(btc.dated))
        return [n for _, n in volcal.events_between(span[0], span[1], extra, btc.weekly)]
    thu = 15  # Thursday 2026-10-15
    assert names((_et(2026, 10, thu, 2, 45), _et(2026, 10, thu, 3, 0))) == []  # the window before: trades
    for start in range(3 * 60, 5 * 60, 15):  # every window 3:00-5:00 AM ET holds exactly one maintenance mark
        h, mi = divmod(start, 60)
        assert names((_et(2026, 10, thu, h, mi), _et(2026, 10, thu, h, mi) + 900)) == ["Kalshi maintenance"]
    assert names((_et(2026, 10, thu, 5, 0), _et(2026, 10, thu, 5, 15))) == []
    assert names((_et(2026, 10, 14, 3, 0), _et(2026, 10, 14, 3, 15))) == []  # Wednesday: no maintenance
    assert names((_et(2026, 10, 28, 13, 45), _et(2026, 10, 28, 14, 0))) == ["FOMC decision"]
    assert names((_et(2026, 11, 10, 8, 15), _et(2026, 11, 10, 8, 30))) == ["CPI"]


def test_a_new_series_gets_a_generic_card():
    s = spec_for_series("KXADA15M", "Cardano 15 min")
    assert s.key == "ada" and s.name == "Cardano" and s.kind == "crypto" and s.badge == "ADA"
    assert spec_for_series("KXBTC15M").name == "Bitcoin"


async def test_discovery_keeps_the_known_nine_and_adds_new_15_minute_coins():
    from cx15m.markets import discover

    class Client:
        async def list_series_raw(self, category):
            return [{"ticker": "KXBTC15M"}, {"ticker": "KXBTCD"}, {"ticker": "KXADA15M", "title": "Cardano 15 min"},
                    {"ticker": "KXBTC15MLEAD"}]
    specs = await discover(Client())
    assert [s.series for s in specs] == [m.series for m in KNOWN] + ["KXADA15M"]

    class Broken:
        async def list_series_raw(self, category):
            raise RuntimeError("HTTP 500")
    assert [s.key for s in await discover(Broken())] == [m.key for m in KNOWN]


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


async def test_nine_simulated_coins_each_get_a_card_with_its_own_scale(tmp_path):
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
    assert by["doge"]["scale"]["decimals"] == 7 and by["btc"]["scale"]["cents"] is False and by["sol"]["scale"]["cents"] is True
    # the demo's 120-second windows settle on a 8-second closing average (60 s of 900, scaled)
    assert board.desk("btc").cfg.trading.settle_avg_s == 8 and board.desk("btc").model.settle_avg_s == 8
    # every coin keeps its own database
    assert sorted(p.name for p in tmp_path.glob("*.db")) == sorted([f"{m.key}.db" for m in KNOWN] + ["edges.db", "pullouts.db"])
    assert all("edge" in m and "watch" in m["edge"] and "record" in m["edge"] for m in st["markets"])
    assert st["edge"]["wins"] + st["edge"]["losses"] >= 0
    board.set_muted("btc", True)
    assert Board(cfg).muted == {"btc"}  # remembered across restarts


def test_the_price_source_follows_the_api_key(tmp_path, monkeypatch):
    from cx15m.feeds.composite import FallbackFeed
    from cx15m.feeds.exchanges import ExchangeFeed
    from cx15m.feeds.kalshi_live import KalshiLiveFeed

    monkeypatch.delenv("KALSHI_API_KEY_ID", raising=False)
    monkeypatch.delenv("KALSHI_PRIVATE_KEY_PATH", raising=False)
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    board = Board(cfg, client=object())
    assert board.feed_source() == "exchanges"
    feeds = [board._make_feed(m) for m in KNOWN]
    assert all(isinstance(f, ExchangeFeed) for f in feeds) and board.exchange_hub.assets == {m.badge for m in KNOWN}
    cfg.kalshi.api_key_id, cfg.kalshi.private_key_path = "key-id", str(tmp_path / "key.pem")
    board2 = Board(cfg, client=object())
    assert board2.feed_source() == "kalshi-index"
    f = board2._make_feed(KNOWN[0])
    assert isinstance(f, FallbackFeed) and f.primary.index_id == "BRTI" and isinstance(f.backup, ExchangeFeed)
    cfg.feed.source = "kalshi-live"
    assert isinstance(Board(cfg, client=object())._make_feed(KNOWN[0]), KalshiLiveFeed)
    cfg.feed.source, cfg.kalshi.api_key_id = "kalshi-index", ""
    assert Board(cfg, client=object()).feed_source() == "exchanges"  # no key: falls back with a warning
