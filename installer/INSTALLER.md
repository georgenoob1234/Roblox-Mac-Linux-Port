# Roblox Mac Linux Port — safe installer

The installer is a user-local Python 3 control-plane script. It uses only the
Python standard library, never needs `sudo`, and owns only files listed in its
manifest. `DO_NOT_SHARE` is never removed, replaced, or followed through a
symlink.

The one-command bootstrap at [`../install.sh`](../install.sh) downloads this
script from the official repository and opens its state-aware menu when a
terminal is available. In CI or another noninteractive shell it uses
`install --yes`. To inspect the bootstrap first, use the `curl`, `less`, and
`sh` commands in the top-level README.

Running `python3 installer/installer.py` without arguments in a terminal opens
the state-aware menu. It shows only the actions that fit the detected state,
offers an update before the menu when a newer release is available, and asks
whether desktop links should be enabled during a fresh install.

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
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" recover
```

`recover` only replays or rolls back an interrupted file transaction; it does
not download a release or modify `DO_NOT_SHARE`/`RobloxVersion`.

Because an update replaces `installer.py` and `run.sh` themselves, the
installer also keeps a standalone recovery entry point outside the install
directory, at `$XDG_STATE_HOME/roblox-mac-linux-port/recovery/recover.sh` (or
`~/.local/state/roblox-mac-linux-port/recovery/recover.sh`). It is refreshed
before every transaction and still works when `run.sh` or `installer.py` was
interrupted mid-replacement; `run.sh` calls it automatically before using any
other file. Run it directly if the launcher itself was interrupted:

```sh
"$HOME/.local/state/roblox-mac-linux-port/recovery/recover.sh" \
  "$HOME/.local/opt/roblox-mac-linux-port"
```

Updates download the latest official GitHub release and verify its published
SHA-256 checksum before extraction. To update from a local extracted release:

```sh
python3 "$HOME/.local/opt/roblox-mac-linux-port/installer.py" update \
  --source "$HOME/Downloads/RobloxLinuxRelease" --yes
```

The installed release records its port version in `.roblox-mac-port-version`;
the GTK Settings Port updates section uses the same release metadata, checksum,
ownership manifest and transaction path as this CLI.

Updates stage and hash the payload before an atomic switch. If an operation is
interrupted, the next install, update, repair, or uninstall finds the journal,
rolls back safely, and keeps evidence when recovery is incomplete. Close Roblox
before changing the installation.

Cancellation is decided by the installer, not by the launcher. It accepts a
cancel request (the `Cancel` button in the progress window, written to a marker
file) during download and staging, stops honouring one once the commit begins,
and reports an accepted cancellation with exit status 125. A late or stale
request therefore cannot turn a committed update into a reported cancellation,
and the launcher never kills the installer during the non-cancellable commit
phase. The cancellable phases are bounded instead (network socket timeouts plus
`ROBLOX_MAC_PORT_UPDATE_TIMEOUT`, default 30m).

The commit boundary is settled through a small shared state file, not by
progress-event delivery: the launcher writes `open` before an operation, the
installer records `committing` at the boundary, and the GTK UI reads the same
file under the same lock before recording a request. Whoever takes the lock
first wins, so a request that loses to the commit is refused and shown as
refused, and one that wins still cancels safely. Nothing about the report
depends on the UI having processed a progress message. A stale marker or state
from a previous run is cleared before each new operation (under the instance
lock), so it can never cancel or block a later update.

If the release commits but the obsolete-backup cleanup cannot finish, the
installer exits with status 3 and the launcher reports the update as applied
with cleanup pending -- never as a failure that kept the previous release,
because the new release is already live. The committed transaction is kept as
evidence and recovery retries the cleanup on the next start; a committed
installation is never rolled back because deleting old backups failed.

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

The interactive menu asks whether to keep your data. The default-safe CLI
equivalent is `--keep-data`; use `--purge-data` only when you explicitly want
the app-owned `DO_NOT_SHARE`, `RobloxVersion`, `.update-work`, and managed
configuration removed from the install root. Unknown files and symlinks that
could redirect outside the root are preserved and reported. Preserved data is
left at the install path and can be deleted manually later if you change your
mind.

Use `--dry-run` with install, update, uninstall, or desktop actions to preview
changes without modifying files. Installer logs are stored at
`$XDG_STATE_HOME/roblox-mac-linux-port/installer.log` (or
`~/.local/state/roblox-mac-linux-port/installer.log`) and do not read
`DO_NOT_SHARE` contents.

The application itself still requires the x86_64 Linux environment, compatible
Vulkan drivers, Wayland, and runtime tools documented in the release README.
