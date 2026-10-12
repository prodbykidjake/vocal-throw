"""Quick scalps, rebuilt (v11): cheap contracts, a few cents, within two minutes, judged on what a scalper lives by.

The v8 card asked "can the bid touch +3¢ at some point?" and called a side whenever that was 35% likely. A scalp is
not won by touching the target once; it is won by reaching the target BEFORE the price falls to the point where you
cut it, and only if that beats the fees. And a random walk started from Kalshi's own price has no edge at all.

This card asks the scalper's question, per cheap side:

  * simulate the next `quick_horizon_s` seconds of the underlying from the settlement feed's price (the model's
    view), turn every path into the side's fair value, and let Kalshi's current premium or discount to that fair
    value fade out over `quick_converge_s` seconds (the market catching up with the price);
  * score each path by FIRST PASSAGE: the bid reaches the target first (win, the resting sell fills), the stop first
    (loss, sold at the bid), or neither (sold at the bid at the horizon);
  * expected dollars on a `quick_dollars` stake after Kalshi's fees: taker fee on the buy and on stop/timeout sells,
    none on the target sell when the series has no maker fee (a resting limit order is a maker order).

A side is CALLED only when the win chance is at least `quick_min_win`, the expected value after fees is positive by
`quick_min_edge` of the stake, the model rates the side at least as high as Kalshi does, the window holds no oil
report, and a confident settlement call does not point the other way. Every call is scored on paper the same way and
kept as a record (Wins | Losses and dollars), so the card shows whether it works before anyone trusts it.
"""
from __future__ import annotations

import math
import time

import numpy as np

from .config import TradingCfg
from .decision import Quotes, cents, p_for_side, pct5
from .fees import FeeSchedule, maker_fee, taker_fee
from .model import Prediction, clamp
from .paths import _normal_block, fair_price, norm_cdf

SEED = 11


def bid_paths(side: str, s0: float, strike: float, sigma: float, seconds_left: float, settle_lag_s: float,
              tie_adj: float, bid0: float, offset: float, horizon_s: float, converge_s: float, n_paths: int = 2000,
              step_s: float = 2.0) -> tuple | None:
    """Simulated bid paths for `side` over the next horizon_s seconds: (bid matrix paths x steps, times, fair now, gap now)."""
    n = int(min(horizon_s, seconds_left - 5.0) // step_s)
    if n < 2 or sigma <= 0 or s0 <= 0:
        return None
    half = max(1, n_paths // 2)
    times = step_s * np.arange(1, n + 1)
    dw = _normal_block(SEED, half, n) * (sigma * math.sqrt(step_s))
    dw = np.vstack([dw, -dw])
    s = s0 + np.cumsum(dw, axis=1)
    tau = np.maximum(seconds_left - times, 0.0) + settle_lag_s
    f = norm_cdf((s - strike + tie_adj) / (sigma * np.sqrt(np.maximum(tau, 1.0))))
    if side == "DOWN":
        f = 1.0 - f
    f0 = fair_price(side, s0, strike, sigma, seconds_left + settle_lag_s, tie_adj)
    gap0 = (bid0 + offset) - f0  # Kalshi's mid minus the model's fair value now; it fades as the market catches up
    fade = np.exp(-times / max(converge_s, 1.0)) if converge_s > 0 else np.ones_like(times)
    return f - offset + gap0 * fade, times, f0, gap0


def _first_hit(mask: np.ndarray, big: int) -> np.ndarray:
    return np.where(mask.any(axis=1), mask.argmax(axis=1), big)


def first_passage(side: str, s0: float, strike: float, sigma: float, seconds_left: float, settle_lag_s: float,
                  tie_adj: float, bid0: float, offset: float, target: float, stop: float | None, horizon_s: float,
                  converge_s: float, n_paths: int = 2000, step_s: float = 2.0) -> dict | None:
    """Win / loss / timeout chances for buying `side` now, selling at `target`, cutting at `stop` (None = no stop)."""
    sim = bid_paths(side, s0, strike, sigma, seconds_left, settle_lag_s, tie_adj, bid0, offset, horizon_s, converge_s, n_paths, step_s)
    if sim is None:
        return None
    bid, times, f0, gap0 = sim
    out = _score(bid, times, target, stop)
    out.update({"fair": f0, "gap": gap0, "horizon_s": float(times[-1])})
    return out


def _score(bid: np.ndarray, times: np.ndarray, target: float, stop: float | None, hit_t=None, hit_s=None) -> dict:
    big = bid.shape[1] + 10
    hit_t = _first_hit(bid >= target, big) if hit_t is None else hit_t
    hit_s = (np.full(bid.shape[0], big) if stop is None else _first_hit(bid <= stop, big)) if hit_s is None else hit_s
    win = hit_t < hit_s
    loss = (hit_s <= hit_t) & (hit_s < big)
    none = ~(win | loss)
    exit_h = bid[:, -1]
    n = bid.shape[1]
    return {"p_win": float(win.mean()), "p_loss": float(loss.mean()), "p_none": float(none.mean()),
            "exit_none": float(exit_h[none].mean()) if none.any() else float(exit_h.mean()),
            "t_win": float(np.median(times[np.minimum(hit_t[win], n - 1)])) if win.any() else None}


class QuickScalper:
    def __init__(self, cfg: TradingCfg, store):
        self.cfg = cfg
        self.store = store
        self.call: dict | None = None
        self.options: list[dict] = []
        self.since = 0.0
        self.note = ""
        self._ticker: str | None = None
        self._last_bid: dict[str, float] = {}
        self._last_log = 0.0
        self.events: list[str] = []  # log lines produced this second (the engine drains them)
        self.fees = FeeSchedule()

    # ------------------------------------------------------------------ evaluation
    def evaluate(self, pred: Prediction, quotes: Quotes, price: float, strike: float | None, seconds_left: float | None,
                 fees: FeeSchedule, report: bool = False, settle_call: dict | None = None) -> list[dict]:
        cfg = self.cfg
        out: list[dict] = []
        if (strike is None or seconds_left is None or seconds_left <= cfg.quick_min_left_s
                or pred.confidence in ("stale", "warming_up", "no_target")):
            return out
        s0 = pred.price_adj if pred.price_adj is not None else price
        sigma = pred.sigma_eff or pred.sigma
        for side in ("UP", "DOWN"):
            ask, bid = quotes.ask(side), quotes.bid(side)
            if ask is None or bid is None or ask < cfg.quick_min_ask or ask > cfg.quick_max_ask or ask <= bid - 1e-9:
                continue
            offset = clamp((ask - bid) / 2.0, 0.0, 0.05)
            sim = bid_paths(side, s0, strike, sigma, seconds_left, cfg.settle_lag_s, cfg.tie_adj, bid, offset,
                            cfg.quick_horizon_s, cfg.quick_converge_s, cfg.mc_paths)
            if sim is None:
                continue
            paths, times, fair, gap = sim
            limit = ask  # the buy takes the ask
            n = cfg.quick_dollars / limit
            fee_in = taker_fee(limit, n, fees)
            big = paths.shape[1] + 10
            targets = sorted({round(limit + c / 100.0, 3) for c in cfg.quick_targets_cents if limit + c / 100.0 <= 0.95})
            stops = [None] + sorted({round(limit - c / 100.0, 3) for c in cfg.quick_stops_cents if c > 0 and limit - c / 100.0 >= 0.01},
                                    reverse=True)
            t_hits = {t: _first_hit(paths >= t, big) for t in targets}
            s_hits = {st: (np.full(paths.shape[0], big) if st is None else _first_hit(paths <= st, big)) for st in stops}
            best_any = best_ok = None
            for t in targets:
                for st in stops:
                    sc = _score(paths, times, t, st, t_hits[t], s_hits[st])
                    win_pnl = n * (t - limit) - fee_in - maker_fee(t, n, fees)
                    stop_px = max(0.0, (st if st is not None else 0.0) - 0.01)  # a stop sells into the bid, often a cent through it
                    loss_pnl = n * (stop_px - limit) - fee_in - taker_fee(stop_px, n, fees) if st is not None else 0.0
                    none_pnl = n * (sc["exit_none"] - limit) - fee_in - taker_fee(sc["exit_none"], n, fees)
                    ev = sc["p_win"] * win_pnl + sc["p_loss"] * loss_pnl + sc["p_none"] * none_pnl
                    cand = {"target": t, "stop": st, "ev": ev, "win_pnl": win_pnl, "loss_pnl": loss_pnl, "none_pnl": none_pnl, **sc}
                    if best_any is None or ev > best_any["ev"]:
                        best_any = cand
                    if sc["p_win"] >= cfg.quick_min_win and (best_ok is None or ev > best_ok["ev"]):
                        best_ok = cand
            pick = best_ok or best_any
            if pick is None:
                continue
            p_side = p_for_side(pred, side)
            pm = None if pred.p_market is None else (pred.p_market if side == "UP" else 1.0 - pred.p_market)
            why_not = []
            if best_ok is None:
                why_not.append(f"no target/stop wins {pct5(cfg.quick_min_win)} of the time (best {pct5(best_any['p_win'])})")
            if pick["ev"] < cfg.quick_min_edge * cfg.quick_dollars:
                why_not.append(f"expected {'+' if pick['ev'] >= 0 else '-'}${abs(pick['ev']):.2f} after fees")
            if pm is not None and p_side < pm - 1e-9:
                why_not.append(f"model {pct5(p_side)} under Kalshi's {pct5(pm)}")
            if report:
                why_not.append("oil report in this window")
            if settle_call and settle_call.get("kind") == "confident" and settle_call.get("side") != side:
                why_not.append(f"the settlement call says {settle_call['side']}")
            out.append({"side": side, "ask": ask, "limit": round(limit, 3), "target": round(pick["target"], 3),
                        "stop": None if pick["stop"] is None else round(pick["stop"], 3),
                        "p_win": round(pick["p_win"], 3), "p_loss": round(pick["p_loss"], 3), "p_none": round(pick["p_none"], 3),
                        "t_win": None if pick["t_win"] is None else round(pick["t_win"]), "horizon_s": round(float(times[-1])),
                        "ev": round(pick["ev"], 2), "win_pnl": round(pick["win_pnl"], 2),
                        "loss_pnl": round(pick["loss_pnl"] if pick["stop"] is not None else pick["none_pnl"], 2),
                        "amount": cfg.quick_dollars, "shares": round(n, 1), "p_side": round(p_side, 3),
                        "p_market": None if pm is None else round(pm, 3), "fair": round(fair, 3), "gap": round(gap, 3),
                        "maker_free": maker_fee(pick["target"], n, fees) == 0.0,
                        "call": not why_not, "why_not": "; ".join(why_not)})
        out.sort(key=lambda o: (o["call"], o["ev"]), reverse=True)
        return out

    # ------------------------------------------------------------------ per second
    def step(self, now: float, ticker: str | None, pred: Prediction | None, quotes: Quotes, price: float | None,
             strike, seconds_left, fees: FeeSchedule, report: bool, settle_call: dict | None, show: bool):
        self.events = []
        self.fees = fees
        if ticker != self._ticker:
            for side, bid in self._last_bid.items():  # window over: score what was open at its last bid
                self._close_open(now, self._ticker, side, bid, "window ended")
            self._ticker, self._last_bid = ticker, {}
            self.call, self.since = None, now
        if ticker is None:
            self.options, self.call = [], None
            return
        for side in ("UP", "DOWN"):
            b = quotes.bid(side)
            if b is not None:
                self._last_bid[side] = b
        self._score(now, ticker)
        if not show or pred is None or not self.cfg.quick_scalps:
            self.options, self.call = [], None
            self.note = "paused while you hold a position" if not show else ""
            return
        self.options = self.evaluate(pred, quotes, price if price is not None else strike, strike, seconds_left, fees, report, settle_call)
        best = next((o for o in self.options if o["call"]), None)
        cur = self.call
        if cur is not None and now - self.since < 5.0:
            live = next((o for o in self.options if o["side"] == cur["side"]), None)
            if live is not None and live["p_win"] >= self.cfg.quick_min_win - 0.05 and abs(live["ask"] - cur["ask"]) <= 0.01:
                self.call = live
                return
        if best is not None and (cur is None or best["side"] != cur["side"]):
            self.since = now
            self._open(now, ticker, best)
        self.call = best

    def _open(self, now: float, ticker: str, o: dict):
        """A new call is scored on paper from the ask it was called at (one open paper scalp per side at a time)."""
        if self.store.quick_open(ticker, o["side"]) is not None:
            return
        self.store.add_quick(ticker, o["side"], now, o["limit"], o["target"], o["stop"] or 0.0, now + o["horizon_s"], o["p_win"], o["ev"],
                             o["amount"])
        if now - self._last_log >= 20:
            self._last_log = now
            self.events.append(f"quick scalp: BUY {o['side']} @ {cents(o['limit'])} · sell {cents(o['target'])} · "
                               f"{'stop ' + cents(o['stop']) if o['stop'] else 'no stop, out at ' + str(round(o['horizon_s'])) + ' s'} · "
                               f"{pct5(o['p_win'])} to hit the target first · EV {'+' if o['ev'] >= 0 else '-'}${abs(o['ev']):.2f}")

    def _score(self, now: float, ticker: str):
        for row in self.store.quick_open_rows(ticker):
            bid = self._last_bid.get(row["side"])
            if bid is None:
                continue
            if bid >= row["target"]:
                self._close(now, row, row["target"], "target", maker=True)
            elif row["stop"] and bid <= row["stop"]:
                self._close(now, row, bid, "stop")
            elif now >= row["horizon_end"]:
                self._close(now, row, bid, "time")

    def _close_open(self, now: float, ticker: str | None, side: str, bid: float, reason: str):
        if ticker is None:
            return
        row = self.store.quick_open(ticker, side)
        if row is not None:
            self._close(now, row, bid, reason)

    def _close(self, now: float, row: dict, exit_px: float, reason: str, maker: bool = False):
        fees = self.fees
        n = row["amount"] / row["ask"]
        pnl = n * (exit_px - row["ask"]) - taker_fee(row["ask"], n, fees) - (maker_fee(exit_px, n, fees) if maker else taker_fee(exit_px, n, fees))
        win = pnl > 0
        self.store.close_quick(row["id"], now, exit_px, round(pnl, 2), "win" if win else "loss", reason)
        rec = self.store.quick_record()
        self.events.append(f"quick scalp {row['side']} @ {cents(row['ask'])}: {'WIN' if win else 'LOSS'} {'+' if pnl >= 0 else '-'}${abs(pnl):.2f} "
                           f"({'hit ' + cents(row['target']) if reason == 'target' else reason}) · record {rec['wins']}-{rec['losses']}")

    def state(self, now: float | None = None) -> dict:
        now = time.time() if now is None else now
        open_rows = self.store.quick_open_rows(self._ticker) if self._ticker else []
        return {"call": self.call, "options": self.options, "note": self.note, "record": self.store.quick_record(),
                "open": [{"side": r["side"], "ask": r["ask"], "target": r["target"], "stop": r["stop"],
                          "left_s": max(0, round(r["horizon_end"] - now)), "bid": self._last_bid.get(r["side"])} for r in open_rows],
                "rules": {"min_win": self.cfg.quick_min_win, "horizon_s": self.cfg.quick_horizon_s, "dollars": self.cfg.quick_dollars}}

