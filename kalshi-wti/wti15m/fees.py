"""Kalshi trading fees.

Fee schedule (general table): taker fee = multiplier * 0.07 * contracts * price * (1 - price),
rounded UP to the next cent per order. Series with fee_type 'quadratic_with_maker_fees' also charge
makers multiplier * 0.0175 * ... ; plain 'quadratic' series charge makers nothing.
Series fee_type/fee_multiplier come from GET /series/{ticker}.
"""
from __future__ import annotations

import math
from dataclasses import dataclass

TAKER_RATE = 0.07
MAKER_RATE = 0.0175


@dataclass(frozen=True)
class FeeSchedule:
    fee_type: str = "quadratic"  # quadratic | quadratic_with_maker_fees | flat
    multiplier: float = 1.0

    @property
    def is_estimate(self) -> bool:
        # 'flat' uses a per-series table we cannot see; we fall back to the general formula.
        return self.fee_type not in ("quadratic", "quadratic_with_maker_fees")

    @classmethod
    def from_series(cls, fee_type, multiplier) -> "FeeSchedule":
        try:
            mult = float(multiplier) if multiplier not in (None, "") else 1.0
        except (TypeError, ValueError):
            mult = 1.0
        return cls(fee_type=str(fee_type or "quadratic"), multiplier=mult)


def round_up_cents(amount: float) -> float:
    """Round up to the next cent, tolerant of float noise (0.63000000001 -> 0.63)."""
    cents = amount * 100
    return math.ceil(cents - 1e-9) / 100


def taker_fee(price: float, count: int = 1, schedule: FeeSchedule | None = None) -> float:
    """Total taker fee in dollars for an order of `count` contracts at `price` (0..1)."""
    schedule = schedule or FeeSchedule()
    if count <= 0:
        return 0.0
    price = min(max(price, 0.0), 1.0)
    raw = schedule.multiplier * TAKER_RATE * count * price * (1.0 - price)
    return round_up_cents(raw)


def maker_fee(price: float, count: int = 1, schedule: FeeSchedule | None = None) -> float:
    schedule = schedule or FeeSchedule()
    if schedule.fee_type != "quadratic_with_maker_fees" or count <= 0:
        return 0.0
    price = min(max(price, 0.0), 1.0)
    raw = schedule.multiplier * MAKER_RATE * count * price * (1.0 - price)
    return round_up_cents(raw)


def fee_per_contract(price: float, count: int = 1, schedule: FeeSchedule | None = None) -> float:
    """Taker fee divided across the contracts of the order (what each contract 'costs' in fees)."""
    count = max(1, count)
    return taker_fee(price, count, schedule) / count
