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
    tradingview_symbol: str = "TVC:USOIL"  # the TradingView tab's chart (to look at; it does not feed the model)


@dataclass
class TradingCfg:
    bankroll: float = 200.0
    edge_min: float = 0.05
    kelly_fraction: float = 0.25
    max_contracts: int = 20
    max_stake_fraction: float = 0.10
    confident_margin: float = 0.20
    min_warmup_minutes: float = 30.0
    no_entry_first_s: float = 60.0  # (legacy) the study period below is the one that counts when longer
    # --- v10: study each window, learn how much WTI really moves, respect the weekly oil reports ---
    study_s: float = 120.0  # JUNIOR: studies 2 minutes, not 3, before it is allowed to call
    vol_profile: bool = True  # correct the volatility forecast by what each ET hour has really done
    vol_profile_k: float = 8.0  # windows of evidence at which an hour's correction counts half
    event_prior: float = 1.5  # assumed volatility multiplier for report windows until they are measured
    event_entries: str = "block"  # block = no new calls in a window that holds an oil report | allow
    extra_events: list = field(default_factory=list)  # ["2026-11-27 11:00 EIA inventories (holiday)"], ET
    # --- v10: the settlement call (one Up/Down prediction per window, scored at settlement) ---
    call_prob: float = 0.65  # JUNIOR: calls at 65% (v12 waits for 72%) ...
    call_hold_s: float = 6.0  # ... held for 6 seconds (v12: 10)
    # JUNIOR: assumes oil moves 20% less than the live estimate says. Real windows showed v12's model expecting more
    # movement than happened (most at 10 minutes left), so Junior trusts the current gap to the target sooner.
    vol_scale: float = 0.80
    call_deadline_s: float = 120.0  # otherwise call the lean when this many seconds are left
    call_join_min_s: float = 300.0  # no call for a window first seen with less than this left (no free wins)
    call_min_left_s: float = 60.0  # and never a call with less than this left
    late_entry_s: float = 60.0
    late_entry_min_z: float = 2.0
    hold_to_settle_prob: float = 0.90
    stop_prob: float = 0.35
    profit_target: float = 0.20
    max_spread: float = 0.10
    tie_adj: float = 0.005  # settlement rounds to the cent and a tie pays Up
    # Seconds between the window's close and the price Kalshi settles on, added to the horizon. v12 measured it on
    # real windows: the settlement equals Kalshi's live Pyth price AT the close (to the cent in 15 of 16 windows;
    # the price a minute later matched 2 of 16). The Stats tab keeps checking both moments.
    settle_lag_s: float = 0.0
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
    mc_paths: int = 2000  # simulated price paths per evaluation
    # --- feed basis correction ---
    basis_min_windows: int = 6  # apply a signed feed shift only after this many measured windows ...
    basis_min_t: float = 1.5  # ... and only when |bias| is at least this many standard errors from zero


@dataclass
class ServerCfg:
    host: str = "127.0.0.1"
    port: int = 8791  # WTI v12 8787, gas 8788, gold 8789, board 8790
    open_browser: bool = True


@dataclass
class NotifyCfg:
    desktop: bool = True
    sound: bool = True


@dataclass
class StorageCfg:
    db_path: str = "data/wtijr.db"


@dataclass
class Config:
    kalshi: KalshiCfg = field(default_factory=KalshiCfg)
    feed: FeedCfg = field(default_factory=FeedCfg)
    trading: TradingCfg = field(default_factory=TradingCfg)
    server: ServerCfg = field(default_factory=ServerCfg)
    notify: NotifyCfg = field(default_factory=NotifyCfg)
    storage: StorageCfg = field(default_factory=StorageCfg)
    path: str | None = None
    notes: list = field(default_factory=list)  # settings from an older config.toml that were replaced (shown at startup)


# Values an older config.example.toml shipped that later turned out wrong. A config.toml copied from that example
# still carries them, so they are replaced by the current default (with a note) instead of silently winning.
OUTDATED = {
    ("trading", "settle_lag_s"): ((60,), "v12 measured that Kalshi settles on the price at the close, not a minute later"),
    # a config.toml copied from the v12 coach carries its (more cautious) call rules: Junior uses its own
    ("trading", "call_prob"): ((0.70, 0.72), "WTI Junior calls at 65%"),
    ("trading", "call_hold_s"): ((10,), "WTI Junior holds 6 seconds"),
    ("trading", "study_s"): ((180,), "WTI Junior studies 2 minutes"),
}


def _upgrade(raw: dict, notes: list) -> dict:
    for (section, key), (olds, why) in OUTDATED.items():
        sec = raw.get(section)
        if isinstance(sec, dict) and key in sec and sec[key] in olds:
            old = sec[key]
            sec = dict(sec)
            del sec[key]
            raw = {**raw, section: sec}
            notes.append(f"config.toml: [{section}] {key} = {old} is outdated and was ignored ({why})")
    return raw


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
    notes: list[str] = []
    raw = _upgrade(raw, notes)
    cfg = Config(
        kalshi=_build(KalshiCfg, raw.get("kalshi"), "kalshi"),
        feed=_build(FeedCfg, raw.get("feed"), "feed"),
        trading=_build(TradingCfg, raw.get("trading"), "trading"),
        server=_build(ServerCfg, raw.get("server"), "server"),
        notify=_build(NotifyCfg, raw.get("notify"), "notify"),
        storage=_build(StorageCfg, raw.get("storage"), "storage"),
        path=used,
        notes=notes,
    )
    return cfg
