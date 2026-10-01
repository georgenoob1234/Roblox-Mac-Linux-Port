#!/usr/bin/env python3
"""Compile two private helpers into verified stock function slots; retain all ABI metadata."""
from pathlib import Path
import hashlib,json,runpy,subprocess
here=Path(__file__).resolve().parent
root=here.parents[3]
out=here/'build';out.mkdir(exist_ok=True)
stock=root/'optimized/darling/stock/darling-root/usr/lib/system/libsystem_kernel.dylib'
if not stock.is_file():stock=root/'darling-root/usr/lib/system/libsystem_kernel.dylib'
original=out/'original.dylib'
expected='7c4fb3bdf9093cca7e7e6d6533bf8f3595133b30480740fd29d47f199224f5c6'
assert hashlib.sha256(stock.read_bytes()).hexdigest()==expected, "Unsupported stock kernel; rebase and revalidate the helper slots"
if not original.exists():
    assert hashlib.sha256(stock.read_bytes()).hexdigest()==expected
    original.write_bytes(stock.read_bytes())
data=bytearray(original.read_bytes())
assert hashlib.sha256(data).hexdigest()==expected
base,segments,syms,_=runpy.run_path(str(here.parent/'symbolize.py'))['macho'](original)
addresses={name:address for address,name,_ in syms}
assert addresses['_sem_up']==0x57e70 and addresses['_sem_down']==0x57fd0
assert addresses['___linux_futex_reterr']==0x4bca0
commands=[['clang','-c',str(here/'semaphore.S'),'-o',str(out/'semaphore.o')],
 ['ld','--section-start=.up=0x57e70','--section-start=.down=0x57fd0',
  '--defsym=__linux_futex_reterr=0x4bca0','-T',str(out/'slots.ld'),str(out/'semaphore.o'),'-o',str(out/'semaphore.elf')]]
(out/'slots.ld').write_text('SECTIONS { .up : { *(.text.sem_up) } .down : { *(.text.sem_down) } /DISCARD/ : { *(.comment) *(.note*) } }\n')
for command in commands:subprocess.run(command,check=True)
ranges=[]
for section,start,end in (('up',0x57e70,0x57ed0),('down',0x57fd0,0x58090)):
    binary=out/(section+'.bin')
    subprocess.run(['objcopy','-O','binary','--only-section=.'+section,str(out/'semaphore.elf'),str(binary)],check=True)
    code=binary.read_bytes();assert 0<len(code)<=end-start
    offset=next(base+off+start-vm for vm,off,size in segments if vm<=start<vm+size)
    data[offset:offset+end-start]=code+b'\x90'*(end-start-len(code))
    ranges.append((offset,offset+end-start))
candidate=out/'libsystem_kernel.dylib'
temporary=candidate.with_suffix('.next');temporary.write_bytes(data);temporary.replace(candidate)
old=original.read_bytes()
assert len(old)==len(data)
assert all(any(lo<=i<hi for lo,hi in ranges) for i,(a,b) in enumerate(zip(old,data)) if a!=b)
(out/'provenance.json').write_text(json.dumps({'original_sha256':expected,'candidate_sha256':hashlib.sha256(data).hexdigest(),
 'source_sha256':hashlib.sha256((here/'semaphore.S').read_bytes()).hexdigest(),'commands':commands,'modified_file_ranges':ranges},indent=2)+'\n')
print('PASS only two private semaphore bodies changed; headers, exports, dependencies and other code identical')
