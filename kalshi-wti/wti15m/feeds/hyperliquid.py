"""Hyperliquid WTI perpetual (trade.xyz HIP-3 market on the "xyz" dex) as the live WTI price.

Public endpoints, no key:
  POST https://api.hyperliquid.xyz/info   {"type":"meta","dex":"xyz"}                 -> universe (names)
                                           {"type":"allMids","dex":"xyz"}              -> {"<name>": "90.19", ...}
                                           {"type":"candleSnapshot","req":{"coin":..,"interval":"1m","startTime":ms,"endTime":ms}}
  WS   wss://api.hyperliquid.xyz/ws       {"method":"subscribe","subscription":{"type":"allMids","dex":"xyz"}}

This is NOT Kalshi's settlement feed (that is a Pyth WTI series); it is a very closely tracking
stand-in. The engine measures the gap at every settlement and the model widens its uncertainty by it.
"""
from __future__ import annotations

import asyncio
import json
import logging
import time

import httpx

from .base import PriceFeed

log = logging.getLogger(__name__)

PREFERRED = ["xyz:CL", "CL", "xyz:WTI", "WTI", "xyz:WTIOIL", "WTIOIL", "xyz:USOIL", "USOIL", "xyz:OIL", "OIL"]


def pick_wti_symbol(names: list[str]) -> str | None:
    lower = {n.lower(): n for n in names}
    for pref in PREFERRED:
        if pref.lower() in lower:
            return lower[pref.lower()]
    for name in names:
        base = name.split(":")[-1].upper()
        if base.startswith("WTI") or base in ("CL", "USOIL", "CRUDE", "OIL", "CRUDEOIL"):
            return name
    return None


def lookup_mid(mids: dict, symbol: str) -> float | None:
    """Find our symbol in an allMids dict whether or not keys carry the dex prefix."""
    if not isinstance(mids, dict):
        return None
    candidates = [symbol, symbol.split(":")[-1]]
    if ":" not in symbol:
        candidates.append(f"xyz:{symbol}")
    for key in candidates:
        value = mids.get(key)
        if value not in (None, ""):
            try:
                return float(value)
            except (TypeError, ValueError):
                return None
    return None


class HyperliquidFeed(PriceFeed):
    name = "hyperliquid"

    def __init__(self, rest_url: str = "https://api.hyperliquid.xyz/info", ws_url: str = "wss://api.hyperliquid.xyz/ws",
                 dex: str = "xyz", symbol: str = "", warmup_minutes: int = 60, http: httpx.AsyncClient | None = None):
        super().__init__()
        self.rest_url = rest_url
        self.ws_url = ws_url
        self.dex = dex
        self.symbol = symbol or None
        self.warmup_minutes = warmup_minutes
        self._http = http or httpx.AsyncClient(timeout=10.0, headers={"User-Agent": "wti15m-coach/0.1"})
        self._task: asyncio.Task | None = None
        self._stop = asyncio.Event()
        self.universe: list[str] = []

    # ------------------------------------------------------------------ REST helpers
    async def info(self, payload: dict):
        resp = await self._http.post(self.rest_url, json=payload)
        if resp.status_code != 200:
            raise RuntimeError(f"info {payload.get('type')} -> HTTP {resp.status_code}: {resp.text[:160]}")
        return resp.json()

    async def resolve_symbol(self) -> str:
        if self.symbol:
            return self.symbol
        meta = await self.info({"type": "meta", "dex": self.dex} if self.dex else {"type": "meta"})
        universe = meta.get("universe", []) if isinstance(meta, dict) else []
        self.universe = [u.get("name", "") for u in universe if isinstance(u, dict)]
        symbol = pick_wti_symbol(self.universe)
        if not symbol:
            raise RuntimeError(f"no WTI market found on dex '{self.dex}'. Universe: {self.universe[:40]}")
        self.symbol = symbol
        log.info("hyperliquid: using symbol %s", symbol)
        return symbol

    async def fetch_candles(self, minutes: int) -> list[tuple[float, float]]:
        end_ms = int(time.time() * 1000)
        start_ms = end_ms - minutes * 60_000
        data = await self.info({"type": "candleSnapshot",
                                "req": {"coin": self.symbol, "interval": "1m", "startTime": start_ms, "endTime": end_ms}})
        closes: list[tuple[float, float]] = []
        for c in data if isinstance(data, list) else []:
            try:
                ts = float(c.get("T", c.get("t"))) / 1000.0
                closes.append((ts, float(c["c"])))
            except (TypeError, ValueError, KeyError, AttributeError):
                continue
        closes.sort()
        return closes

    async def fetch_mid(self) -> float | None:
        payload = {"type": "allMids", "dex": self.dex} if self.dex else {"type": "allMids"}
        mids = await self.info(payload)
        return lookup_mid(mids, self.symbol or "")

    # ------------------------------------------------------------------ lifecycle
    async def start(self):
        self._stop.clear()
        self._task = asyncio.create_task(self._run(), name="hyperliquid-feed")

    async def stop(self):
        self._stop.set()
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
        await self._http.aclose()

    async def _run(self):
        backoff = 2.0
        while not self._stop.is_set():
            try:
                await self.resolve_symbol()
                break
            except Exception as exc:
                self.last_error = f"symbol: {exc}"
                self.mode = "error"
                log.warning("hyperliquid symbol resolution failed: %s", exc)
                await asyncio.sleep(min(backoff, 30))
                backoff *= 1.5
        try:
            closes = await self.fetch_candles(self.warmup_minutes)
            if closes:
                self._publish_warmup(closes)
                log.info("hyperliquid: warmed up with %d 1m candles", len(closes))
        except Exception as exc:
            self.last_error = f"warmup: {exc}"
            log.warning("hyperliquid warm-up failed: %s", exc)

        backoff = 1.0
        while not self._stop.is_set():
            try:
                self.mode = "websocket"
                await self._ws_loop()
                backoff = 1.0
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                self.connected = False
                self.last_error = f"ws: {exc}"
                log.warning("hyperliquid websocket error: %s; polling REST for a while", exc)
                self.mode = "rest-poll"
                await self._rest_poll(seconds=min(60, 10 * backoff))
                backoff = min(backoff * 2, 6)

    async def _ws_loop(self):
        import websockets

        async with websockets.connect(self.ws_url, ping_interval=None, open_timeout=10, max_size=2**22) as ws:
            sub = {"type": "allMids"}
            if self.dex:
                sub["dex"] = self.dex
            await ws.send(json.dumps({"method": "subscribe", "subscription": sub}))
            self.connected = True
            self.last_error = None
            last_data = time.time()
            while not self._stop.is_set():
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=15)
                except asyncio.TimeoutError:
                    await ws.send(json.dumps({"method": "ping"}))
                    if time.time() - last_data > 60:
                        raise RuntimeError("no data for 60s")
                    continue
                try:
                    msg = json.loads(raw)
                except ValueError:
                    continue
                channel = msg.get("channel") if isinstance(msg, dict) else None
                if channel == "allMids":
                    mids = (msg.get("data") or {}).get("mids") or {}
                    price = lookup_mid(mids, self.symbol or "")
                    if price is not None:
                        last_data = time.time()
                        self._publish(last_data, price)
                elif channel == "error":
                    raise RuntimeError(f"server error: {msg.get('data')}")

    async def _rest_poll(self, seconds: float):
        deadline = time.time() + seconds
        while not self._stop.is_set() and time.time() < deadline:
            try:
                price = await self.fetch_mid()
                if price is not None:
                    self.connected = True
                    self._publish(time.time(), price)
            except Exception as exc:
                self.connected = False
                self.last_error = f"rest: {exc}"
            await asyncio.sleep(1.0)
