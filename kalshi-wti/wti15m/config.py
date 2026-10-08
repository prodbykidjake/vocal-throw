"""Configuration loading (TOML, all keys optional)."""
from __future__ import annotations

import dataclasses
import logging
import pathlib
import tomllib
from dataclasses import dataclass, field

log = logging.getLogger(__name__)


@dataclass
class KalshiCfg:
    base_url: str = "https://api.elections.kalshi.com/trade-api/v2"
    series_ticker: str = "KXWTI15M"
    poll_interval_s: float = 1.0
    timeout_s: float = 10.0


@dataclass
class FeedCfg:
    source: str = "kalshi_live"  # kalshi_live (Kalshi's own Pyth series) | hyperliquid | sim
    fallback: str = "hyperliquid"  # used automatically when the primary is stale; "" for none
    kalshi_live_poll_s: float = 1.0
    hyperliquid_dex: str = "xyz"
    hyperliquid_symbol: str = ""
    hyperliquid_rest: str = "https://api.hyperliquid.xyz/info"
    hyperliquid_ws: str = "wss://api.hyperliquid.xyz/ws"
    stale_after_s: float = 8.0  # Hyperliquid mids arrive every 1-3 s; only call it stale well beyond that
    warmup_candles_min: int = 60


@dataclass
class TradingCfg:
    bankroll: float = 200.0
    edge_min: float = 0.05
    kelly_fraction: float = 0.25
    max_contracts: int = 20
    max_stake_fraction: float = 0.10
    confident_margin: float = 0.20
    min_warmup_minutes: float = 30.0
    no_entry_first_s: float = 60.0
    late_entry_s: float = 60.0
    late_entry_min_z: float = 2.0
    hold_to_settle_prob: float = 0.90
    stop_prob: float = 0.35
    profit_target: float = 0.20
    max_spread: float = 0.10
    tie_adj: float = 0.005  # settlement rounds to the cent and a tie pays Up
    # Kalshi settles on the CLOSE of the 1-minute Pyth candle AT the close time (e.g. the 7:00 PM candle,
    # which ends at 7:00:59), so the price keeps moving ~60 s after trading stops. Added to the horizon.
    settle_lag_s: float = 60.0


@dataclass
class ServerCfg:
    host: str = "127.0.0.1"
    port: int = 8787
    open_browser: bool = True


@dataclass
class NotifyCfg:
    desktop: bool = True
    sound: bool = True


@dataclass
class StorageCfg:
    db_path: str = "data/wti15m.db"


@dataclass
class Config:
    kalshi: KalshiCfg = field(default_factory=KalshiCfg)
    feed: FeedCfg = field(default_factory=FeedCfg)
    trading: TradingCfg = field(default_factory=TradingCfg)
    server: ServerCfg = field(default_factory=ServerCfg)
    notify: NotifyCfg = field(default_factory=NotifyCfg)
    storage: StorageCfg = field(default_factory=StorageCfg)
    path: str | None = None


def _build(cls, data: dict | None, section: str):
    obj = cls()
    if not data:
        return obj
    names = {f.name for f in dataclasses.fields(cls)}
    for key, value in data.items():
        if key in names:
            setattr(obj, key, value)
        else:
            log.warning("config: unknown key [%s].%s ignored", section, key)
    return obj


def load(path: str | None = None) -> Config:
    """Load config.toml if present (or the given path). Missing file = defaults."""
    candidates = [path] if path else ["config.toml", str(pathlib.Path(__file__).resolve().parents[1] / "config.toml")]
    raw: dict = {}
    used = None
    for cand in candidates:
        if cand and pathlib.Path(cand).is_file():
            with open(cand, "rb") as fh:
                raw = tomllib.load(fh)
            used = cand
            break
    cfg = Config(
        kalshi=_build(KalshiCfg, raw.get("kalshi"), "kalshi"),
        feed=_build(FeedCfg, raw.get("feed"), "feed"),
        trading=_build(TradingCfg, raw.get("trading"), "trading"),
        server=_build(ServerCfg, raw.get("server"), "server"),
        notify=_build(NotifyCfg, raw.get("notify"), "notify"),
        storage=_build(StorageCfg, raw.get("storage"), "storage"),
        path=used,
    )
    return cfg
