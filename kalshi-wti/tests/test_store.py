import json
import pathlib

from wti15m.decision import Signal
from wti15m.kalshi import Market
from wti15m.model import Prediction
from wti15m.store import Store

FIX = pathlib.Path(__file__).resolve().parents[1] / "fixtures"


def pred(p=0.6):
    return Prediction(p, p, p, 0.5, 0.3, 0.005, 0.005, 0.005, 600, 0.02, 0.12, "lean", 0.0, [1.0, 0.4, 0.0, 0, 0, 0, 0.6, 0, 0], [])


def test_store_roundtrip(tmp_path):
    st = Store(str(tmp_path / "t.db"))
    m = Market.from_api(json.loads((FIX / "synthetic-markets-open.json").read_text())["markets"][0])
    st.upsert_window(m)
    st.add_tick(1.0, "sim", 90.0)
    st.add_quote(1.0, m)
    sid = st.add_snapshot(1.0, m.ticker, 600, 90.19, 90.21, pred())
    assert sid == 1
    st.add_signal(1.0, m.ticker, Signal("WAIT", "DOWN", None, 0, None, 0.55, "WAIT", [], ["coin_flip"], "lean"), pred(), 600)
    tid = st.open_paper_trade(m.ticker, "DOWN", 1.0, 0.52, 5, 0.09, "lean", 0.6)
    st.close_paper_trade(tid, 2.0, 1.0, 0.0, "settle")
    st.label_snapshots(m.ticker, 0)
    st.settle_window(m.ticker, "no", 90.16, 0.01)
    X, y, meta = st.training_rows()
    assert X.shape == (1, 9) and y.tolist() == [0.0]
    w = st.window(m.ticker)
    assert w["result"] == "no" and abs(w["feed_error"] - 0.01) < 1e-9
    recent = st.recent_windows()
    assert len(recent) == 1 and abs(recent[0]["paper_pnl"] - ((1.0 - 0.52) * 5 - 0.09)) < 1e-9
    stats = st.stats()
    assert stats["settled_windows"] == 1 and stats["paper"]["trades"] == 1 and stats["feed_error"]["n"] == 1
    pid = st.open_position(m.ticker, "DOWN", 3, 0.5, 1.0)
    assert st.get_open_position()["id"] == pid
    st.close_position(pid, 2.0, 1.0, 1.5)
    assert st.get_open_position() is None
    st.set_state("calibrator", "{}")
    assert st.get_state("calibrator") == "{}"
    st.close()
