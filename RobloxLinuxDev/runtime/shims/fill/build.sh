#!/bin/sh
# Regenerate and build the fill dylib. Re-run after every client update.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
DR=${DARLING_ROOT:-$ROOT/darling-root}

python3 "$HERE/gen.py"
clang --target=x86_64-apple-macos11 -fuse-ld=lld -dynamiclib \
    -isysroot "$DR" -nostdlibinc -Wno-objc-root-class \
    -install_name "@rpath/librbxfill.dylib" \
    -F "$DR/System/Library/Frameworks" -framework Foundation \
    -o "$HERE/librbxfill.dylib" "$HERE/fill.m"
echo "built $HERE/librbxfill.dylib"
