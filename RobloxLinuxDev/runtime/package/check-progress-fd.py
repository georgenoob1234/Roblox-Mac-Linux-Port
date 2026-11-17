#!/usr/bin/env python3
"""ROBLOX_MAC_PROGRESS_FD names a descriptor number. A reader may only write
to it when that number was the inherited progress pipe at startup: when it
was not, the number is free and the process's own pipes, sockets or files
can land on it later (found 2026-11-17: prepare-cache.py wrote a progress
line into subprocess's internal error pipe and aborted shader preparation).
Each reader claims the descriptor once, at startup, or never."""
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOP = HERE.parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CHILD = r'''
import os, sys, importlib.util
kind, path = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location("m", path)
m = importlib.util.module_from_spec(spec)
sys.modules["m"] = m
if kind == "installer":
    sys.argv = ["installer.py"]
spec.loader.exec_module(m)
# FD 8 was not inherited; something of the process's own now lands on it.
r, w = os.pipe()
while w < 8:
    w = os.dup(w)
if w != 8:
    os.dup2(w, 8)
if kind == "prepare-cache":
    report = m.progress_reporter(10)
    for done in range(1, 11):
        report(done)
else:
    m.progress_event("downloading", 30, "test")
os.close(8)
if r == 8:
    raise SystemExit("test setup error")
os.set_blocking(r, False)
try:
    data = os.read(r, 4096)
except BlockingIOError:
    data = b""
print(repr(data))
'''


def written_to_foreign_fd(kind, path):
    """Run a reader with ROBLOX_MAC_PROGRESS_FD=8 but FD 8 not inherited, then
    put a pipe of the process's own on 8: nothing may be written into it."""
    env = {**os.environ, "ROBLOX_MAC_PROGRESS_FD": "8"}
    result = subprocess.run([sys.executable, "-c", CHILD, kind, str(path)], env=env,
                            capture_output=True, text=True, close_fds=True, timeout=60)
    assert result.returncode == 0, (kind, result.stderr)
    print(f"     {kind} wrote into the foreign descriptor: {result.stdout.strip()[:90]}")
    return result.stdout.strip()


def written_to_inherited_pipe(kind, path):
    """With FD 8 the inherited progress pipe, the events arrive."""
    r, w = os.pipe()
    os.dup2(w, 8); os.close(w)
    code = ("import sys, importlib.util\n"
            "kind = sys.argv[1]\n"
            "spec = importlib.util.spec_from_file_location('m', sys.argv[2]); m = importlib.util.module_from_spec(spec); sys.modules['m'] = m\n"
            "sys.argv = ['x']; spec.loader.exec_module(m)\n"
            "r = m.progress_reporter(10) if kind == 'prepare-cache' else None\n"
            "[r(d) for d in range(1, 11)] if r else m.progress_event('downloading', 30, 'test')\n")
    try:
        subprocess.run([sys.executable, "-c", code, kind, str(path)], env={**os.environ, "ROBLOX_MAC_PROGRESS_FD": "8"},
                       pass_fds=(8,), check=True, timeout=60)
    finally:
        os.close(8)
    with os.fdopen(r) as reader:
        return reader.read()


def translate_all_probe_survives(shell):
    """translate-all.sh checks the named descriptor before using it. The check
    must not end the script when the descriptor is closed (review 2026-11-17:
    `: >&8` is a special builtin, and its failed redirection exits the shell)."""
    text = (HERE.parent / "metal" / "translate-all.sh").read_text()
    start = text.index("progress_fd=${ROBLOX_MAC_PROGRESS_FD:-}")
    end = text.index("\n", text.index("|| progress_fd=", start))
    probe = text[start:end] + '\necho "survived fd=[$progress_fd]"\n'
    result = subprocess.run([shell, "-c", probe], env={**os.environ, "ROBLOX_MAC_PROGRESS_FD": "8"},
                            capture_output=True, text=True, close_fds=True, timeout=30)
    return result.stdout.strip() == "survived fd=[]", (shell, result.returncode, result.stdout, result.stderr)


def main():
    import shutil
    failed = 0
    for shell in ("sh", "dash"):
        if not shutil.which(shell):
            print(f"skip translate-all probe under {shell}: not installed")
            continue
        ok, detail = translate_all_probe_survives(shell)
        if ok:
            print(f"ok   translate-all.sh: a closed descriptor is dropped, not fatal ({shell})")
        else:
            failed += 1
            print(f"FAIL translate-all.sh probe under {shell}: {detail}")
    readers = [("prepare-cache", HERE.parent / "metal" / "prepare-cache.py"),
               ("installer", TOP / "installer" / "installer.py")]
    for kind, path in readers:
        for label, check in (("never writes to a descriptor it did not inherit",
                              lambda: written_to_foreign_fd(kind, path) == "b''"),
                             ("writes to the inherited progress pipe",
                              lambda: '"phase"' in written_to_inherited_pipe(kind, path))):
            try:
                assert check(), (kind, label)
                print(f"ok   {kind}: {label}")
            except AssertionError as error:
                failed += 1
                print(f"FAIL {kind}: {label}: {error}")
    if failed:
        sys.exit(f"check-progress-fd: {failed} failed")
    print("check-progress-fd: all passed")


if __name__ == "__main__":
    main()
