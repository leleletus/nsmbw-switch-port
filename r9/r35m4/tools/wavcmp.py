#!/usr/bin/env python3
# wavcmp.py a.wav b.wav: compare the sample data of two NWII_AUDIODUMP files over their common length
# (runs stop on a wall-clock timeout, so the lengths differ). Prints the first differing frame, if any.
import sys
a, b = (open(p, 'rb').read()[44:] for p in sys.argv[1:3])
n = min(len(a), len(b)) // 4 * 4
first = next((i for i in range(0, n, 4096) if a[i:i + 4096] != b[i:i + 4096]), None)
if first is None:
    print(f"identical over the common {n // 4} frames ({n / 4 / 32000:.1f} s of audio); lengths {len(a) // 4} / {len(b) // 4}")
    sys.exit(0)
j = next(i for i in range(first, min(first + 4096, n), 4) if a[i:i + 4] != b[i:i + 4])
diff = sum(1 for i in range(0, n, 4) if a[i:i + 4] != b[i:i + 4])
print(f"DIFFER: first at frame {j // 4} ({j / 4 / 32000:.2f} s), {diff} of {n // 4} frames differ")
sys.exit(1)
