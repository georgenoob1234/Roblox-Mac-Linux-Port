#!/usr/bin/env python3
"""A launch that finds updates ends in the game that was asked for.

B10 flow 9: port updates are offered at launch like client updates, in one
prompt with the client update (George's answer f), skippable (answer b),
following the same auto/ask/off setting.
B10 flow 10: after a client update, a port update (which replaces the running
scripts, so the launch restarts through the new run.sh), or both, the game is
started once, with the place id or the roblox: link it was asked for.

Runs the real bootstrap.sh and bootstrap.py in a fake release whose AppImage,
client updater, port installer and prompt are small scripts that record what
they were asked to do. No network, no real install, no window.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

PACKAGE = Path(__file__).resolve().parent

APPIMAGE = r'''#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "${1:-}" in
  --shell) [ -z "${FIRST_RUN_ERROR:-}" ] || { echo "$FIRST_RUN_ERROR" >&2; exit 1; }; exit 0;;
  --client-version) [ -z "${CHECK_ERROR:-}" ] || { echo "$CHECK_ERROR" >&2; exit 6; }; echo "${CLIENT_LATEST:-version-0000bb}"; exit 0;;
  --client-version-display) echo 0.748.0.7480000; exit 0;;
  --uri) shift; printf 'game port=%s client=%s uri=%s args=%s\n' "$(cat "$HERE/.roblox-mac-port-version")" "$(cat "$HERE/RobloxVersion/.version")" "$1" "$*" >> "$HERE/trace"; exit 0;;
  *) printf 'game port=%s client=%s args=%s\n' "$(cat "$HERE/.roblox-mac-port-version")" "$(cat "$HERE/RobloxVersion/.version")" "$*" >> "$HERE/trace"; exit 0;;
esac
'''

RUN_SH = r'''#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "${1:-}" in
  --shell|--client-version|--client-version-display) exec "$HERE/RobloxLinux.AppImage" "$@";;
esac
printf 'run.sh port=%s args=%s\n' "$(cat "$HERE/.roblox-mac-port-version")" "$*" >> "$HERE/trace"
"$HERE/bootstrap.sh" "$@"
'''

UPDATER = r'''#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
echo 'client-update' >> "$HERE/trace"
[ -z "${CLIENT_UPDATE_ERROR:-}" ] || { echo 'fetching client version-0000bb' >&2; echo "$CLIENT_UPDATE_ERROR" >&2; exit 1; }
printf '%s\n' "${CLIENT_LATEST:-version-0000bb}" > "$HERE/RobloxVersion/.version"
'''

INSTALLER = r'''import json, os, shutil, sys
here = os.path.dirname(os.path.abspath(__file__))
def trace(line):
    with open(os.path.join(here, "trace"), "a") as stream:
        stream.write(line + "\n")
if sys.argv[1] == "release-status":
    latest = os.environ.get("PORT_LATEST", "")
    if not latest:
        print("! Could not query the latest GitHub release: could not reach GitHub (test).", file=sys.stderr)
        sys.exit(1)
    print(json.dumps({"installed_version": open(os.path.join(here, ".roblox-mac-port-version")).read().strip(),
                      "latest_version": latest}))
    sys.exit(0)
if sys.argv[1] == "update":
    trace("port-update")
    result = int(os.environ.get("PORT_UPDATE_RESULT", "0"))
    if result not in (0, 3, 125):
        print("! " + os.environ.get("PORT_UPDATE_ERROR", "Port update failed (test)."), file=sys.stderr)
    if result == 0:
        # Like the real installer: the scripts are replaced by new files.
        for name in ("run.sh", "bootstrap.sh"):
            path = os.path.join(here, name)
            shutil.copy2(path, path + ".new")
            os.replace(path + ".new", path)
        with open(os.path.join(here, ".roblox-mac-port-version"), "w") as stream:
            stream.write(os.environ["PORT_LATEST"] + "\n")
    sys.exit(result)
sys.exit(2)
'''

PROMPT_UI = r'''#!/bin/sh
case "${2:-}" in
  --progress) cat >/dev/null; exit 0;;
  --dialog) shift 2; printf 'prompt client=%s/%s port=%s/%s\n' "${2:-}" "${1:-}" "${4:-}" "${3:-}" >> "$TRACE"; exit "${PROMPT_ANSWER:-0}";;
esac
exit 3
'''


def make_release(tmp, settings):
    release = Path(tempfile.mkdtemp(prefix="release ", dir=tmp))
    for name in ("bootstrap.sh", "bootstrap.py"):
        shutil.copy2(PACKAGE / name, release / name)
    for name, text in (("RobloxLinux.AppImage", APPIMAGE), ("run.sh", RUN_SH), ("update-roblox.sh", UPDATER)):
        (release / name).write_text(text)
        (release / name).chmod(0o755)
    (release / "installer.py").write_text(INSTALLER)
    (release / ".roblox-mac-port-version").write_text("v-old\n")
    (release / "RobloxVersion").mkdir()
    (release / "RobloxVersion/.version").write_text("version-0000aa\n")
    private = release / "DO_NOT_SHARE"
    (private / "prefix").mkdir(parents=True)
    (private / "first-run.initialized").write_text("initialized\n")
    (private / "bootstrapper.json").write_text(json.dumps({"check_interval_hours": 0, **settings}))
    ui = release / "prompt-ui"
    ui.write_text(PROMPT_UI)
    ui.chmod(0o755)
    (release / "trace").write_text("")
    return release


def quiet_tools(release):
    """Notification and dialog tools that record instead of reaching the desktop."""
    tools = release / "quiet-bin"
    if not tools.exists():
        tools.mkdir()
        for name in ("notify-send", "kdialog", "zenity", "gdbus"):
            (tools / name).write_text(f'#!/bin/sh\nprintf "%s\\n" "{name} $*" >> "{release / "notifications"}"\nexit 0\n')
            (tools / name).chmod(0o755)
    return tools


def launch(release, target, **env_extra):
    env = {**os.environ, "ROBLOX_MAC_UI_APP": str(release / "prompt-ui"), "TRACE": str(release / "trace"),
           "PATH": f"{quiet_tools(release)}:{os.environ['PATH']}", "DBUS_SESSION_BUS_ADDRESS": "",
           "ROBLOX_MAC_NOTIFY_DBUS_SESSION_BUS_ADDRESS": "",
           "ROBLOX_MAC_UPDATE_CHECK_TIMEOUT": "5", "ROBLOX_MAC_UPDATE_CHECK_GRACE": "5", **env_extra}
    args = []
    if target == "place":
        args = ["--place-id", "1818"]
    else:
        env.update(ROBLOX_MAC_LAUNCH_URI="roblox://experiences/start?placeId=1818", ROBLOX_MAC_URI_RECEIVED_AT=str(int(time.time())))
    result = subprocess.run([release / "run.sh", *args], env=env, capture_output=True, text=True, timeout=60)
    trace = (release / "trace").read_text().splitlines()
    games = [line for line in trace if line.startswith("game ")]
    return result, trace, games


def expect_game(games, target, port, client):
    assert len(games) == 1, ("the game must start exactly once", games)
    game = games[0]
    assert f"port={port} client={client}" in game, (game, port, client)
    if target == "place":
        assert game.endswith("args=--place-id 1818"), game
    else:
        assert "uri=roblox://experiences/start?placeId=1818" in game, game


def check_updates_end_in_the_requested_game(tmp):
    for target in ("place", "link"):
        # Client update only.
        release = make_release(tmp, {"auto_update": "ask"})
        result, trace, games = launch(release, target, PORT_LATEST="v-old")
        assert result.returncode == 0, result.stderr
        prompts = [l for l in trace if l.startswith("prompt")]
        # The latest client is shown by its readable version once known, else by its id.
        assert len(prompts) == 1 and prompts[0].split()[1] in ("client=version-0000aa/0.748.0.7480000", "client=version-0000aa/version-0000bb") \
            and prompts[0].endswith(" port=/"), trace
        expect_game(games, target, "v-old", "version-0000bb")
        # Port update only: the launch restarts through the new run.sh.
        release = make_release(tmp, {"auto_update": "ask"})
        result, trace, games = launch(release, target, CLIENT_LATEST="version-0000aa", PORT_LATEST="v-new")
        assert result.returncode == 0, result.stderr
        prompts = [l for l in trace if l.startswith("prompt")]
        assert prompts == ["prompt client=/ port=v-old/v-new"], trace
        assert "port-update" in trace and [l for l in trace if l.startswith("run.sh")][-1].startswith("run.sh port=v-new"), trace
        expect_game(games, target, "v-new", "version-0000aa")
        # Both: one prompt, both updates, one game.
        release = make_release(tmp, {"auto_update": "ask"})
        result, trace, games = launch(release, target, PORT_LATEST="v-new")
        assert result.returncode == 0, result.stderr
        prompts = [l for l in trace if l.startswith("prompt")]
        assert len(prompts) == 1 and prompts[0].endswith("port=v-old/v-new") and "client=/" not in prompts[0], trace
        assert "client-update" in trace and "port-update" in trace, trace
        expect_game(games, target, "v-new", "version-0000bb")


def check_skip_failed_auto_and_off(tmp):
    # Skip this version skips both listed updates; the next launch does not ask.
    release = make_release(tmp, {"auto_update": "ask"})
    result, trace, games = launch(release, "place", PORT_LATEST="v-new", PROMPT_ANSWER="2")
    assert result.returncode == 0 and "port-update" not in trace and "client-update" not in trace, trace
    expect_game(games, "place", "v-old", "version-0000aa")
    settings = json.loads((release / "DO_NOT_SHARE/bootstrapper.json").read_text())
    assert settings["skipped_version"] == "version-0000bb" and settings["skipped_port_version"] == "v-new", settings
    (release / "trace").write_text("")
    result, trace, games = launch(release, "place", PORT_LATEST="v-new")
    assert not [l for l in trace if l.startswith("prompt")], trace
    expect_game(games, "place", "v-old", "version-0000aa")
    # A failed port update is reported and the game still starts on the old port.
    release = make_release(tmp, {"auto_update": "ask"})
    result, trace, games = launch(release, "link", CLIENT_LATEST="version-0000aa", PORT_LATEST="v-new", PORT_UPDATE_RESULT="1")
    assert result.returncode == 0 and "port-update" in trace, (trace, result.stderr)
    assert "Port update failed" in result.stderr, result.stderr
    expect_game(games, "link", "v-old", "version-0000aa")
    # Auto updates both without asking.
    release = make_release(tmp, {"auto_update": "auto"})
    result, trace, games = launch(release, "place", PORT_LATEST="v-new")
    assert not [l for l in trace if l.startswith("prompt")] and "port-update" in trace and "client-update" in trace, trace
    expect_game(games, "place", "v-new", "version-0000bb")
    # Off neither asks nor updates.
    release = make_release(tmp, {"auto_update": "off"})
    result, trace, games = launch(release, "place", PORT_LATEST="v-new")
    assert not [l for l in trace if l.startswith("prompt")] and "port-update" not in trace and "client-update" not in trace, trace
    expect_game(games, "place", "v-old", "version-0000aa")
    # A port check that fails does not stop the launch.
    release = make_release(tmp, {"auto_update": "ask"})
    result, trace, games = launch(release, "place", CLIENT_LATEST="version-0000aa", PORT_LATEST="")
    assert not [l for l in trace if l.startswith("prompt")], trace
    expect_game(games, "place", "v-old", "version-0000aa")


def notifications(release):
    path = release / "notifications"
    return path.read_text().splitlines() if path.exists() else []


def check_failures_say_what_why_and_where(tmp):
    """B10 defect 2: every error says what failed, why, what the user can
    do, and where the log is."""
    def expect(release, *parts):
        log = release / "DO_NOT_SHARE" / "launcher.log"
        sent = [line for line in notifications(release) if parts[0] in line]
        assert sent, (parts[0], notifications(release))
        for part in parts[1:] + (str(log),):
            assert part in sent[-1], (part, sent[-1])
        assert log.exists() and parts[1] in log.read_text(), (parts[1], log.read_text() if log.exists() else "no log")
    # A failed client update: the updater's own last error line is the reason.
    release = make_release(tmp, {"auto_update": "auto"})
    result, trace, games = launch(release, "place", PORT_LATEST="v-old",
                                  CLIENT_UPDATE_ERROR="curl: (6) Could not resolve host: setup.rbxcdn.com")
    expect(release, "Roblox update failed", "Could not resolve host: setup.rbxcdn.com", "installed client was kept")
    expect_game(games, "place", "v-old", "version-0000aa")
    # A failed first-run setup.
    release = make_release(tmp, {"auto_update": "off"})
    (release / "DO_NOT_SHARE/first-run.initialized").unlink()
    result, trace, games = launch(release, "place", FIRST_RUN_ERROR="darling: could not set up the prefix (test)")
    assert result.returncode != 0 and not games, (result.returncode, games)
    expect(release, "first-run setup failed", "darling: could not set up the prefix (test)", "Start Roblox again")
    # A failed port update: the installer's "! " line is the reason.
    release = make_release(tmp, {"auto_update": "auto"})
    result, trace, games = launch(release, "place", CLIENT_LATEST="version-0000aa", PORT_LATEST="v-new",
                                  PORT_UPDATE_RESULT="1", PORT_UPDATE_ERROR="Could not download the release: test reason.")
    expect(release, "Port update failed", "Could not download the release: test reason", "current release was kept")
    expect_game(games, "place", "v-old", "version-0000aa")
    # A failed update check keeps its reason for the Settings page.
    release = make_release(tmp, {"auto_update": "ask"})
    env = {**os.environ, "PATH": f"{quiet_tools(release)}:{os.environ['PATH']}", "DBUS_SESSION_BUS_ADDRESS": "",
           "ROBLOX_MAC_NOTIFY_DBUS_SESSION_BUS_ADDRESS": "", "ROBLOX_MAC_NO_PROGRESS": "1",
           "CHECK_ERROR": "curl: (28) Connection timed out after 8001 milliseconds"}
    checked = subprocess.run([release / "bootstrap.sh", "check"], env=env, capture_output=True, text=True, timeout=60)
    assert checked.returncode == 1, checked.stderr
    status = json.loads(checked.stdout)
    assert status["settings"]["last_check_error"] == "Connection timed out after 8001 milliseconds", status["settings"]
    env.pop("CHECK_ERROR")
    checked = subprocess.run([release / "bootstrap.sh", "check"], env=env, capture_output=True, text=True, timeout=60)
    assert checked.returncode == 0 and json.loads(checked.stdout)["settings"]["last_check_error"] == "", checked.stdout


CHECKS = [check_updates_end_in_the_requested_game, check_skip_failed_auto_and_off, check_failures_say_what_why_and_where]


def main():
    failed = 0
    with tempfile.TemporaryDirectory(prefix="launch updates ") as tmp:
        for check in CHECKS:
            try:
                check(tmp)
                print(f"ok   {check.__name__}")
            except AssertionError as error:
                failed += 1
                print(f"FAIL {check.__name__}: {error}")
    if failed:
        sys.exit(f"check-launch-updates: {failed} of {len(CHECKS)} failed")
    print(f"check-launch-updates: {len(CHECKS)} passed")


if __name__ == "__main__":
    main()
