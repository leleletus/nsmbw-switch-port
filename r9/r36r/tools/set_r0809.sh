#!/usr/bin/env bash
# set_r0809.sh <rN>: R08 = <rN> base, R09 = <rN> todo (COALESCE+FREECHUNK+PREFIX+CULL=1+TGMEMO), same queue positions
set -euo pipefail
v=$1; C=/home/djvemo/nsmbw-port/tools/switchctl/.cola; H=/home/djvemo/nsmbw-port/tools/switchctl; B=/home/djvemo/nsmbw-port/r9/r36r/bin
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
T="$L;NWII_R_COALESCE=\"1\";NWII_R_FREECHUNK=\"1\";NWII_R_PREFIX=\"1\";NWII_R_CULL=\"1\";NWII_R_TGMEMO=\"1\""
mk() {
  cp $B/nsmbw_$v.nro "$C/$1.nro"
  { printf "export BURST_FROM=%q BURST_SECS=%q\n" 20 0; printf "export %s=%q\n" SHOT_EVERY 10; printf "export %s=%q\n" DEBUG_EXTRA "$2"
    printf "%q %q %q %q\n" "$H/bench_title.sh" "$C/$1.nro" "$1" 470; } > "$C/$3_$1.job.new"
  for o in $(cd $C && ls | grep "^$3_.*\.job$" || true); do mv "$C/$o" "$C/$o.reemplazado"; done
  mv "$C/$3_$1.job.new" "$C/$3_$1.job"
}
cp $B/nsmbw_$v.elf $B/nsmbw_${v}_R08R09.elf
mk R08_${v}_base "$L" 4728041143686
mk R09_${v}_todo "$T" 4728041151401
ls $C | grep '\.job$'
