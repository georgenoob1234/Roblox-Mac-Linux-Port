#!/bin/sh
# Terminal launcher with diagnostics. --diagnose checks the system without starting Roblox.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# AppImageLauncher can relocate the image or run a memfd whose parent is /.
# Keep the original image and let uruntime name its cache inside our private tmp.
umask 077
mkdir -p "$HERE/DO_NOT_SHARE/tmp"
chmod 700 "$HERE/DO_NOT_SHARE"
export APPIMAGELAUNCHER_DISABLE=1 _FORCE_HEADLESS=1
export TARGET_APPIMAGE=$HERE/RobloxLinux.AppImage
unset APPIMAGE_TARGET_DIR
export TMPDIR=$HERE/DO_NOT_SHARE/tmp TMP=$HERE/DO_NOT_SHARE/tmp TEMP=$HERE/DO_NOT_SHARE/tmp
# Capture the session bus before the runtime redirects HOME/XDG_* inside its private prefix.
export ROBLOX_MAC_NOTIFY_DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-}
export ROBLOX_MAC_NOTIFY_XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-}
export ROBLOX_MAC_NOTIFY_HOME=${HOME:-}
export ROBLOX_MAC_HOST_HOME=${ROBLOX_MAC_HOST_HOME:-${HOME:-}}
export ROBLOX_MAC_HOST_CONFIG=${ROBLOX_MAC_HOST_CONFIG:-${XDG_CONFIG_HOME:-${HOME:-}/.config}}
export ROBLOX_MAC_HOST_DATA_DIRS=${ROBLOX_MAC_HOST_DATA_DIRS:-${XDG_DATA_DIRS:-/usr/local/share:/usr/share}}
[ -f "$HERE/icon.png" ] && export ROBLOX_MAC_ICON_PATH=${ROBLOX_MAC_ICON_PATH:-$HERE/icon.png}
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
export ROBLOX_MAC_UI_APP=$TARGET_APPIMAGE
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
    _short_base=/tmp
    if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "${XDG_RUNTIME_DIR:-}" ] && [ -w "${XDG_RUNTIME_DIR:-}" ]; then
        _xlen=$(LC_ALL=C printf %s "$XDG_RUNTIME_DIR" | wc -c)
        [ "$_xlen" -le 32 ] && _short_base=$XDG_RUNTIME_DIR
    fi
    _app_target=$(mktemp -d "$_short_base/rbx-mnt.XXXXXX")
    chmod 700 "$_app_target"
    export APPIMAGE_TARGET_DIR=$_app_target
    cleanup_target_dir() { rm -rf -- "$_app_target" 2>/dev/null || true; }
    trap cleanup_target_dir EXIT INT TERM HUP
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
