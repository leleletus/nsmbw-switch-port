#!/usr/bin/env bash
# set_r0407_r11.sh: R04 = r11 todo (+TGMEMO), R05 = r11 base, R06 = r11 todo sin TGMEMO, R07 = r11 todo + TPROF/RPHASE
set -euo pipefail
C=/home/djvemo/nsmbw-port/tools/switchctl/.cola; H=/home/djvemo/nsmbw-port/tools/switchctl; B=/home/djvemo/nsmbw-port/r9/r36r/bin
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
T="$L;NWII_R_COALESCE=\"1\";NWII_R_FREECHUNK=\"1\";NWII_R_PREFIX=\"1\";NWII_R_CULL=\"1\""
mk() {  # name debug_extra seq [pull_prefix]
  cp $B/nsmbw_r11.nro "$C/$1.nro"
  { printf "export BURST_FROM=%q BURST_SECS=%q\n" 20 0; printf "export %s=%q\n" SHOT_EVERY 10; printf "export %s=%q\n" DEBUG_EXTRA "$2"
    [[ -n ${4:-} ]] && printf "export %s=%q\n" PULL_PREFIX "$4"
    printf "%q %q %q %q\n" "$H/bench_title.sh" "$C/$1.nro" "$1" 470; } > "$C/$3_$1.job.new"
  for o in $(cd $C && ls | grep "^$3_.*\.job$" || true); do mv "$C/$o" "$C/$o.reemplazado"; done
  mv "$C/$3_$1.job.new" "$C/$3_$1.job"
}
cp $B/nsmbw_r11.elf $B/nsmbw_r11_R04R07.elf
mk R04_r11_todo "$T;NWII_R_TGMEMO=\"1\"" 4723870566309
mk R05_r11_base "$L" 4723870575288
mk R06_r11_sintgm "$T" 4726063703398
mk R07_r11_todo_tprof "$T;NWII_R_TGMEMO=\"1\";NWII_TPROF=\"1\";NWII_RPHASE=\"1\"" 4726063710565 tprof
ls $C | grep '\.job$'
