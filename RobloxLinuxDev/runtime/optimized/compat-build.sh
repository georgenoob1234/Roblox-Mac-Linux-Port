#!/bin/sh
# Build only this supplement; the parent owns launcher/library loading policy.
set -eu
umask 077
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
SDK=$ROOT/src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
DR=${DARLING_ROOT:-$ROOT/darling-root}
OUT=${OUT:-$HERE}
case "${1:-}" in ''|--check|--check-crash) ;; *) echo "Usage: $0 [--check|--check-crash]" >&2; exit 2;; esac
mkdir -p "$OUT/compat-tmp"
export TMPDIR=$OUT/compat-tmp
build() {
    clang --target=x86_64-apple-macos11 -O2 -march=x86-64 -mtune=generic -fblocks -fobjc-exceptions \
        -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 -DTARGET_OS_WASI=0 -fuse-ld=lld -Wl,-no_adhoc_codesign \
        -Wall -Wextra -Wno-unused-parameter -Wno-nullability-completeness \
        -isysroot "$DR" -nostdlibinc -isystem "$SDK/usr/include" \
        -F "$SDK/System/Library/Frameworks" -F "$DR/System/Library/Frameworks" \
        -framework Foundation -framework AppKit -framework Metal \
        -framework AVFoundation -framework UserNotifications -framework GameController \
        "$@"
}
cc -O2 -shared -fPIC -Wall -Wextra "$HERE/video-host.c" -o "$OUT/libtracka-video-host.new.so" $(pkg-config --cflags --libs libavcodec libavutil libswscale) -Wl,--disable-new-dtags,-rpath,'$ORIGIN/../browser/lib'
build "$HERE/compat.m" "$HERE/video.m" "$HERE/crash.c" -dynamiclib -install_name @rpath/libtracka-compat.dylib -o "$OUT/libtracka-compat.new.dylib"
mv "$OUT/libtracka-video-host.new.so" "$OUT/libtracka-video-host.so"
mv "$OUT/libtracka-compat.new.dylib" "$OUT/libtracka-compat.dylib"
echo "built $OUT/libtracka-compat.dylib (x86_64, O2, generic)"
if [ "${1:-}" = --check ] || [ "${1:-}" = --check-crash ]; then
    build -x objective-c "$HERE/crash.c" -DTRACKA_CRASH_CHECK -o "$OUT/crash-check"
    python3 "$HERE/crash-check.py"
fi
if [ "${1:-}" = --check ]; then
    build "$HERE/video-check.m" -o "$OUT/video-check"
    python3 "$HERE/video-check.py"
    build "$HERE/compat.m" -DTRACKA_COMPAT_CHECK -o "$OUT/compat-check"
    STATE=$(mktemp -d "$OUT/compat-tmp/check.XXXXXX")
    COMMAND=$(python3 - "$OUT" "$ROOT" <<'PY'
import shlex, sys
root = '/Volumes/SystemRoot' + sys.argv[1]
shared = '/Volumes/SystemRoot' + sys.argv[2] + '/shims'
env = ['env', 'DYLD_FORCE_FLAT_NAMESPACE=1', 'DYLD_LIBRARY_PATH=' + shared + '/metal',
       'DYLD_INSERT_LIBRARIES=' + shared + '/appkit/libappkitgaps.dylib:' + shared + '/metal/libmetalgaps.dylib:' + root + '/libtracka-compat.dylib']
print(' && '.join(shlex.join(env + [root + '/compat-check', mode])
                  for mode in ('host', 'write', 'read', 'remove', 'empty')))
PY
    )
    # Isolated preferences and no display sockets; never launches the client.
    timeout --kill-after=5s 45s env -u DISPLAY -u WAYLAND_DISPLAY ROBLOX_MAC_CONFIG=/dev/null ROBLOX_MAC_DATA="$STATE" \
        ROBLOX_MAC_WAYLAND=0 "$ROOT/bin/roblox-mac" --shell "$COMMAND" >"$STATE/run.log" 2>&1 || {
        echo "compat check failed; private log: $STATE/run.log" >&2
        exit 1
    }
    rg '^PASS ' "$STATE/run.log"
fi
