#!/usr/bin/env python3
"""Store one Roblox browser URI as opaque bytes in the release private data."""
import fcntl
import os
import tempfile
from contextlib import contextmanager
from pathlib import Path

def paths(data):
    root = Path(data)
    return root / "pending-uri", root / "pending-uri.lock"

def _raw(value):
    if not isinstance(value, str) or not value:
        raise ValueError("URI must be a non-empty string")
    return os.fsencode(value)

@contextmanager
def _lock(lock_path, shared=False):
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+b") as stream:
        os.chmod(lock_path, 0o600)
        fcntl.flock(stream, fcntl.LOCK_SH if shared else fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(stream, fcntl.LOCK_UN)

def _atomic(path, data):
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.chmod(name, 0o600); os.replace(name, path); os.chmod(path, 0o600)
        directory = os.open(path.parent, os.O_DIRECTORY)
        try: os.fsync(directory)
        finally: os.close(directory)
    except BaseException:
        try: os.unlink(name)
        except OSError: pass
        raise

def receive(data, uri):
    pending, lock = paths(data); value = _raw(uri)
    pending.parent.mkdir(parents=True, exist_ok=True)
    with _lock(lock): _atomic(pending, value)
    return metadata(value)

def peek(data):
    pending, lock = paths(data)
    if not pending.exists():
        return None
    with _lock(lock, shared=True):
        try: return os.fsdecode(pending.read_bytes())
        except OSError: return None

def clear_if_consumed(data, uri):
    pending, lock = paths(data); value = _raw(uri)
    with _lock(lock):
        try:
            if pending.read_bytes() != value: return False
            pending.unlink(); return True
        except OSError: return False

def metadata(value):
    scheme = value.split(b":", 1)[0].decode("ascii", "replace")
    return scheme, len(value)

if __name__ == "__main__":
    raise SystemExit("library only")
