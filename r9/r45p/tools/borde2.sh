#!/usr/bin/env bash
# [r45p] borde2.sh <bin_input_test> <tag> <n> [ENV...]: n corridas (70 s, 1x) del selector con el render/trabajador
# frenados + NWII_P_STALELOG=1; por corrida: borde superior (brillo) y las 3 últimas copias grandes (hecha/STALE).
bin=$(realpath "$1"); cd /home/djvemo/nsmbw-port/r9/r41m; tag=$2; n=$3; shift 3
for i in $(seq 1 $n); do
  name=brd2_${tag}_$i
  flock pc/.runlock env NWII_NO_SAVE=1 NWII_INPUT_PHASES=3 NWII_INPUT_SCRIPT_1=300-306:1000:100:6 NWII_R11_SLOW_RENDER_US=22000 \
    NWII_D_SLOWWORKER=${SLOWW:-3} NWII_P_STALELOG=1 "$@" EVERY=600 PROFILE=free RES=1 pc/run.sh "$bin" $name 70 mp '0-100000:100:20:4' > /dev/null 2>&1
  f=$(ls pc/runs/$name/t_*.png 2>/dev/null | tail -1)
  m=$(magick "$f" -crop 640x6+0+0 -colorspace gray -format '%[fx:mean*255]' info: 2>/dev/null)
  echo "$i: borde $m | ultimas copias grandes: $(grep -a 'P45S' pc/runs/$name/run.log | tail -4 | awk '{print $3}' | tr '\n' ' ') | $(grep -a 'P45S' pc/runs/$name/run.log | tail -1 | grep -o 'dest=[0-9A-F]* [0-9x]*')"
done
