#!/usr/bin/env bash
# aud.sh <n> <tag> [env...]: audio de PC de bin/NSMBW_<n> con y sin el entorno -> test/aud_<tag>.txt
# (1) determinista (DET=1: reloj virtual, AX en linea) con S2 APAGADO (con S2 el reloj de pared manda y no es
#     determinista): 120 s; cmp byte a byte del tramo comun (la duracion depende del tiempo de pared).
# (2) tiempo real con S2 (como en la consola), titulo + demos de atraccion, 100 s: control x2 y variante;
#     r35m4/tools/wavq.py control1-control2 (ruido) y control1-variante.
cd /home/djvemo/nsmbw-port/r9/r43a; N=$1; T=$2; shift 2
O=test/aud_$T.txt; echo "env: $*" > $O
# DET no es determinista del todo (control contra control se separa a los ~18 s, en el titulo): se mide el primer
# byte distinto de control-control y de control-variante (datos, sin la cabecera WAV); la exactitud del mezclador
# la da NWII_AX_VERIFY (lista a lista contra la mezcla de referencia).
for r in r r2; do flock pc/.runlock env DET=1 tools/audio_run.sh bin/NSMBW_$N ad_${T}_$r 60 NWII_S2_OFF=1 > /dev/null 2>&1; done
flock pc/.runlock env DET=1 tools/audio_run.sh bin/NSMBW_$N ad_${T}_v 60 NWII_S2_OFF=1 "$@" > /dev/null 2>&1
fd() { local a=$1 b=$2 m; m=$(( $(stat -c %s $a) < $(stat -c %s $b) ? $(stat -c %s $a) : $(stat -c %s $b) ))
  local x; x=$(cmp -i 44 -n $((m-44)) $a $b 2>&1 | grep -o 'byte [0-9]*' | awk '{print $2}')
  if [[ -z $x ]]; then echo "IDENTICO en $((m-44)) bytes ($(( (m-44)/128000 )) s)"; else echo "primer byte distinto $x ($(echo "scale=2; $x/128000" | bc) s) de $((m-44))"; fi; }
echo "DET sin S2: control-control: $(fd pc/runs/ad_${T}_r/audio.wav pc/runs/ad_${T}_r2/audio.wav) | control-variante: $(fd pc/runs/ad_${T}_r/audio.wav pc/runs/ad_${T}_v/audio.wav)" >> $O
flock pc/.runlock tools/audio_run.sh bin/NSMBW_$N ar_${T}_c1 100 NWII_REALTIME=1 > /dev/null 2>&1
flock pc/.runlock tools/audio_run.sh bin/NSMBW_$N ar_${T}_c2 100 NWII_REALTIME=1 > /dev/null 2>&1
flock pc/.runlock tools/audio_run.sh bin/NSMBW_$N ar_${T}_v 100 NWII_REALTIME=1 "$@" > /dev/null 2>&1
echo "== tiempo real, control1 vs control2 (ruido)" >> $O
python3 ../r35m4/tools/wavq.py pc/runs/ar_${T}_c1/audio.wav pc/runs/ar_${T}_c2/audio.wav >> $O 2>&1
echo "== tiempo real, control1 vs variante" >> $O
python3 ../r35m4/tools/wavq.py pc/runs/ar_${T}_c1/audio.wav pc/runs/ar_${T}_v/audio.wav >> $O 2>&1
for r in c1 c2 v; do echo "$r: $(grep -a '^\[S2\] ACTIVO' pc/runs/ar_${T}_$r/run.log | tail -1 | cut -c1-60) | $(grep -a '^\[A43\]' pc/runs/ar_${T}_$r/run.log | tail -1 | cut -c1-120)"; done >> $O
echo FIN >> $O
