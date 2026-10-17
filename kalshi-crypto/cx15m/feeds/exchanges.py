"""The public price for each coin: the median of the best bid/ask midpoints on the exchanges CF Benchmarks builds its
indices from (Coinbase, Kraken, Bitstamp), no account or key needed.

Kalshi settles on a CF Benchmarks real-time index (BRTI for Bitcoin, ETHUSD_RTI, ...), which CF computes once a
second from those exchanges' order books. The index itself is only streamed with a Kalshi API key (kalshi_index.py);
without one the median midpoint tracks it to within a few hundredths of a percent, and the board measures what is
left (the feed's closing-minute average against each settlement value) and corrects for a steady offset.

One ExchangeHub serves every coin on the board with one connection per exchange:
  Coinbase  wss://ws-feed.exchange.coinbase.com, "ticker" channel (best_bid / best_ask on every trade)
  Kraken    wss://ws.kraken.com/v2, "ticker" channel with event_trigger "bbo" (on every best bid/ask change)
  Bitstamp  https://www.bitstamp.net/api/v2/ticker/ every 2 seconds (all pairs in one request)
A coin an exchange does not list just leaves that exchange out of its median. ExchangeFeed then prints the median
once a second for one coin, like the index does, after seeding its volatility from Coinbase's (or Kraken's) 1-minute
candles so the board can call from its first window.
"""
from __future__ import annotations

import asyncio
import json
import logging
import statistics
import time

import httpx

from .base import PriceFeed

log = logging.getLogger(__name__)

UA = {"User-Agent": "cx15m-board/1.0", "Accept": "application/json"}
COINBASE_WS = "wss://ws-feed.exchange.coinbase.com"
KRAKEN_WS = "wss://ws.kraken.com/v2"
BITSTAMP_TICKERS = "https://www.bitstamp.net/api/v2/ticker/"
COINBASE_CANDLES = "https://api.exchange.coinbase.com/products/{product}/candles"
KRAKEN_OHLC = "https://api.kraken.com/0/public/OHLC"
OUTLIER = 0.005  # with 3+ exchanges, a midpoint more than 0.5% from the others' median is left out (a broken book)
NAMES = {"coinbase": "Coinbase", "kraken": "Kraken", "bitstamp": "Bitstamp"}
KRAKEN_REST = {"BTC": "XBT", "DOGE": "XDG"}  # Kraken's REST names (its WebSocket v2 uses BTC/USD, DOGE/USD)


def ws_connect(url: str, headers: dict | None = None):
    """websockets' connect (the asyncio client of websockets 13+, or the legacy one)."""
    try:
        from websockets.asyncio.client import connect
        return connect(url, additional_headers=headers, ping_interval=20, ping_timeout=20, max_size=2 ** 22,
                       open_timeout=15)
    except ImportError:  # websockets 12
        import websockets
        return websockets.connect(url, extra_headers=headers, ping_interval=20, ping_timeout=20, max_size=2 ** 22,
                                  open_timeout=15)


def _num(x) -> float | None:
    try:
        v = float(x)
    except (TypeError, ValueError):
        return None
    return v if v > 0 else None


def mid_of(bid, ask) -> float | None:
    b, a = _num(bid), _num(ask)
    if b is None or a is None or a < b:
        return None
    return (a + b) / 2.0


# ----------------------------------------------------------------------------- per-exchange symbols and parsers
def coinbase_symbol(asset: str) -> str:
    return f"{asset.upper()}-USD"


def kraken_symbol(asset: str) -> str:
    return f"{asset.upper()}/USD"


def bitstamp_symbol(asset: str) -> str:
    return f"{asset.upper()}/USD"


def parse_coinbase(msg: dict) -> list[tuple[str, float]]:
    """{"type": "ticker", "product_id": "BTC-USD", "best_bid": "...", "best_ask": "..."} -> [("BTC", mid)]"""
    if msg.get("type") != "ticker":
        return []
    pid = str(msg.get("product_id") or "")
    m = mid_of(msg.get("best_bid"), msg.get("best_ask"))
    return [(pid.split("-")[0].upper(), m)] if m and pid.endswith("-USD") else []


def parse_kraken(msg: dict) -> list[tuple[str, float]]:
    """{"channel": "ticker", "type": "snapshot"|"update", "data": [{"symbol": "BTC/USD", "bid": .., "ask": ..}]}"""
    if msg.get("channel") != "ticker" or not isinstance(msg.get("data"), list):
        return []
    out = []
    for d in msg["data"]:
        if not isinstance(d, dict):
            continue
        sym = str(d.get("symbol") or "")
        m = mid_of(d.get("bid"), d.get("ask"))
        if m and sym.endswith("/USD"):
            out.append((sym.split("/")[0].upper(), m))
    return out


def parse_bitstamp(rows) -> list[tuple[str, float]]:
    """GET /api/v2/ticker/ -> [{"pair": "BTC/USD", "bid": "...", "ask": "..."}, ...] (one pair: a dict)."""
    if isinstance(rows, dict):
        rows = [rows]
    out = []
    for r in rows if isinstance(rows, list) else []:
        if not isinstance(r, dict):
            continue
        pair = str(r.get("pair") or "")
        m = mid_of(r.get("bid"), r.get("ask"))
        if m and pair.endswith("/USD"):
            out.append((pair.split("/")[0].upper(), m))
    return out


def median_mid(quotes: dict[str, tuple[float, float]], now: float, max_age_s: float) -> tuple[float | None, list[str]]:
    """The median of the fresh midpoints {exchange: (ts, mid)}, leaving out one that is far from the others when
    there are 3 or more. Returns (price, exchanges used)."""
    fresh = {ex: m for ex, (ts, m) in quotes.items() if now - ts <= max_age_s and m > 0}
    if not fresh:
        return None, []
    if len(fresh) >= 3:
        med = statistics.median(fresh.values())
        fresh = {ex: m for ex, m in fresh.items() if abs(m / med - 1) <= OUTLIER} or fresh
    return statistics.median(fresh.values()), sorted(fresh)


# ----------------------------------------------------------------------------- the hub
class ExchangeHub:
    """Best bid/ask midpoints for every coin on the board, one connection per exchange."""

    def __init__(self, exchanges: list[str] | None = None, max_age_s: float = 20.0, http: httpx.AsyncClient | None = None):
        self.exchanges = [e for e in (exchanges or list(NAMES)) if e in NAMES]
        self.max_age_s = max_age_s
        self.assets: set[str] = set()
        self.quotes: dict[str, dict[str, tuple[float, float]]] = {}  # asset -> exchange -> (ts, mid)
        self.errors: dict[str, str] = {}
        self.connected: dict[str, bool] = {e: False for e in self.exchanges}
        self._http = http
        self._tasks: list[asyncio.Task] = []
        self._users = 0
        self._started = False

    def add(self, asset: str):
        self.assets.add(asset.upper())

    def on_quote(self, exchange: str, asset: str, mid: float, ts: float | None = None):
        if asset in self.assets and mid and mid > 0:
            self.quotes.setdefault(asset, {})[exchange] = (time.time() if ts is None else ts, mid)

    def mid(self, asset: str, now: float | None = None) -> tuple[float | None, list[str]]:
        return median_mid(self.quotes.get(asset.upper(), {}), time.time() if now is None else now, self.max_age_s)

    # ------------------------------------------------------------------ lifecycle (shared by the coins' feeds)
    async def start(self):
        self._users += 1
        if self._started:
            return
        self._started = True
        await asyncio.sleep(0)  # let every coin's feed add its asset first
        if self._http is None:
            self._http = httpx.AsyncClient(timeout=10.0, headers=UA)
        runners = {"coinbase": self._run_coinbase, "kraken": self._run_kraken, "bitstamp": self._run_bitstamp}
        self._tasks = [asyncio.create_task(self._forever(name, runners[name]), name=f"exchange-{name}")
                       for name in self.exchanges]

    async def stop(self):
        self._users = max(0, self._users - 1)
        if self._users:
            return
        for t in self._tasks:
            t.cancel()
        for t in self._tasks:
            try:
                await t
            except (asyncio.CancelledError, Exception):
                pass
        self._tasks = []
        self._started = False
        if self._http is not None:
            await self._http.aclose()
            self._http = None

    async def _forever(self, name: str, runner):
        failures = 0
        while True:
            try:
                await runner()
                failures = 0
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                failures += 1
                self.errors[name] = str(exc)[:160]
                if failures in (1, 10) or failures % 100 == 0:
                    log.warning("%s price feed failed (%d): %s", NAMES[name], failures, exc)
            self.connected[name] = False
            await asyncio.sleep(min(60.0, 2.0 * 2 ** min(failures, 5)))

    async def _run_coinbase(self):
        async with ws_connect(COINBASE_WS) as ws:
            for asset in sorted(self.assets):  # one product per request: an unlisted coin cannot fail the others
                await ws.send(json.dumps({"type": "subscribe", "product_ids": [coinbase_symbol(asset)],
                                          "channels": ["ticker"]}))
            self.connected["coinbase"] = True
            async for raw in ws:
                msg = json.loads(raw)
                if msg.get("type") == "error":
                    log.info("Coinbase: %s", msg.get("reason") or msg.get("message"))
                    continue
                for asset, m in parse_coinbase(msg):
                    self.on_quote("coinbase", asset, m)

    async def _run_kraken(self):
        async with ws_connect(KRAKEN_WS) as ws:
            for i, asset in enumerate(sorted(self.assets)):
                await ws.send(json.dumps({"method": "subscribe", "req_id": i + 1,
                                          "params": {"channel": "ticker", "symbol": [kraken_symbol(asset)],
                                                     "event_trigger": "bbo"}}))
            self.connected["kraken"] = True
            async for raw in ws:
                msg = json.loads(raw)
                if msg.get("method") == "subscribe" and msg.get("success") is False:
                    log.info("Kraken: %s", msg.get("error"))
                    continue
                for asset, m in parse_kraken(msg):
                    self.on_quote("kraken", asset, m)

    async def _run_bitstamp(self):
        per_pair = False
        while True:
            if not per_pair:
                resp = await self._http.get(BITSTAMP_TICKERS)
                if resp.status_code == 404:
                    per_pair = True  # an older API without the all-pairs list: ask pair by pair, more slowly
                    continue
                resp.raise_for_status()
                rows = parse_bitstamp(resp.json())
            else:
                rows = []
                for asset in sorted(self.assets):
                    r = await self._http.get(f"{BITSTAMP_TICKERS}{asset.lower()}usd/")
                    if r.status_code == 200:
                        rows += [(asset, m) for _, m in parse_bitstamp({**r.json(), "pair": f"{asset}/USD"})]
            now = time.time()
            for asset, m in rows:
                self.on_quote("bitstamp", asset, m, now)
            self.connected["bitstamp"] = True
            self.errors.pop("bitstamp", None)
            await asyncio.sleep(2.0 if not per_pair else 2.0 + 0.5 * len(self.assets))


# ----------------------------------------------------------------------------- one coin
async def fetch_candles(http: httpx.AsyncClient, asset: str) -> list[tuple[float, float]]:
    """About five hours of 1-minute closes (ts, close), oldest first: Coinbase's, else Kraken's, else none."""
    try:
        r = await http.get(COINBASE_CANDLES.format(product=coinbase_symbol(asset)), params={"granularity": 60})
        if r.status_code == 200 and isinstance(r.json(), list):
            rows = [(float(c[0]) + 60.0, float(c[4])) for c in r.json() if isinstance(c, list) and len(c) >= 5]
            if len(rows) >= 30:
                return sorted(rows)
    except Exception as exc:
        log.info("Coinbase candles for %s failed: %s", asset, exc)
    try:
        r = await http.get(KRAKEN_OHLC, params={"pair": f"{KRAKEN_REST.get(asset.upper(), asset.upper())}USD", "interval": 1})
        data = r.json() if r.status_code == 200 else {}
        result = data.get("result") or {}
        for key, rows in result.items():
            if key != "last" and isinstance(rows, list):
                out = [(float(c[0]) + 60.0, float(c[4])) for c in rows[-300:] if isinstance(c, list) and len(c) >= 5]
                return sorted(out)
    except Exception as exc:
        log.info("Kraken candles for %s failed: %s", asset, exc)
    return []


class ExchangeFeed(PriceFeed):
    """One coin's price: the hub's median midpoint, printed once a second."""
    name = "exchanges"

    def __init__(self, hub: ExchangeHub, asset: str, label: str = ""):
        super().__init__()
        self.hub = hub
        self.asset = asset.upper()
        self.symbol = f"{label or self.asset}: median of {', '.join(NAMES[e] for e in hub.exchanges)}"
        self.mode = "connecting"
        self.used: list[str] = []
        hub.add(self.asset)
        self._task: asyncio.Task | None = None

    async def start(self):
        await self.hub.start()
        self._task = asyncio.create_task(self._run(), name=f"exchanges-{self.asset}")

    async def stop(self):
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
        await self.hub.stop()

    def print_once(self, now: float | None = None) -> float | None:
        """Publish this second's median (testable without the network)."""
        now = time.time() if now is None else now
        price, used = self.hub.mid(self.asset, now)
        self.used = used
        if price is None:
            self.mode = "waiting for exchange prices"
            self.connected = False
            errs = "; ".join(f"{NAMES[k]}: {v}" for k, v in self.hub.errors.items())
            self.last_error = errs or None
            return None
        self.connected = True
        self.last_error = None
        self.mode = f"exchanges ({', '.join(NAMES[e] for e in used)})"
        self._publish(float(int(now)), price)
        return price

    async def _run(self):
        if self.hub._http is not None:
            closes = await fetch_candles(self.hub._http, self.asset)
            if closes:
                self._publish_warmup([c for c in closes if c[0] <= time.time() - 1])
                log.info("exchanges: %s warmed up with %d one-minute closes", self.asset, len(closes))
        while True:
            try:
                self.print_once()
            except Exception:
                log.exception("exchanges %s print failed", self.asset)
            await asyncio.sleep(1.0 - (time.time() % 1.0) + 0.05)  # just after each whole second, like the index
