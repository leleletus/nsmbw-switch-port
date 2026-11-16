#!/usr/bin/env bash
# [r41m] audio_m6e.sh: audio PC de m6e (título + demos de atracción 100 s, tiempo real) con el arreglo de la lista AX
# y con NWII_C_AXLIST_OFF=1; comparación con r35m4/tools/wavq.py -> test/audio_m6e.txt
cd /home/djvemo/nsmbw-port/r9/r41m
flock pc/.runlock tools/audio_run.sh bin/NSMBW_m6e aud_m6e_off 100 NWII_REALTIME=1 NWII_C_AXLIST_OFF=1 > /dev/null 2>&1
flock pc/.runlock tools/audio_run.sh bin/NSMBW_m6e aud_m6e_on 100 NWII_REALTIME=1 > /dev/null 2>&1
python3 ../r35m4/tools/wavq.py pc/runs/aud_m6e_off/audio.wav pc/runs/aud_m6e_on/audio.wav > test/audio_m6e.txt 2>&1
echo FIN >> test/audio_m6e.txt
