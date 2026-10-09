"""Where can the contract price go before the close? Monte Carlo over the model's own price process.

The side's fair contract price at time t is
    c_t = Φ((S_t − K + tie) / (σ·√(τ_t + lag)))        (1 − that for DOWN)
with S_t = S_0 + σ·W_t, τ_t the trading time left at t and lag the settlement candle. We simulate paths on a
coarse grid, track the running max of c_t over the time you could still sell (trading time left minus the last
`exclude_last_s`), and read off
    touch(level) = P(max c_t ≥ level)   "chance the price gets to `level` before the close"
    level(prob)  = the price reached with probability `prob`.
A fixed seed with antithetic paths means the curve moves with the inputs, not with the dice, so the numbers
on screen do not jitter from second to second.
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from .model import clamp

SQRT2 = math.sqrt(2.0)
BG_BETA = 0.5826  # Broadie–Glasserman: a path checked every Δt under-reports its true max by ≈ 0.5826·σ·√Δt
_BLOCK_CACHE: dict[tuple[int, int], np.ndarray] = {}


def _normal_block(seed: int, rows: int, n_steps: int) -> np.ndarray:
    """A fixed block of standard normals per (seed, rows), sliced to n_steps: as the horizon shrinks by a
    step every few seconds each path keeps the same increments, so the numbers do not jump on the dice."""
    key = (seed, rows)
    blk = _BLOCK_CACHE.get(key)
    if blk is None or blk.shape[1] < n_steps:
        blk = np.random.default_rng(seed).standard_normal((rows, max(n_steps, 200)))
        _BLOCK_CACHE[key] = blk
    return blk[:, :n_steps]


def norm_cdf(x: np.ndarray) -> np.ndarray:
    """Vectorised Φ via Abramowitz–Stegun 7.1.26 (|error| < 1.5e-7); numpy has no erf."""
    z = np.abs(x) / SQRT2
    t = 1.0 / (1.0 + 0.3275911 * z)
    poly = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))))
    erf = 1.0 - poly * np.exp(-z * z)
    return 0.5 * (1.0 + np.sign(x) * erf)


@dataclass
class TouchCurve:
    side: str
    start: float  # fair contract price for the side right now
    maxima: np.ndarray  # running max per path, sorted ascending
    horizon_s: float  # seconds of sellable time simulated
    zone: "TouchCurve | None" = None  # the same paths cut off `zone_exclude_last_s` earlier (one simulation, two horizons)

    def touch(self, level: float) -> float:
        """Probability the side's price reaches `level` at some point before the close."""
        if level <= self.start:
            return 1.0
        n = len(self.maxima)
        if n == 0:
            return 0.0
        idx = int(np.searchsorted(self.maxima, level, side="left"))
        return float((n - idx) / n)

    def level(self, prob: float) -> float:
        """The price reached with probability `prob` (the (1 − prob) quantile of the running max)."""
        if len(self.maxima) == 0:
            return self.start
        prob = clamp(prob, 0.001, 0.999)
        return float(np.quantile(self.maxima, 1.0 - prob))

    def as_dict(self) -> dict:
        return {"side": self.side, "start": round(self.start, 4), "horizon_s": round(self.horizon_s),
                "p50": round(self.level(0.5), 3), "p20": round(self.level(0.2), 3)}


def fair_price(side: str, s: float, strike: float, sigma: float, tau_s: float, tie_adj: float) -> float:
    """The side's fair contract price for a single point (same formula as the model's base probability)."""
    if tau_s <= 0.5:
        p = 1.0 if s - strike + tie_adj >= 0 else 0.0
    else:
        p = float(norm_cdf(np.array([(s - strike + tie_adj) / (max(sigma, 1e-9) * math.sqrt(tau_s))]))[0])
    return p if side == "UP" else 1.0 - p


def simulate(side: str, s0: float, strike: float | None, sigma: float, seconds_left: float | None, settle_lag_s: float,
             tie_adj: float = 0.005, n_paths: int = 2000, step_s: float = 5.0, exclude_last_s: float = 10.0,
             seed: int = 7, offset: float = 0.0, zone_exclude_last_s: float | None = None) -> TouchCurve | None:
    """Touch curve for `side` from the current state. None when there is nothing to simulate.
    `offset` is subtracted from every simulated fair price: pass half the spread so the curve describes the
    BID you can actually sell into rather than the mid. With `zone_exclude_last_s` the result also carries
    `.zone`, the same paths cut off that many seconds before the close (no second simulation)."""
    if strike is None or seconds_left is None or sigma <= 0 or s0 <= 0:
        return None
    side = "UP" if side == "UP" else "DOWN"
    start = fair_price(side, s0, strike, sigma, seconds_left + settle_lag_s, tie_adj) - offset
    horizon = seconds_left - exclude_last_s
    n_steps = int(horizon // step_s)
    n_zone = None
    if zone_exclude_last_s is not None:
        n_zone = max(0, min(n_steps, int((seconds_left - zone_exclude_last_s) // step_s)))
    if n_steps < 1:
        flat = TouchCurve(side, start, np.full(1, start), 0.0)
        if n_zone is not None:
            flat.zone = TouchCurve(side, start, np.full(1, start), 0.0)
        return flat
    n_paths = max(2, n_paths)
    half = n_paths // 2
    # The grid is anchored to the close: step boundaries sit at fixed times before the close, the first step is
    # the partial one, and each step draws the random column that belongs to its absolute slot. As the clock
    # runs the same future step keeps the same increment, so the numbers move with the price, not with the grid.
    first = seconds_left - math.floor(seconds_left / step_s + 1e-9) * step_s
    if first < 1e-6:
        first = step_s
    times = first + step_s * np.arange(0, n_steps)
    times = times[times <= horizon + 1e-9]
    n_steps = len(times)
    if n_steps < 1:
        flat = TouchCurve(side, start, np.full(1, start), 0.0)
        if n_zone is not None:
            flat.zone = TouchCurve(side, start, np.full(1, start), 0.0)
        return flat
    slots = np.rint((seconds_left - times) / step_s).astype(int)  # steps remaining to the close at each grid point
    block = _normal_block(seed, half, int(slots.max()) + 1)
    dts = np.concatenate([[first], np.full(n_steps - 1, step_s)])
    dw = block[:, slots] * (sigma * np.sqrt(dts))
    dw = np.vstack([dw, -dw])  # antithetic pairs
    s = s0 + np.cumsum(dw, axis=1)
    # the max between grid points is missed on a 5-s grid: shift the path toward the side's favour by the
    # discrete-monitoring correction so touch()/level() are not biased low
    s = s + (BG_BETA * sigma * math.sqrt(step_s)) * (1.0 if side == "UP" else -1.0)
    if n_zone is not None:
        n_zone = int(np.sum(times <= seconds_left - zone_exclude_last_s + 1e-9))
    tau = (seconds_left - times) + settle_lag_s  # horizon still ahead of each simulated moment
    z = (s - strike + tie_adj) / (sigma * np.sqrt(np.maximum(tau, 1.0)))
    c = norm_cdf(z)
    if side == "DOWN":
        c = 1.0 - c
    maxima = np.maximum(c.max(axis=1) - offset, start)
    maxima.sort()
    out = TouchCurve(side, start, maxima, float(n_steps * step_s))
    if n_zone is not None:
        if n_zone < 1:
            out.zone = TouchCurve(side, start, np.full(1, start), 0.0)
        else:
            zm = np.maximum(c[:, :n_zone].max(axis=1) - offset, start)
            zm.sort()
            out.zone = TouchCurve(side, start, zm, float(n_zone * step_s))
    return out
