#!/usr/bin/env python3
"""Build the existing Darling Objective-C runtime with release diagnostics."""
from pathlib import Path
import argparse,concurrent.futures,os,re,subprocess
root=Path(__file__).resolve().parents[1];a=root.parent/'runtime';src=a/'src/darling';objc=src/'src/external/objc4/runtime'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir',type=Path,default=root/'build/objc')
args=parser.parse_args();out=args.output_dir.resolve()
if root.parent not in out.parents:parser.error('--output-dir must be inside the project')
out.mkdir(parents=True,exist_ok=True)
(out/'include').mkdir(exist_ok=True)
system=out/'include/System'
if system.is_symlink():system.unlink()
system.mkdir(exist_ok=True)
for name in ('machine','i386','arm'):
    if not (system/name).exists():(system/name).symlink_to(src/'src/external/xnu/osfmk'/name,target_is_directory=True)
if not (system/'pthread_machdep.h').exists():(system/'pthread_machdep.h').symlink_to(src/'src/external/libpthread/private/pthread/private.h')
env=os.environ.copy();env['TMPDIR']=str(out/'tmp')
(out/'tmp').mkdir(exist_ok=True)
sdk=src/'Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
flags=['clang++','--target=x86_64-apple-macos11','-O2','-D__DARWIN_ONLY_UNIX_CONFORMANCE=1','-DDARLING','-DNDEBUG=1','-DOBJC_IS_DEBUG_BUILD=0','-DOBJC_NO_GC','-D__STDC_NO_ATOMICS__','-D__DARWIN_UNIX03','-DTARGET_OS_IPHONE=0','-DOS_OBJECT_USE_OBJC=0','-D_LIBCPP_VISIBLE=','-fPIC','-fblocks','-fobjc-legacy-dispatch','-fvisibility-inlines-hidden','-fstrict-aliasing','-fno-delete-null-pointer-checks','-fno-rtti','-fno-exceptions','-Wno-c++11-narrowing','-Wno-invalid-offsetof','-Wno-deprecated-objc-isa-usage','-Wno-cast-of-sel-type','-Wno-unused-command-line-argument','-isysroot',str(a/'darling-root'),'-nostdinc++','-isystem',str(src/'src/external/libcxx/include'),'-isystem',str(sdk/'usr/include'),'-I',str(objc),'-I',str(out/'include')]
names=re.search(r'set\(objc_SRCS\s+(.*?)\n\)',(objc/'CMakeLists.txt').read_text(),re.S).group(1).split()
def compile(name):
    dest=out/(name.replace('/','_')+'.o')
    result=subprocess.run(flags+(['-std=gnu++14','-include','utility'] if name.endswith('.mm') else [])+['-c',str(objc/name),'-o',str(dest)],capture_output=True,text=True,env=env)
    if result.returncode:
        print(name+':\n'+result.stderr,flush=True);result.check_returncode()
    return str(dest)
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as workers:objects=list(workers.map(compile,names))
subprocess.run(['clang++','--target=x86_64-apple-macos11','-fuse-ld=lld','-dynamiclib','-isysroot',str(a/'darling-root'),'-Wl,-no_adhoc_codesign,-compatibility_version,1.0.0,-current_version,228.0.0','-Wl,-install_name,/usr/lib/libobjc.A.dylib','-Wl,-sectalign,__DATA,__objc_data,0x1000','-Wl,-rename_section,__DATA,__mod_init_func,__DATA,__objc_init_func','-Wl,-unexported_symbols_list,'+str(objc.parent/'unexported_symbols'),'-lc++','-lc++abi',*objects,'-o',str(out/'libobjc.A.new.dylib')],check=True,env=env)
# Darling's old dyld requires lexicographically sorted weak-bind opcodes.
import shutil
shutil.copy2(a/'shims/metal/sort-weak-bindings.py',out/'sort-weak-bindings.py')
subprocess.run(['python3',str(out/'sort-weak-bindings.py'),str(out/'libobjc.A.new.dylib')],check=True)
(out/'libobjc.A.new.dylib').replace(out/'libobjc.A.dylib')
