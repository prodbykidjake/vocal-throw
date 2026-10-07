"""Desktop notifications: macOS Notification Center + system sound (no extra dependencies)."""
from __future__ import annotations

import logging
import shutil
import subprocess
import sys

log = logging.getLogger(__name__)


class Notifier:
    def __init__(self, desktop: bool = True, sound: bool = True):
        self.desktop = desktop
        self.sound = sound

    def notify(self, title: str, message: str, sound_name: str = "Glass"):
        log.info("NOTIFY %s: %s", title, message)
        try:
            if sys.platform == "darwin":
                if self.desktop:
                    script = f'display notification "{_esc(message)}" with title "{_esc(title)}"'
                    subprocess.Popen(["osascript", "-e", script], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if self.sound:
                    subprocess.Popen(["afplay", f"/System/Library/Sounds/{sound_name}.aiff"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            elif self.desktop and shutil.which("notify-send"):
                subprocess.Popen(["notify-send", title, message], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception as exc:  # notifications are best-effort
            log.warning("notification failed: %s", exc)


def _esc(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')
