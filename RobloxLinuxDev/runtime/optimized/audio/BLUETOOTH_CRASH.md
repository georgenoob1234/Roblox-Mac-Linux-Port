# Bluetooth connection crash investigation — 2026-10-22

Status: **fixed and physically retested**. A fresh Bluetooth reproduction
established repeated teardown of an already-disposed AudioUnit. AudioToolbox
now returns `badComponentInstance` for selector dispatch on that invalid
handle, allowing the client to create and start a replacement unit. The user
confirmed connection/disconnection no longer crash and playback works.
Branch: `fix/bluetooth-audio-connection-crash`,
based on freshly fetched `origin/main` at `a28cff41e2e0a6811ffca655db93a47b90700c4e`.

## Captures and actual installed binaries

Inputs: `/home/george/Downloads/fatal.log` and `fatal(1).log`.
`fatal(1).log` begins with the complete bytes of `fatal.log`, then appends a
second capture. These represent two distinct crashes, not three. Their 20
recorded Roblox return addresses match exactly, despite different audio image
addresses, stack addresses and stack cookies.

Installed release: `/home/george/.local/opt/roblox-mac-linux-port/RobloxLinux.AppImage`
(version marker `v1.0.5`). Extracted its audio frameworks with the AppImage's
built-in unsquashfs, using its reported filesystem offset **1189056**. This
does not install or overwrite anything. The vanished `/run/user/1000/rbx.*`
prefixes themselves are unavailable; the logs contain neither framework UUIDs
nor file hashes. The persistent installed binary matches the crashing code,
mapped segment sizes/offsets and register state; byte-for-byte identity with
the vanished copy cannot independently be verified from these logs.

| Installed component | SHA-256 |
| --- | --- |
| AudioToolbox | `c771cb6d6d96b211140d249d8ffa277d39233efdd7a85ffe4b60de1f8b2bb092` |
| CoreAudio framework | `720afe49863c193a536383c51fb2c2402db4ad8f797d6c888c393312372a1561` |
| CoreAudio.component executable | `24fe8ddb19adb6707ac16826e0bf25d62961e3ef744d0654f6092fba6905f70d` |
| RobloxPlayer | `a901265fc499c0a151dae1809664f7d6b71135ac74044e84076852f008e436d7` |

AudioToolbox is Mach-O x86_64, UUID `A28D8689-DC94-3993-8DAD-756B5B1180FD`.
RobloxPlayer is Mach-O x86_64, UUID `F29B72EC-3245-39CF-8D82-96709F52B36A`,
client version `0.744.0.7441080`.

**The checkout's built AudioToolbox differs from the installed AppImage.** Its
hash is `a959645fbdf47d0f7258470ab7ed714cb453919574e22278962b658d9b29f080`,
UUID `B901962A-274D-3E2E-B6D6-249BEA350860`. In that local build, the equivalent
indirect call is at `+0xAAD`; its `+0xACD` instruction is an unrelated `lea`.
Using the local build without checking identity would misidentify this crash.

## Load bias, instruction and containing function

Installed AudioToolbox load commands:

| Segment | Preferred VM address | File offset | VM size | File size |
| --- | --- | --- | --- | --- |
| `__TEXT` | `0` | `0` | `0x10000` | `0x10000` |
| `__DATA` | `0x10000` | `0x10000` | `0x1000` | `0x1000` |
| `__LINKEDIT` | `0x11000` | `0x11000` | `0xE000` | `0xD084` |

The first capture maps executable file offset zero at `0x7F1A3CF0B000`, so its
slide is `0x7F1A3CF0B000 - __TEXT.vmaddr = 0x7F1A3CF0B000`.
`0x7F1A3CF0BACD - slide = 0xACD`. Applying the segment translation gives file
offset `__TEXT.fileoff + (0xACD - __TEXT.vmaddr) = 0xACD`.
The second capture maps it at `0x7F2CB6A62000`; its RIP `0x7F2CB6A62ACD`
also translates to preferred address/file offset `0xACD`. The equal preferred
address and file offset are a consequence of these segment values.

The binary retains local function symbols. The containing symbol starts at
`0xA90`: `__Z12dispatchCallIJEEiP23ComponentInstanceRecordsDpT_`, demangled
`int dispatchCall<>(ComponentInstanceRecord*, short)`. The fault is `+0x3D`
inside that function:

```asm
0xAC2: callq AudioComponentManager::instanceInterface(ComponentInstanceRecord*)
0xAC7: movq  %rax, %r14
0xACA: movswl %bx, %edi
0xACD: ff 50 10     callq *0x10(%rax)
```

`RAX=0`, so fetching the indirect-call target reads address `0x10`. It fails
before invoking the target. The field at offset `0x10` is
`AudioComponentPlugInInterface::Lookup` (after its two pointer-sized
`Open`/`Close` fields). `RBX=RDI=0x202` is `kAudioOutputUnitStopSelect`.
The operation is therefore **AudioOutputUnitStop**, not device enumeration or
an unknown format query. `instanceInterface()` returns null when the handle
is absent from its map; dispatch dereferences that result without validation.
The map only inserts non-null factory objects, so ordinary registry behavior
points to an unregistered handle. Registry corruption cannot be ruled out.

The original handle value is **not recoverable from the saved registers**:
dispatch overwrites its saved handle in `R14` with the null interface at
`0xAC7`. `RDI=0x202` is the selector, not the handle.

## Roblox caller and limits of symbolication

Roblox `__TEXT` is preferred/mapped at `0x100000000`, file offset zero; its
slide in both captures is zero. `LC_FUNCTION_STARTS` supplies boundaries,
but the relevant function names and source/debug lines are stripped.

| Recorded return address | Containing function start | Evidence |
| --- | --- | --- |
| `0x10282C445` | `0x10282C34C` | References literal `OutputCoreAudio::close : OutputCoreAudio %p,  mInDrivers %p` |
| `0x10282AEF3` | `0x10282AEB8` | References literal `OutputCoreAudio::closeCallback output %p` |
| `0x1027F7B63` | `0x1027F7B48` | Calls output callback; references `fmod_output.cpp` |
| `0x10281C7EE` | `0x10281C780` | Indirect call to output object; no recovered name |
| `0x102816E96` | `0x102816E82` | Calls previous function; no recovered name |
| `0x10280D63D` | `0x10280D5EA` | References diagnostic literal `System::setOutput` |
| `0x102754881` | `0x102754868` | Calls preceding FMOD function with second argument zero |
| `0x10275498E` | `0x10275489E` | Calls previous wrapper |

The first caller executes:

```asm
0x10282C439: movq 0x4C0(%rbx), %rdi
0x10282C440: callq 0x105E11598  ; indirect-symbol table: _AudioOutputUnitStop
0x10282C445: movq 0x4C0(%rbx), %rdi
0x10282C44C: callq 0x105E115B6  ; _AudioUnitUninitialize
0x10282C451: movq 0x4C0(%rbx), %rdi
0x10282C458: callq 0x105E114FC  ; _AudioComponentInstanceDispose
```

This establishes FMOD's CoreAudio output teardown path. Labels derived from
diagnostic strings above are **not recovered symbol-table names**. The other
12 stable return addresses are `0x1034450E4`, `0x10344F6C5`, `0x105176BE4`,
`0x102FAD714`, `0x102FAD4B0`, `0x102FAD832`, `0x102FAD8A6`, `0x102FB3E4D`,
`0x102FB3FD3`, `0x1032F76ED`, `0x1032F75DE`, `0x1032E1F39`. They remain
unnamed. Matching private Roblox/FMOD symbols or source would be needed for
full source-level symbolication.

## Responsible source and connection path

The pinned dependency is `darlinghq/darling`, release `v0.1.20260608`, source
archive SHA-256 `4fc9eb9a94af2aa396d4fafa0b6bbf0bcd7e210170854d143c080de3b5bb8df7`
([dependency pin](../../../third_party/dependencies.json)). The cached archive's
hash was verified; at the start of the investigation, `AUComponent.cpp`, `ComponentDispatch.h`,
`AudioComponentManager.h` and `AudioComponentManager.mm` in
`runtime/src/darling/src/CoreAudio/AudioToolbox/` match its contents byte-for-byte.
The installed instruction sequence agrees with that source. There is no
installed build manifest proving a more precise source/build identity.

`AUComponent.cpp` forwards Stop through `dispatchCall`; `ComponentDispatch.h`
does the unchecked interface lookup. `AudioComponentManager.mm` owns creation,
Open, Close, registry insertion/removal and interface lookup. `isOurInstance`
classifies handles using bit 31, rather than checking registry membership.
`instanceInterface` locks the registry while finding the object but releases
that lock before its caller invokes the interface. A hypothetical concurrent
dispose is therefore a lifetime concern, **not an established cause here**.

AudioToolbox is locally rebuildable through `optimized/darling/build.sh`.
The fix is persisted in the pinned source overlay under
`third_party/darling-overlay/src/CoreAudio/AudioToolbox/ComponentDispatch.h`,
registered in `third_party/darling-changes.json`. It is also applied to the
local extracted dependency and rebuilt development runtime. Editing only the
ignored extracted source or an installed AppImage would not persist the fix.

CoreAudio is replaced by this project's [AudioHardware.cpp](CoreAudio/AudioHardware.cpp),
[HostDevices.h](CoreAudio/HostDevices.h), Pulse backend and [AUHAL.cpp](AUHAL.cpp).
`optimized/audio/build.sh` builds its framework and audio-unit component.

* Device discovery takes PulseAudio server/sink/source snapshots, including
  monitor sources. It does not talk directly to BlueZ/Bluetooth.
* `GetObject` opportunistically refreshes snapshots; registered listeners also
  start a one-second refresh timer. Snapshots preserve the last good inventory
  on server errors.
* New endpoint names create persistent device/stream objects with stable IDs.
  Removal marks devices dead and omits them from enumeration; objects remain
  available for outstanding references. Reappearance reuses the ID.
* Inventory, alive and default-device changes produce separate property
  notifications. Callbacks are serialized with listener removal, outside the
  registry lock. They may run inline on a querying thread or on the refresh
  timer's dispatch thread. Registered procedure/client-data values are retained
  in the listener list until removal.
* Device properties report direction/channel configuration, PCM stream formats,
  sample rates, latency, buffer size and preferred mono/stereo channel layouts.
  AUHAL selects devices and creates/stops Pulse-backed IO procedures.

Connecting headphones can add sink/source endpoints and change defaults;
switching an already-present output can change defaults without changing the
inventory. That explains why these operations exercise different event
sequences. It does **not** establish which event leads to the invalid unit.

## Checks actually performed

All synthetic audio servers were private PipeWire/Pulse instances without
hardware-discovery policy. They did not change the desktop's routing.

* Built a small Mach-O registered-component probe. Valid Stop/Dispose passed
  using the installed AudioToolbox. Deliberately calling Stop on its disposed
  handle crashed; a diagnostic build recorded the missing registry lookup.
  This reproduces the immediate invalid-handle failure, not Bluetooth's cause.
* A null-returning synthetic component factory returned error `-3000` with
  non-null, unregistered handle `0x80001000`. This is a separate concrete API
  defect. No evidence ties factory failure to the original captures, so it
  was not patched as the proposed Bluetooth fix.
* Ran existing `device-check --synthetic` and `fmod-unit-check` against the
  extracted **installed** AudioToolbox, CoreAudio and CoreAudio.component.
  Passed inventory/format queries, selected-device playback/capture, default
  output notification, device add/remove/reappearance with stable UID, eight
  concurrent readers, 32 immediate stop cycles, and 20 AudioUnit restarts.
  FMOD-style synthetic initialization is not execution of Roblox's FMOD code.
* Built logging-only AudioToolbox and CoreAudio variants in `/tmp`.
  AudioToolbox emitted existing `TargetConditionals.h` macro-redefinition
  warnings. CoreAudio/component builds emitted SDK nullability and deprecated
  atomic/spinlock warnings. No compilation/link errors remained.
* Ran `python3 runtime/optimized/audio/check-capture.py --build-dir
  /tmp/rbx-bluetooth-investigation/trace/coreaudio-build`: all six PCM capture
  checks passed. This exercises the logging-only CoreAudio build.
* Built `build-lifecycle-trace.py --output
  /tmp/rbx-bluetooth-investigation/reviewable-trace-3` and verified its logs
  against the normal, failed-factory and disposed-handle probe modes. Normal
  behavior remained successful; the intentional stale-handle case still failed
  and recorded its handle and missing-lookup stack, as intended.
* `python3 -m py_compile runtime/optimized/audio/build-lifecycle-trace.py`,
  `git diff --check`, and the repository's `sh .github/scripts/ci-lint.sh`
  passed. Lint ran from the repository root; other relative commands here run
  from `RobloxLinuxDev`.
* Temporary harness setup failures were corrected: one missing CarbonCore
  include path, an extraction destination that already existed, and omitted
  device-check file handshakes. These were not audio regressions.

Artifacts/commands are retained in `/tmp/rbx-bluetooth-investigation/`:
`analyze.py`, `analysis.txt`, `frame-*.asm`, `captures.json`, `build-probe.py`,
`component-lifecycle.cpp`, `run-synthetic.py`, `synthetic-test-2.log`, build and
lifecycle logs. Successful installed-runtime synthetic logs are in
`/tmp/rbx-bt-synthetic-cromf2jf/`; capture logs in `/tmp/capture-check-zoxzps_c/`.
These are temporary, machine-specific evidence paths.

## Fresh physical reproduction, fix and retest

The user reproduced the crash in the logging-only runtime, producing
`/tmp/rbx-bluetooth-investigation/physical-20261022T203726.log`.
The mapped framework paths were checked in the running process: both tracing
frameworks were actually loaded. The log records:

1. Successful factory/Open, Initialize and Start for handle `0x80001000`.
2. Device-list (`dev#`, selector `0x64657623`) notification callbacks to the
   two Roblox/FMOD listener addresses `0x10282A79A` and `0x10282C324`.
3. On one client thread: Stop, Stop, Uninitialize, successful Dispose and
   registry removal of `0x80001000`.
4. On that same thread: another Stop of `0x80001000`, followed by the missing
   registry lookup and crash. The user confirmed the crash at this point.

This establishes a stale handle following completed disposal, rather than a
failed factory or a concurrent disposal race. The compatibility defect is
that `isOurInstance()` only recognizes a tagged ID; dispatch treated that as
proof of a live interface. The fix validates the live registry lookup before
calling Lookup and returns the same `badComponentInstance` error already used
by Dispose for absent handles. It does not resurrect a disposed object, call
freed plugin code, report false success, or disable notifications/Bluetooth.
Why the proprietary FMOD implementation repeats its teardown is not established
at source level; the repeated calls and Darling's invalid-handle failure are
directly observed.

Only the selector-dispatch validity check changes production behavior. No
changes were made to notification delivery, device enumeration, formats,
playback, clipboard or shortcuts. The unrelated factory-failure and potential
concurrency concerns described above remain outside this fix.

The focused `component-lifecycle-check.cpp` recreates this ordering without
hardware. It failed against the original installed AudioToolbox and passes
against the patched framework and updated development runtime. It also checks
that live/unsupported selectors preserve their behavior, stale property queries
return the invalid-instance error, freed plugin callbacks are never invoked,
and 32 replacement instances succeed. `check-component-lifecycle.py` runs it
in an isolated guest; the normal build now executes it after Darling install.

The smallest affected build was:

```sh
ninja -C runtime/src/darling-o2 -j 2 src/CoreAudio/AudioToolbox/AudioToolbox
python3 runtime/optimized/audio/check-component-lifecycle.py
```

Ninja rebuilt `AUComponent.cpp.o`, `AudioFileComponent.cpp.o` and AudioToolbox.
Only the existing TargetConditionals macro warnings remained. Export and dylib
dependency compatibility checks passed. Only AudioToolbox was updated in the
development sysroot/staging tree, with its existing receipt hash updated; the
installed release AppImage was not overwritten.

Patched uninstrumented framework SHA-256:
`67645a812ccfe566b028a894cf99691b11e72fcd5fd6f8493677c79b9550ff24`.
The private synthetic device/routing/hotplug and 20-restart checks also passed
with this patched AudioToolbox and the original installed CoreAudio/component
(`/tmp/rbx-bt-synthetic-msqbzgli/`).

Also ran `sh runtime/optimized/audio/build.sh` successfully, including the new
probe, followed by the lifecycle runner against the default development
runtime. The full audio build retains the existing SDK nullability and
deprecated atomic/spinlock warnings. Repository CI lint, Python compilation,
shell syntax, diff whitespace and overlay/manifest/applied-source consistency
checks passed.

The final microphone check has an intermittent result worth preserving:
one run (`/tmp/capture-check-1ic5ei2l/`) terminated during mono round 1 without
a useful fault report. A control with the original installed AudioToolbox
completed all six PCM checks but emitted a PulseAudio `pa_context_ref`
assertion during shutdown (`/tmp/capture-check-6f_0bpyr/`). The patched rerun
completed all six checks cleanly (`/tmp/capture-check-ok86yilw/`). This does not
establish the cause of the intermittent capture/shutdown issue; it has not
been fixed or conflated with the proven Bluetooth duplicate-teardown crash.
The first failed run is retained rather than reported as an unconditional pass.

For the physical retest, the installed AppImage used a logging build of the
same patched AudioToolbox, with **the original installed CoreAudio**. Its log,
`/tmp/rbx-bluetooth-investigation/patched-physical-20261022T204241.log`, shows
the invalid Stop and Uninitialize returning through the error path, followed
by successful factory/Open/Initialize/Start of `0x80001001`. Later device
changes created and started further replacement units. The user reported:
“connection doesnt crash, audio works. Disconnection doesnt crash.”
Darling's `backtrace()` supplied no frames in these diagnostic records;
the lifecycle ordering is recorded, but these are not fully symbolicated
new stacks. The earlier binary-derived call chain remains the stack evidence.

## Physical matrix

| Case | Evidence available |
| --- | --- |
| A: disconnected at start, connect during gameplay | Fresh unpatched crash captured; patched user retest passed with working audio. |
| B: connected before startup | Physical result unverified. Synthetic pre-existing endpoints passed. |
| C: switch already-connected output | Original user report: no crash. Patched synthetic default switch passed; no separate physical-switch confirmation. |
| D: different Bluetooth device | Not tested. |

Patched disconnection during gameplay also passed the user's physical test.
Cases B and D remain optional coverage, not claimed as verified.

For the current session,
`sh /tmp/rbx-bluetooth-investigation/run-patched-traced.sh` launches the installed
AppImage with the patched diagnostic AudioToolbox and writes a private log.
The original `run-traced.sh` uses the **unfixed** diagnostic build and is
retained only as evidence. A normal installed-AppImage launch does not use
these overrides: the fix must be included in a rebuilt release to persist in
that installed package.

To rebuild the reviewable **AudioToolbox-only** tracer later:

```sh
python3 runtime/optimized/audio/build-lifecycle-trace.py --output /tmp/rbx-audio-trace
# Use the printed DYLD_FRAMEWORK_PATH with the installed AppImage for that launch.
```

The builder compiles two copied source files and links existing Darling
objects; it never installs the result or edits the extracted dependency.
It logs handles, thread IDs, factory/Open/Close results, Stop/Start dispatch
and missing-lookup stacks, capped at 4096 regular events. Missing lookups are
always recorded. No PCM, endpoint names, cookies or authentication payloads
are logged. Logging can change timing, so a disappearing crash alone is not
proof of a fix.

The affected AudioToolbox component was rebuilt and physically retested;
a full runtime rebuild and AppImage repackaging were not needed for that test
and have not been performed. A Git merge does not update the installed AppImage.
