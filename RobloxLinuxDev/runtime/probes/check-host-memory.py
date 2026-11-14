#!/usr/bin/env python3
"""Run probes/host-memory.c in the guest with librbxidentity inserted, the way the
game is loaded. The Roblox client sizes its memory budget from the host's memory
statistics; the figure it works out must follow Linux's MemAvailable. If it
follows MemFree instead, the client believes that a machine whose memory is
mostly file cache is almost full."""
import argparse,os,pathlib,shlex,shutil,subprocess,tempfile
here=pathlib.Path(__file__).resolve().parent;root=here.parent
native=root/'optimized/native/lib'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library',type=pathlib.Path,default=root/'shims/identity/librbxidentity.dylib')
library=parser.parse_args().library.resolve(strict=True)
sdk=root/'src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
work=pathlib.Path(tempfile.mkdtemp(prefix='rbx-host-memory-',dir='/tmp'))
probe=work/'host-memory-check'
subprocess.run(['clang','--target=x86_64-apple-macos11','-DPLATFORM_MacOSX','-O2','-Wall','-Wextra','-fuse-ld=lld',
                '-isysroot',root/'darling-root','-isystem',sdk/'usr/include',here/'host-memory.c','-o',probe],
               check=True,env=dict(os.environ,TMPDIR=str(work)))
env=dict(os.environ,ROBLOX_MAC_CONFIG='/dev/null',ROBLOX_MAC_DATA=str(work/'data'),ROBLOX_MAC_WAYLAND='0')
for name in ('DISPLAY','WAYLAND_DISPLAY','DYLD_INSERT_LIBRARIES','DYLD_LIBRARY_PATH'):env.pop(name,None)
guest='/Volumes/SystemRoot'
command=shlex.join(['env','DYLD_FORCE_FLAT_NAMESPACE=1','DYLD_LIBRARY_PATH='+guest+str(native),
                    'DYLD_INSERT_LIBRARIES='+guest+str(library),guest+str(probe)])+'; echo exit=$?'
result=subprocess.run(['timeout','--kill-after=5s','60s',str(root/'bin/roblox-mac'),'--shell',command],
                      env=env,capture_output=True,text=True)
text=result.stdout+result.stderr
lines=[line for line in text.splitlines() if line.startswith(('PASS','FAIL','host ','note:','exit='))]
assert 'exit=0' in lines and any(line.startswith('PASS host memory') for line in lines),'\n'.join(lines) or text[-800:]
shutil.rmtree(work,ignore_errors=True)
print('\n'.join(line for line in lines if not line.startswith('exit=')))
