# shcache_dump.py <shadercache.bin> <dir>: escribe cada fuente FS distinta (NSC2) como <dir>/fs_<key>.glsl
import struct, sys, os
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'NSC2'
os.makedirs(sys.argv[2], exist_ok=True)
o, seen = 12, set()
while o + 20 <= len(d):
    key, fmt, bl, sl = struct.unpack_from('<QIII', d, o)
    o += 20
    if o + bl + sl > len(d): break
    s = d[o + bl:o + bl + sl]; o += bl + sl
    if s in seen or not s.startswith(b'#version'): continue
    seen.add(s)
    open(os.path.join(sys.argv[2], 'fs_%016x.glsl' % key), 'wb').write(s)
print(len(seen))
