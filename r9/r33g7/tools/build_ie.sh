#!/usr/bin/env bash
# build_ie.sh <variante i> <variante e> <sitios.tsv> : rehace la cadena de la variante "inline":
#   copia de r20g6/e2/export_pre -> apply_g7_inline.py apply -> <i>/export_pre -> pasada G5/G6c de c3 (mismos
#   argumentos que r30g6c3/LEEME, con --orig/--leaves-from = <i>/export_pre) -> <i>/export -> apply_g7.py con los
#   feats de d -> <e>/export.   Correr con ~/nsmbw-port/scripts/lim.
set -euo pipefail
cd /home/djvemo/nsmbw-port/r9/r33g7
I=$1; E=$2; SITES=$3; FEATS=${FEATS:-stfs,psq,fp,poll,icm,disp,cr}
rm -rf $I $E; mkdir -p $I $E
cp -a --reflink=auto ../r20g6/e2/export_pre $I/export_pre; rm -rf $I/export_pre/build
python3 tools/apply_g7_inline.py apply $I/export_pre --sites $SITES > $I/inline.log
cp -a --reflink=auto $I/export_pre $I/export
G5_JOBS=3 python3 tools/apply_g6c.py $I/export --orig $I/export_pre --eager --entry-fns --exclude tools/g5_exclude_g6b.txt \
  --inline-leaves --leaves-from $I/export_pre --proto 3 > $I/pass.log 2>&1
cp -a --reflink=auto $I/export $E/export
python3 tools/apply_g7.py $E/export --feat $FEATS --jobs 4 > $E/pass.log
echo "build_ie $I $E ok"
