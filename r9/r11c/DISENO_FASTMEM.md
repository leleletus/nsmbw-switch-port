# R11c — Fastmem para NSMBW (PC y Switch): medidas, prototipo y diseño

8-nov-2026. Frente C de R11: **base para G7** (docs/switch/README.md). No se mandó nada a la consola, no se tocó
`r9/r10c`, `r9/r11a`, `r9/r11b`, `recomp/`, `NWiiRecomp/`, `switch/` ni la NAND del usuario. Sin commits.
Todo lo nuevo está en `r9/r11c/`.

## 0. Resumen

- **Perfil (PC, 4 corridas, 861 s: título/demos, menús→mapa→1-1, mapa andando, Coin Battle 4J):** el juego hace
  **34,5 M accesos/s a RAM** (26–46 M/s según escena, ~0,6 M por frame) y **1,64 M/s a algo que no es RAM**,
  en **1 397 sitios** (de 675 069). El 99,4 % es la tubería GX (1,63 M/s, 1 101 sitios); luego DSP 9 300/s,
  VI 780/s, PE 250/s, IPC/Hollywood ~100/s. La caché bloqueada (259 000/s, 126 sitios: `PSMTX*` y matrices de
  g3d) y la página 0 (34 000/s, globales del SO: 27 000 lecturas y 7 600 escrituras/s) **sí se mapean**. Ningún
  sitio mezcla RAM y MMIO. El caso especial 0xC no se tocó nunca (solo `lbz` de 0x0–0x7).
- **Reconocimiento estático:** una propagación de constantes de `lis/addi/ori/...` por función reconoce
  **3 019 sitios MMIO** en todo el programa y cubre el **90 % de los sitios y el 99,97 % de los accesos** MMIO
  medidos. Sin la lista del perfil solo fallarían **~490/s** (3 sitios: VI 239/s en `func_801BBBD0`, PE 2×126/s en
  `GXFinishInterruptHandler`, punteros guardados en globales). Lista final (estática + perfil):
  `prof_mmio_list.tsv`, 3 100 entradas (pc, tipo), 140 solo del perfil.
- **Coste de un fallo en este PC:** **≈1,2 µs** con la máquina libre (1,5–2,9 µs con compilaciones al lado),
  frente a 2–5 ns de la llamada explícita. En la Switch, sin medir (estimo 2–5 µs).
- **Código AArch64 (devkitA64, 4 unidades reales):** con fastmem el `.text` baja un **44 %** (4,95 → 2,76 MB) y las
  funciones calientes quedan en menos de la mitad de instrucciones (`func_801A5C10` 1 872 → 762) con la mitad de
  recargas de `ctx`. Es la condición que faltaba para G5 (registros del juego en registros ARM).
- **Decisiones:** reserva de 4 GiB con vistas (MEM1×4, MEM2×4, LC×1), acceso `base + ea` sin máscara; cada acceso
  es un `asm volatile` de forma fija con entrada en una tabla de excepciones (con `memcpy` en C GCC **borra
  accesos**: 266 guardados menos en una unidad, inaceptable para la tubería); sitios de la lista → llamada directa
  al camino lento; resto → fallo → camino lento. En Switch, entrada de excepción propia con
  `svcReturnFromException(0)` (sin registro libre necesario y seguro con varios hilos).
- **Ganancia estimada:** −4 instrucciones por acceso (≈2,3–3 M instrucciones por frame, ~5–7 % del hilo del juego
  de forma directa) más la mitad de recargas de `ctx` y −44 % de código: **−10 a −25 % del hilo del juego** según
  cuánto del tiempo del código generado sea por acceso (medir); y sobre todo deja vía libre a G5.

---

## 1. Qué accesos del juego tocan algo que no es RAM (perfil en PC)

### 1.1 Cómo se midió

- `tools/apply_site_ids.py` (pasada nueva, **solo para perfilar**): sobre una copia del código generado de r10c
  (`export_prof/`, sha256 de cada `output_*.cpp` comprobado contra `r9/r10c/export`, marca `// [r11c-siteid]`,
  se niega a correr dos veces) da a cada acceso un número de sitio y le asocia la dirección de la instrucción
  PowerPC (el comentario `// 0xADDR: OPCODE` anterior). `mv_.read32(ea)` → `mv_.read32_s(ID, ea)`,
  `psq_l_fast(ctx, mv_, …)` → `psq_l_fast_s(ID, ctx, mv_, …)`. Resultado: **675 069 sitios** en 87 unidades
  (167 sin comentario `// 0x`: el cuerpo nativo de la reverb, con pc sintético `0xAF00xxxx`). Tabla: `sites.tsv`.
  Comprobado: el código de operación de cada sitio cuadra con su tipo de acceso (lwz→read32, stfs→write_f32,
  lmw/stmw → varios sitios con el mismo pc, etc.).
- `nWiiRuntime/include/runtime/cpu_context.h` (copia de r11c): las variantes `*_s` cuentan y luego llaman al
  acceso **de siempre** (mismos bytes, mismo orden). Si la dirección es RAM que fastmem mapearía (MEM1 desde
  0x1000, MEM2) suben `g_site_fast[id]`; si no, `site_slow(id, ea)` lo apunta por (sitio, región).
  La página 0 (0x0–0xFFF) se cuenta aparte a propósito (ver §4.3).
- `nWiiRuntime/src/core/site_prof.cpp`: con `NWII_SITEPROF_DIR` escribe cada 10 s una instantánea acumulada
  (`slow_NNNN.txt` con escena actual, `fast_NNNN.bin`).
- Binario: `bin/NSMBW_siteprof_input_test` (compilación completa de PC, `FULL=1 JOBS=2`).
- Corridas: `tools/run_prof.sh` (offscreen, sin sonido, NAND de plantilla privada `pc/nand_tpl_*`):
  `idle` (título y demos, 240 s), `lvl11` (menús → mapa → 1-1, 300 s), `walk` (mapa andando, 150 s),
  `cb4` (Coin Battle 4 jugadores, guion de R6, 200 s).
- Informe: `tools/siteprof_report.py` → `prof_sites.tsv` (todos los sitios que tocaron algo que no es RAM, con
  región, accesos, accesos/s, accesos a RAM del mismo sitio y la clasificación estática).

### 1.2 Resultados

Totales de las snapshots finales (`prof_report.txt`; tasas por segundo de corrida, la máquina a 59,5–59,8 fps
de media con el perfilador puesto):

| Corrida | s | Accesos a RAM | No RAM | Sitios ejecutados | Sitios no RAM |
|---|---|---|---|---|---|
| idle (título, película, demos) | 230 | 37,9 M/s | 2,33 M/s | 206 142 | 1 591 |
| lvl11 (menús → mapa → 1-1) | 290 | 33,2 M/s | 1,53 M/s | 199 098 | 1 385 |
| walk (mapa del mundo 1) | 150 | 36,8 M/s | 2,29 M/s | 190 315 | 1 334 |
| cb4 (Coin Battle, 4 jugadores) | 190 | 39,3 M/s | 1,78 M/s | 195 404 | 1 402 |

Por escena (intervalos de 10 s): STAGE 28–46 M/s de RAM y 0,5–3,4 M/s a la tubería; WORLD_MAP 29–35 M/s y
1,5–2,0 M/s; película 26 M/s y 1,2 M/s (+ 0,63 M/s a la LC). DSP (9 300/s) y VI (780/s) son constantes: sondeos.

Por región, las cuatro corridas juntas:

| Región | Sitios | Accesos/s | Reconocidos al recompilar | Con fastmem |
|---|---|---|---|---|
| WGPIPE 0x0C008000 | 1 101 | 1 627 536 | 1 033 (los 68 restantes, en `func_802C2700`, <1/s) | llamada explícita |
| LC 0xE0000000 | 126 | 258 581 | 0 (puntero) | **mapeada: acceso directo** |
| página 0 (0x10–0xFFF) | 88 | 34 448 (26 859 lect., 7 589 escr.) | 71 | **mapeada RW** |
| DSP | 58 | 9 312 | 58 | llamada explícita |
| VI | 36 | 778 | 35 | llamada explícita |
| PE | 16 | 252 | 0 (puntero `__peReg`) | lista del perfil |
| HW 0x0D00 (IPC) | 70 | 67 | 55 | llamada explícita |
| HW 0x0D8 | 70 | 34 | 70 | llamada explícita |
| CP | 33 | 1 | 0 | lista del perfil |
| PI / MI | 13 | 1 | 6 | llamada explícita / fallo |
| 0x0–0xF | 3 | 24 accesos en total | 2 | mapeada (solo `lbz` 0x0–0x7) |

Tipos en la tubería: 378 sitios `stfs`, 373 `stb`, 259 `stw`, 69 `sth`, 22 `psq_st`. El más caliente llega a
~20 000/s (`GXSetTevIndirect`, luego `func_801C5570`, `func_801C9A80`, `GXLoadTexObjPreLoaded`); en total 24 sitios
≥ 10 000/s y 400 ≥ 1 000/s: el tráfico está muy repartido, por eso hace falta la lista completa y no solo los
calientes.

**Ningún sitio mezcla RAM y MMIO** (63 sitios mezclan RAM y LC: `memcpy`/`PSMTXCopy`; con fastmem la LC está
mapeada, así que da igual). Así que basta con dos clases: sitio MMIO (llamada directa, sin comprobación) y el
resto (acceso directo).

### 1.3 ¿Se reconocen al recompilar?

`tools/static_mmio.py` reconstruye cada función desde los comentarios `// 0xADDR: OPCODE`, construye el grafo de
control (saltos, llamadas, `blr`/`bctr`) y propaga constantes de los GPR: `lis/addis/addi/li/ori/oris/xori/andi./
mr/rlwinm` con entradas conocidas; cualquier otra escritura de un GPR → desconocido; una llamada borra r0 y r3–r12;
las entradas de tablas de saltos y las que no se alcanzan empiezan sin nada conocido; en las uniones se queda solo
lo que coincide. Si la dirección efectiva de un acceso sale constante, se clasifica por región. Tarda 16 s en
Python para las 1,72 M instrucciones.

Sobre **todo el programa** (675 069 sitios):

| Clase estática | Sitios |
|---|---|
| desconocida (puntero en registro) | 583 026 |
| RAM (dirección constante) | 88 804 |
| **WGPIPE** (`lis rX,0xCC01` + `-0x8000`) | **2 615** (las que más: `func_802A47C0` y `func_802A9640` con 120 cada una; `func_8000D000`, el dibujo inmediato de la propia ROM, 66) |
| HW 0x0D00xxxx (IPC…) | 108 |
| DSP 0x0C005xxx | 92 |
| HW 0x0D8xxxxx (Hollywood; el runtime devuelve 0) | 92 |
| VI 0x0C002xxx | 88 |
| PI 0x0C003xxx | 12 |
| MI 0x0C004xxx | 12 |
| página 0 (0x10–0xFFF, globales del SO) | 215 |
| 0x0–0xF | 5 |
| **total MMIO estático** | **3 019** |

Detalle que importó: el análisis debe empezar en la **dirección de la función**, no en su instrucción más baja
(hay funciones con trozos por debajo de su entrada, p. ej. `func_8000D000` tiene código en 0x8000BFxx). La primera
versión empezaba por la más baja y perdía 300 sitios de la tubería; con la entrada correcta pasa de 2 312 a 2 615.
Los puntos de reanudación (`switch (ctx.pc)` tras una llamada o un sondeo) no son entradas nuevas: continúan la
misma ejecución con los mismos registros del juego.

Cruce con el perfil: de los 1 397 sitios que tocaron MMIO, **1 257 (90 %) son estáticos y suman el 99,97 % de
los accesos**. Sin la lista del perfil fallarían solo los que guardan el puntero en una variable global:

| Sitio | Función | Región | /s |
|---|---|---|---|
| 0x801BBE44 `sth` | `func_801BBBD0` (VI, tabla de registros) | VI | 239 |
| 0x801C521C `lhz` / 0x801C5224 `sth` | `GXFinishInterruptHandler` (`__peReg`) | PE | 126 + 126 |
| 0x80224348 `lwz` | `func_80224340` | 0x0D000004/8 | 1 |

≈490 fallos/s → ~1,5 ms/s en la Switch si un fallo cuesta 3 µs (0,15 %): se podrían dejar caer, pero con la lista
del perfil (`prof_mmio_list.tsv`, 3 100 entradas por (pc, tipo): 3 019 estáticas + 140 del perfil, que también
incluyen los 68 de `func_802C2700`) no falla nada de lo medido. Un sitio de la lista que en otro momento toque RAM
seguiría siendo correcto (el camino lento sabe hacer RAM).

Una clasificación equivocada **nunca rompe nada**: un sitio MMIO que no está en la lista cae en el manejador de
fallos (correcto pero lento); un sitio de RAM que sí está en la lista conserva la comprobación de hoy (correcto,
algo más lento). La lista solo decide la velocidad.

---

## 2. Prototipo en PC: reserva, vistas y manejador de fallos

`proto/fastmem_proto.cpp` (prueba aislada, no el juego; `g++ -O2 -std=gnu++20`):

- Reserva de **4 GiB + 64 KiB** `PROT_NONE` (`MAP_NORESERVE`). MEM1, MEM2 y la caché bloqueada son `memfd`
  mapeados con `MAP_FIXED|MAP_SHARED` en sus direcciones efectivas: MEM1 en 0x00/0x40/0x80/0xC0000000, MEM2 en
  0x10/0x50/0x90/0xD0000000, LC solo en 0xE0000000 (como `get_ptr`, que compara la dirección efectiva, no la
  física). Así el acceso es `base + (uint32_t)ea`, **sin máscara**: los espejos los dan los mapeos. La misma
  memoria se mapea otra vez para el runtime (punteros canónicos, con una página de relleno tras MEM1).
- Accesos sin comprobación: carga + `bswap`.
- `SIGSEGV` (SA_SIGINFO): si la dirección cae en la reserva, decodifica la instrucción x86-64 que falló (`mov`
  8A/8B/88/89, `movzx` 0F B6/B7, `mov` inmediato C6/C7, prefijo 66, REX, ModRM/SIB/desplazamiento), calcula la
  dirección desde los registros del `ucontext`, hace el acceso por el camino lento (aquí una imitación de
  `MMU::*_slow`: registros 0x0C00/0x0D00, tubería GX como flujo de bytes, caso 0xC, LC, sin mapear → 0),
  escribe el registro destino (con la extensión de cero/ancho correcto de x86) y salta la instrucción.
- Pruebas: espejos, MEM2, LC, MMIO de 8/16/32 bits, la tubería GX con 1/2/4 bytes y con inmediatos (orden de bytes
  correcto), sin mapear, el cruce del final de MEM1 (lee el relleno, como hoy). **Todas pasan.**

**Coste medido de un fallo en este PC** (Core Ultra 5 125H, Linux 7.3): **≈1,2 µs** (1 187–1 260 ns, 10⁶ fallos,
3 repeticiones), frente a **2–3 ns** de la llamada explícita al camino lento: ×500. Un acceso a RAM por fastmem y
por la comprobación actual cuestan lo mismo en x86 (0,6–0,8 ns: la comprobación es barata aquí; lo que cuesta en
el A57 es el código extra y los registros, §5).

Tres cosas que salieron del prototipo y deciden el diseño:

1. **En x86, GCC mete la carga del juego dentro de otras instrucciones** (`cmp dword [rbx+rax], imm32` al comparar
   un `read32` con una constante). Un decodificador pequeño de cargas/guardados no puede con eso. En AArch64 no
   pasa (solo `ldr`/`str` tocan memoria), pero en PC obliga a hacer cada acceso con `asm`.
2. **Con `memcpy` en C, GCC elimina accesos del juego.** En `output_24` compilado para Switch con fastmem en C
   desaparecen 268 cargas y 266 guardados frente a la versión con `asm volatile` (p. ej. `func_801A5C10` tiene
   71 sitios: 71 accesos con `asm volatile`, 63 en C). Para RAM es legal, pero si la dirección resulta ser la
   tubería GX, dos `stw` seguidos al mismo `ea` son dos datos del FIFO y GCC borraría el primero (eliminación de
   guardados muertos). Hoy no pasa porque la llamada al camino lento separa los accesos. **Cada acceso del juego
   tiene que ser un `asm volatile`**: se ejecuta exactamente una vez y en el mismo orden que los demás.
3. **El manejador cambia memoria a espaldas del compilador.** En la prueba, `g_pipe.size()` se evaluó antes de
   los guardados (el `asm` solo declara el byte del juego que toca). En el runtime pasa lo mismo con cualquier
   estado del anfitrión que el código generado tenga en registros: **el camino lento por fallo no puede escribir
   en `ctx` ni en variables que lea el código generado** (§6).

---

## 3. Qué formas de carga/guardado salen en AArch64

Con `r9/r10c/nx/build/nsmbw.elf` (r10c actual) el acceso típico es
`and w0, wA, #0x3fffffff; sub; cmp wLim; b.hs lento; ldr/str wX, [x5, w0, uxtw]; rev` y el camino MEM2 y la
llamada al camino lento aparte (en `func_800B0DB0`: `str w13, [x5, w0, uxtw]` tras `cmp w11, w4; b.cs`).

Para ver qué haría GCC con fastmem compilé unidades reales de r10c con devkitA64 y los mismos parámetros del build
(`fm/tools/build_tu.sh`, cabecera experimental `fm/nWiiRuntime/include/runtime/cpu_context.h`):
`base` = MemView de r10c; `fmc` = fastmem en C (`memcpy` a `g_fm_base + ea`); `fmv` = fastmem con
`asm volatile` + tabla de excepciones (la propuesta).

| Unidad | `.text` base | `.text` fastmem (`fmv`) | Cambio | Tabla `nwii_fm_tab` |
|---|---|---|---|---|
| output_10 | 1 409 520 | 727 836 | **−48 %** | 73 KB (9 166 accesos) |
| output_22 | 1 063 228 | 721 696 | −32 % | 51 KB |
| output_24 | 1 160 364 | 616 372 | −47 % | 61 KB |
| output_50 | 1 316 768 | 689 748 | −48 % | 67 KB |
| **suma** | 4 949 880 | 2 755 652 | **−44 %** | 8 B por acceso (≈5,4 MB en total) |

Funciones calientes de output_24 (instrucciones totales / cargas que no son del juego, es decir `ctx` y pila /
guardados en pila):

| Función | base | fastmem C | fastmem `asm volatile` |
|---|---|---|---|
| `func_801A5C10` (delay AXFX, 1 % del hilo en Switch) | 1 872 / 190 / 7 | 736 / 91 / 11 | **762 / 90 / 17** |
| `func_801A3880` (reverb, generada) | 5 284 / 404 / 8 | 2 603 / 263 / 10 | **2 630 / 249 / 10** |
| `func_801A2B10` | 2 121 / 234 / 22 | 1 096 / 115 / 8 | **1 093 / 111 / 8** |

Las funciones quedan en **menos de la mitad de instrucciones** y las recargas de `ctx` también bajan a la mitad
(la llamada al camino lento era la que obligaba a recargar: lo mismo que vio R8 con los registros locales).
`asm volatile` cuesta un 1 % más que C y es lo único seguro (§2, punto 2).

Formas que aparecen en los accesos del juego con `asm volatile` (output_24, 7 595 entradas):
`ldr w`, `ldr x`, `ldrh w`, `ldrb w`, `str w`, `str x`, `strh w`, `strb w`, **siempre `[Xbase, Wea, UXTW]`**.
Con C aparecen además `ldrsb` (GCC junta `lbz` + `extsb`). Nada de `ldp/stp`, ni índices con desplazamiento, ni
registros FP: el valor pasa por un GPR para el `rev`.

`proto/a64_decode.h` decodifica todas las cargas/guardados de un registro (entero y FP; desplazamiento con registro
UXTW/SXTW/LSL, inmediato, sin escalar, pre/post-índice, con signo) y rechaza pares y exclusivas.
`proto/a64_decode_check.py` lo comprueba contra el texto de `objdump` en **202 791 instrucciones reales** de los
tres objetos de output_24: **0 errores**.

---

## 4. Diseño

### 4.1 Memoria

Una sola reserva de **4 GiB + 64 KiB** por proceso; dirección del juego `ea` ↔ `base + ea`.

| Rango efectivo | Qué hay | Cómo |
|---|---|---|
| 0x00000000, 0x40000000, 0x80000000, 0xC0000000 (+24 MiB) | MEM1 | 4 vistas del mismo bloque |
| 0x10000000, 0x50000000, 0x90000000, 0xD0000000 (+64 MiB) | MEM2 | 4 vistas |
| 0xE0000000 (+256 KiB) | caché bloqueada | 1 vista (0x20000000/0x60000000/0xA0000000 siguen sin mapear, como hoy) |
| todo lo demás (MMIO 0x0C00/0x0D00/0x0D8, tubería GX 0x0C008, 0x0180_0000…, 0x1400_0000…) | nada | fallo → camino lento de siempre |

- **PC (Linux):** `mmap(PROT_NONE, MAP_NORESERVE)` + un `memfd` por región + `mmap(MAP_FIXED|MAP_SHARED)` por
  vista (probado en el prototipo). Windows/macOS: `VirtualAlloc2`/`MapViewOfFile3` y `mach_vm_remap` (no probado).
- **Switch:** receta de melonDS (SISTEMA_HORIZON_LIBNX.md §2.2), escrita en `proto/fastmem_nx.cpp` y compilada con
  devkitA64 (no ejecutada): un bloque del heap `aligned_alloc(2 MiB)` con MEM1 | relleno | MEM2 | LC →
  `svcMapProcessCodeMemory` una vez + `svcSetProcessMemoryPermission(RW)` (el puntero resultante es el canónico
  del runtime; el original deja de valer) → `virtmemFindAslr(4 GiB + 2 MiB)` alineado a 2 MiB +
  `virtmemAddReservation` → 9 `svcMapProcessMemory` (4 MEM1, 4 MEM2, 1 LC). Las vistas quedan alineadas a 2 MiB
  (bloques grandes en la TLB).

### 4.2 El runtime sigue usando los mismos búferes

`MMU::mem1/mem2/lc` dejan de ser `std::vector` y pasan a ser una vista (puntero + tamaño, con `data()`, `size()`
y `operator[]`) sobre el bloque canónico; `size()` sigue contando los 8 bytes de relleno, así que `get_ptr`,
`fast_ptr` y todos sus usuarios (DMA del DSP `hw_dsp.cpp`, AX `ax_hle*.cpp`/`ax_async.cpp`, GX
`fifo_parser.cpp`/`gx.cpp`/`texture_cache_gl.cpp`, IPC/DI, los HLE, `NWII_AXFX_VERIFY`) no cambian. Las vistas y el
canónico son las mismas páginas físicas: lo que escribe un DMA lo lee el juego y al revés, sin copias (en el A57
la caché de datos no tiene alias entre direcciones virtuales). En Switch, **no leer de la SD directamente a la
memoria canónica** (estado *CodeMemory*: libnx parte las lecturas en 4 KB, SISTEMA §6): leer al heap y copiar,
como ya hace el DVD virtual con su búfer.

### 4.3 Código generado

- `MemView` guarda `b = g_fm_base` (una carga por función) y cada acceso es **un `asm volatile`** con la forma fija
  `ldr/ldrh/ldrb/str/strh/strb Wt|Xt, [Xb, Wea, UXTW]` y una entrada en la sección `nwii_fm_tab`:
  `{int32 pc − &entrada, uint32 tipo | sitio << 8}` (como la tabla de excepciones del núcleo de Linux). El `rev`
  queda fuera del `asm` (GCC lo programa). Operandos `"m"` exactos para que GCC sepa qué byte toca y pueda mover
  lo demás (`ctx`) alrededor. En PC lo mismo con `movl/movzwl/movzbl/movq`.
- **Sitios MMIO conocidos** (lista estática de §1.3 + la del perfil §1.2, en un archivo que lee la pasada):
  `mv_.read32(ea)` → `mv_.read32_io(ea)` = llamada directa al `*_slow` de hoy, sin comprobación ni fallo.
- `psq_l/psq_st` con GQR float: dos accesos `asm` (también MEM2 sin salir del camino rápido); otros tipos de GQR
  siguen llamando a `psq_load/psq_store`.
- Página 0 de MEM1 (0x0–0xFFF): **mapeada RW como el resto**. El perfil descarta las alternativas: tiene 27 000
  lecturas y 7 600 escrituras/s (`OSSetCurrentContext` escribe 0x800000C0/D4 en cada cambio de contexto, el
  planificador lee 0x800000E4), así que protegerla costaría miles de fallos por segundo. La única diferencia con
  hoy es el caso especial de `read32`/`write32` en la dirección física **exacta** 0xC (hoy lee 0 y descarta la
  escritura): en las 4 corridas no se tocó nunca (solo `lbz` de 0x0–0x7, 24 veces). Para no perderlo de vista:
  comprobar en cada VI que los bytes 0xC–0xF siguen a 0 y avisar en el registro si no (con eso la lectura de 0xC
  da 0 igual que hoy mientras nadie escriba ahí).
- `NWII_WATCH`: proteger la página vigilada contra escritura (falla → `watch_hit` → escribe), en vez del límite
  de escritura a 0 de hoy.
- El cruce del final de MEM1 (0x017FFFF9–FF de más de un byte) falla en la página siguiente y el manejador hace
  el acceso como el camino rápido de hoy (lee/escribe el relleno del búfer canónico): igual que ahora.

### 4.4 El manejador

Común: busca el `pc` en la tabla ordenada (al arrancar se ordena; ~675 k entradas, búsqueda binaria); si no está →
es un fallo de verdad (se deja morir como siempre). Si está: tipo de acceso de la tabla, registros de la instrucción
(`Rt`, `Rn`, `Rm` de los bits; la forma es fija), `ea = Xn + uxtw(Wm) − base`, acceso por el `*_slow` del MMU que
corresponda al **tipo del sitio** (así `write_f32` y `write32` siguen siendo distintos, como hoy), registro
destino, `pc += 4`. Cuenta fallos por sitio y los vuelca al salir (`[FASTMEM] site … faults …`) para alimentar la
lista.

- **PC:** `SIGSEGV` con `SA_SIGINFO`, como el prototipo (cuidado: el manejador corre en el hilo que falló;
  solo el hilo de la CPU del juego ejecuta código generado).
- **Switch:** entrada de excepción propia (`__libnx_exception_entry` es débil en libnx), escrita en
  `proto/fastmem_nx.cpp`: guarda x9–x29 y q0–q31, cambia a una pila propia de 256 KiB, llama al manejador en C con
  el `ThreadExceptionFrameA64` (x0–x8, lr, sp, elr, esr, far), restaura y hace `svcReturnFromException(0)`: el
  núcleo restaura x0–x8, lr, sp y pc desde el marco editado, sin necesitar un registro libre para saltar (el
  problema del `ARM_RestoreContext` de melonDS con código de GCC). El núcleo mantiene el candado de excepciones
  del proceso hasta ese `svcReturnFromException`, así que **un área global basta aunque fallen dos hilos**. Lo que
  no es nuestro → `svcReturnFromException(1)` → informe de cuelgue de Atmosphère como ahora. Solo 2 cruces de
  núcleo (el fallo y la vuelta), sin el `svcGetInfo` de la entrada de libnx. Requisitos: Atmosphère con
  `disable_user_exception_handlers = 0` (por defecto) y no tener un depurador conectado (con gdbstub el núcleo
  manda el fallo al depurador).

---

## 5. Ganancia esperada

Lo que se quita en cada acceso a RAM en el A57 (r10c, §3): `and #0x3fffffff`, `sub #0x10`, `cmp lim`, `b.hs` →
**4 instrucciones** de las 6 del camino rápido (queda `ldr/str [b, w, uxtw]` + `rev`). MEM2 se ahorra más (9 → 2).

- **Directo:** 34,5 M accesos/s a RAM a 60 fps ≈ **0,58 M por frame** (0,76 M en las escenas más cargadas). Por
  4 instrucciones: 2,3–3,0 M instrucciones por frame. Con IPC ≈ 1 en este código, 2,3–3 M ciclos de los ~45 M que
  gasta el hilo del juego por frame (r9combo a 1785 MHz): **−5 a −7 %**.
- **Indirecto (lo grande):** sin llamadas en cada acceso GCC deja de recargar `ctx` (recargas que no son del juego
  −50 % en las tres funciones medidas) y el código baja un 44 % (menos presión en la caché de instrucciones de
  48 KB y en la decodificación, punto 5 del README). En la Switch el código generado es ~45 % del hilo del juego
  (`func_*` en el perfil de r10a_fix, ~63 % del tiempo ocupado). Si su tiempo baja entre un 20 % y un 40 % (la mitad
  de instrucciones estáticas; dinámicamente menos, porque los caminos lentos ya no se ejecutaban), el hilo del
  juego baja **un 10–25 %**. Es una estimación: hay que medirla con el A/B en la Switch.
- **Habilita G5:** R8 vio que los registros en locales no ganaban nada por los derrames alrededor de la llamada al
  camino lento (`func_801A5C10` 184 → 663 guardados). Con fastmem esa función tiene 5 llamadas en vez de 75
  (todas a funciones del juego o al sondeo), así que G5 puede mantener los registros del juego en registros ARM.
- **Lo que no cambia:** las 1,6 M escrituras/s a la tubería GX siguen llamando al runtime (`wgp_write32`,
  `wg_flush_now`: ~2,5 % del hilo en el perfil de Switch); eso es trabajo aparte del runtime (lote de escrituras).
- **Coste de los fallos residuales:** con la lista, 0 en lo medido; sin la lista del perfil, ~490/s (≈0,15 %).
- **Memoria:** +5,4 MB de tabla de excepciones (8 B por acceso) en el NRO, −50 MB de `.text` aprox. (−44 % de
  ~116 MB).

---

## 6. Riesgos

1. **Efectos del camino lento que el código generado no ve.** Con la llamada de hoy, GCC asume que todo pudo
   cambiar; con un fallo, solo el byte del juego. El camino lento por fallo **no puede escribir `ctx`** (ni
   `poll_kick_global()` desde ahí: debe dejar una bandera atómica que mire el siguiente sondeo, como hacen ya los
   otros hilos) **ni variables que lea el código generado** (`g_watch_addr`, `g_trace_calls`, tablas de
   despacho…). Los DMA que escriban RAM del juego sí son seguros, porque cada acceso es `asm volatile` y se vuelve
   a leer. Hay que auditar los `HW_Reg_Write*` (PI, VI, DSP, DI, EXI, AI, IPC) con esto en mente; los sitios con
   llamada explícita no tienen el problema.
2. **Coste de un fallo en la Switch: sin medir.** En PC 1,2 µs; en la Switch, dos cruces de núcleo más el candado
   del planificador: estimo 2–5 µs (una `svc` simple cuesta ~340 ciclos según BENCHMARK_SWITCH_REAL.md). Hay que
   medirlo con `tools/nxbench` antes de decidir el umbral de la lista (§1.2).
3. **Sitios que el perfil no alcanzó** (otros mundos, menús, guardado, errores de disco): caen en el manejador. Son
   correctos; si alguno es frecuente se verá en el volcado `[FASTMEM]` y entra en la lista. El código del SDK que
   escribe registros (DI, EXI, SI, AI) es casi todo de arranque o de eventos.
4. **Atmosphère/depurador**: con gdb conectado los fallos van al depurador; para depurar hace falta una opción
   `NWII_FASTMEM=0` (MemView de hoy) — dos sabores del código generado o un `if` por función.
5. **Memoria y estado *CodeMemory* en Switch** (lecturas de SD a la vista; no servir de búfer de IPC/GPU): leer al
   heap y copiar. 4 GiB de espacio virtual sobran en 39 bits (y en 36 bits del forwarder).
6. **Tabla de excepciones**: con `--gc-sections` la sección `nwii_fm_tab` se queda solo si algo usa
   `__start_nwii_fm_tab` (nombre de identificador C) o con `KEEP` en el guion del enlazador; comprobar en el ELF que
   el número de entradas = número de accesos.

---

## 7. Plan de verificación

1. **Prueba unitaria** del manejador en PC (ya existe el esqueleto: `proto/fastmem_proto.cpp`) y del decodificador
   AArch64 contra objdump (`proto/a64_decode_check.py`, 0 errores en 202 791 instrucciones) — repetirla sobre el
   `.elf` completo del NRO fastmem: **cada entrada de la tabla debe apuntar a una instrucción de la forma fija**.
2. **Diferencial por función (PC)**: compilar cada unidad dos veces en el mismo binario de prueba, con MemView de hoy
   y con fastmem, renombrando las funciones del segundo sabor (`-include` de un `#define func_X fm_func_X` generado
   desde `functions.h`). Al entrar en una función muestreada (`NWII_FM_VERIFY=n`, como `NWII_AXFX_VERIFY`): copiar
   RAM (MEM1+MEM2+LC) y `ctx`, ejecutar la versión de hoy **grabando la secuencia de accesos lentos** (dirección,
   tipo, valor leído/escrito), restaurar, ejecutar la fastmem **sirviendo los accesos lentos desde esa grabación**
   (así el MMIO no avanza dos veces) y comparar: hash de toda la RAM, todos los registros (GPR, FPR/PS1 bit a bit,
   CR, XER, CTR, LR, PC, `inst_count`) y la secuencia de accesos lentos (misma dirección, tipo, valor, **mismo
   orden**). Una función que llama a otras se compara entera (las dos ramas llaman a su propio sabor).
3. **Repetición determinista** (idea para la integración completa): grabar en una corrida con el binario de hoy la
   secuencia de accesos lentos (con `inst_count` de cada uno) y la entrada del mando; reproducir con el binario
   fastmem sirviendo las lecturas lentas desde la grabación y comprobar que cada escritura lenta coincide y que el
   hash de la RAM coincide en cada VI. El primer `inst_count` distinto señala la función culpable. Requiere el
   modo de reloj determinista (sin `NWII_REALTIME`) y un solo hilo de la CPU del juego.
4. **Humo**: `tools/run_prof.sh` con el binario fastmem (las cuatro escenas), 0 fallos de la lista caliente,
   capturas iguales a r10c en las mismas tramas (`r9/render/test/hashes.py`).
5. **Switch**: primero `nxbench` (coste de un fallo, `svcMapProcessMemory` con 9 vistas), luego A/B sin OC.

---

## 8. Siguiente paso de implementación

1. **`nxbench`: coste de un fallo en la Switch** (entrada propia de `proto/fastmem_nx.cpp` con un acceso a una
   página sin mapear en bucle) y de los 9 `svcMapProcessMemory`. Es lo único que falta para fijar el umbral de la
   lista; si sale >10 µs, todo sitio del perfil con >10/s va a la lista (ya lo está).
2. **Pasada `apply_r11_fastmem.py`** sobre el código generado (como las de r9): lee `prof_mmio_list.tsv` y cambia
   los sitios de la lista a `mv_.read32_io(ea)` / `write32_io` (llamada directa al `*_slow`); el resto queda como
   `mv_.read32(ea)`, que ahora es el `asm volatile` con tabla. sha256 por archivo y marca, como siempre.
3. **Runtime:** `MMU::mem1/mem2/lc` como vista sobre el bloque canónico; `fastmem_init()` (PC: `memfd`; Switch:
   `fastmem_nx.cpp`) antes de cargar el DOL; manejador `SIGSEGV`/entrada propia con la tabla; contador de fallos por
   sitio con volcado al salir; `NWII_FASTMEM=0` para depurar (MemView de hoy, segundo sabor del código generado).
4. **Verificación en PC** (§7: diferencial por función con la secuencia de accesos lentos grabada, luego humo con
   las 4 corridas de `tools/run_prof.sh` y capturas iguales), y después el A/B en la Switch sin OC.
5. Con eso hecho, retomar G5 (registros del juego en registros ARM) sobre el código ya sin llamadas.

---

## 9. Archivos

| Qué | Dónde |
|---|---|
| pasada de números de sitio (solo perfil) | `tools/apply_site_ids.py` → `sites.tsv` |
| clasificación estática (propagación de constantes) | `tools/static_mmio.py` → `static_sites.tsv` |
| contadores del perfil | `nWiiRuntime/src/core/site_prof.cpp`, variantes `*_s` en `nWiiRuntime/include/runtime/cpu_context.h` |
| código generado con sitios | `export_prof/` (= r10c + `apply_site_ids.py`) |
| binario de perfil | `bin/NSMBW_siteprof`, `bin/NSMBW_siteprof_input_test` |
| corridas | `tools/run_prof.sh`, `pc/runs/{idle,lvl11,walk,cb4}/` |
| informe | `tools/siteprof_report.py` → `prof_sites.tsv`, `prof_report.txt` |
| prototipo PC (reserva, vistas, SIGSEGV, coste) | `proto/fastmem_proto.cpp` |
| decodificador AArch64 + comprobación con objdump | `proto/a64_decode.h`, `proto/a64_decode_test.cpp`, `proto/a64_decode_check.py` |
| esquema Switch (mapeos + entrada de excepción propia), compila con devkitA64 | `proto/fastmem_nx.cpp` |
| experimento de código AArch64 (MemView fastmem en C / `asm` / `asm volatile`) | `fm/nWiiRuntime/include/runtime/cpu_context.h`, `fm/tools/*.sh`, `fm/obj/` |
