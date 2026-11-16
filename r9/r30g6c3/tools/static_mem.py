#!/usr/bin/env python3
"""static_mem.py <label=nx_obj_dir>... : like static_cmp.py but per class (hostprof.classify): for the functions
with console samples (fn_weights.tsv), weighted mean per function of: instructions, ctx loads/stores (by field),
stack loads/stores, mov/const. A function's cost = its _g6 body if it has one. Static (every path counts the
same, cold slow paths included), so read the ctx/stack columns, not only the total."""
import collections, os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostprof as H
OD = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump'
wf = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fn_weights.tsv')
W = {l.split()[0]: float(l.split()[1]) for l in open(wf) if l.strip()}
def scan(d):
    res = {}
    for o in sorted(f for f in os.listdir(d) if f.startswith('output_') and f.endswith('.o')):
        cur, ins = None, {}
        p = subprocess.Popen([OD, '-d', '--no-show-raw-insn', os.path.join(d, o)], stdout=subprocess.PIPE, text=True)
        def flush():
            if cur is not None:
                creg = H.ctx_reg(ins); c = collections.Counter(H.classify(mn, ops, creg) for mn, ops in ins.values())
                c['ins'] = len(ins); res[cur] = c
        for line in p.stdout:
            m = re.match(r'^[0-9a-f]+ <(\w+)>:$', line)
            if m:
                flush(); n = m.group(1); b = n[:-3] if n.endswith('_g6') else n
                cur = n if b in W else None; ins = {}
                continue
            if cur is None or ':\t' not in line: continue
            a, rest = line.split(':\t', 1); parts = rest.split('\t', 1)
            try: ins[int(a.strip(), 16)] = (parts[0].strip(), parts[1].split('//')[0].split('<')[0].strip() if len(parts) > 1 else '')
            except ValueError: pass
        flush(); p.wait()
    return res
builds = []
for x in sys.argv[1:]:
    lab, d = x.split('=', 1); builds.append((lab, scan(d)))
def cost(s, n): return s.get(n + '_g6') or s.get(n)
names = [n for n in W if all(cost(s, n) for _, s in builds)]
tw = sum(W[n] for n in names)
cols = ['ins', 'ctx ld', 'ctx st', 'ctx.pc', 'ctx.lr', 'ctx.inst_count', 'ctx.gpr', 'ctx.cr', 'stack ld', 'stack st', 'mov/const', 'branch']
def val(c, k):
    if k == 'ctx ld': return sum(v for kk, v in c.items() if kk.startswith('ctx.') and kk.endswith(' ld'))
    if k == 'ctx st': return sum(v for kk, v in c.items() if kk.startswith('ctx.') and kk.endswith(' st'))
    if k.startswith('ctx.'): return c.get(k + ' ld', 0) + c.get(k + ' st', 0)
    return c.get(k, 0)
print('functions with samples in every build: %d; weighted mean per function (weights = console samples)' % len(names))
print('%-8s' % 'build' + ''.join('%11s' % k for k in cols))
for lab, s in builds:
    print('%-8s' % lab + ''.join('%11.1f' % (sum(W[n] * val(cost(s, n), k) for n in names) / tw) for k in cols))
