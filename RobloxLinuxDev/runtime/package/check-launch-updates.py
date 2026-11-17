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
  --client-version) [ -z "${LINGER:-}" ] || sleep "$LINGER" &
    echo client-check >> "$HERE/trace"; [ -z "${CHECK_ERROR:-}" ] || { echo "$CHECK_ERROR" >&2; exit 6; }; echo "${CLIENT_LATEST:-version-0000bb}"; exit 0;;
  --client-version-display) [ -z "${LINGER:-}" ] || sleep "$LINGER" &
    echo 0.748.0.7480000; exit 0;;
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
printf 'progress-fd=%s\n' "${ROBLOX_MAC_PROGRESS_FD:-}" >> "$HERE/trace"
printf 'update-final=%s\n' "${ROBLOX_MAC_UPDATE_FINAL:-1}" >> "$HERE/trace"
[ -z "${CLIENT_UPDATE_ERROR:-}" ] || { echo 'fetching client version-0000bb' >&2; echo "$CLIENT_UPDATE_ERROR" >&2; exit 1; }
printf '%s\n' "${CLIENT_LATEST:-version-0000bb}" > "$HERE/RobloxVersion/.version"
'''

INSTALLER = r'''import json, os, shutil, sys
here = os.path.dirname(os.path.abspath(__file__))
def trace(line):
    with open(os.path.join(here, "trace"), "a") as stream:
        stream.write(line + "\n")
if sys.argv[1] == "release-status":
    trace("port-check")
    import time; time.sleep(float(os.environ.get("PORT_DELAY", "0")))
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
  --progress)
    # Records every event; CANCEL_AFTER=N presses Cancel after N seconds.
    [ -z "${CANCEL_AFTER:-}" ] || { (sleep "$CANCEL_AFTER"; : > "$ROBLOX_MAC_CANCEL_FILE") & }
    cat >> "$TRACE.events"; exit 0;;
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
    (release / ".roblox-mac-port-version").write_text("v1.1.1\n")
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
        result, trace, games = launch(release, target, PORT_LATEST="v1.1.1")
        assert result.returncode == 0, result.stderr
        prompts = [l for l in trace if l.startswith("prompt")]
        # The latest client is shown by its readable version once known, else by its id.
        assert len(prompts) == 1 and prompts[0].split()[1] in ("client=version-0000aa/0.748.0.7480000", "client=version-0000aa/version-0000bb") \
            and prompts[0].endswith(" port=/"), trace
        expect_game(games, target, "v1.1.1", "version-0000bb")
        # Port update only: the launch restarts through the new run.sh.
        release = make_release(tmp, {"auto_update": "ask"})
        result, trace, games = launch(release, target, CLIENT_LATEST="version-0000aa", PORT_LATEST="v1.2.0")
        assert result.returncode == 0, result.stderr
        prompts = [l for l in trace if l.startswith("prompt")]
        assert prompts == ["prompt client=/ port=v1.1.1/v1.2.0"], trace
        assert "port-update" in trace and [l for l in trace if l.startswith("run.sh")][-1].startswith("run.sh port=v1.2.0"), trace
        expect_game(games, target, "v1.2.0", "version-0000aa")
        # Both: one prompt, both updates, one game.
        release = make_release(tmp, {"auto_update": "ask"})
        result, trace, games = launch(release, target, PORT_LATEST="v1.2.0")
        assert result.returncode == 0, result.stderr
        prompts = [l for l in trace if l.startswith("prompt")]
        assert len(prompts) == 1 and prompts[0].endswith("port=v1.1.1/v1.2.0") and "client=/" not in prompts[0], trace
        assert "client-update" in trace and "port-update" in trace, trace
        expect_game(games, target, "v1.2.0", "version-0000bb")


def check_skip_failed_auto_and_off(tmp):
    # Skip this version skips both listed updates; the next launch does not ask.
    release = make_release(tmp, {"auto_update": "ask"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.2.0", PROMPT_ANSWER="2")
    assert result.returncode == 0 and "port-update" not in trace and "client-update" not in trace, trace
    expect_game(games, "place", "v1.1.1", "version-0000aa")
    settings = json.loads((release / "DO_NOT_SHARE/bootstrapper.json").read_text())
    assert settings["skipped_version"] == "version-0000bb" and settings["skipped_port_version"] == "v1.2.0", settings
    (release / "trace").write_text("")
    result, trace, games = launch(release, "place", PORT_LATEST="v1.2.0")
    assert not [l for l in trace if l.startswith("prompt")], trace
    expect_game(games, "place", "v1.1.1", "version-0000aa")
    # A failed port update is reported and the game still starts on the old port.
    release = make_release(tmp, {"auto_update": "ask"})
    result, trace, games = launch(release, "link", CLIENT_LATEST="version-0000aa", PORT_LATEST="v1.2.0", PORT_UPDATE_RESULT="1")
    assert result.returncode == 0 and "port-update" in trace, (trace, result.stderr)
    assert "Port update failed" in result.stderr, result.stderr
    expect_game(games, "link", "v1.1.1", "version-0000aa")
    # Auto updates both without asking.
    release = make_release(tmp, {"auto_update": "auto"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.2.0")
    assert not [l for l in trace if l.startswith("prompt")] and "port-update" in trace and "client-update" in trace, trace
    expect_game(games, "place", "v1.2.0", "version-0000bb")
    # Off neither asks nor updates.
    release = make_release(tmp, {"auto_update": "off"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.2.0")
    assert not [l for l in trace if l.startswith("prompt")] and "port-update" not in trace and "client-update" not in trace, trace
    expect_game(games, "place", "v1.1.1", "version-0000aa")
    # A port check that fails does not stop the launch, and says why in the log.
    release = make_release(tmp, {"auto_update": "ask"})
    result, trace, games = launch(release, "place", CLIENT_LATEST="version-0000aa", PORT_LATEST="")
    assert not [l for l in trace if l.startswith("prompt")], trace
    expect_game(games, "place", "v1.1.1", "version-0000aa")
    log = (release / "DO_NOT_SHARE/launcher.log").read_text()
    assert "Could not check for port updates: could not reach GitHub (test)" in log, log


def check_port_offer_only_when_newer(tmp):
    """A port update is offered at launch only when the latest release is newer
    than a release-style installed version. A custom or development build (a
    commit id, "dev") is never offered a release, which could be a downgrade;
    an older latest release is not offered either."""
    cases = [("v1.1.1", "v1.2.0", True), ("v1.2.0", "v1.2.0", False), ("v1.10.0", "v1.9.9", False),
             ("v1.9.9", "v1.10.0", True), ("fc2ec72", "v1.2.0", False), ("dev", "v9.9.9", False),
             ("v1.2", "v1.2.0", False), ("v1.2.0", "v1.2", False)]
    for installed, latest, offered in cases:
        release = make_release(tmp, {"auto_update": "ask"})
        (release / ".roblox-mac-port-version").write_text(installed + "\n")
        result, trace, games = launch(release, "place", CLIENT_LATEST="version-0000aa", PORT_LATEST=latest)
        prompts = [l for l in trace if l.startswith("prompt")]
        assert bool(prompts) == offered, (installed, latest, prompts)
        assert len(games) == 1, (installed, latest, games)


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
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1",
                                  CLIENT_UPDATE_ERROR="curl: (6) Could not resolve host: setup.rbxcdn.com")
    expect(release, "Roblox update failed", "Could not resolve host: setup.rbxcdn.com", "installed client was kept")
    expect_game(games, "place", "v1.1.1", "version-0000aa")
    # A failed first-run setup.
    release = make_release(tmp, {"auto_update": "off"})
    (release / "DO_NOT_SHARE/first-run.initialized").unlink()
    result, trace, games = launch(release, "place", FIRST_RUN_ERROR="darling: could not set up the prefix (test)")
    assert result.returncode != 0 and not games, (result.returncode, games)
    expect(release, "first-run setup failed", "darling: could not set up the prefix (test)", "Start Roblox again")
    # A failed port update: the installer's "! " line is the reason.
    release = make_release(tmp, {"auto_update": "auto"})
    result, trace, games = launch(release, "place", CLIENT_LATEST="version-0000aa", PORT_LATEST="v1.2.0",
                                  PORT_UPDATE_RESULT="1", PORT_UPDATE_ERROR="Could not download the release: test reason.")
    expect(release, "Port update failed", "Could not download the release: test reason", "current release was kept")
    expect_game(games, "place", "v1.1.1", "version-0000aa")
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


def check_failed_checks_retry_after_an_hour(tmp):
    """A failed client or port lookup (offline, GitHub rate limit) is retried
    an hour later, not at every launch; after a successful one the normal
    interval applies. Settings' "Check for updates" always looks."""
    release = make_release(tmp, {"auto_update": "ask", "check_interval_hours": 24})
    settings_file = release / "DO_NOT_SHARE/bootstrapper.json"
    def lookups():
        trace = (release / "trace").read_text().splitlines()
        (release / "trace").write_text("")
        return [line for line in trace if line in ("client-check", "port-check")], trace
    # Both lookups fail; the launch goes on and records no successful check.
    result, trace, games = launch(release, "place", CHECK_ERROR="curl: (6) Could not resolve host (test)", PORT_LATEST="")
    assert len(games) == 1, trace
    done, trace = lookups()
    assert sorted(done) == ["client-check", "port-check"], trace
    settings = json.loads(settings_file.read_text())
    assert settings["last_check"] == 0 and settings["port_last_check"] == 0, settings
    # Within the hour neither lookup is repeated.
    result, trace, games = launch(release, "place", CHECK_ERROR="curl: (6) Could not resolve host (test)", PORT_LATEST="")
    done, trace = lookups()
    assert len(games) == 1 and done == [], trace
    # An hour later both are retried; they succeed and the updates are offered.
    settings = json.loads(settings_file.read_text())
    settings["last_attempt"] -= 3601; settings["port_last_attempt"] -= 3601
    settings_file.write_text(json.dumps(settings))
    result, trace, games = launch(release, "place", PORT_LATEST="v1.2.0", PROMPT_ANSWER="1")
    done, trace = lookups()
    assert sorted(done) == ["client-check", "port-check"], trace
    assert [line for line in trace if line.startswith("prompt")], trace
    settings = json.loads(settings_file.read_text())
    assert settings["last_check"] > 0 and settings["port_last_check"] > 0, settings
    # After a success the 24-hour interval applies, even an hour later.
    settings["last_attempt"] -= 3601; settings["port_last_attempt"] -= 3601
    settings_file.write_text(json.dumps(settings))
    result, trace, games = launch(release, "place", PORT_LATEST="v1.2.0", PROMPT_ANSWER="1")
    done, trace = lookups()
    assert len(games) == 1 and done == [], trace
    # Settings' "Check for updates" looks even when no check is due.
    env = {**os.environ, "PATH": f"{quiet_tools(release)}:{os.environ['PATH']}", "DBUS_SESSION_BUS_ADDRESS": "",
           "ROBLOX_MAC_NOTIFY_DBUS_SESSION_BUS_ADDRESS": "", "ROBLOX_MAC_NO_PROGRESS": "1", "CLIENT_LATEST": "version-0000cc"}
    checked = subprocess.run([release / "bootstrap.sh", "check"], env=env, capture_output=True, text=True, timeout=60)
    done, trace = lookups()
    assert checked.returncode == 0 and done == ["client-check"], (trace, checked.stderr)
    assert json.loads(checked.stdout)["latest_known"] == "version-0000cc", checked.stdout


def check_no_client_is_installed_or_not_started(tmp):
    """George (2026-11-15): with no Roblox client installed the launch asks
    only to install it (Install / Cancel), whatever the update mode or a
    skipped version, and never starts a client that is not there."""
    def no_client(settings):
        release = make_release(tmp, settings)
        (release / "RobloxVersion/.version").unlink()
        return release
    def prompts(trace):
        return [line for line in trace if line.startswith("prompt")]
    # Ask, Off and a skipped version all show the install prompt; Install installs and plays.
    for settings in ({"auto_update": "ask"}, {"auto_update": "off"},
                     {"auto_update": "ask", "skipped_version": "version-0000bb"}):
        release = no_client(settings)
        result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1", PROMPT_ANSWER="0")
        assert prompts(trace) == ["prompt client=/0.748.0.7480000 port=/"], (settings, trace)
        assert "client-update" in trace, (settings, trace)
        expect_game(games, "place", "v1.1.1", "version-0000bb")
    # Review 2026-11-17: a version left behind (an old .display-version, old
    # diagnostics) must not turn the Install prompt back into an update prompt.
    release = no_client({"auto_update": "ask"})
    (release / "RobloxVersion/.display-version").write_text("0.700.0.7000001")
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1", PROMPT_ANSWER="0")
    assert prompts(trace) == ["prompt client=/0.748.0.7480000 port=/"], trace
    # Off means no updates: a port release recorded earlier is not installed along with the client.
    release = no_client({"auto_update": "off", "port_latest": "v1.2.0"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.2.0", PROMPT_ANSWER="0")
    assert prompts(trace) == ["prompt client=/0.748.0.7480000 port=/"] and "port-update" not in trace, trace
    expect_game(games, "place", "v1.1.1", "version-0000bb")
    # Auto installs without asking.
    release = no_client({"auto_update": "auto"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1")
    assert not prompts(trace) and "client-update" in trace, trace
    expect_game(games, "place", "v1.1.1", "version-0000bb")
    # The lookup runs even when no check is due.
    release = no_client({"auto_update": "ask", "check_interval_hours": 24, "last_check": int(time.time()),
                         "last_known_latest": "version-0000aa"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1", PROMPT_ANSWER="0")
    assert "client-check" in trace and prompts(trace) == ["prompt client=/0.748.0.7480000 port=/"], trace
    # Cancel, or any answer but Install, starts nothing.
    for answer in ("4", "1", "2"):
        release = no_client({"auto_update": "ask"})
        result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1", PROMPT_ANSWER=answer)
        assert result.returncode == 125 and not games and "client-update" not in trace, (answer, trace)
        settings = json.loads((release / "DO_NOT_SHARE/bootstrapper.json").read_text())
        assert settings["skipped_version"] == "", (answer, settings)
    # A failed install says so; nothing starts.
    release = no_client({"auto_update": "auto"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1",
                                  CLIENT_UPDATE_ERROR="curl: (6) Could not resolve host: setup.rbxcdn.com")
    assert result.returncode == 1 and not games, trace
    sent = [line for line in notifications(release) if "Roblox install failed" in line]
    assert sent and "Could not resolve host: setup.rbxcdn.com" in sent[-1] and "was kept" not in sent[-1], notifications(release)
    # A failed lookup says Roblox is not installed and why; nothing starts.
    release = no_client({"auto_update": "ask"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1", CHECK_ERROR="curl: (6) Could not resolve host (test)")
    assert result.returncode == 1 and not games and not prompts(trace), trace
    sent = [line for line in notifications(release) if "Roblox is not installed" in line]
    assert sent and "Could not resolve host (test)" in sent[-1], notifications(release)


def check_progress_fd_only_with_a_window(tmp):
    """The updater is told about the progress pipe (FD 8) only when the window's
    pipe is open. Without a window the number is free, and a child that wrote
    to it would write into a pipe, socket or file of its own (2026-11-17)."""
    release = make_release(tmp, {"auto_update": "auto"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1")
    assert "progress-fd=8" in trace, trace
    release = make_release(tmp, {"auto_update": "auto"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1", ROBLOX_MAC_UI_APP="")
    assert "client-update" in trace and "progress-fd=" in trace, trace
    expect_game(games, "place", "v1.1.1", "version-0000bb")


def check_lingering_appimage_daemon_does_not_delay(tmp):
    """The AppImage runtime's FUSE daemon outlives each call and keeps the
    call's stdout open for about 6 s (measured 2026-11-17). Output read through
    a pipe waited for it: "Checking for updates" took 13.5 s and the 1 s launch
    grace never got its answer. The fake leaves a child holding stdout 6 s."""
    release = make_release(tmp, {"auto_update": "ask"})
    started = time.monotonic()
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1", PROMPT_ANSWER="1", LINGER="6",
                                  ROBLOX_MAC_UPDATE_CHECK_GRACE="2")
    elapsed = time.monotonic() - started
    assert [l for l in trace if l.startswith("prompt")], ("the check missed the launch", trace)
    assert elapsed < 5, f"launch took {elapsed:.1f} s"
    expect_game(games, "place", "v1.1.1", "version-0000aa")


def events_of(release):
    path = release / "trace.events"
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()] if path.exists() else []


def check_window_stays_until_the_last_step(tmp):
    """Review 2026-11-17: "update" closes the progress window. In a launch,
    more follows (a port update, the launch itself), so the client update ends
    with a step that keeps the window; only Settings' "Update now" ends with
    "update"."""
    for port_latest in ("v1.2.0", "v1.1.1"):
        release = make_release(tmp, {"auto_update": "auto"})
        result, trace, games = launch(release, "place", PORT_LATEST=port_latest)
        assert "client-update" in trace and "update-final=0" in trace, (port_latest, trace)
        events = events_of(release)
        phases = [e["phase"] for e in events]
        done = phases.index("complete")
        # The update is installed by then: Cancel must not be offered (review 2026-11-17).
        assert events[done]["cancellable"] is False, events[done]
        assert "update" not in phases[:done], (port_latest, phases)
        if port_latest == "v1.1.1":
            assert "update" not in phases and phases[-1] == "launching", phases
        else:
            # The port update runs in the same window; its own "update" ends that
            # window only because the launcher then restarts with a new one.
            port_started = next(i for i, e in enumerate(events) if i > done and e["phase"] == "checking")
            assert "update" not in phases[done:port_started], phases
        assert len(games) == 1, (port_latest, trace)


def check_cancel_while_checking_is_honoured(tmp):
    """Review 2026-11-17: Cancel pressed while "Checking for updates" waited for
    the port lookup was ignored and the game started."""
    release = make_release(tmp, {"auto_update": "ask", "check_interval_hours": 24, "last_check": int(time.time()),
                                 "last_known_latest": "version-0000aa"})
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1", PORT_DELAY="3", CANCEL_AFTER="0.5",
                                  ROBLOX_MAC_UPDATE_CHECK_GRACE="5")
    assert result.returncode == 125 and not games, (result.returncode, trace)


def check_failed_recovery_is_reported(tmp):
    """Review 2026-11-17: an interrupted update that left no usable client was
    reported on stderr only; a launch from the desktop showed nothing."""
    release = make_release(tmp, {"auto_update": "ask"})
    (release / ".update-work").mkdir()
    (release / ".update-work/TXN").write_text("committed version-0000bb 1\n")
    result, trace, games = launch(release, "place", PORT_LATEST="v1.1.1")
    assert result.returncode != 0 and not games, (result.returncode, trace)
    sent = [line for line in notifications(release) if "Roblox could not start" in line]
    assert sent and "interrupted update" in sent[-1], notifications(release)
    assert (release / ".update-work/TXN").exists(), "the evidence must be kept"


CHECKS = [check_updates_end_in_the_requested_game, check_skip_failed_auto_and_off, check_failures_say_what_why_and_where,
          check_port_offer_only_when_newer, check_failed_checks_retry_after_an_hour,
          check_no_client_is_installed_or_not_started, check_progress_fd_only_with_a_window,
          check_lingering_appimage_daemon_does_not_delay, check_window_stays_until_the_last_step,
          check_cancel_while_checking_is_honoured, check_failed_recovery_is_reported]


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
