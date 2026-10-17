# Roblox Mac Linux Port

## If you want the original spidercraft version, download the initial commit/ first release.

An experimental, unofficial runner for the Intel macOS Roblox client on x86_64 Linux, using Darling, Wayland and Vulkan.

## Install for one user

The supported per-user installer downloads the latest published
`RobloxLinuxRelease.tar.gz` from this repository, verifies its published
SHA-256 checksum, and installs it under
`~/.local/opt/roblox-mac-linux-port`. It also installs a user-local desktop
launcher, icon, and `roblox://` / `roblox-player:` associations when there is
no ownership conflict. No root privileges or `sudo` are required.

```bash
curl -fsSL https://raw.githubusercontent.com/georgenoob1234/Roblox-Mac-Linux-Port/main/install.sh | sh
```

When run from a terminal, this command opens the installer menu. A fresh
machine offers **Install** and **Exit**; later runs detect the installation and
offer the appropriate update or uninstall actions. The menu also asks whether
to add the launcher and Roblox links to the application menu.

After installation, the application-owned CLI is:

```sh
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" update --yes
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" status
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" uninstall --yes
```

Uninstall removes only unchanged files recorded in the install manifest. It
preserves `DO_NOT_SHARE` (cookies, settings, caches and logs) by default,
along with user-modified and unexpected files. The directory is left behind
when preserved data remains.

The interactive installer asks whether to keep your data. The explicit CLI
equivalent is `--keep-data` (the default); use
`uninstall --yes --purge-data` only when you want app-owned data removed from
the install root. Unknown files and unsafe symlinks are still preserved.

If you prefer to inspect the bootstrap before running it:

```bash
curl -fsSLo install.sh https://raw.githubusercontent.com/georgenoob1234/Roblox-Mac-Linux-Port/main/install.sh
less install.sh
sh install.sh
```

The command executes a remote script and then downloads a release from GitHub.
The bootstrap uses HTTPS and a pinned checksum for the installer, but both the
script and checksum are hosted by this repository; a compromised repository
account could replace them. Review the script first if you do not trust the
source. Detailed conflict, recovery, desktop integration, and dry-run
instructions are in [`installer/INSTALLER.md`](installer/INSTALLER.md).

**[Download RobloxLinuxRelease.tar.gz](https://github.com/georgenoob1234/Roblox-Mac-Linux-Port/releases/)** for a manual release download. The per-user installer above performs the verified extraction and desktop integration for you.

```text
RobloxLinuxDev/        Source, dependency pins, patches and build scripts
RobloxLinuxRelease/    AppImage, updater, FFlags.json and README
```

The Git repository contains the development source and release instructions. The AppImage and updater are in the downloadable release asset; GitHub's automatic source-code ZIP is for developers.

## How to run

Use x86_64 Linux with glibc 2.39 or newer (Ubuntu 24.04 or newer is the build baseline), a Wayland desktop, compatible Vulkan drivers, Python 3, util-linux and unprivileged user namespaces. FUSE is optional. Other CPU architectures, X11-only sessions and musl-based systems are not supported.

The native UI and media libraries are built or packaged in Ubuntu 24.04. Every
bundled ELF is checked for baseline x86-64 and glibc 2.39 requirements; AVX-512 is
not required. The native UI is also load-tested on that baseline. The host still
supplies glibc, Wayland/XCB libraries and GPU drivers. These checks do not establish gameplay compatibility
on every distribution or GPU.

Download and extract **RobloxLinuxRelease.tar.gz** from the release link above.
Open a terminal in the extracted `RobloxLinuxRelease` folder, then install the
desktop and browser-link entries once:

```sh
./install-uri-handler.sh
```

After that, open **Roblox (Mac Linux Port)** from the desktop application menu.
The launcher performs first-run setup and client updates when needed.

The first `sh run.sh` performs a separate `--shell true` warm-up, records a private marker only after it succeeds, and then starts the client. A failure or timeout stops before the client is launched. An explicit `sh update-roblox.sh` remains available; later launches use a bounded, scheduled update check.
Keep `RobloxVersion` beside the AppImage. Use the scripts on systems with
AppImageLauncher: they prevent relocation and keep temporary paths in the release
folder, avoiding `//DO_NOT_SHARE` permission errors before Roblox starts.

`run.sh` launches with diagnostic logging and sets up the AppImage environment.
To collect system information without opening Roblox:

```sh
sh run.sh --diagnose
```

The command prints a folder under `DO_NOT_SHARE/diagnostics/` containing `system.txt`
and, for debug launches, `runtime.log`. Shader updates also save logs there.
Review individual text files before sharing: runtime output may contain account
details. Never upload the entire `DO_NOT_SHARE` folder.

Close Roblox and run `sh update-roblox.sh` from the release folder to update the official LIVE client. Updates require curl and unzip. They replace only `RobloxVersion`, preserving your login and settings. Roblox can independently expire or revoke a session. Replace the AppImage with a new release to update the compatibility runtime.

Edit `FFlags.json` to set flag overrides. It is applied on each launch; only flags supported by Roblox take effect.

The launcher has a headless interface:

```sh
sh run.sh status
sh run.sh check
sh run.sh update                 # newline-delimited JSON progress
sh run.sh --launcher             # open the full launcher on Play
sh run.sh --settings             # open the full launcher on Settings
sh run.sh get auto_update
sh run.sh set auto_update auto    # choices: auto, ask, off
sh run.sh set check_interval_hours 24
```

Settings live in `DO_NOT_SHARE/bootstrapper.json`; defaults are `ask` and a 24-hour check interval, with `0` checking every launch. Exit status `0` means success (including a dropped second launch), `1` means setup/update/check failure, `2` means invalid arguments or settings, and `125` means cancellation. Browser links are passed as opaque in-memory arguments; a launch received while Roblox is running sends a notification and exits without queueing.

Normal game launches and browser links use the bundled GTK3 bootstrapper window when a
first-run setup or update has work to show. Ask mode offers Update now, Launch
without updating, and Skip this version; a short timeout safely launches without
updating. `sh run.sh --settings` opens the same release's settings without taking
the Roblox instance lock. If a display or GTK is unavailable, the launcher
notifies when possible and continues without updating.

## Private data

Startup creates `RobloxLinuxRelease/DO_NOT_SHARE/` for cookies, settings, browser storage, logs, caches and temporary runtime files. Native login cookies are in `DO_NOT_SHARE/cookies/native.sqlite`; browser cookies are in `DO_NOT_SHARE/browser/cookies.sqlite`.

**Never share `DO_NOT_SHARE`.** It is excluded from Git and release archives. Keep it in your installation to retain your profile. Archive tools do not obey `.gitignore`, so explicitly exclude it when sharing your own copy. The launcher redirects HOME, XDG storage and temporary paths into this folder; desktop display/audio sockets still connect to the host.

See the [release README](RobloxLinuxRelease/README.md) for usage, and the [development README](RobloxLinuxDev/README.md) to build the runtime. [Third-party notices](RobloxLinuxDev/THIRD_PARTY.md) and pinned dependency sources are included.

## Status

This is experimental. The tested LIVE client is `0.740.0.7400927` (`version-00a4ca14e31b41e9`). Startup and short TTK/Jailbreak gameplay sessions were checked; full gameplay compatibility and stable 240 FPS across games are not established. Shader preparation validated 1,938 of 1,950 shaders, with 12 unsupported translator cases remaining.

Keep the release in a reasonably short path (for example `~/RobloxLinuxRelease`). Darling can fail to start when deeply nested folder names exceed Unix-socket path limits.
