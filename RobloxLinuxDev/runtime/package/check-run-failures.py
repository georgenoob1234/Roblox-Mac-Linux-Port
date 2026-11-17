#!/usr/bin/env python3
"""B6: run.sh's own failures, before bootstrap.sh starts, must reach a user
who launched from the desktop (no terminal): a notification that says what
failed, why and what to do. Notifications are recorded by fake tools on the
PATH and never reach the desktop."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def make_release(tmp):
    root = Path(tempfile.mkdtemp(prefix="install ", dir=tmp))
    release = root / "release"
    release.mkdir()
    shutil.copy2(HERE / "run.sh", release / "run.sh")
    (release / "bootstrap.sh").write_text('#!/bin/sh\necho started >> "$(dirname "$0")/trace"\n')
    (release / "bootstrap.sh").chmod(0o755)
    (release / "RobloxLinux.AppImage").write_text("#!/bin/sh\nexit 0\n")
    (release / "RobloxLinux.AppImage").chmod(0o755)
    tools = root / "tools"
    tools.mkdir()
    for name in ("notify-send", "kdialog", "zenity", "gdbus"):
        (tools / name).write_text(f'#!/bin/sh\nprintf "%s\\n" "{name} $*" >> "{root / "notifications"}"\n')
        (tools / name).chmod(0o755)
    return root, release


def run(root, release):
    env = {**os.environ, "PATH": f"{root / 'tools'}:{os.environ['PATH']}", "DBUS_SESSION_BUS_ADDRESS": "",
           "XDG_STATE_HOME": str(root / "state"), "XDG_RUNTIME_DIR": str(root / "runtime")}
    (root / "runtime").mkdir(exist_ok=True, mode=0o700)
    # No terminal on stderr, like a launch from the desktop.
    result = subprocess.run(["sh", str(release / "run.sh")], env=env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)
    notes = (root / "notifications").read_text() if (root / "notifications").exists() else ""
    return result, notes


def check_data_folder_not_creatable(tmp):
    if os.geteuid() == 0:
        print("skip check_data_folder_not_creatable: root can write anywhere")
        return
    root, release = make_release(tmp)
    release.chmod(0o555)
    try:
        result, notes = run(root, release)
    finally:
        release.chmod(0o755)
    assert result.returncode != 0, result.stderr
    assert not (release / "trace").exists(), "bootstrap.sh started"
    assert "Roblox could not start" in notes, (notes, result.stderr)
    for part in (str(release / "DO_NOT_SHARE"), "Permission denied", "writable"):
        assert part in notes, (part, notes)


def check_port_repair_without_helper(tmp):
    root, release = make_release(tmp)
    (root / ".roblox-mac-port-txn-test").mkdir()
    result, notes = run(root, release)
    assert result.returncode != 0 and not (release / "trace").exists(), result.stderr
    assert "Roblox could not start" in notes, (notes, result.stderr)
    for part in ("interrupted port update", "recovery helper is missing", "Reinstall"):
        assert part in notes, (part, notes)


def check_port_repair_failure_says_why(tmp):
    root, release = make_release(tmp)
    (root / ".roblox-mac-port-txn-test").mkdir()
    (release / "installer.py").write_text(
        'import sys\nprint("! Could not restore the backup: test reason.", file=sys.stderr)\nsys.exit(1)\n')
    result, notes = run(root, release)
    assert result.returncode != 0 and not (release / "trace").exists(), result.stderr
    for part in ("Roblox could not start", "interrupted port update", "Could not restore the backup: test reason",
                 str(release / "DO_NOT_SHARE" / "launcher.log")):
        assert part in notes, (part, notes)
    log = (release / "DO_NOT_SHARE" / "launcher.log").read_text()
    assert "Could not restore the backup: test reason" in log, log


CHECKS = [check_data_folder_not_creatable, check_port_repair_without_helper, check_port_repair_failure_says_why]


def main():
    failed = 0
    with tempfile.TemporaryDirectory(prefix="run failures ") as tmp:
        for check in CHECKS:
            try:
                check(tmp)
                print(f"ok   {check.__name__}")
            except AssertionError as error:
                failed += 1
                print(f"FAIL {check.__name__}: {error}")
    if failed:
        sys.exit(f"check-run-failures: {failed} of {len(CHECKS)} failed")
    print(f"check-run-failures: {len(CHECKS)} passed")


if __name__ == "__main__":
    main()
