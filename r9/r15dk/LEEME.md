# r15dk: backend deko3d para la Switch (frente R6)

Empezado el 8-nov-2026. Sin commits. Base: `r9/r13` (copia de `nWiiRuntime` con `cp -a --reflink=auto`).
Diseño completo: **[DISENO_DEKO3D.md](DISENO_DEKO3D.md)** (inventario GL → deko3d, memoria, shaders, ventana,
hitos y esfuerzo).

## Estado

| Hito | Estado |
|---|---|
| 1 · diseño | **hecho** (`DISENO_DEKO3D.md`) |
| 2a · uam dentro de la NRO + conversión GLSL + NRO de prueba `dk3test` | **hecho; corrido en la consola (8/9, T4 = bug #29 de deko3d, rodeado)**; v2 por correr |
| 2b · `dkgl` (subconjunto de GL sobre deko3d, cargado como driver de glad) | **funciona en la consola** (v2, 19:12) |
| 2c · integración en el runtime (config, ventana, presentación, CMake de la variante) | **hecha**: `bin/nsmbw_r15dk3d.nro` (+ `_input_test`, `.elf`) |
| 2d · título en la consola | **hecho**: título, menú y mapa dibujados con deko3d (`switch/real_logs/bench_r15dk3d_b_1108_1912/`), sin avisos de validación de deko3d, sin funciones GL sin implementar, 0 draws saltados |
| 3 · paridad por volcados XFB | por hacer |
| 4 · rendimiento (fase B) | por hacer |

## Qué hay

| Ruta | Qué es |
|---|---|
| `ext/uam` | uam 1.1.0 de devkitPro (git, commit 5a5afc2) **con nuestros parches** (`grep -rn "r15dk parche"`): sched determinista, stall 2 entre bloques (regla de Mesa 20.1), sin dual issue, DKSH a memoria. `-DR15DK_UAM_UPSTREAM` los quita |
| `ext/uam_gen` | lo que generaría meson (bison/flex + `ir_expression_operation*.h`, estos con `tools/minimako`) |
| `ext/uamlib` | `CMakeLists.txt` + `uam_api.{h,cpp}` (`uamlib::Compile(stage, glsl, dksh)`) + herramientas de PC (`uam_r15dk` = la herramienta uam con los parches, `uam_dksh_dump`) |
| `ext/deko3d` | fuente de deko3d v0.5.0 (solo para leer; se enlaza la `libdeko3d(d).a` instalada; compilarla necesita dekotools) |
| `ext/swhd` | `third_party/uam` de SwitchWakerHD (MPL-2.0) **solo como referencia de lectura** (sus parches documentados); no se compila |
| `build/build_uamlib.sh host\|nx` | compila uamlib para el PC (`build/uamlib_host`) o la Switch (`build/uamlib_nx/libuamlib.a`) |
| `nWiiRuntime/src/hle/gx/dk/dk_glsl.{h,cpp}` | conversión del GLSL del renderer al que acepta uam (uniformes → bloque std140 con tabla de desplazamientos, bindings, locations) |
| `tools/dkglsl_test.cpp` | PC: convierte y compila con uamlib los 206 shaders (VS clásico, VS GPU XF 0/1, autoprueba, 201 TEV); `build/dkglsl_test/dkglsl_test <dir_fs> <dir_salida>` |
| `tools/shcache_dump.py` | saca los FS de una `shadercache.bin` (NSC2) a ficheros |
| `work/shsrc` | los 201 FS TEV de `switch/NSMBWnx/cache/shadercache.bin`; `work/dkout`: convertidos + DKSH |
| `/home/djvemo/nsmbw-port/tools/dk3test` | NRO de prueba (fuente `source/main.cpp`, `build.sh`) |
| `bin/dk3test.nro` (+ `.elf`) | **para la consola** |

Comprobado en el PC:
- uamlib sin parches = DKSH idénticos byte a byte al `uam` de devkitPro (9 shaders de los ejemplos).
- 206/206 shaders del renderer convertidos y compilados (12 ms de media en el PC).

## Cómo probar dk3test en la consola

1. Copiar `r9/r15dk/bin/dk3test.nro` a `SD:/switch/dk3test/dk3test.nro` y abrirlo desde el hbmenu.
2. Texto en pantalla mientras corren T1-T7 (T1 compila ~210 shaders con uam en la consola), luego un bucle de
   presentación: **fondo verde = todo pasó, rojo = algo falló**; un cuadro con tablero se mueve en círculo (su
   celda roja debe verse arriba a la izquierda del cuadro). + para salir.
3. Resultado: `SD:/switch/dk3test/dk3test.log` (+ `t2_efb.ppm`, `t5_tev.ppm`).

## Resultados de dk3test en la consola (8-nov 18:25)

`switch/real_logs/dk3test_1108/`: 201/201 TEV compilados con uam **en la consola** (46,5 ms de media, 9,3 s en
total, **idénticos byte a byte** a los del PC); VS de GPU XF 2,0 s; fila 0 de la imagen = abajo (como GL);
`gl_VertexID` incluye el vertexOffset; autoprueba GPU XF 0 diferencias (variantes 0 y 1); 2 000 draws = 0,24 ms de
CPU (0,12 µs/draw); presentación 60 fps. Fallos: T4 (alfa del blend = **bug #29 de deko3d**: rodeado reescribiendo
el método FuncAlphaDst a mano con `dkCmdBufReplayCmds`) y un cuelgue al salir (dk3test escribía en la
consola de texto liberada; arreglado). dk3test v2 añade T4b.

## Backend en el runtime (hito 2b/2c)

- `src/hle/gx/dk/dkgl.{h,cpp}`: GL sobre deko3d (texturas, samplers, FBO/renderbuffers, blits, copias, lecturas,
  buffers persistentes y de un frame, VAO, programas/SSO con uam + caché DKSH en disco `cache/dkshcache.bin`,
  uniformes → bloque std140 con push constants, vallas, swapchain 1280×720).
- `src/hle/gx/dk/dkgl_select.cpp`: `GX_GLProc`, `GX_DkActive`, `GX_Host*` (despacho GL/deko3d).
- `main.cpp`/`config.cpp`: `NWII_BACKEND=deko3d` o `[graphics] backend = "deko3d"`; una NRO cuyo nombre lleva
  `dk3d` arranca con deko3d por defecto (`NWII_BACKEND = "gl"` en debug.toml lo fuerza a GL). Si deko3d falla al
  iniciar, vuelve a GL.
- `renderer_gl.cpp`: solo cambian las búsquedas de funciones por nombre y la autoprueba de GPU XF (se salta con
  deko3d).
- `tools/variant_build_dk.sh`: `variant_build.sh` + uamlib + `libdeko3dd.a` (depuración; `DK_LIB=deko3d` = release) al enlazar. Orden:
  `cd /home/djvemo/nsmbw-port/r9 && BASE=/home/djvemo/nsmbw-port/r9/r13 JOBS=2 ~/nsmbw-port/scripts/lim r15dk/tools/variant_build_dk.sh r15dk r15dk3d nx`
- `bin/sd/cache/dkshcache.bin`: semilla de la caché DKSH (209 shaders: modo SSO + VS de GPU XF, precompilados en el PC
  con el mismo uamlib; opcional, ahorra ~11 s en el primer arranque). Se rehace con
  `SSO=1 DKCACHE=bin/sd/cache/dkshcache.bin build/dkglsl_test/dkglsl_test work/shsrc work/dkout2`.

## Binarios para la consola (8-nov 19:00)

| Archivo | Dónde va | Qué es |
|---|---|---|
| `bin/dk3test.nro` (v2, sha256 `d81f96f2…`) | `SD:/switch/dk3test/` | pruebas T1-T7 + T4b: **10/10 en la consola** (19:03, `switch/real_logs/dk3test_v2_1108/`) |
| `bin/nsmbw_r15dk3d.nro` (v2 19:06, sha256 `85f44bbd…`) | `SD:/switch/NSMBW_prueba_dk3d/` (con "dk3d" en el nombre) | **el juego con backend deko3d** (r13 + dkgl); `NWII_BACKEND = "gl"` en debug.toml lo fuerza a GL |
| `bin/nsmbw_r15dk3d_input_test.nro` | solo pruebas guionizadas (hito 3) | |
| `bin/sd/cache/dkshcache.bin` | `SD:/switch/NSMBWnx/cache/` (opcional) | 209 DKSH precompilados en el PC (modo SSO + VS de GPU XF) |

Choque de símbolos al enlazar: `libEGL.a` (Mesa 20.1 de la pila GL) y uamlib (Mesa 19) tienen el mismo código C++
(nv50_ir, clases GLSL). Primer intento (19:00): renombrar los símbolos de uamlib → enlazaba, pero **los grupos COMDAT
(vtables, inline, plantillas) se deduplican por el nombre del grupo** y el enlazador tiraba los de Mesa GL: con
`NWII_BACKEND="gl"` la NRO se colgaba en `eglInitialize` (llamada a 0; consola 19:31,
`switch/real_logs/bench_r5dk_gl_1108_1931/`). Arreglo (19:36): `build/uamlib_rename.sh` hace un enlace parcial de
toda uamlib en un objeto sin grupos (`ld -r --force-group-allocation`) y deja local todo menos `uamlib::Compile`
→ `libuamlib_ns.a` (vtables en el ELF: 568 = 372 de Mesa GL + las de uam; antes 443). `build/build_uamlib.sh nx`
ya lo hace.
`variant_build_dk.sh` compila `input_script.cpp` con `-DNWII_FASTMEM=1` (sin eso el `_input_test` de r13 no compila).

## Primera corrida del juego con deko3d (v2, 8-nov 19:12, OC 1785/768)

`switch/real_logs/bench_r15dk3d_b_1108_1912/` (corrida y capturas del coordinador): arranca con deko3d (`[GL] backend
deko3d: dkgl ...`), la caché DKSH sembrada tenía 207 shaders, el VS de GPU XF se compiló en la consola (1,7 s: la
clave no coincidió con la semilla, ver "Siguiente"), prewarm de 213 programas en 0,64 s (13 FS nuevos con uam, ~45
ms c/u), GPU XF activo. **Título, menú y mapa se ven correctos a simple vista** (`02_title_*.jpg`, `burst_sheet.jpg`).
Media del banco 51,6 fps (mín. 34) — comparar con GL en las mismas escenas lo lleva el coordinador
(`bench_r13b_rafaga_1108_1844` es GL r13b, otra secuencia).

## Rama activa: `r5dk/` = runtime de R5 (r14r5, a12/a13) + dkgl (8-nov 19:26)

- `r5dk/nWiiRuntime` = copia de `r9/r14r5/nWiiRuntime` de las 19:20 (ya con a13: árbol pasado de a12, `checks_a13_priv`
  220/220 en PC) + lo de r15dk fusionado (`git merge-file`, base r13; un conflicto en `main.cpp`: el swap de
  `[PIPE]` ahora llama `HostSwap`). `src/hle/gx/dk` es un **enlace simbólico** a `r15dk/nWiiRuntime/src/hle/gx/dk`
  (una sola copia de dkgl).
- Con deko3d: `NWII_GPU_XF_BASEINST=0` por defecto (la base del lote por el uniform uXDraw = 4 B de push
  constant; las autopruebas de base instance de R5 son de transform feedback). dkgl implementa además
  `glVertexAttribIPointer`, `glVertexAttribDivisor`, `glDraw{Elements,Arrays}InstancedBaseInstance`; sin
  `glMultiDrawElementsIndirect` (deko3d no tiene multidraw).
- **Autoprueba de GPU XF en deko3d** (`renderer_gl.cpp`, `LinkTfDk`/`RunTfDk`): el transform feedback se emula con
  puntos a un destino RGBA32UI de 2048 de ancho (palabras + índice+1 por píxel) y `glReadPixels`; dkgl acepta
  texturas `GL_RGBA32UI` y lecturas `GL_RGBA_INTEGER`.
- Swapchain 1920×1080 si se arranca en sobremesa (1280×720 en portátil; `NWII_DK_FB=720|1080`).
- Binarios (`r5dk/bin/`): `nsmbw_r5dk3d.nro` (+`_input_test`, libdeko3dd con validación) y `nsmbw_r5dk3drel.nro`
  (+`_input_test`, libdeko3d de release, para medir). Compilar: `~/nsmbw-port/scripts/lim build/build_r5dk.sh`.
  Semilla DKSH de esta rama: `r5dk/bin/sd/cache/dkshcache.bin` (`build/dkglsl_test/dkglsl_test_r5`).
- Pruebas pedidas al coordinador: A/B GL vs deko3d con la MISMA NRO (`NWII_BACKEND="gl"` / `"deko3d"`), en
  especial la demo del desierto con 4 jugadores ([PIPE]/[RPROF]); paridad por volcados (trabajos `par_gl`/`par_dk`
  con el `_input_test`, `NWII_XFBDUMP=sdmc:/switch/NSMBWnx/logs/xfbgl|xfbdk`, cada 60 copias,
  `NWII_NOFRAMESKIP=1`, botón 2 cada 20 frames hasta el mapa) → `tools/xfb_compare.py`.

## Siguiente

1. **Paridad (hito 3):** volcados `NWII_XFBDUMP` con `bin/nsmbw_r15dk3d_input_test.nro` y el guion de entrada,
   una vez con `NWII_BACKEND = "gl"` y otra con deko3d, y `python3 -I tools/xfb_compare.py <dir_gl> <dir_dk> <diffs>`.
   Puntos de riesgo: scissor y borrados con scissor, copias EFB (`gl_FragCoord` del shader de copia), blend con
   alfa de destino, depth range, cambio de resolución interna en sobremesa (el blit de profundidad con escala no
   está hecho: se pierde la Z un frame).
2. (Hecho en r5dk) Autoprueba de GPU XF en deko3d.
3. (Hecho 19:20) La semilla no tenía el VS de GPU XF **sin parchear** (`xf::Init` lo compila una vez suelto con
   `CompileVs` antes del modo SSO): `bin/sd/cache/dkshcache.bin` regenerada con `vs_xf0/1` (209 registros). En la
   consola ya está en la caché del primer arranque, así que no hace falta copiarla otra vez.
4. Rendimiento (hito 4): medir render_us GL frente a deko3d en las mismas escenas; luego fase B (push constants
   mínimos, barreras más finas: hoy hay una barrera completa tras cada subida/copia, estados precalculados).
5. (Hecho en r5dk) Sobremesa a 1080p (sin probar: hace falta acoplar la consola) y versión con la biblioteca de
   release.
6. Resolución fraccionaria (frente RES, `r9/r16res/res.patch`, sin LEEME aún): dkgl ya tiene lo que usa —
   `glViewportIndexedf` (viewport en float: `DkViewport` es float), `glQueryCounter` (sin datos: su perfil de GPU
   `NWII_RES_GPUPROF` da 0 con deko3d) y el blit de profundidad con escala de `GX_ResizeEfb` (motor 2D, Z24S8 como
   píxeles de 32 bits, sin filtrar; los renderbuffers de profundidad llevan `Usage2DEngine`). **Al fusionar RES con
   r5dk:** su `SDL_GL_GetProcAddress("glViewportIndexedf")` tiene que pasar a `GX_GLProc(...)` (si no, con deko3d
   no hay viewport en float: SDL no tiene contexto GL).

## Cómo retomar

- Leer `DISENO_DEKO3D.md` §3 (dkgl) y §12 (hitos).
- Recompilar: `cd r9/r15dk && JOBS=2 ~/nsmbw-port/scripts/lim build/build_uamlib.sh nx` (si cambió uam) y
  `JOBS=2 ~/nsmbw-port/scripts/lim /home/djvemo/nsmbw-port/tools/dk3test/build.sh`.
- Prueba de conversión en PC: `build/dkglsl_test/dkglsl_test work/shsrc work/dkout` (se compila con g++, ver la
  orden en el historial de abajo).

## Historial

- 8-nov 18:25: hito 1 (diseño) y 2a (uamlib, dk_glsl, dk3test) hechos. dk3test entregado para la consola.
- 8-nov 23:25: **cielo negro en deko3d** (m1 unificado, demo del cielo con Para-Beetles,
  `bench_m1dk_titulo_1108_2305`): falta la capa del cielo (en GL `bench_r5dkrel_gl_1108_1945/02_title_130.jpg` sale
  bien). Ninguna corrida deko3d anterior había pasado por ese nivel. El log no avisa de nada (ni draws saltados ni
  fallos de memoria; todos los shaders de la caché). Descartado en PC: los 441 TEV de todas las cachés compilan con
  uam; el TEV de alfa de destino (doble fuente) tiene sus dos salidas. Arreglado un riesgo real: un borrado justo
  tras una copia EFB (copia con borrado) no esperaba al blit del motor 2D que leía el EFB (`XferBarrier` también en
  `Clear`). Diagnóstico nuevo en la línea `[DK]`: texturas enlazadas sin imagen (salen negras), draws saltados,
  descriptores en uso, bloques de memoria nuevos. Pedidas corridas de aislamiento (r5dk 23:25 dk, con
  `NWII_GPU_XF=0`, m1 en GL).
- 9-nov 05:45 — **PAUSA (~24 h). Estado y siguientes pasos:**
  - **Variables "0" en la Switch:** `apply_env()` (switch_platform.cpp) BORRA las variables con valor "0" salvo las
    de `keeps_zero`. Por eso `NWII_GPU_XF="0"` se ignoró en `bench_agua_noxf` (GPU XF siguió activo) y
    `NWII_DK_XF_SYNC="0"` tampoco funcionaría. Las isolaciones de agua de las 04:57-05:10 NO discriminan nada:
    todas empezaban por agua_pinguino recién arrancado y salieron limpias por eso. Usar valores no nulos
    (p. ej. aceptar `NWII_DK_XF_SYNC="off"`) o añadir las claves a `keeps_zero` (no es carpeta nuestra: pedirlo).
  - **Explosiones de agua_pinguino: dependen del ESTADO.** Solo salen cuando agua va después de desierto4j (lista
    completa). Con la NRO 05:12 (`bench_dks_lista2_1109_0526`) siguen, menos (capturas 100, 125, 140). La barrera
    dirigida de GPU XF (05:15, modo 1) no lo arregla. Hipótesis: segundo bloque de 64 MB del montón de imágenes (el
    log lo muestra: "bloque nuevo de 64 MB en el montón 1 (2 bloques)" en la línea 4288, ~frame 4800, antes de agua;
    la relación con las explosiones no está comprobada), posiciones
    de anillo/arena, reutilización de huecos de la arena. Siguiente: lista `NWII_FORCE_DEMO="desierto4j,agua_pinguino,agua_pinguino"`
    y registrar el estado de montones, anillo y arena (crecimiento de bloques, vueltas del anillo, offsets) en el
    momento de la primera explosión.
  - **Cielo negro otra vez** en cielo_paratroopas con 05:12 (en la NRO 1da0b795 de las 04:3x estaba bien). El único
    cambio de dkgl entre las dos es la barrera de GPU XF: probarla con `NWII_DK_XF_SYNC="2"`/`"3"` (o "off" cuando
    exista) para ver si la causa es esa barrera.
  - **Cuelgue a ~275 s** (User Break 0x367). SÍ hubo error de cola antes: nwii.log línea 10069 "LA COLA ... ESTÁ EN
    ERROR ... recuperación 1" y 10136 "cola y swapchain recreados"; el juego siguió y luego abortó. El rastro de 64
    operaciones se llenó de borrados con tijera (barra de progreso, `clear 4000 sc 1`): hay que agrupar los
    repetidos. Símbolos con el .elf exacto (c7ae2b2d, mismos bytes que la NRO): la dirección del draw es exacta
    (`dkgl::BindTargets` → `bl dkCmdBufBindRenderTargets`, +0xc5888c); las de libnx/deko3d cuadran con un
    desplazamiento de 0xa00 (svcBreak ← diagAbortWithResult ← `dk::detail::RaiseError`). O sea: deko3d lanzó un
    error dentro de `dkCmdBufBindRenderTargets` (vista de imagen inválida: dk_image.cpp:176 / ImageInfo::fromImageView)
    después de la recuperación, probablemente un destino (FBO/textura) que quedó con imagen no válida tras recrear
    la cola. "Fence::wait" de la simbolización del coordinador es un símbolo mal asignado. De todos modos, las
    esperas de valla de deko3d no abortan (con la cola en error devuelven), solo flush/present de una cola en error.
    Siguiente: validar la vista de imagen antes de `dkCmdBufBindRenderTargets` (Tex ok, w/h > 0, imagen
    inicializada) y registrarla en vez de llamar; compilar una variante de diagnóstico con `DK_LIB=deko3dd`
    (librería debug: imprime dirección y tipo del fallo de GPU) para saber qué falla al cargar.
  - **NRO actual:** m1dk_s 05:12 (`m1dk_s/bin`: .nro ab72e4e2…, _input_test e0f6bbaf…, .elf c7ae2b2d…). deko3d
    sigue sin ser el predeterminado.
- 9-nov 05:15: **explosiones de agua_pinguino aisladas** (consola, m1dk_s, 720p): `NWII_GPU_XF="0"` limpio
  (`bench_agua_noxf_1109_0457`) y `NWII_DK_DRAW_INVALIDATE="1"` limpio en dos vueltas, 42,1 fps de media
  (`bench_agua_inval_1109_0501`; la demo ya va a ~30 fps por el juego, la barrera por draw no cuesta allí: ~61 draws
  GL/frame). O sea: datos de GPU XF (anillo + arena mapeados, leídos con texturas de buffer). Arreglo dirigido en
  dkgl: antes de cada draw que lee **por textura de buffer un buffer mapeado** (solo los draws de GPU XF), la misma
  barrera que DRAW_INVALIDATE (WaitForIdle + invalidar texturas, L2 y descriptores). `NWII_DK_XF_SYNC` para saber
  qué parte hace falta: 1 defecto, 2 solo invalidar (sin WaitForIdle), 3 solo WaitForIdle, 0 nada (= 05:04).
  La línea `[DK]` cuenta "barreras GPU XF". Por qué no basta la de inicio de lista sigue sin estar claro (todas las
  escrituras de la CPU de una lista van antes de enviarla); si 3 limpia y 2 no, es un peligro dentro de la lista en
  la GPU (p. ej. el `uXDraw` por push constant por lote: en GL la consola usa la instancia base, deko3d el uniforme).
- 9-nov 05:04: consola (`bench_dks_lista_1109_0451`, m1dk_s 720p, lista de demos): **cielo negro arreglado**; sin
  cuadros marrones en las nubes; **siguen las explosiones en agua_pinguino** (allí el worker empaqueta muchísimos
  vértices fuera de display lists: packed 1 191-1 619 draws, ring 4 763-6 488 vértices/frame); al cargar el castillo,
  **cola en error** y después aborto (SwapBuffers seguía llamando a `dkQueuePresentImage`, que vacía la cola y deko3d
  aborta). Ahora: si la cola está en error se vuelca un **rastro de las últimas 64 operaciones** (imágenes nuevas con
  hueco/dirección, subidas, blits, copias, borrados, destinos, bloques de memoria, envíos) y se **recrean la cola y el
  swapchain** (descriptores y uniformes se vuelven a enviar; se pierde un frame); tras 8 fallos deja de dibujar sin
  abortar. NRO m1dk_s 05:04 `9f2c0b11…`.
- 9-nov 04:3x: **m1dk_s** = m1dk + `r9/r21s/forcedemo.patch` (demos forzadas `NWII_FORCE_DEMO=...`,
  `NWII_FORCE_DEMO_LOOP`, `NWII_NO_SAVE`): carpeta `m1dk_s/`, `build/build_m1dk_s.sh`; `m1dk_s/bin/nsmbw_m1dk_s.nro`
  (`1da0b795…`, GL por nombre). Con esto el coordinador reproduce a voluntad la demo submarina, el cielo de los
  Para-Beetles y las plataformas en las nubes.
- 9-nov 02:40 (tras otro cuelgue de la PC por RAM; `lim` ahora deja UNA compilación a la vez): con m1dk 01:30 a 720p
  (`bench_m1dk_0130_1109_0148`) no salió la demo submarina; **nuevo**: en el nivel del cielo gris (setas rosas que
  giran) polígonos marrones enormes, girados como la plataforma (02_title_200/210/220): parece geometría (familia de
  las explosiones de vértices o viewport/transformación), no textura. Pedidas corridas de aislamiento: 1x,
  720p + `NWII_NO_FLOAT_VIEWPORT=1`, 720p + `NWII_DK_DRAW_INVALIDATE=1`, 720p + `NWII_GPU_XF=0`, GL 720p.
- 9-nov 01:30: **contenido de otras texturas** en las baldosas y el fondo de la demo submarina con RES 720p
  (`bench_m1dk_t2_1109_0111/02_title_270.jpg`), sin avisos de dkgl. Causa probable: dkgl **reutilizaba los nombres GL**
  al momento (pila LIFO) y no desenlazaba las texturas borradas de unidades/FBO: un nombre borrado que alguien siguiera
  usando pasaba a ser otra textura. Arreglo: nombres nunca reutilizados (como Mesa) y, como GL, borrar una textura o
  renderbuffer la desenlaza de todas las unidades y FBO. Un uso tardío ahora sale en "texturas sin imagen". NRO
  01:30: m1dk rel `f713840b…`.
- 9-nov 01:05: **imagen entera negra ~35 s** en una demo (`bench_dk2359_1109_0049`, 00:31 deko3d): el juego dibujaba
  normal (RPROF ~2 500 draws GX/frame, sin avisos) pero la imagen salía a 0. Causa probable (misma familia que el
  cielo negro): **caché de descriptores de textura (TIC/TSC)**: dkgl reutiliza huecos de descriptor (texturas de
  copias EFB que se recrean) y la CPU escribe el nuevo mientras la GPU acaba el frame anterior; la barrera de inicio
  de lista invalidaba texturas y L2 pero no los descriptores. Arreglo: `DkInvalidateFlags_Descriptors` en esa barrera.
  Interruptores de diagnóstico nuevos: `NWII_DK_NO_XFER_ORDER=1`, `NWII_DK_NO_DEDUPE=1` (además de
  `NWII_DK_DRAW_INVALIDATE=1`). Compilado r5dk y m1dk (`build/build_all_dk.sh`): m1dk rel `82c37906…`.
- 9-nov 00:58: **m1dk** = `r9/r18m` (m1: R5 a13 + RES + W + G5 v3 + A2) con el dkgl actual (00:31): carpeta
  `m1dk/` (copia de r18m; solo cambia `src/hle/gx/dk/dkgl.cpp`; `tools/build_hot.sh` apunta a m1dk), compilado con
  `build/build_m1dk.sh` (build_m.sh con BASE=r14g5 y libdeko3d de release + orden caliente de build_hot.sh;
  eqcheck 60 249 funciones iguales). `m1dk/bin/nsmbw_m1dk_rel.nro` (`0b7f5c6e…`, arranca con GL; deko3d con
  `NWII_BACKEND="deko3d"`), `_input_test`, `.elf`. Desde ahora el coordinador corre todas las pruebas de consola
  con deko3d y manda los avisos `[DK]`/cuelgues.
- 9-nov 00:31: `bench_dk2359_noxf_1109_0011` (23:59, deko3d, sin GPU XF): sin cuelgue en 240 s, imagen limpia; el
  único aviso ("subida … 1x1 en 0x0") era la textura negra de reserva de dkgl (sus campos w/h no se rellenaban;
  la imagen sí era 1x1): arreglado. Causa del cuelgue aún abierta (en esa corrida no hubo fallo de GPU). NRO 00:31
  (rel `bd1d219c…`).
- 8-nov 23:59: **cuelgues "User Break"** con deko3d (`bench_cielo_dk_noxf_1108_2338`, `bench_res_dk_720p_1108_2350`):
  `dkgl::Submit` → `RaiseError` de deko3d: se envía a una cola en error, es decir, la GPU ya había fallado. Causa
  probable: dkgl pasaba los rectángulos de blits/copias tal cual a los motores 2D/de copia, que no recortan como GL
  (p. ej. la copia EFB→XFB con srcY+h > 528 da una y negativa → coordenada enorme → la GPU escribe fuera). Arreglo:
  recorte como GL en `glBlitFramebuffer` (color y profundidad), `glCopyTexSubImage2D`, `glReadPixels` y subidas
  (avisos "[DK] blit recortado…"); si aun así la cola queda en error, se registra y se deja de enviar en vez de
  abortar. NRO 23:59 (rel `61ff4a2d…`). Ojo: las NRO que llamé "23:25"/"23:35" son las de las 23:15 en disco.
- 8-nov 23:35: causa probable del cielo negro: **orden entre transferencias**. TextureCache sube una textura de la
  RAM (CopyBufferToImage, motor de copia) y en seguida le pega una copia EFB (`glCopyTexSubImage2D`, `stitch_into`);
  dkgl solo esperaba a los fragmentos, así que si la subida acababa después borraba lo pegado (la RAM no tiene las
  copias EFB → negro). Arreglo: `TransferOrder()` (DkBarrier_Full si hay una transferencia pendiente) antes de cada
  copia/blit/lectura y de cada subida a una imagen que ya existía. NRO de las 23:15 (rel `abceae64…`; la llamé "23:35"); por verificar.
- 8-nov 22:55: aislamiento: con `NWII_GPU_XF="0"` (sin arreglo) la demo submarina sale limpia
  (`bench_dk_noxf_1108_2235`) → el fallo necesita los datos de GPU XF leídos por la GPU, como decía el diagnóstico.
  Con el arreglo y la barrera peor (`bench_dkfix_prim_1108_2247`): ~85 frames de una demo acuática con bloques de
  hielo, bolas de hielo y burbujas, **todos limpios**. Falta verlo en la demo submarina exacta (Toad pingüino).
- 8-nov 22:38: **fallo de imagen en deko3d**: "explosiones" de vértices en la demo submarina (triángulos estirados
  desde Toad azul, cuadros con textura de roca), a 1x (`bench_r5dkrel_prim_1108_1941/burst_118-139`, peor con
  `NWII_DK_XFER_BARRIER="prim"`) y con RES 1,36x (`bench_res_dk_13636_1108_2227/02_title_140.jpg`). Diagnóstico:
  coherencia de cachés de GPU: la CPU reescribe la arena/anillo de GPU XF para el frame N+1 mientras la GPU acaba el
  N, y la invalidación que deko3d pone tras cada flush no espera a que la GPU esté ociosa → la L2/caché de texturas
  podía quedarse con líneas viejas (Mesa/nvc0 invalida por draw con buffers coherentes). Arreglo: cada lista de
  comandos empieza con WaitForIdle + invalidar caché de texturas y L2, y `dsb sy` antes de enviar.
  Diagnóstico extra: `NWII_DK_DRAW_INVALIDATE=1` (lo mismo antes de cada draw). **Pendiente de verificar en la
  consola; hasta entonces deko3d no puede ser el predeterminado.**
- 8-nov 22:02: el coordinador arregló el banco (debug.toml de la SD desde una copia maestra, restauración
  verificada); `par3_gl`/`par3_dk` en cola con `nsmbw_r5dk3d_input_test.nro` de las 21:38 (c5d86380). **Al retomar:**
  `python3 -I tools/xfb_compare.py switch/real_logs/bench_par3_gl_* switch/real_logs/bench_par3_dk_* <dir_diffs>`.
- 8-nov 21:55: `par2` inválida: el debug.toml de la SD no se restauró tras `par2_gl` y en `par2_dk` las claves salían
  dos veces (toml++ ignora el fichero entero → corrida GL sin opciones); además sin entrada el juego se queda en la
  pantalla de aviso hasta ~110 s. Propuesta `par3`: botón 2 solo en los primeros 300 frames de KPAD,
  `NWII_REALTIME="0"`, volcado cada 60 copias, 180 s, debug.toml restaurado entre trabajos.
- 8-nov 21:45: mapa del Mundo 1 (consola, portátil, OC): a12 GL 46,3 fps de media (mín. 43,1), [PIPE] draw 10,9 ms;
  **deko3d 53,1 fps (mín. 48,0), draw 8,3 ms** (`bench_mapa_a12_1108_2136`, `bench_mapa_dk_1108_2140`). De esos 8,3 ms
  solo 0,48 ms son dkgl (221 draws/frame): el resto es la lógica GX del renderer.
- 8-nov 21:35: **primera paridad en la consola** (`switch/real_logs/bench_par_gl_1108_2128` vs `bench_par_dk_1108_2132`,
  `tools/xfb_compare.py`): copias XFB 60-300 (arranque, pantallas de aviso) **idénticas bit a bit** GL vs deko3d. Desde la
  360 los dos juegos van en distinto estado (personajes en otro sitio, otro mapa/partida): con `NWII_REALTIME=1` el
  reloj es el de pared y las pulsaciones guionizadas se desfasan; no es render. Pedida otra tanda determinista: sin
  entrada, `NWII_REALTIME="0"`, volcado cada 30 copias (trabajos `par2_gl`/`par2_dk`).
- 8-nov 21:30 (tras el cuelgue de la sesión por falta de RAM): dkgl con viewport float, glQueryCounter y
  blit de profundidad con escala (para RES).
- 8-nov 19:45: bancos en consola (portátil, OC 1785, demo del título 150 s; `switch/real_logs/bench_r5dk*`):
  deko3d release 51,1 / 53,1 fps de media (rel, rel2), GL con la misma NRO (19:36, ya sin cuelgue) 49,0; en el
  desierto 4J todo queda a 30 fps por el hilo del juego. Línea `[DK]`: ~140 draws/frame = 0,27-0,31 ms de CPU en
  dkgl (~2 µs/draw), 20-25 KB de push constants, ~45-60 cambios de destino y barreras por frame (copias EFB), envío
  70-90 µs, esperar imagen ~140 µs. `NWII_DK_XFER_BARRIER="prim"`: sin diferencia medible (el render ya no es el
  límite): se queda "full".
- 8-nov 19:36: arreglado el cuelgue del modo GL de las NRO dk3d (grupos COMDAT de las dos Mesa).
  Consola 19:27 (r5dk, deko3d): demo del desierto 4J: [PIPE] draw 4,3-4,7 ms (a12 GL: 18-19 ms), hilo de render
  13-15 % (GL 34-36 %); sigue a 30 fps porque el hilo del juego necesita ~20 ms/frame. Autoprueba GPU XF en
  deko3d: ok (128 ms).
- 8-nov 19:26: rama r5dk (runtime de R5 + dkgl), autoprueba GPU XF en deko3d, 1080p en sobremesa, NRO de release.
- 8-nov 19:12: **v2 del juego funciona con deko3d** (título/menú/mapa).
- 8-nov 19:06: v1 del juego en la consola: applet de error "2--0001 dkSwapchainCreate" (`switch/real_logs/dk3d_juego_1108/`):
  `SDL_CreateWindow` del driver Switch de SDL configura la ventana nativa (1920×1080/EGL) aunque no sea GL, y en
  deko3d cualquier error es fatal (también con `cbDebug`). v2: con deko3d no se crea ventana SDL (el renderer recibe
  una "ventana" ficticia para no ir a headless) y `dkgl::Init` comprueba la ventana nativa antes (si está ocupada,
  vuelve a GL sin applet).
- 8-nov 19:00: variante `nsmbw_r15dk3d` compilada y entregada junto con dk3test v2.
- 8-nov 18:50: resultados de dk3test; bug #29 confirmado y rodeado (FuncAlphaDst a mano); dkgl + integración escritos.
