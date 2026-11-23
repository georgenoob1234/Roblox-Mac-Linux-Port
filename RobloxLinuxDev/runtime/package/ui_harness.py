"""Drive bootstrapper-ui's real windows in tests, without a screen.

A private GTK broadway display (broadwayd, part of GTK 3) stands in for the
desktop; it needs no X server or compositor. Its socket is named after a
temporary runtime directory and its web view listens on the loopback address
only. The UI's test channel (ROBLOX_MAC_UI_TEST_IN and ROBLOX_MAC_UI_TEST_OUT,
see bootstrapper-ui.c) reports what each window shows and presses or closes
things the way a user would.

    with Display() as display:
        with display.open(binary, "--settings", root) as ui:
            ui.wait(lambda d: ui.stack(d) == "settings")
"""

import json
import os
import select
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path


class Unavailable(Exception):
    """No broadway display could be started (broadwayd missing or failing)."""


class Display:
    def __init__(self):
        self.runtime = None
        self.process = None
        self.number = None
        self._dirty = False

    def _start(self):
        if self.process:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait()
            self.process = None
        broadwayd = shutil.which("broadwayd")
        if not broadwayd:
            raise Unavailable("broadwayd is not installed (GTK 3's libgtk-3-bin)")
        # GDK and broadwayd meet on an abstract socket named after the runtime
        # directory and the display number, so a private runtime directory
        # cannot collide with any other display. The web view broadwayd also
        # serves is bound to the loopback address on a free port.
        for number in range(40, 50):
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            process = subprocess.Popen(
                [broadwayd, "--address", "127.0.0.1", "--port", str(port), f":{number}"],
                env={**os.environ, "XDG_RUNTIME_DIR": self.runtime},
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            output = b""
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and b"Listening on" not in output:
                ready, _, _ = select.select([process.stdout], [], [], 0.1)
                if ready:
                    chunk = os.read(process.stdout.fileno(), 4096)
                    if not chunk:
                        break
                    output += chunk
            if b"Listening on" in output and b"Unable" not in output and process.poll() is None:
                # Keep draining so broadwayd never blocks on a full pipe.
                threading.Thread(target=process.stdout.read, daemon=True).start()
                self.process, self.number, self._dirty = process, number, False
                return
            process.kill()
            process.wait()
            process.stdout.close()
        raise Unavailable("broadwayd did not start")

    def __enter__(self):
        # Socket names are limited to 108 bytes and build.sh points TMPDIR deep
        # into the checkout, so the runtime directory goes to /tmp when it can.
        short = "/tmp" if os.access("/tmp", os.W_OK) else None
        self.runtime = tempfile.mkdtemp(prefix="ui-display-", dir=short)
        os.chmod(self.runtime, 0o700)
        try:
            self._start()
        except Exception:
            self.__exit__(None, None, None)
            raise
        return self

    def __exit__(self, *exc):
        if self.process:
            self.process.kill()
            self.process.wait()
            self.process = None
        if self.runtime:
            shutil.rmtree(self.runtime, ignore_errors=True)
            self.runtime = None

    def env(self, base=None):
        env = dict(os.environ if base is None else base)
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET"):
            env.pop(name, None)
        env.update(GDK_BACKEND="broadway", BROADWAY_DISPLAY=f":{self.number}",
                   XDG_RUNTIME_DIR=self.runtime, GTK_THEME="Adwaita", NO_AT_BRIDGE="1")
        return env

    def open(self, *argv, env=None, stdin=subprocess.DEVNULL):
        if self._dirty or self.process is None or self.process.poll() is not None:
            self._start()
        return Window(self, argv, env, stdin)


class Window:
    """One bootstrapper-ui process and its test channel."""

    def __init__(self, display, argv, env, stdin):
        self.display = display
        self.state = tempfile.mkdtemp(prefix="ui-test-")
        self.command_path = Path(self.state) / "in"
        self.reply_path = Path(self.state) / "out"
        os.mkfifo(self.command_path)
        self.reply_path.touch()
        self.offset = 0
        # Held read-write so writing never blocks before the UI has opened it.
        self.commands = os.open(self.command_path, os.O_RDWR)
        env = display.env(env)
        env.update(ROBLOX_MAC_UI_TEST_IN=str(self.command_path), ROBLOX_MAC_UI_TEST_OUT=str(self.reply_path))
        self.process = subprocess.Popen([str(part) for part in argv], env=env, stdin=stdin,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if self.process.poll() is None:
            # Abrupt client teardown can kill broadwayd asynchronously; restart
            # it before the next window instead of racing its exit.
            self.display._dirty = True
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.process.wait()
        if self.process.stderr:
            self.process.stderr.close()
        os.close(self.commands)
        shutil.rmtree(self.state, ignore_errors=True)

    def send(self, command, timeout=15.0):
        """Send one command; return its JSON answer, or None if the UI has exited."""
        os.write(self.commands, (command + "\n").encode())
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = self.reply_path.read_bytes()
            end = data.find(b"\n", self.offset)
            if end >= 0:
                line = data[self.offset:end]
                self.offset = end + 1
                return json.loads(line)
            if self.process.poll() is not None:
                return None
            time.sleep(0.02)
        raise TimeoutError(f"no answer to {command!r}")

    def dump(self):
        answer = self.send("dump")
        return answer["windows"] if answer else None

    def wait(self, predicate, timeout=10.0):
        """Poll dumps until predicate(windows) is true; return those windows."""
        deadline = time.monotonic() + timeout
        windows = None
        while time.monotonic() < deadline:
            windows = self.dump()
            if windows is None:
                raise AssertionError("the UI exited while waiting")
            if predicate(windows):
                return windows
            time.sleep(0.05)
        raise AssertionError(f"timed out; last state: {json.dumps(windows)[:2000]}")

    def exited(self, timeout=10.0):
        """The exit status, waiting up to timeout; None if still running."""
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    # Helpers over a dump.
    @staticmethod
    def widgets(windows, kind=None):
        return [w for window in windows for w in window["widgets"] if kind is None or w["type"] == kind]

    @staticmethod
    def texts(windows):
        return [w["text"] for w in Window.widgets(windows) if "text" in w]

    @staticmethod
    def button(windows, text):
        for w in Window.widgets(windows):
            if w["type"] in ("button", "toggle") and w["text"] == text:
                return w
        return None

    @staticmethod
    def stack(windows):
        stacks = Window.widgets(windows, "stack")
        return stacks[0]["visible"] if stacks else None
