#!/usr/bin/env bash
# build_m4obj.sh <nx|pc> : (r35m4) objetos de m4n = los de G7 d (nx: dn con -DG7_NOIC; pc: d) + los 14 output_N.cpp
# que cambia la pasada S2 (tools/apply_s2.py), compilados con la MISMA orden que G7 (compile.cmd de r33g7/{dn/nx_obj,d/pc_obj})
# cambiando solo -I<export>. Correr con ~/nsmbw-port/scripts/lim; JOBS (5).
set -euo pipefail
W=$1; V=/home/djvemo/nsmbw-port/r9/r35m4; G=/home/djvemo/nsmbw-port/r9/r33g7
if [[ $W == nx ]]; then SRCCMD=$G/dn/nx_obj/compile.cmd; RUNDIR=/home/djvemo/nsmbw-port/r9/r14g5/nx/build; else SRCCMD=$G/d/pc_obj/compile.cmd; RUNDIR=/home/djvemo/nsmbw-port/r9/r14g5/export/build; fi
tmpl=$(cat $SRCCMD); tmpl=${tmpl//$G\/d\/export/$V\/export}
[[ $tmpl != *r33g7* ]] || { echo "quedan rutas de r33g7 en la orden" >&2; exit 2; }
O=$V/m4/${W}_obj
{ echo "rule cc"; echo "  command = cd $RUNDIR && nice -n 5 $tmpl"; echo "  description = $W \$in"
  for f in $V/diag/s2_d/output_*.cpp; do b=$(basename $f .cpp); echo "build $O/$b.o: cc $V/export/$b.cpp"; done; } > $O/build.ninja
echo "$tmpl" > $O/compile.cmd
ninja -f $O/build.ninja -j${JOBS:-5}
