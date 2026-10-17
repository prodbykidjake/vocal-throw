"""The 15-minute crypto markets on the board, and everything that depends on a market's price scale.

Kalshi's 15-minute crypto Up/Down markets run from ~$0.09 Dogecoin (targets written to 7 decimals, $0.0885266) to
~$80,000 Bitcoin (2 decimals, $80,056.35). Each settles on the simple average of the 60 once-a-second prints of a CF
Benchmarks real-time index in the minute before the close (BRTI for Bitcoin, ETHUSD_RTI, ...), against a target that
is the same average in the minute before the open (= the previous window's settlement); a tie goes Up ("at least").
Nothing scale-dependent is hand-tuned per market: the tie allowance and the settlement-match tolerance come from the
decimals Kalshi writes the targets with, and the volatility floor/cap from the price itself.
"""
from __future__ import annotations

import logging
import re
from dataclasses import dataclass, field

log = logging.getLogger(__name__)

# ----------------------------------------------------------------------------- calendars (ET)
# (weekday, hour, minute, name); Monday = 0
# Kalshi's weekly exchange maintenance, Thursday 3:00-5:00 AM ET: trading is paused, so no calls in the windows it
# covers. One mark inside each of those eight windows (3:05, 3:20, ... 4:50) blocks exactly them.
MAINTENANCE = tuple((3, 3 + (5 + 15 * i) // 60, (5 + 15 * i) % 60, "Kalshi maintenance") for i in range(8))
# FOMC rate decisions, 2:00 PM ET (2026 as listed by the Chicago Fed; early 2027 as announced, tentative).
FOMC = ("2026-10-28 14:00 FOMC decision", "2026-12-09 14:00 FOMC decision", "2027-01-27 14:00 FOMC decision",
        "2027-03-17 14:00 FOMC decision", "2027-04-28 14:00 FOMC decision", "2027-06-09 14:00 FOMC decision")
# U.S. CPI, 8:30 AM ET (Dec 10 on BLS's own 2026 schedule, Nov 10 as several calendars list it, Jan 13 2027 projected).
# Jobs reports and other one-offs: add them as [board] extra_events.
CPI = ("2026-11-10 08:30 CPI", "2026-12-10 08:30 CPI", "2027-01-13 08:30 CPI")
CALENDARS = {"crypto": (MAINTENANCE, FOMC + CPI), "other": (MAINTENANCE, FOMC)}


@dataclass
class MarketSpec:
    key: str  # "btc"
    name: str  # "Bitcoin"
    series: str  # "KXBTC15M"
    badge: str  # "BTC", the card's icon text
    kind: str  # crypto | other (picks the report calendar)
    color: str  # the badge colour (light: the badge text is dark)
    sim_price: float  # demo only: a realistic starting price ...
    sim_sigma: float  # ... and volatility in $/√s (price x yearly volatility / 5616)
    decimals: int = 2  # how Kalshi writes the targets; more seen later raises it
    index: str = ""  # the CF Benchmarks index it settles on
    cents: bool | None = None  # show moves in cents (True), dollars (False), or by price level (None)

    @property
    def weekly(self) -> tuple:
        return CALENDARS.get(self.kind, CALENDARS["other"])[0]

    @property
    def dated(self) -> tuple:
        return CALENDARS.get(self.kind, CALENDARS["other"])[1]


# Kalshi's order on the 15-minute crypto page
KNOWN: list[MarketSpec] = [
    MarketSpec("btc", "Bitcoin", "KXBTC15M", "BTC", "crypto", "#f7931a", 80056.35, 6.4, 2, "BRTI", False),
    MarketSpec("eth", "Ethereum", "KXETH15M", "ETH", "crypto", "#8fa0f0", 2359.49, 0.27, 2, "ETHUSD_RTI", False),
    MarketSpec("sol", "Solana", "KXSOL15M", "SOL", "crypto", "#14f195", 103.7542, 0.014, 4, "SOLUSD_RTI", True),
    MarketSpec("xrp", "XRP", "KXXRP15M", "XRP", "crypto", "#7fc8e8", 1.5059, 0.00022, 4, "XRPUSD_RTI", True),
    MarketSpec("doge", "Dogecoin", "KXDOGE15M", "DOGE", "crypto", "#c2a633", 0.0885266, 0.0000134, 7, "DOGEUSD_RTI", True),
    MarketSpec("hype", "Hyperliquid", "KXHYPE15M", "HYPE", "crypto", "#97fce4", 84.8358, 0.015, 4, "HYPEUSD_RTI", True),
    MarketSpec("near", "NEAR", "KXNEAR15M", "NEAR", "crypto", "#00ec97", 4.6239, 0.00074, 4, "NEARUSD_RTI", True),
    MarketSpec("zec", "Zcash", "KXZEC15M", "ZEC", "crypto", "#f4b728", 815.9556, 0.17, 4, "ZECUSD_RTI", False),
    MarketSpec("bnb", "BNB", "KXBNB15M", "BNB", "crypto", "#f3ba2f", 709.82, 0.07, 2, "BNBUSD_RTI", False),
]
BY_SERIES = {m.series: m for m in KNOWN}


def spec_for_series(ticker: str, title: str = "") -> MarketSpec:
    """A known market, or a generic card for a 15-minute crypto series Kalshi added later."""
    ticker = ticker.upper()
    if ticker in BY_SERIES:
        return BY_SERIES[ticker]
    base = re.sub(r"^KX|15M$", "", ticker).lower() or ticker.lower()
    name = re.sub(r"\s*\(?15[- ]?m(in(ute)?s?)?\)?.*$", "", title or "", flags=re.I).strip() or base.upper()
    return MarketSpec(base, name, ticker, base[:4].upper(), "crypto", "#8b93a1", 100.0, 0.02, 2)


MULTI_COIN = ("LEAD", "COMP")  # KXCRYPTOLEAD15M (coin race) and KXCRYPTOCOMP15M (comparison): not one coin's Up/Down


async def discover(client, category: str = "Crypto") -> list[MarketSpec]:
    """Kalshi's 15-minute crypto series, known ones first in Kalshi's order. The known nine are always included (the
    category also lists daily, hourly and range series, and a partial listing must not drop a card). Another coin's
    15-minute series gets a card only if it has a window open now: Kalshi also lists dormant ones (Cardano, TON,
    Bitcoin Cash in October 2026) and those would be empty cards."""
    found = list(KNOWN)
    try:
        rows = await client.list_series_raw(category)
    except Exception as exc:  # a missing endpoint or a network error must not stop the board
        log.warning("series discovery failed (%s); using the known 15-minute crypto markets", exc)
        return found
    seen = {m.series for m in found}
    for r in rows:
        ticker = str(r.get("ticker") or "").upper()
        if not ticker.endswith("15M") or ticker in seen or any(x in ticker for x in MULTI_COIN):
            continue
        seen.add(ticker)
        try:
            live = await client.get_markets_raw(ticker, status="open")
        except Exception:
            live = None
        if live:
            found.append(spec_for_series(ticker, str(r.get("title") or "")))
        else:
            log.info("series %s has no open window: no card (restart the board once Kalshi trades it)", ticker)
    return found


# ----------------------------------------------------------------------------- scale
def decimals_of(x: float | None) -> int:
    """Decimals Kalshi used for a target (0.0885266 -> 7, 80056.35 -> 2). A trailing zero is invisible in a float
    (709.80 reads 709.8), so the board keeps the largest count it has seen for a market."""
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
    cents: bool | None = None  # the market's own choice of unit (MarketSpec.cents); None = by price level

    def observe_target(self, strike: float | None):
        if strike:
            self.decimals = max(self.decimals, min(8, decimals_of(strike)))
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
        return max(self.tick / 2.0, 1e-6 * (self.price or 1.0))

    def vol_floor(self) -> float:
        return 2e-6 * (self.price or 1.0)  # bitcoin ~0.16, dogecoin ~0.0000002 $/√s

    def vol_cap(self) -> float:
        return 1e-3 * (self.price or 1.0)  # bitcoin ~80 $/√s: real moves are never clipped

    def moves_in_cents(self) -> bool:
        return self.cents if self.cents is not None else (self.price or 0) < 200.0

    def as_dict(self) -> dict:
        return {"decimals": self.decimals, "tie_adj": self.tie_adj, "cents": self.moves_in_cents()}
