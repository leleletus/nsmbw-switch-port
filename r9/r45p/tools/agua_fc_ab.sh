#!/usr/bin/env bash
# [r45p] agua_fc_ab.sh <bin_input_test> <segs>: agua_pinguino con FRAMECHECK=10 a 1.5789x, con y sin COPYMRT
# (volcado de los fotogramas distintos en r45p/test/dump/), para separar el ruido del método de un fallo de MRT.
bin=$(realpath "$1"); cd /home/djvemo/nsmbw-port/r9/r41m; secs=${2:-200}; D=/home/djvemo/nsmbw-port/r9/r45p/test/dump
for v in mrt ctl; do
  ex=(); [[ $v == mrt ]] && ex=(NWII_R2_COPYMRT=1)
  name=aguafc_p45_$v
  flock pc/.runlock env NWII_FORCE_DEMO=agua_pinguino NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 NWII_R9_FRAMECHECK=10 \
    NWII_R9_FRAMECHECK_DUMP=$D/$v "${ex[@]}" EVERY=0 PROFILE=free RES=1.5789 pc/run.sh "$bin" $name $secs mp "0-$secs:100:20:4" > /dev/null 2>&1
  echo "$v: $(grep -a FRAMECHECK pc/runs/$name/run.log | tail -1) | distintas: $(grep -a FRAMECHECK pc/runs/$name/run.log | grep -v ' 0 pixels differ' | sed 's/.*xfb/xfb/' | tr '\n' ';')"
done
