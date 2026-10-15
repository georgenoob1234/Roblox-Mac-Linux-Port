#!/bin/sh
# Runtime validation for the client-update fix (branch fix/update-regression).
#
# Run this on the machine that has the rebuilt release, with normal GPU and
# network access (not inside a container that blocks unshare/mount):
#
#   sh RobloxLinuxDev/runtime/package/validate-update-runtime.sh <release-folder>
#
# <release-folder> is the rebuilt release: the folder holding update-roblox.sh,
# run.sh, bootstrap.sh, bootstrap.py, FFlags.json and RobloxLinux.AppImage.
# The script never writes inside it: every test runs in a disposable copy under
# $TMPDIR with a fresh DO_NOT_SHARE, so the real login/profile data is
# untouched.
#
# What it exercises (real network download, real shader preparation, real
# launcher and recovery):
#   A. Missing RobloxVersion plus stale uncommitted staging: update-roblox.sh
#      must download the client, prepare its shaders and install it.
#   B. SIGKILL between the two swap renames (the crash window): run.sh must
#      recover by completing the staged client and then launch the client.
#   C. Interrupted update whose staged client is unusable but whose backup is
#      valid: recovery must restore the previous client (headless).
#
# The Roblox client window opens during B and is closed automatically after
# VALIDATE_LAUNCH_TIMEOUT seconds (default 300). B's relaunch includes the
# first-run darling setup, which is given ROBLOX_MAC_FIRST_RUN_TIMEOUT seconds
# (default 300) because a cold prefix on slow hardware can outgrow the
# bootstrapper's 120s default. Reserve about 2 GB in $TMPDIR for the AppImage
# copy, the client and shader staging; A and B each perform one full download.
# VALIDATE_UPDATE_TIMEOUT (default 2h) bounds each update run. On failure (or
# with VALIDATE_KEEP=1) the sandbox is kept for inspection.
#
# Exit status: 0 only when every runtime check passed.
set -u
umask 077

SRC=${1:-}
[ -n "$SRC" ] || { echo "usage: $0 <release-folder>" >&2; exit 2; }
SRC=$(CDPATH= cd -- "$SRC" 2>/dev/null && pwd) ||
    { echo "not a directory: $1" >&2; exit 2; }
for name in update-roblox.sh run.sh bootstrap.sh bootstrap.py FFlags.json RobloxLinux.AppImage; do
    [ -e "$SRC/$name" ] || { echo "missing $SRC/$name" >&2; exit 2; }
done
for tool in curl timeout md5sum python3 flock pkill findmnt; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing tool: $tool" >&2; exit 2; }
done

SBX=$(mktemp -d "${TMPDIR:-/tmp}/rbx-validate.XXXXXX") ||
    { echo "could not create the validation sandbox" >&2; exit 2; }
REL=$SBX/release
DNS=$REL/DO_NOT_SHARE
FAILED=0

# Detach every FUSE mount under the sandbox, stale ones included: a killed
# AppImage run leaves them behind, they block rm -rf (read-only), and a stale
# one below DO_NOT_SHARE makes the launch bind-mount fail with EINVAL.
# findmnt, not find: stale FUSE entries fail lstat and get skipped by find.
# Lazy unmounting works even if a process still holds the mount.
unmount_sandbox() {
    findmnt -rn -o TARGET 2>/dev/null | grep -F "$SBX/" | while IFS= read -r _mnt; do
        fusermount3 -uz "$_mnt" 2>/dev/null ||
            fusermount -uz "$_mnt" 2>/dev/null ||
            umount -l "$_mnt" 2>/dev/null || true
    done
}
cleanup() {
    unmount_sandbox
    if [ "${VALIDATE_KEEP:-0}" = 1 ] || [ "$FAILED" = 1 ]; then
        echo "sandbox kept for inspection: $SBX" >&2
    else
        rm -rf -- "$SBX" 2>/dev/null || {
            echo "could not fully remove the sandbox: $SBX" >&2
            echo '  a mount may remain; detach it with: fusermount3 -uz '"$SBX"'/release/DO_NOT_SHARE/tmp/.mount_*' >&2
            echo '  then remove the sandbox with: rm -rf '"$SBX" >&2
        }
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

mkdir -p "$REL"
for name in update-roblox.sh run.sh bootstrap.sh bootstrap.py FFlags.json icon.png; do
    [ -e "$SRC/$name" ] && cp "$SRC/$name" "$REL/$name"
done
# A real copy, not a symlink: the AppImage runtime resolves its own path, and
# STATE (release root) is derived from it.
cp "$SRC/RobloxLinux.AppImage" "$REL/RobloxLinux.AppImage"
chmod +x "$REL/RobloxLinux.AppImage"

mkdir -p "$DNS"
# Synthetic session files: the updater must never change any of them. No
# prefix fixture: roblox-mac initializes the darling prefix only when the
# directory is absent, so a pre-seeded one would skip init and first-run
# would fail on the missing shellspawn socket; the real prefix is created
# by the first launch, after the update.
mkdir -p "$DNS/cookies" "$DNS/browser/data/localstorage"
printf 'test native session' > "$DNS/cookies/native.sqlite"
printf 'test browser session' > "$DNS/browser/cookies.sqlite"
printf 'test browser storage' > "$DNS/browser/data/localstorage/test"
# Headless bootstrap: never prompt, never auto-update, no update check due.
printf '{"auto_update":"off","check_interval_hours":24,"last_check":999999999999}\n' > "$DNS/bootstrapper.json"
# B's relaunch includes the first-run darling setup: a cold prefix on slow
# hardware can outgrow bootstrap's 120s default, so widen the window here.
export ROBLOX_MAC_FIRST_RUN_TIMEOUT="${ROBLOX_MAC_FIRST_RUN_TIMEOUT:-300}"
# Keep every AppImage mount inside the sandbox: run.sh and bootstrap.sh honor
# an inherited APPIMAGE_TARGET_DIR, so all mounts land under it and the
# inter-test teardown can see and detach them. A mount below DO_NOT_SHARE
# would poison the launch bind-mount (EINVAL), and mounts outside the
# sandbox would leak garbage on the host.
APPMNT=$SBX/app-mnt
mkdir -m 700 "$APPMNT"
export APPIMAGE_TARGET_DIR=$APPMNT

pass() { printf 'PASS  %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1" >&2; FAILED=1; }

# Everything the updater itself may legitimately create while running
# (locks, temp downloads, uruntime extraction, --debug diagnostics).
dns_snapshot() {
    (cd "$DNS" && find . -type f \
        ! -name instance.lock ! -name bootstrapper.json \
        ! -path './tmp/*' ! -path './diagnostics/*' \
        ! -name '.version-query.*' ! -name '.check.*' ! -name '.bootstrap-*' \
        -exec md5sum {} \;) | sort
}

# True when nothing still holds the sandbox instance lock.
lock_free() {
    [ -e "$DNS/instance.lock" ] || return 0
    sh -c 'exec 9>>"$1" && flock -n 9' sh "$DNS/instance.lock" 2>/dev/null
}

# Between tests: detach leftover mounts first (lazy unmount works while their
# daemons still run; killing first would leave stale mounts), then kill
# anything still running from the previous test (an orphaned client tree left
# by the launch timeout, or a lingering AppImage/FUSE daemon), verify nothing
# is left mounted, and wait for the instance lock to be free. Everything
# matched here runs inside the sandbox.
quiet_sandbox() {
    unmount_sandbox
    pkill -9 -f "$SBX/" 2>/dev/null || true
    sleep 1
    unmount_sandbox
    if findmnt -rn -o TARGET 2>/dev/null | grep -qF "$SBX/"; then
        fail "a mount under the sandbox could not be detached (findmnt)"
    fi
    if [ -e "$DNS/instance.lock" ]; then
        i=0
        while [ "$i" -lt 50 ] && ! lock_free; do
            i=$(( i + 1 ))
            sleep 0.2
        done
        lock_free || fail "the sandbox instance lock is still held: $DNS/instance.lock"
    fi
}

printf 'sandbox: %s\n' "$SBX"

printf '\n=== A: fresh install (update-roblox.sh: download + shader prep + install)\n'
# The reported broken state: no installed client, stale uncommitted staging.
mkdir -p "$REL/.update-work/next"
printf 'half a download\n' > "$REL/.update-work/next/partial"
before=$(dns_snapshot)
A_OUT=$SBX/a-update.log
status=0
timeout --foreground --kill-after=10s "${VALIDATE_UPDATE_TIMEOUT:-2h}" \
    sh "$REL/update-roblox.sh" >"$A_OUT" 2>&1 || status=$?
[ "$status" = 0 ] || fail "A: update-roblox.sh exited $status (log: $A_OUT)"
grep -q 'Release data must stay inside DO_NOT_SHARE' "$A_OUT" &&
    fail "A: the release confinement rejected the staged data (log: $A_OUT)"
grep -q 'c.zip: No such file or directory' "$A_OUT" &&
    fail "A: the download-progress poll printed a spurious error (log: $A_OUT)"
grep -q '^Roblox updated: version-' "$A_OUT" ||
    fail "A: the update did not report completion (log: $A_OUT)"
[ -x "$REL/RobloxVersion/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] ||
    fail 'A: the client executable was not installed'
[ -s "$REL/RobloxVersion/.version" ] || fail 'A: the version marker is missing'
[ -f "$REL/RobloxVersion/spv-cache-v1/report.json" ] ||
    fail 'A: the shader cache report is missing'
[ ! -e "$REL/.update-work" ] || fail 'A: staging was not cleaned up'
[ "$(dns_snapshot)" = "$before" ] || fail 'A: DO_NOT_SHARE changed during the update'
[ -z "$(find "$DNS" -maxdepth 1 -name '.update.*' -print -quit)" ] ||
    fail 'A: staging leaked into DO_NOT_SHARE'
lock_free ||
    fail 'A: the update stranded the instance lock (an AppImage child kept it)'
[ "$FAILED" = 0 ] &&
    pass "A: installed $(cat "$REL/RobloxVersion/.version") with a prepared shader cache"
quiet_sandbox

printf '\n=== B: crash between the swap renames, then relaunch (recovery completes)\n'
_b_start=$FAILED
V_INSTALLED=$(cat "$REL/RobloxVersion/.version" 2>/dev/null || printf '')
[ -n "$V_INSTALLED" ] || fail 'B: no installed version from A to compare against'
# Force the update path again for the same version (the installed cache is
# incomplete), then die exactly in the window between the two swap renames.
rm -f "$REL/RobloxVersion/spv-cache-v1/report.json"
B_CRASH=$SBX/b-crash.log
status=0
ROBLOX_MAC_TEST_CRASH_AFTER_RENAME1=1 \
    timeout --foreground --kill-after=10s "${VALIDATE_UPDATE_TIMEOUT:-2h}" \
    sh "$REL/update-roblox.sh" >"$B_CRASH" 2>&1 || status=$?
[ "$status" = 137 ] ||
    fail "B: expected SIGKILL (137) in the crash window, got $status (log: $B_CRASH)"
[ -e "$REL/.update-work/TXN" ] || fail 'B: the durable commit marker is missing'
[ -d "$REL/.update-work/previous" ] || fail 'B: the backup client is missing'
[ -d "$REL/.update-work/next" ] || fail 'B: the staged client is missing'
[ ! -e "$REL/RobloxVersion" ] || fail 'B: the interrupted swap unexpectedly completed'
lock_free ||
    fail 'B: the crashed update stranded the instance lock'
quiet_sandbox

B_LAUNCH=$SBX/b-relaunch.log
status=0
timeout --foreground --kill-after=5s "${VALIDATE_LAUNCH_TIMEOUT:-300}" \
    sh "$REL/run.sh" >"$B_LAUNCH" 2>&1 || status=$?
grep -q 'already running' "$B_LAUNCH" &&
    fail "B: the relaunch found the instance lock still held (log: $B_LAUNCH)"
grep -q 'recovered an interrupted update (completed the downloaded client)' "$B_LAUNCH" ||
    fail "B: the relaunch did not complete the staged update (log: $B_LAUNCH)"
[ "$(cat "$REL/RobloxVersion/.version" 2>/dev/null || printf x)" = "$V_INSTALLED" ] ||
    fail 'B: the recovered client has the wrong version'
[ -f "$REL/RobloxVersion/spv-cache-v1/report.json" ] ||
    fail 'B: the recovered client has no shader cache report'
[ ! -e "$REL/.update-work" ] || fail 'B: staging survived recovery'
case "$status" in
    0|124|137)
        [ "$FAILED" = "$_b_start" ] &&
            pass "B: relaunch recovered the staged client and ran the client (exit $status; 124 = launch window closed by timeout, 137 = the client outlived the window and timeout escalated to SIGKILL)";;
    *)
        fail "B: the client did not launch after recovery (exit $status; log: $B_LAUNCH)";;
esac
quiet_sandbox

printf '\n=== C: unusable staged client with a valid backup (recovery restores)\n'
WORK=$REL/.update-work
mkdir -p "$WORK/next"    # no executable, no version, no cache: invalid
[ -d "$REL/RobloxVersion" ] && mv "$REL/RobloxVersion" "$WORK/previous"
printf 'committed %s 1\n' "$V_INSTALLED" > "$WORK/TXN"
C_OUT=$SBX/c-restore.log
status=0
timeout --foreground 360 sh "$REL/bootstrap.sh" check >"$C_OUT" 2>&1 || status=$?
grep -q 'already running' "$C_OUT" &&
    fail "C: bootstrap found the instance lock still held (log: $C_OUT)"
[ "$status" = 0 ] || fail "C: bootstrap.sh check exited $status (log: $C_OUT)"
grep -q 'recovered an interrupted update (restored the previous client)' "$C_OUT" ||
    fail "C: recovery did not restore the backup (log: $C_OUT)"
[ "$(cat "$REL/RobloxVersion/.version" 2>/dev/null || printf x)" = "$V_INSTALLED" ] ||
    fail 'C: the restored client has the wrong version'
[ ! -e "$WORK" ] || fail 'C: staging survived recovery'
[ "$FAILED" = 0 ] && pass 'C: recovery restored the previous client and cleaned staging'

printf '\n'
if [ "$FAILED" = 0 ]; then
    echo 'RUNTIME VALIDATION PASSED: download -> install, crash-window recovery, restore path, DO_NOT_SHARE preservation.'
    echo 'Still confirm manually in the real release: ./run.sh must open the client.'
    exit 0
fi
echo 'RUNTIME VALIDATION FAILED: see the FAIL lines above; logs are kept in the sandbox.' >&2
exit 1
