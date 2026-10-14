from starlette.testclient import TestClient

from cm15m.app import create_app
from cm15m.board import Board
from cm15m.config import Config
from cm15m.phone import COOKIE, load_or_create_key

MAC = ("127.0.0.1", 50000)
PHONE = ("192.168.1.40", 50000)


def make_app(tmp_path, phone_key=None):
    cfg = Config()
    cfg.storage.data_dir = str(tmp_path)
    return create_app(Board(cfg), phone_key)


def test_the_phone_key_is_kept_across_restarts(tmp_path):
    key = load_or_create_key(str(tmp_path))
    assert len(key) >= 16 and load_or_create_key(str(tmp_path)) == key


def test_without_phone_mode_only_the_mac_gets_in(tmp_path):
    app = make_app(tmp_path)
    assert TestClient(app, base_url="http://127.0.0.1:8790", client=MAC).get("/api/state").status_code == 200
    assert TestClient(app, base_url="http://192.168.1.23:8790", client=PHONE).get("/api/state").status_code == 400


def test_phone_needs_the_link_once_then_stays_in(tmp_path):
    key = "k" * 24
    phone = TestClient(make_app(tmp_path, key), base_url="http://192.168.1.23:8790", client=PHONE, follow_redirects=False)
    assert phone.get("/").status_code == 403 and phone.get("/?key=wrong").status_code == 403
    r = phone.get(f"/?key={key}")
    assert r.status_code == 303 and r.headers["location"] == "/" and phone.cookies.get(COOKIE) == key
    assert phone.get("/").status_code == 200 and phone.get("/api/state").status_code == 200


def test_a_device_on_the_wifi_cannot_fake_being_the_mac_and_other_sites_get_nothing(tmp_path):
    app = make_app(tmp_path, "k" * 24)
    assert TestClient(app, base_url="http://127.0.0.1:8790", client=MAC).get("/api/state").status_code == 200
    assert TestClient(app, base_url="http://127.0.0.1:8790", client=PHONE).get("/api/state").status_code == 403
    assert TestClient(app, base_url="http://evil.example:8790", client=MAC).get("/api/state").status_code == 403
