"""Per-coin settlement-call rules (the commodities board's: 85% bar, 5:00 study, no lean call, until each coin is tuned on
its history) and the strictness switch (Standard / Strict / Very strict), all three run on the same windows so their
records can be compared live."""
import asyncio

from cx15m import config, tuning
from cx15m.board import Board
from cx15m.calls import CallTracker, blend_up
from cx15m.config import Config, TradingCfg
from cx15m.model import Prediction
from cx15m.store import Store


def pred(p_up, p_market=None, conf="lean"):
    return Prediction(p_up, p_up, p_up, p_market, 0.0, 0.004, None, None, 600, 0.0, 0.1, conf, 0.0)


def tuned(key, **kw):
    t = tuning.apply(TradingCfg(), tuning.for_market(key))
    for k, v in kw.items():
        setattr(t, k, v)
    return CallTracker(t, Store(":memory:"))


def test_each_coin_has_its_rules_and_config_can_override_them():
    btc, doge = tuning.for_market("btc"), tuning.for_market("doge")
    assert (btc.call_prob, btc.study_s) == (0.92, 180.0) and (doge.call_prob, doge.study_s) == (0.85, 300.0)
    assert set(tuning.TUNING) == set(tuning.MODE_BARS) == {"btc", "eth", "sol", "xrp", "doge", "hype", "near", "zec", "bnb"}
    assert tuning.for_market("newcoin") == tuning.DEFAULT and tuning.DEFAULT.call_prob == 0.85 and not tuning.DEFAULT.late_call
    assert tuning.DEFAULT.study_s == 300 and tuning.DEFAULT.hold_s == 10 and tuning.DEFAULT.blend_market == 0.0
    o = tuning.for_market("btc", {"btc": {"call_prob": 0.9, "bogus": 1}})
    assert o.call_prob == 0.9 and o.study_s == 180
    t = tuning.apply(TradingCfg(), tuning.for_market("eth"))
    assert (t.call_prob, t.study_s, t.late_call, t.call_hold_s, t.rules_tag) == (0.87, 300.0, False, 10.0, "v1")
    assert tuning.apply(TradingCfg(), tuning.for_market("eth"), window_s=90).study_s == 30  # the demo's short windows



def test_config_reads_tuning_sections(tmp_path):
    p = tmp_path / "config.toml"
    p.write_text("[tuning]\nenabled = false\n[tuning.btc]\ncall_prob = 0.9\n")
    cfg = config.load(str(p))
    assert not tuning.enabled(cfg.tuning) and tuning.for_market("btc", cfg.tuning).call_prob == 0.9
    assert tuning.enabled(Config().tuning)


def test_every_desk_gets_its_own_rules_and_the_shared_settings_stay_untouched(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.tuning = {"sol": {"call_prob": 0.9}}
    board = Board(cfg)
    board.sim = None
    btc, sol = board._desk_cfg("btc").trading, board._desk_cfg("sol").trading
    assert btc.call_prob == 0.92 and sol.call_prob == 0.9 and btc is not sol
    assert cfg.trading.call_prob == 0.72 and cfg.trading.late_call  # the original rules stay the shared default
    cfg.tuning = {"enabled": False}
    assert board._desk_cfg("btc").trading.call_prob == 0.72



def test_no_lean_call_at_the_deadline_but_a_sure_call_still_counts_until_the_last_minute():
    ct = tuned("doge")
    t0 = 1000.0
    assert ct.step(t0 - 700, "W1", pred(0.80, 0.78), 880, 20, 300) is None  # seen from the start of the window
    # past the study, 80% is not enough at Dogecoin's 85%; the old 2:00 lean call does not happen
    for i in range(120):
        assert ct.step(t0 + i, "W1", pred(0.80, 0.78), 180 - i, 720 + i, 300) is None
    assert ct.watch["deadline_in_s"] is None and ct.watch["closes_in_s"] is not None
    # with 1:30 left it gets sure enough and holds 10 s: a confident call
    made = None
    for i in range(12):
        made = ct.step(t0 + 200 + i, "W1", pred(0.90, 0.85), 90 - i, 810 + i, 300) or made
    assert made and made["kind"] == "confident" and made["side"] == "UP"
    # another window that never gets there: no call at all, and the card says why
    ct2 = tuned("doge")
    for i in range(900):
        assert ct2.step(t0 + i, "W2", pred(0.80, 0.78), 900 - i, i, 300) is None
    assert ct2.watch["status"] == "no call this window (never 85% sure for 10s)"


def test_a_coin_can_call_on_its_blend_with_kalshi():
    assert abs(blend_up(0.8, 0.8, 0.65) - 0.8) < 1e-9 and blend_up(0.8, None, 0.65) == 0.8
    b = blend_up(0.80, 0.92, 0.65)  # log-odds: 0.35 * logit(.80) + 0.65 * logit(.92)
    assert 0.88 < b < 0.90
    ct = CallTracker(tuning.apply(TradingCfg(), tuning.for_market("doge", {"doge": {"blend_market": 0.65}})), Store(":memory:"))
    made = None
    for i in range(12):  # the engine alone says 80% (below 85%); with Kalshi at 92% the blend clears it
        made = ct.step(1000 + i, "G1", pred(0.80, 0.92), 500 - i, 400 + i, 300) or made
    assert made and abs(made["p_side"] - b) < 1e-6
    assert abs(ct.meter["p_up"] - round(b, 3)) < 1e-9 and ct.meter["model_up"] == 0.8


def test_the_rules_record_counts_only_calls_made_under_the_current_rules():
    store = Store(":memory:")
    store.add_call("OLD", "UP", 0.75, 0.7, 10.0, 500, "late")
    store.settle_call("OLD", "DOWN", 20.0)
    t = tuning.apply(TradingCfg(), tuning.for_market("btc"))
    ct = CallTracker(t, store)
    made = None
    for i in range(12):  # a call made under the v1 rules carries its tag
        made = ct.step(1e9 + i, "NEW", pred(0.92, 0.9), 500 - i, 400 + i, 300) or made
    assert made and made["rules"] == "v1"
    store.settle_call("NEW", "UP", 1e9 + 600)
    # tuning switched off for a while: its calls (the original rules, a lean call here) never count toward "since v1"
    ct_off = CallTracker(TradingCfg(), store)
    for i in range(130):
        ct_off.step(2e9 + i, "OFF", pred(0.6, 0.6), 900 - i if i == 0 else 125 - i, 300 + i, 180)
    store.settle_call("OFF", "DOWN", 2e9 + 900)
    s = CallTracker(t, store).state()
    assert (s["record"]["wins"], s["record"]["losses"]) == (1, 2)
    assert (s["record_rules"]["wins"], s["record_rules"]["losses"]) == (1, 0)
    assert CallTracker(t, store).rules_since == ct.rules_since  # remembered across restarts
    assert "record_rules" not in CallTracker(TradingCfg(), Store(":memory:")).state()  # untagged: no second record


def test_no_call_says_why_when_kalshi_held_it_back():
    for p_market, disagree, why in ((0.52, True, "engine and Kalshi too far apart"), (None, False, "no fresh Kalshi odds")):
        ct = tuned("btc")
        for i in range(900):
            p = Prediction(0.95, 0.95, 0.95, p_market, 0.0, 0.004, None, None, 600, 0.0, 0.1, "lean", 0.0, disagree=disagree)
            assert ct.step(1000 + i, "W", p, 900 - i, i, 300) is None
        assert ct.watch["status"] == f"no call this window ({why})"


def test_overrides_are_read_forgivingly_and_bad_values_are_ignored(caplog):
    o = {"enabled": "false", "BTC": {"call_prob": 90, "late_call": "true"}, "eth": {"call_prob": 1.7, "blend_market": "x"},
         "bitcoin": {"call_prob": 0.9}}
    assert not tuning.enabled(o)
    btc = tuning.for_market("btc", o)
    assert btc.call_prob == 0.9 and btc.late_call is True  # [tuning.BTC] counts; 90 means 90%
    eth = tuning.for_market("eth", o)
    assert eth.call_prob == 0.87 and eth.blend_market == 0.0  # out of range / not a number: Ethereum's own kept
    assert tuning.check(o, ["btc", "eth", "sol"]) == ["bitcoin"]
    assert "matches no market" in caplog.text



def test_desks_take_the_boards_report_settings(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.board.extra_events = {"btc": ["2026-11-06 08:30 Jobs report"]}
    board = Board(cfg, sim={"window_s": 900, "warmup_minutes": 10})
    asyncio.run(board.build())
    names = [name for _, name in board.desk("btc")._extra]
    assert "Jobs report" in names and "CPI" in names and "Jobs report" not in [n for _, n in board.desk("eth")._extra]



def test_no_call_in_a_report_or_maintenance_window(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.trading.min_warmup_minutes = 1
    cfg.board.markets = ["btc", "eth"]
    board = Board(cfg, sim={"window_s": 120, "warmup_minutes": 10})

    async def run():
        await board.build()
        board.desk("btc")._window_events = lambda open_ts, close_ts: [(open_ts + 30, "Test report")]
        board.desk("eth")._window_events = lambda open_ts, close_ts: [(open_ts + 5, "Kalshi maintenance")]
        stop = asyncio.Event()
        task = asyncio.create_task(board.run(stop))
        await asyncio.sleep(3)
        stop.set()
        await task

    asyncio.run(run())
    btc, eth = (m["call"] for m in board.state["markets"])
    assert btc["watch"]["status"].startswith("no call this window (Test report") and btc["call"] is None and btc["meter"]
    assert eth["watch"]["status"] == "no call this window (Kalshi's weekly maintenance, Thursday 3:00-5:00 AM ET)"



def test_strict_modes_raise_the_bar():
    for key in ("btc", "eth", "sol", "xrp", "doge", "hype", "near", "zec", "bnb", "newcoin"):
        std, st, vs = (tuning.for_market(key, mode=m) for m in tuning.MODES)
        assert std.call_prob < st.call_prob < vs.call_prob <= 0.99
        assert st.hold_s == std.hold_s == vs.hold_s == 10 and st.blend_market == std.blend_market
        assert 180 <= vs.study_s <= st.study_s <= 300 or key == "newcoin"
    btc = tuning.for_market("btc", mode="very_strict")
    assert (btc.call_prob, btc.study_s) == (0.99, 180.0)  # (bar, study) per mode
    xrp = tuning.for_market("xrp", mode="strict")
    assert (xrp.call_prob, xrp.study_s) == (0.93, 240.0)
    assert tuning.for_market("newcoin", mode="very_strict").call_prob == tuning.MODE_DEFAULT_BARS["very_strict"] == 0.97
    assert tuning.mode_of("Very strict") == "very_strict" and tuning.mode_of("bogus") == "standard"
    assert (tuning.tag("standard"), tuning.tag("strict")) == ("v1", "strict")
    # a value set in config.toml still wins over the mode's bar
    assert tuning.for_market("btc", {"btc": {"call_prob": 0.9}}, mode="strict").call_prob == 0.9



def test_the_switch_changes_every_card_and_is_remembered(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.trading.min_warmup_minutes = 1
    board = Board(cfg, sim={"window_s": 900, "warmup_minutes": 10})
    asyncio.run(board.build())
    assert board.mode == "standard" and board.desk("btc").calls.cfg.call_prob == 0.92
    board.set_mode("strict")
    btc, doge = board.desk("btc"), board.desk("doge")
    assert btc.calls.cfg.call_prob == 0.97 and btc.calls.cfg.rules_tag == "strict" and btc.calls.cfg.study_s == 180
    assert doge.calls.cfg.call_prob == 0.92 and doge.calls.rules_since is not None and doge.calls.cfg.study_s == 300
    assert Board(cfg).mode == "strict"  # remembered across restarts
    try:
        board.set_mode("reckless")
        raise AssertionError("a bad mode must be refused")
    except ValueError:
        pass
    # every market runs all three modes on the same windows
    assert all(set(v) == set(tuning.MODES) for v in board.shadows.values()) and len(board.shadows) == len(board.desks)


def test_each_mode_records_its_own_calls_on_the_same_windows(tmp_path):
    import datetime as dt
    import types

    from cx15m import clock
    from cx15m.shadow import ShadowCalls

    now = dt.datetime.now(clock.UTC)
    mk = types.SimpleNamespace(ticker="S1", yes_bid=0.88, yes_ask=0.90, no_bid=0.10, no_ask=0.12,
                               open_time=now - dt.timedelta(seconds=400), close_time=now + dt.timedelta(seconds=500),
                               seconds_left=lambda at: 500.0, seconds_elapsed=lambda at: 400.0)
    desk = types.SimpleNamespace(tracker=types.SimpleNamespace(current=mk, quotes_fresh=lambda a, b: True),
                                 pred=pred(0.92, 0.89), _vol_info={"events": []})
    std = ShadowCalls("btc", "standard", TradingCfg(), str(tmp_path))
    vs = ShadowCalls("btc", "very_strict", TradingCfg(), str(tmp_path))
    for i in range(12):  # the model at 92% for 12 s: enough for Standard (85%), not for Very strict (97%)
        std.step(desk, 1000 + i)
        vs.step(desk, 1000 + i)
    assert std.calls.call and std.calls.call["rules"] == "v1" and vs.calls.call is None
    std.settle_from(lambda t: {"result": "yes"})
    assert (std.state()["wins"], std.state()["losses"], vs.state()["wins"]) == (1, 0, 0)
    assert (tmp_path / "shadow" / "btc-standard.db").exists() and (tmp_path / "shadow" / "btc-very_strict.db").exists()


def test_no_call_while_the_exact_index_is_down(tmp_path):
    import dataclasses

    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.trading.min_warmup_minutes = 1
    cfg.board.markets = ["btc"]
    board = Board(cfg, sim={"window_s": 120, "warmup_minutes": 10})

    async def run():
        await board.build()
        feed = board.desk("btc").feed
        real = feed.health
        feed.health = lambda: dataclasses.replace(real(), mode="FALLBACK: exchanges (Coinbase)")  # the index is out
        stop = asyncio.Event()
        task = asyncio.create_task(board.run(stop))
        await asyncio.sleep(3)
        stop.set()
        await task

    asyncio.run(run())
    c = board.state["markets"][0]["call"]
    assert c["watch"]["status"].startswith("no new call: the exact index is down") and c["call"] is None and c["meter"]



def test_the_background_modes_skip_the_backup_price_too(tmp_path):
    import datetime as dt
    import types

    from cx15m import clock
    from cx15m.shadow import ShadowCalls

    now = dt.datetime.now(clock.UTC)
    mk = types.SimpleNamespace(ticker="W1", yes_bid=0.88, yes_ask=0.90, no_bid=0.10, no_ask=0.12,
                               open_time=now - dt.timedelta(seconds=400), close_time=now + dt.timedelta(seconds=500),
                               seconds_left=lambda at: 500.0, seconds_elapsed=lambda at: 400.0)

    def desk(mode):
        return types.SimpleNamespace(tracker=types.SimpleNamespace(current=mk, quotes_fresh=lambda a, b: True),
                                     pred=pred(0.92, 0.89), _vol_info={"events": []},
                                     feed=types.SimpleNamespace(health=lambda: types.SimpleNamespace(mode=mode)))

    live = ShadowCalls("btc", "standard", TradingCfg(), str(tmp_path / "a"))
    backup = ShadowCalls("btc", "standard", TradingCfg(), str(tmp_path / "b"))
    for i in range(12):
        live.step(desk("kalshi-index"), 1000 + i)
        backup.step(desk("FALLBACK: exchanges (Coinbase)"), 1000 + i)
    assert live.calls.call is not None and backup.calls.call is None


def test_early_calls_are_counted_under_the_current_rules_only(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.board.markets = ["btc", "doge"]
    board = Board(cfg, sim={"window_s": 900, "warmup_minutes": 10})
    asyncio.run(board.build())
    rows = (("T1", 420.0, "UP", "UP", "v1"),     # 7:00 left, won: early
            ("T2", 390.0, "UP", "DOWN", "v1"),   # exactly 6:30 left, lost: early
            ("T3", 300.0, "UP", "UP", "v1"),     # 5:00 left: not early
            ("T4", 500.0, "UP", "UP", "strict"),   # early, but made in another mode
            ("T5", 600.0, "UP", None, "v1"),     # early, not settled yet
            ("T6", 700.0, "DOWN", "DOWN", None))   # early, made with tuning off (no rules tag)
    for key in ("btc", "doge"):
        d = board.desk(key)
        store = board._window_store(d)
        for tk, left, side, result, rules in rows:
            store.add_call(tk, side, 0.9, 0.85, 1000.0, left, "confident", 0.9, 0.88, rules)
            if result:
                store.settle_call(tk, result, 2000.0)
        board._early_step(d)
        assert d.state["early"] == {"wins": 1, "losses": 1, "tag": "v1"}
    board.set_mode("strict")
    for key in ("btc", "doge"):
        board._early_step(board.desk(key))
        assert (board.desk(key).state["early"]["wins"], board.desk(key).state["early"]["losses"]) == (1, 0)
    for key in ("btc", "doge"):  # tuning off: only the untagged calls count
        d = board.desk(key)
        d.cfg.trading.rules_tag = ""
        board._early_step(d)
        assert d.state["early"] == {"wins": 1, "losses": 0, "tag": ""}
    assert board._build_state()["early_left_s"] == tuning.EARLY_LEFT_S == 390.0



def test_the_demo_scales_the_early_line_with_its_short_windows(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    assert Board(cfg, sim={"window_s": 120}).early_left_s() == 52.0  # 6:30 of 15:00 = 52 s of a 2-minute window
