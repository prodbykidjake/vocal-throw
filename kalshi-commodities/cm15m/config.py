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
    timeout_s: float = 10.0
    max_requests_per_s: float = 14.0  # shared by every market on the board (Kalshi's public tier allows ~20)
    poll_s: float = 2.0  # each market's order book and live price, staggered across markets
    discovery_s: float = 10.0  # how often each market re-reads its window list
    category: str = "Commodities"  # Kalshi category searched for 15-minute series


@dataclass
class FeedCfg:
    stale_after_s: float = 8.0  # the live price older than this = "stale": no new calls


@dataclass
class TradingCfg:
    """The settlement call's rules (the same as the WTI coach v12.2) and what the model needs."""
    confident_margin: float = 0.20
    min_warmup_minutes: float = 30.0
    study_s: float = 180.0  # no calls in a window's first 3 minutes: the board studies the window
    vol_profile: bool = True  # learn how much each market really moves by ET hour and around its reports
    vol_profile_k: float = 8.0
    event_prior: float = 1.5
    event_entries: str = "block"  # block = no new calls in a window that holds a scheduled report | allow
    call_prob: float = 0.72  # call when the model gives one side at least this ...
    call_hold_s: float = 10.0  # ... for this many seconds in a row, after the study period
    call_deadline_s: float = 120.0  # otherwise call the lean when this many seconds are left
    call_join_min_s: float = 300.0  # no call for a window first seen with less than this left (no free wins)
    call_min_left_s: float = 60.0  # and never a call with less than this left
    late_call: bool = True  # False = no lean call at the deadline: a window that never gets sure enough gets no call
    call_blend_market: float = 0.0  # > 0 = the call reads a log-odds blend with Kalshi's mid (Kalshi's weight)
    rules_tag: str = ""  # set = the card also keeps a record of the calls made since these rules first ran
    settle_lag_s: float = 0.0  # Kalshi settles on the price AT the close (measured on WTI; the Stats check keeps watching)
    basis_min_windows: int = 6
    basis_min_t: float = 1.5


@dataclass
class BoardCfg:
    markets: list = field(default_factory=list)  # keys to show, e.g. ["gold", "wti"]; empty = every 15-minute commodity
    extra_events: dict = field(default_factory=dict)  # {"gold": ["2026-11-13 08:30 CPI"], ...}, ET
    muted: list = field(default_factory=list)  # markets whose calls do not notify (also switchable on each card)
    edge_gap: float = 0.08  # EDGE: buy only when the model's chance for its side beats that side's ask by this much ...
    edge_min_p: float = 0.75  # ... and is at least this (checked at each whole minute 3:00-13:00; see cm15m/edge.py)


@dataclass
class ServerCfg:
    host: str = "127.0.0.1"
    port: int = 8790  # WTI 8787, natural gas 8788, gold 8789: the board runs next to all of them
    open_browser: bool = True
    phone: bool = False  # true = `serve` also answers your phone (same Wi-Fi or Tailscale) via a private link


@dataclass
class NotifyCfg:
    desktop: bool = True
    sound: bool = True


@dataclass
class StorageCfg:
    data_dir: str = "data"  # one database per market: data/gold.db, data/wti.db, ...


@dataclass
class Config:
    kalshi: KalshiCfg = field(default_factory=KalshiCfg)
    feed: FeedCfg = field(default_factory=FeedCfg)
    trading: TradingCfg = field(default_factory=TradingCfg)
    board: BoardCfg = field(default_factory=BoardCfg)
    server: ServerCfg = field(default_factory=ServerCfg)
    notify: NotifyCfg = field(default_factory=NotifyCfg)
    storage: StorageCfg = field(default_factory=StorageCfg)
    tuning: dict = field(default_factory=dict)  # [tuning] enabled = false | [tuning.<market>] call_prob = ... (cm15m/tuning.py)
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
    board = raw.get("board")
    if isinstance(board, dict) and board.get("edge_gap") == 0.06 and board.get("edge_min_p", 0.55) == 0.55:
        # v1.2's EDGE rule copied from an old config.example.toml: v1.3 tightened it (8 points, 75%; see cm15m/edge.py)
        board = {k: v for k, v in board.items() if k not in ("edge_gap", "edge_min_p")}
        raw = {**raw, "board": board}
        log.warning("config: [board] edge_gap = 0.06 / edge_min_p = 0.55 are v1.2's EDGE rule; using v1.3's 0.08 / 0.75")
    return Config(
        kalshi=_build(KalshiCfg, raw.get("kalshi"), "kalshi"),
        feed=_build(FeedCfg, raw.get("feed"), "feed"),
        trading=_build(TradingCfg, raw.get("trading"), "trading"),
        board=_build(BoardCfg, raw.get("board"), "board"),
        server=_build(ServerCfg, raw.get("server"), "server"),
        notify=_build(NotifyCfg, raw.get("notify"), "notify"),
        storage=_build(StorageCfg, raw.get("storage"), "storage"),
        tuning=dict(raw.get("tuning") or {}),
        path=used,
    )
