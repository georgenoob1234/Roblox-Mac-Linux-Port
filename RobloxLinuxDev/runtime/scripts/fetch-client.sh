#!/bin/sh
# Keep $DATA/RobloxPlayer.app at the current x86_64 macOS client version.
# Download into staging first; a failed download must preserve the installed client.
set -eu
DATA=${ROBLOX_MAC_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/roblox-mac}
CHANNEL=${ROBLOX_CHANNEL:-MacPlayer}

V=$(curl -fsS "https://clientsettingscdn.roblox.com/v2/client-version/$CHANNEL" \
    | python3 -c 'import sys,json,re;v=json.load(sys.stdin)["clientVersionUpload"];assert re.fullmatch(r"version-[0-9a-f]+",v), "Invalid client version";print(v)')
if [ "${1:-}" = --version ]; then printf '%s\n' "$V"; exit 0; fi

if [ "$(cat "$DATA/.version" 2>/dev/null || true)" = "$V" ] && [ -x "$DATA/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ]; then
    echo "client up to date: $V" >&2
    exit 0
fi

echo "fetching client $V" >&2
mkdir -p "$DATA"
tmp=$(mktemp -d "$DATA/.dl.XXXXXX")
cleanup() {
    if [ -d "$tmp/previous.app" ] && [ ! -e "$DATA/RobloxPlayer.app" ]; then
        mv "$tmp/previous.app" "$DATA/RobloxPlayer.app"
    fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
curl -fsSL -o "$tmp/c.zip" "https://setup.rbxcdn.com/mac/$V-RobloxPlayer.zip"
python3 - "$tmp/c.zip" <<'PY'
import pathlib, stat, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    for entry in archive.infolist():
        path = pathlib.PurePosixPath(entry.filename)
        if path.is_absolute() or '..' in path.parts or not path.parts or path.parts[0] != 'RobloxPlayer.app' or stat.S_ISLNK(entry.external_attr >> 16):
            raise SystemExit('Unsafe client archive entry')
PY
unzip -qo "$tmp/c.zip" -d "$tmp"
[ -x "$tmp/RobloxPlayer.app/Contents/MacOS/RobloxPlayer" ] || { echo 'Client archive has no executable' >&2; exit 1; }
[ ! -e "$DATA/RobloxPlayer.app" ] || mv "$DATA/RobloxPlayer.app" "$tmp/previous.app"
if ! mv "$tmp/RobloxPlayer.app" "$DATA/RobloxPlayer.app"; then
    [ ! -e "$tmp/previous.app" ] || mv "$tmp/previous.app" "$DATA/RobloxPlayer.app"
    exit 1
fi
printf '%s' "$V" > "$DATA/.version"
echo "$V" >&2
