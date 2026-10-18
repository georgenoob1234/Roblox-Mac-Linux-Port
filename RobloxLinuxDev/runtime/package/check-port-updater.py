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
import json
import os
import shutil
import subprocess
import sys
import tempfile
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
emit("downloading", 30, "Downloading", True)
if os.environ.get("PORT_STUB_REQUIRE_NO_MARKER") == "1" and marker and os.path.exists(marker):
    emit("failed", 0, "A stale cancellation marker was not cleared", False)
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
emit({0: "complete", 125: "cancelled"}.get(result, "failed"),
     100 if result == 0 else 0, "done", False)
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

print("PASS port version detection, packaged release metadata, legacy ownership adoption, "
      "cancellation boundaries, payload-independent recovery and preservation")