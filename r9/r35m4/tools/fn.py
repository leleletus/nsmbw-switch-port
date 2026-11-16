#!/usr/bin/env python3
# fn.py <addr> [export_dir]: imprime una funcion del codigo generado de forma compacta (direccion + sentencia)
import sys, json, re, os
a = sys.argv[1].upper(); exp = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '../../r30g6c3/c3/export')
G = json.load(open(os.path.join(os.path.dirname(__file__), '../datos/callgraph.json')))
f = os.path.join(exp, G[a]["file"]); on = False; addr = None
for line in open(f, errors='replace'):
    if line.startswith('extern "C" uint64_t ') and '_g6(nwii' in line:
        if on: break
        on = (a + '_g6(') in line and line.rstrip().endswith('{')
        if on: print(line[:90].strip())
        continue
    if not on: continue
    s = line.strip()
    m = re.match(r'// (0x[0-9A-F]{8}): ([0-9A-F]{8})', s)
    if m: addr = m.group(1); continue
    if s.startswith('loc_') or s.startswith('G5ic +=') or not s or s in ('{', '}'): continue
    if addr is None and (s.startswith('uint32_t G5lr') or s.startswith('if (NWII_UNLIKELY(G6lr0') or s.startswith('G5pc = ctx.pc') or s.startswith('case ') or s.startswith('switch') or s.startswith('if (ctx.pc !=') or s.startswith('default:')): continue
    s = re.sub(r'mv_\.(\w+)<\d+u>', r'\1', s)
    s = re.sub(r'\{ uint64_t t_ = (\w+)\(ctx, G5ic, G5lr\);.*', r'CALL \1', s)
    s = re.sub(r'(ctx\.\w+(\[\d+\])?(\.\w+)? = G5\w+; )+', '', s)
    s = re.sub(r'if \(NWII_UNLIKELY\(g6_poll_due\(ctx\)\)\) \{.*?\}\s*\}?', 'POLL; ', s)
    print(addr or '          ', s[:150]); addr = None
