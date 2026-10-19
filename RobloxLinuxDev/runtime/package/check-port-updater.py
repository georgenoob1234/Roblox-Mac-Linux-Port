#!/usr/bin/env python3
"""Offline port-release, installer-transaction and cancellation checks.

Every case runs against a disposable HOME/XDG tree and a tiny synthetic
release; nothing is downloaded and no Roblox client is involved. The launcher
and recovery paths are exercised through the real ``run.sh`` dispatch and
through the real standalone recovery entry point that ``installer.py``
publishes outside the payload.
"""
from __future__ import annotations

import importlib.util
import fcntl
import json
import os
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PACKAGE = ROOT / "RobloxLinuxDev/runtime/package"
INSTALLER = ROOT / "installer/installer.py"
STUB_APPIMAGE = b"#!/bin/sh\nexit 0\n"

spec = importlib.util.spec_from_file_location("port_installer", INSTALLER)
installer = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules[spec.name] = installer
spec.loader.exec_module(installer)
assert installer.build_parser().parse_args(["release-status", "--prefix", "/tmp/port", "--json"]).json is True

# A tiny stub bootstrapper UI that behaves like the GTK progress window: it
# reads the event stream, and optionally requests cancellation.
STUB_INSTALLER = '''#!/usr/bin/env python3
import json, os, sys, time
raw = os.environ.get("ROBLOX_MAC_PROGRESS_FD", "")
def emit(phase, percent, message, cancellable):
    if raw.isdigit():
        os.write(int(raw), (json.dumps({"phase": phase, "percent": percent,
            "message": message, "bytes_done": 0, "bytes_total": 0,
            "cancellable": cancellable}) + "\\n").encode())
marker = os.environ.get("ROBLOX_MAC_CANCEL_FILE", "")
state = os.environ.get("ROBLOX_MAC_CANCEL_STATE_FILE", "")
emit("downloading", 30, "Downloading", True)
if os.environ.get("PORT_STUB_REQUIRE_NO_MARKER") == "1" and marker and os.path.exists(marker):
    emit("failed", 0, "A stale cancellation marker was not cleared", False)
    raise SystemExit(1)
if os.environ.get("PORT_STUB_REQUIRE_OPEN_STATE") == "1":
    current = ""
    if state and os.path.exists(state):
        current = open(state).read().strip()
    if current != "open" or (marker and os.path.exists(marker)):
        emit("failed", 0, "Stale cancellation state was not cleared: " + repr(current), False)
        raise SystemExit(1)
if os.environ.get("PORT_STUB_POLL_CANCEL") == "1":
    for _ in range(30):
        if marker and os.path.exists(marker):
            emit("cancelled", 0, "Cancelled", False)
            raise SystemExit(125)
        time.sleep(0.01)
emit("installing", 94, "Installing", False)
if os.environ.get("PORT_STUB_LATE_CANCEL") == "1" and marker:
    open(marker, "w").write("late\\n")
result = int(os.environ.get("PORT_STUB_RESULT", "0"))
_phase, _message = {
    0: ("complete", "Port update complete"),
    3: ("complete", "Port update applied; finishing cleanup on the next start"),
    125: ("cancelled", "done"),
}.get(result, ("failed", "done"))
emit(_phase, 100 if result in (0, 3) else 0, _message, False)
raise SystemExit(result)
'''

TERMINAL = {"update": 0, "cancelled": 4, "failed": 1}


def cli_env(home: Path, **extra: str) -> dict[str, str]:
    env = os.environ.copy()
    env.update({
        "HOME": str(home),
        "XDG_STATE_HOME": str(home / "state"),
        "XDG_DATA_HOME": str(home / "data"),
        "XDG_CONFIG_HOME": str(home / "config"),
        "XDG_SESSION_TYPE": "wayland",
        "WAYLAND_DISPLAY": "wayland-test",
    })
    env.update(extra)
    return env


def cli(env: dict[str, str], *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(INSTALLER), *args], env=env,
                          capture_output=True, text=True)


def release(root: Path, version: str) -> Path:
    """A minimal synthetic release used for the payload/transaction cases."""
    source = root / f"source-{version}"
    source.mkdir()
    files = {
        "FFlags.json": b"{}\n", "RobloxLinux.AppImage": STUB_APPIMAGE,
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


def real_release(root: Path, version: str) -> Path:
    """A release built from the real launcher scripts plus a stub AppImage.

    Only the files that actually ship in the package directory are copied in;
    FFlags.json and the (very large) AppImage are synthesised.
    """
    source = root / f"real-{version}"
    source.mkdir()
    for name in ("bootstrap.py", "bootstrap.sh", "icon.png", "icon_bw.png",
                 "install-uri-handler.sh", "run.sh", "update-roblox.sh", ".gitignore"):
        shutil.copy2(PACKAGE / name, source / name)
    # Keep the two versions distinguishable so a real update replaces files.
    for name in ("run.sh", "bootstrap.sh"):
        path = source / name
        path.write_bytes(path.read_bytes() + f"# {version}\n".encode())
    (source / "FFlags.json").write_text("{}\n")
    # A functional installer that differs between versions, so the update
    # really replaces it (a release may carry its own installer.py).
    (source / "installer.py").write_bytes(INSTALLER.read_bytes() + f"# {version}\n".encode())
    (source / "installer.py").chmod(0o755)
    (source / "RobloxLinux.AppImage").write_bytes(STUB_APPIMAGE)
    (source / "RobloxLinux.AppImage").chmod(0o755)
    (source / installer.PORT_VERSION_FILE).write_text(version + "\n")
    return source


def managed_snapshot(target: Path) -> dict[str, bytes]:
    names = installer.REQUIRED_FILES | {"installer.py", installer.PORT_VERSION_FILE,
                                        installer.MANIFEST_NAME}
    return {name: (target / name).read_bytes() for name in names if (target / name).is_file()}


def install_quietly(target: Path, source: Path, version: str) -> None:
    installer.run_install_or_update(target, source, version, yes=True, confirmed=True,
                                    install_desktop_integration=False)


def interrupt_update(target: Path, source: Path, version: str, victim: str) -> None:
    """Kill an update at the exact rename that replaces ``victim``."""
    original_replace = installer.os.replace
    try:
        def interrupt(src, dst):
            if Path(dst) == target / victim:
                raise KeyboardInterrupt()
            return original_replace(src, dst)
        installer.os.replace = interrupt
        try:
            install_quietly(target, source, version)
        except KeyboardInterrupt:
            pass
    finally:
        installer.os.replace = original_replace


def run_launcher(env: dict[str, str], *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["sh", *args], env=env, capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix="port-updater-") as tmpdir:
    root = Path(tmpdir)
    home = root / "home"
    home.mkdir()
    # Keep the installer's state directory (and therefore the published
    # recovery helper) inside the disposable tree.
    for key, value in (("HOME", home), ("XDG_STATE_HOME", home / "state"),
                       ("XDG_DATA_HOME", home / "data"), ("XDG_CONFIG_HOME", home / "config")):
        os.environ[key] = str(value)

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

    # ------------------------------------------------------------------
    # Cancellation is honoured at every cancellable phase and never after the
    # commit boundary. Each case asserts the real exception/terminal status
    # plus the on-disk result.
    # ------------------------------------------------------------------
    marker_path = root / "cancel-marker"

    # A cancellation that arrives before any work begins.
    cancel_target = root / "cancel-pre"
    install_quietly(cancel_target, source1, "v1.0.0")
    baseline = managed_snapshot(cancel_target)
    marker_path.write_text("cancel\n")
    os.environ["ROBLOX_MAC_CANCEL_FILE"] = str(marker_path)
    try:
        try:
            install_quietly(cancel_target, source2, "v2.0.0")
        except installer.CancelledError as exc:
            assert "cancelled" in str(exc).lower()
        else:
            raise AssertionError("early cancellation was ignored")
    finally:
        os.environ.pop("ROBLOX_MAC_CANCEL_FILE", None)
    assert managed_snapshot(cancel_target) == baseline
    assert not list(cancel_target.parent.glob(installer.TXN_PREFIX + "*"))
    marker_path.unlink()
    # From here on the marker path is configured; each case controls the
    # request by creating or removing the marker file itself.
    os.environ["ROBLOX_MAC_CANCEL_FILE"] = str(marker_path)

    # A cancellation that arrives mid-download: the real download loop must
    # notice the marker between chunks and abort without touching the install.
    download_target = root / "cancel-download"
    install_quietly(download_target, source1, "v1.0.0")
    download_baseline = managed_snapshot(download_target)
    archive_dest = root / "cancel-download.tar.gz"
    reads = {"count": 0}

    class FakeResponse:
        headers = {"Content-Length": str(4 * 1024 * 1024)}

        def read(self, _size: int) -> bytes:
            reads["count"] += 1
            if reads["count"] == 2:
                marker_path.write_text("cancel\n")  # the user hits Cancel mid-stream
            if reads["count"] > 4:
                return b""
            return b"x" * (1024 * 1024)

        def __enter__(self):
            return self

        def __exit__(self, *_exc):
            return False

    original_urlopen = installer.urllib.request.urlopen
    installer.urllib.request.urlopen = lambda *_a, **_k: FakeResponse()
    try:
        try:
            installer.download_file(
                "https://github.com/georgenoob1234/Roblox-Mac-Linux-Port/releases/a", archive_dest)
        except installer.CancelledError:
            pass
        else:
            raise AssertionError("a cancellation during the download was ignored")
    finally:
        installer.urllib.request.urlopen = original_urlopen
    # Two chunks were received before the marker was written; the loop then
    # observed it at the next iteration, so the abort happened mid-download.
    assert reads["count"] >= 2, reads["count"]
    assert managed_snapshot(download_target) == download_baseline
    marker_path.unlink()

    # A cancellation that arrives mid-staging, between archive members.
    staging_target = root / "cancel-staging"
    install_quietly(staging_target, source1, "v1.0.0")
    staging_baseline = managed_snapshot(staging_target)
    staging_archive = root / "staging.tar.gz"
    import io
    import tarfile
    with tarfile.open(staging_archive, "w:gz") as archive:
        for name, payload in (("RobloxLinuxRelease", None), ("RobloxLinuxRelease/run.sh", b"a"),
                              ("RobloxLinuxRelease/bootstrap.sh", b"b"),
                              ("RobloxLinuxRelease/update-roblox.sh", b"c"),
                              ("RobloxLinuxRelease/FFlags.json", b"d"),
                              ("RobloxLinuxRelease/icon.png", b"e"),
                              ("RobloxLinuxRelease/icon_bw.png", b"f"),
                              ("RobloxLinuxRelease/bootstrap.py", b"g"),
                              ("RobloxLinuxRelease/RobloxLinux.AppImage", b"h"),
                              ("RobloxLinuxRelease/install-uri-handler.sh", b"i")):
            info = tarfile.TarInfo(name)
            if payload is None:
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                archive.addfile(info)
            else:
                info.size = len(payload)
                info.mode = 0o755
                archive.addfile(info, io.BytesIO(payload))
    original_copyfileobj = installer.shutil.copyfileobj

    def cancel_after_first_member(src, dst, *args, **kwargs):
        result = original_copyfileobj(src, dst, *args, **kwargs)
        marker_path.write_text("cancel\n")
        return result

    installer.shutil.copyfileobj = cancel_after_first_member
    try:
        try:
            installer.extract_release_archive(staging_archive, root / "staging-out")
        except installer.CancelledError:
            pass
        else:
            raise AssertionError("a cancellation during staging was ignored")
    finally:
        installer.shutil.copyfileobj = original_copyfileobj
    assert managed_snapshot(staging_target) == staging_baseline
    marker_path.unlink()

    # A cancellation that arrives immediately before the commit boundary is
    # still accepted: the request is written as soon as staging finishes,
    # which is the last moment the installer still honours the marker.
    boundary_target = root / "cancel-boundary"
    install_quietly(boundary_target, source1, "v1.0.0")
    boundary_baseline = managed_snapshot(boundary_target)
    original_stage = installer.copy_payload_to_stage

    def marker_after_staging(source, names, stage, release_tag):
        manifest = original_stage(source, names, stage, release_tag)
        marker_path.write_text("cancel\n")
        return manifest

    installer.copy_payload_to_stage = marker_after_staging
    try:
        try:
            install_quietly(boundary_target, source2, "v2.0.0")
        except installer.CancelledError:
            pass
        else:
            raise AssertionError("a cancellation at the commit boundary was ignored")
    finally:
        installer.copy_payload_to_stage = original_stage
    assert managed_snapshot(boundary_target) == boundary_baseline
    assert not list(boundary_target.parent.glob(installer.TXN_PREFIX + "*"))
    marker_path.unlink()

    # A cancellation that arrives after the commit began is rejected: the
    # operation reports success and installs the new release.
    late_target = root / "cancel-late"
    install_quietly(late_target, source1, "v1.0.0")
    original_progress = installer.progress_event

    def marker_during_commit(phase, percent, message, bytes_done=0, bytes_total=0, cancellable=True):
        original_progress(phase, percent, message, bytes_done, bytes_total, cancellable)
        if phase == "installing":
            marker_path.write_text("cancel\n")

    installer.progress_event = marker_during_commit
    try:
        install_quietly(late_target, source2, "v2.0.0")
    finally:
        installer.progress_event = original_progress
    assert marker_path.exists()
    assert (late_target / "run.sh").read_bytes() == (source2 / "run.sh").read_bytes()
    assert installer.installed_port_version(late_target) == "v2.0.0"
    marker_path.unlink()

    # A genuine transaction failure is reported as a failure, never as a
    # cancellation, even when a cancellation request is present at that time.
    fail_target = root / "commit-failure"
    install_quietly(fail_target, source1, "v1.0.0")
    fail_baseline = managed_snapshot(fail_target)
    original_replace = installer.os.replace
    seen = {"count": 0}

    def fail_during_commit(src, dst):
        if Path(dst).parent == fail_target and Path(src).parent.name == "payload":
            seen["count"] += 1
            if seen["count"] == 2:
                marker_path.write_text("cancel\n")
                raise OSError("simulated commit failure")
        return original_replace(src, dst)

    installer.os.replace = fail_during_commit
    try:
        try:
            install_quietly(fail_target, source2, "v2.0.0")
        except Exception as exc:
            # The failure must be reported as a failure -- never as the
            # cancellation that was requested at the same moment.
            assert not isinstance(exc, installer.CancelledError), exc
            assert "simulated commit failure" in str(exc), exc
        else:
            raise AssertionError("a transaction failure was ignored")
    finally:
        installer.os.replace = original_replace
    assert marker_path.exists()
    assert managed_snapshot(fail_target) == fail_baseline
    assert not list(fail_target.parent.glob(installer.TXN_PREFIX + "*"))
    marker_path.unlink()

    # ------------------------------------------------------------------
    # A stalled cancellable phase is bounded by the pre-commit watchdog, while
    # the commit phase itself is never interrupted.
    # ------------------------------------------------------------------
    stalled = installer._PreCommitWatchdog(0.0)
    stalled.arm()
    try:
        time.sleep(0.05)
        raise AssertionError("a stalled cancellable phase was not bounded")
    except installer.CancelledError:
        raise AssertionError("a stall was reported as a cancellation")
    except installer.InstallerError as exc:
        assert "stopped making progress" in str(exc), exc
    finally:
        stalled.disarm()

    marker_path.write_text("cancel\n")
    cancelled_stall = installer._PreCommitWatchdog(0.0)
    cancelled_stall.arm()
    try:
        time.sleep(0.05)
        raise AssertionError("an accepted cancellation was not reported")
    except installer.CancelledError:
        pass
    finally:
        cancelled_stall.disarm()
        marker_path.unlink()

    # The watchdog is disarmed exactly at the commit boundary, so the commit
    # can never be interrupted by it.
    armed_target = root / "watchdog-boundary"
    install_quietly(armed_target, source1, "v1.0.0")
    observed = {}
    original_apply = installer.apply_transaction

    def apply_checked(*args, **kwargs):
        observed["armed_at_commit"] = installer._active_watchdog is not None
        return original_apply(*args, **kwargs)

    installer.apply_transaction = apply_checked
    installer.arm_pre_commit_watchdog()
    try:
        install_quietly(armed_target, source2, "v2.0.0")
    finally:
        installer.apply_transaction = original_apply
        installer.disarm_pre_commit_watchdog()
    assert observed.get("armed_at_commit") is False, observed
    assert installer._active_watchdog is None
    assert installer.installed_port_version(armed_target) == "v2.0.0"

    # ------------------------------------------------------------------
    # Real CLI terminal statuses: cancel -> 125, stalled phases -> failure.
    # ------------------------------------------------------------------
    cli_home = root / "cli-home"
    cli_home.mkdir()
    cli_env_map = cli_env(cli_home)
    cli_target = root / "cli-install"
    installed = cli(cli_env_map, "install", "--source", str(source1), "--prefix", str(cli_target),
                    "--yes", "--no-desktop")
    assert installed.returncode == 0, (installed.returncode, installed.stderr)
    cli_baseline = managed_snapshot(cli_target)
    cli_marker = root / "cli-marker"
    cli_marker.write_text("cancel\n")
    cancelled_cli = cli({**cli_env_map, "ROBLOX_MAC_CANCEL_FILE": str(cli_marker)},
                        "update", "--source", str(source2), "--prefix", str(cli_target),
                        "--yes", "--no-desktop")
    assert cancelled_cli.returncode == installer.EXIT_CANCELLED, \
        (cancelled_cli.returncode, cancelled_cli.stderr)
    assert managed_snapshot(cli_target) == cli_baseline
    assert not list(cli_target.parent.glob(installer.TXN_PREFIX + "*"))
    cli_marker.unlink()
    # A deadline that expires during the cancellable phases is a failure, and
    # the install is left untouched.
    timed_out = cli({**cli_env_map, "ROBLOX_MAC_PORT_UPDATE_TIMEOUT": "0.001"},
                    "update", "--source", str(source2), "--prefix", str(cli_target),
                    "--yes", "--no-desktop")
    assert timed_out.returncode == 1, (timed_out.returncode, timed_out.stderr)
    assert managed_snapshot(cli_target) == cli_baseline
    assert not list(cli_target.parent.glob(installer.TXN_PREFIX + "*"))

    # ------------------------------------------------------------------
    # Recovery of an interrupted port update through the real entry points.
    # ------------------------------------------------------------------
    real1 = real_release(root, "v1.0.0")
    real2 = real_release(root, "v2.0.0")
    real3 = real_release(root, "v3.0.0")

    def prepare_recovery_target(name: str) -> Path:
        path = root / name
        install_quietly(path, real1, "v1.0.0")
        (path / "DO_NOT_SHARE/diagnostics").mkdir(parents=True, exist_ok=True)
        (path / "DO_NOT_SHARE/diagnostics/log").write_text("persistent\n")
        (path / "user-extra").write_text("unrelated\n")
        return path

    # (1) The transaction interrupted while replacing installer.py leaves the
    # managed installer missing. Recovery must not depend on it: the launcher
    # has to reach the payload-independent helper published earlier.
    missing_installer = prepare_recovery_target("rec-missing-installer")
    expected = managed_snapshot(missing_installer)
    interrupt_update(missing_installer, real2, "v2.0.0", "installer.py")
    assert not (missing_installer / "installer.py").exists(), "installer.py was not left missing"
    assert (missing_installer / "run.sh").is_file()
    assert list(missing_installer.parent.glob(installer.TXN_PREFIX + "*"))
    helper = installer.recovery_helper_script()
    assert helper.is_file() and os.access(helper, os.X_OK), helper
    assert helper == installer.recovery_helper_dir() / "recover.sh"
    launcher_env = cli_env(home)
    launcher_env["XDG_RUNTIME_DIR"] = str(root / "runtime-road")
    (root / "runtime-road").mkdir()
    launched = run_launcher(launcher_env, str(missing_installer / "run.sh"), "--diagnose")
    assert launched.returncode == 0, (launched.returncode, launched.stdout, launched.stderr)
    assert managed_snapshot(missing_installer) == expected
    assert not list(missing_installer.parent.glob(installer.TXN_PREFIX + "*"))
    assert (missing_installer / "DO_NOT_SHARE/diagnostics/log").read_text() == "persistent\n"
    assert (missing_installer / "user-extra").read_text() == "unrelated\n"

    # (2) The transaction interrupted while replacing run.sh leaves the
    # launcher missing, so no run.sh can execute. The standalone recovery
    # entry point must restore it on its own.
    missing_run = prepare_recovery_target("rec-missing-run")
    expected_run = managed_snapshot(missing_run)
    interrupt_update(missing_run, real2, "v2.0.0", "run.sh")
    assert not (missing_run / "run.sh").exists(), "run.sh was not left missing"
    assert list(missing_run.parent.glob(installer.TXN_PREFIX + "*"))
    recovered = run_launcher(cli_env(home), str(installer.recovery_helper_script()), str(missing_run))
    assert recovered.returncode == 0, (recovered.returncode, recovered.stdout, recovered.stderr)
    assert managed_snapshot(missing_run) == expected_run
    assert not list(missing_run.parent.glob(installer.TXN_PREFIX + "*"))
    assert (missing_run / "DO_NOT_SHARE/diagnostics/log").read_text() == "persistent\n"
    assert (missing_run / "user-extra").read_text() == "unrelated\n"
    # The restored launcher is a working, byte-identical copy of the old one.
    assert (missing_run / "run.sh").read_bytes() == (real1 / "run.sh").read_bytes()
    relaunched = run_launcher({**cli_env(home), "XDG_RUNTIME_DIR": str(root / "runtime-road")},
                              str(missing_run / "run.sh"), "--diagnose")
    assert relaunched.returncode == 0, (relaunched.returncode, relaunched.stderr)

    # (3) Recovery is idempotent: repeating it changes nothing and never
    # deletes ambiguous evidence that is not a transaction.
    again = run_launcher(cli_env(home), str(installer.recovery_helper_script()), str(missing_run))
    assert again.returncode == 0
    assert managed_snapshot(missing_run) == expected_run
    installer.recover_transactions(missing_run)

    # (4) An interruption while replacing the manifest rolls back every file
    # that was already replaced, including the manifest itself.
    manifest_target = prepare_recovery_target("rec-manifest")
    expected_manifest = managed_snapshot(manifest_target)
    interrupt_update(manifest_target, real2, "v2.0.0", installer.MANIFEST_NAME)
    assert list(manifest_target.parent.glob(installer.TXN_PREFIX + "*"))
    installer.recover_transactions(manifest_target)
    assert managed_snapshot(manifest_target) == expected_manifest
    assert installer.installed_port_version(manifest_target) == "v1.0.0"
    assert (manifest_target / "DO_NOT_SHARE/diagnostics/log").read_text() == "persistent\n"
    assert (manifest_target / "user-extra").read_text() == "unrelated\n"

    # (5) Unexpected user files present during recovery are preserved, and the
    # managed files are restored around them.
    user_target = prepare_recovery_target("rec-userfiles")
    (user_target / "DO_NOT_SHARE/session.sqlite").write_text("login\n")
    (user_target / "notes.txt").write_text("mine\n")
    expected_user = managed_snapshot(user_target)
    interrupt_update(user_target, real2, "v2.0.0", "installer.py")
    assert list(user_target.parent.glob(installer.TXN_PREFIX + "*"))
    installer.recover_transactions(user_target)
    assert (user_target / "DO_NOT_SHARE/session.sqlite").read_text() == "login\n"
    assert (user_target / "notes.txt").read_text() == "mine\n"
    for name, data in expected_user.items():
        assert (user_target / name).read_bytes() == data, name

    # (6) A user edit made to a managed file after the interruption is kept as
    # evidence instead of being silently discarded, while the original is
    # restored so the installation is usable again.
    edited_target = prepare_recovery_target("rec-edited")
    expected_edited = managed_snapshot(edited_target)
    interrupt_update(edited_target, real2, "v2.0.0", installer.MANIFEST_NAME)
    assert (edited_target / "run.sh").read_bytes() != expected_edited["run.sh"], \
        "the interrupted update did not replace run.sh"
    (edited_target / "run.sh").write_text("#!/bin/sh\n# edited after the crash\n")
    installer.recover_transactions(edited_target)
    assert (edited_target / "run.sh").read_bytes() == expected_edited["run.sh"]
    for name, data in expected_edited.items():
        assert (edited_target / name).read_bytes() == data, name
    preserved = [p for p in (edited_target.parent / installer.BACKUP_PREFIX).glob("*/*") if p.is_file()]
    assert any("edited after the crash" in p.read_text(errors="ignore") for p in preserved), \
        "the concurrent edit was not preserved as evidence"

    # ------------------------------------------------------------------
    # The real run.sh dispatch must answer the version query before creating
    # any launch-only temporary resource, and must not launch anything.
    # ------------------------------------------------------------------
    query = root / "query"
    query.mkdir()
    shutil.copy2(PACKAGE / "run.sh", query / "run.sh")
    shutil.copy2(PACKAGE / "bootstrap.py", query / "bootstrap.py")
    (query / "RobloxLinux.AppImage").write_text(
        "#!/bin/sh\nprintf launched > \"$(dirname -- \"$0\")/launched.sentinel\"\nexit 0\n")
    (query / "RobloxLinux.AppImage").chmod(0o755)
    (query / installer.PORT_VERSION_FILE).write_text("v9.9.9\n")
    runtime_dir = Path(tempfile.mkdtemp(prefix="rt", dir="/tmp"))
    query_env = {**os.environ, "XDG_RUNTIME_DIR": str(runtime_dir)}
    query_env.pop("ROBLOX_MAC_UI_APP", None)
    before_query = set(query.iterdir())
    before_runtime = set(runtime_dir.iterdir())
    for _ in range(5):
        queried = subprocess.run(["sh", str(query / "run.sh"), "port-version"],
                                 env=query_env, capture_output=True, text=True, check=True)
        assert queried.stdout.strip() == "v9.9.9"
    assert set(query.iterdir()) == before_query, "the version query created files beside the install"
    assert set(runtime_dir.iterdir()) == before_runtime, "the version query leaked a launch directory"
    assert not (query / "launched.sentinel").exists(), "the version query launched the client"
    assert not (query / "DO_NOT_SHARE").exists(), "the version query created launch state"
    bad_query = subprocess.run(["sh", str(query / "run.sh"), "port-version", "extra"],
                               env=query_env, capture_output=True, text=True)
    assert bad_query.returncode == 2
    shutil.rmtree(runtime_dir, ignore_errors=True)

    # ------------------------------------------------------------------
    # The shell wrapper's terminal status is the installer's own status, and
    # Settings is reopened in every case.
    # ------------------------------------------------------------------
    handoff = root / "handoff"
    handoff.mkdir()
    (handoff / "DO_NOT_SHARE").mkdir()
    shutil.copy2(PACKAGE / "bootstrap.sh", handoff / "bootstrap.sh")
    shutil.copy2(PACKAGE / "bootstrap.py", handoff / "bootstrap.py")
    (handoff / "installer.py").write_text(STUB_INSTALLER)
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
    marker = handoff / "DO_NOT_SHARE/.bootstrap-cancel"
    env = {**os.environ, "ROBLOX_MAC_UI_APP": str(ui), "EVENT_LOG": str(event_log),
           "SETTINGS_LOG": str(settings_log)}

    def handoff_run(extra_env: dict[str, str]) -> subprocess.CompletedProcess[str]:
        settings_log.write_text("")
        return subprocess.run([handoff / "bootstrap.sh", "port-update-settings"],
                              env={**env, **extra_env}, capture_output=True, text=True)

    def phases() -> list[str]:
        return [json.loads(line)["phase"] for line in event_log.read_text().splitlines()]

    # Success: the installer reports its own success and Settings reopen.
    result = handoff_run({"PORT_STUB_RESULT": "0"})
    assert result.returncode == 0, (result.returncode, result.stderr)
    assert settings_log.read_text().splitlines() == ["settings"]
    assert phases() == ["checking", "downloading", "installing", "complete", "update"], phases()
    assert not marker.exists()

    # An accepted cancellation is reported by the installer and forwarded.
    cancelled = handoff_run({"PORT_STUB_RESULT": "125"})
    assert cancelled.returncode == 125, (cancelled.returncode, cancelled.stderr)
    assert settings_log.read_text().splitlines() == ["settings"]
    assert phases()[-1] == "cancelled"
    assert not marker.exists()

    # A genuine failure stays a failure even when a cancellation request
    # appears at the same time: the wrapper must never re-read the marker.
    failed = handoff_run({"PORT_STUB_RESULT": "1", "PORT_STUB_LATE_CANCEL": "1"})
    assert failed.returncode == 1, (failed.returncode, failed.stderr)
    assert settings_log.read_text().splitlines() == ["settings"]
    assert phases()[-1] == "failed"

    # A late cancellation request after a successful commit does not turn the
    # success into a cancellation.
    late = handoff_run({"PORT_STUB_RESULT": "0", "PORT_STUB_LATE_CANCEL": "1"})
    assert late.returncode == 0, (late.returncode, late.stderr)
    assert settings_log.read_text().splitlines() == ["settings"]
    assert phases()[-1] == "update"

    # A stale marker from a previous operation is cleared before the next one
    # starts, so it can never cancel it.
    marker.write_text("stale\n")
    stale = handoff_run({"PORT_STUB_RESULT": "0", "PORT_STUB_REQUIRE_NO_MARKER": "1"})
    assert stale.returncode == 0, (stale.returncode, stale.stderr)
    assert settings_log.read_text().splitlines() == ["settings"]
    assert not marker.exists()

    # The same reset also clears a stale *state* file: a leftover "committing"
    # from an interrupted operation must not make a fresh update look
    # uncancellable, and a leftover request must not cancel it.
    state_file = handoff / "DO_NOT_SHARE/.bootstrap-cancel-state"
    state_file.write_text("committing\n")
    marker.write_text("stale\n")
    stale_state = handoff_run({"PORT_STUB_RESULT": "0", "PORT_STUB_REQUIRE_OPEN_STATE": "1"})
    assert stale_state.returncode == 0, (stale_state.returncode, stale_state.stderr)
    assert settings_log.read_text().splitlines() == ["settings"]
    assert not marker.exists()
    assert state_file.read_text().strip() == "open"

    # An update that committed but could not finish its backup cleanup is
    # reported as applied: there must be no "failed"/"kept the previous
    # release" wording, and Settings still reopen.
    cleanup = handoff_run({"PORT_STUB_RESULT": "3"})
    assert cleanup.returncode == 0, (cleanup.returncode, cleanup.stderr)
    assert settings_log.read_text().splitlines() == ["settings"]
    last_event = json.loads(event_log.read_text().splitlines()[-1])
    assert last_event["phase"] == "update", last_event
    assert "cleanup" in last_event["message"], last_event
    assert "failed" not in phases(), phases()

    # A cancel requested through the progress window during the cancellable
    # phase reaches the installer, which reports the accepted cancellation the
    # wrapper forwards; Settings is still reopened.
    cancel_ui = root / "port-cancel-ui"
    cancel_ui.write_text("#!/bin/sh\ncase \"${2:-}\" in\n  --progress) touch \"$ROBLOX_MAC_CANCEL_FILE\"; cat > \"$EVENT_LOG\"; exit 4;;\nesac\nexit 3\n")
    cancel_ui.chmod(0o755)
    settings_log.write_text("")
    cancelled_ui = subprocess.run([handoff / "bootstrap.sh", "port-update-settings"],
                                  env={**env, "ROBLOX_MAC_UI_APP": str(cancel_ui),
                                       "PORT_STUB_RESULT": "0", "PORT_STUB_POLL_CANCEL": "1"},
                                  capture_output=True, text=True)
    assert cancelled_ui.returncode == 125, (cancelled_ui.returncode, cancelled_ui.stderr)
    assert settings_log.read_text().splitlines() == ["settings"]
    assert not marker.exists()

    # ------------------------------------------------------------------
    # The commit boundary is settled by a lock-protected state file shared
    # with the GTK UI -- not by progress-event delivery. These cases drive the
    # exact boundary; a marker written after the "installing" event would not
    # prove anything.
    # ------------------------------------------------------------------
    def ui_cancel_request(state_path: Path, request_marker: Path) -> bool:
        """Mirror of bootstrapper-ui.c's progress_try_request_cancel().

        It takes the same lock, reads the same state, and records the request
        only while the state is still "open" -- so if this returns False the UI
        would have refused and shown the refusal, never "Cancelling…".
        """
        fd = os.open(str(state_path) + ".lock", os.O_RDWR | os.O_CREAT, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            try:
                current = state_path.read_text().strip()
            except OSError:
                current = ""
            if current in ("", "open"):
                request_marker.write_text("cancel\n")
                return True
            return False
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)

    def read_state(state_path: Path) -> str:
        try:
            return state_path.read_text().strip()
        except OSError:
            return ""

    def with_boundary_hook(hook):
        """Patch the call made immediately before the boundary decision."""
        original = installer.disarm_pre_commit_watchdog
        fired = {"done": False}

        def wrapper():
            if not fired["done"]:
                fired["done"] = True
                hook()
            return original()

        installer.disarm_pre_commit_watchdog = wrapper
        return lambda: setattr(installer, "disarm_pre_commit_watchdog", original)

    def with_commit_hook(hook):
        """Patch the commit itself, which only runs after the boundary is won."""
        original = installer.apply_transaction
        seen = {}

        def wrapper(*args, **kwargs):
            hook(seen)
            return original(*args, **kwargs)

        installer.apply_transaction = wrapper
        return lambda: setattr(installer, "apply_transaction", original), seen

    boundary_state = root / "boundary-state"
    boundary_marker = root / "boundary-marker"
    saved_cancel_env = {key: os.environ.get(key) for key in
                        ("ROBLOX_MAC_CANCEL_STATE_FILE", "ROBLOX_MAC_CANCEL_FILE")}
    os.environ["ROBLOX_MAC_CANCEL_STATE_FILE"] = str(boundary_state)
    os.environ["ROBLOX_MAC_CANCEL_FILE"] = str(boundary_marker)
    def fresh(name: str) -> Path:
        """A target already on the old release, with a clean "open" state.

        This mirrors start_progress, which clears a previous operation's marker
        and state before a new one begins.
        """
        path = root / name
        boundary_state.write_text("open\n")
        boundary_marker.unlink(missing_ok=True)
        install_quietly(path, real1, "v1.0.0")
        # The baseline install crossed a boundary of its own; restore the
        # pre-operation state the launcher would have written.
        boundary_state.write_text("open\n")
        boundary_marker.unlink(missing_ok=True)
        return path

    try:
        # (1) Cancellation wins immediately before the commit: the UI records
        # the request at the instant before the boundary, while the state is
        # still "open". The operation must abort, keep the old release, leave
        # no transaction/backup behind, and report the cancellation status.
        wins = fresh("boundary-cancel-wins")
        wins_baseline = managed_snapshot(wins)
        accepted = {"value": None}

        def request_at_boundary():
            accepted["value"] = ui_cancel_request(boundary_state, boundary_marker)

        restore = with_boundary_hook(request_at_boundary)
        try:
            try:
                install_quietly(wins, real2, "v2.0.0")
            except installer.CancelledError:
                pass
            else:
                raise AssertionError("a request recorded before the boundary was not honoured")
        finally:
            restore()
        assert accepted["value"] is True, accepted
        assert read_state(boundary_state) == "cancelled", read_state(boundary_state)
        assert managed_snapshot(wins) == wins_baseline, "the old release must be intact"
        assert not list(wins.parent.glob(installer.TXN_PREFIX + "*")), "no transaction may remain"

        # (2) Commit wins immediately before a cancellation request: the state
        # is already "committing" when the click arrives, so the request must
        # be refused -- the UI must not claim it was accepted -- and the new
        # release must complete.
        commits = fresh("boundary-commit-wins")

        def request_after_boundary(s):
            s["state_at_commit"] = read_state(boundary_state)
            s["accepted_late"] = ui_cancel_request(boundary_state, boundary_marker)
            s["marker_after"] = boundary_marker.exists()

        restore, seen = with_commit_hook(request_after_boundary)
        try:
            install_quietly(commits, real2, "v2.0.0")
        finally:
            restore()
        assert seen["state_at_commit"] == "committing", seen
        assert seen["accepted_late"] is False, seen
        assert seen["marker_after"] is False, "a refused request must not record a marker"
        assert read_state(boundary_state) == "committed", read_state(boundary_state)
        assert installer.installed_port_version(commits) == "v2.0.0"
        assert (commits / "run.sh").read_bytes() == (real2 / "run.sh").read_bytes()

        # (3) The request arrives while the UI has not yet processed the
        # non-cancellable "installing" event -- the exact defect. The state
        # file, not event delivery, must decide: the request is refused even
        # though the button was still enabled.
        pending = fresh("boundary-event-pending")
        delivered = {}
        original_progress = installer.progress_event

        def observe_progress(phase, percent, message, bytes_done=0, bytes_total=0, cancellable=True):
            if phase == "installing":
                # Emitted, but the UI has deliberately not acted on it, so its
                # Cancel button is still enabled in this scenario.
                delivered["installing_emitted"] = True
                delivered["ui_processed"] = False
            return original_progress(phase, percent, message, bytes_done, bytes_total, cancellable)

        def request_before_processing(s):
            s["state_during_unprocessed_event"] = read_state(boundary_state)
            s["accepted"] = ui_cancel_request(boundary_state, boundary_marker)

        installer.progress_event = observe_progress
        restore, seen = with_commit_hook(request_before_processing)
        try:
            install_quietly(pending, real2, "v2.0.0")
        finally:
            installer.progress_event = original_progress
            restore()
        assert delivered.get("installing_emitted") is True, delivered
        assert delivered.get("ui_processed") is False, delivered
        assert seen["state_during_unprocessed_event"] == "committing", seen
        assert seen["accepted"] is False, seen
        assert not boundary_marker.exists()
        assert installer.installed_port_version(pending) == "v2.0.0"

        # (4) A late cancellation marker does not change a successful result.
        late_success = fresh("boundary-late-marker")

        def mark_late(s):
            boundary_marker.write_text("late\n")

        restore, seen = with_commit_hook(mark_late)
        try:
            install_quietly(late_success, real2, "v2.0.0")
        finally:
            restore()
        assert boundary_marker.exists()
        assert read_state(boundary_state) == "committed"
        assert installer.installed_port_version(late_success) == "v2.0.0"
        boundary_marker.unlink()

        # (5) A genuine failure past the boundary stays a failure: it must not
        # be reported as the cancellation that was (correctly) refused, and the
        # committed-then-failed transaction must roll back to the old release.
        failing = fresh("boundary-commit-failure")
        failing_baseline = managed_snapshot(failing)
        original_replace = installer.os.replace
        calls = {"n": 0}

        def fail_mid_commit(src, dst):
            if Path(dst).parent == failing and Path(src).parent.name == "payload":
                calls["n"] += 1
                if calls["n"] == 2:
                    boundary_marker.write_text("late\n")
                    raise OSError("simulated failure after the boundary")
            return original_replace(src, dst)

        installer.os.replace = fail_mid_commit
        try:
            try:
                install_quietly(failing, real2, "v2.0.0")
            except Exception as exc:
                assert not isinstance(exc, installer.CancelledError), exc
                assert "simulated failure after the boundary" in str(exc), exc
            else:
                raise AssertionError("a post-boundary failure was ignored")
        finally:
            installer.os.replace = original_replace
        assert managed_snapshot(failing) == failing_baseline, "a failed commit must roll back"
        assert not list(failing.parent.glob(installer.TXN_PREFIX + "*"))
        boundary_marker.unlink()

        # (6) A subsequent update is unaffected by the state the cancelled
        # operation left behind: the launcher resets it to "open".
        follow = fresh("boundary-follow-up")
        # The state a cancelled operation leaves behind.
        boundary_state.write_text("cancelled\n")
        boundary_marker.write_text("stale\n")
        # start_progress performs exactly this reset before every operation.
        boundary_marker.unlink()
        installer.write_cancel_state(installer.CANCEL_STATE_OPEN)
        install_quietly(follow, real2, "v2.0.0")
        assert installer.installed_port_version(follow) == "v2.0.0"
        assert read_state(boundary_state) == "committed"

        # The boundary and the request are mutually exclusive: over many
        # simultaneous attempts, an accepted request is always honoured and a
        # commit never coexists with one.
        cancelled_trials = 0
        for _ in range(80):
            boundary_state.write_text("open\n")
            boundary_marker.unlink(missing_ok=True)
            race = {}
            start = threading.Barrier(2)

            def installer_side():
                start.wait()
                race["boundary"] = installer.cancel_boundary()

            def ui_side():
                start.wait()
                race["request"] = ui_cancel_request(boundary_state, boundary_marker)

            threads = [threading.Thread(target=installer_side), threading.Thread(target=ui_side)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            assert not (race["request"] and not race["boundary"]), race
            if race["boundary"]:
                assert boundary_marker.exists()
                assert read_state(boundary_state) == "cancelled"
                cancelled_trials += 1
            else:
                assert race["request"] is False, race
                assert read_state(boundary_state) == "committing"
        assert cancelled_trials > 0, "the race never exercised the cancellation-wins path"
    finally:
        for key, value in saved_cancel_env.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        boundary_marker.unlink(missing_ok=True)

    def txns() -> set:
        return set(root.glob(installer.TXN_PREFIX + "*"))

    # ------------------------------------------------------------------
    # The shared cancellation protocol fails closed: an unsafe, unopenable, or
    # unverifiable state must abort before any live file changes, instead of
    # silently degrading to the marker alone.
    # ------------------------------------------------------------------
    proto_target = root / "protocol-target"
    install_quietly(proto_target, real1, "v1.0.0")
    proto_baseline = managed_snapshot(proto_target)
    proto_state = root / "protocol-state"
    proto_marker = root / "protocol-marker"
    proto_saved = {key: os.environ.get(key) for key in
                   ("ROBLOX_MAC_CANCEL_STATE_FILE", "ROBLOX_MAC_CANCEL_FILE")}
    original_write_cancel = installer.write_cancel_state

    # A mirror of bootstrapper-ui.c's progress_try_request_cancel(): it reports
    # acceptance only when it takes the lock, reads a valid "open" state, and
    # writes the marker; otherwise it refuses or reports uncertainty.
    def ui_try_request(state_path: Path, request_marker: Path, cancel_file: Path | None = None) -> str:
        destination = cancel_file if cancel_file is not None else request_marker
        lock_path = Path(str(state_path) + ".lock")
        try:
            fd = os.open(str(lock_path), os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        except OSError:
            return "uncertain"
        try:
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                return "uncertain"
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                return "uncertain"
            try:
                state_st = os.lstat(state_path)
                if stat.S_ISLNK(state_st.st_mode) or not stat.S_ISREG(state_st.st_mode):
                    return "uncertain"
                value = state_path.read_text().strip()
            except OSError:
                return "uncertain"
            if value == "open":
                try:
                    destination.write_text("cancel\n")
                except OSError:
                    return "uncertain"
                return "accepted"
            if value in ("committing", "committed", "cancelled"):
                return "refused"
            return "uncertain"
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)

    try:
        os.environ["ROBLOX_MAC_CANCEL_STATE_FILE"] = str(proto_state)
        os.environ["ROBLOX_MAC_CANCEL_FILE"] = str(proto_marker)
        proto_marker.unlink(missing_ok=True)

        # (1) A symlinked state file is rejected safely and never followed.
        real_state = root / "protocol-real-state"
        real_state.write_text("open\n")
        proto_state.unlink(missing_ok=True)
        proto_state.symlink_to(real_state)
        try:
            installer.cancel_boundary()
        except installer.CancelProtocolError:
            pass
        else:
            raise AssertionError("a symlinked shared state was accepted")
        before = txns()
        try:
            install_quietly(proto_target, real2, "v2.0.0")
        except installer.CancelProtocolError:
            pass
        else:
            raise AssertionError("a symlinked shared state allowed a commit")
        assert managed_snapshot(proto_target) == proto_baseline, "the old release must be intact"
        assert txns() == before, "an aborted boundary must leave no transaction"
        proto_state.unlink()

        # (2) An unopenable lock (a planted symlink) fails closed as well.
        proto_state.write_text("open\n")
        lock_link = Path(str(proto_state) + ".lock")
        lock_link.unlink(missing_ok=True)
        planted_lock = root / "protocol-planted-lock"
        planted_lock.write_text("")
        lock_link.symlink_to(planted_lock)
        try:
            installer.cancel_boundary()
        except installer.CancelProtocolError:
            pass
        else:
            raise AssertionError("a symlinked lock was accepted")
        before = txns()
        try:
            install_quietly(proto_target, real2, "v2.0.0")
        except installer.CancelProtocolError:
            pass
        else:
            raise AssertionError("an unopenable lock allowed a commit")
        assert managed_snapshot(proto_target) == proto_baseline
        assert txns() == before
        lock_link.unlink()

        # (3) A failed state write aborts before any live file changes.
        proto_state.write_text("open\n")

        def failing_write(*_args, **_kwargs):
            raise installer.CancelProtocolError("simulated state write failure")

        installer.write_cancel_state = failing_write
        try:
            before = txns()
            try:
                install_quietly(proto_target, real2, "v2.0.0")
            except installer.CancelProtocolError:
                pass
            else:
                raise AssertionError("a failed state write did not abort the commit")
        finally:
            installer.write_cancel_state = original_write_cancel
        assert managed_snapshot(proto_target) == proto_baseline
        assert txns() == before

        # (4) A transition that cannot be verified (a silent no-op write) also
        # aborts rather than letting an unverified commit proceed.
        proto_state.write_text("open\n")
        installer.write_cancel_state = lambda *_a, **_k: None
        try:
            before = txns()
            try:
                install_quietly(proto_target, real2, "v2.0.0")
            except installer.CancelProtocolError:
                pass
            else:
                raise AssertionError("an unverified transition did not abort the commit")
        finally:
            installer.write_cancel_state = original_write_cancel
        assert managed_snapshot(proto_target) == proto_baseline
        assert txns() == before

        # (5) The GTK UI never reports acceptance when it cannot take the lock,
        # read a safe state, or write the marker.
        proto_marker.unlink(missing_ok=True)
        lock_link.unlink(missing_ok=True)
        lock_link.symlink_to(planted_lock)
        assert ui_try_request(proto_state, proto_marker) == "uncertain"
        assert not proto_marker.exists()
        lock_link.unlink()
        proto_state.unlink()
        proto_state.symlink_to(real_state)
        assert ui_try_request(proto_state, proto_marker) == "uncertain"
        assert not proto_marker.exists()
        proto_state.unlink()
        proto_state.write_text("open\n")
        missing_destination = root / "protocol-absent-dir" / "cancel"
        assert ui_try_request(proto_state, proto_marker, missing_destination) == "uncertain"
        assert not proto_marker.exists()
        proto_state.write_text("committing\n")
        assert ui_try_request(proto_state, proto_marker) == "refused"
        assert not proto_marker.exists()
        proto_state.write_text("open\n")
        assert ui_try_request(proto_state, proto_marker) == "accepted"
        assert proto_marker.exists()
        proto_marker.unlink()

        # (6) A genuinely absent protocol (legacy/direct CLI invocation) keeps
        # the documented marker-only behaviour.
        os.environ.pop("ROBLOX_MAC_CANCEL_STATE_FILE", None)
        proto_marker.unlink(missing_ok=True)
        assert installer.cancel_boundary() is False
        proto_marker.write_text("cancel\n")
        assert installer.cancel_boundary() is True
        proto_marker.unlink()
    finally:
        for key, value in proto_saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        proto_marker.unlink(missing_ok=True)
        for leftover in (root / "protocol-real-state", root / "protocol-planted-lock"):
            leftover.unlink(missing_ok=True)

    # ------------------------------------------------------------------
    # Protocol files are opened safely: a FIFO must fail promptly instead of
    # blocking, and an unsafe descriptor (FIFO, directory, foreign owner) must
    # fail closed. The Python cases run in a subprocess with a timeout so a
    # regression cannot leave the suite blocked.
    # ------------------------------------------------------------------
    def probe_cancel_python(state_path: Path, mode: str, marker: Path | None = None,
                            fake_uid: str = "", timeout: int = 20) -> subprocess.CompletedProcess[str]:
        program = "\n".join([
            "import importlib.util, os, sys",
            "spec = importlib.util.spec_from_file_location('port_installer', sys.argv[1])",
            "m = importlib.util.module_from_spec(spec); sys.modules['port_installer'] = m",
            "spec.loader.exec_module(m)",
            "os.environ['ROBLOX_MAC_CANCEL_STATE_FILE'] = sys.argv[2]",
            "if sys.argv[3]:",
            "    os.environ['ROBLOX_MAC_CANCEL_FILE'] = sys.argv[3]",
            "if sys.argv[5]:",
            "    m.os.getuid = lambda: int(sys.argv[5])",
            "try:",
            "    value = m.read_cancel_state() if sys.argv[4] == 'read' else m.cancel_boundary()",
            "except m.CancelProtocolError as exc:",
            "    print('PROTOCOL_ERROR', exc); sys.exit(7)",
            "print('OK', value)",
        ])
        return subprocess.run(
            [sys.executable, "-c", program, str(INSTALLER), str(state_path),
             str(marker) if marker else "", mode, fake_uid],
            capture_output=True, text=True, timeout=timeout)

    scratch = root / "protocol-scratch"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir()
    fifo_state = scratch / "fifo-state"
    fifo_lock_state = scratch / "fifo-lock-state"
    fifo_lock = Path(str(fifo_lock_state) + ".lock")
    dir_state = scratch / "dir-state"
    dir_lock_state = scratch / "dir-lock-state"
    dir_lock = Path(str(dir_lock_state) + ".lock")
    mismatched = scratch / "mismatched"
    valid_dir = scratch / "valid"
    valid_state = valid_dir / "state"

    # A FIFO at the state path must fail promptly (no blocking open).
    os.mkfifo(fifo_state)
    result = probe_cancel_python(fifo_state, "read")
    assert result.returncode == 7, (result.returncode, result.stdout, result.stderr)
    assert "PROTOCOL_ERROR" in result.stdout, result.stdout

    # A FIFO at the lock path must fail promptly too.
    fifo_lock_state.write_text("open\n")
    os.mkfifo(fifo_lock)
    result = probe_cancel_python(fifo_lock_state, "boundary")
    assert result.returncode == 7, (result.returncode, result.stdout, result.stderr)

    # A directory at the state or lock path is not a regular file.
    dir_state.mkdir()
    result = probe_cancel_python(dir_state, "read")
    assert result.returncode == 7, (result.returncode, result.stdout, result.stderr)
    dir_lock_state.write_text("open\n")
    dir_lock.mkdir()
    result = probe_cancel_python(dir_lock_state, "boundary")
    assert result.returncode == 7, (result.returncode, result.stdout, result.stderr)

    # A descriptor owned by a different user is refused (uid patched to differ).
    mismatched.write_text("open\n")
    result = probe_cancel_python(mismatched, "read", fake_uid=str(os.getuid() + 1))
    assert result.returncode == 7, (result.returncode, result.stdout, result.stderr)
    assert "owned" in result.stdout, result.stdout

    # A valid regular state still reads successfully.
    valid_dir.mkdir()
    valid_state.write_text("open\n")
    result = probe_cancel_python(valid_state, "read")
    assert result.returncode == 0 and "OK open" in result.stdout, (result.returncode, result.stdout)

    # ------------------------------------------------------------------
    # The real shell initialization path (start_progress -> cancel_state_write)
    # must reject a FIFO at the lock or state path promptly, must not start the
    # operation, and must still reset normally once the path is safe again.
    # ------------------------------------------------------------------
    def run_handoff_timed(extra_env=None, timeout=30) -> subprocess.CompletedProcess[str]:
        return subprocess.run([handoff / "bootstrap.sh", "port-update-settings"],
                              env={**env, **(extra_env or {})}, capture_output=True,
                              text=True, timeout=timeout)

    shell_state = handoff / "DO_NOT_SHARE" / ".bootstrap-cancel-state"
    shell_lock = Path(str(shell_state) + ".lock")

    shell_state.unlink(missing_ok=True)
    shell_lock.unlink(missing_ok=True)
    event_log.write_text("")
    os.mkfifo(shell_lock)
    try:
        fifo_lock_result = run_handoff_timed()
    finally:
        shell_lock.unlink(missing_ok=True)
    assert fifo_lock_result.returncode != 0, (fifo_lock_result.returncode, fifo_lock_result.stderr)
    assert not shell_state.exists(), "the shell must not record state through an unsafe lock"
    assert event_log.read_text().strip() == "", "the operation must not start with a FIFO lock"

    shell_state.unlink(missing_ok=True)
    shell_lock.unlink(missing_ok=True)
    event_log.write_text("")
    os.mkfifo(shell_state)
    try:
        fifo_state_result = run_handoff_timed()
    finally:
        shell_state.unlink(missing_ok=True)
        shell_lock.unlink(missing_ok=True)
    assert fifo_state_result.returncode != 0, (fifo_state_result.returncode, fifo_state_result.stderr)
    assert event_log.read_text().strip() == "", "the operation must not start with a FIFO state"

    # A normal reset still works after the unsafe-path cases.
    event_log.write_text("")
    ok_result = run_handoff_timed({"PORT_STUB_RESULT": "0"})
    assert ok_result.returncode == 0, (ok_result.returncode, ok_result.stderr)
    assert shell_state.read_text().strip() == "open"

    # There is no unsafe shell fallback. When Python cannot run, the operation
    # must be refused before it starts, even when the state and lock paths are
    # perfectly safe and could have been written by a redirection.
    broken_bin = root / "protocol-broken-bin"
    broken_bin.mkdir(exist_ok=True)
    (broken_bin / "python3").write_text("#!/bin/sh\nexit 1\n")
    (broken_bin / "python3").chmod(0o755)
    broken_env = {"PATH": str(broken_bin) + os.pathsep + os.environ["PATH"]}

    shell_state.unlink(missing_ok=True)
    shell_lock.unlink(missing_ok=True)
    event_log.write_text("")
    missing_python = run_handoff_timed(broken_env)
    assert missing_python.returncode != 0, (missing_python.returncode, missing_python.stderr)
    assert event_log.read_text().strip() == "", "an update started without the cancellation helper"
    assert not shell_state.exists(), "a missing helper must not leave a partially initialized state"

    # A helper that starts Python but fails while executing the shared-state
    # helper must be propagated as well, before any progress event or state.
    failing_bin = root / "protocol-failing-bin"
    failing_bin.mkdir(exist_ok=True)
    failing_helper = failing_bin / "python3"
    failing_helper.write_text(
        "#!/bin/sh\n"
        "if [ \"${1:-}\" = \"-\" ]; then exit 12; fi\n"
        f"exec {shlex.quote(sys.executable)} \"$@\"\n")
    failing_helper.chmod(0o755)
    shell_state.unlink(missing_ok=True)
    shell_lock.unlink(missing_ok=True)
    event_log.write_text("")
    helper_failed = run_handoff_timed(
        {"PATH": str(failing_bin) + os.pathsep + os.environ["PATH"]})
    assert helper_failed.returncode != 0, (helper_failed.returncode, helper_failed.stderr)
    assert event_log.read_text().strip() == "", "an update started after helper initialization failed"
    assert not shell_state.exists()

    # A symlinked state or lock is rejected without touching its target.
    victim = root / "protocol-shell-victim"
    victim.write_text("victim-original\n")

    shell_state.unlink(missing_ok=True)
    shell_lock.unlink(missing_ok=True)
    shell_state.symlink_to(victim)
    event_log.write_text("")
    try:
        linked_state = run_handoff_timed()
    finally:
        shell_state.unlink(missing_ok=True)
        shell_lock.unlink(missing_ok=True)
    assert linked_state.returncode != 0, (linked_state.returncode, linked_state.stderr)
    assert victim.read_text() == "victim-original\n", "a symlinked state target was modified"
    assert event_log.read_text().strip() == "", "a symlinked state allowed the operation to start"

    victim.write_text("victim-original\n")
    shell_lock.symlink_to(victim)
    event_log.write_text("")
    try:
        linked_lock = run_handoff_timed()
    finally:
        shell_lock.unlink(missing_ok=True)
        shell_state.unlink(missing_ok=True)
    assert linked_lock.returncode != 0, (linked_lock.returncode, linked_lock.stderr)
    assert victim.read_text() == "victim-original\n", "a symlinked lock target was modified"
    assert event_log.read_text().strip() == "", "a symlinked lock allowed the operation to start"

    # A temp file planted at the old predictable name must never redirect or
    # truncate the helper's replacement: the helper creates a unique file with
    # exclusive creation in the state directory instead.
    plant_bin = root / "protocol-plant-bin"
    plant_bin.mkdir(exist_ok=True)
    (plant_bin / "python3").write_text(
        "#!/bin/sh\n"
        "if [ \"${1:-}\" = \"-\" ] && [ -n \"${PLANT_LOG:-}\" ]; then\n"
        "    _tmp=\"${PLANT_STATE}.tmp.$$\"\n"
        "    rm -f -- \"$_tmp\" 2>/dev/null || true\n"
        "    if [ \"${PLANT_KIND:-}\" = symlink ]; then\n"
        "        ln -s -- \"${PLANT_VICTIM}\" \"$_tmp\" 2>/dev/null || true\n"
        "    else\n"
        "        printf 'sentinel-content\\n' > \"$_tmp\" 2>/dev/null || true\n"
        "    fi\n"
        "    printf '%s\\n' \"$_tmp\" >> \"$PLANT_LOG\" 2>/dev/null || true\n"
        "fi\n"
        f"exec {shlex.quote(sys.executable)} \"$@\"\n")
    (plant_bin / "python3").chmod(0o755)
    plant_env = {"PATH": str(plant_bin) + os.pathsep + os.environ["PATH"],
                 "PLANT_STATE": str(shell_state)}

    plant_log = root / "protocol-plant-log"
    victim.write_text("victim-original\n")
    shell_state.unlink(missing_ok=True)
    shell_lock.unlink(missing_ok=True)
    plant_log.unlink(missing_ok=True)
    event_log.write_text("")
    planted_symlink = run_handoff_timed({
        **plant_env, "PLANT_LOG": str(plant_log), "PLANT_KIND": "symlink",
        "PLANT_VICTIM": str(victim),
    })
    assert planted_symlink.returncode == 0, (planted_symlink.returncode, planted_symlink.stderr)
    assert victim.read_text() == "victim-original\n", "a planted symlink redirected the state write"
    assert shell_state.read_text().strip() == "open"
    planted = [Path(line) for line in plant_log.read_text().splitlines()]
    assert planted, "the helper never ran through the planting wrapper"
    for path in planted:
        assert path.is_symlink(), f"the helper replaced the planted symlink: {path}"
        assert path.resolve() == victim.resolve(), path

    shell_state.unlink(missing_ok=True)
    shell_lock.unlink(missing_ok=True)
    plant_log.unlink(missing_ok=True)
    event_log.write_text("")
    planted_file = run_handoff_timed({
        **plant_env, "PLANT_LOG": str(plant_log), "PLANT_KIND": "file",
    })
    assert planted_file.returncode == 0, (planted_file.returncode, planted_file.stderr)
    assert shell_state.read_text().strip() == "open"
    planted = [Path(line) for line in plant_log.read_text().splitlines()]
    assert planted, "the helper never ran through the planting wrapper"
    for path in planted:
        assert path.read_text() == "sentinel-content\n", \
            f"the helper truncated a pre-existing file at {path}"
    for path in planted:
        path.unlink(missing_ok=True)

    # ------------------------------------------------------------------
    # The durable commit point is irreversible: once the journal says
    # "committed" the new release is never rolled back, and an ambiguous
    # journal write keeps its evidence instead of guessing.
    # ------------------------------------------------------------------
    def run_with_commit_faults(path: Path, write_wrap=None, finalize_fault=None, mark=None):
        """Install v2.0.0 over ``path`` with journal/bookkeeping faults injected.

        Returns ``(outcome, record)`` where ``outcome`` is ``"ok"``, the raised
        exception, and ``record`` reports rollback calls, the persisted journal
        state seen at finalize, and the transactions this run left behind.
        """
        before = txns()
        original_write_journal = installer.write_journal
        original_finalize = installer.finalize_transaction
        original_rollback = installer.rollback_transaction
        original_mark = installer.mark_commit_completed
        record = {"rollbacks": 0, "finalize_states": []}

        def counted_rollback(*args, **kwargs):
            record["rollbacks"] += 1
            return original_rollback(*args, **kwargs)

        def recording_finalize(target, txn, journal, **kwargs):
            record["finalize_states"].append(installer._persisted_journal_state(txn))
            if finalize_fault is not None:
                return finalize_fault(target, txn, journal, **kwargs)
            return original_finalize(target, txn, journal, **kwargs)

        if write_wrap is not None:
            installer.write_journal = write_wrap(original_write_journal)
        installer.rollback_transaction = counted_rollback
        installer.finalize_transaction = recording_finalize
        if mark is not None:
            installer.mark_commit_completed = mark
        try:
            try:
                install_quietly(path, real2, "v2.0.0")
                outcome = "ok"
            except installer.CleanupPendingError as exc:
                outcome = exc
            except Exception as exc:  # noqa: BLE001 - the test reports the real failure
                outcome = exc
        finally:
            installer.write_journal = original_write_journal
            installer.finalize_transaction = original_finalize
            installer.rollback_transaction = original_rollback
            installer.mark_commit_completed = original_mark
        record["new_txns"] = txns() - before
        return outcome, record

    # (B1) A cancellation-state failure after the committed journal lands is
    # post-commit bookkeeping: the new release stays and rollback is not called.
    b1 = root / "commit-bookkeeping"
    install_quietly(b1, real1, "v1.0.0")

    def boom_mark():
        raise RuntimeError("simulated cancellation bookkeeping failure")

    b1_outcome, b1_record = run_with_commit_faults(b1, mark=boom_mark)
    assert b1_outcome == "ok", b1_outcome
    assert b1_record["rollbacks"] == 0, b1_record
    assert b1_record["finalize_states"] == ["committed"], b1_record
    assert installer.installed_port_version(b1) == "v2.0.0"
    assert (b1 / "run.sh").read_bytes() == (real2 / "run.sh").read_bytes()
    assert not b1_record["new_txns"], b1_record

    # (B2) A journal write that fails before the committed state reaches disk
    # is still safely rolled back.
    b2 = root / "commit-write-before"
    install_quietly(b2, real1, "v1.0.0")

    def b2_wrap(original):
        def wrapped(txn, journal):
            if journal.get("state") == "committed":
                raise OSError("simulated journal write failure before the commit")
            return original(txn, journal)
        return wrapped

    b2_outcome, b2_record = run_with_commit_faults(b2, write_wrap=b2_wrap)
    assert isinstance(b2_outcome, OSError), b2_outcome
    assert "before the commit" in str(b2_outcome), b2_outcome
    assert b2_record["rollbacks"] == 1, b2_record
    assert not b2_record["finalize_states"], b2_record
    assert installer.installed_port_version(b2) == "v1.0.0"
    assert not b2_record["new_txns"], b2_record

    # (B3) A journal write that raises after the committed state is already
    # visible on disk is committed, not failed: no rollback, and recovery later
    # treats the transaction as committed.
    b3 = root / "commit-write-after"
    install_quietly(b3, real1, "v1.0.0")

    def b3_wrap(original):
        def wrapped(txn, journal):
            result = original(txn, journal)
            if journal.get("state") == "committed":
                raise OSError("simulated fsync failure after the committed rename")
            return result
        return wrapped

    b3_outcome, b3_record = run_with_commit_faults(b3, write_wrap=b3_wrap)
    assert b3_outcome == "ok", b3_outcome
    assert b3_record["rollbacks"] == 0, b3_record
    assert b3_record["finalize_states"] == ["committed"], b3_record
    assert installer.installed_port_version(b3) == "v2.0.0"
    assert not b3_record["new_txns"], b3_record

    # (B4) An unreadable or ambiguous journal during error handling keeps its
    # evidence and never performs a destructive rollback based on a guess.
    b4 = root / "commit-write-ambiguous"
    install_quietly(b4, real1, "v1.0.0")

    def b4_wrap(original):
        def wrapped(txn, journal):
            if journal.get("state") == "committed":
                (txn / "journal.json").write_text("{ not a valid journal", encoding="utf-8")
                raise OSError("simulated journal corruption")
            return original(txn, journal)
        return wrapped

    b4_outcome, b4_record = run_with_commit_faults(b4, write_wrap=b4_wrap)
    assert isinstance(b4_outcome, installer.InstallerError), b4_outcome
    assert not isinstance(b4_outcome, installer.CancelledError), b4_outcome
    assert "could not determine" in str(b4_outcome).lower(), b4_outcome
    assert b4_record["rollbacks"] == 0, b4_record
    assert not b4_record["finalize_states"], b4_record
    assert len(b4_record["new_txns"]) == 1, b4_record
    try:
        installer.recover_transactions(b4)
    except installer.InstallerError:
        pass
    else:
        raise AssertionError("recovery guessed an ambiguous journal state")
    assert all(t.is_dir() for t in b4_record["new_txns"]), "the ambiguous evidence was discarded"
    # Remove the deliberately corrupt transaction so it does not poison the
    # recover_transactions() scans of the later cases in this shared tree.
    for stale in b4_record["new_txns"]:
        shutil.rmtree(stale, ignore_errors=True)

    # A transaction whose persisted journal is "committed" is never rolled back.
    guard_txn = root / "commit-guard-txn"
    (guard_txn / "backup").mkdir(parents=True)
    (guard_txn / "journal.json").write_text(json.dumps({"state": "committed", "files": []}))
    try:
        installer.rollback_transaction(root / "commit-guard-target", guard_txn,
                                       {"state": "committed", "files": []})
    except installer.InstallerError:
        pass
    else:
        raise AssertionError("a committed journal was rolled back")
    shutil.rmtree(guard_txn)

    # ------------------------------------------------------------------
    # The commit point is durable only when the directory fsync that confirms
    # it succeeds. A failure *after* the committed journal's rename (the real
    # boundary, not a wrapper that raises once everything has returned) must not
    # be treated as durable: no rollback, no backup deletion, an explicit
    # pending outcome, and a recovery that re-establishes durability first.
    # ------------------------------------------------------------------
    original_fsync_strict = installer.fsync_dir_strict

    def run_with_durability_fault(path: Path):
        before = txns()
        original_write_journal = installer.write_journal
        original_finalize = installer.finalize_transaction
        original_rollback = installer.rollback_transaction
        armed = {"path": None}
        record = {"rollbacks": 0, "finalize_states": []}

        def armed_strict(directory):
            # The committed journal has already been renamed into place; only
            # the directory fsync that would confirm its durability fails.
            if armed["path"] is not None and Path(directory) == armed["path"]:
                raise installer.TransactionDurabilityError(
                    "simulated directory fsync failure after the committed rename")
            return original_fsync_strict(directory)

        def arming_write_journal(txn, journal, *args, **kwargs):
            if journal.get("state") == "committed":
                armed["path"] = Path(txn)
            return original_write_journal(txn, journal, *args, **kwargs)

        def counted_rollback(*args, **kwargs):
            record["rollbacks"] += 1
            return original_rollback(*args, **kwargs)

        def recording_finalize(target, txn, journal, **kwargs):
            record["finalize_states"].append(installer._persisted_journal_state(txn))
            return original_finalize(target, txn, journal, **kwargs)

        installer.fsync_dir_strict = armed_strict
        installer.write_journal = arming_write_journal
        installer.rollback_transaction = counted_rollback
        installer.finalize_transaction = recording_finalize
        try:
            try:
                install_quietly(path, real2, "v2.0.0")
                outcome = "ok"
            except installer.CleanupPendingError as exc:
                outcome = exc
            except Exception as exc:  # noqa: BLE001 - the test reports the real failure
                outcome = exc
        finally:
            installer.fsync_dir_strict = original_fsync_strict
            installer.write_journal = original_write_journal
            installer.rollback_transaction = original_rollback
            installer.finalize_transaction = original_finalize
        record["new_txns"] = txns() - before
        return outcome, record

    a5 = root / "commit-fsync-uncertain"
    install_quietly(a5, real1, "v1.0.0")
    a5_outcome, a5_record = run_with_durability_fault(a5)
    assert isinstance(a5_outcome, installer.CleanupPendingError), (a5_outcome, a5_record)
    assert a5_record["rollbacks"] == 0, a5_record
    assert a5_record["finalize_states"] == [], "backups must not be deleted while durability is unconfirmed"
    assert installer.installed_port_version(a5) == "v2.0.0"
    assert len(a5_record["new_txns"]) == 1, a5_record
    a5_txn = next(iter(a5_record["new_txns"]))
    assert json.loads((a5_txn / "journal.json").read_text())["state"] == "committed"
    assert (a5_txn / "backup" / "run.sh").is_file(), "the transaction evidence must be retained"
    # A later recovery re-establishes durability and finishes the cleanup.
    installer.recover_transactions(a5)
    assert not a5_txn.exists(), "recovery did not resolve the pending transaction"
    assert installer.installed_port_version(a5) == "v2.0.0"

    # If the filesystem keeps rejecting directory fsync, recovery must not
    # discard evidence or falsely claim durability.
    a6 = root / "commit-fsync-stuck"
    install_quietly(a6, real1, "v1.0.0")
    a6_outcome, a6_record = run_with_durability_fault(a6)
    assert isinstance(a6_outcome, installer.CleanupPendingError), (a6_outcome, a6_record)
    assert len(a6_record["new_txns"]) == 1, a6_record
    a6_txn = next(iter(a6_record["new_txns"]))

    def fail_only_a6(directory):
        if Path(directory) == a6_txn:
            raise installer.TransactionDurabilityError("simulated persistent directory fsync failure")
        return original_fsync_strict(directory)

    installer.fsync_dir_strict = fail_only_a6
    try:
        recovery_outcome = installer.recover_transactions(a6)
        assert recovery_outcome == installer.TXN_OUTCOME_DURABILITY_PENDING, recovery_outcome
        try:
            installer.recover_transactions(a6, require_durable=True)
        except installer.TransactionDurabilityError as exc:
            assert "not confirmed durable" in str(exc), exc
        else:
            raise AssertionError("mutation recovery accepted an unresolved commit")
    finally:
        installer.fsync_dir_strict = original_fsync_strict
    assert a6_txn.is_dir(), "the evidence must survive a persistent fsync failure"
    assert (a6_txn / "backup" / "run.sh").is_file(), "backups must not be discarded"
    assert installer.installed_port_version(a6) == "v2.0.0"
    # Once the filesystem cooperates again, recovery finishes safely.
    assert installer.recover_transactions(a6) is None
    assert not a6_txn.exists()
    assert installer.installed_port_version(a6) == "v2.0.0"

    # ------------------------------------------------------------------
    # An unresolved committed transaction blocks the *next* update at the real
    # entry point, before staging or any live-file change, and keeps all its
    # evidence. Recovery must resolve it before another update can proceed --
    # while an ordinary cleanup failure (a5/cleanup-stuck) stays non-blocking.
    # ------------------------------------------------------------------
    a7 = root / "commit-fsync-blocks-followup"
    install_quietly(a7, real1, "v1.0.0")
    a7_fault = {"txn": None}
    a7_rollbacks = {"n": 0}
    original_a7_write_journal = installer.write_journal
    original_a7_rollback = installer.rollback_transaction

    def a7_strict(directory):
        if a7_fault["txn"] is not None and Path(directory) == a7_fault["txn"]:
            raise installer.TransactionDurabilityError(
                "simulated persistent directory fsync failure")
        return original_fsync_strict(directory)

    def a7_write_journal(txn, journal, *args, **kwargs):
        if journal.get("state") == "committed":
            a7_fault["txn"] = Path(txn)
        return original_a7_write_journal(txn, journal, *args, **kwargs)

    def a7_count_rollback(*args, **kwargs):
        a7_rollbacks["n"] += 1
        return original_a7_rollback(*args, **kwargs)

    installer.fsync_dir_strict = a7_strict
    installer.write_journal = a7_write_journal
    installer.rollback_transaction = a7_count_rollback
    try:
        try:
            install_quietly(a7, real2, "v2.0.0")
        except installer.CommitDurabilityPendingError as exc:
            assert "durability" in str(exc).lower(), exc
        else:
            raise AssertionError("an unconfirmed commit was reported as a clean success")
        a7_txn = a7_fault["txn"]
        assert a7_txn is not None and a7_txn.is_dir(), a7_fault
        assert installer.installed_port_version(a7) == "v2.0.0"
        assert a7_rollbacks["n"] == 0, "an unconfirmed commit must never be rolled back"
        a7_live = managed_snapshot(a7)
        a7_journal = (a7_txn / "journal.json").read_bytes()
        a7_backup = (a7_txn / "backup" / "run.sh").read_bytes()
        before_txns = txns()
        try:
            # The second update must be refused while only a7_txn's directory
            # fsync is failing; every unrelated fsync still succeeds.
            install_quietly(a7, real3, "v3.0.0")
        except installer.TransactionDurabilityError as exc:
            assert "not confirmed durable" in str(exc), exc
        else:
            raise AssertionError("a second update started while commit durability was unresolved")
        assert managed_snapshot(a7) == a7_live, "the refused update changed live files"
        assert txns() == before_txns, "the refused update started a transaction"
        assert (a7_txn / "journal.json").read_bytes() == a7_journal, "journal evidence changed"
        assert (a7_txn / "backup" / "run.sh").read_bytes() == a7_backup, "backup evidence changed"
        assert installer.installed_port_version(a7) == "v2.0.0"
    finally:
        installer.fsync_dir_strict = original_fsync_strict
        installer.write_journal = original_a7_write_journal
        installer.rollback_transaction = original_a7_rollback
    # Recovery re-establishes durability and finishes cleanup.
    assert installer.recover_transactions(a7) is None
    assert not a7_txn.exists(), "recovery did not resolve the pending transaction"
    assert installer.installed_port_version(a7) == "v2.0.0"
    assert (a7 / "run.sh").read_bytes() == (real2 / "run.sh").read_bytes()
    # Once resolved, a normal update proceeds.
    install_quietly(a7, real3, "v3.0.0")
    assert installer.installed_port_version(a7) == "v3.0.0"
    assert (a7 / "run.sh").read_bytes() == (real3 / "run.sh").read_bytes()

    # The GTK UI must take the same lock and consult the same state file, so
    # the handshake cannot be bypassed by the window itself.
    ui_source = (PACKAGE / "bootstrapper-ui.c").read_text()
    assert "flock(fd, LOCK_EX)" in ui_source, "the UI must take the shared cancel lock"
    assert '"%s.lock"' in ui_source, "the UI must use the shared lock file"
    assert '.bootstrap-cancel-state' in ui_source, "the UI must read the shared state file"
    assert "can no longer be cancelled" in ui_source, "the UI must show a refusal, not an accepted cancel"
    assert "_GNU_SOURCE" in ui_source and "O_NOFOLLOW" in ui_source, \
        "the UI must not follow a symlinked state or lock"
    assert "CANCEL_STATE_UNKNOWN" in ui_source and "CANCEL_STATE_BUSY" in ui_source, \
        "the UI must distinguish a busy or unreadable state from open"
    assert "could not be verified safely" in ui_source, "the UI must show a status-uncertain message"

    # ------------------------------------------------------------------
    # A cleanup failure after a durable commit is reported as applied, never
    # as a failed update that kept the previous release.
    # ------------------------------------------------------------------
    def fail_finalize(*_args, **_kwargs):
        raise OSError("simulated cleanup failure")

    def commit_then_fail_cleanup(name: str) -> Path:
        path = root / name
        install_quietly(path, real1, "v1.0.0")
        installer.finalize_transaction = fail_finalize
        try:
            try:
                install_quietly(path, real2, "v2.0.0")
            except installer.CleanupPendingError as exc:
                assert "cleanup" in str(exc).lower(), exc
            else:
                raise AssertionError("a failed cleanup was reported as a plain success")
        finally:
            installer.finalize_transaction = original_finalize
        return path

    original_finalize = installer.finalize_transaction
    pending_target = commit_then_fail_cleanup("cleanup-pending")
    # The new release is live: a committed install is never rolled back because
    # deleting obsolete backups failed.
    assert installer.installed_port_version(pending_target) == "v2.0.0"
    assert (pending_target / "run.sh").read_bytes() == (real2 / "run.sh").read_bytes()
    # The committed transaction is kept as evidence for the retry.
    pending_txns = list(pending_target.parent.glob(installer.TXN_PREFIX + "*"))
    assert len(pending_txns) == 1, pending_txns
    journal = json.loads((pending_txns[0] / "journal.json").read_text(encoding="utf-8"))
    assert journal["state"] == "committed", journal["state"]
    assert (pending_txns[0] / "backup" / "run.sh").is_file(), "cleanup evidence must be retained"
    # A later recovery finishes the cleanup, idempotently.
    installer.recover_transactions(pending_target)
    assert not list(pending_target.parent.glob(installer.TXN_PREFIX + "*"))
    assert installer.installed_port_version(pending_target) == "v2.0.0"
    installer.recover_transactions(pending_target)
    assert installer.installed_port_version(pending_target) == "v2.0.0"

    # A cleanup that keeps failing must not block the launcher and must not
    # discard the committed release or its evidence.
    stuck_target = commit_then_fail_cleanup("cleanup-stuck")
    stuck_txns = list(stuck_target.parent.glob(installer.TXN_PREFIX + "*"))
    assert len(stuck_txns) == 1, stuck_txns
    installer.finalize_transaction = fail_finalize
    try:
        installer.recover_transactions(stuck_target)  # must not raise
    finally:
        installer.finalize_transaction = original_finalize
    assert installer.installed_port_version(stuck_target) == "v2.0.0"
    assert list(stuck_target.parent.glob(installer.TXN_PREFIX + "*")) == stuck_txns
    # And the real recovery entry point finishes it once cleanup can run.
    recovered = cli(cli_env_map, "recover", "--prefix", str(stuck_target))
    assert recovered.returncode == 0, (recovered.returncode, recovered.stderr)
    assert not list(stuck_target.parent.glob(installer.TXN_PREFIX + "*"))
    assert installer.installed_port_version(stuck_target) == "v2.0.0"

print("PASS port version detection, packaged release metadata, legacy ownership adoption, "
      "cancellation boundaries, payload-independent recovery and preservation")