from ng15m.feeds.hyperliquid import lookup_mid, pick_gas_symbol


def test_pick_gas_symbol_prefers_known_names_and_never_takes_oil():
    assert pick_gas_symbol(["xyz:NVDA", "xyz:CL", "xyz:NATGAS"]) == "xyz:NATGAS"
    assert pick_gas_symbol(["BTC", "ETH", "NG"]) == "NG"
    assert pick_gas_symbol(["xyz:NATGASUSD"]) == "xyz:NATGASUSD"
    assert pick_gas_symbol(["xyz:CL", "xyz:WTI", "xyz:NGINX"]) is None


def test_lookup_mid_handles_prefixes():
    assert lookup_mid({"xyz:CL": "90.19"}, "xyz:CL") == 90.19
    assert lookup_mid({"CL": "90.19"}, "xyz:CL") == 90.19
    assert lookup_mid({"xyz:CL": "90.19"}, "CL") == 90.19
    assert lookup_mid({"BTC": "1"}, "xyz:CL") is None
    assert lookup_mid([], "xyz:CL") is None
