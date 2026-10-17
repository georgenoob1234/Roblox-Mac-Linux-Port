# Shader Failure Triage and Fix Plan

Status: evidence tooling complete, fixes pending the on-machine categorization of the 12
remaining failures (1,938 of 1,950 shaders validated at the last corpus run).

This plan pairs `analyze_shader_failures.py` (same directory) with concrete, narrowly scoped
fix recipes. Nothing here touches the core Indium Vulkan backend C++.

## 0. Scope and ground rules

- The translator is `metal2vulkan` (Rust, checkout at `RobloxLinuxDev/metal2vulkan`, port
  patch tracked at `third_party/patches/metal2vulkan.patch`). The Indium adaptation layer is
  `runtime/metal/prepare-cache.py`. Those two, plus this pipeline's scripts, are the only
  code we may change.
- **Never** emit dummy SPIR-V, stub a missing opcode with a no-op, or mark a failed shader
  as supported. If a construct genuinely cannot be expressed in the target ABI, the honest
  outcome is the existing FALLBACK path plus a documented count of unsupported shaders.
- Every translator change must be byte-neutral for shaders that already translate: the
  adapted SPIR-V in `spv-cache-v1` is content-addressed by the AIR blob's SHA-256, so
  section 3.3's byte-stability gate catches any collateral change.
- The 12 shaders are only "fixed" when section 3.3 (full corpus, zero regressions) and
  section 3.4 (GPU gameplay smoke test) both pass.

## 1. Evidence flow (step 1)

The repository does not ship the 12 failing AIR files or their diagnostics; they are
generated on the build machine from the real client pack:

```sh
cd RobloxLinuxDev                        # build machine; client present at RobloxPlayer.app
sh runtime/metal/translate-all.sh        # writes out/air, out/spv, out/repros AND
                                         # out/shader-failures.json (new)
python3 runtime/metal/prepare-cache.py   # writes runtime/data/spv-cache-v1/report.json
python3 runtime/metal/analyze_shader_failures.py \
    --pack RobloxPlayer.app/Contents/Resources/shaders/shaders_metal_osx.pack \
    --manifest runtime/metal/out/failure-manifest.txt
```

`translate-all.sh` now runs the collector automatically; the analyzer is read-only, and
both JSON inputs can be copied elsewhere for offline triage (paths inside them refer to the
machine that collected them). `report.json` failure entries now carry the actual
`spirv-val` verdict text (`prepare-cache.py` change in this branch); previously a rejected
adapted module only reported the exit status.

Analyzer exit status: 0 = every failure bucketed, 1 = some failure is `uncategorized`
(extend the tables in `analyze_shader_failures.py` first), 2 = inputs missing.
`python3 runtime/metal/analyze_shader_failures.py --self-test` runs the built-in fixture
regression tests for the collector and every category rule.

Correlation rule: a shader that fails translation also appears in `report.json` as a
downstream `FileNotFoundError`; the manifest records that as evidence, not as a second root
cause. A shader that translates but fails adaptation is an Indium-side (descriptor/signature)
failure.

## 2. Root-cause categories and fix recipes (step 2)

The analyzer buckets failures into the categories below. Each recipe names the exact files,
the change shape, and the test that must accompany it. Apply recipes only for categories
that actually appear in the manifest; do not pre-emptively widen the translator.

### 2.1 `air-unsupported` — missing AIR opcode/intrinsic (metal2vulkan)

Recognition: `native emitter: ...`, `air.*` intrinsic named in an error, `expects N
operands`, `llvm-dis failed:`.

Evidence to gather first:
```sh
llvm-dis runtime/metal/out/air/NNNN.air -o /tmp/NNNN.ll
grep -n 'air\.' /tmp/NNNN.ll | sort -u | less   # every AIR intrinsic the module uses
```
Compare against the match arms in `metal2vulkan/src/native/emitter/ops/air_calls.rs` and
the call-family routing in `metal2vulkan/src/passes/air_calls/`.

Two cases, in the order to check them:

1. **Signature variant of a modeled intrinsic** (the most likely case; the tracked patch
   already fixed two of these: 4-vs-5-operand `air.atomic.global.*`, and `_grad` sampling
   with vs. without the `min_lod_clamp` operand). Fix: widen the existing match arm to
   accept the operand count/type variant, reusing the emission that already exists — the
   two commits in `metal2vulkan.patch` are the exact template, including their tests in
   `src/native/tests/intrinsics.rs` / `textures.rs` asserting old and new signatures
   produce identical SPIR-V.

2. **Genuinely unmodeled intrinsic.** Add a match arm in `air_calls.rs` (plus family
   routing if needed) that emits the semantically equivalent SPIR-V. Opcode map for the
   common families:
   - atomics: `air.atomic.global.add/sub` → `OpAtomicIAdd`/`OpAtomicISub`;
     `min.s/min.u/max.s/max.u` → `OpAtomicSMin`/`OpAtomicUMin`/`OpAtomicSMax`/`OpAtomicUMax`;
     `and/or/xor` → `OpAtomicAnd`/`OpAtomicOr`/`OpAtomicXor`; `xchg` → `OpAtomicExchange`;
     `cmpxchg.weak/strong` → `OpAtomicCompareExchange` (weak/strong differ only in the
     unequal-operand semantics); reuse the existing AIR order/scope → SPIR-V
     memory-semantics/scope operand mapping the 5-operand form already uses.
   - sampling: implicit-LOD → `OpImageSampleImplicitLod`; `*_level` →
     `OpImageSampleExplicitLod` + `Lod` image operand; `*_grad` → same + `Grad` (never both
     `Lod` and `Grad`); comparison samplers add `Dref`; constant offsets add `ConstOffset`.
     All sample forms go through `OpSampledImage` on the reflected `OpTypeImage`.
   - texel fetch/read: `air.fetch_texture_*` → `OpImageFetch`; `air.read_texture` (storage
     image) → `OpImageRead`; `air.write_texture` → `OpImageWrite` (unknown-format
     read/write needs the capabilities in 2.2).
   - synchronization: `air.barrier` → `OpControlBarrier` (Workgroup scope, AcquireRelease
     semantics covering WorkgroupMemory); `air.membar.*` → `OpMemoryBarrier` with the scope
     the barrier family names.
   - threadgroup tile ops (`air.tile.*`) and imageblocks are threadgroup-storage games of
     their own — model them only from the reflect module's existing imageblock layouts, or
     they stay unsupported.

   No decorations are required for any of the above (atomics and barriers carry their
   memory semantics as instruction operands; images get their decorations from the
   reflection-driven binding pass).

Test requirement for either case: a corpus-style unit test in `src/native/tests/` that
translates an `.ll` containing the exact AIR form, disassembles, asserts the opcode and
operands, and runs `spirv-val` — the two tests inside `metal2vulkan.patch` are the pattern.
A new arm without such a test must not be committed.

If the construct cannot be expressed for Vulkan (e.g. it depends on Metal-specific
raster-order groups or tile memory Indium has no ABI for), leave the FALLBACK and add the
shader family to the README status count. That is a documented limitation, not a bug.

### 2.2 `spirv-validation` — SPIR-V validation error

Recognition: `spirv-val failed:` (translator-internal validation, the module never got
written) or `spirv-val rejected the adapted module: <rule>` / `returned non-zero exit
status` (prepare-cache stage).

First read the exact rule from the evidence — `spirv-val` errors name the instruction and
the rule (often a `VUID-` for the `vulkan1.2` env). Where the fix goes depends on the stage:

- **Translator output invalid**: bisect which lowering phase introduced the rule violation
  with `METAL2VULKAN_PASS_CONTRACT=1 METAL2VULKAN_PHASE_DUMP=/tmp/phase-NNN
  metal2vulkan <air> /tmp/out.spv`, then read the offending module with `spirv-dis`.
  Typical families and remedies:
  - storage image with unknown format that is read/written: declare the concrete format on
    `OpTypeImage` from the reflected AIR texture type, or (only if the format is genuinely
    dynamic) `OpCapability StorageImageWriteWithoutFormat` /
    `StorageImageReadWithoutFormat` — a capability change, no backend code needed.
  - kernel missing workgroup size: emit `OpExecutionMode LocalSize x,y,z` from the
    reflected `local_size` when the module declares one (reflection already carries it;
    `--local` only overrides).
  - entry-point interface mismatches: every global `OpVariable` in Input/Output must be an
    `OpEntryPoint` Interface operand (VUID-EntryPoint rules).
  - structured control flow: `OpSelectionMerge`/`OpLoopMerge` ordering and construct rules;
    `METAL2VULKAN_WHY=1` and `METAL2VULKAN_SPI_WHY=1` print the structured-plan reject
    reason instead of guessing.
- **Adapted module invalid (validation failed only after `remap_sets`)**: the remap itself
  broke a rule — e.g. two varyings collapsed onto one Location, or a DescriptorSet
  decoration became inconsistent with its Binding. Fix in `prepare-cache.py`'s
  `remap_sets()` (it already rejects duplicate semantics pre-write; extend the guard to
  whatever rule the verdict names) — never by weakening validation.

`OpMemoryModel` stays `Logical GLSL450` (Vulkan 1.2 requires Logical); pointer-network
constructs the emitter cannot express under logical addressing are modeled, not bypassed.

### 2.3 `descriptor-layout` — descriptor set/layout mismatch (prepare-cache.py)

Recognition: `descriptor array/set unsupported`, `non-descriptor resource unsupported`,
`unsupported binding <kind>`, `texel buffer unsupported`, `imageblock descriptors
unsupported`, `unexpected descriptor set`.

The manifest's `reflection:` block shows the exact binding (kind, Metal index, set,
binding, count) that tripped each rejection. Decision tree, in order:

1. **`descriptor array/set unsupported` (count > 1)**: read Indium's descriptor binding
   code (`native/renderer/indium/`) — read-only, we may not modify it — and check whether
   its rebuilt ABI binds arrays at one binding (the reflection module already models
   runtime-indexed texture arrays with an "ABI capacity"). If Indium consumes arrays:
   widen `adapt()` in `prepare-cache.py` to accept `count > 1` for exactly the kind(s)
   Indium supports, keeping `set == 0`, and pass `count` into the meta `bindings` entry.
   `remap_sets()` needs no change (it only rewrites set numbers and varying locations).
   If Indium binds exactly one descriptor per Metal index, the rejection is correct and the
   shader family stays unsupported — record it in the README status; do not fake an array
   as a single descriptor.
2. **`unexpected descriptor set`**: the translator emitted a non-zero set for a
   pre-remap descriptor (embedded argument-buffer resources do this). Root cause is on the
   metal2vulkan side (`DescriptorLayout` in `src/reflect/mod.rs`); fix there rather than
   papering over it in the remap.
3. **`texel buffer` / `imageblock` / `non-descriptor resource`**: Indium ABI gaps. Unless
   the Indium metadata format (unchangeable here) already represents them, these remain
   honest rejections: update the README's unsupported count, never the validation gate.

### 2.4 `resource-limit` — resource limit exceeded

Recognition: `20-second translation ceiling exceeded`, `500-MiB process-group RSS ceiling
exceeded`, `killed by signal [no verdict]`, `timed out after`.

First separate crash from starvation (translate-all.sh already retries one case):
```sh
python3 runtime/scripts/diagnostics.py --llvm-check runtime/metal/out/air/NNNN.air
```
- SIGSEGV/SIGILL → the translator crashed; that is a real bug, not a limit. Reproduce under
  `rust-gdb --args metal2vulkan ...air ...spv --emit-meta ...json`, fix the faulting pass,
  and reclassify the shader (the underlying category is usually 2.1/2.2 once the backtrace
  is known).
- SIGKILL / breach lines → the module genuinely exceeded the ceiling. Read
  `out/spv/NNNN.time` (elapsed seconds, peak RSS KiB — the manifest prints both). Fix by
  bounding the offending pass, not by skipping the shader: the relooper group size
  (`METAL2VULKAN_RELOOPER_MAX_BLOCKS`, hard max 1024) and the spirv-val concurrency gate
  already exist for exactly the known memory-heavy cases (BVH-builder modules).
  Raising the ceilings in `translate-one.py` is a last resort that weakens the guard for
  every shader: it needs measured numbers from `.time` files and a full corpus re-run to
  prove `nproc` parallel workers still fit the machine (the N100 runs 4 vCPUs / ~10 GB).

### 2.5 `signature-mismatch` — signature/reflection mismatch

Recognition: `entry-point name mismatch`, `multiple entry points unsupported`, stage
metadata missing, missing reflection (`FileNotFoundError` on a `.json` the translation
should have written), varying-semantic errors from `remap_sets()`.

- **entry-point name mismatch**: the translator's reflection `entry_point` disagrees with
  the metallib function table's `NAME` tag that `prepare-cache.py` parses. Run the analyzer
  with `--pack` — it prints both sides. If the pack is right, fix the name extraction in
  metal2vulkan's meta/reflect code for the new entry-name form; if the table form changed,
  extend `prepare-cache.py`'s table walk (both are in scope; change only the side that is
  wrong).
- **multiple entry points / function-table form**: metallib function-table version != 1.
  Inspect with the analyzer's `--pack` path resolution; a new table version needs a
  coordinated walk update in `prepare-cache.py` and metal2vulkan's meta reader.
- **stage metadata missing**: `llvm-dis` the AIR and confirm the module truly lacks
  `!air.vertex/!air.fragment/!air.kernel` named metadata. If the stage is derivable from a
  new metadata form, extend `detect_stage` (`metal2vulkan/src/lib.rs`); if the entry is a
  non-shader (specialized variant stub), it is corpus noise — filter it in the collector,
  never in validation.
- **reflection emission failure** (PASS line in the log, no `.json`): read the `.out`
  error; typically a serde schema or write path — fix `write_meta`/the reflection struct,
  not the caller.
- **varying semantic errors**: `remap_sets()` only accepts `user(locnN)`; the reflection
  summary in the manifest shows the actual semantic string. Extend the regex for the real
  form only after confirming what Metal emitted.

### 2.6 `uncategorized`

An error string no table knows lands here and the analyzer exits 1. Add the pattern to
`analyze_shader_failures.py` (translation side: `STAGE_SIGNATURE`/`AIR_PATTERNS`; adaptation
side: the `ADAPT_*` tables), add a fixture to `--self-test`, re-run it, re-analyze. The
category tables mirror strings that the code actually raises, so this is mechanical.

## 3. Validation protocol (step 3)

Every command below runs from `RobloxLinuxDev` on the build machine. Scratch work goes to
`mktemp -d` or `/tmp`; no command deletes shader caches, `.build/`, `DO_NOT_SHARE`, or the
client. `translate-all.sh` removes only its own `out/air`, `out/spv`, `out/repros`, and
`prepare-cache.py --output` writes only where told — point it at scratch until the final
step.

### 3.1 Isolate and re-translate one failing shader

```sh
cd RobloxLinuxDev
N=0100                                    # a failing index from the manifest
M2V=metal2vulkan/target/release/metal2vulkan
[ -x "$M2V" ] || M2V=runtime/usr/bin/metal2vulkan   # pinned prebuilt translator
WORK=$(mktemp -d)
mkdir -p "$WORK/spv"
# Same limits, logging and repro capture as the corpus run:
TMPDIR="$WORK" METAL2VULKAN_REPRO_DIR="$WORK/repros" \
    python3 runtime/metal/translate-one.py "$M2V" "runtime/metal/out/air/$N.air" "$WORK/spv/$N"
cat "$WORK/spv/$N.out"                    # full metal2vulkan diagnostics
cat "$WORK/spv/$N.time"                   # elapsed seconds, peak RSS KiB
```

`translate-one.py` prints PASS/FAIL and enforces the same 20 s / 500 MiB ceilings, so a fix
verified here behaves like it will in the corpus. For crashes, replace it with:
```sh
rust-gdb --args "$M2V" "runtime/metal/out/air/$N.air" "$WORK/spv/$N.spv" \
    --emit-meta "$WORK/spv/$N.json"
```

### 3.2 Validate the patched SPIR-V with spirv-val

Translator output (exists only once translation succeeds):
```sh
spirv-val --target-env vulkan1.2 "$WORK/spv/$N.spv"
```

Adapted module — the form Indium actually consumes — via a one-entry mini-pack so
`prepare-cache.py` runs its full adapt + remap + validate path for exactly this shader:
```sh
PACK=RobloxPlayer.app/Contents/Resources/shaders/shaders_metal_osx.pack
python3 - "$PACK" "$N" "$WORK/mini-pack" <<'PY'
import re, struct, sys
from pathlib import Path
pack, index, out = Path(sys.argv[1]), int(sys.argv[2]), Path(sys.argv[3])
data = pack.read_bytes()
for i, m in enumerate(re.finditer(b'MTLB', data)):
    if i == index:
        size = struct.unpack_from('<Q', data, m.start() + 16)[0]
        out.write_bytes(data[m.start():m.start() + size])
        break
else:
    raise SystemExit(f'shader index {index} not found in the pack')
PY
mkdir -p "$WORK/mini-app/Contents/Resources/shaders" "$WORK/one/spv"
cp "$WORK/mini-pack" "$WORK/mini-app/Contents/Resources/shaders/shaders_metal_osx.pack"
cp "runtime/metal/out/spv/$N.spv" "$WORK/one/spv/0000.spv"   # one-entry pack -> index 0
cp "runtime/metal/out/spv/$N.json" "$WORK/one/spv/0000.json"
python3 runtime/metal/prepare-cache.py --app "$WORK/mini-app" \
    --source "$WORK/one/spv" --output "$WORK/one-cache"
cat "$WORK/one-cache/report.json"          # expect prepared == total == 1, no failures
```

### 3.3 Full corpus rerun — zero regressions

```sh
cd RobloxLinuxDev
# (once, before applying any fix) snapshot today's baseline for the byte-stability gate:
python3 runtime/metal/prepare-cache.py --output /tmp/cache-baseline

# after each fix:
sh runtime/metal/translate-all.sh | tee /tmp/translate-all.log
WORK=$(mktemp -d)
python3 runtime/metal/prepare-cache.py --output "$WORK/cache-new"
python3 runtime/metal/analyze_shader_failures.py \
    --pack RobloxPlayer.app/Contents/Resources/shaders/shaders_metal_osx.pack \
    --manifest runtime/metal/out/failure-manifest.txt

# Byte-stability gate: previously prepared shaders must keep identical adapted bytes.
python3 - "$WORK/cache-new" /tmp/cache-baseline <<'PY'
import sys
from pathlib import Path
new, old = Path(sys.argv[1]), Path(sys.argv[2])
new_keys = {p.stem for p in new.glob('*.spv')}
old_keys = {p.stem for p in old.glob('*.spv')}
added, removed = new_keys - old_keys, old_keys - new_keys
changed = [k for k in sorted(old_keys & new_keys)
           if (old / f'{k}.spv').read_bytes() != (new / f'{k}.spv').read_bytes()]
print(f'added {len(added)} removed {len(removed)} changed {len(changed)}')
for k in changed:
    print('  changed:', k)
sys.exit(1 if (removed or changed) else 0)
PY
```

Done state, all of these at once:
- `translate-all` log: 1,950 PASS, zero FAIL, no failure-reason lines;
- `report.json`: `prepared == total == 1950`, `failures == []`;
- the manifest: zero failing shaders (analyzer exit 0);
- cache gate: `added 12 removed 0 changed 0` — any `changed` key must be justified with a
  `spirv-dis` diff and reverted if it is not part of the intended fix.

### 3.4 GPU gameplay smoke test

Shader-level validation proves the modules are legal SPIR-V, not that they render. After
the corpus is green:

```sh
cd RobloxLinuxDev
runtime/bin/roblox-mac --prepare-shaders    # rebuilds the live cache once, with the fixes
runtime/bin/roblox-mac                      # launch, log in, join a place
# measurable variant:
runtime/bin/roblox-mac --frame-trace "$PWD/.build/profile/frames.csv" --place-id <ID>
python3 native/frame-times.py .build/profile/frames.csv --all-contexts --start 30 --duration 30
# optional Vulkan pipeline pre-check on the GPU:
python3 native/renderer/compute-sync-check.py --gpu
```

Checklist — driven by the shader families the manifest actually fixed (its `entry=` names
say what each family renders):
1. Login screen and main menu render (UI/text shaders).
2. A simple flat place: skybox, textured terrain, character models, shadows.
3. A busy experience (the TTK/Jailbreak sessions from the project status): post-processing
   toggles in graphics settings (DoF/blur/bloom), particles, transparent surfaces.
4. 60+ seconds stable, no Vulkan validation output in the debug log, no shader-cache miss
   or renderer-fallback messages; frames progress in the trace during camera motion.
5. `sh run.sh --diagnose` equivalent for the release build (only after this branch is
   merged and a release regenerated) — `check-diagnostics.py` covers shader log
   preservation, and diagnostics folders must never contain AIR/SPIR-V binaries.

A family that cannot be made to render correctly is not "fixed": put it back on the
unsupported list with its reason rather than shipping a visually broken shader.

## 4. Shipping the translator changes

metal2vulkan fixes live in the nested checkout and must stay in sync with the tracked
patch — `bootstrap.py` re-applies `third_party/patches/metal2vulkan.patch` on fresh clones
(verified: the tracked patch is exactly `git -C metal2vulkan diff` today):

```sh
cd RobloxLinuxDev/metal2vulkan
cargo test --release --features serde             # includes the new corpus-style tests
cargo build --release --features serde --bin metal2vulkan
git diff > ../third_party/patches/metal2vulkan.patch
```

Python/script changes (`prepare-cache.py`, `translate-all.sh`,
`analyze_shader_failures.py`) ship directly from this repository;
`build-appimage.sh` bundles all five metal files into the AppImage. `RobloxLinuxRelease/`
is generated output — regenerate it with `runtime/package/build-release.sh` when the
changes are ready to ship; never hand-port fixes into the release copy.
