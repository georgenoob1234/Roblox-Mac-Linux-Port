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

say() { echo "roblox: $*" >&2; }
event() {
    if [ "$EVENTS" = 1 ]; then
        python3 "$HELPER" event "$1" "$2" "$3"
    fi
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
    event first-run 5 'Setting up for the first time'
    say 'Setting up Roblox for the first time; this may take a minute.'
    if ! timeout "${ROBLOX_MAC_FIRST_RUN_TIMEOUT:-120}" env ROBLOX_MAC_LOCK_HELD=1 "$APP" --shell true; then
        say 'First-run setup failed or timed out; the client was not launched.'
        notify 'Roblox first-run setup failed; the client was not launched.' 'Roblox setup failed'
        return 1
    fi
    tmp=$DATA/.first-run.initialized.$$
    printf 'initialized\n' > "$tmp"
    chmod 600 "$tmp"
    mv -f "$tmp" "$MARKER"
    event first-run 15 'First-run setup complete'
}

check_latest() {
    now=$(date +%s)
    due=$(python3 "$HELPER" due "$DATA" "$now" 2>/dev/null || echo 1)
    if [ "$due" != 1 ]; then
        settings_get last_known_latest
        return 0
    fi
    latest=$(timeout "${ROBLOX_MAC_UPDATE_CHECK_TIMEOUT:-8}" "$APP" --client-version 2>/dev/null \
        | sed -n '/^version-[0-9a-f][0-9a-f]*$/p' | tail -n 1 || true)
    settings_set last_check "$now" >/dev/null 2>&1 || true
    if [ -n "$latest" ]; then
        settings_set last_known_latest "$latest" >/dev/null 2>&1 || true
        printf '%s\n' "$latest"
    else
        settings_get last_known_latest
    fi
}

do_update() {
    event downloading 40 'Downloading Roblox'
    if ! timeout "${ROBLOX_MAC_UPDATE_TIMEOUT:-30m}" env ROBLOX_MAC_LOCK_HELD=1 sh "$HERE/update-roblox.sh"; then
        say 'Update failed or timed out; the installed client was kept.'
        notify 'Roblox update failed; the installed client was kept.' 'Roblox update failed'
        return 1
    fi
    event verifying 70 'Verifying the downloaded client'
    event 'preparing shaders' 88 'Preparing shaders'
    event update 100 'Update complete'
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
        first_run || exit 1
        event checking 25 'Checking for updates'
        latest=$(check_latest)
        printf '%s\n' "$latest"; exit 0;;
    update)
        EVENTS=1
        mkdir -p "$DATA"; exec 9>"$DATA/instance.lock"
        if ! flock -n -E 73 9; then
            say 'Roblox is already running.'; notify 'Roblox is already running.' 'Roblox is already running'; exit 0
        fi
        first_run || exit 1
        event checking 25 'Checking for updates'
        latest=$(check_latest)
        [ -n "$latest" ] || { say 'No latest version is known; update check failed.'; exit 1; }
        do_update; exit $?;;
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
first_run || exit 1
installed=$(installed_version)
event checking 25 'Checking for updates'
latest=$(check_latest)
mode=$(settings_get auto_update 2>/dev/null || printf 'ask')
if [ -n "$latest" ] && [ "$latest" != "$installed" ] && [ "$(settings_get skipped_version 2>/dev/null || true)" != "$latest" ]; then
    case "$mode" in
        auto) do_update || true;;
        ask) if ask_update "$latest" "$installed"; then do_update || true; fi;;
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
event launching 100 'Launching Roblox'
launch "$@"
