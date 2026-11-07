#!/usr/bin/env python3
"""Run a probe under the injected compat shim: only the background installer may be skipped."""
import os,pathlib,shlex,shutil,subprocess,sys,tempfile
here=pathlib.Path(__file__).resolve().parent;root=here.parent
out=pathlib.Path(os.environ.get('OUT',str(here))).resolve()
probe=pathlib.Path(sys.argv[1]).resolve()
guest='/Volumes/SystemRoot'
work=pathlib.Path(tempfile.mkdtemp(prefix='rbx-installer-',dir='/tmp'))
def stage(rel):
    path=work/rel;path.parent.mkdir(parents=True,exist_ok=True);shutil.copy(probe,path);return path
cases=[ # (relative path, args, should the shim let it run?)
    ('a/RobloxPlayerInstaller',['-background','true','-stage','prepare'],False),
    ('a/RobloxPlayerInstaller',['-install'],True),
    ('a/RobloxPlayerInstaller',['-background','false'],True),
    ('RobloxPlayerInstaller.app/Contents/MacOS/RobloxPlayer',['-background','true'],True),
    ('b/RobloxPlayer',[],True)]
for rel,_,_ in cases:stage(rel)
env=dict(os.environ,ROBLOX_MAC_CONFIG='/dev/null',ROBLOX_MAC_WAYLAND='0')
for name in ('DISPLAY','WAYLAND_DISPLAY','DYLD_INSERT_LIBRARIES'):env.pop(name,None)
shim=guest+str(out/'libtracka-compat.dylib')
for rel,args,runs in cases:
    state=pathlib.Path(tempfile.mkdtemp(prefix='rbx-installer-state-',dir='/tmp'))
    command=shlex.join(['env','DYLD_INSERT_LIBRARIES='+shim,guest+str(work/rel)]+args)+'; echo exit=$?'
    result=subprocess.run(['timeout','--kill-after=5s','40s',str(root/'bin/roblox-mac'),'--shell',command],
        env=dict(env,ROBLOX_MAC_DATA=str(state)),capture_output=True,text=True)
    text=result.stdout+result.stderr
    assert 'exit=0' in text,(rel,args,text[-500:])
    assert ('PROBE RAN' in text)==runs,(rel,args,'expected run=%s'%runs,text[-500:])
shutil.rmtree(work,ignore_errors=True)
print('PASS installer short-circuit: background installer skipped; foreground, background=false and lookalike paths run')
