#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SRC=$ROOT/src/darling
DR=$ROOT/darling-root
export TMPDIR=$ROOT/logs
if [ "${1:-}" != --gaps ]; then
clang++ --target=x86_64-apple-macos11 -DPLATFORM_MacOSX -std=c++17 -fblocks -O2 -fuse-ld=lld -Wl,-no_adhoc_codesign -dynamiclib \
 -isysroot "$DR" -nostdinc++ -isystem "$SRC/src/external/libcxx/include" \
 -isystem "$SRC/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include" \
 -I "$ROOT/src/indium/include" -F "$DR/System/Library/Frameworks" -framework Foundation \
 -L "$DR/usr/lib/system" -lcommonCrypto -install_name /usr/lib/darling/libiridium.dylib \
 -o "$HERE/libiridium.dylib" "$HERE/iridium.mm"

clang++ --target=x86_64-apple-macos11 -DPLATFORM_MacOSX -std=c++17 -fblocks -O2 -fuse-ld=lld -Wl,-no_adhoc_codesign -dynamiclib -DDARLING \
 -isysroot "$DR" -nostdinc++ -isystem "$SRC/src/external/libcxx/include" \
 -isystem "$SRC/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include" \
 -I "$ROOT/src/indium/include" -I "$ROOT/src/indium/private-include" \
 -I "$ROOT/src/Vulkan-Headers-1.3.290/include" -L "$HERE" -liridium \
 -install_name /usr/lib/darling/libindium.dylib \
 -o "$HERE/libindium.dylib" "$ROOT"/src/indium/src/indium/*.cpp

fi

clang++ --target=x86_64-apple-macos11 -DPLATFORM_MacOSX -std=c++17 -fblocks -O2 -fuse-ld=lld -Wl,-no_adhoc_codesign -dynamiclib \
 -isysroot "$DR" -nostdinc++ -isystem "$SRC/src/external/libcxx/include" \
 -isystem "$SRC/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include" \
 -I "$ROOT/src/indium/include" -F "$DR/System/Library/Frameworks" -framework Foundation -framework Metal \
 -I "$ROOT/src/indium/private-include" -I "$ROOT/src/Vulkan-Headers-1.3.290/include" -L "$HERE" -lindium \
 -install_name @rpath/libmetalgaps.dylib -F "$SRC/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks" -I "$SRC/src/external/cocotron/QuartzCore" -framework QuartzCore -framework OpenGL -framework CoreGraphics \
 -o "$HERE/libmetalgaps.dylib" "$HERE/metal-gaps.mm" "$HERE/metal-libraries.mm" "$HERE/metal-blit.mm" "$HERE/metal-vertex.mm" "$HERE/metal-sampler.mm" "$HERE/metal-depth.mm" "$HERE/carenderer.mm" -L "$DR/usr/lib/system" -lcommonCrypto

python3 "$HERE/sort-weak-bindings.py" "$HERE/libiridium.dylib" "$HERE/libindium.dylib" "$HERE/libmetalgaps.dylib"

mkdir -p "$HERE/runtime-shaders"
for channels in 1 2 4; do
 glslangValidator --target-env vulkan1.2 -V -DCHANNELS=$channels "$HERE/runtime-blit.comp" -o "$HERE/runtime-shaders/blit$channels.spv"
 spirv-val --target-env vulkan1.2 "$HERE/runtime-shaders/blit$channels.spv"
done
