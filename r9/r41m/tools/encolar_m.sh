#!/usr/bin/env bash
# [r41m] encolar_m.sh <nro> <tag> <n0> [variantes="off on"]: corridas del frente M en la cola de la consola, MISMO
# binario, contiguas, lista fija. off = entorno de C02/Z21 (todo apagado); on = entorno de D02 (NWII_V_WAKE=1,
# NWII_V_LATE=8, NWII_D_DROPFAST=1), 400 s; largo = on durante 1500 s (capturas cada 20 s). Nombres M<nn>_<tag>_<v>.
set -eu
nro=$1; p=$2; n=$3
Q=/home/djvemo/nsmbw-port/tools/switchctl
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
ON=';NWII_V_WAKE="1";NWII_V_LATE="8";NWII_D_DROPFAST="1"'
# m6b: + R2 «todo11» (los 8 de r2g + TEXMEMO + CULL=2 + XDUBO; = R2_06)
R2=';NWII_R2_LATEFREE="1";NWII_R2_CLEARTEX="1";NWII_R2_COPYFBO="1";NWII_R2_LAZYNAT="1";NWII_R2_COPYUNI="1";NWII_R2_COPYST="1";NWII_R2_LAZYEFB="1";NWII_R2_PREHASH="1";NWII_R2_TEXMEMO="1";NWII_R_CULL="2";NWII_R2_XDUBO="1"'
add() { BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=$3 DEBUG_EXTRA="$L$2" $Q/cola.sh add "$nro" "$(printf M%02d $n)_${p}_$1" $4; n=$((n + 1)); }
for v in ${4:-off on largo}; do
  case $v in
    off)   add off '' 10 400 ;;
    on)    add on "$ON" 10 400 ;;
    largo) add on_largo "$ON" 20 1500 ;;
    r2)    add on "$ON$R2" 10 400 ;;
    def)     add def "" 10 400 ;;                              # m6e: SIN interruptores (valores por defecto)
    def_largo) add def_largo "" 20 1500 ;;
    merge)   add merge ';NWII_D_MERGE="1"' 10 400 ;;                       # m6h
    agua)    add agua ';NWII_W_AGUA="1";NWII_W_WGP="1"' 10 400 ;;
    todo)    add todo ';NWII_D_MERGE="1";NWII_W_AGUA="1";NWII_W_WGP="1"' 10 400 ;;
    todo_logbg) add todo_logbg ';NWII_D_MERGE="1";NWII_W_AGUA="1";NWII_W_WGP="1";NWII_M_LOGBG="1"' 10 400 ;;
    todo_audio) add todo_audio ';NWII_D_MERGE="1";NWII_W_AGUA="1";NWII_W_WGP="1";NWII_A_MIX="1";NWII_A_NOSPIN="1";NWII_A_REQSPIN="200"' 10 400 ;;
    kernel4) add kernel4 ';NWII_AXFX_KERNEL="4"' 10 400 ;;              # m6i
    noprehash) add noprehash ';NWII_R2_PREHASH_OFF="1"' 10 400 ;;   # m6g: por defecto salvo PREHASH
    logsync) add logsync "$ON$R2" 10 400 ;;                  # m6c: registro como siempre (+ medida [LOGF])
    logbg)   add logbg "$ON$R2;NWII_M_LOGBG=\"1\"" 10 400 ;;   # m6c: registro en hilo de fondo
  esac
done
$Q/cola.sh list | grep '\.job$' | tail -16
