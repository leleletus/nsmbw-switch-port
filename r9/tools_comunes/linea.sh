#!/bin/bash
# linea.sh <nombre o patron de corrida>... -> una linea por corrida: fps dib | fps juego | render ms/fot por escena
cd /home/djvemo/nsmbw-port
for n in "$@"; do
  D=$(find switch/real_logs -maxdepth 1 -name "bench_${n}*" | sort | tail -1)
  [ -z "$D" ] && { echo "$n: no existe"; continue; }
  python3 r9/r31t/tools/por_escena_real.py "$D" 2>/dev/null | awk -v n="$(basename $D | sed 's/^bench_//')" '
    /^== /{esc=$2}
    /^bench_/{dib[esc]=$2; jg[esc]=$3; ms[esc]=$5; r[esc]=$7}
    END{split("desierto4j agua_pinguino cielo_paratroopas nubes_plataformas castillo",e," ");
      printf "%-34s dib", n; for(i=1;i<=5;i++) printf " %5s", dib[e[i]];
      printf " | juego"; for(i=1;i<=5;i++) printf " %5s", jg[e[i]];
      printf " | ms juego"; for(i=1;i<=5;i++) printf " %5s", ms[e[i]];
      printf " | render ms"; for(i=1;i<=5;i++) printf " %5s", r[e[i]]; printf "\n"}'
done
