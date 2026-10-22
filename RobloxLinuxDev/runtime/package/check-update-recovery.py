#!/usr/bin/env python3
"""Offline checks for crash-safe Roblox client updates and startup recovery.

Static assertions inspect the updater/launcher sources. The runtime cases run the
real scripts against a disposable release folder and a mock AppImage, so no
network, Darling runtime or real client is needed:

  1. normal update end-to-end
  2. crash between the two swap renames (SIGKILL) + launcher recovery
  2b. the same interrupted state constructed as a fixture + launcher recovery
  3. crash after download but before the commit marker
  4. incomplete staged client must never be installed (roll back / fail loudly)
  5. recovery reuses the staged client without any download
  6. a normal update still works after a recovery
  7. DO_NOT_SHARE stays byte-for-byte untouched in every case
  8. crash after the install rename but before cleanup: keep the new client
  9. crash after the marker but before the swap: keep the old client (rollback)
  10. an invalid installed client is restored from the validated backup
  11. an invalid installed client is replaced by the validated staged client
  12. nothing valid anywhere: fail loudly, keep evidence, stay idempotent
  13. recovery reruns are no-ops after a successful recovery
  14. the updater refuses an invalid install without deleting the backup
  15. a broken fsync helper degrades to install-now-clean-later
  16. a staged regular-file open failure aborts before the commit marker and swap
  16b. a staged regular-file fsync failure aborts before the commit marker and swap
  16c. staged bundle symlinks are handled deliberately and persisted without error
"""
import fcntl
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PKG = ROOT / "package"
UPDATE = (PKG / "update-roblox.sh").read_text()
BOOTSTRAP = (PKG / "bootstrap.sh").read_text()
RUN = (PKG / "run.sh").read_text()

# --- Static checks --------------------------------------------------------------------------
# Staging is a fixed, predictable directory beside the client, never a random
# scratch dir inside DO_NOT_SHARE.
assert 'mktemp -d "$DATA/.update' not in UPDATE
assert "WORK=$HERE/.update-work" in UPDATE
assert "WORK=$HERE/.update-work" in BOOTSTRAP and "WORK=$HERE/.update-work" in RUN
assert "recover_interrupted_update" in BOOTSTRAP and "recover_interrupted_update" in RUN
# A durable commit marker is written and fsynced before the swap renames.
assert "TXN" in UPDATE and "os.fsync" in UPDATE
# The backup must not be deleted until the client is confirmed valid.
assert 'rm -rf -- "$WORK"' in UPDATE
# Same-filesystem guard for the atomic rename.
assert "stat -c %d" in UPDATE
# Traps keep the standard exit codes.
assert "trap 'exit 130' INT" in UPDATE and "trap 'exit 143' TERM HUP" in UPDATE
# FD 8 is the bootstrapper's progress channel. The updater lock lives on FD 7,
# and both lock descriptors are closed before an AppImage-spawning child runs.
assert 'exec 7<"$HERE"' in UPDATE
assert 'flock -n -E 74 7' in UPDATE
assert 'exec 8<"$HERE"' not in UPDATE
assert '--client-version 7>&- 8>&- 9>&-' in UPDATE
assert '--download-client 7>&- 9>&-' in UPDATE
assert '--prepare-shaders 7>&- 8>&- 9>&-' in UPDATE
# The updater stages inside the launcher-managed work directory; the runtime's
# release confinement accepts ROBLOX_MAC_DATA there by canonical path prefix,
# so staging must not masquerade as a live-launch alias data directory.
assert 'ROBLOX_MAC_ALIAS_DIR' not in UPDATE
assert 'ROBLOX_MAC_DATA="$WORK/data"' in UPDATE
assert 'ROBLOX_MAC_DATA="$WORK/next/runtime"' in UPDATE
# bootstrap.sh and run.sh must carry byte-identical recovery implementations,
# so the two entry points cannot drift into different transaction semantics.
MARK_BEGIN = "# >>> update transaction recovery"
MARK_END = "# <<< update transaction recovery <<<"

def recovery_block(text):
    start = text.index(MARK_BEGIN)
    return text[start:text.index(MARK_END, start) + len(MARK_END)]

assert recovery_block(BOOTSTRAP) == recovery_block(RUN), \
    "bootstrap.sh and run.sh recovery blocks diverged"
# The updater must sync the staged data, then write the durable marker, then
# perform the two renames — each followed by a parent-directory sync.
_sync_tree = UPDATE.index('sync_tree "$WORK/next"')
_marker = UPDATE.index('> "$WORK/TXN"')
_rename1 = UPDATE.index('mv "$HERE/RobloxVersion" "$WORK/previous"')
_rename2 = UPDATE.index('mv "$WORK/next" "$HERE/RobloxVersion"')
assert _sync_tree < _marker < _rename1 < _rename2
assert 'fsync_dir "$HERE" "$WORK"' in UPDATE[_rename1:_rename2], \
    "rename 1 is not followed by a parent-directory sync"
assert 'fsync_dir "$HERE" "$WORK"' in UPDATE[_rename2:], \
    "rename 2 is not followed by a parent-directory sync"
assert 'os.path.dirname(os.path.dirname' in UPDATE, \
    "the marker sync does not cover the release directory entry"
# The updater must refuse any interrupted state it cannot resolve itself,
# before its staging cleanup could delete recovery evidence.
assert UPDATE.index("interrupted update is pending") < UPDATE.index('rm -rf -- "$WORK"')

OLD = "version-old"
NEW = "version-abcd"
NEW2 = "version-bcde"
SESSION = {
    "cookies/native.sqlite": b"test native session",
    "browser/cookies.sqlite": b"test browser session",
    "browser/data/localstorage/test": b"test browser storage",
    "prefix/Users/root/Library/Preferences/test.plist": b"test client preferences",
    "home/.config/user-settings": b"keep me",
}
APPIMAGE = """#!/bin/sh
set -eu
# Stand-in for the real AppImage runtime's FUSE daemon: a background child
# that outlives its command and keeps every inherited descriptor open, as
# squashfuse does when uruntime keeps a mount for reuse. The updater and the
# bootstrapper must close their lock descriptors for AppImage-spawning
# children (7>&- 9>&-), or this sleeper strands the instance lock and the
# update lock. FD 8 remains open only for the download progress channel.
# Output is redirected so it never keeps the harness's capture
# pipes open.
sleep 30 >/dev/null 2>&1 </dev/null &
# run.sh must hand every AppImage invocation a mount directory outside the
# release folder, and so outside DO_NOT_SHARE: a FUSE mount below the data
# directory makes the launch-time bind-mount fail with EINVAL.
[ -n "${APPIMAGE_TARGET_DIR:-}" ] && [ -d "$APPIMAGE_TARGET_DIR" ] || exit 9
case "$APPIMAGE_TARGET_DIR" in "$(dirname -- "$0")"/*) exit 9;; esac
version=${TEST_CLIENT_VERSION:-version-abcd}
[ "${1:-}" != --debug ] || shift
case "${1:-}" in
--client-version) test ! -e /proc/$$/fd/7; test ! -e /proc/$$/fd/9; printf '%s\\n' "$version";;
--client-version-display) printf '0.740.0.7400927\\n';;
--download-client)
    test ! -e /proc/$$/fd/7; test ! -e /proc/$$/fd/9
    base="$ROBLOX_MAC_DATA"
    mkdir -p "$base/RobloxPlayer.app/Contents/MacOS"
    printf '#!/bin/sh\\nexit 0\\n' > "$base/RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
    chmod +x "$base/RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
    printf '%s\\n' "$version" > "$base/.version";;
--prepare-shaders)
    test ! -e /proc/$$/fd/7; test ! -e /proc/$$/fd/8; test ! -e /proc/$$/fd/9
    cache="$(dirname "$ROBLOX_MAC_APP")/spv-cache-v1"
    mkdir -p "$cache"; printf '{}\\n' > "$cache/report.json";;
--shell) mkdir -p "$(dirname -- "$0")/DO_NOT_SHARE/prefix";;
*) printf 'launched\\n' > "$(dirname -- "$0")/launched";;
esac
"""


def clean_env(extra=None):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("ROBLOX_", "APPIMAGE", "APPDIR", "TRACKA_"))}
    if extra:
        env.update(extra)
    return env


def write_client(root, version, sentinel=None):
    (root / "RobloxPlayer.app/Contents/MacOS").mkdir(parents=True, exist_ok=True)
    player = root / "RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
    player.write_text("#!/bin/sh\nexit 0\n")
    player.chmod(0o755)
    (root / ".version").write_text(version + "\n")
    (root / "spv-cache-v1").mkdir(parents=True, exist_ok=True)
    (root / "spv-cache-v1/report.json").write_text("{}\n")
    if sentinel:
        (root / sentinel).write_text("staged\n")


def snapshot(private):
    out = {}
    for path in private.rglob("*"):
        if not path.is_file():
            continue
        rel = str(path.relative_to(private))
        if rel == "instance.lock" or rel.startswith("tmp/") or path.name.startswith((".check.", ".bootstrap-")):
            continue
        out[rel] = path.read_bytes()
    return out


def make_release(tmp: Path):
    release = tmp / "release"
    release.mkdir(parents=True)
    for name in ("update-roblox.sh", "run.sh", "bootstrap.sh", "bootstrap.py"):
        shutil.copy2(PKG / name, release / name)
    (release / "RobloxLinux.AppImage").write_text(APPIMAGE)
    (release / "RobloxLinux.AppImage").chmod(0o755)
    private = release / "DO_NOT_SHARE"
    (private / "tmp").mkdir(parents=True)
    (private / "prefix").mkdir(parents=True)
    for name, data in SESSION.items():
        path = private / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    # Skip first-run warm-up and keep the launch from writing to DO_NOT_SHARE:
    # a far-future last_check makes the update check not due, so a launch only
    # recovers, reads settings and starts the (mock) client.
    (private / "first-run.initialized").write_text("initialized\n")
    (private / "bootstrapper.json").write_text(
        '{"auto_update":"off","check_interval_hours":24,"last_check":999999999999}')
    write_client(release / "RobloxVersion", OLD)
    return release, private


def update(release, env=None):
    return subprocess.run(["sh", str(release / "update-roblox.sh")],
                          env=clean_env(env), capture_output=True, text=True)


def launch(release, env=None):
    return subprocess.run(["sh", str(release / "run.sh")], env=clean_env(env),
                          capture_output=True, text=True)


def assert_intact(private, before, label):
    assert snapshot(private) == before, f"{label}: DO_NOT_SHARE changed"
    assert not list(private.glob(".update.*")), f"{label}: staging leaked into DO_NOT_SHARE"


def lock_free(private):
    """True when nothing still holds the instance lock (a stranded FD would fail)."""
    lock = private / "instance.lock"
    if not lock.exists():
        return True
    fd = os.open(lock, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
        return True
    except OSError:
        return False
    finally:
        os.close(fd)


with tempfile.TemporaryDirectory(prefix="roblox recovery ") as tmpdir:
    tmp = Path(tmpdir)

    # 1. Normal update end-to-end. -----------------------------------------------------------
    release, private = make_release(tmp / "s1")
    before = snapshot(private)
    result = update(release)
    assert result.returncode == 0, result.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == NEW
    assert (release / "RobloxVersion/RobloxPlayer.app/Contents/MacOS/RobloxPlayer").stat().st_mode & 0o111
    assert not (release / ".update-work").exists(), "work dir not cleaned"
    assert lock_free(private), "S1: the update stranded the instance lock"
    assert_intact(private, before, "S1")
    assert "already up to date" in update(release).stdout
    print("  PASS 1: normal update installs the client and cleans staging.")

    # 2. Real crash between rename #1 and #2, then launcher recovery. ------------------------
    release, private = make_release(tmp / "s2")
    before = snapshot(private)
    crashed = update(release, {"ROBLOX_MAC_TEST_CRASH_AFTER_RENAME1": "1"})
    assert crashed.returncode == -9, f"S2: expected SIGKILL, got {crashed.returncode}"
    work = release / ".update-work"
    assert (work / "TXN").exists(), "S2: commit marker missing after crash"
    assert (work / "previous").is_dir(), "S2: backup missing after crash"
    assert (work / "next").is_dir(), "S2: staged client missing after crash"
    assert not (release / "RobloxVersion").exists(), "S2: swap unexpectedly completed"
    assert_intact(private, before, "S2 (interrupted)")
    recovered = launch(release)
    assert recovered.returncode == 0, recovered.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == NEW
    assert not work.exists(), "S2: work dir not cleaned after recovery"
    assert "recovered an interrupted update" in recovered.stderr
    assert_intact(private, before, "S2 (recovered)")
    print("  PASS 2: SIGKILL between renames is recovered on next launch (forward-complete).")

    # 2b. Same interrupted state built as a fixture (recovery independent of the writer). ----
    release, private = make_release(tmp / "s2b")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    write_client(work / "next", NEW)
    write_client(work / "previous", OLD)
    shutil.rmtree(release / "RobloxVersion")
    recovered = launch(release)
    assert recovered.returncode == 0, recovered.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == NEW
    assert not work.exists()
    assert_intact(private, before, "S2b")
    print("  PASS 2b: fixture-built interrupted state is recovered by the launcher.")

    # 3. Crash after download but before the commit marker: staging is stale. ----------------
    release, private = make_release(tmp / "s3")
    before = snapshot(private)
    work = release / ".update-work"
    (work / "next").mkdir(parents=True)
    (work / "next/partial").write_text("half a download\n")  # no marker, no usable client
    result = launch(release)
    assert result.returncode == 0, result.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == OLD, "S3: client changed"
    assert not work.exists(), "S3: stale staging not removed"
    assert_intact(private, before, "S3")
    print("  PASS 3: pre-commit staging is discarded without touching the client.")

    # 4a. Incomplete staged client + usable backup: roll back, never install broken. ----------
    release, private = make_release(tmp / "s4a")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    (work / "next/RobloxPlayer.app/Contents/MacOS").mkdir(parents=True)
    (work / "next/.version").write_text(NEW + "\n")  # missing report.json -> invalid
    write_client(work / "previous", OLD)
    shutil.rmtree(release / "RobloxVersion")
    recovered = launch(release)
    assert recovered.returncode == 0, recovered.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == OLD, "S4a: installed a broken client"
    assert not work.exists()
    assert "restored the previous client" in recovered.stderr
    assert_intact(private, before, "S4a")
    print("  PASS 4a: an incomplete staged client is not installed; the backup is restored.")

    # 4b. Incomplete staged client, no backup: fail loudly, keep the state. ------------------
    release, private = make_release(tmp / "s4b")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    (work / "next").mkdir()  # invalid: no app, no report
    shutil.rmtree(release / "RobloxVersion")
    recovered = launch(release)
    assert recovered.returncode != 0, "S4b: recovery should fail loudly"
    assert not (release / "RobloxVersion").exists(), "S4b: a broken client was installed"
    assert (work / "TXN").exists(), "S4b: recovery data was discarded on failure"
    assert lock_free(private), "S4b: a failed recovery stranded the instance lock"
    assert_intact(private, before, "S4b")
    print("  PASS 4b: with nothing to restore, recovery fails loudly and keeps its data.")

    # 5. Recovery reuses the staged client with no download even if network is off. ----------
    release, private = make_release(tmp / "s5")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    write_client(work / "next", NEW, sentinel=".staged-by-download")
    write_client(work / "previous", OLD)
    shutil.rmtree(release / "RobloxVersion")
    fakebin = tmp / "s5-bin"
    fakebin.mkdir()
    curl_calls = tmp / "s5-curl.calls"
    (fakebin / "curl").write_text(f"#!/bin/sh\necho called >> '{curl_calls}'\nexit 1\n")
    (fakebin / "curl").chmod(0o755)
    recovered = launch(release, {"PATH": str(fakebin) + ":" + os.environ["PATH"]})
    assert recovered.returncode == 0, recovered.stderr
    assert (release / "RobloxVersion/.staged-by-download").exists(), "S5: staged client was not reused"
    assert (release / "RobloxVersion/.version").read_text().strip() == NEW
    assert not curl_calls.exists(), "S5: recovery re-downloaded instead of reusing staging"
    assert not work.exists()
    assert_intact(private, before, "S5")
    print("  PASS 5: recovery reuses the staged client without any download.")

    # 6. A normal update still works after a recovery. ---------------------------------------
    release, private = make_release(tmp / "s6")
    before = snapshot(private)
    crashed = update(release, {"ROBLOX_MAC_TEST_CRASH_AFTER_RENAME1": "1"})
    assert crashed.returncode == -9
    assert launch(release).returncode == 0
    assert (release / "RobloxVersion/.version").read_text().strip() == NEW
    again = update(release, {"TEST_CLIENT_VERSION": NEW2})
    assert again.returncode == 0, again.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == NEW2
    assert not (release / ".update-work").exists()
    assert lock_free(private), "S6: an update stranded the instance lock"
    assert_intact(private, before, "S6")
    print("  PASS 6: a normal update still works after a recovery.")

    # 6b. Manual update on an ambiguous interrupted state refuses and keeps the backup. ------
    release, private = make_release(tmp / "s6b")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    write_client(work / "previous", OLD)
    shutil.rmtree(release / "RobloxVersion")
    refused = update(release)
    assert refused.returncode != 0, "S6b: manual update should refuse an interrupted state"
    assert "interrupted update is pending" in refused.stderr
    assert (work / "previous").is_dir(), "S6b: the backup was deleted"
    assert_intact(private, before, "S6b")
    print("  PASS 6b: a manual update refuses an ambiguous state without deleting the backup.")

    # 7. A missing client with only stale, uncommitted staging is a normal first run. --------
    release, private = make_release(tmp / "s7")
    before = snapshot(private)
    (release / ".update-work/next").mkdir(parents=True)
    (release / ".update-work/next/partial").write_text("half a download\n")  # no marker
    shutil.rmtree(release / "RobloxVersion")
    first = launch(release)
    assert first.returncode == 0, (first.returncode, first.stderr)
    assert "no staged update to restore" not in first.stderr
    assert (release / "launched").exists(), "S7: launcher did not reach its download/check path"
    assert not (release / ".update-work").exists(), "S7: stale staging not removed"
    assert lock_free(private), "S7: the launch stranded the instance lock"
    assert_intact(private, before, "S7")
    print("  PASS 7: a missing client with stale staging continues to the download/check path.")

    # 8. Crash after the install rename but before cleanup: keep the new client.
    release, private = make_release(tmp / "s8")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    write_client(work / "previous", OLD)
    write_client(release / "RobloxVersion", NEW, sentinel=".installed")  # rename #2 done
    recovered = launch(release)
    assert recovered.returncode == 0, recovered.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == NEW
    assert (release / "RobloxVersion/.installed").exists(), "S8: the installed client was replaced"
    assert not work.exists(), "S8: work dir not cleaned"
    assert "kept the verified installed client" in recovered.stderr
    assert_intact(private, before, "S8")
    print("  PASS 8: a crash after the install rename keeps the new client.")

    # 9. Crash after the marker but before the first rename: roll back. -------
    release, private = make_release(tmp / "s9")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    write_client(work / "next", NEW, sentinel=".staged")  # verified, not swapped in
    recovered = launch(release)
    assert recovered.returncode == 0, recovered.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == OLD, "S9: rollback lost the client"
    assert not (release / "RobloxVersion/.staged").exists(), "S9: the staged client was installed"
    assert not work.exists(), "S9: work dir not cleaned"
    assert "kept the verified installed client" in recovered.stderr
    assert_intact(private, before, "S9")
    print("  PASS 9: a crash before the swap keeps the original client (rollback).")

    # 10. Marker + installed client that fails validation + valid backup. ----
    #     Recovery no longer requires RobloxVersion to be absent to restore:
    #     an unusable install is replaced by the validated backup.
    release, private = make_release(tmp / "s10")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    write_client(work / "previous", OLD)
    (release / "RobloxVersion/spv-cache-v1/report.json").unlink()  # invalid install
    recovered = launch(release)
    assert recovered.returncode == 0, recovered.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == OLD
    assert (release / "RobloxVersion/spv-cache-v1/report.json").exists(), "S10: backup not restored"
    assert not (release / "RobloxVersion/previous").exists(), "S10: backup nested into the client dir"
    assert not work.exists(), "S10: work dir not cleaned"
    assert "restored the previous client" in recovered.stderr
    assert_intact(private, before, "S10")
    print("  PASS 10: an invalid installed client is restored from the validated backup.")

    # 10b. Same, with a non-executable player binary in the installed client.
    release, private = make_release(tmp / "s10b")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    write_client(work / "previous", OLD)
    (release / "RobloxVersion/RobloxPlayer.app/Contents/MacOS/RobloxPlayer").chmod(0o644)
    recovered = launch(release)
    assert recovered.returncode == 0, recovered.stderr
    player = release / "RobloxVersion/RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
    assert player.stat().st_mode & 0o111, "S10b: backup not restored"
    assert (release / "RobloxVersion/.version").read_text().strip() == OLD
    assert not work.exists()
    assert "restored the previous client" in recovered.stderr
    assert_intact(private, before, "S10b")
    print("  PASS 10b: a non-executable installed client is restored from the backup.")

    # 11. Invalid installed client with a valid staged client: complete. -----
    release, private = make_release(tmp / "s11")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    write_client(work / "next", NEW)
    (release / "RobloxVersion/.version").unlink()  # invalid install
    recovered = launch(release)
    assert recovered.returncode == 0, recovered.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == NEW
    assert not (release / "RobloxVersion/next").exists(), "S11: staged client nested into the client dir"
    assert not work.exists()
    assert "completed the downloaded client" in recovered.stderr
    assert_intact(private, before, "S11")
    print("  PASS 11: an invalid installed client is replaced by the staged client.")

    # 12. Nothing valid anywhere: fail loudly, keep evidence, stay idempotent.
    release, private = make_release(tmp / "s12")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    write_client(work / "next", NEW)
    (work / "next/.version").unlink()  # invalid staged client
    write_client(work / "previous", OLD)
    (work / "previous/RobloxPlayer.app/Contents/MacOS/RobloxPlayer").chmod(0o644)  # invalid backup
    (release / "RobloxVersion/spv-cache-v1/report.json").unlink()  # invalid install
    for attempt in ("first", "second"):
        failed = launch(release)
        assert failed.returncode != 0, f"S12: the {attempt} recovery should fail loudly"
        assert (work / "TXN").exists() and (work / "next").is_dir() and (work / "previous").is_dir(), \
            f"S12: the {attempt} recovery discarded its evidence"
        assert (release / "RobloxVersion").is_dir(), f"S12: the {attempt} recovery removed the unusable client"
        assert "left no usable client" in failed.stderr
        assert lock_free(private), f"S12: the {attempt} failed recovery stranded the lock"
        assert_intact(private, before, f"S12 ({attempt})")
    print("  PASS 12: with nothing valid, recovery fails loudly and keeps its evidence (idempotent).")

    # 13. Running recovery again after a successful recovery is a no-op. -----
    release, private = make_release(tmp / "s13")
    before = snapshot(private)
    crashed = update(release, {"ROBLOX_MAC_TEST_CRASH_AFTER_RENAME1": "1"})
    assert crashed.returncode == -9
    first = launch(release)
    assert first.returncode == 0, first.stderr
    assert "recovered an interrupted update" in first.stderr
    second = launch(release)
    assert second.returncode == 0, second.stderr
    assert "recovered an interrupted update" not in second.stderr, "S13: recovery ran again on clean state"
    assert (release / "RobloxVersion/.version").read_text().strip() == NEW
    assert not (release / ".update-work").exists()
    assert_intact(private, before, "S13")
    print("  PASS 13: re-running recovery after a successful recovery is a no-op.")

    # 14. The updater refuses an invalid installed client with a valid backup.
    release, private = make_release(tmp / "s14")
    before = snapshot(private)
    work = release / ".update-work"
    work.mkdir()
    (work / "TXN").write_text(f"committed {NEW} 1\n")
    write_client(work / "previous", OLD)
    (release / "RobloxVersion/.version").unlink()  # invalid install
    refused = update(release)
    assert refused.returncode != 0, "S14: the updater should refuse an unresolved state"
    assert "interrupted update is pending" in refused.stderr
    assert (work / "previous").is_dir() and (work / "TXN").exists(), "S14: the updater deleted the backup"
    assert not (release / "RobloxVersion/.version").exists(), "S14: the updater touched the client"
    assert lock_free(private), "S14: the refused update stranded the lock"
    assert_intact(private, before, "S14")
    print("  PASS 14: the updater refuses an invalid install without deleting the backup.")

    # 15. A broken fsync helper: install now, keep the state, clean up later.
    release, private = make_release(tmp / "s15")
    before = snapshot(private)
    crashed = update(release, {"ROBLOX_MAC_TEST_CRASH_AFTER_RENAME1": "1"})
    assert crashed.returncode == -9
    fakebin = tmp / "s15-bin"
    fakebin.mkdir()
    (fakebin / "python3").write_text("#!/bin/sh\nexit 1\n")
    (fakebin / "python3").chmod(0o755)
    degraded = launch(release, {"PATH": str(fakebin) + ":" + os.environ["PATH"],
                                "DISPLAY": "", "WAYLAND_DISPLAY": ""})
    assert degraded.returncode == 0, degraded.stderr
    assert (release / "RobloxVersion/.version").read_text().strip() == NEW, "S15: client not recovered"
    assert (release / "launched").exists(), "S15: the recovered client did not launch"
    assert (release / ".update-work" / "TXN").exists(), "S15: state dropped without a durable install"
    assert "could not sync the release directory" in degraded.stderr
    assert lock_free(private), "S15: the degraded launch stranded the lock"
    assert_intact(private, before, "S15")
    fixed = launch(release)
    assert fixed.returncode == 0, fixed.stderr
    assert not (release / ".update-work").exists(), "S15: the retry did not finish the cleanup"
    assert "kept the verified installed client" in fixed.stderr
    assert_intact(private, before, "S15 (fixed)")
    print("  PASS 15: without a working fsync helper the install survives and the cleanup is retried.")

    # 16. A staged regular-file open failure aborts before the commit marker and swap.
    release, private = make_release(tmp / "s16")
    before = snapshot(private)
    fake_site = tmp / "s16-site"
    fake_site.mkdir()
    (fake_site / "sitecustomize.py").write_text("""import errno, os
orig_open = os.open
def mock_open(path, flags, *args, **kwargs):
    if str(path).endswith("/next/.version"):
        raise OSError(errno.EIO, "Simulated open failure", str(path))
    return orig_open(path, flags, *args, **kwargs)
os.open = mock_open
""")
    failed = update(release, {"PYTHONPATH": str(fake_site)})
    assert failed.returncode != 0, "S16: update should fail on staged file sync error"
    assert not (release / ".update-work" / "TXN").exists(), "S16: commit marker written despite sync failure"
    assert not (release / ".update-work" / "previous").exists(), "S16: backup created despite sync failure"
    assert (release / "RobloxVersion/.version").read_text().strip() == OLD, "S16: client was swapped despite sync failure"
    assert "could not open staged file" in failed.stderr, f"S16: unexpected stderr: {failed.stderr}"
    assert lock_free(private), "S16: failed sync stranded the instance lock"
    assert_intact(private, before, "S16")
    print("  PASS 16: a staged regular-file open failure aborts before the commit marker and swap.")

    # 16b. A staged regular-file fsync failure aborts before the commit marker and swap.
    release, private = make_release(tmp / "s16b")
    before = snapshot(private)
    fake_site = tmp / "s16b-site"
    fake_site.mkdir()
    (fake_site / "sitecustomize.py").write_text("""import errno, os
orig_fsync = os.fsync
def mock_fsync(fd):
    link = None
    try:
        link = os.readlink(f"/proc/self/fd/{fd}")
    except OSError:
        pass
    if link and "/next/" in link:
        raise OSError(errno.EIO, "Simulated fsync failure")
    return orig_fsync(fd)
os.fsync = mock_fsync
""")
    failed = update(release, {"PYTHONPATH": str(fake_site)})
    assert failed.returncode != 0, "S16b: update should fail on staged file fsync error"
    assert not (release / ".update-work" / "TXN").exists(), "S16b: commit marker written despite sync failure"
    assert not (release / ".update-work" / "previous").exists(), "S16b: backup created despite sync failure"
    assert (release / "RobloxVersion/.version").read_text().strip() == OLD, "S16b: client was swapped despite sync failure"
    assert "could not sync staged file" in failed.stderr, f"S16b: unexpected stderr: {failed.stderr}"
    assert lock_free(private), "S16b: failed sync stranded the instance lock"
    assert_intact(private, before, "S16b")
    print("  PASS 16b: a staged regular-file fsync failure aborts before the commit marker and swap.")

    # 16c. Staged bundle symlinks are handled deliberately and persisted without error.
    release, private = make_release(tmp / "s16c")
    before = snapshot(private)
    appimage = (release / "RobloxLinux.AppImage").read_text()
    marker = 'printf \'%s\\n\' "$version" > "$base/.version"'
    extra_symlinks = (
        marker + "\n"
        '    ln -s RobloxPlayer "$base/RobloxPlayer.app/Contents/MacOS/symlink-bin"\n'
        '    ln -s nonexistent "$base/RobloxPlayer.app/Contents/broken-link"\n'
        '    mkdir -p "$base/RobloxPlayer.app/Contents/Frameworks/A"\n'
        '    ln -s A "$base/RobloxPlayer.app/Contents/Frameworks/Current"'
    )
    assert marker in appimage
    (release / "RobloxLinux.AppImage").write_text(appimage.replace(marker, extra_symlinks, 1))
    result = update(release)
    assert result.returncode == 0, result.stderr
    installed = release / "RobloxVersion"
    assert (installed / ".version").read_text().strip() == NEW
    assert (installed / "RobloxPlayer.app/Contents/MacOS/symlink-bin").is_symlink()
    assert (installed / "RobloxPlayer.app/Contents/broken-link").is_symlink()
    assert (installed / "RobloxPlayer.app/Contents/Frameworks/Current").is_symlink()
    assert lock_free(private), "S16c: update stranded the instance lock"
    assert_intact(private, before, "S16c")
    print("  PASS 16c: staged bundle symlinks are handled deliberately and persisted without error.")

print("PASS update crash-safety: fixed staging, durable marker, startup recovery, "
      "rollback, staged-client reuse, full recovery-state matrix, DO_NOT_SHARE preservation")
