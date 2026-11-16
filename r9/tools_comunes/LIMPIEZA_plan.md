# Plan de limpieza (solo lectura; NADA se ha borrado)

Generado 15-nov-2026 ~20:50. Medido con nice/ionice. GB = 1e9 bytes de lo que borraría cada comando, sin duplicados entre entradas (las entradas de patrón `find` cuentan solo lo que no cubre otra fila). Entradas menores de 5 MB omitidas.

Resumen: NIVEL 1 = 98.1 GB (114 entradas) | NIVEL 2 (tabla) = 31.9 GB, más los candidatos N2 de la lista siguiente | proyecto 281 GB.

## Cómo se decidió
- Se leyeron los *.sh *.py *.md *.cmake CMakeLists.txt de los frentes vivos (116 ficheros: r41m r40d r42k r43a r44w r35m4/tools r17pgo/*.sh r33g7 r39r2/tools r38c/tools r37v r36r/tools r31t/tools r32dx/tools tools_comunes tools/switchctl scripts) y se sacaron las rutas r9/ r8/ perf/ render/ recomp/ switch/ ... mencionadas, más variables ($M=r24m2, $G5=r14g5, $G=r20g6, $C3=r30g6c3, $R=r9/render, $T, $V).
- NIVEL 1 = frentes que NINGUN guion vivo menciona (r10a r10b r10c r11a r11b r11base r11c r11d r12a r12fm r14r5 r16a2 r16res r16w r19n r21s r22r7 r23g6c r26x1 r27x2 r28d1 r29m3 combo cpucode guestloop) y las etapas pre-r9 (perf r8 render cpu glitch mp scenes oracle investigate) solo en ejecutables/objetos/capturas.
- Nunca se propone tocar *.md *.patch *.py *.sh, tools/, profiles/, *.gcda, rutas con save/respaldo, nand*, game, status/ESTADO.md, tools/switchctl, ni las 10 carpetas de switch/real_logs de verif_refs.txt. Los `find ... -delete` excluyen nombres con save y _deps. Antes de ejecutar, cambia -delete por -print y revisa.
- Ningun fichero de N1 se modifico en las ultimas 24 h (lo mas reciente tiene ~130 h). OJO: ahora mismo hay `ssh nsmbw-build .../scripts/lim bash -s` en marcha y /tmp/nsmbw-lim/build0.lock: compilaciones remotas en curso (no se entro por ssh). Antes de borrar, comprobar que no hay compilacion activa y que nsmbw-build no lee ningun r9/<frente> de la lista.
- Limite: los guiones pueden construir rutas con variables que grep no resuelve; por eso lo dudoso va a nivel 2.

## NIVEL 1 - borrable ya (regenerable, sin referencias vivas)
| GB | ruta | contenido | comando |
|---:|---|---|---|
| 5.87 | r8 | objetos *.o/*.a/*.elf/*.nro restantes de la etapa (no toca perf/r6/bin, _deps ni docs) (1529 ficheros) | `find r8 -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 5.78 | r9/r22r7/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (54 ficheros) | `rm -rf r9/r22r7/bin` |
| 5.16 | r9/r19n/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (48 ficheros) | `rm -rf r9/r19n/bin` |
| 4.48 | r9/r14r5/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (45 ficheros) | `rm -rf r9/r14r5/bin` |
| 3.98 | r9/r26x1/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (37 ficheros) | `rm -rf r9/r26x1/bin` |
| 3.62 | render/bin | ejecutables / objetos del port antiguo (etapa pre-r9) (26 ficheros) | `rm -rf render/bin` |
| 3.46 | r9/r19n/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (110 ficheros) | `find r9/r19n/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 3.01 | perf | objetos *.o/*.a/*.elf/*.nro restantes de la etapa (no toca perf/r6/bin, _deps ni docs) (1097 ficheros) | `find perf -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 2.51 | r9/r27x2/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (24 ficheros) | `rm -rf r9/r27x2/bin` |
| 2.44 | r9/r11a/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (17 ficheros) | `rm -rf r9/r11a/bin` |
| 2.38 | r9/r22r7/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (87 ficheros) | `find r9/r22r7/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 2.35 | r9/r11b/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (16 ficheros) | `rm -rf r9/r11b/bin` |
| 2.12 | r9/r27x2/runs | FIFOs de traza (7 ficheros) | `find r9/r27x2/runs -type f -name '*.fifo' -delete` |
| 2.04 | r9/r11d/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (14 ficheros) | `rm -rf r9/r11d/bin` |
| 1.96 | r9/r26x1/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (91 ficheros) | `find r9/r26x1/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 1.92 | render | objetos *.o/*.a/*.elf/*.nro restantes de la etapa (no toca perf/r6/bin, _deps ni docs) (677 ficheros) | `find render -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 1.82 | r9/cpucode/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (13 ficheros) | `rm -rf r9/cpucode/bin` |
| 1.79 | r9/r21s/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (19 ficheros) | `rm -rf r9/r21s/bin` |
| 1.62 | r9/r10c/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (12 ficheros) | `rm -rf r9/r10c/bin` |
| 1.47 | r9/r16w/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (15 ficheros) | `rm -rf r9/r16w/bin` |
| 1.43 | investigate | objetos *.o/*.a/*.elf/*.nro restantes de la etapa (no toca perf/r6/bin, _deps ni docs) (370 ficheros) | `find investigate -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 1.42 | r9/guestloop/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (10 ficheros) | `rm -rf r9/guestloop/bin` |
| 1.31 | r9/r15dk/m1dk_s | objetos/ELF/NRO/map (frente viejo, sin referencias) (73 ficheros) | `find r9/r15dk/m1dk_s -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 1.31 | r9/r15dk/m1dk | objetos/ELF/NRO/map (frente viejo, sin referencias) (72 ficheros) | `find r9/r15dk/m1dk -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 1.17 | r9/r28d1/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (11 ficheros) | `rm -rf r9/r28d1/bin` |
| 1.14 | r9/r27x2/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (90 ficheros) | `find r9/r27x2/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 1.08 | r9/r16w/combo_r5 | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (116 ficheros) | `find r9/r16w/combo_r5 -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 1.00 | r9/r23g6c/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (10 ficheros) | `rm -rf r9/r23g6c/bin` |
| 0.98 | r9/r16res/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (10 ficheros) | `rm -rf r9/r16res/bin` |
| 0.98 | r9/r16a2/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (10 ficheros) | `rm -rf r9/r16a2/bin` |
| 0.96 | r9/r21s/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (68 ficheros) | `find r9/r21s/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.96 | cpu/bin | ejecutables / objetos del port antiguo (etapa pre-r9) (7 ficheros) | `rm -rf cpu/bin` |
| 0.88 | r9/r15dk/r5dk | objetos/ELF/NRO/map (frente viejo, sin referencias) (66 ficheros) | `find r9/r15dk/r5dk -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.84 | glitch/runs | capturas/trazas de corridas antiguas (no referencia) (1249 ficheros) | `rm -rf glitch/runs` |
| 0.82 | r9/r10a/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (6 ficheros) | `rm -rf r9/r10a/bin` |
| 0.80 | r9/r16a2/runs | wav de corridas viejas (12 ficheros) | `find r9/r16a2/runs -type f -name '*.wav' -delete` |
| 0.71 | r9/r12a/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (5 ficheros) | `rm -rf r9/r12a/bin` |
| 0.71 | r9/r11base/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (5 ficheros) | `rm -rf r9/r11base/bin` |
| 0.69 | r9/r28d1/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (70 ficheros) | `find r9/r28d1/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.66 | perf/bin | ejecutables / objetos del port antiguo (etapa pre-r9) (5 ficheros) | `rm -rf perf/bin` |
| 0.63 | r9/guestloop/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (142 ficheros) | `find r9/guestloop/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.60 | r9/r10c/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (147 ficheros) | `find r9/r10c/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.60 | r9/r10a/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (146 ficheros) | `find r9/r10a/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.59 | perf/NSMBW.a3b_base | ejecutable base antiguo (fichero) (1 ficheros) | `rm -f perf/NSMBW.a3b_base` |
| 0.59 | r9/cpucode/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (142 ficheros) | `find r9/cpucode/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.58 | r9/r16res/r5dkres | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (115 ficheros) | `find r9/r16res/r5dkres -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.56 | scenes/bin | ejecutables / objetos del port antiguo (etapa pre-r9) (4 ficheros) | `rm -rf scenes/bin` |
| 0.56 | r9/r10b/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (4 ficheros) | `rm -rf r9/r10b/bin` |
| 0.51 | r9/r16a2/combo_r5 | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (112 ficheros) | `find r9/r16a2/combo_r5 -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.50 | r9/r14r5/mdi | objetos/ELF/NRO/map (frente viejo, sin referencias) (60 ficheros) | `find r9/r14r5/mdi -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.47 | r9/r23g6c/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (4 ficheros) | `find r9/r23g6c/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.46 | r9/r29m3/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (5 ficheros) | `rm -rf r9/r29m3/bin` |
| 0.46 | r9/r23g6c/difftest | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (354 ficheros) | `find r9/r23g6c/difftest -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.45 | r9/r20g6/nx_link | objetos/ELF/NRO/map (frente viejo, sin referencias) (4 ficheros) | `find r9/r20g6/nx_link -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.45 | cpu/runs | capturas/trazas de corridas antiguas (no referencia) (1237 ficheros) | `rm -rf cpu/runs` |
| 0.44 | r9/r12fm/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (147 ficheros) | `find r9/r12fm/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.44 | r9/r11c/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (2 ficheros) | `rm -rf r9/r11c/bin` |
| 0.42 | mp/bin | ejecutables / objetos del port antiguo (etapa pre-r9) (3 ficheros) | `rm -rf mp/bin` |
| 0.41 | render/runs | capturas/trazas de corridas antiguas (no referencia) (1287 ficheros) | `rm -rf render/runs` |
| 0.39 | r9/r19n/difftest | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (179 ficheros) | `find r9/r19n/difftest -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.39 | r9/r11c/export_prof | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (140 ficheros) | `find r9/r11c/export_prof -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.39 | r9/r12fm/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (4 ficheros) | `rm -rf r9/r12fm/bin` |
| 0.34 | render/shots | capturas/trazas de corridas antiguas (no referencia) (324 ficheros) | `rm -rf render/shots` |
| 0.34 | mp/gen | ejecutables / objetos del port antiguo (etapa pre-r9) (98 ficheros) | `rm -rf mp/gen` |
| 0.33 | cpu | objetos *.o/*.a/*.elf/*.nro restantes de la etapa (no toca perf/r6/bin, _deps ni docs) (136 ficheros) | `find cpu -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.33 | glitch/gen | ejecutables / objetos del port antiguo (etapa pre-r9) (93 ficheros) | `rm -rf glitch/gen` |
| 0.33 | oracle | objetos *.o/*.a/*.elf/*.nro restantes de la etapa (no toca perf/r6/bin, _deps ni docs) (133 ficheros) | `find oracle -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.29 | r9/r11b/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (57 ficheros) | `find r9/r11b/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.29 | r9/r12a/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (57 ficheros) | `find r9/r12a/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.29 | r9/r11a/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (57 ficheros) | `find r9/r11a/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.29 | r9/r11d/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (57 ficheros) | `find r9/r11d/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.29 | r9/r11base/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (57 ficheros) | `find r9/r11base/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.29 | r9/r10b/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (56 ficheros) | `find r9/r10b/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.29 | perf/NSMBW.baseline | ejecutable base antiguo (fichero) (1 ficheros) | `rm -f perf/NSMBW.baseline` |
| 0.28 | r9/r27x2/m2 | objetos/ELF/NRO/map (frente viejo, sin referencias) (44 ficheros) | `find r9/r27x2/m2 -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.28 | glitch/bin | ejecutables / objetos del port antiguo (etapa pre-r9) (2 ficheros) | `rm -rf glitch/bin` |
| 0.27 | r9/combo/bin | ejecutables/ELF/NRO del frente (sin ningun doc) (2 ficheros) | `rm -rf r9/combo/bin` |
| 0.26 | mp/nxgen | ejecutables / objetos del port antiguo (etapa pre-r9) (134 ficheros) | `rm -rf mp/nxgen` |
| 0.26 | scenes/gen | ejecutables / objetos del port antiguo (etapa pre-r9) (93 ficheros) | `rm -rf scenes/gen` |
| 0.25 | r9/r29m3/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (65 ficheros) | `find r9/r29m3/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.25 | mp/nxbin | ejecutables / objetos del port antiguo (etapa pre-r9) (10 ficheros) | `rm -rf mp/nxbin` |
| 0.22 | r9/r16a2/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (58 ficheros) | `find r9/r16a2/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.21 | r9/r14r5/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (57 ficheros) | `find r9/r14r5/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.21 | r9/r16res/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (57 ficheros) | `find r9/r16res/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.21 | r9/r16w/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (57 ficheros) | `find r9/r16w/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.21 | r9/r20g6/e1 | objetos/ELF/NRO/map (frente viejo, sin referencias) (174 ficheros) | `find r9/r20g6/e1 -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.19 | scenes/switch_gen | ejecutables / objetos del port antiguo (etapa pre-r9) (90 ficheros) | `rm -rf scenes/switch_gen` |
| 0.18 | mp/runs | capturas/trazas de corridas antiguas (no referencia) (1745 ficheros) | `rm -rf mp/runs` |
| 0.15 | r9/combo/nx | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (54 ficheros) | `find r9/combo/nx -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.07 | r9/r11a/tsan | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (51 ficheros) | `find r9/r11a/tsan -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.05 | r9/r27x2/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (72 ficheros) | `find r9/r27x2/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.03 | perf/shots_base_200 | capturas/trazas de corridas antiguas (no referencia) (25 ficheros) | `rm -rf perf/shots_base_200` |
| 0.03 | r9/r19n/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (66 ficheros) | `find r9/r19n/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.03 | perf/shots_base_long | capturas/trazas de corridas antiguas (no referencia) (29 ficheros) | `rm -rf perf/shots_base_long` |
| 0.02 | r9/r26x1/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (63 ficheros) | `find r9/r26x1/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r28d1/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (57 ficheros) | `find r9/r28d1/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | mp | objetos *.o/*.a/*.elf/*.nro restantes de la etapa (no toca perf/r6/bin, _deps ni docs) (95 ficheros) | `find mp -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | perf/shots4 | capturas/trazas de corridas antiguas (no referencia) (12 ficheros) | `rm -rf perf/shots4` |
| 0.01 | perf/shots3 | capturas/trazas de corridas antiguas (no referencia) (20 ficheros) | `rm -rf perf/shots3` |
| 0.01 | r9/r22r7/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (54 ficheros) | `find r9/r22r7/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r21s/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (54 ficheros) | `find r9/r21s/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r16a2/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (51 ficheros) | `find r9/r16a2/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r26x1/difftest | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (9 ficheros) | `find r9/r26x1/difftest -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r29m3/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (56 ficheros) | `find r9/r29m3/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r16res/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (50 ficheros) | `find r9/r16res/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r14r5/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (50 ficheros) | `find r9/r14r5/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r16w/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (50 ficheros) | `find r9/r16w/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r11b/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (50 ficheros) | `find r9/r11b/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r12a/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (50 ficheros) | `find r9/r12a/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r11a/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (50 ficheros) | `find r9/r11a/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r11d/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (50 ficheros) | `find r9/r11d/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | r9/r11base/pc_build | objetos/ELF/NRO/map (se conservan .cmd/.txt/.yaml/scripts) (50 ficheros) | `find r9/r11base/pc_build -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | glitch | objetos *.o/*.a/*.elf/*.nro restantes de la etapa (no toca perf/r6/bin, _deps ni docs) (46 ficheros) | `find glitch -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.01 | perf/shots2 | capturas/trazas de corridas antiguas (no referencia) (7 ficheros) | `rm -rf perf/shots2` |
| **98.1** | TOTAL | | |

## NIVEL 2 - probablemente borrable, necesita confirmacion
| GB | ruta | por que se duda | comando |
|---:|---|---|---|
| 4.45 | r9/r34s2/bin | tools/rbuild_s2.sh y build_s2.sh (s2/pc_obj, s2/nx_obj); mencionado por r35m4, r41m, r43a | `rm -rf r9/r34s2/bin` |
| 3.97 | r9/render/bin | $R=r9/render: pc/.runlock, test/checks.sh, pc/runs, nx/$TAG usados por muchos guiones de pc_checks/pcrun | `rm -rf r9/render/bin` |
| 2.82 | r9/threads/bin | mencionado por switchctl/bench_title.sh, por_escena.py, r31t/r36r/r38c | `rm -rf r9/threads/bin` |
| 2.21 | r9/r20g6/bin | build_dx.sh/link_g1.sh de r32dx usan e2/pc_obj; r33g7 usa e2/export_pre | `rm -rf r9/r20g6/bin` |
| 1.80 | r9/r18m/bin | origen de r17pgo/m1src (procedencia) | `rm -rf r9/r18m/bin` |
| 1.75 | r9/r18m/nx | origen de r17pgo/m1src (procedencia) | `rm -rf r9/r18m/nx` |
| 1.12 | r9/r14g5/bin | $G5 en build_t/build_dx/build_m*: export/ y nx/build son usados (symlinks de pcproj); el resto sin referencia directa | `rm -rf r9/r14g5/bin` |
| 0.76 | r9/r14g5/difftest | $G5 en build_t/build_dx/build_m*: export/ y nx/build son usados (symlinks de pcproj); el resto sin referencia directa | `rm -rf r9/r14g5/difftest` |
| 0.76 | r9/r20g6/nx | build_dx.sh/link_g1.sh de r32dx usan e2/pc_obj; r33g7 usa e2/export_pre | `rm -rf r9/r20g6/nx` |
| 0.70 | r9/r34s2/nx | tools/rbuild_s2.sh y build_s2.sh (s2/pc_obj, s2/nx_obj); mencionado por r35m4, r41m, r43a | `rm -rf r9/r34s2/nx` |
| 0.68 | r9/r14g5/export | $G5 en build_t/build_dx/build_m*: export/ y nx/build son usados (symlinks de pcproj); el resto sin referencia directa | `rm -rf r9/r14g5/export` |
| 0.65 | r9/r16l/build | leido por r42k (doc) | `rm -rf r9/r16l/build` |
| 0.62 | r9/r13/export | copia de origen de m1/r17pgo (LEEME r17pgo); procedencia | `rm -rf r9/r13/export` |
| 0.61 | r9/r20g6/difftest | build_dx.sh/link_g1.sh de r32dx usan e2/pc_obj; r33g7 usa e2/export_pre | `rm -rf r9/r20g6/difftest` |
| 0.56 | r9/base/bin | citron_scene.sh referenciado por r17pgo/build_nx.sh y r42k | `rm -rf r9/base/bin` |
| 0.49 | r9/r13/bin | copia de origen de m1/r17pgo (LEEME r17pgo); procedencia | `rm -rf r9/r13/bin` |
| 0.48 | r9/r30g6c3/bin | $C3 (c3/pc_obj, nx_obj, nx/link_g6c3m.cmd) usado por build_m*.sh, build_s2.sh, build_c38.sh | `rm -rf r9/r30g6c3/bin` |
| 0.46 | r9/r24m2/bin | $M (nx/*.cmd, nx/order, rt/*.o) usado por build_t.sh y build_dx.sh | `rm -rf r9/r24m2/bin` |
| 0.45 | r9/r14g5/nx | $G5 en build_t/build_dx/build_m*: export/ y nx/build son usados (symlinks de pcproj); el resto sin referencia directa | `rm -rf r9/r14g5/nx` |
| 0.44 | r9/r34s2/runs | tools/rbuild_s2.sh y build_s2.sh (s2/pc_obj, s2/nx_obj); mencionado por r35m4, r41m, r43a | `rm -rf r9/r34s2/runs` |
| 0.44 | r9/r13/nx | copia de origen de m1/r17pgo (LEEME r17pgo); procedencia | `rm -rf r9/r13/nx` |
| 0.39 | r9/r16l/bin | leido por r42k (doc) | `rm -rf r9/r16l/bin` |
| 0.38 | r9/r18m/difftest | origen de r17pgo/m1src (procedencia) | `rm -rf r9/r18m/difftest` |
| 0.34 | r9/r15dk/bin | libuamlib_ns.a en build/ la usan pgo_release.sh y pgo_m4.sh | `rm -rf r9/r15dk/bin` |
| 0.32 | r9/r18m/export | origen de r17pgo/m1src (procedencia) | `rm -rf r9/r18m/export` |
| 0.30 | r9/threads/nx | mencionado por switchctl/bench_title.sh, por_escena.py, r31t/r36r/r38c | `rm -rf r9/threads/nx` |
| 0.30 | r9/render/nx | $R=r9/render: pc/.runlock, test/checks.sh, pc/runs, nx/$TAG usados por muchos guiones de pc_checks/pcrun | `rm -rf r9/render/nx` |
| 0.30 | r9/threads/export | mencionado por switchctl/bench_title.sh, por_escena.py, r31t/r36r/r38c | `rm -rf r9/threads/export` |
| 0.30 | r9/render/export | $R=r9/render: pc/.runlock, test/checks.sh, pc/runs, nx/$TAG usados por muchos guiones de pc_checks/pcrun | `rm -rf r9/render/export` |
| 0.28 | r9/r15dk/build | libuamlib_ns.a en build/ la usan pgo_release.sh y pgo_m4.sh | `rm -rf r9/r15dk/build` |
| 0.28 | r9/base/export | citron_scene.sh referenciado por r17pgo/build_nx.sh y r42k | `rm -rf r9/base/export` |
| 0.26 | r9/threads/gen | mencionado por switchctl/bench_title.sh, por_escena.py, r31t/r36r/r38c | `rm -rf r9/threads/gen` |
| 0.26 | r9/render/gen | $R=r9/render: pc/.runlock, test/checks.sh, pc/runs, nx/$TAG usados por muchos guiones de pc_checks/pcrun | `rm -rf r9/render/gen` |
| 0.26 | r9/base/gen | citron_scene.sh referenciado por r17pgo/build_nx.sh y r42k | `rm -rf r9/base/gen` |
| 0.26 | r9/r30g6c3/nx | $C3 (c3/pc_obj, nx_obj, nx/link_g6c3m.cmd) usado por build_m*.sh, build_s2.sh, build_c38.sh | `rm -rf r9/r30g6c3/nx` |
| 0.26 | r9/r15dk/nx | libuamlib_ns.a en build/ la usan pgo_release.sh y pgo_m4.sh | `rm -rf r9/r15dk/nx` |
| 0.24 | r9/r24m2/nx | $M (nx/*.cmd, nx/order, rt/*.o) usado por build_t.sh y build_dx.sh | `rm -rf r9/r24m2/nx` |
| 0.20 | r9/r20g6/e2 | build_dx.sh/link_g1.sh de r32dx usan e2/pc_obj; r33g7 usa e2/export_pre | `rm -rf r9/r20g6/e2` |
| 0.19 | r9/threads/switch_gen | mencionado por switchctl/bench_title.sh, por_escena.py, r31t/r36r/r38c | `rm -rf r9/threads/switch_gen` |
| 0.19 | r9/render/switch_gen | $R=r9/render: pc/.runlock, test/checks.sh, pc/runs, nx/$TAG usados por muchos guiones de pc_checks/pcrun | `rm -rf r9/render/switch_gen` |
| 0.19 | r9/base/switch_gen | citron_scene.sh referenciado por r17pgo/build_nx.sh y r42k | `rm -rf r9/base/switch_gen` |
| 0.04 | r9/r13/runs | copia de origen de m1/r17pgo (LEEME r17pgo); procedencia | `rm -rf r9/r13/runs` |
| 0.03 | r9/base/runs | citron_scene.sh referenciado por r17pgo/build_nx.sh y r42k | `rm -rf r9/base/runs` |
| 0.02 | r9/r34s2/pc_build_tsan | objetos/ELF/NRO/map (frente viejo, sin referencias) | `find r9/r34s2/pc_build_tsan -type f \( -name '*.o' -o -name '*.a' -o -name '*.elf' -o -name '*.nro' -o -name '*.nso' -o -name '*.map' \) -not -path '*/_deps/*' -not -iname '*save*' -delete` |
| 0.02 | r9/r18m/pc_build | origen de r17pgo/m1src (procedencia) | `rm -rf r9/r18m/pc_build` |
| 0.02 | r9/r34s2/pc_build | tools/rbuild_s2.sh y build_s2.sh (s2/pc_obj, s2/nx_obj); mencionado por r35m4, r41m, r43a | `rm -rf r9/r34s2/pc_build` |
| 0.01 | r9/r18m/runs | origen de r17pgo/m1src (procedencia) | `rm -rf r9/r18m/runs` |
| 0.01 | r9/threads/runs | mencionado por switchctl/bench_title.sh, por_escena.py, r31t/r36r/r38c | `rm -rf r9/threads/runs` |

Otros candidatos N2 (tamano con du; sin comando salvo que se confirme):
- switch/real_logs: 304 carpetas NO listadas en verif_refs.txt = 3,79 GB (13 366 jpg en total); 166 anteriores al 12-nov = 2,05 GB. Capturas de corridas de Switch que el usuario revisa a ojo: borrar solo tras confirmar. Lista: `ls -d switch/real_logs/*/ | sed 's#.*/\(.*\)/#\1#' | grep -vxFf <(grep -v '^#' tools/switchctl/verif_refs.txt)`.
- switch/real_builds 6,8 GB (33 NRO/ELF de compilaciones instaladas; m5 candidata): conservar las 3 ultimas, el resto N2.
- r9/*/export, gen, switch_gen de los frentes de N1 (~0,25-0,3 GB cada uno, copias casi iguales del codigo generado, regenerable con recomp pero con coste): r10a r10c cpucode guestloop (export+gen+switch_gen), r12fm, r18m, r19n, r21s, r22r7, r26x1, r28d1 (export 0,30 cada una) ~4 GB en total.
- Frentes VIVOS con bin/ pesado (regenerables pero en curso; NO tocar ahora, revisar al cerrar cada frente conservando solo el NRO instalado): r36r/bin 8,5 GB (+nx 1,6), r17pgo nx 7,1 y bin 6,6, r39r2/bin 4,7, r33g7/bin 4,1, r31t/bin 3,6 (+nx 0,8), r41m/bin 3,4, r40d/bin 2,8, r35m4/bin 2,5, r43a/bin 2,1 (+pc 0,6), r44w/bin 2,1, r38c/bin 1,5, r37v/bin 1,3, r42k/bin 1,3, r32dx datos 1,3 / runs 0,8 / pc_gcov 0,5.
- r9/r17pgo (15,8 GB): profiles/ (138 MB) INTOCABLE; m1src..m4src y g7dsrc (~0,35 GB cada una, procedencia de fuentes).
- recomp/bin 1,65 GB (scripts/estado.sh lo menciona), recomp/export 0,94 (CONSERVAR recomp/export/build/_deps, lo usan build_t/build_s2/build_m*.sh), recomp/logs 0,1.
- perf/r6 2,8 GB (conservar perf/r6/bin/NSMBW: scripts/estado.sh; perf/r6/export 1,4 y gen 0,3 son N2), perf/r7 1,2, perf/r5 1,0, perf/export_old 2,0, perf/export 1,0, perf/oldsrc 0,7, perf/rbuild 0,7 (con 139-215 docs/py/sh dentro).
- r8/{threads,codegen,memory,shaders,base} (10,2 GB; tras N1 quedan ~4 GB de cpp/datos), render/{export,export_regen,export_regen.old,scale,r4x} (~5 GB), oracle/native 2,3 GB, investigate (5,6 GB; tras N1 ~4 GB de txt/pkl/bin de analisis), verify 2,0 GB (evidencias png por ronda), build_usuario 1,2 GB (entregas al usuario nro/sd), status/r8_merge_backup 0,6, status/hangfix_runs 0,5, status/r7a_merge_backup 0,15, switch/bin 1,7 (13 NRO), switch/AppDir 0,3, switch/export-snap 0,26, switch/dist 0,15, dist 0,19, dolphin-oracle 0,2, cputest 0,13, avi de oracle/dolphin, cpu/dolphin, glitch/dolphin (0,17 c/u).
- tools/ghidra_12.1.4_PUBLIC 0,9 GB, tools/compilers.zip, tools/venv: herramientas reinstalables.
- tools/switchctl/.cola 1,5 GB y .cola_nro 0,6 GB: bajo tools/switchctl, INTOCABLE.

## CONSERVAR (referenciado por algo vivo, o intocable)
- Intocables: nand, nand_respaldo_antes_r6_1028_1615, game, switch/save_backups, **/save_tpl, todo con save/respaldo en la ruta, tools/switchctl, status/ESTADO.md, *.md *.patch *.py *.sh, profiles/ y *.gcda, 10 carpetas de verif_refs.txt, NWiiRecomp (repo git propio con cambios sin commit).
- r9/r14g5/{export,nx/build,cmake,nWiiRuntime}: 1890 enlaces simbolicos output_*.cpp (r41m/pcproj*, r44w/pcproj, r33g7, ...) apuntan a r9/r14g5/export; ademas $G5/export/build y $G5/nx/build en build_t/build_dx/build_m*.sh. Borrarlo rompe compilaciones PC vivas.
- r9/r24m2/{nx (*.cmd, order), rt, nWiiRuntime_r7f_src}: build_t.sh (r31t), build_dx.sh y link_g1.sh (r32dx).
- r9/r30g6c3/{c3,nx,tools}: $C3 en build_m4/m4r/m5/m5a/c38/r2/s2/t.
- r9/r34s2/{s2,tools,nWiiRuntime} y su bin/nx/runs recientes (rbuild_s2.sh, build_s2.sh; citado 7 veces por r35m4, tambien r41m y r43a; actividad hace 14-20 h).
- r9/r15dk/build/uamlib_nx/libuamlib_ns.a: pgo_release.sh (r17pgo) y pgo_m4.sh (r35m4).
- r9/render/{pc,test,nx,profile_*.txt,shadercache.tpl,default.toml,nand_tpl_mp}: $R en pcrun.sh, pc_verify.sh, run_dx.sh, audio_run.sh, det_efbvar.sh, pc_checks*.sh.
- r9/base (citron_scene.sh: r17pgo/build_nx.sh), r9/r13/{nWiiRuntime,export} y r9/r18m/{nWiiRuntime,export} (procedencia r17pgo), r9/r20g6/{e2/export,e2/pc_obj,nWiiRuntime} (build_dx.sh, link_g1.sh, r33g7), r9/r16l (leido por r42k), r9/threads (bench_title.sh, por_escena.py, r31t, r36r, r38c).
- r9/r17pgo entero, r9/r35m4, r33g7, r39r2, r37v, r38c, r36r, r31t, r32dx, r40d, r41m, r42k, r43a, r44w, r9/tools_comunes.
- switch/nsmbw/build/_deps (tomlplusplus-src, glad-src), recomp/export/build/_deps, switch/include/SDL2, switch/NSMBWnx (gcda/, debug.toml, logs), switch/NSMBW_bench, switch/real_builds, switch/real_logs (verif_refs), symbols, status, perf/r6/bin/NSMBW, recomp/rels_from_dolphin.toml (sym_match.py), cpu/gxw.txt.
- Symlinks de nWiiRuntime* en frentes vivos (_m5, _m5a, _r, _m6b.._m6h) apuntan a r33g7/a, r33g7/d, r35m4/m4 y a carpetas del mismo frente: no borrar r33g7/a, r33g7/d, r35m4/m4.

## Para un repositorio
### Trabajo PROPIO y ligero
- NWiiRecomp/ (repo git propio, origen github BlackLineInteractive/NWiiRecomp; ~50 MB sin build/, excluir NWiiRecomp/build 0,71 GB). Cambios locales sin commit en nWiiAnalyzer/{include,src}, nWiiRecomp/{include,src}, nWiiRuntime/{CMakeLists.txt,include,src}: es un fork, mejor subir como rama o parche (git diff de los M).
- nWiiRuntime mas recientes (solo src+include+CMakeLists, ~1,6-1,9 MB cada uno): r9/r44w/nWiiRuntime (1,8 MB), r9/r43a/nWiiRuntime (1,8), r9/r41m/nWiiRuntime_m6h y _m6i (1,6-1,9), r9/r40d/nWiiRuntime (1,7) y _merge (1,6). Basta subir el mas reciente + parches.
- Parches: 156 *.patch = 5,7 MB (p. ej. r9/r44w/w2_runtime_vs_m6e.patch, r9/r38c/c5_vs_m6a.patch, r9/r37v/*.patch, r9/r31t/r31t_vs_r7f.patch, r9/r41m/parches/, build_usuario/*.patch, glitch/r6_glitch.patch, mp/r6_mp.patch).
- Guiones: tools/switchctl sin .cola/.cola_nro (~0,9 MB), scripts (~0,2 MB), r9/tools_comunes (3 KB), tools/ y test/ de cada frente, CMakeLists.txt raiz, env.fish.
- Documentacion: status/ESTADO.md (77 KB), LEEME.md / README / FINDINGS.md / SHOWCASE.md y demas *.md (~8,6 MB sumados, incluye docs de terceros: filtrar tools/ghidra*, tools/venv). Revisar que no contengan /home/djvemo, IPs de la Switch ni partidas.

### NO subir NUNCA a internet
- Archivos del juego: game/ (0,35 GB), bin_usa/ (main.dol, .rel), NSMBW-Decomp/ (74 MB), tools/compilers.zip, cputest/, shadercache.bin, dist/ y switch/dist/, switch/NSMBWnx/ (brstm, arc, brsar), switch/AppDir, build_usuario/sd.
- Partidas: nand/, nand_respaldo_antes_r6_1028_1615/, switch/save_backups/, **/save_tpl, **/nand_tpl*, switch/shots/run10_quiet_save, verify/explore/{s6_save,nand_saved_tpl}, scenes/citron/save_tpl.
- Codigo generado del juego: recomp/export, perf/export*, perf/r6/export, render/export*, cpu/export, mp/export, glitch/export, scenes/export, switch/export-snap, r9/*/export*, r9/*/e1, r9/*/e2, r9/*/gen, r9/*/switch_gen, r9/r14g5/export, r9/r35m4/export, pcproj* de r41m/r44w/r38c (enlaces a output_*.cpp, main_output.cpp, functions.h, fastdispatch.cpp).
- Binarios/objetos: *.nro *.elf *.nso *.o *.a, todas las bin/ (r9/*/bin, render/bin...), switch/real_builds, switch/bin, NWiiRecomp/build, recomp/bin, nx*/, pc_build*/, pc_gcov.
- Perfiles PGO: r9/r17pgo/profiles/, *.gcda, switch/NSMBWnx/gcda, r9/r32dx/pc_gcov, r9/*/datos/.
- Capturas/audio/video: switch/real_logs/ (13 k jpg), switch/shots, render/shots, */runs, verify/*/ png, investigate/, glitch/runs (ppm), oracle/dolphin + dolphin-oracle + cpu/dolphin (avi), *.wav, *.fifo, dolphin.log.
- Simbolos de terceros / derivados: symbols/ (75 MB), NSMBW-Decomp, ghidra_projects/, tools/ghidra_12.1.4_PUBLIC, recomp/*.toml (nsmbw.toml, rels_from_dolphin.toml), r9/*/sites.tsv, *.pack de perf.
- Privados: tools/switchctl/.cola*, .claude/, rea_doctor.json, env.fish y REQUISITOS_SUDO.md (revisar IP/SSH), referencias a nsmbw-build.
