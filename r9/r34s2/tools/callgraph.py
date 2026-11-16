#!/usr/bin/env python3
# callgraph.py <export_dir> : grafo de llamadas DIRECTAS del codigo generado (G6c3).
#   -> escribe <out>.json {func_addr: {"name":..,"file":..,"calls":[addr..],"inl":[addr..],"indirect":n}}
import re, sys, json, glob, os
exp = sys.argv[1]; out = sys.argv[2]
defre = re.compile(r'^extern "C" uint64_t (\w+?)_g6\(nwii::runtime::CPUContext& ctx')
defre2 = re.compile(r'^void (\w+)\(CPUContext& ctx\) \{$')
callre = re.compile(r'\b(\w+?_?([0-9A-F]{8}))(?:_g6)?\(ctx[,)]')
inlre = re.compile(r'\[g6i\] (\w+) inlined')
G = {}
for f in sorted(glob.glob(os.path.join(exp, 'output_*.cpp'))):
    cur = None
    for line in open(f, errors='replace'):
        m = defre.match(line) or defre2.match(line)
        if m:
            nm = m.group(1); a = nm[-8:]
            try: int(a, 16)
            except ValueError: cur = None; continue
            cur = G.setdefault(a, {"name": nm, "file": os.path.basename(f), "calls": set(), "inl": set(), "indirect": 0})
            continue
        if cur is None: continue
        if 'nwii_dispatch' in line or 'g6_disp' in line or 'fast_dispatch' in line: cur["indirect"] += 1
        for m in callre.finditer(line):
            a = m.group(2)
            if a != cur["name"][-8:]: cur["calls"].add(a)
        for m in inlre.finditer(line):
            cur["inl"].add(m.group(1)[-8:])
for v in G.values(): v["calls"] = sorted(v["calls"]); v["inl"] = sorted(v["inl"])
json.dump(G, open(out, 'w'))
print(len(G), "funciones")
