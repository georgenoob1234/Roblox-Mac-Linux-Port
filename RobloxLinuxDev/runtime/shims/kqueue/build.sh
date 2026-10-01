#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
OUT=${OUT:-$HERE}
export TMPDIR=$OUT/tmp
mkdir -p "$OUT" "$TMPDIR"
clang --target=x86_64-apple-macos11 -DPLATFORM_MacOSX -O2 -fuse-ld=lld -dynamiclib \
 -isysroot "$ROOT/darling-root" \
 -isystem "$ROOT/src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include" \
 -install_name @rpath/librbxkqueue.dylib -o "$OUT/librbxkqueue.new.dylib" "$HERE/kqueue.c" "$HERE/ulock.c" "$HERE/io.c" "$HERE/semaphore.c" "$HERE/thread-switch.c" "$HERE/workq.c" "$HERE/workq-stack.S"
mv "$OUT/librbxkqueue.new.dylib" "$OUT/librbxkqueue.dylib"
