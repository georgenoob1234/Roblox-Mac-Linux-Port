#!/usr/bin/env python3
"""Run native/clock-check.c in the guest the way the game is loaded: flat
namespace, the native system libraries, libtracka-clock and librbxkqueue
inserted. The clock library answers the Darwin clocks, gettimeofday and time()
from the vDSO; the probe checks that they agree with the kernel's answers,
never run backwards and leave the other clock IDs alone."""
import argparse,os,pathlib,shlex,shutil,subprocess,tempfile
here=pathlib.Path(__file__).resolve().parent;root=here.parent
native=root/'optimized/native/lib'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library',type=pathlib.Path,default=native/'libtracka-clock.dylib')
library=parser.parse_args().library.resolve(strict=True)
sdk=root/'src/darling/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
work=pathlib.Path(tempfile.mkdtemp(prefix='rbx-clock-',dir='/tmp'))
probe=work/'clock-check'
subprocess.run(['clang','--target=x86_64-apple-macos11','-DPLATFORM_MacOSX','-O2','-fuse-ld=lld',
                '-isysroot',root/'darling-root','-isystem',sdk/'usr/include',root.parent/'native/clock-check.c','-o',probe],
               check=True,env=dict(os.environ,TMPDIR=str(work)))
env=dict(os.environ,ROBLOX_MAC_CONFIG='/dev/null',ROBLOX_MAC_DATA=str(work/'data'),ROBLOX_MAC_WAYLAND='0')
for name in ('DISPLAY','WAYLAND_DISPLAY','DYLD_INSERT_LIBRARIES','DYLD_LIBRARY_PATH'):env.pop(name,None)
guest='/Volumes/SystemRoot'
command=shlex.join(['env','DYLD_FORCE_FLAT_NAMESPACE=1','DYLD_LIBRARY_PATH='+guest+str(native),
                    'DYLD_INSERT_LIBRARIES='+guest+str(library)+':'+guest+str(native/'librbxkqueue.dylib'),
                    guest+str(probe)])+'; echo exit=$?'
result=subprocess.run(['timeout','--kill-after=5s','120s',str(root/'bin/roblox-mac'),'--shell',command],
                      env=env,capture_output=True,text=True)
text=result.stdout+result.stderr
lines=[line for line in text.splitlines() if line.startswith(('PASS','FAIL','exit='))]
assert 'exit=0' in lines and any(line.startswith('PASS') for line in lines),'\n'.join(lines) or text[-800:]
shutil.rmtree(work,ignore_errors=True)
print('\n'.join(line for line in lines if not line.startswith('exit=')))
