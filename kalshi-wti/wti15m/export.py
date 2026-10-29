"""`python -m wti15m export`: a compact, zipped copy of the database to send for analysis.

Copies the live database safely (SQLite's backup API works while `serve` is running), keeps every window, call and
setting, trims the bulky second-by-second tables (ticks, quotes, snapshots, signals) to the last N days, compacts the
copy and zips it. The original database is never changed.
"""
from __future__ import annotations

import datetime as dt
import os
import sqlite3
import tempfile
import time
import zipfile

BULKY = ("ticks", "quotes", "snapshots", "signals")


def export(db_path: str, days: float | None = 7.0, out: str | None = None, now: float | None = None) -> str:
    if not os.path.isfile(db_path):
        raise FileNotFoundError(f"no database at {db_path}")
    now = time.time() if now is None else now
    out = out or os.path.join(os.path.dirname(db_path) or ".",
                              f"export-{dt.datetime.fromtimestamp(now).strftime('%Y%m%d-%H%M')}.zip")
    with tempfile.TemporaryDirectory() as tmp:
        copy = os.path.join(tmp, os.path.basename(db_path))
        src = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        dst = sqlite3.connect(copy)
        try:
            src.backup(dst)
        finally:
            src.close()
        try:
            if days is not None and days > 0:
                cutoff = now - days * 86400
                have = {r[0] for r in dst.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                for table in BULKY:
                    if table in have:
                        dst.execute(f"DELETE FROM {table} WHERE ts < ?", (cutoff,))
            dst.commit()
            dst.execute("VACUUM")
        finally:
            dst.close()
        with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            z.write(copy, arcname=os.path.basename(db_path))
    return out
