"""Per-market settlement-call rules (v1.4): fewer calls, surer calls.

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

RULES_TAG = "v1.4"


@dataclass(frozen=True)
class Tuning:
    call_prob: float = 0.85  # call when the chance for one side reaches this for 10 s ...
    study_s: float = 300.0  # ... after studying the window this long
    late_call: bool = False  # no lean call at 2:00 left: a window that never gets sure enough gets no call
    blend_market: float = 0.0  # > 0 = the call reads a log-odds blend of the model and Kalshi's mid (Kalshi's weight)


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


def enabled(overrides: dict | None) -> bool:
    return bool((overrides or {}).get("enabled", True))


def for_market(key: str, overrides: dict | None = None) -> Tuning:
    """The market's rules, with any [tuning.<key>] values from config.toml on top."""
    base = TUNING.get(key, DEFAULT)
    extra = (overrides or {}).get(key)
    if not isinstance(extra, dict) or not extra:
        return base
    names = {f.name for f in dataclasses.fields(Tuning)}
    for k in extra:
        if k not in names:
            log.warning("config: unknown key [tuning.%s].%s ignored", key, k)
    return dataclasses.replace(base, **{k: v for k, v in extra.items() if k in names})


def apply(trading, tuning: Tuning, window_s: float | None = None):
    """Write a market's rules into its own copy of the trading settings. `window_s` (the demo's short simulated
    windows) scales the study period with the window, as the demo does for every other clock."""
    trading.call_prob = float(tuning.call_prob)
    trading.late_call = bool(tuning.late_call)
    trading.call_blend_market = float(tuning.blend_market)
    trading.rules_tag = RULES_TAG
    if window_s is None:
        trading.study_s = float(tuning.study_s)
    else:
        trading.study_s = float(max(5, round(tuning.study_s * window_s / 900.0)))
    return trading
