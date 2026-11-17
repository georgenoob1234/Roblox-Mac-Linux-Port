#!/usr/bin/env python3
"""B6: the client download (runtime/scripts/fetch-client.sh) against a local
fake server, with the real curl, python3 and unzip. Every failure must end
promptly with one readable last stderr line (bootstrap.sh shows that line as
the reason), never a traceback, and must leave no client behind.
Never contacts Roblox: the URLs are overridden through the environment."""
import http.server
import io
import os
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FETCH = HERE.parent / "scripts" / "fetch-client.sh"
VERSION = "version-0123abcd"


def client_zip():
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w") as archive:
        info = zipfile.ZipInfo("RobloxPlayer.app/Contents/MacOS/RobloxPlayer")
        info.external_attr = 0o100755 << 16
        archive.writestr(info, b"#!/bin/sh\n" + os.urandom(200_000))
        archive.writestr("RobloxPlayer.app/Contents/Info.plist", b"<plist/>")
    return data.getvalue()


ZIP = client_zip()


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    version_mode = "ok"
    zip_mode = "ok"
    stop = threading.Event()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def stall(self):
        self.server.stop.wait(120)

    def send(self, status, body, length=None):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body) if length is None else length))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        if self.path.startswith("/version"):
            mode = self.server.version_mode
            if mode == "stall":
                return self.stall()
            if mode == "500":
                return self.send(500, b"oops")
            if mode == "garbage":
                return self.send(200, b"<html>not json</html>")
            if mode == "badversion":
                return self.send(200, b'{"clientVersionUpload": "../../evil"}')
            return self.send(200, ('{"clientVersionUpload": "%s", "version": "0.748.0.7480000"}' % VERSION).encode())
        if self.path == f"/mac/{VERSION}-RobloxPlayer.zip":
            mode = self.server.zip_mode
            if mode == "404":
                return self.send(404, b"not found")
            if mode == "stall":
                if self.command == "HEAD":
                    return self.stall()
                self.send_response(200)
                self.send_header("Content-Length", str(len(ZIP)))
                self.end_headers()
                self.wfile.write(ZIP[:1000])
                self.wfile.flush()
                return self.stall()
            if mode == "truncated":
                if self.command == "HEAD":
                    return self.send(200, b"", length=len(ZIP))
                self.send_response(200)
                self.send_header("Content-Length", str(len(ZIP)))
                self.end_headers()
                self.wfile.write(ZIP[: len(ZIP) // 2])
                return
            if mode == "corrupt":
                return self.send(200, os.urandom(len(ZIP)))
            if mode == "damaged":
                # A real zip whose data no longer matches its checksums.
                body = bytearray(ZIP)
                body[len(body) // 3] ^= 0xFF
                return self.send(200, bytes(body))
            return self.send(200, ZIP)
        self.send(404, b"")


def fetch(server, tmp, stall=2):
    data = Path(tempfile.mkdtemp(prefix="data ", dir=tmp))
    port = server.server_address[1]
    env = {**os.environ, "ROBLOX_MAC_DATA": str(data),
           "ROBLOX_MAC_CLIENT_VERSION_URL": f"http://127.0.0.1:{port}/version",
           "ROBLOX_MAC_CLIENT_CDN": f"http://127.0.0.1:{port}",
           "ROBLOX_MAC_DOWNLOAD_STALL_SECONDS": str(stall)}
    env.pop("ROBLOX_MAC_PROGRESS_FD", None)
    started = time.monotonic()
    try:
        result = subprocess.run(["sh", str(FETCH)], env=env, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        raise AssertionError("still running after 60 s")
    lines = [line for line in result.stderr.splitlines() if line.strip()]
    return result, (lines[-1] if lines else ""), time.monotonic() - started, data


def expect_failure(server, tmp, label, *words, within=30):
    result, last, elapsed, data = fetch(server, tmp)
    assert result.returncode != 0, (label, result.stderr)
    assert elapsed < within, (label, f"took {elapsed:.0f} s", result.stderr)
    assert "Traceback" not in result.stderr and "Error:" not in last, (label, result.stderr)
    for word in words:
        assert word.lower() in last.lower(), (label, word, last)
    assert not (data / "RobloxPlayer.app").exists() and not (data / ".version").exists(), (label, list(data.iterdir()))
    leftovers = [p.name for p in data.iterdir() if p.name.startswith((".dl.", ".version-query."))]
    assert not leftovers, (label, leftovers)
    print(f"ok   {label}: {last}")


def main():
    server = Server(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    failed = 0
    with tempfile.TemporaryDirectory(prefix="client download ") as tmp:
        cases = [
            ("a good download installs the client", "ok", "ok", None),
            ("version service error", "500", "ok", ("500",)),
            ("version service answers something that is not JSON", "garbage", "ok", ("unexpected answer",)),
            ("version service answers an invalid version", "badversion", "ok", ("unexpected answer",)),
            ("version service stalls", "stall", "ok", ("timed out",)),
            ("client archive not found", "ok", "404", ("404",)),
            ("client download stalls", "ok", "stall", ("stalled",)),
            ("client download is cut short", "ok", "truncated", ("cut short",)),
            ("client download is not a zip", "ok", "corrupt", ("damaged",)),
            ("client download fails its checksums", "ok", "damaged", ("damaged",)),
        ]
        for label, version_mode, zip_mode, words in cases:
            server.version_mode, server.zip_mode = version_mode, zip_mode
            try:
                if words is None:
                    result, last, elapsed, data = fetch(server, tmp)
                    assert result.returncode == 0, result.stderr
                    assert (data / ".version").read_text() == VERSION, list(data.iterdir())
                    assert os.access(data / "RobloxPlayer.app/Contents/MacOS/RobloxPlayer", os.X_OK)
                    print(f"ok   {label}")
                else:
                    expect_failure(server, tmp, label, *words)
            except AssertionError as error:
                failed += 1
                print(f"FAIL {label}: {error}")
    server.stop.set()
    server.shutdown()
    if failed:
        sys.exit(f"check-client-download: {failed} of {len(cases)} failed")
    print(f"check-client-download: {len(cases)} passed")


if __name__ == "__main__":
    main()
