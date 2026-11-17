#!/bin/sh
# shaders_metal_osx.pack -> out/air/*.air -> out/spv/*.spv + *.json through metal2vulkan.
# Prints the pass/fail count and the distinct failure reasons. Re-run after every client update.
#   ./translate-all.sh [path/to/RobloxPlayer.app]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
APP=${1:-${ROBLOX_MAC_APP:-$ROOT/RobloxPlayer.app}}
M2V=${ROBLOX_MAC_TRANSLATOR:-$ROOT/metal2vulkan/target/release/metal2vulkan}
[ ! -x "$HERE/../usr/bin/metal2vulkan" ] || M2V=$HERE/../usr/bin/metal2vulkan
for tool in python3 llvm-dis spirv-val; do
    command -v "$tool" >/dev/null || { echo "missing shader preparation tool: $tool" >&2; exit 1; }
done
[ -x "$M2V" ] || (cd "$ROOT/metal2vulkan" && cargo build --release --features serde --bin metal2vulkan)

OUT=${ROBLOX_MAC_SHADER_BUILD:-$HERE/out}
rm -rf "$OUT/air" "$OUT/spv" "$OUT/repros"; mkdir -p "$OUT/air" "$OUT/spv" "$OUT/repros"
python3 "$HERE/pack2air.py" "$APP/Contents/Resources/shaders/shaders_metal_osx.pack" "$OUT/air"

export METAL2VULKAN_REPRO_DIR=$OUT/repros
export TMPDIR=$OUT/tmp
mkdir -p "$TMPDIR"
# With the updater's progress channel, report "Translating shaders (N of M)" from
# 74 to 86 % while the shaders translate (prepare-cache.py goes on to 90 %).
progress_fd=${ROBLOX_MAC_PROGRESS_FD:-}
case "$progress_fd" in ''|*[!0-9]*) progress_fd=;; esac
poller=
total=$(find "$OUT/air" -maxdepth 1 -name '*.air' | wc -l)
if [ -n "$progress_fd" ] && [ "$total" -gt 0 ]; then
    (
        last=-1
        while :; do
            done_count=$(find "$OUT/spv" -maxdepth 1 -name '*.out' | wc -l)
            if [ "$done_count" -ne "$last" ]; then
                printf '{"phase":"preparing","percent":%d,"message":"Translating shaders (%d of %d)","bytes_done":0,"bytes_total":0,"cancellable":true}\n' \
                    $((74 + done_count * 12 / total)) "$done_count" "$total" >&"$progress_fd" 2>/dev/null || exit 0
                last=$done_count
            fi
            sleep 0.5
        done
    ) &
    poller=$!
fi
find "$OUT/air" -maxdepth 1 -name '*.air' -print0 | xargs -0 -P"$(nproc)" -I{} sh -c '
    b=$(basename "$1" .air)
    python3 "$2/translate-one.py" "$3" "$1" "$4/spv/$b"
' sh {} "$HERE" "$M2V" "$OUT" | sort | uniq -c
if [ -n "$poller" ]; then kill "$poller" 2>/dev/null || true; wait "$poller" 2>/dev/null || true; fi
echo '--- failure reasons:'
grep -h 'metal2vulkan: ' "$OUT"/spv/*.out | grep -v 'wrote\|PASS\|auto-detected\|FALLBACK' \
    | sed 's/[0-9]\+/N/g; s/;.*//' | sort | uniq -c | sort -rn
# Machine-readable per-shader failure records for analyze_shader_failures.py.
# Best-effort: the translated artifacts are already on disk, and a broken summary
# must never fail shader preparation itself.
python3 "$HERE/analyze_shader_failures.py" --collect "$OUT" \
    || echo 'shader failure summary unavailable' >&2
# The translator omits the terminating signal. Retry one affected input directly
# so diagnostics distinguish SIGILL, SIGSEGV and SIGKILL under the same limits.
failed=$(grep -l 'llvm-dis killed by signal' "$OUT"/spv/*.out | head -n 1)
if [ -n "$failed" ]; then
    python3 "$HERE/../scripts/diagnostics.py" --llvm-check "$OUT/air/$(basename "$failed" .out).air"
fi
