# Funciones calientes del hilo del juego (NSMBW en la Switch real) y su posible reemplazo nativo 1:1

8-nov-2026. Investigación solo de lectura: no se cambió código, ni `nand/`, ni `game/`. Lo único nuevo es este archivo.

- Perfil: `switch/real_logs/r9combo_1108/profile_cpu.txt` (crudo) y `profile_symbolized.txt`. Hilo `cpu`, 105 286
  muestras en 120 s (1 por ms), NRO **r9combo** (`switch/real_builds/r9combo_1107/`).
- Código generado de referencia: `r9/r10a/export/output_*.cpp`. Runtime: `r9/r10a/nWiiRuntime`.
- Nombres: `symbols/usa_symbols.txt`, las firmas del SDK que ya lleva el código generado (`GXLoadTexObjPreLoaded__gx_a_...`),
  `NSMBW-Decomp/syms.txt` (direcciones PAL; en esta zona USA = PAL − 0x140) y el desensamblado del DOL USA
  (`bin_usa/wiimj2d.dol`, con `llvm-mc`). Si un nombre no está confirmado por símbolo, la tabla dice **probable**.

---

## 0. Resumen

1. **Lo más rentable y seguro son las operaciones de caché de datos** (`DCInvalidateRange`, `DCFlushRange`,
   `DCFlushRangeNoSync`, `DCZeroRange`). En este perfil se llevan **8,6 % del hilo del juego** y no hacen nada útil
   para nosotros: son un bucle de 3 instrucciones por línea de 32 bytes con un **sondeo de interrupciones en cada
   vuelta**. Casi todas las llamadas vienen del audio: `__AXSyncPBs` invalida y luego vacía todo el arreglo de
   PB (≈1 150 líneas por cuadro de AX si hay 96 PB, 331 cuadros/s). Se pueden reemplazar por un cálculo O(1) del estado final
   (registros e `inst_count`), dejando el único sondeo que importa. Es 1:1 exacto y casi no tiene riesgo.
2. **AX/AXFX en total pesa ~20 %**: la reverb (ya nativa, 8,8 %), el delay AXFX `func_801A5C10` (1 %),
   `__AXSyncPBs` y compañía (~1,5 %) y las cachés del punto 1. Siguientes pasos: la pasada
   `apply_r9_axfx_fast.py` (ya escrita) y la misma traducción nativa para el delay, que es solo con enteros.
3. **Funciones hoja del SDK/MSL que valen la pena** (todas de semántica cerrada y bit a bit):
   `strcmp`/`strncmp` 1,7 %, `PSMTX*` 2,1 %, `memcpy` 0,9 %, `sqrt` de fdlibm 0,6 %,
   `fLiMgBa_c::searchNodeByID` 0,3 %. Además, solo en las cargas, la descompresión LZ de `CX` (1,4 %).
4. **GX se lleva 8,2 %**, pero repartido entre ~40 funciones pequeñas. Lo que cuesta es el camino de la tubería
   de escritura (`wgp_write32`, `wg_flush_now`: un vaciado con mutex después de cada posible comando BP, unos
   160 000/s). Conviene atacar primero ese camino del runtime antes que hacer HLE función por función.
   `GXCopyDisp` (1,2 %) no es CPU: es la espera a que el anfitrión pinte (`FifoWaitForHost`).
5. **Ojo al comparar con r10a**: el perfil es del sondeo de r8, que no se podía inlinear y leía el reloj en cada
   vuelta (`process_pending_callbacks` 12,2 % + `read_timebase` 7,8 %). r10a ya tiene el sondeo inline con cuenta
   atrás de R9 guestloop (comprobado en `nx/build/.../output_25.o`: solo se llama a `_slow`). En r10a los bucles
   con sondeo cuestan menos, así que el 8,6 % del punto 1 bajará a ~1 %. Antes de priorizar, hay que medir un
   perfil nuevo de r10a.
6. **Cómo encajarlo**: una pasada nueva sobre el código generado (`perf/apply_r10_hle.py`, propuesta), como
   `apply_r9_axfx.py`. Mete al entrar `if (ctx.pc == ENTRADA && nwii_hle_X(ctx)) return;`, deja el código generado
   como respaldo, se apaga con `NWII_HLE_X=0` y se compara con `NWII_HLE_VERIFY=n` (RAM + registros, como
   `NWII_AXFX_VERIFY`) y con el comparador diferencial `r8/codegen/difftest` (estados aleatorios + casos borde).

---

## 1. De dónde salen los números

### 1.1 Dos métricas

- **"% propio host"**: la columna del simbolizador (dónde estaba el PC de ARM). Reparte el coste de una función
  del juego entre su propio código y los ayudantes del runtime (`psq_load`, `wgp_write32`, el sondeo...).
- **"% pc invitado"**: el perfilador también guarda `CPUContext::pc`. El código generado escribe `ctx.pc` en
  tres sitios: al llamar (`ctx.pc = <entrada del destino>`), al volver (`ctx.pc = ctx.lr`, la dirección de
  retorno en quien llamó) y en cada sondeo de un salto hacia atrás (`ctx.pc = <dirección del bdnz/bc>`). Por eso
  la muestra cae en la función del juego que se está ejecutando, **con sus ayudantes del runtime incluidos**. Es
  la mejor medida del "coste propio" de cada función del juego. Agregué los PC por función (`r10a/export`,
  inicio..siguiente inicio). Script de trabajo: `/tmp/claude-1000/-home-djvemo-nsmbw-port/1b06eac6-a23b-489b-924a-77167831057b/scratchpad/gprof.py`.

### 1.2 El aviso "the function names below are WRONG" no aplica

Es un falso aviso. El `.elf` guardado contiene la cadena `build Nov  7 2026 17:23:37` (la de `switch_profiler.cpp`,
la misma que trae el perfil). La línea `[NX] ... built ... 17:23:39` sale de otra unidad (`switch_platform.cpp`),
compilada 2 s después. Es el mismo binario y los nombres valen. Habría que arreglar el simbolizador para que
compare la cadena del perfilador.

### 1.3 Contexto del perfil

- El juego va a ~30 fps (`nwii.log`). El hilo `cpu` está al 76-85 % y el principal (render) al 55-66 %.
- El 30,4 % de las muestras del hilo `cpu` caen en el bucle ocioso de `SelectThread` (`0x801B4FF8`), y casi
  todo eso es `svcSleepThread` (29,3 %): el hilo del juego **espera**. El resto se reparte en 29,6 % de código
  generado, 28,4 % de runtime, 2,4 % de E/S de ficheros y otros.
- Del 28,4 % de runtime, la mayor parte es el sondeo de r8 (`process_pending_callbacks` 12,2 % y
  `read_timebase` 7,8 %; el 94,7 % de las llamadas al reloj vienen del sondeo). r10a ya lo cambió (punto 0.5).

---

## 2. Identificación

Columnas: **insts** = instrucciones PPC de la función, **sondeos** = cuántos `process_pending_callbacks` emite el
código generado (uno por salto hacia atrás, más el de `sc`). **HLE** = se puede hacer un reemplazo nativo bit a bit
(✔ sí, sin complicaciones · ◐ sí, con trabajo o riesgo · ✘ no compensa / no es CPU).

### 2.1 Las funciones de la lista

| Dirección | Nombre real | Biblioteca | Qué hace | insts / bucles (sondeos) | % pc invitado | % propio host | HLE |
|---|---|---|---|---|---|---|---|
| 801A3880 | callback de reverb AXFX (HandleReverb; ya nativo: `nwii_r9_axfx_native`) | AXFX | reverb de 96 muestras × 3 canales | 329 (2) | 8,83 | 8,79 (nativo) | ya hecho; falta `apply_r9_axfx_fast` |
| 801A5C10 | **probable `AXFXDelayCallback`** (delay entero) | AXFX | 96 muestras × 3 canales, líneas de retardo con `mullw` y escala /128 | 158; bucle de 96 vueltas × 121 insts (1) | 1,02 | 0,77 | ✔ (traducción nativa, todo entero) |
| 802821E0 | sin símbolo; región `nw4r::snd::detail` (Voice/AxVoice) | NW4R snd | cálculo de parámetros por voz (usa `__cvt_fp2unsigned`, psq) | 747 (0) | 0,25 | 0,59 | ✘ (lógica de middleware) |
| 801A2B10 | **`__AXSyncPBs`** (AXVPB.c) | AX | invalida los PB, recorre 32 pilas de prioridad y sus voces (PB de 320 B), copia/sincroniza, vacía los PB | 196; bucles por prioridad/voz (3) | 0,99 | 0,45 | ◐ (lo caro son sus `DC*`; punto 5.1) |
| 802E1AB4 | `strcmp` | MSL | compara por palabras (truco 0xFEFEFEFF/0x80808080) y luego por bytes | 71; 3 bucles (3) | 1,62 | 0,35 | ✔ |
| 801C7350 | `GXLoadTexObjPreLoaded` | GX | escribe los registros BP de la textura y la sombra `__GXData` | 89 (0) | 0,80 | 0,27 | ◐ (lo caro es la tubería GX) |
| 80250E70 | sin símbolo; región `nw4r::g3d` (CalcWorld/CalcView, caché bloqueada 0xE000_0000) | NW4R g3d | recorre nodos y arma matrices de mundo | 369; bucle de 273 insts (2) | 0,34 | 0,27 | ✘ |
| 801C0540 | `PSMTXConcat` | MTX | producto de matrices 3×4 con paired-singles | 51 (0) | 0,72 | 0,26 | ✔ (cuidado con FP; 4.4) |
| 801D6B60 | **probable `CXReadUncompLZ`** (descompresión LZ por partes, cabecera 0x10) | CX | descomprime archivos `.LZ` mientras llegan del disco | 235; 5 bucles (5) | 1,06 | 0,25 | ✔ (traducción nativa; solo en cargas) |
| 80264400 | sin símbolo; `nw4r::snd::detail` (con `OSDisable/RestoreInterrupts`) | NW4R snd | actualización de voces/canales | 158 (0) | 0,24 | 0,25 | ✘ |
| 801AC440 | `DCInvalidateRange` | OS | `dcbi` por línea de 32 B | 11; bucle de 3 insts (1) | **4,31** | 0,23 | ✔✔ |
| 80273560 | sin símbolo; `nw4r::snd` (Seq/SoundArchivePlayer, con `OSLockMutex`) | NW4R snd | actualización de secuencias | 350 (4) | 0,28 | 0,22 | ✘ |
| 801AC470 | `DCFlushRange` | OS | `dcbf` por línea + `sc` (sync) | 12; bucle de 3 insts (1 + el de `sc`) | **3,67** | 0,20 | ✔✔ |
| 801C6D90 | `GXInitTexObj` | GX | rellena un `GXTexObj` (llama a `memset`, empaqueta campos de bits) | 131 (0) | 0,11 | 0,20 | ◐ |
| 80119C50 | sin símbolo; código del juego | juego | bucle de 66 elementos: `PSMTXScale` + `PSMTXConcat` + m3d (`setLocalMtx`, `setScale`, `calc`) | 300; bucle de 181 insts × 66 (3) | 0,92 | 0,18 | ✘ |
| 802DCBFC | `__ptmf_scall` | runtime de CodeWarrior | llamada a un puntero a método (ajusta `this` y salta) | 10 (0) | 0,11 | 0,18 | ✘ (trampolín; nada que ganar) |
| 802631F0 | sin símbolo; `nw4r::snd::detail` | NW4R snd | gestor de voces (llama a 80264400) | 392 (0) | 0,19 | 0,18 | ✘ |
| 8027E080 | sin símbolo; `nw4r::snd` | NW4R snd | — | 201 (5) | 0,29 | 0,18 | ✘ |
| 801C52F0 | `__GXSetDirtyState` | GX | vuelca a la FIFO el estado sucio de `__GXData` | 158; 2 bucles (2) | 0,39 | 0,17 | ◐ |
| 801A0E90 | **probable `__AXNextFrame`** (AXCL.c: arma la lista de comandos del DSP) | AX | escribe la lista de comandos (`sth`) con las direcciones de aux y la vacía | 623 (0) | 0,17 | 0,17 | ◐ |
| 802AE9C0 | sin símbolo; `nw4r::lyt::TextBox` (usa `Font::GetCharStrmReader`) | NW4R lyt | texto de la interfaz | 248 (1) | 0,21 | 0,17 | ✘ |
| 801B4EA0 | **`SelectThread`** (OSThread.c) | OS | planificador; en `0x801B4FF8` el bucle ocioso, que llama a `idle_wait` (duerme) | 139 (2) | 30,41 (casi todo dormido) | 0,17 | ✘ (es espera) |
| 802CC730 | sin símbolo; **probable `EGG::LightTexture`** (dibujo) | EGG | dibuja la textura de luz (GXBegin, carga de matrices) | 176 (0) | 0,22 | 0,17 | ✘ |
| 8022D720 | sin símbolo; **probable `nw4r::ut::CharWriter`** (`GXInitTexObj`/`LOD`/`GXLoadTexObj`/`GXBegin`) | NW4R ut | carga de la textura de un glifo | 159 (0) | 0,37 | 0,16 | ✘ |
| 80267F40 | sin símbolo; `nw4r::snd::detail::BasicSound` | NW4R snd | actualización de parámetros del sonido | 439 (2) | 0,24 | 0,16 | ✘ |
| 801C6FF0 | `GXInitTexObjLOD` | GX | LOD de la textura: floats → enteros (`fctiwz`), rangos | 65 (0) | 0,15 | 0,15 | ◐ (FP exacto) |
| 8024C2B0 | sin símbolo; `nw4r::g3d` (animación, entre `AnmObj` y `G3DState`) | NW4R g3d | interpolación de animación | 389 (0) | 0,24 | 0,15 | ✘ |
| 802B3B00 | sin símbolo; **probable `nw4r::lyt::Material::SetupGX`** | NW4R lyt | configura TEV, indirectas y texturas | 898 (9) | 0,23 | 0,15 | ✘ |
| 802CC050 | sin símbolo; `EGG::LightTexture` (región) | EGG | idem; bucle de 12 insts | 265 (2) | 0,35 | 0,15 | ✘ |

### 2.2 Los PC invitados más calientes

| PC | Dónde | Qué es | % |
|---|---|---|---|
| 0x801B4FF8 | `SelectThread` | sondeo del bucle ocioso (`lwz RunQueueBits; cmpwi; beq`) → `idle_wait` | 30,25 |
| 0x801AC464 | `DCInvalidateRange` | sondeo del `bdnz` (1 por línea de 32 B) | 4,30 |
| 0x801AC494 | `DCFlushRange` | sondeo del `bdnz` | 3,65 |
| 0x80225500 | `IOS_IoctlAsync` (HLE del runtime) | lecturas de disco del DI hechas en el acto (`fsFileRead`, `svcSendSyncRequest`) | 2,56 |
| 0x802E1BC4 | `strcmp` | sondeo del bucle por bytes | 1,22 |
| 0x801C6150 | `GXCopyDisp` | entrada; la copia al XFB espera al anfitrión (`FifoWaitForHost`) | 1,18 |
| 0x801C0500 | `PSMTXCopy` | entrada (sin bucles) | 0,45 |
| 0x801C9A80 | `GXLoadTexMtxImm` | entrada | 0,42 |
| 0x801D6E90 | `CXReadUncompLZ` | sondeo de la copia byte a byte de una referencia LZ | 0,67 |
| 0x801A2DE4 | `__AXSyncPBs` | sondeo del bucle por voz | 0,66 |
| 0x8011A0B8 | juego (80119C50) | sondeo del bucle de 66 elementos | 0,82 |
| 0x80082E94 | juego (`dBg_c`, región de `dBgParameter`) | sondeo de un bucle de 88 insts | 0,47 |
| 0x802E87CC / 0x802E8754 | `sqrt` (fdlibm `__ieee754_sqrt`, dentro de `func_802E861C`) | sondeos de los dos bucles bit a bit | 0,35 / 0,24 |
| 0x801AC4F4 | `DCFlushRangeNoSync` | sondeo del `bdnz` | 0,35 |
| 0x80162C50 | `fLiMgBa_c::searchNodeByID` (f_line.cpp) | sondeo de la búsqueda en lista | 0,32 |
| 0x80294F28, 0x80294DE8, 0x80294D38 | región `nw4r::ef` (buscar recurso por nombre) | bucles que llaman a `strcmp` por entrada | 0,28 + 0,21 + 0,13 |
| 0x801D82BC | `CXUncompressLZ` | copia de 6 insts | 0,25 |
| 0x801AC554 | `DCZeroRange` | sondeo del `bdnz` (`dcbz`) | 0,22 |
| 0x800044B8, 0x800044D4 | `memcpy` | **bucles byte a byte de 3 insts** (cola y copias cortas) | 0,21 + 0,13 |
| 0x801B119C | `OSRestoreInterrupts` | retorno de interrupción que se interpreta (`resume_pruned`, ya visto en r8) | 0,32 |

Por biblioteca, en "pc invitado": **AX+AXFX 11,6 %** (más 8,6 % de `DC*` que llaman sobre todo ellos),
**GX 8,2 %**, **MTX 2,1 %**.

---

## 3. Cómo se reemplaza hoy una función (lo que ya existe)

1. **`[hle_hooks]` en el TOML** (`recomp/nsmbw.toml`; lo emite `NWiiRecomp/nWiiRecomp/src/recompiler.cpp`,
   `emit_function`). Para la dirección dada, el cuerpo entero de la función queda en:
   ```cpp
   void func_801DFA50(CPUContext& ctx) {
       NWII_HLE_Nop(ctx);          // handler nativo, por nombre
       ctx.pc = ctx.lr; return;
   }
   ```
   NSMBW lo usa para el WPAD/KPAD (`NWII_HLE_Nop`, `NWII_HLE_Return0`, en `src/hle/wpad_hle.cpp`). Inconvenientes:
   hay que regenerar y recompilar todo (`FULL=1`), no queda el código original como respaldo y no se puede
   comparar en marcha.
2. **Enlace por nombre de símbolo** (`hle_handlers()` en el mismo `recompiler.cpp`): `OSAllocFromHeap`,
   `OSCreateHeap`, `OSFreeToHeap`, `OSInitAlloc`, `OSReport`, `PADInit`, `PADRead`, `VIWaitForRetrace`.
   `symbols/usa_symbols_nohle.csv` quita esos nombres para que no se enlacen solos.
3. **Costuras GX que existen pero no están activas en NSMBW** (`src/hle/gx/gx_matrix_seam.cpp`,
   `gx_state_seam.cpp`: `GXLoadPosMtxImm`, `GXLoadTexMtxImm`, `GXSetCullMode`...). **No son 1:1**: copian las
   palabras tal cual en vez de hacer `psq_l`/`psq_st` (sin el paso a cero de los subnormales), no dejan los registros
   como el SDK y las de estado no tocan la sombra `__GXData`. El propio recompilador avisa de que eso rompió
   las LYT. No hay que reutilizarlas tal cual.
4. **Pasadas sobre el código generado** (`r9/cpucode/perf/`): `apply_r9_axfx.py` (cuerpo nativo de la reverb,
   **es el modelo a seguir**), `apply_r9_axfx_fast.py`, `apply_r9_psq.py` (psq en línea), `apply_r9_crdse.py`
   (CR muertos), y antes `perf/apply_r8_codegen.py` y `apply_fastdispatch.py`. Todas comprueban el sha256 del
   texto que esperan, marcan el archivo (`// [r9axfx]`) y se niegan a correr dos veces. En r10a solo está
   aplicada `[r9axfx]`.

### 3.1 Diseño propuesto: `perf/apply_r10_hle.py`

Por cada función de la lista (empezando por las `DC*`):

```cpp
void DCInvalidateRange_801AC440(CPUContext& ctx) {
    if (nwii::runtime::g_trace_calls && ctx.pc == 0x801AC440) nwii::runtime::trace_call(0x801AC440, ctx);
    if (ctx.pc == 0x801AC440 && nwii_hle_801AC440(ctx)) return;   // [r10hle]
    if (ctx.pc != 0x801AC440) { switch (ctx.pc) { ... } }          // como antes
    ...                                                            // cuerpo generado = respaldo y referencia
}
```

- Va **detrás** de la traza y **solo si `ctx.pc == entrada`**. Si se vuelve de una interrupción a mitad de la
  función (un `case` del `switch`), sigue el código generado.
- `nwii_hle_X` devuelve `false` (y no toca nada) si no se cumple alguna condición previa: `NWII_HLE_X=0`, ronda de
  verificación, `NWII_WATCH` puesto, memoria que no sea RAM normal (MMIO, la tubería GX `0xCC008000`, la caché
  bloqueada `0xE0000000` cuando no se soporte, regiones partidas entre MEM1 y MEM2) o un caso raro (punto 5).
- Si devuelve `true`, `ctx` queda **exactamente** como lo deja el código generado en su `blr`: todos los GPR, FPR,
  PS1, CR, CTR y XER que la función escribe (también los volátiles), `inst_count` con la misma suma y
  `ctx.pc = ctx.lr`. Las escrituras a memoria son las mismas, incluidas las de la pila (`stwu`, `stfd`).
- Las llamadas indirectas (`nwii_dispatch`, tabla de `fastdispatch.cpp`) van al mismo símbolo, así que también
  pasan por el reemplazo.
- Los archivos tocados se recompilan (`FULL=1` si la pasada cambia `cpu_context.h`; si solo cambia
  `output_*.cpp`, se recompilan esas unidades). Los ayudantes comunes van en una cabecera nueva del runtime
  (`include/runtime/hle_leaf.h`) para que los use también `difftest`.

**¿Por qué reproducir también los registros volátiles?** Ningún código de CodeWarrior lee `r0`, `r5-r12`, `f0-f13`,
`ctr` ni `cr0` después de una llamada, pero si llega una interrupción justo después, `OSSaveContext` escribe todos
los registros en el `OSContext` del hilo, **en la RAM del juego**. Si no son los mismos, la RAM deja de ser bit a
bit igual. Además, los comparadores (`difftest`, `NWII_*_VERIFY`) los comparan. En todas estas funciones el
estado final se calcula fácil.

---

## 4. Semántica que hay que copiar exactamente

### 4.1 Sondeos de interrupción

El código generado sondea en cada salto hacia atrás y en `sc`. Hay tres formas correctas de tratarlos en un
reemplazo:

- **(a) Solo con MSR[EE] apagado** (así lo hace la reverb AXFX): con EE apagado un sondeo no puede entregar nada.
  Con EE encendido se ejecuta el código generado.
- **(b) "Avanzar hasta el último sondeo"** (para los bucles sin efectos, como las `DC*`): se deja el estado como
  justo antes del último `bdnz` (`ctr = 1`, `r3` final, `inst_count` hasta ahí), se pone `ctx.pc` en la dirección
  de ese sondeo (su `case` existe), se llama a `process_pending_callbacks` y, si no entrega nada, se termina
  (`ctr = 0`, `+1`, `pc = lr`). Si entrega una interrupción, al volver se reanuda en el `case` y el código
  generado hace la última vuelta: idéntico. Equivale a una ejecución en la que los sondeos anteriores no
  encontraron nada. Eso ya puede pasar hoy, porque el reloj es de pared.
- **(c) Quitarlos** en funciones hoja cortas y acotadas (`strcmp`, `sqrt`, `PSMTX*`, `memcpy` de pocos KB). Es
  como si la función no tuviera bucles (como `PSMTXConcat`, que no sondea nunca). La interrupción llega unos
  microsegundos después, en el siguiente sondeo de quien llamó. Ninguna de estas funciones espera a algo que
  cambie una interrupción.

### 4.2 Coma flotante (`cpu_context.h`)

- `fmuls`/`ps_mul*`: `ppc_single(a * ppc_force25(c))`. `ppc_force25` redondea la mantisa de C a 25 bits (como
  Dolphin).
- `fmadds`/`ps_madd*`/`ps_madds0/1`: `ppc_single(ppc_madd_s(a, c, ±b))`, donde
  `ppc_madd_s = std::fma(a, force25(c), b)` en double más un ajuste del empate exacto a single
  (`(bits & 0x1FFFFFFF) == 0x10000000`). Así redondea **una sola vez**, como el hardware. `fnmadds`/`ps_nmadd`
  niegan sin tocar los NaN.
- `psq_l` de tipo float: carga exacta. `psq_st` de tipo float: `ppc_single_bits_ftz` (los subnormales se guardan
  como cero). `stfs`: `ppc_single_bits` (desnormaliza). `lfd`/`stfd`: 64 bits crudos, y `lfd` **solo escribe ps0**.
- Switch: `-ffp-contract=off` (`cmake/nsmbw-switch.cmake`), para que GCC no junte multiplicaciones y sumas.

**¿Sirve NEON?** Con `float32x4` (`FMLA`/`FMUL` en single), no del todo. Si las entradas son singles (lo normal tras
`psq_l`), coincide en los casos normales: el producto de dos singles es exacto en double, y `force25` no cambia
un single. Pero se aparta en tres casos: resultados subnormales en single (el ajuste del empate solo cubre la
posición de redondeo normal y el doble redondeo puede diferir), operandos C que no sean singles (ahí `force25`
sí redondea) y el signo y la carga de los NaN. Con **`float64x2_t`** sí: los dos carriles son ps0/ps1,
`vfmaq_f64` es la misma instrucción `fmadd d` que `std::fma` en AArch64, se comprueba el empate por carril (rama
casi nunca tomada) y `vcvt_f32_f64` es el mismo `(float)`. Eso da los mismos bits que el runtime, NaN incluidos.
La versión escalar en double con las funciones de `cpu_context.h` ya es exacta. NEON solo ganaría en
`PSMTXConcatArray` o `PSMTXMultVecArray`.

**Detalle de `PSMTXConcat` que hay que copiar**: guarda f14, f15 y f31 con `stfd` y los recupera con `lfd`, que solo
restaura **ps0**. El **ps1** de f14/f15/f31 queda con los valores intermedios del cálculo. El reemplazo tiene que
dejar exactamente eso, igual que escribir los 64 bits de los tres en la pila (`r1-56`, `r1-48`, `r1-24`) y el
enlace del marco en `r1-64`.

### 4.3 `inst_count`

El código generado suma por bloque (`ctx.inst_count += k`). Con el reloj de pared solo se usa para el latido del
log, pero sin él (`NWII_REALTIME=0`) **es el reloj del juego**. El reemplazo debe sumar lo mismo, calculado por el
camino que se tomó (en las `DC*`: `2 + 5 + 3·n + 1`).

---

## 5. Notas por función y diseño del reemplazo

### 5.1 `DCInvalidateRange`, `DCFlushRange`, `DCFlushRangeNoSync`, `DCStoreRange(NoSync)`, `DCZeroRange`, `ICInvalidateRange` — prioridad 1

**Quién los llama tanto.** Por los PC de retorno del perfil y el desensamblado:

- `__AXSyncPBs` (`801A2B10`) en cada cuadro de AX (≈331/s, según `ax thread 331 lists/s` en el log):
  `DCInvalidateRange(PBs, N·320)` + `DCInvalidateRange(sync, N·64)` al empezar, y `DCFlushRange` de lo mismo al
  terminar. N = número de PB (`r13-0x506C`). Son **10 líneas por PB + 2 de la tabla de sincronización**: con N=96,
  1 152 vueltas al invalidar y otras 1 152 al vaciar.
- `__AXProcessAux` (`801A08F0`, probable): por cada bus aux activo, `DCInvalidateRange` de 1536/1152 B
  (48/36 líneas), el callback AXFX y `DCFlushRangeNoSync` de lo mismo. Si el bus está inactivo, `memset` +
  `DCFlushRange`.
- `__AXNextFrame` (`801A0E90`): `DCFlushRange` de la lista de comandos.
- `nw4r::g3d` (`80250230`, `80250710`, `8025A520`): `DCInvalidateRange(n·48)` y `(n·36)` de los arreglos de
  matrices antes del DMA de la caché bloqueada.

Unas 2 600 vueltas por cuadro de AX son ~860 000/s. Con el sondeo de r8 (~100 ns en la Switch) dan las ~8,6 % del
perfil, así que las cuentas cuadran. Con el sondeo inline de r10a (~20 instrucciones AArch64 por vuelta, visto en
`output_25.o`) sería ~0,8-1 %.

**Reemplazo exacto de `DCInvalidateRange` (y, con un cambio, de `DCStoreRange`/`ICInvalidateRange`):**
```cpp
// r3 = dirección, r4 = tamaño. cmplwi r4,0 ; blelr ; r5 = r3&31 ; r4 = (r4+r5+31)>>5 ; mtctr r4 ; bucle
bool nwii_hle_801AC440(CPUContext& c) {
  const uint32_t len = c.gpr[4];
  const uint32_t r5 = c.gpr[3] & 31, n = (len + r5 + 31) >> 5;   // == rotl(x,27) & 0x7FFFFFF
  if (len != 0 && n == 0) return false;                           // 2^32 vueltas: al código generado (sin tocar nada)
  c.cr[0].lt = false; c.cr[0].gt = len != 0; c.cr[0].eq = len == 0; c.cr[0].so = c.xer >> 31;
  c.inst_count += 2;
  if (len == 0) { c.pc = c.lr; return true; }
  c.gpr[5] = r5; c.gpr[4] = n;
  c.inst_count += 5 + 3ull * n;
  c.gpr[3] += 32u * n;                                            // valor tras la última addi
  c.ctr = 1; c.pc = 0x801AC464;                                   // justo antes del último bdnz
  if (process_pending_callbacks(c)) return true;                  // interrupción: se reanuda en el case
  c.ctr = 0; c.inst_count += 1; c.pc = c.lr; return true;         // blr
}
```
En el código generado, cada vuelta hace `inst_count += 3`, `dcbi` (nada) y `addi r3,r3,32`, sondea con
`ctx.pc = 0x801AC464` y solo después hace `ctr--`. En el último sondeo el estado es justo el de arriba: `r3`
final, `ctr = 1` e `inst_count` con `2 + 5 + 3·n`. Si se entrega una interrupción, al volver se reanuda en el
`case 0x801AC464`, que hace `ctr-- → 0` y el `blr`, igual que el código generado. `difftest` lo confirma (punto 8).

- `DCFlushRange` es igual con 0x801AC494 como último sondeo. Al terminar falta el `sc`: `inst_count += 1`,
  `ctx.pc = 0x801AC49C`, `process_pending_callbacks`, y luego `inst_count += 1` y `pc = lr`. Es lo mismo que hace
  el código generado (el `sc` solo sondea; `status/handoff_cpu.md`).
- `DCFlushRangeNoSync`/`DCStoreRangeNoSync`: sin `sc`.
- `DCZeroRange` **sí escribe**: `dcbz` pone a cero cada línea de 32 B alineada (`CPUContext::dcbz`: `memset`
  si las 32 B están en el mismo bloque, si no 8 × `write32`). El reemplazo llama a `c.dcbz(ea)` por línea o hace
  un `memset` del rango alineado si todo es RAM normal. Los registros quedan igual que arriba.
- `ICInvalidateRange`: `icbi` + `sync` + `isync`. Sin efecto en código recompilado; el mismo esquema, sin `sc`.

**Riesgo:** casi nulo. Son 12 instrucciones y no tocan memoria (salvo `dcbz`). **Ganancia:** el 8,6 % del perfil
o ~1 % en r10a. Además, quita miles de sondeos por cuadro de AX que llegan mientras se cocinan las interrupciones
de audio.

### 5.2 Reverb AXFX (`801A3880`) — ya nativa

`nwii_r9_axfx_native` es la traducción línea a línea, sin `ctx` (`apply_r9_axfx.py`). Se comprobó bit a bit con
`NWII_AXFX_VERIFY` (651/651, `r9/cpucode/LEEME.md`). Aun así ocupa **8,8 %**: está limitada por la memoria y los
accesos a través de `MemView`. El siguiente paso ya está escrito: **`apply_r9_axfx_fast.py`** (muestras 0-94 sin
las comprobaciones por acceso, con punteros directos tras comprobar que las regiones no se solapan; la 95 la
hace el cuerpo nativo para dejar los registros exactos). No va en r9combo ni en r10a. Hay que aplicarlo, pasar
`NWII_AXFX_VERIFY` en PC y medir en la Switch.

### 5.3 Delay AXFX (`801A5C10`, probable `AXFXDelayCallback`) — prioridad alta

Bucle de 96 vueltas (`li r25,96; mtctr`) de 121 instrucciones, **solo enteros** (`lwz`, `add`, `mullw`, escala
`128 − x`, `stw`), tres líneas de retardo y entradas y salidas de 3 canales. Se llama con `_savegpr_20` /
`_restgpr_20` (`802DCD58` / `802DCDA4`). Se aplica la misma receta que a la reverb: traducción mecánica desde
después de `_savegpr` hasta antes de `_restgpr`, con los registros en variables locales y el sondeo del `bdnz`
(0x801A5E6C) tratado como en (a), porque el callback corre con EE apagado. Al ser todo entero no hay ninguna
duda de FP. Se puede reutilizar casi entero el script de `apply_r9_axfx.py` (cambian `FUNC`, `START`, `END`, el
sha256 y el nombre). Ganancia ~1 % y riesgo bajo.

### 5.4 `__AXSyncPBs` (`801A2B10`), `__AXProcessAux` (`801A08F0`), `__AXNextFrame` (`801A0E90`), sincronización por voz (`801A25C0`)

Son el lado de la CPU del DSP, que el runtime ya emula (`src/hle/ax/`). Tienen estado y bastante lógica (pilas
de prioridad, banderas de sincronización por campo, `memcpy` de 320 y 48 B por voz). Un HLE completo sería
posible, porque el DSP ya está en HLE, pero es caro de escribir y de verificar. **Mejor:** con las `DC*` (5.1),
`memcpy` (5.7) y, si hace falta, una traducción nativa del bucle por voz, se consigue casi todo. Solo `801A2B10`
suma ~1 %.

### 5.5 `strcmp` (`802E1AB4`) y `strncmp` (`802E1BD0`) — prioridad alta

71 instrucciones y tres bucles: alineación byte a byte (hasta 3 vueltas), palabra a palabra con la detección de
cero `(w − 0x01010101) & ~w & 0x80808080` y cola byte a byte. Hay **684 llamadas en el código**. Las calientes
son las búsquedas de recursos por nombre de `nw4r::ef` (`80294E90`, `80294D60`, `80294CA0`: recorren una tabla
y llaman a `strcmp` por entrada) y el código del jugador (`800D4EF0`, `800DF570`). Suman 1,6 % + 0,6 % en los
buscadores.

Reemplazo: **los mismos tres bucles en variables locales**, con punteros del anfitrión a los dos textos (con la
condición de que `[p, p+len+4)` esté en RAM normal; si no, el código generado). Así se obtienen sin pensar los
valores finales de `r0`, `r3`-`r8`, `ctr`, `cr0` e `inst_count`, que dependen del camino. Se quitan los sondeos
(4.1 c). Las lecturas por palabra pueden pasarse hasta 3 bytes del final del texto, igual que el original: solo
son lecturas. Ganancia ~1,5 % en el perfil. Riesgo bajo.

### 5.6 `PSMTXConcat` (`801C0540`), `PSMTXCopy` (`801C0500`), `PSMTXIdentity` (`801C04D0`), `PSMTXConcatArray` (`801C0610`), resto de `PSMTX*`/`PSVEC*` — prioridad media

- `PSMTXConcat`: 51 instrucciones sin saltos. 12 `psq_l` de pares de A y B (todas **antes** del primer `psq_st`,
  así que `AB == A` o `AB == B` es seguro), 1 `psq_l` de la constante `{0,1}` en `0x804295A0`, 6 `ps_muls0`,
  15 `ps_madds0/1`, 6 `psq_st`, `stfd`/`lfd` de f14, f15 y f31 en la pila. Hay 499 llamadas en el código.
- Reemplazo: el mismo orden de operaciones con `ppc_force25`, `ppc_madd_s`, `ppc_single` y
  `ppc_single_bits_ftz`, sobre `double` locales (o `float64x2_t`), con el mismo `MemView` (que también trata la
  caché bloqueada). Hay que dejar los registros `f0`-`f15` y `f31` (ps0 y ps1, con el ps1 "pisado" de 4.2), `r6`
  y `r1`, la pila y `inst_count += 51`. La condición previa es GQR0 == 0 (float sin escala), que es lo que
  configura el SDK. Si no se cumple, el código generado.
- `PSMTXCopy`: 6 `psq_l` + 6 `psq_st` de pares (f0-f5). **No es un `memcpy` de 48 B**: `psq_st` de tipo float
  pasa los subnormales a cero (FTZ). Reemplazo: copiar cada float con `ppc_single_bits_ftz` (o `memcpy` si ningún
  float es subnormal) y dejar f0-f5.
- `PSMTXIdentity`: constantes.
- En r8 se descartó este HLE (`r8/codegen/LEEME.md` §2e) porque PSMTXConcat ya había bajado de 470 a 316 ns y
  "no compensa el riesgo". En la Switch, MTX es **2,1 %** del hilo y una versión nativa pasaría de ~300 ns a unas
  decenas. Con `difftest` (millones de matrices aleatorias, NaN, inf y subnormales incluidos) el riesgo queda
  medido. **Antes de esto, aplicar `apply_r9_psq.py`** (psq en línea, 1:1, ya escrita): en el perfil
  `psq_load`+`psq_store` son 2,6 % propio y r10a no la lleva.

### 5.7 `memcpy` (`80004364`) y `memset` (`800046B4`) — prioridad media

`memcpy` es la versión optimizada para Gekko. Sin bytes no hace nada. Si `src < dst` copia hacia atrás. Copias de
128 B o más con la misma alineación módulo 8: bloques de 32 B con 4 `lfd` + 4 `stfd` (pasan por f1-f4 y dejan en
ellos el último bloque). De 20 B o más con la misma alineación módulo 4: bloques de 16 B con `lwz`/`stw`. Las colas
y **todo lo demás (menos de 20 B, o alineaciones distintas, de cualquier tamaño)** van **byte a byte con un sondeo
por byte** (`0x8000441C`, `0x800044B8`, `0x800044D4`). Una copia grande desalineada son 3 instrucciones y un sondeo
por byte. La semántica resultante en memoria es
la de `memmove`: cada bloque se lee entero antes de escribirlo, y la dirección se elige según el solapamiento.

Reemplazo: si los dos rangos son RAM normal del mismo banco y no tocan la tubería GX, `std::memmove` y luego los
registros finales en forma cerrada: `r3` intacto, `r4`, `r8`, `r9`-`r12`, `r5`, `r6`, `ctr`, `cr0`/`cr1`/`cr5`
según el camino, y **f1-f4 = los 64 bits del último bloque de 32 B** con el ps1 sin tocar. Es más fácil hacer
una traducción en locales con el `memmove` en medio y solo los registros calculados. 0,9 % en el perfil.
`memset` no aparece caliente: mismo esquema si hace falta.

### 5.8 `sqrt` (fdlibm `__ieee754_sqrt`, en `func_802E861C`, código desde `802E8620`) — prioridad media-baja

Raíz bit a bit del fdlibm de MSL: dos bucles (13 y 28 instrucciones, unas 22 + 32 vueltas), `errno = 33`
(`r13-18072`) en los errores de dominio. 0,6 %. El resultado de fdlibm es la raíz correctamente redondeada, igual
que `fsqrt` del anfitrión, pero los registros enteros intermedios (`r0`, `r3`-`r12`, `cr0`, `cr1`) y la pila
(`stfd f1` en `r1-24`) también hay que dejarlos. Opciones: (1) traducción en locales de los dos bucles, exacta
por construcción, ~5-10× más rápida; (2) `std::sqrt` y reconstruir los registros con el algoritmo entero.
Solo para `x > 0` finito; ±0, negativos, inf y NaN van al código generado.

### 5.9 `CXReadUncompLZ` (`801D6B60`, probable) y `CXUncompressLZ` (`801D8190`) — solo en cargas

Descompresión LZ (cabecera 0x10, por partes según llegan los datos del disco), estado en una estructura
`{destp, destCount, forceDestCount, flags, flagIndex, length, ...}` en la RAM del juego. Todo con enteros y
bytes. 1,4 % del perfil, pero **solo durante las cargas**, así que no sube los fps. Sí acorta las pantallas de
carga. Se hace con una traducción en locales con punteros del anfitrión (el `memcpy` de referencias solapadas
tiene que ser byte a byte, como el original). Riesgo bajo.

### 5.10 `fLiMgBa_c::searchNodeByID` (`80162C30`) y `countNodeByProfName` (`80162C60`)

`f_line.cpp` del framework: recorre la lista enlazada y devuelve el nodo cuyo `fBase_c` tiene ese ID
(`fManager_c::searchBaseByID` usa 8 de estas listas). Son 7 instrucciones por nodo y 0,33 %. El reemplazo es un
bucle de lecturas de 32 bits y deja `r3` (nodo o 0), `r5`, `r0` y `cr0` del último paso. Es trivial y exacto.

### 5.11 GX (8,2 % en conjunto)

El coste está **repartido**: `GXCopyDisp` 1,18 (espera, ver 5.12), `GXLoadTexObjPreLoaded` 0,80,
`GXLoadTexMtxImm` 0,42, `__GXSetDirtyState` 0,39, `GXSetFog` 0,26, `GXSetTevIndirect` 0,20, `GXLoadPosMtxImm`
0,20, `GXSetTevOrder` 0,15, `GXInitTexObjLOD` 0,15, `GXInitTexObj` 0,11... y otras ~30 de menos de 0,15 %. Casi
todas son 20-90 instrucciones con varias escrituras a `0xCC008000`, y cada escritura pasa por
`MMU::write*_slow` → `wgp_write32` (1,48 % propio), `GX_WGPIPE_Write8` (0,44 %) y `wg_flush_now` (1,03 %). El log
dice **~160 000 vaciados/s de 11 B de media**: la "regla BP" de `gx.cpp` entrega el búfer a la cola del
anfitrión (mutex + `vector::insert`) **4 bytes después de cada byte 0x61**, y cada comando BP lleva uno.

- Un HLE de cada función GX tiene que escribir **los mismos bytes en la FIFO y los mismos campos de `__GXData`**
  (la sombra que luego relee `__GXSetDirtyState`). Es posible y comprobable con `NWII_WGP_CHECK`, que ya graba lo
  escrito y lo entregado, pero ahorra solo el lado del juego de ~40 funciones pequeñas.
- **Más rentable y también 1:1 para el juego:** abaratar el camino del runtime. Una entrada
  `GX_WGPIPE_WriteBlock(bytes, n)` para los reemplazos, y una regla BP que vacíe solo con los registros que el
  anfitrión necesita ver al momento (0x45 fin de dibujo, 0x47/0x48 tokens, 0x52 copia) o una cola sin mutex.
  Esto no cambia nada de lo que el juego ve (RAM, registros, interrupciones), porque el anfitrión ya recoge la
  FIFO en momentos arbitrarios. Pero es terreno del frente de render y hay que medirlo con `NWII_WGP_CHECK`.
- `GXLoadTexMtxImm`/`GXLoadPosMtxImm`: 12 (u 8) `psq_l` del origen y `psq_st` a la tubería (FTZ). Una versión
  exacta escribe `0x10`, la cuenta, la dirección y los 12 floats con `ppc_single_bits_ftz`, y deja los registros
  FP como el SDK. Las costuras de `gx_matrix_seam.cpp` **no** hacen el FTZ.

### 5.12 Lo que no es CPU del juego

- **`SelectThread` / `0x801B4FF8` (30 %)**: el hilo está ocioso y duerme en `idle_wait`. Es margen, no coste.
- **`GXCopyDisp` (`801C6150`, 1,18 %)**: la escritura del BP 0x52 con el bit 14 (copia al XFB, fin de cuadro)
  hace que `wgp_write32` llame a `FifoWaitForHost()` cuando hay demasiados cuadros en cola. Es la espera al hilo
  de render, y un HLE no la quita.
- **`IOS_IoctlAsync` (`0x80225500`, 2,56 %)**: el HLE del DI lee el archivo de la SD en el acto (`fsFileRead`,
  `svcSendSyncRequest` 2,36 %). Solo afecta a las cargas. Se podría mejorar con lectura anticipada o en otro hilo,
  pero eso es del runtime, no de las funciones del juego.
- **`OSRestoreInterrupts` / `0x801B119C`**: el retorno de interrupción que se interpreta (ya documentado en r8).

### 5.13 Middleware y juego (nw4r snd/g3d/lyt/ut/ef, EGG, `d_*`)

`802821E0`, `80264400`, `802631F0`, `80267F40`, `80273560`, `8027E080` (snd), `80250E70`, `8024C2B0` (g3d),
`802AE9C0`, `802B3B00` (lyt), `8022D720` (ut), `802CC050`, `802CC730` (EGG), `80119C50`, `80082BE0` (juego).
Cada una pesa entre 0,2 y 0,9 %. Son grandes, tienen estado y llaman a muchas otras, así que **no se reemplazan
por HLE**. Si alguna se vuelve dominante en r10a, se le aplica la "traducción nativa" de `apply_r9_axfx.py` a
su bucle interior (exacta por construcción), pero la ganancia esperada es pequeña (en la reverb fue ~10 → 8,8 %).
`__ptmf_scall` (`802DCBFC`, 10 instrucciones) es un trampolín: no hay nada que reemplazar.

---

## 6. Bucles calientes que sondean en cada vuelta

En r10a el sondeo es inline: decrementar `poll_left`, mirar MSR[EE] y tres cargas (`pi_intsr & pi_intmr`,
`pending_cb_count`, `poll_dec_due`). Sigue costando ~20 instrucciones AArch64 por vuelta, más guardar `ctx.pc`.

| PC del sondeo | Función | Instrucciones por vuelta | Vueltas (estimadas) | % pc invitado (perfil r8) |
|---|---|---|---|---|
| 0x801B4FF8 | `SelectThread` (ocioso) | 3 | hasta que haya hilo; duerme en `idle_wait` | 30,25 |
| 0x801AC464 | `DCInvalidateRange` | 3 | 1 por 32 B: ~1 150 por cuadro de AX en `__AXSyncPBs` (N=96) + 36-48 por bus aux + g3d n·48/32 | 4,30 |
| 0x801AC494 (+0x801AC49C `sc`) | `DCFlushRange` | 3 | ídem al terminar `__AXSyncPBs`; la lista de comandos; los aux inactivos | 3,65 |
| 0x801AC4F4 | `DCFlushRangeNoSync` | 3 | 48 o 36 por bus aux y cuadro de AX | 0,35 |
| 0x801AC554 | `DCZeroRange` | 3 (`dcbz`) | tamaño/32 | 0,22 |
| 0x802E1BC4, 0x802E1B7C, 0x802E1B2C | `strcmp` | 8 / 8 / 10 | cola ≤3 bytes; largo/4 palabras; alineación ≤3 | 1,22 + 0,20 |
| 0x802E1C04 | `strncmp` | 10 | ≤ n | 0,10 |
| 0x800044B8, 0x800044D4, 0x8000441C, 0x80004464 | `memcpy` | 3 (¡por byte!) | colas < 16/32 B, copias cortas o desalineadas | 0,21 + 0,13 |
| 0x801A5E6C | delay AXFX | 121 | 96 por llamada (≈331 llamadas/s y bus) | 0,91 |
| 0x801A2DE4, 0x801A2D80, 0x801A2D88 | `__AXSyncPBs` | 17 / 126 / 132 | por voz; por las 32 prioridades | 0,66 + 0,17 + 0,13 |
| 0x801D6E90, 0x801D6EC0, 0x801D6E04 | `CXReadUncompLZ` | 16 / 127 / 60 | 3-18 bytes por referencia; 8 marcas por byte de banderas | 0,67 + 0,20 + 0,17 |
| 0x801D82BC | `CXUncompressLZ` | 6 | por byte | 0,25 |
| 0x802E87CC, 0x802E8754 | `sqrt` (fdlibm) | 28 / 13 | ~32 / ~22 | 0,35 + 0,24 |
| 0x80162C50 | `fLiMgBa_c::searchNodeByID` | 7 | nodos de la lista (cientos) | 0,32 |
| 0x80294F28, 0x80294DE8, 0x80294D38 | búsquedas por nombre de `nw4r::ef` | 27 + `strcmp` | entradas de la tabla | 0,28 + 0,21 + 0,13 |
| 0x8011A0B8 | juego `80119C50` | 181 | 66 | 0,82 |
| 0x80082E94 | juego `80082BE0` (`dBg`) | 88 | ? | 0,47 |
| 0x801C0744 | `PSMTXConcatArray` | 36 | n matrices | 0,10 |
| 0x801C5488, 0x801C5514 | `__GXSetDirtyState` | 11 / 16 | etapas TEV / texturas | 0,12 + 0,09 |
| 0x802CC364 | EGG (`802CC050`) | 12 | ? | 0,18 |

Los bucles de 3 instrucciones (`DC*`, `memcpy` byte a byte) son donde el sondeo pesa más que el trabajo. Con el
reemplazo de 5.1 desaparecen las ~860 000 vueltas/s de las `DC*`.

---

## 7. Ranking

Estimación = % del perfil (r8) → lo esperable en r10a (sondeo inline), viabilidad y riesgo. "Ganancia" es lo que
se ahorraría del hilo del juego.

| # | Qué | % ahora (perfil) | Ganancia esperable | Viabilidad | Riesgo 1:1 | Nota |
|---|---|---|---|---|---|---|
| 1 | **`DC*`/`IC*` en forma cerrada** (5.1) | 8,6 | ~8 % con el sondeo de r8; **~1 % en r10a** | muy alta (12 insts, sin memoria) | muy bajo | `difftest` + `VERIFY`; primero |
| 2 | **`apply_r9_axfx_fast.py`** (5.2) | 8,8 | 2-4 % (a medir) | ya escrito | bajo (`NWII_AXFX_VERIFY`) | aplicar en r10a y verificar |
| 3 | **Delay AXFX nativo** (5.3) | 1,0 | 0,5-0,8 % | alta (copia del script de la reverb, solo enteros) | bajo | |
| 4 | **`strcmp`/`strncmp`** (5.5) | 1,7 | 1-1,5 % | alta | bajo | buscadores de `nw4r::ef` como extra |
| 5 | **`apply_r9_psq.py`** (no es HLE; ya escrito) | 2,6 propio | 1-2 % | ya escrito | bajo | lo usan todas las funciones con matrices |
| 6 | **`PSMTXConcat/Copy/Identity/ConcatArray`** (5.6) | 2,1 | 1-1,5 % | media (FP exacto, ps1 pisado) | medio-bajo con `difftest` | |
| 7 | **`memcpy`** (+`memset`) (5.7) | 0,9 | 0,5-0,8 % | media (registros por camino, f1-f4) | bajo | |
| 8 | **Camino de la tubería GX** (5.11, runtime) | ~2,9 propio (`wgp_*`, `wg_flush_now`) + parte de 8,2 GX | 1-3 % | media | bajo para el juego; hay que vigilar la sincronía con el anfitrión | frente render; `NWII_WGP_CHECK` |
| 9 | **`sqrt` fdlibm** (5.8) | 0,6 | 0,4-0,5 % | media | bajo | |
| 10 | **`searchNodeByID`** (5.10) | 0,33 | 0,25 % | muy alta | muy bajo | |
| 11 | **`CX` LZ** (5.9) | 1,4 (cargas) | cargas más cortas, no más fps | alta | bajo | |
| 12 | HLE de funciones GX una a una | 0,1-0,8 c/u | poco cada una | media-baja (`__GXData`) | medio | solo tras el 8 |
| — | `SelectThread`, `GXCopyDisp`, `IOS_IoctlAsync`, nw4r/EGG/juego | — | — | — | — | no es HLE (5.12, 5.13) |

Orden de trabajo propuesto: **1 → 2 → 5 → 3 → 4 → 6 → 7**, con un perfil nuevo de r10a en la Switch después de
1+2+5, porque con el sondeo inline los porcentajes cambian.

---

## 8. Cómo verificar que es 1:1

1. **Comparador diferencial offline** (`r8/codegen/difftest/`: `build.sh A B dir`, `difftest --seeds N`,
   `--bench`). Enlaza dos versiones del código generado y, desde el mismo estado aleatorio, compara GPR, FPR/PS1
   bit a bit, CR, XER, CTR, LR, PC, MSR, FPSCR, GQR, SRR0/1, SPRG, `inst_count` y los hashes de MEM1, MEM2, la
   caché bloqueada y los efectos del runtime (MMIO, FIFO GX, sondeos; un sondeo puede devolver "interrupción"
   en la N-ésima vuelta). Para el HLE, la versión B es el código generado **con la pasada `apply_r10_hle.py`**.
   Además de los estados aleatorios, hacen falta **entradas dirigidas**:
   - `DC*`: tamaños 0, 1, 31, 32, 33, 1 MB y `0xFFFFFFE0+` (desborde → respaldo), direcciones alineadas y no
     alineadas, MEM1/MEM2/caché bloqueada, sondeo que entrega en la última vuelta (para probar la reanudación en
     el `case`).
   - `strcmp`: cadenas iguales y distintas con el NUL en cada posición módulo 4, alineaciones relativas 0-3,
     bytes ≥ 0x80 (la resta es con signo de 32 bits sobre bytes sin signo), una cadena junto al final de MEM1.
   - `PSMTX*`: matrices con subnormales, ±0, inf, NaN (con y sin señal), valores cerca de empates de redondeo,
     `AB == A`, `AB == B` y orígenes en la caché bloqueada. Al menos 10⁶ casos por función, y el comparador marca
     `nan_sign_only` aparte (r8 ya vio que el signo del NaN cambia entre x86 y ARM: **compararlo en la Switch o
     en ARM**).
   - `memcpy`: todas las combinaciones de alineación módulo 8 × tamaños 0-300 × solapamiento hacia delante y
     hacia atrás.
   - `sqrt`: muchos patrones aleatorios de 64 bits, todos los exponentes, mantisas en los bordes de redondeo y
     subnormales; ±0, negativos, inf y NaN deben ir al respaldo.
2. **Verificación en marcha `NWII_HLE_VERIFY=n`** (generaliza `NWII_AXFX_VERIFY` de `apply_r9_axfx.py`): cada
   n-ésima llamada se guardan los registros y la RAM (o, para hojas de escritura conocida, solo las regiones que
   pueden cambiar y un hash completo cada 1000 rondas). Luego se ejecuta el reemplazo, se guarda un hash de la RAM
   y los registros, se restaura y se ejecuta el código generado **con los sondeos callados** (un indicador que haga
   que `process_pending_callbacks` devuelva `false` sin avanzar el planificador, así ningún evento escribe RAM en
   medio), y se compara al volver. El informe es `[R10] hle <func> verify ok=.. bad=..`, como en la reverb.
   Hacerlo en PC con la entrada guionizada del 1-1 (`NSMBW_*_input_test`, NAND privada `mp`) y en la Switch con
   `debug.toml`.
3. **Pruebas de CPU** (`cputest/`, 288 casos) y **humo** (PC: título → mapa → 1-1 sin `[Gov]`; citron si vuelve
   a funcionar). Con el reloj virtual (`NWII_REALTIME=0`), `inst_count` es el reloj, así que la misma entrada
   guionizada debe dar **las mismas imágenes en los mismos cuadros** con y sin `NWII_HLE=0`. Esa es la prueba
   más fuerte de "mismo resultado".
4. **Criterio para aceptar**: 0 diferencias en `difftest` (salvo `nan_sign_only` en x86), 0 `bad` en
   `NWII_HLE_VERIFY` en varias corridas largas, y cuadros idénticos con reloj virtual.

---

## 9. Archivos útiles

| Qué | Dónde |
|---|---|
| perfil crudo / simbolizado | `switch/real_logs/r9combo_1108/profile_cpu.txt`, `profile_symbolized.txt`, `nwii.log` |
| `.elf` del perfil | `switch/real_builds/r9combo_1107/NSMBWnx_r9combo.elf` |
| sondeo inline (r10a) | `r9/r10a/nWiiRuntime/include/runtime/cpu_context.h` (`process_pending_callbacks`), `src/hle/ios.cpp` (`process_pending_callbacks_slow`) |
| sondeo de r8 (el del perfil) | `build_usuario/nWiiRuntime/src/hle/ios.cpp:1593` |
| FP de Broadway | `cpu_context.h`: `ppc_force25`, `ppc_madd_s`, `ppc_single`, `ppc_single_bits(_ftz)`, `ps_*`, `dcbz` |
| tubería GX y regla BP | `r9/r10a/nWiiRuntime/src/hle/gx.cpp` (`wg_byte`, `wg_flush_now`, `wgp_write32`, `NWII_WGP_CHECK`) |
| `[hle_hooks]` y enlace por nombre | `recomp/nsmbw.toml`, `NWiiRecomp/nWiiRecomp/src/recompiler.cpp` (`hle_handlers`, `emit_function`) |
| modelo de pasada nativa + verificación | `r9/cpucode/perf/apply_r9_axfx.py`, `apply_r9_axfx_fast.py`, `apply_r9_psq.py` |
| comparador diferencial | `r8/codegen/difftest/` (y `r8/codegen/LEEME.md` §4) |
| scripts de este análisis | `/tmp/claude-1000/-home-djvemo-nsmbw-port/1b06eac6-a23b-489b-924a-77167831057b/scratchpad/` (`dis.py`: desensambla el DOL USA; `gprof.py`: % por función desde los PC invitados; `callers.py`: quién llama según los PC de retorno) |
