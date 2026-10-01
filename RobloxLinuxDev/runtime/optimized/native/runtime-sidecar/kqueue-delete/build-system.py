#!/usr/bin/env python3
"""Rebuild the small libSystem container to retain direct lifecycle calls."""
import runpy
import subprocess
import re
import hashlib
import json
from pathlib import Path

here=Path(__file__).resolve().parent
build=runpy.run_path(str(here/'build.py'))
globals().update({k:build[k] for k in ('root','src','sdk','out','flags','run','objects','names','control_obj','control_queue','race_control_queue','ordering')})
system=src/'src/external/libsystem'
extra=['-std=gnu99','-fvisibility=default','-DPRIVATE=1','-DHAVE_STDINT_H=1','-DHAVE_SYSTEM_CORESERVICES',
       '-F'+str(sdk/'System/Library/Frameworks'),
       '-I'+str(src/'src/external/libc/darwin'),
       '-I'+str(src/'src/external/libmalloc/private'),
       '-I'+str(src/'src/external/xnu/libsyscall/wrappers')]
system_objects=[]
for name in ('init.c','CompatibilityHacks.c','darling/src/dummy.c'):
    obj=out/'objects'/('system_'+name.replace('/','_')+'.o')
    run(flags+extra+['-c',system/name,'-o',obj],'system_'+name.replace('/','_'))
    system_objects.append(obj)
stock=root/'darling-root/usr/lib/libSystem.B.dylib'
deps=subprocess.check_output(['llvm-objdump','--macho','--dylibs-used',str(stock)],text=True)
reexports=[line.strip().split(' ')[0] for line in deps.splitlines() if line.endswith('reexport)')]
assert len(reexports)==33
for mode in ('control','close-race-control','candidate'):
    controls=({'src/common/kevent.c':control_obj,'src/common/kqueue.c':control_queue} if mode=='control' else
              {'src/common/kqueue.c':race_control_queue} if mode=='close-race-control' else {})
    selected=[controls.get(n,o) for n,o in zip(names,objects)]
    run(['clang','--target=x86_64-apple-macos11','-fuse-ld=lld','-dynamiclib','-nostdlib',
         '-isysroot',root/'darling-root','-Wl,--threads=2,-no_adhoc_codesign,-bind_at_load',
         '-Wl,-install_name,/usr/lib/libSystem.B.dylib,-current_version,1281.0.0,-compatibility_version,1.0.0',
         *ordering,*selected,*system_objects,
         *['-Wl,-reexport_library,'+str(root/'darling-root'/p.lstrip('/')) for p in reexports],
         '-o',out/mode/'libSystem.B.dylib'],mode+'-system-link')
def exports(path):
    return sorted(line.split()[-1] for line in subprocess.check_output(['llvm-nm','-gU',str(path)],text=True).splitlines() if line.split())
reference=exports(stock)
for mode in ('control','close-race-control','candidate'):
    candidate=out/mode/'libSystem.B.dylib'
    assert exports(candidate)==reference, 'Defined export mismatch: '+mode
    actual=subprocess.check_output(['llvm-objdump','--macho','--dylibs-used',str(candidate)],text=True).splitlines()[1:]
    assert actual==deps.splitlines()[1:], 'Install identity/dependency mismatch: '+mode
    weak=subprocess.check_output(['llvm-objdump','--macho','--weak-bind',str(candidate)],text=True)
    overrides=[line.split()[-1] for line in weak.splitlines() if 'strong' in line]
    assert overrides==sorted(overrides), 'Weak-binding overrides must be sorted'
(out/'system-provenance.json').write_text(json.dumps({
    'container_sources':{str(system/n):hashlib.sha256((system/n).read_bytes()).hexdigest() for n in ('init.c','CompatibilityHacks.c','darling/src/dummy.c')},
    'container_flags':flags+extra,'reexports':reexports,'defined_exports':reference,
    'artifacts':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in
        (stock,out/'control/libSystem.B.dylib',out/'close-race-control/libSystem.B.dylib',out/'candidate/libSystem.B.dylib')}
},indent=2)+'\n')
print('PASS source-built libSystem controls with original initializers and 33 reexports')
