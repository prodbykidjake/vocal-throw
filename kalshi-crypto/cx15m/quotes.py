"""Kalshi's Up/Down prices for a window (bid/ask per side)."""
from __future__ import annotations

from dataclasses import dataclass


@dataclass
class Quotes:
    yes_bid: float | None = None
    yes_ask: float | None = None
    no_bid: float | None = None
    no_ask: float | None = None

    @classmethod
    def from_market(cls, m) -> "Quotes":
        return cls(m.yes_bid, m.yes_ask, m.no_bid, m.no_ask)

    def ask(self, side: str) -> float | None:
        return self.yes_ask if side == "UP" else self.no_ask

    def bid(self, side: str) -> float | None:
        return self.yes_bid if side == "UP" else self.no_bid
