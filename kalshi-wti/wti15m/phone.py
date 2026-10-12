"""Phone access: with `serve --phone` (or `[server] phone = true`) the dashboard also answers other devices on your
network (your phone on the same Wi-Fi, or over Tailscale), but only ones that opened the private phone link once.

The link carries a random key kept in data/phone_key.txt (the same key across restarts, so the phone stays logged
in). Opening it stores the key in a cookie on the phone for a year; every request that does not come from the Mac
itself must carry that cookie. Delete data/phone_key.txt to lock every phone out and get a new link."""
from __future__ import annotations

import os
import pathlib
import secrets
import socket

KEY_FILE = "phone_key.txt"
COOKIE = "wti15m_phone"


def load_or_create_key(data_dir: str) -> str:
    path = pathlib.Path(data_dir) / KEY_FILE
    try:
        key = path.read_text(encoding="utf-8").strip()
        if len(key) >= 16:
            return key
    except OSError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    key = secrets.token_urlsafe(18)
    path.write_text(key + "\n", encoding="utf-8")
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    return key


def key_matches(given: str | None, key: str) -> bool:
    return bool(given) and secrets.compare_digest(given.encode("utf-8"), key.encode("utf-8"))


def lan_ip() -> str | None:
    """The Mac's address on the local network (a UDP 'connect' sends nothing; it only picks the outgoing interface)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            ip = s.getsockname()[0]
        return None if ip.startswith("127.") else ip
    except OSError:
        return None


def phone_links(port: int, key: str) -> list[str]:
    links = []
    ip = lan_ip()
    if ip:
        links.append(f"http://{ip}:{port}/?key={key}")
    name = socket.gethostname()
    if name and name not in ("localhost",) and "." not in name.removesuffix(".local"):
        local = name if name.endswith(".local") else f"{name}.local"
        links.append(f"http://{local}:{port}/?key={key}")
    return links
