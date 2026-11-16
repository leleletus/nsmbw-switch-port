import bisect,collections,sys,re
nm=sys.argv[1]; profs=sys.argv[2:]
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
    m=re.search(r'(?:^func_|_)(8[0-9A-Fa-f]{7})$',n)
    return m.group(1).upper() if m else None
IDLE={'svcSleepThread','_ZN4nwii7runtime9idle_waitERNS0_10CPUContextE'}
res=[]
for prof in profs:
    base=None; selfc=collections.Counter(); helpc=collections.Counter(); tot=0; idle=0; gtot=0; rt=collections.Counter()
    for l in open(prof):
        p=l.split()
        if not p or p[0].startswith('#'): continue
        if p[0]=='base': base=int(p[1],16)
        elif len(p)==3 and p[0].isdigit():
            n=int(p[0]); pc=int(p[1],16)-base; lr=int(p[2],16)-base; tot+=n
            s=look(pc)
            if s in IDLE: idle+=n; continue
            g=guest(s)
            if g: selfc[g]+=n; gtot+=n
            else:
                rt[s]+=n
                gl=guest(look(lr))
                if gl and s and 'nwii' in s: helpc[gl]+=n
    busy=tot-idle
    sys.stderr.write('%s total %d idle %d busy %d guestself %d (%.1f%% of busy) helper-attr %d\n'%(prof,tot,idle,busy,gtot,100*gtot/busy,sum(helpc.values())))
    res.append((busy,selfc,helpc,rt))
keys=set()
for r in res: keys|=set(r[1])|set(r[2])
rows=[]
for k in keys:
    s=sum(r[1][k]/r[0] for r in res)/len(res)*100
    h=sum(r[2][k]/r[0] for r in res)/len(res)*100
    rows.append((s+h,s,h,k))
rows.sort(reverse=True)
for t,s,h,k in rows: print('%s\t%.3f\t%.3f\t%.3f'%(k,t,s,h))
# runtime top
sys.stderr.write('runtime top:\n')
rr=collections.Counter()
for r in res:
    for k,v in r[3].items(): rr[k]+=v/r[0]*100/len(res)
for k,v in rr.most_common(25): sys.stderr.write('  %.2f %s\n'%(v,k))
