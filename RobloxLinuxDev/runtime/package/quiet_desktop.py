"""Keep a check's launches off the tester's desktop.

install() puts recording stand-ins for notify-send, kdialog, zenity and gdbus
first in PATH and clears the session bus addresses, for this process and
everything it starts. A launcher that fails in a test then cannot send a real
"Roblox update failed" notification or open a real dialog on the desktop of
whoever runs the checks. Cases that test notifications put their own recorders
first in PATH, as before.
"""

import atexit
import os
import shutil
import tempfile
from pathlib import Path

TOOLS = ("notify-send", "kdialog", "zenity", "gdbus")


def install():
    folder = Path(tempfile.mkdtemp(prefix="quiet-desktop-"))
    atexit.register(shutil.rmtree, folder, True)
    record = folder / "calls"
    for name in TOOLS:
        tool = folder / name
        tool.write_text(f'#!/bin/sh\nprintf "%s\\n" "{name} $*" >> "{record}"\nexit 0\n')
        tool.chmod(0o755)
    os.environ["PATH"] = f"{folder}:{os.environ.get('PATH', '/usr/bin:/bin')}"
    for name in ("DBUS_SESSION_BUS_ADDRESS", "ROBLOX_MAC_NOTIFY_DBUS_SESSION_BUS_ADDRESS"):
        os.environ[name] = ""
    return record
