#!/usr/bin/env python3
"""Adapt existing metal2vulkan outputs to the rebuilt Indium descriptor ABI."""
import argparse, concurrent.futures, hashlib, json, os, pathlib, re, struct, subprocess
ROOT = pathlib.Path(__file__).resolve().parents[1]
PACK = ROOT.parent / 'RobloxPlayer.app/Contents/Resources/shaders/shaders_metal_osx.pack'
OUT = ROOT / 'data/spv-cache-v1'
SOURCE = ROOT / 'metal/out/spv'

def adapt(blob, meta):
    if len(blob) < 88 or blob[:4] != b'MTLB': raise ValueError('invalid metallib')
    size, offset, length = struct.unpack_from('<QQQ', blob, 16)
    if size != len(blob) or offset + length > size: raise ValueError('invalid function table')
    if struct.unpack_from('<I',blob,offset)[0] != 1: raise ValueError('multiple entry points unsupported')
    pos=offset+8; name=None
    while pos+4<=offset+length:
        tag=blob[pos:pos+4]; pos+=4
        if tag==b'ENDT':break
        n=struct.unpack_from('<H',blob,pos)[0];pos+=2
        if pos+n>size:raise ValueError('invalid tag length')
        if tag==b'NAME':name=blob[pos:pos+n].rstrip(b'\0').decode()
        pos+=n
    if name != meta['entry_point']:raise ValueError('entry-point name mismatch')
    if meta.get('implicit_imageblock_attachments') or meta.get('fragment_imageblock'):raise ValueError('imageblock descriptors unsupported')
    stage={'Vertex':1,'Fragment':2,'Kernel':3}[meta['stage']]
    bindings=[]
    for b in meta['bindings']:
        desc=b.get('descriptor')
        if desc is None:raise ValueError('non-descriptor resource unsupported')
        if desc['count']!=1 or desc['set']!=0:raise ValueError('descriptor array/set unsupported')
        if (b.get('texture_shape') or {}).get('dimension')=='Buffer':raise ValueError('texel buffer unsupported')
        kind={'Buffer':0,'Texture':1,'StorageImage':1,'Sampler':2}.get(b['kind'])
        if kind is None:raise ValueError('unsupported binding '+b['kind'])
        access=0 if b['kind']!='StorageImage' else {'ReadOnly':1,'WriteOnly':2,'ReadWrite':3}.get(b.get('access'),3)
        bindings.append([kind,b['metal_index'],desc['binding'],access])
    return {'version':1,'entry':name,'stage':stage,'bindings':bindings}

def remap_sets(data, stage, varyings=()):
    if len(data)%4:raise ValueError('unaligned SPIR-V')
    words=list(struct.unpack('<%dI'%(len(data)//4),data))
    if len(words)<5 or words[0]!=0x07230203:raise ValueError('invalid SPIR-V header')
    locations={}
    for varying in varyings:
        semantic=re.fullmatch(r'user\(locn(\d+)\)',varying.get('user_semantic') or '')
        if semantic is None:raise ValueError('unsupported varying semantic')
        locations[varying['location']]=int(semantic[1])
    if len(set(locations.values()))!=len(locations):raise ValueError('duplicate varying semantic')
    instructions=[];storage={};at=5
    while at<len(words):
        n=words[at]>>16; op=words[at]&65535
        if not n or at+n>len(words):raise ValueError('invalid SPIR-V instruction')
        instructions.append((at,n,op))
        if op==59 and n>=4:storage[words[at+2]]=words[at+3] # OpVariable
        at+=n
    for at,n,op in instructions:
        if op==71 and n==4 and words[at+2]==30 and storage.get(words[at+1])==({1:3,2:1}.get(stage)):
            # Link independently compiled stages by Metal's user semantic, not
            # their compact, stage-local reflection locations. Do not alter
            # vertex attributes or fragment render-target locations.
            old=words[at+3]
            if old not in locations:raise ValueError('varying location absent from reflection')
            words[at+3]=locations[old]
        if op==71 and n==4 and words[at+2]==34:
            if words[at+3]!=0:raise ValueError('unexpected descriptor set')
            words[at+3]=1 if stage==2 else 0
    return struct.pack('<%dI'%len(words),*words)

def prepare(item):
    index,blob=item; source=SOURCE/f'{index:04d}'
    try:
        reflection=json.loads(source.with_suffix('.json').read_text())
        meta=adapt(blob,reflection)
        data=remap_sets(source.with_suffix('.spv').read_bytes(),meta['stage'],reflection.get('varyings',()))
        key=hashlib.sha256(blob).hexdigest();dest=OUT/key
        dest.with_suffix('.spv').write_bytes(data)
        env = dict(os.environ)
        if libraries := env.get('ROBLOX_MAC_SHADER_LIBRARY_PATH'):
            env['LD_LIBRARY_PATH'] = libraries
        subprocess.run(['spirv-val','--target-env','vulkan1.2',str(dest.with_suffix('.spv'))],check=True,capture_output=True,env=env)
        dest.with_suffix('.json').write_text(json.dumps(meta))
        return index,key,None
    except (ValueError,KeyError,OSError,subprocess.CalledProcessError) as e:return index,None,str(e)

def main():
    global PACK, OUT, SOURCE
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app',type=pathlib.Path,default=ROOT.parent/'RobloxPlayer.app')
    parser.add_argument('--source',type=pathlib.Path,default=SOURCE)
    parser.add_argument('--output',type=pathlib.Path,default=OUT)
    args=parser.parse_args()
    PACK=args.app/'Contents/Resources/shaders/shaders_metal_osx.pack'
    OUT=args.output;SOURCE=args.source
    OUT.mkdir(parents=True,exist_ok=True)
    data=PACK.read_bytes();items=[]
    for index,m in enumerate(re.finditer(b'MTLB',data)):
        n=struct.unpack_from('<Q',data,m.start()+16)[0];items.append((index,data[m.start():m.start()+n]))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:results=list(pool.map(prepare,items))
    report={'prepared':sum(key is not None for _,key,_ in results),'total':len(items),'pack_sha256':hashlib.sha256(data).hexdigest(),'failures':[(i,e) for i,key,e in results if key is None]}
    if not report['prepared']:raise SystemExit('No shaders prepared; keep the previous client')
    (OUT/'report.json').write_text(json.dumps(report,indent=2));print(report['prepared'],'/',report['total'],'validated shaders prepared')
if __name__=='__main__':main()
