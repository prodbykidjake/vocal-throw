"""Configuration loading (TOML, all keys optional)."""
from __future__ import annotations

import dataclasses
import logging
import os
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
    # --- committed trade plans and dollar sizing (scalping) ---
    unit_dollars: float = 10.0  # one "unit"; tiers: lean 0.5, confident 1, strong 2, near-certain 3 units
    learn_unit: bool = False  # use the median of your last reported buys as the unit instead
    max_trade_dollars: float = 50.0
    min_scalp_cents: float = 8.0  # a plan needs at least this much room between the limit and the sell target
    confirm_s: float = 3.0  # a setup must persist this long before it becomes a plan
    min_hold_s: float = 20.0  # within this, only a hard invalidation cancels a plan
    miss_margin_cents: float = 4.0  # ask this far past the limit ...
    miss_seconds: float = 10.0  # ... for this long = missed
    cooldown_s: float = 30.0  # after a missed/cancelled plan, before the next one
    settle_advice: str = "when_clearly_worse"  # when_clearly_worse | never
    # --- forward-looking exits: "SELL BETWEEN low–high" from simulated price paths ---
    zone_low_prob: float = 0.50  # low end of the sell zone = price reached with this probability before the close
    zone_high_prob: float = 0.20  # high end = price reached with this probability
    zone_refresh_s: float = 30.0  # re-plan the zone at most this often (it ratchets up freely, down only when unlikely)
    zone_drop_prob: float = 0.25  # lower the zone only when the old low has under this chance of being reached
    zone_exclude_last_s: float = 30.0  # the zone looks at prices reachable before the last N s of trading (thin, binary)
    give_up_prob: float = 0.10  # sell at a loss only when the chance of getting back to breakeven is at or under this
    give_up_min_cash: float = 1.0  # ... and the sale still returns at least this many dollars
    plan_min_chance: float = 0.30  # a plan's sell target must have at least this chance of being reached
    # --- scalp calls: cheap contracts, judged by the chance of a sell level being reached, not by settlement edge ---
    scalp_calls: bool = True
    scalp_max_ask: float = 0.55  # only contracts at or under this price get scalp calls
    scalp_min_chance: float = 0.40  # the sell level must have at least this simulated chance of being reached
    scalp_min_score: float = 0.50  # chance + tilt (model vs market, momentum) must reach this
    # --- quick scalps card: cheap contracts, a few cents of profit, within a few minutes (riskier, less confident) ---
    quick_scalps: bool = False  # retired: the quick-scalps card lost money; plans only
    quick_max_ask: float = 0.35  # only contracts at or under this price
    quick_target_cents: float = 3.0  # sell at least this much above the limit ...
    quick_target_pct: float = 0.15  # ... or this fraction of the price, whichever is more
    quick_horizon_s: float = 180.0  # the pop must be likely within this many seconds
    quick_min_chance: float = 0.35  # minimum simulated chance of the pop
    quick_dollars: float = 5.0  # suggested size
    mc_paths: int = 2000  # simulated price paths per evaluation
    # --- feed basis correction ---
    basis_min_windows: int = 6  # apply a signed feed shift only after this many measured windows ...
    basis_min_t: float = 1.5  # ... and only when |bias| is at least this many standard errors from zero


@dataclass
class AutoCfg:
    """Automatic order placement on your Kalshi account. Off unless enabled AND an API key is configured."""
    enabled: bool = False
    dry_run: bool = True  # log every order it would send, send nothing (flip to false once the log looks right)
    api_key_id: str = ""  # Kalshi > Settings > API keys (or env KALSHI_API_KEY_ID)
    private_key_path: str = ""  # the .pem Kalshi gave you with that key (or env KALSHI_PRIVATE_KEY_PATH); never commit it
    take_plans: bool = True  # buy the signal card's plans at their limit
    take_quick: bool = False  # retired with the quick-scalps card
    max_order_dollars: float = 20.0  # cap on one buy
    max_window_dollars: float = 50.0  # bought on one 15-minute window
    max_day_dollars: float = 300.0  # bought per calendar day
    max_day_loss: float = 40.0  # realized loss in a day that stops buying until tomorrow
    max_losses_in_a_row: int = 4  # stop buying after this many losing trades in a row (resume = manual)
    buy_ttl_s: float = 20.0  # an unfilled buy is cancelled after this
    sell_slip_cents: float = 3.0  # a sell takes the book down to bid − this many cents
    sync_s: float = 5.0  # how often to read your Kalshi balance and positions
    fractional: bool = True  # the market allows fractional contracts (set false if orders are rejected over the count)
    order_api: str = "v2"  # v2 = /portfolio/events/orders (bid/ask on the Yes price) | legacy = /portfolio/orders


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
    auto: AutoCfg = field(default_factory=AutoCfg)
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
        auto=_build(AutoCfg, raw.get("auto"), "auto"),
        path=used,
    )
    # credentials may live in the environment instead of the file
    cfg.auto.api_key_id = os.environ.get("KALSHI_API_KEY_ID", cfg.auto.api_key_id)
    cfg.auto.private_key_path = os.environ.get("KALSHI_PRIVATE_KEY_PATH", cfg.auto.private_key_path)
    return cfg
