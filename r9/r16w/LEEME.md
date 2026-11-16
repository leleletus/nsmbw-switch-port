# r16w: el camino de escritura GX del hilo del juego, más barato (frente W, 8-nov-2026)

**Estado (8-nov 23:00):** terminado. **w4r5** (`combo_r5/bin/nsmbw_w4r5.nro`) es la versión final, verificada
en PC; w2r5 ya se probó en la consola (§4). El frente de fusión r18m tiene la lista de archivos. Solo runtime: no se toca el código
generado ni el recompilador. Base: runtime de `r9/r13` (copiado en `nWiiRuntime/`), compilado con
`tools/variant_build.sh` (copia de la de r14r5: `-DNWII_FASTMEM=1` en el `.o` del input_test de Switch) contra
el código del juego de r13: `BASE=/home/djvemo/nsmbw-port/r9/r13 JOBS=2 ~/nsmbw-port/scripts/lim
tools/variant_build.sh . w2 both`.

## 1. Qué pasaba (consola, r13b, demo del título, `switch/real_logs/bench_r13b_1108_1813/`)

Hilo del juego (núcleo 1), % de todas las muestras: `wgp_write32` 2,51, `wg_flush_now` 1,17,
`GX_WGPIPE_Write8` 0,66, `Write16` 0,18, `psq_store` 0,30, `mutexUnlock` 0,24; esperas en el kernel:
`svcArbitrateLock` **4,05** y `svcWaitProcessWideKeyAtomic` 2,59. El log: `wgp 1088k writes/s 212k flushes/s (15 B)`.

- **Por qué tantos vaciados:** la "regla BP" de r8 entregaba el búfer de escritura (write-gather) al anfitrión
  4 bytes después de **cualquier** byte 0x61, o sea después de cada comando BP (el SDK escribe uno en casi
  cada llamada GX). Cada entrega = candado + `vector::insert` de 13-15 bytes.
- **Quién esperaba en `svcArbitrateLock`:** el candado `s_idx_mtx` (un `std::mutex`) de las copias de
  LOAD_INDX (arreglo del iris, r11d). El juego lo tomaba **dos veces por cada `GXCallDisplayList`**
  (~22 000/s en el título) y el parser del worker otras dos por lista. En el perfil, el 57 % de las llamadas a
  `pthread_mutex_lock` del hilo del juego vienen de `wgp_write32` (donde va en línea `idx_dl_call`). El worker
  corre en el núcleo 2 con prioridad baja y el mezclador AX lo interrumpe, a veces con el candado tomado:
  el juego se quedaba bloqueado. Además cada registro pedía memoria (un `vector` por arreglo) que se
  liberaba en el otro hilo.
- **`svcWaitProcessWideKeyAtomic`:** es la espera de `ax::finish()` (el juego espera la mezcla del hilo AX:
  `ax ... wait 55-67 ms/s` en las ventanas pesadas). La causa la encontró R5 (a12): el hilo AX nacía con
  prioridad 0x3B, por debajo del worker. w2 lleva ese mismo arreglo.
- `svcSleepThread` 46 % es la espera normal (`idle_wait`, ritmo de frames).

## 2. Cambios

| Qué | Dónde |
|---|---|
| **Entrega del búfer write-gather** solo cuando hace falta: búfer lleno (ahora **4 KB**, antes 512 B), BP **0x52** (copia EFB/XFB: la del XFB cierra el frame y la cuenta `FifoWaitForHost`), **0x45** (draw done), **0x47/0x48** (tokens: los que `NWII_LATE_SYNC` señala desde el parser), por **edad** (más de 500 µs de reloj del anfitrión, mirado en el refresco de reloj del juego, ~200 000/s) y al quedar el juego ocioso (`idle_wait`) o tras vaciar la FIFO del CP. El anfitrión no necesita los bytes antes: solo parsea lo que encuentra en sus sondeos de 1 ms, los arreglos de vértices se leen al llegar el fin de frame y las listas/LOAD_INDX se copian en el momento de la llamada | `hle/gx.cpp`, `hle/ios.cpp` (`poll_tb_refresh`), `hw/hw_cp.cpp` |
| **Caminos en línea** de `GX_WGPIPE_Write8/16/32/F32/F64`: una comprobación (`s_wg_mode` + bandera de redirección) y se guardan los bytes; `Write32` de datos (sin comando pendiente) ya no pasa por el detector BP/LOAD_INDX; contadores sin atómicos por escritura (se publican al entregar). El camino completo de antes queda para depuración (`NWII_WGP_CHECK`, `NWII_SAMPLE`), modo directo y listas en construcción (redirección) | `hle/gx.cpp` |
| **Copias de LOAD_INDX sin candado:** colas de un productor (juego) y un consumidor (parser) con índices acquire/release, los bytes de los arreglos en un anillo de bytes de 1 MB (sin `new` por registro), y los rangos por lista (`s_idx_need`) pertenecen al parser, que manda los cambios al juego por una tercera cola; el juego los aplica antes de cada búsqueda | `hle/gx.cpp` |
| **Perfilador:** para las muestras en una espera del kernel guarda la cadena de direcciones de retorno (barrido de la pila desde `sp`); líneas `wstack` en `profile_*.txt`, se leen con `tools/wstack_symbolize.py` | `platform/switch/switch_profiler.cpp` |
| (w2) **Hilo AX a prioridad 0x2C** en la Switch: el mismo cambio de R5 a12 (archivo idéntico) | `hle/ax/ax_async.cpp` |

Lo que el parser recibe es lo mismo: los mismos bytes en el mismo orden, los mismos registros LOAD_INDX en el
mismo orden, la misma ventana de búsqueda y la misma poda por frames. Diferencias solo pasados los topes: con
la cola llena (16 384 registros) se descarta el registro nuevo (r13 descartaba el más viejo) y con el anillo de
bytes lleno el registro va sin bytes; en los dos casos el parser lee la RAM en vivo, como para cualquier
registro que le falte (contador `dropped` en `[IDXSNAP]`, 0 en todas las pruebas).

### Interruptores (entorno en PC, `debug.toml` en la Switch)

- `NWII_WGP_MAXAGE_US` (500): edad máxima del búfer; `0` la quita.
- `NWII_WGP_CAP` (4096): tamaño del búfer (16-4096).
- `NWII_WGP_FLUSH_ALL_BP=1`: entrega además tras cada BP del SDK (parecido a r13).
- `NWII_WGP_DIRECT=1`: camino directo (sin búfer), como antes.
- `NWII_WGP_CHECK=<prefijo>` (PC): graba lo escrito y lo entregado; deben ser iguales.
- `NWII_AX_PRIO` (0x2C, de R5).
- La línea de estadísticas ahora dice `wgp Nk writes/s M.Mk flushes/s (B; bp X/s, age Y/s)`.

## 3. Binarios

| Nombre | Qué es |
|---|---|
| `bin/nsmbw_w1.nro` (+`.elf`) | r13 + los cambios de §2 sin el de AX ni los de w3/w4 |
| **`bin/nsmbw_w2.nro`** (+`.elf`) | w1 + prioridad AX de R5 (a12) |
| **`combo_r5/bin/nsmbw_w2r5.nro`** (+`.elf`) | runtime actual de R5 (`r9/r14r5`, a13) + los cambios de §2 (fusión de 3 vías de `gx.cpp`; un conflicto trivial: `PipeNoteXfb()` en la ruta directa). Contra a13 en la consola aísla este frente |
| `combo_r5/bin/nsmbw_w3r5.nro` | w2r5 + ventana del perfilador (§2b) |
| **`combo_r5/bin/nsmbw_w4r5.nro`** (+`.elf`) | **final:** w3r5 + caminos en línea para las palabras BP y CP (§2b). Diferencia con r14r5: `combo_r5/w4r5_vs_r14r5.patch` (4 archivos: `hle/gx.cpp`, `hle/ios.cpp`, `hw/hw_cp.cpp`, `platform/switch/switch_profiler.cpp`) |
| `bin/nsmbw_w4.nro` | lo mismo sobre r13 (con la prioridad AX de a12) |
| `*_input_test*` | solo pruebas (entrada por guion): nunca como juego |

## 2b. Añadido después (w3, w4)

- **w3 · ventana del perfilador** (para medir solo un tramo, p. ej. el desierto): en `debug.toml`,
  `NWII_NX_PROFILE_FROM` / `NWII_NX_PROFILE_TO` (segundos desde que arranca el perfilador, ~arranque de la NRO) y
  `NWII_NX_PROFILE_SLOW_MS` (muestrea solo mientras el intervalo entre frames del juego, media de 1 s, pasa de esos
  ms durante `NWII_NX_PROFILE_SLOW_HOLD_S`, 2 s; se cierra tras 1 s por debajo; un segundo sin frames, una carga,
  no cuenta como lento). Se combinan (Y). El log dice `[PROFILE] window open/closed at X s` y los archivos llevan
  `sampled_s` y `windows`; se escriben al cerrarse cada ventana. Contador nuevo `g_guest_xfb` en `gx.cpp`.
- **w4 · caminos en línea para las palabras de comando:** el perfil de w2r5 en la consola aún daba 1,4 % al camino
  completo `wgp_write32`: un tercio eran las cargas acquire de las guardas de dos `static` locales, y lo demás las
  ~250 000 palabras BP/s del SDK y los valores de registros CP, que seguían yendo por él. Ahora el BP que no pide
  entrega y el valor CP van en línea (hacen exactamente lo mismo que el camino completo) y las guardas son banderas.

## 4. Medidas y verificación

### PC (Intel, Mesa; `r9/render/pc/run.sh` bajo `flock .runlock`; resultados en `runs/`)

| Prueba | Resultado |
|---|---|
| `checks.sh` 1x (FRAMECHECK píxel a píxel cada 30 frames) | w2, w2r5 y w4r5: title/map/walk/lvl11 **220/220 idénticos** cada uno, `diff_lines=0`, `mismatches=0` |
| `NWII_WGP_CHECK` (título 95 s, w2 y w2r5) | **336 MB escritos por el juego = 336 MB entregados**, idénticos byte a byte; 0 desajustes de la bandera de redirección |
| worker apagado (`NWII_RENDER_WORKER=0`, título + 4 demos, 145 s) | w2, w2r5, w4r5: 59-59,5 fps, sin parones, FASTMEM 0 fallos, GPUXF ok |
| larga (w2): título 235 s + mapa andando 175 s | 59,7 / 59,7 fps, sin parones, `dropped 0` |
| `[IDXSNAP]` (copias de LOAD_INDX) | lecturas en vivo como r13 (main 0, listas ~2-3 mil de millones de usos), `dropped 0` |
| entregas del búfer (título) | r13: **271 000/s de 13 B** → r16w: **2 500-5 000/s de 1,3-2,6 KB** (BP ~1 000-2 000/s, edad ~200/s) |
| A/B CPU tipo Switch (`PROFILE=sw`, `tools/ab_title.sh`, demo del título) | hilo del juego **35,4 % (r13) → 32,2 % (w2)**, −9 % relativo (una sola pareja A-B, no ABBA: la repetición se cortó al cerrar el frente); ver `runs/ab_sw_title.txt` |
| `det.sh` (reloj virtual) título, r13 contra w1 | idénticos hasta la copia 350 y luego divergen, **igual que r13 contra r13** (límite conocido de ese arnés: r11a, r12fm) |

### Consola (las corrió el coordinador; OC 1785, portátil)

- **w2r5** (`switch/real_logs/bench_w2r5_1108_2203/`): `wgp 2421k writes/s 2.7k flushes/s (2149 B)` frente a
  `274k flushes/s (13 B)` de a12; hilo del juego en las ventanas de 60 fps del título **69-70 % (a12: 72-74 %)**.
  Sin cuelgues, imagen bien. El tramo del desierto 4J sigue en 30 fps (el juego tarda >16,7 ms por frame ahí).
- Esperas del hilo del juego (herramienta `wstack`): `svcArbitrateLock` **4,05 % (r13b) → 14 muestras (0,01 %)**;
  `svcWaitProcessWideKeyAtomic` desaparece (prioridad AX de a12). Lo que queda es `svcSleepThread`: ~94 %
  `idle_wait` (normal) y ~5 % `FifoWaitForHost` (el juego 8 frames por delante del anfitrión).
- Mapa del mundo con w2r5 (GL): 57,8-58,4 fps en las 31 ventanas (a12: 43-46), según el coordinador
  (`switch/real_logs/bench_mapa_w2r5_*`).

## 5. Cómo verificar de nuevo

```sh
cd ~/nsmbw-port/r9/r16w
tools/verify_w.sh w4r5 checks wcheck off long     # cada corrida toma r9/render/pc/.runlock
flock ../render/pc/.runlock tools/ab_title.sh x 150 ../r13/bin/NSMBW_r13_input_test bin/NSMBW_w4_input_test
python3 -I tools/wstack_symbolize.py --elf <el .elf> <profile_cpu.txt>   # quién espera, en la consola
```
(`bin/NSMBW_w4r5_input_test` y `NSMBW_w2r5_input_test` son enlaces a `combo_r5/bin/`.)

## 6. Riesgos

- La entrega es más tardía (hasta 4 KB o 500 µs): si algún día el anfitrión leyera algo de la RAM del juego **al
  parsear** que el juego cambia enseguida, lo vería más tarde. Hoy lo único así son las listas sin copia y
  LOAD_INDX, que ya se copian al llamar; con 1 ms de sondeo el anfitrión ya veía tarde. `NWII_WGP_FLUSH_ALL_BP=1`
  y `NWII_WGP_MAXAGE_US` permiten acercarse a r13 si algo raro aparece.
- Un BP escrito por otro camino que no sea `Write8(0x61)` + `Write32` (ninguno en NSMBW que se sepa) no fuerza la
  entrega: llega por edad (≤ 500 µs) u ocio. La copia del XFB del SDK sí va por ese camino.
- Colas de LOAD_INDX llenas: se pierde el registro nuevo (r13 perdía el viejo); el parser lee la RAM en vivo.
  No pasó en ninguna prueba (`dropped 0`).
- El barrido de pila del perfilador lee la pila de un hilo pausado (solo con `NWII_NX_PROFILE`), dentro de su
  mapeo (consultado con `svcQueryMemory`).
