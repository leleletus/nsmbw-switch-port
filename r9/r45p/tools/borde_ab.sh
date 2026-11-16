#!/usr/bin/env bash
# [r45p] borde_ab.sh <bin_input_test> <tag> <n> [extra ENV...]: n corridas cortas (60 s) del selector con el render y el
# trabajador frenados (como triple_ab.sh), sin y con NWII_P_TRIPLE=1; de cada una, el último fotograma volcado y si el
# borde de arriba muestra la captura del mapa (no negro). Resultado: test/borde_<tag>.txt + PNG en r41m/pc/runs/brd_*
bin=$(realpath "$1"); cd /home/djvemo/nsmbw-port/r9/r41m; tag=$2; n=$3; shift 3
for i in $(seq 1 $n); do for v in ctl triple; do
  ex=(); [[ $v == triple ]] && ex=(NWII_P_TRIPLE=1)
  name=brd_${tag}_${v}_$i
  flock pc/.runlock env NWII_NO_SAVE=1 NWII_INPUT_PHASES=3 NWII_INPUT_SCRIPT_1=300-306:1000:100:6 NWII_R11_SLOW_RENDER_US=22000 \
    NWII_D_SLOWWORKER=3 "${ex[@]}" "$@" EVERY=600 PROFILE=free RES=1 pc/run.sh "$bin" $name 70 mp '0-100000:100:20:4' > /dev/null 2>&1
  f=$(ls pc/runs/$name/t_*.png 2>/dev/null | tail -1)
  m=$(magick "$f" -crop 640x6+0+0 -colorspace gray -format '%[fx:mean*255]' info: 2>/dev/null)
  echo "$v $i: $(basename "$f") brillo franja superior $m | $(grep -a -c 'P45T' pc/runs/$name/run.log) P45T"
done; done
