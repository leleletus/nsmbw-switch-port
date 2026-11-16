# r33g7: frente G7 — compactar el código generado caliente (14-nov-2026)

Base: export de G6c3 (`r9/r30g6c3/c3/export`, `--proto 3`) + runtime r31t. Línea base A/B: **m3n**
(`r9/r31t/bin/nsmbw_m3n.nro/.elf`). Sin commits. No se toca `r30g6c3`, `r31t`, `r32dx`, `r17pgo` (solo lectura),
ni `nand/`, ni la SD. Todo lo pesado de esta laptop por `scripts/lim`; compilaciones completas en la otra laptop.

> **Estado: TERMINADO (15-nov 05:10)**: ver §5 y el final de la bitácora. Recomendado: d (`bin/nsmbw_g7dn2.nro`; con PGO `r17pgo/bin/g7d_pgo_g7d_all.nro`).

## 1. Qué dicen los datos antes de tocar nada

Herramientas nuevas (todas `python3 -I` o bash, sin IA) en `tools/`:
- `symhist.py <elf> <profile_cpu.txt>`: muestras del hilo del juego por símbolo del runtime.
- `symdis.py <elf> <profile> <símbolo>`: desensamblado con muestras por instrucción.
- `dx_hdrlines.py <escena>`: con los datos exactos de DX (`r32dx`, m2/e2), instrucciones AArch64 ejecutadas por
  línea de cabecera (qué ayudante en línea pesa cuánto).
- `dx_static_cls.py <escena>`: bytes de código, en las funciones que se ejecutan, por clase de sentencia,
  ejecutados / no ejecutados.
- `try1.sh <N> <etiqueta> <feats>`: compila una sola unidad `output_N.cpp` de c3 con unas transformaciones y
  da su tamaño (iteración rápida, ~25 s).

Resultados (desierto4j, m2/e2 con las cuentas de DX; 10,5 M de AArch64 por fotograma):

| Código en línea de cabeceras | AArch64/fot | % |
|---|---|---|
| `read32` (carga + `rev`) | 702 k | 6,7 |
| `write32` | 395 k | 3,8 |
| **sondeo en línea** (`poll_due`: 5 cargas atómicas + cuenta atrás) | **855 k** | **8,1** |
| **psq_l / psq_st + `ppc_single_bits_ftz`** | ~500 k | 4,8 |
| `fill` (fpr + ps1) | 156 k | 1,5 |
| `ppc_single_bits` (stfs) | 110 k | 1,0 |
| `ppc_force25` (fmuls) | ~120 k | 1,1 |

Tamaño: de los 6,6 MB de las funciones que se ejecutan en el desierto solo se ejecutan 3,5 MB. De los 3,1 MB
que no se ejecutan, **1,8 MB (58 %) es código de cabeceras integrado**: la rama de denormales de `stfs`
(`ppc_single_bits`, ~410 KB), el camino general de `psq_load/psq_store` integrado en cada sitio (~300 KB), la
rama de denormales de `Force25Bit`, etc.

Perfil de la consola de m3n (U01), hilo del juego, runtime: `psq_store`/`psq_load` (lambdas, camino general)
2 577 muestras = 1,6 % del tiempo ocupado: son las conversiones entero↔float de MWCC
(`psq_l f,d(r),1,3` = u16→float: 738 sitios con GQR 3). Sondeo lento + reloj: 4,4 %. Escritura GX: ~6 %.

## 2. Transformaciones (pasada `tools/apply_g7.py` + cabecera `tools/g7.h`)

`apply_g7.py <export> --feat a,b,c` reescribe llamadas del código generado a ayudantes de `g7.h` (cabecera que
solo incluye el código generado; **el runtime y su `cpu_context.h` no cambian** con stfs/psq/fp). Cada "feat"
se puede quitar para aislar su efecto:

| feat | Qué cambia | Camino caliente (AArch64) | Exactitud |
|---|---|---|---|
| `stfs` | `ppc_to_single(d)` → `g7::to_single(d)`: `f=(float)d; if ((double)f==d) return f;` y lo demás al original por trampolín | 12 → 8 instrucciones por `stfs`; sitio frío 25 → 3 | si `d` es representable en single, seleccionar bits = convertir (incluye ±0, ±inf, denormales de single); NaN y no representables van al original |
| `psq` | `psq_l_fast/psq_st_fast` → `g7::psq_l/psq_st`: mismo camino rápido float (FTZ sin saltos); camino general en UNA función compartida fuera de línea (`psq_l_gen/psq_st_gen`, sin lambdas) | sitio de 270 → ~110 bytes; el camino general (conversiones entero↔float) más corto | mismas lecturas/escrituras por `ctx.mmu`, mismo orden y conversiones |
| `fp` | `ctx.fmuls/fmadds…/ps_mul…/ps_madd…` → `g7::…`: `Force25Bit` rápido (28 bits bajos a cero → no cambia), empate del fma por trampolín; `ps_madd*` fuera de línea compartidos | fmuls: 9 → 5 instrucciones de Force25 | misma expresión; la rama rara llama a la función original |
| `io` | `mv_.write*_io` → llamada directa a `GX_WGPIPE_*` | (no se usa: hace crecer el código, +30 B por sitio, y ahorra poco) | — |
| `poll` | sondeo en línea reducido a la cuenta atrás (necesita runtime con avisos) | ~15 → 4 instrucciones por sondeo | ver §4 |

**Trampolines (solo AArch64):** una llamada normal de C en un camino frío obliga a GCC a salvar los registros
vivos (los G5 viven en registros caller-saved) y a volcar/recargar `ctx`; con funciones frías "normales" el
código CRECÍA (+3,9 % en `output_10` con `stfs`). Los caminos fríos sin memoria (`ppc_single_bits`,
`ppc_force25`, empate del fma) se llaman con `bl g7t_*` desde un `asm` en línea: el trampolín (definido una vez,
en `output_0.cpp` con `G7_IMPL`) guarda x0-x18 y q0-q31, llama a la función de C y restaura todo menos d0.
El sitio frío queda en 2-3 instrucciones. En PC (x86) son llamadas normales a las mismas funciones de C.
Como los trampolines solo existen en la consola, **`g7.h` lleva una autoprueba que corre al arrancar**
(constructor estático en `output_0.cpp`): compara cada ayudante con el original en 32 valores fríos y comprueba
con `g7t_regcheck_g6` que los trampolines conservan todos los registros; si falla, `abort()` (la NRO no arranca).

Verificación de los ayudantes: `cputest/g7_unit.cpp` (PC): 169 M de comprobaciones bit a bit contra los
originales (todos los singles ensanchados, dobles al azar, denormales, NaN, empates de fma, todos los tipos y
escalas de GQR en RAM/MEM2/tubería/MMIO/sin mapear): 0 fallos.

### Variantes

| Variante | feats | Runtime | Objetos |
|---|---|---|---|
| **a** | stfs, psq, fp | r31t (sin cambios) → A/B exacto con m3n | `a/{nx_obj,pc_obj}` |
| **b** | a + poll + icm | **r33g7** (avisos del sondeo, §4) | `b/{nx_obj,pc_obj}` |
| **bn** | b compilada con `-DG7_NOIC` (solo Switch; mismo export que b) | r33g7 | `bn/nx_obj` |
| **c** | b + disp | r33g7 | `c/{nx_obj,pc_obj}` |
| **cn** | c con `-DG7_NOIC` (solo Switch) | r33g7 | `cn/nx_obj` |
| **d** | c + cr | r33g7 | `d/{nx_obj,pc_obj}` |
| **dn** | d con `-DG7_NOIC` (solo Switch) | r33g7 | `dn/nx_obj` |

- `icm` + `-DG7_NOIC`: la suma `G5ic += N` de cada bloque básico pasa por la macro `G7IC(N)`. Sin el define es
  idéntica (PC, comparador, pruebas). Con `-DG7_NOIC` (solo objetos de Switch) desaparece: en la consola el reloj
  es el real (`NWII_REALTIME`) y `ctx.inst_count` solo se usa para el latido del registro y las estadísticas
  (`grep inst_count nWiiRuntime/src`: `read_timebase()` solo lo devuelve con el reloj virtual). Para que el
  contador siga avanzando si alguien arrancara con el reloj virtual, suma 32 en cada sondeo lento. Un `add`
  menos por bloque básico (~3-4 % de las instrucciones ejecutadas).
- `disp`: los `bctrl` (llamadas indirectas, 20 000 sitios, 6-10 mil por fotograma) pasan de
  "volcar lr/pc/inst_count + `nwii_dispatch` en línea + envoltorio + `switch` de entrada + `g6_slowret` +
  comparar `ctx.pc`" a una llamada a `g7_bctrl_g6(ctx, ic, R, pc)`: tabla hash dirección → cuerpo `_g6`
  (montada al primer uso desde `g7_disp.inc`, solo con las entradas que la tabla de despacho real lleva a esa
  misma función) y llamada con el protocolo directo de c3; al volver deja `ctx.inst_count/pc/lr` como los dejaba
  `g6_slowret`. Si el destino no es la entrada de una función `_g6` (o no hay pila nativa, o ctr = 0) hace la
  sentencia de c3 tal cual. ~55 → ~40 instrucciones por llamada indirecta y cada sitio 15 instrucciones más corto.

- `cr` (campos de CR perezosos y empaquetados): c3 guarda cada campo CR en 4 `bool`; cada comparación calcula los
  cuatro aunque solo se use uno en el salto (los otros solo se leen en los volcados, casi siempre fríos: camino
  lento del sondeo, salidas `g6_exact`), cada volcado son 4 `strb` y cada recarga 4 `ldrb`. Ahora, por campo con
  sus 4 bits declarados en la función, 4 locales: modo comparación (`G5cNm = 0`: guarda los dos operandos
  extendidos a 64 bits con o sin signo y una copia de XER; lt/gt/eq/so se calculan donde se usan) o modo palabra
  (`G5cNm = 1`: los 4 bytes de `ctx.cr[N]` tal cual). Las 63 620 + 13 574 + 7 687 comparaciones enteras
  (`cmpw`/`cmplw`/formas Rc) pasan a `G7CR_CMPS/CMPU/CMPS3` (no calculan nada); los 82 970 volcados de un campo
  entero a UNA palabra (`G7CR_STORE`), las 33 896 recargas a UNA carga (`G7CR_LOAD`); `fcmp` → `G7CR_SET4`;
  `cror` → `G7CR_SET`; toda lectura → `G7CRB(N, bit)`. Los campos con solo algunos bits declarados (cr1.eq de las
  llamadas con argumentos variables) se dejan como están. Ejemplo (`func_80162D00`, búsqueda en lista, 615
  llamadas por fotograma): el bucle pasa de 21 a 16 instrucciones por vuelta. Una recarga parcial (solo los bits
  vivos) se convierte en recarga de la palabra entera: los bits no recargados están muertos en ese punto (la pasada
  G5 solo recarga tras un escape, que deja todo volcado, o tras un ayudante que reescribe el campo entero).

## 4. Sondeo compacto (feat `poll`) y los cambios del runtime (`nWiiRuntime/`, copia de r31t)

Antes, cada sondeo en línea (53-128 mil por fotograma) hacía: cuenta atrás, `MSR[EE]`, `poll_dec_due`,
`PI_INTSR & PI_INTMR`, `pending_cb_count` (5 cargas, ~15 instrucciones: 8 % de todo lo ejecutado). Ahora
`g7::poll_due` es solo la cuenta atrás (`ldr; subs; str; b.le`, con `asm goto` en AArch64). Para que el sondeo
lento corra en los mismos sondeos que antes, quien puede volver cierta la condición fuerza el sondeo siguiente
(`poll_kick`):

| Cambio en el runtime | Archivo |
|---|---|
| `trigger_pi_interrupt` → + `poll_kick_global()` | `include/runtime/hw/hw.h` |
| escribir `PI_INTMR` → + `poll_kick_global()` | `src/hw/hw_pi.cpp` |
| `queue_callback` → + `poll_kick()` | `include/runtime/cpu_context.h` (única línea que difiere del de r14g5/r31t; el código generado no la usa) |
| `mtmsr` del intérprete y carga de contexto → `poll_ee_kick` (si EE queda encendido con algo pendiente) | `src/hle/interpreter.cpp`, `src/hle/ios.cpp` |
| salida del sondeo lento con la condición aún cierta → se fuerza el siguiente guardando la cuenta atrás (`poll_persist_exit`, `s_g7_shadow`): igual que antes, camino lento en cada sondeo mientras dure, y el reloj se refresca cada 16 | `src/hle/ios.cpp` |

No hace falta nada más: los `mtmsr`/`rfi` del código generado ya hacen un sondeo completo al encender EE
(`process_pending_callbacks`, con la comprobación antigua, que no se toca), y `run_game` sondea con la
comprobación antigua en cada vuelta (cualquier `rfi`, cambio de contexto o retorno de callback pasa por ahí).
Con el reloj virtual (PC, `checks.sh`) todos los sondeos van por el camino lento (`k = 1`), así que el
comportamiento es idéntico por construcción. Con el reloj real, la única diferencia posible: si el aviso de
otro hilo se pisa con la cuenta atrás (carga-resta-almacén no atómico), esa interrupción se atiende en el
siguiente sondeo lento natural (≤ 16 sondeos, ~5 µs). Parche: `diff -ru ../r31t/nWiiRuntime nWiiRuntime`.

## 3. Cómo rehacerlo

```
cd ~/nsmbw-port/r9/r33g7
V=a; FEATS=stfs,psq,fp
mkdir -p $V && cp -a --reflink=auto ../r30g6c3/c3/export $V/export && rm -rf $V/export/build
~/nsmbw-port/scripts/lim python3 tools/apply_g7.py $V/export --feat $FEATS --jobs 4 > $V/pass.log
# pruebas de CPU y de los ayudantes (PC)
cd cputest && python3 ../tools/apply_g7.py --file gen_tests_g6c3.inc gen_tests_g7$V.inc --feat $FEATS
~/nsmbw-port/scripts/lim g++ -std=c++20 -O2 -I../../r14g5/nWiiRuntime/include -I. -I../tools -DGEN_INC="\"gen_tests_g7$V.inc\"" \
  test.cpp stub_gov.cpp ../../r14g5/nWiiRuntime/src/hle/interpreter.cpp -o test_g7$V && ./test_g7$V | grep -v '^\[G7\]' | cmp - g6.out
~/nsmbw-port/scripts/lim g++ -std=c++20 -O2 -I../../r14g5/nWiiRuntime/include -I../tools g7_unit.cpp stub_gov.cpp -o g7_unit && ./g7_unit
cd ..
# objetos en la otra laptop
~/nsmbw-port/scripts/remoto.sh push r9/r33g7/tools r9/r33g7/$V
~/nsmbw-port/scripts/remoto.sh run g7$V "JOBS=11 r9/r33g7/tools/rbuild.sh $V both"
```

## Bitácora

- 21:35 variante **a** (stfs,psq,fp) compilada en la otra laptop. cputest 288/288 (salida idéntica salvo la línea
  `[G7] autoprueba ok`), g7_unit 0 fallos. NRO `bin/nsmbw_g7a.nro` (.text 75,58 → 71,53 MB; tabla fastmem: 0 mal
  formadas, 69 sin entrada = c3). En cola de la consola: G01_g7a_lista, G02_m3n_lista (control).
- 22:00 a: `checks.sh` 1x title/map/walk/lvl11 idénticas (220/100/80/220, diff_lines=0), sanity OK (59,4 fps,
  FASTMEM 0). Comparador (aquí, 5 procesos) semilla 1: 40 024 iguales, 0 diferencias, 6 "solo signo de NaN"
  (funciones con `g7::fmuls` o que llaman a una: con dos operandos NaN el NaN que sale de la multiplicación del
  anfitrión depende del orden en que el compilador pone los operandos; el comparador ya lo clasifica aparte), 1 no
  concluyente (como c3).
- 22:05 b (poll+icm), bn, c (disp), cn: exports hechos, cputest 288/288 idénticos; compilando en la otra laptop
  (`remoto g7_b`, `g7_c`; hay una cadena de PGO y otro agente compilando allí: JOBS 6-8).
- 22:20 comparador de **a** completo (2 semillas): 40 024 iguales, 0 diferencias, 6 solo-signo-de-NaN, 1 no
  concluyente en las dos. Lista de PC de a: completa, hoja limpia (`runs/lista_g7a/hoja.jpg`). **a verificada.**
- 22:20 NRO `g7b`, `g7bn` enlazadas con el runtime de r33g7 (.text 70,9 / 70,0 MB; fastmem 0 mal formadas).
  En cola: G03_g7bn_lista. Nuevas: `disp` (variante c) y `cr` (variante d), cputest 288/288 idénticos;
  compilando c/cn/d/dn en la otra laptop y comparador de b, c, d.
- Cuentas exactas tipo DX (gcov): `dx/` = copia parametrizada de las herramientas de r32dx (`DXV=dx/<variante>`);
  fuente partido y mapa hechos para c3 y d; objetos gcov/-g1 de c3 en la cadena remota `g7_b`.
- 22:25-22:31 (sin anotar por el agente anterior, reconstruido por el relevo) variante **i** = segunda ronda de
  integración de llamados guiada por DX (`tools/apply_g7_inline.py plan` → `diag/inl_sel.tsv`, 986 sitios de
  ≤ 200 PPC y ≥ 2 llamadas/fot; `apply` sobre una copia de `r20g6/e2/export_pre` → `i/export_pre`; luego la pasada
  G5/G6c de c3 tal cual → `i/export`). Variante **e** = i + feats de d (stfs,psq,fp,poll,icm,disp,cr) → `e/export`.
- 22:45 **relevo** (el agente anterior se cortó por cuota). Comparador de b en curso en la otra laptop; NRO
  `bin/nsmbw_g7dn.nro` enlazada (runtime r33g7; .text 68,49 MB, .data 5,84 MB por la tabla de `disp`; fastmem 0 mal
  formadas, 75 sin entrada = sitios que GCC eliminó: ninguna instrucción sin entrada). En cola: G04_g7dn_lista.
  Remoto: `g7_d2` (objetos PC de d + comparador d), `g7_e` (en = e con -DG7_NOIC para Switch, objetos PC de e,
  comparador e). Aquí: verificación de PC de b (checks, sanity, lista).
- 22:52 comparador de b: semilla 1 = 40 024 iguales, 0 diferencias (6 NaN, 1 no concl.). Se paran en la otra laptop
  las cuentas DX de c3 (`g7_b`, paso robjs) y la variante c (`g7_c`, = d sin `cr`): no son necesarias para elegir y
  hacen falta los hilos para e y para el PGO. Medida estática ponderada (`static_mem.py`): bn 1 574,6 → dn 1 556,5
  instrucciones por función caliente (ctx.cr 35 → 11).
- 22:52 **PGO de d arrancado en paralelo** (para no perder la ventana): `tools/pgo_g7.sh g7d d/export nWiiRuntime
  -DG7_NOIC` (unidad systemd `g7-pgo-g7d`, registro `runs/pgo_g7d.log`): foto `r17pgo/g7dsrc` + `conf/g7d.env`
  (GEN_FLAGS=-DG7_NOIC en instrumentada y optimizada), instrumentada en la otra laptop, entrenamiento Y01/Y02 (solo
  demos, como m3), finish, y medida Y03 (g7d+PGO) / Y04 (m3+PGO) contiguas. Si d no pasa la verificación se cancela.
- 23:05 e NO compilaba (Switch ni PC): el sitio `func_8026B560 → func_801A5C10` integra un cuerpo con el gancho
  `[a2delay]` (funciones `static` de `output_24.cpp`) en `output_36.cpp`. Se quita ese sitio
  (`diag/inl_sel2.tsv`, 985 sitios) y se rehace la cadena con `tools/build_ie.sh i2 e2 diag/inl_sel2.tsv` (2 min):
  i2 = i salvo ese sitio (las demás diferencias son solo renumeración de etiquetas). Comprobado que ningún
  `output_*.cpp` de e2 usa un nombre local de otro archivo. Las carpetas rotas quedan como `i_roto/`, `e_roto/`.
  En la otra laptop: `g7_e2` (en2 = e2 con -DG7_NOIC, objetos PC de e2, comparador e2).
- 23:08 b: checks 1x title/map/walk/lvl11 idénticas (220/220 cada una, diff_lines=0), PINDEX/DLMEMO 0 errores,
  FASTMEM 0; comparador 2 semillas 0 diferencias. **b verificada.**
- Probado y descartado (estático, output_26/10): `-DNWII_FM_OPND=2` (operando de memoria sin alias con ctx) no cambia
  `PSMTXConcat` (745 instr.): lo que obliga a recargar `ctx.fpr` son las llamadas fuera de línea `g7::ps_madd*`;
  `-fno-schedule-insns` / `-fsched-pressure`: código idéntico.
- 23:17 consola **G01 (a)**: IMAGEN OK (1/37 raras), capturas.jpg mirada (bien). ms juego/fot REAL contra U01 (m3n
  del 12-nov; el control contiguo G02 va detrás): desierto 19,1→17,7 · agua 24,8→23,3 · cielo 22,3→21,1 ·
  nubes 18,7→17,1 · castillo 13,3→10,8. PGO de g7d: la instrumentada compila con -DG7_NOIC y -fprofile-generate.
- 23:30 **fallo de exactitud en `disp`** (comparador de d, semilla 1: 3 062 funciones con B = SIGSEGV): en
  `g7_bctrl_g6`, con `ctr == 0` una casilla vacía de la tabla hash (addr 0) casaba y se llamaba a un puntero nulo (c3
  hace `note_null_call` y sigue). En una partida normal no pasa (en la Wii un `bctrl` a 0 colgaría el juego), pero no
  es exacto. Arreglo de una línea (sin mover números de línea): `if (t != nullptr && ctr != 0)`. Afecta solo a
  `output_0.cpp` (sección `G7_IMPL`): `tools/g7.h` y las copias de `d/`, `e2/`, `c/` arregladas; `dn/nx_obj/output_0.o`
  recompilado → **`bin/nsmbw_g7dn2.nro/.elf`** (= g7dn arreglada; G04 mide la g7dn vieja: mismo rendimiento).
  Otra laptop: `g7_d3` (output_0 de PC de d + comparador d), `g7_e3` (en2/e2 + comparador e2, rehechos con el arreglo).
  PGO g7d: la instrumentada ya tenía output_0 compilado con el fallo; se arregla `r17pgo/g7dsrc/export/g7.h`
  (SHA256SUMS regenerado, original en SHA256SUMS.prep) y `conf/g7d.env` lleva `-Wno-error=coverage-mismatch`
  (finish ignorará el perfil de esa única función).
- 23:35 consola, contiguas (ms juego/fot REAL; desierto · agua · cielo · nubes · castillo):
  G02 m3n 18,9 · 24,6 · 22,0 · 18,4 · 13,0 | G01 a 17,7 · 23,3 · 21,1 · 17,1 · 10,8 | **G03 bn 17,0 · 22,3 · 20,2 ·
  16,2 · 10,1**. IMAGEN OK las tres, capturas miradas (bien).
- 23:44 **G04 dn** (g7dn vieja, antes del arreglo de ctr==0: mismo rendimiento): 16,7 · 22,1 · 19,7 · 15,9 · 10,1
  (contra G02 m3n: −2,2 · −2,5 · −2,3 · −2,5 · −2,9 ms). IMAGEN OK (3/38 raras: fundidos), capturas miradas.
  disp+cr sobre bn: −0,3 · −0,2 · −0,5 · −0,3 · 0.
- 23:50 tropiezo con el PGO de g7d: al cambiar `g7dsrc/export/g7.h` durante la instrumentada, el paso final de
  ninja (-j1) empezó a recompilar todo; y al añadir `-Wno-error=coverage-mismatch` a GEN_FLAGS cambió la línea de
  compilación (otra recompilación completa). Se para, se quita ese flag (GEN_FLAGS="-DG7_NOIC" como al principio),
  se tocan los .o y se relanza: se recompilan solo main_output (lo borró la parada) y output_10; output_0 ya quedó
  compilado con el g7.h arreglado → perfil y fuente coinciden. ~25 min perdidos.
- 23:47 NRO **`bin/nsmbw_g7en2.nro/.elf`** (e2 = inline + todo lo de d, con el arreglo; .text 70,09 MB, fastmem 0 mal
  formadas, 75 sin entrada). Estático ponderado: dn 1 556 → en2 2 127 instr. por función caliente (+37 %: los
  cuerpos integrados). En cola: G05_g7en2_lista y G06_g7dn2_lista (contiguas; detrás de S08-S11 de otro agente).
- 00:00 comparador de d con el arreglo, semilla 1: 40 024 iguales, 0 diferencias. Verificación de PC de d (checks
  1x idénticas 220/220 las cuatro, PINDEX/DLMEMO 0, FASTMEM 0, sanity 59,7 fps, lista completa).
- Perfil de consola G04 (dn) contra G02 (m3n): `g7::ps_madds1` 1 330 + `ps_madds0` 1 134 + `ps_madd` 643 + msub/nmsub
  440 muestras fuera de línea (en m3n GCC integraba madds0/madds1: solo 258 muestras fuera). Variante **f** = d con
  `-DG7_INL_MADDS` (madds0/madds1 en línea; misma fuente C, solo cambia la integración): estático +22 % en
  output_26/10 (PSMTXConcat 745 → 1 628 instr.), dinámico por medir. Otra laptop: `g7_f` (fn = Switch).
- 00:20 la instrumentada de g7d se está recompilando entera de todos modos (ninja la vio sucia tras el cambio de
  g7.h pese a tocar los .o): queda toda compilada con el g7.h arreglado. Llega hacia las 00:45; luego Y01/Y02.
- 00:27 **G05 en2 (inline 2.ª ronda) PEOR que dn**: 17,6 · 22,8 · 20,1 · 16,7 · 10,3 (dn: 16,7 · 22,1 · 19,7 · 15,9 ·
  10,1). Integrar más llamados hace crecer el código caliente (+37 % estático) y se pierde más en caché de
  instrucciones de lo que se ahorra en llamadas: confirma la tesis de G7 (compactar). **Variante e/e2 descartada**;
  se para su comparador en la otra laptop. IMAGEN OK, capturas miradas.
- 00:40 **AVISO para quien copie d/export (p. ej. m4n de S2, r9/r35m4)**: `d/export/g7.h` se cambió UNA vez, a las
  23:30, con el arreglo de `g7_bctrl_g6` (ctr == 0; ver arriba). md5 actual = el de `r17pgo/g7dsrc/export/g7.h`
  (69085dbd…). Si copiasteis d/export antes de las 23:30, copiad de nuevo `g7.h` (solo cambia output_0). Desde
  entonces d/export no se toca. (`tools/g7.h` maestro sí tiene además la opción `-DG7_INL_MADDS` de la variante f;
  sin ese define genera el mismo código que d/export/g7.h.)
  Otra laptop: ocupada por la instrumentada de g7d hasta ~00:45 y luego por `pgo_release.sh finish g7d` (JOBS=11)
  tras el entrenamiento Y01/Y02 (~01:30-02:15). Se anotará aquí cuando quede libre.
- 00:33 instrumentada `r17pgo/bin/g7d_pgogen.nro` lista (fastmem 0 mal formadas, 0 avisos de perfil). Entrenamiento
  en cola: Y01_g7dgen_demosA, Y02_g7dgen_demosB (detrás de G07 y de R01 de otro agente). **Otra laptop LIBRE de G7
  desde las 00:33** hasta que termine Y02 (~01:35); entonces `pgo_g7d_fin` (JOBS=11, ~40 min).
- 00:36 **G06 dn2** (d arreglada, la que se entrega): 16,7 · 22,2 · 19,7 · 15,8 · 9,9 = G04 (repetibilidad ±0,2 ms).
  IMAGEN OK, capturas miradas. Verificación de PC de dn2 (con el output_0 arreglado): checks 1x idénticas 220/220
  las cuatro, lista completa.
- 00:45 **G07 f** (madds0/1 en línea) = d dentro del ruido (16,9 · 22,1 · 19,7 · 15,8 · 10,2 contra dn2 16,7 · 22,2 ·
  19,7 · 15,8 · 9,9): **descartada**. IMAGEN OK, capturas miradas.
- 00:50 variante **h** = d SIN la primera ronda de integración de G6 (`r20g6/e2/export_t`, antes de
  apply_g6_inline: 4 930 sitios de callees ≤ 32 PPC) → G5/G6c → feats de d (`tools/build_h.sh`). Idea: si integrar
  más empeora (e2), quizá integrar menos mejore. Otra laptop: `g7_h` (hn, Switch, JOBS=9; termina antes que Y02).
- 00:56 NRO `bin/nsmbw_g7hn.nro/.elf` (h, Switch): .text 67,41 MB (dn 68,49), fastmem 0 mal formadas. En cola:
  G08_g7hn_lista + G09_g7dn2_lista (control contiguo), detrás de Y01/Y02 (entrenamiento PGO) y R02/R03 (otro
  agente). Otra laptop libre otra vez (hasta el finish de g7d).
- 01:13 Y01 (entrenamiento A) bien: `r17pgo/profiles/raw/261115_055504` (89 .gcda de g7dsrc, 15 volcados; las 5 demos
  de la lista). Y02 en curso.
- 01:31 Y02 bien (`profiles/raw/261115_061251`). **Otra laptop OCUPADA desde las 01:31 por `pgo_g7d_fin`
  (pgo_release.sh finish g7d, JOBS=11)**; se anotará cuando termine. Comparador de h semilla 1: 40 024 iguales,
  0 diferencias.
- 01:40 comparador de d (con el arreglo) COMPLETO: semillas 1 y 2 = 40 024 iguales, 0 diferencias, 6 solo-signo-NaN, 1 no concluyente (igual que a/b). **d verificada.**
- 01:45 comparador de h: semillas 1 y 2 = 40 024 iguales, 0 diferencias (6 NaN, 1 no concl.).
- 01:40 PC de h: checks 1x idénticas 220/220 las cuatro, PINDEX/DLMEMO 0, FASTMEM 0, sanity 59,5 fps, lista completa. **h verificada** (falta la consola: G08).
- 01:59 **G08 h (sin la 1.ª ronda de integración) PEOR que d**: 17,4 · 22,8 · 20,6 · 16,4 · 10,5 (+0,6-0,9 ms).
  IMAGEN OK, capturas miradas. **Descartada.** Conclusión de las tres pruebas de integración: d (1.ª ronda de
  callees ≤ 32 PPC, sin 2.ª ronda, madds fuera de línea) es el punto óptimo; más (e2, f) o menos (h) empeora o empata.
- 02:10 `pgo_g7d_fin` está esperando en `lim` de la otra laptop detrás de la instrumentada de m4 (`m4_pgogen`, otro
  agente, -j4, 56/89 objetos): el finish de g7d empezará cuando acabe (~02:40) y tardará ~40 min. Medida Y03/Y04
  hacia las 03:30-04:00.
- 02:08 G09 dn2 (control contiguo de h): 16,8 · 22,1 · 19,7 · 15,7 · 10,1 = G06 (repetible). IMAGEN OK, capturas miradas.

## 5. Resultado (consola, Switch v1 a 1020 MHz, lista fija de 5 demos, ms de trabajo del hilo del juego por fotograma REAL)

| Variante | Qué es | desierto | agua | cielo | nubes | castillo | Verificación |
|---|---|---|---|---|---|---|---|
| m3n (G02) | línea base (c3 + runtime r31t) | 18,9 | 24,6 | 22,0 | 18,4 | 13,0 | — |
| a (G01) | stfs, psq, fp | 17,7 | 23,3 | 21,1 | 17,1 | 10,8 | cputest, difftest 2 sem., checks, lista, IMAGEN OK |
| bn (G03) | a + poll + icm (sin `G5ic`) | 17,0 | 22,3 | 20,2 | 16,2 | 10,1 | ídem (b) |
| dn (G04) | bn + disp + cr | 16,7 | 22,1 | 19,7 | 15,9 | 10,1 | (fallo ctr==0, ver bitácora) |
| **dn2 (G06/G09)** | **dn con el arreglo de disp — LA BUENA** | **16,7/16,8** | **22,2/22,1** | **19,7/19,7** | **15,8/15,7** | **9,9/10,1** | cputest, difftest 2 sem. 0 dif., checks 220/220, lista, IMAGEN OK |
| en2 (G05) | dn + 2.ª ronda de integración (985 sitios) | 17,6 | 22,8 | 20,1 | 16,7 | 10,3 | descartada |
| fn (G07) | dn + ps_madds0/1 en línea | 16,9 | 22,1 | 19,7 | 15,8 | 10,2 | descartada (= d) |
| hn (G08) | dn sin la 1.ª ronda de integración | 17,4 | 22,8 | 20,6 | 16,4 | 10,5 | difftest/checks OK; descartada |

dn2 contra m3n: −2,2 · −2,5 · −2,3 · −2,6 · −3,0 ms (−11 a −23 %). Medida estática (instrucciones AArch64 por
función caliente, ponderado por muestras de consola, `tools/static_mem.py`): c3 1 898 → a 1 618 → b 1 597 → bn 1 575
→ dn 1 557 (−18 %); en2 2 127; hn 1 316 (menos código pero más llamadas: más lento). .text: m3n 75,58 MB → dn2 68,49.

Entrega (sin PGO): export `r9/r33g7/d/export` (+ `-DG7_NOIC` en los objetos de Switch), runtime
`r9/r33g7/nWiiRuntime`, objetos `dn/nx_obj` (Switch) / `d/pc_obj` (PC), NRO **`bin/nsmbw_g7dn2.nro`** (+ `.elf`,
`_input_test.nro`, `NSMBW_g7dn2*` de PC). PGO: `r17pgo/conf/g7d.env`, foto `r17pgo/g7dsrc`, perfiles
`r17pgo/profiles/raw/261115_055504` + `261115_061251` → `r17pgo/bin/g7d_pgo_g7d_all.nro` (en curso, ver bitácora).
- 02:35 la instrumentada de m4 terminó; **pgo_g7d_fin compilando en la otra laptop** (main_output -j1, luego -j11; ~40 min).
- 02:30 **PGO de d listo**: `r17pgo/bin/g7d_pgo_g7d_all.nro/.elf` (+ `_input_test.nro`), 0 avisos de perfil, fastmem
  0 mal formadas; copia del .elf en `bin/g7d_pgo_Y03.elf`. sha256 nro 066eba6b…, input_test 96549622…
  **Otra laptop LIBRE de G7 desde las 02:30.** En cola: Y03_g7dpgo_lista, Y04_m3pgo_lista (contiguas; detrás de
  M01-M03 y R04-R07 de otros agentes, ~03:35-03:55).
- 03:22 **Y03 g7d+PGO**: 14,3 · 20,2 · 17,4 · 13,9 · 9,0 (X01 m3+PGO de anoche: 15,6 · 21,9 · 19,2 · 15,0 · 9,6).
  IMAGEN OK (3/38 raras), capturas miradas (la lista da la vuelta y vuelve al desierto: va más rápido). Y04 (m3+PGO,
  contigua) en curso.
- 03:25 orden L encima (`r17pgo/tools/order_relink.sh g7d_pgo_g7d_all` con los perfiles cpu+main de Y03, en la otra
  laptop; solo re-enlace): `r17pgo/bin/g7d_pgo_g7d_all_ord.nro/.elf`, eqcheck: 137 "distintas" todas en clones
  locales con nombre repetido (ninguna REAL DIFFERENCE), fastmem 0 mal formadas. En cola: Y05 (ord) e Y06 (g7d+PGO
  sin ord, control contiguo), detrás de R08-R11 y Z01/Z02 de otros agentes (~04:50-05:10).
- 03:32 **Y04 m3+PGO (contigua a Y03)**: 15,4 · 21,8 · 19,2 · 14,9 · 9,6 (= X01). **g7d+PGO contra m3+PGO: −1,1 ·
  −1,6 · −1,8 · −1,0 · −0,6 ms** (−7 a −9 %; castillo −6 %). IMAGEN OK las dos, capturas miradas.
- 05:05 **Y05 g7d+PGO+orden L** 14,2 · 19,6 · 17,0 · 13,6 · 9,1 / **Y06 g7d+PGO** (control contiguo) 14,0 · 19,7 · 17,0
  · 13,6 · 9,0: el orden L no aporta nada (ruido ±0,2). Y03→Y06 repetible (±0,4). IMAGEN OK las dos, capturas
  miradas. Recomendado: `r17pgo/bin/g7d_pgo_g7d_all.nro` (sin L).

### PGO (consola, ms juego/fot REAL; desierto · agua · cielo · nubes · castillo)

| | Y04 m3+PGO | Y03 g7d+PGO | Y06 g7d+PGO | Y05 g7d+PGO+L |
|---|---|---|---|---|
| ms | 15,4 · 21,8 · 19,2 · 14,9 · 9,6 | 14,3 · 20,2 · 17,4 · 13,9 · 9,0 | 14,0 · 19,7 · 17,0 · 13,6 · 9,0 | 14,2 · 19,6 · 17,0 · 13,6 · 9,1 |

g7d+PGO contra m3+PGO (media Y03/Y06): −1,3 · −1,9 · −2,0 · −1,2 · −0,6 ms.

> **Estado (05:10): TERMINADO.** Entrega: d (sin PGO: `bin/nsmbw_g7dn2.nro`) y g7d+PGO
> (`r17pgo/bin/g7d_pgo_g7d_all.nro`). La otra laptop queda libre de G7.
