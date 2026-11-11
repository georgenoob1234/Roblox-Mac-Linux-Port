#!/usr/bin/env python3
"""Run probes/sockopt.c in the guest the way the game is loaded: flat namespace,
the native system libraries and librbxkqueue inserted. The runtime must accept
IP_DONTFRAG / IPV6_DONTFRAG; the Roblox client gives up on a game connection
when IP_DONTFRAG is rejected.

The probe runs in a private network namespace whose loopback has a 1500-byte
MTU and IPv6 enabled, so its over-MTU sends never leave the machine and do not
depend on the host's network configuration."""
import argparse,os,pathlib,shlex,shutil,subprocess,tempfile
here=pathlib.Path(__file__).resolve().parent;root=here.parent
native=root/'optimized/native/lib'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library',type=pathlib.Path,default=native/'librbxkqueue.dylib')
library=parser.parse_args().library.resolve(strict=True)
sdk=root/'src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
work=pathlib.Path(tempfile.mkdtemp(prefix='rbx-sockopt-',dir='/tmp'))
probe=work/'sockopt-check'
subprocess.run(['clang','--target=x86_64-apple-macos11','-DPLATFORM_MacOSX','-O2','-Wall','-Wextra','-fuse-ld=lld',
                '-isysroot',root/'darling-root','-isystem',sdk/'usr/include',here/'sockopt.c','-o',probe],
               check=True,env=dict(os.environ,TMPDIR=str(work)))
env=dict(os.environ,ROBLOX_MAC_CONFIG='/dev/null',ROBLOX_MAC_DATA=str(work/'data'),ROBLOX_MAC_WAYLAND='0')
for name in ('DISPLAY','WAYLAND_DISPLAY','DYLD_INSERT_LIBRARIES','DYLD_LIBRARY_PATH'):env.pop(name,None)
# IPv6 may be switched off on the host; inside the namespace that is ours to change.
network='ip link set lo mtu 1500 up && { echo 0 > /proc/sys/net/ipv6/conf/lo/disable_ipv6 || true; } 2>/dev/null'
private=subprocess.run(['unshare','-Urn','sh','-c',network],capture_output=True).returncode==0
guest='/Volumes/SystemRoot'
command=shlex.join(['env','DYLD_FORCE_FLAT_NAMESPACE=1','DYLD_LIBRARY_PATH='+guest+str(native),
                    'DYLD_INSERT_LIBRARIES='+guest+str(library),guest+str(probe)]+(['4000'] if private else []))+'; echo exit=$?'
launcher=['timeout','--kill-after=5s','60s',str(root/'bin/roblox-mac'),'--shell',command]
if private:launcher=['unshare','-Urn','sh','-c',network+' && exec "$@"','sockopt-check']+launcher
result=subprocess.run(launcher,env=env,capture_output=True,text=True)
text=result.stdout+result.stderr
lines=[line for line in text.splitlines() if line.startswith(('PASS','FAIL','SKIP','exit='))]
if not private:lines.insert(0,'SKIP sends: no private network namespace (unshare -Urn with iproute2)')
assert 'exit=0' in lines and any(line.startswith('PASS sockopt') for line in lines),'\n'.join(lines) or text[-800:]
shutil.rmtree(work,ignore_errors=True)
print('\n'.join(line for line in lines if not line.startswith('exit=')))
