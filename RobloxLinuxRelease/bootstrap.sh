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
# is opened with O_NOFOLLOW|O_NONBLOCK and validated on the opened descriptor
# with fstat(), so a symlinked, FIFO, or foreign-owned path is rejected promptly
# instead of being followed or blocking the shell forever, and the flock spans
# the whole read-modify-write. Returns non-zero when the state could not be
# recorded, so the caller never starts an operation on stale state.
#
# The helper runs Python because it must use the same flock/atomic-replace
# protocol as the installer and the UI. There is deliberately no shell
# fallback: a redirection-based fallback cannot safely open a path that another
# process could replace between the check and the open, so a host where this
# helper cannot run refuses to start the operation instead of weakening the
# protocol.
CANCEL_PYTHON=
cancel_state_write() {
    if [ -z "$CANCEL_PYTHON" ]; then
        if python3 -c 'import fcntl, os, stat, tempfile' >/dev/null 2>&1; then
            CANCEL_PYTHON=1
        else
            CANCEL_PYTHON=0
        fi
    fi
    if [ "$CANCEL_PYTHON" != 1 ]; then
        say 'Python 3 with fcntl support is required to record the shared update state.'
        return 1
    fi
    python3 - "$CANCEL_STATE" "$1" <<'PY'
import fcntl, os, stat, sys, tempfile

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
tmp = None
try:
    fcntl.flock(lock_fd, fcntl.LOCK_EX)
    try:
        # Validate the state through the descriptor actually opened, not just
        # the path: O_NOFOLLOW rejects a symlink and fstat rejects a FIFO or
        # any other non-regular entry. A missing state is the first-run case.
        state_fd = safe_open(state_path, os.O_RDONLY)
    except FileNotFoundError:
        pass
    except OSError as exc:
        print(f"cancel_state_write: unsafe state path {state_path}: {exc}", file=sys.stderr)
        sys.exit(1)
    else:
        os.close(state_fd)
    # A unique file created exclusively in the same directory: a file or
    # symlink planted at a predictable temporary name can be neither opened
    # nor truncated by the replacement.
    fd, tmp = tempfile.mkstemp(prefix=os.path.basename(state_path) + ".tmp-",
                               dir=os.path.dirname(state_path) or ".")
    try:
        os.fchmod(fd, 0o600)
        os.write(fd, (value + "\n").encode("utf-8"))
        os.fsync(fd)
    finally:
        os.close(fd)
    os.replace(tmp, state_path)
    tmp = None
except OSError as exc:
    print(f"cancel_state_write: could not record {value!r} at {state_path}: {exc}", file=sys.stderr)
    sys.exit(1)
finally:
    if tmp is not None:
        try:
            os.unlink(tmp)
        except OSError:
            pass
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
    finally:
        os.close(lock_fd)
PY
    return $?
}

# The launcher's own log. What the user is told about a failure points here.
LOG=$DATA/launcher.log
log_line() {
    [ -d "$DATA" ] || return 0
    if [ -f "$LOG" ] && [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -gt 1048576 ]; then
        mv -f -- "$LOG" "$LOG.1" 2>/dev/null || true
    fi
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG" 2>/dev/null || true
}
say() { echo "roblox: $*" >&2; log_line "$*"; }
# keep_errors FILE STEP: pass a step's captured error output on to stderr and
# into the log.
keep_errors() {
    [ -s "$1" ] || return 0
    cat -- "$1" >&2
    while IFS= read -r _line || [ -n "$_line" ]; do log_line "$2: $_line"; done < "$1"
}
# reason_from FILE: the step's last error line, without tool prefixes.
reason_from() {
    sed -n '/[^[:space:]]/p' "$1" 2>/dev/null | tail -n 1 |
        sed -e 's/^roblox: //' -e 's/^! //' -e 's/^curl: ([0-9]*) //' -e 's/[.]$//'
}
# report_failure WHAT WHY TODO: what failed, why, what the user can do, and
# where the details are (B10 defect 2).
report_failure() {
    event failed 0 "$1: $2" 0 0 false false "$3" "$LOG"
    say "$1: $2. $3"
    notify "$1: $2. $3 Details: $LOG" "$1"
}
# event PHASE PERCENT MESSAGE [DONE] [TOTAL] [CANCELLABLE] [HIDDEN] [HINT] [LOG]
# HIDDEN=true asks the progress window to stay off screen until the next event
# (sent while the update prompt is open, so the prompt is never covered).
# HINT and LOG go with a failure: what the user can do, and the log's path;
# the window keeps a failure on screen until the user closes it.
event() {
    payload=$(python3 - "$1" "$2" "$3" "${4:-0}" "${5:-0}" "${6:-true}" "${7:-false}" "${8:-}" "${9:-}" <<'PY'
import json, sys
payload = {"phase": sys.argv[1], "percent": int(sys.argv[2]), "message": sys.argv[3],
           "bytes_done": int(sys.argv[4]), "bytes_total": int(sys.argv[5]),
           "cancellable": sys.argv[6].lower() == "true"}
if sys.argv[7].lower() == "true":
    payload["hidden"] = True
if sys.argv[8]:
    payload["hint"] = sys.argv[8]
if sys.argv[9]:
    payload["log"] = sys.argv[9]
print(json.dumps(payload, separators=(",", ":")))
PY
    )
    if [ "$EVENTS" = 1 ]; then
        printf '%s\n' "$payload"
    fi
    if [ "$UI_FD" = 1 ]; then printf '%s\n' "$payload" >&8 2>/dev/null || true; fi
}
start_progress() {
    [ -n "$UI_APP" ] || return 0
    # A port update mutates the managed port files and therefore requires the
    # shared protocol. A normal launch or client update can still run without
    # it (the port updater is the only component that relies on the state
    # file), so it degrades with a warning instead of refusing to start.
    require_cancel_state=${1:-}
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
        if [ "$require_cancel_state" = required ]; then
            say 'Could not reset the shared update state; not starting the operation.'
            return 1
        fi
        say 'warning: the shared update state is unavailable; continuing without it.'
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
    # The redirection is scoped to the group. On a bare `exec` it is permanent:
    # it sent this script's stderr, and the launched game's, to /dev/null for
    # the rest of the run.
    { exec 8>&-; } 2>/dev/null || true
    if [ -n "$UI_PID" ]; then
        wait "$UI_PID" 2>/dev/null; ui_result=$?
        UI_RESULT=$ui_result
        [ -e "${ROBLOX_MAC_CANCEL_FILE:-}" ] && UI_CANCELLED=1
        # 0 done, 4 cancelled, 1 the producer reported a failure or ended
        # without an outcome (its caller reports that). Only 3 means the
        # window could not be created. By now the update decision has been
        # made either way, so none of these says anything about updating.
        case "$ui_result" in
            0|1|4) ;;
            3) notify 'The progress window could not be shown; Roblox continues without it.' 'Roblox';;
            *) say "progress window exited with status $ui_result";;
        esac
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

# ask_update CLIENT_LATEST CLIENT_INSTALLED PORT_LATEST PORT_INSTALLED: one
# prompt for every update that is ready (an empty LATEST means none of that
# kind). Returns 0 update now, 1 launch without updating, 125 cancel the
# launch (nothing is updated, no game starts). "Skip this version" skips
# every listed update. The prompt has no timer (B10 flow 7, 8 and 9).
ask_update() {
    client_latest=$1
    client_installed=$2
    port_latest=$3
    port_installed=$4
    if [ -n "$UI_APP" ]; then
        # Readable client versions where they are known.
        client_shown=$client_latest
        installed_shown=$client_installed
        if [ -n "$client_latest" ]; then
            shown=$(status_json 2>/dev/null | python3 -c 'import json, sys
latest, installed = sys.argv[1], sys.argv[2]
try:
    status = json.load(sys.stdin)
except Exception:
    status = {}
print(status.get("latest_display_version") if status.get("latest_known") == latest and status.get("latest_display_version") else latest)
print(status.get("installed_display_version") or installed)' "$client_latest" "$client_installed" 2>/dev/null)
            if [ -n "$shown" ]; then
                client_shown=$(printf '%s\n' "$shown" | sed -n 1p)
                installed_shown=$(printf '%s\n' "$shown" | sed -n 2p)
            fi
        else
            installed_shown=
        fi
        event prompt 25 'Waiting for an answer to the update prompt' 0 0 false true
        "$UI_APP" --bootstrapper-ui --dialog "$client_shown" "$installed_shown" "$port_latest" "$port_installed" 9>&- >/dev/null 2>&1
        choice=$?
        case "$choice" in
            0) return 0;;
            1) return 1;;
            2)
                [ -z "$client_latest" ] || settings_set skipped_version "$client_latest" >/dev/null 2>&1 || true
                [ -z "$port_latest" ] || settings_set skipped_port_version "$port_latest" >/dev/null 2>&1 || true
                return 1;;
            4) return 125;;
        esac
        notify "The update prompt failed (status $choice); launching without updating." 'Roblox update'
        return 1
    fi
    what=
    [ -z "$client_latest" ] || what="Roblox $client_latest (installed: ${client_installed:-none})"
    if [ -n "$port_latest" ]; then
        [ -z "$what" ] || what="$what and "
        what="${what}the port $port_latest (installed: ${port_installed:-unknown})"
    fi
    if [ -t 0 ] && [ -t 1 ]; then
        say "An update is available: $what."
        say 'Choose: 1) Update now  2) Launch without updating  3) Cancel  [Enter = 2]'
        if ! IFS= read -r answer </dev/tty 2>/dev/null; then return 125; fi
        case "$answer" in
            1) return 0;;
            ''|2) return 1;;
            *) return 125;;
        esac
    fi
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title 'Roblox update available' \
            --yes-label 'Update now' --no-label 'Launch without updating' --cancel-label 'Cancel' \
            --yesnocancel "An update is available: $what. Update now?" >/dev/null 2>&1
        case $? in 0) return 0;; 1) return 1;; *) return 125;; esac
    fi
    if command -v zenity >/dev/null 2>&1; then
        # The extra button prints its label; Cancel and closing print nothing.
        zenity_answer=$(zenity --question --title='Roblox update available' \
            --ok-label='Update now' --cancel-label='Cancel' --extra-button='Launch without updating' \
            --text="An update is available: $what. Update now?" 2>/dev/null)
        case $? in
            0) return 0;;
            *) [ "$zenity_answer" = 'Launch without updating' ] && return 1; return 125;;
        esac
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
    first_errors=$DATA/.first-run-errors.$$
    (unset ROBLOX_MAC_ALIAS_DIR ROBLOX_MAC_STAGE ROBLOX_MAC_REAL_DATA
     timeout --foreground --kill-after=5s "${ROBLOX_MAC_FIRST_RUN_TIMEOUT:-120}" sh -c \
        'exec 9>&-; exec "$@"' sh env ROBLOX_MAC_LOCK_HELD=1 sh "$HERE/run.sh" --shell true) 2>"$first_errors"
    first_status=$?
    keep_errors "$first_errors" first-run
    if [ "$first_status" -ne 0 ]; then
        if [ "$first_status" -eq 124 ]; then
            why="it did not finish within ${ROBLOX_MAC_FIRST_RUN_TIMEOUT:-120} seconds"
        else
            why=$(reason_from "$first_errors")
            [ -n "$why" ] || why="it ended with status $first_status"
        fi
        rm -f -- "$first_errors"
        report_failure 'Roblox first-run setup failed' "$why" 'Roblox was not started. Start Roblox again.'
        return 1
    fi
    rm -f -- "$first_errors"
    tmp=$DATA/.first-run.initialized.$$
    printf 'initialized\n' > "$tmp"
    chmod 600 "$tmp"
    mv -f "$tmp" "$MARKER"
    event first-run 15 'First-run setup complete' 0 0 false
}

now_ms() {
    _ms=$(date +%s%3N 2>/dev/null)
    case "$_ms" in ''|*[!0-9]*) echo $(( $(date +%s) * 1000 ));; *) echo "$_ms";; esac
}
# The launch waits for its update lookups until this moment at most.
lookup_deadline() {
    if [ -z "${LOOKUP_DEADLINE_MS:-}" ]; then
        _grace_ms=$(awk -v grace="${ROBLOX_MAC_UPDATE_CHECK_GRACE:-1}" 'BEGIN { t = int(grace * 1000); print (t > 0 ? t : 0) }' 2>/dev/null || echo 1000)
        LOOKUP_DEADLINE_MS=$(( $(now_ms) + _grace_ms ))
    fi
    echo "$LOOKUP_DEADLINE_MS"
}

# check_latest [wait]: print the latest client version known after this call.
# The lookup runs in the background. With "wait" (Settings' "Check for
# updates", an explicit update) the answer is awaited; otherwise (a launch)
# it is awaited for at most ROBLOX_MAC_UPDATE_CHECK_GRACE seconds (default 1)
# and the launch goes on: the background lookup still records its answer, so
# the update is offered at the next start (B10 flow 6). Returns 0 with a
# fresh or not-yet-due answer, 1 when the lookup failed, 125 when cancelled.
check_latest() {
    if cancelled; then return 125; fi
    now=$(date +%s)
    due=$(python3 "$HELPER" due "$DATA" "$now" 2>/dev/null || echo 1)
    if [ "$due" != 1 ]; then
        settings_get last_known_latest
        return 0
    fi
    settings_set last_check "$now" >/dev/null 2>&1 || true
    check_result_file=$DATA/.check.$$
    rm -f -- "$check_result_file" "$check_result_file.tmp" "$check_result_file.abandoned"
    # The lookup must not keep the instance lock (9) or the progress pipe (8)
    # open, and must not hold this script's output open after a launch.
    (
        exec 8>&- 9>&-
        check_timeout=${ROBLOX_MAC_UPDATE_CHECK_TIMEOUT:-8}
        check_output=$(timeout "$check_timeout" "$APP" --client-version 2>"$check_result_file.err")
        check_status=$?
        found=$(printf '%s\n' "$check_output" | sed -n '/^version-[0-9a-f][0-9a-f]*$/p' | tail -n 1)
        if [ "$check_status" -eq 0 ] && [ -n "$found" ]; then
            settings_set last_known_latest "$found" >/dev/null 2>&1 || true
            settings_set last_check_error '' >/dev/null 2>&1 || true
            printf 'ok %s\n' "$found" > "$check_result_file.tmp"
        else
            # Keep the reason for the Settings page and the log (B10 defect 2).
            if [ "$check_status" -eq 124 ]; then
                why="Roblox's version service did not answer within $check_timeout seconds"
            else
                why=$(reason_from "$check_result_file.err")
                [ -n "$why" ] || why="the version lookup ended with status $check_status"
            fi
            settings_set last_check_error "$why" >/dev/null 2>&1 || true
            while IFS= read -r _line || [ -n "$_line" ]; do log_line "update check: $_line"; done < "$check_result_file.err"
            log_line "Could not check for Roblox updates: $why"
            printf 'failed %s\n' "$found" > "$check_result_file.tmp"
        fi
        rm -f -- "$check_result_file.err"
        mv -f -- "$check_result_file.tmp" "$check_result_file"
        if [ "$check_status" -eq 0 ] && [ -n "$found" ]; then
            display=$(timeout "$check_timeout" "$APP" --client-version-display 2>/dev/null | sed -n '/^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$/p' | tail -n 1 || true)
            [ -z "$display" ] || settings_set last_known_latest_display "$display" >/dev/null 2>&1 || true
        fi
        # Nobody will read an answer the launch stopped waiting for.
        [ -e "$check_result_file.abandoned" ] && rm -f -- "$check_result_file" "$check_result_file.abandoned"
        exit 0
    ) </dev/null >/dev/null 2>&1 &
    check_pid=$!
    deadline=$(lookup_deadline)
    while :; do
        if cancelled; then
            kill "$check_pid" 2>/dev/null || true; wait "$check_pid" 2>/dev/null || true
            rm -f -- "$check_result_file" "$check_result_file.tmp" "$check_result_file.abandoned"
            return 125
        fi
        if [ "${1:-}" = wait ]; then
            kill -0 "$check_pid" 2>/dev/null || break
        else
            [ -e "$check_result_file" ] && break
            kill -0 "$check_pid" 2>/dev/null || break
            if [ "$(now_ms)" -ge "$deadline" ]; then
                # Not answered in time: launch now, offer the update next start.
                : > "$check_result_file.abandoned"
                [ -e "$check_result_file" ] && rm -f -- "$check_result_file" "$check_result_file.abandoned"
                settings_get last_known_latest
                return 0
            fi
        fi
        sleep 0.05
    done
    [ "${1:-}" = wait ] && wait "$check_pid" 2>/dev/null
    answer=$(cat "$check_result_file" 2>/dev/null || true)
    rm -f -- "$check_result_file" "$check_result_file.abandoned"
    case "$answer" in
        "ok "*) printf '%s\n' "${answer#ok }"; return 0;;
    esac
    found=${answer#failed}; found=${found# }
    if [ -n "$found" ]; then printf '%s\n' "$found"; else settings_get last_known_latest; fi
    return 1
}

# Port releases are offered at launch like client updates (B10 flow 9).
# start_port_lookup begins the background release query when it is due;
# finish_port_lookup prints the latest known port release, waiting for the
# query no longer than the launch's lookup deadline. A late answer is still
# recorded (port_latest), so it is offered at the next start.
PORT_LOOKUP_PID=
PORT_LOOKUP_FILE=
start_port_lookup() {
    [ -f "$HERE/installer.py" ] || return 0
    now=$(date +%s)
    due=$(python3 "$HELPER" due "$DATA" "$now" port_last_check 2>/dev/null || echo 1)
    [ "$due" = 1 ] || return 0
    PORT_LOOKUP_FILE=$DATA/.port-check.$$
    rm -f -- "$PORT_LOOKUP_FILE" "$PORT_LOOKUP_FILE.tmp" "$PORT_LOOKUP_FILE.abandoned"
    lookup_deadline >/dev/null
    (
        exec 8>&- 9>&-
        found=$(python3 "$HERE/installer.py" release-status --prefix "$HERE" 2>/dev/null |
            python3 -c 'import json, sys
try:
    print(json.load(sys.stdin).get("latest_version") or "")
except Exception:
    pass' 2>/dev/null)
        if [ -n "$found" ]; then
            settings_set port_latest "$found" >/dev/null 2>&1 || true
            settings_set port_last_check "$(date +%s)" >/dev/null 2>&1 || true
            printf 'ok %s\n' "$found" > "$PORT_LOOKUP_FILE.tmp"
        else
            printf 'failed\n' > "$PORT_LOOKUP_FILE.tmp"
        fi
        mv -f -- "$PORT_LOOKUP_FILE.tmp" "$PORT_LOOKUP_FILE"
        [ -e "$PORT_LOOKUP_FILE.abandoned" ] && rm -f -- "$PORT_LOOKUP_FILE" "$PORT_LOOKUP_FILE.abandoned"
        exit 0
    ) </dev/null >/dev/null 2>&1 &
    PORT_LOOKUP_PID=$!
}
finish_port_lookup() {
    if [ -n "$PORT_LOOKUP_PID" ]; then
        deadline=$(lookup_deadline)
        while [ ! -e "$PORT_LOOKUP_FILE" ] && kill -0 "$PORT_LOOKUP_PID" 2>/dev/null; do
            if cancelled || [ "$(now_ms)" -ge "$deadline" ]; then
                : > "$PORT_LOOKUP_FILE.abandoned"
                break
            fi
            sleep 0.05
        done
        if [ -e "$PORT_LOOKUP_FILE" ]; then
            rm -f -- "$PORT_LOOKUP_FILE" "$PORT_LOOKUP_FILE.abandoned"
        fi
        PORT_LOOKUP_PID=
    fi
    settings_get port_latest
}

do_update() {
    if cancelled; then event cancelled 0 'Update cancelled' 0 0 false; return 125; fi
    event downloading 25 'Downloading Roblox' 0 0 true
    # The bootstrapper keeps FD 9; updater children must not inherit it.
    update_errors=$DATA/.update-errors.$$
    timeout --foreground --kill-after=5s "${ROBLOX_MAC_UPDATE_TIMEOUT:-30m}" sh -c \
        'exec 9>&-; exec "$@"' sh env ROBLOX_MAC_LOCK_HELD=1 \
            ROBLOX_MAC_PROGRESS_FD=8 ROBLOX_MAC_CANCEL_FILE="${ROBLOX_MAC_CANCEL_FILE:-}" \
            sh "$HERE/update-roblox.sh" 2>"$update_errors"
    update_status=$?
    keep_errors "$update_errors" update
    if [ "$update_status" -ne 0 ]; then
        if cancelled; then
            rm -f -- "$update_errors"
            event cancelled 0 'Update cancelled; keeping the installed client' 0 0 false
            say 'Update cancelled; the installed client was kept.'
            notify 'Update cancelled; the installed client was kept. The browser link was not launched.' 'Roblox update cancelled'
            return 125
        fi
        if [ "$update_status" -eq 124 ]; then
            why="it did not finish within ${ROBLOX_MAC_UPDATE_TIMEOUT:-30m}"
        else
            why=$(reason_from "$update_errors")
            [ -n "$why" ] || why="the updater ended with status $update_status"
        fi
        rm -f -- "$update_errors"
        report_failure 'Roblox update failed' "$why" 'The installed client was kept. Check your internet connection and try again later.'
        return 1
    fi
    rm -f -- "$update_errors"
    if cancelled; then event cancelled 0 'Update cancelled' 0 0 false; return 125; fi
    event verifying 78 'Verifying the downloaded client' 0 0 true
    event preparing 90 'Preparing shaders' 0 0 true
    event update 100 'Update complete' 0 0 false
}

do_port_update() {
    if [ ! -f "$HERE/installer.py" ]; then
        report_failure 'Port update failed' 'this release does not include the port updater (installer.py)' 'Install the latest release from the project page.'
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
    port_errors=$DATA/.port-update-errors.$$
    env ROBLOX_MAC_PROGRESS_FD=8 \
        ROBLOX_MAC_CANCEL_FILE="${ROBLOX_MAC_CANCEL_FILE:-}" \
        ROBLOX_MAC_CANCEL_STATE_FILE="${ROBLOX_MAC_CANCEL_STATE_FILE:-}" \
        ROBLOX_MAC_LOCK_HELD=1 \
        python3 "$HERE/installer.py" update --prefix "$HERE" --yes --no-desktop 2>"$port_errors"
    _port_update_status=$?
    keep_errors "$port_errors" port-update
    port_why=$(reason_from "$port_errors")
    rm -f -- "$port_errors"
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
    [ -n "$port_why" ] || port_why="the installer ended with status $_port_update_status"
    report_failure 'Port update failed' "$port_why" 'The current release was kept. Check your internet connection and try again later.'
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
        first_run || { exec 9>&-; finish_progress; exit 1; }
        event checking 25 'Checking for updates'
        latest=$(check_latest wait); check_result=$?
        [ "$check_result" -eq 125 ] && { finish_progress; notify 'Update check cancelled.' 'Roblox'; exit 125; }
        [ "$check_result" -eq 0 ] || { event failed 0 "Could not check for updates: $(settings_get last_check_error 2>/dev/null || true)" 0 0 false false 'Check your internet connection and try again.' "$LOG"; exec 9>&-; finish_progress; status_json; exit 1; }
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
        first_run || { exec 9>&-; finish_progress; exit 1; }
        event checking 25 'Checking for updates'
        latest=$(check_latest wait); check_result=$?
        [ "$check_result" -eq 125 ] && { finish_progress; exit 125; }
        [ "$check_result" -eq 0 ] || { event failed 0 "Could not check for updates: $(settings_get last_check_error 2>/dev/null || true)" 0 0 false false 'Check your internet connection and try again.' "$LOG"; exec 9>&-; finish_progress; exit 1; }
        [ -n "$latest" ] || { say 'No latest version is known; update check failed.'; finish_progress; exit 1; }
        do_update; result=$?; exec 9>&-; finish_progress; [ "$UI_CANCELLED" -eq 1 ] && result=125; exit "$result";;
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
        start_progress required || { exec 9>&-; exit 1; }
        do_port_update; result=$?; exec 9>&-; finish_progress
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
first_run || { exec 9>&-; finish_progress; exit 1; }
installed=$(installed_version)
event checking 25 'Checking for updates'
mode=$(settings_get auto_update 2>/dev/null || printf 'ask')
# A launch restarted after a port update already decided about updates.
restarted=${ROBLOX_MAC_PORT_RESTARTED:-0}
unset ROBLOX_MAC_PORT_RESTARTED
[ "$mode" = off ] || [ "$restarted" = 1 ] || start_port_lookup
latest=$(check_latest)
check_result=$?
port_latest=$(finish_port_lookup)
if [ "$check_result" -eq 125 ] || [ "$UI_CANCELLED" -eq 1 ]; then
    finish_progress
    notify 'Update cancelled; the installed client was kept. The browser link was not launched.' 'Roblox update cancelled'
    exit 125
fi
client_offer=
port_offer=
port_installed=
if [ -n "$latest" ] && [ "$latest" != "$installed" ] && [ "$(settings_get skipped_version 2>/dev/null || true)" != "$latest" ]; then
    client_offer=$latest
fi
if [ "$restarted" != 1 ] && [ -f "$HERE/installer.py" ] && [ -n "$port_latest" ]; then
    port_installed=$(python3 "$HELPER" port-version "$HERE" 2>/dev/null || true)
    if [ -n "$port_installed" ] && [ "$port_latest" != "$port_installed" ] &&
       [ "$(settings_get skipped_port_version 2>/dev/null || true)" != "$port_latest" ]; then
        port_offer=$port_latest
    fi
fi
[ -n "$port_offer" ] || port_installed=
answer=1
if [ -n "$client_offer$port_offer" ]; then
    case "$mode" in
        auto) answer=0;;
        ask) ask_update "$client_offer" "$installed" "$port_offer" "$port_installed"; answer=$?;;
    esac
fi
if [ "$answer" -eq 125 ]; then
    event cancelled 0 'Launch cancelled' 0 0 false
    finish_progress
    say 'Launch cancelled at the update prompt; nothing was updated.'
    exit 125
fi
if [ "$answer" -eq 0 ] && [ -n "$client_offer" ]; then
    do_update; result=$?
    [ "$result" -eq 125 ] && { finish_progress; notify 'Update cancelled; the installed client was kept. The browser link was not launched.' 'Roblox update cancelled'; exit 125; }
fi
if [ "$answer" -eq 0 ] && [ -n "$port_offer" ]; then
    do_port_update; result=$?
    case "$result" in
        0)
            # The port update replaced the launcher scripts and the AppImage.
            # Start again through the new ones with the same place id or link
            # (B10 flow 10); the instance lock is taken again by the new run.
            finish_progress
            say 'Port updated; starting Roblox with the new release.'
            exec 9>&-
            ROBLOX_MAC_PORT_RESTARTED=1
            export ROBLOX_MAC_PORT_RESTARTED
            exec sh "$HERE/run.sh" "$@";;
        125)
            finish_progress
            notify 'Port update cancelled; the current release was kept. Roblox was not started.' 'Roblox update cancelled'
            exit 125;;
        *)
            say 'Roblox is starting with the current release.';;
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
