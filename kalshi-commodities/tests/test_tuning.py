"""Per-market settlement-call rules: v1.4's (higher bar, longer study, no lean call; gas reads a blend with Kalshi) and
v1.5's strictness switch (Standard / Strict / Very strict, each market with its own bars), all three run on the same
windows so their records can be compared live."""
import asyncio

from cm15m import config, tuning
from cm15m.board import Board
from cm15m.calls import CallTracker, blend_up
from cm15m.config import Config, TradingCfg
from cm15m.model import Prediction
from cm15m.store import Store


def pred(p_up, p_market=None, conf="lean"):
    return Prediction(p_up, p_up, p_up, p_market, 0.0, 0.004, None, None, 600, 0.0, 0.1, conf, 0.0)


def tuned(key, **kw):
    t = tuning.apply(TradingCfg(), tuning.for_market(key))
    for k, v in kw.items():
        setattr(t, k, v)
    return CallTracker(t, Store(":memory:"))


def test_each_market_has_its_rules_and_config_can_override_them():
    assert tuning.for_market("wti").call_prob == 0.88 and tuning.for_market("gold").study_s == 240
    assert tuning.for_market("natgas").blend_market == 0.65 and tuning.for_market("platinum").call_prob == 0.85
    assert tuning.for_market("newmetal") == tuning.DEFAULT and not tuning.DEFAULT.late_call
    o = tuning.for_market("gold", {"gold": {"call_prob": 0.9, "bogus": 1}})
    assert o.call_prob == 0.9 and o.study_s == 240
    t = tuning.apply(TradingCfg(), tuning.for_market("copper"))
    assert (t.call_prob, t.study_s, t.late_call, t.call_hold_s, t.rules_tag) == (0.88, 300.0, False, 10.0, "v1.4")
    assert tuning.apply(TradingCfg(), tuning.for_market("copper"), window_s=90).study_s == 30  # the demo's short windows


def test_config_reads_tuning_sections(tmp_path):
    p = tmp_path / "config.toml"
    p.write_text("[tuning]\nenabled = false\n[tuning.gold]\ncall_prob = 0.9\n")
    cfg = config.load(str(p))
    assert not tuning.enabled(cfg.tuning) and tuning.for_market("gold", cfg.tuning).call_prob == 0.9
    assert tuning.enabled(Config().tuning)


def test_every_desk_gets_its_own_rules_and_the_shared_settings_stay_untouched(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    board = Board(cfg)
    board.sim = None
    silver, platinum = board._desk_cfg("silver").trading, board._desk_cfg("platinum").trading
    assert silver.call_prob == 0.88 and platinum.call_prob == 0.85 and silver is not platinum
    assert cfg.trading.call_prob == 0.72 and cfg.trading.late_call  # v1.3's rules stay the shared default
    cfg.tuning = {"enabled": False}
    assert board._desk_cfg("silver").trading.call_prob == 0.72


def test_engines_get_their_rules_on_the_board(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.trading.min_warmup_minutes = 1
    board = Board(cfg, sim={"window_s": 900, "warmup_minutes": 10})
    asyncio.run(board.build())
    by = {d.spec.key: d for d in board.desks}
    wti, gas, gold = by["wti"].cfg.trading, by["natgas"].cfg.trading, by["gold"].cfg.trading
    assert (wti.call_prob, wti.study_s, wti.late_call) == (0.88, 300.0, False)
    assert gas.call_blend_market == 0.65 and gold.study_s == 240.0
    assert by["silver"].cfg.trading.call_prob == 0.88 and by["silver"].calls.cfg.call_prob == 0.88


def test_no_lean_call_at_the_deadline_but_a_sure_call_still_counts_until_the_last_minute():
    ct = tuned("wti")
    t0 = 1000.0
    assert ct.step(t0 - 700, "W1", pred(0.80, 0.78), 880, 20, 300) is None  # seen from the start of the window
    # past the study, 80% is not enough at 88%; the old 2:00 lean call does not happen
    for i in range(120):
        assert ct.step(t0 + i, "W1", pred(0.80, 0.78), 180 - i, 720 + i, 300) is None
    assert ct.watch["deadline_in_s"] is None and ct.watch["closes_in_s"] is not None
    # with 1:30 left it gets sure enough and holds 10 s: a confident call
    made = None
    for i in range(12):
        made = ct.step(t0 + 200 + i, "W1", pred(0.90, 0.85), 90 - i, 810 + i, 300) or made
    assert made and made["kind"] == "confident" and made["side"] == "UP"
    # another window that never gets there: no call at all, and the card says why
    ct2 = tuned("wti")
    for i in range(900):
        assert ct2.step(t0 + i, "W2", pred(0.80, 0.78), 900 - i, i, 300) is None
    assert ct2.watch["status"] == "no call this window (never 88% sure for 10s)"


def test_gas_calls_on_its_blend_with_kalshi():
    assert abs(blend_up(0.8, 0.8, 0.65) - 0.8) < 1e-9 and blend_up(0.8, None, 0.65) == 0.8
    b = blend_up(0.80, 0.92, 0.65)  # log-odds: 0.35 * logit(.80) + 0.65 * logit(.92)
    assert 0.88 < b < 0.90
    ct = tuned("natgas")
    made = None
    for i in range(12):  # the engine alone says 80% (below 85%); with Kalshi at 92% the blend clears it
        made = ct.step(1000 + i, "G1", pred(0.80, 0.92), 500 - i, 400 + i, 240) or made
    assert made and abs(made["p_side"] - b) < 1e-6
    assert abs(ct.meter["p_up"] - round(b, 3)) < 1e-9 and ct.meter["model_up"] == 0.8


def test_since_v14_record_counts_only_calls_made_under_the_new_rules():
    store = Store(":memory:")
    store.add_call("OLD", "UP", 0.75, 0.7, 10.0, 500, "late")
    store.settle_call("OLD", "DOWN", 20.0)
    t = tuning.apply(TradingCfg(), tuning.for_market("gold"))
    ct = CallTracker(t, store)
    made = None
    for i in range(12):  # a call made under v1.4 carries its rules
        made = ct.step(1e9 + i, "NEW", pred(0.92, 0.9), 500 - i, 400 + i, 240) or made
    assert made and made["rules"] == "v1.4"
    store.settle_call("NEW", "UP", 1e9 + 600)
    # tuning switched off for a while: its calls (v1.3 rules, a lean call here) never count toward "since v1.4"
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
        ct = tuned("wti")
        for i in range(900):
            p = Prediction(0.95, 0.95, 0.95, p_market, 0.0, 0.004, None, None, 600, 0.0, 0.1, "lean", 0.0, disagree=disagree)
            assert ct.step(1000 + i, "W", p, 900 - i, i, 300) is None
        assert ct.watch["status"] == f"no call this window ({why})"


def test_overrides_are_read_forgivingly_and_bad_values_are_ignored(caplog):
    o = {"enabled": "false", "WTI": {"call_prob": 90, "late_call": "true"}, "gold": {"call_prob": 1.7, "blend_market": "x"},
         "natural_gas": {"call_prob": 0.9}}
    assert not tuning.enabled(o)
    wti = tuning.for_market("wti", o)
    assert wti.call_prob == 0.9 and wti.late_call is True  # [tuning.WTI] counts; 90 means 90%
    gold = tuning.for_market("gold", o)
    assert gold.call_prob == 0.88 and gold.blend_market == 0.0  # out of range / not a number: built-in kept
    assert tuning.check(o, ["wti", "gold", "natgas"]) == ["natural_gas"]
    assert "matches no market" in caplog.text


def test_engines_take_the_boards_report_settings(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.board.extra_events = {"gold": ["2026-11-06 08:30 Jobs report"]}
    board = Board(cfg, sim={"window_s": 900, "warmup_minutes": 10})
    asyncio.run(board.build())
    gold = board.desk("gold").engine
    assert [name for _, name in gold._extra_events] == ["Jobs report"] and gold.cfg.trading.call_block_events
    cfg2 = Config()
    cfg2.storage.data_dir = str(tmp_path / "b")
    cfg2.trading.event_entries = "allow"
    cfg2.tuning = {"enabled": False}
    board2 = Board(cfg2, sim={"window_s": 900, "warmup_minutes": 10})
    asyncio.run(board2.build())
    t = board2.desk("wti").engine.cfg.trading
    assert t.event_entries == "allow" and not t.call_block_events and t.call_prob == 0.72  # v1.3's rules


def test_engines_make_no_call_in_a_report_window(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.trading.min_warmup_minutes = 1
    cfg.board.markets = ["wti"]
    board = Board(cfg, sim={"window_s": 120, "warmup_minutes": 10})

    async def run():
        await board.build()
        eng = board.desk("wti").engine
        eng._window_events = lambda open_ts, close_ts: [(open_ts + 30, "Test report")]
        stop = asyncio.Event()
        task = asyncio.create_task(board.run(stop))
        await asyncio.sleep(3)
        stop.set()
        await task

    asyncio.run(run())
    c = board.state["markets"][0]["call"]
    assert c["watch"]["status"].startswith("no call this window (Test report") and c["call"] is None and c["meter"]


def test_strict_modes_raise_each_markets_own_bar():
    for key in ("wti", "gold", "silver", "copper", "natgas", "palladium", "platinum"):
        std, st, vs = (tuning.for_market(key, mode=m) for m in tuning.MODES)
        assert std.call_prob < st.call_prob < vs.call_prob < 0.99
        assert st.study_s == std.study_s and st.hold_s == std.hold_s == 10 and st.blend_market == std.blend_market
    assert tuning.for_market("natgas", mode="strict").call_prob < tuning.for_market("gold", mode="strict").call_prob
    assert tuning.for_market("newmetal", mode="very_strict").call_prob == tuning.MODE_DEFAULT_BARS["very_strict"]
    assert tuning.mode_of("Very strict") == "very_strict" and tuning.mode_of("bogus") == "standard"
    assert (tuning.tag("standard"), tuning.tag("strict")) == ("v1.4", "strict")
    # a value set in config.toml still wins over the mode's bar
    assert tuning.for_market("gold", {"gold": {"call_prob": 0.9}}, mode="strict").call_prob == 0.9


def test_the_switch_changes_every_card_and_is_remembered(tmp_path):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    cfg.notify.desktop = cfg.notify.sound = False
    cfg.trading.min_warmup_minutes = 1
    board = Board(cfg, sim={"window_s": 900, "warmup_minutes": 10})
    asyncio.run(board.build())
    assert board.mode == "standard" and board.desk("silver").calls.cfg.call_prob == 0.88
    board.set_mode("strict")
    silver, wti = board.desk("silver"), board.desk("wti").engine
    assert silver.calls.cfg.call_prob == tuning.MODE_BARS["silver"]["strict"] and silver.calls.cfg.rules_tag == "strict"
    assert wti.calls.cfg.call_prob == tuning.MODE_BARS["wti"]["strict"] and wti.calls.rules_since is not None
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

    from cm15m import clock
    from cm15m.shadow import ShadowCalls

    now = dt.datetime.now(clock.UTC)
    mk = types.SimpleNamespace(ticker="S1", yes_bid=0.88, yes_ask=0.90, no_bid=0.10, no_ask=0.12,
                               open_time=now - dt.timedelta(seconds=400), close_time=now + dt.timedelta(seconds=500),
                               seconds_left=lambda at: 500.0, seconds_elapsed=lambda at: 400.0)
    desk = types.SimpleNamespace(tracker=types.SimpleNamespace(current=mk, quotes_fresh=lambda a, b: True),
                                 pred=pred(0.92, 0.89), _vol_info={"events": []})
    std = ShadowCalls("silver", "standard", TradingCfg(), str(tmp_path))
    vs = ShadowCalls("silver", "very_strict", TradingCfg(), str(tmp_path))
    for i in range(12):  # the engine at 92% for 12 s: enough for Standard (88%), not for Very strict (97.1%)
        std.step(desk, 1000 + i)
        vs.step(desk, 1000 + i)
    assert std.calls.call and std.calls.call["rules"] == "v1.4" and vs.calls.call is None
    std.settle_from(lambda t: {"result": "yes"})
    assert (std.state()["wins"], std.state()["losses"], vs.state()["wins"]) == (1, 0, 0)
    assert (tmp_path / "shadow" / "silver-standard.db").exists() and (tmp_path / "shadow" / "silver-very_strict.db").exists()
