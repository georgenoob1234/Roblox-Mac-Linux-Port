#!/bin/sh
# Build the framework stubs Darling doesn't ship, straight into the vendored macOS root.
# Host clang targets Mach-O directly (clang 22 + ld64.lld) — no Darwin SDK needed, the only
# external symbol is NSObject and that comes from Darling's own Foundation.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${DARLING_ROOT:-$(CDPATH= cd -- "$HERE/../.." && pwd)/darling-root}
FW=$ROOT/System/Library/Frameworks

for src in "$HERE"/*.m; do
    name=$(basename "$src" .m)
    out=$FW/$name.framework/Versions/A
    mkdir -p "$out"
    clang --target=x86_64-apple-macos11 -fuse-ld=lld -dynamiclib \
        -isysroot "$ROOT" -nostdlibinc \
        -install_name "/System/Library/Frameworks/$name.framework/Versions/A/$name" \
        -compatibility_version 1.0.0 -current_version 1.0.0 \
        -F "$FW" -framework Foundation \
        -o "$out/$name" "$src"
    ln -sfn A "$FW/$name.framework/Versions/Current"
    ln -sfn Versions/Current/"$name" "$FW/$name.framework/$name"
    echo "built $name"
done
