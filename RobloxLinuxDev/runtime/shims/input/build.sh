#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DR=${DARLING_ROOT:-$(CDPATH= cd -- "$HERE/../.." && pwd)/darling-root}
mkdir -p "$HERE/build"
cc -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections "$HERE/check.c" -o "$HERE/build/input-check"
"$HERE/build/input-check"
clang --target=x86_64-apple-macos11 -fuse-ld=lld -dynamiclib \
    -isysroot "$DR" -nostdlibinc \
    -install_name "@rpath/librbxinput.dylib" \
    -F "$DR/System/Library/Frameworks" -framework CoreFoundation -o "$HERE/librbxinput.dylib" "$HERE/input.c"
echo "built $HERE/librbxinput.dylib"
