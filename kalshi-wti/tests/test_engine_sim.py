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
