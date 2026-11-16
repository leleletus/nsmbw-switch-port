#!/usr/bin/env python3
# apply_s2.py <export_in> <dir_out>   (frente S2, r9/r34s2)
# Pasada pequena sobre el codigo generado G6c3 (NO toca el generador): copia a <dir_out> SOLO los output_N.cpp
# que cambian, con tres clases de ganchos hacia el runtime (src/core/s2.cpp), todos tras `if (nwii_s2_on)`:
#  E1  cada escritura de MSR (mtmsr / rfi):  nwii_s2_ee(ctx, ee_was, LR, sitio)   -> candado S (ver LEEME)
#  E2  entrada de unas pocas funciones del SO: if (nwii_s2_os(ctx, F, LR)) return   -> la CPU de sonido las resuelve en nativo
#  E3  copias pegadas ([g6i]) de OSSetCurrentContext / OSGetCurrentThread: se saltan en la CPU de sonido
#  E4  copias del cuerpo de una funcion E2 dentro de otra que salta a ella con `b` (7 de OSWakeupThread, 1 de
#      OSSleepThread): en la CPU de sonido se sale al despachador hacia la funcion de verdad (que lleva E2)
# Con nwii_s2_on == false el codigo hace exactamente lo de antes (una carga + salto por gancho).
import re, sys, os, glob
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
OSFN = {  # E2: funciones con gancho de entrada
    0x801B2E60: "OSLockMutex", 0x801B2F40: "OSUnlockMutex", 0x801B3080: "OSTryLockMutex",
    0x801B23F0: "OSSendMessage", 0x801B24C0: "OSReceiveMessage", 0x801B5C90: "OSSleepThread",
    0x801B5D80: "OSWakeupThread", 0x801ACD40: "OSSetCurrentContext", 0x801B4B30: "OSGetCurrentThread",
    0x801B50F0: "OSYieldThread",
    # trampas (no deberian ejecutarse en la CPU de sonido): avisan y siguen
    0x801B3150: "OSWaitCond", 0x801B53A0: "OSExitThread", 0x801B4EA0: "SelectThread", 0x801B50D0: "__OSReschedule",
    0x801B5130: "OSCreateThread?", 0x801B5670: "OSJoinThread?", 0x801B5860: "OSResumeThread?", 0x801B5B00: "OSSuspendThread?",
    0x801AB540: "OSSetAlarm?", 0x801AB5B0: "OSSetPeriodicAlarm", 0x801AB640: "OSCancelAlarm?", 0x801CAB20: "DVDReadPrio",
}
INL = {0x801ACD40: "skip", 0x801B4B30: "thr"}  # E3
# E4: funciones E2 cuyo cuerpo puede estar copiado dentro de otras (las de las trampas no se tratan)
DUPFN = {0x801B2E60, 0x801B2F40, 0x801B3080, 0x801B23F0, 0x801B24C0, 0x801B5C90, 0x801B5D80, 0x801ACD40, 0x801B4B30, 0x801B50F0}
DECL = ('extern "C" { extern bool nwii_s2_on; void nwii_s2_ee(nwii::runtime::CPUContext&, uint32_t, uint32_t, uint32_t); '
        'bool nwii_s2_os(nwii::runtime::CPUContext&, uint32_t, uint32_t); bool nwii_s2_snd(nwii::runtime::CPUContext&); '
        'uint32_t nwii_s2_thr(void); }  // [r34-s2]\n')
defre = re.compile(r'^extern "C" uint64_t (\w+?)_g6\(nwii::runtime::CPUContext& ctx, uint64_t G5ic_in, uint64_t G6lr0\) \{$')
def2 = re.compile(r'^void (\w+)\(CPUContext& ctx\) \{$')
adre = re.compile(r'^\s*// (0x[0-9A-F]{8}): [0-9A-F]{8}')
inlre = re.compile(r'^\s*G5pc = 0x([0-9A-F]{8}); // \[g6i\] (\w+) inlined')
lrre = re.compile(r'^\s*G5lr = 0x([0-9A-F]{8}); // save return address')
tot = {"E1": 0, "E2": 0, "E3": 0, "E4": 0}; seen_fn = set(); warn = []
for f in sorted(glob.glob(os.path.join(src, 'output_*.cpp'))):
    L = open(f, errors='surrogateescape').read().split('\n')
    out = []; n = {"E1": 0, "E2": 0, "E3": 0, "E4": 0}; hdr_locals = []
    cur = None; g6 = False; lrx = 'ctx.lr'; site = None; i = 0; lastlr = None; inl_wait = None
    while i < len(L):
        s = L[i]
        m = defre.match(s)
        if m:
            cur = int(m.group(1)[-8:], 16); g6 = True; lrx = None; ent_done = False
        else:
            m2 = def2.match(s)
            if m2 and re.search(r'[0-9A-F]{8}$', m2.group(1)):
                cur = int(m2.group(1)[-8:], 16); g6 = False; lrx = 'ctx.lr'; ent_done = True
        if g6 and lrx is None and 'uint32_t G5lr = ' in s:
            lrx = 'G5lr'
            hdr_locals = re.findall(r'\b(?:uint32_t|bool|uint64_t|uint8_t|int32_t) (G5\w+) = ', s)
            hdr_locals += ['G7CR%s' % k for k in re.findall(r'\bG7CR_DECL\((\d)\)', s)]  # [r35m4] campos CR de G7 (feat cr)
        m = adre.match(s)
        if m: site = m.group(1)
        m = lrre.match(s)
        if m: lastlr = m.group(1)
        out.append(s)
        # E2: etiqueta de entrada de la funcion real
        if g6 and cur in OSFN and not ent_done and s == 'loc_%08X: ;' % cur:
            if lrx != 'G5lr': warn.append("E2 sin G5lr en %08X (%s)" % (cur, f))
            else:
                out.append('    if (NWII_UNLIKELY(nwii_s2_on) && nwii_s2_os(ctx, 0x%08Xu, G5lr)) return G6RETF(G5ic, G5lr);  // [r34-s2] E2 %s' % (cur, OSFN[cur]))
                n["E2"] += 1; seen_fn.add(cur)
            ent_done = True
        # E4: copia del cuerpo de una funcion E2 dentro de OTRA funcion (la que salta a ella con `b`): en el
        # contexto de sonido se vuelca todo y se sale al despachador con pc = entrada de la funcion de verdad,
        # que si lleva el gancho E2 (misma via que cualquier "escape" de G6c3).
        if g6 and s.startswith('loc_') and s.endswith(': ;') and len(s) == 15:
            try: la = int(s[4:12], 16)
            except ValueError: la = None
            if la in DUPFN and la != cur:
                fl = []; ok = lrx == 'G5lr'
                for nm in hdr_locals:
                    m3 = re.fullmatch(r'G5r(\d+)', nm)
                    m4 = re.fullmatch(r'G5c(\d)(eq|gt|lt|so)', nm)
                    if m3: fl.append('ctx.gpr[%s] = %s;' % (m3.group(1), nm))
                    elif m4: fl.append('ctx.cr[%s].%s = %s;' % (m4.group(1), m4.group(2), nm))
                    elif nm == 'G5xer': fl.insert(0, 'ctx.xer = G5xer;')
                    elif nm == 'G5ca': fl.append('ctx.xer = (ctx.xer & ~(1u << 29)) | ((uint32_t)G5ca << 29);')
                    elif nm == 'G5ctr': fl.append('ctx.ctr = G5ctr;')
                    elif nm in ('G5lr', 'G5pc', 'G5ic'): pass
                    elif re.fullmatch(r'G7CR\d', nm): fl.append('G7CR_STORE(%s);' % nm[4:])
                    else: ok = False; warn.append("E4 local desconocido %s en %08X (%s)" % (nm, cur, f))
                if ok:
                    out.append('    if (NWII_UNLIKELY(nwii_s2_on) && nwii_s2_snd(ctx)) { %s ctx.lr = G5lr; ctx.pc = 0x%08Xu; ctx.inst_count = G5ic; return g6_exact(ctx, G6lr0); }  // [r34-s2] E4 copia de %s' % (' '.join(fl), la, OSFN[la]))
                    n["E4"] += 1
        # E3: copia pegada
        m = inlre.match(s)
        if m and int(m.group(1), 16) in INL:
            a = int(m.group(1), 16); lab = L[i + 1]
            ml = re.match(r'^loc_1([0-9A-F]+)%08X: ;$' % a, lab)
            if not ml or lastlr is None: warn.append("E3 forma rara en %s:%d" % (f, i + 1))
            else:
                ident = ml.group(1); ret = None
                for j in range(i + 2, min(i + 400, len(L))):
                    mg = re.search(r'goto (loc_2%s([0-9A-F]{8}));' % ident, L[j])
                    if mg: ret = mg; break
                if ret is None or ret.group(2) != lastlr: warn.append("E3 sin retorno en %s:%d" % (f, i + 1))
                else:
                    out.append(lab); i += 1
                    body = 'G5r3 = nwii_s2_thr(); ' if INL[a] == "thr" else ''
                    out.append('    if (NWII_UNLIKELY(nwii_s2_on) && nwii_s2_snd(ctx)) { %sG5pc = 0x%su; goto %s; }  // [r34-s2] E3' % (body, lastlr, ret.group(1)))
                    n["E3"] += 1
        # E1: escritura de MSR
        if s.strip() == 'uint32_t ee_was = ctx.msr & 0x8000;':
            nx = L[i + 1]
            if not re.match(r'^\s*ctx\.msr = [^;]+;', nx): warn.append("E1 forma rara en %s:%d" % (f, i + 1))
            else:
                out.append(nx); i += 1
                ind = re.match(r'^\s*', nx).group(0)
                out.append('%sif (NWII_UNLIKELY(nwii_s2_on)) nwii_s2_ee(ctx, ee_was, %s, %su);  // [r34-s2] E1' % (ind, lrx or 'ctx.lr', site))
                n["E1"] += 1
        i += 1
    if sum(n.values()):
        # declaraciones: tras el ultimo #include del principio
        k = max(j for j, s in enumerate(out[:200]) if s.startswith('#include'))
        out.insert(k + 1, DECL.rstrip('\n'))
        dp = os.path.join(dst, os.path.basename(f)); txt = '\n'.join(out)
        if not (os.path.exists(dp) and open(dp, errors='surrogateescape').read() == txt):  # sin cambios: no se toca (ninja)
            open(dp, 'w', errors='surrogateescape').write(txt)
        print(os.path.basename(f), n)
        for k2 in tot: tot[k2] += n[k2]
print("total", tot)
for a in sorted(OSFN):
    if a not in seen_fn: print("AVISO: sin gancho de entrada:", "%08X" % a, OSFN[a])
for w in warn: print("AVISO:", w)
