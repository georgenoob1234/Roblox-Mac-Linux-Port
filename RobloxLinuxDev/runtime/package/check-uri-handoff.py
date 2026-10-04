#!/usr/bin/env python3
"""Offline checks for opaque URI transport, lock notifications and the desktop handler."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
launcher_source = (ROOT / "bin/roblox-mac").read_text()
bootstrap_source = (ROOT / "package/bootstrap.sh").read_text()
assert "roblox URI received: scheme=%s length=%s" in launcher_source
assert "pending-uri" not in launcher_source and "retry_pending" not in launcher_source
assert "flock -n -E 73 9" in launcher_source
assert "flock -n -E 73 9" in bootstrap_source
assert "notify-send" in bootstrap_source and "gdbus" in bootstrap_source
assert "ROBLOX_MAC_LAUNCH_URI" in launcher_source

with tempfile.TemporaryDirectory(prefix="roblox uri ") as temp:
    folder = Path(temp)
    private = folder / "DO_NOT_SHARE"
    private.mkdir(mode=0o700)
    app = private / "missing.app"
    first = 'roblox://place/+%2B:тест "quoted"'
    result = subprocess.run([ROOT / "bin/roblox-mac", "--uri", first],
                            env={**os.environ, "ROBLOX_MAC_CONFIG": "/dev/null",
                                 "ROBLOX_MAC_DATA": str(private), "ROBLOX_MAC_APP": str(app)},
                            capture_output=True, text=True)
    assert first not in result.stdout and first not in result.stderr
    assert "scheme=roblox" in result.stderr and "length=" in result.stderr
    assert not (private / "pending-uri").exists()

    release = Path(temp) / "release with spaces"
    release.mkdir()
    shutil.copy2(ROOT.parent.parent / "RobloxLinuxRelease/install-uri-handler.sh", release / "install-uri-handler.sh")
    shutil.copy2(ROOT.parent.parent / "RobloxLinuxRelease/icon.png", release / "icon.png")
    (release / "run.sh").write_text("#!/bin/sh\nexit 0\n"); (release / "run.sh").chmod(0o755)
    data = folder / "data"; config = folder / "config"; fakebin = folder / "bin"
    fakebin.mkdir(); calls = folder / "xdg-mime.calls"
    (fakebin / "xdg-mime").write_text("#!/bin/sh\necho \"$*\" >> \"$CALLS\"\n"); (fakebin / "xdg-mime").chmod(0o755)
    env = {**os.environ, "HOME": temp, "XDG_DATA_HOME": str(data), "XDG_CONFIG_HOME": str(config),
           "PATH": str(fakebin) + ":" + os.environ["PATH"], "CALLS": str(calls)}
    subprocess.run([release / "install-uri-handler.sh"], env=env, check=True, capture_output=True, text=True)
    desktop = data / "applications/roblox-mac-port.desktop"
    subprocess.run(["desktop-file-validate", desktop], check=True)
    text = desktop.read_text()
    assert "NoDisplay" not in text and "%u" in text and "MimeType=x-scheme-handler/roblox;" in text
    assert "Exec=" in text and "release\\ with\\ spaces/run.sh %u" in text
    icon = data / "icons/hicolor/256x256/apps/roblox-mac-port.png"
    assert icon.read_bytes() == (release / "icon.png").read_bytes()
    subprocess.run([release / "install-uri-handler.sh"], env=env, check=True, capture_output=True, text=True)
    assert len(calls.read_text().splitlines()) == 4
    mime = config / "mimeapps.list"; mime.parent.mkdir()
    mime.write_text("[Default Applications]\nx-scheme-handler/roblox=roblox-mac-port.desktop;other.desktop;\n"
                    "x-scheme-handler/roblox-player=roblox-mac-port.desktop;\n"
                    "x-scheme-handler/other=other.desktop;\n")
    subprocess.run([release / "install-uri-handler.sh", "uninstall"], env=env, check=True, capture_output=True, text=True)
    assert not desktop.exists() and "roblox-mac-port.desktop" not in mime.read_text()
    assert not icon.exists()
    assert "x-scheme-handler/other=other.desktop;" in mime.read_text()
print("PASS opaque in-memory URI transport, no pending queue, lock/notification hooks, desktop validation, installer idempotence/uninstall")
