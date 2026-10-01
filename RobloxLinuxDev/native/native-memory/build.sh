#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A=$HERE/../../runtime
SRC=$A/src/darling/src/external/libplatform
OUT=${OUT:-$HERE/../build/native-memory}
export TMPDIR=$OUT/tmp
mkdir -p "$OUT" "$TMPDIR"
# Darling routines are the default; the host can opt in to libc copy/compare.
clang --target=x86_64-apple-macos11 -O3 -fno-builtin -fno-strict-aliasing -DVARIANT_STATIC=1 -DPRIVATE -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 \
 -isysroot "$A/darling-root" -isystem "$A/src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include" \
 -I "$SRC/private" -I "$SRC/include" -dynamiclib -fuse-ld=lld -Wl,-no_adhoc_codesign \
 -install_name @rpath/libtrackb-memory.dylib -I "$SRC/src/string/generic" "$HERE/memory.c" "$SRC/src/string/generic/memchr.c" "$HERE/fill.c" "$SRC/src/string/generic/memset_pattern.c" -o "$OUT/libtrackb-memory.new.dylib"
mv "$OUT/libtrackb-memory.new.dylib" "$OUT/libtrackb-memory.dylib"
