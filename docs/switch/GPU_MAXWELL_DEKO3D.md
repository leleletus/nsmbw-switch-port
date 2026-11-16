# La GPU de la Switch (Maxwell GM20B), Mesa/nouveau y deko3d: qué conviene para el render de NSMBW

Investigación del 8-nov-2026. Objetivo: decidir cómo bajar el tiempo del hilo de render en la Switch
(hoy ~19-21 ms/frame en un nivel y ~70 ms en el mapa del mundo, medido en consola real), que se va en
CPU: decodificar el FIFO GX, transformar vértices en CPU (posición/normal/texgen), armar lotes
(~11 000 draws GX → ~230 draws GL en el mapa) y dentro de Mesa (`memmove` de `glBufferSubData`,
envío por `nvIoctl`/binder).

**Cómo leer las marcas de fiabilidad**

| Marca | Significado |
|---|---|
| **[C]** | Confirmado en fuente primaria: whitepaper de NVIDIA, código fuente (Mesa de devkitPro, libnx, libdrm_nouveau, deko3d, uam, Dolphin) o documentación oficial del proyecto. |
| **[T]** | Medido o afirmado por un tercero (otro port casero) en su propia consola. Es creíble y muy relevante, pero no lo hemos reproducido. |
| **[E]** | Estimación o cálculo nuestro a partir de datos [C]/[T]. Hay que medirlo antes de fiarse. |
| **[NV]** | No verificado: no encontré fuente sólida. |

Las referencias numeradas `[n]` están al final.

---

## 0. Resumen corto (lo accionable)

1. **La GPU no es el cuello de botella hoy; la CPU sí.** La GM20B tiene 256 núcleos CUDA en 2 SMM y da
   157 GFLOPS FP32 a 307,2 MHz y 393 GFLOPS a 768 MHz [C/E]. Transformar en la GPU unos 100 000
   vértices por frame con texgen costaría del orden de 0,2-0,5 ms [E]. En la CPU (A57 a 1020 MHz)
   ese mismo trabajo cuesta milisegundos.
2. **En la Switch, Mesa guarda todos los buffers en memoria "GART" sin caché de CPU.** El port de
   libdrm_nouveau crea cada buffer con `nvMapCreate(..., is_cpu_cacheable=false)`, y libnx lo marca
   *uncached* con `svcSetMemoryAttribute` [C]. Por eso el `memmove` de `glBufferSubData` escribe en
   memoria sin caché, y cada byte que subimos cuesta caro. Nuestro `BatchVertex` ocupa 72 bytes
   (18 floats) [C, nuestro código]. Un vértice GX crudo suele ocupar 12-24 bytes.
3. **En Switch tenemos activo por defecto el camino lento.** `switch_platform.cpp:575` fija
   `NWII_NORING=1`, así que hacemos `glBufferData(..., datos)` por lote, unas 230 veces por frame.
   Cada llamada crea un recurso nuevo en Mesa, valida estado y copia [C]. El camino rápido de GL en
   este driver es un anillo con escrituras no sincronizadas, o mejor `ARB_buffer_storage`
   persistente y coherente. Este Mesa 20.1 expone las dos cosas [C].
4. **Otros ports de juegos de GameCube a Switch midieron lo mismo.** SwitchWaker (Wind Waker sobre
   GX→WebGPU→GL→Mesa) pasó a deko3d. En el hilo de render bajó de **17-26 µs/draw a 1,8-2,8 µs/draw**,
   la CPU del worker de ~15-20 ms a ~2 ms, y la GPU de 17,5 a 11 ms [T]. En cambio, SwitchWakerHD
   (Cemu→Switch) ganó sólo ~20 %, porque su propio código de traducción dominaba [T]. **La lección
   para nosotros:** deko3d sólo quita el coste del driver. Si el grueso es nuestro decodificado y
   nuestra transformación en CPU, hay que atacar eso primero.
5. **"deko3d no compila shaders en la consola" ya no es un bloqueo.** uam (GLSL→Maxwell) corre en el
   PC. Varios ports (SwitchWakerHD, SwitchWaker, sa2-nx) además lo compilan como biblioteca dentro de
   la NRO para los casos que faltan: unos 70 ms de media por shader en un hilo de fondo [T]. Nuestras
   ~201+ variantes TEV se precompilan offline en segundos [E].
6. **FP16 no está a nuestro alcance.** El hardware hace FP16 al doble de velocidad en pares (vec2) [C],
   pero ni nouveau (Mesa 20.1, `PIPE_SHADER_CAP_FP16 = 0`) ni uam emiten instrucciones FP16
   empaquetadas [C]. `mediump` se ejecuta en FP32. No hay que contar con esa ganancia.
7. **Orden recomendado:**
   - **(b)** anillo persistente y menos bytes por vértice: días de trabajo, ganancia segura.
   - **(a)** transformación y texgen en el vertex shader con una paleta de matrices en SSBO/UBO y un
     "draw id" por vértice: 1-3 semanas, la mayor ganancia.
   - **(c)** backend deko3d: 3-6 semanas; ganancia grande en driver y algo de GPU.
   - **(d)** ubershader: no hace falta salvo como red de seguridad.

---

## 1. La GPU: especificaciones y qué cuesta cada cosa

### 1.1 Datos de hardware

| Dato | Valor | Fuente |
|---|---|---|
| Chip | Tegra X1 (T210), GPU **GM20B** (Maxwell 2ª gen.). `arch 0x120`, `impl 0xB`, `sm_version 0x503` (SM 5.3), clase 3D `0xB197` (MAXWELL_B) | [C] [1][3] |
| Estructura | 1 GPC × **2 SMM** × 128 = **256 núcleos CUDA**. 16 TMU, 16 ROP, 2 *geometry units* (PolyMorph), **256 KB de L2** | [C] [1][3] |
| Pico FP32 / FP16 según NVIDIA | 512 / 1024 GFLOPS. Son valores de la Shield, a ~1 GHz; la Switch no llega a ese reloj | [C] [1] |
| FP16 | FMA/ADD/MUL **vec2 empaquetado**: dos halfs en un registro de 32 bits, doble ritmo | [C] [1] |
| Memoria | LPDDR4 de 64 bits, **compartida con la CPU** (UMA). 25,6 GB/s a 1600 MHz. 4 GB en total | [C] [1][2] |
| Compresión | Delta color compression de 3ª generación y compresión "end-to-end" (ahorran ancho de banda) | [C] [1] |
| Texturas | Formato **block-linear**: GOBs de 64×8 bytes (512 B) apilados en bloques de *block height* GOBs | [C] [12 winsys][25] |

**Relojes de la Switch.** No hay hoja oficial. Los valores vienen de filtraciones de documentación para
desarrolladores (Digital Foundry) y de la tabla de sys-clk [4][5]:

| Modo | GPU | Memoria (EMC) |
|---|---|---|
| Portátil "oficial" | 307,2 MHz o 384 MHz | 1331,2 MHz |
| Portátil máximo (usado por algunos juegos) | 460,8 MHz | 1331,2 MHz |
| Sobremesa | 768 MHz | 1600 MHz |
| Máximo con overclock | 921,6 MHz | 1600 MHz |

En Mariko, sys-clk sube el máximo en portátil a 614 MHz [4]. La consola puede cambiar de reloj sobre
la marcha: en DOOM se registraron 28 cambios en 6 minutos [5].

**Rendimiento teórico por reloj [E].** Fórmulas: FP32 = 256 × 2 (FMA) × f; texel/pixel = 16 × f;
ancho de banda = 8 B × 2 × f_mem.

| GPU | FP32 GFLOPS | FP16 GFLOPS | Texels bilineales/s | Píxeles ROP/s | Ancho de banda |
|---|---|---|---|---|---|
| 307,2 MHz | 157 | 315 | 4,9 G | 4,9 G | 21,3 GB/s (EMC 1331) |
| 384 MHz | 197 | 393 | 6,1 G | 6,1 G | 21,3 GB/s |
| 460,8 MHz | 236 | 472 | 7,4 G | 7,4 G | 21,3 GB/s |
| 768 MHz | 393 | 786 | 12,3 G | 12,3 G | 25,6 GB/s |

El ancho de banda **se comparte con la CPU**: cada byte que la CPU escribe en buffers de vértices sale
del mismo presupuesto que la GPU usa para texturas y framebuffer [1].

### 1.2 Formatos de textura comprimidos nativos

| Formato | Soporte | Nota para nosotros |
|---|---|---|
| BC1-BC7 (DXT) | Sí, en deko3d (`DkImageFormat_RGB_BC1`, …) y en nouveau [C] [8][12] | El CMPR de GX **no es BC1 1:1**. GX interpola los colores intermedios con 3/8-5/8, no con 1/3-2/3, y en el modo de 3 colores el color 3 es la media transparente, no negro transparente. Dolphin lo decodifica en CPU por eso ("GCN decodes differently from PC so we can't use native support") [C] [24]. Pasar CMPR→BC1 directamente daría diferencias pequeñas de color: **no es 1:1**. |
| ASTC (4×4 … 12×12) | Sí. En Mesa 20.1 sólo se habilita para GK20A/GM20B (`chipset 0x12b`) [C] [12 nvc0_screen.c] | No sirve para GX (habría que recomprimir, con pérdida). |
| ETC2/EAC | Sí: `DkImageFormat_*ETC2*` en deko3d; en nouveau, igual que ASTC [C] [8][12] | Igual. |
| I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/C4/C8/C14X2 de GX | No hay equivalente nativo | Se siguen decodificando a RGBA8 (o R8/RG8/RGB565 según el caso). Puede hacerse en CPU una vez por textura, o en GPU con un compute shader, como hace Dolphin con su decodificación en GPU [NV para el detalle de Dolphin]. |

### 1.3 Cuánto costaría hacer la transformación GX en la GPU [E]

Operaciones por vértice de un vertex shader estilo Dolphin [24]:

| Paso | Operaciones (FLOP) |
|---|---|
| Posición: matriz 3×4 | ~12 FMA = 24 |
| Proyección 4×4 | 16 FMA = 32 |
| Normal 3×3 + normalizar | ~9 FMA + ~6 = 25 |
| Por texgen: matriz 2×4/3×4 + post-transform 3×4 + normalizar | ~30-60 |
| Iluminación por canal (NSMBW la usa poco) | Varía |

Total típico: **~150-250 FLOP/vértice**. Para 100 000 vértices/frame (**cifra a medir**: hoy no la
tenemos instrumentada) son ~25 MFLOP. A 307,2 MHz y a pico (157 GFLOPS) eso son 0,16 ms; con una
eficiencia realista del 30-50 %, **0,3-0,5 ms de GPU**.

El *fetch* de atributos tampoco preocupa: 100 000 × 24 B = 2,4 MB/frame, unos 144 MB/s a 60 fps, menos
del 1 % del ancho de banda. En la CPU, el mismo trabajo (más escribir 72 B por vértice en memoria sin
caché) cuesta del orden de 100-200 ns/vértice, es decir **10-20 ms para 100 000 vértices** [E, a
confirmar con el perfilador]. Ésa es la principal razón de la estrategia (a).

---

## 2. Mesa/nouveau en la Switch: por qué cuesta CPU y qué se puede ajustar

### 2.1 La pila real

```
glDraw*/glBufferSubData  (Mesa 20.1.0-rc3, rama switch-20.1.0-rc3 de devkitPro)        [12]
  → state tracker (st/mesa) → driver gallium nvc0 (nvc0_state_validate, nouveau_buffer.c)
  → libdrm_nouveau (port de devkitPro: source/nouveau.c, pushbuf.c)                       [13]
  → libnx nvGpuChannelAppendEntry / nvGpuChannelKickoff → nvioctlChannel_KickoffPb        [14]
  → IPC al servicio nvdrv (sysmodule nvservices) → GPU
eglSwapBuffers → nwindowQueueBuffer → binder/vi (presentación)                            [12 egl_switch.c]
```

- **Cada envío (kick) es una IPC** al servicio `nvdrv` (`KICKOFF_PB`/`SUBMIT_GPFIFO`) [C] [3][14].
  Si el GPFIFO está lleno y nvdrv responde *Busy*, libnx **duerme 100 ms** y reintenta
  (`svcSleepThread(100000000)` en `nvGpuChannelKickoff`) [C] [14]. Es raro, pero sería una pausa
  enorme.
- **Crear un buffer de GPU cuesta varias IPC:**
  1. `memalign`;
  2. `nvMapCreate` (que hace `nvioctlNvmap_Create` + `Alloc` + `GetId`);
  3. `svcSetMemoryAttribute(..., Uncached)`;
  4. `nvAddressSpaceMap`;
  5. un `memset` de todo el buffer.

  Todo esto está en `nouveau_bo_new` del port [C] [13][14]. Mesa reparte los buffers pequeños
  (≤ 2 MiB, `MM_MAX_ORDER 21`) dentro de *slabs* mediante `nouveau_mm`, así que esta creación cara
  sólo ocurre al crecer un slab o con buffers grandes [C] [12 nouveau_mm.c].
- **Los BOs en Switch no tienen caché de CPU.** `nouveau_bo_new` llama a
  `nvMapCreate(..., kind, false)`, y libnx, con `is_cpu_cacheable=false`, hace
  `armDCacheFlush` + `svcSetMemoryAttribute(addr, size, 8, 8)`, donde 8 = `MemAttr_IsUncached`
  [C] [13][14]. Consecuencias:
  - Escribir en buffers mapeados va a memoria sin caché. Escribir de forma secuencial es aceptable;
    **leer de ahí es lentísimo** [E: comportamiento general de ARM con memoria Normal no cacheable;
    no lo he medido en A57].
  - Nunca hay que leer, ni hacer leer-modificar-escribir, sobre memoria mapeada de GL.
- **No hay VRAM.** `vram_size` no se rellena en el port, así que `vram_domain = NOUVEAU_BO_GART` y
  todos los buffers (vértices, índices, UBOs) viven en GART [C] [12 nouveau_screen.c][13].
- **Liberar un BO bloquea hasta que la GPU termina con él.** `nouveau_bo_del` empieza con
  `nouveau_bo_fence_wait(bo, 0)` = `nvFenceWait(fence, -1)` [C] [13]. SwitchWaker lo comprobó en
  consola: destruir cada frame la textura del swapchain hacía que el primer `glClear` del frame
  siguiente esperara a que la GPU terminara el anterior, unos 42-46 ms. Lo arreglaron borrando
  objetos GL de forma diferida, por *fences* [T] [16 SWITCH_PERF_STUDY §6.1]. **Para nosotros:** nada
  de `glDeleteTextures`/`glDeleteBuffers` ni de recrear texturas (EFB, copias) cada frame. Mejor
  reutilizar objetos de un *pool*.

### 2.2 Qué hace exactamente `glBufferSubData` / `glBufferData` en este Mesa [C] [12]

- **`glBufferData(target, size, data, usage)` con el mismo tamaño y uso que antes:** se reutiliza el
  recurso, con un `buffer_subdata` con `DISCARD_WHOLE_RESOURCE` (`st_cb_bufferobjects.c`).
  **Con otro tamaño:** se suelta el recurso y se crea uno nuevo (`resource_create`), y además el
  state tracker tiene que revalidar los *bindings*.
  - **Nuestro caso con `NWII_NORING=1`:** el tamaño cambia en cada lote, así que creamos un recurso
    nuevo unas 230 veces por frame.
- **`glBufferSubData` a un rango que la GPU todavía no ha usado:** nouveau lo trata como
  `DISCARD_RANGE | UNSYNCHRONIZED` gracias a `valid_buffer_range`, y escribe **directamente** en el
  BO de GART con un `memcpy` a memoria sin caché, sin *staging* (`nouveau_buffer.c`, líneas
  ~399-405 y ~466). Así funciona nuestro anillo R4 (`NWII_NORING=0`): es el camino bueno de GL.
- **`glBufferSubData` a un rango en uso por la GPU:**
  - **≤ 1024 bytes** (`transfer_pushbuf_threshold = 1024` en chipset ≥ 0xe0): los datos se copian
    *inline* dentro del pushbuffer (`push_data`). Son dos copias más por byte.
  - **Más grande:** se reserva *staging* en GART, se copia ahí y la GPU copia de vuelta
    (`copy_data`). Una copia de CPU más una de GPU.
  - Si no hay *discard*, puede llegar a **esperar a la GPU** (`nouveau_buffer_sync`).
- **`glMapBufferRange` con `GL_MAP_UNSYNCHRONIZED_BIT`:** devuelve el puntero directo al BO (línea
  ~466). Es el mismo camino que usa Dolphin [24].
- **`glBufferStorage` con `MAP_PERSISTENT|MAP_COHERENT`:** el buffer va a GART
  (`PIPE_RESOURCE_FLAG_MAP_PERSISTENT/COHERENT`) y se mapea **una sola vez**. Mesa 20.1 nvc0 expone
  `PIPE_CAP_BUFFER_MAP_PERSISTENT_COHERENT` [C] [12 nvc0_screen.c]. Así se ahorran las llamadas
  map/unmap y el objeto *transfer* de cada subida, aunque el `memcpy` a memoria sin caché sigue ahí.
  La sincronización pasa a ser nuestra (`glFenceSync` por segmento del anillo). Dolphin elige este
  modo, `BufferStorage`, siempre que el driver lo soporta [C] [24 OGLStreamBuffer.cpp].

### 2.3 Coste por draw medido por otros sobre este mismo Mesa 20.1

| Proyecto | Qué midieron | Fuente |
|---|---|---|
| SwitchWaker (Wind Waker, GX→Dawn→GLES→Mesa) | `glDraw*` **3,6-3,9 µs** típico (3,4-18 µs según el estado sucio; el grueso es `nvc0_state_validate`). Un cambio de FBO cuesta decenas de µs. El worker completo ~13 µs/draw, incluido Dawn | [T] [16 PERF_STUDY §2.4, MIGRATION_PLAN §1.2] |
| SwitchWaker, A/B GL vs deko3d (portátil, 460,8 MHz) | **17,4-26,3 µs/draw (GL) → 1,8-2,8 µs/draw (deko3d)**. CPU del worker 14,5-19,7 → 1,5-2,1 ms. GPU 13,7-21,0 → 7,5-12,7 ms | [T] [16 MIGRATION_PLAN "Phase 3 console results"] |
| SwitchWakerHD (Wind Waker HD, Cemu→Switch) | GL 6,2-8,4 µs + ~2,5 µs del hilo GL de Mesa → deko3d 5,6-7,2 µs/draw. fps medio 28,1 → 29,6-29,9. **GPU sin cambios (~33 ms)** | [T] [16 MIGRATION_PLAN §1.3][17] |
| sms-switch (Super Mario Sunshine) | "Mesa nouveau ~30 µs de CPU por draw; 24-31 ms de hilo GL con ~800 draws". El repositorio devuelve 404 hoy; el dato sale del índice del buscador | [T/NV] [21] |
| banjo-nx (vía la guía de sa2-nx), NVK en Switch | ~3 µs por bind de pipeline, 2,4-2,9 µs por draw, 1,3-1,6 µs por actualización de descriptor/UBO | [T] [18] |

**Qué significa para nosotros [E].** Con unos 230 draws GL, el coste de `glDraw*` en sí es de unos
1-7 ms por frame. El resto del tiempo "dentro de Mesa" sale de cómo subimos los datos (recursos nuevos
por lote, copias a memoria sin caché, IPC al crecer slabs) y de los cambios de programa y uniformes.

SwitchWaker vio además que `glBindBufferRange` por draw es caro en nvc0: vuelve a enlazar todos los
UBO de las dos etapas y emite `MEM_BARRIER 0x1011`, que invalida cachés en la GPU. Lo resolvieron con
una "ventana" UBO de 64 KiB y un offset en un uniforme [T] [16 PERF_STUDY §7.2].

### 2.4 Qué ofrece este Mesa (nvc0, Mesa 20.1) [C] [12 nvc0_screen.c]

| Funcionalidad | Estado | Comentario |
|---|---|---|
| GLSL / GL | `GLSL_FEATURE_LEVEL` 430 (core y compat) | GL 4.3 más extensiones |
| `ARB_buffer_storage` (persistente/coherente) | Sí (`PIPE_CAP_BUFFER_MAP_PERSISTENT_COHERENT`) | Recomendado para el anillo |
| Invalidar buffers (`glInvalidateBufferData`) | Sí (`PIPE_CAP_INVALIDATE_BUFFER`) | |
| `glMultiDrawElements`, `glMultiDrawElementsIndirect`, `ARB_indirect_parameters`, `ARB_shader_draw_parameters` (`gl_DrawID`, `gl_BaseInstance`) | Sí (`MULTI_DRAW_INDIRECT[_PARAMS]`, `DRAW_PARAMETERS`) | Útil para pasar un "draw id" sin tocar los vértices |
| Instancing, `ARB_base_instance` | Sí (`TGSI_INSTANCEID`, `START_INSTANCE`, `VERTEX_ELEMENT_INSTANCE_DIVISOR`) | |
| UBO | 64 KiB por UBO (`NVC0_MAX_CONSTBUF_SIZE`), alineación de offset 256 | Caben de sobra las matrices XF (≈ 2,4 KB) |
| SSBO en vertex shader | Sí (GL 4.3) | Para paletas grandes y "vertex pulling" |
| Dual-source blending | 1 RT | Útil para ciertos modos de alpha de GX |
| FP16 en shaders | **No** (`PIPE_SHADER_CAP_FP16 = 0`) | `mediump` = FP32 |
| `glthread` (hilo de Mesa) | **No disponible** [C/E]: `egl_switch.c` nunca llama a `st_start_thread` | No hay hilo de driver gratis |

### 2.5 Ajustes por variables de entorno (las que existen de verdad en este Mesa)

| Variable | Efecto | Fuente |
|---|---|---|
| `MESA_NO_ERROR=1` | Desactiva la comprobación de errores de la API (`KHR_no_error`). Ahorra CPU por llamada; si usamos mal la API, el resultado es indefinido. Las propias muestras de switchbrew lo sugieren "para producción" | [C] [15][10][12 main/context.c] |
| `NV50_PROG_OPTIMIZE` (0-3, por defecto 3) | Nivel de optimización del compilador nv50_ir. Bajarlo acelera la compilación y empeora el código | [C] [12 nvc0_program.c] |
| `NV50_PROG_DEBUG`, `NV50_PROG_CHIPSET`, `NV50_PROG_USE_NIR` | Depuración y frontend NIR. `USE_NIR` era experimental en 20.1 | [C] [12] |
| `NOUVEAU_MESA_DEBUG`, `MESA_VERBOSE`, `EGL_LOG_LEVEL` | Logs | [C] [10][12] |
| `MESA_SHADER_CACHE_DISABLE`, `MESA_GLSL_CACHE_DIR` | Caché de shaders en disco | [C] [15] |
| `GALLIUM_HUD` | HUD de estadísticas; no probado en Switch | [C] [15] / [NV] en Switch |

**No existen** perillas para el coste por draw, el tamaño del pushbuffer ni la sincronización de
buffers. Esas cosas sólo se cambian con el patrón de uso de la API o parcheando Mesa, como hizo
SwitchWaker con 4 parches y 2397 líneas [T] [16].

### 2.6 Trampa al medir la GPU

SwitchWaker comprobó que `GL_TIME_ELAPSED` de nouveau en Tegra X1 **marca por debajo de lo real, por
un factor de ~1,63**: el PTIMER va a 19,2 MHz pero se interpreta como si fuera de 31,25 MHz, y
nvgpu de Linux reescala por 31,25/19,2 = 1,6276. Hay que multiplicar los tiempos de GPU medidos por
1,6276 [T] [16 PERF_STUDY §8.1].

---

## 3. deko3d

### 3.1 Qué es

- API gráfica de bajo nivel para la Tegra X1, hecha por fincs/devkitPro **a partir de la ingeniería
  inversa de NVN** (la API oficial), aunque no es un clon de ella. Licencia Zlib. Se instala con
  `pacman`, en el grupo `switch-dev` [C] [6].
- Su modelo se parece a Vulkan: dispositivo, colas, *command buffers*, memoria gestionada por la
  aplicación y estado de GPU por cola [C] [6][7].
- Hay una versión de depuración, `libdeko3dd`, con validación, y una de release, `libdeko3d`, sin
  comprobaciones [C] [7].
- Según su README cubre "~80 %" de las funciones habituales. Faltan, entre otras, transform feedback,
  multidraw, render condicional, texturas sparse y una herramienta de conversión de texturas [C] [6].
- Su README afirma "CPU overhead sustancialmente menor que OpenGL", Zcull, *tiled cache*, render
  targets comprimidos y optimizaciones del compilador que nouveau no tiene en Kepler/Maxwell.
  **No publica benchmarks** [C] [6]. Los números reales son los de los ports de §2.3 [T].

### 3.2 Modelo de la API [C] [7][8]

| Concepto | Detalle | Por qué importa |
|---|---|---|
| `DkMemBlock` | Bloques de memoria con flags `CpuUncached`/`CpuCached`, `GpuCached`/`GpuUncached`, `Code` (shaders) e `Image`. Tamaño múltiplo de `DK_MEMBLOCK_ALIGNMENT` (0x1000). **Cada bloque gasta recursos del SO**: reservar bloques grandes y subdividirlos uno mismo | La memoria la gestionamos nosotros: un anillo de vértices es un bloque `CpuUncached\|GpuCached` en el que escribimos con un offset |
| `DkCmdBuf` | Graba comandos en memoria que le damos (`dkCmdBufAddMemory`, `GpuCached\|CpuUncached`). Si se acaba, llama a `cbAddMem`. `dkCmdBufFinishList` → `DkCmdList`. Un cmdbuf por hilo | Puede grabarse en paralelo |
| `DkQueue` | Junta las listas y las envía en **un solo lote** al hacer `dkQueueFlush`, al llenarse o al pasar `flushThreshold`. Tras cada flush invalida las cachés de imagen, shader, descriptores y L2 | Un kick (una IPC) por lote, no por draw |
| Uniformes | `dkCmdBufBindUniformBuffer(s)`. **`dkCmdBufPushConstants`** escribe los datos *inline* en el command buffer, en orden con los draws ("todos los UBO tienen semántica de push constant", hasta 64 KiB; alineación 0x100) | Actualizar sólo las filas de matriz que cambian entre dos draws GX, sin buffers intermedios |
| Vértices | `bindVtxAttribState`, `bindVtxBufferState`, `bindVtxBuffers`, `bindIdxBuffer`, `draw`/`drawIndexed`/`*Indirect`. Formatos 8/16/32 bits Unorm/Snorm/Uint/Sint/**Uscaled/Sscaled**/Float y 10-10-10-2 | Los formatos GX (u8/s8/u16/s16/f32) se leen tal cual; el desplazamiento de punto fijo (`frac`) se aplica con un multiplicador uniforme |
| Primitivas | `DkPrimitive_Quads`, `TriangleFan`, … nativas | GX dibuja muchos quads: no hace falta generar índices |
| Imágenes y descriptores | `DkImageLayout` + `DkImage` (block-linear, opcionalmente con compresión HW), conjuntos de descriptores de imagen y sampler enlazados por dirección de GPU | Texturas GX decodificadas una vez, en un *pool* |
| Captura y repetición | `dkCmdBufBeginCaptureCmds`/`EndCaptureCmds`/`ReplayCmds`: graba palabras de comandos crudas y las reinyecta "con cero coste computacional" | Bloques de estado que se repiten (p. ej. el setup de cada pasada) |
| Sincronización | `DkFence`: espera en CPU (`dkFenceWait`) o en GPU (`dkQueueWaitFence`). `DkVariable`. `dkQueueSignalFence(flush=true)` vuelca cachés | Una fence por segmento de anillo y por frame en vuelo |
| Presentación | `dkSwapchainCreate` sobre el `NWindow` de libnx; `dkQueueAcquireImage`, `dkQueuePresentImage` (hace flush) y `dkSwapchainSetSwapInterval` (vsync) | El mismo binder/vi que usa EGL: la presentación cuesta igual |
| Shaders | **Sólo código nativo Maxwell (SM 5.3)** en formato DKSH: "no hay forma de usar GLSL o SPIR-V en tiempo de ejecución" con la biblioteca. Se compilan con **uam** | Ver §3.4 |

### 3.3 Datos dinámicos de vértices de forma eficiente (patrón recomendado)

1. **Un bloque grande `CpuUncached | GpuCached`** dividido en N segmentos, N = frames en vuelo + 1
   (2-3). En la escena "Cube" de los ejemplos oficiales, los pools de datos usan exactamente
   `CpuUncached|GpuCached` [C] [10].
2. **Escribir los vértices directamente en el segmento** desde el decodificador del FIFO, sin un
   `std::vector` intermedio, y siempre de forma secuencial y sin leer.
3. Al final de cada segmento o frame, **`dkQueueSignalFence`**. Antes de reutilizar el segmento,
   **`dkFenceWait`** sobre su fence. Las fences no pueden cambiar de dirección mientras estén
   pendientes [C] [7][11].
4. Las matrices y constantes GX por draw van con **`dkCmdBufPushConstants`** (inline, ordenadas) o en
   un anillo de UBO con `bindUniformBuffer` por offset.
5. El ejemplo `SampleFramework/CCmdMemRing.h` de switch-examples implementa justo un anillo de
   memoria de comandos por frame [C] [10]. SwitchWakerHD usa un anillo de comandos de 4 × 4 MiB y
   anillos de *stream* por slot, de varios MiB por tipo [T] [16 MIGRATION_PLAN §4].

**Trampas conocidas** [C/T] [11][16]:

- `dkCmdBufClear` no libera la memoria: rebobina a la última porción.
- Si los recursos por frame sólo están protegidos por acquire/present, puede haber *flicker*: hay que
  usar fences propias.
- `dkCmdBufPushData` no ordena como un push constant: necesita sus propias barreras.
- Los bits de relleno de `DkVtxAttribState` deben estar a cero, o hay error de método de GPU.
- Cuando la GPU falla, la cola queda inservible y el SO puede matar el proceso en segundos.

### 3.4 Shaders: el problema de los TEV generados en tiempo de ejecución

**uam** es el compilador [C] [9]: frontend GLSL de Mesa 19.0.8 + backend nv50_ir de nouveau, con
*dual-issue* Maxwell añadido. Salida: `.dksh`. Etapas: vert, tess, geom, frag, comp.

Diferencias con GL que nos afectan [C] [9]:

- Los *bindings* de UBO, SSBO y samplers **tienen que ser explícitos**: `layout(binding=N)`.
- Los **uniformes sueltos** (fuera de un bloque UBO) **dan error**. Hay que pasar todos los
  uniformes de `tev_shader_gen.cpp` a un bloque UBO.
- La división y el módulo enteros con divisor no constante se convierten a división en float. Hay
  que revisarlo si el TEV generado los usa.
- `gl_FragCoord` sigue el origen del dispositivo; `pixel_center_integer` da error.
- No hay *linking*: los varyings se emparejan por `layout(location)`.
- Hasta 16 UBO, 16 SSBO y 32 samplers por etapa.

Opciones para nuestras ~201+ variantes TEV:

1. **Precompilar todas en el PC con uam**, en el build, y meter un *pack* `.dksh` en el romfs.
   SwitchWaker compila ~3000 pipelines (1880 DKSH tras deduplicar, 6 MiB) en ~10 s, y la consola
   los carga todos en 0,26-1,1 s [T] [16]. Para nosotros harían falta:
   - generar el GLSL de cada variante a partir de los estados TEV registrados (ya tenemos
     `shadercache.bin` y el generador);
   - una post-pasada de *bindings*.
2. **uam como biblioteca dentro de la NRO** para variantes nuevas. SwitchWakerHD lo hizo con
   7 parches: mutexes, salida a memoria, sin `exit`, planificación determinista y **sin
   dual-issue**, porque el *dual-issue* de uam corrompía warps.
   - Compila **70 ms de media, p99 426 ms** por shader en un hilo de fondo.
   - Mientras tanto, se salta el draw o se usa otro camino [T] [16 MIGRATION_PLAN §1.3, §4, §7].
   - Licencia: uam es zlib + MIT; SwitchWakerHD es MPL-2.0 por archivo [T] [16 §4].
3. **Ubershader** (ver §4 (d)): evita compilar, pero cuesta GPU.

La recomendación es 1 + 2: offline todo lo conocido, y uam en la consola como red de seguridad.

**Bugs conocidos de deko3d 0.5.0 y uam** [T] [16 MIGRATION_PLAN §7 y "Phase 3 console results"]:

- `dkCmdBufBindBlendStates` programaba el factor destino de alfa con el de color (devkitPro/deko3d#29).
- Imágenes BC con un nivel 0 de 6-8 filas de bloques se solapaban con el mip 1.
- `dkCmdBufCopyImage` ignora `srcRect->z`.
- El orden del *clamp* de LOD estaba invertido.
- La biblioteca de release aborta sin llamar a `cbDebug`.
- uam omitía un *stall* al final de un bloque (parche 8 de SwitchWaker).
- El *dual-issue* de uam producía *specks*.
- Hay un issue abierto (#10) sobre texturas muy pequeñas (32×8) que se cortan [C] [11].

### 3.5 ¿Cuánto código es un renderer GL→deko3d?

| Referencia | Tamaño |
|---|---|
| Ejemplo oficial "cubo" | ~380 líneas, más el *SampleFramework* (pools de memoria, anillo de comandos, descriptor sets, carga de shaders; ~30 KB) [C] [10] |
| SwitchWakerHD (backend completo para Cemu) | memory 330 + descriptors 249 + gpu_sync 386 + formats 235 + shaders 1569 + backend 1570 + draw 1841 + surfaces 1853 + overlay 192 + capture 1015 ≈ **9 000 líneas** [T] [16 §4] |
| SwitchWaker | Plan de 6-9 semanas "efectivas"; los agentes de HD lo hicieron en ~1 día de reloj con ~12 ciclos en consola [T] [16 §0] |
| Nuestro `renderer_gl.cpp` | 3813 líneas [C, nuestro código] |

Un backend deko3d equivalente rondaría **3000-5000 líneas**, más la herramienta offline de DKSH
[E]. Hay que añadir capturas para depurar, porque sin Mesa no hay errores GL: `libdeko3dd`,
`dkQueueIsInErrorState` y volcados de imagen [T] [16 §7.14].

### 3.6 Proyectos que usan deko3d (y con qué resultado)

| Proyecto | Qué hace | Resultado | Fuente |
|---|---|---|---|
| **SwitchWaker** (Wind Waker GC, GX) | GX→WGSL→Tint GLSL→uam→DKSH. Quitaron el renderer GL el 9-oct-2026 | Worker 15-20 ms → ~2 ms. GPU 17,5 → 11 ms. 30 fps estables en las escenas medidas | [T] [16] |
| **SwitchWakerHD** (Wind Waker HD, Wii U) | Recompilación estática con un backend tipo Cemu sobre deko3d | +20 % de CPU por draw; GPU igual | [T] [17] |
| **sa2-nx** (Sonic Adventure 2, recompilación de X360) | D3D9→renderer propio→deko3d; uam en consola una vez por variante | Hilo de render 17,6 → 11,1 ms con ~1400 draws, tras optimizar *su* código (deltas, hashing, NEON). GPU 5-6 ms a 460,8 MHz | [T] [18] |
| **melonDS Switch** | Renderers 2D/3D y GUI en deko3d "para bajar CPU frente a OpenGL" | Menos overhead y binario mucho menor (cualitativo) | [T] [20] |
| **DrasticDS_nx** | deko3d por defecto, con Vulkan/NVK, GL/nvc0 y Zink como alternativas | Sin cifras | [T] [19] |
| **SlipstreamCE** (Halo CE), **sdl3-nx** (SDL_GPU sobre deko3d), **sms-switch** (Sunshine; PR "dkgl", hoy 404) | — | Sin cifras verificables | [T/NV] [21][22] |
| **RetroArch Switch** | Usa la pila de libnx/Mesa (GL) | No encontré un driver deko3d en RetroArch | [NV] [26] |

**Emuladores de Switch (Ryujinx, yuzu):** no usan deko3d, porque emulan la GPU Maxwell en el PC.
Son útiles como documentación del hardware: layout block-linear (`BlockLinearLayout.cs` de Ryujinx,
`decoders.cpp` de yuzu) y clases 3D [C] [25].

---

## 4. Estrategias para nuestro caso, ordenadas por ganancia/coste

### 4.0 Referencia: qué hace Dolphin [C] [23][24]

- **Vertex loader JIT** (x64 y ARM64): convierte los formatos de vértice GX (big-endian, indexados,
  `frac`) al formato nativo del host en CPU. **No transforma**: la posición queda en espacio de
  modelo y se guarda el índice de matriz (`posmtx`) como atributo.
- **Transformación en GPU:** el vertex shader generado lee `I_TRANSFORMMATRICES[posidx]` (64 float4),
  `I_NORMALMATRICES` (32 float4) y `I_POSTTRANSFORMMATRICES` (64 float4) de un UBO, y hace en GPU la
  posición, la normal, la iluminación y el texgen (`VertexShaderGen.cpp`, `ConstantManager.h`). Los
  draws con índice de matriz por vértice (*skinning*) salen gratis.
- **Subida de datos:** `StreamBuffer` con `glBufferStorage(PERSISTENT|COHERENT)` y *fences*
  (`OGLStreamBuffer.cpp`), con alternativas *MapAndSync*/*MapAndOrphan*.
- **Ubershaders** (2017): un intérprete de TEV/XF dentro de shaders enormes. En modo *exclusive*
  "es una carga masiva para la GPU", y en NVIDIA Shield TV (Tegra X1 a ~1 GHz) "puede ir a velocidad
  completa en juegos muy básicos". El modo *hybrid* usa el ubershader mientras compila el
  especializado en segundo plano [C] [23].

### 4.1 Tabla de decisión [E salvo citas]

| # | Estrategia | Ganancia esperada | Coste | Riesgo / fidelidad 1:1 |
|---|---|---|---|---|
| **b** | **Arreglar la subida de datos en GL.** Pasos: (1) por defecto, anillo persistente con `glBufferStorage(MAP_WRITE\|PERSISTENT\|COHERENT)` y `glFenceSync` por segmento, en lugar de `NWII_NORING=1`; (2) escribir los vértices **directamente** en el puntero mapeado; (3) **vértices compactos**: posición en 3 floats, color en 4×u8 normalizado, UV en float o half (half sólo como almacenamiento: el fetch lo convierte a FP32; ver §1.2 y [8]), sin campos vacíos; (4) `MESA_NO_ERROR=1`; (5) no borrar ni crear objetos GL por frame | Elimina ~230 creaciones de recurso por frame, una copia CPU por byte y la mitad o más de los bytes escritos en memoria sin caché. Del orden de **varios ms** en el nivel y bastante más en el mapa | **Bajo: 1-3 días** | Ninguno de fidelidad. Ojo: el anillo R4 tuvo un *crash* en citron (emulador), no en hardware. Con *persistent* + fences el diseño es el estándar (Dolphin) |
| **a** | **Transformación y texgen en el vertex shader** (modelo Dolphin): (1) subir los atributos GX convertidos pero **sin transformar**; (2) guardar cada "snapshot" de matrices XF de un draw GX en un **SSBO/UBO-anillo**, con unos 100 B-2,4 KB por cambio; (3) pasar un **draw id** (u8/u16 por vértice, o `gl_BaseInstance`/`gl_DrawID`) para que el shader encuentre sus matrices; (4) mantener el *batching* actual de ~230 draws | Quita el grueso del trabajo de CPU por vértice: transformación, normal, texgen y escritura de 72 B. Probable **la mayor reducción** en el mapa del mundo (11 000 draws GX) | **Medio: 1-3 semanas.** Hay que tocar `fifo_parser.cpp` (hoy transforma en el *parse*) y `tev_shader_gen.cpp` (VS) | Fidelidad: la GPU calcula en FP32 con FMA, igual que Dolphin, que se toma como referencia; puede haber diferencias de ±1 ULP frente a la CPU. Riesgo medio con matrices "banked" por vértice, que se resuelve con el índice por vértice como Dolphin |
| **c** | **Backend deko3d** (sustituye a Mesa). Con `pushConstants` para las matrices, anillos propios, quads nativos y DKSH precompilados | Driver: de ~4-30 µs/draw a ~2 µs. Sin `memmove` de Mesa ni validación. Una IPC por lote. En SwitchWaker también bajó la GPU (17,5 → 11 ms), porque quitaron barreras y validación. En nuestro caso, **0,5-7 ms/frame más lo de las subidas** [E] | **Alto: 3-6 semanas.** Más una herramienta DKSH y, opcionalmente, uamlib en la NRO | Fidelidad igual que GL si los shaders son los mismos. Hay que heredar los arreglos de bugs de deko3d/uam (§3.4) |
| **d** | **Ubershader TEV** (un solo shader que interpreta el estado TEV por uniformes) | Evita compilar variantes, pero **cuesta GPU**: en TX1, Dolphin dice que sólo va a velocidad completa en juegos muy básicos [23] | Medio | Sólo merece la pena como *fallback* (modo hybrid) si no podemos precompilar. Con 201+ variantes conocidas, **no hace falta** |

**Orden recomendado: b → a → c.** (b) se mide en días y puede revertirse con `env.txt`. (a) ataca el
coste dominante (nuestra CPU). (c) vale la pena cuando (a)+(b) hayan reducido lo nuestro y lo que
quede sea driver. Hacer (c) antes que (a) repetiría la experiencia de SwitchWakerHD: poco cambio
porque el coste propio domina [T] [16 §1.3].

**Complementario (independiente de la API):**

- **Repartir trabajo entre núcleos.** Las aplicaciones tienen 3 núcleos A57 [NV: dato de switchbrew,
  no revisado aquí]. Se puede decodificar el FIFO y convertir vértices en un hilo, y emitir GL o
  deko3d en otro.
- **Copias por delta y menos hashing.** sa2-nx ganó 6,5 ms de hilo de render sólo con esto y sin
  tocar el driver [T] [18 §7].

### 4.2 Detalles concretos de (a) para GX

- **Endianness:** los datos GX son big-endian. Hay dos formas de resolverlo:
  - con `bswap` en la conversión en CPU, como Dolphin;
  - leyendo los arrays GX crudos desde un SSBO y haciendo el swap en el shader (*vertex pulling*).
    Esto ahorra la copia en CPU, pero los atributos indexados GX tendrían que resolverse en el
    shader [E].
- **Formatos `frac`:** subirlos como `Sscaled/Uscaled` (GL: atributos enteros sin normalizar
  convertidos a float) y multiplicar en el shader por `2^-frac`, que llega como uniforme [C para los
  formatos: 8].
- **Matrices:** la memoria XF tiene 256 floats de pos/tex, 96 de normal y 256 de post-transform
  [C] [24 ConstantManager.h]. En GL, **no** hay que hacer `glBindBufferRange` por draw, porque nvc0
  revalida todos los UBO e invalida cachés (§2.3). Es mejor una ventana fija y un offset por vértice
  o por draw. En deko3d, `pushConstants` de las filas que cambian.

---

## 5. Fortalezas y debilidades de la GPU de la Switch para esta carga

### 5.1 Lo que es barato

- **ALU de vértices.** Un GX "pesado" no llega ni de lejos al presupuesto de vértices de Maxwell (§1.3).
- **Texturas pequeñas sin mipmaps (lo típico de Wii).** 16 TMU dan 4,9 Gtexels/s incluso a
  307,2 MHz [C/E]. SwitchWaker confirma que las texturas no eran el problema [T] [16 PERF_STUDY §8.3].
- **Blending y escritura de color.** 16 ROP y compresión de color [C] [1].
- **Render-to-texture y copias EFB a resolución Wii.** Una copia 640×528 RGBA8 son ~1,35 MB de
  lectura y escritura, unos 0,1 ms de ancho de banda [E]. Con deko3d hay motor 2D y de copia, y en
  memoria unificada no hay subida por PCIe. Las **lecturas a CPU** (`glReadPixels` síncrono, *EFB
  peek*) sí cuestan: paran la tubería hasta que la GPU acaba. Hay que hacerlas asíncronas (PBO + fence)
  o evitarlas.
- **Cambiar de shader con deko3d.** Un bind de shaders son unas pocas palabras de comando [C] [7].
  En GL/nvc0 revalida mucho estado (§2.3).

### 5.2 Lo que es caro

- **CPU por draw en GL/Mesa** (4-30 µs) y cada IPC a nvdrv [C/T].
- **Escribir mucho en memoria sin caché**, y sobre todo **leer de ella** [C/E].
- **Fragmentos con shader TEV largo.** SwitchWaker mide **~2,4 ms de GPU por pantalla 720p de
  fragmentos TEV a 460,8 MHz**, unos 0,83 fragmentos por ciclo, muy por debajo del pico ROP de
  16/ciclo. El coste lo marca la ALU por fragmento, no los ROP [T] [16 PERF_STUDY §8.1].
  - Para NSMBW con unas 4-6 capas de *overdraw* a 720p serían ~10-15 ms a 460 MHz y ~15-22 ms a
    307 MHz [E, extrapolado].
  - **Primer paso: medir la GPU** con el factor 1,6276 de §2.6.
  - Palanca principal: resolución interna (EFB ×1 = 640×528 ≈ 0,37 pantallas 720p).
- **`discard` y alpha test** desactivan el Z temprano: el depth pasa a ser tardío [T] [16 PERF_STUDY §8.2].
  deko3d soporta Zcull (`DkQueueFlags_DisableZcull` para apagarlo) [C] [6][8]; nouveau no lo usa en
  Maxwell, según el README de deko3d [C] [6].
- **FP16:** el hardware lo tiene [C] [1], pero las herramientas libres (nouveau, uam) no lo emiten
  [C] [12][9]. Sólo NVN, la API oficial, lo aprovecharía. **No cuenta.**
- **Reloj en portátil** (307,2/384 MHz): 2-2,5 veces menos GPU que en sobremesa [C] [4].

### 5.3 Resumen

La consola tiene GPU de sobra para lo que pide NSMBW en vértices, texturas y copias. Los límites
reales son:

1. la CPU (nuestro trabajo por vértice y el driver GL);
2. el coste por píxel de los shaders TEV a 307-460 MHz si renderizamos a 720p o más.

FP16 no es una palanca disponible.

---

## 6. Qué medir antes de decidir (pasos concretos)

1. **Desglosar los ~19-21 ms y los ~70 ms del hilo de render** en:
   - decodificación del FIFO;
   - transformación y texgen en CPU;
   - armado de lotes;
   - `glBufferData`/`SubData`;
   - `glDraw*` y estado;
   - swap.

   Contar también **vértices/frame** y **bytes subidos/frame** en el nivel y en el mapa.
2. **Medir la GPU por frame** (`GL_TIME_ELAPSED` × 1,6276) a 307,2/384/460,8 MHz, para saber si tras
   arreglar la CPU el límite pasa a ser la GPU.
3. **A/B en consola:**
   - `NWII_NORING=1` frente a `0`;
   - un anillo persistente (nuevo);
   - `MESA_NO_ERROR=1`.
4. Si tras (b) y (a) el driver sigue por encima de unos 3 ms/frame, prototipar deko3d sólo para la
   pasada principal, siguiendo el plan por fases de SwitchWaker [16].

---

## 7. Fuentes

1. NVIDIA, *Tegra X1 whitepaper v1.0* (ene-2015): tabla de la GPU, FP16 vec2, L2, ROP/TMU,
   LPDDR4 de 64 bits a 25,6 GB/s. https://images.nvidia.com/content/pdf/tegra/Tegra-X1-whitepaper-v1.0.pdf
2. switchbrew, *Hardware*: SoC y LPDDR4/LPDDR4x de 4 GB por modelo. https://switchbrew.org/wiki/Hardware
3. switchbrew, *NV services*: nvdrv, ioctls, `GET_CHARACTERISTICS` (arch 0x120, impl 0xB "gm20b",
   2 TPC, L2 0x40000, clase 3D 0xB197). https://switchbrew.org/wiki/NV_services
4. sys-clk (tabla de relojes de GPU y memoria oficiales y máximos). https://github.com/retronx-team/sys-clk
5. Digital Foundry vía Nintendo Life: relojes 307,2/384/460/768 MHz y modo *boost*.
   https://www.nintendolife.com/news/2016/12/eurogamer_report_suggests_nintendo_switch_gpu_runs_60_percent_slower_when_undocked ·
   https://www.nintendolife.com/news/2019/05/digital_foundry_finds_mortal_kombat_11_also_using_switchs_boost_mode
6. deko3d, README. https://github.com/devkitPro/deko3d
7. deko3d, *Primer.md* (memoria, cmdbuf, colas, fences, push constants, captura y repetición).
   https://github.com/devkitPro/deko3d/blob/master/Primer.md
8. deko3d, `include/deko3d.h` (alineaciones, formatos BC/ASTC/ETC2, atributos Uscaled/Sscaled,
   `DkPrimitive_Quads`, `dkSwapchainSetSwapInterval`, `dkCmdBufPushConstants`).
   https://github.com/devkitPro/deko3d/blob/master/include/deko3d.h
9. uam, README ("Differences with standard GL and mesa/nouveau") y
   `mesa-imported/codegen/nv50_ir_emit_gm107.cpp` (sin emisión HADD2/HFMA2). https://github.com/devkitPro/uam
10. switchbrew/switch-examples: `graphics/deko3d/deko_examples` (Example03_Cube, SampleFramework/CCmdMemRing.h)
    y `graphics/opengl/textured_cube/source/main.cpp` (`setMesaConfig`: `MESA_NO_ERROR`, `NV50_PROG_*`).
    https://github.com/switchbrew/switch-examples/tree/master/graphics
11. RSDuck, *So you want to use deko3d for maximum speed and minimum bloat?* (trampas de deko3d).
    https://gist.github.com/RSDuck/b60d89fbcb4374977cb8fbc7fccdbde4
12. devkitPro/mesa, rama `switch-20.1.0-rc3`: `src/gallium/drivers/nouveau/nouveau_buffer.c`,
    `nouveau_screen.c`, `nouveau_mm.c`, `nvc0/nvc0_screen.c`, `nvc0/nvc0_program.c`,
    `src/mesa/state_tracker/st_cb_bufferobjects.c`, `src/mesa/main/context.c`,
    `src/egl/drivers/switch/egl_switch.c`, `src/gallium/winsys/nouveau/switch/nouveau_switch_winsys.c`.
    https://github.com/devkitPro/mesa/tree/switch-20.1.0-rc3
13. devkitPro/libdrm_nouveau: `source/nouveau.c` (`nouveau_bo_new`/`nouveau_bo_del`) y
    `source/pushbuf.c` (`pushbuf_submit`). https://github.com/devkitPro/libdrm_nouveau
14. libnx: `nx/source/nvidia/map.c` (`nvMapCreate`, `svcSetMemoryAttribute` uncached),
    `nx/source/nvidia/gpu_channel.c` (`nvGpuChannelKickoff`, reintento con `svcSleepThread(100 ms)`),
    `nx/include/switch/kernel/svc.h` (`MemAttr_IsUncached = BIT(3)`). https://github.com/switchbrew/libnx
15. Mesa, *Environment variables*. https://docs.mesa3d.org/envvars.html
16. centollOS/SwitchWaker: README,
    https://github.com/centollOS/SwitchWaker/blob/main/docs/DEKO3D_MIGRATION_PLAN.md y
    https://github.com/centollOS/SwitchWaker/blob/main/docs/SWITCH_PERF_STUDY.md
    (medidas en consola GL frente a deko3d, coste de Mesa por draw, PTIMER 1,6276, bloqueo de
    `nouveau_bo_del`). https://github.com/centollOS/SwitchWaker
17. centollOS/SwitchWakerHD (backend deko3d, uam como biblioteca, MPL-2.0). https://github.com/centollOS/SwitchWakerHD
18. sa2-nx, *Making a deko3d renderer for a ReXGlue Switch port* (números de hilo de render y
    trampas; cifras de NVK de banjo-nx).
    https://github.com/ChanseyIsTheBest/sa2-nx/blob/v1.0.3.1/docs/deko3d-renderer-guide.md
19. DrasticDS_nx (deko3d/NVK/nvc0/Zink). https://github.com/NaGaa95/DrasticDS_nx
20. melonDS Switch: https://gamebrew.org/wiki/MelonDS_Switch ·
    https://gbatemp.net/threads/melonds-for-switch-continuation.570021/post-9135488
21. chasem-dev/sms-switch PR #3 "dkgl" (hoy 404; dato tomado del índice del buscador).
    https://github.com/chasem-dev/sms-switch/pull/3
22. SlipstreamCE: https://github.com/thelinkin3000/SlipstreamCE · sdl3-nx (SDL_GPU sobre deko3d):
    https://github.com/nx-mod/sdl3-nx
23. Dolphin, *Ubershaders: A Ridiculous Solution to an Impossible Problem* (2017).
    https://dolphin-emu.org/blog/2017/07/30/ubershaders/
24. Código de Dolphin: `Source/Core/VideoCommon/VertexShaderGen.cpp`, `ConstantManager.h`,
    `VertexLoaderARM64.cpp`, `TextureDecoder_Util.h` (`DXTBlend` 3/8),
    `TextureDecoder_Generic.cpp` y `VideoBackends/OGL/OGLStreamBuffer.cpp`.
    https://github.com/dolphin-emu/dolphin/tree/master/Source/Core
25. Layout block-linear: tegra_swizzle (GOB 64×8 B, Tegra X1),
    https://docs.rs/crate/tegra_swizzle/latest/source/README.md ·
    Ryujinx `BlockLinearLayout.cs` (espejo),
    https://git.dawidpotocki.com/mirror/ryujinx/plain/Ryujinx.Graphics.Texture/BlockLinearLayout.cs?h=1.1.189 ·
    yuzu `video_core/textures/decoders.cpp` (espejo),
    https://git.openpunk.com/OpenPunk/yuzu/src/commit/4c348f406958a2d4f314a7ccecb310f902370e28/src/video_core/textures/decoders.cpp
26. libretro, compilación de RetroArch para Switch (libnx). https://docs.libretro.com/development/retroarch/compilation/switch-libnx/

Nuestro código citado: `NWiiRecomp/nWiiRuntime/src/hle/gx/renderer_gl.cpp` (`BatchVertex` de 72 B,
anillo R4 y `glBufferData` por lote), `NWiiRecomp/nWiiRuntime/src/platform/switch/switch_platform.cpp:575`
(`NWII_NORING=1` por defecto) y `NWiiRecomp/nWiiRuntime/src/hle/gx/fifo_parser.cpp` (transformación
en el *parse*).
