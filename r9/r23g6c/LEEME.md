# r23g6c: frente G6, etapa 3 — cambiar cómo se llaman entre sí las funciones del juego (9-nov-2026)

Base: G6 etapa 2 (`r9/r20g6/e2`, verificada). Lo necesario se copió aquí (herramientas, comparador,
pruebas de CPU, `gen/jt.txt`, `nWiiRuntime` para las cabeceras del comparador). No se escribió en r20g6
ni en otras carpetas. Sin commits. Todo lo compilado pasó por `scripts/lim`, JOBS=2.

> **Estado al parar (9-nov ~07:00, el usuario para ~24 h):** paso 1 implementado y comprobado en lo
> que es rápido (las pruebas de CPU 288/288 con salida idéntica; la pasada sin la opción reproduce e2
> byte a byte). Objetos de Switch del paso 1 compilados sin errores (`c1/nx_obj`, 87, marcados `.ok`;
> medida estática abajo).
> **Falta para dar el paso 1 por verificado:** objetos de PC, enlace con el runtime de s1, tabla de
> fastmem, comparador (2 semillas) y corridas de PC. Todo eso lo hace `tools/chain.sh c1` (ver
> "Cómo seguir"). El paso 2 está escrito en la pasada (opción `--proto 2`) pero sin compilar ni probar.
> El paso 3 está diseñado (abajo), sin escribir.

> **Estado 10-nov (~08:40): paso 1 verificado tras un arreglo; paso 2 compilado y verificado en PC.**
> Ver "Verificación 10-nov" abajo. Binarios para la consola: `bin/nsmbw_g6c1s.nro/.elf` (c1 arreglado)
> y `bin/nsmbw_g6c2s.nro/.elf` (c2). Los `*_input_test*` son solo para las pruebas de PC.

## Verificación 10-nov: las "diferencias" del comparador en c1

El comparador de la cadena (A = m1, B = c1, `--dispatch`) dio semilla 1: 211, semilla 2: 355
diferencias. Clasificación:

1. **Del comparador (207 + 351): no son de c1.** Pasando el comparador de **e2** (`r20g6/difftest/w_e2`)
   con `--dispatch` sobre esas mismas funciones salen las mismas diferencias, con el resumen idéntico
   línea a línea (B de c1 = B de e2). Causa: `--dispatch` nunca se había pasado a todas las funciones en
   e1/e2 (el "0 diferencias" de e2 es `e2b_*`, **sin** `--dispatch`). Con despacho real, un `bctr` (o
   una llamada indirecta) a una dirección aleatoria **a media función** va en A (m1) a su `switch` de
   entrada completo y sigue nativo, y en e1/e2/c1 (switch podado por `apply_g6_entry.py`) a
   `resume_pruned` → status 4. Ejemplo: `func_80017430` salta por `ctr` a 0x809CAD18 (dentro de
   `func_809CACF0`): A sigue y vuelve (status 0), B aborta (status 4). En el juego eso es correcto
   (`resume_pruned` interpreta). Arreglo del comparador: **A = objetos de e2** (`r20g6/difftest/objB_e2`)
   en `tools/chain.sh` (variable `DT_A` para cambiarlo). e2 ya está comprobada contra m1 sin despacho;
   c1/c2 contra e2 con despacho compara exactamente el cambio de protocolo, también por los caminos
   indirectos.
2. **Error real de c1 (4 funciones por semilla: `func_808CC0C0`, `808E1F00`, `808E20E0`, `808E3230`,
   `808DD590`, `808F3010`)**: misma salida salvo `inst_count` más bajo en B y `log` distinto (el sondeo
   lento registra `inst_count`). Causa en `tools/apply_g6c.py`: el análisis hacia delante de "sucios"
   solo ponía `G5ic` sucio (ENTRY) en el nodo 0. En las funciones cuya dirección propia **no es la
   primera** (897 con `_g6`; p. ej. `func_801006C0` empieza en 0x800F0EB0), la llamada directa entra
   por `case 0x801006C0: goto loc_801006C0;` del `switch`, que el análisis no trataba como entrada:
   en ese camino `G5ic` se creía limpio y el sondeo lento / los accesos MMIO (`write8_io`) no hacían
   `ctx.inst_count = G5ic` antes → `ctx.inst_count` viejo en `process_pending_callbacks_slow`, y si
   había interrupción se devolvía el viejo (se perdían instrucciones del reloj virtual). Las corridas
   de PC no lo vieron (solo cambia el reloj en sondeos lentos de esas funciones).
   **Arreglo**: todos los destinos `case ...: goto loc_X` del `switch` de entrada reciben ENTRY
   (en una reanudación normal `G5ic_in == ctx.inst_count`, así que es conservador y casi gratis). Para
   `--proto 2`, además, los registros que se pasan se calculan con la vivacidad en la etiqueta de la
   dirección propia (antes: la primera dirección). Cambian 11 output_*.cpp de c1 (73 líneas, todas
   `ctx.inst_count = G5ic;` añadidos); copias viejas en `c1/export_prefix10nov/`, resultados viejos en
   `runs/prefix10nov/` y `difftest/runs/m1ref_prefix/`.
   Nota: la pasada ahora escribe `nwii::runtime::CPUContext&` en la cabecera de los `_g6` (antes
   `CPUContext&`; equivalente). Para no recompilar todo, en `c1/export` se dejó la forma vieja en los
   11 ficheros copiados (normalizados con sed); el resto de ficheros no cambia.

### Resultados tras el arreglo (A = e2, `--dispatch ../gen/jt.txt`, 40 031 funciones)

| | semilla 1 | semilla 2 |
|---|---|---|
| c1 | 40 030 iguales, **0 diferencias**, 1 no concluyente | 40 030 iguales, **0 diferencias**, 1 no concluyente |
| c2 | 40 030 iguales, **0 diferencias**, 1 no concluyente | 40 030 iguales, **0 diferencias**, 1 no concluyente |

(`runs/difftest_c1.txt`, `runs/difftest_c2.txt`, detalle en `difftest/runs/c1_*`, `c2_*`.)
Pruebas de CPU: c1 y c2 288/288, salida idéntica (`cputest/g6c2.out` = `g6c1.out` = `g6.out`).
Tabla de fastmem: 0 mal formadas (69 sin entrada, como e2) en c1 y c2.

Corridas de PC (`runs/*_g6c1s.txt`, `runs/*_g6c2s.txt`):
- checks 1x: c1 y c2 title/map/walk/lvl11 **idénticas** (200/200, 200/200, 200/200, 220/220),
  diff_lines=0.
- sanity / title / cb4 / long: mismas escenas que antes, `[FASTMEM]` 0 fallos, sin cuelgues,
  `resume_pruned_lines` 28-29 (como e1/e2).

### Paso 2 (c2): compilado, verificado en PC; medida estática peor

`c2/export` = pasada `--proto 2` (con el arreglo). 38 658 funciones con `_g6`, registros pasados por
función (`c2/export/g6c_fns.txt`): 0: 990, 1: 8 232, 2: 18 262, 3: 7 825, 4: 1 871, 5: 743, 6: 735.
Compila sin errores en Switch y PC (`c2/build_*.log`).
Medida estática (`runs/static_c2.txt`): total 13 231 869 ins (**+8,6 %** sobre e2), ponderada
2 004,3 ins/función (**+2,2 %**; c1: +0,1 %). Algunas calientes bajan (func_80237BD0 335 → 316,
func_80237C40 306 → 289) pero la mayoría sube (PSMTXCopy 814 → 883, func_801A2B10 1 780 → 1 894,
func_80267F40 6 347 → 6 579). Motivo probable: los registros pasados cuentan como sucios a la entrada,
así que el llamado los vuelve a escribir en `ctx` en **cada** salida aunque no los toque (antes los
escribía el llamador solo si estaban sucios), y el `G6R` de vuelta + envoltorios. Lo que ahorra es
dinámico (cargas de `ctx` en el llamado, escrituras en el llamador), solo medible en la consola.
Si se sigue con el paso 2/3, la mejora obvia: no marcar sucio a la entrada un registro pasado que el
llamador ya tenía limpio (habría que pasar esa información, o volcar en el llamador solo los sucios y
pasar los demás como "limpios"); o pasar menos registros (solo los que el llamado lee antes de
escribir *y* modifica).

## Qué es la etapa 3

Hoy toda llamada directa entre funciones del juego pasa por `ctx` en memoria: el llamador escribe en
`ctx` los registros sucios, `ctx.inst_count`, `ctx.lr` y `ctx.pc`; el llamado comprueba `ctx.pc`, lee de
`ctx` lo que usa; al volver escribe lo suyo y `ctx.pc = ctx.lr`; el llamador compara `ctx.pc` y recarga.
La etapa 3 pasa parte de eso a registros del ARM (argumentos y valor de vuelta de C++), y solo escribe
en `ctx` antes de un "escape" (sondeo lento de interrupciones, llamada indirecta, HLE, MMIO,
reanudación, llamada a una función sin transformar).

Todo está en una sola pasada nueva, `tools/apply_g6c.py` = copia de la pasada G5
(`apply_g5_regs.py`, que es la que sabe qué registros están sucios/vivos) con la opción `--proto N`.
Sin `--proto` la salida es idéntica byte a byte a la de e2 (comprobado con output_3 y output_24).

### Paso 1 (`--proto 1`, carpeta `c1/`): `inst_count` en registros

Cada función transformada `F` pasa a ser dos:
- `extern "C" uint64_t F_g6(CPUContext& ctx, uint64_t ic)`: el cuerpo; recibe el reloj virtual y
  devuelve el nuevo;
- `void F(CPUContext& ctx) { ctx.inst_count = F_g6(ctx, ctx.inst_count); }`: envoltorio para
  `run_game`, `nwii_dispatch`, las tablas, el HLE y las funciones no transformadas (mismo nombre y
  firma que antes: `main_output.o` y `fastdispatch.o` de r14g5 sirven tal cual).
- Una llamada directa a una función con `_g6` queda `ctx.pc = 0xF; G5ic = F_g6(ctx, G5ic);` (sin
  `ctx.inst_count = G5ic` antes ni `G5ic = ctx.inst_count` después). Las llamadas de cola directas
  (`if (native_stack_ok()) F(ctx); return;`) pasan a `return F_g6(ctx, G5ic)`.
- Exactitud: (1) lo que devuelve un `_g6` es siempre el `inst_count` verdadero (la local donde es
  válida; `ctx.inst_count` tras un escape que pudo cambiarlo: sondeo lento, despacho, reanudación);
  (2) a la entrada de un `_g6` la local cuenta como "sucia" (el llamador no escribió `ctx`), así que el
  análisis de G5 la vuelca antes de cualquier escape, igual que cualquier registro sucio; (3) tras una
  llamada `_g6` el llamador la tiene sucia y el resto limpio (como antes).
- Esto ya cubre el "paso 4" del plan para `inst_count` (solo se escribe en `ctx` antes de escapes).
- Declaraciones de los `_g6`: `export/g6c_decls.h` (lo incluye cada output_*.cpp); lista con los
  registros que se pasan: `export/g6c_fns.txt`.
- Resultado de la pasada: 38 658 funciones con `_g6`, 96 701 llamadas directas convertidas, 1 337 sin
  transformar (1 334 de la lista de exclusión de G5 como en e2, 2 que pueden "caerse por el final" de
  la función —WPAD, frías— y la de siempre de BT).

Comprobado:
- Pruebas de CPU (`cputest/`, `gen_tests_g6c1.inc`): **288/288, salida idéntica** a la de G5/e2.
- La pasada refactorizada para el paso 2 sigue dando exactamente `c1/export` con `--proto 1`.

### Paso 2 (`--proto 2`; compilado y verificado el 10-nov, ver arriba)

Además de `ic`, los registros de la Wii que la función lee al entrar (de r1, r3-r10, como mucho 6:
x2..x7) van como argumentos (`F_g6(ctx, ic, r1, r3, r5...)`; cada función tiene su lista, calculada
en una primera vuelta de la pasada con la vivacidad a la entrada) y la vuelta trae
`struct G6R { uint64_t ic; uint64_t r; }` con r = r3 | r1 << 32 (x0/x1). Invariantes: a la entrada
`ctx` + argumentos = estado exacto (los pasados cuentan como sucios); en toda salida `ctx` + {ic, r3,
r1} devueltos = estado exacto (en salidas tras un escape se devuelven los de `ctx`). El llamador no
vuelca los registros que pasa; el llamado vuelca (si están sucios) los demás al salir. En la cola sin
pila nativa se vuelcan los pasados antes de volver. Probado solo en una unidad (output_3) a nivel de
texto.

### Paso 3 (diseñado, sin escribir)

1. Sin `ctx.pc = F` antes de las llamadas directas ni `if (ctx.pc != F)` a la entrada: el envoltorio
   marca la reanudación con el bit 63 de `ic` (`tbnz`, 1 instrucción); el cuerpo hace
   `if ((int64_t)G5ic < 0) { G5ic &= ~bit; if (ctx.pc != F) switch... }`. Solo si la función tiene
   `switch` de entrada (si no, el envoltorio no pone el bit).
2. `lr` como local de G5 (`G5lr`) que se pasa como argumento (deja 5 registros para r1/r3..): adiós
   `ctx.lr = R` (mov/movk/str) y la carga de `mflr`.
3. Vuelta con indicador en el bit 63 de `ic` en vez de `ctx.pc = ctx.lr` + comparar: `blr` normal si
   `G5lr == lr_de_entrada` (en las hojas GCC lo resuelve solo); si no, `ctx.pc = ctx.lr = G5lr` y bit
   puesto. Las salidas tras escape devuelven "normal" solo si `ctx.pc == lr0 && ctx.lr == lr0`. El
   llamador sustituye `if (ctx.pc != R) return;` (la línea siguiente a la llamada) por
   `if ((int64_t)G5ic < 0) { G5ic &= ~bit; <volcar lo sucio salvo lr>; return <mismo criterio>; }`.
   Llamadas de cola directas: si el llamado vuelve "normal" pero `G5lr != lr0`, se marca. La forma
   de cola de los callees copiados (`... F(ctx); goto LOC;`, 169 sitios) se queda con el envoltorio.
   El envoltorio escribe `ctx.pc = ctx.lr = lr0` si la vuelta es normal.
   Formas de `return` que hay que cubrir (contadas en e2): `if (ctx.pc != R) return;` 110 522,
   `ctx.pc = ctx.lr; return;` 39 092, `case ...: return;` 36 165, `blr` condicional 1 470, `return;`
   suelto 15.
4. Posible extra: el `ctx.pc = X` de cada sondeo (vuelta de bucle) solo en el camino lento.

## Medida estática (AArch64, tamaños de símbolo de los objetos de Switch; `tools/static_cmp.py`)

Ponderado con las muestras de la consola por función (`fn_weights.tsv`); el coste de una función es
su cuerpo `_g6` si lo tiene (`runs/static_c1.txt`).

| | e2 | c1 (paso 1) |
|---|---|---|
| instrucciones de todo el código generado | 12 185 562 | 12 582 149 (+3,3 %: los envoltorios) |
| media ponderada por muestras (ins/función) | 1 960,7 | 1 962,6 (+0,1 %) |

**El paso 1 solo no reduce el tamaño estático** de las funciones calientes (algunas −8 %, p. ej.
func_80237BD0 335 → 308, func_80237C40 306 → 280; otras +1-2 % porque `ic` ocupa un registro más). Lo
que quita es un `str` + `ldr` de `ctx` por llamada directa (camino dinámico), no visible en esta
medida; se esperaba ~1-3 %. Su valor real es ser la base de los pasos 2 y 3 (mismo mecanismo de
`_g6` + envoltorio). Se puede medir en la consola o saltar directamente a c2 si se prefiere no gastar
una medida en él.

## Cómo seguir (actualizado 10-nov)

- Pasos 1 y 2 verificados en PC (arriba). Siguiente: medir en la consola `bin/nsmbw_g6c1s.nro` y
  `bin/nsmbw_g6c2s.nro` (+ `.elf`) contra s1 / g6e1s / g6e2s (lo hace el coordinador). La medida
  estática de c2 es peor (+2,2 % ponderada); si la consola tampoco lo mejora, revisar el volcado de
  los registros pasados (ver "Paso 2 (c2)") antes del paso 3.
- `tools/chain.sh <c>` compara ahora contra e2 (`DT_A` para otro A). Para rehacer c1/c2 desde cero: los
  mismos comandos de abajo (la pasada ya trae el arreglo de las entradas del `switch`).

### Instrucciones anteriores (9-nov)

1. `cd ~/nsmbw-port/r9/r23g6c && nohup tools/chain.sh c1 &` (los objetos de Switch ya están: salta
   ese paso) — hace, de uno en uno y con `lim`:
   objetos de PC, enlace con el runtime de s1 + orden L (`tools/build_g6cs.sh c1` →
   `bin/nsmbw_g6c1s.nro/.elf`, `_input_test.nro` solo pruebas, `NSMBW_g6c1s(_input_test)` de PC),
   tabla de fastmem (`runs/fmtab_c1.txt`), comparador contra m1 en las 40 031 funciones con 2 semillas
   y `--dispatch ../gen/jt.txt` (`runs/difftest_c1.txt`) y las corridas de PC `checks sanity title cb4
   long` (`runs/*_g6c1s.txt`). Log: `runs/chain_c1.log`. ~2 h.
2. Si todo está bien (comparador 0 diferencias reales, checks 1x idénticas, corridas sin fallos,
   fastmem 0 mal formadas): mandar `bin/nsmbw_g6c1s.nro` + `.elf` al coordinador para medir en la
   consola contra s1 / g6e1s / g6e2s.
3. Paso 2: `cp -a ../r20g6/e2/export_pre c2/export; rm -rf c2/export/build`, y
   `G5_JOBS=3 ~/nsmbw-port/scripts/lim python3 tools/apply_g6c.py c2/export --orig ../r20g6/e2/export_pre
   --eager --entry-fns --exclude tools/g5_exclude_g6b.txt --inline-leaves --leaves-from
   ../r20g6/e2/export_pre --proto 2`; pruebas de CPU con `--proto 2 --file`; `tools/chain.sh c2`.
4. Paso 3 según el diseño de arriba.

Detalles de las herramientas:
- `tools/apply_g6c.py`: la pasada (con `--proto` hace dos vueltas: la 1.ª decide qué funciones se
  transforman y qué registros se pasan; la 2.ª escribe y comprueba que el conjunto coincide).
- `tools/build_g6.sh`: compila los output_*.cpp con el comando exacto de r14g5 (sin cambios de G6).
- `tools/build_g6cs.sh <c>`: enlace = el de `r21s/tools/build_g6s.sh` (runtime de s1) con estos
  objetos; orden L = el de G6 e2 con cada `_g6` delante de su función (`tools/mk_order.py`).
- `difftest/mkobjs.sh`: acepta los `_g6` de más en B (el resto de símbolos tiene que coincidir).
- `tools/verify_g6.sh`: copia de la de r20g6 apuntando aquí (nombres de corrida `g6c_*`).
- Pruebas de CPU: `cd cputest; python3 ../tools/apply_g6c.py --eager --entry-fns --proto N --file
  gen_tests.inc gen_tests_g6cN.inc`; compilar con
  `g++ -std=c++20 -O2 -I../../r14g5/nWiiRuntime/include -I. -DGEN_INC='"gen_tests_g6cN.inc"' test.cpp
  stub_gov.cpp ../../r14g5/nWiiRuntime/src/hle/interpreter.cpp -o test_g6cN` (con `lim`).

Aviso del coordinador para opciones nuevas en la Switch: las variables de entorno con valor "0" se
borran salvo que estén en `keeps_zero` (`switch_platform.cpp`, `apply_env`). (Esta etapa no añade
ninguna.)
