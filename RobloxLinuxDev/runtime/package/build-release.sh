#!/bin/sh
# Package the runtime and updater; users download the client on first setup.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:-$HERE/../../../RobloxLinuxRelease}
[ ! -e "$OUT" ] || { echo "Output already exists: $OUT" >&2; exit 1; }
sh "$HERE/build-appimage.sh"
mkdir -p "$(dirname -- "$OUT")"
stage=$(mktemp -d "$(dirname -- "$OUT")/.release.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
cp "$HERE/roblox-mac-x86_64.AppImage" "$stage/RobloxLinux.AppImage"
cp "$HERE/update-roblox.sh" "$HERE/run.sh" "$stage/"
cp "$HERE/README.md" "$stage/README.md"
cp "$HERE/.gitignore" "$stage/.gitignore"
printf '{}\n' > "$stage/FFlags.json"
mv "$stage" "$OUT"
echo "Release ready: $OUT"
