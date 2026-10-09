"""Dollar sizing by confidence tier. A 'unit' is what you normally put on a window; the tier scales it.

Tiers come from the model's probability for the side. Below the lowest tier a contract can still be a
trade when it is cheap enough: a 32% chance priced at 14¢ is a 'longshot' (positive edge, less likely to
win), sized at half a unit, or a full unit when the model's chance is at least double the price."""
from __future__ import annotations

from statistics import median

from .config import TradingCfg

TIERS = [  # (min probability for the side, units, label)
    (0.90, 3.0, "near-certain"),
    (0.80, 2.0, "strong"),
    (0.65, 1.0, "confident"),
    (0.58, 0.5, "lean"),
]
LONGSHOT_UNITS = (0.5, 1.0)  # (edge ≥ edge_min, model chance ≥ 2 × cost)


def tier_for(p_side: float, ask: float | None = None, fee: float = 0.0, edge_min: float = 0.05) -> tuple[str, float] | None:
    """(label, units) for a side the model gives p_side; with the ask, cheap contracts below the lowest tier
    qualify as 'longshot' when the edge after fees still clears edge_min."""
    for threshold, units, label in TIERS:
        if p_side >= threshold:
            return label, units
    if ask is not None and 0 < ask < 1:
        cost = ask + fee
        if p_side - cost >= edge_min:
            return "longshot", LONGSHOT_UNITS[1] if p_side >= 2.0 * cost else LONGSHOT_UNITS[0]
    return None


def unit_dollars(cfg: TradingCfg, recent_amounts: list[float] | None = None) -> float:
    """The configured unit, or (learn_unit) the median of the user's recent reported buys."""
    if cfg.learn_unit and recent_amounts:
        amounts = [a for a in recent_amounts if a and a > 0]
        if len(amounts) >= 3:
            return round(median(amounts), 2)
    return float(cfg.unit_dollars)


def dollars_for(p_side: float, cfg: TradingCfg, recent_amounts: list[float] | None = None, ask: float | None = None,
                fee: float = 0.0) -> tuple[float, str] | None:
    """(dollars, tier label) for a side the model gives p_side, or None if it is below every tier."""
    tier = tier_for(p_side, ask, fee, cfg.edge_min)
    if tier is None:
        return None
    label, units = tier
    amount = min(units * unit_dollars(cfg, recent_amounts), cfg.max_trade_dollars)
    return round(max(amount, 1.0), 2), label
