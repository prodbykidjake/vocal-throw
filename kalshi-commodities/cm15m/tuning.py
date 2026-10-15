"""Per-market settlement-call rules. v1.5 (the default): each market reads its own mix of the engine and Kalshi's price
and calls the first second it is sure enough; v1.4 (rollback: [tuning] version = "1.4"): a fixed bar held for 10 s.

v1.5, why: re-tested on the same two months, walk-forward (choices made only on earlier days, scored on 3 later blocks
of days at the SAME share of windows called as v1.4):
  * The 10-second hold: at the same number of calls, calling the first second a slightly higher bar is reached was
    0.6-2.5 points more accurate than holding a lower bar for 10 s, in all seven markets. The hold was a crude way of
    being careful; the bar is a better one. Calls come 20-50 s later (still 5+ minutes before the close).
  * How much to trust the engine vs Kalshi's price differs by market (fitted log-odds weights, which also take the
    over-confidence out of the engine's tails): gold leans on Kalshi most (0.31), WTI 0.18, silver 0.13, copper 0.12;
    natural gas keeps v1.4's mix (Kalshi 0.65); palladium and platinum: the engine alone (Kalshi added nothing there).
  * Walk-forward, v1.5 vs v1.4 at the same coverage: silver +1.5 points, natural gas +2.3, platinum +1.3, copper
    +1.1, gold +1.0, WTI +0.5 (borderline), palladium +0.6 (not significant). Most of it comes from calling a little
    later; ideas that did not survive the test (momentum, volatility-so-far, time-left terms, a gold-led silver) are
    not used. History is not a promise: the card keeps a "since v1.5" record next to what v1.4 would have called on
    the same windows (cm15m/shadow.py).

v1.4: fewer calls, surer calls.

Why: on two months of Kalshi history (Aug-Oct 2026, 17,023 windows over the seven markets) every market's win rate
rose with the call bar, in both halves of the data, while the late "lean" calls at 2:00 left won only 63-73%. The
rules below were chosen on each market's first 60% of days. Replayed through the board's own code (these desks and
engines, the report-window block included), the last 40% of days, never used to choose them, went:

  market      bar   study  extra                                  win rate, held-out days   windows called
  WTI         88%   5:00                                          90.5%  (v1.3: 76.4%)      83%
  Gold        88%   4:00                                          91.4%  (78.0%)            84%
  Silver      88%   5:00                                          89.3%  (77.6%)            87%
  Copper      88%   5:00                                          90.7%  (77.0%)            84%
  Natural gas 85%   4:00   reads a blend with Kalshi's mid         90.9%  (77.9%)            82%
                           (log-odds, Kalshi's weight 0.65: Kalshi's price forecast gas better than the engine alone)
  Palladium   85%   5:00                                          89.8%  (78.0%)            87%
  Platinum    85%   5:00                                          88.8%  (80.8%)            90%
  all seven                                                       90.4%  (77.8%)            85%
  (new)       85%   5:00

The price of that: calls come later (median 6:27 left instead of 10:17) when Kalshi's ask is higher (about 90c instead
of 78c), so bought at the ask they stay about break-even (+0.1c a call on the held-out days; -0.8c before). EDGE
(cm15m/edge.py) is still the buy signal.

Every market: the 10-second hold, no call in the last minute, none for a window joined with under 5:00 left, and no
call in a window that holds a scheduled report (the coach engines now skip those too: windows opening at a release
won only 64%). Override any value per market in config.toml, e.g. [tuning.gold] call_prob = 0.90; [tuning]
enabled = false goes back to v1.3's rules (72% bar, 3:00 study, a lean call at 2:00 left).
"""
from __future__ import annotations

import dataclasses
import logging
from dataclasses import dataclass

log = logging.getLogger(__name__)

CURRENT = "1.5"


@dataclass(frozen=True)
class Tuning:
    call_prob: float = 0.85  # call when the chance for one side reaches this (for hold_s seconds) ...
    study_s: float = 300.0  # ... after studying the window this long
    late_call: bool = False  # no lean call at 2:00 left: a window that never gets sure enough gets no call
    blend_market: float = 0.0  # > 0 = the call reads a log-odds blend of the model and Kalshi's mid (Kalshi's weight)
    blend_model: float | None = None  # the model's weight in that blend (None = 1 - Kalshi's weight)
    hold_s: float = 10.0  # seconds the chance must stay above the bar (v1.5: 0 = the first second it gets there)


TUNING_V14: dict[str, Tuning] = {
    "wti": Tuning(call_prob=0.88, study_s=300.0),
    "gold": Tuning(call_prob=0.88, study_s=240.0),
    "silver": Tuning(call_prob=0.88, study_s=300.0),
    "copper": Tuning(call_prob=0.88, study_s=300.0),
    "natgas": Tuning(call_prob=0.85, study_s=240.0, blend_market=0.65),
    "palladium": Tuning(call_prob=0.85, study_s=300.0),
    "platinum": Tuning(call_prob=0.85, study_s=300.0),
}
DEFAULT_V14 = Tuning()

# v1.5: fitted on Aug-Oct 13 (scratchpad analysis wf/v15/final.py): bar = v1.4's share of windows called
TUNING_V15: dict[str, Tuning] = {
    "wti": Tuning(call_prob=0.913, study_s=300.0, hold_s=0, blend_market=0.179, blend_model=0.815),
    "gold": Tuning(call_prob=0.907, study_s=300.0, hold_s=0, blend_market=0.306, blend_model=0.698),
    "silver": Tuning(call_prob=0.905, study_s=300.0, hold_s=0, blend_market=0.132, blend_model=0.820),
    "copper": Tuning(call_prob=0.909, study_s=300.0, hold_s=0, blend_market=0.120, blend_model=0.856),
    "natgas": Tuning(call_prob=0.895, study_s=240.0, hold_s=0, blend_market=0.65),
    "palladium": Tuning(call_prob=0.894, study_s=300.0, hold_s=0),
    "platinum": Tuning(call_prob=0.901, study_s=300.0, hold_s=0),
}
DEFAULT_V15 = Tuning(call_prob=0.90, study_s=300.0, hold_s=0)

VERSIONS: dict[str, tuple[dict[str, Tuning], Tuning]] = {"1.4": (TUNING_V14, DEFAULT_V14), "1.5": (TUNING_V15, DEFAULT_V15)}
TUNING, DEFAULT = VERSIONS[CURRENT]


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


def version(overrides: dict | None) -> str:
    """The rules version: [tuning] version = "1.4" rolls back to v1.4; anything else unknown means the current one."""
    v = (overrides or {}).get("version", CURRENT)
    v = f"{v:.1f}" if isinstance(v, float) else str(v).strip().lstrip("vV")
    if v not in VERSIONS:
        log.warning("config: [tuning] version = %r is not one of %s; using %s", v, ", ".join(VERSIONS), CURRENT)
        return CURRENT
    return v


def tag(overrides: dict | None) -> str:
    return "v" + version(overrides)


def check(overrides: dict | None, keys) -> list[str]:
    """Warn about [tuning.<name>] sections that match none of the board's markets; returns their names."""
    keys = {k.lower() for k in keys}
    unknown = sorted(k for k in _sections(overrides) if k not in keys)
    for k in unknown:
        log.warning("config: [tuning.%s] matches no market on the board (%s); ignored", k, ", ".join(sorted(keys)))
    return unknown


def for_market(key: str, overrides: dict | None = None) -> Tuning:
    """The market's rules for the configured version, with any valid [tuning.<key>] values from config.toml on top."""
    table, default = VERSIONS[version(overrides)]
    base = table.get(key, default)
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
        elif k == "blend_model":
            vals[k] = _number(v, where, 0.0, 1.5, 1.0 - base.blend_market if base.blend_model is None else base.blend_model)
        elif k == "hold_s":
            vals[k] = _number(v, where, 0.0, 60.0, base.hold_s)
        elif k == "late_call":
            vals[k] = _bool(v, where, base.late_call)
    return dataclasses.replace(base, **vals)


def apply(trading, tuning: Tuning, window_s: float | None = None, rules_tag: str = "v" + CURRENT):
    """Write a market's rules into its own copy of the trading settings. `window_s` (the demo's short simulated
    windows) scales the study period with the window, as the demo does for every other clock (and keeps the demo's
    shorter hold)."""
    trading.call_prob = float(tuning.call_prob)
    trading.late_call = bool(tuning.late_call)
    trading.call_blend_market = float(tuning.blend_market)
    trading.call_blend_model = None if tuning.blend_model is None else float(tuning.blend_model)
    trading.rules_tag = rules_tag
    if window_s is None:
        trading.study_s = float(tuning.study_s)
        trading.call_hold_s = float(tuning.hold_s)
    else:
        trading.study_s = float(max(5, round(tuning.study_s * window_s / 900.0)))
        trading.call_hold_s = float(min(trading.call_hold_s, tuning.hold_s))
    return trading
