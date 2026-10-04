# Roblox Mac Linux Port

An experimental way to run the Intel macOS Roblox client on x86_64 Linux,
using Darling and a Vulkan renderer. This is an unofficial compatibility layer.

Download **[RobloxLinuxRelease.tar.gz from Releases](https://github.com/spidercraft/Roblox-Mac-Linux-Port/releases/latest)**
and extract it before launching. Build sources are in `RobloxLinuxDev` in the
same repository. The release includes the AppImage and updater. Run
`sh update-roblox.sh` from the extracted folder to download the official client
and prepare its shaders before launching. GitHub's automatic source-code ZIP
is for developers.
If needed, make the AppImage
executable with `chmod +x RobloxLinux.AppImage`.

```text
RobloxLinuxRelease/
├── RobloxLinux.AppImage
├── RobloxVersion/     (created by update-roblox.sh)
├── update-roblox.sh
├── run.sh             (terminal launch with diagnostic logs)
├── bootstrap.sh       (first-run, lock and update orchestration)
├── bootstrap.py       (settings/status JSON helper)
├── install-uri-handler.sh (optional browser protocol registration)
├── icon.png           (icon installed for the browser protocol handler)
├── FFlags.json
├── README.md
├── .gitignore
└── DO_NOT_SHARE/       (created on startup; never share)
```

## How to run

Download and extract the release archive above, then open a terminal in the
extracted `RobloxLinuxRelease` folder. For first-time setup:

```sh
chmod +x RobloxLinux.AppImage
sh update-roblox.sh
sh run.sh
```

To open browser Play, friend, private-server and other Roblox links in this
release, install the user-local protocol handler once:

```sh
sh install-uri-handler.sh
xdg-mime query default x-scheme-handler/roblox
```

The handler passes the complete `roblox://` or `roblox-player:` URI as one
opaque argument. If Roblox is already running, the second launch sends a
desktop notification and exits; it never queues or writes the URI. Remove the
registration with `sh install-uri-handler.sh uninstall`.

The first `sh run.sh` performs a separate `--shell true` warm-up when
`DO_NOT_SHARE/first-run.initialized` is absent, waits for its exit status, and
only then launches the client. On failure or timeout it stops before a known
blank-window launch. Keep the generated **RobloxVersion** beside the AppImage.

The headless interface is:

```sh
sh run.sh status
sh run.sh check
sh run.sh update                 # newline-delimited JSON progress
sh run.sh get auto_update
sh run.sh set auto_update auto|ask|off
sh run.sh set check_interval_hours 24
```

`DO_NOT_SHARE/bootstrapper.json` stores the update mode (`ask` by default),
check interval (24 hours by default), skipped version, last check and last
known latest. Network checks have a short timeout and never prevent a launch;
`0` checks every launch. Exit codes are `0` for success or a dropped second
launch, `1` for setup/update/check failure, and `2` for invalid input.
Use the scripts on systems with AppImageLauncher. They prevent it from moving
the AppImage away from its client and keep temporary files in this release folder.
Older releases could fail before Roblox started with `//DO_NOT_SHARE` permission
errors and Qt plugin warnings; replace the AppImage, `run.sh` and updater together.
Linux needs x86_64, glibc 2.39 or newer (the Ubuntu 24.04 baseline), a Wayland
desktop, compatible Vulkan drivers, Python 3, util-linux and unprivileged user
namespaces. X11-only sessions, other CPU architectures and musl-based systems are
not supported. The updater also requires curl and unzip.
FUSE is optional.
The native UI and media libraries are built or packaged in Ubuntu 24.04. Release
checks reject bundled ELF files requiring AVX-512 or glibc newer than 2.39 and
test native UI loading on the baseline. The host still supplies glibc, Wayland/XCB libraries and GPU
drivers; full gameplay compatibility on every system is not guaranteed.

For troubleshooting, run `sh run.sh`.
For a system check without launching Roblox, run `sh run.sh --diagnose`
to record the CPU and its features,
RAM/swap, kernel, distribution, GPU driver, user-namespace checks and shader-tool
versions or terminating signals under the shader memory limit. It also checks
native UI and EGL driver loading and reports missing libraries or symbol versions.
Each run prints its folder under `DO_NOT_SHARE/diagnostics/`, containing
`system.txt` and, for debug launches, `runtime.log` with the exit status.
Debug launches enable Vulkan loader errors and warnings to expose driver-loading failures.
The updater automatically logs shader preparation there and preserves individual
shader text logs even when an update fails. No session files or environment dump
are collected. Runtime output can contain account details: review individual text
files before sharing them, and never upload the whole `DO_NOT_SHARE` folder.

- **RobloxVersion/** contains the downloaded client and prepared shaders.
- **update-roblox.sh** downloads the latest client. Close Roblox, then run
  `sh update-roblox.sh`. Updating also requires curl and unzip.
- **FFlags.json** contains your flag overrides (`{}` by default). They apply
  each time Roblox starts; Roblox decides which flags it supports.
- **DO_NOT_SHARE/** is created automatically on startup. It contains your
  cookies, settings, browser storage, logs, caches and temporary runtime files.
  Updates preserve this folder, including your saved login.

Your login cookies are in:

```text
DO_NOT_SHARE/cookies/native.sqlite
DO_NOT_SHARE/browser/cookies.sqlite
```

**Never share DO_NOT_SHARE.** When sharing RobloxLinuxRelease, exclude that entire
folder. Do not delete it from your own installation unless you want to reset
your login and settings. The supplied release archive excludes it.
The included `.gitignore` also excludes it from Git; ZIP/tar tools do not obey
`.gitignore`, so leave the folder out when making your own archive.

Persistent app data stays inside this folder; nothing is installed into your
home data/config directories. Move the whole RobloxLinuxRelease folder to keep your
profile. Roblox can still expire or revoke a session independently of updates.

Startup and short TTK/Jailbreak gameplay sessions have been checked. Stable
240 FPS is not established across games. Some shader variants are unsupported,
so rendering issues are still possible.

[Development sources and build instructions](https://github.com/spidercraft/Roblox-Mac-Linux-Port/tree/main/RobloxLinuxDev) are in the same repository.

Keep the release in a reasonably short path (for example `~/RobloxLinuxRelease`). Darling can fail to start when deeply nested folder names exceed Unix-socket path limits.

Performance defaults keep host-native memory routines enabled and avoid an extra VSync wait; Roblox still controls its own FPS cap. Machine-specific settings such as `ROBLOX_MAC_CPU_AFFINITY` belong in `DO_NOT_SHARE/config.env`, which stays private.
