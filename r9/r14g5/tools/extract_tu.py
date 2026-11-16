#!/usr/bin/env python3
"""extract_tu.py <export_dir> <out.cpp> <func_prefix>...: mini TU with the given generated
functions (name or name prefix, e.g. PSMTXConcat), for quick AArch64 code-size experiments."""
import os, re, sys
d, out, names = sys.argv[1], sys.argv[2], sys.argv[3:]
files = sorted(x for x in os.listdir(d) if re.match(r'output_\d+\.cpp$', x))
head, body, found = None, [], set()
for x in files:
    src = open(os.path.join(d, x)).read().split('\n')
    i = 0
    while i < len(src):
        l = src[i]
        m = re.match(r'void (\w+)\(CPUContext& ctx\) \{$', l)
        if m and any(m.group(1) == n or m.group(1).startswith(n + '_') for n in names):
            j = i
            while src[j] != '}': j += 1
            if head is None:
                k = src.index('#include "functions.h"')
                head = src[:k + 1]
            body += src[i:j + 1] + ['']
            found.add(m.group(1))
            i = j
        i += 1
print('found', sorted(found), file=sys.stderr)
open(out, 'w').write('\n'.join(head + [''] + body))
