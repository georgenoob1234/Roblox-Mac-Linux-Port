#!/bin/sh
# Headless release bootstrapper: one lock, first-run setup, bounded update check, then launch.
set -u
umask 077
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DATA=$HERE/DO_NOT_SHARE
APP=$HERE/RobloxLinux.AppImage
STATE=$DATA/bootstrapper.json
HELPER=$HERE/bootstrap.py
MARKER=$DATA/first-run.initialized
# Shared cancellation handshake with the installer and the GTK UI. The marker
# records a request; the state file records whether cancellation is still
# accepted ("open") or the commit has begun ("committing"/"committed"). The
# installer and the UI settle the boundary under <state>.lock; this script only
# resets the state before an operation, which it does while holding the
# instance lock, so no other operation can be running.
CANCEL_MARKER=$DATA/.bootstrap-cancel
CANCEL_STATE=$DATA/.bootstrap-cancel-state
URI_MAX_AGE=${ROBLOX_MAC_URI_MAX_AGE:-600}
EVENTS=${ROBLOX_MAC_EVENTS:-0}
if [ "${ROBLOX_MAC_NO_PROGRESS:-0}" = 1 ]; then
    UI_APP=
else
    UI_APP=${ROBLOX_MAC_UI_APP:-}
fi
UI_FD=0
UI_PID=
UI_FIFO=
UI_CANCELLED=0
UI_RESULT=0

cancelled() { [ -n "${ROBLOX_MAC_CANCEL_FILE:-}" ] && [ -e "$ROBLOX_MAC_CANCEL_FILE" ]; }

# Atomically record the shared cancellation state under <state>.lock, the same
# lock the installer and the GTK UI use to settle the commit boundary. The lock
# and the state are opened with O_NOFOLLOW|O_NONBLOCK and validated on the
# opened descriptor with fstat(), so a symlinked, FIFO, or foreign-owned path is
# rejected promptly instead of being followed or blocking the shell forever, and
# the flock spans the whole read-modify-write. Returns non-zero when the state
# could not be recorded, so the caller never starts an operation on stale state.
#
# The Python helper is used when it can run. The launcher must still start on a
# host where Python cannot run at all, so cancel_state_write_shell below is an
# equivalent fallback: it also never follows a symlink, never opens a FIFO (an
# O_RDWR open does not block on one), still verifies the descriptor it actually
# opened, and still returns non-zero on any unsafe or failed path.
CANCEL_PYTHON=
cancel_state_write() {
    if [ -z "$CANCEL_PYTHON" ]; then
        if python3 -c 'import fcntl, os, stat' >/dev/null 2>&1; then
            CANCEL_PYTHON=1
        else
            CANCEL_PYTHON=0
        fi
    fi
    if [ "$CANCEL_PYTHON" = 1 ]; then
        python3 - "$CANCEL_STATE" "$1" <<'PY'
import fcntl, os, stat, sys

state_path = sys.argv[1]
value = sys.argv[2]
SAFE_FLAGS = (getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
              | getattr(os, "O_NONBLOCK", 0))


def safe_open(path, flags):
    fd = os.open(path, flags | SAFE_FLAGS, 0o600)
    st = os.fstat(fd)
    if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid():
        os.close(fd)
        raise OSError("not a regular file owned by this user")
    return fd


lock_path = state_path + ".lock"
try:
    lock_fd = safe_open(lock_path, os.O_RDWR | os.O_CREAT)
except OSError as exc:
    print(f"cancel_state_write: unsafe lock path {lock_path}: {exc}", file=sys.stderr)
    sys.exit(1)
try:
    fcntl.flock(lock_fd, fcntl.LOCK_EX)
    try:
        st = os.lstat(state_path)
        if stat.S_ISLNK(st.st_mode) or not stat.S_ISREG(st.st_mode):
            raise OSError("not a regular file")
        if st.st_uid != os.getuid():
            raise OSError("owned by another user")
    except FileNotFoundError:
        pass
    except OSError as exc:
        print(f"cancel_state_write: unsafe state path {state_path}: {exc}", file=sys.stderr)
        sys.exit(1)
    tmp = f"{state_path}.tmp.{os.getpid()}"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | SAFE_FLAGS, 0o600)
    try:
        os.write(fd, (value + "\n").encode("utf-8"))
    finally:
        os.close(fd)
    os.chmod(tmp, 0o600)
    os.replace(tmp, state_path)
except OSError as exc:
    print(f"cancel_state_write: could not record {value!r} at {state_path}: {exc}", file=sys.stderr)
    sys.exit(1)
finally:
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
    finally:
        os.close(lock_fd)
PY
        return $?
    fi
    cancel_state_write_shell "$1"
}

# Equivalent path used only when Python cannot run. `test -f` stats without
# opening, so a FIFO, directory, or other special file is rejected before the
# open; the open itself is O_RDWR (`<>`), which never blocks on a FIFO; and the
# descriptor actually opened is verified through /proc before the lock is taken.
cancel_state_write_shell() {
    lock=$CANCEL_STATE.lock
    [ -h "$CANCEL_STATE" ] && return 1
    if [ -e "$CANCEL_STATE" ] && [ ! -f "$CANCEL_STATE" ]; then return 1; fi
    [ -h "$lock" ] && return 1
    if [ -e "$lock" ] && [ ! -f "$lock" ]; then return 1; fi
    # The braces scope the redirect, so a failure here does not silence the
    # launcher's stderr for the rest of the run.
    if ! { exec 7<>"$lock"; } 2>/dev/null; then return 1; fi
    fd_owner=$(stat -Lc %u /proc/self/fd/7 2>/dev/null || printf '')
    if [ ! -f /proc/self/fd/7 ] || { [ -n "$fd_owner" ] && [ "$fd_owner" != "$(id -u)" ]; }; then
        exec 7>&-
        return 1
    fi
    if [ -e "$CANCEL_STATE" ]; then
        owner=$(stat -Lc %u -- "$CANCEL_STATE" 2>/dev/null || printf '')
        if [ -n "$owner" ] && [ "$owner" != "$(id -u)" ]; then exec 7>&-; return 1; fi
    fi
    flock -x 7 2>/dev/null || { exec 7>&-; return 1; }
    tmp=$CANCEL_STATE.tmp.$$
    if ! printf '%s\n' "$1" > "$tmp" 2>/dev/null; then rm -f -- "$tmp"; exec 7>&-; return 1; fi
    chmod 600 "$tmp" 2>/dev/null || true
    if ! mv -f -- "$tmp" "$CANCEL_STATE" 2>/dev/null; then rm -f -- "$tmp"; exec 7>&-; return 1; fi
    exec 7>&-
    return 0
}

say() { echo "roblox: $*" >&2; }
event() {
    payload=$(python3 - "$1" "$2" "$3" "${4:-0}" "${5:-0}" "${6:-true}" <<'PY'
import json, sys
print(json.dumps({"phase": sys.argv[1], "percent": int(sys.argv[2]), "message": sys.argv[3],
                  "bytes_done": int(sys.argv[4]), "bytes_total": int(sys.argv[5]),
                  "cancellable": sys.argv[6].lower() == "true"}, separators=(",", ":")))
PY
    )
    if [ "$EVENTS" = 1 ]; then
        printf '%s\n' "$payload"
    fi
    if [ "$UI_FD" = 1 ]; then printf '%s\n' "$payload" >&8 2>/dev/null || true; fi
}
start_progress() {
    [ -n "$UI_APP" ] || return 0
    ROBLOX_MAC_CANCEL_FILE=$CANCEL_MARKER
    ROBLOX_MAC_CANCEL_STATE_FILE=$CANCEL_STATE
    export ROBLOX_MAC_CANCEL_FILE ROBLOX_MAC_CANCEL_STATE_FILE
    # Drop any request and state left by a previous operation before the new
    # one can observe them: a stale "committing" must never make a fresh
    # update look uncancellable, and a stale marker must never cancel it. If
    # the reset cannot be recorded the operation must not start, so a stale
    # state can never be mistaken for a fresh one; the caller aborts.
    rm -f -- "$ROBLOX_MAC_CANCEL_FILE"
    if ! cancel_state_write open; then
        ROBLOX_MAC_CANCEL_FILE=
        ROBLOX_MAC_CANCEL_STATE_FILE=
        export ROBLOX_MAC_CANCEL_FILE ROBLOX_MAC_CANCEL_STATE_FILE
        say 'Could not reset the shared update state; not starting the operation.'
        return 1
    fi
    UI_FIFO=$DATA/.bootstrap-progress.$$
    rm -f -- "$UI_FIFO"
    mkfifo "$UI_FIFO" 2>/dev/null || { UI_FIFO=; return 0; }
    # Read/write keeps startup non-blocking even when GTK is unavailable.
    exec 8<>"$UI_FIFO"
    # AppImage-spawning children never inherit FD 9 (the instance lock): the
    # AppImage runtime's FUSE daemon outlives its command (it keeps the mount
    # for reuse) and would strand the lock after this process exits.
    "$UI_APP" --bootstrapper-ui --progress "$DATA" 8>&- 9>&- <"$UI_FIFO" >/dev/null 2>&1 & UI_PID=$!
    rm -f -- "$UI_FIFO"
    UI_FD=1
}
finish_progress() {
    [ "$UI_FD" = 1 ] || return 0
    exec 8>&- 2>/dev/null || true
    exec 8<&- 2>/dev/null || true
    if [ -n "$UI_PID" ]; then
        wait "$UI_PID" 2>/dev/null; ui_result=$?
        UI_RESULT=$ui_result
        [ -e "${ROBLOX_MAC_CANCEL_FILE:-}" ] && UI_CANCELLED=1
        [ "$ui_result" -eq 0 ] || [ "$ui_result" -eq 4 ] || notify 'Bootstrapper UI unavailable; launching without updating.' 'Roblox'
    fi
    rm -f -- "${ROBLOX_MAC_CANCEL_FILE:-}" 2>/dev/null || true
    UI_FD=0; UI_PID=; UI_FIFO=
}
notify() {
    title=${2:-Roblox}
    body=$1
    dbus=${ROBLOX_MAC_NOTIFY_DBUS_SESSION_BUS_ADDRESS:-${DBUS_SESSION_BUS_ADDRESS:-}}
    runtime=${ROBLOX_MAC_NOTIFY_XDG_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-}}
    if command -v gdbus >/dev/null 2>&1 && [ -n "$dbus" ] &&
       env "HOME=${ROBLOX_MAC_NOTIFY_HOME:-${HOME:-/tmp}}" \
           "XDG_RUNTIME_DIR=${runtime:-/tmp}" "DBUS_SESSION_BUS_ADDRESS=$dbus" \
           gdbus call --session --dest org.freedesktop.Notifications \
           --object-path /org/freedesktop/Notifications \
           --method org.freedesktop.Notifications.Notify Roblox 0 '' "$title" "$body" '[]' '{}' 5000 >/dev/null 2>&1; then
        return 0
    fi
    if command -v notify-send >/dev/null 2>&1 && notify-send --app-name=Roblox "$title" "$body" >/dev/null 2>&1; then
        return 0
    fi
    if command -v kdialog >/dev/null 2>&1 && timeout 8 kdialog --title "$title" --passivepopup "$body" 5 >/dev/null 2>&1; then
        return 0
    fi
    if command -v zenity >/dev/null 2>&1 && timeout 8 zenity --notification --text="$body" >/dev/null 2>&1; then
        return 0
    fi
    echo "roblox: $body" >&2
}
settings_get() { python3 "$HELPER" settings "$DATA" get "$1"; }
settings_set() { python3 "$HELPER" settings "$DATA" set "$1" "$2"; }
installed_version() { cat "$HERE/RobloxVersion/.version" 2>/dev/null || true; }

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

_created_boot_alias=0
ensure_alias() {
    if [ -z "${ROBLOX_MAC_ALIAS_DIR:-}" ] || [ ! -d "${ROBLOX_MAC_ALIAS_DIR:-}" ]; then
        _short_base=/tmp
        if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "${XDG_RUNTIME_DIR:-}" ] && [ -w "${XDG_RUNTIME_DIR:-}" ]; then
            _xlen=$(LC_ALL=C printf %s "$XDG_RUNTIME_DIR" | wc -c)
            [ "$_xlen" -le 32 ] && _short_base=$XDG_RUNTIME_DIR
        fi
        _alias_dir=$(mktemp -d "$_short_base/rbx.XXXXXX")
        chmod 700 "$_alias_dir"
        mkdir -m 700 "$_alias_dir/stage"
        mkdir -m 700 "$_alias_dir/data"
        export ROBLOX_MAC_ALIAS_DIR=$_alias_dir
        export ROBLOX_MAC_STAGE=$_alias_dir/stage
        export ROBLOX_MAC_REAL_DATA=$DATA
        _created_boot_alias=1
    fi
}
cleanup_boot_alias() {
    if [ "${_created_boot_alias:-0}" = 1 ] && [ -n "${ROBLOX_MAC_ALIAS_DIR:-}" ]; then
        rm -rf -- "$ROBLOX_MAC_ALIAS_DIR" 2>/dev/null || true
    fi
}
trap cleanup_boot_alias EXIT
trap 'cleanup_boot_alias; exit 130' INT
trap 'cleanup_boot_alias; exit 143' TERM HUP

launch() {
    ensure_alias
    uri=${ROBLOX_MAC_LAUNCH_URI:-}
    dec=${ROBLOX_MAC_DECORATIONS:-}
    if [ -z "$dec" ]; then
        dec=$(settings_get window_decoration 2>/dev/null || printf 'auto')
    fi
    case "$dec" in auto|system|gtk|legacy) ;; *) dec=auto;; esac
    export ROBLOX_MAC_DECORATIONS=$dec

    cur=${ROBLOX_MAC_CURSOR_APPEARANCE:-}
    if [ -z "$cur" ]; then
        cur=$(settings_get cursor_appearance 2>/dev/null || printf 'system')
    fi
    case "$cur" in system|desktop) cur=system;; legacy|roblox) cur=legacy;; *) cur=system;; esac
    export ROBLOX_MAC_CURSOR_APPEARANCE=$cur

    if [ -n "$uri" ]; then
        env ROBLOX_MAC_LOCK_HELD=1 "$APP" --uri "$uri" "$@" 9>&-
        _status=$?
        exit $_status
    fi
    env ROBLOX_MAC_LOCK_HELD=1 "$APP" "$@" 9>&-
    _status=$?
    exit $_status
}

ask_update() {
    latest=$1
    installed=$2
    if [ -n "$UI_APP" ]; then
        choice=$(timeout 31 "$UI_APP" --bootstrapper-ui --dialog "$latest" "$installed" 9>&- >/dev/null 2>&1; printf '%s' "$?" )
        case "$choice" in
            0) return 0;;
            2) settings_set skipped_version "$latest" >/dev/null 2>&1 || true;;
        esac
        if [ "$choice" -gt 2 ] 2>/dev/null; then notify 'Update prompt failed; launching without updating.' 'Roblox update'; fi
        return 1
    fi
    if [ -t 0 ] && [ -t 1 ]; then
        say "Roblox $latest is available (installed: ${installed:-none})."
        say 'Choose: 1) Update now  2) Launch without updating [2 in 30s]'
        answer=$(timeout 30 sh -c 'IFS= read -r value; printf "%s" "$value"' </dev/tty 2>/dev/null || true)
        [ "$answer" = 1 ]
        return
    fi
    if command -v kdialog >/dev/null 2>&1; then
        timeout 30 kdialog --title 'Roblox update available' --yesno "Roblox $latest is available. Update now?" >/dev/null 2>&1
        return
    fi
    if command -v zenity >/dev/null 2>&1; then
        timeout 30 zenity --question --title='Roblox update available' --text="Roblox $latest is available. Update now?" >/dev/null 2>&1
        return
    fi
    return 1
}

first_run() {
    [ -f "$MARKER" ] && [ -d "$DATA/prefix" ] && return 0
    event first-run 5 'Setting up for the first time' 0 0 false
    say 'Setting up Roblox for the first time; this may take a minute.'
    # The warm-up runs in a subshell so it allocates its own throwaway alias
    # directory. This prevents the warm-up's stage symlinks from contaminating
    # the alias that the actual game launch will receive from run.sh.
    # The warm-up does not own the instance lock. Close FD 9 before exec so a
    # Darling/AppImage child cannot strand the lock if timeout has to kill it.
    if ! (unset ROBLOX_MAC_ALIAS_DIR ROBLOX_MAC_STAGE ROBLOX_MAC_REAL_DATA
          timeout --foreground --kill-after=5s "${ROBLOX_MAC_FIRST_RUN_TIMEOUT:-120}" sh -c \
        'exec 9>&-; exec "$@"' sh env ROBLOX_MAC_LOCK_HELD=1 sh "$HERE/run.sh" --shell true); then
        say 'First-run setup failed or timed out; the client was not launched.'
        notify 'Roblox first-run setup failed; the client was not launched.' 'Roblox setup failed'
        return 1
    fi
    tmp=$DATA/.first-run.initialized.$$
    printf 'initialized\n' > "$tmp"
    chmod 600 "$tmp"
    mv -f "$tmp" "$MARKER"
    event first-run 15 'First-run setup complete' 0 0 false
}

check_latest() {
    if cancelled; then return 125; fi
    now=$(date +%s)
    due=$(python3 "$HELPER" due "$DATA" "$now" 2>/dev/null || echo 1)
    if [ "$due" != 1 ]; then
        settings_get last_known_latest
        return 0
    fi
    check_output=$DATA/.check.$$
    rm -f -- "$check_output"
    timeout "${ROBLOX_MAC_UPDATE_CHECK_TIMEOUT:-8}" "$APP" --client-version 9>&- >"$check_output" 2>/dev/null &
    check_pid=$!
    while kill -0 "$check_pid" 2>/dev/null; do
        if cancelled; then kill "$check_pid" 2>/dev/null || true; wait "$check_pid" 2>/dev/null || true; rm -f -- "$check_output"; return 125; fi
        sleep 0.1
    done
    wait "$check_pid" 2>/dev/null
    check_status=$?
    latest=$(sed -n '/^version-[0-9a-f][0-9a-f]*$/p' "$check_output" 2>/dev/null | tail -n 1 || true)
    rm -f -- "$check_output"
    settings_set last_check "$now" >/dev/null 2>&1 || true
    if [ "$check_status" -ne 0 ]; then
        [ -n "$latest" ] && printf '%s\n' "$latest"
        return 1
    fi
    if [ -n "$latest" ]; then
        settings_set last_known_latest "$latest" >/dev/null 2>&1 || true
        display=$(timeout "${ROBLOX_MAC_UPDATE_CHECK_TIMEOUT:-8}" "$APP" --client-version-display 9>&- 2>/dev/null | sed -n '/^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$/p' | tail -n 1 || true)
        [ -z "$display" ] || settings_set last_known_latest_display "$display" >/dev/null 2>&1 || true
        printf '%s\n' "$latest"
    else
        settings_get last_known_latest
        return 1
    fi
}

do_update() {
    if cancelled; then event cancelled 0 'Update cancelled' 0 0 false; return 125; fi
    event downloading 25 'Downloading Roblox' 0 0 true
    # The bootstrapper keeps FD 9; updater children must not inherit it.
    if ! timeout --foreground --kill-after=5s "${ROBLOX_MAC_UPDATE_TIMEOUT:-30m}" sh -c \
        'exec 9>&-; exec "$@"' sh env ROBLOX_MAC_LOCK_HELD=1 \
            ROBLOX_MAC_PROGRESS_FD=8 ROBLOX_MAC_CANCEL_FILE="${ROBLOX_MAC_CANCEL_FILE:-}" \
            sh "$HERE/update-roblox.sh"; then
        if cancelled; then
            event cancelled 0 'Update cancelled; keeping the installed client' 0 0 false
            say 'Update cancelled; the installed client was kept.'
            notify 'Update cancelled; the installed client was kept. The browser link was not launched.' 'Roblox update cancelled'
            return 125
        fi
        event failed 0 'Update failed; keeping the installed client' 0 0 false
        say 'Update failed or timed out; the installed client was kept.'
        notify 'Roblox update failed; the installed client was kept.' 'Roblox update failed'
        return 1
    fi
    if cancelled; then event cancelled 0 'Update cancelled' 0 0 false; return 125; fi
    event verifying 78 'Verifying the downloaded client' 0 0 true
    event preparing 90 'Preparing shaders' 0 0 true
    event update 100 'Update complete' 0 0 false
}

do_port_update() {
    if [ ! -f "$HERE/installer.py" ]; then
        event failed 0 'Port updater is not included in this release' 0 0 false
        say 'Port updater is unavailable; install a release containing installer.py.'
        return 1
    fi
    event checking 5 'Checking the latest port release' 0 0 true
    # The installer owns the cancellation decision. It polls the marker during
    # its cancellable phases, stops honouring it at the commit boundary, and
    # reports an accepted cancellation with its own terminal status (125). The
    # GTK UI and the installer settle the boundary through the shared
    # CANCEL_STATE file under CANCEL_LOCK, so this wrapper never guesses from
    # the marker file -- a late request, or a marker left over from an earlier
    # operation, must not change the result of an update that actually
    # committed.
    #
    # It is deliberately not wrapped in `timeout`: a signal during the
    # non-cancellable commit phase is exactly what leaves run.sh or
    # installer.py missing. The installer bounds its own cancellable phases
    # instead (network socket timeouts plus ROBLOX_MAC_PORT_UPDATE_TIMEOUT,
    # default 30m, applied only before the commit boundary), and an
    # interruption of the commit itself is repaired by the launcher's
    # recovery entry point on the next start.
    env ROBLOX_MAC_PROGRESS_FD=8 \
        ROBLOX_MAC_CANCEL_FILE="${ROBLOX_MAC_CANCEL_FILE:-}" \
        ROBLOX_MAC_CANCEL_STATE_FILE="${ROBLOX_MAC_CANCEL_STATE_FILE:-}" \
        ROBLOX_MAC_LOCK_HELD=1 \
        python3 "$HERE/installer.py" update --prefix "$HERE" --yes --no-desktop
    _port_update_status=$?
    case "$_port_update_status" in
        0)
            event update 100 'Port update complete' 0 0 false
            return 0;;
        3)
            # The new release is committed and live; only obsolete-backup
            # cleanup could not finish. Report it as applied -- never with the
            # failure wording, which would wrongly claim the old release was
            # kept and the change never happened.
            event update 100 'Port update applied; finishing cleanup on the next start' 0 0 false
            say 'Port update applied; leftover cleanup will finish on the next start.'
            return 0;;
        125)
            event cancelled 0 'Port update cancelled; the current release was kept' 0 0 false
            say 'Port update cancelled; the current release was kept.'
            return 125;;
    esac
    event failed 0 'Port update failed; the current release was kept' 0 0 false
    say 'Port update failed; the current release was kept.'
    return 1
}

status_json() { python3 "$HELPER" status "$DATA" "$HERE/RobloxVersion"; }

command=${1:-launch}
case "$command" in
    status)
        status_json; exit 0;;
    get)
        [ "$#" -eq 2 ] || { echo 'Usage: run.sh get SETTING' >&2; exit 2; }
        settings_get "$2"; exit $?;;
    set)
        [ "$#" -eq 3 ] || { echo 'Usage: run.sh set SETTING VALUE' >&2; exit 2; }
        settings_set "$2" "$3"; exit $?;;
    reset-appearance)
        python3 "$HELPER" settings "$DATA" reset_appearance; exit $?;;
    check)
        mkdir -p "$DATA"; exec 9>"$DATA/instance.lock"
        if ! flock -n -E 73 9; then
            say 'Roblox is already running.'; notify 'Roblox is already running.' 'Roblox is already running'; exit 0
        fi
        recover_interrupted_update || { exec 9>&-; exit 1; }
        start_progress || { exec 9>&-; exit 1; }
        first_run || { finish_progress; exit 1; }
        event checking 25 'Checking for updates'
        latest=$(check_latest); check_result=$?
        [ "$check_result" -eq 125 ] && { finish_progress; notify 'Update check cancelled.' 'Roblox'; exit 125; }
        [ "$check_result" -eq 0 ] || { event failed 0 'Could not check for updates' 0 0 false; finish_progress; status_json; exit 1; }
        finish_progress
        exec 9>&-
        status_json; exit 0;;
    update)
        EVENTS=1
        mkdir -p "$DATA"; exec 9>"$DATA/instance.lock"
        if ! flock -n -E 73 9; then
            say 'Roblox is already running.'; notify 'Roblox is already running.' 'Roblox is already running'; exit 0
        fi
        recover_interrupted_update || { exec 9>&-; exit 1; }
        start_progress || { exec 9>&-; exit 1; }
        first_run || { finish_progress; exit 1; }
        event checking 25 'Checking for updates'
        latest=$(check_latest); check_result=$?
        [ "$check_result" -eq 125 ] && { finish_progress; exit 125; }
        [ "$check_result" -eq 0 ] || { event failed 0 'Could not check for updates' 0 0 false; finish_progress; exit 1; }
        [ -n "$latest" ] || { say 'No latest version is known; update check failed.'; finish_progress; exit 1; }
        do_update; result=$?; finish_progress; [ "$UI_CANCELLED" -eq 1 ] && result=125; exit "$result";;
    update-settings)
        [ "$#" -eq 1 ] || { echo 'Usage: run.sh update-settings' >&2; exit 2; }
        sh "$HERE/run.sh" update
        update_result=$?
        sh "$HERE/run.sh" --settings
        settings_result=$?
        [ "$settings_result" -eq 0 ] || exit "$settings_result"
        exit "$update_result";;
    port-update)
        EVENTS=1
        mkdir -p "$DATA"; exec 9>"$DATA/instance.lock"
        if ! flock -n -E 73 9; then
            event failed 0 'Close Roblox before updating the port' 0 0 false
            finish_progress; exit 1
        fi
        start_progress || { exec 9>&-; exit 1; }
        do_port_update; result=$?; finish_progress; exec 9>&-
        exit "$result";;
    port-update-settings)
        [ "$#" -eq 1 ] || { echo 'Usage: run.sh port-update-settings' >&2; exit 2; }
        sh "$HERE/run.sh" port-update
        update_result=$?
        sh "$HERE/run.sh" --settings
        settings_result=$?
        [ "$settings_result" -eq 0 ] || exit "$settings_result"
        exit "$update_result";;
    launch)
        :;;
    *)
        # Preserve client arguments such as --place-id for normal launches.
        :;;
esac

mkdir -p "$DATA"; exec 9>"$DATA/instance.lock"
if ! flock -n -E 73 9; then
    say 'Roblox is already running.'
    notify 'Roblox is already running.' 'Roblox is already running'
    exit 0
fi
recover_interrupted_update || { exec 9>&-; finish_progress; exit 1; }
start_progress || { exec 9>&-; exit 1; }
first_run || { finish_progress; exit 1; }
installed=$(installed_version)
event checking 25 'Checking for updates'
latest=$(check_latest)
check_result=$?
if [ "$check_result" -eq 125 ] || [ "$UI_CANCELLED" -eq 1 ]; then
    finish_progress
    notify 'Update cancelled; the installed client was kept. The browser link was not launched.' 'Roblox update cancelled'
    exit 125
fi
mode=$(settings_get auto_update 2>/dev/null || printf 'ask')
if [ -n "$latest" ] && [ "$latest" != "$installed" ] && [ "$(settings_get skipped_version 2>/dev/null || true)" != "$latest" ]; then
    case "$mode" in
        auto) do_update; result=$?; [ "$result" -eq 125 ] && { finish_progress; notify 'Update cancelled; the installed client was kept. The browser link was not launched.' 'Roblox update cancelled'; exit 125; };;
        ask) if ask_update "$latest" "$installed"; then do_update; result=$?; [ "$result" -eq 125 ] && { finish_progress; notify 'Update cancelled; the installed client was kept. The browser link was not launched.' 'Roblox update cancelled'; exit 125; }; fi;;
        off) :;;
    esac
fi
if [ -n "${ROBLOX_MAC_URI_RECEIVED_AT:-}" ] && [ -n "${ROBLOX_MAC_LAUNCH_URI:-}" ]; then
    now=$(date +%s)
    if [ "$((now - ROBLOX_MAC_URI_RECEIVED_AT))" -gt "$URI_MAX_AGE" ]; then
        say 'The browser link expired during setup or update; launching without it.'
        notify 'The browser link expired; Roblox is launching normally.' 'Roblox link expired'
        unset ROBLOX_MAC_LAUNCH_URI
    fi
fi
event launching 100 'Launching Roblox' 0 0 false
finish_progress
launch "$@"
