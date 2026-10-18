#!/usr/bin/env python3
"""Offline port-release and installer transaction checks."""
import importlib.util
import json
import os
import shutil
import tempfile
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("port_installer", ROOT / "installer/installer.py")
installer = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules[spec.name] = installer
spec.loader.exec_module(installer)
assert installer.build_parser().parse_args(["release-status", "--prefix", "/tmp/port", "--json"]).json is True


def release(root: Path, version: str) -> Path:
    source = root / f"source-{version}"
    source.mkdir()
    files = {
        "FFlags.json": b"{}\n", "RobloxLinux.AppImage": b"appimage\n",
        "bootstrap.py": b"print('bootstrap')\n", "bootstrap.sh": b"#!/bin/sh\nexit 0\n",
        "icon.png": b"icon", "icon_bw.png": b"icon-bw", "install-uri-handler.sh": b"#!/bin/sh\n",
        "run.sh": f"#!/bin/sh\n# {version}\n".encode(),
        "update-roblox.sh": b"#!/bin/sh\nexit 0\n", "installer.py": b"# installer\n",
    }
    for name in installer.REQUIRED_FILES:
        path = source / name
        path.write_bytes(files[name])
        path.chmod(0o755)
    (source / installer.PORT_VERSION_FILE).write_text(version + "\n")
    return source


with tempfile.TemporaryDirectory(prefix="port-updater-") as tmpdir:
    root = Path(tmpdir)
    target = root / "legacy"
    source1 = release(root, "v1.0.0")
    shutil.copytree(source1, target)
    (target / "DO_NOT_SHARE/diagnostics").mkdir(parents=True)
    (target / "DO_NOT_SHARE/diagnostics/log").write_text("keep\n")
    (target / "RobloxVersion").mkdir()
    (target / "RobloxVersion/.version").write_text("client-version\n")
    (target / "unknown-user-file").write_text("preserve\n")
    # Direct extracted releases have no installer manifest; recognized files
    # are adopted while unknown data and Roblox files remain untouched.
    (target / installer.MANIFEST_NAME).unlink(missing_ok=True)
    assert installer.installed_port_version(target) == "v1.0.0"
    source2 = release(root, "v2.0.0")
    installer.run_install_or_update(target, source2, "v2.0.0", yes=True,
                                    confirmed=True, install_desktop_integration=False)
    assert installer.installed_port_version(target) == "v2.0.0"
    assert (target / "run.sh").read_bytes() == (source2 / "run.sh").read_bytes()
    assert (target / "RobloxVersion/.version").read_text() == "client-version\n"
    assert (target / "DO_NOT_SHARE/diagnostics/log").read_text() == "keep\n"
    assert (target / "unknown-user-file").read_text() == "preserve\n"
    assert (target / installer.MANIFEST_NAME).is_file()

    # The published metadata must select the packaged archive and checksum.
    original_metadata = installer.fetch_json
    installer.fetch_json = lambda _url: {
        "tag_name": "v3.0.0",
        "assets": [
            {"name": "RobloxLinuxRelease.tar.gz", "browser_download_url": "https://github.com/georgenoob1234/Roblox-Mac-Linux-Port/releases/a", "digest": "sha256:" + "a" * 64, "size": 42},
            {"name": "RobloxLinuxRelease.tar.gz.sha256", "browser_download_url": "https://github.com/georgenoob1234/Roblox-Mac-Linux-Port/releases/c"},
        ],
    }
    metadata = installer.latest_release_metadata()
    installer.fetch_json = original_metadata
    assert metadata["tag"] == "v3.0.0" and metadata["digest"] == "a" * 64

    # A cancellation marker is honored during staging and never installs a
    # partial archive.
    cancel = root / "cancel"
    cancel.write_text("cancel\n")
    os.environ["ROBLOX_MAC_CANCEL_FILE"] = str(cancel)
    try:
        try:
            installer.extract_release_archive(root / "missing.tar.gz", root / "extract")
        except installer.InstallerError as exc:
            assert "cancelled" in str(exc).lower()
        else:
            raise AssertionError("cancellation was ignored")
    finally:
        os.environ.pop("ROBLOX_MAC_CANCEL_FILE", None)


    handoff = root / "handoff"
    handoff.mkdir()
    (handoff / "DO_NOT_SHARE").mkdir()
    shutil.copy2(ROOT / "RobloxLinuxDev/runtime/package/bootstrap.sh", handoff / "bootstrap.sh")
    shutil.copy2(ROOT / "RobloxLinuxDev/runtime/package/bootstrap.py", handoff / "bootstrap.py")
    (handoff / "installer.py").write_text("""#!/usr/bin/env python3
import json, os, time
fd = int(os.environ["ROBLOX_MAC_PROGRESS_FD"])
def emit(phase, percent, message, cancellable):
    os.write(fd, (json.dumps({"phase": phase, "percent": percent, "message": message,
                               "bytes_done": 0, "bytes_total": 0,
                               "cancellable": cancellable}) + "\\n").encode())
emit("downloading", 30, "Downloading", True)
for _ in range(20):
    if os.path.exists(os.environ.get("ROBLOX_MAC_CANCEL_FILE", "")):
        emit("cancelled", 0, "Cancelled", False)
        raise SystemExit(125)
    time.sleep(0.01)
emit("complete", 100, "Complete", False)
""")
    (handoff / "installer.py").chmod(0o755)
    run = handoff / "run.sh"
    run.write_text("""#!/bin/sh
set -eu
case "${1:-}" in
  port-update|port-update-settings) exec sh "$(dirname -- "$0")/bootstrap.sh" "$@";;
  --settings) echo settings >> "$SETTINGS_LOG"; exit 0;;
  *) exit 2;;
esac
""")
    run.chmod(0o755)
    ui = root / "port-ui"
    ui.write_text("""#!/bin/sh
case "${2:-}" in
  --progress) cat > "$EVENT_LOG"; exit 0;;
esac
exit 3
""")
    ui.chmod(0o755)
    event_log = root / "port-events"
    settings_log = root / "port-settings"
    env = {**os.environ, "ROBLOX_MAC_UI_APP": str(ui), "EVENT_LOG": str(event_log),
           "SETTINGS_LOG": str(settings_log)}
    result = __import__("subprocess").run([handoff / "bootstrap.sh", "port-update-settings"], env=env, capture_output=True, text=True)
    assert result.returncode == 0 and settings_log.read_text().splitlines() == ["settings"]
    assert [json.loads(line)["phase"] for line in event_log.read_text().splitlines()] == ["checking", "downloading", "complete", "update"]
    cancel_ui = root / "port-cancel-ui"
    cancel_ui.write_text("#!/bin/sh\ncase \"${2:-}\" in\n  --progress) touch \"$ROBLOX_MAC_CANCEL_FILE\"; cat > \"$EVENT_LOG\"; exit 4;;\nesac\nexit 3\n")
    cancel_ui.chmod(0o755)
    settings_log.write_text("")
    cancelled = __import__("subprocess").run([handoff / "bootstrap.sh", "port-update-settings"],
                                               env={**env, "ROBLOX_MAC_UI_APP": str(cancel_ui)}, capture_output=True, text=True)
    assert cancelled.returncode == 125 and settings_log.read_text().splitlines() == ["settings"]

print("PASS port version detection, packaged release metadata, legacy ownership adoption, preservation and cancellation")
