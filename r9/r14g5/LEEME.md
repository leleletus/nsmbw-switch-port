# R14 G5 — registros de la Wii en variables locales (sobre r13)

> **Resumen (8-nov 22:30).** NRO para la consola: `bin/nsmbw_r14g5_v3.nro` (+ `.elf`, sha256 d7a8d03f…),
> verificado en PC (comparador por función, corridas con guion). v1 (`bin/nsmbw_r14g5_v1.nro`) ya se midió en la
> consola: código generado −5,2 % del hilo del juego. v3 = v1 sin las funciones que empeoraban + hojas
> inlineadas + prioridad AX 0x2C. Para retomar: §3 y los comandos de v3 en "Estado".

8-nov-2026. Frente G5 de `docs/switch/README.md`. Base: `r9/r13` (fastmem + render worker + GPU XF). No se
tocó r13 ni otras carpetas de r9, recomp/, NWiiRecomp/, switch/, la NAND ni la consola. Sin commits.

## Estado (se actualiza sobre la marcha)

- [x] Medida previa con unidades sueltas (devkitA64 GCC, `-O2`): **con fastmem, GCC sí deja las locales en
      registros** (ver §1).
- [x] Pasada `tools/apply_g5_regs.py` aplicada a `export/` (39 994 funciones transformadas, 1 saltada, 29 sin
      registros; 8 406 sondeos partidos; los 442 bucles `lmw`/`stmw`/`mfcr`/`mtcrf` desenrollados).
- [x] Compilación completa PC (`bin/NSMBW_r14g5*`) y Switch (`bin/nsmbw_r14g5.nro` + `.elf`), 18:52-18:59.
      Tabla de fastmem de las dos: 0 entradas mal formadas (`tools/check_fm_tab.py`). Texto Switch 88,6 MB
      (r13 ≈ 86): +3 %.
- [x] **Fallo encontrado y arreglado (18:57)**: la recarga antes de un `goto` en `if (<llamada con ctx>) goto X;`
      sin llaves (solo existe en `func_801A3880`, la ruta nativa de la reverb AXFX) dejaba el `goto` fuera del
      `if` → siempre saltaba. Lo vi porque GCC borró 27 accesos de esa función (sitios de fastmem sin entrada
      en la tabla: 63 frente a 10 en r13; los otros 26 son código muerto real, p. ej. `cmpw r5,r5; blt`).
      El comparador también lo detecta (`DIFF func_801A3880` con 2 de 3 semillas). Ahora va entre llaves.
- [x] **Comparador diferencial por función** (`difftest/`, adaptado del de R8): A = objetos PC de r13, B = de
      r14g5, enlazados juntos (`A_*`/`B_*`). 40 031 funciones × 2 semillas = 80 062 ejecuciones: **0
      diferencias reales**; 1 DIFF = `func_801A3880`, el fallo de arriba (eran los objetos de antes del
      arreglo; con el arreglo, las 369 funciones de `output_24` × 2 semillas: 738/738 iguales); 2
      `NANSIGN` (solo el signo de un NaN, por el orden de operandos que elige GCC en x86; igual que en R8); 2
      no concluyentes (las dos versiones revientan igual). Resultados: `difftest/runs/`.
- [x] **Pruebas de CPU** (`cpu/tests`, copia de las de R8 en `cputest/`): 288/288, salida idéntica con y
      sin la pasada.
- [x] **Corridas con guion en PC** (`tools/verify_g5.sh`, resúmenes en `runs/`): `sanity` (título → mapa →
      1-1, 165 s), `title` (título quieto 200 s: demos del título con 4 jugadores y la fiesta de Peach,
      MOVIE, 5 vueltas), `cb4` (Coin Battle 4J, 200 s), `long` (300 s, mapa ↔ niveles 4 veces): todas OK,
      mismas escenas que r13, `[FASTMEM]` 0 fallos, `resume_pruned` solo en las direcciones de siempre
      (0x801b119c, 0x801d0c10), sin cuelgues.
- [x] A/B en PC con perfil débil (`tools/ab_g5.sh`, ABBA, `e2`; `runs/ab_e2.txt`, solo título y 1-1, cortado
      después): **en x86 G5 no gana, pierde un poco** (1-1: hilo CPU 24,7/26,4 % → 28,3/29,9 % a ~60 fps;
      título: 22,5/25,3 → 23,8/25,9 %). Esperado: x86-64 tiene 15 registros y las locales van a la pila; el
      objetivo es AArch64 (31 registros). Para builds de PC se puede seguir usando el `export/` de r13.
- [x] **Consola (v1, 19:15, coordinador)**: `switch/real_logs/bench_r14g5_1108_1915/` frente a r13b: código
      generado −5,2 % de muestras del hilo del juego, ventanas fijas a 60 fps 73-74 % → 69-71 % de hilo; media
      49,2 fps (r13b 48,0), mínimo igual (34,4). Sin cuelgues.
- [x] Análisis función por función (§4) → **v2**: lo mismo que v1 pero dejando como en r13 las 2 463 funciones
      cuyo código AArch64 crecía > 1,2× (`g5_exclude.txt`), + prioridad del hilo AX 0x2C (de r9/r14r5),
      + entrega del write-gather solo en BP de sincronía, **opcional y apagada** (`NWII_WG_BPFLUSH=sync`;
      el tema es del frente W, r9/r16w; no se sigue aquí).
- [x] **v3** (compilado 21:45 tras la caída de la sesión de las 19:52, todo con `scripts/lim`, JOBS=2;
      `bin/nsmbw_r14g5_v3.nro` + `.elf`, tabla fastmem 0 mal formadas; corridas `sanity`/`title`/`cb4` en PC
      OK, `runs/v3/`; **comparador v3: 40 031 funciones, semilla nueva (3): 40 030 iguales, 0 diferencias,
      1 no concluyente**, `difftest/runs/v3/`): v2 + **funciones hoja inlineadas** (`--inline-leaves`): las 326 funciones
      generadas que son solo cargas/guardados entre GPR + `inst_count` + `blr` (sobre todo `_savegpr_N` /
      `_restgpr_N` de los prólogos/epílogos, y getters triviales) se copian en el llamador en vez de
      llamarlas (3 913 llamadas): mismas sentencias en el mismo orden, luego `ctx.pc = <retorno>` (lo que
      deja el `blr`). Así los registros del llamador no se vuelcan/recargan alrededor. Aplicar:
      `python3 tools/apply_g5_regs.py export --orig ../r13/export --eager --entry-fns --exclude g5_exclude.txt
      --inline-leaves --leaves-from ../r13/export`.

## 1. Medidas previas (antes de compilar todo)

`tools/extract_tu.py` saca funciones calientes a una unidad pequeña (`exp/hot_base.cpp`), `tools/tu_nx.sh` la
compila como la build de Switch y `tools/fnstats.sh` cuenta por función: instrucciones, cargas/guardados
relativos al registro de `ctx`, a la pila (`sp`), accesos al juego (`[x, w, uxtw]`) y llamadas.

| función (AArch64) | r13 | R8 reglocals sobre r13 | **G5 (esta pasada)** |
|---|---|---|---|
| `func_801A5C10` (delay AXFX) | 752 ins, ctx 83 ld / 141 st | 696 ins, ctx 55/44, pila 60/46 | **570 ins, ctx 68/48, pila 37/36** |
| `func_801A2B10` | 1106 ins, ctx 100/205 | 1064, ctx 113/136 | 1055, ctx 154/148 |
| `func_802821E0` | 10 993 ins, ctx 1197/731 | 10 928, ctx 907/544 | 10 512, ctx 643/499 |
| `PSMTXConcat` | 1797 ins, ctx 59/18 | 1791 | 1823, ctx 40/15 |
| `PSMTXCopy` | 785 ins | 785 | 830 |

- El bucle interno de `func_801A5C10` (36 instrucciones PPC con sondeo al final) queda **sin ningún acceso a
  `ctx`**: registros en registros, accesos al juego en línea, el sondeo solo lee `poll_left`/`msr`/banderas.
  Lo que antes eran 2 operaciones encadenadas a través de `ctx` (15 ciclos medidos en la consola) ahora es
  registro a registro.
- Los recuentos estáticos engañan: lo que más queda en `ctx` son los volcados/recargas alrededor de las
  llamadas al juego (cada llamada vuelca lo sucio y recarga lo vivo), que se ejecutan una vez por llamada,
  no por vuelta de bucle. Unidad entera `output_24` (Switch): texto 727 439 → 724 535 bytes; cargas de
  `ctx` 18 912 → 16 783, guardados 19 749 → 16 125 (estático).
- **FPR en locales (`--fpr`, probado, NO se usa)**: expandir los ayudantes de coma flotante sobre locales
  hace el código peor (`func_802821E0` 10,5 k → 15,2 k instrucciones, 2 000 cargas de pila más): los caminos
  lentos de `psq_l`/`psq_st` (llamada fuera de línea si el GQR no es float) y las demás llamadas obligan a
  guardar los dobles vivos en la pila (solo d8-d15 sobreviven a una llamada). Se queda en la pasada como
  opción para retomarlo (habría que quitar antes esas llamadas de los caminos rápidos).
- Operando de memoria del asm de fastmem (`NWII_FM_OPND`, cabecera): r13 le pasaba a GCC los bytes exactos
  (`*(FmB4*)(fb + ea)`), así que GCC calculaba `fb + ea` en un registro en cada acceso aunque la instrucción
  no lo usa. Ahora (modo 1) el operando es la ventana entera de 4 GiB (`may_alias`, mismo efecto de orden
  para GCC, sin cálculo extra). El modo 2 (tipo propio, sin alias con `ctx`) no ganó nada medible y es más
  arriesgado: no se usa.
- XER: el bit CA va en su propia local (`G5ca`); el resto en `G5xer`. Cada `srawi`/`addc`/`subfic` escribía
  CA con lectura-modificación-escritura de todo XER y GCC no podía quitar las escrituras muertas; separado,
  las elimina (en `func_801A5C10` −70 instrucciones).

## 2. La pasada (`tools/apply_g5_regs.py`)

Corre después de la de fastmem (marca `// [r14-g5]` en la primera línea; idempotente; un archivo sin marca
tiene que tener el sha256 del mismo archivo de `r9/r13/export`, si no, se niega). Extiende la de R8
(`r8/codegen/tools/apply_reglocals.py`, ya verificada con su comparador):

- Locales: `ctx.gpr[N]` → `G5rN`, `ctx.cr[N].lt/gt/eq/so` → `G5cNlt`..., XER → `G5xer` + `G5ca`,
  `ctx.ctr` → `G5ctr`, `ctx.inst_count` → `G5ic`. No van a locales: pc, lr, msr, srr0/1, gqr, fpscr, FPR.
- Se cargan todas al entrar (antes del `switch` de entrada: `ctx` es exacto en cualquier entrada; GCC
  quita las cargas muertas).
- Volcado (`ctx.X = local`) de lo que puede estar sucio (análisis hacia delante sobre el grafo de `goto`)
  antes de todo lo que puede mirar `ctx`: llamadas al juego, `nwii_dispatch`, `interpret_step`,
  `resume_pruned`, `micro_interpret`, `idle_wait`, HLE, miembros de `ctx` desconocidos, bloques con índice
  variable (`lmw`/`stmw`/`mfcr`/`mtcrf`), retornos. Recarga después de lo que está vivo (vivacidad hacia
  atrás; un volcado cuenta como uso, así un volcado posterior nunca escribe una local vieja).
- **Sondeo partido**: `if (process_pending_callbacks(ctx)) return;` →
  `if (NWII_UNLIKELY(poll_due(ctx))) { <volcado> if (process_pending_callbacks_slow(ctx)) return; <recarga> }`.
  `poll_due` (cabecera) es la parte en línea de antes (cuenta atrás, MSR[EE], banderas) y no lee ningún
  registro del juego; `process_pending_callbacks(ctx)` sigue existiendo como `poll_due && slow` (mismo
  comportamiento para el código no transformado).
- Efectos parciales: llamadas MMIO (`mv_.X_io`, `psq_*_io`) y `mftb`/`mfdec`/`mtdec` leen `inst_count`
  (reloj virtual): se vuelca `G5ic` antes si está sucia. `fcmp`/`ps_cmp*` se expanden en línea (escriben CR).
  `psq_l_fast`/`psq_st_fast` solo tocan fpr/ps1/gqr.
- Bucles `lmw`/`stmw`/`mfcr`/`mtcrf` (`for (int i = A; i < B; ++i) {...}` con `ctx.gpr[i]`/`ctx.cr[i]`):
  se desenrollan (un `{...}` por `i`, mismas sentencias en el mismo orden, `i` constante), así los
  registros que tocan siguen en locales en vez de volcar/recargar todo alrededor (`--no-unroll` lo apaga).
- Lo que no reconoce → la función queda igual (1 en todo el juego: `btm_return_link_keys_evt`).

## 3. Cómo retomar

- Reaplicar desde cero: copiar `r9/r13/export/*` (sin `build/`) a `export/` y
  `python3 tools/apply_g5_regs.py export --orig ../r13/export` (~2 min). La cabecera de r14g5
  (`nWiiRuntime/include/runtime/cpu_context.h`) lleva `poll_due` y `NWII_FM_OPND`.
- Experimentos rápidos: `python3 tools/extract_tu.py export exp/x.cpp func_801A5C10 ...`,
  `python3 tools/apply_g5_regs.py --file in.cpp out.cpp`, `tools/tu_nx.sh out.cpp out.o`,
  `tools/fnstats.sh out.o`.
- Compilar: `FULL=1 JOBS=2 ~/nsmbw-port/scripts/lim ./build_pc.sh bin/NSMBW_r14g5` y
  `FULL=1 JOBS=2 ~/nsmbw-port/scripts/lim ./build_nx.sh bin/nsmbw_r14g5.nro`.

## 4. Por qué la ganancia en la consola fue solo −5 % (análisis de v1)

Perfiles simbolizados de r13b y v1 (mismo guion: demo del título 150 s), muestras por función del juego
frente a recuentos estáticos de los dos `.elf` / objetos (`tools/fn_ins.sh`, `exp/sel/`):

- Mejoran mucho las funciones con bucles y pocas llamadas: `func_801A5C10` −37 %, `func_802631F0` −55 %,
  `func_802821E0` −30 %, `func_801C9A80`/`func_80162C30`/`func_8024C2B0` −25 %, `DCInvalidateRange` −21 %,
  `strcmp` −18 %.
- Empeoran las funciones con **salto computado local** (`nwii_entry`: cada instrucción es un punto de
  reanudación, 10 774 funciones): `func_8025DB30` +37 %, `func_80250710` +25 %, `GXLoadTexObjPreLoaded` +22 %,
  `func_80250E70` +15 %: ninguna local puede quedarse en un registro en ese grafo y GCC las manda a la pila
  (cargas/guardados de pila 8 → 348 en `GXLoadTexObjPreLoaded`, código 576 → 1083 instrucciones). También
  algunas con muchas llamadas (`func_80240240` +27 %) y `PSMTXConcat` +20 % (los caminos lentos de `psq_*`
  son llamadas que obligan a guardar registros).
- Agrupando las funciones por crecimiento del código (v1/r13) y sumando sus muestras: < 0,9× → −22 %,
  0,9-1,0× → −9 %, 1,0-1,05× → −4 %, 1,05-1,2× → −2/−3 %, > 1,2× → +1 %. Por eso v2 excluye solo > 1,2×:
  la ganancia extra esperada es pequeña (~1 % del código generado).
- La razón de fondo: el código generado es muy difuso (la función más caliente es < 1 %) y el hilo del juego
  pasa ~47 % dormido en esta prueba; el −5 % del código generado es ~1,7 % del hilo. En la demo del desierto
  el hilo trabaja ~65-80 %: ahí pesan también el write-gather de GX (~6,5 %, frente W) y las esperas de
  candados (~4 %).
- **Recarga perezosa (`LAZY`, implementada en la pasada, NO usada)**: recargar justo antes del primer uso en vez
  de justo después de cada llamada, con recargas en las aristas que entran a una etiqueta. En los recuentos
  estáticos no mejora a v1 (las funciones que empeoraban eran las `nwii_entry`, no las recargas); sin forma
  de medirlo dinámicamente en AArch64 aquí, se dejó apagada. `--eager` (por defecto en v2) = v1.
- FPR en locales: peor (ver §1).

Siguientes palancas para el hilo del juego (no hechas): inlinear `_savegpr_N`/`_restgpr_N` (llamadas del
juego en casi todos los prólogos/epílogos; `func_802DCD74`, `func_802DCBFC`... ~1 % y obligan a volcar/recargar
alrededor); caminos lentos de `psq_l/psq_st` sin llamada normal (preservando registros); write-gather de GX
(frente W).

## 5. Avisos

- `cpu_context.h` de r13 (y de esta copia): la rama sin fastmem de `psq_st_fast` tiene duplicado
  `ctx.psq_store(S, ea, W, I); }` al final del `#else`: con `-DNWII_FASTMEM=0` no compila (r12fm decía que sí).
  Solo afecta a builds sin fastmem (el único que se distribuye lleva fastmem).
- Los `.nro` de prueba: `bin/nsmbw_r14g5_v1.nro` (+ `.elf`, lo que se probó en la consola a las 19:15) y
  `bin/nsmbw_r14g5.nro` (+ `.elf`) = v2.
- `apply_g5_regs.py` en modo carpeta usa `multiprocessing` con `fork` (con `forkserver`, el predeterminado de
  Python 3.14, los procesos hijos no veían las opciones).
