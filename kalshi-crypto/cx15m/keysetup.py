"""`python -m cx15m setup-key`: wire a Kalshi API key into the board in one step.

1. Finds the private key file Kalshi gave you (~/kalshi-key.pem, or the newest kalshi*.pem / kalshi*.txt / *.pem in
   ~/Downloads) and checks it really is a private key.
2. Moves it to ~/kalshi-key.pem, readable by you only (out of Downloads, where it is easy to share by accident).
3. Asks for the Key ID (shown next to the key on kalshi.com) and writes both into config.toml's [kalshi] section,
   making config.toml from config.example.toml if it does not exist yet.

The key's contents are never printed, logged or sent anywhere; only the board's signed connection uses it.
"""
from __future__ import annotations

import os
import pathlib
import re
import shutil

DEST_NAME = "kalshi-key.pem"
KEY_TEXT = "~/" + DEST_NAME


def _is_private_key(path: pathlib.Path) -> bool:
    try:
        if not path.is_file() or path.stat().st_size > 20_000:
            return False
        from .feeds.kalshi_index import key_kind, load_private_key
        key_kind(load_private_key(str(path)))
        return True
    except Exception:
        return False


def find_key_file(home: pathlib.Path) -> pathlib.Path | None:
    """The key already in place, else the newest private key file in Downloads (Kalshi's download, by name first)."""
    dest = home / DEST_NAME
    if _is_private_key(dest):
        return dest
    downloads = home / "Downloads"
    if not downloads.is_dir():
        return None
    named = [p for p in downloads.iterdir() if "kalshi" in p.name.lower() and p.suffix.lower() in (".pem", ".txt", ".key")]
    other = [p for p in downloads.glob("*.pem") if p not in named]
    for group in (named, other):
        for p in sorted(group, key=lambda q: q.stat().st_mtime, reverse=True):
            if _is_private_key(p):
                return p
    return None


def install_key(src: pathlib.Path, home: pathlib.Path) -> pathlib.Path:
    """Move the key to ~/kalshi-key.pem (keeping an existing different one as kalshi-key.old.pem) and make it
    readable by its owner only."""
    dest = home / DEST_NAME
    if src.resolve() != dest.resolve():
        if dest.exists():
            dest.replace(home / "kalshi-key.old.pem")
        shutil.move(str(src), str(dest))
    os.chmod(dest, 0o600)
    return dest


def clean_key_id(text: str) -> str:
    """The Key ID as pasted (quotes, spaces and a "Key ID:" label removed). Raises ValueError if it does not look
    like one, e.g. when the private key itself was pasted by mistake."""
    s = str(text or "").strip().strip('"\'').strip()
    s = re.sub(r"^(api\s*)?key\s*id\s*[:=]\s*", "", s, flags=re.I).strip().strip('"\'')
    if "PRIVATE KEY" in s.upper() or "BEGIN" in s.upper():
        raise ValueError("that is the private key itself: paste only the Key ID (a short code like 1a2b3c4d-...)")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_\-]{7,79}", s):
        raise ValueError("that does not look like a Key ID (letters, digits and dashes, like 1a2b3c4d-5e6f-...)")
    return s


def write_config(config_path: pathlib.Path, example_path: pathlib.Path, key_id: str, key_path: str = KEY_TEXT) -> str:
    """Set api_key_id and private_key_path under [kalshi], keeping everything else in the file. Returns the text."""
    if config_path.exists():
        text = config_path.read_text(encoding="utf-8")
    elif example_path.exists():
        text = example_path.read_text(encoding="utf-8")
    else:
        text = "[kalshi]\n"
    lines = text.splitlines()
    values = {"api_key_id": f'api_key_id = "{key_id}"', "private_key_path": f'private_key_path = "{key_path}"'}
    start = next((i for i, ln in enumerate(lines) if ln.strip() == "[kalshi]"), None)
    if start is None:
        lines = ["[kalshi]", *values.values(), ""] + lines
    else:
        end = next((i for i in range(start + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
        done = set()
        for i in range(start + 1, end):
            for name, line in values.items():
                if re.match(rf"\s*#?\s*{name}\s*=", lines[i]) and name not in done:
                    lines[i] = line  # replaces the example's commented line, or an old value
                    done.add(name)
        missing = [line for name, line in values.items() if name not in done]
        lines[start + 1:start + 1] = missing
    out = "\n".join(lines) + "\n"
    config_path.write_text(out, encoding="utf-8")
    return out


def run(app_dir: pathlib.Path, home: pathlib.Path | None = None, key_id: str | None = None, ask=input, say=print) -> int:
    home = home or pathlib.Path.home()
    src = find_key_file(home)
    if src is None:
        say("No Kalshi private key file found in your home folder or Downloads. On kalshi.com: Account & security ->"
            " API Keys -> Create Key, let it download, then run this again.")
        return 2
    dest = install_key(src, home)
    say(f"key file: {dest} (moved from {src.parent})" if src != dest else f"key file: {dest}")
    while True:
        try:
            key_id = clean_key_id(key_id if key_id is not None else ask("Paste your Kalshi Key ID (not the private key): "))
            break
        except ValueError as exc:
            say(str(exc))
            key_id = None
            if ask is input:
                continue
            return 2
    write_config(app_dir / "config.toml", app_dir / "config.example.toml", key_id)
    say(f"config.toml: Key ID {key_id[:8]}... and {KEY_TEXT} saved under [kalshi]. The board now reads the exact index.")
    return 0
