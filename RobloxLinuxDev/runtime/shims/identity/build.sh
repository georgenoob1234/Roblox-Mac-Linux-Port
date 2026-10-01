#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SDK=$ROOT/src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
export TMPDIR=$ROOT/logs
clang -D__DARWIN_ONLY_UNIX_CONFORMANCE=1 --target=x86_64-apple-macos11 -fuse-ld=lld -dynamiclib -O2 \
    -isysroot "$ROOT/darling-root" -nostdlibinc -isystem "$SDK/usr/include" \
    -install_name @rpath/librbxidentity.dylib -o "$HERE/librbxidentity.dylib" "$HERE/identity.c" "$HERE/memory.c"
