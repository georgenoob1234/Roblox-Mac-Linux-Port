#!/usr/bin/env python3
"""Compare the candidate's public Mach-O interface with the packaged x86_64 slice."""
from pathlib import Path
import argparse,hashlib,json,re,struct,subprocess

root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('level',nargs='?',choices=('O2','O3'),default='O2')
parser.add_argument('--output-dir',type=Path,default=root/'native/build/native-malloc')
args=parser.parse_args();out=args.output_dir.resolve();level=args.level
if root not in out.parents:parser.error('--output-dir must be inside the project')
# The -O2 Darling install keeps the Debian originals as the ABI baseline.
stock=root/'runtime/optimized/darling/stock/darling-root'
baseline=(stock if stock.exists() else root/'runtime/darling-root')/'usr/lib/system/libsystem_malloc.dylib'
candidate=out/level/'libsystem_malloc.dylib'
def inspect(path):
    raw=path.read_bytes();data=raw
    if data[:4]==b'\xca\xfe\xba\xbe':
        for i in range(struct.unpack_from('>I',data,4)[0]):
            cpu,subtype,offset,size,alignment=struct.unpack_from('>IIIII',data,8+i*20)
            if cpu==0x1000007:data=raw[offset:offset+size];break
    assert struct.unpack_from('<II',data)==(0xfeedfacf,0x1000007)
    libraries={};position=32
    for _ in range(struct.unpack_from('<I',data,16)[0]):
        command,size=struct.unpack_from('<II',data,position)
        assert size>=8 and position+size<=len(data)
        if command in (0xc,0xd,0x80000018,0x8000001f,0x80000023):
            start=position+struct.unpack_from('<I',data,position+8)[0]
            name=data[start:data.index(0,start)].decode()
            current,compatible=struct.unpack_from('<II',data,position+16)
            libraries[name]={'kind':command,'current':current,'compatible':compatible}
        position+=size
    text=subprocess.check_output(['llvm-objdump','--macho','--arch=x86_64','--exports-trie',str(path)],text=True)
    exports=sorted(re.findall(r'^0x[0-9a-fA-F]+\s+(.+)$',text,re.M))
    assert exports
    return {'sha256':hashlib.sha256(raw).hexdigest(),'x86_64_bytes':len(data),'libraries':libraries,'exports':exports}
before,after=inspect(baseline),inspect(candidate)
assert before['sha256']=='015d86c293a49247a047849dd5a9d3ff9080ef861436b0d6e838ab3406a6486e', 'packaged baseline changed'
assert before['exports']==after['exports'], 'export names/flags differ'
assert before['libraries'].keys()==after['libraries'].keys(), 'dependency set differs'
version_differences={}
for name,old in before['libraries'].items():
    new=after['libraries'][name]
    assert old['kind']==new['kind'] and old['compatible']==new['compatible'], name
    if old['current']!=new['current']:
        version_differences[name]={'baseline':old['current'],'candidate':new['current']}
# Existing final libdyld advertises 421.1; the packaged allocator records 0.
# Both declare compatibility version 1; the install ID and upward edge match.
assert version_differences in ({},{'/usr/lib/system/libdyld.dylib':{'baseline':0,'candidate':27590912}})
report={'baseline':before,'candidate':after,'dependency_current_version_differences':version_differences,'result':'PASS'}
(out/f'abi-{level}.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS x86_64 Mach-O: {len(after["exports"])} matching exports/flags, install ID, dependency set/kinds/compatibility; bytes={after["x86_64_bytes"]}')
print(f'Dependency current-version differences: {version_differences}')
