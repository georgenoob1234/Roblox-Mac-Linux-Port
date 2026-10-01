#!/bin/sh
# Reuse Track B sources verbatim; never write its build tree.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
A=$(CDPATH= cd -- "$HERE/../.." && pwd)
OUT=${OUT:-$HERE}
export OUT TMPDIR=$OUT/tmp
mkdir -p "$TMPDIR"
[ "${1:-}" = --adapter-only ] || sh "$A/../native/renderer/build.sh"
clang --target=x86_64-apple-macos11 -DPLATFORM_MacOSX -fblocks -O2 -fuse-ld=lld \
    -Wl,-no_adhoc_codesign -dynamiclib -isysroot "$A/darling-root" \
    -isystem "$A/src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include" \
    -F "$A/src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks" \
    -framework Foundation -framework AppKit -framework OpenGL \
    -install_name @rpath/libtracka-present.dylib \
    -o "$OUT/libtracka-present.new.dylib" "$HERE/presentation.m"
mv "$OUT/libtracka-present.new.dylib" "$OUT/libtracka-present.dylib"
chmod 755 "$OUT/libindium.dylib" "$OUT/libtracka-present.dylib"
printf '%s\n' "$OUT/libindium.dylib" "$OUT/libtracka-present.dylib"
