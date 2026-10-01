#!/bin/sh
# M1 — produce the Darling userspace as plain files and prove it boots a Mach-O, host-native.
# No Docker, no root, no system install: the Ubuntu debs are unpacked with ar+bsdtar.
# Pass: sw_vers prints a macOS version.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LOG=$HERE/logs/m1-$(date +%s).log
mkdir -p "$HERE/logs"

if [ ! -d "$HERE/darling-root" ]; then
    ls "$HERE"/debs/*.deb >/dev/null 2>&1 || {
        echo "no debs in $HERE/debs — download the noble amd64 debs from Darling's GitHub releases" >&2
        exit 1
    }
    tmp=$(mktemp -d "$HERE/.debs.XXXXXX"); trap 'rm -rf "$tmp"' EXIT
    for d in "$HERE"/debs/*.deb; do
        ar p "$d" data.tar.zst | bsdtar -xf - -C "$tmp"
    done
    mv "$tmp/usr/libexec/darling" "$HERE/darling-root"
    cp "$tmp/usr/bin/darlingserver" "$HERE/darlingserver"
    cp "$tmp/usr/bin/darling"       "$HERE/darling-cli"
    chmod u+s "$HERE/darling-cli"   # setuid bit is a no-op inside the user namespace; kept so darling's own check passes
    # The stub frameworks live inside the root: rebuild them after regenerating it.
    # fill last: it skips whatever the hand-written shims already define.
    for s in stubs appkit xattr setsid input dns cfnet identity debug fill; do [ -x "$HERE/shims/$s/build.sh" ] && "$HERE/shims/$s/build.sh"; done
fi

"$HERE/bin/roblox-mac" --shell 'uname -a; sw_vers' 2>&1 | tee "$LOG"
echo "log: $LOG"
