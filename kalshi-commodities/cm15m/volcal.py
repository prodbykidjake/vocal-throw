"""Learning how much WTI really moves, by time of day and around scheduled oil reports.

The odds of a 15-minute Up/Down window depend mostly on one number: how far the price can move before the
settlement candle closes (σ√τ). The live estimate (an EWMA of recent moves) lags oil's daily rhythm: it is still
quiet when the 9:00 ET open starts, still jumpy an hour after the 2:30 ET settle, and blind to the weekly reports.

Every window records the σ the model forecast at the end of its study period and the σ the price actually showed
over the rest of the window. VolProfile turns those pairs into a multiplier per ET hour (and per report), shrunk
toward "no correction" until enough windows exist, so the correction grows with evidence and never jumps on one
noisy window.
"""
from __future__ import annotations

import datetime as dt
import math
import statistics
from dataclasses import dataclass

from . import clock

# Weekly U.S. oil releases that move WTI (ET). EIA's Weekly Petroleum Status Report moves a day later after a
# Monday federal holiday; add those dates to [trading] extra_events in the config.
WEEKLY_EVENTS = (
    (1, 16, 30, "API inventories"),  # Tuesday 4:30 PM ET (American Petroleum Institute)
    (2, 10, 30, "EIA inventories"),  # Wednesday 10:30 AM ET (EIA Weekly Petroleum Status Report)
    (4, 13, 0, "Baker Hughes rigs"),  # Friday 1:00 PM ET
)
LOG_CLAMP = 1.5  # one window can move the estimate by at most e^±1.5 (a 4.5x surprise is treated as 4.5x)
MULT_RANGE = (0.5, 3.0)


def realized_sigma(ticks: list[tuple[float, float]], start: float, end: float, step_s: float = 5.0,
                   min_returns: int = 10) -> tuple[float | None, int]:
    """Realized σ in dollars per √second over [start, end] from (ts, price) ticks, resampled every step_s seconds
    (the same units as the model's σ). None when there are fewer than min_returns returns."""
    buckets: dict[int, float] = {}
    for ts, px in ticks:
        if start <= ts <= end and px and px > 0:
            buckets[int((ts - start) // step_s)] = px  # last price in each bucket
    keys = sorted(buckets)
    if len(keys) < 2:
        return None, 0
    ss, dt_sum, n = 0.0, 0.0, 0
    for a, b in zip(keys, keys[1:]):
        r = buckets[b] - buckets[a]
        ss += r * r
        dt_sum += (b - a) * step_s
        n += 1
    if n < min_returns or dt_sum <= 0:
        return None, n
    return math.sqrt(ss / dt_sum), n


def parse_extra_events(items: list[str] | None) -> list[tuple[float, str]]:
    """'2026-11-27 11:00 EIA inventories (holiday)' -> (epoch, name), times in ET. Bad lines are skipped."""
    out = []
    for item in items or []:
        try:
            date_s, time_s, *name = str(item).split()
            when = dt.datetime.strptime(f"{date_s} {time_s}", "%Y-%m-%d %H:%M").replace(tzinfo=clock.ET)
            out.append((when.timestamp(), " ".join(name) or "scheduled report"))
        except ValueError:
            continue
    return out


def events_between(start_ts: float, end_ts: float, extra: list[tuple[float, str]] | None = None,
                   weekly: tuple = WEEKLY_EVENTS) -> list[tuple[float, str]]:
    """Scheduled reports whose release time falls inside [start_ts, end_ts], as (epoch, name). `weekly` is the
    market's own weekly calendar (see markets.py); `extra` holds dated one-offs (FOMC, holiday shifts, ...)."""
    out = []
    day = dt.datetime.fromtimestamp(start_ts, clock.ET).date() - dt.timedelta(days=1)
    last = dt.datetime.fromtimestamp(end_ts, clock.ET).date()
    while day <= last:
        for weekday, hour, minute, name in weekly:
            if day.weekday() == weekday:
                ts = dt.datetime(day.year, day.month, day.day, hour, minute, tzinfo=clock.ET).timestamp()
                if start_ts <= ts <= end_ts:
                    out.append((ts, name))
        day += dt.timedelta(days=1)
    out.extend((ts, name) for ts, name in (extra or []) if start_ts <= ts <= end_ts)
    return sorted(out)


def et_hour(ts: float) -> int:
    return dt.datetime.fromtimestamp(ts, clock.ET).hour


@dataclass
class VolRow:
    open_ts: float
    forecast: float  # σ the model forecast (raw EWMA, before any correction)
    realized: float  # σ the price showed
    event: str = ""  # report name if the window held one

    @property
    def log_ratio(self) -> float:
        return max(-LOG_CLAMP, min(LOG_CLAMP, math.log(self.realized / self.forecast)))


class VolProfile:
    """Multiplier for the raw σ forecast: by ET hour, or by report for windows holding one.

    log-multiplier for an hour = (n·mean_hour + k·overall) / (n + k), overall itself shrunk toward 0 the same way;
    for a report: shrunk toward log(event_prior) instead, since report windows are known to be wilder."""

    def __init__(self, k: float = 8.0, event_prior: float = 1.5):
        self.k = k
        self.event_prior = event_prior
        self.overall = 0.0
        self.n_total = 0
        self.by_hour: dict[int, tuple[float, int]] = {}
        self.by_event: dict[str, tuple[float, int]] = {}
        self.error_raw: float | None = None  # median |log(realized/forecast)| without the correction
        self.error_fit: float | None = None  # ... with it (in-sample, leave-one-out on the hour means)

    def fit(self, rows: list[VolRow]):
        rows = [r for r in rows if r.forecast and r.realized and r.forecast > 0 and r.realized > 0]
        plain = [r for r in rows if not r.event]
        self.n_total = len(plain)
        mean = statistics.fmean(r.log_ratio for r in plain) if plain else 0.0
        self.overall = mean * len(plain) / (len(plain) + self.k)
        hours: dict[int, list[float]] = {}
        for r in plain:
            hours.setdefault(et_hour(r.open_ts), []).append(r.log_ratio)
        self.by_hour = {h: ((sum(v) + self.k * self.overall) / (len(v) + self.k), len(v)) for h, v in hours.items()}
        events: dict[str, list[float]] = {}
        for r in rows:
            if r.event:
                events.setdefault(r.event, []).append(r.log_ratio)
        prior = math.log(self.event_prior)
        self.by_event = {e: ((sum(v) + self.k * prior) / (len(v) + self.k), len(v)) for e, v in events.items()}
        # how far off the raw forecast is, and how far off it would have been with the hour correction
        # (leave-one-out, so a window does not get credit for correcting itself)
        if plain:
            self.error_raw = statistics.median(abs(r.log_ratio) for r in plain)
            errs = []
            for r in plain:
                v = hours[et_hour(r.open_ts)]
                loo = (sum(v) - r.log_ratio + self.k * self.overall) / (len(v) - 1 + self.k)
                errs.append(abs(r.log_ratio - loo))
            self.error_fit = statistics.median(errs)
        else:
            self.error_raw = self.error_fit = None

    def mult(self, open_ts: float, events: list[tuple[float, str]] | None = None) -> tuple[float, str, int]:
        """(multiplier, plain-English reason, windows behind it) for a window opening at open_ts."""
        lo, hi = MULT_RANGE
        if events:
            name = events[0][1]
            log_m, n = self.by_event.get(name, (math.log(self.event_prior), 0))
            m = min(hi, max(lo, math.exp(log_m)))
            src = f"learned from {n} {name} window{'s' if n != 1 else ''}" if n else "starting assumption until report windows are measured"
            return m, f"{name} report in this window: moves ×{m:.2f} ({src})", n
        h = et_hour(open_ts)
        log_m, n = self.by_hour.get(h, (0.0, 0))  # an hour with no windows of its own gets no correction
        m = min(hi, max(lo, math.exp(log_m)))
        label = dt.time(h).strftime("%-I %p").lower()
        if n == 0 and self.n_total == 0:
            return 1.0, "no measured windows yet: using the live volatility as is", 0
        return m, f"{label} ET windows move ×{m:.2f} the live estimate (learned from {n} window{'s' if n != 1 else ''} at this hour, {self.n_total} overall)", n

    def summary(self) -> dict:
        return {
            "n": self.n_total, "overall": round(math.exp(self.overall), 3),
            "hours": {str(h): {"mult": round(math.exp(v), 3), "n": n} for h, (v, n) in sorted(self.by_hour.items())},
            "events": {e: {"mult": round(math.exp(v), 3), "n": n} for e, (v, n) in sorted(self.by_event.items())},
            "error_raw": None if self.error_raw is None else round(math.exp(self.error_raw) - 1, 3),
            "error_fit": None if self.error_fit is None else round(math.exp(self.error_fit) - 1, 3),
        }
