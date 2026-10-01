#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
V=$HERE/vendor/imgui-1.91.9b
mkdir -p "$HERE/build"
export TMPDIR=$HERE/build
set --
for source in "$HERE/profiler.cpp" "$V/imgui.cpp" "$V/imgui_draw.cpp" "$V/imgui_tables.cpp" "$V/imgui_widgets.cpp" "$V/backends/imgui_impl_opengl3.cpp"; do
 object=$HERE/build/$(basename "$source" .cpp).o
 if [ ! -f "$object" ] || [ "$source" -nt "$object" ] || [ "$HERE/api.h" -nt "$object" ] || [ "$HERE/model.h" -nt "$object" ] || [ "$HERE/resources.h" -nt "$object" ] || [ "$HERE/panel.h" -nt "$object" ] || [ "$HERE/accounting.h" -nt "$object" ] || [ "$HERE/gpu-usage.h" -nt "$object" ] || [ "$HERE/sampler.h" -nt "$object" ] || [ "$HERE/mappings.h" -nt "$object" ] || [ "$HERE/memory.h" -nt "$object" ]; then
  c++ -std=c++17 -O2 -fPIC -I "$V" -I "$HERE/../src/Vulkan-Headers-1.3.290/include" -c "$source" -o "$object"
 fi
 set -- "$@" "$object"
done
ar rcs "$HERE/build/libprofiler.a" "$@"
c++ -std=c++17 -O2 "$HERE/check.cpp" -o "$HERE/build/check"
"$HERE/build/check"
c++ -std=c++17 -O2 -rdynamic "$HERE/check-renderer.cpp" -ldl -o "$HERE/build/check-renderer"
"$HERE/build/check-renderer"
