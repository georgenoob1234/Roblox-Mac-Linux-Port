#!/usr/bin/env python3
"""shaders_metal_osx.pack -> one .air (LLVM bitcode wrapper) per embedded metallib.
Pack = RBXS container of MTLB metallibs; metallib header holds the bitcode section offset/size at +72/+80."""
import re, struct, sys, os
pack, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
d = open(pack, 'rb').read()
n = 0
for i, m in enumerate(re.finditer(b'MTLB', d)):
    s = m.start()
    fsz = struct.unpack_from('<Q', d, s + 16)[0]
    boff, bsz = struct.unpack_from('<QQ', d, s + 72)
    assert boff + bsz == fsz, (i, boff, bsz, fsz)
    open(f'{out}/{i:04d}.air', 'wb').write(d[s + boff:s + boff + bsz]); n += 1
print(n, 'air files')
