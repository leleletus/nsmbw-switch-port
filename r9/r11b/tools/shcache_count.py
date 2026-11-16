# shcache_count.py <shadercache.bin>: records, distinct keys, distinct FS sources (NSC2 layout of renderer_gl.cpp)
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'NSC2'
o, n, keys, srcs = 12, 0, set(), set()
while o + 20 <= len(d):
    key, fmt, bl, sl = struct.unpack_from('<QIII', d, o)
    o += 20
    if o + bl + sl > len(d): break
    srcs.add(d[o + bl:o + bl + sl]); keys.add(key); n += 1; o += bl + sl
print(f"records={n} keys={len(keys)} sources={len(srcs)} bytes={len(d)}")
