#!/usr/bin/env python3
"""Verify the fatal report with a tiny crashing probe, never RobloxPlayer."""
import os,pathlib,resource,shlex,subprocess,tempfile
here=pathlib.Path(__file__).resolve().parent;root=here.parent
out=pathlib.Path(os.environ.get('OUT',str(here))).resolve()
state=pathlib.Path(tempfile.mkdtemp(prefix='c-',dir=here));report=state/'fatal.log'
guest='/Volumes/SystemRoot'
command=shlex.join(['env','ROBLOX_MAC_CRASH_LOG='+str(report),guest+str(out/'crash-check')])
env=dict(os.environ,ROBLOX_MAC_CONFIG='/dev/null',ROBLOX_MAC_DATA=str(state/'data'),ROBLOX_MAC_WAYLAND='0')
for name in ('DISPLAY','WAYLAND_DISPLAY','DYLD_INSERT_LIBRARIES'):env.pop(name,None)
def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
with (state/'probe.log').open('w') as log:
    result=subprocess.run(['timeout','--kill-after=5s','30s',str(root/'bin/roblox-mac'),'--shell',command],env=env,
                          preexec_fn=no_core,stdout=log,stderr=subprocess.STDOUT)
text=report.read_text() if report.exists() else ''
# Darling shellspawn reports 1 when its process namespace dies from a fatal signal.
assert result.returncode in (1,139,-11),(result.returncode,state/'probe.log')
assert 'signal=0xb\nfault=0x1\nrip=0x' in text,text[:500]
assert 'return=0x' in text and 'MAPS\n' in text and 'crash-check' in text and text.endswith('END FAULT\n')
assert report.stat().st_mode&0o777==0o600
print('PASS fatal report: original SIGSEGV/address, frame addresses, module maps, private file, normal fatal termination')
print('Crash probe artifacts:',state)
