import os
import sqlite3
import zipfile

from wti15m.export import export
from wti15m.store import Store


def test_export_keeps_windows_and_calls_trims_old_raw_data_and_leaves_the_original_alone(tmp_path):
    db = tmp_path / "wti15m.db"
    st = Store(str(db))
    now = 10 * 86400.0
    for ts in (now - 9 * 86400, now - 2 * 86400, now - 60):
        st.add_tick(ts, "kalshi-live", 90.0)
    st.add_call("OLD", "UP", 0.75, 0.74, now - 9 * 86400, 500, "confident", ask=0.75)
    st.conn.commit()
    out = export(str(db), days=7, out=str(tmp_path / "x.zip"), now=now)
    with zipfile.ZipFile(out) as z:
        z.extractall(tmp_path / "unz")
    c = sqlite3.connect(tmp_path / "unz" / "wti15m.db")
    assert c.execute("SELECT COUNT(*) FROM ticks").fetchone()[0] == 2  # the 9-day-old tick is trimmed
    assert c.execute("SELECT COUNT(*) FROM calls").fetchone()[0] == 1  # every call is kept
    assert sqlite3.connect(db).execute("SELECT COUNT(*) FROM ticks").fetchone()[0] == 3  # original untouched
    assert os.path.getsize(out) > 0
