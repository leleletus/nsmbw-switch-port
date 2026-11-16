#!/usr/bin/env python3
"""subsis.py <nm -n -S> <profile_cpu.txt>...: reparto plano del hilo del juego por subsistema (rangos USA).
Cuenta muestras propias de cada funcion invitada + ayudantes del runtime cuyo LR cae en ella (como rank.py)."""
import bisect, collections, sys, re
RANGES = [  # (ini, fin, nombre)
 (0x80004000,0x80006780,'MSL/rt (memcpy,memset)'),
 (0x80006780,0x80160000,'juego DOL (bases)'),
 (0x80160000,0x80164000,'juego: cLib/framework (fBase, listas)'),
 (0x80164000,0x80165000,'juego: m2d'),
 (0x80165000,0x8016B000,'juego: m3d (envoltorio g3d)'),
 (0x8016B000,0x80177000,'juego: mLib resto (mDvd,mEf,mHeap,mMtx...)'),
 (0x80177000,0x80195000,'HBM (nw4hbm)'),
 (0x80195000,0x801A0000,'juego: Snd* (gestor de sonido del juego)'),
 (0x801A0000,0x801ABA00,'SDK: AI/AX/AXFX/DSP'),
 (0x801ABA00,0x801BC000,'SDK: OS'),
 (0x801BC000,0x801C0400,'SDK: VI/EXI/SI'),
 (0x801C0400,0x801C3000,'SDK: MTX'),
 (0x801C3000,0x801CAD00,'SDK: GX'),
 (0x801CAD00,0x80228D00,'SDK: DVD/MEM/CX/SC/WPAD/KPAD/NAND/IPC...'),
 (0x80228D00,0x80237B00,'nw4r::ut/db'),
 (0x80237B00,0x8023A000,'nw4r::math'),
 (0x8023A000,0x80266000,'nw4r::g3d'),
 (0x80266000,0x80285800,'nw4r::snd'),
 (0x80285800,0x802AB4F0,'nw4r::ef'),
 (0x802AB4F0,0x802B8A70,'nw4r::lyt'),
 (0x802B8A70,0x802DC000,'EGG'),
 (0x802DC000,0x80300000,'MSL/rt (libc, fdlibm, ptmf)'),
 (0x80760000,0x81000000,'juego REL'),
]
def rng(a):
    for lo,hi,n in RANGES:
        if lo<=a<hi: return n
    return 'otro invitado'
nm=sys.argv[1]
syms=[]
for l in open(nm):
    q=l.split()
    if len(q)==4 and q[2] in 'tTwW': syms.append((int(q[0],16),int(q[1],16),q[3]))
    elif len(q)==3 and q[1] in 'tTwW': syms.append((int(q[0],16),0,q[2]))
syms.sort(); st=[s[0] for s in syms]
def look(o):
    i=bisect.bisect_right(st,o)-1
    if i>=0 and o<syms[i][0]+max(4,syms[i][1]): return syms[i][2]
    return None
def guest(n):
    if n is None: return None
    m=re.search(r'(?:^func_|_)(8[0-9A-Fa-f]{7})(?:_g6)?$',n)
    return int(m.group(1),16) if m else None
IDLE={'svcSleepThread','_ZN4nwii7runtime9idle_waitERNS0_10CPUContextE'}
for prof in sys.argv[2:]:
    base=None; c=collections.Counter(); rt=collections.Counter(); tot=idle=0
    for l in open(prof):
        p=l.split()
        if not p or p[0].startswith('#'): continue
        if p[0]=='base': base=int(p[1],16)
        elif len(p)==3 and p[0].isdigit():
            n=int(p[0]); pc=int(p[1],16)-base; lr=int(p[2],16)-base; tot+=n
            s=look(pc)
            if s in IDLE: idle+=n; continue
            g=guest(s)
            if g: c[rng(g)]+=n
            else:
                gl=guest(look(lr))
                if gl and s and 'nwii' in s: c[rng(gl)+' ']+=0; c[rng(gl)]+=n
                else: rt[s]+=n
    busy=tot-idle
    print('== %s: muestras %d, ociosas %d (%.1f %%), ocupadas %d'%(prof.split('/')[-2],tot,idle,100*idle/tot,busy))
    for k,v in sorted(c.items(),key=lambda x:-x[1]):
        if v: print('  %5.1f %%  %s'%(100*v/busy,k))
    print('  %5.1f %%  runtime puro (sin funcion invitada en PC ni LR)'%(100*sum(rt.values())/busy))
    for k,v in rt.most_common(14): print('      %4.1f %s'%(100*v/busy,k))
