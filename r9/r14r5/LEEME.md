# r14r5: hilo de render (GL) más barato (frente R5, 8-nov-2026)

**Estado (8-nov 19:25):** a12 probado en la consola por el coordinador (gran mejora, §3); **a13 = a12 + `Pal3`
sin `memcmp`** es el último (`bin/nsmbw_a13.nro`, `.elf`), verificado en PC. Pendiente: el desierto (§4). Base: runtime de `r9/r13` (copiado aquí, `nWiiRuntime/`), compilado solo-runtime con
`tools/variant_build.sh` (copia de `r9/tools/variant_build.sh` con `-DNWII_FASTMEM=1` en el `.o` del
`input_test` de Switch, que fallaba al compilar sin él). Enlaza con el código del juego de r13.

## 0. Punto de partida: el perfil de r13 en la consola (`switch/real_logs/r13_1108/`)

Simbolizado por líneas: compilé `renderer_gl.cpp`, `texture_cache_gl.cpp` y `gx.cpp` de r13 con los
mismos flags + `-g` (código idéntico) y llevé cada muestra a su línea (`addr2line -j .text.<función>`).

- Las ventanas a ~30 fps son el **mapa del mundo** (11-12 mil dibujos GX por frame): render ~20,7 ms/frame,
  hilo del juego ~20 ms, worker (núcleo 2) ~14 ms. Las de 36-38 fps son niveles con 2,8-3,5 mil dibujos
  y **24-30 mil vértices "de anillo"** por frame (dibujos que no son de display list: personajes, etc.).
- Hilo principal (todo el log, 2217 s): 48 % dormido; de lo ocupado, ~55 % runtime, ~32 % Mesa/nouveau,
  ~5 % IPC del kernel, ~5 % newlib.
- Lo más caro del runtime, por línea:
  - `RenderOnce` 7,6 % de todas las muestras (~15 % de lo ocupado): **fallos de caché**. La primera
    lectura de cada `GXCommand` (136 bytes, ~45 000 por frame en el mapa) y la cadena de punteros del
    prefetch (`comando → DecodedVerts → emit_cache`).
  - `EmitPrimitive`/`xf::Emit` 4,5 %: primeras lecturas de la entrada de la caché de emisión (fallos).
  - `hash_ram` 3,4 % (hash del contenido de las texturas en la RAM del juego; 62 % desde
    `get_texture`, 38 % desde `efb_copy_done`).
  - `xf::WriteVerts` 2,5 % (vértices de anillo), `efb_copy_done` 2,0 % (**recorría toda la caché de
    texturas, ~420 entradas, por cada copia EFB**), `rph::Level()` 0,9 % (una función no inline con
    un `static` local, llamada varias veces por comando), `GetShaderHash` 0,8 %, `Pal3` 0,7 %.
- El worker no tiene perfil en esos logs (no hay `profile_gxw.txt`).

## 1. Cambios (cada uno con su medida, ver §2)

| Paso | Qué | Dónde |
|---|---|---|
| a1 | Interruptores de depuración (`NWII_RPHASE`, `NWII_DRAWTRACE`, `NWII_DRAWSTATS`, `NWII_ECACHE*`, `NWII_NOEMIT4`) leídos una vez (`InitFlags`) a globales simples | `renderer_gl.cpp` |
| a1 | `efb_copy_done`: índice de la caché de texturas por dirección (`by_addr_`), solo mira las que pueden contener la copia | `texture_cache_gl.{h,cpp}` |
| a1 | Hash de una copia EFB de un frame anterior memorizado por frame (igual que las texturas normales); `hash_ram` con 4 cadenas FNV en paralelo (mismas palabras muestreadas) | `texture_cache_gl.cpp` |
| a1 | Línea `[TCSTAT]` cada 600 frames (hashes por frame, copias EFB, entradas revisadas) | `texture_cache_gl.cpp` |
| a2 | Prefetch adaptado a GPU XF: ya no trae ~1,5 KB de vértices de cada dibujo de display list (GPU XF no los lee), sí la cola de la entrada (`x_init`, `xi`, `x_slot`) | `renderer_gl.cpp` |
| a3 | El worker crea la entrada de la caché de emisión de cada dibujo de display list (máscaras + `xf::Analyze`, función pura de los vértices) antes de entregar el frame | `renderer_gl.cpp` (`GX_PrepDrawForRender`), `gx.cpp` |
| a4 | El worker entrega la lista de entradas por dibujo (`FramePacket::entries`): el render ya no sigue `comando → DecodedVerts → entrada`; prefetch de los comandos 8 por delante; `GetShaderHash` por palabras (era por bytes) | `renderer_gl.cpp`, `gx.cpp`, `gx_state.h` |
| a5 | El primer dibujo del lote va como *base instance* (`gl_BaseInstanceARB`) en vez del uniform `uXDraw`: Mesa ya no resube todo el buffer de constantes del vertex shader (~800 B) en cada `glDraw`. Autoprueba con transform feedback; si falla, uniform | `renderer_gl.cpp`, `tev_shader_gen.{h,cpp}` |
| a6 | Dibujos que no son de display list: el worker hace `Analyze` + `WriteVerts`; el render solo copia las palabras al anillo | `renderer_gl.cpp`, `gx.cpp` |
| a7 | Con worker, el hilo de render **nunca** crea una entrada de la caché de emisión (si faltara, dibuja sin entrada: misma imagen); evita una carrera con el worker | `renderer_gl.cpp`, `gx.cpp` (`GX_RenderWorkerOn`) |
| a8 | Prefetch sin leer la entrada dentro del prefetcher (entradas 6 dibujos por delante) | `renderer_gl.cpp` |
| a9 | `NWII_GPU_XF_MDI=1` (opcional, apagado por defecto): un `glMultiDrawElementsIndirect` por lote, índices de los dibujos de display list fijos en el arena; autoprueba con TF | `renderer_gl.cpp` |
| a10 | Base del lote: prueba al arrancar `gl_BaseInstanceARB` (modo 1), si no un atributo instanciado leído de un buffer "iota" (modo 2, 6 MB), si no el uniform; línea `[GPUXF] draw-base tests:` con lo leído | `renderer_gl.cpp`, `tev_shader_gen.*` |
| a11 | Línea `[PIPE]` cada 2 s: histograma de intervalos entre frames del juego, latencias worker/render, swap, periodo del bucle | `gx.cpp`, `core/main.cpp` |
| a12 | **Hilo AX a prioridad 0x2C en la Switch** (ver §3) | `hle/ax/ax_async.cpp` |
| a13 | Comparación de 48 bytes de la paleta de matrices sin llamar a `memcmp` (`Pal3`/`PalPosNrm`) | `renderer_gl.cpp` |

Interruptores nuevos: `NWII_GPU_XF_BASEINST=0|1|2` (uniform / solo sysval / solo atributo), `NWII_GPU_XF_MDI=1`,
`NWII_AX_PRIO=<prio>`.

## 2. Medidas

### PC, perfil débil (`limc sw`), CPU del hilo de render por frame (`test/pstat.sh`: `perf stat` por hilo)

Los núcleos LP-E cambian de frecuencia entre corridas (0,9-2,4 GHz), así que comparo **ciclos** e
**instrucciones** por frame (frames = los de `[RPROF]`).

| Escena | r13 (base0) Mcyc / Minstr | r14r5 (a8) Mcyc / Minstr | cambio (ciclos) |
|---|---|---|---|
| Mapa Mundo 1 (11 000 dibujos GX) | 10,2-11,4 / 18,6 | 7,6-8,0 / 16,7 | **−27 a −30 %** |
| 1-1 | 7,8-8,2 / 6,8 | 6,5-6,6 / 6,0 | −17 a −20 % |
| Coin Battle 4J | 6,8 / 7,15 | 4,6 / 6,25 | −32 % (1 par) |

A igual frecuencia (pares a 2,3 GHz, `test/pstat_map_2.txt`): mapa 6,26-6,35 → 4,35-4,63 ms/frame (−29 %, a5).
Archivos: `test/pstat_map_*.txt`, `test/pstat_series1.txt`.

### Consola (las corrió el coordinador; `switch/real_logs/bench_r5a6_*`, `bench_r5a9_*`, `bench_r5a10_*`)

- Cinemática del castillo (12 300 dibujos GX/frame): hilo de render ocupado **78-82 % (r13b) → 43-52 %**;
  núcleo 0 del monitor 75-99 % → 26-63 %. Los fps casi no suben (33-45): el límite pasó a ser otro (§3).
- `gl_BaseInstanceARB` funciona en nouveau (a10: autoprueba ok). La autoprueba de a5-a9 fallaba porque la
  primera lectura de transform feedback tras enlazar un programa da ceros en la consola (el sondeo con
  `glDrawArrays` de a10 lo muestra), no por la función.

### Imagen

`checks.sh` 1x (FRAMECHECK + ECACHE): a6 título/mapa/paseo/1-1 **4 × 220/220 idénticos, diff_lines=0,
mismatches=0** (`test/checks_a6_1x.txt`). Con arnés privado (`test/checks_priv.sh`, `test/chk_a9_variants.txt`):
a9 mapa/1-1, a9 con `NWII_RENDER_WORKER=0` mapa/1-1, a9 con `NWII_GPU_XF_MDI=1` título/mapa(/paseo/1-1):
todos idénticos; a10 con el modo atributo forzado: mapa 140/140.
Ojo: FRAMECHECK compara GPU XF contra el camino r8 con la misma caché de texturas, así que no ve los cambios
de la caché de texturas (orden de `stitch_one`, hash memorizado por frame); por diseño dan lo mismo.

## 3. La cinemática del castillo: por qué no sube de ~37 fps (aunque ningún hilo esté lleno)

- `[PIPE]` en PC: los intervalos entre frames del juego salen cuantizados a 16,7/33 ms (el juego pierde VI)
  mientras el render espera paquetes (ready→taken ~0,3 ms): el cuello es el frame del juego, no el solape
  worker/render. (En PC, además, el perfil `sw` reparte 1,5 núcleos entre los tres hilos.)
- En la consola, en esas ventanas: `ax ... mix 280-320 us (max 10-42 ms) wait 57-76 ms/s` (en escenas
  ligeras: mix 92 us, wait 4 ms/s). **El hilo AX corre a prioridad 0x3B** (libnx crea todo `pthread` con
  0x3B: desensamblado de `__syscall_thread_create` en `libnx.a`), por debajo del worker de render (0x2D) en el
  mismo núcleo 2: cuando el worker trabaja, el mezclador AX espera, y el hilo del juego espera en
  `ax::finish()` ~1,5-2 ms por frame; con ~15 ms de CPU del juego, pierde el VI. a12 sube el hilo AX a 0x2C.
- El hilo del juego (creado también con `pthread_create`) está a 0x3B, solo en el núcleo 1.

**Resultado en la consola (a12, `switch/real_logs/bench_r5a12_1108_1905/`):** `ax wait` 55-76 ms/s → **0**; cinemática
del castillo **34-38 → 53-57 fps**, hilos ya solapados (render 71-73 %, juego 78-83 %, núcleo 2 78-82 %).

## 4. Pendiente: la demo del desierto (4 jugadores), 31-41 fps

`[PIPE]` de la consola: frames del juego a 2 VI (intervalo 31 ms; juego ~20 ms de CPU por frame), render
"draw" 17,4 ms de pared con ~11 ms de CPU (≈ 6,5 ms bloqueado dentro del render), worker frame-end→ready 5,7 ms
(latencia; el render solo espera 0,6 ms por paquete). ~5 000 dibujos GX, ~310 GL, 25 copias EFB y 6 stitches
por frame; sin esperas del anillo GPU XF; swap 0,2 ms. Pedido al coordinador: una corrida con
`NWII_RPHASE=1` y `NWII_GPUTIME=1` para repartir el tiempo de pared del render por fase (gldraw, copy, tex,
frame) y ver si la GPU se acerca a 16 ms. Candidatos si es "copy": las 25 copias EFB cambian de FBO y
`GLInvalidate()` hace que el siguiente lote reenvíe todo el estado GL (y Mesa lo revalide entero).

## 5. Siguientes pasos

- Desierto: según RPHASE/GPUTIME (arriba).
- MDI (`NWII_GPU_XF_MDI=1`): ahora que `gl_BaseInstanceARB` funciona en nouveau, probarlo en la consola
  (PC: título/mapa idénticos con MDI).
- El hilo del juego también está a 0x3B (pthread de libnx), solo en el núcleo 1.
- Arnés: `test/pstat.sh` (perf stat por hilo, frecuencia variable de los LP-E: comparar ciclos e
  instrucciones), `test/ab.sh` (con `cb4`), `test/prof.sh` (perf record), `test/checks_priv.sh`
  (checks.sh con `pc/` privado; `PROFILE`, `SCENES`).
