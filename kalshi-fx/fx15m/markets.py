"""The 15-minute foreign-exchange markets on the board, and everything that depends on a pair's price scale.

Kalshi's 15-minute FX markets (Financials, fifteen_min): EUR/USD, GBP/USD, USD/JPY, AUD/USD, USD/CAD and USD/MXN.
Each window settles on the Pyth Network close price of the pair at the window's close; the target is Pyth's price at
the open. Targets are written to 5 decimals (3 for USD/JPY), so moves read in pips: 0.0001, or 0.01 for yen.
Nothing scale-dependent is hand-tuned per pair: the tie allowance and the settlement-match tolerance come from the
decimals Kalshi writes the targets with, the volatility floor/cap from the price itself.
"""
from __future__ import annotations

import datetime as dt
import logging
import re
from dataclasses import dataclass, field

log = logging.getLogger(__name__)

# ----------------------------------------------------------------------------- calendars (ET)
# (weekday, hour, minute, name); Monday = 0. U.S. jobless claims move every dollar pair at 8:30 on Thursdays.
FX_WEEKLY = ((3, 8, 30, "U.S. jobless claims"),)
# FOMC rate decisions, 2:00 PM ET (2026 as listed by the Chicago Fed; early 2027 as announced, tentative).
FOMC = ("2026-10-28 14:00 FOMC decision", "2026-12-09 14:00 FOMC decision", "2027-01-27 14:00 FOMC decision",
        "2027-03-17 14:00 FOMC decision", "2027-04-28 14:00 FOMC decision", "2027-06-09 14:00 FOMC decision")
# U.S. CPI, 8:30 AM ET (Dec 10 is on BLS's own 2026 schedule, Nov 10 as several calendars list it, Jan 13 2027 projected).
CPI = ("2026-11-10 08:30 CPI", "2026-12-10 08:30 CPI", "2027-01-13 08:30 CPI")


def first_fridays(start: dt.date, months: int = 14) -> tuple:
    """U.S. jobs report (NFP), 8:30 AM ET, projected on each month's first Friday. The BLS sometimes moves it (a
    holiday, a shutdown): add the real date to extra_events when it differs."""
    out, y, m = [], start.year, start.month
    for _ in range(months):
        d = dt.date(y, m, 1)
        d += dt.timedelta(days=(4 - d.weekday()) % 7)
        out.append(f"{d.isoformat()} 08:30 U.S. jobs report")
        y, m = (y + 1, 1) if m == 12 else (y, m + 1)
    return tuple(out)


NFP = first_fridays(dt.date(2026, 10, 1))
# Central-bank decisions (ECB, BoE, BoJ, RBA, BoC, Banxico) move their own pair hard, but their 2026-27 dates could not
# be checked here: add them per pair as extra_events in config.toml, e.g. [board.extra_events] eurusd = ["2026-12-17 08:15 ECB"]
CALENDARS = {"fx": (FX_WEEKLY, FOMC + CPI + NFP), "other": ((), FOMC + CPI + NFP)}


@dataclass
class MarketSpec:
    key: str  # "eurusd"
    name: str  # "EUR/USD"
    series: str  # "KXEURUSD15M"
    badge: str  # "EUR", the card's icon text
    kind: str  # fx | other (picks the report calendar)
    color: str  # the badge colour
    sim_price: float  # demo only: a realistic starting price ...
    sim_sigma: float  # ... and volatility in price units per √s
    decimals: int = 5  # how Kalshi writes the targets; more seen later raises it

    @property
    def weekly(self) -> tuple:
        return CALENDARS.get(self.kind, CALENDARS["other"])[0]

    @property
    def dated(self) -> tuple:
        return CALENDARS.get(self.kind, CALENDARS["other"])[1]


# Kalshi's order on the 15-minute Financials page. Sim volatility: about 4-6 pips over 15 minutes.
KNOWN: list[MarketSpec] = [
    MarketSpec("eurusd", "EUR/USD", "KXEURUSD15M", "EUR", "fx", "#4f7cff", 1.09752, 0.000014, 5),
    MarketSpec("gbpusd", "GBP/USD", "KXGBPUSD15M", "GBP", "fx", "#b04cff", 1.31701, 0.000018, 5),
    MarketSpec("usdjpy", "USD/JPY", "KXUSDJPY15M", "JPY", "fx", "#ff5a7a", 159.256, 0.0024, 3),
    MarketSpec("audusd", "AUD/USD", "KXAUDUSD15M", "AUD", "fx", "#2ec4a6", 0.67301, 0.000011, 5),
    MarketSpec("usdcad", "USD/CAD", "KXUSDCAD15M", "CAD", "fx", "#e64b3c", 1.43131, 0.000011, 5),
    MarketSpec("usdmxn", "USD/MXN", "KXUSDMXN15M", "MXN", "fx", "#2fa35b", 18.2, 0.00025, 5),
]
BY_SERIES = {m.series: m for m in KNOWN}
# a currency pair's 15-minute series: KX + six letters + 15M (the rates, S&P, Nasdaq and Dow series are not pairs)
PAIR_RE = re.compile(r"^KX([A-Z]{3})([A-Z]{3})15M$")


def spec_for_series(ticker: str, title: str = "") -> MarketSpec | None:
    """A known pair, a generic card for a pair Kalshi added later, or None for a series that is not a currency pair."""
    ticker = ticker.upper()
    if ticker in BY_SERIES:
        return BY_SERIES[ticker]
    mt = PAIR_RE.match(ticker)
    if not mt:
        return None
    a, b = mt.groups()
    return MarketSpec(f"{a}{b}".lower(), f"{a}/{b}", ticker, b if a == "USD" else a, "fx", "#8b93a1", 1.0, 0.00001, 5)


async def discover(client, category: str = "Financials") -> list[MarketSpec]:
    """Kalshi's 15-minute FX series, in Kalshi's order (known first). TEST series and non-pairs are skipped; a pair
    without an open window is left off unless it is one of the known six (USD/MXN shows "no window" while dormant).
    Falls back to the known list when the series listing is unavailable, so the board always starts."""
    try:
        rows = await client.list_series_raw(category)
    except Exception as exc:  # a missing endpoint or a network error must not stop the board
        log.warning("series discovery failed (%s); using the known 15-minute FX pairs", exc)
        return list(KNOWN)
    found = []
    for r in rows:
        ticker = str(r.get("ticker") or "").upper()
        if not ticker.endswith("15M") or "TEST" in ticker:
            continue
        spec = spec_for_series(ticker, str(r.get("title") or ""))
        if spec is None:
            continue
        if ticker not in BY_SERIES:
            try:
                if not await client.get_markets_raw(ticker, status="open"):
                    log.info("series %s has no open window: no card (restart the board once Kalshi trades it)", ticker)
                    continue
            except Exception:
                continue
        found.append(spec)
    if not found:
        return list(KNOWN)
    order = {m.series: i for i, m in enumerate(KNOWN)}
    found.sort(key=lambda m: (order.get(m.series, 99), m.name))
    seen, out = set(), []
    for m in found:
        if m.series not in seen:
            seen.add(m.series)
            out.append(m)
    return out


# ----------------------------------------------------------------------------- scale
def decimals_of(x: float | None) -> int:
    """Decimals Kalshi used for a target (3.34148 -> 5, 4173.29 -> 2). A trailing zero is invisible in a float
    (1693.040 reads 1693.04), so the board keeps the largest count it has seen for a market."""
    if x is None:
        return 0
    text = f"{x:.8f}".rstrip("0")
    return len(text.split(".")[1]) if "." in text else 0


@dataclass
class Scale:
    """What depends on a market's price level. `decimals` grows as targets with more decimals are seen."""
    decimals: int = 5
    price: float | None = None
    seen: list = field(default_factory=list)

    def observe_target(self, strike: float | None):
        if strike:
            self.decimals = max(self.decimals, min(6, decimals_of(strike)))
            self.price = self.price or strike

    def observe_price(self, price: float | None):
        if price and price > 0:
            self.price = price

    @property
    def tick(self) -> float:
        return 10.0 ** (-self.decimals)

    @property
    def tie_adj(self) -> float:
        """Up wins ties; with targets written to `decimals`, Up means price >= target - half of the last digit."""
        return self.tick / 2.0

    @property
    def match_tol(self) -> float:
        """A feed price this close to the settlement IS the settlement price (for the timing check)."""
        return max(self.tick / 2.0, 0.00005)

    def vol_floor(self) -> float:
        return 2e-6 * (self.price or 1.0)  # gold ~0.008, gas ~0.000007 $/√s

    def vol_cap(self) -> float:
        return 1e-3 * (self.price or 1.0)  # gold ~4, gas ~0.0033 $/√s: real moves are never clipped

    def moves_in_cents(self) -> bool:
        return False  # FX moves read in pips

    @property
    def pip(self) -> float:
        """A pip: 0.0001 for a 5-decimal pair, 0.01 for yen (3 decimals)."""
        return 10.0 ** (-(max(self.decimals, 2) - 1))

    def as_dict(self) -> dict:
        return {"decimals": self.decimals, "tie_adj": self.tie_adj, "cents": False, "pip": self.pip}
