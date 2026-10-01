#!/usr/bin/env python3
"""Build generic native Darling libmalloc and its headless check, without installing."""
from pathlib import Path
import argparse,concurrent.futures,hashlib,json,os,re,shutil,struct,subprocess

scripts=Path(__file__).resolve().parent
root=scripts.parents[1];a=root/'runtime';src=a/'src/darling'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('level',nargs='?',choices=('O2','O3'),default='O2')
parser.add_argument('--output-dir',type=Path,default=root/'native/build/native-malloc')
parser.add_argument('--verify-source',action='store_true',help='verify the source manifest and exit without writing')
args=parser.parse_args();out=args.output_dir.resolve();level=args.level
if root not in out.parents:parser.error('--output-dir must be inside the project')
source=src/'src/external/libmalloc'
manifest={str(p.relative_to(source)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(source.rglob('*')) if p.is_file()}
pin=scripts/'source-sha256.json'
if json.loads(pin.read_text())!=manifest:raise SystemExit('Darling libmalloc source does not match source-sha256.json')
print(f'Verified {len(manifest)} pinned Darling libmalloc source files',flush=True)
if args.verify_source:raise SystemExit(0)
env=os.environ.copy();env['TMPDIR']=str(out/'tmp')
(out/'tmp').mkdir(parents=True,exist_ok=True)
include=out/'include/System';include.mkdir(parents=True,exist_ok=True)
for name in ('i386','arm','machine','kern'):
    p=include/name
    if not p.exists():p.symlink_to(src/'src/external/xnu/osfmk'/name,target_is_directory=True)
sdk=src/'Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
build=out/level;build.mkdir(exist_ok=True)
# Keep Mach-O compilation generic: Clang's znver4 tuning has miscompiled TLV calls.
# Do not forward TRACKB_CPU_TUNE, CFLAGS, or the Linux runtime's tuning settings.
flags=['clang','--target=x86_64-apple-macos11','-'+level,'-fPIC','-fblocks','-DDARLING','-DPRIVATE',
       '-DOS_UNFAIR_LOCK_INLINE=1','-D__DARWIN_UNIX03','-D__DARWIN_ONLY_UNIX_CONFORMANCE=1',
       '-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE','-DDARWIN','-DTARGET_OS_MAC=1','-DPLATFORM_MacOSX',
       '-Wno-deprecated-declarations','-Wno-nullability-completeness',
       '-isysroot',str(a/'darling-root'),'-nostdlibinc','-isystem',str(sdk/'usr/include'),
       '-I',str(out/'include'),'-I',str(source/'include/malloc'),'-I',str(source/'private'),
       '-I',str(source/'include'),'-I',str(source/'resolver')]
names=re.search(r'set\(libmalloc_sources\s+(.*?)\n\)',(source/'CMakeLists.txt').read_text(),re.S).group(1).split()
commands=[]
def run(command,log):
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,env=env,timeout=60)
    text='\n'.join(line for line in result.stdout.splitlines() if not re.search(r'ticket|joinscript|body:|status code:|cookie|token|authorization',line,re.I))+'\n'
    log.write_text(text)
    if result.returncode:print(text,flush=True);raise RuntimeError(f'{log.name}: exit {result.returncode}')
def compile(name):
    dest=build/(Path(name).name+'.o')
    command=flags+(['-DOS_VARIANT_NOTRESOLVED=1','-DOS_VARIANT_RESOLVED=1'] if name=='src/nanov2_malloc.c' else [])+['-MMD','-MF',str(dest)+'.d','-c',str(source/name),'-o',str(dest)]
    commands.append(command);run(command,build/(Path(name).name+'.log'))
    return str(dest)
try:
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:objects=list(pool.map(compile,names))
    # Link against the Debian runtime this allocator was pinned to, which the
    # -O2 Darling install keeps under optimized/darling/stock.
    stock=a/'optimized/darling/stock/darling-root/usr/lib/system'
    system=stock if stock.exists() else a/'darling-root/usr/lib/system'
    command=['clang','--target=x86_64-apple-macos11','-fuse-ld=lld','-nostdlib','-dynamiclib',
             '-Wl,--threads=4,-no_adhoc_codesign,-no_fixup_chains,-install_name,/usr/lib/system/libsystem_malloc.dylib,-compatibility_version,1.0.0,-current_version,0.0.0',
             *objects,*[str(system/name) for name in ('libsystem_kernel.dylib','libsystem_platform.dylib','libdyld.dylib','libcompiler_rt.dylib')],
             str(system/'libsystem_c.dylib'),'-o',str(build/'libsystem_malloc.dylib')]
    commands.append(command);run(command,build/'link.log')
    # LLD ignores -upward_library. This load command has the identical layout;
    # restore the packaged library's upward edge before any execution.
    library=build/'libsystem_malloc.dylib';data=bytearray(library.read_bytes())
    assert struct.unpack_from('<I',data)[0]==0xfeedfacf
    position=32;changed=0
    for _ in range(struct.unpack_from('<I',data,16)[0]):
        command,size=struct.unpack_from('<II',data,position)
        assert size>=8 and position+size<=len(data)
        if command==0xc:
            start=position+struct.unpack_from('<I',data,position+8)[0]
            name=data[start:data.index(0,start)].decode()
            if name=='/usr/lib/system/libsystem_c.dylib':
                struct.pack_into('<I',data,position,0x80000023);changed+=1
        position+=size
    assert changed==1, 'expected exactly one libc upward dependency'
    library.write_bytes(data)
    normalizer=build/'sort-weak-bindings.py'
    shutil.copy2(a/'shims/metal/sort-weak-bindings.py',normalizer)
    run(['python3',str(normalizer),str(library)],build/'weak-bind.log')
    # Keep real malloc calls in intentional failure/errno probes; Clang otherwise
    # folds them away. Both allocator levels use this identical O2 check binary.
    command=['clang','--target=x86_64-apple-macos11','-O2','-fno-builtin','-fblocks',
             '-D__DARWIN_ONLY_UNIX_CONFORMANCE=1','-fuse-ld=lld','-Wl,--threads=4,-no_adhoc_codesign',
             '-Wno-nullability-completeness','-isysroot',str(a/'darling-root'),'-nostdlibinc',
             '-isystem',str(sdk/'usr/include'),'-F',str(sdk/'System/Library/Frameworks'),
             '-framework','Foundation',str(scripts/'check.m'),'-o',str(out/'check')]
    commands.append(command);run(command,out/'build-check.log')
finally:(build/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
print(f'Built {build}/libsystem_malloc.dylib from {len(names)} unchanged C sources on CPUs {sorted(os.sched_getaffinity(0))}')
