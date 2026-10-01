#!/bin/sh
# Run from any directory. Build dependencies are documented in README.md.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE"
export PYTHONDONTWRITEBYTECODE=1
export TMPDIR="$HERE/.build/tmp" CARGO_TARGET_DIR="$HERE/metal2vulkan/target" CARGO_HOME="$HERE/.build/cargo"
mkdir -p "$TMPDIR" "$HERE/runtime/logs"
python3 -B bootstrap.py
ROBLOX_MAC_PORTABLE_ROOT=$(python3 -B runtime/package/portable_runtime.py)
export ROBLOX_MAC_PORTABLE_ROOT
export ROBLOX_MAC_DATA="$HERE/.build/profile" ROBLOX_MAC_CONFIG=/dev/null
export ROBLOX_MAC_APP="$HERE/.build/client/RobloxPlayer.app"
ROBLOX_MAC_DATA="$HERE/.build/client" sh runtime/scripts/fetch-client.sh
sh runtime/scripts/m1-darling.sh
# Rebuild shims even when the sysroot already exists.
for shim in stubs appkit xattr setsid input dns cfnet identity debug metal; do
    sh "runtime/shims/$shim/build.sh"
done
cp third_party/metal2vulkan-Cargo.lock metal2vulkan/Cargo.lock
(cd metal2vulkan && cargo build --locked --release --features serde)
sh runtime/browser/build.sh
sh runtime/optimized/audio/build.sh
cp runtime/optimized/audio/build/CoreAudio.framework/Versions/A/CoreAudio runtime/darling-root/System/Library/Frameworks/CoreAudio.framework/Versions/A/CoreAudio
cp runtime/optimized/audio/build/CoreAudio.component/Contents/MacOS/CoreAudio runtime/darling-root/System/Library/Components/CoreAudio.component/Contents/MacOS/CoreAudio
# Inventory the final framework exports; earlier fill generation shadows the
# real CoreAudio clock with a zero-returning placeholder in the flat namespace.
sh runtime/shims/fill/build.sh
CFLAGS= CXXFLAGS= CPPFLAGS= LDFLAGS= cmake -S runtime/src/darling -B runtime/src/darling-o2 -G Ninja \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DDEBIAN_PACKAGING=ON -DJSC_UNIFIED_BUILD=ON -DTARGET_i386=OFF \
    -DCOMPONENTS=gui_frameworks,gui_stubs,cli -DENABLE_METAL=ON -DDARLING_NO_CCACHE=ON \
    -DCMAKE_BUILD_TYPE=O2 -DCMAKE_C_FLAGS_O2=-O2 -DCMAKE_CXX_FLAGS_O2=-O2 \
    "-DCMAKE_EXE_LINKER_FLAGS=-B\"$ROBLOX_MAC_PORTABLE_ROOT/usr/lib/\"" \
    -DCMAKE_OBJC_FLAGS_O2=-O2 -DCMAKE_OBJCXX_FLAGS_O2=-O2 \
    -DCMAKE_ASM_FLAGS_O2= -DCMAKE_ASM-ATT_FLAGS_O2= \
    -DVulkan_INCLUDE_DIR="$HERE/runtime/src/Vulkan-Headers-1.3.290/include" \
    -DVulkan_LIBRARY=/usr/lib/libvulkan.so.1
ninja -C runtime/src/darling-o2 -j "${BUILD_JOBS:-4}" src/external/libc/libsystem_c.dylib darlingserver
c++ -std=c++17 -O2 -pthread -include array -include cstring \
    -I runtime/src/darling/src/external/darlingserver/internal-include \
    runtime/src/darling/src/external/darlingserver/src/message.cpp \
    native/server-reply-check.cpp -o .build/server-reply-check
.build/server-reply-check
# Restore the complete optimized base runtime, not just our adapter libraries.
sh runtime/optimized/darling/build.sh
sh runtime/optimized/build.sh
python3 -B runtime/optimized/native/timezone-sidecar/check.py --library runtime/optimized/native/lib/libsystem_c.dylib
python3 -B runtime/package/check-release.py
sh runtime/package/build-appimage.sh
printf '\nBuilt: %s/runtime/package/roblox-mac-x86_64.AppImage\n' "$HERE"
