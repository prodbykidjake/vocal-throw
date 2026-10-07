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
    w = sub.add_parser("watch", help="terminal-only live view (no browser)")
    w.add_argument("--sim", action="store_true", help="use the simulated market + feed")
    s = sub.add_parser("serve", help="run the dashboard at http://127.0.0.1:8787")
    s.add_argument("--no-browser", action="store_true")
    d = sub.add_parser("demo", help="dashboard on a simulated market/feed (works offline)")
    d.add_argument("--window", type=int, default=120, help="simulated window length in seconds")
    d.add_argument("--warmup", type=int, default=10, help="simulated warm-up minutes required before signals")
    d.add_argument("--no-browser", action="store_true")
    d.add_argument("--db", default="data/demo.db")
    r = sub.add_parser("replay", help="re-score recorded windows")
    r.add_argument("--since", help="ISO date, e.g. 2026-10-08")
    r.add_argument("--retrain", action="store_true", help="walk-forward retrain the calibrator from scratch")
    r.add_argument("--save", action="store_true", help="with --retrain: save the retrained calibrator")
    r.add_argument("--db", help="database to read (default: the configured one)")
    return p


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
        feed = HyperliquidFeed(cfg.feed.hyperliquid_rest, cfg.feed.hyperliquid_ws, cfg.feed.hyperliquid_dex,
                               cfg.feed.hyperliquid_symbol, cfg.feed.warmup_candles_min)
        client = KalshiClient(cfg.kalshi.base_url, cfg.kalshi.timeout_s)
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
    if args.cmd == "watch":
        try:
            asyncio.run(run_watch(cfg, args.sim))
        except KeyboardInterrupt:
            pass
        return 0
    if args.cmd == "serve":
        engine = make_engine(cfg)
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
