"""Per-market settlement-call rules: v1.4's engines, and (v1.5) a strictness switch with three modes.

v1.5, what testing found. Seven per-market recipes were tested on the same two months, walk-forward (choices made only
on earlier days, scored on 3 later blocks of days at the same share of windows called as v1.4): a per-market mix of
the engine and Kalshi's price, momentum, volatility-so-far, time-left terms, holds of 0-20 s. Once calls were held to
the same timing and the same last-minute cut-off, none of them beat v1.4 (changes of -0.9 to +0.7 points); what looked
like gains came from calling later, from one-second spikes, or from the very last eligible second. So v1.4's engines
stay. What reliably buys accuracy is calling fewer windows: the same rule with a higher bar per market.

  mode         windows called   wins (held-out blocks)   called with      typical ask   bar
  Standard     ~85%             89-91%                   ~6:20 left       ~90c          v1.4's (85-88%)
  Strict       ~75%             93.5-95%                 ~5:00 left       ~93c          per market, 89-94%
  Very strict  ~65%             96.5-98%                 ~3:50 left       ~96c          per market, 94-97.5%

Profit buying the call at the ask stays about even at every level (Kalshi's price rises with the certainty): the
switch buys trust in the call, not an edge. EDGE (cm15m/edge.py) is still the buy signal. The board runs all three
modes on the same windows in the background (cm15m/shadow.py) and shows their records side by side, so the choice can
be made on live results.

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

MODES = ("standard", "strict", "very_strict")
MODE_LABELS = {"standard": "Standard", "strict": "Strict", "very_strict": "Very strict"}
MODE_TAGS = {"standard": "v1.4", "strict": "strict", "very_strict": "very-strict"}  # tags the calls each mode makes
# fitted on Aug-Oct 13 (scratchpad analysis wf/v15/fitmodes.py): the bar at which ~75% / ~65% of windows get a call
MODE_BARS = {
    "wti": {"strict": 0.935, "very_strict": 0.973},
    "gold": {"strict": 0.939, "very_strict": 0.973},
    "silver": {"strict": 0.930, "very_strict": 0.971},
    "copper": {"strict": 0.936, "very_strict": 0.975},
    "natgas": {"strict": 0.892, "very_strict": 0.939},
    "palladium": {"strict": 0.909, "very_strict": 0.953},
    "platinum": {"strict": 0.925, "very_strict": 0.962},
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
    "wti": Tuning(call_prob=0.88, study_s=300.0),
    "gold": Tuning(call_prob=0.88, study_s=240.0),
    "silver": Tuning(call_prob=0.88, study_s=300.0),
    "copper": Tuning(call_prob=0.88, study_s=300.0),
    "natgas": Tuning(call_prob=0.85, study_s=240.0, blend_market=0.65),
    "palladium": Tuning(call_prob=0.85, study_s=300.0),
    "platinum": Tuning(call_prob=0.85, study_s=300.0),
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
        base = dataclasses.replace(base, call_prob=MODE_BARS.get(key, MODE_DEFAULT_BARS)[mode])
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


def apply(trading, tuning: Tuning, window_s: float | None = None, rules_tag: str = "v1.4"):
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
