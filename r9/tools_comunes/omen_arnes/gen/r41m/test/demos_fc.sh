#!/usr/bin/env bash
# [r41m: copia de r40d, a su vez copia de r39r2/test/demos_fc.sh, arnés propio] demos_fc.sh <bin_input_test> <tag> [secs=100] [extra env...]: each demo of the fixed list at 1.5789x with
# NWII_R9_FRAMECHECK=10 (R9/GPU-XF path, with NWII_R_CULL, compared pixel by pixel with the r8 path) and the
# coalescing state check; prints one line per demo. Uses r9/r36r/pc (own run dir and lock).
set -u
V=/home/djvemo/nsmbw-port/r9/verif_omen/r41m
bin=$(realpath "$1"); tag=$2; secs=${3:-100}; shift 3 || shift $#
for d in desierto4j agua_pinguino cielo_paratroopas nubes_plataformas castillo; do
  name=fc_${tag}_$d
  ( cd $V && flock pc/.runlock env NWII_FORCE_DEMO=$d NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 NWII_R9_FRAMECHECK=10 \
      NWII_R_COALESCE_CHECK=1 "$@" EVERY=0 PROFILE=free RES=1.5789 pc/run.sh "$bin" "$name" "$secs" mp "0-$secs:100:20:4" > /dev/null 2>&1 )
  L=$V/pc/runs/$name/run.log
  printf '%-22s %s diff_lines=%s | %s | culled/fot %s | %s\n' "$d" \
    "$(grep -a FRAMECHECK "$L" | tail -1 | sed 's/.*checks=/checks=/; s/ (pass1.*//')" \
    "$(grep -a FRAMECHECK "$L" | grep -vc ' 0 pixels differ')" \
    "$(grep -a RCOAL "$L" | tail -1 | grep -o 'check: .*')" \
    "$(grep -a GPUXF "$L" | grep -o 'culled [0-9]*' | awk '{printf "%s ", $2}')" \
    "$(tail -1 "$L")"
  printf '%-22s %s | D40: %s\n' "" "$(grep -a D40CHECK "$L" | tail -1)" \
    "$(grep -a '^\[D40\] lotes' "$L" | awk '{for(i=1;i<=NF;i++){if($i=="barato")b+=$(i+1); if($i=="viejo")v+=$(i+1)}} END{printf "lotes baratos %d viejos %d", b, v}')"
done
