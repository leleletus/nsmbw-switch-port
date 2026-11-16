#!/usr/bin/env bash
# det_efbvar.sh <bin_input_test> <escena: title|map|lvl11> <segundos>: A/B determinista de NWII_T_EFBVAR en PC
# a la escala de la Switch (1,5789x): dos corridas apagado (control: deben salir iguales entre si) y una
# encendido; compara los hash de los fotogramas volcados (r9/render/test/det.sh). Resultado por la salida.
set -u
bin=$1; sc=$2; secs=$3
R=/home/djvemo/nsmbw-port/r9/render
cd $R || exit 1
for v in a0:0 b0:0 c1:1; do
  n=t31det_${sc}_${v%%:*}
  EXTRA="NWII_T_EFBVAR=${v##*:}" RES=1.5789 SCALED=1 flock pc/.runlock test/det.sh "$bin" $n $sc $secs 10 > /dev/null 2>&1
  grep -a "TCSTAT2" test/runs/$n/run.log | tail -1 | cut -c1-200
done
python3 - "$sc" <<'PY'
import sys
sc=sys.argv[1]; R='/home/djvemo/nsmbw-port/r9/render/test/runs/'
def ld(d): return dict(l.split()[0::2] for l in open(R+d+'/hashes.txt'))
a,b,c=ld('t31det_%s_a0'%sc),ld('t31det_%s_b0'%sc),ld('t31det_%s_c1'%sc)
k=sorted(set(a)&set(b)&set(c))
print('fotogramas comparados',len(k))
print('control (apagado vs apagado): iguales',sum(a[x]==b[x] for x in k))
print('apagado vs NWII_T_EFBVAR=1: iguales',sum(a[x]==c[x] for x in k))
PY
