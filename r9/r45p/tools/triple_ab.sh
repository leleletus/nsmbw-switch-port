#!/usr/bin/env bash
# [r45p] triple_ab.sh <bin_input_test> <tag> [slow_us=22000] [segs=110]: selector de mundos en PC con el render frenado
# (NWII_R11_SLOW_RENDER_US: el render pasa a ser el lado lento, como en la consola) y el trabajador frenado x3
# (NWII_D_SLOWWORKER=3), sin y con NWII_P_TRIPLE=1. Compara fotogramas dibujados/s, descartados y la vuelta del render.
bin=$(realpath "$1"); cd /home/djvemo/nsmbw-port/r9/r41m; tag=$2; slow=${3:-22000}; secs=${4:-110}
for v in ctl triple; do
  ex=(); [[ $v == triple ]] && ex=(NWII_P_TRIPLE=1)
  name=tri_${tag}_$v
  flock pc/.runlock env NWII_NO_SAVE=1 NWII_INPUT_PHASES=3 NWII_INPUT_SCRIPT_1=300-306:1000:100:6 NWII_R11_SLOW_RENDER_US=$slow \
    NWII_D_SLOWWORKER=3 "${ex[@]}" EVERY=1200 PROFILE=free RES=1.5789 pc/run.sh "$bin" $name $secs mp '0-100000:100:20:4' > /dev/null 2>&1
  L=pc/runs/$name/run.log
  echo "== $v: $(grep -a 'P45\]' $L | head -1)"
  python3 /home/djvemo/nsmbw-port/r9/r45p/tools/pantalla.py --sel 300 $L | tail -1
  grep -a '^\[P45T\]' $L | tail -1; grep -a '^\[FPS\]' $L | tail -2 | cut -c1-80
done
