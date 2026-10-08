"""`probe`: dump raw JSON from Kalshi and Hyperliquid so field names/units can be confirmed."""
from __future__ import annotations

import asyncio
import datetime as dt
import json
import pathlib

import httpx

from .config import Config
from .kalshi import KalshiClient, Market, Series, select_live_market

HL_REST = "https://api.hyperliquid.xyz/info"


def _dump(folder: pathlib.Path, name: str, data) -> None:
    (folder / f"{name}.json").write_text(json.dumps(data, indent=2, default=str))


async def run(cfg: Config, out_dir: str | None = None) -> int:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    folder = pathlib.Path(out_dir or pathlib.Path(__file__).resolve().parents[1] / "fixtures" / f"probe-{stamp}")
    folder.mkdir(parents=True, exist_ok=True)
    print(f"writing raw API dumps to {folder}\n")
    ok = True

    kc = KalshiClient(cfg.kalshi.base_url, cfg.kalshi.timeout_s)
    try:
        series_raw = await kc.get_series_raw(cfg.kalshi.series_ticker)
        _dump(folder, "kalshi_series", series_raw)
        s = Series.from_api(series_raw)
        print(f"[kalshi] series {s.ticker}: fee_type={s.fee_type} multiplier={s.fee_multiplier}")
        print(f"[kalshi] settlement_sources: {s.settlement_sources}")
    except Exception as exc:
        ok = False
        print(f"[kalshi] series FAILED: {exc}")
    try:
        markets_raw = await kc.get_markets_raw(cfg.kalshi.series_ticker, status="open")
        _dump(folder, "kalshi_markets_open", {"markets": markets_raw})
        markets = [Market.from_api(d) for d in markets_raw]
        print(f"[kalshi] {len(markets)} open market(s): {[m.ticker for m in markets]}")
        live = select_live_market(markets)
        if live:
            print(f"[kalshi] live: {live.ticker} strike={live.strike} ({live.strike_source}) "
                  f"open={live.open_time} close={live.close_time} yes {live.yes_bid}/{live.yes_ask} no {live.no_bid}/{live.no_ask}")
            print(f"[kalshi] fields present: {sorted(live.raw.keys())}")
            print(f"[kalshi] rules_primary: {live.rules_primary[:400]}")
            single = await kc.get_market_raw(live.ticker)
            _dump(folder, "kalshi_market_single", single)
            try:
                from .feeds.kalshi_live import event_ticker_for, parse_series
                ev = event_ticker_for(live.ticker, live.event_ticker)
                ld_raw = await kc.get_live_data_raw(ev, "15min")
                _dump(folder, "kalshi_live_data", ld_raw)
                pts = parse_series(ld_raw)
                print(f"[kalshi] live_data for {ev}: keys={list(ld_raw.keys()) if isinstance(ld_raw, dict) else type(ld_raw)} "
                      f"points={len(pts)} last={[(dt.datetime.fromtimestamp(t).strftime('%H:%M:%S'), v) for t, v in pts[-3:]]}")
            except Exception as exc:
                print(f"[kalshi] live_data FAILED: {exc}")
            try:
                book_raw = await kc.get_orderbook_raw(live.ticker)
                _dump(folder, "kalshi_orderbook", book_raw)
                from .kalshi import parse_orderbook
                print(f"[kalshi] orderbook raw keys: {list((book_raw.get('orderbook') or book_raw).keys()) if isinstance(book_raw, dict) else type(book_raw)}")
                print(f"[kalshi] orderbook top: {parse_orderbook(book_raw)}")
            except Exception as exc:
                print(f"[kalshi] orderbook FAILED: {exc}")
        else:
            print("[kalshi] no live market right now (between windows or outside trading hours)")
        settled_raw = await kc.get_markets_raw(cfg.kalshi.series_ticker, status="settled", limit=5, max_pages=1)
        _dump(folder, "kalshi_markets_settled", {"markets": settled_raw})
        if settled_raw:
            m = Market.from_api(settled_raw[0])
            print(f"[kalshi] last settled: {m.ticker} result={m.result} status={m.status} strike={m.strike}")
    except Exception as exc:
        ok = False
        print(f"[kalshi] markets FAILED: {exc}")
    await kc.aclose()

    async with httpx.AsyncClient(timeout=15) as http:
        try:
            meta = (await http.post(cfg.feed.hyperliquid_rest, json={"type": "meta", "dex": cfg.feed.hyperliquid_dex})).json()
            _dump(folder, "hyperliquid_meta", meta)
            names = [u.get("name") for u in meta.get("universe", [])]
            from .feeds.hyperliquid import pick_wti_symbol
            sym = cfg.feed.hyperliquid_symbol or pick_wti_symbol(names)
            print(f"[hyperliquid] dex={cfg.feed.hyperliquid_dex} universe ({len(names)}): {names[:60]}")
            print(f"[hyperliquid] WTI symbol guess: {sym}")
            mids = (await http.post(cfg.feed.hyperliquid_rest, json={"type": "allMids", "dex": cfg.feed.hyperliquid_dex})).json()
            _dump(folder, "hyperliquid_allmids", mids)
            keys = list(mids.keys())[:20] if isinstance(mids, dict) else type(mids).__name__
            print(f"[hyperliquid] allMids keys sample: {keys}")
            if sym:
                from .feeds.hyperliquid import lookup_mid
                print(f"[hyperliquid] mid for {sym}: {lookup_mid(mids, sym)}")
                end = int(dt.datetime.now().timestamp() * 1000)
                candles = (await http.post(cfg.feed.hyperliquid_rest, json={"type": "candleSnapshot", "req": {
                    "coin": sym, "interval": "1m", "startTime": end - 10 * 60_000, "endTime": end}})).json()
                _dump(folder, "hyperliquid_candles", candles)
                print(f"[hyperliquid] candles: {len(candles) if isinstance(candles, list) else candles} "
                      f"sample={candles[-1] if isinstance(candles, list) and candles else None}")
        except Exception as exc:
            ok = False
            print(f"[hyperliquid] FAILED: {exc}")
    print("\nDone. Paste this output (and/or the JSON files) back to Claude.")
    return 0 if ok else 1


async def quotes_check(cfg: Config, seconds: int = 20) -> int:
    """`quotes`: once a second, print the live window's prices from the three Kalshi sources side by side,
    so a lagging source is obvious: market LIST, single MARKET, and ORDERBOOK."""
    import asyncio

    from .kalshi import parse_orderbook

    kc = KalshiClient(cfg.kalshi.base_url, cfg.kalshi.timeout_s)
    try:
        markets = await kc.get_markets(cfg.kalshi.series_ticker, status="open")
        live = select_live_market(markets)
        if live is None:
            print("no live market right now")
            return 1
        print(f"{live.ticker}  target={live.strike}  (yes = Up, no = Down; bid/ask in dollars)\n")
        print(f"{'time':>8}  {'LIST yes':>13}  {'MARKET yes':>13}  {'BOOK yes':>13}  {'BOOK no':>13}")
        for _ in range(seconds):
            t0 = dt.datetime.now().strftime("%H:%M:%S")
            try:
                lst = [m for m in await kc.get_markets(cfg.kalshi.series_ticker, status="open") if m.ticker == live.ticker]
                l = lst[0] if lst else None
                l_txt = f"{l.yes_bid}/{l.yes_ask}" if l else "--"
            except Exception as exc:
                l_txt = f"err"
            try:
                s = await kc.get_market(live.ticker)
                s_txt = f"{s.yes_bid}/{s.yes_ask}"
            except Exception:
                s_txt = "err"
            try:
                book = parse_orderbook(await kc.get_orderbook_raw(live.ticker))
                b_yes = f"{book['yes_bid']}/{book['yes_ask']}" if book else "--"
                b_no = f"{book['no_bid']}/{book['no_ask']}" if book else "--"
            except Exception as exc:
                b_yes, b_no = f"err {str(exc)[:40]}", ""
            print(f"{t0:>8}  {l_txt:>13}  {s_txt:>13}  {b_yes:>13}  {b_no:>13}", flush=True)
            await asyncio.sleep(1.0)
        print("\nCompare with the Kalshi web page: the column that matches it is the live one.")
        return 0
    finally:
        await kc.aclose()


async def livedata_check(cfg: Config, seconds: int = 20) -> int:
    """`livedata`: once a second, print Kalshi's own live price (the settlement feed) next to the Hyperliquid mid."""
    import asyncio

    from .feeds.hyperliquid import lookup_mid, pick_wti_symbol
    from .feeds.kalshi_live import event_ticker_for, parse_series

    kc = KalshiClient(cfg.kalshi.base_url, cfg.kalshi.timeout_s)
    async with httpx.AsyncClient(timeout=10) as http:
        try:
            markets = await kc.get_markets(cfg.kalshi.series_ticker, status="open")
            live = select_live_market(markets)
            if live is None:
                print("no live market right now")
                return 1
            ev = event_ticker_for(live.ticker, live.event_ticker)
            sym = cfg.feed.hyperliquid_symbol
            if not sym:
                meta = (await http.post(cfg.feed.hyperliquid_rest, json={"type": "meta", "dex": cfg.feed.hyperliquid_dex})).json()
                sym = pick_wti_symbol([u.get("name") for u in meta.get("universe", [])]) or "xyz:CL"
            print(f"{live.ticker}  target={live.strike}  event={ev}\n")
            print(f"{'time':>8}  {'KALSHI live (t)':>22}  {'HYPERLIQUID mid':>16}  {'gap':>7}")
            for _ in range(seconds):
                t0 = dt.datetime.now().strftime("%H:%M:%S")
                try:
                    pts = parse_series(await kc.get_live_data_raw(ev, "15min"))
                    k_txt = f"{pts[-1][1]:.3f} ({dt.datetime.fromtimestamp(pts[-1][0]).strftime('%H:%M:%S')})" if pts else "--"
                    k_val = pts[-1][1] if pts else None
                except Exception as exc:
                    k_txt, k_val = f"err {str(exc)[:30]}", None
                try:
                    mids = (await http.post(cfg.feed.hyperliquid_rest, json={"type": "allMids", "dex": cfg.feed.hyperliquid_dex})).json()
                    h_val = lookup_mid(mids, sym)
                    h_txt = f"{h_val:.3f}" if h_val is not None else "--"
                except Exception as exc:
                    h_txt, h_val = f"err {str(exc)[:30]}", None
                gap = f"{(h_val - k_val) * 100:+.1f}¢" if (k_val is not None and h_val is not None) else ""
                print(f"{t0:>8}  {k_txt:>22}  {h_txt:>16}  {gap:>7}", flush=True)
                await asyncio.sleep(1.0)
            print("\nThe KALSHI column should match the price on the Kalshi website; the gap is the fallback feed's error.")
            return 0
        finally:
            await kc.aclose()
