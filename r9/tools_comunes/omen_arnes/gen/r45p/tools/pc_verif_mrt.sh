#!/usr/bin/env bash
# [r45p] pc_verif_mrt.sh <bin_input_test> <tag> [ENV=val ...]: verificación de imagen de PC de una variante:
#  FC de las 5 demos a 1.5789x (r41m/test/demos_fc.sh), checks 1x (r41m/test/checks.sh RESS=1) y FRAMECHECK=30 en
#  mapa y selector a 1.5789x (tools/pc_pant_fc.sh). Resultados en r9/r45p/test/{fc,checks,pant}_<tag>.txt
bin=$(realpath "$1"); cd /home/djvemo/nsmbw-port/r9/verif_omen/r41m; tag=$2; shift 2
for kv in "$@"; do export "$kv"; done
O=/home/djvemo/nsmbw-port/r9/verif_omen/r45p/test; mkdir -p $O
{ echo "env: $*"; test/demos_fc.sh "$bin" $tag 100; echo FIN; } > $O/fc_$tag.txt 2>&1
{ echo "env: $*"; RESS=1 test/checks.sh "$bin" $tag 120; echo FIN; } > $O/checks_$tag.txt 2>&1
/home/djvemo/nsmbw-port/r9/verif_omen/r45p/tools/pc_pant_fc.sh "$bin" $tag > $O/pant_$tag.txt 2>&1
