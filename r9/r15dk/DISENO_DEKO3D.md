# Backend deko3d para la Switch: diseño (frente R6, r15dk, 8-nov-2026)

**Objetivo:** un segundo backend gráfico, solo para la Switch, sobre **deko3d** (la API nativa de la GPU Maxwell,
sin Mesa), elegible al arrancar (`[graphics] backend = "deko3d"` en `config.toml` o `NWII_BACKEND=deko3d` en
`debug.toml`). GL sigue siendo el valor por defecto y la red de seguridad. La imagen tiene que ser **la misma** que la
del camino GL de la misma compilación.

Por qué: en la consola el hilo de render gasta una parte grande en Mesa/nouveau (validación por draw, `memmove` a
memoria sin caché, IPC por kick). SwitchWaker (Wind Waker, GX→deko3d) pasó de 17-26 µs/draw en GL a 1,8-2,8 µs/draw
(ver `docs/switch/GPU_MAXWELL_DEKO3D.md` §2.3). Nosotros hacemos ~120-230 draws GL por frame más copias EFB, cambios
de programa y uniformes: el ahorro esperado es de 1-7 ms/frame de driver, más lo que cuesta hoy subir datos.

Marcas: **[V]** verificado por nosotros (PC o consola), **[D]** según el código/las cabeceras de deko3d o uam,
**[E]** estimación.

---

## 0. Resumen de las decisiones

1. **Arquitectura en dos fases.**
   - **Fase A (paridad):** `dkgl`, una capa que implementa sobre deko3d **solo el subconjunto de GL que usa nuestro
     runtime** (~120 funciones, inventario en §2) y **se instala como driver de glad**: con backend deko3d,
     `main.cpp` llama `gladLoadGLLoader(dkgl::GetProcAddress)` en vez de `SDL_GL_GetProcAddress`, así que todos
     los `gl*` del runtime (son punteros de glad) van a dkgl **sin compilar nada dos veces y sin coste extra** en el
     camino GL. La lógica GX (estado, lotes, GPU XF, copias EFB, caché de texturas) es **el mismo código y el
     mismo binario** en los dos backends: la paridad de imagen sale por construcción, y solo hay que verificar la
     capa (§9). (Esto sustituye a la idea inicial de compilar `renderer_gl.cpp` dos veces con renombrado.)
   - **Fase B (rendimiento nativo):** cuando la imagen sea igual, sustituir en la compilación deko3d los caminos
     calientes por llamadas directas (push constants solo de las filas que cambian, estados precalculados con
     captura/repetición de comandos, sin VAO emulado, texturas sin paso por `glTexImage2D`). Siempre detrás de
     `#if NWII_DK` dentro del mismo fichero, para que GL no cambie.
   - Descartado por ahora: reescribir el renderer como interfaz abstracta (`GpuDevice`) — toca 5 900 líneas
     verificadas bit a bit y arriesga el camino GL. Se puede hacer después, ya con dos backends que comparar.
2. **Shaders:** el GLSL que generamos (TEV, VS clásico, VS de GPU XF, copia EFB) se convierte con `dk_glsl`
   (uniformes sueltos → bloque std140 en el binding 0 de cada etapa, bindings explícitos de samplers, locations
   fijas de varyings) y se compila con **uam enlazado dentro de la NRO** (`uamlib`, hecho y probado en PC:
   §5). Caché en disco de DKSH + **paquete precompilado en el PC** (los 201 TEV conocidos + VS) en el romfs.
3. **Ventana:** deko3d usa directamente `nwindowGetDefault()` con un `DkSwapchain`. SDL se queda para eventos,
   audio de reserva y `appletMainLoop`, pero **sin contexto GL ni superficie EGL** (§7).
4. **Memoria:** pocos bloques grandes subdivididos por nosotros (código, datos CPU-sin-caché, imágenes,
   lectura), un anillo de memoria de comandos por frame en vuelo, y vallas (`DkFence`) por frame (§6).
5. **Primer hito (hecho):** `tools/dk3test`, una NRO de prueba que valida en la consola toda la cadena:
   uam dentro de la NRO, conversión de los 201 TEV reales, Quads, texturas, push constants, `gl_VertexID` con
   índices codificados (GPU XF), doble fuente, buffer textures, la autoprueba de GPU XF bit a bit, coste por
   draw y presentación escalada (§10).

---

## 1. Qué usa hoy el renderer (inventario)

Ficheros con GL: `src/hle/gx/renderer_gl.cpp` (5 883 líneas en r13), `src/hle/texture_cache_gl.cpp` (599),
`src/hle/gx.cpp` (consultas de tiempo de GPU), `src/core/main.cpp` (EFB, XFB, presentación, volcados, ~27
llamadas). Llamadas distintas: 104. Las más usadas (cuenta en el código fuente): `glBindFramebuffer` 57,
`glGetUniformLocation` 37, `glGetIntegerv` 26, `glBindTexture` 20, `glUniform1i` 18, `glBindBuffer` 17,
`glUseProgram`/`glBindVertexArray` 16, `glDisable` 15…

| Función del renderer | Cómo lo hace en GL | Dónde |
|---|---|---|
| EFB (framebuffer de la Wii) | FBO con textura RGBA8 640s×528s + renderbuffer depth24/stencil8; `s` = resolución interna (1-4) | `GX_CreateEfb`, `GX_ResizeEfb` (blit color + depth al cambiar) |
| Borrado de copia (BP 0x52 bit 11) y `pe_clear_pending` | `glScissor` + `glColorMask`/`glDepthMask` + `glClear` | `ClearEfbCopySource`, `RenderOnce` |
| Copia EFB→XFB | `glBlitFramebuffer` EFB→FBO del XFB (NEAREST) | `CopyEfbToXfb` |
| Copia EFB→textura | pasada con shader de conversión (`InitCopyShader`: media 2×2, intensidad YUV, selección de canales) a una textura de la caché; a escala >1 además blit lineal a una copia nativa | `CopyEfbToTexture` |
| Copias EFB a RAM | **no existen**: las copias se quedan en la GPU y se reconocen por dirección + hash de la RAM | `texture_cache_gl.cpp` (`efb_copy_done`, `stitch_*`) |
| Lecturas a CPU (readback) | `glReadPixels` solo para depuración y volcados (`NWII_XFBDUMP`, FRAMECHECK, capturas F11/F12, PixWatch) | varios |
| Texturas | `glTexImage2D` RGBA8 sin mipmaps (decodificadas en CPU); una textura nueva por contenido nuevo; borrado diferido | `TextureCache::get_texture` |
| Pegado de copias EFB en texturas de RAM | `glCopyTexSubImage2D` desde un FBO de lectura | `stitch_one` |
| Muestreadores | un `GLSampler` por modo TX_SETMODE0: wrap S/T (clamp/repeat/mirror), mag/min nearest/linear | `ApplyTexMode` |
| Programas | VS + FS generados; en la Switch **programas separables** (SSO: `glUseProgramStages`, un VS por proceso, FS por TEV); en PC enlazados con binarios en caché | `sso::`, `shcache::`, `CompileTevProgram` |
| Uniformes | `glGetUniformLocation` + `glUniform*`/`glProgramUniform*` (mat4 `mvp`, `uTexMtx[8]`, colores TEV, luces, `uXDraw`…) | `LocateUniforms`, `SetupDrawState` |
| Vértices | (a) GPU XF (defecto): un buffer persistente coherente (`glBufferStorage`+`glMapBufferRange`) visto como dos *buffer textures* (`R32UI`, `RGBA32UI`), índices `(draw<<16)\|vértice` en un anillo de índices, un `glDrawElements` por lote; (b) vbuf R9: anillo persistente de `BatchVertex` (72 B) + índices; (c) r8: `glBufferData`/`glBufferSubData` | `xf::`, `vbuf::`, `FlushBatch` |
| Sincronización | `glFenceSync`/`glClientWaitSync` por frame para reciclar los anillos; `glFinish` en prewarm/autoprueba | `xf::`, `vbuf::` |
| Estado de dibujo | caché propia (`GLStateCache`: enable, blend eq/func separados, depth func/mask, color mask, logic op, cull/front face, viewport, depth range, scissor, unidad/textura/sampler) | `C*()` |
| Blend | `glBlendFuncSeparate`/`glBlendEquationSeparate`, **doble fuente** (`GL_SRC1_ALPHA`) para el alfa de destino, `GL_COLOR_LOGIC_OP` | `ApplyBlend` |
| Z | `GL_DEPTH_TEST`, 8 funciones, `glDepthMask`, `glDepthRange(n,f)` del viewport GX, `GL_DEPTH_CLAMP` siempre | `ApplyZMode`, `ApplyViewport` |
| Alpha test | `discard` en el FS (no hay alpha test de función fija) | `tev_shader_gen.cpp` |
| Recorte | `glScissor` (BP 0x20/0x21 + offset 0x59), por escala | `ApplyScissor` |
| Cull | `glCullFace`/`glFrontFace` (frente = CW porque la proyección invierte Y) | `ApplyCullMode` |
| Presentación | `GX_PresentBlit`: blit lineal del XFB al framebuffer 0 (con reducciones 2:1 previas si la escala es grande) + `SDL_GL_SwapWindow` | `main.cpp` |
| Overlay de fps / barra del prewarm | `glScissor` + `glClear` de rectángulos sobre el framebuffer 0 | `DrawPerfOverlay`, `PrewarmProgressBar` |
| Medidas | `GL_TIME_ELAPSED` (`GpuTimer`, `gx.cpp`) | opcional |
| Autoprueba/comprobación de GPU XF | **transform feedback** (`glTransformFeedbackVaryings`, `glGetBufferSubData`) | `xf::SelfTest`, `NWII_GPU_XF_CHECK` |
| Captura F11/F12 | textura 2D array + `glFramebufferTextureLayer` + blits | PC (en la Switch no se usa) |

---

## 2. Correspondencia GL → deko3d

| GL | deko3d | Notas |
|---|---|---|
| contexto, `SDL_GL_*` | `DkDevice` con `DkDeviceFlags_OriginLowerLeft \| DepthMinusOneToOne` (convenciones de GL: así viewport, scissor, `gl_FragCoord` y la profundidad no cambian) | [V] dk3test usa estos flags |
| FBO + `glBindFramebuffer` | `dkCmdBufBindRenderTargets(color[], depth)`; un "FBO" de dkgl = vistas de color/profundidad | sin coste de validación |
| textura RGBA8 | `DkImage` block-linear `RGBA8_Unorm` (+`Usage2DEngine` si se copia/escala; `UsageRender` si es destino) en un bloque `GpuCached\|Image`, descriptor en un conjunto de descriptores | subida: `dkCmdBufCopyBufferToImage` desde memoria de paso |
| renderbuffer depth24/stencil8 | `DkImage` `Z24S8` con `UsageRender` | |
| `glTexParameter`/sampler | `DkSampler` → `DkSamplerDescriptor`; tabla de descriptores de muestreador (pocos: uno por modo GX) | `DkWrapMode_Repeat/MirroredRepeat/ClampToEdge`, `DkFilter_Nearest/Linear` |
| `glActiveTexture`/`glBindTexture`/`glBindSampler` | `dkCmdBufBindTextures(stage, id, dkMakeTextureHandle(img, smp))` en el draw si cambió | binding = el que da `dk_glsl` (`uTexN` → N) |
| `glTexBuffer` (buffer texture) | `DkImage` de tipo `DkImageType_Buffer` sobre la región del bloque de datos (`R32_Uint`, `RGBA32_Uint`) | [D] `dk_image.cpp`; [V] dk3test T6 |
| `glBufferStorage` persistente + `glMapBufferRange` | región de un `DkMemBlock` `CpuUncached\|GpuCached` (igual que el GART sin caché de Mesa) | el puntero CPU es fijo |
| `glBufferData`/`glBufferSubData` | anillo propio en el bloque de datos (escritura secuencial) | camino r8/R4, solo si GPU XF y vbuf están apagados |
| VAO + `glVertexAttribPointer` | `DkVtxAttribState[]` + `DkVtxBufferState[]` + `dkCmdBufBindVtxBuffers` | el VAO de dkgl guarda las tres cosas |
| índices | `dkCmdBufBindIdxBuffer(Uint32, addr)` + `dkCmdBufDrawIndexed` | GPU XF: `gl_VertexID` = valor del índice [V] T3 |
| `glDrawArrays(GL_TRIANGLES)` | `dkCmdBufDraw` | `DkPrimitive_Quads` nativo para la fase B |
| `glUseProgram` / SSO `glUseProgramStages` | `dkCmdBufBindShaders(GraphicsMask, {vs, fs})` | uam no enlaza: VS y FS van por separado igual que en SSO |
| `glUniform*` / `glProgramUniform*` | copia en CPU del bloque std140 de cada etapa (desplazamientos de `dk_glsl::Reflection`); en el draw, si cambió, `dkCmdBufPushConstants(ubo, size, off, n, data)` + `dkCmdBufBindUniformBuffer` | push constants = en orden con los draws, sin buffers intermedios |
| `glUniform1i(sampler, unidad)` | tabla binding→unidad del programa | |
| blend separado + ecuaciones | `DkBlendState` (factores y op de color/alfa) + `DkColorState.blendEnableMask` | **ojo bug deko3d 0.5.0 #29**: `dkCmdBufBindBlendStates` programaba el factor destino de alfa con el de color (§11) |
| doble fuente `GL_SRC1_ALPHA` | `DkBlendFactor_Src1Alpha`/`InvSrc1Alpha`; FS con `layout(location=0, index=1)` | [V] T4 |
| `GL_COLOR_LOGIC_OP` + `glLogicOp` | `DkColorState.logicOp` (16 ops, mismo orden que GL) | |
| `glColorMask` | `DkColorWriteState` | |
| depth test/func/mask | `DkDepthStencilState` | |
| `glDepthRange(n,f)` | `DkViewport.near/far` | |
| `GL_DEPTH_CLAMP` | `DkRasterizerState.depthClampEnable` | |
| cull/front face | `DkRasterizerState.cullMode/frontFace` | |
| `glViewport`/`glScissor` (+ `GL_SCISSOR_TEST`) | `dkCmdBufSetViewports`/`SetScissors` (scissor desactivado = rectángulo completo) | |
| `glClear` con scissor y máscaras | `dkCmdBufClearColor`/`ClearDepthStencil` respetan scissor y máscara de escritura [D, a confirmar en consola] | si no: un cuadro con el shader de borrado |
| `glBlitFramebuffer` color | `dkCmdBufBlitImage` (motor 2D, `FilterNearest/Linear`, con escala) | EFB→XFB y presentación |
| `glBlitFramebuffer` depth (cambio de escala) | `dkCmdBufCopyImage` si el tamaño es igual; con escala: un draw que escribe `gl_FragDepth` | solo al cambiar de resolución interna |
| `glCopyTexSubImage2D` | `dkCmdBufCopyImage` (rectángulo) | **bug: ignora `srcRect->z`** (no nos afecta, 2D) |
| `glReadPixels` | `dkCmdBufCopyImageToBuffer` a un bloque de lectura + `dkQueueWaitIdle` (o valla) | solo depuración/volcados; **la fila 0 = abajo, igual que GL** (medido en la consola, dk3test T2): se copia tal cual |
| `glFenceSync`/`glClientWaitSync` | `dkQueueSignalFence(q, &fence, true)` / `dkFenceWait` | por frame en vuelo |
| `glFinish` | `dkQueueFlush` + `dkQueueWaitIdle` | |
| `GL_TIME_ELAPSED` | `dkCmdBufReportCounter(DkCounter_Timestamp)` antes/después | opcional |
| transform feedback | **no existe** | la autoprueba de GPU XF dibuja puntos a un destino `RGBA32_Uint` (como dk3test T6); `NWII_GPU_XF_CHECK` igual o desactivado |
| binarios de programa (`glGetProgramBinary`) | DKSH (los bytes de uam) | caché propia (§5) |
| `glGetError`, `glGetIntegerv`, `glIsEnabled`… | consultas al estado que guarda dkgl | sin ida a la GPU |
| `SDL_GL_SwapWindow` | `dkQueueAcquireImage` + blit al swapchain + `dkQueuePresentImage` | §7 |

---

## 3. dkgl: la capa (fase A)

### 3.1 Cómo se engancha (hecho en r15dk)

- `src/hle/gx/dk/dkgl.{h,cpp}`: las funciones GL (estáticas) y `dkgl::GetProcAddress(nombre)`; las que glad pide
  y no usamos devuelven un stub que avisa en el log si se llama. `glGetProgramBinary`/`glProgramBinary` devuelven
  `nullptr` (la caché de binarios GL se apaga sola).
- `src/hle/gx/dk/dkgl_select.cpp` (todas las plataformas): `GX_GLProc(nombre)` (dkgl o SDL; sustituye a los 6
  `SDL_GL_GetProcAddress` de `renderer_gl.cpp`), `GX_DkActive()`, y `GX_Host{Window,Drawable,Swap,…}` para la barra
  del prewarm y el overlay (antes `SDL_GL_*`).
- `main.cpp`: `NWII_BACKEND=deko3d` (o `[graphics] backend = "deko3d"`, que `config.cpp` pasa al entorno) → ventana
  SDL sin `SDL_WINDOW_OPENGL`, `dkgl::Init()` (dispositivo, cola, swapchain 1280×720 sobre `nwindowGetDefault()`),
  glad con dkgl; si falla, vuelve a crear la ventana con GL. `HostSwap`/`HostDrawableSize` en la presentación.
- `renderer_gl.cpp`: la autoprueba de GPU XF (transform feedback) se salta con deko3d (verificada en la consola por
  dk3test T6).
- Compilación: `r9/r15dk/tools/variant_build_dk.sh` = `variant_build.sh` + `libuamlib.a` + `libdeko3dd.a` en la
  línea de enlace.

### 3.1b Idea descartada: compilación doble

- `src/hle/gx/dk/dkgl.h`: declara `namespace dkgl { … }` con las funciones GL que usamos (mismas firmas y
  constantes de `glad`) y, al incluirse con `-include`, `#define glXxx dkgl::Xxx` para cada una.
- `src/hle/gx/dk/dk_rename.h`: renombra los símbolos que exporta el renderer (`GX_CreateEfb`, `GX_ResizeEfb`,
  `GX_PresentBlit`, `GX_GetXfbFramebuffer`, `GX_GetPresentRect`, `CreateRendererGL`, `DrawPerfOverlay`,
  `CaptureRecord`, `CaptureSave`, `DumpPresentedXfb`, `TextureCache`…) a `*_dk`.
- `renderer_dk.cpp` = `#include "renderer_gl.cpp"` compilado con esos dos `-include`; igual
  `texture_cache_dk.cpp`. Lo que es estado global estático (`static` dentro de los `.cpp`) queda duplicado sin
  choque.
- `renderer_select.cpp`: define los nombres originales y despacha a la versión GL o DK según el backend elegido
  al arrancar (una rama por llamada: son llamadas por frame, no por draw).
- Las tres o cuatro cosas que no tienen equivalente (transform feedback, binarios de programa, consultas de
  tiempo) se apagan en la compilación DK con `#if NWII_DK` en el sitio, con un camino propio cuando hace falta
  (autoprueba de GPU XF).

### 3.2 Modelo de ejecución

- Un `DkCmdBuf` por hilo GL (solo el hilo de render; el worker no toca GL), con memoria de un **anillo de
  comandos** de 3 × 4 MiB (`CpuUncached|GpuCached`), un segmento por frame en vuelo; `cbAddMem` pide más del
  segmento actual.
- `dkgl` lleva el estado GL "actual" (programa, VAO, texturas por unidad, FBO, enables, blend…) en una
  estructura, con bits de "sucio". Las funciones `gl*` solo cambian esa estructura (unos ns). En
  `glDraw*`/`glClear`/`glBlitFramebuffer` se emite lo sucio y el comando. Esto es lo que hacía Mesa, pero sin
  validación de API, sin traducción a TGSI, sin `memmove` de recursos ni IPC por draw.
- Envío: `dkQueueSubmitCommands` + `dkQueueFlush` al presentar, en `glFinish`/`glClientWaitSync`/lecturas, y si
  el segmento pasa de un umbral. La IPC a nvdrv es una por envío (no por draw).
- Vallas: una `DkFence` por frame en vuelo; las de `glFenceSync` (anillos de GPU XF/vbuf) son `DkFence`
  propias del objeto "sync".

### 3.3 Memoria

| Bloque | Flags | Tamaño inicial | Uso |
|---|---|---|---|
| código | `CpuUncached\|GpuCached\|Code` | 32 MiB (crece por bloques) | DKSH (VS GPU XF ~55 KiB, VS clásico ~47 KiB, FS TEV 1-6 KiB) |
| datos | `CpuUncached\|GpuCached` | 64 MiB | buffers GL (GPU XF 48 MiB: arena 24 + anillo 24; índices; UBO de push constants; descriptores) |
| imágenes | `GpuCached\|Image` | 64 MiB + crecer | EFB (1x: 1,35 MiB color + 1,35 depth; 2x: ×4), XFB, texturas de la caché (decenas de MiB en un nivel) |
| paso (staging) | `CpuUncached\|GpuCached` | 8 MiB anillo | subidas de texturas (`CopyBufferToImage`) |
| lectura | `CpuCached\|GpuCached` | 4 MiB | `glReadPixels` (volcados) |
| comandos | `CpuUncached\|GpuCached` | 12 MiB | anillo de 3 frames |

Los bloques cuestan recursos del SO: se reservan pocos y grandes, y dkgl lleva su propio asignador (bloques
de tamaño fijo por clase para texturas, como el arena de vbuf). **Nada de crear/destruir `DkMemBlock` por
frame.** Las texturas borradas (`glDeleteTextures`) se liberan cuando la valla del frame que las usó ha pasado
(hoy Mesa bloquea en `nouveau_bo_del`).

### 3.4 Texturas

- `glTexImage2D(RGBA8, data)`: hueco en el bloque de imágenes + copia a paso + `dkCmdBufCopyBufferToImage`.
  Coste CPU = una copia secuencial a memoria sin caché (como hoy dentro de Mesa) — fase B: decodificar
  directamente en la memoria de paso.
- Las texturas son pequeñas (≤ 1024²) y sin mipmaps.
- Ojo deko3d #10 (texturas muy pequeñas cortadas, p. ej. 32×8): probar en la consola en el hito 2.

### 3.5 Lo que no se emula

`glGetProgramBinary`/`glProgramBinary` (la caché DKSH ocupa su lugar), transform feedback, `GL_TIME_ELAPSED`
(opcional con timestamps), texturas 2D array (captura de PC), `glGetBufferSubData`.

---

## 4. Uniformes → UBO / push constants

- `dk_glsl::Convert` reúne todos los `uniform T x[N];` que no son samplers en
  `layout(std140, binding = 0) uniform DkUniforms { … };` y da su tabla de desplazamientos std140
  (escalares 4 B, `vec2` 8, `vec3`/`vec4` 16; arrays y columnas de matriz con paso 16). [V] los 206 shaders
  del renderer convierten y compilan con uam en el PC (§5.3).
- Tamaños reales: FS TEV 128 B (colores TEV y K) o ~576 B con indirectas; VS clásico 1 344 B; VS GPU XF 848 B.
- `glGetUniformLocation` devuelve un índice a esa tabla (y −1 si no existe, como GL).
- Por draw, dkgl envía con `dkCmdBufPushConstants` **solo el rango sucio** de cada etapa (mín.–máx. de lo que
  se escribió desde el último draw). Los push constants van en el flujo de comandos, en orden: el siguiente draw
  ve los valores nuevos y el anterior los viejos, sin anillo de UBO ni vallas. El UBO destino es una ranura fija
  de 64 KiB por etapa (no hace falta un anillo: el push constant serializa).
- SSO (Switch): el VS es uno por proceso y sus uniformes viven en su propio bloque; los del FS en el suyo. Igual
  que con `glProgramUniform*` hoy.

---

## 5. Shaders: uam en la NRO + caché + precompilado

### 5.1 uamlib

- Fuente: **uam 1.1.0 de devkitPro** (`ext/uam`, commit 5a5afc2) sin meson: `ext/uamlib/CMakeLists.txt` propio;
  los ficheros que meson generaría (bison, flex y los tres `ir_expression_operation*.h` que hace Mesa con mako)
  están en `ext/uam_gen` (mako no está instalado: `tools/minimako` implementa el subconjunto de mako que usa
  ese script). [V] **compilado sin nuestros parches, los DKSH son idénticos byte a byte a los del `uam` instalado
  por devkitPro** (9 shaders de los ejemplos).
- Parches propios (marcados `r15dk parche N`; los mismos arreglos que documentó SwitchWakerHD, reescritos por
  nosotros):
  1. `Instruction::init` deja `sched = 0x7e0`: uam leía memoria sin inicializar en `setDelay` (resultados que
     dependían del montón).
  2. Regla de Mesa 20.1 restaurada: *stall* 2 si la siguiente instrucción está en otro bloque (uam la quitó;
     motas en SwitchWaker, devkitPro/uam#7).
  3. Sin *dual issue* (motas en SwitchWakerHD; Mesa 20.1, el que usa hoy nuestro GL, nunca lo hace).
  4. `OutputDkshMem`: los bytes del `.dksh` a memoria en vez de a fichero.
  Con `-DR15DK_UAM_UPSTREAM` se compila sin 1-3 (para comparar).
- Límites: un solo hilo compilando a la vez (el `c11/threads.h` de uam es un stub), pila grande (dk3test usa un
  hilo con 8 MiB); los errores fatales internos de Mesa todavía hacen `exit`/`abort` (pendiente: parche como el
  4 de SwitchWakerHD si aparece algún caso; nuestros 206 shaders compilan todos).
- Tamaño: `libuamlib.a` 51 MB (con secciones por función; el binario final añade ~3-4 MB tras `--gc-sections`
  [E]).

### 5.2 Estrategia en tiempo de ejecución

1. **Paquete precompilado** en el romfs (`romfs:/dksh/pack.bin`): DKSH de todos los FS de la caché de shaders
   conocida (201 en la consola hoy) + VS clásico + VS GPU XF (variantes 0 y 1) + shaders auxiliares (copia EFB),
   compilados en el PC con el mismo `uamlib` (mismos parches). Clave = FNV-64 del GLSL **convertido** + id de
   versión del compilador ("uam 1.1.0 r15dk p1-4") + versión de `dk_glsl`.
2. **Caché en disco** `cache/dkshcache.bin` (formato como `shadercache.bin`: clave, longitud, DKSH): lo que se
   compila en la consola se añade (un registro por `fwrite` + `fsync`, como hoy).
3. **Fallo de caché** (shader TEV nuevo): compilar con uamlib en un hilo de fondo (núcleo 1 o 2, pila 8 MiB).
   Mientras tanto: (a) primera versión = esperar (como hoy con GL, que compila en el hilo de render y tiene el
   presupuesto `s_compiles_this_frame`); (b) después = saltar ese draw durante 1-3 frames como hacen SwitchWaker/
   Dolphin híbrido. La `shadercache.bin` existente (fuentes FS) sigue sirviendo para el prewarm: al arrancar se
   precompilan en segundo plano los que no estén en el paquete ni en la caché DKSH, con la barra de progreso.
4. Tiempo de compilación: PC 12,7 ms de media por shader (2,6 s los 206); en la consola lo mide dk3test (T1). La
   investigación de terceros da ~70 ms de media en la consola [T].

### 5.3 Verificado en el PC

`tools/dkglsl_test.cpp` (r15dk): VS clásico, FS de estado vacío, VS GPU XF variantes 0 y 1, autoprueba XF y los
**201 FS TEV** de `switch/NSMBWnx/cache/shadercache.bin`: **206/206 convierten y compilan con uam**. Diferencias
con GL que importaban y están resueltas en la conversión: uniformes sueltos, bindings explícitos, locations de
varyings (uam no enlaza), `#extension GL_ARB_gpu_shader5` (núcleo en 4.60), `precise` (nv50_ir de uam lo respeta
igual que Mesa 20.1: `peephole.cpp` no fusiona a MAD un `add` precise). La división entera no aparece en los TEV
(solo divisiones float).

---

## 6. Frames, vallas y anillos

- `NWII_RENDER_WORKER` (worker en el núcleo 2) no cambia: dkgl solo corre en el hilo de render.
- Frames en vuelo: 2 (como GL con vsync y doble buffer). Al empezar el frame N se espera la valla de N−2 y se
  recicla su segmento del anillo de comandos.
- GPU XF y vbuf ya llevan su propio reciclado por `glFenceSync` (anillos de 24 MiB): en dkgl esas vallas son
  `DkFence` reales.
- Copias EFB→textura dentro del frame: deko3d no reordena; entre el draw al destino y el muestreo hace falta
  `dkCmdBufBarrier(DkBarrier_Fragments, DkInvalidateFlags_Image)` (en GL lo hacía el driver al cambiar de FBO).
  dkgl lo emite al cambiar de render target si el anterior se usa como textura. Idem tras `CopyBufferToImage`
  (`DkInvalidateFlags_Image`).

---

## 7. Ventana, SDL y presentación

- Hoy: `SDL_CreateWindow(SDL_WINDOW_OPENGL)` → SDL crea una superficie EGL sobre `nwindowGetDefault()` y Mesa
  presenta. Con deko3d **la ventana nativa la toma el `DkSwapchain`**; si EGL la tuviera conectada, el swapchain
  fallaría (dk3test tiene que soltar la consola de texto antes de crear el suyo).
- Hecho: con `backend = deko3d`, `main.cpp` hace `SDL_Init(VIDEO|AUDIO)` (eventos, salida, audio de reserva) y **no
  crea ventana SDL** (medido en la consola: `SDL_CreateWindow` del driver Switch configura la ventana nativa aunque no
  sea GL y entonces `dkSwapchainCreate` falla; en deko3d todo error es fatal, incluso con `cbDebug`). El renderer
  recibe una "ventana" ficticia (`GX_SetWindow`) para no pasar a modo headless.
  `SDL_GL_*` no se llaman; `DesiredInternalScale`/acoplado siguen con `appletGetOperationMode()`.
- Swapchain: 3 imágenes RGBA8 1920×1080 (`UsageRender|UsagePresent|HwCompression|Usage2DEngine`); en portátil se
  dibuja en 1280×720 y se recorta con `dkSwapchainSetCrop` (o se recrea el swapchain al acoplar). `dkSwapchainSetSwapInterval(1)`.
- Presentación (`GX_PresentBlit` versión DK): `dkQueueAcquireImage` → borrar la imagen a negro → `dkCmdBufBlitImage`
  lineal del XFB al rectángulo 4:3/16:9 (mismas cuentas que hoy; las reducciones 2:1 previas son las mismas
  blits) → overlay → `dkQueuePresentImage`.
- Overlay de fps y barra del prewarm: los rectángulos son `dkCmdBufClearColor` con scissor sobre la imagen del
  swapchain (mismo código, dkgl emula `glClear` con scissor).
- Orientación (medida, dk3test T2): con `OriginLowerLeft` la fila 0 de una imagen en memoria es la de abajo,
  como en GL, y el swapchain invierte al presentar: blits, copias y lecturas usan las coordenadas GL tal cual y los
  volcados (`NWII_XFBDUMP`) no cambian.

---

## 8. Elección del backend en tiempo de ejecución

- `config.h` no se puede tocar en una compilación solo-runtime: `config.cpp` (que sí se puede) acepta
  `[graphics] backend = "deko3d"` y, sin tocar el enum, hace `setenv("NWII_BACKEND", "deko3d")`. `debug.toml` ya
  pasa variables de entorno: `NWII_BACKEND = "deko3d"`.
- Al arrancar (`main.cpp`, antes de crear la ventana): si se pidió deko3d y `dkDeviceCreate` funciona, se usa;
  si falla algo en la inicialización (dispositivo, swapchain, compilar el VS de GPU XF), se registra y **se vuelve
  a GL** (aún no se ha creado el contexto GL).
- En PC el backend deko3d no existe: se ignora con un aviso.

---

## 9. Verificación (hito 3)

- La lógica GX es la misma: lo que puede diferir es la capa (estado mal traducido, std140, orientación, blits,
  borrados con scissor, copias). Comparación por volcados en la consola: el NRO `*_input_test.nro` con
  `NWII_INPUT_SCRIPT`/`NWII_INPUT_PHASES` (debug.toml) recorre título → menú → mapa → 1-1 de forma determinista
  y `NWII_XFBDUMP` vuelca XFB en frames fijos; se corre dos veces, `NWII_BACKEND=gl` y `=deko3d`, y se comparan los
  PNG en el PC (`compare` por píxel; tolerancia 0 esperada salvo filtrado lineal del blit de presentación, que no
  se vuelca).
- Diferencias aceptables a priori: ninguna en el XFB. Mesa 20.1 (GL en la Switch) y uam comparten el backend
  nv50_ir: el código de los shaders es casi el mismo; los parches 2/3 alinean uam con Mesa 20.1.
- Autoprueba de GPU XF en deko3d al arrancar (como dk3test T6) — si falla, GPU XF se apaga como en GL.

---

## 10. dk3test (hito 2a, hecho)

`tools/dk3test` → `dk3test.nro` (copia en `r9/r15dk/bin/dk3test.nro`). Pruebas (log en
`sdmc:/switch/dk3test/dk3test.log`, imágenes `t2_efb.ppm`, `t5_tev.ppm`):

| Prueba | Qué valida |
|---|---|
| T1 | uamlib dentro de la NRO: compila en la consola 9 shaders de prueba, el VS de GPU XF y los **201 FS TEV**; tiempos; **mismo DKSH que en el PC** (para el paquete precompilado) |
| T2 | EFB 640×528 RGBA8 + Z24S8, cuadro `DkPrimitive_Quads` con textura (tablero), muestreador, UBO por push constants, lectura a CPU; orientación de las filas |
| T3 | `gl_VertexID` = valor del índice `(5<<16)\|k` (GPU XF), y si incluye `vertexOffset` |
| T4 | doble fuente (`index = 1`, `Src1Alpha`) |
| T5 | un TEV real (con indirectas o grande) dibujado sin error de GPU |
| T6 | **autoprueba de GPU XF** sobre buffer textures (R32UI y RGBA32UI sobre la misma memoria), 65 536 casos de `div_rn`, `sqrt_rn` y fila de matriz, variantes 0 y 1, contra la CPU **bit a bit** |
| T7 | coste de CPU de grabar 2 000 draws (push constant + textura + draw) y tiempo de GPU |
| bucle | presentación: EFB → swapchain 1280×720 con el motor 2D (lineal), fondo verde si todo pasó |

**Resultados en la consola (8-nov 18:25, OC 1785/768; `switch/real_logs/dk3test_1108/`):**
- T1: **201/201 TEV compilados con uam en la consola**, media **46,5 ms**, máx. 85,5 ms, total 9,3 s; **201/201
  idénticos byte a byte al DKSH del PC** (el paquete precompilado sirve). VS de GPU XF: **2,0 s** (→ caché en disco
  obligatoria). Shaders de prueba 3-37 ms.
- T2: la **fila 0 en memoria es la de ABAJO** (como `glReadPixels`): rectángulos GL = rectángulos deko3d, sin
  inversiones. En pantalla el cuadro sale bien orientado (el swapchain con `OriginLowerLeft` invierte al presentar).
- T3: `gl_VertexID` = valor del índice **e incluye `vertexOffset`** (como GL con base vertex).
- T4: doble fuente: RGB correcto; **alfa mal = bug #29 de deko3d** (el factor destino de alfa usa el de color),
  presente en 0.5.0 y en master. Rodeo: tras `dkCmdBufBindBlendState` se reescribe a mano el método
  `IndependentBlend[i].FuncAlphaDst` (0x780 + 8i + 6) con `dkCmdBufReplayCmds` (`BindBlendFixed` en dkgl y en
  dk3test). (Recompilar deko3d no se pudo: necesita `dekodef`/`dekomme` de dekotools, que no están instalados.)
  dk3test v2 lo comprueba (T4 + T4b).
- T5: TEV real dibujado sin error. T6: **autoprueba GPU XF 0 diferencias en las dos variantes** (65 536 casos).
- T7: **2 000 draws: grabar 0,24 ms (0,12 µs/draw)**, enviar 0,05 ms, GPU 0,59 ms. Presentación 60,2 fps,
  131 µs/frame de grabación.
- Se colgó al salir por un fallo de dk3test (escribía en la consola de texto ya liberada): arreglado en v2.

---

## 11. Riesgos y bugs conocidos

| Riesgo | Mitigación |
|---|---|
| deko3d 0.5.0 #29: `dkCmdBufBindBlendStates` usa el factor destino de color para el de alfa | **confirmado en la consola (dk3test T4)**; rodeo: reescribir `FuncAlphaDst` con `dkCmdBufReplayCmds` tras cada bind (`BindBlendFixed`) |
| la biblioteca de release aborta sin `cbDebug` | desarrollo con `libdeko3dd` (validación); release con `libdeko3d` |
| texturas muy pequeñas (#10) | probar 32×8 en el hito 2 |
| `dkCmdBufCopyImage` ignora `srcRect->z` | solo 2D |
| la GPU falla → cola inservible, el SO mata el proceso | `dkQueueIsInErrorState` tras cada envío en depuración + log inmediato |
| errores fatales de Mesa dentro de uam (`exit`) | todos nuestros shaders compilan; parche "no exit" si hace falta |
| memoria: uam ~decenas de MB por compilación sin liberar del todo | medir en la consola (T1 da el pico); patch 9 de SwitchWakerHD como referencia |
| `nwindow` ocupado por SDL/EGL | ventana SDL sin OpenGL o sin ventana SDL (§7) |

---

## 12. Hitos y esfuerzo

| Hito | Contenido | Esfuerzo [E] | Estado |
|---|---|---|---|
| 1 | este documento | 1 sesión | **hecho** |
| 2a | uamlib + `dk_glsl` + dk3test (validación en la consola) | 1 sesión | **hecho; 10/10 en la consola** |
| 2b | dkgl núcleo: dispositivo, bloques, anillo de comandos, vallas, texturas/samplers, FBO/blit/clear/readback, buffers (persistentes y stream), VAO, programas/uniformes/SSO con uamlib + caché DKSH, draw | 3-5 sesiones | **hecho** (~1 300 líneas, 1 sesión) |
| 2c | integración: `config.cpp` (backend), `main.cpp` (sin ventana SDL, swapchain, presentación, overlay), glad con dkgl, CMake/enlace de la variante (deko3d + uamlib renombrada) | 2 sesiones | **hecho** |
| 2d | título en la consola con deko3d | 3-6 ciclos de prueba en la consola | **hecho al 2.º ciclo** (título, menú y mapa) |
| 3 | paridad: volcados XFB GL vs deko3d con el NRO de entrada guionizada; arreglar diferencias; autoprueba GPU XF sin transform feedback | 1-2 semanas | |
| 4 | rendimiento: paquete DKSH precompilado, fase B (push constants mínimos, estados precalculados, subida de texturas directa, sin `glTexImage2D`), medir µs/draw y ms/frame en la consola frente a GL | 1-2 semanas | |

Primera estimación de ganancia (a medir en el hito 4): hilo de render −1 a −7 ms/frame en niveles y más en el
mapa (muchos cambios de programa/uniformes y copias EFB), según SwitchWaker y nuestros ~120-230 draws por frame.
