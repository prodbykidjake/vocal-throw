from starlette.testclient import TestClient

from wti15m.app import create_app
from wti15m.config import Config
from wti15m.engine import Engine
from wti15m.feeds.sim import SimFeed
from wti15m.notify import Notifier
from wti15m.phone import COOKIE, load_or_create_key
from wti15m.sim import SimKalshi
from wti15m.store import Store

MAC = ("127.0.0.1", 50000)
PHONE = ("192.168.1.40", 50000)


def make_app(tmp_path, phone_key=None):
    feed = SimFeed(start_price=90.0, warmup_minutes=5, seed=1)
    engine = Engine(Config(), Store(str(tmp_path / "p.db")), SimKalshi(feed, window_s=120), feed,
                    Notifier(desktop=False, sound=False))
    return create_app(engine, phone_key)


def test_the_phone_key_is_kept_across_restarts(tmp_path):
    key = load_or_create_key(str(tmp_path))
    assert len(key) >= 16
    assert load_or_create_key(str(tmp_path)) == key


def test_without_phone_mode_only_the_mac_gets_in(tmp_path):
    app = make_app(tmp_path)
    assert TestClient(app, base_url="http://127.0.0.1:8787", client=MAC).get("/api/state").status_code == 200
    assert TestClient(app, base_url="http://192.168.1.23:8787", client=PHONE).get("/api/state").status_code == 400


def test_phone_needs_the_link_once_then_stays_in(tmp_path):
    key = "k" * 24
    app = make_app(tmp_path, key)
    phone = TestClient(app, base_url="http://192.168.1.23:8787", client=PHONE, follow_redirects=False)
    assert phone.get("/").status_code == 403
    assert phone.get("/api/state").status_code == 403
    assert phone.get("/?key=wrong").status_code == 403
    r = phone.get(f"/?key={key}")
    assert r.status_code == 303 and r.headers["location"] == "/"  # the key leaves the address bar
    assert phone.cookies.get(COOKIE) == key
    assert phone.get("/").status_code == 200
    assert phone.get("/api/state").status_code == 200
    # the position box works from the phone too
    r = phone.request("DELETE", "/api/position", headers={"X-WTI15M": "1"})
    assert r.status_code == 200


def test_phone_mode_still_lets_the_mac_in_without_a_key(tmp_path):
    app = make_app(tmp_path, "k" * 24)
    mac = TestClient(app, base_url="http://127.0.0.1:8787", client=MAC)
    assert mac.get("/api/state").status_code == 200
    assert mac.request("DELETE", "/api/position", headers={"X-WTI15M": "1"}).status_code == 200


def test_a_device_on_the_wifi_cannot_fake_being_the_mac(tmp_path):
    app = make_app(tmp_path, "k" * 24)
    spoof = TestClient(app, base_url="http://127.0.0.1:8787", client=PHONE)  # Host says 127.0.0.1, address says not
    assert spoof.get("/api/state").status_code == 403
    assert spoof.request("DELETE", "/api/position", headers={"X-WTI15M": "1"}).status_code == 403


def test_another_site_cannot_use_the_phone_cookie(tmp_path):
    key = "k" * 24
    app = make_app(tmp_path, key)
    rebound = TestClient(app, base_url="http://evil.example:8787", client=MAC)  # a DNS-rebound page on the Mac
    assert rebound.get("/api/state").status_code == 403
