#!/usr/bin/env bash
# verif_omen.sh <bin_pc> <etiqueta> [ENV=val ...]   — verificación de PC estándar en la OMEN (ssh nsmbw-build).
#   <bin_pc>: binario de PC SIN _input_test (p. ej. r9/r45p/bin/NSMBW_m6q); hace falta también <bin_pc>_input_test.
#   1) sincroniza por la LAN lo mínimo (juego, bin_usa, hang_summary.py, arnés de r41m/r45p con rutas a r9/verif_omen,
#      limc de la Omen con 6 hilos: CPUs 2-7) y los dos binarios a r9/verif_omen/bin/<etiqueta>/;
#   2) lanza allí omen_arnes/omen_verif.sh (= r45p/tools/verif_def.sh: FC 5 demos + checks 1x + mapa/selector con
#      DLMERGE_CHECK/MERGE_CHECK/DROPFAST_CHECK, estrés (c) NEST×150 s + NESTL×150 s lento), desacoplado de ssh;
#   3) espera (sondeo cada 60 s) y trae los resultados a r9/tools_comunes/omen_runs/<etiqueta>/.
# GL en la Omen: EGL de NVIDIA (GTX 1070) + capa omen_arnes/egl46.c (LD_PRELOAD; sube a 4.6 el contexto 3.2 que pide el
#   port, ver el comentario de ese archivo); VO_GL=llvmpipe para Mesa por software (lento).
# Variables: NEST (2), NESTL (1), VO_CPUS (2-7), VO_GL (nvidia), VO_SOLO_SYNC=1 (solo sincronizar), VO_ESPERA=0 (lanzar y salir; luego: verif_omen.sh --traer <etiqueta>).
# Duración típica: ~30 min. Las corridas de la Omen se encolan entre sí con flock (pc/.runlock), como aquí.
set -u
H=nsmbw-build
P=/home/djvemo/nsmbw-port
RV=$P/r9/verif_omen                     # raíz del arnés en la Omen
L=$P/r9/tools_comunes                   # aquí
G=$L/omen_arnes/gen                     # copia local de lo que se manda (regenerada en cada llamada)

traer() {   # traer <etiqueta>
  local T=$1 O=$L/omen_runs/$1
  mkdir -p $O/test $O/runs
  rsync -a "$H:$RV/estado/$T.info" "$H:$RV/estado/$T.log" $O/ 2>/dev/null
  rsync -a --include="*_${T}.txt" --include="*_${T}_lento.txt" --exclude='*' "$H:$RV/r45p/test/" $O/test/
  rsync -a --exclude='work_*' --include="fc_${T}_*/***" --include="chk_${T}_*/***" --include="pant_${T}_*/***" \
    --include="${T}_[0-9]*/***" --include="${T}_lento_[0-9]*/***" --exclude='*' "$H:$RV/r41m/pc/runs/" $O/runs/
  echo "resultados en $O"
}

if [[ ${1:-} == --traer ]]; then traer "${2:?etiqueta}"; exit; fi
B=$(realpath "${1:?bin_pc}"); T=${2:?etiqueta}; shift 2
[[ $T =~ ^[A-Za-z0-9_]+$ ]] || { echo "etiqueta: solo letras, cifras y _" >&2; exit 2; }
[[ -x $B && -x ${B}_input_test ]] || { echo "falta $B o ${B}_input_test" >&2; exit 2; }
for kv in "$@"; do [[ $kv == *=* ]] || { echo "argumento no ENV=val: $kv" >&2; exit 2; }; done
ssh -o ConnectTimeout=10 $H true || { echo "la Omen no responde" >&2; exit 1; }

# --- 1) arnés: copias de los guiones vivos con las rutas de r9 cambiadas a r9/verif_omen (el juego, bin_usa y
#        cpu/tools quedan en su misma ruta absoluta en la Omen) ---
mkdir -p $G/r41m/pc $G/r41m/test $G/r41m/tools $G/r45p/tools
re='s#/home/djvemo/nsmbw-port/r9/#/home/djvemo/nsmbw-port/r9/verif_omen/#g'
for f in r41m/pc/run.sh r41m/test/demos_fc.sh r41m/test/checks.sh r41m/tools/pcrun.sh \
         r45p/tools/pc_verif_mrt.sh r45p/tools/pc_pant_fc.sh r45p/tools/estres_p.sh; do
  sed "$re" $P/r9/$f > $G/$f.tmp && chmod +x $G/$f.tmp && { cmp -s $G/$f.tmp $G/$f && rm -f $G/$f.tmp || mv $G/$f.tmp $G/$f; }
done
cp -a $L/omen_arnes/limc $G/r41m/pc/limc
cp -a $L/omen_arnes/omen_verif.sh $G/omen_verif.sh
mkdir -p $G/lib; cp -a $L/omen_arnes/egl46.c $G/lib/egl46.c
rsync -a $P/r9/r41m/pc/{default.toml,mp.toml,shadercache.tpl,nand_tpl_mp,.runlock} $G/r41m/pc/
ssh $H mkdir -p $P/game $P/cpu/tools $P/bin_usa $RV/bin/$T $RV/estado $RV/r45p/test $RV/r41m/pc/runs
# juego (339 MB la 1.ª vez), REL de bin_usa (9 MB), hang_summary.py y arnés (4 MB): sin --delete (nunca borra allí)
rsync -a $P/game/SMNE01 $H:$P/game/
rsync -a $P/bin_usa/ $H:$P/bin_usa/
rsync -a $P/cpu/tools/hang_summary.py $H:$P/cpu/tools/
rsync -a $G/ $H:$RV/
ssh $H bash -s <<EOF
cd $RV/lib && { [[ libegl46.so -nt egl46.c ]] || gcc -O2 -shared -fPIC -o libegl46.so egl46.c -ldl; }
EOF
# binarios (2 × ~100 MB; comprimidos por la red)
rsync -az "$B" "${B}_input_test" $H:$RV/bin/$T/
RB=$RV/bin/$T/$(basename "$B")
[[ -n ${VO_SOLO_SYNC:-} ]] && { echo "sincronizado (sin lanzar): $RB"; exit 0; }

# --- 2) lanzar desacoplado ---
ssh $H rm -f $RV/estado/$T.hecho
args=$(printf '%q ' "$T" "$RB" "$@")
ssh $H bash -s <<EOF
cd $RV && NEST=${NEST:-2} NESTL=${NESTL:-1} VO_CPUS=${VO_CPUS:-2-7} VO_GL=${VO_GL:-nvidia} nohup setsid bash omen_verif.sh $args > estado/$T.log 2>&1 < /dev/null &
EOF
echo "$(date '+%T') lanzada en la Omen: $T ($(basename "$B")) env: $*"
[[ ${VO_ESPERA:-1} == 0 ]] && { echo "para traer: $0 --traer $T"; exit 0; }

# --- 3) esperar y traer ---
t0=$(date +%s)
until ssh -o ConnectTimeout=10 $H test -e $RV/estado/$T.hecho 2>/dev/null; do sleep 60; done
echo "$(date '+%T') terminada en $(( ($(date +%s) - t0) / 60 )) min"
traer $T
O=$L/omen_runs/$T
cat $O/$T.info
for f in $O/test/{fc,checks,pant,estres}_$T.txt $O/test/estres_${T}_lento.txt $O/test/mergechk_$T.txt; do
  [[ -f $f ]] && { echo "## $(basename $f)"; cat $f; }
done
