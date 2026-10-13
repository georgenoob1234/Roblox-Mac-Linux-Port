# Build Roblox Mac Linux Port

This directory contains the runtime, native renderer, browser, shims and packaging source for running the macOS Roblox client on Linux x86_64.

- `runtime/` — launcher, compatibility shims, browser integration, profiler, shader pipeline and AppImage packaging.
- `native/` — x86_64 runtime libraries and Vulkan renderer.
- `third_party/` — exact upstream revisions, archive checksums, source overlays and patches.
- `bootstrap.py` — fetches pinned dependencies into this folder, preserving existing dependency trees.
- `build.sh` — end-to-end build: bootstrap, compile, verify, package AppImage.

---

## 1. Supported build environment

| Component | Details |
|-----------|---------|
| **Host OS** | CachyOS or Arch Linux, x86_64 |
| **Compiler** | Clang/LLVM (tested with 22.1.8; LLVM 23 supported with deployment-target fix) |
| **Build system** | CMake 4.4+, Ninja |
| **Rust** | Cargo (tested with 1.98.1) |
| **Python** | 3.11+ |
| **Container** | Docker — Ubuntu 24.04 baseline for native UI/media dependency packaging |
| **CPU baseline** | x86-64 (generic) — no AVX/AVX2 required in final binaries |
| **glibc** | 2.39+ required on target systems |
| **Disk** | ~25 GB free (est.) for sources, build artifacts, Docker images and AppImage |
| **RAM** | ~8 GB minimum recommended (est.) |

### Why these constraints

- **Clang/LLVM 23 fix:** Clang ≥ 23 lowers Objective-C literals at `-mmacosx-version-min ≥ 10.16` onto Apple-only symbols (`__NSArray0__struct`, `OBJC_CLASS_$_NSConstantIntegerNumber`) that Darling's GNUstep-based runtime does not export. The entire Darling runtime compiles at deployment target 10.15, which keeps the classic `__NSArray0__`/`__NSDictionary0__` ABI that Darling's CoreFoundation provides.
- **Host LLVM vs portable LLVM runtime:** The host supplies the compiler and shader tools. The portable LLVM runtime packages (from pinned Arch archives in `runtime/package/shader-packages.lock`) are bundled into the AppImage for shader compilation on the user's machine. Host libraries are never copied into the release.
- **Ubuntu 24.04 Docker baseline:** Supplies maintained GTK3, WebKitGTK 4.1, SDL3, FFmpeg and GStreamer dependencies. The container recipe is `runtime/package/baseline.Dockerfile`.
- **CPU baseline and glibc 2.39:** `check-portable.py` rejects any ELF that requires a newer CPU baseline or glibc. This ensures the AppImage runs on the broadest range of x86_64 Linux systems.
- **Normal user only:** Do not run the build as root. The build uses unprivileged user namespaces and creates files owned by your user.

---

## 2. Prerequisite installation

### Required packages (Arch/CachyOS)

```sh
sudo pacman -S --needed \
  git curl unzip \
  libarchive \
  binutils patch \
  clang lld llvm \
  cmake ninja \
  rust cargo \
  pkgconf \
  glslang spirv-tools \
  wayland wayland-protocols \
  gtk3 \
  webkit2gtk-4.1 \
  json-glib \
  sdl3 \
  ffmpeg \
  libpulse \
  libxcursor libx11 libegl libgl \
  docker
```

> **Note:** Package names are based on the standard Arch repositories. If a package name has changed or is unavailable, check `pacman -Ss <name>` or the Arch Package Search. The `sdl3` package name should be verified — the Dockerfile builds SDL 3.4.2 from source if the system package is unavailable.

### Additional Darling build dependencies

Darling's CMake configuration checks for its own upstream development dependencies and reports missing packages. Common ones already covered above include `libedit` and `ncurses`. If CMake reports a missing package, install the corresponding Arch package and re-run.

### Docker setup

Docker is used to build the native UI/media dependencies in a pinned Ubuntu 24.04 container. Users do not interact with Docker directly; `build.sh` invokes it.

```sh
# Start and enable the Docker daemon
sudo systemctl enable --now docker

# Add your user to the docker group (required for unprivileged access)
sudo usermod -aG docker "$USER"
# Log out and back in for the group change to take effect
```

> **Security warning:** Docker group membership grants root-equivalent access to the host. Only add trusted users. Do not run Docker on a shared or untrusted machine without understanding the implications.

### Pre-build checks

```sh
# Verify required tools
command -v clang cmake ninja cargo python3 docker bsdtar git curl

# Verify user namespaces are enabled (required by the runtime)
unshare -U true && echo 'User namespaces: OK'

# Verify Docker works without sudo
docker run --rm hello-world

# Check available disk space (need ~25 GB free)
df -h .
```

---

## 3. Fresh clone and dependency bootstrap

```sh
git clone https://github.com/georgenoob1234/Roblox-Mac-Linux-Port.git
cd Roblox-Mac-Linux-Port/RobloxLinuxDev
python3 -B bootstrap.py
```

**What bootstrap does:**

- Reads `third_party/dependencies.json` for pinned URLs, commits and SHA-256 checksums.
- Downloads archives (Darling source, Vulkan headers, ImGui, AppImage tools) into `.downloads/` and verifies SHA-256.
- Clones Git repositories (Indium, metal2vulkan) at pinned commits and applies patches from `third_party/patches/`.
- Extracts Darling Debian packages from the noble amd64 archive.
- Copies source overlays from `third_party/darling-overlay/` onto the extracted Darling source.

**Important:** Bootstrap preserves existing dependency checkouts. If a dependency directory already exists, it is skipped (overlays are still reapplied). Bootstrap does not refresh or repair a stale previously populated tree. To re-fetch a specific dependency, remove its directory first.

---

## 4. Complete build

```sh
cd RobloxLinuxDev
sh build.sh
```

**What `build.sh` does (in order):**

1. Runs `bootstrap.py` to fetch/verify dependencies.
2. Downloads the official Roblox macOS client into `.build/client/`.
3. Unpacks Darling Debian packages and creates the runtime sysroot (`runtime/darling-root`).
4. Builds all compatibility shims (stubs, appkit, xattr, setsid, input, dns, cfnet, identity, debug, metal).
5. Builds the metal2vulkan shader translator (Rust/Cargo).
6. Builds the browser integration (GTK3 + WebKitGTK host process, Wayland client library).
7. Builds optimized audio (CoreAudio replacement using PulseAudio).
8. Builds Darling's base libraries and `darlingserver` at `-O2` with deployment target 10.15.
9. Runs the Darling server reply-ordering check.
10. Builds the optimized Darling base runtime (replaces stock `-O0` Debian libraries).
11. Builds the optimized native runtime (renderer, compat layer, UI, native libraries).
12. Runs the timezone sidecar check and release verification checks.
13. Packages everything into an AppImage.

**Output:** `runtime/package/roblox-mac-x86_64.AppImage`

After building, launch with:

```sh
runtime/bin/roblox-mac
```

The default client is in `.build/client/` and the private profile in `.build/profile/`.

---

## 5. Partial and incremental builds

These commands rebuild specific components. They require a prior complete build (or at minimum, the prerequisite components listed).

| Command | Builds | Requires |
|---------|--------|----------|
| `sh runtime/optimized/darling/build.sh` | Darling runtime at `-O2` | Darling source from bootstrap, `runtime/darling-root` from `m1-darling.sh` |
| `sh runtime/optimized/build.sh` | Full optimized native runtime (appkit shim, UI, renderer, compat, native libs) | Darling sysroot, optimized Darling base |
| `sh runtime/optimized/renderer/build.sh` | Vulkan renderer adapter (`libindium.dylib`, `libtracka-present.dylib`) | Native renderer source (`native/renderer/build.sh`), Darling sysroot |
| `sh runtime/optimized/audio/build.sh` | CoreAudio replacement (`CoreAudio.framework`, `CoreAudio.component`) | Darling sysroot, PulseAudio headers |
| `sh runtime/optimized/compat-build.sh` | Compatibility layer (`libtracka-compat.dylib`, `libtracka-video-host.so`) | Darling sysroot, SDK, FFmpeg pkg-config |
| `sh runtime/browser/build.sh` | Browser host process and Wayland library | GTK3, WebKitGTK 4.1, SDL3, Wayland dev packages |
| `sh runtime/package/build-appimage.sh` | Final AppImage from built components | **All** prior build steps completed; refuses to package if artifacts are missing or stale |

> **Note:** Rebuilding an individual component does not automatically update the AppImage. After rebuilding, re-run `sh runtime/package/build-appimage.sh` to repackage. The AppImage builder runs extensive consistency checks and will fail if optimized artifacts are missing, stale, or overwritten.

---

## 6. Tests and troubleshooting

### Offline / static checks

These run without a GPU, network, or running Roblox client:

```sh
# Release path validation, flag checks, updater rollback, download safety
python3 runtime/package/check-release.py

# AppImage extraction and path handling (no FUSE required)
python3 runtime/package/check-appimage.py /path/to/roblox-mac-x86_64.AppImage

# Signal reporting, log storage, exit-status forwarding, shader log preservation
python3 runtime/package/check-diagnostics.py

# Package provenance, CPU baseline, glibc version, ELF portability
python3 runtime/package/check-portable.py

# EGL/Vulkan driver loading (needs Mesa EGL/Vulkan and vulkaninfo)
python3 runtime/package/check-egl.py runtime/package/AppDir --surfaceless --vulkan

# URI handler registration and pending store
python3 runtime/package/check-uri-handoff.py

# Bootstrapper UI
python3 runtime/package/check-bootstrapper.py
```

### Compiler / ABI verification

```sh
# Darling server reply-ordering and socket checks (built during build.sh)
.build/server-reply-check

# Renderer compute synchronization (GPU optional)
python3 native/renderer/compute-sync-check.py
# With GPU verification of dependent instance updates and buffer copies:
python3 native/renderer/compute-sync-check.py --gpu

# Optimized runtime launch check (built during optimized/build.sh)
python3 runtime/optimized/check-launch.py

# Audio capture check (needs PipeWire, pipewire-pulse, WirePlumber)
python3 runtime/optimized/audio/check-capture.py
```

### Shell / Darling runtime checks

```sh
# Darling sysroot boot test (prints macOS uname/sw_vers)
sh runtime/scripts/m1-darling.sh

# Compat layer checks (crash handling, video host)
sh runtime/optimized/compat-build.sh --check
```

### Renderer / video checks

```sh
# Video compat check (isolated, no client launch)
sh runtime/optimized/compat-build.sh --check-crash
```

### Gameplay verification

Actual gameplay requires a Vulkan-capable GPU and cannot be tested in CI without GPU passthrough. The checks above verify build correctness, library loading, and runtime plumbing, but do not confirm rendering or gameplay.

### Unix-domain socket path constraint

The runtime derives two Unix-domain socket paths from `ROBLOX_MAC_DATA`:

- `shellspawn.sock` at `$PREFIX/var/run/shellspawn.sock`
- UI socket at `/Volumes/SystemRoot$ROBLOX_MAC_DATA/launch.XXXXXX/ui.sock`

Linux `sockaddr_un.sun_path` is 108 bytes (107 usable + NUL). If the data directory is too deep, the runtime refuses to start. **Keep `ROBLOX_MAC_DATA` shallow** — for example, `~/rbx-data` or `~/RobloxLinuxRelease/DO_NOT_SHARE`.

> **Do not** wipe `.build/`, runtime profiles, or Darling prefixes as a generic troubleshooting step. These contain downloaded dependencies, compiled artifacts, and login state. Only remove them if you intend to start over from scratch.

---

## 7. Build a release yourself

```sh
# 1. Complete prerequisites and full build (sections 2–4)
sh build.sh

# 2. Package into a fresh release folder (refuses to overwrite existing)
sh runtime/package/build-release.sh /path/to/new/RobloxLinuxRelease

# 3. Create the distribution archive (excludes DO_NOT_SHARE)
tar -czf RobloxLinuxRelease.tar.gz \
  --exclude='RobloxLinuxRelease/DO_NOT_SHARE' \
  -C /path/to/new RobloxLinuxRelease

# 4. Verify contents
tar -tzf RobloxLinuxRelease.tar.gz | head -20

# 5. Verify no private files are included
tar -tzf RobloxLinuxRelease.tar.gz | grep -iE 'DO_NOT_SHARE|\.build|\.downloads|cookies|logs' && echo 'FAIL: private files found' || echo 'OK: no private files'

# 6. Generate and verify checksum
sha256sum RobloxLinuxRelease.tar.gz > RobloxLinuxRelease.tar.gz.sha256
sha256sum -c RobloxLinuxRelease.tar.gz.sha256
```

**Release folder contents:**

| File | Purpose |
|------|---------|
| `RobloxLinux.AppImage` | The runtime AppImage (does not bundle the Roblox client) |
| `update-roblox.sh` | Downloads/updates the Roblox client and prepares shaders |
| `run.sh` | Diagnostic launcher |
| `bootstrap.sh` / `bootstrap.py` | Dependency fetcher (for advanced users) |
| `install-uri-handler.sh` | Registers `roblox://` and `roblox-player:` URI handler |
| `icon.png` / `icon_bw.png` | Application icons |
| `README.md` | End-user instructions |
| `FFlags.json` | Empty feature flags (users may add their own) |
| `.gitignore` | Prevents accidental commit of private data |

**DO_NOT_SHARE:** Created beside the AppImage at runtime. Contains the user's login, profile, browser storage, and private data. **Never distribute this folder.** Source builds use `.build/profile` and `.build/client` for the same purpose — these are development outputs and must not be distributed.

---

## 8. GitHub Actions / self-hosted runner

### Workflow overview

| Workflow | Trigger | Runner | Purpose |
|----------|---------|--------|---------|
| `ci.yml` → `lint` | Every push, PRs to `main` | `ubuntu-latest` | Shell/Python/JSON/YAML syntax, Darling overlay consistency |
| `ci.yml` → `build` | Pushes to `main` only, when enabled | `[self-hosted, linux, x64, roblox-builder]` | Full build + AppImage verification from `/home/runner/r` |
| `build-release.yml` | `workflow_dispatch` (manual) | Self-hosted build → `ubuntu-latest` publish (publish mode only) | Full build, verified `RobloxLinuxRelease.tar.gz` artifact; optionally tag + publish a GitHub Release |

The older always-publish `release.yml` and the one-off `runner-smoke.yml` were removed; `build-release.yml` replaces them.

### Enable self-hosted builds

The build job is **disabled by default** until the CachyOS VM is ready. To enable:

1. Go to **Settings → Secrets and variables → Actions → Variables**.
2. Click **New repository variable**.
3. Name: `ENABLE_SELF_HOSTED_BUILDS`, Value: `true`.

Until this variable is set to `true`, only the `lint` job runs on pushes. The build job is skipped.

### Persistent short checkout (`/home/runner/r`)

The runtime derives Unix-domain socket paths from the data directory (`RobloxLinuxDev/.build/profile`), and Linux limits socket paths to 107 bytes. The default GitHub Actions workspace path (`/home/runner/actions-runner/_work/repo/repo`) is too deep, so all self-hosted builds use the dedicated short checkout `/home/runner/r`.

That checkout persists between runs. Before every build, `.github/scripts/sync-checkout.sh` synchronizes it to the exact commit being built:

- It fetches the pinned commit SHA and checks it out detached — never a moving branch ref — so the build, packaging and any release always use the same commit.
- It preserves ignored dependency trees and build outputs (`RobloxLinuxDev/.build`, `.downloads`, `metal2vulkan`, ...): no `git reset --hard`, no `git clean`, no symlink relocation. Cached state survives between runs, and a timed-out or failed build is resumed incrementally by re-running the workflow.
- It aborts if the working tree has unexpected tracked modifications instead of overwriting them.
- It refuses to use a checkout whose `origin` points anywhere but this repository.

Because CI and release builds share the persistent checkout, the self-hosted build jobs of `ci.yml` and `build-release.yml` share one concurrency group (`roblox-builder-vm`): only one build runs on the VM at a time, in-progress builds are never cancelled, and queued runs wait (superseding older still-pending runs only).

### Self-hosted runner setup

**Target hardware:**

- Beelink Intel N100 mini PC
- ZimaOS host
- Dedicated CachyOS x86_64 VM (4 vCPUs, ~10 GB RAM, 150 GB+ disk)
- `BUILD_JOBS=2`
- No GPU passthrough required for compilation
- No port forwarding required; runner connects outbound to GitHub

**Register the runner:**

1. Create a dedicated non-root user on the CachyOS VM (e.g., `runner`).
2. Download the runner package from **Settings → Actions → Runners → New self-hosted runner**.
3. Extract and configure:
   ```sh
   sudo -u runner mkdir -p /opt/github-runner && cd /opt/github-runner
   sudo -u runner tar xzf actions-runner-linux-x64-*.tar.gz
   sudo -u runner ./config.sh --url https://github.com/georgenoob1234/Roblox-Mac-Linux-Port --labels linux,x64,roblox-builder
   ```
4. Install and start the service (or run interactively for testing):
   ```sh
   sudo ./svc.sh install runner
   sudo ./svc.sh start
   ```
5. Install the build prerequisites (section 2) as the `runner` user.
6. Ensure Docker is accessible to the `runner` user (`usermod -aG docker runner`).

**Security:**

- This is a **public repository**. Never run arbitrary PR code on the persistent self-hosted VM.
- The CI build job runs **only on trusted pushes to `main`**, never on untrusted PRs.
- Keep the VM isolated from ZimaOS/NAS shares and private networks.
- The CI working directory must be **dedicated to CI** — do not share it with personal development checkouts, login profiles, or private data.
- The runner's persistent `/home/runner/r` checkout preserves build caches between runs (see above). Ensure it contains only CI-related data.

### Manual build and release (Build Release workflow)

Go to **Actions → Build Release → Run workflow** (main branch only) and choose a mode:

- **Artifact only (default):** leave `publish` unchecked and the tag empty. The workflow validates the inputs, runs the full build on the self-hosted runner, packages and verifies `RobloxLinuxRelease.tar.gz` plus its SHA-256 checksum, and uploads it as a workflow artifact named `RobloxLinuxRelease-<short SHA>` (14-day retention). **No tag and no GitHub Release are created.** Download the artifact from the workflow run page under **Artifacts** (after unzipping, `sha256sum -c RobloxLinuxRelease.tar.gz.sha256`).
- **Build and publish:** check `publish` and enter a version tag (e.g., `v0.2.0`). The tag is required, must match `vX.Y.Z` (optional `-suffix`), and must not already exist — the workflow refuses to overwrite existing tags or releases. Once the build, packaging, archive validation and artifact upload have all succeeded, a separate GitHub-hosted job with only `contents: write` re-verifies the artifact checksum, creates an annotated tag pointing at the exact built commit, and publishes a GitHub Release with `RobloxLinuxRelease.tar.gz` and its checksum. The release URL is printed at the end of the run and in the run summary. Published releases appear under **Releases** on the repository page.

Notes:

- Tag format and availability are validated before the long self-hosted build starts; a stray tag with `publish` unchecked is rejected.
- Publishing happens immediately after verification, without an extra approval prompt. For an optional approval step, create a GitHub Environment with required reviewers in **Settings → Environments** and add `environment: <name>` to the `publish` job.
- The first full build on the N100 VM bootstraps everything (Darling sysroot, dependencies, Roblox client download) and can take many hours; the job timeout is 12 hours. Build state persists in `/home/runner/r`, so re-running the workflow after a timeout or failure resumes incrementally instead of starting over.
- Runner prerequisites are those of section 2 (CachyOS packages, Docker access for the `runner` user, ~25 GiB free disk); the runner must never run as root. `BUILD_JOBS=2`. Expected limitations: 4 vCPUs mean long compile times, and only one build runs on the VM at a time.
- Only the original repository's `main` branch can dispatch the self-hosted build; pull requests and forks never run on the VM.

---

## Checks and private files

`python3 runtime/package/check-release.py` runs the offline storage, flags, updater preservation/rollback and download validation checks. The full build also runs compiler/ABI checks and an isolated guest timezone check. It does not establish full gameplay compatibility.

`python3 runtime/package/check-appimage.py /path/to/RobloxLinux.AppImage` checks a built image through a memory-backed executable, reproducing AppImageLauncher's path handling and verifying extraction without FUSE and private data paths.

`python3 runtime/package/check-diagnostics.py` checks signal reporting, private log storage, exit-status and argument forwarding, and shader log preservation. The launcher accepts `--diagnose` for a system report or `--debug` before its usual arguments to log a run.

`python3 runtime/package/check-portable.py` checks that optimized or unverified packages are rejected. To exercise LLVM disassembly and native UI loading on a CPU without AVX, run it inside the baseline container with `--appdir /out`, `--sysroot /`, `--llvm-as /portable/usr/bin/llvm-as`, and `--qemu` pointing to a mounted `qemu-x86_64-static`. This also tests with Ubuntu's graphics loader libraries instead of accidentally testing the build host's CPU-specific drivers. The packaged `browser/BUILD-PACKAGES.txt` records Ubuntu/source package versions; `browser/SHADER-PACKAGES.json` records shader package versions and SHA-256 hashes. `--appdir` also rejects newer glibc requirements in every ELF. Packaging checks native UI library loading and shader-tool startup inside Ubuntu 24.04 itself.

Run `python3 runtime/package/check-egl.py runtime/package/AppDir --surfaceless --vulkan` on the baseline and in a Fedora container with Mesa EGL/Vulkan drivers, Wayland/XCB libraries and `vulkaninfo` installed. This loads each host EGL driver after the bundled UI, initializes Mesa EGL and creates a Vulkan instance; it catches library conflicts that loading the UI alone misses. Wayland and XCB libraries must stay on the host with its graphics drivers, which may need newer display symbols.

The renderer build checks compute synchronization using its production barrier code. Run `python3 native/renderer/compute-sync-check.py --gpu` to also verify dependent instance updates and buffer copies on a Vulkan GPU; this requires `glslangValidator` and the Vulkan loader development library. The optional `--negative-control` reports corruption when the dependency is omitted, without requiring undefined behavior to reproduce on every driver.

Build CoreAudio before regenerating `runtime/shims/fill`: the flat-namespace loader otherwise lets a generated placeholder override the real audio clock. Packaging rejects audio-clock placeholders and missing clock exports.

After `sh runtime/optimized/audio/build.sh`, run `python3 runtime/optimized/audio/check-capture.py` to check microphone initialization, changing PCM samples and restarts through `AudioUnitRender`. It uses a private PipeWire server and a synthetic monitor source, with no physical microphone or speaker; PipeWire, pipewire-pulse and WirePlumber are required.

The build also compiles Darling's base libraries and server at `-O2`, preserving the project's framework replacements. The server's immediate-reply implementation avoids an extra main-thread handoff on synchronous guest calls. Its socket check covers reply ordering, callbacks, a full socket and a disconnected peer; packaging rejects overwritten optimized artifacts, the stock server and unapplied source overlays. Foundation stays on the stock build: replacing only that framework fixes the optimized build's reproducible Home-loading regression (HTTP 500 from the recommendations request). The installer retains it and packaging verifies its stock checksum. A clean base-runtime build can take substantially longer than rebuilding the adapters.

The launcher tears down its guest namespace if the namespace supervisor dies. The Darling CLI also watches the server with a Linux pidfd, so a server failure cannot leave it waiting forever for the guest exit status. The base-runtime build checks normal command exit and server failure before/during that wait.

For one-window frame captures, use `--frame-trace /absolute/path/to/new.csv` before `--place-id ID`. Analyze GL/Vulkan transitions together with `python3 native/frame-times.py /path/to/new.csv --all-contexts --start 30 --duration 30`. This measures completed presentation calls, not GPU execution or compositor scanout. Keep captures under `DO_NOT_SHARE`, leave verbose renderer logging off, and run no builds during gameplay measurements.

F8 opens the normal build's Custom Settings overlay, an ImGui panel hosted by GTK above the game. The game keeps its Vulkan presenter while the HUD is visible; skipped Cocoa flushes do not count as frames. GTK refreshes the HUD at 15 Hz by default, independently of game rendering. Guest sampled addresses use mapped module names and offsets in this host-side HUD because Darwin symbol resolution requires a guest thread.

Metal application completion handlers run outside GPU polling, so a blocked handler cannot stop unrelated uploads from completing. Handlers stay ordered within each command; `waitUntilCompleted` also waits for those handlers, as required by [Metal's API](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted()). The renderer's existing check covers blocked callbacks and independent completion progress.

The release creates `DO_NOT_SHARE` beside its AppImage. Source builds use `.build/profile`, `.build/client` and runtime build/check folders; they are development outputs and must not be distributed. Publish tracked source files or the clean release archive, not a ZIP of this entire development working directory.

See [THIRD_PARTY.md](THIRD_PARTY.md) for source provenance and licenses. Internal library names such as `libtracka-*.dylib` remain for loader compatibility; there are no separate Track A/B products.
