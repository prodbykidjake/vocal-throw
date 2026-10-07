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
