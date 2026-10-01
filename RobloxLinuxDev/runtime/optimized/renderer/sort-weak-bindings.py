#!/usr/bin/env python3
"""Normalize our LLD output for Darling dyld's sorted weak-symbol merge.

LLD 22 emits weak bindings in discovery order; Darling's classic dyld merge
requires lexicographic symbol order. Never run this on the Roblox executable.
"""
import pathlib, struct, sys

def uleb(n):
    out=bytearray()
    while True:
        b=n&127;n>>=7;out.append(b|(128 if n else 0))
        if not n:return out

def sleb(n):
    out=bytearray()
    while True:
        b=n&127;n>>=7;done=(n==0 and not b&64) or (n==-1 and b&64)
        out.append(b|(0 if done else 128))
        if done:return out

def decode(data):
    at=0;symbol=None;segment=offset=addend=kind=0;symbols={}
    def number(signed=False):
        nonlocal at
        value=shift=0
        while True:
            if at>=len(data) or shift>63:raise ValueError('invalid LEB')
            b=data[at];at+=1;value|=(b&127)<<shift;shift+=7
            if not b&128:return value-(1<<shift) if signed and b&64 else value
    def bind():
        nonlocal offset
        if symbol is None or kind!=1:raise ValueError('unsupported weak binding')
        symbols[symbol][1].append((segment,offset,kind,addend));offset+=8
    while at<len(data):
        b=data[at];at+=1;op=b&240;imm=b&15
        if op==0:
            if any(data[at:]):raise ValueError('data after bind terminator')
            break
        if op==0x40:
            end=data.index(0,at);symbol=bytes(data[at:end]);at=end+1
            if symbol in symbols and symbols[symbol][0]!=imm:raise ValueError('conflicting flags')
            symbols.setdefault(symbol,(imm,[]))
        elif op==0x50:kind=imm
        elif op==0x60:addend=number(True)
        elif op==0x70:segment=imm;offset=number()
        elif op==0x80:offset+=number()
        elif op==0x90:bind()
        elif op==0xa0:bind();offset+=number()
        elif op==0xb0:bind();offset+=imm*8
        elif op==0xc0:
            count=number();skip=number()
            if count>len(data)*1024:raise ValueError('excessive bind count')
            for _ in range(count):bind();offset+=skip
        else:raise ValueError(f'unsupported bind opcode {op:x}')
    return symbols

def encode(symbols):
    out=bytearray()
    for name,(flags,bindings) in sorted(symbols.items()):
        out.append(0x40|flags);out+=name+b'\0'
        for segment,offset,kind,addend in bindings:
            out.append(0x50|kind);out.append(0x60);out+=sleb(addend)
            out.append(0x70|segment);out+=uleb(offset);out.append(0x90)
    out.append(0);return out

def normalize(path):
    path=path.resolve()
    if path.parent!=pathlib.Path(__file__).resolve().parent or path.suffix!='.dylib':
        raise ValueError('only local compatibility build dylibs may be normalized')
    data=bytearray(path.read_bytes())
    if len(data)<32 or struct.unpack_from('<III',data)[:2]!=(0xfeedfacf,0x1000007):raise ValueError('expected x86_64 Mach-O')
    pos=32;weak=link=None
    for _ in range(struct.unpack_from('<I',data,16)[0]):
        cmd,size=struct.unpack_from('<II',data,pos)
        if size<8 or pos+size>len(data):raise ValueError('invalid load command')
        if cmd in (0x22,0x80000022):weak=pos
        if cmd==0x19 and data[pos+8:pos+24].rstrip(b'\0')==b'__LINKEDIT':link=pos
        if cmd==0x1d:raise ValueError('link with -no_adhoc_codesign before normalization')
        pos+=size
    if weak is None:return
    offset,size=struct.unpack_from('<II',data,weak+24)
    if not size:return
    if link is None or offset+size>len(data):raise ValueError('invalid weak binding range')
    symbols=decode(data[offset:offset+size])
    if list(symbols)==sorted(symbols):return
    encoded=encode(symbols);assert decode(encoded)==symbols
    # Keep dyld's required rebase/bind/weak/lazy/export ordering in __LINKEDIT.
    encoded.extend(b'\0'*((-len(encoded))%8))
    end=offset+size;delta=len(encoded)-size
    pos=32
    for _ in range(struct.unpack_from('<I',data,16)[0]):
        cmd,cmdsize=struct.unpack_from('<II',data,pos)
        fields=[]
        if cmd in (0x22,0x80000022):fields=[8,16,24,32,40]
        elif cmd==2:fields=[8,16] # symbol and string tables
        elif cmd==0xb:fields=[32,40,48,56,64,72] # dynamic symbol tables
        elif cmd in (0x1e,0x26,0x29,0x2e,0x80000033,0x80000034):fields=[8]
        for relative in fields:
            old=struct.unpack_from('<I',data,pos+relative)[0]
            if old>=end:struct.pack_into('<I',data,pos+relative,old+delta)
        pos+=cmdsize
    struct.pack_into('<I',data,weak+28,len(encoded))
    data[offset:end]=encoded
    fileoff=struct.unpack_from('<Q',data,link+40)[0];filesize=len(data)-fileoff
    struct.pack_into('<Q',data,link+48,filesize)
    struct.pack_into('<Q',data,link+32,(filesize+4095)&~4095)
    path.write_bytes(data)
    print(f'{path.name}: sorted {len(symbols)} weak symbols for Darling dyld')

if __name__=='__main__':
    sample={b'z':(0,[(1,400,1,-4),(2,4096,1,0)]),b'a':(8,[])}
    assert decode(encode(sample))==sample
    for arg in sys.argv[1:]:normalize(pathlib.Path(arg))
