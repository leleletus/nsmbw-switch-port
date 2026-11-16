#!/usr/bin/env python3
# cg_query.py datos/callgraph.json reach <root>... | callers <addr> | ext <lo> <hi> (callees fuera del rango, con nº de llamadores)
import json, sys
G = json.load(open(sys.argv[1])); cmd = sys.argv[2]
def nm(a): return G[a]["name"] if a in G else "?"+a
if cmd == "reach":
    seen = set(); st = [a.upper() for a in sys.argv[3:]]
    while st:
        a = st.pop()
        if a in seen or a not in G: continue
        seen.add(a); st += G[a]["calls"] + G[a]["inl"]
    for a in sorted(seen): print(a, nm(a), G[a]["file"], "ind=%d" % G[a]["indirect"])
elif cmd == "callers":
    t = sys.argv[3].upper()
    for a, v in sorted(G.items()):
        if t in v["calls"] or t in v["inl"]: print(a, v["name"], v["file"], "INL" if t in v["inl"] else "")
elif cmd == "ext":
    lo, hi = int(sys.argv[3], 16), int(sys.argv[4], 16); cnt = {}
    for a, v in G.items():
        if lo <= int(a, 16) < hi:
            for c in v["calls"] + v["inl"]:
                if not (lo <= int(c, 16) < hi): cnt.setdefault(c, []).append(a)
    for c in sorted(cnt): print(c, nm(c), len(cnt[c]), G[c]["file"] if c in G else "")
