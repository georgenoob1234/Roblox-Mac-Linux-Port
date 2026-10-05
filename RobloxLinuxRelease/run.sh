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
case "${1:-}" in
    --diagnose|--debug|--client-version|--client-version-display|--download-client|--prepare-shaders|--shell|--inside)
        exec "$HERE/RobloxLinux.AppImage" "$@";;
esac
[ -t 2 ] || export ROBLOX_MAC_NO_TERMINAL=1
exec "$HERE/bootstrap.sh" "$@"
