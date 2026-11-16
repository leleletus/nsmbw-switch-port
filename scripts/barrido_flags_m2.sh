#!/usr/bin/env bash
# barrido_flags_m2.sh: no-AI sweep of compiler flags for the generated code of m2 (no PGO), built on the other laptop,
# then measured on the console against m2 with the fixed list. Variants (GEN_FLAGS after the base -O2):
#   f1 = -O3 · f2 = -Os · f3 = -O2 -fira-loop-pressure -fira-hoist-pressure
# Read: cd switch/real_logs && python3 ../../tools/switchctl/por_escena.py bench_Q02_m2_control_* bench_F0*
set -uo pipefail
P=$HOME/nsmbw-port; R=$P/scripts/remoto.sh; C=$P/r9/r17pgo; Q=$P/tools/switchctl
echo "=== inicio $(date "+%F %T")"
$R run barrido_flags 'cd r9/r17pgo && set -a && source conf/m2.env && set +a && for v in "f1:-O3" "f2:-Os" "f3:-O2 -fira-loop-pressure -fira-hoist-pressure"; do n=m2_${v%%:*}; echo "== $n: ${v#*:}"; PGO=off GEN_FLAGS="${v#*:}" JOBS=11 ./build_pgo.sh $n || echo "VARIANTE $n FALLO"; ls -la bin/$n.nro; done'
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
for v in f1 f2 f3; do
  until ssh nsmbw-build "test -f nsmbw-port/r9/r17pgo/bin/m2_$v.nro -a nsmbw-port/r9/r17pgo/bin/m2_$v.nro -nt nsmbw-port/remoto/barrido_flags/t0" 2>/dev/null; do
    ssh nsmbw-build "test -f nsmbw-port/remoto/barrido_flags/rc" 2>/dev/null && ! ssh nsmbw-build "test -f nsmbw-port/r9/r17pgo/bin/m2_$v.nro" && { echo "FALLÓ: no hay m2_$v.nro"; continue 2; }
    sleep 60
  done
  sleep 20
  rsync -a "nsmbw-build:nsmbw-port/r9/r17pgo/bin/m2_$v.nro" "nsmbw-build:nsmbw-port/r9/r17pgo/bin/m2_$v.elf" "$C/bin/" || { echo "FALLÓ la copia de $v"; continue; }
  (cd "$Q" && BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L" ./cola.sh add "$C/bin/m2_$v.nro" F0${v#f}_m2_$v 400
   systemctl --user is-active -q nsmbw-cola || systemd-run --user --unit=nsmbw-cola --collect --quiet \
     -p StandardOutput=append:$Q/cola_run.log -p StandardError=append:$Q/cola_run.log $Q/cola.sh run)
  echo "$(date '+%F %T') $v en la cola"
done
echo "$(date '+%F %T') LISTO barrido"
