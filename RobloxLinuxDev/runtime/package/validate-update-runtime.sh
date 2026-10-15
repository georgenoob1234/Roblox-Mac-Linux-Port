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
# five minutes. Reserve about 2 GB in $TMPDIR for the AppImage copy, the
# client and shader staging; A and B each perform one full download.
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
for tool in curl timeout md5sum python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing tool: $tool" >&2; exit 2; }
done

SBX=$(mktemp -d "${TMPDIR:-/tmp}/rbx-validate.XXXXXX") ||
    { echo "could not create the validation sandbox" >&2; exit 2; }
REL=$SBX/release
DNS=$REL/DO_NOT_SHARE
FAILED=0

# The AppImage runtime FUSE-mounts itself read-only under DO_NOT_SHARE/tmp
# (run.sh points TMPDIR there). A killed run — Ctrl-C, a timeout kill — can
# leave that mount behind, and the sandbox cannot be removed while it is
# mounted, so detach every leftover before removing or keeping the sandbox.
# Lazy unmounting works even if a process still holds the mount.
unmount_sandbox() {
    for mount_dir in $(find "$SBX" -maxdepth 5 -name '.mount_*' -type d -prune 2>/dev/null); do
        fusermount3 -uz "$mount_dir" 2>/dev/null ||
            fusermount -uz "$mount_dir" 2>/dev/null ||
            umount -l "$mount_dir" 2>/dev/null || true
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
# Synthetic session files: the updater must never change any of them.
mkdir -p "$DNS/cookies" "$DNS/browser/data/localstorage" \
    "$DNS/prefix/Users/root/Library/Preferences"
printf 'test native session' > "$DNS/cookies/native.sqlite"
printf 'test browser session' > "$DNS/browser/cookies.sqlite"
printf 'test browser storage' > "$DNS/browser/data/localstorage/test"
printf 'test client preferences' > "$DNS/prefix/Users/root/Library/Preferences/test.plist"
# Headless bootstrap: never prompt, never auto-update, no update check due.
printf '{"auto_update":"off","check_interval_hours":24,"last_check":999999999999}\n' > "$DNS/bootstrapper.json"

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
[ "$FAILED" = 0 ] &&
    pass "A: installed $(cat "$REL/RobloxVersion/.version") with a prepared shader cache"

printf '\n=== B: crash between the swap renames, then relaunch (recovery completes)\n'
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

B_LAUNCH=$SBX/b-relaunch.log
status=0
timeout --foreground --kill-after=5s 300 sh "$REL/run.sh" >"$B_LAUNCH" 2>&1 || status=$?
grep -q 'recovered an interrupted update (completed the downloaded client)' "$B_LAUNCH" ||
    fail "B: the relaunch did not complete the staged update (log: $B_LAUNCH)"
[ "$(cat "$REL/RobloxVersion/.version" 2>/dev/null || printf x)" = "$V_INSTALLED" ] ||
    fail 'B: the recovered client has the wrong version'
[ -f "$REL/RobloxVersion/spv-cache-v1/report.json" ] ||
    fail 'B: the recovered client has no shader cache report'
[ ! -e "$REL/.update-work" ] || fail 'B: staging survived recovery'
case "$status" in
    0|124)
        pass "B: relaunch recovered the staged client and ran the client (exit $status; 124 = window closed by the 5-minute timeout)";;
    *)
        fail "B: the client did not launch after recovery (exit $status; log: $B_LAUNCH)";;
esac

printf '\n=== C: unusable staged client with a valid backup (recovery restores)\n'
WORK=$REL/.update-work
mkdir -p "$WORK/next"    # no executable, no version, no cache: invalid
[ -d "$REL/RobloxVersion" ] && mv "$REL/RobloxVersion" "$WORK/previous"
printf 'committed %s 1\n' "$V_INSTALLED" > "$WORK/TXN"
C_OUT=$SBX/c-restore.log
status=0
timeout --foreground 180 sh "$REL/bootstrap.sh" check >"$C_OUT" 2>&1 || status=$?
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
