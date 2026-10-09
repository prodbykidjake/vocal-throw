"""Dollar sizing by confidence tier. A 'unit' is what you normally put on a window; the tier scales it."""
from __future__ import annotations

from statistics import median

from .config import TradingCfg

TIERS = [  # (min probability for the side, units, label)
    (0.90, 3.0, "near-certain"),
    (0.80, 2.0, "strong"),
    (0.65, 1.0, "confident"),
    (0.58, 0.5, "lean"),
]


def tier_for(p_side: float) -> tuple[str, float] | None:
    for threshold, units, label in TIERS:
        if p_side >= threshold:
            return label, units
    return None


def unit_dollars(cfg: TradingCfg, recent_amounts: list[float] | None = None) -> float:
    """The configured unit, or (learn_unit) the median of the user's recent reported buys."""
    if cfg.learn_unit and recent_amounts:
        amounts = [a for a in recent_amounts if a and a > 0]
        if len(amounts) >= 3:
            return round(median(amounts), 2)
    return float(cfg.unit_dollars)


def dollars_for(p_side: float, cfg: TradingCfg, recent_amounts: list[float] | None = None) -> tuple[float, str] | None:
    """(dollars, tier label) for a side the model gives p_side, or None if below the lowest tier."""
    tier = tier_for(p_side)
    if tier is None:
        return None
    label, units = tier
    amount = min(units * unit_dollars(cfg, recent_amounts), cfg.max_trade_dollars)
    return round(max(amount, 1.0), 2), label
