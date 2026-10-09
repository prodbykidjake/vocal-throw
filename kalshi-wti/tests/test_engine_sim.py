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
        # user position round trip: $3 of DOWN at the live ask -> fractional shares, live cash-out, scalp box
        pos = engine.open_position("DOWN", 3.0)
        assert pos.avg_price == st["market"]["no_ask"] and abs(pos.qty * pos.avg_price - 3.0) < 1e-9
        engine.step()
        ps = engine.state["position"]
        assert abs(ps["qty"] - pos.qty) < 1e-3 and ps["live"]["cash_out"] is not None and ps["live"]["pnl"] is not None
        assert ps["scalp"]["action"] in ("SELL NOW", "SELL BETWEEN", "SELL AT", "HOLD")
        assert engine.state["signal"]["action"] in ("HOLD", "SELL")
        if ps["scalp"]["action"] == "SELL BETWEEN":  # the zone was planned once and persisted on the position
            assert ps["target"] == ps["scalp"]["target"] and ps["target_high"] == ps["scalp"]["target_high"] and ps["zone_ts"] > 0
            assert store.get_open_position()["target"] == ps["target"]
        res = engine.close_position()  # at the live bid
        assert "pnl" in res and "cash_out" in res and engine.position is None
        with pytest.raises(ValueError):
            engine.open_position("SIDEWAYS", 1.0)
        engine.open_position("UP", 2.0, 0.40)
        with pytest.raises(ValueError):
            engine.close_position(5.0)  # not a contract price
        engine.cancel_position()
        rows = engine.store.positions()
        assert engine.position is None and len(rows) == 1 and rows[0]["closed_ts"] is not None  # only the real trade remains
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
        # recorded ticks around the close and around the settlement candle's close (the buffer only has 60 s samples there)
        store.add_tick(close_ts - 1.0, "sim", 90.02)
        store.add_tick(close_ts + 58.0, "sim", 90.03)
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


@pytest.mark.asyncio
async def test_recovery_requeues_unsettled_window_and_settles_restored_position(tmp_path):
    import time as _time

    from wti15m.kalshi import Market

    cfg = Config()
    store = Store(str(tmp_path / "rec.db"))
    now = _time.time()
    close_ts = now - 120
    old_win = Market.from_api({"ticker": "OLD", "status": "open", "floor_strike": 90.0,
                               "open_time": close_ts - 900, "close_time": close_ts})
    store.upsert_window(old_win)
    store.add_tick(close_ts - 1, "sim", 90.07)
    # a position left open on a window that has since settled
    settled = Market.from_api({"ticker": "DONE", "status": "finalized", "result": "yes", "floor_strike": 89.0,
                               "open_time": close_ts - 1800, "close_time": close_ts - 900})
    store.upsert_window(settled)
    store.open_position("DONE", "UP", 10.0, 0.30, now - 1000, "", 3.0, 0.02, 0.35)
    feed = SimFeed(start_price=90.0, warmup_minutes=5, seed=3)
    client = SimKalshi(feed, window_s=120)
    engine = Engine(cfg, store, client, feed, Notifier(desktop=False, sound=False))
    assert "OLD" in engine.tracker.pending
    assert engine._close_capture["OLD"]["feed_price"] == 90.07  # rebuilt from the ticks table
    assert engine._window_strikes[int(round(close_ts - 900))] == 90.0
    assert engine.position is None  # settled at 1.0: pnl = 10 - 3.0
    pos_rows = store.positions()
    assert pos_rows and pos_rows[0]["exit_price"] == 1.0 and abs(pos_rows[0]["pnl"] - 6.98) < 1e-9  # 10 - 3.00 - 0.02 fee
    store.close()


def test_basis_is_kept_per_feed_source(tmp_path):
    from wti15m.kalshi import Market

    cfg = Config()
    store = Store(str(tmp_path / "b.db"))
    feed = SimFeed(start_price=90.0, warmup_minutes=5, seed=4)
    engine = Engine(cfg, store, SimKalshi(feed, window_s=120), feed, Notifier(desktop=False, sound=False))
    # errors measured while on the fallback feed: a consistent -6c (with a little noise) over 7 windows
    errs = [-0.06, -0.055, -0.065, -0.06, -0.058, -0.062, -0.06]
    for i, e in enumerate(errs):
        m = Market.from_api({"ticker": f"H{i}", "status": "finalized", "result": "yes", "floor_strike": 90.0,
                             "open_time": 1000 + i * 900, "close_time": 1900 + i * 900})
        store.upsert_window(m)
        store.set_settle_price(f"H{i}", 90.0, 90.0 + e, e, e, e, "hyperliquid")
        engine._update_basis_error("hyperliquid")
        if i < 5:  # fewer than basis_min_windows: measured but NOT applied
            assert float(store.get_state("basis_signed:hyperliquid")) == 0.0
            assert abs(float(store.get_state("basis_raw:hyperliquid"))) > 0.02
    assert engine.model.basis_signed == 0.0  # the active (sim) feed has no measured bias
    engine._load_basis("hyperliquid")
    assert engine.model.basis_signed < -0.04 and engine.model.basis_error >= 0.05
    engine._load_basis("kalshi-live")
    assert engine.model.basis_signed == 0.0 and engine.model.basis_error == 0.0
    assert store.feed_errors("hyperliquid")[0] == -0.06 and store.feed_errors("kalshi-live") == []
    assert any("not applied" in ev["text"] for ev in engine.events) and any("applied" in ev["text"] for ev in engine.events)
    store.close()


def test_basis_is_not_applied_when_the_sign_keeps_flipping(tmp_path):
    from wti15m.kalshi import Market

    cfg = Config()
    store = Store(str(tmp_path / "c.db"))
    feed = SimFeed(start_price=90.0, warmup_minutes=5, seed=4)
    engine = Engine(cfg, store, SimKalshi(feed, window_s=120), feed, Notifier(desktop=False, sound=False))
    for i, e in enumerate([0.02, -0.03, 0.025, -0.02, 0.03, -0.025, 0.022, -0.028]):
        m = Market.from_api({"ticker": f"K{i}", "status": "finalized", "result": "yes", "floor_strike": 90.0,
                             "open_time": 1000 + i * 900, "close_time": 1900 + i * 900})
        store.upsert_window(m)
        store.set_settle_price(f"K{i}", 90.0, 90.0 + e, e, e, e, "kalshi-live")
    engine._update_basis_error("kalshi-live")
    assert float(store.get_state("basis_signed:kalshi-live")) == 0.0  # noise, not a bias: nothing is shifted
    assert float(store.get_state("basis_error:kalshi-live")) > 0.02  # ... but the uncertainty allowance is kept
    store.close()


def test_store_prune_and_settled_count(tmp_path):
    import time as _time

    from wti15m.kalshi import Market

    store = Store(str(tmp_path / "p.db"))
    store.add_tick(_time.time() - 10 * 86400, "x", 1.0)
    store.add_tick(_time.time(), "x", 2.0)
    store.prune()
    assert store.ticks_since(0) == [(pytest.approx(_time.time(), abs=5), 2.0)]
    store.upsert_window(Market.from_api({"ticker": "A", "status": "finalized", "result": "no"}))
    store.upsert_window(Market.from_api({"ticker": "B", "status": "open"}))
    assert store.settled_window_count() == 1
    store.close()


def test_quick_scalp_target_becomes_the_zone_low(tmp_path):
    cfg = Config()
    store = Store(str(tmp_path / "q.db"))
    feed = SimFeed(start_price=90.0, warmup_minutes=40, seed=3)
    engine = Engine(cfg, store, SimKalshi(feed, window_s=120), feed, Notifier(desktop=False, sound=False))
    import asyncio
    async def run():
        await feed.start()
        for _ in range(8):
            await engine.tracker.poll_once()
            engine.step()
            await asyncio.sleep(0.05)
        await feed.stop()
    asyncio.run(run())
    st = engine.state
    assert "quick" in st and isinstance(st["quick"]["options"], list)
    m = engine.tracker.current
    side = "DOWN" if m.no_ask and m.no_ask <= 0.5 else "UP"
    pos = engine.open_position(side, 5.0, target=0.80)
    assert pos.target is not None and pos.target >= 0.80 - 1e-9 and pos.target_high > pos.target
    store.close()
