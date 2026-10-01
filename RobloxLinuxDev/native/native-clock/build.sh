#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A=$HERE/../../runtime
OUT=$HERE/../build/native-clock
export TMPDIR=$HERE/../build/tmp
mkdir -p "$OUT" "$TMPDIR"
clang --target=x86_64-apple-macos11 -O2 -DDARLING -DPRIVATE -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 -fblocks -fuse-ld=lld -Wl,-no_adhoc_codesign \
 -isysroot "$A/darling-root" -isystem "$A/src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include" \
 -I "$A/src/darling/src/external/libc/gen" -dynamiclib -install_name @rpath/libtrackb-clock.dylib \
 "$HERE/clock.c" -o "$OUT/libtrackb-clock.new.dylib"
mv "$OUT/libtrackb-clock.new.dylib" "$OUT/libtrackb-clock.dylib"
