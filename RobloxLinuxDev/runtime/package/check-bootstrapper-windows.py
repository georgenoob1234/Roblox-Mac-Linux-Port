#!/usr/bin/env python3
"""Open bootstrapper-ui's real windows on a private headless display and check
what they show and how they react to buttons and to their close button.

The windows run against a fake release root (a run.sh that answers "status"
and "port-version" from files the test writes), so nothing here touches an
installed release, the network or the user's settings.

Skips when GTK 3 / json-glib development files or broadwayd are missing,
unless ROBLOX_MAC_REQUIRE_UI_CHECK=1 (CI sets it), which turns that into a
failure.
"""

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ui_harness import Display, Unavailable, Window  # noqa: E402

REQUIRED = os.environ.get("ROBLOX_MAC_REQUIRE_UI_CHECK") == "1"


def skip(reason):
    if REQUIRED:
        sys.exit(f"FAIL check-bootstrapper-windows: {reason} (ROBLOX_MAC_REQUIRE_UI_CHECK=1)")
    print(f"SKIP check-bootstrapper-windows: {reason}")
    sys.exit(0)


def build(tmp):
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "gtk+-3.0", "json-glib-1.0"],
                           capture_output=True, text=True)
    if flags.returncode != 0:
        skip("GTK 3 / json-glib development files are not available")
    binary = Path(tmp) / "bootstrapper-ui"
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    "-Wno-deprecated-declarations", HERE / "bootstrapper-ui.c", "-o", binary,
                    *flags.stdout.split()], check=True)
    return binary


STATUS = {
    "settings": {"auto_update": "ask", "check_interval_hours": 24, "window_decoration": "auto",
                 "cursor_appearance": "system"},
    "installed_display_version": "0.747.0.7471481", "installed_version": "version-installed",
    "latest_known": "", "latest_display_version": "", "update_available": False, "game_running": False,
}


def fake_root(tmp, status=STATUS, port_version="1.2.3"):
    root = Path(tempfile.mkdtemp(prefix="release ", dir=tmp))
    (root / "status.json").write_text(json.dumps(status))
    (root / "port-version").write_text(port_version + "\n")
    (root / "run.sh").write_text(
        '#!/bin/sh\nHERE=$(dirname "$0")\n'
        'case "$1" in\n'
        '  status) cat "$HERE/status.json" ;;\n'
        '  port-version) cat "$HERE/port-version" ;;\n'
        '  set) printf \'%s=%s\\n\' "$2" "$3" >> "$HERE/set.log" ;;\n'
        '  *) exit 9 ;;\n'
        'esac\n')
    return root


def event(phase, percent=0, message="", done=0, total=0, cancellable=False, hidden=False):
    payload = {"phase": phase, "percent": percent, "message": message, "bytes_done": done,
               "bytes_total": total, "cancellable": cancellable}
    if hidden:
        payload["hidden"] = True
    return (json.dumps(payload) + "\n").encode()


def check_harness_reads_launcher(display, binary, tmp):
    root = fake_root(tmp)
    with display.open(binary, "--launcher", root) as ui:
        windows = ui.wait(lambda w: Window.stack(w) == "play" and "Client version: 0.747.0.7471481" in Window.texts(w))
        assert [w["title"] for w in windows] == ["Roblox Mac Linux Port"], windows
        nav = [w["text"] for w in Window.widgets(windows, "toggle")]
        assert any("Info" in text for text in nav), nav
        info = next(text for text in nav if "Info" in text)
        assert ui.send(f"click {info}") == {"ok": True}
        ui.wait(lambda w: Window.stack(w) == "info")
        assert ui.send("click No such button") == {"ok": False, "error": "no such button"}
        assert ui.send("close") == {"ok": True}
        assert ui.exited(10) == 0


def check_harness_reads_progress(display, binary, tmp):
    root = fake_root(tmp)
    # bootstrap.sh writes this before an operation; the installer flips it at commit.
    (root / ".bootstrap-cancel-state").write_text("open\n")
    read, write = os.pipe()
    try:
        with display.open(binary, "--progress", root, stdin=read) as ui:
            os.close(read)
            read = -1
            ui.wait(lambda w: Window.widgets(w, "progress"))
            os.write(write, event("downloading", 40, "Downloading Roblox", 400, 1000, cancellable=True))
            windows = ui.wait(lambda w: "Downloading Roblox" in Window.texts(w))
            bars = Window.widgets(windows, "progress")
            assert len(bars) == 1 and abs(bars[0]["fraction"] - 0.4) < 1e-6, bars
            cancel = Window.button(windows, "Cancel")
            assert cancel and cancel["sensitive"], cancel
            (root / ".bootstrap-cancel-state").write_text("committing\n")
            os.write(write, event("installing", 90, "Installing", cancellable=False))
            windows = ui.wait(lambda w: "Installing" in Window.texts(w))
            assert not Window.button(windows, "Cancel")["sensitive"]
            assert ui.send("click Cancel") == {"ok": False, "error": "insensitive"}
            os.write(write, event("launching", 100, "Starting Roblox"))
            assert ui.exited(10) == 0
    finally:
        if read >= 0:
            os.close(read)
        os.close(write)


def check_harness_reads_prompt(display, binary, tmp):
    with display.open(binary, "--dialog", "0.748.0.7480000", "0.747.0.7471481") as ui:
        windows = ui.wait(lambda w: Window.button(w, "Update now") is not None)
        assert any("0.748.0.7480000" in text for text in Window.texts(windows)), Window.texts(windows)
        assert ui.send("click Update now") == {"ok": True}
        assert ui.exited(10) == 0


def check_settings_opens_on_settings_page(display, binary, tmp):
    """B10 defect 1: --settings highlighted Settings but showed the Play page."""
    root = fake_root(tmp)
    with display.open(binary, "--settings", root) as ui:
        windows = ui.wait(lambda w: "Client version: 0.747.0.7471481" in Window.texts(w))
        active = [w["text"] for w in Window.widgets(windows, "toggle") if w["active"]]
        assert len(active) == 1 and "Settings" in active[0], active
        try:
            ui.wait(lambda w: Window.stack(w) == "settings", timeout=3)
        except AssertionError:
            raise AssertionError(f"Settings is highlighted but the page shown is {Window.stack(ui.dump())!r}")


class Progress:
    """A progress window fed through a pipe, the way bootstrap.sh feeds it."""

    def __init__(self, display, binary, tmp, state="open"):
        self.root = fake_root(tmp)
        if state is not None:
            (self.root / ".bootstrap-cancel-state").write_text(state + "\n")
        read, self.write = os.pipe()
        self.ui = display.open(binary, "--progress", self.root, stdin=read)
        os.close(read)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        os.close(self.write)
        self.ui.__exit__(*exc)

    def send(self, *args, **kwargs):
        os.write(self.write, event(*args, **kwargs))


def check_progress_shows_events_sent_before_it_appears(display, binary, tmp):
    """An event that arrived during the window's 450 ms delay was dropped."""
    with Progress(display, binary, tmp) as progress:
        progress.send("downloading", 40, "Downloading Roblox", 400, 1000, cancellable=True)
        windows = progress.ui.wait(lambda w: Window.widgets(w, "progress"))
        try:
            progress.ui.wait(lambda w: "Downloading Roblox" in Window.texts(w), timeout=2)
        except AssertionError:
            raise AssertionError(f"the first event is not shown: {Window.texts(progress.ui.dump())}")


def check_progress_close_while_cancellable_cancels(display, binary, tmp):
    """Closing the window is the same as pressing Cancel when Cancel is allowed."""
    with Progress(display, binary, tmp) as progress:
        progress.ui.wait(lambda w: Window.widgets(w, "progress"))
        progress.send("downloading", 40, "Downloading Roblox", 400, 1000, cancellable=True)
        progress.ui.wait(lambda w: (Window.button(w, "Cancel") or {}).get("sensitive"))
        assert progress.ui.send("close") == {"ok": True}
        progress.ui.wait(lambda w: any("Cancelling" in text for text in Window.texts(w)))
        assert (progress.root / ".bootstrap-cancel").read_text() == "cancel\n"


def check_progress_refused_close_says_why(display, binary, tmp):
    """B10 defect 3: closing in a phase that cannot be cancelled did nothing
    visible. The reason must stay on screen while progress continues."""
    with Progress(display, binary, tmp, state="committing") as progress:
        progress.ui.wait(lambda w: Window.widgets(w, "progress"))
        progress.send("installing", 50, "Installing Roblox")
        progress.ui.wait(lambda w: "Installing Roblox" in Window.texts(w))
        assert progress.ui.send("close") == {"ok": True}
        progress.send("installing", 60, "Installing Roblox (60%)")
        windows = progress.ui.wait(lambda w: "Installing Roblox (60%)" in Window.texts(w))
        reasons = [text for text in Window.texts(windows) if "can't be cancelled" in text or "cannot be cancelled" in text]
        assert reasons, f"no reason visible after a refused close: {Window.texts(windows)}"
        assert progress.ui.exited(0.5) is None, "the window closed although the phase cannot be cancelled"
        assert not (progress.root / ".bootstrap-cancel").exists()


def check_progress_stays_hidden_while_prompt_is_open(display, binary, tmp):
    """B10 defect 4: the progress window appeared after the update prompt and
    covered it. A "hidden" event (sent before the prompt opens) must keep it
    off screen, even when it arrives before the window's first appearance."""
    with Progress(display, binary, tmp) as progress:
        progress.send("checking", 25, "Checking for updates")
        progress.send("prompt", 25, "Waiting for an answer to the update prompt", hidden=True)
        time.sleep(1.0)  # well past the 450 ms show delay
        windows = progress.ui.dump()
        assert windows == [], f"the progress window is visible while the prompt is open: {windows}"
        progress.send("downloading", 30, "Downloading Roblox", cancellable=True)
        progress.ui.wait(lambda w: "Downloading Roblox" in Window.texts(w))
        progress.send("prompt", 30, "Waiting", hidden=True)
        progress.ui.wait(lambda w: w == [])
        progress.send("launching", 100, "Launching Roblox")
        assert progress.ui.exited(5) == 0


def check_failed_port_check_says_why(display, binary, tmp):
    """B10 defect 5: a failed port-update check must say what failed, why,
    what to do, and where the log is."""
    root = fake_root(tmp)
    (root / "installer.py").write_text(
        "import sys\n"
        "print('! Could not query the latest GitHub release: could not reach GitHub "
        "(Temporary failure in name resolution).', file=sys.stderr)\n"
        "sys.exit(1)\n")
    state = Path(tmp) / "state home"
    with display.open(binary, "--settings", root, env={**os.environ, "XDG_STATE_HOME": str(state)}) as ui:
        ui.wait(lambda w: Window.button(w, "Check for port updates") is not None
                and Window.button(w, "Check for port updates")["sensitive"])
        assert ui.send("click Check for port updates") == {"ok": True}
        windows = ui.wait(lambda w: any("Could not check for port updates" in t for t in Window.texts(w)))
        message = next(t for t in Window.texts(windows) if "Could not check for port updates" in t)
        assert "Temporary failure in name resolution" in message, message
        assert "try again" in message, message
        assert str(state / "roblox-mac-linux-port" / "installer.log") in message, message


def check_prompt_close_and_cancel_mean_cancel(display, binary, tmp):
    """B10 flow 7: closing the update prompt, or its Cancel button, cancels the
    launch (exit 4) at once. Before, closing hid the window and the prompt
    answered "launch without updating" after its 30 s timer."""
    with display.open(binary, "--dialog", "0.748.0.7480000", "0.747.0.7471481") as ui:
        ui.wait(lambda w: Window.button(w, "Update now") is not None)
        assert ui.send("close") == {"ok": True}
        assert ui.exited(3) == 4, "closing the prompt did not cancel"
    with display.open(binary, "--dialog", "0.748.0.7480000", "0.747.0.7471481") as ui:
        windows = ui.wait(lambda w: Window.button(w, "Update now") is not None)
        assert Window.button(windows, "Cancel"), Window.texts(windows)
        assert not any("30 seconds" in text for text in Window.texts(windows)), Window.texts(windows)
        assert ui.send("click Cancel") == {"ok": True}
        assert ui.exited(3) == 4


CHECKS = [check_harness_reads_launcher, check_harness_reads_progress, check_harness_reads_prompt,
          check_settings_opens_on_settings_page, check_progress_shows_events_sent_before_it_appears,
          check_progress_close_while_cancellable_cancels, check_progress_refused_close_says_why,
          check_progress_stays_hidden_while_prompt_is_open, check_failed_port_check_says_why,
          check_prompt_close_and_cancel_mean_cancel]


def main():
    with tempfile.TemporaryDirectory(prefix="bootstrapper-windows ") as tmp:
        binary = build(tmp)
        try:
            display = Display().__enter__()
        except Unavailable as error:
            skip(str(error))
        failed = 0
        try:
            for check in CHECKS:
                try:
                    check(display, binary, tmp)
                    print(f"ok   {check.__name__}")
                except AssertionError as error:
                    failed += 1
                    print(f"FAIL {check.__name__}: {error}")
        finally:
            display.__exit__(None, None, None)
        if failed:
            sys.exit(f"check-bootstrapper-windows: {failed} of {len(CHECKS)} failed")
        print(f"check-bootstrapper-windows: {len(CHECKS)} passed")


if __name__ == "__main__":
    main()
