"""CLI: python -m wti15m {probe|watch|serve|demo|replay}"""
from __future__ import annotations

import argparse
import asyncio
import logging
import signal
import sys
import threading
import time
import webbrowser

from . import __version__, config
from .clock import fmt_countdown
from .decision import cents, pct


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="wti15m", description="Kalshi WTI 15-min coach")
    p.add_argument("--config", help="path to config.toml (default: ./config.toml if present)")
    p.add_argument("--log-level", default="INFO")
    p.add_argument("--version", action="version", version=__version__)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("probe", help="dump raw Kalshi + Hyperliquid API responses to confirm fields")
    q = sub.add_parser("quotes", help="compare Kalshi price sources (list vs market vs orderbook) once a second")
    q.add_argument("--seconds", type=int, default=20)
    ld = sub.add_parser("livedata", help="compare Kalshi's live price series with the Hyperliquid mid once a second")
    ld.add_argument("--seconds", type=int, default=20)
    w = sub.add_parser("watch", help="terminal-only live view (no browser)")
    w.add_argument("--sim", action="store_true", help="use the simulated market + feed")
    s = sub.add_parser("serve", help="run the dashboard at http://127.0.0.1:8787")
    s.add_argument("--no-browser", action="store_true")
    sub.add_parser("auth", help="check the Kalshi API key: exchange status, balance, open positions (places nothing)")
    d = sub.add_parser("demo", help="dashboard on a simulated market/feed (works offline)")
    d.add_argument("--window", type=int, default=120, help="simulated window length in seconds")
    d.add_argument("--warmup", type=int, default=10, help="simulated warm-up minutes required before signals")
    d.add_argument("--no-browser", action="store_true")
    d.add_argument("--db", default="data/demo.db")
    d.add_argument("--auto", action="store_true", help="also run the auto trader against a simulated account")
    r = sub.add_parser("replay", help="re-score recorded windows")
    r.add_argument("--since", help="ISO date, e.g. 2026-10-08")
    r.add_argument("--retrain", action="store_true", help="walk-forward retrain the calibrator from scratch")
    r.add_argument("--save", action="store_true", help="with --retrain: save the retrained calibrator")
    r.add_argument("--db", help="database to read (default: the configured one)")
    return p


LOOPBACK = ("127.0.0.1", "localhost", "::1")


def attach_auto(cfg, engine, sim: bool = False):
    """Give the engine an AutoTrader: a simulated account (demo, or dry_run on live quotes) or your Kalshi account."""
    from .autotrader import AutoTrader
    from .broker import KalshiAuth, KalshiBroker, SimBroker
    from .fees import taker_fee

    if not cfg.auto.enabled:
        return None
    if sim or cfg.auto.dry_run:
        def quotes():
            m = engine.tracker.current
            return None if m is None else {"ticker": m.ticker, "yes_bid": m.yes_bid, "yes_ask": m.yes_ask, "no_bid": m.no_bid, "no_ask": m.no_ask}
        broker = SimBroker(quotes, balance=cfg.trading.bankroll, fractional=cfg.auto.fractional,
                           fee_fn=lambda p, c: taker_fee(p, c, engine.fees))
        mode = "sim" if sim else "dry_run"
    else:
        if cfg.server.host not in LOOPBACK:
            raise SystemExit(f"refusing LIVE auto trading with the dashboard bound to {cfg.server.host}: "
                             f"anyone on the network could pause/stop it. Set [server] host = \"127.0.0.1\".")
        auth = KalshiAuth(cfg.auto.api_key_id, cfg.auto.private_key_path)
        broker = KalshiBroker(cfg.kalshi.base_url, auth, cfg.kalshi.timeout_s, cfg.auto.order_api, cfg.auto.fractional)
        mode = "live"
    engine.auto = AutoTrader(cfg, broker, engine.store, engine, mode)
    logging.getLogger(__name__).info("auto trading: %s (plans=%s quick=%s, max $%.0f/order, $%.0f/window, $%.0f/day, loss stop $%.0f)",
                                     mode.upper(), cfg.auto.take_plans, cfg.auto.take_quick, cfg.auto.max_order_dollars,
                                     cfg.auto.max_window_dollars, cfg.auto.max_day_dollars, cfg.auto.max_day_loss)
    return engine.auto


async def run_auth(cfg) -> int:
    """Prove the API key works without placing anything."""
    from .broker import BrokerError, KalshiAuth, KalshiBroker

    try:
        auth = KalshiAuth(cfg.auto.api_key_id, cfg.auto.private_key_path)
    except BrokerError as exc:
        print(f"not configured: {exc}")
        return 1
    broker = KalshiBroker(cfg.kalshi.base_url, auth, cfg.kalshi.timeout_s, cfg.auto.order_api, cfg.auto.fractional)
    print(f"key {auth.key_id[:8]}… ({auth.key_type}) against {cfg.kalshi.base_url}")
    try:
        st = await broker.exchange_status()
        print(f"exchange: {st}")
        bal, shards = await broker.balances()
        print(f"balance: ${bal:.2f}" + ("  by exchange: " + ", ".join(f"{k}: ${v:.2f}" for k, v in sorted(shards.items())) if shards else ""))
        try:
            from .kalshi import KalshiClient
            pub = KalshiClient(cfg.kalshi.base_url, cfg.kalshi.timeout_s)
            mk = await pub.get_markets(cfg.kalshi.series_ticker, status="open")
            await pub.aclose()
            idx = next((m.exchange_index for m in mk if m.exchange_index is not None), None)
            if idx is not None:
                here = shards.get(idx) if shards else None
                print(f"WTI 15-min trades on exchange {idx}" + (f": ${here:.2f} of your cash is there" if here is not None else ""))
                if here is not None and here < 1.0 and bal >= 1.0:
                    print("  -> your cash is on another exchange. The auto trader moves it over when it needs to, which needs "
                          "an API key with Trade AND Transfers ticked (money only moves between your own Kalshi balances).")
        except Exception as exc:  # informational only
            print(f"(could not look up the WTI market's exchange: {exc})")
        pos = await broker.positions()
        live = [p for p in pos if p.qty > 0]
        print(f"open positions: {len(live)}")
        for p in live[:10]:
            print(f"  {p.ticker}: {p.qty:.2f} {p.side} (${p.exposure:.2f} in)")
        print("API key OK. Set [auto] enabled = true and dry_run = true first; the log shows every order it WOULD send.")
        return 0
    except BrokerError as exc:
        print(f"Kalshi refused: {exc}")
        print("401/403 = key id or private key file wrong (or the clock is off); check [auto] api_key_id / private_key_path.")
        return 1
    finally:
        await broker.aclose()


def make_engine(cfg, sim: bool = False, window_s: int = 900, db_path: str | None = None):
    from .engine import Engine
    from .notify import Notifier
    from .store import Store

    store = Store(db_path or cfg.storage.db_path)
    if sim or cfg.feed.source == "sim":
        from .feeds.sim import SimFeed
        from .sim import SimKalshi
        feed = SimFeed(start_price=90.0, sigma_per_sqrt_s=0.005, warmup_minutes=max(5, int(cfg.trading.min_warmup_minutes) + 5))
        client = SimKalshi(feed, window_s=window_s, series_ticker=cfg.kalshi.series_ticker)
    else:
        from .feeds.hyperliquid import HyperliquidFeed
        from .kalshi import KalshiClient

        def hyperliquid():
            return HyperliquidFeed(cfg.feed.hyperliquid_rest, cfg.feed.hyperliquid_ws, cfg.feed.hyperliquid_dex,
                                   cfg.feed.hyperliquid_symbol, cfg.feed.warmup_candles_min)

        client = KalshiClient(cfg.kalshi.base_url, cfg.kalshi.timeout_s)
        if cfg.feed.source == "kalshi_live":
            from .feeds.composite import CompositeFeed
            from .feeds.kalshi_live import KalshiLiveFeed, event_ticker_for
            primary = KalshiLiveFeed(cfg.kalshi.base_url, poll_s=cfg.feed.kalshi_live_poll_s)
            fallback = hyperliquid() if cfg.feed.fallback == "hyperliquid" else None
            feed = CompositeFeed(primary, fallback, cfg.feed.stale_after_s)
            engine = Engine(cfg, store, client, feed, Notifier(cfg.notify.desktop, cfg.notify.sound))
            primary.event_ticker_fn = lambda: (event_ticker_for(engine.tracker.current.ticker, engine.tracker.current.event_ticker)
                                               if engine.tracker.current else None)
            return engine
        feed = hyperliquid()
    return Engine(cfg, store, client, feed, Notifier(cfg.notify.desktop, cfg.notify.sound))


async def run_watch(cfg, sim: bool):
    engine = make_engine(cfg, sim=sim)
    stop = asyncio.Event()
    task = asyncio.create_task(engine.run(stop))
    try:
        while True:
            await asyncio.sleep(1.0)
            st = engine.state
            m, pred, sig, feed = st.get("market"), st.get("prediction"), st.get("signal"), st.get("feed") or {}
            parts = [time.strftime("%H:%M:%S")]
            if m:
                parts.append(f"{m['ticker']} target {m['strike']} close {m['close_label']} left {st['countdown']}")
                parts.append(f"yes {m['yes_bid']}/{m['yes_ask']}")
            else:
                parts.append(f"no live market ({(st.get('tracker') or {}).get('last_error') or 'waiting'})")
            tick = st.get("tick")
            if tick:
                parts.append(f"WTI {tick['price']:.2f} ({feed.get('mode')}, age {feed.get('age_s') and round(feed['age_s'], 1)}s)")
            else:
                parts.append(f"feed: {feed.get('mode')} {feed.get('last_error') or ''}")
            if pred:
                parts.append(f"model Up {pct(pred['p_up'])} z {pred['z']:+.2f} [{pred['confidence']}]")
            if sig:
                parts.append(sig["headline"])
            print(" | ".join(parts), flush=True)
    finally:
        stop.set()
        await task


def run_server(cfg, engine, open_browser: bool):
    import uvicorn

    from .app import create_app

    app = create_app(engine)
    stop = asyncio.Event()

    async def main():
        engine_task = asyncio.create_task(engine.run(stop))
        server = uvicorn.Server(uvicorn.Config(app, host=cfg.server.host, port=cfg.server.port, log_level="warning"))
        url = f"http://{cfg.server.host}:{cfg.server.port}"
        print(f"dashboard: {url}   (Ctrl-C to stop)")
        if open_browser:
            threading.Timer(1.5, lambda: webbrowser.open(url)).start()
        try:
            await server.serve()
        finally:
            stop.set()
            await engine_task

    asyncio.run(main())


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(level=getattr(logging, args.log_level.upper(), logging.INFO),
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s", datefmt="%H:%M:%S")
    logging.getLogger("httpx").setLevel(logging.WARNING)
    cfg = config.load(args.config)
    if cfg.path:
        logging.getLogger(__name__).info("config: %s", cfg.path)
    if args.cmd == "probe":
        from . import probe
        return asyncio.run(probe.run(cfg))
    if args.cmd == "quotes":
        from . import probe
        return asyncio.run(probe.quotes_check(cfg, args.seconds))
    if args.cmd == "livedata":
        from . import probe
        return asyncio.run(probe.livedata_check(cfg, args.seconds))
    if args.cmd == "watch":
        try:
            asyncio.run(run_watch(cfg, args.sim))
        except KeyboardInterrupt:
            pass
        return 0
    if args.cmd == "auth":
        return asyncio.run(run_auth(cfg))
    if args.cmd == "serve":
        engine = make_engine(cfg)
        attach_auto(cfg, engine)
        try:
            run_server(cfg, engine, cfg.server.open_browser and not args.no_browser)
        except KeyboardInterrupt:
            pass
        return 0
    if args.cmd == "demo":
        cfg.trading.min_warmup_minutes = args.warmup
        # short simulated windows: scale the entry gates so the demo actually produces signals
        cfg.trading.no_entry_first_s = max(5, args.window // 9)
        cfg.trading.late_entry_s = max(5, args.window // 15)
        cfg.feed.source = "sim"
        cfg.notify.desktop = False
        cfg.notify.sound = False
        engine = make_engine(cfg, sim=True, window_s=args.window, db_path=args.db)
        if args.auto:
            cfg.auto.enabled = True
            attach_auto(cfg, engine, sim=True)
        try:
            run_server(cfg, engine, cfg.server.open_browser and not args.no_browser)
        except KeyboardInterrupt:
            pass
        return 0
    if args.cmd == "replay":
        from . import replay
        from .store import Store
        store = Store(args.db or cfg.storage.db_path)
        report = replay.run(store, args.since, args.retrain, args.save)
        print(replay.format_report(report))
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
