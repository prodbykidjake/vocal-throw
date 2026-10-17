"""The coins' live prices: the exchanges' median midpoint (no key), Kalshi's exact index (with an API key), and the
switch to the exchanges, marked FALLBACK, when the index goes quiet."""
import base64
import pathlib
import json
import time

import pytest

from cx15m.feeds import exchanges as ex
from cx15m.feeds.base import PriceFeed, on_backup
from cx15m.feeds.composite import FallbackFeed
from cx15m.feeds.kalshi_index import KalshiIndexFeed, KalshiIndexHub, has_key, key_settings, parse_index, sign_headers


def test_exchange_messages_are_read_into_midpoints():
    assert ex.parse_coinbase({"type": "ticker", "product_id": "BTC-USD", "price": "80010.00", "best_bid": "80000.00",
                              "best_ask": "80001.00"}) == [("BTC", 80000.5)]
    assert ex.parse_coinbase({"type": "subscriptions"}) == []
    assert ex.parse_coinbase({"type": "ticker", "product_id": "BTC-USD", "best_bid": "2", "best_ask": "1"}) == []  # crossed
    msg = {"channel": "ticker", "type": "update", "data": [{"symbol": "DOGE/USD", "bid": 0.08851, "ask": 0.08853},
                                                         {"symbol": "ETH/USD", "bid": 2359.4, "ask": 2359.6}]}
    assert ex.parse_kraken(msg) == [("DOGE", pytest.approx(0.08852)), ("ETH", pytest.approx(2359.5))]
    assert ex.parse_kraken({"channel": "heartbeat"}) == []
    rows = [{"pair": "SOL/USD", "bid": "103.70", "ask": "103.80"}, {"pair": "SOL/EUR", "bid": "90", "ask": "91"}, "junk"]
    assert ex.parse_bitstamp(rows) == [("SOL", pytest.approx(103.75))]


def test_median_leaves_out_stale_quotes_and_a_broken_book():
    now = 1000.0
    q = {"coinbase": (999.0, 100.0), "kraken": (998.0, 100.2), "bitstamp": (999.5, 100.1)}
    assert ex.median_mid(q, now, 20) == (pytest.approx(100.1), ["bitstamp", "coinbase", "kraken"])
    q["kraken"] = (970.0, 100.2)  # 30 s old
    price, used = ex.median_mid(q, now, 20)
    assert price == pytest.approx(100.05) and used == ["bitstamp", "coinbase"]
    q = {"coinbase": (999.0, 100.0), "kraken": (999.0, 100.1), "bitstamp": (999.0, 103.0)}  # 3% off: left out
    assert ex.median_mid(q, now, 20) == (pytest.approx(100.05), ["coinbase", "kraken"])
    assert ex.median_mid({}, now, 20) == (None, [])


def test_exchange_feed_prints_the_median_once_a_second():
    hub = ex.ExchangeHub(["coinbase", "kraken"], max_age_s=20)
    feed = ex.ExchangeFeed(hub, "btc", "Bitcoin")
    assert hub.assets == {"BTC"} and "median of Coinbase, Kraken" in feed.symbol
    now = time.time()
    assert feed.print_once(now) is None and feed.mode == "waiting for exchange prices"
    hub.on_quote("coinbase", "BTC", 80000.0, now - 1)
    hub.on_quote("kraken", "BTC", 80002.0, now - 2)
    hub.on_quote("kraken", "ETH", 2000.0, now)  # a coin no feed asked for is ignored
    assert feed.print_once(now) == pytest.approx(80001.0)
    assert feed.latest().ts == float(int(now)) and feed.mode == "exchanges (Coinbase, Kraken)" and "ETH" not in hub.quotes


class Stub(PriceFeed):
    name = "stub"

    async def start(self):
        pass

    async def stop(self):
        pass


def test_fallback_feed_switches_to_the_exchanges_and_says_so():
    primary, backup = Stub(), Stub()
    primary.name, backup.name = "kalshi-index", "exchanges"
    primary.mode, backup.mode = "kalshi-index", "exchanges (Coinbase)"
    f = FallbackFeed(primary, backup, stale_s=5)
    now = time.time()
    backup._publish(now - 1, 99.0)  # the index has said nothing yet: the backup's price is used, marked FALLBACK
    assert f.latest().price == 99.0 and f.active_name == "exchanges" and on_backup(f)
    assert f.health().mode.startswith("FALLBACK: exchanges (Coinbase)")
    primary._publish(now, 100.0)
    backup._publish(now + 0.5, 99.5)  # while the index is fresh the backup's prints are ignored
    assert f.latest().price == 100.0 and f.active_name == "kalshi-index" and not on_backup(f)
    assert f.health().mode == "kalshi-index"


def test_index_messages_are_parsed_wherever_their_fields_are():
    frame = json.dumps({"type": "value", "time": 1_760_000_000_000, "id": "BRTI", "value": "80056.35"})
    msg = {"type": "cfbenchmarks_value", "sid": 1, "msg": {"index_id": "BRTI", "received_at": 1_760_000_000_123,
                                                           "data": frame, "avg_60s_data": {"value": "80050.10"},
                                                           "last_60s_windowed_average_15min": "80051.25"}}
    v = parse_index(json.dumps(msg))
    assert v == {"id": "BRTI", "ts": 1_760_000_000.0, "value": 80056.35, "avg60": 80050.10, "settle_avg": 80051.25}
    assert parse_index({"type": "subscribed", "msg": {"channel": "cfbenchmarks_value"}}) is None
    flat = parse_index({"type": "cfbenchmarks_value", "msg": {"index_id": "dogeusd_rti", "value": 0.0885266,
                                                               "received_at": "2026-10-17T12:00:00Z"}})
    assert flat["id"] == "DOGEUSD_RTI" and flat["value"] == 0.0885266 and flat["settle_avg"] is None


def test_index_feed_publishes_and_offers_kalshis_running_average():
    hub = KalshiIndexHub("wss://example", "id", "/nonexistent.pem")
    feed = KalshiIndexFeed(hub, "brti", "Bitcoin")
    now = time.time()
    raw = {"type": "cfbenchmarks_value", "msg": {"index_id": "BRTI", "data": json.dumps({"time": now * 1000, "value": "80000.5"}),
                                                  "last_60s_windowed_average_15min": "79990.0"}}
    assert hub.handle(json.dumps(raw)) and feed.latest().price == 80000.5 and feed.mode == "kalshi-index"
    assert feed.settle_avg(now) == 79990.0 and feed.settle_avg(now + 10) is None
    assert not hub.handle(json.dumps({"type": "cfbenchmarks_value", "msg": {"index_id": "ETHUSD_RTI", "value": 1}}))


def test_api_key_settings_and_the_signed_handshake(tmp_path, monkeypatch):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding, rsa

    from cx15m.config import KalshiCfg

    monkeypatch.delenv("KALSHI_API_KEY_ID", raising=False)
    monkeypatch.delenv("KALSHI_PRIVATE_KEY_PATH", raising=False)
    assert not has_key(KalshiCfg())
    monkeypatch.setenv("KALSHI_API_KEY_ID", "abc-123")
    monkeypatch.setenv("KALSHI_PRIVATE_KEY_PATH", "~/kalshi-key.pem")
    key_id, path = key_settings(KalshiCfg())
    assert has_key(KalshiCfg()) and key_id == "abc-123" and path.endswith("kalshi-key.pem") and "~" not in path
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    h = sign_headers("abc-123", key, ts_ms=1_760_000_000_000)
    assert h["KALSHI-ACCESS-KEY"] == "abc-123" and h["KALSHI-ACCESS-TIMESTAMP"] == "1760000000000"
    key.public_key().verify(base64.b64decode(h["KALSHI-ACCESS-SIGNATURE"]), b"1760000000000GET/trade-api/ws/v2",
                            padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.DIGEST_LENGTH),
                            hashes.SHA256())  # raises if the signature is wrong
    pem = tmp_path / "k.pem"
    pem.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                      serialization.NoEncryption()))
    from cx15m.feeds.kalshi_index import load_private_key
    assert load_private_key(str(pem)).key_size == 2048


def test_ed25519_keys_sign_the_same_handshake():
    from cryptography.hazmat.primitives.asymmetric import ed25519

    from cx15m.feeds.kalshi_index import key_kind

    key = ed25519.Ed25519PrivateKey.generate()
    h = sign_headers("abc-123", key, ts_ms=1_760_000_000_000)
    key.public_key().verify(base64.b64decode(h["KALSHI-ACCESS-SIGNATURE"]), b"1760000000000GET/trade-api/ws/v2")
    assert key_kind(key) == "ed25519"
    with pytest.raises(ValueError):
        key_kind(object())


def test_setup_key_moves_the_key_and_writes_the_config(tmp_path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519

    from cx15m import config, keysetup

    home, app = tmp_path / "home", tmp_path / "app"
    (home / "Downloads").mkdir(parents=True)
    app.mkdir()
    pem = ed25519.Ed25519PrivateKey.generate().private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                                           serialization.NoEncryption())
    (home / "Downloads" / "notes.pem").write_text("not a key")
    (home / "Downloads" / "kalshi-key.pem").write_bytes(pem)
    example = pathlib.Path(__file__).resolve().parents[1] / "config.example.toml"
    (app / "config.example.toml").write_text(example.read_text())
    said = []
    assert keysetup.run(app, home, key_id=' "Key ID: 1a2b3c4d-5e6f-7a8b-9c0d-112233445566" ', say=said.append) == 0
    dest = home / "kalshi-key.pem"
    assert dest.read_bytes() == pem and not (home / "Downloads" / "kalshi-key.pem").exists()
    assert oct(dest.stat().st_mode & 0o777) == "0o600"
    assert not any("PRIVATE" in line for line in said)  # the key's contents are never printed
    cfg = config.load(str(app / "config.toml"))
    assert cfg.kalshi.api_key_id == "1a2b3c4d-5e6f-7a8b-9c0d-112233445566" and cfg.kalshi.private_key_path == "~/kalshi-key.pem"
    assert cfg.server.port == 8792 and cfg.kalshi.category == "Crypto"  # the rest of the example kept
    # run again with a new ID: replaced in place, not duplicated; the key already in place is found
    assert keysetup.run(app, home, key_id="ffff0000-1111", say=said.append) == 0
    text = (app / "config.toml").read_text()
    assert text.count("api_key_id =") == 1 and 'api_key_id = "ffff0000-1111"' in text


def test_setup_key_refuses_a_pasted_private_key_and_a_missing_file(tmp_path):
    from cx15m import keysetup

    with pytest.raises(ValueError):
        keysetup.clean_key_id("-----BEGIN PRIVATE KEY----- MC4C...")
    with pytest.raises(ValueError):
        keysetup.clean_key_id("hello world")
    said = []
    assert keysetup.run(tmp_path, tmp_path, key_id="1a2b3c4d-5e6f", say=said.append) == 2 and "No Kalshi private key" in said[0]
    out = keysetup.write_config(tmp_path / "c.toml", tmp_path / "none.toml", "1a2b3c4d-5e6f")
    assert out.startswith('[kalshi]\napi_key_id = "1a2b3c4d-5e6f"')
