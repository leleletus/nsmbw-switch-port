import bisect,sys,re
S=sys.argv[1]
names={}; nl=[]
for l in open('/home/djvemo/nsmbw-port/symbols/usa_symbols.txt'):
    p=l.split()
    a=int(p[0],16); names[a]=(p[2],p[-1]); nl.append((a,p[2],p[-1]))
nl.sort(); nk=[x[0] for x in nl]
fns={}; slices=[]
for l in open(S+'/decomp_map.tsv'):
    p=l.rstrip('\n').split('\t')
    if p[0]=='FN': fns.setdefault(int(p[1],16),[]).append((p[3],p[4],p[5]))
    elif p[0]=='SLICE' and p[4].startswith('USA'):
        a,b=p[4][4:].split('-'); slices.append((int(a,16),int(b,16),p[2]))
rows=[l.split() for l in open(S+'/rank.tsv')]
N=int(sys.argv[2])
cum=0; tot=0; dec=0
for i,(a,t,s,h) in enumerate(rows[:N]):
    A=int(a,16); t=float(t); tot+=t
    nm=names.get(A,(None,''))
    st='no'; src=''
    if A in fns:
        f=fns[A][0]; st='SI(exact)'; src=f[1]; nm=nm if nm[0] else (f[0],'')
    elif nm[1].startswith('[match:') or nm[1].startswith('[layout:'):
        st='SI(exact)'; src=nm[1].split(':',1)[1][:-1]
    else:
        for x,y,sr in slices:
            if x-0x40<=A<y+0x40: st='slice?'; src=sr
    if st!='no': dec+=t
    j=bisect.bisect_right(nk,A)-1
    near='' if nm[0] else ('after %s+0x%x'%(nl[j][1],A-nl[j][0]) if j>=0 else '')
    print('%d\t%s\t%.2f\t%.2f\t%.2f\t%s\t%s\t%s\t%s'%(i+1,a,t,float(s),float(h),nm[0] or '-',st,src,near))
sys.stderr.write('top%d total %.2f decompiled %.2f\n'%(N,tot,dec))
