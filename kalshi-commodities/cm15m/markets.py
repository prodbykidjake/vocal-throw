"""The 15-minute commodity markets on the board, and everything that depends on a market's price scale.

Kalshi's 15-minute commodities range from ~$3 natural gas (five-decimal targets, $3.34148) to ~$4,000 gold
(two decimals, $4,173.29). Nothing scale-dependent is hand-tuned per market: the tie allowance and the
settlement-match tolerance come from the decimals Kalshi writes the targets with, and the volatility floor/cap
from the price itself, so a new market Kalshi adds is measured correctly from its first window.
"""
from __future__ import annotations

import logging
import re
from dataclasses import dataclass, field

log = logging.getLogger(__name__)

# ----------------------------------------------------------------------------- calendars (ET)
# (weekday, hour, minute, name); Monday = 0
OIL_WEEKLY = ((1, 16, 30, "API inventories"), (2, 10, 30, "EIA inventories"), (4, 13, 0, "Baker Hughes rigs"))
GAS_WEEKLY = ((3, 10, 30, "EIA gas storage"), (4, 13, 0, "Baker Hughes rigs"))
METAL_WEEKLY = ((3, 8, 30, "Jobless claims"),)
# FOMC rate decisions, 2:00 PM ET (2026 as listed by the Chicago Fed; early 2027 as announced, tentative).
# Monthly CPI / jobs / PCE dates could not be checked against the official schedules: add them as extra_events.
FOMC = ("2026-10-28 14:00 FOMC decision", "2026-12-09 14:00 FOMC decision", "2027-01-27 14:00 FOMC decision",
        "2027-03-17 14:00 FOMC decision", "2027-04-28 14:00 FOMC decision", "2027-06-09 14:00 FOMC decision")
# U.S. CPI, 8:30 AM ET (v1.4: the September report on Oct 14 moved every metal several standard deviations inside two
# minutes; Dec 10 is on BLS's own 2026 schedule, Nov 10 as several calendars list it, Jan 13 2027 projected).
CPI = ("2026-11-10 08:30 CPI", "2026-12-10 08:30 CPI", "2027-01-13 08:30 CPI")
CALENDARS = {"oil": (OIL_WEEKLY, ()), "gas": (GAS_WEEKLY, ()), "metal": (METAL_WEEKLY, FOMC + CPI), "other": ((), FOMC)}


@dataclass
class MarketSpec:
    key: str  # "gold"
    name: str  # "Gold"
    series: str  # "KXGOLD15M"
    badge: str  # "GLD", the card's icon text
    kind: str  # oil | gas | metal | other (picks the report calendar)
    color: str  # the badge colour
    sim_price: float  # demo only: a realistic starting price ...
    sim_sigma: float  # ... and volatility in $/√s
    decimals: int = 2  # how Kalshi writes the targets (from the 15-minute page); more seen later raises it

    @property
    def weekly(self) -> tuple:
        return CALENDARS.get(self.kind, CALENDARS["other"])[0]

    @property
    def dated(self) -> tuple:
        return CALENDARS.get(self.kind, CALENDARS["other"])[1]


# Kalshi's order on the 15-minute commodities page
KNOWN: list[MarketSpec] = [
    MarketSpec("gold", "Gold", "KXGOLD15M", "GLD", "metal", "#d4a017", 4173.29, 0.15, 2),
    MarketSpec("silver", "Silver", "KXSILVER15M", "SLV", "metal", "#b8bec7", 60.707, 0.006, 3),
    MarketSpec("wti", "WTI Oil", "KXWTI15M", "OIL", "oil", "#8fa3b8", 92.16, 0.005, 2),
    MarketSpec("copper", "Copper", "KXCOPPER15M", "COPR", "metal", "#c27a4e", 6.82193, 0.0005, 5),
    MarketSpec("natgas", "Natural Gas", "KXNATGAS15M", "GAS", "gas", "#1e9bff", 3.34148, 0.0005, 5),
    MarketSpec("palladium", "Palladium", "KXPALLADIUM15M", "PALL", "metal", "#e8e4d0", 1158.565, 0.08, 3),
    MarketSpec("platinum", "Platinum", "KXPLATINUM15M", "PLT", "metal", "#cfd3d8", 1693.04, 0.08, 3),
]
BY_SERIES = {m.series: m for m in KNOWN}

_METAL_WORDS = ("gold", "silver", "copper", "palladium", "platinum", "aluminium", "aluminum", "nickel", "zinc")


def spec_for_series(ticker: str, title: str = "") -> MarketSpec:
    """A known market, or a generic card for a 15-minute commodity Kalshi added later."""
    ticker = ticker.upper()
    if ticker in BY_SERIES:
        return BY_SERIES[ticker]
    base = re.sub(r"^KX|15M$", "", ticker).lower() or ticker.lower()
    name = re.sub(r"\s*\(?15[- ]?min(ute)?s?\)?\s*$", "", title or "", flags=re.I).strip() or base.title()
    low = f"{name} {base}".lower()
    kind = "metal" if any(w in low for w in _METAL_WORDS) else "gas" if "gas" in low else "oil" if ("oil" in low or "crude" in low) else "other"
    return MarketSpec(base, name, ticker, base[:4].upper(), kind, "#8b93a1", 100.0, 0.01, 2)


async def discover(client, category: str = "Commodities") -> list[MarketSpec]:
    """Kalshi's 15-minute commodity series, in Kalshi's order (known first). Falls back to the known list when the
    series listing is unavailable, so the board always starts."""
    try:
        rows = await client.list_series_raw(category)
    except Exception as exc:  # a missing endpoint or a network error must not stop the board
        log.warning("series discovery failed (%s); using the known 15-minute commodities", exc)
        return list(KNOWN)
    found = []
    for r in rows:
        ticker = str(r.get("ticker") or "").upper()
        if ticker.endswith("15M"):
            found.append(spec_for_series(ticker, str(r.get("title") or "")))
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
    decimals: int = 2
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
        return (self.price or 0) < 200.0

    def as_dict(self) -> dict:
        return {"decimals": self.decimals, "tie_adj": self.tie_adj, "cents": self.moves_in_cents()}
