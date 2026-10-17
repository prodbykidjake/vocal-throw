"""Per-coin settlement-call rules, and the strictness switch with three modes (from the commodities board, cm15m).

What carries over from two months of commodity history (Aug-Oct 2026, 17,023 windows, cm15m/tuning.py): every
market's win rate rose with the call bar, in both halves of the data, while late "lean" calls at 2:00 left won only
63-73%; a 10-second hold, no call in the last minute and none for a window joined with under 5:00 left kept the
calls honest; and the reliable way to buy accuracy was calling fewer windows. So every coin starts with those rules:

  mode         bar     study   hold   lean call at 2:00
  Standard     85%     5:00    10 s   none
  Strict       93%     5:00    10 s   none
  Very strict  97%     5:00    10 s   none

Those bars are starting points, not yet tuned on crypto: each coin's own bars come from its Kalshi history
(tools/backfill.py on each KX...15M series), chosen walk-forward on earlier days and checked on later ones, the way
the commodities bars were. Until then, the board runs all three modes on the same windows in the background
(cx15m/shadow.py) and shows their records side by side, so the live results say which bar each coin deserves.

Override any value per coin in config.toml, e.g. [tuning.btc] call_prob = 0.90; [tuning] enabled = false goes back
to the original single-price rules (72% bar, 3:00 study, a lean call at 2:00 left).
"""
from __future__ import annotations

import dataclasses
import logging
from dataclasses import dataclass

log = logging.getLogger(__name__)

MODES = ("standard", "strict", "very_strict")
MODE_LABELS = {"standard": "Standard", "strict": "Strict", "very_strict": "Very strict"}
MODE_TAGS = {"standard": "v1", "strict": "strict", "very_strict": "very-strict"}  # tags the calls each mode makes
# per-coin Strict / Very strict rules: a bar, or (bar, study seconds) when that mode studies the window differently
MODE_BARS: dict[str, dict[str, float | tuple[float, float]]] = {}
MODE_DEFAULT_BARS = {"strict": 0.93, "very_strict": 0.97}
# a call made with this much left is EARLY (6:30). On commodities those calls won as often as the later ones at a
# lower ask (94.7% vs 95.3% in Strict, +1.4c a call against +0.3c); the board keeps their own record on crypto too
EARLY_LEFT_S = 390.0


@dataclass(frozen=True)
class Tuning:
    call_prob: float = 0.85  # call when the chance for one side reaches this for hold_s seconds ...
    study_s: float = 300.0  # ... after studying the window this long
    late_call: bool = False  # no lean call at 2:00 left: a window that never gets sure enough gets no call
    blend_market: float = 0.0  # > 0 = the call reads a log-odds blend of the model and Kalshi's mid (Kalshi's weight)
    hold_s: float = 10.0  # seconds the chance must stay above the bar


TUNING: dict[str, Tuning] = {}  # per-coin Standard rules, once tuned on history (none yet)
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
    """The market's rules in a mode (v1.4's bar, or the Strict / Very strict bar for that market), with any valid
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
