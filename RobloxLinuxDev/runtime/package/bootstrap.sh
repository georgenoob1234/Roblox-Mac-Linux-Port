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
URI_MAX_AGE=${ROBLOX_MAC_URI_MAX_AGE:-600}
EVENTS=${ROBLOX_MAC_EVENTS:-0}
UI_APP=${ROBLOX_MAC_UI_APP:-}
UI_FD=0
UI_PID=
UI_FIFO=
UI_CANCELLED=0
UI_RESULT=0

cancelled() { [ -n "${ROBLOX_MAC_CANCEL_FILE:-}" ] && [ -e "$ROBLOX_MAC_CANCEL_FILE" ]; }

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
    ROBLOX_MAC_CANCEL_FILE=$DATA/.bootstrap-cancel
    export ROBLOX_MAC_CANCEL_FILE
    rm -f -- "$ROBLOX_MAC_CANCEL_FILE"
    UI_FIFO=$DATA/.bootstrap-progress.$$
    rm -f -- "$UI_FIFO"
    mkfifo "$UI_FIFO" 2>/dev/null || { UI_FIFO=; return 0; }
    # Read/write keeps startup non-blocking even when GTK is unavailable.
    exec 8<>"$UI_FIFO"
    "$UI_APP" --bootstrapper-ui --progress "$DATA" 8>&- <"$UI_FIFO" >/dev/null 2>&1 & UI_PID=$!
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
    [ "$UI_CANCELLED" -eq 1 ] || rm -f -- "${ROBLOX_MAC_CANCEL_FILE:-}" 2>/dev/null || true
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

launch() {
    uri=${ROBLOX_MAC_LAUNCH_URI:-}
    if [ -n "$uri" ]; then
        exec env ROBLOX_MAC_LOCK_HELD=1 "$APP" --uri "$uri" "$@"
    fi
    exec env ROBLOX_MAC_LOCK_HELD=1 "$APP" "$@"
}

ask_update() {
    latest=$1
    installed=$2
    if [ -n "$UI_APP" ]; then
        choice=$(timeout 31 "$UI_APP" --bootstrapper-ui --dialog "$latest" "$installed" >/dev/null 2>&1; printf '%s' "$?" )
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
    # The warm-up does not own the instance lock. Close FD 9 before exec so a
    # Darling/AppImage child cannot strand the lock if timeout has to kill it.
    if ! timeout --foreground --kill-after=5s "${ROBLOX_MAC_FIRST_RUN_TIMEOUT:-120}" sh -c \
        'exec 9>&-; exec "$@"' sh env ROBLOX_MAC_LOCK_HELD=1 sh "$HERE/run.sh" --shell true; then
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
    timeout "${ROBLOX_MAC_UPDATE_CHECK_TIMEOUT:-8}" "$APP" --client-version >"$check_output" 2>/dev/null &
    check_pid=$!
    while kill -0 "$check_pid" 2>/dev/null; do
        if cancelled; then kill "$check_pid" 2>/dev/null || true; wait "$check_pid" 2>/dev/null || true; rm -f -- "$check_output"; return 125; fi
        sleep 0.1
    done
    wait "$check_pid" 2>/dev/null || true
    latest=$(sed -n '/^version-[0-9a-f][0-9a-f]*$/p' "$check_output" 2>/dev/null | tail -n 1 || true)
    rm -f -- "$check_output"
    settings_set last_check "$now" >/dev/null 2>&1 || true
    if [ -n "$latest" ]; then
        settings_set last_known_latest "$latest" >/dev/null 2>&1 || true
        display=$(timeout "${ROBLOX_MAC_UPDATE_CHECK_TIMEOUT:-8}" "$APP" --client-version-display 2>/dev/null | sed -n '/^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$/p' | tail -n 1 || true)
        [ -z "$display" ] || settings_set last_known_latest_display "$display" >/dev/null 2>&1 || true
        printf '%s\n' "$latest"
    else
        settings_get last_known_latest
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
            say 'Update cancelled; the installed client was kept.'
            notify 'Update cancelled; the installed client was kept. The browser link was not launched.' 'Roblox update cancelled'
            return 125
        fi
        say 'Update failed or timed out; the installed client was kept.'
        notify 'Roblox update failed; the installed client was kept.' 'Roblox update failed'
        return 1
    fi
    if cancelled; then event cancelled 0 'Update cancelled' 0 0 false; return 125; fi
    event verifying 78 'Verifying the downloaded client' 0 0 true
    event preparing 90 'Preparing shaders' 0 0 true
    event update 100 'Update complete' 0 0 false
}

status_json() { python3 "$HELPER" status "$DATA" "$HERE/RobloxVersion"; }

command=${1:-launch}
case "$command" in
    status)
        status_json; exit 0;;
    get)
        [ "$#" -eq 2 ] || { echo 'Usage: run.sh get SETTING' >&2; exit 2; }
        settings_get "$2"; exit 0;;
    set)
        [ "$#" -eq 3 ] || { echo 'Usage: run.sh set SETTING VALUE' >&2; exit 2; }
        settings_set "$2" "$3"; exit 0;;
    check)
        mkdir -p "$DATA"; exec 9>"$DATA/instance.lock"
        if ! flock -n -E 73 9; then
            say 'Roblox is already running.'; notify 'Roblox is already running.' 'Roblox is already running'; exit 0
        fi
        start_progress
        first_run || { finish_progress; exit 1; }
        event checking 25 'Checking for updates'
        latest=$(check_latest); check_result=$?
        [ "$check_result" -eq 125 ] && { finish_progress; notify 'Update check cancelled.' 'Roblox'; exit 125; }
        finish_progress
        exec 9>&-
        status_json; exit 0;;
    update)
        EVENTS=1
        mkdir -p "$DATA"; exec 9>"$DATA/instance.lock"
        if ! flock -n -E 73 9; then
            say 'Roblox is already running.'; notify 'Roblox is already running.' 'Roblox is already running'; exit 0
        fi
        start_progress
        first_run || { finish_progress; exit 1; }
        event checking 25 'Checking for updates'
        latest=$(check_latest); check_result=$?
        [ "$check_result" -eq 125 ] && { finish_progress; exit 125; }
        [ -n "$latest" ] || { say 'No latest version is known; update check failed.'; finish_progress; exit 1; }
        do_update; result=$?; finish_progress; [ "$UI_CANCELLED" -eq 1 ] && result=125; exit "$result";;
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
start_progress
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
