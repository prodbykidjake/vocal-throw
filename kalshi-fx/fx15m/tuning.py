"""Per-pair settlement-call rules and the strictness switch (Standard / Strict / Very strict).

How they were chosen. Kalshi history for each pair (23 Sep - 18 Nov 2026 for EUR/USD, GBP/USD and USD/JPY, 3,716
windows each; 7 Oct - 18 Nov for AUD/USD and USD/CAD, 2,777 each; USD/MXN has had no windows) was replayed second by
second through this board's own model (fx15m/model.py with its learned volatility and learner, the report-window block
included), with Kalshi's 1-minute quotes for its odds. For each pair and mode the lowest bar (and a study time of
3:00, 4:00 or 5:00) whose calls won the mode's target on the first 60% of days was kept: Standard 90%, Strict 94%,
Very strict 97%. They were then scored on the last 40% of days, never used to choose them:

  pair      Standard                    Strict                      Very strict
  EUR/USD   86%, 5:00: 90.4%, 88% called  91%, 5:00: 93.6%, 80%       96%, 3:00: 97.2%, 69%
  GBP/USD   86%, 4:00: 90.3%, 87%         92%, 3:00: 93.7%, 78%       96%, 3:00: 96.5%, 68%
  USD/JPY   86%, 5:00: 90.4%, 86%         93%, 3:00: 94.9%, 76%       98%, 3:00: 98.4%, 61%
  AUD/USD   87%, 3:00: 90.3%, 86%         92%, 3:00: 94.3%, 79%       97%, 3:00: 97.5%, 67%
  USD/CAD   83%, 3:00: 87.0%, 91%         90%, 3:00: 91.7%, 82%       96%, 3:00: 96.5%, 70%
  all five  89.8% won, 88% called         93.7%, 79%                  97.2%, 67%

USD/CAD fell short of its targets on the later days (its choosing days were calmer): Strict or Very strict suits it.
Kalshi prices these markets tightly: bought at the ask the calls lost 1.3c (Standard), 1.0c (Strict) and 0.1c (Very
strict) a call on those days. The calls are forecasts you can trust, not an edge on Kalshi's price.

Checked and not adopted: the crypto board's stricter 3-6 PM ET bar (FX calls in those hours did about as well as the
rest, better on some pairs, worse on others); EDGE as a buy signal (82% won but -1.5c a signal on the later days, so
it runs as a test with no alerts, [board] edge = "watch"). PULL OUT (30%) was right on 347 of 436 alarms (80%).

Every pair: the 10-second hold, no lean call, no call in the last minute or for a window joined with under 5:00 left,
and none in a window holding a scheduled U.S. release (fx15m/markets.py). Override any value per pair in config.toml,
e.g. [tuning.usdcad] call_prob = 0.88; [tuning] enabled = false falls back to the defaults (85%, 5:00).
"""
from __future__ import annotations

import dataclasses
import logging
from dataclasses import dataclass

log = logging.getLogger(__name__)

MODES = ("standard", "strict", "very_strict")
MODE_LABELS = {"standard": "Standard", "strict": "Strict", "very_strict": "Very strict"}
MODE_TAGS = {"standard": "v1", "strict": "strict", "very_strict": "very-strict"}  # tags the calls each mode makes
# each pair's Strict / Very strict bars (filled in from the history replays; see the docstring)
MODE_BARS: dict[str, dict[str, float | tuple[float, float]]] = {
    "eurusd": {"strict": (0.91, 300.0), "very_strict": (0.96, 180.0)},
    "gbpusd": {"strict": (0.92, 180.0), "very_strict": (0.96, 180.0)},
    "usdjpy": {"strict": (0.93, 180.0), "very_strict": (0.98, 180.0)},
    "audusd": {"strict": (0.92, 180.0), "very_strict": (0.97, 180.0)},
    "usdcad": {"strict": (0.90, 180.0), "very_strict": (0.96, 180.0)},
}
MODE_DEFAULT_BARS = {"strict": 0.93, "very_strict": 0.97}  # a market Kalshi adds later
# v1.6: a call made with this much left is EARLY. In Strict about a third of the calls come that early (median ~8:25 left);
# they won as often as the later ones (94.7% vs 95.3%) at a lower ask (~93c vs ~95c): +1.4c a call against +0.3c
EARLY_LEFT_S = 390.0


@dataclass(frozen=True)
class Tuning:
    call_prob: float = 0.85  # call when the chance for one side reaches this for hold_s seconds ...
    study_s: float = 300.0  # ... after studying the window this long
    late_call: bool = False  # no lean call at 2:00 left: a window that never gets sure enough gets no call
    blend_market: float = 0.0  # > 0 = the call reads a log-odds blend of the model and Kalshi's mid (Kalshi's weight)
    hold_s: float = 10.0  # seconds the chance must stay above the bar


TUNING: dict[str, Tuning] = {
    "eurusd": Tuning(call_prob=0.86, study_s=300.0),
    "gbpusd": Tuning(call_prob=0.86, study_s=240.0),
    "usdjpy": Tuning(call_prob=0.86, study_s=300.0),
    "audusd": Tuning(call_prob=0.87, study_s=180.0),
    "usdcad": Tuning(call_prob=0.83, study_s=180.0),
}
DEFAULT = Tuning()


def _bool(value, where: str, default: bool) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str) and value.strip().lower() in ("true", "false", "yes", "no", "on", "off"):
        return value.strip().lower() in ("true", "yes", "on")
    log.warning("config: %s = %r is not true/false; using %s", where, value, str(default).lower())
    return default


def _number(value, where: str, lo: float, hi: float, default: float, percent: bool = False) -> float:
    try:
        x = float(value)
    except (TypeError, ValueError):
        x = float("nan")
    if percent and 1.0 < x <= 100.0:  # "call_prob = 88" means 88%
        log.warning("config: %s = %r read as %.2f", where, value, x / 100.0)
        x /= 100.0
    if not lo <= x <= hi:
        log.warning("config: %s = %r is outside %s-%s; using %s", where, value, lo, hi, default)
        return default
    return x


def _sections(overrides: dict | None) -> dict[str, dict]:
    """[tuning.<market>] sections by lower-case market key ([tuning.WTI] is [tuning.wti])."""
    return {str(k).lower(): v for k, v in (overrides or {}).items() if isinstance(v, dict)}


def enabled(overrides: dict | None) -> bool:
    return _bool((overrides or {}).get("enabled", True), "[tuning] enabled", True)


def mode_of(value) -> str:
    """'strict', 'Very strict', 'very-strict' ... -> a mode key; anything else -> standard."""
    key = str(value or "standard").strip().lower().replace("-", "_").replace(" ", "_")
    if key not in MODES:
        log.warning("call mode %r is not one of %s; using standard", value, ", ".join(MODES))
        return "standard"
    return key


def tag(mode: str = "standard") -> str:
    return MODE_TAGS[mode_of(mode)]


def check(overrides: dict | None, keys) -> list[str]:
    """Warn about [tuning.<name>] sections that match none of the board's markets; returns their names."""
    keys = {k.lower() for k in keys}
    unknown = sorted(k for k in _sections(overrides) if k not in keys)
    for k in unknown:
        log.warning("config: [tuning.%s] matches no market on the board (%s); ignored", k, ", ".join(sorted(keys)))
    return unknown


def for_market(key: str, overrides: dict | None = None, mode: str = "standard") -> Tuning:
    """The pair's rules in a mode (its Standard bar, or its Strict / Very strict (bar, study)), with any valid
    [tuning.<key>] values from config.toml on top."""
    base = TUNING.get(key, DEFAULT)
    mode = mode_of(mode)
    if mode != "standard":
        rule = MODE_BARS.get(key, MODE_DEFAULT_BARS)[mode]
        bar, study = (rule if isinstance(rule, tuple) else (rule, base.study_s))
        base = dataclasses.replace(base, call_prob=float(bar), study_s=float(study))
    extra = _sections(overrides).get(key.lower())
    if not extra:
        return base
    names = {f.name for f in dataclasses.fields(Tuning)}
    vals = {}
    for k, v in extra.items():
        where = f"[tuning.{key}] {k}"
        if k not in names:
            log.warning("config: unknown key %s ignored", where)
        elif k == "call_prob":
            vals[k] = _number(v, where, 0.5, 0.999, base.call_prob, percent=True)
        elif k == "study_s":
            vals[k] = _number(v, where, 0.0, 840.0, base.study_s)
        elif k == "blend_market":
            vals[k] = _number(v, where, 0.0, 1.0, base.blend_market)
        elif k == "hold_s":
            vals[k] = _number(v, where, 0.0, 60.0, base.hold_s)
        elif k == "late_call":
            vals[k] = _bool(v, where, base.late_call)
    return dataclasses.replace(base, **vals)


def apply(trading, tuning: Tuning, window_s: float | None = None, rules_tag: str = "v1"):
    """Write a market's rules into its own copy of the trading settings. `window_s` (the demo's short simulated
    windows) scales the study period with the window, as the demo does for every other clock (and keeps the demo's
    shorter hold)."""
    trading.call_prob = float(tuning.call_prob)
    trading.late_call = bool(tuning.late_call)
    trading.call_blend_market = float(tuning.blend_market)
    trading.rules_tag = rules_tag
    if window_s is None:
        trading.study_s = float(tuning.study_s)
        trading.call_hold_s = float(tuning.hold_s)
    else:
        trading.study_s = float(max(5, round(tuning.study_s * window_s / 900.0)))
        trading.call_hold_s = float(min(trading.call_hold_s, tuning.hold_s))
    return trading
