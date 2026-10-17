"""The exact settlement index, through Kalshi's own WebSocket (needs a Kalshi API key; read-only use).

Kalshi streams the CF Benchmarks indices its crypto markets settle on, once a second, on the authenticated
`cfbenchmarks_value` channel of wss://api.elections.kalshi.com/trade-api/ws/v2. Every WebSocket connection to Kalshi
needs a signed handshake, so this feed only runs when config.toml names an API key ([kalshi] api_key_id and
private_key_path). The key is used for nothing else: the board never places orders. The private key stays in its own
file on your computer; it is read when the board starts and never written, logged or sent anywhere (only the
signature of the handshake is).

Handshake (Kalshi's docs): headers KALSHI-ACCESS-KEY (the key ID), KALSHI-ACCESS-TIMESTAMP (milliseconds) and
KALSHI-ACCESS-SIGNATURE = base64(RSA-PSS-SHA256(timestamp + "GET" + "/trade-api/ws/v2")).
Subscribe: {"id": 1, "cmd": "subscribe", "params": {"channels": ["cfbenchmarks_value"], "index_ids": ["BRTI", ...]}}
A value message carries the index ID, the raw CF frame (`data`, a JSON string: {"type": "value", "id": "BRTI",
"time": <ms>, "value": "80056.35"}), the trailing 60-second average, and in the last minute before each quarter hour
`last_60s_windowed_average_15min`, the running settlement average. The parser below accepts those fields in any of
the places they have been seen; `python -m cx15m probe-index` prints raw messages to check them.
"""
from __future__ import annotations

import asyncio
import base64
import json
import logging
import os
import time

from .base import PriceFeed
from .exchanges import ws_connect

log = logging.getLogger(__name__)

WS_PATH = "/trade-api/ws/v2"
CHANNEL = "cfbenchmarks_value"


def key_settings(kalshi_cfg) -> tuple[str, str]:
    """(key ID, private key path) from config.toml, or the KALSHI_API_KEY_ID / KALSHI_PRIVATE_KEY_PATH environment."""
    key_id = (getattr(kalshi_cfg, "api_key_id", "") or os.environ.get("KALSHI_API_KEY_ID", "")).strip()
    path = (getattr(kalshi_cfg, "private_key_path", "") or os.environ.get("KALSHI_PRIVATE_KEY_PATH", "")).strip()
    return key_id, os.path.expanduser(path) if path else ""


def has_key(kalshi_cfg) -> bool:
    key_id, path = key_settings(kalshi_cfg)
    return bool(key_id and path)


def load_private_key(path: str):
    from cryptography.hazmat.primitives import serialization
    with open(path, "rb") as fh:
        return serialization.load_pem_private_key(fh.read(), password=None)


def sign_headers(key_id: str, private_key, ts_ms: int | None = None) -> dict:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding
    ts = str(int(time.time() * 1000) if ts_ms is None else ts_ms)
    sig = private_key.sign((ts + "GET" + WS_PATH).encode(),
                           padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.DIGEST_LENGTH),
                           hashes.SHA256())
    return {"KALSHI-ACCESS-KEY": key_id, "KALSHI-ACCESS-TIMESTAMP": ts,
            "KALSHI-ACCESS-SIGNATURE": base64.b64encode(sig).decode()}


def _value(x) -> float | None:
    if isinstance(x, dict):
        x = x.get("value", x.get("v"))
    try:
        v = float(str(x).replace(",", ""))
    except (TypeError, ValueError):
        return None
    return v if v > 0 else None


def _seconds(t) -> float | None:
    try:
        v = float(t)
    except (TypeError, ValueError):
        if isinstance(t, str):  # an ISO time
            import datetime as dt
            try:
                return dt.datetime.fromisoformat(t.replace("Z", "+00:00")).timestamp()
            except ValueError:
                return None
        return None
    return v / 1000.0 if v > 1e11 else v


def parse_index(message) -> dict | None:
    """One `cfbenchmarks_value` message -> {"id", "ts", "value", "avg60", "settle_avg"} (None for anything else)."""
    if isinstance(message, (str, bytes)):
        try:
            message = json.loads(message)
        except ValueError:
            return None
    if not isinstance(message, dict) or message.get("type") not in (CHANNEL, f"{CHANNEL}_5hz", None):
        return None
    msg = message.get("msg", message)
    if not isinstance(msg, dict):
        return None
    frame = msg.get("data")
    if isinstance(frame, (str, bytes)):
        try:
            frame = json.loads(frame)
        except ValueError:
            frame = None
    frame = frame if isinstance(frame, dict) else {}
    index_id = msg.get("index_id") or frame.get("id") or msg.get("id")
    value = _value(frame.get("value")) or _value(msg.get("value"))
    if not index_id or value is None:
        return None
    ts = _seconds(frame.get("time")) or _seconds(msg.get("time")) or _seconds(msg.get("received_at")) or time.time()
    return {"id": str(index_id).upper(), "ts": ts, "value": value, "avg60": _value(msg.get("avg_60s_data")),
            "settle_avg": _value(msg.get("last_60s_windowed_average_15min"))}


class KalshiIndexHub:
    """One authenticated connection streaming every coin's index."""

    def __init__(self, ws_url: str, key_id: str, key_path: str):
        self.ws_url = ws_url
        self.key_id = key_id
        self.key_path = key_path
        self.feeds: dict[str, "KalshiIndexFeed"] = {}
        self.connected = False
        self.last_error: str | None = None
        self._task: asyncio.Task | None = None
        self._users = 0
        self._key = None

    def add(self, feed: "KalshiIndexFeed"):
        self.feeds[feed.index_id] = feed

    async def start(self):
        self._users += 1
        if self._task is None:
            self._task = asyncio.create_task(self._forever(), name="kalshi-index")

    async def stop(self):
        self._users = max(0, self._users - 1)
        if self._users or self._task is None:
            return
        self._task.cancel()
        try:
            await self._task
        except (asyncio.CancelledError, Exception):
            pass
        self._task = None

    def handle(self, raw) -> bool:
        v = parse_index(raw)
        if v is None:
            return False
        feed = self.feeds.get(v["id"])
        if feed is None:
            return False
        feed.on_value(v)
        return True

    async def _forever(self):
        failures = 0
        while True:
            try:
                if self._key is None:
                    self._key = load_private_key(self.key_path)
                await self._run()
                failures = 0
            except asyncio.CancelledError:
                raise
            except FileNotFoundError:
                self.last_error = f"private key file not found: {self.key_path}"
                failures += 1
            except Exception as exc:
                failures += 1
                text = str(exc)
                if "401" in text or "403" in text:
                    text = f"Kalshi refused the API key ({text[:80]}): check api_key_id and the key file"
                self.last_error = text[:200]
                if failures in (1, 10) or failures % 100 == 0:
                    log.warning("Kalshi index feed failed (%d): %s", failures, self.last_error)
            self.connected = False
            for f in self.feeds.values():
                f.connected = False
                f.last_error = self.last_error
                f.mode = "index down"
            await asyncio.sleep(min(60.0, 2.0 * 2 ** min(failures, 5)))

    async def _run(self):
        async with ws_connect(self.ws_url, sign_headers(self.key_id, self._key)) as ws:
            await ws.send(json.dumps({"id": 1, "cmd": "subscribe",
                                      "params": {"channels": [CHANNEL], "index_ids": sorted(self.feeds)}}))
            self.connected = True
            self.last_error = None
            async for raw in ws:
                if not self.handle(raw):
                    try:
                        msg = json.loads(raw)
                    except ValueError:
                        continue
                    if msg.get("type") == "error":
                        self.last_error = str(msg.get("msg"))[:200]
                        log.warning("Kalshi index feed: %s", self.last_error)


class KalshiIndexFeed(PriceFeed):
    """One coin's exact index (BRTI, ETHUSD_RTI, ...)."""
    name = "kalshi-index"

    def __init__(self, hub: KalshiIndexHub, index_id: str, label: str = ""):
        super().__init__()
        self.hub = hub
        self.index_id = index_id.upper()
        self.symbol = f"{label or self.index_id}: CF Benchmarks {self.index_id} (exact, via Kalshi)"
        self.mode = "connecting"
        self.avg60: float | None = None
        self._settle_avg: tuple[float, float] | None = None  # (ts, running settlement average)
        hub.add(self)

    def on_value(self, v: dict):
        self.connected = True
        self.last_error = None
        self.mode = "kalshi-index"
        self.avg60 = v.get("avg60")
        if v.get("settle_avg"):
            self._settle_avg = (v["ts"], v["settle_avg"])
        self._publish(v["ts"], v["value"])

    def settle_avg(self, now: float | None = None) -> float | None:
        """Kalshi's own running settlement average, in the last minute before a close (None when not fresh)."""
        now = time.time() if now is None else now
        if self._settle_avg and now - self._settle_avg[0] <= 3.0:
            return self._settle_avg[1]
        return None

    async def start(self):
        await self.hub.start()

    async def stop(self):
        await self.hub.stop()
