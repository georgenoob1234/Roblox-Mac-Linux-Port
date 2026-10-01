# Third-party provenance

The dependency identities and archive SHA-256 values are in
`third_party/dependencies.json`. Preserve upstream notices when distributing
modified code or binaries and satisfy each component's source obligations.

| Component | Upstream | License / notices |
|---|---|---|
| Darling and bundled compatibility sources | https://github.com/darlinghq/darling | GPL-3.0 and component-specific licenses; retain the source archive's LICENSE and per-file notices |
| Indium | https://github.com/darlinghq/indium | ISC; see pinned upstream LICENSE and file headers |
| metal2vulkan | https://github.com/steelbrain/metal2vulkan | LGPL-3.0-or-later |
| Dear ImGui | https://github.com/ocornut/imgui | MIT |
| Vulkan-Headers | https://github.com/KhronosGroup/Vulkan-Headers | Apache-2.0 / MIT notices in the pinned archive |
| AppImage build tools | https://github.com/AppImage | Preserve the corresponding upstream tool/runtime licenses |
| uruntime (AppImage launcher) | https://github.com/VHSgunzo/uruntime | MIT; see third_party/licenses/uruntime.txt |

`third_party/patches/` contains local changes against pinned upstream commits.
`third_party/darling-overlay/` contains the five modified source files compared
byte-for-byte against the pinned Darling source archive. Existing notices in
those files are retained. Several CoreAudio and renderer files under `runtime/`
and `native/` are derived from Darling/Indium; their existing headers still apply.

GTK, WebKitGTK, SDL, PulseAudio, EGL and Vulkan host libraries are external
build/runtime dependencies; they are not included in this Git source snapshot.
Binary packaging has additional dependency/license requirements. No Roblox,
macOS SDK binary, game asset, captured AIR or SPIR-V corpus is included here.
