from wti15m.feeds.hyperliquid import lookup_mid, pick_wti_symbol


def test_pick_wti_symbol_prefers_known_names():
    assert pick_wti_symbol(["xyz:NVDA", "xyz:CL", "xyz:GOLD"]) == "xyz:CL"
    assert pick_wti_symbol(["BTC", "ETH", "WTIOIL"]) == "WTIOIL"
    assert pick_wti_symbol(["xyz:WTIUSD"]) == "xyz:WTIUSD"
    assert pick_wti_symbol(["xyz:NVDA", "xyz:CLOV"]) is None


def test_lookup_mid_handles_prefixes():
    assert lookup_mid({"xyz:CL": "90.19"}, "xyz:CL") == 90.19
    assert lookup_mid({"CL": "90.19"}, "xyz:CL") == 90.19
    assert lookup_mid({"xyz:CL": "90.19"}, "CL") == 90.19
    assert lookup_mid({"BTC": "1"}, "xyz:CL") is None
    assert lookup_mid([], "xyz:CL") is None
