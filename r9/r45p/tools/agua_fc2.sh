#!/usr/bin/env bash
# [r45p] agua_fc2.sh <bin_input_test> <tag> [segs=400] [ENV extra de la variante mrt...] (VARS="mrt ctl" por defecto): agua_pinguino con FRAMECHECK=2 (fundidos incluidos), con y sin
# COPYMRT (+NWII_P_MRT_CHECK en la de MRT), volcado de los fotogramas distintos. -> test/agua_fc2_<tag>.txt
bin=$(realpath "$1"); cd /home/djvemo/nsmbw-port/r9/r41m; tag=$2; secs=${3:-400}; VARS=${VARS:-mrt ctl}; shift 3 || shift $#
D=/home/djvemo/nsmbw-port/r9/r45p/test/dump; O=/home/djvemo/nsmbw-port/r9/r45p/test/agua_fc2_$tag.txt; : > $O
for v in $VARS; do
  ex=(); [[ $v == mrt ]] && ex=(NWII_R2_COPYMRT=1 NWII_P_MRT_CHECK=1 "$@")
  name=aguafc2_${tag}_$v
  flock pc/.runlock env NWII_FORCE_DEMO=agua_pinguino NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 NWII_R9_FRAMECHECK=2 \
    NWII_R9_FRAMECHECK_DUMP=$D/fc2_${tag}_$v "${ex[@]}" EVERY=0 PROFILE=free RES=1.5789 pc/run.sh "$bin" $name $secs mp "0-$secs:100:20:4" > /dev/null 2>&1
  L=pc/runs/$name/run.log
  echo "$v: $(grep -a FRAMECHECK $L | tail -1 | sed 's/ | xfb.*//') | $(grep -a '^\[P45MRT\]' $L | tail -1) | distintas: $(grep -a FRAMECHECK $L | grep -v ' 0 pixels differ' | sed 's/.*xfb/xfb/; s/ 1010x720 scale 1.5789x//' | tr '\n' ';')" >> $O
done
echo FIN >> $O
