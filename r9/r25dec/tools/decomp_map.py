import sys,os,json,collections
sys.path.insert(0,os.path.expanduser('~/nsmbw-port/scripts'))
import sym_match as sm
ROOT=os.path.expanduser('~/nsmbw-port')
usa=collections.defaultdict(dict)  # src -> name -> usa
for l in open(ROOT+'/symbols/usa_symbols.txt'):
    p=l.split()
    if len(p)>=4 and p[-1].startswith('[match:') :
        usa[p[-1][7:-1]][p[2]]=int(p[0],16)
out=open(sys.argv[1],'w')
for unit,mid in sm.UNIT_TO_MODULE.items():
    j=json.load(open(os.path.join(sm.DECOMP,'slices',('wiimj2d.json' if mid==0 else unit+'.json'))))
    base=int(j['meta']['sections']['.text']['addr'],16) if mid==0 else 0
    for sl in j['slices']:
        src=sl.get('source')
        if not src or '.text' not in sl.get('memoryRanges',{}): continue
        a,b=[int(x,16) for x in sl['memoryRanges']['.text'].split('-')]
        obj=os.path.join(sm.OBJDIR,unit,os.path.splitext(src)[0]+'.o')
        if not os.path.exists(obj): print('missing',obj); continue
        funcs=[(n,v,s,bind) for n,v,s,w,m,c,bind in sm.object_functions(obj)]
        deltas=collections.Counter()
        for n,v,s,bind in funcs:
            if bind!='STB_WEAK' and n in usa[src]: deltas[usa[src][n]-(base+a+v)]+=1
        if not deltas:
            out.write('SLICE\t%s\t%s\tPAL %x-%x\tnodelta\n'%(unit,src,base+a,base+b)); continue
        d,cnt=deltas.most_common(1)[0]
        out.write('SLICE\t%s\t%s\tPAL %x-%x\tUSA %X-%X\tdelta %d (%d votes, %d alt)\n'%(unit,src,base+a,base+b,base+a+d,base+b+d,d,cnt,len(deltas)-1))
        first_weak=min((v for n,v,s,bind in funcs if bind=='STB_WEAK'),default=1<<32)
        for n,v,s,bind in funcs:
            out.write('FN\t%08X\t%x\t%s\t%s\t%s\n'%(base+a+v+d,s,n,src,'weak' if bind=='STB_WEAK' or v>=first_weak else 'strong'))
