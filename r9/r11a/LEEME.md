# r11a: render repartido en dos núcleos (frente A, 8-nov-2026)

**Qué es:** el hilo de render (núcleo 0, dueño del contexto GL) ya no prepara los frames. Un hilo
nuevo, el **worker de render** (núcleo 2 en la Switch, prioridad 0x2D; un `std::thread` en PC),
toma los bytes del FIFO GX, los parsea (con las display lists expandidas), busca el final de frame,
hace el frame-skip, decodifica los vértices de los draws que sobreviven, destruye los comandos de
los frames ya dibujados y entrega el **frame entero** al hilo de render, que solo aplica el estado y
dibuja (texturas, copias EFB, transformación de vértices, lotes, GL). Mientras el render dibuja el
frame N, el worker prepara el N+1. Es la entrega "pequeña y segura" (parse + expansión de DL +
decodificación + destrucción en el núcleo 2); la transformación de vértices y el armado de lotes
siguen en el hilo de render (dependen del estado XF que se aplica en orden al dibujar, y el frente
B los está pasando a la GPU).

- **Estado (8-nov 13:20):** hecho y verificado en PC (frame check, TSan, render lento, parón,
  7 min); **sin probar en la Switch real ni en citron** (citron no llegó a arrancar el juego a
  tiempo, ver §4). Ganancia en fps de la Switch: por medir.
- **Interruptor:** `NWII_RENDER_WORKER=0` (entorno en PC, `debug.toml` en la Switch) vuelve al
  camino de siempre: el mismo código, en el mismo orden, todo en el hilo de render.
- **Para instalar en la Switch: `r9/r11a/bin/nsmbw_r11a.nro`** (sha256 `90c5418e04c622c7…`; `nsmbw_r11a.elf` para simbolizar
  perfiles; `nsmbw_r11a_input_test.nro` solo para pruebas). PC: `bin/NSMBW_r11a`,
  `bin/NSMBW_r11a_input_test`. (`w1`-`w3`: versiones intermedias, ver "Historia".)
- Compilar: `cd ~/nsmbw-port/r9 && JOBS=3 ~/nsmbw-port/scripts/lim tools/variant_build.sh r11a r11a both`
  (solo runtime + main.o contra el código generado de r10c; no toca las cabeceras prohibidas).

## 1. ¿Valía la pena? Medida de partida (PC, perfil `sw`, binario r11base)

`NWII_R7PH=cpu_atom` (ciclos del hilo de render por frame, por fase) en el mapa del Mundo 1 y el 1-1:

| Escena | total | parse | scan+split | decode | destroy | **render (GL + vértices)** | **preparación** |
|---|---|---|---|---|---|---|---|
| Mapa (11 037 draws GX) | 28,4 M | 3,5 M | 1,2 M | 4,6 M | 1,4 M | 17,6 M (62 %) | **10,8 M (38 %)** |
| 1-1 (2 797 draws) | 14,8 M | 1,9 M | 0,45 M | 1,4 M | 0,7 M | 10,4 M (70 %) | **4,4 M (30 %)** |

`perf` del hilo de render en el mapa (`pc/prof/r11a_base_map.txt` de r9/render): lo que es
preparación (ProcessGXFifo 14,5 %, ExpandDisplayList 4,7, RangesHash 3,8, DecodeDraw 3,0, copia de
GXCommand 2,7, DLContentHash 2,6, ParseStream 2,2, destructores de shared_count 1,3…) suma ~35 %;
Mesa (iris) solo 5,4 % en PC; la transformación (TexGen4 9,6, EmitPos4 7,4, EmitPrimitiveVB 5,2) y
RenderOnce 13,3 son lo que queda en el render. En la Switch real (perfil r10e, `profile_main.txt`)
la preparación es ~13 % de todas las muestras del hilo principal = **~27 % de su tiempo ocupado**
(ProcessGXFifo 2,8, ExpandDisplayList 1,9, FrameClear 1,6, ParseStream 1,3, DecodeDraw 1,1, copias
de GXCommand 0,9…); Mesa/nouveau + IPC ~10 % y la transformación ~11 % se quedan.

## 2. Diseño

Código: `nWiiRuntime/src/hle/gx.cpp` (casi todo), `src/hle/gx/fifo_parser.cpp`,
`include/runtime/gx/fifo_parser.h`, y cambios pequeños en `renderer_gl.cpp` (2 sitios, fuera de la
emisión de vértices), `core/main.cpp`, `platform/switch/switch_platform.{h,cpp}`,
`switch_profiler.cpp`.

1. **Drenaje partido en dos mitades.** `PrepFrame` (sin GL) = lo que antes hacía `ProcessGXFifo`
   hasta la decodificación; `RenderPacket` (hilo GL) = el resto (estadísticas, `Render()`). Un
   `FramePacket` lleva el frame (comandos), la lista de draws, el estado del parser y contadores.
   Sin worker se llaman una tras otra en `ProcessGXFifo` (idéntico a antes); con worker, `PrepFrame`
   corre en su hilo y el hilo de render solo toma paquetes listos.
2. **Estado del parser separado de `g_state`.** Antes el parser escribía en `g_state` los registros
   CP (VAT, arrays, índices de matriz) y "sembraba" `g_state.bp` y los bancos TEV con cada BP que
   parseaba, mientras el render lee `g_state` (hash de shader: CP 0x50/0x60; BP aún no reaplicados)
   y lo escribe (y NWII_R9_FRAMECHECK lo copia entero). Ahora el parser escribe en una copia privada
   (`s_ps`) y anota sus semillas (`s_seed`); `FifoParser::TakeRenderState` las entrega con el frame
   y `ApplyRenderState` (hilo de render) las pone en `g_state` justo antes de `Render()`: los mismos
   valores que dejaba el drenaje de un hilo, que siempre parseaba todo antes de dibujar. Sin worker
   se aplican al final de cada parse (como antes). `need_normal` ya no lee `g_state.xf` (nadie lo usa).
3. **Entrega por frames enteros, dos paquetes.** Uno lo dibuja el render, el otro lo llena el
   worker; uno lleno espera en `s_ready`. Un despertar por frame (condvar del render cuando hay
   paquete; `wait_next_frame` en `main.cpp` espera esa condvar en vez de dormir 1 ms). El worker
   sondea el FIFO cada 1 ms (4 ms tras 25 ms sin frame), como hacía el bucle del frontend.
4. **Siempre el frame más nuevo (retracción).** Si el worker completa otro frame y el render aún no
   tomó el que espera, lo retira y lo convierte en la parte "descartada" del nuevo (quita sus draws,
   marca sus copias EFB `stale_copy`, junta semillas: las viejas primero, las nuevas encima), igual
   que un drenaje que encuentra dos finales de frame. Así, con el render como cuello de botella
   (Switch), lo que se dibuja es el frame más nuevo disponible, como antes, y no uno que esperó un
   frame entero. Con `NWII_NOFRAMESKIP` no retira: espera.
5. **Contrapresión del juego intacta.** Antes el juego esperaba en su copia XFB si había ≥ 8 frames
   que el host no había tomado. Ahora cuentan también los que el worker tomó y el render todavía no
   (`s_held_frames`, bajo `g_fifo_mutex`): si el render se cuelga (compilación de shader de 2 s), el
   juego se para igual que antes en vez de seguir corriendo.
6. **Propiedad de los objetos compartidos.** Los contadores de `HostPtr` (DrawRaw, DecodedVerts) no
   son atómicos: todas sus copias/destrucciones ocurren en el worker (parse, caché de DL, decode,
   destrucción de paquetes ya dibujados). El render solo lee comandos y vértices por puntero y
   escribe `emit_cache` (que el worker nunca toca). Un paquete solo se destruye después de que el
   render lo devolvió (mutex = happens-before). Si una entrada de la caché de emisión muere en el
   worker (al morir sus DecodedVerts), su hueco del arena de vértices se devuelve en el hilo de render
   en el siguiente `Render()` (`vbuf::ArenaFreeAny` / `TakeDeferred`: un frame más tarde, nunca antes).
7. **Lecturas de RAM del juego.** Igual que hoy: display lists (instantánea del momento de la
   llamada, o RAM), cargas LOAD_INDX y arrays de vértices se leen al parsear/decodificar (ahora en el
   worker, **antes o al mismo tiempo** que antes: el worker no espera a que el render quede libre);
   texturas, TLUT (BP 0x65) y copias EFB se siguen leyendo en el render, en el mismo orden, y como el
   render ya no parsea, empiezan a dibujar **antes o igual** que antes respecto al juego. Ninguna
   lectura se movió a más tarde. La retracción (punto 4) evita que un frame preparado espere
   mientras el juego sobrescribe sus texturas.
8. **Switch.** Hilo con `pthread_create` (pila de 1 MB), `svcSetThreadCoreMask` al núcleo 2,
   `svcSetThreadPriority(0x2D)`: un paso por debajo del mezclador AX y la salida de audio (0x2C/0x2B,
   también en el núcleo 2), que siempre lo interrumpen. `NWII_RENDER_WORKER_CORE` /
   `NWII_RENDER_WORKER_PRIO` lo cambian. `fps_log` añade "gxw x%" a la línea de hilos y el perfilador
   (`NWII_NX_PROFILE`) escribe también `logs/profile_gxw.txt`.
9. **Fuera del worker** (se queda en el hilo de render, avisando en el log): sin ventana (headless),
   `NWII_R7PH`, `NWII_FRAMESIG`, `NWII_LATE_SYNC`.
10. Salida: `GX_StopWorker()` al acabar el bucle principal (y por `atexit`), antes de destruir GL.
11. Registros: `[RPROF]` añade `prep_cpu_us/frame` (CPU de la preparación), `worker=`,
    `retracts=`, `held=`; `[FPS]` añade `gx worker N%`; `[GXPROF]` igual que antes (contadores
    del parser, acumulados por paquete). Prueba: `NWII_R11_SLOW_RENDER_US=<us>` (render lento a
    propósito) y `NWII_R11_STALL=<frame>,<ms>` (un parón).

## 3. Números en PC

`rt/test/ab.sh` (copia de `pc_ab.sh` con cargas de hilo): medias del [RPROF]/[FPS] en la escena.
`drain_cpu` = CPU del hilo de render por frame en `ProcessGXFifo` (con worker: aplicar estado +
render); `prep_cpu` = CPU de la preparación (en el worker o dentro del drenaje).

**Perfil `sw`** (2 núcleos LP-E, cuota 150 % para todo el proceso), orden A B C B A.
*Ojo: el frente B corría a la vez en los mismos núcleos LP-E: fps y % son ruidosos.*

| Escena | binario | drain_cpu (µs) | prep_cpu | hilo render | worker | juego | fps |
|---|---|---|---|---|---|---|---|
| Mapa | r11base | 18 710 / 18 119 | – | 78 / 79 % | – | 59 % | 41,7 / 43,7 |
| Mapa | r11a (worker) | **11 210 / 10 522** | 6 281 / 6 061 | **48 / 49 %** | 28 / 29 % | 54 % | 42,6 / 46,7 |
| Mapa | r11a, `NWII_RENDER_WORKER=0` | 17 688 | 6 279 | 80 % | – | 58 % | 44,8 |
| 1-1 | r11base | 6 789 / 6 089 | – | 41 / 37 % | – | 45 % | 60 |
| 1-1 | r11a (worker) | **5 317 / 5 167** | 1 934 / 1 836 | **32 / 32 %** | 12 % | 50 % | 60 |
| 1-1 | r11a, worker off | 6 669 | 1 504 | 40 % | – | 46 % | 60 |

**Perfil `e3`** (3 núcleos E 13-15, sin cuota: tres núcleos como los 0-2 de la Switch; perfil nuevo
en `rt/pc/limc`), A B B A. *También sin el candado de corridas (el frente B corría a la vez).*

| Escena | binario | drain_cpu (µs) | prep_cpu | hilo render | worker | fps |
|---|---|---|---|---|---|---|
| Mapa | r11base | 9 218 / 9 491 | – | 56 / 58 % | – | 60 |
| Mapa | r11a | **5 788 / 5 976** | 3 794 / 3 792 | **35 / 36 %** | 23 % | 60 |
| 1-1 | r11base | 3 987 / 3 703 | – | 24 / 23 % | – | 60 |
| 1-1 | r11a | **3 055 / 2 994** | 1 260 / 1 237 | **19 / 18 %** | 8 % | 60 |

**Repetición con el candado de corridas** (`rt/ab_locked.sh` bajo `flock r9/render/pc/.runlock`,
13:08-13:20; ninguna otra corrida del juego a la vez, pero sí compilaciones/Dolphin de otros):

| Perfil | Escena | binario | drain_cpu (µs) | render_cpu | prep_cpu | hilo render | worker | juego | fps |
|---|---|---|---|---|---|---|---|---|---|
| sw | Mapa | r11base (1.ª) | 13 967 | 8 518 | – | 80 % | – | 52 % | 57,5 |
| sw | Mapa | r11a | **9 664** | 9 657 | 5 540 | **49 %** | 29 % | 52 % | 50,6 |
| sw | Mapa | r11a | **10 159** | 10 152 | 5 775 | **49 %** | 29 % | 53 % | 48,1 |
| sw | Mapa | r11base (4.ª) | 17 786 | 10 915 | – | 79 % | – | 59 % | 44,1 |
| e3 | Mapa | r11base | 8 674 | 5 125 | – | 53 % | – | 32 % | 60 |
| e3 | Mapa | r11a | **5 511** | 5 508 | 3 555 | **33 %** | 22 % | 32 % | 60 |

Con `sw` el ruido es enorme (r11base dio 57,5 y 44,1 fps en la 1.ª y la 4.ª corrida): los fps de
`sw` **no sirven** para decidir. Lo que sí se repite en todas las tandas: el hilo de render baja
~35-45 % en el mapa (drain_cpu y % de hilo), y el `Render()` en sí se encarece un 5-15 % (en `sw`
hasta +15 %: datos escritos por otro núcleo y, con 3 hilos ocupados en 2 núcleos con cuota, más
expulsiones). **Con solo 2 núcleos para todo (perfil `sw`) el worker puede no ganar nada o perder**;
la ganancia necesita un núcleo libre de verdad, como el 2 de la Switch (solo audio, pocos %).

Lectura:
- El hilo de render baja **~37-40 % en el mapa y ~20-25 % en el 1-1** (CPU por frame).
- El `Render()` en sí sube un poco con el worker (mapa 5,7 → 5,8-6,0 ms en e3; 1-1 2,7-2,8 → 3,0):
  los comandos y vértices los escribió otro núcleo (fallos de caché) y hay más competencia por la
  memoria. La preparación también cuesta algo más en el worker (1-1: 1,5 → 1,9 ms en sw).
- En PC los fps no cambian: a 60 fps el render no es el cuello de botella (y en `sw` la cuota de
  150 % para los tres hilos lo es). La ganancia de fps solo aparece donde el render manda, como en
  la Switch.
- El camino sin worker (`NWII_RENDER_WORKER=0`) cuesta lo mismo que r11base (refactor neutro).

En la Switch `[RPROF] prep_cpu_us/frame` sale 0 (newlib no tiene reloj por hilo): mirar
"threads main x% … gxw y%" del `fps_log` y el perfilador (`profile_main.txt` / `profile_gxw.txt`).

**Qué esperar en la Switch (estimación, no medido):** en el mapa el hilo principal está al 98-99 %;
si la preparación es ~27-38 % de su tiempo, el render por frame baja en esa proporción menos el
sobrecoste (~5 %): **~1,25-1,5× fps en el mapa** si el juego no pasa a ser el límite; en el 1-1 el
render (60-85 %) deja más margen, pero ahí el cuello es el hilo del juego. El núcleo 2 pasa de casi
ocioso a ~30-60 % (más en el mapa con render lento, porque con retracción prepara cada frame del
juego aunque el render se salte algunos).

## 4. Verificación

| Prueba | Resultado |
|---|---|
| `test/checks.sh` (render doble cada 30 frames + caché de emisión recalculada), 1x, title/map/walk/lvl11, binario w1 (worker sin retracción) | 4 × `checks=220 identical=220 diff_lines=0`, `mismatches=0` |
| idem con el binario final r11a (`rt/verify_r11a.sh`, bajo `flock r9/render/pc/.runlock`) | 4 × `checks=220 identical=220 diff_lines=0`, `mismatches=0` (title, map, walk, lvl11) |
| ThreadSanitizer (runtime + main instrumentados, `rt/tsan_build.sh`), mapa ~1 min y 1-1 | **0 avisos** en código del worker/parser/render/caché; los 26-30 avisos "GX" son de Mesa (su hilo `gdrv0`, sin instrumentar) y el resto son carreras previas ajenas (VI, audio, sondeos). La corrida acaba en ~1-2 min por un límite de TSan con los `longjmp` del juego (pila sombra), no por el worker. |
| Render lento a propósito (`NWII_R11_SLOW_RENDER_US=40000`, ~21-23 fps presentados) + frame check + caché de emisión, mapa y 1-1, 150 s | retracciones: 5 700 (mapa) y 5 467 (1-1); frame check 100/100 idénticos, `diff_lines=0`, `mismatches=0`; `held` máx. 1 y vuelve a 0; escenas correctas (llega al STAGE del 1-1); capturas bien; 0 errores |
| Parón de 3 s del render en el frame 1800 (`NWII_R11_STALL=1800,3000`, dentro del 1-1), worker on / off | el juego espera igual en los dos: "backlog waits 2 (2979 ms)" con worker, "2 (2965 ms)" sin él; sigue normal después, 0 errores |
| Corrida larga 7 min (perfil e3, guion del 1-1): mapa → 1-1 → mapa → 1-1 (Mario muere y vuelve a entrar) | sin cuelgues ni errores, 59-60 fps, `held=0` al final, capturas bien (`rt/pc/runs/r11a_long/`) |
| citron, NRO r11a | **no se pudo**: citron tardó 240 s y luego 540 s solo en arrancar (recorre la lista de juegos de `/mnt/Remote_RetroROMs/Switch`, lento hoy) y el tiempo de la corrida se acabó justo al arrancar el juego (`rt/citron_map/`). El NRO compila y enlaza; el código Switch nuevo es poco (pthread + `svcSetThreadCoreMask` + `svcSetThreadPriority`, igual que en `ax_async.cpp`/`switch_audio.cpp`). |

**Comparación entre binarios (r11base vs r11a) frame a frame: no concluyente.** `test/det.sh`
(reloj virtual, sin frame-skip, todas las copias XFB) **no es determinista** ni con el mismo binario:
r11base contra r11base da 842 de 1643 frames distintos desde el ~350 (r11base vs r11a: 852 de 1766;
r11base vs r11a sin worker: 1001 de 1686), así que esa vía no puede probar nada en ninguna dirección.
La garantía viene de: mismo código en el mismo orden sobre los mismos bytes; el estado entregado
es por construcción el que dejaba el drenaje de un hilo; TSan limpio; frame check; y las capturas
(`render/pc/runs/chk_r11a_w1_*/t_*.png`, `rt/pc/runs/r11a_*`) se ven bien.

## 5. Riesgos

- **Contadores no atómicos de `HostPtr`**: la seguridad depende de la regla "solo el worker
  copia/destruye; el render solo lee". Si alguien (p. ej. el frente B) hace que el render copie un
  `HostPtr<DecodedVerts>`/`DrawRaw` o guarde comandos entre frames, hay que revisarlo (o volver a
  contadores atómicos para esos tipos). `emit_cache` vive en DecodedVerts y lo escribe el render.
- **Fusión con r11b**: los cambios en `renderer_gl.cpp` son solo `vbuf::ArenaFreeAny`/
  `TakeDeferred`, `#include <mutex>` y la declaración de `GX_OnRenderWorker`.
- **Semántica de tiempo**: el render de la Switch puede empezar un frame antes o igual que antes
  (nunca después), y la retracción hace que el worker prepare más frames de los que se dibujan
  (CPU del núcleo 2, no del 0). Con el AX "robado" (si el núcleo 2 está ocupado el juego mezcla él
  mismo) podría subir un poco el hilo del juego: mirar `inline` en la línea de AX del fps_log.
- **Determinismo entre binarios** no verificable con las herramientas actuales (ver arriba).
- `NWII_FRAMESIG` / `NWII_R7PH` / `NWII_LATE_SYNC` desactivan el worker.

## 6. Cómo seguir

1. Probar en la Switch `nsmbw_r11a.nro` frente a `nsmbw_r10?.nro` sin OC en el mapa y el 1-1;
   en `fps_log`: fps, "threads main/cpu/ax/aud/gxw", `[RPROF] retracts=` y `held=`. A/B con
   `NWII_RENDER_WORKER = "0"` en `debug.toml` usando el mismo NRO.
2. Si el núcleo 2 queda corto (audio con cortes, AX "inline" alto): subir la prioridad del audio no
   hace falta (ya interrumpe), pero se puede bajar la del worker (`NWII_RENDER_WORKER_PRIO=0x30`).
3. Siguiente paso natural: combinar con r11b (transformación en GPU). Con menos trabajo de vértices
   en el render, la parte movida al worker pesa más (en PC, ~50 % del hilo de render del mapa).
   El hash de texturas (`hash_ram`) **no** debe moverse al worker: lee RAM del juego al dibujar.
4. Si hiciera falta más: preparar en el worker la parte de `DrawPrimitive` que no depende del
   estado XF (índices de primitivas `EmitIndices`, clasificación de draws), con cuidado de no tocar
   la emisión de vértices que cambia r11b.

## 7. Herramientas en `rt/`

Copia privada del arnés de `r9/render` (para no pisar `r9/render/pc/run` del frente B; usar igual
`flock r9/render/pc/.runlock`): `pc/run.sh`, `pc/limc` (+ perfil `e3`), `test/checks.sh`,
`test/det.sh`, `test/ab.sh` (nuevo), `tsan_build.sh` (binario TSan en `r11a/tsan/`),
`verify_r11a.sh` (las pruebas finales de la tabla 4). Corridas en `rt/pc/runs/`, `rt/test/runs/`.

## 8. Parche

`r9/r11a/r11a_vs_r11base.patch` (`diff -ruN r11base/nWiiRuntime r11a/nWiiRuntime`, ~1 300 líneas)
para fusionar con otros frentes.

## Historia

- w1: worker con cola de 1 (esperaba a que el render tomara el paquete). checks.sh OK.
- w2/w3: retracción (siempre el frame más nuevo) + contrapresión `s_held_frames` + espera
  `NOFRAMESKIP`. A/B de PC con w3.
- r11a: w3 + `retracts=`/`held=` en [RPROF] + perillas de prueba `NWII_R11_SLOW_RENDER_US`/`NWII_R11_STALL`.
