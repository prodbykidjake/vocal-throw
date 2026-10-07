"""Offline evaluation over recorded windows: Brier scores (model vs base vs market) and an optional
walk-forward retrain of the calibrator from scratch."""
from __future__ import annotations

import datetime as dt

import numpy as np

from .model import Calibrator, sigmoid
from .store import Store

BUCKETS = {"10min": (540, 660), "5min": (240, 360), "2min": (90, 150), "1min": (30, 89)}


def run(store: Store, since: str | None = None, retrain: bool = False, save: bool = False) -> dict:
    since_ts = None
    if since:
        since_ts = dt.datetime.fromisoformat(since).replace(tzinfo=dt.timezone.utc).timestamp()
    tickers = []
    for row in reversed(store.recent_windows(5000)):
        if row.get("result") in ("yes", "no") and (since_ts is None or (row.get("close_time") or 0) >= since_ts):
            tickers.append(row["ticker"])
    cal = Calibrator()
    rows_out = []
    for ticker in tickers:
        X, y, meta = store.training_rows(ticker)
        if len(X) == 0:
            continue
        for x, label, m in zip(X, y, meta):
            p_base = sigmoid(float(x[1]))
            if retrain:
                p_final, _ = cal.predict(p_base, np.asarray(x))
            else:
                p_final = m["p_final"]
            rows_out.append({"ticker": ticker, "seconds_left": m["seconds_left"], "p_final": p_final, "p_base": p_base,
                             "p_market": m["p_market"], "label": label})
        if retrain and len(X) >= 3:
            cal.fit_window(X, y)
    report: dict = {"windows": len(tickers), "snapshots": len(rows_out), "retrained": retrain, "brier": {}}
    for name, (lo, hi) in BUCKETS.items():
        sel = [r for r in rows_out if lo <= r["seconds_left"] <= hi]
        if not sel:
            continue
        def brier(key):
            vals = [(r[key] - r["label"]) ** 2 for r in sel if r[key] is not None]
            return round(sum(vals) / len(vals), 4) if vals else None
        report["brier"][name] = {"n": len(sel), "model": brier("p_final"), "base": brier("p_base"), "market": brier("p_market")}
    if retrain:
        report["calibrator"] = {"n_windows": cal.n_windows, "weights": cal.weights()}
        if save:
            store.set_state("calibrator", cal.to_json())
            report["saved"] = True
    return report


def format_report(report: dict) -> str:
    lines = [f"windows: {report['windows']}   snapshots: {report['snapshots']}   retrained: {report['retrained']}", "",
             "Brier score (lower is better; 0.25 = coin flip)", f"{'time left':>10} {'n':>6} {'model':>8} {'base':>8} {'market':>8}"]
    for name, b in report["brier"].items():
        lines.append(f"{name:>10} {b['n']:>6} {_f(b['model']):>8} {_f(b['base']):>8} {_f(b['market']):>8}")
    if not report["brier"]:
        lines.append("  (no labeled snapshots yet — run the app through some settled windows first)")
    if "calibrator" in report:
        lines.append("")
        lines.append(f"calibrator after walk-forward retrain: {report['calibrator']}")
        if report.get("saved"):
            lines.append("saved as the live calibrator")
    return "\n".join(lines)


def _f(x):
    return "--" if x is None else f"{x:.4f}"
