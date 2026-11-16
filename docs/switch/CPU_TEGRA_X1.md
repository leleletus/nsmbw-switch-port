# CPU de la Nintendo Switch (Tegra X1, Cortex-A57): referencia para el recompilador

Fecha: 8-nov-2026. Ámbito: el hilo del juego recompilado (PowerPC Gekko/Broadway → C++ → devkitA64 GCC 16.1.0,
`-march=armv8-a+crc+crypto -mtune=cortex-a57 -O2 -ffp-contract=off`, `.nro` homebrew).

Objetivo de este documento: saber **qué instrucción nativa es la más barata** para cada cosa sin perder la
igualdad bit a bit con la Wii (Dolphin como oráculo). Cada número lleva su fuente entre corchetes; la lista
completa de fuentes (URL + sección) está al final (§10). Lo que **no** se pudo confirmar se marca con
**[SIN CONFIRMAR]**.

Convenciones:
- "lat" = latencia en ciclos (hasta que un consumidor dependiente puede usar el resultado).
- "rend" = rendimiento máximo en instrucciones/ciclo de todo el núcleo (1/3 = una cada 3 ciclos).
- Pipes del A57: B (saltos), I0/I1 (ALU entera simple), M (multiciclo: shift+ALU, mul, div, CRC), L (carga),
  S (almacenamiento), F0/F1 (FP/NEON) [SOG §2.1].
- [BENCH] = medido en la Switch real del usuario con `tools/nxbench` el 8-nov-2026
  (`docs/switch/BENCHMARK_SWITCH_REAL.md`). Los ciclos no dependen del reloj.

---

## 0. Resumen ejecutivo (lo más accionable)

1. **El binario es enorme y "ancho"**: `.text` = 121 MB; 39 144 funciones recompiladas, media 2,6 KB; en una
   muestra, **10-26 instrucciones A64 por instrucción PowerPC** [LOCAL-ELF]. El A57 decodifica **3 µops/ciclo**
   [SOG §4.1] con L1I de 48 KB [TRM §6.1] y su frontal rinde ~1 IPC cuando el código viene de L2 [CNC "Fetch and
   Decode"]. Reducir instrucciones por instrucción guest es la palanca nº 1.
2. **Ida y vuelta por memoria** (almacenar → cargar): ~6-7 ciclos [CNC "Load/Store"; BENCH], frente a 1 ciclo
   de una ALU. Hoy cada `stw/sth` del guest obliga a recargar `ctx.gpr[]` porque un almacenamiento por
   `uint8_t*` puede alias-ar cualquier cosa [LOCAL-CG]. Con `CPUContext& __restrict ctx` GCC deja de recargar
   (19 → 14 instrucciones en el ejemplo, §7.2). Paso intermedio barato antes de "registros guest en registros
   ARM".
3. **Floats directos**: `fadds/fsubs/fmuls/fdivs/fsqrts` con operandos que **ya son single exactos** dan el
   mismo resultado en `float` que en `double`+redondeo (doble redondeo inocuo, 53 ≥ 2·24+2) [DR1; DR2].
   Ahorra los dos `fcvt` (5+5 ciclos) [SOG §3.11]. **No** vale para `fmadds` (FMA no está cubierta).
4. **Evitar mover datos entre registros enteros y FP**: `fmov` GPR↔FP = 5 ciclos y **usa el único pipe de carga
   (L)** [SOG §3.11]; `scvtf/fcvtzs` desde/hacia GPR = 10 ciclos [SOG §3.11] (~12 medido [BENCH]). Para
   `lfs/lfd/stfs/stfd`, cargar directamente en registro FP y hacer `rev32/rev64` NEON (3 ciclos) [SOG §3.16]
   en vez de `ldr w; rev; fmov`.
5. **Multiplicaciones de 32 bits**: `mul w` = 3 ciclos y 1/ciclo; `madd x` = 5 ciclos y **1 cada 3 ciclos**;
   `smulh/umulh` = 6 ciclos y bloquea el pipe M [SOG §3.5]. `mulhw` → `smull x` + `asr #32`, nunca `smulh`.
6. **ALU con operando desplazado/extendido** (`add x0,x1,x2,lsl #2`, `add w0,w1,w2,uxtb`) = 2 ciclos y solo
   pipe M (1/ciclo) [SOG §3.3]; `bfi/bfxil` igual (2 ciclos, M) [SOG §3.7]; `ubfx/lsl/ror/rev/clz/csel` = 1
   ciclo en I0/I1 (2/ciclo) [SOG §3.3-3.7].
7. **`thread_local` cuesta una llamada**: con `-mtp=soft` cada acceso es `bl __aarch64_read_tp` (`mrs
   tpidrro_el0; ldr`) y obliga a montar marco en funciones hoja [LOCAL-CG; LOCAL-LIBNX]. `native_stack_ok()` lo
   usa en cada llamada indirecta del guest: mover el límite a `CPUContext`.
8. **Globales `extern` vía GOT**: con `-fPIE` cada `extern` cuesta una carga extra dependiente (`adrp; ldr GOT;
   ldr`) [LOCAL-CG], visible en la entrada de cada función recompilada [LOCAL-ELF]. Declararlas con
   `#pragma GCC visibility push(hidden)` lo quita (verificado).
9. **Banderas GCC baratas y seguras para la exactitud**: `-fno-math-errno` (quita la llamada a `sqrt` de libm)
    y `-fomit-frame-pointer` (devkitA64 mantiene `x29` por defecto en AArch64 aunque `-Q` diga lo contrario)
    [LOCAL-CG]. Las de errata A53 **no se aplican** en la práctica con nuestras banderas (verificado).

---

## 1. Topología y relojes

| Dato | Valor | Fuente |
|---|---|---|
| SoC | NVIDIA Tegra X1 (Erista: ODNX02-A2; Mariko/Lite/OLED: ODNX10-A1, "X1+") | [SB-HW "Specifications"] |
| Núcleos CPU | 4× Cortex-A57 ("Atlas") + 4× Cortex-A53 ("Apollo") en el die | [SB-TX1] |
| Uso del clúster A53 | Nintendo no los usa | [CNC "Introduction"] |
| Revisión del A57 | r1p1 (MIDR variante 0x1, revisión 1) en el Tegra X1 del Jetson Nano | [JETSON] |
| Revisión en la Switch (Erista y Mariko) | **[SIN CONFIRMAR]**: mismo T210 que el Jetson Nano en Erista; Mariko no comprobado. `MIDR_EL1` desde EL0 probablemente no se puede leer en Horizon | — |
| L2 | 2 MB compartida por los 4 A57 (A53: 512 KB aparte) | [SB-TX1; CNC "Overview"] |
| Reloj CPU de los juegos (portátil y dock) | **1020 MHz** (todas las configuraciones normales) | [SB-PTM §PerformanceConfiguration] |
| 1224 MHz | Solo en unidades SDEV (desarrollo) | [SB-PTM] |
| 1785 MHz | Solo en "CpuBoostMode" (7.0.0+), y entonces la GPU baja a 76,8 MHz (pantallas de carga) | [SB-PTM; LIBNX apm.h] |
| Memoria (EMC) | 1600 MHz o 1331,2 MHz según configuración (por defecto 0x20003: 1331,2) | [SB-PTM] |
| Pasos de sys-clk | 612, 714, 816, 918, 1020, 1122, 1224, 1326, 1428, 1581, 1683, 1785 MHz | [SYSCLK README] |
| Más de 1785 MHz | Suites de OC comunitarias; la consola del usuario llegó a 1963,5 MHz con OC + `appletSetCpuBoostMode` [BENCH]. Límites seguros: **[SIN CONFIRMAR]**, fuera de alcance (el objetivo es 1020 MHz) | [BENCH] |
| Núcleos permitidos a homebrew (hbloader) | 0-2 (`lowest_cpu_id` 0, `highest_cpu_id` 2), núcleo por defecto 0 | [HBL hbl.json] |
| Confirmado en la consola | máscara 0x7 (núcleos 0-2) en modo aplicación | [BENCH] |
| Núcleo 3 | Del sistema; `threadCreate` acepta 0-3, pero el NPDM de hbloader limita a 0-2 | [LIBNX thread.h; HBL] |
| Prioridad 0x3B | Prioridad especial que activa la multitarea preemptiva en los núcleos 0-2 (0x3F en el 3) | [LIBNX thread.h] |
| Modo applet (álbum) | Menos RAM; si también recorta CPU: **[SIN CONFIRMAR]** | — |
| Contador del sistema (CNTFRQ) | **19,2 MHz**: libnx convierte `ticks = ns·12/625` (= 0,0192 ticks/ns) y lee `cntfrq_el0` | [LIBNX counter.h] |
| Coste de `armGetSystemTick()` | 8 ciclos | [BENCH] |
| Cambiar el reloj desde homebrew | `clkrstSetClockRate` (8.0.0+) o `pcvSetClockRate` (1.0.0-7.0.1); `appletSetCpuBoostMode` (1785 MHz pero GPU al mínimo) | [LIBNX clkrst.h, pcv.h, apm.h] |

Presupuesto: a 1020 MHz, 16,7 ms = **17,0 M ciclos** por frame y por hilo. El "~25 ms/frame" del hilo del
juego se midió **a 1785 MHz** (≈45 M ciclos), es decir ~44 ms a 1020 MHz [BENCH §6]. Subir el reloj ya está
descontado: el objetivo del proyecto es 1020 MHz sin overclock.

---

## 2. Cachés y memoria

### 2.1 Tabla

| Estructura | Valor | Fuente |
|---|---|---|
| L1I | 48 KB, 3 vías, línea 64 B, PIPT, LRU, paridad | [TRM §6.1] |
| L1D | 32 KB, 2 vías, línea 64 B, PIPT, LRU, ECC por 32 bits (opcional en la implementación) | [TRM §6.1; CNC "Cache and Memory Latency"] |
| Latencia L1D (carga→uso) | 4 ciclos; 5 con direccionamiento indexado complejo | [SOG §3.8; CNC; 7CPU] |
| Cargas a registros FP/NEON | +1 ciclo (5) | [SOG §3.12] |
| Fallos L1D pendientes | 6 líneas de 64 B (no bloqueante, hit-under-miss) | [TRM §6.4] |
| L2 | 2 MB, 16 vías, línea 64 B, ECC obligatoria, reemplazo aleatorio, **inclusiva** de las L1D | [TRM §2.1.1; CNC] |
| Latencia L2 | 22 ciclos (>50 si además falla la L2 TLB) | [CNC]; 22-24 medido [BENCH] |
| Latencia RAM | ~250 ciclos | [CNC]; 245-265 ciclos (~135 ns a 1785 MHz) [BENCH] |
| Fallos L2 pendientes | hasta 20 | [CNC "Some Light Benchmarking"] |
| Ancho de banda L1D | 1 carga + 1 almacenamiento por ciclo | [SOG §4.5] |
| Ancho de banda L2 (1 núcleo) | ~8 B/ciclo; 4 núcleos juntos 15,7 B/ciclo | [CNC "Bandwidth"] |
| RAM teórica | LPDDR4-3200 de 64 bits = 25,6 GB/s (X1+: LPDDR4X, ~31,8 GB/s) | [SB-TX1 "Memory Frequency"; CNC] |
| RAM real desde la CPU | < 8 GB/s con todos los núcleos (≈30 % de la teórica) | [CNC "Bandwidth"] |
| RAM real 1 núcleo | `memcpy` 3,7 GB/s, lectura NEON 4,9 GB/s, `memset` 10,6 GB/s | [BENCH §2] |
| Nota | Con EMC a 1331,2 MHz (config. por defecto) la teórica baja a ~21,3 GB/s (cálculo propio) | [SB-PTM] |

### 2.2 TLB y páginas

| Estructura | Valor | Fuente |
|---|---|---|
| L1 ITLB | 48 entradas, totalmente asociativa, páginas nativas 4 KB/64 KB/1 MB | [TRM §5.2.1] |
| L1 DTLB | 32 entradas, totalmente asociativa, 4 KB/64 KB/1 MB; acierto en 1 ciclo | [TRM §5.2.2] |
| L2 TLB (unificada) | 1024 entradas, 4 vías; 4K, 64K, 1M, 16M, 2M, 1G, 512M | [TRM §5.2.3] |
| Bloques > 1 MB | La L1 TLB solo guarda 1 MB de ellos (un bloque de 2 MB ocupa 2 entradas al usarse) | [TRM §5.2.1] |
| Fallo L1 DTLB (acierta L2 TLB) | ~7 ciclos; fallo L2 TLB 30-36 ciclos | [7CPU, con páginas de 64 KB] |
| Coste de la L2 TLB | "más de 20 ciclos" | [CNC "Final Words"] |
| Cobertura L1 DTLB | 128 KB con 4 KB; 32 MB con entradas de 1 MB | cálculo [TRM] |

Consecuencia: la RAM del guest (24 MB MEM1 + 64 MB MEM2) solo puede quedar en la L1 DTLB si el kernel la mapea
con bloques grandes. **Cómo mapea Horizon el heap de una aplicación (páginas de 4 KB o bloques de 2 MB):
[SIN CONFIRMAR]**. Medirlo (§9, prueba M3).

### 2.3 Prefetchers

- **L1I**: tras un fallo, pide también la línea siguiente a la L2 [TRM §6.3.6].
- **L1D**: existe (bit CPUACTLR_EL1[56] lo desactiva) [TRM §4.3, CPUACTLR]; el algoritmo no está documentado
  en el TRM público. **[SIN CONFIRMAR]**.
- **L2**: detección de *stride* dentro de la página de 4 KB (o de 64 KB si así está mapeada); 0/2/4/8
  prefetches por fallo de datos, 0-3 por fallo de instrucciones; cola de 10 entradas por núcleo; máximo 12
  prefetches en vuelo [TRM §7.4]. Tras un fallo de L2 emite hasta 8, sin salir de la página de 4 KB
  [CNC "Some Light Benchmarking"].
- Los bits de control (CPUACTLR/CPUECTLR) solo se escriben desde EL1+; una aplicación no puede tocarlos.
- **No-temporales (`LDNP/STNP`)**: en revisiones anteriores a r1p3 (la nuestra es r1p1) no se prefetchean ni se
  cachean y pueden ir **más lentos** que las normales; ARM recomienda activar CPUACTLR_EL1[52], que el usuario
  no controla [SOG §4.7]. → **No usar `ldnp/stnp`**. (La `memcpy` de newlib de devkitA64 no los usa: verificado
  [LOCAL-LIBC].)

### 2.4 Almacenamientos, reenvío y alineación

- Cola de almacenamientos: **12** en vuelo; cargas: 32 [CNC "Load/Store"]. Es pequeña y se llena con
  facilidad.
- **No hay predicción de dependencias de memoria**: una carga espera a conocer las direcciones de todos los
  almacenamientos anteriores [CNC "Load/Store"].
- **Reenvío almacenamiento→carga**: ~7 ciclos; ~14 si el solapamiento es parcial con un almacenamiento
  alineado a 16 B [CNC]. 6-7 medido [BENCH]. (Comparar: dejar el valor en registro = 0-1 ciclos.)
- Los almacenamientos se emiten en cuanto se conoce su **dirección**, sin esperar al dato, y se escriben "en
  segundo plano" [SOG §3.9].
- **ECC de la L1D**: un almacenamiento que no cubre un bloque alineado de 4 bytes obliga a leer, fusionar y
  recalcular el ECC; el hardware intenta fusionar almacenamientos contiguos para evitarlo [TRM §6.4.8]. Si
  NVIDIA activó la ECC de L1D en el Tegra X1: **[SIN CONFIRMAR]** (es opcional [CNC]). Relevante para
  `ctx.cr[n].lt/gt/eq/so` guardados como 4 bytes sueltos (§9, prueba M5).
- **Desalineados**: casi todos sin penalización; penalizan las **cargas que cruzan una línea de 64 B** y los
  **almacenamientos que cruzan un límite de 16 B** [SOG §4.6]. Cruzar 64 B: +1 ciclo [7CPU]. Los accesos
  del guest son casi siempre naturales.
- **DC ZVA**: bloque de 64 bytes (`DCZID_EL0.BS = 4` → 16 palabras) [TRM §4.3.27]. La `memset` de newlib en
  devkitA64 ya usa `dc zva` [LOCAL-LIBC]; `memset` mide 10,6 GB/s [BENCH].

---

## 3. Pipeline

| Dato | Valor | Fuente |
|---|---|---|
| Ancho de decodificación/despacho | 3 µops/ciclo | [SOG §4.1; TRM §2.1.1 "Instruction fetch": 3 instr./ciclo] |
| Límites por ciclo al despachar | 1 B, 2 I, 2 M, 1 F0, 1 F1, 2 L/S | [SOG §4.1] |
| Pipes de ejecución | 8 (B, I0, I1, M, L, S, F0, F1), 1 µop/ciclo cada uno | [SOG §2.1] |
| Fetch | hasta 128 bits (4 instrucciones) por acceso a L1I | [TRM §6.3] |
| Ventana fuera de orden | ~128 instrucciones (40 "bundles" en vuelo) | [CNC "Out of Order"; LLVM `MicroOpBufferSize = 128`] |
| Fichero de registros | 128 entradas de 32 bits, unificado entero+FP (un valor de 128 bits ocupa 4) | [CNC "Unified Register File"] |
| Planificadores | casi todos de 8 entradas; salto 12; AGU de carga/almac. 16 compartidas | [CNC "Schedulers"] |
| Eliminación de `mov` / idiomas de cero | **No** tiene (cada `mov` cuesta una µop real) | [CNC "Fetch and Decode"] |
| Fusión de instrucciones | `MOVZ+MOVK` / `MOVK+MOVK` contiguos (r1p0+): constante de 32 bits en 1 ciclo | [SOG §3.4 nota 1, §4.14] |
| Fusión `ADRP+ADD` | Solo r1p3+ → **no en r1p1** | [SOG §4.15] |
| Fallo de predicción | ~15 ciclos (A57 y A15) | [AT tabla A15/A57, con "?"]; 14 [LLVM]; 16-19 [7CPU]; **~21 medido** [BENCH] |
| BTB L1 | 64 entradas, salto tomado sin burbuja (1 ciclo) | [CNC "Branch Prediction"] |
| BTB L2 | 2048-4096 entradas, 2 ciclos; empeora mucho cuando el código no cabe en L1I | [CNC] |
| Predictor indirecto | hasta 16 destinos por salto; 128 destinos en total sin penalización apreciable | [CNC] |
| Pila de retorno | ~32 entradas (inferido por la curva) | [CNC] |
| Historia global | modesta; no sigue patrones largos | [CNC] |
| `blr` | latencia 2 (3 si el registro es LR), 1/ciclo, pipes I+B | [SOG §3.2] |
| Llamada indirecta medida | 6 ciclos con destino fijo; ~28 con destino variable | [BENCH] |
| Alineación de saltos | No más de 2 saltos tomados por bloque alineado de 16 B; alinear entradas de función y destinos a 16 B | [SOG §4.8] |
| Instrucciones que fijan flags | Evitar formas `S` innecesarias (degradan) | [SOG §4.9] |
| Registros especiales (MRS/MSR) | No renombrados: muchos se ejecutan en orden, no especulativos o con vaciado | [SOG §4.12] |
| Escribir FPCR | Auto-sincronizante (no necesita ISB); coste **[SIN CONFIRMAR]** | [SOG §4.16] |

---

## 4. Latencias y rendimiento (AArch64) — Cortex-A57 Software Optimization Guide

Todas las filas son de [SOG §3.x] salvo que se diga otra cosa. Marcas: ⚠ = notablemente lento o con trampa en
el A57. Columna "BENCH" = medido en la Switch real cuando existe.

### 4.1 Enteros

| Grupo | Instrucciones | lat | rend | Pipe | BENCH | Nota |
|---|---|---|---|---|---|---|
| ALU básica | `add sub and orr eor bic` (y `adds`…), `adc sbc` | 1 | 2 | I0/I1 | add 1, 2/ciclo | §3.3 |
| ⚠ ALU con shift o extend | `add x0,x1,x2,lsl #n`, `add w0,w1,w2,uxtb`, `orr …,ror #n` | 2 | 1 | M | — | §3.3: va al único pipe M |
| Comparación condicional | `ccmp ccmn` | 1 | 2 | I | — | §3.3 |
| Selección | `csel csinc csinv csneg` (`cset`, `cinc`) | 1 | 2 | I | cmp+csel 2 | §3.3 |
| Inmediatos | `movz movn movk` | 1 | 2 | I | — | §3.4; pares fusionados (§4.14) |
| Direcciones | `adr adrp` | 1 | 2 | I | — | §3.4 |
| Shift variable | `lsl lsr asr ror` por registro | 1 | 2 | I | — | §3.4 |
| Rotación por inmediato | `ror` = `extr` 1 registro | 1 | 2 | I | ror 1 | §3.7 |
| ⚠ `extr` de 2 registros | `extr x0,x1,x2,#n` | 3 | 1 | I+M | — | §3.7 |
| Campo de bits | `ubfx sbfx ubfiz sbfiz`, `lsl/lsr/asr #imm`, `uxtb/sxth/sxtw` (UBFM/SBFM) | 1 | 2 | I | ubfx 1 | §3.7 |
| ⚠ Inserción de bits | `bfi bfxil` (BFM) | 2 | 1 | M | bfi 2 | §3.7 |
| Contar ceros | `clz cls` | 1 | 2 | I | clz 1 | §3.7 |
| Invertir | `rbit rev rev16 rev32` | 1 | 2 | I | rev 1 | §3.7 |
| Mul/MAC 32 bits | `mul madd msub` (W) | 3 (acum. 1) | 1 | M | mul 3 | §3.5 |
| ⚠ Mul/MAC 64 bits | `mul madd msub` (X) | 5 (acum. 3) | **1/3** | M | 5 | §3.5: bloquea M 2 ciclos extra |
| Mul larga | `smull umull smaddl umaddl` (32×32→64) | 3 (acum. 1) | 1 | M | — | §3.5 |
| ⚠ Mul alta | `smulh umulh` | 6 | 1/4 | M | — | §3.5: bloquea M 3 ciclos |
| ⚠ División 32 bits | `sdiv udiv` (W) | 4-20 | 1/20-1/4 | M | udiv ~6 | §3.5: iterativa, termina antes según datos |
| ⚠ División 64 bits | `sdiv udiv` (X) | 4-36 | 1/36-1/4 | M | ~6 | §3.5 |
| CRC | `crc32* crc32c*` | 3 | 1 | M | 3 | §3.20 |
| NOP | `nop` | — | 1 | despacho | — | §3.7: ocupa un hueco de despacho |

### 4.2 Saltos

| Grupo | Instrucciones | lat | rend | Pipe |
|---|---|---|---|---|
| Inmediato | `b`, `b.cond` | 1 | 1 | B |
| Registro | `br ret` | 1 | 1 | B |
| Con enlace | `bl` | 1 | 1 | I + B |
| Con enlace, registro | `blr xN` (N≠30) | 2 | 1 | I + B |
| Con enlace, registro = LR | `blr x30` | 3 | 1 | I + B |
| Comparar y saltar | `cbz cbnz tbz tbnz` | 1 | 1 | B |

Solo 1 salto por ciclo (pipe B único) [SOG §4.1].

### 4.3 Cargas y almacenamientos (enteros)

| Grupo | Ejemplo | lat | rend | Pipes | Nota |
|---|---|---|---|---|---|
| Inmediato, sin escalar, literal | `ldr w0,[x1,#8]`, `ldur` | 4 | 1 | L | |
| Registro base + registro | `ldr w0,[x1,x2]` | 4 | 1 | L | |
| Registro escalado ×4/×8 | `ldr w0,[x1,x2,lsl #2]` | 4 | 1 | L | |
| Registro con extend | `ldr w0,[x1,w2,uxtw]` | 4 | 1 | L | lo que usa hoy `fast_ptr` |
| ⚠ Registro escalado ×2 | `ldrh w0,[x1,x2,lsl #1]` | 5 | 1 | I + L | µop extra |
| Pre/post-índice | `ldr w0,[x1,#4]!` | 4 (base 1) | 1 | L + I | |
| Par | `ldp` | 4 | 1 | L | |
| ⚠ Par con signo | `ldpsw` | 5 | 1/2 | (I+) L | |
| Almacenamiento (casi todas las formas) | `str`, `strb`, `strh`, `stur`, registro, extend | 1 | 1 | S | |
| ⚠ `strh` escalado ×2 | `strh w0,[x1,x2,lsl #1]` | 3 | 1 | I + S | |
| Par 32 bits | `stp w,w` | 1 | 1 | S | |
| ⚠ Par 64 bits | `stp x,x` | 2 | **1/2** | S | no es más rápido que dos `str x` |

Un solo pipe L y un solo pipe S: como mucho **1 carga + 1 almacenamiento por ciclo** [SOG §4.5]. Para copias,
intercalar `ldp`/`stp` sin writeback [SOG §4.5].

### 4.4 Coma flotante escalar

| Grupo | Instrucciones | lat | rend | Pipe | BENCH | Nota |
|---|---|---|---|---|---|---|
| Abs/neg | `fabs fneg` | 3 | 2 | F0/F1 | — | |
| Suma | `fadd fsub` | 5 | 2 | F0/F1 | 5 | ⚠ medido ~1,3-1,6 ciclos/op, no 2/ciclo [BENCH; CNC "Floating Point"] |
| Multiplicación, FZ=1 | `fmul fnmul` | 5 | 2 | F0/F1 | — | |
| Multiplicación, FZ=0 (la nuestra) | `fmul fnmul` | 6 | 2 | F0/F1 | 6 | |
| FMA, FZ=1 | `fmadd fmsub fnmadd fnmsub` | 9 (acum. 4) | 2 | F0/F1 | — | |
| ⚠ FMA, FZ=0 | ídem | 10 (acum. 4) | 2 | F0/F1 | 10 | más lenta que `fmul`+`fadd` en latencia |
| Comparación | `fcmp fcmpe fccmp` | 3 | **1** | F1 | — | solo F1 |
| Selección | `fcsel` | 3 | 2 | F0/F1 | — | |
| Min/max | `fmin fmax fminnm fmaxnm` | 5 | 2 | F0/F1 | — | |
| Redondeo a entero (en FP) | `frinta/i/m/n/p/x/z` | 5 | 2 | F0/F1 | — | |
| ⚠ División single | `fdiv s` | 7-17 | 2/15-2/5 | F0 | 18 | iterativa, bloquea F0 |
| ⚠ División double | `fdiv d` | 7-32 | 1/30-1/5 | F0 | 32 | |
| ⚠ Raíz single/double | `fsqrt` | 7-17 / 7-32 | igual que `fdiv` | F0 | ~17 / ~7 (*) | (*) dato que converge; orientativo |
| ⚠ Conversión de precisión | `fcvt s↔d`, `fcvt h↔s/d` | 5 | 1 | F0/F1 | 5 por sentido | 1/ciclo, no 2 |
| Mover inmediato / registro FP | `fmov d0,#1.0`, `fmov d0,d1` | 3 | 2 | F0/F1 | — | sin eliminación de movs |
| ⚠ GPR → FP | `fmov s0,w0`, `fmov d0,x0` | 5 | 1 | **L** | ~4,5 | gasta el pipe de carga |
| ⚠ FP → GPR | `fmov w0,s0` | 5 | 1 | **L** | ~4,5 | |
| ⚠ Entero (GPR) → FP | `scvtf ucvtf` desde W/X | 10 | 1 | L + F | ~12 | |
| ⚠ FP → entero (GPR) | `fcvtzs fcvtzu fcvtns…` a W/X | 10 | 1 | L + F | ~12 | |
| FP → entero dentro de FP | `fcvtzs d0,d0` / `fcvtzs s0,s0` (forma SIMD escalar) | 5 | 2 | F0/F1 | — | §3.15 "ASIMD FP convert, other, D-form"; que la forma escalar caiga en ese grupo: **[SIN CONFIRMAR]** (prueba F6) |
| Carga FP | `ldr s/d` (imm, reg, extend, S/D escalado) | 5 | 1 | L | — | §3.12 |
| ⚠ Carga Q o H escalada | `ldr q0,[x1,x2,lsl #4]` | 6 | 1 | I + L | — | |
| Carga par FP | `ldp s/d` 5 (1/ciclo); `ldp q` 6 (1/2) | | | L | — | |
| Almac. FP B/H/S/D | `str s0`, `str d0` | 1 | 1 | S | — | §3.13 |
| ⚠ Almac. Q | `str q0` | 2 | 1/2 | S (+I) | — | |
| ⚠ Almac. par | `stp d` 2 (1/2); `stp q` 4 (1/4) | | | S | — | |

### 4.5 NEON (ASIMD)

El A57 implementa NEON con dos pipes de 64 bits de ancho efectivo para muchas operaciones: **las formas Q
(128 bits) de FP, mul, desplazamientos por registro, BSL, CNT… van a mitad de rendimiento que las formas D
(64 bits)**. Las sumas/lógicas/comparaciones enteras básicas no distinguen D/Q en la tabla (2/ciclo) [SOG §3.14],
y [CNC] confirma 2/ciclo en sumas enteras de 128 bits.

| Grupo | D (64 bits) lat / rend | Q (128 bits) lat / rend | Pipe | Nota |
|---|---|---|---|---|
| Suma/resta/lógica/comparación/min-max enteros | 3 / 2 | 3 / 2 | F0/F1 | §3.14 |
| Diferencia absoluta `sabd` | 3 / 2 | 3 / 1 | F0/F1 | |
| ⚠ Multiplicación entera `mul` | 4 / 1 (r1p0+) | 5 / 1/2 | F0 | §3.14 nota 4 |
| MAC entera `mla` | 4 (acum. 1) / 1 | 5 (acum. 2) / 1/2 | F0 | |
| Shift por inmediato `shl ushr sxtl…` | 3 / 1 | 3 / 1 | F1 | solo F1 |
| ⚠ Shift por registro `sshl ushl` | 3 / 1 | 4 / 1/2 | F1 | |
| Inserción por shift `sli sri` | 3 / 1 | 4 / 1/2 | F1 | |
| Reducción `addv` 4S/4H | 4 / 1; 8B/8H 7 / 1; 16B 8 / 1/2 | | F1 | |
| FP `fadd fsub fabd` | 5 / 2 | 5 / **1** | F0/F1 | §3.15 |
| FP `fmul` (FZ=0) | 6 / 2 | 6 / **1** | F0/F1 | |
| FP `fmla fmls` | 9 (acum. 4) / 2 | 10 (acum. 4) / **1** | F0/F1 | [BENCH]: 4S acumulando = 4 |
| FP comparación | 5 / 2 | 5 / 1 | F0/F1 | |
| FP conversión entero↔FP | 5 / 2 | 5 / 1 | F0/F1 | |
| ⚠ `fcvtl` / `fcvtn` (ensanchar/estrechar) | 8 / 2/3 | | F0/F1 | §3.15 |
| ⚠ `faddp` pairwise Q | | 8 / 2/3 | F0/F1 | |
| ⚠ `fdiv` F32 | 7-17 / 1/15-1/5 | 14-34 / 1/30-1/10 | F0 | F64 Q: 14-64 |
| `frecpe frsqrte` | 5 / 2 | 5 / 1 | F0/F1 | |
| `frecps frsqrts` (FZ=0) | 10 / 2 | 10 / 1 | F0/F1 | |
| `bsl bit bif` | 3 / 2 | 3 / 1 | F0/F1 | |
| `cnt clz cls` | 3 / 2 | 3 / 1 | F0/F1 | |
| `rev16 rev32 rev64`, `rbit` | 3 / 2 | (sin distinción) | F0/F1 | [BENCH] rev32 de 16 B = 3 |
| `trn uzp zip`, `ext`, `dup` de elemento, `ins` elem→elem, `movi` | 3 / 2 | | F0/F1 | |
| ⚠ `tbl tbx` (N registros de tabla) | 3·N | 3·N + 3 | F0/F1 | §3.16 nota 1 |
| ⚠ `umov` a GPR (W/X) | 5 / 1 | | L | |
| ⚠ `smov`, `umov` B/H | 6 / 1 | | L + I | |
| ⚠ `ins` desde GPR, `dup` desde GPR | 8 / 1 | | L + F | |
| `ld1` 1 registro | 5 / 1 | 5 / 1 | L | §3.17 |
| `ld1` 2 registros | 5 / 1 | 6 / 1/2 | L | |
| `ld1` 4 registros | 6 / 1/2 | 8 / 1/4 | L | |
| ⚠ `ld1` un carril B/H/S, `ld1r` | 8 / 1 | 8 / 1 | L + F | |
| `ld2` | 8 / 1 | 9 / 1/2 | L + F | |
| `st1` 1 registro | 1 / 1 | 2 / 1/2 | S | §3.18 |
| ⚠ `st1` 4 registros | 4 / 1/4 | 8 / 1/8 | S | |
| ⚠ `st1` un carril B/H/S | 3 / 1 | | F + S | |

### 4.6 Criptografía

| Grupo | lat | rend | Pipe | Fuente |
|---|---|---|---|---|
| `aese aesd aesmc aesimc` (pares `aese+aesmc` contiguos más rápidos, r0p1+) | 3 | 1 | F0 | [SOG §3.19, §4.13] |
| `pmull` 64×64 | 3 | 1 | F0 | [SOG §3.19] |
| `sha1h sha1su1`, `sha256su0` | 3 | 1 | F0 | [SOG §3.19] |
| `sha1c/m/p`, `sha256h/h2`, `sha256su1` | 6 | 1/2 | F0 | [SOG §3.19] |
| `sha1su0` | 6 | 2 | F0/F1 | [SOG §3.19] |

---

## 5. Atómicos y barreras

- **ARMv8.0 sin LSE**: `ID_AA64ISAR0_EL1` solo declara AES, SHA1, SHA2 y CRC32; los bits [63:20] (donde
  estaría "Atomic") son RES0 [TRM §4.3.20, tabla 4-34]. No existen `ldadd`, `cas`, `swp`.
- GCC de devkitA64 emite los atómicos **en línea** con bucles LL/SC (`ldaxr/stlxr`, `ldxr/stxr`); `load` seq_cst
  → `ldar`, `store` seq_cst → `stlr`, `atomic_thread_fence` → `dmb ish`; `-moutline-atomics` está desactivado
  [LOCAL-CG]. (En otros GCC viene activado por defecto [GCC "-moutline-atomics"]; aquí no hace falta tocarlo.)
- El monitor exclusivo de memoria Normal Write-Back compartida lo hace la propia L1 con la coherencia, sin
  ir al bus [TRM §6.4.5].
- Costes: la SOG **no** publica latencias de `ldxr/stxr/ldar/stlr/dmb` (LLVM las modela como 1 ciclo sin base
  medida [LLVM]). Medido en la consola [BENCH]: `fetch_add` 21-29 ciclos; `std::shared_ptr` copia+destrucción 73;
  mutex sin contención 78 (libnx) / 89 (`std::mutex`); línea de caché ida y vuelta entre núcleos ~270; despertar
  un hilo en otro núcleo ~8 400 ciclos. `dmb ish` aislado y `ldar/stlr`: **[SIN CONFIRMAR]** (§9, prueba A1).
- Consecuencias:
  - Refcounts atómicos (`shared_ptr`) en caminos calientes: evitar; usar propiedad única o conteo no atómico
    dentro de un hilo.
  - Una sola escritura/lectura publicada entre hilos: `stlr`/`ldar` (release/acquire) en vez de `dmb ish`
    completo.
  - `memory_order_relaxed` genera `ldxr/stxr` sin barreras de adquisición/liberación [LOCAL-CG].
  - Datos de hilos distintos en líneas de 64 B distintas (falso compartir: ~270 ciclos por rebote [BENCH]).

---

## 6. Coma flotante: lo que importa para la exactitud con la Wii

### 6.1 Qué soporta el A57

| Característica | Estado | Fuente |
|---|---|---|
| FP32 y FP64 escalares y NEON (FP64 en vectores 2D) | Sí | [TRM §4.3.18: `ID_AA64PFR0.FP = 0`, `AdvSIMD = 0`] |
| FP16 aritmético | **No** (FP=0 significa "FP sin half-precision"; la aritmética FP16 es de ARMv8.2) | [TRM §4.3.18] |
| FP16 conversiones | Sí: `fcvt` h↔s/d, `fcvtl/fcvtn` | [TRM §14.4.4 MVFR1: FPHP=2, SIMDHP=1; SOG §3.11, §3.15] |
| FMA fusionada | Sí, escalar y NEON | [TRM §14.4.4 MVFR1: SIMDFMAC=1; SOG §3.10] |
| FP16 en la consola | solo conversión, 10 ciclos ida y vuelta | [BENCH] |

### 6.2 `-ffp-contract=off` y FMA

- GCC en modo GNU contrae `a*b+c` en `fmadd` por defecto; `-ffp-contract=off` lo impide. **Hay que mantenerlo**:
  `fmul`+`fadd` del guest tienen dos redondeos y `fmadd` uno, así que el resultado cambia.
- `std::fma(a,b,c)` sigue generando un único `fmadd d` con `-ffp-contract=off` (lo verifica [LOCAL-CG]). El
  `fmadd` de PowerPC en doble precisión redondea una sola vez, igual que `fmadd d` del A64.
- `fmadds` (single) en el runtime = FMA en double + redondeo a single (doble redondeo, como el intérprete de
  Dolphin). Eso **no** es igual que `fmadd s`, porque el doble redondeo no es inocuo para FMA [DR1].
  Mantener `fmadd d` + `fcvt s,d` + `fcvt d,s`.
- Cuidado con `fnmadd/fnmsub` del A64: calculan `(-a) + (-(n·m))` con un redondeo; el PowerPC calcula
  `-(a·c ± b)` (negación al final). Pueden diferir en el **signo de un cero exacto** y en el signo de NaN.
  Esto sale del pseudocódigo de la arquitectura y no se ha probado: **[SIN CONFIRMAR]**. Opción segura:
  `fmadd` + `fneg` (+3 ciclos).
- Latencia: `fmadd` = 10 ciclos con FZ=0, frente a `fmul` (6) + `fadd` (5) = 11 [SOG §3.10]. Fusionar no ahorra
  casi nada en el A57; solo se gana en rendimiento (una µop).

### 6.3 Denormales, FZ, DN, redondeo

- **FPCR.FZ (flush-to-zero)**: con FZ=1, `fmul` baja de 6 a 5 y `fmadd` de 10 a 9 [SOG §3.10]. Pero FZ=1 cambia
  los resultados con denormales. **Mantener FZ=0** salvo que se demuestre que el guest trabaja en un modo
  equivalente (el bit NI del Gekko no es igual que FZ de ARM).
- **Coste de los denormales**: la SOG da latencias fijas para `fadd/fmul/fmadd` (solo `fdiv/fsqrt` varían con
  los datos) y no menciona asistencias por microcódigo [SOG §3.10]. Medido: `fmul` con denormales = 6 ciclos,
  **sin penalización** [BENCH]. `fadd`/`fmadd`/`fcvt` con denormales: **[SIN CONFIRMAR]** (§9, prueba F4).
- **FPCR.DN (NaN por defecto)**: con DN=0 el A64 propaga el NaN de entrada (silenciado). El orden de prioridad
  entre operandos NaN del A64 no coincide en general con el del PowerPC; el runtime ya documenta que no modela
  los payloads de NaN (`cpu_context.h`).
- **Modo de redondeo (FPCR.RMode)**: escribir FPCR no necesita ISB [SOG §4.16], pero los MSR de registros
  especiales no se renombran y suelen serializar [SOG §4.12]; coste exacto **[SIN CONFIRMAR]**. No cambiar FPCR
  por instrucción; si el guest cambia `FPSCR.RN`, cambiar FPCR una vez y dejarlo.
- **Balanceo F0/F1 por paridad de registro** (anterior a r1p3, nuestra r1p1): las mul/FMA FP se reparten entre F0
  y F1 según el registro destino par o impar; ARM recomienda mezclar destinos pares e impares y, en cadenas de
  acumulación, que fuente y destino tengan la misma paridad [SOG §4.10-4.11]. GCC no lo hace a propósito.
  Efecto real **[SIN CONFIRMAR]**.

### 6.4 "Single calculado en double y redondeado": coste y atajo exacto

Patrón actual de `fadds`/`fmuls`: `fmul d` (6) → `fcvt s,d` (5) → `fcvt d,s` (5) = **16 ciclos** de latencia
(BENCH: `fadds` 15 frente a 5 de un `fadd`). Además `fcvt` va a 1/ciclo (no 2) [SOG §3.11].

**Atajo exacto**: si los dos (o tres) operandos son valores **exactamente representables en single**, entonces
para `+ - × ÷ √` es lo mismo `(float)((double)a OP (double)b)` que `a OP b` en `float` (un solo redondeo).
El doble redondeo es inocuo cuando la precisión intermedia cumple p' ≥ p+1 (suma), ≥ p (mul/div), ≥ p+2 (raíz);
para single (p=24) dentro de double (53 = 24+29) se cumple en todos los casos [DR1 §1; DR2]. Condiciones:

- Operandos single exactos: vienen de `lfs`, `psq_l`, o de otra operación single (`fadds`, `fmuls`, `frsp`…).
  Un FPR que salió de una operación double (`fadd`, `lfd`) **no** vale sin comprobarlo.
- `Force25Bit` sobre el operando C de las multiplicaciones single: si C ya es single (24 bits), redondear a 25
  bits no lo cambia, así que también desaparece.
- Mismo modo de redondeo y FZ=0 en ambos caminos.
- El resultado se guarda en double extendiendo el float (`fcvt d,s`: 5 ciclos) solo cuando hace falta (al
  final de una cadena, o si se guarda en `ctx.fpr[]` como double).
- **No** vale para `fmadds/fmsubs/fnmadds/fnmsubs` (FMA) ni para `fres/frsqrte` (tablas de Dolphin).
- Validar contra el oráculo con pruebas aleatorias + casos límite (denormales, infinitos, NaN, cancelación).

### 6.5 Paired singles

- Un par `ps0/ps1` cabe en un registro D de NEON (`v.2s`). Las formas D (64 bits) van a **rendimiento completo**
  (`fadd/fmul v.2s` = 5-6 ciclos, 2/ciclo), igual que un escalar [SOG §3.15]. Las formas Q (`v.2d`, 2 doubles)
  van a la mitad.
- Con el atajo de §6.4 (operandos single), `ps_add/ps_sub/ps_mul/ps_div` pasan a ser una sola instrucción
  `v.2s`. Si se emula "en double y redondear" hacen falta `fcvtl` (8) + op 2D (5-6, 1/ciclo) + `fcvtn` (8) +
  `fcvtl` (8): unas 30 latencias y rendimiento bajo [SOG §3.15].
- Hoy `ps0` vive en `fpr[]` y `ps1` en `ps1[]` (arrays separados). Para usar `v.2s` hace falta tenerlos juntos
  (en registro o en memoria contigua). Si no, cada operación necesita `ins`/`ld1` de carril (3/8 ciclos).
- `ps_merge*` → `ins`/`zip`/`trn` (3 ciclos); `ps_sum0/1` → `faddp` D (5) + `ins`; `ps_madd` (FMA) → NEON
  `fmla` 2S es **un** redondeo en single, distinto de la referencia (FMA en double + redondeo): no usar sin
  prueba.

---

## 7. Guía práctica para el recompilador

### 7.1 Medidas del binario actual

`switch/nsmbw/build/nsmbw.elf` (build del 31-oct) [LOCAL-ELF]:

- `.text` 121 355 352 bytes; 39 144 funciones `func_8*` que suman 104 MB (media 2 660 B).
- Expansión en 4 funciones de muestra (tamaño guest ÷ tamaño host):
  - `func_800B0DB0`: 60 instrucciones PowerPC → 594 instrucciones A64 (×10)
  - `func_800B0EA0`: 76 → 1 983 (×26)
  - `func_800B0FD0`: 76 → 859 (×11)
  - `func_800B1100`: 52 → 673 (×13)
- Un `sth` del guest (en `func_800B0EA0`) compila a **11 instrucciones**:
  1. `ldr w1,[x19,#12]`: **recarga** `ctx.gpr[3]`.
  2. `ldrh w2,[x19,#20]`: **recarga** `gpr[5]`.
  3. `add` de la dirección efectiva.
  4. `and #0x3fffffff`.
  5. `mov`+`movk` del límite (constante fusionada, 1 ciclo [SOG §4.14]).
  6. `sub` + `cmp` + `b.hi` hacia la ruta lenta.
  7. `add` de la dirección host.
  8. `rev16`.
  9. `strh`.
- En cada entrada de función: 2 cargas por GOT (`adrp; ldr; ldr`) de globales (`g_watch_addr` y la bandera de
  traza) + comparación de `ctx.pc`.

Con 3 µops/ciclo como techo y ~1 IPC real cuando el código no está en L1I [CNC], el coste por instrucción
guest es del orden de **10-25 ciclos**. El Broadway hacía ~1 instrucción por ciclo a 729 MHz. Reducir el
número de instrucciones A64 por instrucción guest y el tamaño del código caliente es lo que más rinde.

### 7.2 Patrones rápidos y lentos en el A57

**Valores en registros, no en memoria**
- Cada valor guest que vuelve de memoria cuesta ~7 ciclos de reenvío [CNC] frente a 1 de una ALU, y la cola de
  almacenamiento es de solo 12 [CNC].
- Causa concreta hoy: un almacenamiento por `uint8_t*` (memcpy a RAM guest) puede alias-ar `ctx`, así que GCC
  recarga `ctx.gpr[]`, `ctx.inst_count`… después de cada `stw/sth/stb` del guest [LOCAL-CG].
- Remedios, de menos a más trabajo:
  1. `void func_X(CPUContext& __restrict ctx)` (GCC acepta `__restrict` en referencias). En el ejemplo de
     prueba, 19 → 14 instrucciones y un solo `inst_count +=` [LOCAL-CG]. Es válido porque la RAM guest
     (`mem1/mem2`) nunca se solapa con el objeto `CPUContext`. Hay que confirmar que ninguna ruta lenta
     (`*_slow`, MMIO) escribe en `ctx` a través de un puntero que no sea `ctx`.
  2. Copiar los GPR guest usados a variables locales al entrar en la función y escribirlos en `ctx` solo
     antes de llamadas, salidas y sondeos (ya en el plan: [BENCH §6] punto 2).
- `ctx.inst_count += N` en cada bloque básico es un cargar-sumar-guardar si no se queda en registro: acumular en
  un local y volcarlo en las salidas.

**CR (registro de condición)**
- Hoy `cmpwi` escribe 4 bytes sueltos (`lt gt eq so`). Son 4 almacenamientos + la carga de `xer`.
  - Mejor que el `b.cond` posterior use directamente el resultado de la comparación (en registro), y
    escribir el CR en `ctx` solo si se lee después o en la salida.
  - Si hay que guardarlo, un solo `strb`/`str` con el campo empaquetado (`cset` + `bfi`/`orr`) en vez de 4
    bytes.
  - Posible coste extra por ECC de L1D con escrituras de bytes sueltos: §2.4, prueba M5.

**Comparaciones y selecciones**
- `csel/cset/csinc` = 1 ciclo, 2/ciclo [SOG §3.3]: buenos para saltos impredecibles. Con saltos predecibles, un
  salto sale más barato que una cadena de `csel` (fallar la predicción cuesta ~15-21 ciclos [AT; BENCH]).
  ARM recomienda la ejecución condicional solo para operaciones de ≤2 ciclos [SOG §4.2].
- No usar formas que fijan flags (`adds`, `ands`) salvo que se use el flag [SOG §4.9]. Para el carry de XER
  (`addc/adde`), `adds`/`adcs` es lo natural; si se materializa como 0/1, `cset w, cs` (1 ciclo).

**Rotaciones y máscaras (`rlwinm`, `rlwimi`, `rlwnm`)**
- `rlwinm` con máscara contigua que no envuelve → **un solo** `ubfx`/`ubfiz`/`lsl`/`lsr`/`and #imm` (1 ciclo, I).
- Caso general → `ror w` (1 ciclo) + `and #imm` (1 ciclo). GCC ya lo hace a partir de `(r<<n | r>>(32-n)) & m`
  [LOCAL-CG].
- `rlwimi` → `bfi`/`bfxil` (2 ciclos, pipe M). Si hay muchos seguidos, el pipe M es el cuello (también lo
  usan mul, div, CRC y las ALU con shift).
- `rlwnm` (shift variable) → `ror w, w, wN` (1 ciclo) + `and`.
- Evitar la ALU con operando desplazado en código crítico (`add x, x, x, lsl #n` = 2 ciclos, M) cuando dos
  instrucciones simples pueden ir en I0/I1. GCC con `-mtune=cortex-a57` ya pondera esto.
- `cntlzw` → `clz w` (1 ciclo) [LOCAL-CG].

**Multiplicación y división**
- `mullw/mulli` → `mul w` (3 ciclos). No usar `mul x` (5 ciclos, 1/3) si basta con 32 bits [SOG §3.5].
- `mulhw/mulhwu` → `smull/umull x` (3) + `asr/lsr x, #32` (1) = 4 ciclos. **No** `smulh/umulh` (6 ciclos y
  bloquea M).
- `divw/divwu` → `sdiv/udiv w` (4-20). Los casos /0 y `INT_MIN/-1` necesitan su propia rama para igualar a la
  Wii/Dolphin (ARM devuelve 0); esa rama es predecible.

**Accesos a memoria guest**
- Byte-swap: `rev` (32), `rev16` (16), `rev x` (64) = 1 ciclo, 2/ciclo [SOG §3.7]; [BENCH]: "gratis".
- `lha` (halfword con signo) → `ldrh` + `rev w` + `asr #16`: 3 instrucciones de 1 ciclo, o `rev16` + `sxth`.
- `lfs` hoy: `ldr w` (4) + `rev` (1) + `fmov s,w` (5, pipe L) + `fcvt d,s` (5) = 15. Alternativa: `ldr s0` (5) +
  `rev32 v0.8b` (3) + `fcvt` (5) = 13, sin gastar el pipe L en el `fmov` [SOG §3.12, §3.16, §3.11]. Para `lfd`:
  `ldr d0` + `rev64 v0.8b` en vez de `ldr x; rev x; fmov d,x`. Lo mismo al revés para `stfs/stfd`. Hay que
  medirlo (§9, prueba F2): GCC tiende a hacer el `rev` en GPR a partir de `__builtin_bswap32` + `memcpy`, así
  que puede hacer falta un intrínseco (`vrev32_u8`) o asm en línea.
- `ldr w0,[x1,w2,uxtw]` (base + índice extendido) = 4 ciclos, sin coste extra [SOG §3.8]. Es el modo que ya usa
  `fast_ptr`.
- Comprobación de límites actual: `and`, `sub`, `cmp`, `b.hi` + constante = ~5 instrucciones por acceso
  [LOCAL-ELF]. Idea a estudiar (no verificada en Horizon): reservar 1 GB de espacio virtual, mapear MEM1 en +0 y
  MEM2 en +0x10000000, y dejar que los accesos fuera de rango fallen en un manejador de excepciones (como el
  "fastmem" de Dolphin). El acceso quedaría en `and` + `ldr [base, w, uxtw]`. Habría que comprobar que se
  puede: `virtmemReserve` + `svcMapMemory`, y si libnx deja recuperar una excepción de acceso.

**Llamadas**
- Llamadas directas `bl` = 1 ciclo y bien predichas.
- `blr` con destino fijo: 6 ciclos; con destino variable: ~28 [BENCH].
- El predictor indirecto admite hasta 16 destinos por salto [CNC]: un `switch` de despacho con muchos casos
  calientes desde **un solo** `br` predice peor que varios puntos de salto distintos.
- Pila de retorno de ~32 [CNC]: llamadas guest muy profundas (>32) fallan la predicción del `ret`.
- `thread_local` con `-mtp=soft` = `bl __aarch64_read_tp` (`mrs tpidrro_el0; ldr x0,[x0,#0x1f8]; ret`)
  [LOCAL-LIBNX] + montar marco. `native_stack_ok()` (24 usos solo en `output_10.cpp`) lee `g_native_stack_limit`
  así. Guardar el límite en `CPUContext` o en un global normal (el juego corre en un hilo).

**Constantes y direcciones**
- `movz+movk` contiguos se fusionan (1 ciclo) [SOG §4.14]. No vale la pena guardar constantes en registros si
  falta sitio.
- `adrp+add` **no** se fusiona en r1p1 [SOG §4.15]: 2 ciclos.
- Globales `extern` con `-fPIE`: `adrp` + `ldr` (GOT) + `ldr` = 2 cargas dependientes (8 ciclos) [LOCAL-CG].
  Con `#pragma GCC visibility push(hidden)` alrededor de sus declaraciones: `adrp` + `ldr` [LOCAL-CG].
  `-fvisibility=hidden` **no** basta para declaraciones `extern` (verificado). El enlazador no relaja estas
  cargas (se ven en el ELF final [LOCAL-ELF]).

**Registros de 32 y de 64 bits**
- ALU de 32 y 64 bits cuesta lo mismo [SOG §3.3]. La diferencia está en mul/div (§4.1). Las escrituras a `w`
  ponen a cero la parte alta: no hace falta `uxtw` después.

**Código frío y tamaño**
- L1I de 48 KB, BTB L2 ligada a la L1I [CNC], ITLB de 48 entradas [TRM §5.2.1]. Sacar de línea las rutas lentas
  (`*_slow`, trazas, `resume_pruned`) con `[[unlikely]]`/`__builtin_expect` y funciones `cold`/`noinline`, para
  que el camino caliente sea compacto.
- GCC ya alinea funciones a 16 B y bucles a 8 con `-mtune=cortex-a57` [LOCAL-CG], en línea con [SOG §4.8].
  Alinear más (etiquetas) aumenta el código: no conviene sin medir.

### 7.3 Banderas de GCC

| Bandera | Efecto en el A57 / nuestro caso | ¿Exactitud bit a bit? | Recomendación |
|---|---|---|---|
| `-ffp-contract=off` | Evita FMA implícitas | Imprescindible | Mantener |
| `-fno-math-errno` | `sqrt` → `fsqrt` sin la comparación + llamada a libm (verificado [LOCAL-CG]) | Sí (solo deja de escribir `errno`) | **Probar ya** |
| `-fomit-frame-pointer` | devkitA64 en AArch64 crea `x29` en funciones no hoja aunque `-Q` diga "enabled"; con la bandera explícita desaparece y `x29` queda libre (verificado [LOCAL-CG]) | Sí | **Probar**; pierde las trazas por cadena de marcos (comprobar el volcado de fallos) |
| `-O3` | Más inlining/desenrollado: más código en un binario de 121 MB | Sí (con `-ffp-contract=off`) | Medir; posible pérdida por I-cache |
| `-Os` en código frío | Menos presión de I-cache | Sí | Candidato para funciones guest poco usadas (necesita perfil) |
| `-mcpu=cortex-a57` | Equivale a `-march=armv8-a+crc+crypto -mtune=cortex-a57` [GCC "-mcpu"] | Sí | Sin ganancia esperada |
| LTO | Inlining entre unidades; el runtime ya está en cabeceras inline; tiempo/RAM de enlace enormes con 121 MB | Sí | Prioridad baja |
| PGO (`-fprofile-generate/use`) | Ordena caliente/frío y decide inlining | Sí | Útil; si gcov funciona en Switch (escribir `.gcda` en la SD): **[SIN CONFIRMAR]** |
| `-fno-trapping-math` | Permite mover/especular operaciones FP | Valores iguales; las flags de excepción FPSCR no se modelan | Probablemente seguro; validar con el oráculo |
| `-ffast-math`, `-Ofast`, `-mlow-precision-*`, `-freciprocal-math` | Cambian resultados | **No** | Prohibido [GCC] |
| `-fno-plt` | Sin efecto (enlace estático) | — | Ignorar |
| `-mfix-cortex-a53-835769/843419` | `-Q` los muestra "enabled", pero con nuestras banderas no insertan `nop` ni pasan nada al enlazador (verificado en codegen y en `-dumpspecs`) [LOCAL-CG]. Además el A53 no ejecuta nuestro código | — | Nada que hacer |
| `-moutline-atomics` | Desactivado en devkitA64; LL/SC en línea [LOCAL-CG] | — | Nada que hacer |
| `-mstrict-align` | Desactivado (bien: el A57 tolera desalineados [SOG §4.6]) | — | Mantener desactivado |
| `-fPIE` frente a `-fPIC` | El juego usa `-fPIE`; el runtime de `build-runtime` usa `-fPIC` (más GOT) | — | Usar `-fPIE` en todo + visibilidad oculta |

---

## 8. Fortalezas, debilidades y chuleta

### 8.1 Fortalezas y debilidades

| Fortalezas | Debilidades |
|---|---|
| ALU entera de 1 ciclo en 2 pipes; `rev/ubfx/ror/clz/csel` = 1 ciclo [SOG] | Solo **3 µops/ciclo** de despacho [SOG §4.1] y sin eliminación de `mov` [CNC] |
| Carga L1 de 4 ciclos, desalineados casi gratis [SOG §3.8, §4.6] | **Un** pipe de carga y **uno** de almacenamiento [SOG §4.5]; cola de almacenamientos de 12 [CNC] |
| Byte-swap gratis (`rev*`) [SOG §3.7] | Reenvío almacenamiento→carga ~7 ciclos; sin predicción de dependencias [CNC] |
| L2 de 2 MB, 22 ciclos [CNC] | RAM ~250 ciclos; < 8 GB/s reales [CNC; BENCH] |
| Predictor decente; indirecto de hasta 16 destinos [CNC] | Fallo de predicción ~15-21 ciclos [AT; BENCH]; la BTB se degrada fuera de L1I [CNC] |
| FMA, CRC32 y AES/SHA por hardware [SOG §3.10, §3.19-3.20] | FP de latencia alta: `fadd` 5, `fmul` 6, FMA 10 (FZ=0) [SOG §3.10] |
| NEON D-form a rendimiento completo [SOG §3.15] | NEON Q-form de FP/mul a mitad de rendimiento [SOG §3.14-3.15] |
| Denormales en `fmul` sin penalización [BENCH] | `fdiv/fsqrt` iterativos (hasta 32 ciclos en double) [SOG §3.10] |
| | Cruzar entre bancos de registros caro: `fmov` 5 (pipe L), `scvtf/fcvtzs` 10 [SOG §3.11] |
| | `madd x` 1/3, `smulh` 6, `bfi` y ALU con shift solo en M [SOG §3.3-3.7] |
| | Sin LSE: atómicos LL/SC, `fetch_add` 21-29 ciclos [TRM §4.3.20; BENCH] |
| | TLB pequeñas: 32 entradas L1D, L2 TLB > 20 ciclos [TRM §5.2; CNC] |
| | Sin aritmética FP16 [TRM §4.3.18] |

### 8.2 Chuleta: la forma más barata

| Operación (guest/C++) | Instrucciones A64 recomendadas | Coste | Evitar |
|---|---|---|---|
| bswap32 / 16 / 64 | `rev w` / `rev16 w` / `rev x` | 1 | tablas, shifts manuales |
| `lwz` | `ldr w,[base,w_ea,uxtw]` + `rev` | 4 + 1 | recargar la base desde la GOT cada vez |
| `lhz` / `lha` | `ldrh` + `rev16` / `ldrh` + `rev w` + `asr #16` | 4 + 1 / +2 | `ldrh [x, x, lsl #1]` (5 ciclos) |
| `stw` | `rev` + `str w,[base,w,uxtw]` | 1 + 1 | `stp x` (1/2 ciclo) en copias |
| `lfs` → double | `ldr s` + `rev32 v.8b` + `fcvt d,s` (medir) | 5+3+5 | `ldr w; rev; fmov s,w` (pipe L) |
| `lfd` | `ldr d` + `rev64 v.8b` (medir) | 5+3 | `ldr x; rev x; fmov d,x` |
| `rlwinm` máscara simple | `ubfx`/`ubfiz`/`lsl`/`lsr`/`and #imm` | 1 | — |
| `rlwinm` general | `ror` + `and #imm` | 2 | `extr` de 2 registros (3) |
| `rlwimi` | `bfi`/`bfxil` | 2 (M) | — |
| `cntlzw` | `clz w` | 1 | bucles |
| `mullw` | `mul w` | 3 | `mul x` (5, 1/3) |
| `mulhw/mulhwu` | `smull`/`umull` + `asr`/`lsr #32` | 4 | `smulh/umulh` (6, bloquea M) |
| `divw` | `sdiv w` + rama para /0 y desbordamiento | 4-20 | `sdiv x` (hasta 36) |
| `cmpw` + `bc` | `cmp` + `b.cond` | 1 + salto | escribir 4 bytes de CR si nadie los lee |
| Selección sin salto | `cmp` + `csel`/`cset` | 2 | salto impredecible (~15-21) |
| Constante 32 bits | `movz` + `movk` contiguos | 1 (fusionados) | cargarla de memoria |
| `fadds/fmuls` con operandos single | `fadd s`/`fmul s` (§6.4) | 5 / 6 | `op d` + 2 `fcvt` (16) |
| `fadd/fmul` double | `fadd d`/`fmul d` | 5 / 6 | — |
| `fmadd` double | `fmadd d` (`std::fma`) | 10 | `fmul`+`fadd` (resultado distinto) |
| `fmadds` | `fmadd d` + `fcvt s,d` + `fcvt d,s` | 20 | `fmadd s` (resultado distinto) |
| `frsp` | `fcvt s,d` (+ `fcvt d,s` si se guarda como double) | 5 (+5) | — |
| `fsel` | `fcmp` + `fcsel` | 3 + 3 | salto |
| `fctiwz` (resultado en FPR) + `stfiwx` | `fcvtzs d0,d0` (int64) + `sqxtn s0,d0` (satura a int32, 4 ciclos, F1 [SOG §3.16]) + `str s0`; NaN → 0x80000000 en PPC y 0 en ARM: tratarlo aparte | 5+4+1 | `fcvtzs w` (10) + `fmov s,w` (5) |
| Entero → FP | Si se puede, `scvtf` NEON sobre un valor ya en registro FP | 5 | `scvtf d, w` (10) |
| `ps_add/ps_mul` (operandos single) | `fadd/fmul v.2s` con ps0/ps1 juntos | 5-6, 2/ciclo | `v.2d` + `fcvtl/fcvtn` |
| Contador global en un hilo | variable local, volcar al final | 0 | `ctx.x += n` tras cada almacenamiento guest |
| Atómico de un solo hilo | variable normal | 1 | `std::atomic` / `shared_ptr` (21-73) |
| Publicar entre hilos | `stlr` / `ldar` | ? | `dmb ish` completo |
| Copiar memoria | `ldp/stp q` intercalados, sin writeback | 1 L + 1 S/ciclo | `ldnp/stnp` (r1p1) |
| Poner a cero | `memset` (usa `dc zva`, 64 B) | 10,6 GB/s | bucles de `strb` |
| Acceso TLS | dato en `ctx` o global | 4 | `thread_local` (`bl` + `mrs` + `ldr`) |
| Global `extern` | declaración con visibilidad oculta | 1 carga | GOT (2 cargas) |

---

## 9. Microbenchmarks pendientes en la Switch real

Ya medido en `tools/nxbench` [BENCH]:
- latencias de enteros, FP, `fcvt` y `fmov`
- jerarquía de memoria y anchos de banda
- atómicos, mutex, `svc`, despertar hilos
- fallo de predicción y `blr`

Lo siguiente falta o hay que repetirlo. Todo en ciclos: calibrar los ciclos con una cadena de N `add`
dependientes (1 ciclo cada uno [SOG §3.3]) contra `cntpct_el0` (19,2 MHz), porque `PMCCNTR_EL0` seguramente no
es accesible desde EL0 **[SIN CONFIRMAR]**.

**Memoria**
- **M1**: reenvío almacenamiento→carga del mismo tamaño, parcial (`strb` ×4 → `ldr w`, el patrón de
  `ctx.cr[]`) y desalineado. Esperado: 7 / 14 ciclos [CNC].
- **M2**: comprobación de límites de `fast_ptr` con un bloque que quepa en L1D (repetir la prueba que midió RAM
  en vez de la comprobación).
- **M3**: latencia por puntero aleatorio en 64 KB…88 MB sobre el heap de libnx, con zancada de 4 KB y de 64 KB.
  Si hay salto a ~12-16 KB → páginas de 4 KB. Comparar con un búfer alineado a 2 MB para ver si Horizon mapea
  bloques.
- **M4**: almacenamientos por segundo con `ctx` "caliente": 4× `strb` contiguos frente a 1× `str w`.
- **M5**: rendimiento de bytes sueltos en una línea frente a palabras alineadas (detectaría la ECC de L1D).

**Coma flotante**
- **F1**: cadena `fadds` actual (`fadd d` + `fcvt` + `fcvt`) frente a `fadd s`, con los mismos datos, y
  comprobación bit a bit del resultado; ídem `fmuls` con y sin `ppc_force25`.
- **F2**: `lfs`/`stfs`/`lfd`/`stfd`: variante GPR (`ldr w; rev; fmov`) frente a variante NEON (`ldr s; rev32`),
  en latencia y en rendimiento mezclado con otras cargas (el `fmov` gasta el pipe L).
- **F3**: `fadd`/`fmul` escalares independientes: ¿2/ciclo (SOG) o ~1/ciclo (CNC, BENCH)? Probar mezclando
  registros destino pares e impares (balanceo F0/F1 de r1p1 [SOG §4.11]).
- **F4**: denormales en `fadd`, `fmadd`, `fcvt d→s` con resultado denormal, `fdiv`.
- **F5**: coste de `msr fpcr` (cambiar el redondeo) y de `mrs fpcr`.
- **F6**: `fcvtzs` hacia registro FP + `str s` frente a `fcvtzs w` + `str w`.
- **F7**: `ps_*` con `v.2s` frente a dos escalares.

**Enteros y llamadas**
- **I1**: `smull+asr` frente a `smulh`; `mul x` en cadena y en rendimiento (comprobar 1/3).
- **I2**: `sdiv w` con datos típicos del juego (el iterativo termina antes).
- **I3**: acceso a `thread_local` (`-mtp=soft`) frente a un campo de `ctx`, dentro de una función hoja.
- **I4**: global `extern` vía GOT frente a visibilidad oculta, en un bucle de llamadas.
- **I5**: pila de retorno: recursión de profundidad 8/16/32/48/64; ¿dónde empieza a fallar el `ret`?
- **I6**: predictor indirecto: un `br` con 2/4/8/16/32 destinos en patrón fijo y aleatorio.
- **I7**: rendimiento del frontal: bloques de código de 16 KB…4 MB recorridos en secuencia (IPC desde L1I, L2
  y RAM) para estimar el coste del tamaño de código.

**Atómicos**
- **A1**: `dmb ish` aislado, `ldar`, `stlr`, `ldaxr/stlxr` sin contención; `ldxr/stxr` con dos núcleos
  peleando.

**Sistema**
- **S1**: intentar leer `MIDR_EL1` (si Horizon lo emula) para confirmar r1p1 en Erista y en Mariko.
- **S2**: en modo applet (álbum), máscara de núcleos (`svcGetInfo(InfoType_CoreMask)`) y tiempo de CPU
  disponible frente a modo aplicación.

---

## 10. Fuentes

| Etiqueta | Fuente | Secciones usadas |
|---|---|---|
| [SOG] | ARM, *Cortex-A57 Software Optimization Guide*, UAN 0015B, 28-ene-2016. https://documentation-service.arm.com/static/5ed75ee1ca06a95ce53f93c0 | §2.1 pipes; §3.2-3.20 tablas de instrucciones; §4.1 despacho; §4.2 ejecución condicional; §4.4 riesgos S/D; §4.5 throughput L/S; §4.6 alineación; §4.7 no temporales; §4.8 alineación de saltos; §4.9 flags; §4.10-4.11 F0/F1; §4.12 registros especiales; §4.13 AES; §4.14 literales; §4.15 ADRP+ADD; §4.16 FPCR |
| [TRM] | ARM, *Cortex-A57 MPCore Processor Technical Reference Manual*, r1p0, DDI 0488C (2013). Copia: https://web.eece.maine.edu/~vweaver/classes/ece571_2016s/DDI0488C_cortex_a57_mpcore_r1p0_trm.pdf (índice oficial: https://developer.arm.com/documentation/ddi0488/latest). Es la edición r1p0; las diferencias con r1p1 en lo citado: **[SIN CONFIRMAR]** | §1.3 características; §2.1.1 componentes; §4.3.18 ID_AA64PFR0; §4.3.20 ID_AA64ISAR0 (tabla 4-34); §4.3.27 DCZID_EL0; CPUACTLR/CPUECTLR (§4.3); §5.2.1-5.2.3 TLB; §6.1-6.4 L1 (6.3.6 prefetch I, 6.4.5 exclusivos, 6.4.8 ECC); §7.4 prefetcher L2; §14.4.4 MVFR1 |
| [CNC] | Chips and Cheese, "Cortex A57, Nintendo Switch's CPU", 12-dic-2023. https://chipsandcheese.com/p/cortex-a57-nintendo-switchs-cpu (medido en una Switch con Linux, A57 a 1,78 GHz) | Introduction; Overview; Branch Prediction; Fetch and Decode; Out of Order Execution Engine; Unified Register File; Schedulers; Floating Point and Vector Execution; Load/Store; Cache and Memory Latency; Bandwidth; Some Light Benchmarking; Final Words |
| [7CPU] | 7-cpu.com, "ARM Cortex-A57". https://www.7-cpu.com/cpu/Cortex-A57.html (AMD Opteron A1170, 2 GHz, **no** la Switch: L2 de 1 MB, con L3) | latencias, TLB (páginas de 64 KB), fallo de predicción |
| [AT] | AnandTech, "The Samsung Galaxy Note 4 Exynos Review", p. 5 "Cortex A57 - Architecture". https://www.anandtech.com/show/8718/the-samsung-galaxy-note-4-exynos-review/5 — el sitio ya no responde; dato tomado del extracto del buscador (tabla A15/A57: 15 ciclos de penalización, con "?"). Fuente secundaria | tabla comparativa |
| [LLVM] | LLVM, `AArch64SchedA57.td`. https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AArch64/AArch64SchedA57.td | `IssueWidth=3`, `MicroOpBufferSize=128`, `MispredictPenalty=14`, `WriteBarrier` |
| [SB-PTM] | Switchbrew, "PTM services". https://switchbrew.org/wiki/PTM_services | §PerformanceConfiguration; CpuBoostMode |
| [SB-TX1] | Switchbrew, "Tegra X1". https://switchbrew.org/wiki/Tegra_X1 | cachés por clúster; Memory Frequency |
| [SB-HW] | Switchbrew, "Hardware". https://switchbrew.org/wiki/Hardware | Specifications (SoC, RAM) |
| [SYSCLK] | retronx-team/sys-clk README. https://github.com/retronx-team/sys-clk | tabla de frecuencias |
| [HBL] | switchbrew/nx-hbloader `hbl.json`. https://github.com/switchbrew/nx-hbloader/blob/master/hbl.json | `default_cpu_id`, `lowest/highest_cpu_id`, `service_access` |
| [LIBNX] | switchbrew/libnx, cabeceras `arm/counter.h`, `kernel/thread.h`, `services/apm.h`, `services/clkrst.h`, `services/pcv.h`. https://github.com/switchbrew/libnx/tree/master/nx/include/switch (y copia local en `/opt/devkitpro/libnx/include/switch/`) | funciones citadas |
| [JETSON] | `/proc/cpuinfo` de un Jetson Nano (Tegra X1). https://gist.github.com/kitsook/87f473503b42f313862c843d0798c35d | CPU variant 0x1, part 0xd07, revision 1 |
| [GCC] | GCC, "AArch64 Options". https://gcc.gnu.org/onlinedocs/gcc/AArch64-Options.html | `-mfix-cortex-a53-*`, `-moutline-atomics`, `-mcpu`/`-mtune`, `-mstrict-align`, `-mlow-precision-*` |
| [DR1] | É. Martin-Dorel, G. Melquiond, J.-M. Muller, "Some issues related to double roundings", BIT 2013. https://core.ac.uk/download/52311178.pdf | condiciones de Figueroa (p' ≥ p+1 / p / p+2) |
| [DR2] | P. Roux, "Innocuous Double Rounding of Basic Arithmetic Operations", JFR 2014. https://jfr.unibo.it/article/view/4359 | doble redondeo inocuo |
| [BENCH] | Medición propia en la Switch del usuario, `docs/switch/BENCHMARK_SWITCH_REAL.md` (8-nov-2026; código en `tools/nxbench/source/main.cpp`; datos crudos en `switch/real_logs/nxbench_1108/`) | todas las filas marcadas BENCH |
| [LOCAL-CG] | Experimentos de generación de código con devkitA64 GCC 16.1.0 (`aarch64-none-elf-g++ -march=armv8-a+crc+crypto -mtune=cortex-a57 -O2 [-ffp-contract=off] -fPIE/-fPIC -S`), 8-nov-2026 | GOT/visibilidad, `__restrict`, `thread_local`, `std::fma`, `sqrt`+errno, frame pointer, errata A53, atómicos, alineaciones por defecto |
| [LOCAL-ELF] | Desensamblado de `switch/nsmbw/build/nsmbw.elf` (build del 31-oct-2026) con `aarch64-none-elf-objdump/nm/size` | tamaños de sección y de funciones, `func_800B0EA0` |
| [LOCAL-LIBNX] | `objdump -d /opt/devkitpro/libnx/lib/libnx.a`: `__aarch64_read_tp` = `mrs x0, tpidrro_el0; ldr x0, [x0, #504]; ret` | TLS |
| [LOCAL-LIBC] | `objdump -d /opt/devkitpro/devkitA64/aarch64-none-elf/lib/libc.a`: `memset` usa `dc zva`; `memcpy` no usa `ldnp/stnp` | memset/memcpy |
