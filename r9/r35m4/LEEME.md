# r35m4: m4n = G7 variante d (compactación) + S2 (sonido en otro núcleo) — 15-nov-2026

Sin commits. Solo lectura: `r33g7`, `r17pgo/g7dsrc`, `r34s2`, `r31t`, `r30g6c3`. Nada en la SD.

> **ESTADO 15-nov 06:16: TERMINADO** — m4n, m4 (PGO) y m4r (PGO + render R) verificadas y medidas; resumen en §10.

## 1. Qué es
- **Código generado** (`export/`) = `r33g7/d/export` (G7 d: feats stfs,psq,fp,poll,icm,disp,cr; `g7.h` con el
  arreglo de ctr==0) + `tools/apply_s2.py` (pasada S2 de r34s2). Mismos ganchos que sobre c3:
  E1 323, E2 22, E3 23, E4 8 (`diag/apply_s2_d.log`; sobre c3 la salida es idéntica byte a byte a la de r34s2).
  Único cambio en la pasada: con la feat `cr` los campos CR viven en locales de G7 (`G7CR_DECL(N)` en la cabecera
  de la función) y no en `G5cNeq/…`; el volcado de E4 ahora añade `G7CR_STORE(N);` para ellos (antes se habrían
  quedado sin volcar: cr0 en los 8 sitios E4).
- **Objetos**: los de G7 para los 73 archivos que S2 no toca (`r33g7/dn/nx_obj` con `-DG7_NOIC`, `r33g7/d/pc_obj`;
  fuentes de d sin cambios desde que se compilaron) + los 14 de S2 compilados aquí con la MISMA orden de G7
  (`tools/build_m4obj.sh`, solo cambia `-I<export>`) → `m4/{nx_obj,pc_obj}`.
- **Runtime** (`nWiiRuntime/`) = `r34s2/nWiiRuntime` (S2, s2g) + parche de G7 (`diag/g7_runtime.patch` =
  `diff -ruN r31t r33g7`): `cpu_context.h`, `hw.h`, `interpreter.cpp`, `hw_pi.cpp` quedan iguales a los de r33g7;
  `ios.cpp` fusionado a mano: el sondeo lento de S2 (`snd_poll` para el contexto de sonido, `game_poll`) va ANTES
  de la parte de G7 (`process_pending_callbacks_slow_inner` + `poll_persist_exit`), así el contexto de sonido nunca
  toca `s_g7_shadow/s_g7_rekick` (estáticos del hilo del juego).
  Además (encargo 7): **quitado del todo el parche viejo del DEC** («64 veces en el mismo pc → forzar alarmas»,
  r34s2 §9) y en `s2.cpp` el bucle de la CPU de sonido mira `hosted` antes que `n` (aviso de TSan).
- Enlace: `tools/build_m4.sh <nombre> m4 [pc|nx|both]` (= `r33g7/tools/build_g7.sh` con V=r35m4, RT=g7).

## 2. Interacciones G7 × S2 revisadas
- `poll` (sondeo compacto): el contexto de sonido entra en su sondeo lento cada 64 cuentas (`snd_poll`), como antes;
  los avisos de G7 (`poll_kick_global`) son al contexto del juego. `queue_callback` ahora hace `poll_kick()` (G7):
  la CPU de sonido encola callbacks al juego → el juego los ve en el sondeo siguiente (mejor que antes).
- `disp` (`g7_bctrl_g6`): tabla hash global montada perezosamente; la monta el hilo del juego al arrancar (mucho
  antes de la toma del sonido) y luego es de solo lectura. `native_stack_ok()` en el hilo de sonido siempre da
  cierto (como en s2g con c3).
- `icm` + `-DG7_NOIC`: S2 no usa `inst_count` para nada de tiempo.

## 3. Verificación (antes de consola)
Ver bitácora.

## Bitácora
- 00:30 export, pasada, runtime fusionado; 14 objetos nx/pc compilados aquí (3 + 3 min); `bin/nsmbw_m4n.nro/.elf`,
  `bin/NSMBW_m4n[_input_test]` (00:38).
- cputest (gen_tests de G7 d + E1 de S2 en sus 2 `mtmsr`, ganchos inertes): 288 passed, 0 failed, salida idéntica a
  `g6.out` (`cputest/`).
- Tabla fastmem del ELF (`r17pgo/tools/check_fm_tab_g5.py`): 722 769 entradas, **0 mal formadas** (g7dn2: 722 772, 0).
- 00:55 PC checks 1x apagado: title 200/200, map 140/140, walk 160/160, lvl11 220/220, `diff_lines=0`. Título con
  S2 encendido: copias EFB normales (20-28/fot), ACTIVO(2).
- 00:58 consola en cola (detrás de Y01/Y02/R02/R03/G08/G09 ajenos): **M01_m4n_off, M02_m4n_on, M03_g7dn2_ctl**.
- 00:59 **PGO m4**: `tools/pgo_m4.sh m4 export nWiiRuntime -DG7_NOIC` (unidad systemd `m4-pgo`, registro
  `runs/pgo_m4.log`): foto `r17pgo/m4src` + `conf/m4.env`, instrumentada en la otra laptop con **PJOBS=4** (allí
  G7 compila `hn` con -j9; 20 GB libres; el PGO de g7d ya pasó su gen y está entrenando en consola). Luego encola
  Z01/Z02 (demos A/B, 900 s, **NWII_S2=1**), finish (JOBS=11) y deja `bin/m4_pgo.nro`; la medida se encola a mano.
  Si m4n falla en consola: `systemctl --user stop m4-pgo` y quitar Z01/Z02 de la cola.
- 01:12 **fallo de apagado** visto en checks (S2 encendido, title): tras el SIGTERM el proceso seguía al 100 % y no
  terminaba (sin ptrace; por /proc: queda solo el hilo de la CPU girando). Causa probable y arreglada: el hilo de
  sonido salía (longjmp de apagado desde un manejador, EE = 0) con el candado S tomado y el hilo del juego giraba
  para siempre en `game_acquire`. `s2.cpp`: `snd_quit_release()` suelta S (y una petición a medias) en las 3 salidas.
  Solo afecta al apagado. Recompilado (solo runtime) 01:14; las copias de M01/M02 en la cola se reemplazaron por
  esta NRO (md5 igual a `bin/nsmbw_m4n.nro`; `bin/nsmbw_m4n_M01.elf`). OJO: la foto del PGO (`r17pgo/m4src`) se
  hizo a las 00:59, ANTES de este arreglo (solo cambia el apagado; no se toca la foto para no romper SHA256SUMS
  con la instrumentada ya en marcha).
- Comparador (A = c3, B = m4/pc_obj, ganchos S2 inertes): semilla 1 y 2: 40 024 iguales, 0 diferencias, 6 solo
  signo de NaN, 1 no concluyente (= G7 d).
- PC checks 1x **encendido** (binario arreglado): title 200/200, map/walk/lvl11 220/220, `diff_lines=0`, 0 trampas,
  0 asserts, 0 `force-expired`, FASTMEM 0. `cmp_png` contra m3n: title 2,4 %, map 2,0 %, lvl11 3,3 % (animaciones al
  ritmo de la música); walk 54,9 % apagado y encendido (igual que s2a: la cámara del mapa desplazada unos píxeles,
  misma escena; mirado). Capturas de PC del título y 1-1 miradas: modelos con luz.
- 01:33 el finish del PGO de g7d (JOBS=11) arrancó en la otra laptop: la instrumentada de m4 queda **pausada**
  (SIGSTOP a su ninja y sus 4 cc1plus; PIDs en `runs/pgo_m4_parados.txt`); `tools/reanudar_gen.sh` (unidad
  `m4-reanudar`) la reanuda con SIGCONT cuando termina `pgo_g7d_fin`.
- 02:20 **error mío**: la pausa (SIGSTOP) dejaba la instrumentada de m4 con el turno de `scripts/lim` de la otra
  laptop, y el finish de g7d (que espera ese turno) estuvo parado ~45 min. Se reanudó y luego se PARÓ la
  instrumentada de m4 (`remoto parar pgo_m4_gen`; los .o hechos se quedan: ninja sigue donde iba) para dejar pasar
  el finish de g7d. `tools/reanudar_gen.sh` (unidad `m4-pgo2`) espera a `pgo_g7d_fin` y relanza `pgo_m4.sh` con
  PJOBS=8. Lección: en la otra laptop no pausar trabajos que tengan el turno de lim.

## 4. Consola m4n (sin PGO), corridas contiguas, lista fija de 5 demos, 400 s (ms juego/fot REAL, fot/s juego)
`bench_M01_m4n_off_1115_0208`, `bench_M02_m4n_on_1115_0216`, control `bench_M03_g7dn2_ctl_1115_0225`
(+ G09 g7dn2 de G7 justo antes). Las cuatro IMAGEN OK; `capturas.jpg` de M01 y M02 miradas (modelos con luz,
misma secuencia). M02: ACTIVO(2), 0 trampas, 0 asserts, 125 mensajes perdidos (ráfagas en cargas), 0 force-expired.

| Escena | g7dn2 (M03) | m4n apagado (M01) | **m4n encendido (M02)** | m4n on − g7dn2 | proyección 60 fot/s: off → **on** |
|---|---|---|---|---|---|
| desierto4j | 16,8 (44,4) | 17,1 (43,7) | **12,7** (48,7) | −4,1 | 15,9 → **12,7** |
| agua_pinguino | 22,2 (33,4) | 22,2 (33,5) | **16,5** (34,7) | −5,7 | 19,7 → **16,5** |
| cielo_paratroopas | 19,7 (31,9) | 19,7 (32,2) | **15,3** (35,3) | −4,4 | 17,7 → **15,3** |
| nubes_plataformas | 15,7 (50,7) | 15,9 (49,0) | **12,6** (53,5) | −3,1 | 15,3 → **12,6** |
| castillo | 10,0 (58,9) | 10,3 (59,0) | **7,0** (59,1) | −3,0 | 10,2 → **7,0** |

m4n apagado = g7dn2 dentro del ruido (+0,0-0,3). Con S2 las cinco escenas quedan bajo 16,7 ms (agua justo: 16,5).
Proyección como en r34s2: el ahorro del sonido es por segundo → a 60 fot/s el apagado pagaría (off−on)·fps_off/60.

## 5. PGO «m4»
- 02:36 instrumentada `r17pgo/bin/m4_pgogen.nro/.elf` (otra laptop, PJOBS=8 tras el finish de g7d; tabla fastmem
  0 mal formadas). Entrenamiento en cola: Z01_m4gen_demosA, Z02_m4gen_demosB (900 s, demos forzadas, NO_SAVE, gl,
  **NWII_S2=1**), detrás de R04-R09, Y03, Y04 ajenos. Después la unidad `m4-pgo2` hace el finish (JOBS=11) y deja
  `bin/m4_pgo.nro/.elf`. Medida a encolar a mano: Z03 m4pgo encendido, Z04 m4pgo apagado, Z05 g7d+PGO
  (`r17pgo/bin/g7d_pgo_g7d_all.nro`), Z06 m3+PGO (`r17pgo/bin/m3_pgo_m3_all.nro`), contiguas.
- Audio PC m4n (título 60 s, tiempo real, `diag/wav_m4n_titulo.txt`): encendido vs apagado envolvente 0,994,
  espectral mediana 1,000 (mín. 0,934), 0 huecos, clics 15 vs 14; copias EFB iguales (28/48).
- 04:06 Z01 (entrenamiento A) terminado, perfiles en `r17pgo/profiles/raw/261115_085146`. OJO: la instrumentada con
  S2 va a ~6 fot/s (m3gen sin S2: ~14,5): el hilo del juego espera ~35 % (candado S / peticiones con el sonido
  también instrumentado, y contadores gcov compartidos entre dos núcleos). Cubre menos demos en 900 s y el código
  de sonido pesa más (relativo) que en una partida real. Imagen bien (captura de lanzamiento mirada).
- 04:39 **PGO listo**: Z02 terminó (perfiles 261115_085146 y 261115_090934, 142 .gcda cada uno), finish en la otra
  laptop sola (JOBS=11, 11 min, 0 avisos de perfil, fastmem 0 mal formadas). `bin/m4_pgo.nro/.elf`
  (= `r17pgo/bin/m4_pgo_m4_all.*`; `bin/m4_pgo_Z03.elf`). Recordatorio: lleva el s2.cpp de antes del arreglo de
  apagado (foto de las 00:59).
- 04:40 en cola, contiguas: Z03_m4pgo_on, Z04_m4pgo_off, Z05_g7dpgo_ctl, Z06_m3pgo_ctl (detrás de Y05/Y06 de G7).

## 6. m4r = m4 PGO + parche de render R (r36r_vs_r31t.patch: gx.cpp, renderer_gl.cpp)
- El runtime NO lleva perfil (`PGO_RUNTIME=OFF` en build_pgo.sh), así que no hay coverage-mismatch: en la otra
  laptop `r17pgo/m4rsrc/nWiiRuntime` (copia de m4src + parche) y `nx/m4r_pgo` (copia de la carpeta del finish de m4;
  en su build.ninja solo esas 2 fuentes apuntan a m4rsrc). Se ejecutan a mano (sin ninja, que re-ejecutaba cmake)
  las órdenes exactas de `ninja -t commands`: compilar gx.o y renderer_gl.o, `ar` del runtime, enlace, nacp, nro
  (`tools/m4r_pgo_remote.sh` documenta la idea; lo ejecutado está en esta bitácora). Los objetos del código generado
  son los MISMOS de m4. `bin/m4r_pgo.nro/.elf` (fastmem 0 mal formadas; contiene NWII_R_COALESCE).
- PC: `tools/build_m4r.sh` (runtime `nWiiRuntime_r` = el de m4n + parche; sin PGO) → `bin/NSMBW_m4r[_input_test]`.
- Consola en cola: Z07_m4r_onR (S2 + los 5 NWII_R_*), Z08_m4r_off (todo apagado), Z09_m4pgo_on_tprof (histograma
  de fotogramas), Z10_m4r_onR_s2p2e (como Z07 con `NWII_S2_PRIO=0x2E`, ver abajo).
- **Hallazgo** (`r36r/tools/fiesta.py` sobre M01/M02): con S2 la fiesta del castillo DIBUJA menos (32,1 → 25,1 fot/s)
  y el trabajador de render pasa de 21,3 a 38,2 ms/fot: la CPU de sonido vive en el núcleo 2 con prioridad 0x2C,
  por encima del trabajador (0x2D, mismo núcleo). El juego va igual (58-59 fot/s). Z10 prueba la CPU de sonido por
  debajo del trabajador (0x2E; el mezclador AX sigue en 0x2C). `NWII_S2_CORE="0"` no se puede pedir por debug.toml
  (`apply_env` borra los "0").
- `tools/tprof_m4.py … hist` (copia de r31t/tools/tprof.py + orden `hist`): por fotograma del juego, ms ocupados del
  hilo principal y ms de pared. Con s2g (S10): desierto ocupado mediana 13,3 ms (0,4 % > 16,7) pero el 53 % de los
  fotogramas dura > 18 ms de pared: el límite no es el hilo del juego.
- PC m4r (S2 + 5 NWII_R_* + NWII_S2_CHECK): checks 1x title/map/walk/lvl11 220/220, `diff_lines=0`, 0 trampas,
  0 asserts, 0 perdidos, FASTMEM 0, salida normal. cmp_png contra m4n encendido: map/walk 0,0 %, lvl11 3,1 %,
  title 0-2,6 % salvo t_006300 (88,5 %: otra demo de atracción elegida, ambas limpias, mirado); contra m3n ≤2,5 %.
  (No uso `r36r/test/demos_fc.sh`: escribe en r9/r36r.)

## 7. Consola con PGO, contiguas (Z03-Z06; ms juego/fot REAL · fps juego / dibujados · render ms/fot)
| Escena | m3 PGO (Z06) | g7d PGO (Z05) | m4 PGO apagado (Z04) | **m4 PGO encendido (Z03)** |
|---|---|---|---|---|
| desierto4j | 15,8 · 48,4/49,6 · 14,3 | 14,0 · 55,3/55,1 · 14,1 | 14,4 · 53,6/53,3 · 14,4 | **11,6** · 53,7/53,3 · 14,7 |
| agua_pinguino | 21,6 · 33,9/33,6 · 15,8 | 19,7 · 35,6/34,7 · 15,4 | 20,6 · 34,9/34,8 · 16,1 | **15,4** · 36,0/35,0 · 16,3 |
| cielo_paratroopas | 19,2 · 32,8/32,7 · 14,2 | 17,1 · 41,1/40,7 · 13,9 | 18,0 · 36,9/36,9 · 14,6 | **14,3** · 45,5/45,2 · 14,7 |
| nubes_plataformas | 15,0 · 54,5/54,5 · 10,8 | 13,6 · 59,4/59,4 · 10,2 | 14,2 · 58,0/58,0 · 10,9 | **11,3** · 58,4/58,3 · 10,9 |
| castillo | 9,7 · 59,1/46,5 · 17,6 | 8,9 · 59,3/47,2 · 17,3 | 9,4 · 59,3/46,3 · 18,0 | **6,3** · 59,2/41,5 · 17,9 |
Las cuatro IMAGEN OK; capturas de Z03 y Z04 miradas. Z03: 0 trampas/asserts, 107 perdidos.
- m4 PGO apagado es ~0,5-0,9 ms PEOR que g7d PGO: el perfil de m4 se entrenó con S2 encendido (y a ~6 fot/s), así
  que con S2 apagado está peor ajustado. Encendido (que es como se usará) es lo mejor medido: contra m3 PGO
  −4,2 · −6,2 · −4,9 · −3,7 · −3,4 ms; contra g7d PGO −2,4 · −4,3 · −2,8 · −2,3 · −2,6 ms.
- Fiesta del castillo (`fiesta.py`): dibujados 26,4 (Z03, S2) vs 32,3 (Z04) / 33,2 (g7d) / 32,5 (m3); trabajador
  35,8 vs 23,3 ms/fot. La CPU de sonido le quita núcleo 2 al trabajador de render (ver §6).
- Z07 **m4r** (S2 + 5 NWII_R_*): IMAGEN OK, capturas miradas; 0 trampas/asserts, 106 perdidos. Contra Z03 (m4 on):
  juego igual o mejor (11,4 · 14,9 · 14,2 · 11,3 · 6,2 ms), render ms/fot 14,0 · 14,8 · 14,3 · 10,4 · **13,3** (Z03:
  14,7 · 16,3 · 14,7 · 10,9 · 17,9). Fiesta: dibujados 26,4 → **32,4**, render 26,1 → 18,7 ms, trabajador 35,8 → 27,2.
  El parche R compensa lo que S2 le quita al trabajador (Z04 sin S2 ni R: 32,3 dibujados).
- Z08 m4r todo apagado = Z04 (m4 apagado) dentro del ruido: 14,5 · 20,5 · 17,8 · 14,0 · 9,3 ms juego (Z04: 14,4 ·
  20,6 · 18,0 · 14,2 · 9,4); fiesta 33,1 vs 32,3 dibujados. IMAGEN OK.

## 8. ¿Por qué agua/cielo no llegan a 60 con el juego por debajo de 16,7 ms? (Z09 = m4 PGO + S2 + TPROF)
`tools/tprof_m4.py <bench> datos/m4_pgo.nm hist` (`diag/hist_z09.txt`): por fotograma del juego, ms ocupados del hilo
principal y ms de pared.
| Escena | ocupado mediana / p90 | fot. ocupado > 16,7 ms | fot. de pared > 18 ms | pared ~33 ms (2 retrazos) |
|---|---|---|---|---|
| desierto4j | 10,2 / 12,3 | 0,0 % | 13,1 % | 11,6 % |
| agua_pinguino | 14,3 / 17,4 | 10,1 % | **70,2 %** | 70,1 % |
| cielo_paratroopas | 13,3 / 14,3 | 0,0 % | 31,5 % | 31,4 % |
| nubes_plataformas | 10,2 / 11,2 | 0,0 % | 3,6 % | 2,8 % |
| castillo | 4,1 / 8,2 | 0,0 % | 2,6 % | 0,5 % |
Sí es cuantización por el retrazo, pero no por el hilo del juego: en agua solo el 10 % de los fotogramas pasa de
16,7 ms de trabajo y aun así el 70 % dura dos retrazos. El hilo principal espera en `EGG Display::beginFrame` (espera
al retrazo, 12,8 ms/fot en agua; `tprof … esperas`): el fotograma anterior aún no ha salido por el render (render
ms/fot de agua 16,3 en Z03, 14,8 con R en Z07), así que juego + render en cadena no caben en 16,7. El siguiente
límite de agua y cielo es el render/sincronía con el VI, no la CPU del juego.

## 9. CPU de sonido por debajo del trabajador (Z10 = m4r onR + `NWII_S2_PRIO=0x2E`)
- Fiesta del castillo: dibujados 32,4 → **52,6**, trabajador 27,2 → 9,4 ms/fot (deja de ser expulsado).
- PERO el sonido se queda sin núcleo: AID media 300,8/s (Z07: 326,4), 47 ventanas < 320/s (Z07: 2), listas AX
  297/s → el audio iría más lento/entrecortado; y el juego baja algo en otras escenas (nubes 58,3 → 54,8 fot/s:
  espera al sonido). **No es aceptable tal cual**. Lo que falta: sacar la CPU de sonido del núcleo 2 o repartir
  (p. ej. núcleo 0 con el render: hoy `NWII_S2_CORE="0"` no llega porque `apply_env` borra los "0"; habría que
  aceptar otro valor) o bajar la prioridad solo durante ráfagas del trabajador. Sin medir.

## 10. RESUMEN (06:20)
- **m4n** (sin PGO) y **m4** (PGO, entrenado con S2) verificadas: comparador 2 semillas = G7 d, cputest 288/288,
  checks 1x apagado y encendido idénticos, capturas de PC y consola miradas, todas las corridas IMAGEN OK.
- Mejor medido para uso con S2: **m4r con S2 + NWII_R_*** (Z07): ms juego/fot 11,4 · 14,9 · 14,2 · 11,3 · 6,2;
  fot/s juego 54,1 · 37,1 · 46,2 · 58,3 · 59,3; dibujados 53,6 · 35,5 · 45,9 · 58,2 · 47,0.
- Binarios: `bin/nsmbw_m4n.nro/.elf`, `bin/m4_pgo.nro/.elf` (= `r17pgo/bin/m4_pgo_m4_all.*`), `bin/m4r_pgo.nro/.elf`
  (= `r17pgo/bin/m4r_pgo.*`). NADA instalado en la SD.
- Riesgos: shutdown de S2 arreglado en m4n pero NO en m4/m4r PGO (foto anterior); mensajes perdidos del SoundThread
  en cargas (~110 por 400 s); el perfil PGO entrenado a ~6 fot/s con S2 favorece el sonido (m4 PGO apagado es peor
  que g7d PGO); S2 quita núcleo 2 al trabajador (fiesta) salvo con R; agua/cielo limitados por render + retrazo.

## 11. m5 = candidata de instalación (m4r + arreglo de apagado de S2 + S2 y R ENCENDIDOS por defecto)
- Runtime `nWiiRuntime_m5` (= `nWiiRuntime_r` + 3 cambios): `s2.cpp` init: S2 encendido salvo `NWII_S2_OFF` (o
  `NWII_S2=0` explícito en PC); `gx.cpp`/`renderer_gl.cpp`: `m5_rflag()`: cada `NWII_R_*` explícito manda, si no está
  → encendido salvo `NWII_R_OFF`. (`apply_env` de la Switch BORRA los valores "0"/"off"/"false"/"no", por eso el
  apagado es una variable aparte con valor "1".)
- **Línea de apagado para debug.toml** (apaga sonido en otro núcleo y los 5 cambios de render):
  `NWII_S2_OFF = "1"` y `NWII_R_OFF = "1"` (cada una por separado apaga solo lo suyo).
- PGO: en la otra laptop `r17pgo/m5src` (= m4rsrc + los 3 archivos) y `nx/m5_pgo` (copia de `nx/m4r_pgo`); se
  recompilan s2.o, gx.o, renderer_gl.o y se enlaza con los MISMOS objetos PGO de m4. `bin/m5_pgo.nro/.elf`
  (sha256 en `diag/m5_sha256.txt`: nro dc0430ca…07fd, elf f56b83ea…19ec; fastmem 0 mal formadas).
- PC: `tools/build_m5.sh` → `bin/NSMBW_m5[_input_test]`; `tools/pc_checks_m5.sh` (checks sin variables y con el
  apagado, cmp_png, título 100 s con audio).
- Consola en cola 06:19: Z11_m5_defecto, Z12_m5_apagado (lista fija), Z13_m5_audio_on / Z14_m5_audio_off (título
  100 s sin demo forzada, `NO_PRESS=1`, `NWII_AUDIODUMP=sdmc:/switch/NSMBWnx/logs/audm5_*.wav`, traídos por
  `PULL_PREFIX=audm5_`). La cola estaba parada (vacía): arrancada con la orden de status/PAUSA_1112.md.
- Audio de salida en consola (líneas [NX]: audout): underruns acumulados en 400 s: Z03 m4 S2 62 / Z04 apagado 84;
  Z07 m4r S2+R 59 / Z08 apagado 82; salida media 31,3 kHz en todas; 0 ms descartados; hueco máx. ~10 ms en todas.
  Z10 (S2 con prioridad 0x2E) 1 757 underruns y 28,0 kHz: confirma que esa variante no vale.
- PC m5: checks 1x **sin variables** 220/220 las cuatro, `diff_lines=0`, ACTIVO(2), 0 trampas/asserts/perdidos, RCOAL
  activo (= m4r encendido; cmp_png contra m4r: ≤2,6 % salvo t_006300 = otra demo de atracción); **con el apagado**
  (`NWII_S2_OFF=1 NWII_R_OFF=1`) 220/220, sin [S2] ni RCOAL, cmp_png contra m4n apagado ≤2,8 %. El proceso termina
  bien al salir en las 8 (exit 124 = el `timeout` de la prueba, sin quedarse colgado). Audio PC título 100 s, por
  defecto contra apagado: envolvente 0,975, espectral mediana 0,999, 0 huecos, clics 24 vs 20.
- Consola: **Z11 m5 sin variables** = Z07 (11,5 · 15,3 · 14,2 · 11,3 · 6,2 ms; fiesta 33,2 dibujados; 53 underruns;
  115 perdidos, 0 trampas); **Z12 con el apagado** = Z08 (14,5 · 20,7 · 17,8 · 14,0 · 9,3 ms; sin [S2] ni RCOAL en
  el registro). Las dos IMAGEN OK, capturas miradas.
- Mensajes perdidos (Z11): todos al empezar/acabar cada demo (cargas), ninguno en mitad del juego: +6/+26 arranque,
  +8 desierto, +15 agua, +7 cielo, +16 nubes, +5/+20 castillo, +7/+5 vuelta. Qué son: el callback de AX manda al
  SoundThread un aviso «ha pasado una trama» con OSSendMessage NOBLOCK sobre una cola de 4; si el hilo va retrasado
  (aquí, esperando el mutex que el juego tiene durante la carga) la cola está llena y el aviso se descarta, igual
  que en la consola original (misma cola, misma llamada sin bloqueo: el gancho de S2 reproduce exactamente ese
  comportamiento). Efecto: el SoundThread procesa menos tramas en ese instante (la música/secuencias pueden ir un
  poco por detrás durante la carga); NO se pierde ninguna orden de sonido (las órdenes del juego van por las
  estructuras de nw4r protegidas con el mutex, no por esta cola).
- **Volcado de audio en consola, primer intento (Z13/Z14, `NWII_AUDIODUMP` del runtime):** no vale: escribe 2 bytes
  por fwrite y hace fflush cada segundo en la SD desde el hilo del audio → con S2 la AID baja a ~240/s, salida 22,8
  kHz, 1 550 underruns (Z11 sin volcado: 333/s, 31,3 kHz, 53). Variante **m5a** (= m5 + volcado en RAM que se escribe
  de una vez al llegar a `NWII_AUDIODUMP_SECS`, en otro hilo; `var_aud/hw_dsp.cpp`, `r17pgo/nx/m5a_pgo`): Z16
  (encendido) y Z17 (apagado), título 130 s, 80 s de audio. Z15 (CPU de sonido en el núcleo 0, `bin/m5c0_pgo`,
  opcional) va detrás.

## 12. 06:47 REINICIO DE LA CONSOLA con m5a (Z16) — ALTO a la consola (orden del coordinador)
- Z16 (m5a, volcado de audio en RAM + `std::thread` aparte que escribe el .wav de 10 MB al llegar a 80 s) reinició
  la Switch entera a los ~90 s (sin crash report). El .wav quedó completo en la SD. El registro acaba normal (última
  línea [S2] con `retraso max 68 ms`: el hilo de sonido se paró ~68 ms ese segundo, justo cuando empieza la
  escritura). Diferencia m5 → m5a: SOLO `wav_append` (hw_dsp.cpp), que con S2 se ejecuta en el hilo anfitrión de
  sonido: buffer de 10 MB reservado al empezar, y al límite un hilo nuevo (pthread de libnx creado DESDE el hilo de
  sonido) que hace fopen + un fwrite de 10 MB + fclose. Z13 (m5 + volcado viejo, fwrite incremental desde el mismo
  hilo) no cayó. PC (`bin/NSMBW_m5a`, misma lógica, `NWII_AUDIODUMP_SECS=40`, S2 encendido): escribe y sigue sin
  problema (`r34s2/runs/m5a_repro`). Hipótesis más probable: el camino nuevo de la Switch (crear el hilo desde el
  hilo de sonido y/o la escritura de 10 MB de una vez a la SD), no S2 ni R en sí. No demostrado.
- m5b (escritura síncrona única) compilada (`bin/m5b_pgo.*`) pero NO probada: Z18/Z19 retirados a
  `tools/switchctl/.cola/pausa_1115_crash/`. ERROR MÍO: a las 06:49:56, antes de leer la orden de alto, rearranqué la
  cola con Z15 (m5c0 = m5 con la CPU de sonido en el núcleo 0, SIN volcado); termina ~06:59 y la cola se para sola.
- Audio de Z13/Z14 (volcado viejo): NO sirve para juzgar la calidad: el propio volcado perturba (Z13 AID ~240/s,
  22,8 kHz, 1 550 underruns; Z14 apagado 321 listas/s, 26,9 kHz, 889 underruns y 629 clics). Comparaciones:
  Z13 vs Z14 envolvente 0,872 / espectral 0,813; Z14 vs PC apagado 0,909 / 0,863 (además el título sin demo forzada
  elige demos distintas). La evidencia válida de audio en consola sigue siendo la de las corridas SIN volcado:
  AID/listas AX = apagado, 31,3 kHz de salida, underruns 53-62 con S2 frente a 82-84 sin S2, 0 ms descartados.

## 13. 07:00 OJO: Z15 corrió con OVERCLOCK (cpu 1785 MHz, gpu 1152 MHz)
- Todas las corridas hasta Z14 y Z16 van a 1020/460 MHz (líneas `[NX] game … cpu … MHz`); **Z15 (tras el reinicio
  de la consola) va a 1785/1152**: sys-clk volvió al perfil con OC. Sus cifras de vídeo (7,4 ms, 59,7 dibujados…)
  NO son comparables con Z11 ni dicen nada del núcleo 0 a 1020 MHz. Su audio malo (AID 281-312/s) sí apunta a que
  en el núcleo 0 la CPU de sonido (0x2C) no gana al hilo principal/render (también 0x2C en el núcleo 0: misma
  prioridad = no lo expulsa).
- Variante pedida sin recompilar: `bin/m5c0_pgo.nro` + `NWII_S2_PRIO = "0x2B"` en debug.toml (por encima del hilo
  principal 0x2C del núcleo 0; en ese núcleo no vive nada más con prioridad propia). NO encolada: primero hay que
  devolver la consola a 1020 MHz, o Z20 tampoco sería comparable.
