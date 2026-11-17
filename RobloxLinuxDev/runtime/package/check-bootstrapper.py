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
# The UI is bootstrapper-ui.c plus the files it includes from ui/.
ui_source = "\n".join(path.read_text() for path in [ROOT / "package/bootstrapper-ui.c", *sorted((ROOT / "package/ui").glob("*.[ch]"))])
assert "g_environ_unsetenv" in ui_source and '"LD_LIBRARY_PATH"' in ui_source
assert '"APPDIR"' in ui_source and 'g_spawn_async(state->root' in ui_source
assert '"setsid"' in ui_source and 'host_environment()' in ui_source
assert "bytes_done" in ui_source and "cancellable" in ui_source and "Cancel" in ui_source
assert "g_subprocess_communicate_utf8_async" in ui_source and "update-settings" in ui_source
assert "progress_delete" in ui_source and "ROBLOX_MAC_NO_PROGRESS" in ui_source
assert "Check for _port updates" in ui_source and "port-update-settings" in ui_source
assert "button.nav:checked" in ui_source and '"Updates", "update", "updates"' in ui_source
assert "_Check for updates" in ui_source and '"When updates are found", "section"' in ui_source
assert 'state->update = button("_Update now", "download", "primary")' in ui_source
assert "window_decoration" in ui_source and "cursor_appearance" in ui_source
assert "_Restore defaults" in ui_source
# The progress reader must drain the pipe before it honours a hang-up: the
# producer's final event and its close can arrive in the same poll().
progress_input = ui_source.split("static gboolean progress_input", 1)[1].split("static int run_progress", 1)[0]
assert progress_input.index("g_io_channel_read_line") < progress_input.index("progress_close")
fetch_source = (ROOT / "scripts/fetch-client.sh").read_text()
assert "Content-Length" in fetch_source or "content-length" in fetch_source
assert "ROBLOX_MAC_CANCEL_FILE" in fetch_source and "exit 125" in fetch_source
with tempfile.TemporaryDirectory(prefix="roblox bootstrap ") as tmpdir:
    tmp = Path(tmpdir)
    release = tmp / "release"
    release.mkdir()
    private = release / "DO_NOT_SHARE"
    private.mkdir()
    shutil.copy2(ROOT / "package/bootstrap.sh", release / "bootstrap.sh")
    shutil.copy2(ROOT / "package/bootstrap.py", release / "bootstrap.py")
    (release / "run.sh").write_text("#!/bin/sh\nexec \"$(dirname -- \"$0\")/RobloxLinux.AppImage\" \"$@\"\n")
    (release / "run.sh").chmod(0o755)
    (release / "RobloxVersion").mkdir()
    (release / "RobloxVersion/.version").write_text("version-old\n")
    (release / "RobloxVersion/.display-version").write_text("0.740.0.7400927\n")
    (release / "RobloxLinux.AppImage").write_text("""#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "${1:-}" in
  --shell) test ! -e /proc/$$/fd/9; mkdir -p "$HERE/DO_NOT_SHARE/prefix"; exit 0;;
  --client-version) sleep "${CHECK_DELAY:-0}"; [ "${FAIL_CHECK:-0}" = 1 ] && exit 1; echo version-abcd; exit 0;;
  # Without this, the display-version query fell through to "launched" below.
  --client-version-display) exit 1;;
  --uri) printf '%s\n' launched-with-uri > "$HERE/uri-launch"; exit 0;;
  *) printf '%s %s %s\n' launched "${ROBLOX_MAC_DECORATIONS:-}" "${ROBLOX_MAC_CURSOR_APPEARANCE:-}" > "$HERE/launched"; sleep "${FAKE_LAUNCH_SLEEP:-0}"; exit 0;;
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
    # Notifications and dialogs must never reach the desktop of whoever runs
    # the checks: these record instead (cases that test notify() itself put
    # their own recorders first in PATH).
    desktop_quiet_bin = tmp / "quiet-bin"
    desktop_quiet_bin.mkdir()
    for name in ("notify-send", "kdialog", "zenity", "gdbus"):
        (desktop_quiet_bin / name).write_text(f'#!/bin/sh\nprintf "%s\\n" "{name} $*" >> "{tmp / "quiet-notifications"}"\nexit 0\n')
        (desktop_quiet_bin / name).chmod(0o755)
    env = {**os.environ, "PATH": f"{desktop_quiet_bin}:{os.environ['PATH']}", "ROBLOX_MAC_UPDATE_CHECK_TIMEOUT": "2",
           "DBUS_SESSION_BUS_ADDRESS": "", "ROBLOX_MAC_NOTIFY_DBUS_SESSION_BUS_ADDRESS": ""}
    bootstrap = release / "bootstrap.sh"

    status = json.loads(subprocess.check_output([bootstrap, "status"], env=env, text=True))
    assert status["settings"]["auto_update"] == "ask"
    assert status["settings"]["check_interval_hours"] == 24
    assert status["settings"]["window_decoration"] == "auto"
    assert status["settings"]["cursor_appearance"] == "system"
    assert status["installed_display_version"] == "0.740.0.7400927"
    (private / "bootstrapper.json").write_text(json.dumps({"last_known_latest": "version-3bc33ee7ffad426f"}))
    mapped = json.loads(subprocess.check_output([bootstrap, "status"], env=env, text=True))
    assert mapped["latest_display_version"] == "0.741.0.7411056"
    event = json.loads(subprocess.check_output([ROOT / "package/bootstrap.py", "event", "downloading", "42", "Downloading",], text=True))
    assert event["percent"] == 42 and event["cancellable"] is True and event["bytes_done"] == 0
    subprocess.run([bootstrap, "set", "check_interval_hours", "0"], env=env, check=True)
    settings = json.loads((private / "bootstrapper.json").read_text())
    assert settings["check_interval_hours"] == 0
    (private / "bootstrapper.json").write_text(json.dumps({"last_check": 100, "check_interval_hours": 24}))
    due = subprocess.check_output([ROOT / "package/bootstrap.py", "due", private, "101"], text=True).strip()
    assert due == "0"
    due = subprocess.check_output([ROOT / "package/bootstrap.py", "due", private, "86500"], text=True).strip()
    assert due == "1"

    # Two writers at once must not lose each other's change: every "set" is
    # a read-modify-write of bootstrapper.json (the launcher's background
    # update check and the Settings window can write at the same time).
    race_dir = tmp / "settings-race"
    race_dir.mkdir()
    writers = [subprocess.Popen(["sh", "-c", f'for i in $(seq 1 60); do "{ROOT / "package/bootstrap.py"}" settings "{race_dir}" set {key} "{value}$i" || exit 1; done'])
               for key, value in (("skipped_version", "version-skip"), ("last_known_latest", "version-latest"))]
    assert all(writer.wait(timeout=120) == 0 for writer in writers)
    raced = json.loads((race_dir / "bootstrapper.json").read_text())
    assert raced["skipped_version"] == "version-skip60" and raced["last_known_latest"] == "version-latest60", raced
    # (The race test above runs before the general settings tests.)

    # Test setting appearance settings
    subprocess.run([bootstrap, "set", "window_decoration", "legacy"], env=env, check=True)
    assert subprocess.check_output([bootstrap, "get", "window_decoration"], env=env, text=True).strip() == "legacy"
    subprocess.run([bootstrap, "set", "cursor_appearance", "legacy"], env=env, check=True)
    assert subprocess.check_output([bootstrap, "get", "cursor_appearance"], env=env, text=True).strip() == "legacy"

    # Test invalid values rejected
    assert subprocess.run([bootstrap, "set", "window_decoration", "invalid_dec"], env=env).returncode != 0
    assert subprocess.run([bootstrap, "set", "cursor_appearance", "invalid_cur"], env=env).returncode != 0

    # Test reset-appearance
    subprocess.run([bootstrap, "reset-appearance"], env=env, check=True)
    assert subprocess.check_output([bootstrap, "get", "window_decoration"], env=env, text=True).strip() == "auto"
    assert subprocess.check_output([bootstrap, "get", "cursor_appearance"], env=env, text=True).strip() == "system"

    # Test backward compatibility: old settings file without appearance keys
    (private / "bootstrapper.json").write_text(json.dumps({"auto_update": "ask", "check_interval_hours": 12}))
    status_compat = json.loads(subprocess.check_output([bootstrap, "status"], env=env, text=True))
    assert status_compat["settings"]["window_decoration"] == "auto"
    assert status_compat["settings"]["cursor_appearance"] == "system"
    assert status_compat["settings"]["check_interval_hours"] == 12

    subprocess.run([bootstrap, "set", "auto_update", "off"], env=env, check=True)
    first = subprocess.run([bootstrap], env={**env, "ROBLOX_MAC_DECORATIONS": "gtk", "ROBLOX_MAC_CURSOR_APPEARANCE": "legacy"},
                           capture_output=True, text=True, check=True)
    assert (private / "first-run.initialized").exists()
    assert (release / "launched").read_text().strip() == "launched gtk legacy"
    assert "Setting up Roblox for the first time" in first.stderr

    checked = json.loads(subprocess.check_output([bootstrap, "check"], env=env, text=True))
    assert checked["latest_known"] == "version-abcd" and "settings" in checked

    # A Settings check can suppress the progress helper and still return the
    # authoritative status JSON for the asynchronous GTK callback.
    subprocess.run([bootstrap, "set", "check_interval_hours", "0"], env=env, check=True)
    no_progress = subprocess.run([bootstrap, "check"], env={**env, "ROBLOX_MAC_NO_PROGRESS": "1"},
                                 capture_output=True, text=True, check=True)
    assert json.loads(no_progress.stdout)["latest_known"] == "version-abcd"
    failed_check = subprocess.run([bootstrap, "check"], env={**env, "FAIL_CHECK": "1"},
                                  capture_output=True, text=True)
    assert failed_check.returncode == 1
    assert json.loads(failed_check.stdout)["latest_known"] == "version-abcd"

    # The handoff waits for the updater, opens Settings exactly once, and
    # preserves success, failure and cancellation status.
    handoff = tmp / "handoff"
    handoff.mkdir()
    (handoff / "DO_NOT_SHARE").mkdir()
    (handoff / "bootstrap.sh").write_text((ROOT / "package/bootstrap.sh").read_text())
    (handoff / "bootstrap.sh").chmod(0o755)
    (handoff / "bootstrap.py").write_text((ROOT / "package/bootstrap.py").read_text())
    handoff_run = handoff / "run.sh"
    handoff_run.write_text('''#!/bin/sh
set -eu
case "${1:-}" in
  update)
    echo update-start >> "$HANDOFF_LOG"
    sleep 0.15
    printf updater-exited > "$HANDOFF_DONE"
    case "${HANDOFF_RESULT:-0}" in 0) exit 0;; 125) exit 125;; *) exit 1;; esac
    ;;
  --settings)
    test -f "$HANDOFF_DONE"
    echo settings >> "$HANDOFF_LOG"
    exit 0
    ;;
  *) exit 2;;
esac
''')
    handoff_run.chmod(0o755)
    handoff_log = handoff / "handoff.log"
    handoff_done = handoff / "updater.done"
    handoff_env = {**env, "HANDOFF_LOG": str(handoff_log), "HANDOFF_DONE": str(handoff_done)}
    success = subprocess.run([handoff / "bootstrap.sh", "update-settings"], env=handoff_env, capture_output=True, text=True)
    assert success.returncode == 0 and handoff_log.read_text().splitlines() == ["update-start", "settings"]
    handoff_log.write_text(""); handoff_done.unlink()
    failed_handoff = subprocess.run([handoff / "bootstrap.sh", "update-settings"], env={**handoff_env, "HANDOFF_RESULT": "1"}, capture_output=True, text=True)
    assert failed_handoff.returncode == 1 and handoff_log.read_text().splitlines() == ["update-start", "settings"]
    handoff_log.write_text(""); handoff_done.unlink()
    cancelled_handoff = subprocess.run([handoff / "bootstrap.sh", "update-settings"], env={**handoff_env, "HANDOFF_RESULT": "125"}, capture_output=True, text=True)
    assert cancelled_handoff.returncode == 125 and handoff_log.read_text().splitlines() == ["update-start", "settings"]
    assert subprocess.run([handoff / "bootstrap.sh", "update-settings", "extra"], env=handoff_env).returncode == 2

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

    # B10 defect 4: the progress window must not cover the update prompt.
    # Before the prompt opens, the progress window is told to hide.
    prompt_log = tmp / "prompt-order.log"
    prompt_ui = Path(tmp) / "prompt-ui"
    prompt_ui.write_text("""#!/bin/sh
case "${2:-}" in
  --progress) while IFS= read -r line; do printf 'progress %s\\n' "$line" >> "$PROMPT_LOG"; done; exit 0;;
  --dialog) sleep 0.3; echo dialog >> "$PROMPT_LOG"; exit 1;;
esac
exit 3
""")
    prompt_ui.chmod(0o755)
    (private / "bootstrapper.json").write_text(json.dumps({"auto_update": "ask", "check_interval_hours": 0}))
    subprocess.run([bootstrap], env={**env, "ROBLOX_MAC_UI_APP": str(prompt_ui), "PROMPT_LOG": str(prompt_log)},
                   capture_output=True, text=True, check=True)
    order = prompt_log.read_text().splitlines()
    assert "dialog" in order, order
    before = [json.loads(line.split(" ", 1)[1]) for line in order[:order.index("dialog")] if line.startswith("progress ")]
    assert before and before[-1].get("hidden") is True, f"the progress window was not hidden before the prompt: {order}"

    # B10 flow 7 and 8: the prompt can cancel the launch (exit 4 from the GTK
    # prompt; Cancel or closing in kdialog/zenity). Nothing is updated, no
    # game starts, the progress window is told the launch was cancelled, and
    # the instance lock is free again. No answer is ever given by a timer.
    assert "DIALOG_TIMEOUT_SECONDS" not in ui_source and "30 seconds" not in ui_source
    bootstrap_source = (ROOT / "package/bootstrap.sh").read_text()
    ask_source = bootstrap_source.split("ask_update() {", 1)[1].split("\n}\n", 1)[0]
    assert "timeout" not in ask_source, "the update prompt still has a timer"
    cancel_ui = Path(tmp) / "prompt-cancel-ui"
    cancel_ui.write_text("""#!/bin/sh
case "${2:-}" in
  --progress) cat > "$PROMPT_LOG"; exit 0;;
  --dialog) exit 4;;
esac
exit 3
""")
    cancel_ui.chmod(0o755)
    def cancel_case(extra_env):
        (private / "bootstrapper.json").write_text(json.dumps({"auto_update": "ask", "check_interval_hours": 0}))
        (release / "RobloxVersion/.version").write_text("version-old\n")
        (release / "launched").unlink(missing_ok=True)
        prompt_log.write_text("")
        result = subprocess.run([bootstrap], env={**env, "PROMPT_LOG": str(prompt_log), **extra_env},
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == 125, (result.returncode, result.stderr)
        assert not (release / "launched").exists(), "a game was started after cancelling at the prompt"
        assert (release / "RobloxVersion/.version").read_text().strip() == "version-old"
        assert json.loads((private / "bootstrapper.json").read_text()).get("skipped_version", "") == ""
        lock = subprocess.run(["flock", "-n", str(private / "instance.lock"), "true"])
        assert lock.returncode == 0, "the instance lock is still held"
        return result
    cancel_case({"ROBLOX_MAC_UI_APP": str(cancel_ui)})
    phases = [json.loads(line)["phase"] for line in prompt_log.read_text().splitlines()]
    assert phases[-1] == "cancelled", phases
    # The fallbacks must never reach the desktop's real dialog or notification
    # tools: the PATH holds links to every other tool plus the fakes only.
    fallback_bin = tmp / "prompt-fallback-bin"
    fallback_bin.mkdir()
    for directory in env["PATH"].split(":"):
        if not directory or not Path(directory).is_dir():
            continue
        for tool in Path(directory).iterdir():
            if tool.name in ("kdialog", "zenity", "notify-send", "gdbus") or (fallback_bin / tool.name).exists():
                continue
            if tool.is_file() and os.access(tool, os.X_OK):
                (fallback_bin / tool.name).symlink_to(tool)
    for name, script in (("kdialog", "exit 2"), ("zenity", "exit 1")):
        for other in ("kdialog", "zenity"):
            (fallback_bin / other).unlink(missing_ok=True)
        (fallback_bin / name).write_text(f"#!/bin/sh\n{script}\n")
        (fallback_bin / name).chmod(0o755)
        cancel_case({"PATH": str(fallback_bin), "ROBLOX_MAC_UI_APP": "",
                     "DBUS_SESSION_BUS_ADDRESS": ""})
    # B10 flow 6: the update check must not delay the launch. A check that
    # has not answered within a short grace period no longer holds the game
    # back; it finishes in the background and its answer is offered at the
    # next start. A fast check still prompts on the same launch.
    timing_log = tmp / "timing-prompt.log"
    timing_ui = Path(tmp) / "timing-ui"
    timing_ui.write_text("""#!/bin/sh
case "${2:-}" in
  --progress) cat >/dev/null; exit 0;;
  --dialog) echo dialog >> "$TIMING_LOG"; exit 1;;
esac
exit 3
""")
    timing_ui.chmod(0o755)
    def timed_launch(settings, **extra):
        (private / "bootstrapper.json").write_text(json.dumps(settings))
        (release / "RobloxVersion/.version").write_text("version-old\n")
        (release / "launched").unlink(missing_ok=True)
        timing_log.write_text("")
        started = time.monotonic()
        result = subprocess.run([bootstrap], env={**env, "ROBLOX_MAC_UI_APP": str(timing_ui), "TIMING_LOG": str(timing_log),
                                                  "ROBLOX_MAC_UPDATE_CHECK_TIMEOUT": "8", **extra},
                                capture_output=True, text=True, timeout=60)
        elapsed = time.monotonic() - started
        assert result.returncode == 0 and (release / "launched").exists(), result.stderr
        return elapsed, timing_log.read_text().splitlines()
    fast_elapsed, prompts = timed_launch({"auto_update": "ask", "check_interval_hours": 0})
    assert prompts == ["dialog"], ("a fast check must still prompt on the same launch", prompts)
    slow_elapsed, prompts = timed_launch({"auto_update": "ask", "check_interval_hours": 0}, CHECK_DELAY="4")
    print(f"launch time: fast check {fast_elapsed:.2f}s, 4 s check {slow_elapsed:.2f}s")
    assert slow_elapsed < fast_elapsed + 1.5, f"a 4 s check delayed the launch: {slow_elapsed:.2f}s (fast {fast_elapsed:.2f}s)"
    assert prompts == [], prompts
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if json.loads((private / "bootstrapper.json").read_text()).get("last_known_latest") == "version-abcd":
            break
        time.sleep(0.1)
    else:
        raise AssertionError("the background check did not record its answer")
    recorded = json.loads((private / "bootstrapper.json").read_text())
    _, prompts = timed_launch({**recorded, "check_interval_hours": 24})
    assert prompts == ["dialog"], ("the update found in the background was not offered at the next start", prompts)
    assert not list(private.glob(".check*")), list(private.glob(".check*"))

    # zenity's extra button (Launch without updating) still launches.
    (fallback_bin / "zenity").write_text("#!/bin/sh\necho 'Launch without updating'\nexit 1\n")
    (private / "bootstrapper.json").write_text(json.dumps({"auto_update": "ask", "check_interval_hours": 0}))
    (release / "launched").unlink(missing_ok=True)
    launched = subprocess.run([bootstrap], env={**env, "PATH": str(fallback_bin), "ROBLOX_MAC_UI_APP": "",
                                                 "DBUS_SESSION_BUS_ADDRESS": ""}, capture_output=True, text=True, timeout=60)
    assert launched.returncode == 0 and (release / "launched").exists(), launched.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == "version-old"

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

    # A progress helper can cancel through the shared marker. The staged
    # update is removed and the previous client remains launchable.
    cancel_ui = Path(tmp) / "cancel-ui"
    cancel_ui.write_text("""#!/bin/sh
case "${2:-}" in
  --progress) touch "$ROBLOX_MAC_CANCEL_FILE"; cat >/dev/null; exit 4;;
esac
exit 3
""")
    cancel_ui.chmod(0o755)
    (private / "bootstrapper.json").write_text(json.dumps({"auto_update": "auto", "check_interval_hours": 0}))
    (release / "RobloxVersion/.version").write_text("version-old\n")
    cancelled = subprocess.run([bootstrap], env={**env, "ROBLOX_MAC_UI_APP": str(cancel_ui)}, capture_output=True, text=True)
    assert cancelled.returncode == 125
    assert (release / "RobloxVersion/.version").read_text().strip() == "version-old"
    assert not list(private.glob(".update.*"))

    # A progress consumer receives the shared newline-delimited event stream.
    event_log = tmp / "progress.events"
    event_ui = Path(tmp) / "event-ui"
    event_ui.write_text('''#!/bin/sh
case "${2:-}" in
  --progress) cat > "$PROGRESS_LOG"; exit 0;;
esac
exit 3
''')
    event_ui.chmod(0o755)
    (private / "bootstrapper.json").write_text(json.dumps({"auto_update": "auto", "check_interval_hours": 0}))
    (release / "RobloxVersion/.version").write_text("version-old\n")
    completed = subprocess.run([bootstrap, "update"], env={**env, "ROBLOX_MAC_UI_APP": str(event_ui), "PROGRESS_LOG": str(event_log)}, capture_output=True, text=True)
    assert completed.returncode == 0
    phases = [json.loads(line)["phase"] for line in event_log.read_text().splitlines()]
    assert phases[:2] == ["checking", "downloading"] and phases[-1] == "update"

    # Exercise the real bootstrapper -> updater -> run.sh descriptor handoff.
    # The fake AppImage emits increasing byte counts through the inherited
    # progress FIFO and refuses to run if either updater lock is still open.
    handoff_release = tmp / "progress-handoff"
    handoff_release.mkdir()
    handoff_private = handoff_release / "DO_NOT_SHARE"
    (handoff_private / "prefix").mkdir(parents=True)
    (handoff_private / "first-run.initialized").write_text("initialized\n")
    (handoff_private / "bootstrapper.json").write_text(
        json.dumps({"auto_update": "auto", "check_interval_hours": 0}))
    (handoff_release / "RobloxVersion/.version").parent.mkdir(parents=True)
    (handoff_release / "RobloxVersion/.version").write_text("version-old\n")
    shutil.copy2(ROOT / "package/bootstrap.sh", handoff_release / "bootstrap.sh")
    shutil.copy2(ROOT / "package/bootstrap.py", handoff_release / "bootstrap.py")
    shutil.copy2(ROOT / "package/update-roblox.sh", handoff_release / "update-roblox.sh")
    (handoff_release / "bootstrap.sh").chmod(0o755)
    (handoff_release / "update-roblox.sh").chmod(0o755)
    (handoff_release / "run.sh").write_text(
        '#!/bin/sh\nexec "$(dirname -- "$0")/RobloxLinux.AppImage" "$@"\n')
    (handoff_release / "run.sh").chmod(0o755)
    handoff_trace = tmp / "progress-handoff.trace"
    handoff_events = tmp / "progress-handoff.events"
    (handoff_release / "RobloxLinux.AppImage").write_text("""#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "${1:-}" in
  --bootstrapper-ui)
    [ "${2:-}" = --progress ] || exit 2
    cat > "$PROGRESS_LOG"
    exit 0
    ;;
  --client-version)
    printf 'version-abcd\\n'
    exit 0
    ;;
  --shell)
    test ! -e /proc/$$/fd/9
    mkdir -p "$HERE/DO_NOT_SHARE/prefix"
    exit 0
    ;;
  --download-client)
    test ! -e /proc/$$/fd/7
    test ! -e /proc/$$/fd/9
    test -e /proc/$$/fd/8
    printf 'download-locks-closed\\n' >> "$TRACE"
    printf '%s\\n' '{"phase":"downloading","percent":30,"message":"Downloading Roblox","bytes_done":10,"bytes_total":100,"cancellable":true}' >&8
    sleep 0.15
    printf '%s\\n' '{"phase":"downloading","percent":45,"message":"Downloading Roblox","bytes_done":40,"bytes_total":100,"cancellable":true}' >&8
    mkdir -p "$ROBLOX_MAC_DATA/RobloxPlayer.app/Contents/MacOS"
    printf '#!/bin/sh\\nexit 0\\n' > "$ROBLOX_MAC_DATA/RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
    chmod +x "$ROBLOX_MAC_DATA/RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
    printf 'version-abcd\\n' > "$ROBLOX_MAC_DATA/.version"
    exit 0
    ;;
  --debug)
    shift
    ;;
esac
if [ "${1:-}" = --prepare-shaders ]; then
    test ! -e /proc/$$/fd/7
    test -e /proc/$$/fd/8
    test ! -e /proc/$$/fd/9
    printf 'prepare-locks-closed\\n' >> "$TRACE"
    printf '%s\\n' '{"phase":"preparing","percent":80,"message":"Preparing shaders (5 of 10)","bytes_done":0,"bytes_total":0,"cancellable":true}' >&8
    mkdir -p "$(dirname -- "$ROBLOX_MAC_APP")/spv-cache-v1"
    printf '{}\\n' > "$(dirname -- "$ROBLOX_MAC_APP")/spv-cache-v1/report.json"
    exit 0
fi
exit 2
""")
    (handoff_release / "RobloxLinux.AppImage").chmod(0o755)
    handoff_result = subprocess.run(
        [handoff_release / "bootstrap.sh", "update"],
        env={**env, "ROBLOX_MAC_UI_APP": str(handoff_release / "RobloxLinux.AppImage"),
             "PROGRESS_LOG": str(handoff_events), "TRACE": str(handoff_trace)},
        capture_output=True, text=True)
    assert handoff_result.returncode == 0, handoff_result.stderr + handoff_result.stdout
    handoff_lines = [json.loads(line) for line in handoff_events.read_text().splitlines()]
    assert handoff_lines[0]["phase"] == "checking"
    assert any(event["phase"] == "downloading" and event["percent"] == 25
               for event in handoff_lines), handoff_lines
    byte_events = [event for event in handoff_lines
                   if event["phase"] == "downloading" and event["bytes_total"] == 100]
    assert [event["bytes_done"] for event in byte_events[:2]] == [10, 40], byte_events
    # George (e2e 2026-11-17): "Verifying" sat at 74 % during shader preparation.
    # The long step is labelled "Preparing shaders" and reports its own progress;
    # "Verifying" is the short check after it.
    phases = [event["phase"] for event in handoff_lines]
    assert "preparing" in phases and "verifying" in phases, phases
    assert phases.index("preparing") < phases.index("verifying"), phases
    assert {"phase": "preparing", "percent": 74} .items() <= next(e for e in handoff_lines if e["phase"] == "preparing").items(), handoff_lines
    assert any(e["message"] == "Preparing shaders (5 of 10)" for e in handoff_lines), handoff_lines
    assert handoff_lines[-1]["phase"] == "update"
    assert handoff_trace.read_text().splitlines() == [
        "download-locks-closed", "prepare-locks-closed"]
    assert (handoff_release / "RobloxVersion/.version").read_text().strip() == "version-abcd"

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

    # The progress window's exit status. Only 3 (the window could not be
    # created) is reported to the user, and nothing claims updating was skipped.
    quiet_bin = Path(tmp) / "quiet-notify-bin"
    quiet_bin.mkdir()
    for name in ("gdbus", "notify-send", "kdialog", "zenity"):
        (quiet_bin / name).write_text("#!/bin/sh\nexit 1\n")
        (quiet_bin / name).chmod(0o755)
    status_ui = Path(tmp) / "status-ui"
    status_ui.write_text('#!/bin/sh\ncase "${2:-}" in --progress) cat > /dev/null; exit "$UI_STATUS";; esac\nexit 3\n')
    status_ui.chmod(0o755)
    for code, expected in ((0, ""), (1, ""), (4, ""), (3, "The progress window could not be shown"),
                           (7, "progress window exited with status 7")):
        launch = subprocess.run([bootstrap], env={**env, "PATH": str(quiet_bin) + ":" + env["PATH"],
                                                  "ROBLOX_MAC_UI_APP": str(status_ui), "UI_STATUS": str(code)},
                                capture_output=True, text=True)
        assert launch.returncode == 0, (code, launch.stderr)
        assert "without updating" not in launch.stderr, (code, launch.stderr)
        assert ("progress window" in launch.stderr) == bool(expected) and expected in launch.stderr, (code, launch.stderr)

    # The real progress window, driven the way bootstrap.sh drives it: a FIFO
    # held read/write by the producer and closed right after the final event.
    ui_binary = Path(tmp) / "bootstrapper-ui"
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "gtk+-3.0", "json-glib-1.0"], capture_output=True, text=True)
    built = flags.returncode == 0 and subprocess.run(
        ["gcc", "-std=c11", "-O2", "-w", ROOT / "package/bootstrapper-ui.c", "-o", ui_binary, *flags.stdout.split()]).returncode == 0

    def progress_exit(chunks, pause=0.0):
        with tempfile.TemporaryDirectory(prefix="progress ") as state:
            fifo = Path(state) / "fifo"
            os.mkfifo(fifo)
            writer = os.open(fifo, os.O_RDWR)
            reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
            ui = subprocess.Popen([ui_binary, "--progress", state], stdin=reader, env=display_env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.close(reader)
            for chunk in chunks:
                os.write(writer, chunk)
                time.sleep(pause)
            os.close(writer)
            return ui.wait(timeout=20)

    def progress_line(phase):
        return (json.dumps({"phase": phase, "percent": 100, "message": phase, "bytes_done": 0, "bytes_total": 0,
                            "cancellable": False}, separators=(",", ":")) + "\n").encode()

    # The windows open on a private headless display, never on the tester's.
    import sys
    sys.path.insert(0, str(ROOT / "package"))
    from ui_harness import Display, Unavailable
    display = None
    try:
        display = Display().__enter__() if built else None
    except Unavailable:
        display = None
    display_env = display.env() if display else {}
    if not built:
        print("SKIP progress window exit status: GTK 3 / json-glib development files are not available")
    elif not display:
        print("SKIP progress window exit status: no headless GTK display (broadwayd)")
    elif progress_exit([]) == 3:
        print("SKIP progress window exit status: no display for GTK")
    else:
        launching = progress_line("launching")
        assert progress_exit([launching]) == 0
        assert progress_exit([progress_line("checking"), launching]) == 0
        assert progress_exit([progress_line("cancelled")]) == 4
        # A "failed" event now stays on screen until the user closes it:
        # check-bootstrapper-windows.py, check_progress_failure_stays_until_closed.
        assert progress_exit([progress_line("checking")]) == 1
        assert progress_exit([]) == 1
        # An event split across two writes is one event, not the end of the stream.
        assert progress_exit([launching[:20], launching[20:]], pause=0.2) == 0
        # End to end: a launch with nothing to update writes "launching" and
        # closes the pipe at once. That must not produce any notification.
        real_ui = Path(tmp) / "real-ui"
        real_ui.write_text(f'#!/bin/sh\n[ "${{2:-}}" = --progress ] || exit 3\nexec \'{ui_binary}\' --progress "$3"\n')
        real_ui.chmod(0o755)
        record_bin = Path(tmp) / "record-notify-bin"
        record_bin.mkdir()
        notified = Path(tmp) / "progress-notifications"
        for name in ("gdbus", "notify-send", "kdialog", "zenity"):
            (record_bin / name).write_text(f"#!/bin/sh\nprintf '%s\\n' \"$*\" >> '{notified}'\nexit 0\n")
            (record_bin / name).chmod(0o755)
        for _ in range(3):
            launch = subprocess.run([bootstrap], env=display.env({**env, "PATH": str(record_bin) + ":" + env["PATH"],
                                                      "DBUS_SESSION_BUS_ADDRESS": "unix:fake", "ROBLOX_MAC_UI_APP": str(real_ui)}),
                                    capture_output=True, text=True)
            assert launch.returncode == 0, launch.stderr
            assert not notified.exists(), notified.read_text()
            assert "progress window" not in launch.stderr, launch.stderr
    if display:
        display.__exit__(None, None, None)
print("PASS bootstrap settings/defaults/fake-clock scheduling, first-run marker, decision flow, rollback and one-lock launch")
