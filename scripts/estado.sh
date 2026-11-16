#!/usr/bin/env bash
# Resumen de lo que está corriendo en el proyecto NSMBW.
#   ~/nsmbw-port/scripts/estado.sh              una vez
#   watch -n 5 ~/nsmbw-port/scripts/estado.sh   en vivo
P=~/nsmbw-port
echo "== Carga: $(cut -d' ' -f1-3 /proc/loadavg) ($(nproc) hilos) | RAM libre: $(free -g | awk '/Mem/{print $7}') GB | energía: $(powerprofilesctl get 2>/dev/null)"
mem=$(systemctl --user show nsmbw.slice -p MemoryCurrent --value 2>/dev/null)
[[ "$mem" =~ ^[0-9]+$ ]] && echo "   memoria usada por los agentes (tope 7 GB): $((mem/1024/1024)) MB"
echo
echo "== Ronda en curso (workflow)"
J=$(ls -t ~/.claude/projects/-home-djvemo/*/subagents/workflows/*/journal.jsonl 2>/dev/null | head -1)
[ -n "$J" ] && python3 - "$J" <<'PY'
import json,sys
st={}
for l in open(sys.argv[1]):
    try: j=json.loads(l)
    except: continue
    k=j.get('key')
    if j.get('type')=='started':
        for kk in [kk for kk,v in st.items() if v[0]==j.get('label') and v[1].startswith('⏳')]: del st[kk]
        st[k]=[j.get('label','?'),'⏳ trabajando']
    elif j.get('type')=='result' and k in st:
        r=j.get('result'); v=r.get('verdict') if isinstance(r,dict) else None
        st[k][1]={'pass':'✅ verificado','issues':'⚠ encontró problemas'}.get(v,'✅ terminado')
for l,s in st.values(): print(f"  {l:22} {s}")
PY
echo
echo "== Instancias del juego (ocultas o tuyas)"
ps -eo pid=,etime=,args= | awk '$3 ~ /NSMBW/ {n=split($3,a,"/"); printf "  pid %-7s %-10s %s\n", $1, $2, a[n]}'
echo
echo "== Compilaciones (archivos compilándose por carpeta)"
ps -eo comm=,args= | awk '$1=="cc1plus" && match($0,/nsmbw-port\/[a-z0-9_]+(\/r6)?/) {c[substr($0,RSTART+11,RLENGTH-11)]++} END {for (d in c) printf "  %s: %d\n", d, c[d]}'
echo
echo "== Dolphin"
ps -eo pid=,etime=,args= | awk '$3 ~ /dolphin-emu/ {printf "  pid %-7s %s\n", $1, $2}'
echo
echo "== Binarios más recientes"
ls -t $P/recomp/bin/* $P/*/bin/NSMBW* $P/perf/r6/bin/NSMBW* 2>/dev/null | head -8 | xargs -r ls -lat --time-style=+'%d %H:%M' | awk '{sub(ENVIRON["HOME"]"/nsmbw-port/","",$8); print "  " $6, $7, $8}'
