"""Probability model for a 15-minute Up/Down window.

1. Base: digital-option probability  p = Φ((S - K + tie_adj) / (σ·√τ))
   S = live price, K = target, τ = seconds left, σ = realized vol per √second (EWMA, fast+slow blend).
2. Learner: tiny online logistic regression over [1, logit(p_base), logit(p_market), momentum z's,
   time-left fraction, vol ratio, previous outcome]. Trained after each settled window on the 15-second
   snapshots of that window. Its weight in the final answer is n/(n+shrink_n) where n = settled windows,
   so with little data the answer is the base model.
3. Confidence label from warm-up, feed freshness and distance from 50%.
"""
from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from statistics import NormalDist

import numpy as np

_ND = NormalDist()

SQRT2 = math.sqrt(2.0)
FEATURE_NAMES = ["bias", "logit_base", "logit_market", "mom_60", "mom_180", "mom_300", "tau_frac", "vol_ratio", "prev_outcome"]
DIM = len(FEATURE_NAMES)


def clamp(x: float, lo: float, hi: float) -> float:
    return lo if x < lo else hi if x > hi else x


def norm_cdf(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / SQRT2))


def logit(p: float) -> float:
    p = clamp(p, 1e-6, 1 - 1e-6)
    return math.log(p / (1 - p))


def sigmoid(x: float) -> float:
    if x >= 0:
        return 1.0 / (1.0 + math.exp(-x))
    e = math.exp(x)
    return e / (1.0 + e)


# ----------------------------------------------------------------------------- volatility

class VolEstimator:
    """EWMA realized variance of DOLLAR price changes per second, from a tick stream resampled every
    `step_s` seconds. Units: sigma() is dollars per √second, matching base_probability().

    Two half-lives: slow (15 min) for the level, fast (90 s) for regime changes. `sigma()` blends them
    (fast clamped to 0.5x..3x slow) and clamps to [floor, cap].
    Default floor 0.0005 $/√s ≈ $0.015 per 15 minutes; cap 0.05 $/√s ≈ $1.50 per 15 minutes.
    (WTI in the screenshots moved $0.10-0.40 per 15 minutes ≈ 0.003-0.013 $/√s.)
    """

    def __init__(self, step_s: float = 5.0, slow_halflife_s: float = 900.0, fast_halflife_s: float = 90.0,
                 floor: float = 0.0005, cap: float = 0.05):
        self.step_s = step_s
        self.slow_hl = slow_halflife_s
        self.fast_hl = fast_halflife_s
        self.floor = floor
        self.cap = cap
        self.var_slow: float | None = None
        self.var_fast: float | None = None
        self.n_updates = 0
        self._bucket: int | None = None
        self._bucket_price: float | None = None
        self._last_close: tuple[float, float] | None = None
        self.first_ts: float | None = None
        self.last_ts: float | None = None

    def _update_var(self, var_per_s: float, dt_s: float):
        a_s = 0.5 ** (dt_s / self.slow_hl)
        a_f = 0.5 ** (dt_s / self.fast_hl)
        self.var_slow = var_per_s if self.var_slow is None else a_s * self.var_slow + (1 - a_s) * var_per_s
        self.var_fast = var_per_s if self.var_fast is None else a_f * self.var_fast + (1 - a_f) * var_per_s
        self.n_updates += 1

    def seed_from_closes(self, closes: list[tuple[float, float]]):
        """Seed from (ts, close) pairs at any spacing, e.g. 1-minute candles."""
        prev = None
        for ts, px in closes:
            if px <= 0:
                continue
            if prev is not None and ts > prev[0] and prev[1] > 0:
                dt_s = ts - prev[0]
                r = px - prev[1]
                self._update_var(r * r / dt_s, dt_s)
            prev = (ts, px)
        if prev is not None:
            if self.first_ts is None or closes[0][0] < self.first_ts:
                self.first_ts = closes[0][0]
            self._last_close = prev
            self.last_ts = prev[0] if self.last_ts is None else max(self.last_ts, prev[0])

    def update(self, ts: float, price: float):
        if price <= 0:
            return
        if self.first_ts is None:
            self.first_ts = ts
        self.last_ts = ts
        bucket = int(ts // self.step_s)
        if self._bucket is None:
            self._bucket, self._bucket_price = bucket, price
            return
        if bucket == self._bucket:
            self._bucket_price = price
            return
        close_ts = (self._bucket + 1) * self.step_s
        if self._last_close is not None and self._bucket_price:
            dt_s = close_ts - self._last_close[0]
            if dt_s > 0 and self._last_close[1] > 0:
                r = self._bucket_price - self._last_close[1]
                self._update_var(r * r / dt_s, dt_s)
        self._last_close = (close_ts, self._bucket_price)
        self._bucket, self._bucket_price = bucket, price

    @property
    def seconds_seen(self) -> float:
        if self.first_ts is None or self.last_ts is None:
            return 0.0
        return max(0.0, self.last_ts - self.first_ts)

    def sigma_slow(self) -> float | None:
        return math.sqrt(self.var_slow) if self.var_slow else None

    def sigma_fast(self) -> float | None:
        return math.sqrt(self.var_fast) if self.var_fast else None

    def sigma(self) -> float | None:
        slow = self.sigma_slow()
        if slow is None:
            return None
        fast = self.sigma_fast() or slow
        fast = clamp(fast, 0.5 * slow, 3.0 * slow)
        blended = math.sqrt(0.5 * slow * slow + 0.5 * fast * fast)
        return clamp(blended, self.floor, self.cap)

    def vol_ratio(self) -> float:
        slow, fast = self.sigma_slow(), self.sigma_fast()
        if not slow or not fast:
            return 0.0
        return clamp(math.log(fast / slow), -2.0, 2.0)


# ----------------------------------------------------------------------------- base model

def base_probability(price: float, strike: float, sigma: float, tau_s: float, tie_adj: float = 0.005) -> tuple[float, float]:
    """P(settle Up) and the z-score. Settlement rounds to the cent and a tie pays Up, hence tie_adj."""
    gap = price - strike + tie_adj
    if tau_s <= 0.5:
        return (1.0 if gap >= 0 else 0.0), (50.0 if gap >= 0 else -50.0)
    denom = max(sigma, 1e-9) * math.sqrt(tau_s)
    z = clamp(gap / denom, -50.0, 50.0)
    return norm_cdf(z), z


def momentum_z(price: float, price_ago: float | None, sigma: float, seconds: float) -> float:
    """Price change over `seconds` in units of the expected move (dollar sigma * sqrt(seconds))."""
    if price_ago is None or price_ago <= 0 or sigma <= 0:
        return 0.0
    return clamp((price - price_ago) / (sigma * math.sqrt(seconds)), -6.0, 6.0)


# ----------------------------------------------------------------------------- learner

class Calibrator:
    """Online logistic regression with a prior that reproduces the base model (w = e_logit_base)."""

    def __init__(self, lr: float = 0.05, l2: float = 0.02, shrink_n: float = 100.0, epochs: int = 25):
        self.lr = lr
        self.l2 = l2
        self.shrink_n = shrink_n
        self.epochs = epochs
        self.w0 = np.zeros(DIM)
        self.w0[1] = 1.0
        self.w = self.w0.copy()
        self.n_windows = 0
        self.n_samples = 0

    @property
    def shrink(self) -> float:
        return self.n_windows / (self.n_windows + self.shrink_n)

    def learner_logit(self, x: np.ndarray) -> float:
        return float(np.dot(self.w, x))

    def predict(self, p_base: float, x: np.ndarray) -> tuple[float, float]:
        """(p_final, p_learner)."""
        lb = logit(p_base)
        ll = clamp(self.learner_logit(x), -12.0, 12.0)
        s = self.shrink
        return sigmoid((1 - s) * lb + s * ll), sigmoid(ll)

    def fit_window(self, X: np.ndarray, y: np.ndarray):
        X = np.asarray(X, dtype=float).reshape(-1, DIM)
        y = np.asarray(y, dtype=float).reshape(-1)
        if len(X) == 0:
            return
        for _ in range(self.epochs):
            z = np.clip(X @ self.w, -30, 30)
            p = 1.0 / (1.0 + np.exp(-z))
            grad = X.T @ (p - y) / len(X) + self.l2 * (self.w - self.w0)
            self.w -= self.lr * grad
        self.n_windows += 1
        self.n_samples += len(X)

    def fit_pooled(self, X: np.ndarray, y: np.ndarray, n_windows: int, iters: int = 300, lr: float = 0.1):
        """Refit from the prior on a pooled batch of recent windows (both labels present), so the weights
        reflect many windows instead of random-walking with the latest outcome."""
        X = np.asarray(X, dtype=float).reshape(-1, DIM)
        y = np.asarray(y, dtype=float).reshape(-1)
        if len(X) == 0:
            return
        w = self.w0.copy()
        for _ in range(iters):
            z = np.clip(X @ w, -30, 30)
            p = 1.0 / (1.0 + np.exp(-z))
            grad = X.T @ (p - y) / len(X) + self.l2 * (w - self.w0)
            w -= lr * grad
        self.w = w
        self.n_windows = int(n_windows)
        self.n_samples = len(X)

    def to_json(self) -> str:
        return json.dumps({"w": self.w.tolist(), "n_windows": self.n_windows, "n_samples": self.n_samples,
                           "features": FEATURE_NAMES})

    @classmethod
    def from_json(cls, text: str | None, **kw) -> "Calibrator":
        cal = cls(**kw)
        if not text:
            return cal
        try:
            data = json.loads(text)
            w = np.asarray(data.get("w", []), dtype=float)
            if w.shape == (DIM,):
                cal.w = w
            cal.n_windows = int(data.get("n_windows", 0))
            cal.n_samples = int(data.get("n_samples", 0))
        except (ValueError, TypeError):
            pass
        return cal

    def weights(self) -> dict:
        return {name: round(float(w), 4) for name, w in zip(FEATURE_NAMES, self.w)}


# ----------------------------------------------------------------------------- prediction

@dataclass
class Prediction:
    p_final: float
    p_base: float
    p_learner: float
    p_market: float | None
    z: float
    sigma: float
    sigma_slow: float | None
    sigma_fast: float | None
    tau_s: float
    gap: float
    expected_move: float  # σ√τ in dollars
    confidence: str  # confident | lean | coinflip | warming_up | stale | no_target
    shrink: float
    features: list[float] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    sigma_eff: float = 0.0  # the sigma actually used (basis-widened)
    price_adj: float | None = None  # feed price after the measured basis correction
    basis_signed: float = 0.0
    implied_price: float | None = None  # where Kalshi's odds say the settlement feed is
    disagree: bool = False

    @property
    def p_down(self) -> float:
        return 1.0 - self.p_final

    def as_dict(self) -> dict:
        return {
            "p_up": round(self.p_final, 4), "p_down": round(self.p_down, 4), "p_base": round(self.p_base, 4),
            "p_learner": round(self.p_learner, 4), "p_market": None if self.p_market is None else round(self.p_market, 4),
            "z": round(self.z, 3), "sigma": self.sigma, "sigma_slow": self.sigma_slow, "sigma_fast": self.sigma_fast,
            "tau_s": round(self.tau_s, 1), "gap": round(self.gap, 4), "expected_move": round(self.expected_move, 4),
            "confidence": self.confidence, "shrink": round(self.shrink, 3), "notes": self.notes, "sigma_eff": self.sigma_eff,
            "price_adj": None if self.price_adj is None else round(self.price_adj, 4), "basis_signed": round(self.basis_signed, 4),
            "implied_price": None if self.implied_price is None else round(self.implied_price, 3), "disagree": self.disagree,
        }


class Model:
    def __init__(self, calibrator: Calibrator | None = None, tie_adj: float = 0.005,
                 min_warmup_s: float = 1800.0, confident_margin: float = 0.20, stale_after_s: float = 3.0):
        self.vol = VolEstimator()
        self.cal = calibrator or Calibrator()
        self.tie_adj = tie_adj
        self.min_warmup_s = min_warmup_s
        self.confident_margin = confident_margin
        self.stale_after_s = stale_after_s
        self.prev_outcome = 0.0  # +1 last window Up, -1 Down, 0 unknown
        self.basis_error = 0.0  # measured |feed - settlement| typical size, dollars (widens uncertainty)
        self.basis_signed = 0.0  # measured feed - settlement bias, dollars (shifts the price); 0 until ~3 windows
        self.disagreement_cap = 0.40  # |model - market| above this caps confidence at "lean"

    def features(self, p_base: float, p_market: float | None, price: float, sigma: float, tau_s: float,
                 price_60: float | None, price_180: float | None, price_300: float | None) -> np.ndarray:
        return np.array([
            1.0,
            clamp(logit(p_base), -8.0, 8.0),
            clamp(logit(p_market), -8.0, 8.0) if p_market is not None else 0.0,
            momentum_z(price, price_60, sigma, 60),
            momentum_z(price, price_180, sigma, 180),
            momentum_z(price, price_300, sigma, 300),
            clamp(tau_s / 900.0, 0.0, 1.0),
            self.vol.vol_ratio(),
            self.prev_outcome,
        ])

    def predict(self, price: float, strike: float | None, tau_s: float, p_market: float | None, feed_age_s: float | None,
                price_60: float | None = None, price_180: float | None = None, price_300: float | None = None) -> Prediction:
        sigma = self.vol.sigma()
        notes: list[str] = []
        if sigma is None:
            sigma = self.vol.floor * 10  # placeholder until the first vol estimate exists
            notes.append("no volatility estimate yet")
        # widen uncertainty by the measured feed-vs-settlement basis: treat it as extra variance at expiry
        sigma_eff = sigma
        if self.basis_error > 0 and tau_s > 0.5:
            sigma_eff = math.sqrt(sigma * sigma + (self.basis_error ** 2) / max(tau_s, 1.0))
        if strike is None:
            return Prediction(0.5, 0.5, 0.5, p_market, 0.0, sigma, self.vol.sigma_slow(), self.vol.sigma_fast(), tau_s,
                              0.0, sigma_eff * math.sqrt(max(tau_s, 0)), "no_target", self.cal.shrink, [], ["no target yet"],
                              sigma_eff)
        price_adj = price - self.basis_signed
        p_base, z = base_probability(price_adj, strike, sigma_eff, tau_s, self.tie_adj)
        x = self.features(p_base, p_market, price, sigma, tau_s, price_60, price_180, price_300)
        p_final, p_learner = self.cal.predict(p_base, x)
        warm = self.vol.seconds_seen
        if warm < self.min_warmup_s:
            conf = "warming_up"
            notes.append(f"warming up: {warm / 60:.0f} of {self.min_warmup_s / 60:.0f} min of price history")
        elif feed_age_s is None or feed_age_s > self.stale_after_s:
            conf = "stale"
            notes.append("price feed is stale")
        elif abs(p_final - 0.5) >= self.confident_margin:
            conf = "confident"
        elif abs(p_final - 0.5) >= 0.08:
            conf = "lean"
        else:
            conf = "coinflip"
        implied = None
        disagree = False
        if p_market is not None and tau_s > 0.5:
            # invert the base model on the market's probability: where Kalshi's odds say the Pyth price is
            pm = clamp(p_market, 0.02, 0.98)
            implied = strike - self.tie_adj + _ND.inv_cdf(pm) * sigma_eff * math.sqrt(tau_s)
            if abs(p_final - p_market) > self.disagreement_cap:
                disagree = True
                notes.append(f"model ({round(p_final * 100)}% Up) and market ({round(p_market * 100)}% Up) disagree a lot: "
                             f"Kalshi's odds imply the Pyth price is near ${implied:.2f}, our feed says ${price:.2f}; "
                             f"trust the market's price for exits")
                if conf == "confident":
                    conf = "lean"
        if abs(self.basis_signed) >= 0.005:
            notes.append(f"feed adjusted by {-self.basis_signed * 100:+.1f}¢ (measured vs recent settlements)")
        return Prediction(p_final, p_base, p_learner, p_market, z, sigma, self.vol.sigma_slow(), self.vol.sigma_fast(),
                          tau_s, price_adj - strike, sigma_eff * math.sqrt(max(tau_s, 0)), conf, self.cal.shrink,
                          [float(v) for v in x], notes, sigma_eff, price_adj, self.basis_signed, implied, disagree)
