from wti15m.fees import FeeSchedule, fee_per_contract, maker_fee, taker_fee


def test_fifty_cent_single_contract_rounds_up_to_two_cents():
    # 0.07 * 1 * 0.5 * 0.5 = 0.0175 -> rounds up to 0.02
    assert taker_fee(0.50, 1) == 0.02


def test_kalshi_table_example_100_contracts_at_ten_cents():
    # Kalshi's fee table: 100 contracts at 10c -> $0.63
    assert taker_fee(0.10, 100) == 0.63


def test_ninety_nine_cent_contract_still_pays_a_cent():
    assert taker_fee(0.99, 1) == 0.01
    assert taker_fee(0.01, 1) == 0.01


def test_multiplier_scales_fee():
    sched = FeeSchedule(fee_type="quadratic", multiplier=2.0)
    assert taker_fee(0.50, 100, sched) == 3.50


def test_maker_fee_only_on_maker_series():
    assert maker_fee(0.5, 100) == 0.0
    sched = FeeSchedule(fee_type="quadratic_with_maker_fees", multiplier=1.0)
    # 0.0175 * 100 * 0.25 = 0.4375 -> 0.44
    assert maker_fee(0.5, 100, sched) == 0.44


def test_fee_per_contract_spreads_rounding():
    assert abs(fee_per_contract(0.5, 10) - 0.018) < 1e-9  # 0.175 -> 0.18 / 10
    assert fee_per_contract(0.5, 1) == 0.02


def test_flat_schedule_is_flagged_as_estimate():
    assert FeeSchedule.from_series("flat", None).is_estimate
    assert not FeeSchedule.from_series("quadratic", "1").is_estimate
