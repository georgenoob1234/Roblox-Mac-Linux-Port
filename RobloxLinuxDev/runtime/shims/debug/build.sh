#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DR=${DARLING_ROOT:-$(CDPATH= cd -- "$HERE/../.." && pwd)/darling-root}
clang --target=x86_64-apple-macos11 -fuse-ld=lld -dynamiclib -isysroot "$DR" -nostdlibinc \
    -install_name "@rpath/libthrowtrace.dylib" -o "$HERE/libthrowtrace.dylib" "$HERE/throwtrace.c" "$HERE/ui-trace.m" -F "$DR/System/Library/Frameworks" -framework AppKit -framework Foundation
echo "built $HERE/libthrowtrace.dylib"
