#!/usr/bin/env bash
# [r45p] mrt_check.sh <bin_input_test> <tag> [segs=300]: NWII_R2_COPYMRT=1 + NWII_P_MRT_CHECK=1 (cada copia MRT contra la
# copia de una en una hecha en el momento de encolarla) + FRAMECHECK=10, a 1.5789x, en agua_pinguino, castillo y el
# selector de mundos. Resultado: r45p/test/mrt_check_<tag>.txt
bin=$(realpath "$1"); cd /home/djvemo/nsmbw-port/r9/r41m; tag=$2; secs=${3:-300}; O=/home/djvemo/nsmbw-port/r9/r45p/test/mrt_check_$tag.txt; : > $O
for d in agua_pinguino castillo selector; do
  name=mrtchk_${tag}_$d
  if [[ $d == selector ]]; then
    ex=(NWII_INPUT_PHASES=3 NWII_INPUT_SCRIPT_1=300-306:1000:100:6); sc='0-100000:100:20:4'
  else ex=(NWII_FORCE_DEMO=$d NWII_FORCE_DEMO_LOOP=1); sc="0-$secs:100:20:4"; fi
  flock pc/.runlock env "${ex[@]}" NWII_NO_SAVE=1 NWII_R2_COPYMRT=1 NWII_P_MRT_CHECK=1 NWII_R9_FRAMECHECK=10 \
    NWII_R9_FRAMECHECK_DUMP=/home/djvemo/nsmbw-port/r9/r45p/test/dump/$tag_$d EVERY=0 PROFILE=free RES=1.5789 \
    pc/run.sh "$bin" $name $secs mp "$sc" > /dev/null 2>&1
  L=pc/runs/$name/run.log
  { echo "== $d"; grep -a '^\[P45MRT\]' $L | tail -4; grep -a FRAMECHECK $L | tail -1; grep -a FRAMECHECK $L | grep -v ' 0 pixels differ' | head -3; tail -1 $L; } >> $O
done
echo FIN >> $O
