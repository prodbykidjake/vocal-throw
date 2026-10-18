from gold15m.feeds.hyperliquid import lookup_mid, pick_gold_symbol


def test_pick_gold_symbol_prefers_known_names_and_never_takes_another_metal():
    assert pick_gold_symbol(["xyz:NVDA", "xyz:SILVER", "xyz:GOLD"]) == "xyz:GOLD"
    assert pick_gold_symbol(["BTC", "ETH", "XAU"]) == "XAU"
    assert pick_gold_symbol(["xyz:GOLDUSD"]) == "xyz:GOLDUSD"
    assert pick_gold_symbol(["xyz:SILVER", "xyz:PLATINUM", "xyz:CL"]) is None


def test_lookup_mid_handles_prefixes():
    assert lookup_mid({"xyz:CL": "90.19"}, "xyz:CL") == 90.19
    assert lookup_mid({"CL": "90.19"}, "xyz:CL") == 90.19
    assert lookup_mid({"xyz:CL": "90.19"}, "CL") == 90.19
    assert lookup_mid({"BTC": "1"}, "xyz:CL") is None
    assert lookup_mid([], "xyz:CL") is None
