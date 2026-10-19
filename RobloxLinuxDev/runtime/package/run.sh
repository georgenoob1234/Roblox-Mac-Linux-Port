#!/bin/sh
# Terminal launcher with diagnostics. --diagnose checks the system without starting Roblox.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# This read-only query must run before launch-only TMPDIR, mount, alias, and
# lock setup. It is used by Settings refreshes and must leave no temp state.
if [ "${1:-}" = port-version ]; then
    [ "$#" -eq 1 ] || { echo 'Usage: run.sh port-version' >&2; exit 2; }
    exec python3 "$HERE/bootstrap.py" port-version "$HERE"
fi
# AppImageLauncher can relocate the image or run a memfd whose parent is /.
# Keep the original image, and keep the AppImage runtime's mounts outside
# DO_NOT_SHARE: a FUSE mount below it makes the launch-time in-namespace
# bind-mount of the data directory fail with EINVAL.
umask 077
mkdir -p "$HERE/DO_NOT_SHARE/tmp"
chmod 700 "$HERE/DO_NOT_SHARE"
# Repair an interrupted port-file transaction before any payload file is used.
# The transaction, its journal and its backups live next to the install
# directory, and its recovery entry point is kept outside the payload, so this
# still works when the interruption left run.sh or installer.py itself
# missing. Recovery runs under the instance lock: the port files are never
# rewritten under a running Roblox, and a double launch still reports "already
# running" and leaves the repair to a later launch.
port_transaction_pending() {
    for _txn in "$HERE"/../.roblox-mac-port-txn-*; do
        if [ -d "$_txn" ]; then return 0; fi
    done
    return 1
}
run_port_recovery() {
    _state=${XDG_STATE_HOME:-${HOME:-}/.local/state}
    _helper=$_state/roblox-mac-linux-port/recovery/recover.sh
    if [ -x "$_helper" ]; then
        ROBLOX_MAC_LOCK_HELD=1 "$_helper" "$HERE"
    elif [ -f "$HERE/installer.py" ]; then
        ROBLOX_MAC_LOCK_HELD=1 python3 "$HERE/installer.py" recover --prefix "$HERE"
    else
        echo 'roblox: an interrupted port update must be repaired before launching, but its recovery helper is missing. Reinstall the port.' >&2
        return 1
    fi
}
recover_port_transaction() {
    port_transaction_pending || return 0
    _port_recovery_status=0
    (
        exec 9>"$HERE/DO_NOT_SHARE/instance.lock" || exit 3
        flock -n -E 73 9 || exit 4
        run_port_recovery
    ) || _port_recovery_status=$?
    case "$_port_recovery_status" in
        # 3 is the installer's applied/pending status: the committed release is
        # live, but its transaction durability is still unconfirmed. Launching
        # it is safe; only a further update is refused until recovery confirms
        # the committed state. 4 means another instance owns the lock.
        0|3|4) return 0;;
    esac
    return 1
}
recover_port_transaction || exit 1
export APPIMAGELAUNCHER_DISABLE=1 _FORCE_HEADLESS=1
export TARGET_APPIMAGE=$HERE/RobloxLinux.AppImage
[ -n "${APPIMAGE_TARGET_DIR:-}" ] && [ -d "${APPIMAGE_TARGET_DIR:-}" ] || unset APPIMAGE_TARGET_DIR
[ -n "${ROBLOX_MAC_ALIAS_DIR:-}" ] && [ -d "${ROBLOX_MAC_ALIAS_DIR:-}" ] || unset ROBLOX_MAC_ALIAS_DIR
export TMPDIR=$HERE/DO_NOT_SHARE/tmp TMP=$HERE/DO_NOT_SHARE/tmp TEMP=$HERE/DO_NOT_SHARE/tmp
# The AppImage runtime mounts or extracts itself at APPIMAGE_TARGET_DIR when
# one is set, and otherwise below TMPDIR (DO_NOT_SHARE/tmp here). Every
# invocation — the updater's internal ones included — therefore gets a
# short-lived mount directory outside DO_NOT_SHARE, beside the per-launch
# mount directories the full launcher flow already uses.
_short_base=/tmp
if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "${XDG_RUNTIME_DIR:-}" ] && [ -w "${XDG_RUNTIME_DIR:-}" ]; then
    _xlen=$(LC_ALL=C printf %s "$XDG_RUNTIME_DIR" | wc -c)
    [ "$_xlen" -le 32 ] && _short_base=$XDG_RUNTIME_DIR
fi
_created_app_target=0
if [ -z "${APPIMAGE_TARGET_DIR:-}" ] || [ ! -d "${APPIMAGE_TARGET_DIR:-}" ]; then
    _app_target=$(mktemp -d "$_short_base/rbx-mnt.XXXXXX")
    chmod 700 "$_app_target"
    export APPIMAGE_TARGET_DIR=$_app_target
    _created_app_target=1
fi
cleanup_target_dir() {
    if [ "${_created_run_alias:-0}" = 1 ] && [ -n "${ROBLOX_MAC_ALIAS_DIR:-}" ]; then
        rm -rf -- "$ROBLOX_MAC_ALIAS_DIR" 2>/dev/null || true
    fi
    if [ "${_created_app_target:-0}" = 1 ] && [ -n "${APPIMAGE_TARGET_DIR:-}" ]; then
        # The AppImage runtime's FUSE daemon can outlive the command, keeping
        # its mount (and this directory) in place; detach it before removal.
        # findmnt, not find: stale FUSE entries fail lstat and get skipped.
        findmnt -rn -o TARGET 2>/dev/null | grep -F "$APPIMAGE_TARGET_DIR" |
        while IFS= read -r _mnt; do
            fusermount3 -uz "$_mnt" 2>/dev/null ||
                fusermount -uz "$_mnt" 2>/dev/null ||
                umount -l "$_mnt" 2>/dev/null || true
        done
        rm -rf -- "$APPIMAGE_TARGET_DIR" 2>/dev/null || true
    fi
}
trap cleanup_target_dir EXIT
trap 'cleanup_target_dir; exit 130' INT
trap 'cleanup_target_dir; exit 143' TERM HUP
# Capture the session bus before the runtime redirects HOME/XDG_* inside its private prefix.
export ROBLOX_MAC_NOTIFY_DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-}
export ROBLOX_MAC_NOTIFY_XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-}
export ROBLOX_MAC_NOTIFY_HOME=${HOME:-}
export ROBLOX_MAC_HOST_HOME=${ROBLOX_MAC_HOST_HOME:-${HOME:-}}
export ROBLOX_MAC_HOST_CONFIG=${ROBLOX_MAC_HOST_CONFIG:-${XDG_CONFIG_HOME:-${HOME:-}/.config}}
export ROBLOX_MAC_HOST_DATA_DIRS=${ROBLOX_MAC_HOST_DATA_DIRS:-${XDG_DATA_DIRS:-/usr/local/share:/usr/share}}
[ -f "$HERE/icon.png" ] && export ROBLOX_MAC_ICON_PATH=${ROBLOX_MAC_ICON_PATH:-$HERE/icon.png}
# >>> update transaction recovery (must stay byte-identical in bootstrap.sh
# and run.sh; check-update-recovery.py compares the marked blocks) >>>
# Repair a client swap interrupted by a crash or power loss. update-roblox.sh
# stages into $WORK, syncs the staged data, and writes the durable
# $WORK/TXN marker immediately before the two swap renames: the marker's
# existence means "commit started". The phase is derived from the filesystem
# rather than trusted from the marker text, in this priority order:
#   no TXN          staging is disposable; the swap never touched the client.
#   client valid    keep the verified client; discard the transaction.
#   next valid      finish the install the transaction committed to.
#   previous valid  roll back to the last known good client.
#   nothing valid   fail loudly, keeping every byte of evidence.
# Invariants: a backup is only restored after full validation; the installed
# client is only replaced once its validated replacement is ready; the
# transaction directory is only deleted after the installed client's
# directory entry has been synced. Only $WORK and the client directory are
# ever touched, never DO_NOT_SHARE. Idempotent: safe before every launch.
WORK=$HERE/.update-work
_upd_log() { echo "roblox: $*" >&2; }
_upd_fsync_dir() {
    python3 - "$@" <<'PY' || return 1
import os, sys
for path in sys.argv[1:]:
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
PY
}
_upd_client_valid() {
    [ -x "$HERE/RobloxVersion/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] &&
    [ -f "$HERE/RobloxVersion/.version" ] &&
    [ -f "$HERE/RobloxVersion/spv-cache-v1/report.json" ]
}
_upd_next_valid() {
    [ -x "$WORK/next/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] &&
    [ -f "$WORK/next/.version" ] &&
    [ -f "$WORK/next/spv-cache-v1/report.json" ]
}
_upd_previous_valid() {
    [ -x "$WORK/previous/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] &&
    [ -f "$WORK/previous/.version" ] &&
    [ -f "$WORK/previous/spv-cache-v1/report.json" ]
}
# Install a validated transaction client (next or previous) over the
# installed one, then drop the transaction once the install is durable.
_upd_install() {
    _upd_src=$1
    _upd_what=$2
    # The installed client failed validation and is unusable, but it is only
    # removed after the replacement is validated and ready to move in.
    if [ -e "$HERE/RobloxVersion" ] && ! rm -rf -- "$HERE/RobloxVersion"; then
        _upd_log 'could not clear the unusable installed client.'
        return 1
    fi
    if ! mv "$_upd_src" "$HERE/RobloxVersion"; then
        _upd_log "could not install $_upd_what."
        return 1
    fi
    # Only delete the backup and marker once the install is durably in place.
    if _upd_fsync_dir "$HERE" "$WORK"; then
        rm -rf -- "$WORK" ||
            _upd_log 'warning: could not discard the finished update state.'
    else
        _upd_log 'warning: could not sync the release directory; update state is kept for the next launch.'
    fi
    return 0
}
recover_interrupted_update() {
    [ -d "$WORK" ] || return 0
    if [ ! -e "$WORK/TXN" ]; then
        # No commit was recorded, so staging is disposable whether or not a
        # client is installed; the swap never touched the installed client.
        rm -rf -- "$WORK" ||
            _upd_log 'warning: could not discard the stale update staging.'
        return 0
    fi
    if _upd_client_valid; then
        # The swap finished (or never started): keep the verified client and
        # drop the transaction, syncing first so the backup is never deleted
        # while the install rename could still be lost to power loss.
        if _upd_fsync_dir "$HERE"; then
            rm -rf -- "$WORK" ||
                _upd_log 'warning: could not discard the finished update state.'
            _upd_log 'recovered an interrupted update (kept the verified installed client).'
        else
            _upd_log 'warning: could not sync the release directory; update state is kept for the next launch.'
        fi
        return 0
    fi
    if _upd_next_valid; then
        _upd_install "$WORK/next" 'the staged update' || return 1
        _upd_log 'recovered an interrupted update (completed the downloaded client).'
        return 0
    fi
    if _upd_previous_valid; then
        _upd_install "$WORK/previous" 'the previous client' || return 1
        _upd_log 'recovered an interrupted update (restored the previous client).'
        return 0
    fi
    _upd_log 'an interrupted update left no usable client; reinstall from a fresh release.'
    return 1
}
# <<< update transaction recovery <<<
URI=
case "${1:-}" in
    --uri) [ "$#" -eq 2 ] || { echo 'Usage: run.sh --uri URI' >&2; exit 2; }; URI=$2; shift 2;;
    roblox://*|roblox-player:*) [ "$#" -eq 1 ] || { echo 'Usage: run.sh URI' >&2; exit 2; }; URI=$1; shift;;
esac
if [ -n "$URI" ]; then
    export ROBLOX_MAC_LAUNCH_URI=$URI ROBLOX_MAC_URI_RECEIVED_AT=$(date +%s)
fi
open_launcher_ui() {
    page=$1
    if "$TARGET_APPIMAGE" --bootstrapper-ui "$page" "$HERE"; then
        exit 0
    fi
    body='The launcher window is unavailable; Roblox is launching without update checks.'
    if command -v notify-send >/dev/null 2>&1; then notify-send --app-name=Roblox 'Roblox launcher' "$body" >/dev/null 2>&1 || true; fi
    echo "roblox: $body" >&2
    exec "$HERE/bootstrap.sh"
}
if [ "${1:-}" = --settings ]; then
    [ -z "$URI" ] && [ "$#" -eq 1 ] || { echo 'Usage: run.sh --settings' >&2; exit 2; }
    open_launcher_ui --settings
fi
if [ "${1:-}" = --launcher ]; then
    [ -z "$URI" ] && [ "$#" -eq 1 ] || { echo 'Usage: run.sh --launcher' >&2; exit 2; }
    open_launcher_ui --launcher
fi
if [ "${ROBLOX_MAC_NO_PROGRESS:-0}" = 1 ]; then
    unset ROBLOX_MAC_UI_APP
else
    export ROBLOX_MAC_UI_APP=$TARGET_APPIMAGE
fi
_is_internal_updater() {
    for _arg in "$@"; do
        case "$_arg" in
            --client-version|--client-version-display|--download-client|--prepare-shaders)
                return 0;;
        esac
    done
    return 1
}
if ! _is_internal_updater "$@"; then
    if [ -d "$WORK" ]; then
        # Serialize with a concurrent updater via the instance lock; its exit
        # is best-effort here because bootstrap.sh repairs and reports again.
        ( exec 9>"$HERE/DO_NOT_SHARE/instance.lock" &&
          flock -n -E 73 9 &&
          recover_interrupted_update ) || true
    fi
    _created_run_alias=0
    if [ -z "${ROBLOX_MAC_ALIAS_DIR:-}" ] || [ ! -d "${ROBLOX_MAC_ALIAS_DIR:-}" ]; then
        _alias_dir=$(mktemp -d "$_short_base/rbx.XXXXXX")
        chmod 700 "$_alias_dir"
        mkdir -m 700 "$_alias_dir/stage"
        mkdir -m 700 "$_alias_dir/data"
        export ROBLOX_MAC_ALIAS_DIR=$_alias_dir
        export ROBLOX_MAC_STAGE=$_alias_dir/stage
        export ROBLOX_MAC_REAL_DATA=$HERE/DO_NOT_SHARE
        _created_run_alias=1
    fi
fi
case "${1:-}" in
    --diagnose|--debug|--client-version|--client-version-display|--download-client|--prepare-shaders|--shell|--inside)
        "$HERE/RobloxLinux.AppImage" "$@"
        _status=$?
        exit $_status;;
esac
[ -t 2 ] || export ROBLOX_MAC_NO_TERMINAL=1
"$HERE/bootstrap.sh" "$@"
_status=$?
exit $_status
