#!/usr/bin/env python3
"""Offline regression tests for the per-user installer.

Every case uses a disposable HOME/XDG tree and a tiny synthetic release. No
GitHub release or Roblox client is downloaded.
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

INSTALLER = Path(__file__).resolve().parents[1] / "installer.py"
spec = importlib.util.spec_from_file_location("safe_installer", INSTALLER)
installer = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(installer)

REQUIRED = sorted(installer.REQUIRED_FILES)

def env_for(home: Path) -> dict[str, str]:
    env = os.environ.copy()
    env.update({
        "HOME": str(home),
        "XDG_DATA_HOME": str(home / "data"),
        "XDG_CONFIG_HOME": str(home / "config"),
        "XDG_STATE_HOME": str(home / "state"),
        "XDG_SESSION_TYPE": "wayland",
        "WAYLAND_DISPLAY": "wayland-test",
    })
    return env


def run_cli(home: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [sys.executable, str(INSTALLER), *args],
        env=env_for(home), text=True, capture_output=True,
    )
    if check and result.returncode != 0:
        raise AssertionError(f"CLI failed ({result.returncode}): {result.stdout}\n{result.stderr}")
    return result


def make_release(root: Path, version: str = "one") -> Path:
    source = root / f"source-{version}"
    source.mkdir()
    contents = {
        "FFlags.json": b"{}\n",
        "RobloxLinux.AppImage": f"appimage-{version}\n".encode(),
        "bootstrap.py": b"print('bootstrap')\n",
        "bootstrap.sh": b"#!/bin/sh\nexit 0\n",
        "icon.png": b"png-icon",
        "icon_bw.png": b"png-settings-icon",
        "install-uri-handler.sh": b"#!/bin/sh\nexit 0\n",
        "run.sh": f"#!/bin/sh\n# version {version}\nexit 0\n".encode(),
        "update-roblox.sh": b"#!/bin/sh\nexit 0\n",
    }
    for name in REQUIRED:
        path = source / name
        path.write_bytes(contents[name])
        if name.endswith((".sh", ".py", ".AppImage")):
            path.chmod(0o755)
    (source / "unexpected-source.txt").write_text("ignored\n")
    return source


def assert_manifest(target: Path) -> dict:
    manifest = json.loads((target / installer.MANIFEST_NAME).read_text())
    assert manifest["install_dir"] == str(target)
    assert set(manifest["files"]) == set(REQUIRED) | {"installer.py"}
    return manifest


def make_tar(path: Path, members: list[tuple[str, bytes | None, str]]) -> None:
    with tarfile.open(path, "w:gz") as archive:
        for name, payload, kind in members:
            info = tarfile.TarInfo(name)
            if kind == "dir":
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                archive.addfile(info)
            elif kind == "file":
                data = payload or b""
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
            else:
                info.type = tarfile.SYMTYPE
                info.linkname = "run.sh"
                archive.addfile(info)


def test_install_update_reinstall_uninstall_and_recovery() -> None:
    with tempfile.TemporaryDirectory(prefix="installer-regression-") as tmp:
        root = Path(tmp)
        home = root / "home"
        home.mkdir()
        os.environ.update(env_for(home))
        source = make_release(root, "one")
        data = source / "DO_NOT_SHARE" / "cookies" / "native.sqlite"
        data.parent.mkdir(parents=True)
        data.write_bytes(b"keep-login")
        target = home / "install"

        conflict_target = home / "unknown-owner"
        conflict_target.mkdir()
        (conflict_target / "run.sh").write_text("pre-existing\n")
        conflict = run_cli(home, "install", "--source", str(source), "--prefix", str(conflict_target), "--yes", check=False)
        assert conflict.returncode != 0
        assert (conflict_target / "run.sh").read_text() == "pre-existing\n"

        run_cli(home, "install", "--source", str(source), "--prefix", str(target), "--yes")
        assert_manifest(target)
        assert (target / "DO_NOT_SHARE/cookies/native.sqlite").read_bytes() == b"keep-login"
        mime = home / "config/mimeapps.list"
        text = mime.read_text()
        assert "x-scheme-handler/roblox=roblox-mac-port.desktop;" in text
        assert (home / "data/applications/roblox-mac-port.desktop").is_file()

        # User configuration and unrelated files survive an update; a modified
        # application file is backed up instead of being silently discarded.
        (target / "FFlags.json").write_text('{"UserFlag":true}\n')
        (target / "run.sh").write_text("#!/bin/sh\n# user edit\n")
        (target / "user-not-owned.txt").write_text("keep\n")
        source2 = make_release(root, "two")
        run_cli(home, "update", "--source", str(source2), "--prefix", str(target), "--yes")
        assert (target / "FFlags.json").read_text() == '{"UserFlag":true}\n'
        assert b"version two" in (target / "run.sh").read_bytes()
        backups = list((target.parent / installer.BACKUP_PREFIX).glob("*/run.sh"))
        assert backups and backups[-1].read_text().find("user edit") >= 0
        assert (target / "user-not-owned.txt").read_text() == "keep\n"

        # Simulate a power loss after the first live rename. The next update
        # recovers the journal and then completes normally.
        original_replace = installer.os.replace
        crashed = {"value": False}
        def crash_once(src: str | os.PathLike[str], dst: str | os.PathLike[str]) -> None:
            if (Path(dst).parent == target and Path(src).parent.name == "payload" and not crashed["value"]):
                crashed["value"] = True
                raise KeyboardInterrupt
            original_replace(src, dst)
        installer.os.replace = crash_once
        try:
            try:
                installer.run_install_or_update(target, source, None, yes=True, confirmed=True)
            except KeyboardInterrupt:
                pass
        finally:
            installer.os.replace = original_replace
        assert crashed["value"]
        assert list(target.parent.glob(installer.TXN_PREFIX + "*")), "interrupted journal was not retained"
        installer.run_install_or_update(target, source, None, yes=True, confirmed=True)
        assert not list(target.parent.glob(installer.TXN_PREFIX + "*"))

        # Uninstall keeps data, the user file, modified backups, and unrelated
        # desktop associations. Reinstallation into the remaining directory
        # must also keep the existing data without merging it.
        run_cli(home, "uninstall", "--prefix", str(target), "--yes")
        assert (target / "DO_NOT_SHARE/cookies/native.sqlite").read_bytes() == b"keep-login"
        assert (target / "user-not-owned.txt").read_text() == "keep\n"
        assert target.is_dir()
        run_cli(home, "install", "--source", str(source), "--prefix", str(target), "--yes")
        assert (target / "DO_NOT_SHARE/cookies/native.sqlite").read_bytes() == b"keep-login"
        run_cli(home, "uninstall", "--prefix", str(target), "--dry-run")
        assert (target / installer.MANIFEST_NAME).is_file()


def test_symlink_archive_checksum_and_download_failures() -> None:
    with tempfile.TemporaryDirectory(prefix="installer-attacks-") as tmp:
        root = Path(tmp)
        home = root / "home"
        home.mkdir()
        os.environ.update(env_for(home))
        source = make_release(root)
        (source / "run.sh").unlink()
        (source / "run.sh").symlink_to("/etc/passwd")
        bad = run_cli(home, "install", "--source", str(source), "--prefix", str(home / "target"), "--yes", check=False)
        assert bad.returncode != 0
        symlink_target = home / "target-link"
        symlink_target.symlink_to(root)
        linked = run_cli(home, "install", "--source", str(make_release(root, "link")), "--prefix", str(symlink_target), "--yes", check=False)
        assert linked.returncode != 0

        malformed = root / "malformed.tar.gz"
        make_tar(malformed, [("RobloxLinuxRelease", None, "dir"), ("../escape", b"x", "file")])
        try:
            installer.extract_release_archive(malformed, root / "extract")
        except installer.InstallerError:
            pass
        else:
            raise AssertionError("path traversal archive was accepted")
        linked_archive = root / "linked.tar.gz"
        make_tar(linked_archive, [("RobloxLinuxRelease", None, "dir"), ("RobloxLinuxRelease/run.sh", None, "symlink")])
        try:
            installer.extract_release_archive(linked_archive, root / "extract-link")
        except installer.InstallerError:
            pass
        else:
            raise AssertionError("symlink archive was accepted")

        original_fetch = installer.fetch_json
        installer.fetch_json = lambda _url: {"tag_name": "v-test", "assets": [{"name": "RobloxLinuxRelease.tar.gz", "browser_download_url": "https://github.com/georgenoob1234/Roblox-Mac-Linux-Port/releases/x"}]}
        try:
            try:
                installer.latest_release_metadata()
            except installer.InstallerError:
                pass
            else:
                raise AssertionError("missing checksum was accepted")
        finally:
            installer.fetch_json = original_fetch

        installer.latest_release_metadata = lambda: {"tag": "v-test", "url": "https://github.com/georgenoob1234/Roblox-Mac-Linux-Port/releases/x", "digest": "a" * 64, "size": 10}
        original_download = installer.download_file
        installer.download_file = lambda *_args, **_kwargs: installer.fail("mock download failure")
        try:
            try:
                installer.get_source_for_action("update", home / "download-target", None)
            except installer.InstallerError:
                pass
            else:
                raise AssertionError("mock download failure did not fail")
        finally:
            installer.download_file = original_download
        assert not list(home.glob("roblox-mac-linux-port-download-*"))

        installer.download_file = lambda _url, destination, *_args, **_kwargs: Path(destination).write_bytes(b"wrong archive")
        try:
            try:
                installer.get_source_for_action("update", home / "checksum-target", None)
            except installer.InstallerError as exc:
                assert "SHA-256 verification failed" in str(exc)
            else:
                raise AssertionError("incorrect checksum was accepted")
        finally:
            installer.download_file = original_download
        assert not list(home.glob("roblox-mac-linux-port-download-*"))


def test_desktop_conflicts_and_association_preservation() -> None:
    with tempfile.TemporaryDirectory(prefix="installer-desktop-") as tmp:
        root = Path(tmp)
        home = root / "home"
        home.mkdir()
        source = make_release(root)
        target = home / "install"
        run_cli(home, "install", "--source", str(source), "--prefix", str(target), "--yes")
        desktop = home / "data/applications/roblox-mac-port.desktop"
        desktop.write_text("[Desktop Entry]\nName=Other App\n")
        mime = home / "config/mimeapps.list"
        mime.write_text("[Default Applications]\nx-scheme-handler/roblox=other.desktop;\nx-scheme-handler/http=browser.desktop;\n")
        conflict = run_cli(home, "desktop", "install", "--prefix", str(target), "--yes", check=False)
        assert conflict.returncode != 0
        assert "Other App" in desktop.read_text()
        assert "x-scheme-handler/http=browser.desktop;" in mime.read_text()
        run_cli(home, "desktop", "install", "--prefix", str(target), "--force-desktop", "--yes")
        assert "Other App" not in desktop.read_text()
        assert "other.desktop" in mime.read_text()
        assert "roblox-mac-port.desktop" in mime.read_text()
        assert list((home / "data/applications").glob(installer.BACKUP_PREFIX + "/*/roblox-mac-port.desktop"))
        run_cli(home, "desktop", "uninstall", "--prefix", str(target), "--yes")
        assert "other.desktop" in mime.read_text()
        assert "roblox-mac-port.desktop" not in mime.read_text()


if __name__ == "__main__":
    test_install_update_reinstall_uninstall_and_recovery()
    test_symlink_archive_checksum_and_download_failures()
    test_desktop_conflicts_and_association_preservation()
    print("PASS installer regression tests")
