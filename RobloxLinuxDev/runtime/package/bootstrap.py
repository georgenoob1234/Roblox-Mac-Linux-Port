#!/usr/bin/env python3
"""Small headless state helper for the Roblox release bootstrapper."""
import json
import os
import sys
import tempfile
import re
from pathlib import Path

DEFAULTS = {
    "auto_update": "ask",
    "check_interval_hours": 24,
    "skipped_version": "",
    "last_check": 0,
    "last_known_latest": "",
    "last_known_latest_display": "",
}
VALID_MODES = {"auto", "ask", "off"}
KNOWN_DISPLAY_VERSIONS = {
    # Release notes and the launcher have carried these tested content hashes.
    "version-00a4ca14e31b41e9": "0.740.0.7400927",
    "version-3bc33ee7ffad426f": "0.741.0.7411056",
}


def settings_path(data):
    return Path(data) / "bootstrapper.json"


def read_settings(data):
    path = settings_path(data)
    values = dict(DEFAULTS)
    try:
        raw = json.loads(path.read_text())
        if isinstance(raw, dict):
            values.update(raw)
    except (OSError, ValueError, TypeError):
        pass
    mode = values.get("auto_update")
    if mode not in VALID_MODES:
        values["auto_update"] = DEFAULTS["auto_update"]
    try:
        interval = int(values.get("check_interval_hours", 24))
        values["check_interval_hours"] = max(0, interval)
    except (TypeError, ValueError):
        values["check_interval_hours"] = DEFAULTS["check_interval_hours"]
    for key in ("last_check",):
        try:
            values[key] = max(0, int(values.get(key, 0)))
        except (TypeError, ValueError):
            values[key] = 0
    for key in ("skipped_version", "last_known_latest", "last_known_latest_display"):
        value = values.get(key, "")
        values[key] = value if isinstance(value, str) else ""
    return values


def write_settings(data, values):
    path = settings_path(data)
    path.parent.mkdir(parents=True, exist_ok=True)
    merged = dict(DEFAULTS)
    merged.update(values)
    fd, name = tempfile.mkstemp(prefix=".bootstrapper.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(merged, stream, sort_keys=True, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(name, 0o600)
        os.replace(name, path)
        directory = os.open(path.parent, os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except BaseException:
        try:
            os.unlink(name)
        except OSError:
            pass
        raise


def installed_version(version_dir):
    path = Path(version_dir) / ".version"
    try:
        value = path.read_text().strip()
    except OSError:
        return ""
    return value if value.startswith("version-") else ""


def readable_version(version_dir, raw=""):
    """Return a user-facing Roblox version when the runtime has recorded one.

    Roblox's update service uses an opaque content hash (version-*).  The
    downloader may also record the numeric client version in .display-version;
    keep the hash available for diagnostics while never presenting it as the
    launcher version when a readable value exists.
    """
    path = Path(version_dir) / ".display-version"
    try:
        value = path.read_text().strip()
    except OSError:
        value = ""
    if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", value):
        return value
    if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", os.environ.get("ROBLOX_MAC_DISPLAY_VERSION", "")):
        return os.environ["ROBLOX_MAC_DISPLAY_VERSION"]
    # Some diagnostic logs contain the canonical numeric build. Use the most
    # recent one only when no explicit metadata file was written.
    diagnostics = Path(version_dir).parent / "DO_NOT_SHARE" / "diagnostics"
    try:
        candidates = sorted(diagnostics.glob("**/*"), key=lambda item: item.stat().st_mtime, reverse=True)
    except OSError:
        candidates = []
    for candidate in candidates[:40]:
        if not candidate.is_file():
            continue
        try:
            match = re.search(r"(?<![\d.])(\d+\.\d+\.\d+\.\d+)(?![\d.])", candidate.read_text(errors="ignore"))
        except OSError:
            continue
        if match:
            return match.group(1)
    return KNOWN_DISPLAY_VERSIONS.get(raw, "")


def game_running(data):
    import fcntl
    path = Path(data) / "instance.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        fcntl.flock(stream, fcntl.LOCK_UN)
    return False


def emit(phase, percent, message, bytes_done=0, bytes_total=0, cancellable=True):
    print(json.dumps({"phase": phase, "percent": percent, "message": message,
                      "bytes_done": bytes_done, "bytes_total": bytes_total,
                      "cancellable": bool(cancellable)}, separators=(",", ":")))


def main(argv):
    if len(argv) < 2:
        raise SystemExit("usage: bootstrap.py settings|status|event DATA ...")
    command = argv[1]
    if command == "settings" and len(argv) >= 4:
        data, operation = argv[2], argv[3]
        values = read_settings(data)
        if operation == "get" and len(argv) == 5:
            key = argv[4]
            if key not in values:
                raise SystemExit(2)
            print(json.dumps(values[key]) if not isinstance(values[key], str) else values[key])
            return
        if operation == "set" and len(argv) == 6:
            key, value = argv[4], argv[5]
            if key not in DEFAULTS:
                raise SystemExit(2)
            if key == "auto_update" and value not in VALID_MODES:
                raise SystemExit("auto_update must be auto, ask or off")
            if key == "check_interval_hours":
                value = max(0, int(value))
            elif key in {"last_check"}:
                value = max(0, int(value))
            values[key] = value
            write_settings(data, values)
            return
        raise SystemExit(2)
    if command == "status" and len(argv) == 4:
        data, version_dir = argv[2], argv[3]
        values = read_settings(data)
        installed = installed_version(version_dir)
        installed_display = readable_version(version_dir, installed)
        latest = values["last_known_latest"]
        latest_display = values.get("last_known_latest_display", "") or KNOWN_DISPLAY_VERSIONS.get(latest, "")
        print(json.dumps({
            "installed_version": installed,
            "installed_display_version": installed_display,
            "latest_known": latest,
            "latest_display_version": latest_display,
            "last_check": values["last_check"],
            "settings": values,
            "update_available": bool(latest and latest != installed),
            "game_running": game_running(data),
        }, sort_keys=True, separators=(",", ":")))
        return
    if command == "event" and len(argv) == 5:
        emit(argv[2], int(argv[3]), argv[4])
        return
    if command == "due" and len(argv) == 4:
        values = read_settings(argv[2])
        now = int(argv[3])
        interval = values["check_interval_hours"]
        print("1" if interval == 0 or now - values["last_check"] >= interval * 3600 else "0")
        return
    raise SystemExit(2)


if __name__ == "__main__":
    main(sys.argv)
