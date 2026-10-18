#!/bin/sh
# Package the runtime and updater; users download the client on first setup.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOP=$(CDPATH= cd -- "$HERE/../../.." && pwd)
OUT=${1:-$HERE/../../../RobloxLinuxRelease}
[ ! -e "$OUT" ] || { echo "Output already exists: $OUT" >&2; exit 1; }
sh "$HERE/build-appimage.sh"
mkdir -p "$(dirname -- "$OUT")"
stage=$(mktemp -d "$(dirname -- "$OUT")/.release.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
cp "$HERE/roblox-mac-x86_64.AppImage" "$stage/RobloxLinux.AppImage"
cp "$HERE/update-roblox.sh" "$HERE/run.sh" "$stage/"
cp "$HERE/bootstrap.sh" "$HERE/bootstrap.py" "$stage/"
cp "$TOP/installer/installer.py" "$stage/installer.py"
cp "$HERE/install-uri-handler.sh" "$stage/install-uri-handler.sh"
cp "$HERE/icon.png" "$stage/icon.png"
cp "$HERE/icon_bw.png" "$stage/icon_bw.png"
cp "$HERE/README.md" "$stage/README.md"
cp "$HERE/.gitignore" "$stage/.gitignore"
port_version=${ROBLOX_MAC_PORT_VERSION:-}
if [ -z "$port_version" ]; then
    port_version=$(git -C "$TOP" describe --tags --exact-match 2>/dev/null || git -C "$TOP" rev-parse --short HEAD)
fi
printf '%s\n' "$port_version" > "$stage/.roblox-mac-port-version"
printf '{}\n' > "$stage/FFlags.json"
mv "$stage" "$OUT"
echo "Release ready: $OUT"
