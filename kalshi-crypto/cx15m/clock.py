"""Time helpers. Everything internal is UTC; display uses US/Eastern like Kalshi does."""
from __future__ import annotations

import datetime as dt
from zoneinfo import ZoneInfo

UTC = dt.timezone.utc
ET = ZoneInfo("America/New_York")


def now() -> dt.datetime:
    return dt.datetime.now(UTC)


def parse_time(value) -> dt.datetime | None:
    """Parse Kalshi ISO-8601 timestamps ('2026-10-07T11:45:00Z' or with offset)."""
    if value in (None, ""):
        return None
    if isinstance(value, dt.datetime):
        return value if value.tzinfo else value.replace(tzinfo=UTC)
    if isinstance(value, (int, float)):
        return dt.datetime.fromtimestamp(float(value), UTC)
    s = str(value).strip()
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    try:
        parsed = dt.datetime.fromisoformat(s)
    except ValueError:
        return None
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=UTC)


def to_epoch(value: dt.datetime | None) -> float | None:
    return None if value is None else value.timestamp()


def seconds_until(value: dt.datetime | None, at: dt.datetime | None = None) -> float | None:
    if value is None:
        return None
    return (value - (at or now())).total_seconds()


def fmt_countdown(seconds: float | None) -> str:
    if seconds is None:
        return "--:--"
    seconds = max(0, int(seconds))
    return f"{seconds // 60:02d}:{seconds % 60:02d}"


def et_label(value: dt.datetime | None) -> str:
    """'7:45am ET' style label matching the Kalshi app."""
    if value is None:
        return "?"
    local = value.astimezone(ET)
    hour = local.hour % 12 or 12
    return f"{hour}:{local.minute:02d}{'am' if local.hour < 12 else 'pm'} ET"
