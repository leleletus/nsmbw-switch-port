#!/bin/bash
# instalar_m6q.sh: instala r9/r45p/bin/m6q.nro como NSMBWnx.nro en la SD (15-nov-2026), con respaldo local y en la SD.
# Se para en cuanto algo no cuadra. NO es un trabajo de la cola; ejecutarlo solo con la cola parada y la consola en hbmenu.
set -u
P=/home/djvemo/nsmbw-port; H=$P/tools/switchctl; IP=<IP_SWITCH>; FTP=ftp://$IP:5000
NEW=$P/r9/r45p/bin/m6q.nro; DIR=/switch/NSMBWnx; TS=$(date +%m%d_%H%M)
BK=$P/switch/real_builds/backup_NSMBWnx_antes_m6q_$TS.nro; OUT=$P/switch/real_builds/m6q_1116
die(){ echo "ALTO: $*"; python3 $H/switchctl.py click PLUS >/dev/null 2>&1; exit 1; }
[ "$(sha256sum < $NEW | cut -c1-16)" = "5c0a27bfa686b06e" ] || die "m6q.nro no tiene el sha esperado"
pgrep -f '[b]ench_title.sh' >/dev/null && die "hay una corrida en marcha"
touch $H/.ocupado; trap 'rm -f $H/.ocupado' EXIT
NAV_TMP=$OUT/.nav.jpg python3 -W ignore $H/nav.py ftpd || die "no pude abrir ftpd"
echo "== listado antes"; curl -s --max-time 30 "$FTP$DIR/" | grep -i -E 'nro|toml' 
RS=$(curl -s --max-time 30 "$FTP$DIR/" | awk '$NF=="NSMBWnx.nro"{print $5}')
[ -n "$RS" ] || die "no encuentro NSMBWnx.nro en la SD"
echo "== bajando el actual ($RS bytes)"; curl -s --max-time 600 -o "$BK" "$FTP$DIR/NSMBWnx.nro" || die "fallo al bajar el actual"
[ "$(stat -c %s "$BK")" = "$RS" ] || die "el respaldo local no mide lo mismo que el de la SD"
echo "respaldo local: $BK sha $(sha256sum < "$BK" | cut -c1-16)"
echo "== SD: anterior <- actual"
curl -s --max-time 30 -Q "DELE $DIR/NSMBWnx_anterior.nro" "$FTP/" -o /dev/null   # puede no existir
curl -s --max-time 30 -Q "RNFR $DIR/NSMBWnx.nro" -Q "RNTO $DIR/NSMBWnx_anterior.nro" "$FTP/" -o /dev/null || die "no pude renombrar el actual a _anterior"
AS=$(curl -s --max-time 30 "$FTP$DIR/" | awk '$NF=="NSMBWnx_anterior.nro"{print $5}'); [ "$AS" = "$RS" ] || die "NSMBWnx_anterior.nro no mide $RS (mide '$AS')"
echo "== subiendo m6q"; curl -s --max-time 900 -T "$NEW" "$FTP$DIR/NSMBWnx.nro" || die "fallo al subir (el anterior sigue en la SD como NSMBWnx_anterior.nro)"
NS=$(curl -s --max-time 30 "$FTP$DIR/" | awk '$NF=="NSMBWnx.nro"{print $5}'); [ "$NS" = "$(stat -c %s $NEW)" ] || die "tamaño subido $NS != $(stat -c %s $NEW)"
echo "== comprobando contenido subido"; [ "$(curl -s --max-time 600 "$FTP$DIR/NSMBWnx.nro" | sha256sum | cut -c1-16)" = "5c0a27bfa686b06e" ] || die "el NRO en la SD no coincide con m6q"
echo "== debug.toml"; curl -s --max-time 60 -T $H/debug.toml.maestro "$FTP$DIR/debug.toml"; sleep 2
cmp -s <(curl -s --max-time 60 "$FTP$DIR/debug.toml") $H/debug.toml.maestro || die "debug.toml de la SD no es el maestro"
cp $NEW $OUT/NSMBWnx.nro; cp $P/r9/r45p/bin/m6q.elf $OUT/ 2>/dev/null; cp $H/debug.toml.maestro $OUT/debug.toml; sha256sum $OUT/NSMBWnx.nro > $OUT/sha256.txt
echo "== listado despues"; curl -s --max-time 30 "$FTP$DIR/" | grep -i -E 'nro|toml'
python3 $H/switchctl.py click PLUS >/dev/null; sleep 4
python3 $H/switchctl.py shot $OUT/tras_instalar.jpg >/dev/null 2>&1; echo "pantalla: $(python3 $H/estado.py $OUT/tras_instalar.jpg)"
echo "INSTALADO m6q $(date '+%F %T')"
