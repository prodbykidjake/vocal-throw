import asyncio
import time

import pytest

from wti15m.autotrader import AutoTrader
from wti15m.broker import SimBroker
from wti15m.config import Config
from wti15m.decision import Quotes, Signal
from wti15m.engine import Engine
from wti15m.feeds.sim import SimFeed
from wti15m.notify import Notifier
from wti15m.plans import Plan
from wti15m.sim import SimKalshi
from wti15m.store import Store


async def _engine(tmp_path, seed=1):
    cfg = Config()
    cfg.trading.min_warmup_minutes = 1
    cfg.trading.no_entry_first_s = 0
    cfg.trading.late_entry_s = 0  # the simulated windows follow the wall clock; the test must not depend on the minute
    cfg.auto.enabled = True
    feed = SimFeed(start_price=90.0, sigma_per_sqrt_s=0.005, warmup_minutes=5, seed=seed)
    client = SimKalshi(feed, window_s=86400)  # a day-long window: the simulated quotes never sit at a window edge
    store = Store(str(tmp_path / "a.db"))
    engine = Engine(cfg, store, client, feed, Notifier(desktop=False, sound=False))

    def quotes():
        m = engine.tracker.current
        return None if m is None else {"ticker": m.ticker, "yes_bid": m.yes_bid, "yes_ask": m.yes_ask, "no_bid": m.no_bid, "no_ask": m.no_ask}
    broker = SimBroker(quotes, balance=200.0)
    trader = AutoTrader(cfg, broker, store, engine, "sim")
    engine.auto = trader
    await feed.start()
    await asyncio.sleep(0.7)
    await engine.tracker.poll_once()
    engine.step()
    return cfg, engine, trader, broker, feed, store


def _plan(engine, side="DOWN"):
    m = engine.tracker.current
    q = Quotes.from_market(m)
    ask = q.ask(side)
    plan = Plan(m.ticker, side, round(ask + 0.005, 3), 10.0, 10.0 / (ask + 0.005), min(0.95, ask + 0.15), 0.5, "scalp", time.time(),
                m.strike, target_high=min(0.95, ask + 0.25))
    plan.id = 1
    engine.plans.plan = plan
    return plan


@pytest.mark.asyncio
async def test_plan_is_bought_then_sold_on_sell_now(tmp_path):
    cfg, engine, trader, broker, feed, store = await _engine(tmp_path)
    try:
        plan = _plan(engine)
        now = time.time()
        await trader.tick(now)
        pos = engine.position
        assert pos is not None and pos.side == "DOWN", (trader.blocked, trader.last_text, trader.sync_error, store.orders(3))  # opened by the fill
        rows = store.orders(5)
        assert rows[0]["action"] == "buy" and rows[0]["status"] == "executed" and rows[0]["filled"] > 0 and rows[0]["mode"] == "sim"
        assert abs(pos.qty * pos.avg_price - 10.0) < 0.3  # ten dollars of Down at the ask
        assert pos.avg_price <= plan.limit + 1e-9 and pos.target is not None
        assert engine.plans.plan is None  # the plan counts as filled
        assert trader.order is None and (await broker.positions(pos.ticker))[0].qty == pytest.approx(pos.qty, abs=0.01)
        assert "FILLED" in trader.last_text
        # the card says SELL NOW -> one IOC reduce-only sell at bid - 3c -> position closed
        await engine.tracker.poll_once()  # fresh quotes (the engine refuses to sell into quotes older than 10 s)
        engine.step()
        bid = Quotes.from_market(engine.tracker.current).bid("DOWN")
        engine.signal = Signal("SELL", "DOWN", bid, 0, None, 0.5, "SELL NOW DOWN", [], ["zone"], "lean")
        await trader.tick(now + 1)
        assert engine.position is None
        rows = store.orders(5)
        assert rows[0]["action"] == "sell" and rows[0]["status"] == "executed" and rows[0]["price"] == pytest.approx(bid - 0.03, abs=1e-6)
        closed = store.positions(1)[0]
        assert closed["closed_ts"] is not None and closed["exit_price"] == pytest.approx(bid, abs=1e-6)
        assert (await broker.positions(closed["ticker"]))[0].qty == 0
        assert trader.day_pnl == 0.0  # recomputed at the next sync
        trader.synced_ts = 0
        await trader.tick(now + 2)
        assert trader.day_pnl == pytest.approx(closed["pnl"], abs=0.01)
    finally:
        await feed.stop()
        store.close()


@pytest.mark.asyncio
async def test_limits_and_pause_block_buys(tmp_path):
    cfg, engine, trader, broker, feed, store = await _engine(tmp_path, seed=2)
    try:
        now = time.time()
        # a bad day already: realized loss past the limit
        pid = store.open_position("OLD", "UP", 10, 0.5, now - 3000, "", 5.0, 0.1, None)
        store.close_position(pid, now - 100, 0.0, -60.0)
        _plan(engine)
        await trader.tick(now)
        assert engine.position is None and store.orders(1) == [] and "daily loss" in trader.blocked
        # the limit lifts: a buy that is larger than the per-order cap is capped, the plan is bought
        store.close_position(pid, now - 100, 0.0, +1.0)
        cfg.auto.max_order_dollars = 4.0
        trader.synced_ts = 0
        trader.attempted.clear()
        await engine.tracker.poll_once()
        engine.step()  # fresh quotes; the step may also end the injected plan, so inject it again
        _plan(engine).id = 3
        await trader.tick(now + 1)
        assert engine.position is not None and store.orders(1)[0]["amount"] == 4.0
        engine.cancel_position()
        broker.pos.clear()
        # paused: no buys even with a fresh plan
        trader.pause("test")
        trader.synced_ts = 0
        p = _plan(engine)
        p.id = 2
        await trader.tick(now + 2)
        assert engine.position is None and trader.blocked.startswith("paused")
        trader.resume()
        assert not trader.paused
        st = trader.as_dict(now + 2)
        assert st["mode"] == "sim" and st["limits"]["order"] == 4.0 and isinstance(st["orders"], list)
    finally:
        await feed.stop()
        store.close()


@pytest.mark.asyncio
async def test_sync_adopts_and_releases_positions(tmp_path):
    cfg, engine, trader, broker, feed, store = await _engine(tmp_path, seed=3)
    try:
        now = time.time()
        m = engine.tracker.current
        # bought in the Kalshi app: the account shows a position the coach does not know about
        broker.pos[m.ticker] = {"side": "UP", "qty": 20.0, "exposure": 6.0}
        await trader.tick(now)
        pos = engine.position
        assert pos is not None and pos.side == "UP" and pos.qty == pytest.approx(20.0) and pos.avg_price == pytest.approx(0.30)
        assert "adopted" in trader.last_text
        # sold in the Kalshi app: the account is flat again while the coach still holds it
        broker.pos.clear()
        pos.opened_ts = now - 100
        trader.synced_ts = 0
        await trader.tick(now + 1)
        assert engine.position is None and "sold by you" in trader.last_text
        assert store.positions(1)[0]["closed_ts"] is not None
    finally:
        await feed.stop()
        store.close()


@pytest.mark.asyncio
async def test_unfilled_buy_is_cancelled_after_ttl_and_partial_fills_add_up(tmp_path):
    cfg, engine, trader, broker, feed, store = await _engine(tmp_path, seed=4)
    try:
        now = time.time()
        m = engine.tracker.current
        q = Quotes.from_market(m)
        plan = _plan(engine, "UP")
        plan.limit = round(q.ask("UP") - 0.05, 3)  # under the ask: rests, never fills
        await trader.tick(now)
        assert trader.order is None and engine.position is None  # ask above limit + 0.5c: not even attempted
        trader.attempted.clear()
        await engine.tracker.poll_once()
        engine.step()  # fresh quotes; the step may also end the injected plan, so inject it again
        plan = _plan(engine, "UP")
        plan.id = 2
        q = Quotes.from_market(engine.tracker.current)
        plan.limit = round(q.ask("UP") + 0.005, 3)
        # make the simulated book refuse the fill by lifting the ask after placement
        orig = broker.quotes_fn
        broker.quotes_fn = lambda: dict(orig(), yes_ask=0.99, no_bid=0.01)
        await trader.tick(now + 1)
        assert trader.order is not None and trader.order["filled"] == 0
        await trader.tick(now + 1 + cfg.auto.buy_ttl_s + 1)  # past the ttl: cancelled
        assert trader.order is None and engine.position is None and store.orders(1)[0]["status"] == "canceled"
        assert "not filled" in trader.last_text
        broker.quotes_fn = orig
        # partial fills: reduce_position keeps the remainder and banks the sold part
        engine.open_position("UP", 10.0, 0.40, "test")
        pos = engine.position
        out = engine.reduce_position(pos.qty / 2, 0.50, 0.05)
        assert not out["closed"] and abs(out["qty_left"] - 12.5) < 0.01 and engine.position.realized > 0
        assert abs(engine.position.amount - 5.0) < 1e-6 and store.get_open_position()["realized"] == pytest.approx(engine.position.realized)
        engine.add_to_position(12.5, 0.60, 0.1)
        assert engine.position.qty == 25.0 and engine.position.avg_price == pytest.approx((5.0 + 7.5) / 25.0)
        out = engine.reduce_position(100, 0.55, 0.1)
        assert out["closed"] and engine.position is None and store.positions(1)[0]["pnl"] == pytest.approx(out["pnl"])
    finally:
        await feed.stop()
        store.close()
