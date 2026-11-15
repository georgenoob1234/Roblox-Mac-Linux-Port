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
        '  check) [ -e "$HERE/check-fails" ] && exit 1; cat "$HERE/status.json" ;;\n'
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
        windows = ui.wait(lambda w: Window.stack(w) == "play" and "Client 0.747.0.7471481" in Window.texts(w))
        assert [w["title"] for w in windows] == ["Roblox Mac Linux Port"], windows
        nav = [w["text"] for w in Window.widgets(windows, "toggle")]
        assert nav == ["Play", "Updates", "Appearance", "About"], nav
        assert ui.send("click About") == {"ok": True}
        ui.wait(lambda w: Window.stack(w) == "about")
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
        windows = ui.wait(lambda w: "Client 0.747.0.7471481" in Window.texts(w))
        active = [w["text"] for w in Window.widgets(windows, "toggle") if w["active"]]
        assert active == ["Updates"], active
        try:
            ui.wait(lambda w: Window.stack(w) == "updates", timeout=3)
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


def check_prompt_lists_client_and_port_updates(display, binary, tmp):
    """B10 flow 9 (George: one prompt for both): --dialog LATEST INSTALLED
    [PORT_LATEST PORT_INSTALLED]; an empty pair means "no update of that kind"."""
    def rows(windows):
        return [text for text in Window.texts(windows) if "→" in text]
    cases = [
        (("0.748.0.7480000", "0.747.0.7471481", "v2026.11.20", "v2026.11.15"),
         ["Roblox", "0.747.0.7471481 → 0.748.0.7480000", "Roblox Mac Linux Port", "v2026.11.15 → v2026.11.20"]),
        (("", "", "v2026.11.20", "v2026.11.15"), ["Roblox Mac Linux Port", "v2026.11.15 → v2026.11.20"]),
        (("0.748.0.7480000", "0.747.0.7471481"), ["Roblox", "0.747.0.7471481 → 0.748.0.7480000"]),
    ]
    def rows(windows):
        # Each update is a card: its name, then "installed → new".
        texts = Window.texts(windows)
        return [t for i, t in enumerate(texts) if "→" in t or (i + 1 < len(texts) and "→" in texts[i + 1])]
    for args, expected in cases:
        with display.open(binary, "--dialog", *args) as ui:
            windows = ui.wait(lambda w: Window.button(w, "Update now") is not None)
            assert rows(windows) == expected, (args, rows(windows))
            assert ui.send("click Skip this version") == {"ok": True}
            assert ui.exited(3) == 2


def check_failed_update_check_says_why(display, binary, tmp):
    """B10 defect 2: Settings → Check for updates must say why it failed and
    where the log is (the launcher keeps the reason as last_check_error)."""
    status = json.loads(json.dumps(STATUS))
    status["settings"]["last_check_error"] = "Could not resolve host: clientsettingscdn.roblox.com"
    root = fake_root(tmp, status=status)
    (root / "check-fails").write_text("")
    with display.open(binary, "--settings", root) as ui:
        ui.wait(lambda w: (Window.button(w, "Check for updates") or {}).get("sensitive"))
        assert ui.send("click Check for updates") == {"ok": True}
        windows = ui.wait(lambda w: any("Could not check for updates" in t for t in Window.texts(w)))
        message = next(t for t in Window.texts(windows) if "Could not check for updates" in t)
        assert "Could not resolve host: clientsettingscdn.roblox.com" in message, message
        assert "try again" in message, message
        assert str(root / "DO_NOT_SHARE" / "launcher.log") in message, message


def check_progress_failure_stays_until_closed(display, binary, tmp):
    """B10 phase 3 (George: yes): a failure stays on screen with what failed,
    why, what to do, the log path and Open log, until the user closes it,
    even after the launcher has closed the pipe."""
    log = str(Path(tmp) / "launcher.log")
    with Progress(display, binary, tmp) as progress:
        progress.ui.wait(lambda w: Window.widgets(w, "progress"))
        os.write(progress.write, (json.dumps({
            "phase": "failed", "percent": 0, "message": "Roblox update failed: Could not resolve host: setup.rbxcdn.com",
            "bytes_done": 0, "bytes_total": 0, "cancellable": False,
            "hint": "The installed client was kept. Check your internet connection and try again later.", "log": log}) + "\n").encode())
        # The red box slides in: wait for its text, not only for Close.
        windows = progress.ui.wait(lambda w: Window.button(w, "Close") is not None
                                   and "Could not resolve host: setup.rbxcdn.com." in Window.texts(w))
        texts = Window.texts(windows)
        for part in ("Roblox update failed", "Could not resolve host: setup.rbxcdn.com.",
                     "The installed client was kept. Check your internet connection and try again later.", log):
            assert part in texts, (part, texts)
        assert Window.button(windows, "Open log"), texts
        progress.ui.wait(lambda w: not Window.widgets(w, "progress"), timeout=3)  # after the crossfade
        os.close(progress.write)
        progress.write = os.open(os.devnull, os.O_WRONLY)
        assert progress.ui.exited(1.0) is None, "the failure closed without the user"
        assert progress.ui.send("click Close") == {"ok": True}
        assert progress.ui.exited(5) == 1


def check_pending_cancel_stays_visible(display, binary, tmp):
    """George, 2026-11-15: after Cancel, the "Cancelling" note vanished when the
    phase changed, and the X then did nothing visible. A pending cancel must
    stay on screen until the launcher ends the run, and X must answer."""
    with Progress(display, binary, tmp) as progress:
        progress.ui.wait(lambda w: Window.widgets(w, "progress"))
        progress.send("downloading", 40, "Downloading Roblox", 400, 1000, cancellable=True)
        progress.ui.wait(lambda w: (Window.button(w, "Cancel") or {}).get("sensitive"))
        assert progress.ui.send("click Cancel") == {"ok": True}
        progress.send("verifying", 80, "Verifying the downloaded client", cancellable=True)
        windows = progress.ui.wait(lambda w: "Verifying the downloaded client" in Window.texts(w))
        def notes(windows):  # the note line, not the button's own "Cancelling…"
            return [w["text"] for w in Window.widgets(windows, "label") if w["text"].startswith("Cancelling")]
        assert notes(windows), f"the pending cancel is no longer shown: {Window.texts(windows)}"
        assert progress.ui.send("close") == {"ok": True}
        windows = progress.ui.dump()
        assert any("closes" in t for t in notes(windows)), f"closing during a pending cancel showed nothing: {Window.texts(windows)}"
        progress.send("cancelled", 0, "Update cancelled")
        assert progress.ui.exited(5) == 4


def check_launch_window_keeps_its_size(display, binary, tmp):
    """George, 2026-11-15: keep the compact size of the original window; the
    yellow note and the red box may grow it vertically (smoothly, they slide
    in), but the width never changes and the window returns to the compact
    height when the note goes away."""
    def size(ui):
        window = ui.dump()[0]
        # On KDE Wayland the window takes the width it asks for; broadway
        # does not, so the wanted width is the one to hold steady.
        return max(window["width"], window["wanted_width"]), window["height"]
    def settled(ui, test, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            current = size(ui)
            time.sleep(0.4)  # longer than the slide animations
            if size(ui) == current and test(current):
                return current
        raise AssertionError(f"size did not settle as expected: {size(ui)}")
    with Progress(display, binary, tmp, state="committing") as progress:
        progress.ui.wait(lambda w: Window.widgets(w, "progress"))
        progress.send("installing", 90, "Replacing the client with the new version")
        progress.ui.wait(lambda w: "Replacing the client with the new version" in Window.texts(w))
        width, compact = settled(progress.ui, lambda s: True)
        assert compact < 200, f"the launch window is not compact any more: {compact} px high"
        assert progress.ui.send("close") == {"ok": True}  # the refusal note slides in
        settled(progress.ui, lambda s: s[0] == width and s[1] > compact)
        progress.send("verifying", 95, "Verifying the installed client")  # the note goes with the phase
        settled(progress.ui, lambda s: s == (width, compact))
        long_reason = "Could not resolve host: setup.rbxcdn.com " + "(a long explanation that has to wrap) " * 4
        os.write(progress.write, (json.dumps({
            "phase": "failed", "percent": 0, "message": "Roblox update failed: " + long_reason,
            "bytes_done": 0, "bytes_total": 0, "cancellable": False,
            "hint": "The installed client was kept. Check your internet connection and try again later. " * 3,
            "log": "/home/someone-with-a-long-name/.local/opt/roblox-mac-linux-port/a/very/deep/folder/that/goes/on/DO_NOT_SHARE/launcher.log"}) + "\n").encode())
        progress.ui.wait(lambda w: Window.button(w, "Close") is not None)
        settled(progress.ui, lambda s: s[0] == width)
        # The longest single line the window may hold: no text may widen it.
        progress.send("checking", -1, "Checking " + "a-very-long-word-without-spaces-" * 6)
        settled(progress.ui, lambda s: s[0] == width)


def check_windows_ask_for_no_more_than_their_size(display, binary, tmp):
    """On KDE Wayland a window takes the width its texts ask for (the launch
    window grew to 1095 px there). No window may ask for more than its own
    size: the prompt asked for 1006 px, the settings window for 925 px."""
    root = fake_root(tmp)
    for args in (["--dialog", "0.748.0.7480000", "0.747.0.7471481", "v2026.11.20", "v2026.11.15"],
                 ["--dialog", "0.748.0.7480000", "0.747.0.7471481"], ["--settings", root], ["--launcher", root]):
        with display.open(binary, *args) as ui:
            ui.wait(lambda w: w)
            time.sleep(0.5)
            window = ui.dump()[0]
            assert window["wanted_width"] <= window["width"], (args[0], window["width"], window["wanted_width"])


def check_dropdowns_open_below_and_pick(display, binary, tmp):
    """George, 2026-11-15: the dropdown menus opened over the button with
    GTK's scroll arrows and a clipped first item. A dropdown is now a button
    whose menu opens below it; picking an item shows it and saves it."""
    root = fake_root(tmp)
    with display.open(binary, "--settings", root) as ui:
        windows = ui.wait(lambda w: Window.button(w, "Ask first") is not None)
        assert Window.button(windows, "Once a day"), Window.texts(windows)
        assert not Window.widgets(windows, "combo"), "a GtkComboBox (menu over the button, scroll arrows) is still used"
        assert ui.send("click Ask first") == {"ok": True}
        assert ui.send("click Automatically") == {"ok": True}
        ui.wait(lambda w: Window.button(w, "Automatically") is not None)
        assert ui.send("click Once a day") == {"ok": True}
        assert ui.send("click Once a week") == {"ok": True}
        ui.wait(lambda w: Window.button(w, "Once a week") is not None)
        changes = (root / "set.log").read_text().splitlines()
        assert changes == ["auto_update=auto", "check_interval_hours=168"], changes


def check_card_controls_line_up(display, binary, tmp):
    """George, 2026-11-15: the dropdowns on the Updates page did not line up
    with each other or with the button under them. The controls at the right
    of the cards share one left and one right edge, on every page."""
    root = fake_root(tmp)
    with display.open(binary, "--settings", root) as ui:
        ui.wait(lambda w: Window.button(w, "Ask first") is not None)
        # Also maximized: the controls must stay at the cards' right edge.
        for width, height in ((900, 600), (1600, 1000)):
            assert ui.send(f"resize {width} {height}") == {"ok": True}
            ui.wait(lambda w: w[0]["width"] == width)
            for page, controls in (("Updates", ["Ask first", "Once a day", "Forget skipped"]),
                                   ("Appearance", ["Automatic", "Restore defaults"])):
                assert ui.send(f"click {page}") == {"ok": True}
                ui.wait(lambda w: all(Window.button(w, c) for c in controls))
                time.sleep(0.4)  # the page crossfade and the resize
                windows = ui.dump()
                edges = [(Window.button(windows, c)["x"], Window.button(windows, c)["x"] + Window.button(windows, c)["allocated_width"])
                         for c in controls]
                assert len(set(edges)) == 1, (width, page, dict(zip(controls, edges)))
                right = edges[0][1]
                # content padding 30 + card border and padding 17 from the window's right edge
                assert width - right <= 60, (width, page, f"controls end at {right}, {width - right} px from the right edge")


CHECKS = [check_harness_reads_launcher, check_harness_reads_progress, check_harness_reads_prompt,
          check_settings_opens_on_settings_page, check_progress_shows_events_sent_before_it_appears,
          check_progress_close_while_cancellable_cancels, check_progress_refused_close_says_why,
          check_progress_stays_hidden_while_prompt_is_open, check_failed_port_check_says_why,
          check_prompt_close_and_cancel_mean_cancel, check_prompt_lists_client_and_port_updates,
          check_failed_update_check_says_why, check_progress_failure_stays_until_closed,
          check_pending_cancel_stays_visible, check_launch_window_keeps_its_size,
          check_windows_ask_for_no_more_than_their_size, check_dropdowns_open_below_and_pick,
          check_card_controls_line_up]


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
