#!/bin/sh
# Download the current official Intel macOS client and prepare its shaders.
set -eu
umask 077
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DATA=$HERE/DO_NOT_SHARE
unset ROBLOX_MAC_DATA ROBLOX_MAC_CONFIG ROBLOX_MAC_BROWSER_DATA
mkdir -p "$DATA"
chmod 700 "$DATA"
export HOME=$DATA/home XDG_CONFIG_HOME=$DATA/config
export XDG_DATA_HOME=$DATA/share XDG_CACHE_HOME=$DATA/cache
export TMPDIR=$DATA/tmp TMP=$DATA/tmp TEMP=$DATA/tmp
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$TMPDIR"

progress() {
    fd=${ROBLOX_MAC_PROGRESS_FD:-}
    case "$fd" in ''|*[!0-9]*) return 0;; esac
    python3 - "$1" "$2" "$3" "${4:-0}" "${5:-0}" "${6:-true}" <<'PY' >&$fd 2>/dev/null || true
import json, sys
print(json.dumps({"phase": sys.argv[1], "percent": int(sys.argv[2]),
                  "message": sys.argv[3], "bytes_done": int(sys.argv[4]),
                  "bytes_total": int(sys.argv[5]),
                  "cancellable": sys.argv[6].lower() == "true"}, separators=(",", ":")))
PY
}
cancelled() { [ -n "${ROBLOX_MAC_CANCEL_FILE:-}" ] && [ -e "$ROBLOX_MAC_CANCEL_FILE" ]; }
if [ "${ROBLOX_MAC_LOCK_HELD:-0}" != 1 ]; then
    exec 9>"$DATA/instance.lock"
    if ! flock -n -E 73 9; then
        echo 'Close Roblox before updating.' >&2
        exit 1
    fi
fi
# Serialize updates for this release folder.
exec 8<"$HERE"
if ! flock -n -E 74 8; then
    echo 'An update is already running.' >&2
    exit 1
fi
# AppImage-spawning children must not inherit the lock file descriptors: the
# AppImage runtime's FUSE daemon outlives its command (it keeps the mount for
# reuse) and would strand the instance lock and the update lock until it
# exits. bootstrap.sh defends its updater children the same way.
version=$(sh "$HERE/run.sh" --client-version 8>&- 9>&-)
# Extract-and-run may print filesystem progress before the application's output.
version=$(printf '%s\n' "$version" | sed -n '/^version-[0-9a-f][0-9a-f]*$/p')
[ -n "$version" ] || { echo 'Could not read the current Roblox version.' >&2; exit 1; }
if [ "$version" = "$(cat "$HERE/RobloxVersion/.version" 2>/dev/null || true)" ] &&
   [ -x "$HERE/RobloxVersion/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] &&
   [ -f "$HERE/RobloxVersion/spv-cache-v1/report.json" ]; then
    echo 'Roblox is already up to date.'
    exit 0
fi
# Fixed, predictable staging beside the client so an interrupted swap can be
# found and repaired on the next launch. It must never live in DO_NOT_SHARE.
WORK=$HERE/.update-work
# Durability helpers. A rename or unlink is only committed once the parent
# directory entry that records it has been synced; these keep every step of
# the transaction's ordering explicit and auditable.
fsync_dir() {
    python3 - "$@" <<'PY'
import os, sys
for path in sys.argv[1:]:
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
PY
}
sync_tree() {
    # Sync every file of the staged client and every directory, bottom-up, so
    # a directory entry is only durable after the content it names.
    python3 - "$1" <<'PY'
import os, stat, sys

def on_error(err):
    sys.stderr.write(f"roblox: could not read staged directory {err.filename or sys.argv[1]}: {err}\n")
    sys.exit(1)

flags = os.O_RDONLY
if hasattr(os, "O_NOFOLLOW"):
    flags |= os.O_NOFOLLOW

for dirpath, dirnames, filenames in os.walk(sys.argv[1], topdown=False, onerror=on_error):
    for name in filenames:
        path = os.path.join(dirpath, name)
        try:
            st = os.lstat(path)
        except OSError as e:
            sys.stderr.write(f"roblox: could not stat staged file {path}: {e}\n")
            sys.exit(1)
        if stat.S_ISLNK(st.st_mode):
            continue
        if not stat.S_ISREG(st.st_mode):
            continue
        try:
            fd = os.open(path, flags)
        except OSError as e:
            sys.stderr.write(f"roblox: could not open staged file {path}: {e}\n")
            sys.exit(1)
        try:
            os.fsync(fd)
        except OSError as e:
            sys.stderr.write(f"roblox: could not sync staged file {path}: {e}\n")
            sys.exit(1)
        finally:
            os.close(fd)
    try:
        fd = os.open(dirpath, os.O_RDONLY)
    except OSError as e:
        sys.stderr.write(f"roblox: could not open staged directory {dirpath}: {e}\n")
        sys.exit(1)
    try:
        os.fsync(fd)
    except OSError as e:
        sys.stderr.write(f"roblox: could not sync staged directory {dirpath}: {e}\n")
        sys.exit(1)
    finally:
        os.close(fd)
PY
}
client_valid() {
    [ -x "$HERE/RobloxVersion/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] &&
    [ -f "$HERE/RobloxVersion/.version" ] &&
    [ -f "$HERE/RobloxVersion/spv-cache-v1/report.json" ]
}
previous_valid() {
    [ -x "$WORK/previous/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] &&
    [ -f "$WORK/previous/.version" ] &&
    [ -f "$WORK/previous/spv-cache-v1/report.json" ]
}
# A run that was interrupted leaves a durable marker in $WORK. The launcher
# repairs that on startup; refuse to build on an unresolved state here rather
# than discard the only copy of the client. When the installed client is
# valid, the interrupted swap has already completed: sync it, then drop the
# leftover backup. With no marker, staging is disposable.
if [ -e "$WORK/TXN" ] && ! client_valid; then
    echo 'An interrupted update is pending; run the launcher to repair it.' >&2
    exit 1
fi
if [ -e "$WORK/TXN" ]; then
    fsync_dir "$HERE" ||
        { echo 'roblox: could not sync the release directory; run the launcher to repair the update state.' >&2
          exit 1; }
fi
rm -rf -- "$WORK"
# A release run confines ROBLOX_MAC_DATA to DO_NOT_SHARE, with one exception:
# this launcher-managed staging directory, which the runtime accepts by
# canonical path prefix. Download directly into staging (never through the
# live-launch alias), then adopt the fixed "next" name the swap and the
# launcher's recovery expect.
mkdir -p "$WORK/data"
cleanup() {
    # Never delete the backup until the new client is confirmed valid and its
    # installation durably committed. If the swap failed, restore the previous
    # client first — validated, because a broken backup must stay in place for
    # the launcher's recovery; keep the work dir for the launcher's recovery
    # when no valid client can be confirmed or the release directory cannot
    # be synced.
    if [ ! -e "$HERE/RobloxVersion" ] && previous_valid; then
        mv "$WORK/previous" "$HERE/RobloxVersion" 2>/dev/null || true
    fi
    if client_valid && fsync_dir "$HERE"; then
        rm -rf -- "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
progress downloading 25 'Starting Roblox download' 0 0 true
if cancelled; then progress cancelled 0 'Update cancelled' 0 0 false; exit 125; fi
# AppImage owns the downloader and shader tools; stage the new client directly
# beside the client so it is verified before anything is renamed.
ROBLOX_MAC_DATA="$WORK/data" \
    ROBLOX_MAC_PROGRESS_FD="${ROBLOX_MAC_PROGRESS_FD:-}" \
    ROBLOX_MAC_CANCEL_FILE="${ROBLOX_MAC_CANCEL_FILE:-}" sh "$HERE/run.sh" --download-client 8>&- 9>&-
mv "$WORK/data" "$WORK/next"
if cancelled; then progress cancelled 0 'Update cancelled' 0 0 false; exit 125; fi
progress verifying 74 'Verifying the downloaded client' 0 0 true
if ! ROBLOX_MAC_DATA="$WORK/next/runtime" ROBLOX_MAC_APP="$WORK/next/RobloxPlayer.app" \
    ROBLOX_MAC_CANCEL_FILE="${ROBLOX_MAC_CANCEL_FILE:-}" sh "$HERE/run.sh" --debug --prepare-shaders 8>&- 9>&- ||
   [ ! -f "$WORK/next/spv-cache-v1/report.json" ]; then
    echo 'Shader preparation failed; the previous client has not been changed.' >&2
    exit 1
fi
if cancelled; then progress cancelled 0 'Update cancelled' 0 0 false; exit 125; fi
progress preparing 90 'Preparing shaders' 0 0 true
rm -rf -- "$WORK/next/runtime"
# Verify the staged client in full before committing anything.
if [ ! -x "$WORK/next/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] ||
   [ ! -f "$WORK/next/.version" ] ||
   [ ! -f "$WORK/next/spv-cache-v1/report.json" ]; then
    echo 'The downloaded client is incomplete; the previous client has not been changed.' >&2
    exit 1
fi
# The swap must be an atomic rename(2). Staging sits under $HERE on the same
# filesystem; refuse the update rather than fall back to a non-atomic copy.
if [ "$(stat -c %d -- "$WORK")" != "$(stat -c %d -- "$HERE")" ]; then
    echo 'Update staging is on a different filesystem; refusing a non-atomic swap.' >&2
    exit 1
fi
# Make the staged client's contents durable before recording the commit: the
# marker must never promise a client whose files could still read back as
# zero-length after power loss.
sync_tree "$WORK/next"
# Commit point: durably record the swap before the renames so a crash between
# them is recoverable. The marker's existence is the commit signal — its text
# is diagnostic only. Sync the marker, its entry in $WORK, and $WORK's own
# entry in $HERE so that any durable rename implies a durable marker.
printf 'committed %s %s\n' "$(cat "$WORK/next/.version")" "$(date +%s)" > "$WORK/TXN"
python3 - "$WORK/TXN" <<'PY'
import os, sys
marker = sys.argv[1]
fd = os.open(marker, os.O_RDONLY)
os.fsync(fd)
os.close(fd)
for path in (os.path.dirname(marker), os.path.dirname(os.path.dirname(marker))):
    fd = os.open(path, os.O_RDONLY)
    os.fsync(fd)
    os.close(fd)
PY
[ ! -e "$HERE/RobloxVersion" ] || mv "$HERE/RobloxVersion" "$WORK/previous"
# Sync both sides of the rename: the client leaving $HERE and the backup
# appearing in $WORK. From here on, every crash lands inside the window the
# durable marker covers.
fsync_dir "$HERE" "$WORK"
if [ -n "${ROBLOX_MAC_TEST_CRASH_AFTER_RENAME1:-}" ]; then
    # Test hook: crash in the window between the two swap renames. SIGKILL so no
    # cleanup trap runs, exactly like kill -9 or a power loss.
    kill -9 $$
fi
mv "$WORK/next" "$HERE/RobloxVersion"
# The install is only committed once both sides of this rename are synced;
# that is what makes deleting the backup safe afterwards.
fsync_dir "$HERE" "$WORK"
progress update 100 'Update complete' 0 0 false
echo "Roblox updated: $(cat "$HERE/RobloxVersion/.version")"
