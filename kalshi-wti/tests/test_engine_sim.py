import asyncio

import pytest

from wti15m.config import Config
from wti15m.engine import Engine
from wti15m.feeds.sim import SimFeed
from wti15m.notify import Notifier
from wti15m.sim import SimKalshi
from wti15m.store import Store


@pytest.mark.asyncio
async def test_engine_builds_live_state_on_simulated_market(tmp_path):
    cfg = Config()
    cfg.trading.min_warmup_minutes = 1
    cfg.trading.no_entry_first_s = 0
    feed = SimFeed(start_price=90.0, sigma_per_sqrt_s=0.005, warmup_minutes=5, seed=1)
    client = SimKalshi(feed, window_s=120)
    store = Store(str(tmp_path / "e.db"))
    engine = Engine(cfg, store, client, feed, Notifier(desktop=False, sound=False))
    await feed.start()
    try:
        await asyncio.sleep(0.7)  # first live tick (the warm-up closes alone read as "stale")
        await engine.tracker.poll_once()
        engine.step()
        st = engine.state
        assert st["status"] == "live"
        assert st["market"]["strike"] is not None
        assert st["prediction"]["confidence"] in ("confident", "lean", "coinflip")
        assert st["signal"]["action"] in ("BUY", "WAIT")
        assert st["feed"]["symbol"] == "SIM:WTI"
        chart = engine.chart(5)
        assert chart["ticks"] and chart["strike"] == st["market"]["strike"]
        # user position round trip
        pos = engine.open_position("DOWN", 2, 0.5)
        engine.step()
        assert engine.state["position"]["qty"] == 2
        assert engine.state["signal"]["action"] in ("HOLD", "SELL")
        res = engine.close_position(0.6)
        assert "pnl" in res and engine.position is None
        with pytest.raises(ValueError):
            engine.open_position("SIDEWAYS", 1, 0.5)
    finally:
        await feed.stop()
        store.close()


@pytest.mark.asyncio
async def test_next_target_resolves_previous_settlement(tmp_path):
    import time as _time

    from wti15m.kalshi import Market

    cfg = Config()
    cfg.trading.min_warmup_minutes = 1
    feed = SimFeed(start_price=90.0, sigma_per_sqrt_s=0.005, warmup_minutes=10, seed=2)
    client = SimKalshi(feed, window_s=120)
    store = Store(str(tmp_path / "r.db"))
    engine = Engine(cfg, store, client, feed, Notifier(desktop=False, sound=False))
    await feed.start()
    try:
        await asyncio.sleep(0.7)
        now = _time.time()
        close_ts = now - 200.0
        prev = Market.from_api({"ticker": "PREV", "status": "closed", "floor_strike": 90.00,
                                "open_time": close_ts - 900, "close_time": close_ts})
        store.upsert_window(prev)
        engine._close_capture["PREV"] = {"feed_price": 90.02, "p_market": 0.6, "close_ts": close_ts, "p_model": 0.55,
                                         "strike": 90.00, "settle_price": None, "err_lag0": None, "err_lag60": None}
        engine.tracker.current = Market.from_api({"ticker": "CUR", "status": "open", "floor_strike": 90.05,
                                                  "open_time": close_ts, "close_time": close_ts + 900})
        engine._resolve_closed_windows(now)
        w = store.window("PREV")
        assert w["settle_price"] == 90.05
        assert w["feed_error_lag0"] is not None and w["feed_error_lag60"] is not None
        assert w["feed_error"] == w["feed_error_lag60"]  # settle_lag_s = 60 by default
        assert engine._close_capture["PREV"]["settle_price"] == 90.05
        # settlement result arrives later: uses the known settle price, labels snapshots
        settled = Market.from_api({"ticker": "PREV", "status": "finalized", "result": "yes", "floor_strike": 90.00,
                                   "expiration_value": "90.05", "open_time": close_ts - 900, "close_time": close_ts})
        engine.handle_settlement(settled)
        w = store.window("PREV")
        assert w["result"] == "yes" and w["settle_price"] == 90.05 and w["feed_error"] is not None
        assert engine.model.prev_outcome == 1.0
    finally:
        await feed.stop()
        store.close()
