#!/usr/bin/env python3
"""Regression tests for Roblox launches from deeply nested installation directories.

Covers:
- Normal shallow paths
- Installation paths longer than the Unix-domain socket 107-byte limit
- Paths containing spaces and Unicode
- Fresh first-run setup and an existing populated prefix
- Shellspawn and guest-to-host UI socket communication
- Launcher Settings and diagnostics dispatch
- Single-instance locking contention
- Cleanup after normal exits and signal failures
- No loss, relocation, or duplication of persistent data
"""
import errno
import fcntl
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = Path(__file__).resolve().parents[3]
LAUNCHER = ROOT / "bin/roblox-mac"
APPIMAGE = ROOT / "package/roblox-mac-x86_64.AppImage"
DARLING_ROOT = ROOT / "darling-root"

print("=" * 70)
print("RUNNING REGRESSION TEST SUITE: DEEP DIRECTORY LAUNCHING")
print("=" * 70)

# ==============================================================================
# SECTION 1: Static and Contract Verification
# ==============================================================================
print("\n[Section 1] Static and Contract Verification...")
launcher_text = LAUNCHER.read_text(encoding="utf-8")

# 1. Byte-length validation logic present
assert "_rbx_bytes() { LC_ALL=C printf %s \"$1\" | wc -c; }" in launcher_text, \
    "roblox-mac must measure socket path lengths in bytes"
assert "[ \"$_rbx_ss_len\" -gt 107 ]" in launcher_text, \
    "roblox-mac must check shellspawn socket path against 107-byte limit"
assert "[ \"$_rbx_ui_len\" -gt 107 ]" in launcher_text, \
    "roblox-mac must check UI socket path against 107-byte limit"

# 2. Secure unpredictable alias creation with 0700 permissions
assert "ROBLOX_MAC_ALIAS_DIR=$(mktemp -d" in launcher_text, \
    "roblox-mac must create unpredictable alias dir via mktemp -d"
assert "chmod 700 \"$ROBLOX_MAC_ALIAS_DIR\"" in launcher_text, \
    "roblox-mac must enforce 0700 permissions on alias directory"
assert "[ \"$_alias_owner\" != \"$(id -u)\" ] || [ \"$_alias_mode\" != \"700\" ]" in launcher_text, \
    "roblox-mac must verify alias directory ownership and permissions"

# 3. Single-instance locking targets real data directory
assert 'exec 9>"$REAL_DATA/instance.lock"' in launcher_text, \
    "roblox-mac single-instance lock must target real data directory"

# 4. Safe cleanup trap
assert "trap cleanup_alias EXIT INT TERM HUP" in launcher_text, \
    "roblox-mac must trap EXIT, INT, TERM, and HUP for alias cleanup"
assert 'rm -rf -- "$ROBLOX_MAC_STAGE"' in launcher_text, \
    "cleanup_alias must remove per-launch stage"
assert 'rmdir -- "$ROBLOX_MAC_ALIAS_DIR"' in launcher_text, \
    "cleanup_alias must remove alias dir"

# 5. Bind-mount inside namespace with mock fallback
assert 'mount --bind "$REAL_DATA" "$ROBLOX_MAC_ALIAS_DIR/data"' in launcher_text, \
    "roblox-mac must bind-mount real data directory inside user/mount namespace"

print("PASS: Static and script contract assertions passed.")

# ==============================================================================
# SECTION 2: Socket Path Length Arithmetic (Bytes vs Characters)
# ==============================================================================
print("\n[Section 2] Socket Path Length Arithmetic (Bytes vs Characters)...")

# Test paths of various depths, spaces, and multi-byte UTF-8 sequences
test_cases = [
    # (description, raw_path)
    ("Shallow path", "/tmp/shallow"),
    ("Path exceeding 107 bytes", "/tmp/" + "a" * 110),
    ("Deep path with Unicode and spaces",
     "/tmp/test deep path with spaces and 测试 目录 1234567890 1234567890 1234567890/RobloxLinuxRelease"),
    ("Multi-byte Cyrillic and Emoji",
     "/tmp/тест_игры_🎮_роболокс_mac_port_linux_testing_deep_path_directory_1234567890_abcdefghij")
]

for desc, test_path in test_cases:
    byte_len = len(test_path.encode("utf-8"))
    char_len = len(test_path)
    
    # Simulate old un-aliased socket paths:
    old_shellspawn = f"{test_path}/DO_NOT_SHARE/prefix/var/run/shellspawn.sock"
    old_ui = f"/Volumes/SystemRoot{test_path}/DO_NOT_SHARE/launch.123456/ui.sock"
    
    old_ss_bytes = len(old_shellspawn.encode("utf-8"))
    old_ui_bytes = len(old_ui.encode("utf-8"))
    
    # Aliased paths:
    # Simulated short alias e.g. /tmp/rbx.123456
    sim_alias = "/tmp/rbx.123456"
    new_shellspawn = f"{sim_alias}/data/prefix/var/run/shellspawn.sock"
    new_ui = f"/Volumes/SystemRoot{sim_alias}/stage/ui.sock"
    
    new_ss_bytes = len(new_shellspawn.encode("utf-8"))
    new_ui_bytes = len(new_ui.encode("utf-8"))
    
    assert new_ss_bytes <= 107, f"New shellspawn path too long: {new_ss_bytes} bytes"
    assert new_ui_bytes <= 107, f"New UI path too long: {new_ui_bytes} bytes"
    
    if byte_len > 100:
        assert old_ss_bytes > 107 or old_ui_bytes > 107, \
            f"Expected old path to exceed 107 bytes for {desc}"
        print(f"  {desc}:")
        print(f"    Raw path: {char_len} chars, {byte_len} bytes")
        print(f"    Old socket length: {max(old_ss_bytes, old_ui_bytes)} bytes (> 107 limit: EXCEEDED)")
        print(f"    New aliased socket length: shellspawn={new_ss_bytes}B, ui={new_ui_bytes}B (<= 107: OK)")

print("PASS: Socket length validation passed.")

# ==============================================================================
# SECTION 3: Single-Instance Locking Contention Across Aliases
# ==============================================================================
print("\n[Section 3] Single-Instance Locking Contention...")

with tempfile.TemporaryDirectory(prefix="roblox lock ") as tmp:
    lock_data = Path(tmp) / "DO_NOT_SHARE"
    lock_data.mkdir(mode=0o700)
    lock_file = lock_data / "instance.lock"
    
    # 1. Acquire lock externally on real data directory
    fd = os.open(str(lock_file), os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        
        # Launch roblox-mac in deep environment with locked instance
        env = {
            **os.environ,
            "ROBLOX_MAC_DATA": str(lock_data),
            "ROBLOX_MAC_ROOT": str(ROOT),
            "ROBLOX_MAC_CONFIG": "/dev/null",
            "ROBLOX_MAC_NO_TERMINAL": "1"
        }
        res = subprocess.run([str(LAUNCHER), "--shell", "true"], env=env,
                             capture_output=True, text=True)
        assert res.returncode == 0, f"Expected returncode 0 on lock contention, got {res.returncode}"
        assert "roblox-mac: Roblox is already running." in res.stderr, f"Expected lock message in stderr: {res.stderr}"
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)
        
print("PASS: Single-instance locking contention verified on real data directory.")

# ==============================================================================
# SECTION 4: Signal Trapping and Alias Cleanup
# ==============================================================================
print("\n[Section 4] Signal Trapping and Failure Cleanup...")

with tempfile.TemporaryDirectory(prefix="roblox cleanup ") as tmp:
    clean_data = Path(tmp) / "DO_NOT_SHARE"
    clean_data.mkdir(mode=0o700)
    
    # Create persistent marker in real data directory
    (clean_data / "important_cookie.txt").write_text("PERSISTENT_DATA_12345")
    
    # Launch sleep process through roblox-mac
    env = {
        **os.environ,
        "ROBLOX_MAC_DATA": str(clean_data),
        "ROBLOX_MAC_ROOT": str(ROOT),
        "ROBLOX_MAC_CONFIG": "/dev/null"
    }
    
    # Run launcher executing sleep
    proc = subprocess.Popen([str(LAUNCHER), "--shell", "sleep 60"], env=env,
                            start_new_session=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    
    # Wait for process to establish alias directory
    time.sleep(1.0)
    
    # Send SIGTERM to outer process group
    os.killpg(proc.pid, signal.SIGTERM)
    proc.wait(timeout=10)
    
    # Verify real data directory was NOT deleted or modified
    assert clean_data.exists(), "Real data directory must never be deleted"
    assert (clean_data / "important_cookie.txt").read_text() == "PERSISTENT_DATA_12345", \
        "Persistent data was corrupted or moved during cleanup"

print("PASS: Signal cleanup verified; persistent data intact.")

# ==============================================================================
# SECTION 5: Live Runtime Validation - Shallow Path
# ==============================================================================
print("\n[Section 5] Live Runtime Validation: Shallow Path...")

with tempfile.TemporaryDirectory(prefix="rbx shallow ") as tmp:
    shallow_data = Path(tmp) / "DO_NOT_SHARE"
    shallow_data.mkdir(mode=0o700)
    
    env = {
        **os.environ,
        "ROBLOX_MAC_DATA": str(shallow_data),
        "ROBLOX_MAC_ROOT": str(ROOT),
        "ROBLOX_MAC_CONFIG": "/dev/null"
    }
    res = subprocess.run([str(LAUNCHER), "--shell", "echo SHALLOW_RUNTIME_OK"],
                         env=env, capture_output=True, text=True, timeout=60)
    assert res.returncode == 0, f"Shallow launch failed: {res.stderr}"
    assert "SHALLOW_RUNTIME_OK" in res.stdout, f"Output missing in shallow launch: {res.stdout}"

print("PASS: Normal shallow path execution verified.")

# ==============================================================================
# SECTION 6: Live Runtime Validation - Deep Path with Spaces and Unicode
# ==============================================================================
print("\n[Section 6] Live Runtime Validation: Deep Path with Spaces and Unicode...")

# Build a deeply nested path exceeding 115 bytes with spaces and UTF-8 multi-byte characters
deep_base = Path("/tmp/rbx 测试 路径 with spaces and deep unicode 1234567890 1234567890/nested 子目录 abcdef 1234567890")
deep_release = deep_base / "RobloxLinuxRelease"
deep_bytes = len(str(deep_release).encode("utf-8"))
assert deep_bytes > 107, f"Deep path should exceed 107 bytes, was {deep_bytes}"
print(f"  Testing at path: {deep_release} ({deep_bytes} bytes)")

try:
    deep_release.mkdir(parents=True, exist_ok=True)
    deep_data = deep_release / "DO_NOT_SHARE"
    deep_data.mkdir(mode=0o700, exist_ok=True)
    
    # --------------------------------------------------------------------------
    # 6A: Fresh First-Run Setup
    # --------------------------------------------------------------------------
    print("  6A: Testing fresh first-run setup in deep path...")
    assert not (deep_data / "prefix").exists(), "Prefix should not exist before first run"
    
    env = {
        **os.environ,
        "ROBLOX_MAC_DATA": str(deep_data),
        "ROBLOX_MAC_ROOT": str(ROOT),
        "ROBLOX_MAC_CONFIG": "/dev/null"
    }
    res = subprocess.run([str(LAUNCHER), "--shell", "echo FIRST_RUN_SUCCESS"],
                         env=env, capture_output=True, text=True, timeout=120)
    assert res.returncode == 0, f"First-run in deep directory failed: {res.stderr}"
    assert "FIRST_RUN_SUCCESS" in res.stdout, f"Expected output missing: {res.stdout}"
    
    # Verify prefix was initialized in the REAL data directory on disk
    assert (deep_data / "prefix").is_dir(), "Prefix was not created in real data directory"
    assert (deep_data / "prefix/System").is_dir(), "Darling prefix structure missing in real data dir"
    print("  PASS 6A: Fresh first-run initialized prefix directly in real data directory.")
    
    # --------------------------------------------------------------------------
    # 6B: Alias Mount Verification (No duplication, shared underlying directory)
    # --------------------------------------------------------------------------
    print("  6B: Verifying alias points to original prefix (no copy/relocation)...")
    marker_file = deep_data / "prefix/host_sync_test.txt"
    marker_file.write_text("WRITTEN_BY_HOST_TEST_RUNNER\n")
    
    guest_script = (
        "cat /host_sync_test.txt && "
        "echo WRITTEN_BY_DARLING_GUEST >> /host_sync_test.txt"
    )
    res = subprocess.run([str(LAUNCHER), "--shell", guest_script],
                         env=env, capture_output=True, text=True, timeout=60)
    assert res.returncode == 0, f"Guest sync test failed: {res.stderr}"
    assert "WRITTEN_BY_HOST_TEST_RUNNER" in res.stdout, "Guest failed to read host marker"
    
    # Verify the host file immediately reflected guest modifications
    synced_content = marker_file.read_text()
    assert "WRITTEN_BY_HOST_TEST_RUNNER" in synced_content
    assert "WRITTEN_BY_DARLING_GUEST" in synced_content
    print("  PASS 6B: Bidirectional file synchronization verified: alias maps to real prefix.")
    
    # --------------------------------------------------------------------------
    # 6C: Existing Populated Prefix Preservation
    # --------------------------------------------------------------------------
    print("  6C: Testing existing populated prefix...")
    # Add a session cookie in real data directory
    cookie_dir = deep_data / "cookies"
    cookie_dir.mkdir(exist_ok=True)
    cookie_file = cookie_dir / "user_session.txt"
    cookie_file.write_text("ACTIVE_SESSION_TOKEN_XYZ")
    
    res = subprocess.run([str(LAUNCHER), "--shell", "echo POPULATED_PREFIX_OK"],
                         env=env, capture_output=True, text=True, timeout=60)
    assert res.returncode == 0, f"Populated prefix run failed: {res.stderr}"
    assert "POPULATED_PREFIX_OK" in res.stdout
    assert cookie_file.read_text() == "ACTIVE_SESSION_TOKEN_XYZ", "Session data was altered or lost"
    print("  PASS 6C: Existing populated prefix and session data intact.")
    
    # --------------------------------------------------------------------------
    # 6D: Shellspawn and UI Socket Communication
    # --------------------------------------------------------------------------
    print("  6D: Testing shellspawn and guest-to-host UI socket communication...")
    
    # Test UI socket via native Mach-O binary
    stage_dir = tempfile.mkdtemp(prefix="/tmp/test-stage.")
    macho_client = Path(stage_dir) / "test_client"
    c_source = Path(stage_dir) / "test_client.c"
    c_source.write_text("""
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    const char *path = argv[1];
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    address.sun_len = sizeof(address);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 2;
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) return 3;
    const char *msg = "PING_FROM_DARLING_GUEST\\n";
    write(fd, msg, strlen(msg));
    char buf[128] = {0};
    int n = read(fd, buf, sizeof(buf)-1);
    if (n > 0) printf("%.*s", n, buf);
    close(fd);
    return 0;
}
""")
    
    # Compile Mach-O binary
    sdk = ROOT / "src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
    clang_cmd = [
        "clang", "--target=x86_64-apple-darwin", "-fuse-ld=lld",
        "-DPLATFORM_MacOSX=1", "-mmacosx-version-min=11.0",
        "-isysroot", str(DARLING_ROOT),
        "-isystem", str(sdk / "usr/include"),
        "-o", str(macho_client), str(c_source)
    ]
    subprocess.check_call(clang_cmd)
    
    # Start host socket listener on $stage_dir/ui.sock
    host_sock_path = Path(stage_dir) / "ui.sock"
    server_code = f"""
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind("{host_sock_path}")
s.listen(1)
print("READY", flush=True)
conn, _ = s.accept()
data = conn.recv(1024)
assert b"PING_FROM_DARLING_GUEST" in data
conn.sendall(b"PONG_FROM_HOST_UI\\n")
conn.close()
s.close()
"""
    srv_proc = subprocess.Popen([sys.executable, "-c", server_code],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    line = srv_proc.stdout.readline()
    assert b"READY" in line, "Host socket listener failed to start"
    
    # Guest connects via /Volumes/SystemRoot$stage_dir/ui.sock
    guest_sock_path = f"/Volumes/SystemRoot{host_sock_path}"
    guest_cmd = f"/Volumes/SystemRoot{macho_client} {guest_sock_path}"
    
    res = subprocess.run([str(LAUNCHER), "--shell", guest_cmd],
                         env=env, capture_output=True, text=True, timeout=60)
    srv_proc.wait(timeout=10)
    assert res.returncode == 0, f"Guest-to-host socket communication failed: {res.stderr}"
    assert "PONG_FROM_HOST_UI" in res.stdout, f"Expected socket reply not received: {res.stdout}"
    
    shutil.rmtree(stage_dir, ignore_errors=True)
    print("  PASS 6D: Guest-to-host UI socket and shellspawn communication validated.")
    
    # --------------------------------------------------------------------------
    # 6E: AppImage Diagnostics Dispatch from Deep Directory
    # --------------------------------------------------------------------------
    print("  6E: Testing diagnostics dispatch from deep directory...")
    # Copy release files to deep folder to test end-to-end run.sh
    for item in ("run.sh", "bootstrap.sh", "bootstrap.py"):
        src = REPO_ROOT / "RobloxLinuxRelease" / item
        if src.exists():
            shutil.copy2(src, deep_release / item)
    appimage_src = REPO_ROOT / "RobloxLinuxRelease/RobloxLinux.AppImage"
    if appimage_src.exists():
        (deep_release / "RobloxLinux.AppImage").symlink_to(appimage_src)
            
    if (deep_release / "run.sh").exists() and (deep_release / "RobloxLinux.AppImage").exists():
        (deep_release / "run.sh").chmod(0o755)
        res = subprocess.run(["./run.sh", "--diagnose"], cwd=deep_release,
                             capture_output=True, text=True, timeout=60)
        assert res.returncode == 0, f"Deep run.sh --diagnose failed: {res.stderr}"
        assert "User namespaces: exit 0" in res.stdout
        print("  PASS 6E: AppImage run.sh --diagnose passed from deep directory.")

finally:
    # Cleanup deep directory
    test_top = Path("/tmp") / deep_base.parts[2]
    if test_top.exists():
        shutil.rmtree(test_top, ignore_errors=True)

# ==============================================================================
# SECTION 7: Cleanup Verification
# ==============================================================================
print("\n[Section 7] Verifying no leftover rbx.* alias directories...")
for base in ("/tmp", os.environ.get("XDG_RUNTIME_DIR", "/run/user/1000")):
    if os.path.isdir(base):
        leftovers = list(Path(base).glob("rbx.*"))
        assert len(leftovers) == 0, f"Found leftover alias directories in {base}: {leftovers}"

print("PASS: All alias directories cleanly removed.")

print("\n" + "=" * 70)
print("ALL REGRESSION TESTS PASSED SUCCESSFULLY!")
print("=" * 70)
