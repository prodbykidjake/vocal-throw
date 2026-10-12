"""CLI: python -m cm15m {serve|demo|probe}"""
from __future__ import annotations

import argparse
import asyncio
import logging
import sys
import threading
import webbrowser

from . import __version__, config


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="cm15m", description="Kalshi 15-minute commodities: a settlement caller per market")
    p.add_argument("--config", help="path to config.toml (default: ./config.toml if present)")
    p.add_argument("--log-level", default="INFO")
    p.add_argument("--version", action="version", version=__version__)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve", help="run the board at http://127.0.0.1:8790")
    s.add_argument("--no-browser", action="store_true")
    d = sub.add_parser("demo", help="the board on simulated markets at real price scales (works offline)")
    d.add_argument("--window", type=int, default=90, help="simulated window length in seconds")
    d.add_argument("--warmup", type=int, default=3, help="simulated warm-up minutes required before calls")
    d.add_argument("--no-browser", action="store_true")
    d.add_argument("--data", default="data-demo", help="folder for the demo's databases")
    sub.add_parser("probe", help="list Kalshi's 15-minute commodity series and each live window's target")
    return p


def run_server(cfg, board, open_browser: bool):
    import uvicorn

    from .app import create_app

    app = create_app(board)
    stop = asyncio.Event()

    async def main():
        board_task = asyncio.create_task(board.run(stop))
        server = uvicorn.Server(uvicorn.Config(app, host=cfg.server.host, port=cfg.server.port, log_level="warning"))
        url = f"http://{cfg.server.host}:{cfg.server.port}"
        print(f"commodities board v{__version__}: {url}   (Ctrl-C to stop)")
        if open_browser:
            threading.Timer(1.5, lambda: webbrowser.open(url)).start()
        try:
            await server.serve()
        finally:
            stop.set()
            await board_task

    asyncio.run(main())


async def probe(cfg) -> int:
    """What the board will follow: every 15-minute series in the category, and each live window's target as Kalshi
    writes it (the decimals set the tie rule)."""
    from .kalshi import KalshiClient, RateLimiter
    from .markets import KNOWN, discover

    client = KalshiClient(cfg.kalshi.base_url, cfg.kalshi.timeout_s, limiter=RateLimiter(5))
    try:
        try:
            rows = await client.list_series_raw(cfg.kalshi.category)
            fifteen = [r.get("ticker") for r in rows if str(r.get("ticker") or "").upper().endswith("15M")]
            print(f"[series] {cfg.kalshi.category}: {len(rows)} series, 15-minute ones: {fifteen}")
        except Exception as exc:
            print(f"[series] listing failed ({exc}); the board will use its known list: {[m.series for m in KNOWN]}")
        specs = await discover(client, cfg.kalshi.category)
        for spec in specs:
            try:
                raw = await client.get_markets_raw(spec.series, status="open")
                live = raw[0] if raw else None
                if live is None:
                    print(f"[{spec.key}] {spec.series}: no open window right now")
                    continue
                series = await client.get_series_raw(spec.series)
                src = (series.get("series") or series).get("settlement_sources")
                print(f"[{spec.key}] {spec.series}: {live.get('ticker')} target={live.get('floor_strike')!r} "
                      f"sub_title={live.get('yes_sub_title')!r} yes {live.get('yes_bid_dollars')}/{live.get('yes_ask_dollars')} "
                      f"settles on {src}")
                ev = live.get("event_ticker")
                if ev:
                    pts = (((await client.get_live_data_raw(ev)).get("live_data") or {}).get("details") or {}).get("timeseries") or []
                    print(f"[{spec.key}]   live price points: {len(pts)}; last: {pts[-1] if pts else None}")
            except Exception as exc:
                print(f"[{spec.key}] {spec.series}: failed: {exc}")
        print("\nDone. Paste this output back to Claude.")
        return 0
    finally:
        await client.aclose()


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(level=getattr(logging, args.log_level.upper(), logging.INFO),
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s", datefmt="%H:%M:%S")
    logging.getLogger("httpx").setLevel(logging.WARNING)
    cfg = config.load(args.config)
    from .board import Board

    if args.cmd == "probe":
        return asyncio.run(probe(cfg))
    if args.cmd == "serve":
        try:
            run_server(cfg, Board(cfg), cfg.server.open_browser and not args.no_browser)
        except KeyboardInterrupt:
            pass
        return 0
    if args.cmd == "demo":
        # short simulated windows: scale the call's clocks with them, like the coaches' demo
        t = cfg.trading
        t.min_warmup_minutes = args.warmup
        t.study_s = max(5, args.window // 6)
        t.call_deadline_s = max(8, args.window // 6)
        t.call_join_min_s = args.window * 0.6
        t.call_min_left_s = 5
        t.call_hold_s = 3
        cfg.storage.data_dir = args.data
        cfg.notify.desktop = cfg.notify.sound = False
        board = Board(cfg, sim={"window_s": args.window, "warmup_minutes": max(5, args.warmup + 5)})
        try:
            run_server(cfg, board, cfg.server.open_browser and not args.no_browser)
        except KeyboardInterrupt:
            pass
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
