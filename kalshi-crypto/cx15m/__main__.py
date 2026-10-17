"""CLI: python -m cx15m {serve|demo|probe|probe-index}"""
from __future__ import annotations

import argparse
import asyncio
import logging
import sys
import threading
import webbrowser

from . import __version__, config


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="cx15m", description="Kalshi 15-minute crypto: a settlement caller per coin")
    p.add_argument("--config", help="path to config.toml (default: ./config.toml if present)")
    p.add_argument("--log-level", default="INFO")
    p.add_argument("--version", action="version", version=__version__)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve", help="run the board at http://127.0.0.1:8792")
    s.add_argument("--no-browser", action="store_true")
    s.add_argument("--phone", action="store_true", help="also open the board to your phone (private link)")
    d = sub.add_parser("demo", help="the board on simulated markets at real price scales (works offline)")
    d.add_argument("--window", type=int, default=90, help="simulated window length in seconds")
    d.add_argument("--warmup", type=int, default=3, help="simulated warm-up minutes required before calls")
    d.add_argument("--no-browser", action="store_true")
    d.add_argument("--phone", action="store_true", help="also open the demo to your phone (private link)")
    d.add_argument("--data", default="data-demo", help="folder for the demo's databases")
    sub.add_parser("probe", help="list Kalshi's 15-minute crypto series, each window's target and settlement, and the "
                                 "exchanges' prices next to them")
    pi = sub.add_parser("probe-index", help="with a Kalshi API key: print the exact index messages for a few seconds")
    pi.add_argument("--seconds", type=float, default=12.0)
    return p


def run_server(cfg, board, open_browser: bool, phone: bool = False):
    import uvicorn

    from .app import create_app

    phone_key = None
    if phone:
        from .phone import load_or_create_key
        phone_key = load_or_create_key(cfg.storage.data_dir)
    app = create_app(board, phone_key)
    stop = asyncio.Event()
    bind = "0.0.0.0" if phone else cfg.server.host

    async def main():
        board_task = asyncio.create_task(board.run(stop))
        server = uvicorn.Server(uvicorn.Config(app, host=bind, port=cfg.server.port, log_level="warning"))
        url = f"http://127.0.0.1:{cfg.server.port}" if phone else f"http://{cfg.server.host}:{cfg.server.port}"
        source = "simulated" if board.sim is not None else board.feed_source()
        print(f"crypto board v{__version__}: {url}   (Ctrl-C to stop) · prices: {source}")
        if phone:
            from .phone import phone_links
            links = phone_links(cfg.server.port, phone_key)
            print("phone: open this link on your phone (same Wi-Fi), then Add to Home Screen:")
            for link in links or [f"http://<your Mac's IP>:{cfg.server.port}/?key={phone_key}"]:
                print(f"   {link}")
            print("   keep the link private: it is the key. Delete data/phone_key.txt to lock phones out and get a new one.")
        if open_browser:
            threading.Timer(1.5, lambda: webbrowser.open(url)).start()
        try:
            await server.serve()
        finally:
            stop.set()
            await board_task

    asyncio.run(main())


async def probe(cfg) -> int:
    """What the board will follow: every 15-minute crypto series, each live window's target as Kalshi writes it (the
    decimals set the tie rule), the last settled window's settlement value next to its successor's target (they should
    be equal), whether Kalshi's keyless chart series carries the index, and the exchanges' prices right now."""
    import httpx

    from .feeds import exchanges as ex
    from .kalshi import KalshiClient, RateLimiter
    from .markets import KNOWN, discover

    client = KalshiClient(cfg.kalshi.base_url, cfg.kalshi.timeout_s, limiter=RateLimiter(5))
    http = httpx.AsyncClient(timeout=10.0, headers=ex.UA)
    try:
        try:
            rows = await client.list_series_raw(cfg.kalshi.category)
            fifteen = [r.get("ticker") for r in rows if str(r.get("ticker") or "").upper().endswith("15M")]
            print(f"[series] {cfg.kalshi.category}: {len(rows)} series, 15-minute ones: {fifteen}")
        except Exception as exc:
            print(f"[series] listing failed ({exc}); the board will use its known list: {[m.series for m in KNOWN]}")
        bitstamp = {}
        try:
            r = await http.get(ex.BITSTAMP_TICKERS)
            bitstamp = dict(ex.parse_bitstamp(r.json())) if r.status_code == 200 else {}
        except Exception as exc:
            print(f"[bitstamp] failed: {exc}")
        specs = await discover(client, cfg.kalshi.category)
        for spec in specs:
            try:
                series = await client.get_series_raw(spec.series)
                src = (series.get("series") or series).get("settlement_sources")
                raw = await client.get_markets_raw(spec.series, status="open")
                live = raw[0] if raw else None
                if live is None:
                    print(f"[{spec.key}] {spec.series}: no open window right now (settles on {src})")
                else:
                    print(f"[{spec.key}] {spec.series}: {live.get('ticker')} target={live.get('floor_strike')!r} "
                          f"sub_title={live.get('yes_sub_title')!r} yes {live.get('yes_bid_dollars')}/"
                          f"{live.get('yes_ask_dollars')} settles on {src}")
                    print(f"[{spec.key}]   rules: {str(live.get('rules_primary') or '')[:400]!r}")
                    ev = live.get("event_ticker")
                    if ev:
                        pts = (((await client.get_live_data_raw(ev)).get("live_data") or {}).get("details") or {}).get("timeseries") or []
                        print(f"[{spec.key}]   Kalshi chart series points: {len(pts)}; last: {pts[-1] if pts else None}")
                settled = await client.get_markets_raw(spec.series, status="settled")
                if settled:
                    s0 = settled[0]
                    print(f"[{spec.key}]   last settled: {s0.get('ticker')} target={s0.get('floor_strike')!r} "
                          f"settlement value={s0.get('expiration_value')!r} result={s0.get('result')!r}")
                mids = {}
                for name, url, parse in (
                        ("coinbase", f"https://api.exchange.coinbase.com/products/{ex.coinbase_symbol(spec.badge)}/ticker",
                         lambda d: ex.mid_of(d.get("bid"), d.get("ask"))),
                        ("kraken", f"https://api.kraken.com/0/public/Ticker?pair={ex.KRAKEN_REST.get(spec.badge, spec.badge)}USD",
                         lambda d: next((ex.mid_of(v["b"][0], v["a"][0]) for v in (d.get("result") or {}).values()), None))):
                    try:
                        r = await http.get(url)
                        mids[name] = parse(r.json()) if r.status_code == 200 else f"HTTP {r.status_code}"
                    except Exception as exc:
                        mids[name] = f"failed: {exc}"[:60]
                mids["bitstamp"] = bitstamp.get(spec.badge.upper(), "not listed")
                print(f"[{spec.key}]   exchanges now: {mids}")
            except Exception as exc:
                print(f"[{spec.key}] {spec.series}: failed: {exc}")
        print("\nDone. Paste this output back to Claude.")
        return 0
    finally:
        await client.aclose()
        await http.aclose()


async def probe_index(cfg, seconds: float) -> int:
    """Connect with the API key, subscribe to every coin's index and print what arrives (no secret is printed)."""
    import json
    import time

    from .feeds.exchanges import ws_connect
    from .feeds.kalshi_index import CHANNEL, has_key, key_settings, load_private_key, parse_index, sign_headers
    from .markets import KNOWN

    if not has_key(cfg.kalshi):
        print("No API key set: add [kalshi] api_key_id and private_key_path to config.toml (see README).")
        return 2
    key_id, path = key_settings(cfg.kalshi)
    try:
        key = load_private_key(path)
    except Exception as exc:
        print(f"Could not read the private key file {path}: {exc}")
        return 2
    ids = sorted({m.index for m in KNOWN if m.index})
    try:
        async with ws_connect(cfg.kalshi.ws_url, sign_headers(key_id, key)) as ws:
            await ws.send(json.dumps({"id": 1, "cmd": "subscribe", "params": {"channels": [CHANNEL], "index_ids": ids}}))
            print(f"connected; subscribed to {ids}")
            end, shown = time.time() + seconds, 0
            while time.time() < end:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, end - time.time()))
                except asyncio.TimeoutError:
                    break
                v = parse_index(raw)
                if shown < 25 or (v and v.get("settle_avg")):
                    print(("parsed " + json.dumps(v) + " <- " if v else "raw ") + str(raw)[:600])
                    shown += 1
    except Exception as exc:
        print(f"failed: {exc}")
        return 1
    print("\nDone. Paste this output back to Claude (it holds no secrets).")
    return 0


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(level=getattr(logging, args.log_level.upper(), logging.INFO),
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s", datefmt="%H:%M:%S")
    logging.getLogger("httpx").setLevel(logging.WARNING)
    cfg = config.load(args.config)
    from .board import Board

    if args.cmd == "probe":
        return asyncio.run(probe(cfg))
    if args.cmd == "probe-index":
        return asyncio.run(probe_index(cfg, args.seconds))
    if args.cmd == "serve":
        try:
            run_server(cfg, Board(cfg), cfg.server.open_browser and not args.no_browser, args.phone or cfg.server.phone)
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
            run_server(cfg, board, cfg.server.open_browser and not args.no_browser, args.phone)
        except KeyboardInterrupt:
            pass
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
