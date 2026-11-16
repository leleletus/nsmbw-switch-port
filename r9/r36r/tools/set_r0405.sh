#!/usr/bin/env bash
# set_r0405.sh <rN>: points the queued R04 (todo) / R05 (base) jobs at bin/nsmbw_<rN>.nro (same queue position).
set -euo pipefail
v=$1; C=/home/djvemo/nsmbw-port/tools/switchctl/.cola; H=/home/djvemo/nsmbw-port/tools/switchctl
B=/home/djvemo/nsmbw-port/r9/r36r/bin
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
mk() { { printf "export BURST_FROM=%q BURST_SECS=%q\n" 20 0; printf "export %s=%q\n" SHOT_EVERY 10; printf "export %s=%q\n" DEBUG_EXTRA "$2"; printf "%q %q %q %q\n" "$H/bench_title.sh" "$C/$1.nro" "$1" 470; } > "$C/$3.new"; mv "$C/$3.new" "$C/$3"; }
cp $B/nsmbw_$v.nro $C/R04_${v}_todo.nro; cp $B/nsmbw_$v.nro $C/R05_${v}_base.nro; cp $B/nsmbw_$v.elf $B/nsmbw_${v}_R04R05.elf
o4=$(cd $C && ls | grep '^4723870566309_R04_.*\.job$'); o5=$(cd $C && ls | grep '^4723870575288_R05_.*\.job$')
mk R04_${v}_todo "$L;NWII_R_COALESCE=\"1\";NWII_R_FREECHUNK=\"1\";NWII_R_PREFIX=\"1\";NWII_R_CULL=\"1\"" 4723870566309_R04_${v}_todo.job
mk R05_${v}_base "$L" 4723870575288_R05_${v}_base.job
[[ $o4 != 4723870566309_R04_${v}_todo.job ]] && mv "$C/$o4" "$C/$o4.reemplazado"
[[ $o5 != 4723870575288_R05_${v}_base.job ]] && mv "$C/$o5" "$C/$o5.reemplazado"
ls $C | grep '\.job$'
