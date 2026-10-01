#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DR=${DARLING_ROOT:-$(CDPATH= cd -- "$HERE/../.." && pwd)/darling-root}
clang --target=x86_64-apple-macos11 -fuse-ld=lld -dynamiclib \
    -isysroot "$DR" -nostdlibinc \
    -install_name "@rpath/libnosetsid.dylib" \
    -o "$HERE/libnosetsid.dylib" "$HERE/setsid.c"
echo "built $HERE/libnosetsid.dylib"
