#!/usr/bin/env bash
# build_h.sh : variante h = d SIN la primera ronda de integracion de G6 (r20g6/e2/export_t = antes de apply_g6_inline)
#   export_t -> pasada G5/G6c de c3 (--orig/--leaves-from export_t) -> apply_g7 con los feats de d. Correr con lim.
set -euo pipefail
cd /home/djvemo/nsmbw-port/r9/r33g7
rm -rf h; mkdir -p h
cp -a --reflink=auto ../r20g6/e2/export_t h/export_pre; rm -rf h/export_pre/build
cp -a --reflink=auto h/export_pre h/export_g6
G5_JOBS=3 python3 tools/apply_g6c.py h/export_g6 --orig h/export_pre --eager --entry-fns --exclude tools/g5_exclude_g6b.txt \
  --inline-leaves --leaves-from h/export_pre --proto 3 > h/pass_g6c.log 2>&1
cp -a --reflink=auto h/export_g6 h/export
python3 tools/apply_g7.py h/export --feat stfs,psq,fp,poll,icm,disp,cr --jobs 4 > h/pass.log
echo "build_h ok"
