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
version=${TEST_CLIENT_VERSION:-version-abcd}
[ "${1:-}" != --debug ] || shift
case "${1:-}" in
--client-version) printf '%s\\n' "$version";;
--client-version-display) printf '0.740.0.7400927\\n';;
--download-client)
    base="$ROBLOX_MAC_DATA"
    mkdir -p "$base/RobloxPlayer.app/Contents/MacOS"
    printf '#!/bin/sh\\nexit 0\\n' > "$base/RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
    chmod +x "$base/RobloxPlayer.app/Contents/MacOS/RobloxPlayer"
    printf '%s\\n' "$version" > "$base/.version";;
--prepare-shaders)
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

print("PASS update crash-safety: fixed staging, durable marker, startup recovery, "
      "rollback, staged-client reuse, DO_NOT_SHARE preservation")