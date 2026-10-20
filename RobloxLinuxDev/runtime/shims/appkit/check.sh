#!/bin/sh
# Build and run the AppKit clipboard/keyboard regression harnesses inside the
# port's Darling guest. Uses only a fake clipboard, so the user's desktop
# clipboard is never touched.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
RUNTIME=$ROOT/RobloxLinuxDev/runtime
SRC=$RUNTIME/src/darling
SDK=$SRC/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
DR=${DARLING_ROOT:-$RUNTIME/darling-root}
OUT=${OUT:-$HERE/tmp/check}
LAUNCHER=${ROBLOX_MAC_LAUNCHER:-$RUNTIME/package/AppDir/bin/roblox-mac}
mkdir -p "$OUT"

sh "$HERE/build.sh"

for test in pasteboard-check input-check; do
    clang --target=x86_64-apple-macos11 -O2 -mtune=generic -fuse-ld=lld \
        -isysroot "$DR" -nostdlibinc -Wno-objc-root-class -Wno-nullability-completeness \
        -DTARGET_OS_WASI=0 -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 -fblocks \
        -isystem "$SDK/usr/include" -F "$SDK/System/Library/Frameworks" \
        -F "$DR/System/Library/Frameworks" -framework AppKit -framework Foundation \
        -framework CoreGraphics -L"$HERE" -lappkitgaps -Wl,-rpath,"$HERE" \
        -o "$OUT/$test" "$HERE/$test.m"
done

[ -x "$LAUNCHER" ] || {
    echo "FAIL: launcher not found: $LAUNCHER" >&2
    echo "Set ROBLOX_MAC_LAUNCHER to the port's bin/roblox-mac." >&2
    exit 1
}
G=/Volumes/SystemRoot
GUEST_OUT=$G$OUT
GUEST_SHIM=$G$HERE
for test in pasteboard-check input-check; do
    "$LAUNCHER" --shell "DYLD_LIBRARY_PATH=$GUEST_SHIM $GUEST_OUT/$test"
done