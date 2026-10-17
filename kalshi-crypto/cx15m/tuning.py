"""Per-coin settlement-call rules, and the strictness switch with three modes.

How they were chosen: 60 days of each coin's Kalshi history (Aug 18 - Oct 17 2026, about 5,700 windows a coin, 51,000
in all) were replayed second by second through this board's own code: the model with the 60-second-average settlement,
volatility measured on Kalshi's average, the index rebuilt from Kalshi's own average series (it reproduces the
settlement values to a third of a cent on Bitcoin), and Kalshi's 1-minute candles for its odds. For each coin and mode
the lowest bar (and the study time: 3:00, 4:00 or 5:00) whose calls won at least the mode's target on the first 60% of
days was kept: Standard 90%, Strict 94%, Very strict 97% (Bitcoin never quite reaches 97% there; its tightest bar,
99%, is used). Scored on the later 40% of days, never used to choose (25% check days, then the last 15% untouched):

  coin   Standard                     Strict                       Very strict
  BTC    92%: 90.7% won, 88% called   97%: 95.3% won, 80% called   99%: 97.2% won, 73% called
  ETH    87%: 90.1%, 93%              94%: 94.7%, 86%              98%: 97.9%, 76%
  SOL    88%: 92.0%, 91%              94%: 95.8%, 83%              98%: 98.4%, 74%
  XRP    86%: 89.5%, 95%              93%: 93.7%, 88%              98%: 97.5%, 77%
  DOGE   85%: 89.8%, 93%              92%: 94.5%, 85%              98%: 98.7%, 73%
  HYPE   87%: 91.6%, 92%              93%: 94.9%, 85%              98%: 98.3%, 75%
  NEAR   86%: 89.7%, 95%              93%: 94.4%, 88%              97%: 97.6%, 80%
  ZEC    87%: 90.4%, 94%              93%: 94.6%, 87%              97%: 97.4%, 81%
  BNB    89%: 90.8%, 92%              94%: 94.6%, 85%              98%: 98.0%, 76%
  all    90.5% won, 92% called        94.7%, 85%                   97.9%, 76%

Calls come with about 7:00 left (Standard), 5:30 (Strict) and 3:50 (Very strict), when Kalshi's ask is about 91c, 96c
and 98-99c: bought at the ask they break even (-1.6c to +1.6c a call by coin), as on the commodities board. PULL OUT
was right on 752 of its 763 alarms on the Standard calls. EDGE (the commodities buy rule) lost 1.5c a signal over the
same days (2155-702), so on crypto it is only watched (cx15m/edge.py, [board] edge).

Every coin: the 10-second hold, no lean call, no call in the last minute, none for a window joined with under 5:00
left, none in a window holding an FOMC decision, a CPI release or Kalshi's Thursday maintenance. A coin Kalshi adds
later gets the defaults (85%, 5:00; Strict 93%, Very strict 97%). Override any value per coin in config.toml, e.g.
[tuning.btc] call_prob = 0.93; [tuning] enabled = false goes back to the original rules (72% bar, 3:00 study, a lean
call at 2:00 left).
"""
from __future__ import annotations

import dataclasses
import logging
from dataclasses import dataclass

log = logging.getLogger(__name__)

MODES = ("standard", "strict", "very_strict")
MODE_LABELS = {"standard": "Standard", "strict": "Strict", "very_strict": "Very strict"}
MODE_TAGS = {"standard": "v1", "strict": "strict", "very_strict": "very-strict"}  # tags the calls each mode makes
# each coin's Strict / Very strict rules (bar, study seconds), the table above
MODE_BARS: dict[str, dict[str, float | tuple[float, float]]] = {
    "btc": {"strict": (0.97, 180.0), "very_strict": (0.99, 180.0)},
    "eth": {"strict": (0.94, 180.0), "very_strict": (0.98, 180.0)},
    "sol": {"strict": (0.94, 180.0), "very_strict": (0.98, 180.0)},
    "xrp": {"strict": (0.93, 240.0), "very_strict": (0.98, 180.0)},
    "doge": {"strict": (0.92, 300.0), "very_strict": (0.98, 180.0)},
    "hype": {"strict": (0.93, 180.0), "very_strict": (0.98, 180.0)},
    "near": {"strict": (0.93, 180.0), "very_strict": (0.97, 180.0)},
    "zec": {"strict": (0.93, 180.0), "very_strict": (0.97, 180.0)},
    "bnb": {"strict": (0.94, 180.0), "very_strict": (0.98, 180.0)},
}
MODE_DEFAULT_BARS = {"strict": 0.93, "very_strict": 0.97}  # a coin Kalshi adds later
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


TUNING: dict[str, Tuning] = {  # each coin's Standard rules (the table above)
    "btc": Tuning(call_prob=0.92, study_s=180.0),
    "eth": Tuning(call_prob=0.87, study_s=300.0),
    "sol": Tuning(call_prob=0.88, study_s=300.0),
    "xrp": Tuning(call_prob=0.86, study_s=300.0),
    "doge": Tuning(call_prob=0.85, study_s=300.0),
    "hype": Tuning(call_prob=0.87, study_s=300.0),
    "near": Tuning(call_prob=0.86, study_s=300.0),
    "zec": Tuning(call_prob=0.87, study_s=300.0),
    "bnb": Tuning(call_prob=0.89, study_s=180.0),
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
