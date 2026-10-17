# Roblox Mac Linux Port — safe installer

The installer is a user-local Python 3 control-plane script. It uses only the
Python standard library, never needs `sudo`, and owns only files listed in its
manifest. `DO_NOT_SHARE` is never removed, replaced, or followed through a
symlink.

The one-command bootstrap at [`../install.sh`](../install.sh) downloads this
script from the official repository and runs `install --yes`. To inspect the
bootstrap first, use the `curl`, `less`, and `sh` commands in the top-level
README.

## Install

For a normal installation, run the root bootstrap. For an already extracted
release directory, run the implementation directly and identify that release
with `--source`:

```sh
python3 installer/installer.py install --source "$HOME/Downloads/RobloxLinuxRelease" --yes
```

The default destination is `~/.local/opt/roblox-mac-linux-port`; use
`--prefix` to choose another user-writable directory. The source release is
copied into a staged transaction before files are switched. Existing paths with
unknown ownership are preserved and reported instead of being overwritten.

If the source folder has `DO_NOT_SHARE` and the destination does not, `--yes`
allows a copy while leaving the source data untouched. Without `--yes`, an
interactive install asks first; a non-interactive install refuses to change
files.

## Update, status, and repair

```sh
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" update --yes
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" status
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" repair --yes
```

Updates download the latest official GitHub release and verify its published
SHA-256 checksum before extraction. To update from a local extracted release:

```sh
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" update \
  --source "$HOME/Downloads/RobloxLinuxRelease" --yes
```

Updates stage and hash the payload before an atomic switch. If an operation is
interrupted, the next install, update, repair, or uninstall finds the journal,
rolls back safely, and keeps evidence when recovery is incomplete. Close Roblox
before changing the installation.

`FFlags.json` is user configuration. Its contents survive updates and
uninstallation; remove it yourself before an update only when you intentionally
want the release default.

## Desktop launcher and URI handler

`install` installs the user-local `.desktop` launchers and icons and registers
`roblox://` and `roblox-player:` only where it can do so without silently
replacing another application. The same operations are explicit:

```sh
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" desktop install --yes
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" desktop uninstall --yes
```

A conflicting desktop file or icon is left untouched. To explicitly replace a
conflict, back it up beside the affected location, and then install:

```sh
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" desktop install \
  --force-desktop --yes
```

MIME editing changes only this application's entries. Existing handlers remain
in the association list, and uninstall removes only this application's entries.

## Safe uninstall

```sh
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" uninstall --yes
```

Uninstall removes only unchanged files recorded in the manifest. It preserves
`DO_NOT_SHARE`, `FFlags.json`, modified application files, unknown files,
`RobloxVersion`, and unrelated contents. If preserved data remains, the
installation directory stays in place. `--remove-client-cache` may remove a
recognized `RobloxVersion` and non-transactional staging cache, but it never
removes `DO_NOT_SHARE`.

Use `--dry-run` with install, update, uninstall, or desktop actions to preview
changes without modifying files. Installer logs are stored at
`$XDG_STATE_HOME/roblox-mac-linux-port/installer.log` (or
`~/.local/state/roblox-mac-linux-port/installer.log`) and do not read
`DO_NOT_SHARE` contents.

The application itself still requires the x86_64 Linux environment, compatible
Vulkan drivers, Wayland, and runtime tools documented in the release README.
