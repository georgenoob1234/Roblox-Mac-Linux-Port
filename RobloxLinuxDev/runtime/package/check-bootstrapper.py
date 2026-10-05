#!/usr/bin/env python3
"""Offline bootstrapper checks: settings, scheduling, setup, update and lock flow."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
ui_source = (ROOT / "package/bootstrapper-ui.c").read_text()
assert "g_environ_unsetenv" in ui_source and '"LD_LIBRARY_PATH"' in ui_source
with tempfile.TemporaryDirectory(prefix="roblox bootstrap ") as tmp:
    release = Path(tmp) / "release"
    release.mkdir()
    private = release / "DO_NOT_SHARE"
    private.mkdir()
    shutil.copy2(ROOT / "package/bootstrap.sh", release / "bootstrap.sh")
    shutil.copy2(ROOT / "package/bootstrap.py", release / "bootstrap.py")
    (release / "run.sh").write_text("#!/bin/sh\nexec \"$(dirname -- \"$0\")/RobloxLinux.AppImage\" \"$@\"\n")
    (release / "run.sh").chmod(0o755)
    (release / "RobloxVersion").mkdir()
    (release / "RobloxVersion/.version").write_text("version-old\n")
    (release / "RobloxLinux.AppImage").write_text("""#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "${1:-}" in
  --shell) test ! -e /proc/$$/fd/9; mkdir -p "$HERE/DO_NOT_SHARE/prefix"; exit 0;;
  --client-version) echo version-abcd; exit 0;;
  --uri) printf '%s\n' launched-with-uri > "$HERE/uri-launch"; exit 0;;
  *) printf '%s\n' launched > "$HERE/launched"; sleep "${FAKE_LAUNCH_SLEEP:-0}"; exit 0;;
esac
""")
    (release / "RobloxLinux.AppImage").chmod(0o755)
    (release / "update-roblox.sh").write_text("""#!/bin/sh
set -eu
[ "${FAIL_UPDATE:-0}" = 1 ] && exit 1
mkdir -p "$(dirname -- "$0")/RobloxVersion"
printf 'version-abcd\\n' > "$(dirname -- "$0")/RobloxVersion/.version"
""")
    (release / "update-roblox.sh").chmod(0o755)
    env = {**os.environ, "PATH": os.environ["PATH"], "ROBLOX_MAC_UPDATE_CHECK_TIMEOUT": "2"}
    bootstrap = release / "bootstrap.sh"

    status = json.loads(subprocess.check_output([bootstrap, "status"], env=env, text=True))
    assert status["settings"]["auto_update"] == "ask"
    assert status["settings"]["check_interval_hours"] == 24
    subprocess.run([bootstrap, "set", "check_interval_hours", "0"], env=env, check=True)
    settings = json.loads((private / "bootstrapper.json").read_text())
    assert settings["check_interval_hours"] == 0
    (private / "bootstrapper.json").write_text(json.dumps({"last_check": 100, "check_interval_hours": 24}))
    due = subprocess.check_output([ROOT / "package/bootstrap.py", "due", private, "101"], text=True).strip()
    assert due == "0"
    due = subprocess.check_output([ROOT / "package/bootstrap.py", "due", private, "86500"], text=True).strip()
    assert due == "1"

    subprocess.run([bootstrap, "set", "auto_update", "off"], env=env, check=True)
    first = subprocess.run([bootstrap], env=env, capture_output=True, text=True, check=True)
    assert (private / "first-run.initialized").exists()
    assert (release / "launched").read_text().strip() == "launched"
    assert "Setting up Roblox for the first time" in first.stderr

    checked = json.loads(subprocess.check_output([bootstrap, "check"], env=env, text=True))
    assert checked["latest_known"] == "version-abcd" and "settings" in checked

    # The UI is only a decision/progress front end. A fake helper proves the
    # core applies Skip this version and keeps the opaque URI out of output.
    fake_ui = Path(tmp) / "fake-ui"
    fake_ui.write_text("""#!/bin/sh
case "${2:-}" in
  --progress) cat >/dev/null; exit 0;;
  --dialog) exit 2;;
esac
exit 3
""")
    fake_ui.chmod(0o755)
    subprocess.run([bootstrap, "set", "auto_update", "ask"], env=env, check=True)
    (private / "bootstrapper.json").write_text(json.dumps({"auto_update": "ask", "check_interval_hours": 0}))
    skipped = subprocess.run([bootstrap], env={**env, "ROBLOX_MAC_UI_APP": str(fake_ui)},
                             capture_output=True, text=True, check=True)
    assert json.loads((private / "bootstrapper.json").read_text())["skipped_version"] == "version-abcd"
    assert "version-abcd" not in skipped.stdout

    subprocess.run([bootstrap, "set", "auto_update", "auto"], env=env, check=True)
    (private / "bootstrapper.json").write_text(json.dumps({"auto_update": "auto", "check_interval_hours": 0}))
    uri = "roblox://place/+%2B:opaque"
    updated = subprocess.run([bootstrap], env={**env, "ROBLOX_MAC_LAUNCH_URI": uri,
                                                "ROBLOX_MAC_URI_RECEIVED_AT": str(int(time.time()))},
                              capture_output=True, text=True, check=True)
    assert (release / "RobloxVersion/.version").read_text().strip() == "version-abcd"
    assert uri not in updated.stdout and uri not in updated.stderr
    assert not (private / "pending-uri").exists()

    (release / "RobloxVersion/.version").write_text("version-old\n")
    failed = subprocess.run([bootstrap, "update"], env={**env, "FAIL_UPDATE": "1", "ROBLOX_MAC_EVENTS": "1"},
                            capture_output=True, text=True)
    assert failed.returncode != 0
    assert (release / "RobloxVersion/.version").read_text().strip() == "version-old"
    assert '"phase":"first-run"' not in failed.stdout

    (private / "bootstrapper.json").write_text(json.dumps({"auto_update": "off", "check_interval_hours": 24}))
    notify_bin = Path(tmp) / "notify-bin"
    notify_bin.mkdir()
    calls = Path(tmp) / "notification.calls"
    for name, status in (("gdbus", 1), ("notify-send", 1), ("kdialog", 1), ("zenity", 0)):
        script = notify_bin / name
        script.write_text(f"#!/bin/sh\necho {name} >> '{calls}'\nexit {status}\n")
        script.chmod(0o755)
    running = subprocess.Popen([bootstrap], env={**env, "FAKE_LAUNCH_SLEEP": "2"},
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(.3)
    second = subprocess.run([bootstrap], env={**env, "PATH": str(notify_bin) + ":" + env["PATH"],
                                                "DBUS_SESSION_BUS_ADDRESS": "unix:fake",
                                                "ROBLOX_MAC_LAUNCH_URI": uri},
                            capture_output=True, text=True)
    running.wait(timeout=5)
    assert second.returncode == 0 and "already running" in second.stderr
    assert uri not in second.stdout and uri not in second.stderr
    assert calls.read_text().splitlines()[:4] == ["gdbus", "notify-send", "kdialog", "zenity"]
    assert uri not in calls.read_text()
print("PASS bootstrap settings/defaults/fake-clock scheduling, first-run marker, decision flow, rollback and one-lock launch")
