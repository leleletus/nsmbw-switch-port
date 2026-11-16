#!/usr/bin/env bash
# [verif_omen] omen_verif.sh <etiqueta> <bin (sin _input_test)> [ENV=val ...]   — CORRE EN LA OMEN (lo lanza verif_omen.sh).
# Equivale a r45p/tools/verif_def.sh con las rutas de la copia de la Omen (r9/verif_omen):
#   pc_verif_mrt.sh (FC 5 demos 1.5789x + checks 1x title/map/walk/lvl11 + mapa/selector) con las comprobaciones estándar
#   NWII_P_DLMERGE_CHECK=10 NWII_D_MERGE_CHECK=10 NWII_D_DROPFAST_CHECK=1 + las ENV extra;
#   estrés (c) NEST×150 s normal y NESTL×150 s con render y trabajador frenados (+ ENV extra); resumen DLMERGE.
# Resultados: r45p/test/*_<etiqueta>*.txt, r41m/pc/runs/{fc,chk,pant}_<etiqueta>_*, <etiqueta>_<i>, <etiqueta>_lento_<i>;
# estado/<etiqueta>.info (tiempos, carga, GPU, sha256) y estado/<etiqueta>.hecho al acabar.
RV=/home/djvemo/nsmbw-port/r9/verif_omen
T=${1:?etiqueta}; B=${2:?bin}; shift 2
cd $RV || exit 1
# GL sin pantalla (SDL offscreen = EGL): VO_GL=nvidia (por defecto; GTX 1070 + capa lib/libegl46.so que sube el
# contexto 3.2 que pide el port a 4.6, ver omen_arnes/egl46.c) o VO_GL=llvmpipe (Mesa por software, ~30 fps a 1.5789x)
case ${VO_GL:=nvidia} in
  nvidia)   export LD_PRELOAD=$RV/lib/libegl46.so ;;
  llvmpipe) export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json ;;
  *) echo "VO_GL=$VO_GL desconocido" >&2; exit 2 ;;
esac
I=estado/$T.info
t0=$(date +%s)
{ echo "etiqueta $T | bin $B | env extra: $*"
  echo "inicio $(date '+%F %T') | carga $(cut -d' ' -f1-3 /proc/loadavg) | CPUs ${VO_CPUS:-2-7} | GL $VO_GL | NEST ${NEST:-2} NESTL ${NESTL:-1}"
  sha256sum "$B" "${B}_input_test"; } > $I
r45p/tools/pc_verif_mrt.sh "${B}_input_test" $T NWII_P_DLMERGE_CHECK=10 NWII_D_MERGE_CHECK=10 NWII_D_DROPFAST_CHECK=1 "$@"
echo "fin FC+checks+pant $(date '+%F %T') ($(( $(date +%s) - t0 )) s) | carga $(cut -d' ' -f1-3 /proc/loadavg)" >> $I
r45p/tools/estres_p.sh "$B" ${T} ${NEST:-2} "$@"
(( ${NESTL:-1} > 0 )) && r45p/tools/estres_p.sh "$B" ${T}_lento ${NESTL:-1} NWII_R11_SLOW_RENDER_US=15000 NWII_D_SLOWWORKER=3 "$@"
for f in $RV/r41m/pc/runs/{fc,chk,pant}_${T}_*; do
  [[ -f $f/run.log ]] || continue
  echo "$(basename $f): $(grep -a '^\[D40MERGE_CHECK\]' $f/run.log | tail -1 | cut -c1-70) | distintas $(grep -a '^\[D40MERGE_CHECK\]' $f/run.log | grep -vc ' 0 pixels differ') | $(grep -a '^\[M6\]' $f/run.log | head -1 | grep -o 'NWII_P_[A-Z]*=[0-9]*' | tr '\n' ' ')"
done > r45p/test/mergechk_$T.txt
echo HECHO >> r45p/test/mergechk_$T.txt
{ echo "fin $(date '+%F %T') | total $(( $(date +%s) - t0 )) s | carga $(cut -d' ' -f1-3 /proc/loadavg)"
  grep -a -m1 '^\[GL\] [A-Z]' r41m/pc/runs/fc_${T}_desierto4j/run.log; } >> $I
touch estado/$T.hecho
