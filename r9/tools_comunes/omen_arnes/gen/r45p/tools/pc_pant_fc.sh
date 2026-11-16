#!/usr/bin/env bash
# [r45p] pc_pant_fc.sh <bin_input_test> <tag>: mapa y selector de mundos (guion de r45p) a 1.5789x con
# NWII_R9_FRAMECHECK=30 + NWII_ECACHE_CHECK=1 (variables extra: del entorno). Una línea por pantalla.
bin=$(realpath "$1"); cd /home/djvemo/nsmbw-port/r9/verif_omen/r41m; tag=$2
export NWII_NO_SAVE=1 NWII_INPUT_PHASES=3 NWII_R9_FRAMECHECK=30 NWII_ECACHE_CHECK=1 NWII_R_COALESCE_CHECK=1
for p in mapa selector; do
  s1=; [[ $p == selector ]] && s1='300-306:1000:100:6'
  name=pant_${tag}_$p
  NWII_INPUT_SCRIPT_1=$s1 flock pc/.runlock env EVERY=1200 PROFILE=free RES=1.5789 pc/run.sh "$bin" "$name" 110 mp '0-100000:100:20:4' > /dev/null 2>&1
  L=pc/runs/$name/run.log
  printf '%-9s %s diff_lines=%s | %s | escenas: %s | %s\n' $p "$(grep -a FRAMECHECK $L | tail -1 | sed 's/.*checks=/checks=/; s/ (pass1.*//')" \
    "$(grep -a FRAMECHECK $L | grep -vc ' 0 pixels differ')" "$(grep -a RCOAL $L | tail -1 | grep -o 'check: .*')" \
    "$(grep -ao 'scene=[0-9]* ([A-Z_]*)' $L | awk '{print $2}' | uniq | tr '\n' ' ')" "$(tail -1 $L)"
done
echo FIN
