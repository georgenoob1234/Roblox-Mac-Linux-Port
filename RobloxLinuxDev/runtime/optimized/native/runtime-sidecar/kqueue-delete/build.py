#!/usr/bin/env python3
"""Build diagnostic libkqueue overlays; use build-system.py for lifecycle-safe containers."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import difflib

here=Path(__file__).resolve().parent
root=here.parents[3]
src=root/'src/darling'
kqueue=src/'src/external/libkqueue'
sdk=src/'Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
kernel=src/'src/external/xnu/darling/src/libsystem_kernel'
out=here/'build'
for name in ('logs','tmp','objects','control','close-race-control','candidate'):(out/name).mkdir(parents=True,exist_ok=True)
overlay=out/'source'
if overlay.exists():shutil.rmtree(overlay)
shutil.copytree(kqueue,overlay,dirs_exist_ok=True,symlinks=True)
original=kqueue/'src/common/kevent.c'
text=original.read_text()
needle='    if ((src->flags & EV_DELETE) || ((kn->kn_flags & KNFL_DEFER_DELETE) && (src->flags & EV_ENABLE))) {\n'
assert text.count(needle)==1
addition='''        if ((src->flags & EV_DELETE) && !(src->flags & EV_ENABLE) &&
            (kn->kev.flags & EV_DISPATCH2) == EV_DISPATCH2 && (kn->kev.flags & EV_DISABLE)) {
            kn->kn_flags |= KNFL_DEFER_DELETE;
            errno = EINPROGRESS;
            return (-1);
        }
'''
patched=text.replace(needle,needle+addition)
(overlay/'src/common/kevent.c').write_text(patched)
(here/'deferred-delete.patch').write_text(''.join(difflib.unified_diff(text.splitlines(True),patched.splitlines(True),
    fromfile='a/src/common/kevent.c',tofile='b/src/common/kevent.c')))
original_kqueue=(kqueue/'src/common/kqueue.c').read_text()
closed_fd=original_kqueue.replace('ev[0].flags = EV_DELETE | EV_RECEIPT;', 'ev[0].flags = EV_DELETE | EV_ENABLE | EV_RECEIPT;').replace(
    'ev[1].flags = EV_DELETE | EV_RECEIPT;', 'ev[1].flags = EV_DELETE | EV_ENABLE | EV_RECEIPT;')
assert closed_fd.count('EV_DELETE | EV_ENABLE | EV_RECEIPT')==2
race_control_source=closed_fd
# sys_close_nocancel calls kqueue_close BEFORE Linux close, then
# kqueue_closed_fd AFTER it. Retire watches in the first hook while this
# descriptor still belongs to its old owner; the second hook is too late.
closed_fd=closed_fd.replace('int VISIBLE\nkqueue_close(int kqfd)',
    'static void kqueue_prepare_close(int fd);\n\nint VISIBLE\nkqueue_close(int kqfd)')
needle='    pthread_mutex_unlock(&kq_mtx);\n\n\treturn result;'
assert closed_fd.count(needle)==1
closed_fd=closed_fd.replace(needle,
    '    pthread_mutex_unlock(&kq_mtx);\n\n\tif (!result) kqueue_prepare_close(kqfd);\n\treturn result;')
closed_fd=closed_fd.replace('void VISIBLE\nkqueue_closed_fd(int fd)\n{',
    'void VISIBLE\nkqueue_closed_fd(int fd)\n{\n'
    '\t// The fd may already belong to another thread. Cleanup ran before close.\n'
    '\t(void) fd;\n}\n\nstatic void\nkqueue_prepare_close(int fd)\n{')
(overlay/'src/common/kqueue.c').write_text(closed_fd)
with (here/'deferred-delete.patch').open('a') as patch:
    patch.write(''.join(difflib.unified_diff(original_kqueue.splitlines(True),closed_fd.splitlines(True),
        fromfile='a/src/common/kqueue.c',tofile='b/src/common/kqueue.c')))
# Exact non-Windows/DARLING source list in libkqueue/CMakeLists.txt.
names=['src/posix/platform.c']+['src/linux/'+n+'.c' for n in
    ('platform','signal','socket','timer','user','vnode','write','read','proc','machport','fs')]+[
    'src/common/'+n+'.c' for n in ('filter','knote','map','kevent','kqueue')]+['src/darling/listenregistry.c']
# CMake GLOB silently excludes listed paths absent in this source revision.
names=[name for name in names if (kqueue/name).is_file()]
flags=['clang','--target=x86_64-apple-macos11','-O0','-g','-std=c99','-fPIC','-fvisibility=hidden',
       '-DDARLING=1','-D__linux__','-D_XOPEN_SOURCE=600','-D_DARWIN_C_SOURCE','-DHAVE_SYS_EVENTFD_H','-DHAVE_SYS_SIGNALFD_H',
       '-DHAVE_SYS_TIMERFD_H','-D__DARWIN_ONLY_UNIX_CONFORMANCE=1','-DTARGET_OS_WASI=0',
       '-isysroot',str(root/'darling-root'),'-nostdlibinc','-isystem',str(sdk/'usr/include'),
       '-I'+str(overlay/'src/common'),'-I'+str(kernel/'emulation/include/linux_premigration/ext'),
       '-I'+str(src/'src/external/darlingserver/include'),'-Wno-undef-prefix',
       '-Werror=implicit-function-declaration','-Werror=int-conversion']
env=os.environ.copy();env['TMPDIR']=str(out/'tmp')
def run(command,name):
    command=list(map(str,command));(out/'logs'/(name+'.command.json')).write_text(json.dumps(command,indent=2))
    result=subprocess.run(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (out/'logs'/(name+'.log')).write_text(result.stdout)
    if result.returncode:raise RuntimeError('Failed '+name+'; see '+str(out/'logs'/(name+'.log')))
objects=[]
for name in names:
    source=overlay/name
    obj=out/'objects'/(name.replace('/','_')+'.o')
    run(flags+['-c',source,'-o',obj],name.replace('/','_'))
    objects.append(obj)
# Original kevent control compiles in the same copied include layout.
(overlay/'src/common/kevent-control.c').write_text(text)
control_obj=out/'control/kevent.o'
run(flags+['-c',overlay/'src/common/kevent-control.c','-o',control_obj],'kevent-control')
(overlay/'src/common/kqueue-control.c').write_text(original_kqueue)
control_queue=out/'control/kqueue.o'
run(flags+['-c',overlay/'src/common/kqueue-control.c','-o',control_queue],'kqueue-control')
(overlay/'src/common/kqueue-race-control.c').write_text(race_control_source)
race_control_queue=out/'close-race-control/kqueue.o'
run(flags+['-c',overlay/'src/common/kqueue-race-control.c','-o',race_control_queue],'kqueue-race-control')
for mode in ('control','candidate'):
    controls={'src/common/kevent.c':control_obj,'src/common/kqueue.c':control_queue}
    selected=[controls.get(n,o) if mode=='control' else o for n,o in zip(names,objects)]
    # This lld emits strong weak-binding overrides in symbol insertion order.
    # Darling dyld merges these streams lexicographically; seed their order.
    hooks=sorted(['___darling_kqueue_register_listen','_kevent64_impl','_kevent_impl',
                  '_kqueue_close','_kqueue_closed_fd','_kqueue_dup','_kqueue_impl'])
    ordering=['-Wl,-u,'+name for name in hooks]
    run(['clang','--target=x86_64-apple-macos11','-fuse-ld=lld','-dynamiclib','-isysroot',root/'darling-root',
         '-Wl,--threads=2,-no_adhoc_codesign','-install_name','@rpath/libtracka-kqueue-delete.dylib',
         *ordering,*selected,'-o',out/mode/'libtracka-kqueue-delete.dylib'],mode+'-link')
(out/'provenance.json').write_text(json.dumps({'source_count':len(names),'flags':flags,
    'sources':{str(kqueue/n):hashlib.sha256((kqueue/n).read_bytes()).hexdigest() for n in names},
    'artifacts':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in
        (original,overlay/'src/common/kevent.c',out/'control/libtracka-kqueue-delete.dylib',out/'candidate/libtracka-kqueue-delete.dylib')}
},indent=2)+'\n')
print('PASS isolated complete libkqueue control/candidate build,',len(names),'sources')
