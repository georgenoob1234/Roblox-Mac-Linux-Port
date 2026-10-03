#!/usr/bin/env python3
"""Offline checks for opaque URI storage and the user-local desktop handler."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
launcher_source = (ROOT / "bin/roblox-mac").read_text()
assert "[ -z \"$LAUNCH_URI\" ] || [ \"$pending\" != \"$LAUNCH_URI\" ]" in launcher_source
spec = importlib.util.spec_from_file_location("uri", ROOT / "scripts/uri_handoff.py")
uri = importlib.util.module_from_spec(spec); spec.loader.exec_module(uri)

with tempfile.TemporaryDirectory(prefix="roblox uri ") as temp:
    private = Path(temp) / "DO_NOT_SHARE"; private.mkdir(mode=0o700)
    first = 'roblox://place/+%2B:тест "quoted"'
    second = 'roblox-player:1+gameinfo=%22α%22%:tail'
    assert uri.receive(private, first) == ("roblox", len(os.fsencode(first)))
    pending, lock = uri.paths(private)
    assert uri.peek(private) == first
    assert pending.stat().st_mode & 0o777 == 0o600
    assert lock.exists() and lock.stat().st_mode & 0o777 == 0o600
    assert second not in str(uri.receive(private, second))
    assert uri.peek(private) == second
    assert not uri.clear_if_consumed(private, first)
    assert uri.peek(private) == second
    assert uri.clear_if_consumed(private, second)
    assert uri.peek(private) is None
    result = subprocess.run([ROOT / "bin/roblox-mac", "--uri", first],
                            env={**os.environ, "ROBLOX_MAC_CONFIG": "/dev/null",
                                 "ROBLOX_MAC_DATA": str(private), "ROBLOX_MAC_APP": str(private / "missing.app")},
                            capture_output=True, text=True)
    assert first not in result.stdout and first not in result.stderr
    assert "scheme=roblox" in result.stderr and "length=" in result.stderr

    release = Path(temp) / "release with spaces"; release.mkdir()
    shutil.copy2(Path(__file__).parents[2] / "../RobloxLinuxRelease/install-uri-handler.sh", release / "install-uri-handler.sh")
    (release / "run.sh").write_text("#!/bin/sh\nexit 0\n"); (release / "run.sh").chmod(0o755)
    data = Path(temp) / "data"; config = Path(temp) / "config"; fakebin = Path(temp) / "bin"
    fakebin.mkdir(); calls = Path(temp) / "xdg-mime.calls"
    (fakebin / "xdg-mime").write_text("#!/bin/sh\necho \"$*\" >> \"$CALLS\"\n"); (fakebin / "xdg-mime").chmod(0o755)
    env = {**os.environ, "HOME": temp, "XDG_DATA_HOME": str(data), "XDG_CONFIG_HOME": str(config),
           "PATH": str(fakebin) + ":" + os.environ["PATH"], "CALLS": str(calls)}
    subprocess.run([release / "install-uri-handler.sh"], env=env, check=True, capture_output=True, text=True)
    desktop = data / "applications/roblox-mac-port.desktop"
    subprocess.run(["desktop-file-validate", desktop], check=True)
    text = desktop.read_text()
    assert "NoDisplay" not in text and "%u" in text and "MimeType=x-scheme-handler/roblox;" in text
    assert "Exec=" in text and "release\\ with\\ spaces/run.sh %u" in text
    subprocess.run([release / "install-uri-handler.sh"], env=env, check=True, capture_output=True)
    assert len(calls.read_text().splitlines()) == 4
    mime = config / "mimeapps.list"; mime.parent.mkdir()
    mime.write_text("[Default Applications]\nx-scheme-handler/roblox=roblox-mac-port.desktop;other.desktop;\n"
                    "x-scheme-handler/other=other.desktop;\n")
    subprocess.run([release / "install-uri-handler.sh", "uninstall"], env=env, check=True, capture_output=True)
    assert not desktop.exists() and "roblox-mac-port.desktop" not in mime.read_text()
    assert "x-scheme-handler/other=other.desktop;" in mime.read_text()
print("PASS URI opaque round-trip, atomic permissions, newest-click-wins, consumed-only clearing, desktop validation, installer idempotence/uninstall")
