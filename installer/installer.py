#!/usr/bin/env python3
"""Safe per-user installer for Roblox Mac Linux Port.

No root privileges or third-party Python packages are required.
The installer owns only files listed in its manifest. DO_NOT_SHARE is never
removed, replaced, merged over, or traversed through a symlink.
"""
from __future__ import annotations

import argparse
import contextlib
import datetime as _dt
import fcntl
import hashlib
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterator

APP_ID = "roblox-mac-linux-port"
APP_NAME = "Roblox Mac Linux Port"
REPOSITORY = "georgenoob1234/Roblox-Mac-Linux-Port"
MANIFEST_NAME = ".roblox-mac-port-install.json"
TXN_PREFIX = ".roblox-mac-port-txn-"
BACKUP_PREFIX = ".roblox-mac-port-backups"
DESKTOP_STATE_NAME = "desktop.json"
DESKTOP_MARKER = "X-Roblox-Mac-Port-Installer=1"
DESKTOP_ID = "roblox-mac-port.desktop"
SETTINGS_DESKTOP_ID = "roblox-mac-port-settings.desktop"
URI_SCHEMES = ("x-scheme-handler/roblox", "x-scheme-handler/roblox-player")

# This allowlist is intentional: an extracted source directory is not copied
# wholesale, and a newly added/unexpected file cannot silently become managed.
PAYLOAD_FILES = {
    ".gitignore",
    "FFlags.json",
    "INSTALLER.md",
    "README.md",
    "RobloxLinux.AppImage",
    "bootstrap.py",
    "bootstrap.sh",
    "icon.png",
    "icon_bw.png",
    "install-uri-handler.sh",
    "run.sh",
    "update-roblox.sh",
    "installer.py",
}
REQUIRED_FILES = {
    "FFlags.json",
    "RobloxLinux.AppImage",
    "bootstrap.py",
    "bootstrap.sh",
    "icon.png",
    "icon_bw.png",
    "install-uri-handler.sh",
    "run.sh",
    "update-roblox.sh",
}
EXECUTABLE_FILES = {
    "RobloxLinux.AppImage",
    "bootstrap.py",
    "bootstrap.sh",
    "install-uri-handler.sh",
    "installer.py",
    "run.sh",
    "update-roblox.sh",
}
PROTECTED_NAMES = {"DO_NOT_SHARE", "RobloxVersion", ".update-work"}
USER_CONFIG_FILES = {"FFlags.json"}
HEX64 = re.compile(r"^[0-9a-f]{64}$")


def _color(code: str, value: str) -> str:
    if (
        sys.stdout.isatty()
        and sys.stderr.isatty()
        and not os.environ.get("NO_COLOR")
        and not os.environ.get("CI")
        and os.environ.get("TERM", "") != "dumb"
    ):
        return f"\033[{code}m{value}\033[0m"
    return value


def write_log(level: str, message: str) -> None:
    """Best-effort private log; never let logging failure affect installation."""
    try:
        root = state_root()
        root.mkdir(parents=True, exist_ok=True, mode=0o700)
        root_st = lstat_or_none(root)
        if root_st is None or stat.S_ISLNK(root_st.st_mode) or not stat.S_ISDIR(root_st.st_mode):
            return
        path = root / "installer.log"
        st = lstat_or_none(path)
        if st is not None and not stat.S_ISREG(st.st_mode):
            return
        if st is not None and st.st_size > 1024 * 1024:
            rotated = root / "installer.log.1"
            rotated_st = lstat_or_none(rotated)
            if rotated_st is None or stat.S_ISREG(rotated_st.st_mode):
                if rotated_st is not None:
                    rotated.unlink()
                os.replace(path, rotated)
        fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_APPEND | getattr(os, "O_NOFOLLOW", 0), 0o600)
        try:
            os.fchmod(fd, 0o600)
            timestamp = _dt.datetime.now().astimezone().isoformat(timespec="seconds")
            payload = f"{timestamp} [{level}] {message.replace(chr(10), ' ')}\n".encode("utf-8", "replace")
            os.write(fd, payload)
        finally:
            os.close(fd)
    except Exception:
        pass


def good(message: str) -> None:
    write_log("INFO", message)
    print(_color("32", "✓") + " " + message)


def note(message: str) -> None:
    write_log("INFO", message)
    print(_color("36", "•") + " " + message)


def warn(message: str) -> None:
    write_log("WARN", message)
    print(_color("33", "! " + message), file=sys.stderr)


def fail(message: str) -> "NoReturn":
    raise InstallerError(message)


class InstallerError(RuntimeError):
    pass


@contextlib.contextmanager
def section(title: str) -> Iterator[None]:
    print("\n" + _color("1;36", f"── {title} " + "─" * max(2, 54 - len(title))))
    yield


def default_prefix() -> Path:
    return Path.home() / ".local" / "opt" / APP_ID


def xdg_path(env_name: str, fallback: Path) -> Path:
    raw = os.environ.get(env_name)
    path = Path(raw).expanduser() if raw else fallback
    if not path.is_absolute():
        # Avoid calling warn() here: logging itself consults XDG_STATE_HOME.
        print(f"Warning: ignoring relative {env_name}; XDG paths must be absolute.", file=sys.stderr)
        path = fallback
    return path.resolve(strict=False)


def state_root() -> Path:
    return xdg_path("XDG_STATE_HOME", Path.home() / ".local" / "state") / APP_ID


def data_home() -> Path:
    return xdg_path("XDG_DATA_HOME", Path.home() / ".local" / "share")


def config_home() -> Path:
    return xdg_path("XDG_CONFIG_HOME", Path.home() / ".config")


def now_stamp() -> str:
    return _dt.datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")


def normalized_target(raw: str | Path) -> Path:
    path = Path(raw).expanduser()
    if not path.is_absolute():
        path = Path.cwd() / path
    # Never accept the installation root itself as a symlink. Resolve parent
    # symlinks only, so the resulting path remains stable for manifest checks.
    if path.is_symlink():
        fail(f"Install location is a symlink; refusing to follow it: {path}")
    parent = path.parent.resolve(strict=False)
    result = parent / path.name
    if result in {Path("/"), Path.home(), Path.home() / ".local", Path.home() / ".local" / "opt"}:
        fail(f"That location is too broad to use as an install directory: {result}")
    return result


def lstat_or_none(path: Path):
    try:
        return path.lstat()
    except FileNotFoundError:
        return None


def assert_regular_file(path: Path, description: str) -> os.stat_result:
    st = lstat_or_none(path)
    if st is None:
        fail(f"Missing {description}: {path}")
    if not stat.S_ISREG(st.st_mode):
        fail(f"{description} is not a regular file (symlinks are not accepted): {path}")
    return st


def sha256_file(path: Path) -> str:
    st = assert_regular_file(path, "file")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    digest = hashlib.sha256()
    try:
        with os.fdopen(fd, "rb", closefd=False) as stream:
            while True:
                block = stream.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
    finally:
        os.close(fd)
    return digest.hexdigest()


def fsync_dir(path: Path) -> None:
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        # Some network and virtual filesystems do not support directory fsync.
        pass


def atomic_write(path: Path, data: bytes, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    existing = lstat_or_none(path)
    if existing is not None and not stat.S_ISREG(existing.st_mode):
        fail(f"Refusing to replace a non-regular file: {path}")
    fd, temp_name = tempfile.mkstemp(prefix=f".{path.name}.tmp-", dir=path.parent)
    temp = Path(temp_name)
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
        fsync_dir(path.parent)
    except Exception:
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass
        raise


def json_bytes(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def atomic_json(path: Path, value: Any, mode: int = 0o600) -> None:
    atomic_write(path, json_bytes(value), mode)


@contextlib.contextmanager
def installer_lock() -> Iterator[None]:
    root = state_root()
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    root_st = lstat_or_none(root)
    if root_st is None or stat.S_ISLNK(root_st.st_mode) or not stat.S_ISDIR(root_st.st_mode):
        fail(f"Installer state path is not a real directory: {root}")
    try:
        root.chmod(0o700)
    except OSError:
        pass
    lock_path = root / "installer.lock"
    st = lstat_or_none(lock_path)
    if st is not None and not stat.S_ISREG(st.st_mode):
        fail(f"Installer lock path is not a regular file: {lock_path}")
    fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail("Another installer operation is already running. Wait for it to finish and retry.")
        yield
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        finally:
            os.close(fd)


def assert_not_running(target: Path) -> None:
    data = target / "DO_NOT_SHARE"
    data_st = lstat_or_none(data)
    if data_st is not None and stat.S_ISLNK(data_st.st_mode):
        fail(f"{data} is a symlink. Refusing to use or modify a redirected data directory.")
    lock_path = data / "instance.lock"
    st = lstat_or_none(lock_path)
    if st is None:
        return
    if not stat.S_ISREG(st.st_mode):
        fail(f"Unexpected instance lock file type: {lock_path}")
    fd = os.open(lock_path, os.O_RDWR | getattr(os, "O_NOFOLLOW", 0))
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail("Roblox appears to be running. Close it completely before installing, updating, or uninstalling.")
        finally:
            with contextlib.suppress(OSError):
                fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)


def check_target_root(target: Path, create: bool = False) -> bool:
    st = lstat_or_none(target)
    if st is None:
        if create:
            target.mkdir(parents=True, mode=0o755)
            fsync_dir(target.parent)
            return True
        return False
    if stat.S_ISLNK(st.st_mode) or not stat.S_ISDIR(st.st_mode):
        fail(f"Install location exists but is not a real directory: {target}")
    if hasattr(os, "getuid") and st.st_uid != os.getuid():
        fail(f"Install directory is owned by another user; refusing to modify it: {target}")
    return True


def load_manifest(target: Path) -> dict[str, Any] | None:
    path = target / MANIFEST_NAME
    st = lstat_or_none(path)
    if st is None:
        return None
    if not stat.S_ISREG(st.st_mode):
        fail(f"Installer manifest is not a regular file: {path}")
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"Cannot read installer manifest ({path}): {exc}. No files were changed.")
    if not isinstance(manifest, dict) or manifest.get("schema") != 1 or manifest.get("app_id") != APP_ID:
        fail(f"Unrecognized installer manifest at {path}. Refusing to guess which files are safe to change.")
    if manifest.get("install_dir") != str(target):
        fail(f"Installer manifest belongs to a different path ({manifest.get('install_dir')!r}), not {str(target)!r}.")
    files = manifest.get("files")
    if not isinstance(files, dict):
        fail(f"Installer manifest has no valid file list: {path}")
    for name, info in files.items():
        if name not in PAYLOAD_FILES or not isinstance(info, dict) or not HEX64.fullmatch(str(info.get("sha256", ""))):
            fail(f"Installer manifest contains an unexpected entry ({name!r}). Refusing destructive changes.")
    return manifest


def check_source_root(raw: str | Path, target: Path) -> Path:
    source = Path(raw).expanduser()
    if not source.is_absolute():
        source = Path.cwd() / source
    if source.is_symlink():
        fail(f"Source directory is a symlink; use its real path instead: {source}")
    source = source.resolve(strict=True)
    if not source.is_dir():
        fail(f"Source is not a directory: {source}")
    if source == target or source in target.parents or target in source.parents:
        fail("Source and install directories overlap. Choose a separate extracted release directory and install location.")
    return source


def safe_payload_names(source: Path) -> list[str]:
    found: list[str] = []
    unknown: list[str] = []
    for child in source.iterdir():
        if child.name in PROTECTED_NAMES or child.name.startswith(TXN_PREFIX):
            continue
        if child.name not in PAYLOAD_FILES:
            # Subdirectories and extra files are deliberately not copied.
            if child.name not in {".git", "__pycache__"}:
                unknown.append(child.name)
            continue
        st = child.lstat()
        if not stat.S_ISREG(st.st_mode):
            fail(f"Release payload entry is not a regular file: {child}")
        found.append(child.name)
    missing = sorted(REQUIRED_FILES - set(found))
    if missing:
        fail("This does not look like a complete runnable release. Missing: " + ", ".join(missing))
    if unknown:
        shown = ", ".join(sorted(unknown)[:8])
        extra = " …" if len(unknown) > 8 else ""
        warn(f"Ignoring unrecognized source entries (not installed): {shown}{extra}")
    return sorted(found)


def source_looks_like_release(source: Path) -> bool:
    try:
        return REQUIRED_FILES.issubset({p.name for p in source.iterdir() if p.is_file() and not p.is_symlink()})
    except OSError:
        return False


def safe_uri_handler_script() -> bytes:
    return b'''#!/bin/sh\n# Compatibility wrapper: desktop integration is managed by installer.py.\nset -eu\nHERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)\naction=${1:-install}\nif [ "$#" -gt 0 ]; then shift; fi\ncase "$action" in\n  install|uninstall) ;;\n  *) echo "Usage: install-uri-handler.sh [install|uninstall]" >&2; exit 2 ;;\nesac\nexec python3 "$HERE/installer.py" desktop "$action" --prefix "$HERE" "$@"\n'''


def copy_payload_to_stage(source: Path, names: list[str], stage: Path, release_tag: str | None) -> dict[str, Any]:
    stage.mkdir(parents=True, exist_ok=True, mode=0o700)
    for name in names:
        destination = stage / name
        if name == "install-uri-handler.sh":
            destination.write_bytes(safe_uri_handler_script())
        else:
            shutil.copyfile(source / name, destination, follow_symlinks=False)
        mode = 0o755 if name in EXECUTABLE_FILES else 0o644
        destination.chmod(mode)
    # The installer is a control-plane file and must remain available even
    # while updating from a release archive that predates its inclusion.
    installer_dest = stage / "installer.py"
    if "installer.py" not in names:
        running_script = Path(__file__).resolve()
        if running_script.is_symlink() or not running_script.is_file():
            fail("Cannot safely locate the running installer script.")
        shutil.copyfile(running_script, installer_dest, follow_symlinks=False)
        installer_dest.chmod(0o755)
    hashes: dict[str, Any] = {}
    for name in sorted({*names, "installer.py"}):
        file_path = stage / name
        hashes[name] = {"sha256": sha256_file(file_path), "mode": stat.S_IMODE(file_path.stat().st_mode)}
    return {
        "schema": 1,
        "app_id": APP_ID,
        "app_name": APP_NAME,
        "install_dir": "",  # Filled in once the destination is normalized.
        "version": release_tag or "local",
        "source": release_tag or str(source),
        "installed_at": _dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "files": hashes,
    }


def preserve_to_backup(source: Path, target_parent: Path, label: str) -> Path:
    backup_root = target_parent / BACKUP_PREFIX
    backup_st = lstat_or_none(backup_root)
    if backup_st is not None and (stat.S_ISLNK(backup_st.st_mode) or not stat.S_ISDIR(backup_st.st_mode)):
        fail(f"Backup path is not a real directory; refusing to use it: {backup_root}")
    if backup_st is None:
        backup_root.mkdir(mode=0o700)
    if hasattr(os, "getuid") and backup_root.stat().st_uid != os.getuid():
        fail(f"Backup directory is owned by another user: {backup_root}")
    try:
        backup_root.chmod(0o700)
    except OSError:
        pass
    slot = backup_root / f"{now_stamp()}-{os.getpid()}-{label}"
    suffix = 1
    while slot.exists():
        slot = backup_root / f"{now_stamp()}-{os.getpid()}-{label}-{suffix}"
        suffix += 1
    slot.mkdir(mode=0o700)
    destination = slot / source.name
    os.replace(source, destination)
    fsync_dir(source.parent)
    fsync_dir(slot)
    return destination


def write_journal(txn: Path, journal: dict[str, Any]) -> None:
    atomic_json(txn / "journal.json", journal, 0o600)


def _remove_if_regular_or_symlink(path: Path) -> None:
    st = lstat_or_none(path)
    if st is None:
        return
    if stat.S_ISDIR(st.st_mode) and not stat.S_ISLNK(st.st_mode):
        fail(f"Refusing to unlink a directory as a file: {path}")
    path.unlink()


def rollback_transaction(target: Path, txn: Path, journal: dict[str, Any]) -> None:
    backup_root = txn / "backup"
    conflicts: list[str] = []
    for op in reversed(journal.get("files", [])):
        rel = op["path"]
        dest = target / rel
        backup = backup_root / rel
        backup_exists = lstat_or_none(backup) is not None
        dest_exists = lstat_or_none(dest) is not None
        if backup_exists:
            if dest_exists:
                current_hash = None
                try:
                    if stat.S_ISREG(dest.lstat().st_mode):
                        current_hash = sha256_file(dest)
                except OSError:
                    pass
                if current_hash == op.get("new_hash") and current_hash is not None:
                    dest.unlink()
                else:
                    try:
                        saved = preserve_to_backup(dest, target.parent, "recovery-conflict")
                        warn(f"A file changed during recovery; preserved it at {saved}")
                    except Exception as exc:
                        conflicts.append(f"could not preserve unexpected {dest}: {exc}")
                        continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            os.replace(backup, dest)
            fsync_dir(dest.parent)
        elif op.get("had_old"):
            # No backup means the original rename had not started; leave the
            # original in place. If it vanished, retain the journal as evidence.
            if not dest_exists:
                conflicts.append(f"original file and its backup are both missing: {dest}")
        elif dest_exists:
            try:
                current_hash = sha256_file(dest) if stat.S_ISREG(dest.lstat().st_mode) else None
            except OSError:
                current_hash = None
            if current_hash == op.get("new_hash") and current_hash is not None:
                dest.unlink()
    manifest = target / MANIFEST_NAME
    manifest_backup = backup_root / "MANIFEST"
    had_manifest = bool(journal.get("had_manifest"))
    if lstat_or_none(manifest_backup) is not None:
        if lstat_or_none(manifest) is not None:
            try:
                current_hash = sha256_file(manifest)
            except Exception:
                current_hash = None
            if current_hash == journal.get("new_manifest_hash"):
                manifest.unlink()
            else:
                try:
                    saved = preserve_to_backup(manifest, target.parent, "recovery-manifest-conflict")
                    warn(f"An unexpected manifest was preserved at {saved}")
                except Exception as exc:
                    conflicts.append(f"could not preserve unexpected manifest: {exc}")
        if not conflicts:
            os.replace(manifest_backup, manifest)
    elif had_manifest and lstat_or_none(manifest) is None:
        conflicts.append("old installer manifest is missing and no backup exists")
    elif not had_manifest and lstat_or_none(manifest) is not None:
        try:
            if sha256_file(manifest) == journal.get("new_manifest_hash"):
                manifest.unlink()
        except Exception:
            conflicts.append("unexpected manifest cannot be safely removed")
    if conflicts:
        fail("Could not fully recover an interrupted installer transaction. Evidence was kept at " + str(txn) + ": " + "; ".join(conflicts))
    shutil.rmtree(txn)
    fsync_dir(target.parent)
    if journal.get("target_created") and target.is_dir():
        with contextlib.suppress(OSError):
            target.rmdir()


def finalize_transaction(target: Path, txn: Path, journal: dict[str, Any], preserve_modified: bool = True) -> None:
    backup_root = txn / "backup"
    for op in journal.get("files", []):
        rel = op["path"]
        backup = backup_root / rel
        if lstat_or_none(backup) is None:
            continue
        if op.get("modified") and op.get("had_old") and preserve_modified:
            saved = preserve_to_backup(backup, target.parent, "user-modified")
            good(f"Preserved your modified {rel} at {saved}")
        else:
            _remove_if_regular_or_symlink(backup)
    manifest_backup = backup_root / "MANIFEST"
    if lstat_or_none(manifest_backup) is not None:
        _remove_if_regular_or_symlink(manifest_backup)
    shutil.rmtree(txn)
    fsync_dir(target.parent)


def recover_transactions(target: Path) -> None:
    if not target.parent.exists():
        return
    for txn in sorted(target.parent.glob(TXN_PREFIX + "*")):
        st = lstat_or_none(txn)
        if st is None or not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid():
            continue
        journal_path = txn / "journal.json"
        if lstat_or_none(journal_path) is None:
            # No journal is written until staging is complete and before the
            # first destination file changes. This is only abandoned staging.
            try:
                shutil.rmtree(txn)
            except OSError:
                warn(f"Could not clean abandoned staging directory: {txn}")
            continue
        try:
            journal = json.loads(journal_path.read_text(encoding="utf-8"))
        except Exception as exc:
            fail(f"Cannot read pending installer recovery journal {journal_path}: {exc}")
        if journal.get("target") != str(target):
            continue
        state = journal.get("state")
        if state == "applying":
            warn("Found an interrupted installer transaction; rolling it back safely.")
            rollback_transaction(target, txn, journal)
            good("Recovered the previous installation state.")
        elif state == "committed":
            warn("Finishing cleanup from a previously completed installation transaction.")
            finalize_transaction(target, txn, journal)
        else:
            fail(f"Unknown installer transaction state in {journal_path}; refusing to guess.")


def inspect_target(target: Path, new_files: dict[str, Any] | None = None) -> dict[str, Any] | None:
    exists = check_target_root(target)
    if not exists:
        return None
    data = target / "DO_NOT_SHARE"
    for protected_name in PROTECTED_NAMES:
        protected_path = target / protected_name
        protected_st = lstat_or_none(protected_path)
        if protected_st is not None and (stat.S_ISLNK(protected_st.st_mode) or not stat.S_ISDIR(protected_st.st_mode)):
            fail(f"{protected_path} is not a real directory. Refusing to follow, overwrite, or delete it.")
    manifest = load_manifest(target)
    if manifest is None:
        conflicts: list[str] = []
        new_names = set(new_files or {})
        for child in target.iterdir():
            child_st = child.lstat()
            if child.name in PROTECTED_NAMES and stat.S_ISDIR(child_st.st_mode) and not stat.S_ISLNK(child_st.st_mode):
                continue
            if child.name in USER_CONFIG_FILES and stat.S_ISREG(child_st.st_mode):
                continue
            if child.name in new_names or child.name == MANIFEST_NAME:
                conflicts.append(child.name)
            else:
                warn(f"Preserving unrelated pre-existing entry without modifying it: {child}")
        if conflicts:
            fail(
                f"{target} contains paths that the release would need to use, but no valid installer manifest proves ownership. "
                "Nothing was changed. Move those conflicting paths aside or choose a new --prefix. Conflicts: "
                + ", ".join(sorted(conflicts)[:10])
            )
        return None
    if new_files is not None:
        previous_names = set(manifest["files"])
        for name in new_files:
            path = target / name
            st = lstat_or_none(path)
            if st is None:
                continue
            if name not in previous_names:
                fail(f"Update conflict: {path} exists but is not owned by this installer. It was left untouched.")
            if not stat.S_ISREG(st.st_mode):
                fail(f"Update conflict: managed path is no longer a regular file: {path}")
        for name in previous_names:
            st = lstat_or_none(target / name)
            if st is not None and not stat.S_ISREG(st.st_mode):
                fail(f"Managed path is not a regular file; refusing to replace or remove it: {target / name}")
    return manifest


def build_operations(target: Path, old_manifest: dict[str, Any] | None, new_manifest: dict[str, Any] | None) -> list[dict[str, Any]]:
    old_files = old_manifest["files"] if old_manifest else {}
    new_files = new_manifest["files"] if new_manifest else {}
    operations: list[dict[str, Any]] = []
    for name in sorted(set(old_files) | set(new_files)):
        dest = target / name
        st = lstat_or_none(dest)
        old_record = old_files.get(name)
        new_record = new_files.get(name)
        if st is None:
            if new_record is not None:
                operations.append({"path": name, "action": "replace", "had_old": False, "old_hash": None,
                                   "new_hash": new_record["sha256"], "modified": False})
            continue
        if not stat.S_ISREG(st.st_mode):
            # An unknown or modified non-file is never deleted, even at an
            # installer-owned path.
            warn(f"Preserving non-regular path without touching it: {dest}")
            continue
        current_hash = sha256_file(dest)
        if new_record is not None:
            if old_record is None:
                if name in USER_CONFIG_FILES and current_hash == new_record["sha256"]:
                    continue
                fail(f"Update conflict: {dest} exists but was not in the previous installer manifest.")
            if current_hash == new_record["sha256"]:
                continue
            operations.append({"path": name, "action": "replace", "had_old": True,
                               "old_hash": current_hash, "new_hash": new_record["sha256"],
                               "modified": current_hash != old_record["sha256"]})
        elif old_record is not None:
            if name in USER_CONFIG_FILES:
                warn(f"Preserving user configuration on uninstall: {dest}")
            elif current_hash == old_record["sha256"]:
                operations.append({"path": name, "action": "remove", "had_old": True,
                                   "old_hash": current_hash, "new_hash": None, "modified": False})
            else:
                warn(f"Preserving modified file no longer shipped by the release: {dest}")
    return operations


def apply_transaction(target: Path, txn: Path, old_manifest: dict[str, Any] | None,
                      new_manifest: dict[str, Any] | None, staged_payload: Path | None,
                      target_created: bool = False, preserve_modified: bool = True) -> None:
    manifest_path = target / MANIFEST_NAME
    old_manifest_exists = lstat_or_none(manifest_path) is not None
    old_manifest_hash = sha256_file(manifest_path) if old_manifest_exists else None
    new_manifest_data = json_bytes(new_manifest) if new_manifest is not None else None
    new_manifest_hash = hashlib.sha256(new_manifest_data).hexdigest() if new_manifest_data is not None else None
    if new_manifest_data is not None:
        atomic_write(txn / "new_manifest.json", new_manifest_data, 0o600)
    operations = build_operations(target, old_manifest, new_manifest)
    journal = {
        "schema": 1,
        "target": str(target),
        "state": "applying",
        "target_created": target_created,
        "had_manifest": old_manifest_exists,
        "old_manifest_hash": old_manifest_hash,
        "new_manifest_hash": new_manifest_hash,
        "files": operations,
    }
    (txn / "backup").mkdir(parents=True, exist_ok=True, mode=0o700)
    write_journal(txn, journal)
    try:
        for op in operations:
            name = op["path"]
            dest = target / name
            backup = txn / "backup" / name
            if op["had_old"]:
                backup.parent.mkdir(parents=True, exist_ok=True)
                os.replace(dest, backup)
                fsync_dir(dest.parent)
                fsync_dir(backup.parent)
            if op["action"] == "replace":
                if staged_payload is None:
                    fail(f"Internal error: no staged payload available for {name}")
                os.replace(staged_payload / name, dest)
                fsync_dir(dest.parent)
        if old_manifest_exists:
            os.replace(manifest_path, txn / "backup" / "MANIFEST")
            fsync_dir(target)
        if new_manifest_data is not None:
            os.replace(txn / "new_manifest.json", manifest_path)
            fsync_dir(target)
        journal["state"] = "committed"
        write_journal(txn, journal)
    except Exception:
        try:
            rollback_transaction(target, txn, journal)
        except Exception as recovery_exc:
            warn(f"Automatic rollback was incomplete: {recovery_exc}")
        raise
    finalize_transaction(target, txn, journal, preserve_modified=preserve_modified)


def check_platform() -> None:
    if not sys.platform.startswith("linux"):
        fail("This release installer supports Linux only.")
    machine = platform.machine().lower()
    if machine not in {"x86_64", "amd64"}:
        fail(f"This release targets x86_64 Linux, but this system reports {machine!r}.")
    libc_name, libc_version = platform.libc_ver()
    if libc_name == "glibc" and libc_version:
        try:
            version = tuple(int(part) for part in libc_version.split(".")[:2])
        except ValueError:
            version = ()
        if version and version < (2, 39):
            warn(f"This release expects glibc 2.39 or newer; this system reports {libc_version}. Installation can continue, but launch may fail.")
    missing = [cmd for cmd in ("curl", "unzip", "flock", "timeout") if shutil.which(cmd) is None]
    if missing:
        warn("Runtime tools not found on PATH: " + ", ".join(missing) + ". Install them before launching/updating Roblox.")
    if not os.environ.get("WAYLAND_DISPLAY") and os.environ.get("XDG_SESSION_TYPE", "").lower() != "wayland":
        warn("No Wayland session was detected. This release does not support X11-only sessions.")


def check_local_space(source: Path, target: Path, names: list[str], dry_run: bool = False) -> None:
    total = 0
    for name in names:
        try:
            total += (source / name).stat().st_size
        except OSError:
            continue
    # One staged copy of the payload is required before the managed files are
    # switched. Keep a small margin for the manifest, icons, and recovery data.
    required = total + 64 * 1024 * 1024
    parent = target.parent
    if not parent.exists():
        if dry_run:
            note(f"Would check free space in {parent} (payload is about {total // (1024*1024)} MiB).")
            return
        parent.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(parent).free
    if free < required:
        fail(f"Not enough free space beside the install location. Need about {required // (1024*1024)} MiB for staging; only {free // (1024*1024)} MiB is free.")


def run_install_or_update(target: Path, source: Path, release_tag: str | None,
                          dry_run: bool = False, migrate_data: bool | None = None,
                          force_desktop: bool = False, yes: bool = False,
                          confirmed: bool = False,
                          install_desktop_integration: bool = True) -> None:
    names = safe_payload_names(source)
    check_local_space(source, target, names, dry_run=dry_run)
    if not target.parent.exists():
        if dry_run:
            note(f"Would create install parent: {target.parent}")
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
    if not dry_run:
        recover_transactions(target)
    old_manifest = inspect_target(target, {name: {} for name in (set(names) | {"installer.py"})})
    target_exists = target.exists()
    if target_exists:
        assert_not_running(target)
    if old_manifest:
        note(f"Existing managed installation detected (version {old_manifest.get('version', 'unknown')}).")
    else:
        note("No managed installation found; this will be a fresh per-user install.")
    print(f"Target: {target}")
    print(f"Release source: {release_tag or source}")
    print(f"Managed payload: {len(names)} release files + installer")
    if dry_run:
        note("Dry run: no files will be changed.")
        for name in names:
            print(f"  + {name}")
        data_source = source / "DO_NOT_SHARE"
        if lstat_or_none(data_source) is not None and lstat_or_none(target / "DO_NOT_SHARE") is None:
            note("Would offer to copy the existing DO_NOT_SHARE folder; source data would remain untouched.")
        return
    if not dry_run and not yes and not confirmed:
        if not sys.stdin.isatty():
            fail("This operation changes files. Re-run with --yes for a non-interactive install/update.")
        if not confirm(f"Install/update {APP_NAME} at {target}?", default=False):
            note("Cancelled; nothing was changed.")
            return
    txn = Path(tempfile.mkdtemp(prefix=TXN_PREFIX, dir=target.parent))
    target_created = False
    try:
        payload_stage = txn / "payload"
        new_manifest = copy_payload_to_stage(source, names, payload_stage, release_tag)
        new_manifest["install_dir"] = str(target)
        # FFlags.json is a user-editable override file. Preserve the current
        # version through updates and record its current hash in the manifest;
        # removing it before an update is the explicit way to reset to defaults.
        existing_flags = target / "FFlags.json"
        flags_st = lstat_or_none(existing_flags)
        if flags_st is not None and "FFlags.json" in new_manifest.get("files", {}):
            if not stat.S_ISREG(flags_st.st_mode):
                fail(f"Refusing to follow or replace the user configuration path: {existing_flags}")
            shutil.copyfile(existing_flags, payload_stage / "FFlags.json", follow_symlinks=False)
            flags_mode = stat.S_IMODE(flags_st.st_mode)
            (payload_stage / "FFlags.json").chmod(flags_mode)
            new_manifest["files"]["FFlags.json"] = {
                "sha256": sha256_file(payload_stage / "FFlags.json"), "mode": flags_mode
            }
        # Source files are all staged and hashed before touching the live install.
        if not target.exists():
            target.mkdir(parents=False, mode=0o755)
            target_created = True
            fsync_dir(target.parent)
        apply_transaction(target, txn, old_manifest, new_manifest, payload_stage, target_created)
    except Exception:
        # If the transaction did not start, remove only our own staging area.
        if txn.exists() and lstat_or_none(txn / "journal.json") is None:
            with contextlib.suppress(OSError):
                shutil.rmtree(txn)
        if target_created and target.exists():
            with contextlib.suppress(OSError):
                target.rmdir()
        raise
    good(f"Application files installed at {target}")

    # Data is migrated only if the destination has no data directory. It is
    # copied (never moved), so the original remains recoverable until the user
    # chooses to remove it themselves. Symlinks are preserved, not followed.
    source_data = source / "DO_NOT_SHARE"
    target_data = target / "DO_NOT_SHARE"
    source_data_st = lstat_or_none(source_data)
    target_data_st = lstat_or_none(target_data)
    if source_data_st is not None and stat.S_ISLNK(source_data_st.st_mode):
        warn(f"Source data is a symlink and was not copied: {source_data}")
    elif source_data_st is not None and stat.S_ISDIR(source_data_st.st_mode) and target_data_st is None:
        should_copy = yes or (confirm("Copy your existing DO_NOT_SHARE data into the new install? The source copy will be kept.", default=True) if sys.stdin.isatty() else False)
        if should_copy:
            assert_not_running(source)
            migration_tmp = target.parent / f".{target.name}.data-migration-{os.getpid()}"
            if lstat_or_none(migration_tmp) is not None:
                fail(f"Data migration staging path already exists: {migration_tmp}")
            try:
                shutil.copytree(source_data, migration_tmp, symlinks=True, copy_function=shutil.copy2)
                migration_tmp.chmod(0o700)
                if lstat_or_none(target_data) is not None:
                    fail(f"Data directory appeared during migration; leaving both copies untouched: {target_data}")
                os.replace(migration_tmp, target_data)
                fsync_dir(target)
                good("Copied DO_NOT_SHARE data. The original folder was left in place.")
            except Exception as exc:
                if lstat_or_none(migration_tmp) is not None:
                    with contextlib.suppress(OSError):
                        shutil.rmtree(migration_tmp)
                warn(f"Application installed, but data migration did not complete: {exc}")
        else:
            warn(f"Existing data was not copied. You can migrate it later from {source_data} to {target_data}.")
    elif source_data_st is not None and target_data_st is not None:
        note("Both source and destination already have DO_NOT_SHARE; neither copy was changed or merged.")
    if lstat_or_none(target / "DO_NOT_SHARE") is None:
        note("DO_NOT_SHARE will be created by the launcher on first run. It is always preserved on uninstall.")

    if install_desktop_integration:
        try:
            install_desktop(target, force=force_desktop, yes=yes, dry_run=False)
        except InstallerError as exc:
            warn(f"Application files are installed, but desktop integration was not changed: {exc}")
            warn("Resolve the desktop-file conflict or run: python3 installer.py desktop install --force-desktop")
    else:
        note("Desktop integration skipped. Run 'installer.py desktop install --yes' later to enable it.")
    if install_desktop_integration:
        good("Done. Launch 'Roblox (Mac Linux Port)' from your application menu, or run run.sh from the install folder.")
    else:
        good("Done. Launch with run.sh from the install folder, or enable desktop integration later.")


def confirm(prompt: str, default: bool = False) -> bool:
    if not sys.stdin.isatty():
        return False
    suffix = "[Y/n]" if default else "[y/N]"
    try:
        answer = input(f"{prompt} {suffix} ").strip().lower()
    except (EOFError, KeyboardInterrupt):
        print()
        return False
    if not answer:
        return default
    return answer in {"y", "yes"}


def desktop_exec_quote(value: str) -> str:
    # Freedesktop Desktop Entry Exec quoting; percent must be doubled to avoid
    # accidental field-code expansion when a home path contains a percent sign.
    value = value.replace("%", "%%")
    value = value.replace("\\", "\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$")
    return f'"{value}"'


def desktop_file_contents(target: Path, settings: bool = False) -> bytes:
    run = str(target / "run.sh")
    if settings:
        name = "Roblox Launcher Settings"
        comment = "Configure Roblox Mac Linux Port"
        exec_line = f"{desktop_exec_quote(run)} --settings"
        icon = "roblox-mac-port-settings"
        mime = ""
    else:
        name = "Roblox (Mac Linux Port)"
        comment = "Launch the Intel macOS Roblox client"
        exec_line = f"{desktop_exec_quote(run)} %u"
        icon = "roblox-mac-port"
        mime = "MimeType=x-scheme-handler/roblox;x-scheme-handler/roblox-player;\n"
    text = (
        "[Desktop Entry]\n"
        f"# Managed by {APP_NAME}; do not remove the ownership marker.\n"
        f"Name={name}\nComment={comment}\nType=Application\n"
        f"{DESKTOP_MARKER}\n"
        f"Exec={exec_line}\nIcon={icon}\nTerminal=false\nStartupNotify=true\n"
        "StartupWMClass=roblox-mac-port\nCategories=Game;\n"
        f"{mime}"
    )
    return text.encode("utf-8")


def ensure_user_data_dir(directory: Path) -> None:
    root = data_home()
    directory = directory.resolve(strict=False) if directory == root else directory.absolute()
    try:
        relative = directory.relative_to(root)
    except ValueError:
        fail(f"Desktop path is outside XDG_DATA_HOME: {directory}")
    root.mkdir(parents=True, exist_ok=True, mode=0o755)
    root_st = lstat_or_none(root)
    if root_st is None or stat.S_ISLNK(root_st.st_mode) or not stat.S_ISDIR(root_st.st_mode):
        fail(f"XDG data directory is not a real directory: {root}")
    current = root
    for part in relative.parts:
        current = current / part
        st = lstat_or_none(current)
        if st is None:
            current.mkdir(mode=0o755)
            fsync_dir(current.parent)
        elif stat.S_ISLNK(st.st_mode) or not stat.S_ISDIR(st.st_mode):
            fail(f"Refusing to follow a symlink or non-directory in desktop path: {current}")


def user_data_dir_is_safe(directory: Path) -> bool:
    root = data_home()
    try:
        relative = directory.absolute().relative_to(root)
    except ValueError:
        return False
    root_st = lstat_or_none(root)
    if root_st is None or stat.S_ISLNK(root_st.st_mode) or not stat.S_ISDIR(root_st.st_mode):
        return False
    current = root
    for part in relative.parts:
        current = current / part
        st = lstat_or_none(current)
        if st is None:
            return False
        if stat.S_ISLNK(st.st_mode) or not stat.S_ISDIR(st.st_mode):
            return False
    return True


def desktop_paths() -> dict[str, Path]:
    root = data_home()
    icon_root = root / "icons" / "hicolor" / "256x256" / "apps"
    apps = root / "applications"
    return {
        str(apps / DESKTOP_ID): apps / DESKTOP_ID,
        str(apps / SETTINGS_DESKTOP_ID): apps / SETTINGS_DESKTOP_ID,
        str(icon_root / "roblox-mac-port.png"): icon_root / "roblox-mac-port.png",
        str(icon_root / "roblox-mac-port-settings.png"): icon_root / "roblox-mac-port-settings.png",
    }


def desktop_state_path() -> Path:
    return state_root() / DESKTOP_STATE_NAME


def load_desktop_state() -> dict[str, Any] | None:
    path = desktop_state_path()
    st = lstat_or_none(path)
    if st is None:
        return None
    if not stat.S_ISREG(st.st_mode):
        fail(f"Desktop ownership state is not a regular file: {path}")
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        fail(f"Cannot read desktop ownership state {path}: {exc}")
    if not isinstance(state, dict) or state.get("app_id") != APP_ID or not isinstance(state.get("files"), dict):
        fail(f"Unrecognized desktop ownership state: {path}")
    allowed_paths = {str(p) for p in desktop_paths().values()}
    for raw, info in state["files"].items():
        if raw not in allowed_paths or not isinstance(info, dict) or not HEX64.fullmatch(str(info.get("sha256", ""))):
            fail(f"Desktop ownership state contains an unexpected path or hash: {raw!r}")
    associations = state.get("mime_associations", [])
    if not isinstance(associations, list) or any(key not in URI_SCHEMES for key in associations):
        fail(f"Desktop ownership state contains unexpected MIME associations: {path}")
    return state


def parse_mimeapps(text: str) -> tuple[list[str], int | None, int | None]:
    lines = text.splitlines(keepends=True)
    section_start = None
    section_end = len(lines)
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if stripped == "[Default Applications]":
                section_start = i
                for j in range(i + 1, len(lines)):
                    s = lines[j].strip()
                    if s.startswith("[") and s.endswith("]"):
                        section_end = j
                        break
                break
    return lines, section_start, section_end if section_start is not None else None


def mime_values(line: str) -> list[str]:
    value = line.split("=", 1)[1].strip()
    return [v for v in value.split(";") if v]


def edit_mimeapps(action: str, owned_keys: list[str] | None = None, force: bool = False,
                  yes: bool = False) -> list[str]:
    path = config_home() / "mimeapps.list"
    st = lstat_or_none(path)
    if st is not None and not stat.S_ISREG(st.st_mode):
        fail(f"MIME association file is not a regular file; refusing to edit it: {path}")
    if st is None:
        original = ""
        mode = 0o600
    else:
        original = path.read_text(encoding="utf-8")
        mode = stat.S_IMODE(st.st_mode)
    lines, section_start, section_end = parse_mimeapps(original)
    if action == "install" and section_start is None:
        if lines and not lines[-1].endswith("\n"):
            lines[-1] += "\n"
        if lines and lines[-1].strip():
            lines.append("\n")
        lines.append("[Default Applications]\n")
        section_start = len(lines) - 1
        section_end = len(lines)
    assert section_start is not None or action == "uninstall"
    changed = False
    managed: list[str] = []
    keys = list(owned_keys or URI_SCHEMES) if action == "uninstall" else list(URI_SCHEMES)
    if section_start is None:
        return []
    # Locate association lines only inside [Default Applications]. Duplicate
    # keys are ambiguous, so leave the file untouched rather than guessing.
    for key in keys:
        current_lines, current_start, current_end = parse_mimeapps("".join(lines))
        if current_start is None:
            continue
        matches = [i for i in range(current_start + 1, current_end or len(current_lines))
                   if current_lines[i].partition("=")[0].strip() == key and "=" in current_lines[i]
                   and not current_lines[i].lstrip().startswith(("#", ";"))]
        if len(matches) > 1:
            warn(f"Multiple {key} entries exist in mimeapps.list; leaving that association unchanged.")
            continue
        if action == "install":
            if matches:
                idx = matches[0]
                values = mime_values(current_lines[idx])
                if values and values[0] == DESKTOP_ID:
                    managed.append(key)
                    continue
                if values and values[0] != DESKTOP_ID and not (force or confirm(
                    f"A different default handler ({values[0]}) is set for {key}. Make this app the default?", default=False
                )):
                    warn(f"Kept the existing default handler for {key}: {values[0]}")
                    continue
                new_values = [DESKTOP_ID] + [v for v in values if v != DESKTOP_ID]
                lines[idx] = f"{key}=" + ";".join(new_values) + ";\n"
                changed = changed or lines[idx] != current_lines[idx]
                managed.append(key)
            else:
                insert_at = current_end if current_end is not None else len(lines)
                lines.insert(insert_at, f"{key}={DESKTOP_ID};\n")
                changed = True
                managed.append(key)
        else:
            if not matches:
                continue
            idx = matches[0]
            values = mime_values(current_lines[idx])
            if DESKTOP_ID not in values:
                continue
            new_values = [v for v in values if v != DESKTOP_ID]
            if new_values:
                lines[idx] = f"{key}=" + ";".join(new_values) + ";\n"
            else:
                del lines[idx]
            changed = True
    if changed:
        atomic_write(path, "".join(lines).encode("utf-8"), mode)
    return managed


def backup_collision(path: Path) -> Path:
    # Keep any pre-existing desktop entry/icon intact before an explicit force
    # replacement. os.replace keeps the original bytes and does not follow links.
    return preserve_to_backup(path, path.parent, "desktop-conflict")


def install_desktop(target: Path, force: bool = False, yes: bool = False, dry_run: bool = False) -> None:
    run = target / "run.sh"
    if lstat_or_none(run) is None or not run.is_file() or run.is_symlink():
        fail(f"Cannot install desktop integration; launcher is missing: {run}")
    icon_source = target / "icon.png"
    settings_icon_source = target / "icon_bw.png"
    for icon in (icon_source, settings_icon_source):
        assert_regular_file(icon, "application icon")
    paths = desktop_paths()
    desired: dict[str, bytes | Path] = {
        str(paths[str(data_home() / "applications" / DESKTOP_ID)]): desktop_file_contents(target, False),
        str(paths[str(data_home() / "applications" / SETTINGS_DESKTOP_ID)]): desktop_file_contents(target, True),
        str(paths[str(data_home() / "icons" / "hicolor" / "256x256" / "apps" / "roblox-mac-port.png")]): icon_source,
        str(paths[str(data_home() / "icons" / "hicolor" / "256x256" / "apps" / "roblox-mac-port-settings.png")]): settings_icon_source,
    }
    state = load_desktop_state()
    previous = state.get("files", {}) if state and state.get("install_dir") == str(target) else {}
    conflicts: list[Path] = []
    for raw, content in desired.items():
        path = Path(raw)
        st = lstat_or_none(path)
        if st is None:
            continue
        old_record = previous.get(raw)
        if old_record and stat.S_ISREG(st.st_mode) and sha256_file(path) == old_record.get("sha256"):
            continue
        conflicts.append(path)
    if conflicts and not (force or (sys.stdin.isatty() and confirm(
        "Existing desktop files are not verified as installer-owned. Back them up and replace them?", default=False
    ))):
        fail("Desktop integration conflict; no desktop files were changed. Existing files were preserved: " + ", ".join(map(str, conflicts)))
    if dry_run:
        note("Would install two desktop entries, two icons, and register the Roblox URI schemes.")
        return
    # Create only real directories below XDG_DATA_HOME; do not follow a
    # redirected applications/icons subdirectory.
    for raw in desired:
        ensure_user_data_dir(Path(raw).parent)
    written: list[Path] = []
    backups: list[tuple[Path, Path]] = []
    rollback_snapshots: dict[Path, tuple[bytes, int]] = {}
    mime_path = config_home() / "mimeapps.list"
    mime_st = lstat_or_none(mime_path)
    if mime_st is not None and not stat.S_ISREG(mime_st.st_mode):
        fail(f"MIME association file is not a regular file; refusing to edit it: {mime_path}")
    mime_snapshot = (mime_path.read_bytes(), stat.S_IMODE(mime_st.st_mode)) if mime_st is not None else None
    try:
        for raw, content in desired.items():
            path = Path(raw)
            st = lstat_or_none(path)
            old_record = previous.get(raw)
            owned_unchanged = bool(st and old_record and stat.S_ISREG(st.st_mode) and sha256_file(path) == old_record.get("sha256"))
            if st is not None and not owned_unchanged:
                saved = backup_collision(path)
                backups.append((path, saved))
            elif st is not None and owned_unchanged:
                rollback_snapshots[path] = (path.read_bytes(), stat.S_IMODE(st.st_mode))
            if isinstance(content, bytes):
                payload = content
                mode = 0o644
            else:
                payload = content.read_bytes()
                mode = 0o644
            atomic_write(path, payload, mode)
            written.append(path)
        mime_owned = edit_mimeapps("install", force=force, yes=yes)
        new_state = {
            "app_id": APP_ID,
            "install_dir": str(target),
            "installed_at": _dt.datetime.now().astimezone().isoformat(timespec="seconds"),
            "files": {raw: {"sha256": sha256_file(Path(raw))} for raw in desired},
            "mime_associations": mime_owned,
        }
        atomic_json(desktop_state_path(), new_state, 0o600)
    except Exception:
        # Restore entries written during this attempt. New files are removed;
        # snapshots restore owned entries, and explicit conflict backups restore
        # any pre-existing unowned entries moved aside by --force-desktop.
        for path in reversed(written):
            with contextlib.suppress(OSError):
                path.unlink()
        for path, (payload, mode) in rollback_snapshots.items():
            with contextlib.suppress(OSError):
                atomic_write(path, payload, mode)
        if mime_snapshot is not None:
            with contextlib.suppress(OSError):
                atomic_write(mime_path, mime_snapshot[0], mime_snapshot[1])
        elif lstat_or_none(mime_path) is not None:
            with contextlib.suppress(OSError):
                mime_path.unlink()
        for original, saved in reversed(backups):
            with contextlib.suppress(OSError):
                os.replace(saved, original)
        raise
    if shutil.which("update-desktop-database"):
        subprocess.run(["update-desktop-database", str(data_home() / "applications")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    good("Desktop launcher and Roblox URI handler installed.")
    if len(mime_owned) < len(URI_SCHEMES):
        warn("One or more URI schemes keep their existing default handler. You can change them later in your desktop settings.")


def uninstall_desktop(dry_run: bool = False, expected_target: Path | None = None) -> None:
    state = load_desktop_state()
    if not state:
        note("No desktop integration ownership record was found; unrelated desktop files will not be removed.")
        return
    if expected_target is not None and state.get("install_dir") != str(expected_target):
        note("Desktop integration belongs to a different install location; leaving it untouched.")
        return
    if dry_run:
        note("Would remove unchanged installer-owned desktop entries/icons and only this app's MIME associations.")
        return
    for key in state.get("mime_associations", []):
        if key not in URI_SCHEMES:
            fail(f"Desktop state contains an unexpected MIME association: {key}")
    edit_mimeapps("uninstall", owned_keys=state.get("mime_associations", []))
    for raw, info in state.get("files", {}).items():
        path = Path(raw)
        if not user_data_dir_is_safe(path.parent):
            warn(f"Preserving desktop path because its parent directory is missing or redirected: {path}")
            continue
        st = lstat_or_none(path)
        if st is None:
            continue
        if not stat.S_ISREG(st.st_mode):
            warn(f"Preserving changed desktop path: {path}")
            continue
        try:
            current_hash = sha256_file(path)
        except Exception:
            current_hash = ""
        if current_hash == info.get("sha256"):
            path.unlink()
            fsync_dir(path.parent)
        else:
            warn(f"Preserving modified desktop file/icon: {path}")
    with contextlib.suppress(FileNotFoundError):
        desktop_state_path().unlink()
    if shutil.which("update-desktop-database"):
        subprocess.run(["update-desktop-database", str(data_home() / "applications")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    good("Removed installer-owned desktop integration. User changes and unrelated handlers were preserved.")


def remove_install(target: Path, yes: bool = False, dry_run: bool = False,
                   remove_client_cache: bool = False, keep_data: bool = True) -> None:
    recover_transactions(target)
    manifest = inspect_target(target)
    if manifest is None:
        fail(f"No valid installer manifest exists at {target}; refusing to delete an unknown folder.")
    assert_not_running(target)
    print(f"Install location: {target}")
    if keep_data:
        print("Data choice: keep DO_NOT_SHARE, RobloxVersion, and user files")
    else:
        print("Data choice: remove app data inside this install when it is a real directory")
    print("Unknown files and external paths are always kept.")
    if keep_data and remove_client_cache:
        print("RobloxVersion will be removed only if its expected client markers are present; interrupted .update-work will be kept.")
    if dry_run:
        note("Dry run: no files will be removed.")
        for name, info in sorted(manifest["files"].items()):
            path = target / name
            st = lstat_or_none(path)
            if st is None:
                print(f"  - {name} (already missing)")
            elif stat.S_ISREG(st.st_mode) and sha256_file(path) == info["sha256"]:
                print(f"  - {name}")
            elif not keep_data and stat.S_ISREG(st.st_mode):
                print(f"  - {name} (modified; full purge requested)")
            else:
                print(f"  ! {name} (modified/unexpected; would be preserved)")
        if not keep_data:
            for name in ("DO_NOT_SHARE", "RobloxVersion", ".update-work"):
                if lstat_or_none(target / name) is not None:
                    print(f"  - {name} (app data)")
        return
    if not yes and (not sys.stdin.isatty() or not confirm(f"Uninstall application files from {target}?", default=False)):
        note("Cancelled; nothing was changed.")
        return
    txn = Path(tempfile.mkdtemp(prefix=TXN_PREFIX, dir=target.parent))
    try:
        apply_transaction(target, txn, manifest, None, None, False, preserve_modified=keep_data)
    except Exception:
        if txn.exists() and lstat_or_none(txn / "journal.json") is None:
            with contextlib.suppress(OSError):
                shutil.rmtree(txn)
        raise
    try:
        uninstall_desktop(expected_target=target)
    except InstallerError as exc:
        warn(f"Application files were removed, but desktop cleanup needs attention: {exc}")
    if keep_data and remove_client_cache:
        remove_runtime_cache(target)
    if not keep_data:
        remove_full_data(target, manifest)
    # Only remove the install root when it is truly empty. DO_NOT_SHARE and any
    # unknown/modified files make it stay in place by design.
    try:
        target.rmdir()
        fsync_dir(target.parent)
        if keep_data:
            good("Uninstalled application files. No data directory remained to preserve.")
        else:
            good("Uninstalled application and app data. No unrelated files were removed.")
    except OSError:
        if keep_data:
            good("Uninstalled application files. Your data was preserved.")
            note(f"Preserved data path: {target / 'DO_NOT_SHARE'}")
        else:
            good("Uninstalled application and app data. Unrelated entries were preserved.")
            note(f"Remaining entries are at: {target}")
        try:
            remaining = sorted(p.name for p in target.iterdir())
            if remaining:
                note("Preserved entries: " + ", ".join(remaining[:12]) + (" …" if len(remaining) > 12 else ""))
        except OSError:
            pass


def remove_runtime_cache(target: Path) -> None:
    client = target / "RobloxVersion"
    st = lstat_or_none(client)
    if st is not None:
        if not stat.S_ISDIR(st.st_mode) or stat.S_ISLNK(st.st_mode):
            warn(f"Preserving unexpected RobloxVersion path: {client}")
        elif (client / ".version").is_file() and (client / "RobloxPlayer.app" / "Contents" / "MacOS" / "RobloxPlayer").is_file():
            shutil.rmtree(client)
            good("Removed the recognized Roblox client cache.")
        else:
            warn(f"Preserving RobloxVersion because its expected client markers were not found: {client}")
    work = target / ".update-work"
    work_st = lstat_or_none(work)
    if work_st is not None:
        if not stat.S_ISDIR(work_st.st_mode) or stat.S_ISLNK(work_st.st_mode):
            warn(f"Preserving unexpected update-work path: {work}")
        elif lstat_or_none(work / "TXN") is not None:
            warn(f"Preserving interrupted update recovery data: {work}")
        else:
            shutil.rmtree(work)
            good("Removed the non-transactional update staging cache.")


def remove_full_data(target: Path, manifest: dict[str, Any]) -> list[Path]:
    """Remove explicitly app-owned data for a confirmed full uninstall.

    Only manifest paths and the three runtime-owned directories are considered.
    Unknown entries are never traversed or deleted, and symlinks are removed
    only as links (never followed).
    """
    removed: list[Path] = []
    for name in manifest.get("files", {}):
        path = target / name
        st = lstat_or_none(path)
        if st is None:
            continue
        if stat.S_ISREG(st.st_mode) or stat.S_ISLNK(st.st_mode):
            path.unlink()
            removed.append(path)
        else:
            warn(f"Preserving unexpected managed path during full uninstall: {path}")
    for name in ("DO_NOT_SHARE", "RobloxVersion", ".update-work"):
        path = target / name
        st = lstat_or_none(path)
        if st is None:
            continue
        if stat.S_ISLNK(st.st_mode):
            warn(f"Preserving symlink during full uninstall: {path}")
            continue
        if not stat.S_ISDIR(st.st_mode):
            warn(f"Preserving unexpected data path during full uninstall: {path}")
            continue
        shutil.rmtree(path)
        removed.append(path)
    return removed


def fetch_json(url: str) -> dict[str, Any]:
    request = urllib.request.Request(url, headers={"User-Agent": f"{APP_ID}-installer", "Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        fail(f"Could not query the latest GitHub release: {exc}")


def latest_release_metadata() -> dict[str, Any]:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", REPOSITORY):
        fail("ROBLOX_MAC_PORT_REPO must be in owner/repository form.")
    release = fetch_json(f"https://api.github.com/repos/{REPOSITORY}/releases/latest")
    assets = release.get("assets", [])
    archive = next((a for a in assets if a.get("name") == "RobloxLinuxRelease.tar.gz"), None)
    checksum = next((a for a in assets if a.get("name") == "RobloxLinuxRelease.tar.gz.sha256"), None)
    if not archive or not str(archive.get("browser_download_url", "")).startswith("https://github.com/"):
        fail("The latest GitHub release does not contain the expected RobloxLinuxRelease.tar.gz asset.")
    digest = archive.get("digest")
    if isinstance(digest, str) and digest.startswith("sha256:"):
        digest = digest.split(":", 1)[1].lower()
    else:
        digest = None
    if not digest and checksum:
        checksum_url = checksum.get("browser_download_url", "")
        if not checksum_url.startswith("https://github.com/"):
            fail("The release checksum asset URL is not trusted HTTPS.")
        request = urllib.request.Request(checksum_url, headers={"User-Agent": f"{APP_ID}-installer"})
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                text = response.read(4096).decode("utf-8", "replace")
        except Exception as exc:
            fail(f"Could not fetch the release checksum: {exc}")
        match = re.search(r"\b([0-9a-fA-F]{64})\b", text)
        digest = match.group(1).lower() if match else None
    if not digest or not HEX64.fullmatch(digest):
        fail("The latest release does not provide a usable SHA-256 checksum. Refusing an unverifiable update.")
    return {"tag": str(release.get("tag_name") or "latest"), "url": archive["browser_download_url"],
            "digest": digest, "size": int(archive.get("size") or 0)}


def download_file(url: str, destination: Path, expected_size: int = 0) -> None:
    if not url.startswith("https://github.com/"):
        fail(f"Refusing to download from an unexpected URL: {url}")
    request = urllib.request.Request(url, headers={"User-Agent": f"{APP_ID}-installer"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, destination.open("wb") as output:
            total_header = response.headers.get("Content-Length")
            total = int(total_header) if total_header and total_header.isdigit() else expected_size
            done = 0
            last_report = 0
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
                done += len(chunk)
                if done - last_report >= 8 * 1024 * 1024:
                    last_report = done
                    if sys.stderr.isatty() and total:
                        percent = min(100, int(done * 100 / total))
                        print(f"\r  Downloading release: {percent:3d}% ({done // (1024*1024)} MiB)", end="", file=sys.stderr, flush=True)
            output.flush()
            os.fsync(output.fileno())
            if sys.stderr.isatty():
                print(file=sys.stderr)
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        fail(f"Download failed: {exc}")


def extract_release_archive(archive_path: Path, destination: Path) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    try:
        with tarfile.open(archive_path, "r:gz") as tar:
            seen: set[str] = set()
            root_candidates: set[str] = set()
            for member in tar.getmembers():
                raw = member.name
                path = PurePosixPath(raw)
                if path.is_absolute() or ".." in path.parts or not path.parts:
                    fail(f"Unsafe path in release archive: {raw!r}")
                if member.issym() or member.islnk() or member.isdev() or not (member.isdir() or member.isfile()):
                    fail(f"Unsupported link or special file in release archive: {raw!r}")
                if member.isdir():
                    if len(path.parts) == 1:
                        root_candidates.add(path.parts[0])
                    continue
                if len(path.parts) == 2:
                    root_name, filename = path.parts
                    root_candidates.add(root_name)
                    if root_name != "RobloxLinuxRelease":
                        fail(f"Unexpected top-level directory in release archive: {root_name!r}")
                elif len(path.parts) == 1:
                    filename = path.parts[0]
                else:
                    fail(f"Nested directories are not expected in the release bundle: {raw!r}")
                if filename not in PAYLOAD_FILES:
                    continue
                if filename in seen:
                    fail(f"Duplicate payload path in release archive: {filename}")
                seen.add(filename)
                source_stream = tar.extractfile(member)
                if source_stream is None:
                    fail(f"Could not read release payload member: {raw}")
                out = destination / filename
                with source_stream, out.open("xb") as stream:
                    shutil.copyfileobj(source_stream, stream, length=1024 * 1024)
                out.chmod(0o755 if filename in EXECUTABLE_FILES else 0o644)
            if root_candidates != {"RobloxLinuxRelease"}:
                fail("Release archive must contain exactly one top-level RobloxLinuxRelease directory.")
    except (tarfile.TarError, OSError) as exc:
        fail(f"Could not safely extract the release archive: {exc}")
    missing = sorted(REQUIRED_FILES - {p.name for p in destination.iterdir()})
    if missing:
        fail("Downloaded release is incomplete; missing: " + ", ".join(missing))
    return destination


def get_source_for_action(action: str, target: Path, source_arg: str | None,
                          dry_run: bool = False) -> tuple[Path | None, str | None, tempfile.TemporaryDirectory | None]:
    if source_arg:
        source = check_source_root(source_arg, target)
        safe_payload_names(source)
        return source, None, None
    script_dir = Path(__file__).resolve().parent
    if action == "install" and script_dir != target and source_looks_like_release(script_dir):
        source = check_source_root(script_dir, target)
        safe_payload_names(source)
        return source, None, None
    if dry_run:
        metadata = latest_release_metadata()
        note(f"Would download verified release {metadata['tag']} from GitHub.")
        return None, metadata["tag"], None
    metadata = latest_release_metadata()
    target.parent.mkdir(parents=True, exist_ok=True)
    free_bytes = shutil.disk_usage(target.parent).free
    estimated_peak = max(512 * 1024 * 1024, metadata["size"] * 2 + 64 * 1024 * 1024)
    if free_bytes < estimated_peak:
        fail(f"Not enough free space beside the install location. Need roughly {estimated_peak // (1024*1024)} MiB temporarily; only {free_bytes // (1024*1024)} MiB is free.")
    temp_obj = tempfile.TemporaryDirectory(prefix=f"{APP_ID}-download-", dir=target.parent)
    temp_root = Path(temp_obj.name)
    archive = temp_root / "RobloxLinuxRelease.tar.gz"
    print(f"Latest release: {_color('1', metadata['tag'])}")
    note(f"Downloading the official release from {REPOSITORY}…")
    download_file(metadata["url"], archive, metadata["size"])
    actual = sha256_file(archive)
    if actual != metadata["digest"]:
        temp_obj.cleanup()
        fail("SHA-256 verification failed for the downloaded release. The archive was rejected.")
    good("Release archive SHA-256 verified.")
    extracted = temp_root / "RobloxLinuxRelease"
    extract_release_archive(archive, extracted)
    archive.unlink()
    return extracted, metadata["tag"], temp_obj


def status(target: Path) -> None:
    exists = check_target_root(target)
    if not exists:
        print(f"{APP_NAME}: not installed at {target}")
        return
    manifest = load_manifest(target)
    if not manifest:
        print(f"Directory exists, but no installer-owned installation was found: {target}")
        print("No changes were made. Use a new --prefix or move the directory aside after backing it up.")
        return
    print(_color("1;36", APP_NAME))
    print(f"Install location : {target}")
    print(f"Installed version: {manifest.get('version', 'unknown')}")
    print(f"Managed files    : {len(manifest.get('files', {}))}")
    data_st = lstat_or_none(target / "DO_NOT_SHARE")
    if data_st is None:
        data_label = "not created yet"
    elif stat.S_ISDIR(data_st.st_mode) and not stat.S_ISLNK(data_st.st_mode):
        data_label = "present (preserved)"
    else:
        data_label = "unexpected path type (will not be followed or removed)"
    print(f"DO_NOT_SHARE     : {data_label}")
    desktop = load_desktop_state()
    print(f"Desktop entries  : {'installer-managed' if desktop and desktop.get('install_dir') == str(target) else 'not installer-managed'}")
    extras = [p.name for p in target.iterdir() if p.name not in set(manifest['files']) | {MANIFEST_NAME}]
    if extras:
        print("Other preserved entries: " + ", ".join(sorted(extras)[:12]) + (" …" if len(extras) > 12 else ""))


TITLE_LINES = (
    " ███████████   ██████   ██████ █████       ███████████",
    "▒▒███▒▒▒▒▒███ ▒▒██████ ██████ ▒▒███       ▒▒███▒▒▒▒▒███",
    " ▒███    ▒███  ▒███▒█████▒███  ▒███        ▒███    ▒███",
    " ▒██████████   ▒███▒▒███ ▒███  ▒███        ▒██████████",
    " ▒███▒▒▒▒▒███  ▒███ ▒▒▒  ▒███  ▒███        ▒███▒▒▒▒▒▒  ",
    " ▒███    ▒███  ▒███      ▒███  ▒███      █ ▒███        ",
    " █████   █████ █████     █████ ███████████ █████       ",
    "▒▒▒▒▒   ▒▒▒▒▒ ▒▒▒▒▒     ▒▒▒▒▒ ▒▒▒▒▒▒▒▒▒▒▒ ▒▒▒▒▒        ",
)


@dataclass
class InstallationState:
    target: Path
    manifest: dict[str, Any] | None
    latest_tag: str | None = None
    release_error: str | None = None

    @property
    def installed(self) -> bool:
        return self.manifest is not None

    @property
    def installed_version(self) -> str:
        if not self.manifest:
            return "not installed"
        return str(self.manifest.get("version") or "unknown")

    @property
    def update_available(self) -> bool:
        return self.installed and bool(self.latest_tag) and self.installed_version != self.latest_tag

    @property
    def desktop_managed(self) -> bool:
        try:
            desktop = load_desktop_state()
        except InstallerError:
            return False
        return bool(desktop and desktop.get("install_dir") == str(self.target))


def color_enabled() -> bool:
    return bool(
        sys.stdout.isatty()
        and sys.stderr.isatty()
        and not os.environ.get("NO_COLOR")
        and not os.environ.get("CI")
        and os.environ.get("TERM", "") != "dumb"
    )


def print_title(animated: bool = False) -> None:
    """Print the supplied static splash artwork."""
    del animated
    for line in TITLE_LINES:
        print(_color("1;31", line.rstrip()))


def print_banner() -> None:
    print_title(animated=False)
    print("-" * 54)


def detect_installation_state(target: Path, check_release: bool = True) -> InstallationState:
    manifest = load_manifest(target) if check_target_root(target) else None
    state = InstallationState(target=target, manifest=manifest)
    if check_release:
        try:
            state.latest_tag = latest_release_metadata()["tag"]
        except InstallerError as exc:
            state.release_error = str(exc)
    return state


def menu_choices(state: InstallationState, update_declined: bool = False) -> list[tuple[str, str]]:
    if not state.installed:
        return [("1", "Install"), ("2", "Exit")]
    if state.update_available or update_declined:
        return [("1", "Update"), ("2", "Uninstall"), ("3", "Exit")]
    return [("1", "Uninstall"), ("2", "Exit")]


def render_state(state: InstallationState) -> None:
    print(f"Target: {state.target}")
    if state.installed:
        print(f"Installed: {state.installed_version}")
        if state.latest_tag:
            print(f"Latest:    {state.latest_tag}")
        elif state.release_error:
            print("Latest:    unavailable (the update check failed)")
        data = state.target / "DO_NOT_SHARE"
        data_status = "present" if data.is_dir() and not data.is_symlink() else "not created"
        print(f"Data:      {data_status} (preserved by default)")
    else:
        print("Installation: not installed")
        if state.latest_tag:
            print(f"Release:      {state.latest_tag} ready to download")
        elif state.release_error:
            print("Release:      unavailable; install will retry the official release check")


def render_update_notice(state: InstallationState) -> None:
    if state.update_available:
        print()
        print(f"Update available: {state.installed_version} -> {state.latest_tag}")
        print("Install the latest release now?")


def print_menu(state: InstallationState, update_declined: bool = False) -> None:
    print()
    for number, label in menu_choices(state, update_declined):
        print(f"  [{number}] {label}")


def ask_desktop_integration() -> bool:
    return confirm(
        "Add Roblox to your application menu and open Roblox links automatically?",
        default=True,
    )


def interactive_install(target: Path, state: InstallationState, integrate_desktop: bool) -> None:
    check_platform()
    src_dir = Path(__file__).resolve().parent
    if not source_looks_like_release(src_dir) or src_dir == target:
        src, tag, temp_obj = get_source_for_action("install" if not state.installed else "update", target, None)
    else:
        src, tag, temp_obj = src_dir, None, None
    try:
        run_install_or_update(
            target,
            src,
            tag,
            confirmed=True,
            yes=True,
            install_desktop_integration=integrate_desktop,
        )
    finally:
        if temp_obj:
            temp_obj.cleanup()


def interactive_menu() -> int:
    target = normalized_target(default_prefix())
    update_declined = False
    first_screen = True
    while True:
        if first_screen:
            print_title(animated=True)
            first_screen = False
        else:
            print(_color("1;31", "ROBLOX MAC LINUX PORT"))
        print("-" * 54)
        try:
            state = detect_installation_state(target)
        except InstallerError as exc:
            warn(str(exc))
            state = InstallationState(target=target)
        render_state(state)
        if state.update_available and not update_declined:
            render_update_notice(state)
            if confirm("Update now?", default=True):
                try:
                    with installer_lock():
                        interactive_install(target, state, state.desktop_managed)
                except (InstallerError, KeyboardInterrupt) as exc:
                    warn(str(exc) or "Update cancelled.")
                update_declined = False
                continue
            update_declined = True
        print_menu(state, update_declined)
        try:
            choice = input("\nChoose an action: ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nBye!")
            return 0
        choices = dict(menu_choices(state, update_declined))
        if choice not in choices:
            warn("Choose one of the displayed options.")
            continue
        action = choices[choice]
        if action == "Exit":
            print("Bye!")
            return 0
        try:
            with installer_lock():
                if action == "Install":
                    integrate = ask_desktop_integration()
                    interactive_install(target, state, integrate)
                    print("\nInstallation complete. Goodbye!")
                    return 0
                elif action == "Update":
                    interactive_install(target, state, state.desktop_managed)
                    update_declined = False
                elif action == "Uninstall":
                    keep_data = confirm("Keep your Roblox data for a future reinstall?", default=True)
                    remove_install(target, yes=True, keep_data=keep_data)
                    # Do not reopen the install menu after a completed uninstall.
                    print("\nUninstall complete. Goodbye!")
                    return 0
        except InstallerError as exc:
            warn(str(exc))
        except KeyboardInterrupt:
            print("\nCancelled.")


def add_common_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--prefix", help=f"Installation directory (default: {default_prefix()})")
    parser.add_argument("--source", help="Use a local extracted release directory instead of downloading")
    parser.add_argument("--yes", action="store_true", help="Assume yes for confirmations; never bypasses file-safety checks")
    parser.add_argument("--dry-run", action="store_true", help="Show planned changes without modifying files")
    parser.add_argument("--force-desktop", action="store_true", help="Back up and replace conflicting desktop files/default URI handlers")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="installer.py",
        description="Safe per-user install, update, desktop integration, and uninstall for Roblox Mac Linux Port.",
        epilog="Examples:\n  python3 installer.py install\n  python3 installer.py update\n  python3 installer.py update --source ~/Downloads/RobloxLinuxRelease\n  python3 installer.py uninstall  # keeps DO_NOT_SHARE\n  python3 installer.py desktop install",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command")
    for command in ("install", "update", "repair"):
        p = sub.add_parser(command, help=f"{command.capitalize()} the application")
        add_common_options(p)
    p = sub.add_parser("uninstall", help="Remove managed application files; keep or purge app data explicitly")
    add_common_options(p)
    p.add_argument("--remove-client-cache", action="store_true", help="Also remove recognized RobloxVersion cache; never remove DO_NOT_SHARE")
    data_choice = p.add_mutually_exclusive_group()
    data_choice.add_argument("--keep-data", dest="keep_data", action="store_true", help="Keep DO_NOT_SHARE and RobloxVersion (default)")
    data_choice.add_argument("--purge-data", dest="keep_data", action="store_false", help="Remove app data inside the install root after confirmation")
    p.set_defaults(keep_data=True)
    p = sub.add_parser("status", help="Show install and desktop-integration status")
    p.add_argument("--prefix", help=f"Installation directory (default: {default_prefix()})")
    p = sub.add_parser("desktop", help="Manage the user-local .desktop launcher and roblox:// URI handler")
    p.add_argument("action", choices=("install", "uninstall"))
    p.add_argument("--prefix", help=f"Installation directory (default: {default_prefix()})")
    p.add_argument("--yes", action="store_true", help="Assume yes for confirmations")
    p.add_argument("--dry-run", action="store_true", help="Show planned changes without modifying files")
    p.add_argument("--force-desktop", action="store_true", help="Back up and replace conflicting desktop files/default URI handlers")
    return parser


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv and sys.stdin.isatty():
        return interactive_menu()
    parser = build_parser()
    args = parser.parse_args(argv)
    if not args.command:
        parser.print_help()
        return 0
    if os.geteuid() == 0:
        fail("Do not run this per-user installer with sudo/root. Run it as your normal desktop user.")
    target = normalized_target(args.prefix or default_prefix())
    with installer_lock():
        if args.command == "status":
            status(target)
            return 0
        if args.command == "desktop":
            if args.action == "install":
                if not target.is_dir():
                    fail(f"Application is not installed at {target}; install it first.")
                if args.dry_run:
                    install_desktop(target, force=args.force_desktop, yes=args.yes, dry_run=True)
                elif args.yes or args.force_desktop:
                    install_desktop(target, force=args.force_desktop, yes=args.yes)
                elif not sys.stdin.isatty():
                    fail("Desktop integration changes files. Re-run with --yes (and --force-desktop only if replacing conflicts).")
                elif confirm("Install desktop launcher and URI handler?", default=True):
                    install_desktop(target, force=args.force_desktop, yes=args.yes)
                else:
                    note("Cancelled; no changes were made.")
            else:
                if args.dry_run:
                    uninstall_desktop(dry_run=True, expected_target=target)
                elif args.yes:
                    uninstall_desktop(expected_target=target)
                elif not sys.stdin.isatty():
                    fail("Desktop uninstallation changes files. Re-run with --yes to confirm.")
                elif confirm("Remove this install's desktop launcher and URI handler?", default=False):
                    uninstall_desktop(expected_target=target)
                else:
                    note("Cancelled; no changes were made.")
            return 0
        if args.command == "uninstall":
            remove_install(
                target,
                yes=args.yes,
                dry_run=args.dry_run,
                remove_client_cache=args.remove_client_cache,
                keep_data=args.keep_data,
            )
            return 0
        if args.command in {"install", "update", "repair"}:
            if args.dry_run and args.source:
                check_platform()
                source = check_source_root(args.source, target)
                run_install_or_update(target, source, None, dry_run=True, yes=args.yes)
                return 0
            confirmed = args.yes
            if not args.yes and not args.dry_run:
                if not sys.stdin.isatty():
                    fail("This operation changes files. Re-run with --yes for a non-interactive install/update.")
                action_label = "download and install the latest release" if args.command in {"update", "repair"} and not args.source else f"{args.command} from the selected release"
                if not confirm(f"Proceed to {action_label} at {target}?", default=False):
                    note("Cancelled; nothing was changed.")
                    return 0
                confirmed = True
            check_platform()
            if not args.dry_run:
                target.parent.mkdir(parents=True, exist_ok=True)
            source, tag, temp_obj = get_source_for_action(args.command, target, args.source, args.dry_run)
            if args.dry_run and source is None:
                print(f"Would update {APP_NAME} to {tag} at {target}; no download or file changes performed.")
                return 0
            try:
                run_install_or_update(target, source, tag, dry_run=args.dry_run, yes=args.yes,
                                      force_desktop=args.force_desktop, confirmed=confirmed)
            finally:
                if temp_obj:
                    temp_obj.cleanup()
            return 0
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except InstallerError as exc:
        warn(str(exc))
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        sys.exit(130)
