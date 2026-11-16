# lib_rank.py <profile_cpu.txt...>: library functions (RVL SDK, nw4r, EGG, HBM, MSL/runtime by address range) ranked by
# their share of the busy CPU-thread samples (guest pc aggregated per generated function, idle loop excluded),
# averaged over the profiles. N = has a native body in r19n (difftest/n1_addrs.txt). TOP=n rows (150).
import re,sys,bisect,collections
S='/tmp/claude-1000/-home-djvemo-nsmbw-port/1b06eac6-a23b-489b-924a-77167831057b/scratchpad/'
ents=[]
for n in open('/home/djvemo/nsmbw-port/r9/r19n/tools/fnames_r18m.txt'):
    n=n.strip(); m=re.search(r'_([0-9A-F]{8})$',n)
    if m: ents.append((int(m.group(1),16),n))
ents.sort(); starts=[e[0] for e in ents]
sym={}
for l in open('/home/djvemo/nsmbw-port/symbols/usa_symbols.txt'):
    p=l.split()
    if len(p)>=3:
        try: sym[int(p[0],16)]=p[2]
        except: pass
symk=sorted(sym)
def lib(a):
    if 0x80004000<=a<0x80006800: return 'MSL/runtime'
    if 0x80171000<=a<0x80195000: return 'HBM'
    if 0x8019A000<=a<0x80200000: return 'RVL SDK'
    if 0x80200000<=a<0x802BC000: return 'nw4r'
    if 0x802BC000<=a<0x802DC000: return 'EGG'
    if 0x802DC000<=a<0x80300000: return 'MSL/runtime'
    return None
tot=collections.Counter(); busy=0
for p in sys.argv[1:]:
    g=collections.Counter(); t=0
    for l in open(p):
        if l.startswith('guest '):
            _,c,pc=l.split(); c=int(c); pc=int(pc,16)
            i=bisect.bisect_right(starts,pc)-1
            if i<0 or not (0x80004000<=pc<0x81800000): continue
            a=ents[i][0]
            if a==0x801B4EA0: continue
            g[a]+=c; t+=c
    for a,c in g.items(): tot[a]+=c/t
    busy+=1
n1=set(int(l,16) for l in open('/home/djvemo/nsmbw-port/r9/r19n/difftest/n1_addrs.txt'))
rows=[(c/busy,a) for a,c in tot.items() if lib(a)]
rows.sort(reverse=True)
s=0
for k,(c,a) in enumerate(rows[:int(__import__('os').environ.get('TOP','150'))]):
    s+=c
    j=bisect.bisect_right(symk,a)-1
    nm=sym[symk[j]] if j>=0 and symk[j]==a else [n for st,n in ents if st==a][0]
    print('%3d %08X %-12s %6.3f%% cum %5.2f%% %s %s'%(k+1,a,lib(a),100*c,100*s,'N' if a in n1 else '-',nm[:70]))
print('library total %.2f%% of busy'%(100*sum(c for c,a in rows)))
