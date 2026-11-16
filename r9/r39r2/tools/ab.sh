#!/usr/bin/env bash
# ab.sh <prefijo A> <prefijo B> ...: imagen, metrica por escena, fases RPHASE, subfases de copias, hash y prehash.
cd /home/djvemo/nsmbw-port/switch/real_logs || exit 1
B=(); for p in "$@"; do d=$(ls -d bench_${p}_* 2>/dev/null | tail -1); [[ -n $d ]] && B+=("$d"); done
for d in "${B[@]}"; do echo "$d: $(tail -1 $d/resumen.txt) | escenas $(grep -c 'DEMO\] start' $d/nwii.log) | $(grep -m1 -o 'cpu [0-9]* MHz, gpu [0-9]* MHz' $d/nwii.log) | $(grep -h -o 'CONGELADA.*\|CRASH.*' $d/resumen.txt | head -1)"; done
python3 /home/djvemo/nsmbw-port/r9/r31t/tools/por_escena_real.py "${B[@]}" | grep -v '^$'
python3 /home/djvemo/nsmbw-port/r9/r39r2/tools/rfases.py "${B[@]}" | grep -v RPROF
for d in "${B[@]}"; do for s in desierto4j agua_pinguino cielo_paratroopas; do
  echo "$d $s"; awk -v s=$s '$0 ~ "DEMO\\] start "s{f=1;k=0;j=0;m=0} $0 ~ "DEMO\\] end "s{f=0} f && /RPHASE2/{k++; if(k==4) print "  " $0} f && /TCSTAT3/{j++; if(j==1) print "  " $0} f && /R2PRE/{m++; if(m==1) print "  " $0}' $d/nwii.log | cut -c1-230
done; done
