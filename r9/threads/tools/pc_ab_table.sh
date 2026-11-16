#!/usr/bin/env bash
# Tabla de corridas de PC (pc/runs/<n>/run.log + pc/prof/<n>.txt): fps, Minst/s, % CPU del hilo del juego y del
# host, CPU del hilo del juego por Minst (lo que se compara: no se satura a 60 fps) y reparto del hilo del juego.
# Uso: pc_ab_table.sh <nombre>...   (solo las líneas [FPS] desde que empieza la escena, menos las 2 primeras y las de < 50 Minst/s)
T=$HOME/nsmbw-port/r9/threads
for d in "$@"; do
  want='phase 1 starts'; [[ $d == *lvl11* ]] && want='phase 2 starts'
  awk -v w="$want" 'index($0,w){on=1} on && /^\[FPS/' $T/pc/runs/$d/run.log 2>/dev/null | awk -v n=$d '{m=0;c=0;h=0;mx="";for(i=1;i<=NF;i++){if($i=="Minst/s,")m=$(i-1); if($i=="thread"&&$(i-1)=="cpu")c=$(i+1)+0; if($i=="thread"&&$(i-1)=="host")h=$(i+1)+0; if($i=="mix")mx=$(i+1)}; if(NR>2 && m>50){S+=$2;M+=m;C+=c;H+=h;k++; if(mx!=""){X+=mx;kx++}}} END{if(k) printf "%-18s n=%2d fps %5.1f Minst %6.1f game %5.1f%% host %5.1f%% game/Minst %.4f mix %s us\n", n,k,S/k,M/k,C/k,H/k,C/M,(kx?sprintf("%.1f",X/kx):"-"); else print n" (sin datos)"}'
  f=$T/pc/prof/$d.txt
  [[ -f $f ]] && awk '/game CPU thread/{p=1;next} p&&/top functions/{exit} p{printf "    %s", $0; c++; if(c%4==0) print ""} END{print ""}' $f | sed 's/  */ /g'
done
