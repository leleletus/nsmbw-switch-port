# r30g6c3: frente G6c3 — protocolo de llamada, paso 3 (12-nov-2026)

Base: G6 e2 (`r20g6/e2/export_pre` + la pasada G5/G6c) y el runtime de **m2** (r7f, piezas de `r24m2/rt`).
Copiado aquí lo necesario (herramientas de r23g6c, comparador, pruebas de CPU, `gen/jt.txt`, cabeceras).
No se tocó la consola, `nand`, `r17pgo` ni carpetas de otros frentes. Sin commits.

> **Estado (12-nov):** c3 hecho y verificado (pruebas de CPU 288/288 idénticas, comparador 0 diferencias en
> 2 semillas con `--dispatch`, tabla fastmem 0 mal formadas, corridas de PC: ver §5). NRO para medir en la
> consola contra m2: **`bin/nsmbw_g6c3m.nro` + `bin/nsmbw_g6c3m.elf`** (runtime de m2 = r7f).
> c3b (`--closed`) quedó descartado por la medida estática (≈ c3, ver §3); no se compiló entero.

## 1. Diagnóstico: por qué c1/c2 empeoraron en la consola

Datos: los perfiles de la consola de las corridas del 10-nov (`switch/real_logs/bench_L01_g6c1s_lista_*`,
`bench_L02_g6c2s_*`, `bench_L03_g6e2s_*`, misma lista de demos, mismo runtime s1) con sus `.elf`.
Herramientas nuevas (todas en `tools/`, sin IA, `python3 -I`):
- `hostprof.py` (copia de r20g6, ahora cuenta también los cuerpos `*_g6` como código generado),
  `prof3.sh` (lo pasa a e2/c1/c2 → `diag/hostprof_*.txt`);
- `fndelta.py` (muestras por función, F + F_g6 + envoltorio juntos → `diag/fndelta.txt`);
- `gentot.py` (muestras totales / en código generado por corrida), `clsfn.py` (qué funciones/instrucciones
  se llevan una clase de hostprof), `pcst.py` (dónde caen los accesos a ctx.pc/lr/inst_count),
  `wrapsamp.py`-equivalente en línea (muestras en los envoltorios).

Resultados:
1. **Los envoltorios cuestan ~2 % del código generado.** Muestras en los envoltorios `void F(ctx)` (las
   funciones con cuerpo `_g6`): c1 2 708 = **2,1 %** de las muestras de los cuerpos; c2 2 418 = **2,2 %**.
   Todo lo que entra por `nwii_dispatch` (llamadas indirectas / métodos virtuales: muchísimas en NSMBW),
   `run_game` o una función sin transformar paga una llamada más con su marco (`stp x30` / `ldp` / `ret`),
   carga/guarda de `inst_count` y, en c2, de hasta 6 argumentos y la vuelta de r3/r1 (13 instrucciones de
   media). En c1 GCC además metió el cuerpo entero en el envoltorio de las funciones pequeñas (código
   duplicado: +3,3 % de código total).
2. **Lo que c1 quitaba no se traduce en tiempo.** Muestras de c1 sin los envoltorios ≈ e2 (+0,3 %); el
   total de c1 (+2,4 %) ≈ e2 + envoltorios. Clases (`diag/hostprof_*.txt`): `ctx.inst_count st` bajó
   (1 234 → 519 muestras) pero `ctx.inst_count ld` subió (1 201 → 2 103: los envoltorios, las recargas tras
   escapes). Un `str`+`ldr` de `ctx` por llamada que acierta en L1 / reenvío de almacén a carga apenas cuesta
   en el A57; lo que cuesta es la cadena de **muchos almacenes seguidos** (los bloques de volcado antes de
   cada llamada y en cada `return`: la muestra cae en el último almacén cuando se llena el búfer de
   almacenes; p. ej. en e2 `func_80237BD0` tiene 151 muestras en el `str ctx.pc` final de una ráfaga de 9
   almacenes de su `return`).
3. **c2 además añade almacenes**: los registros pasados cuentan como sucios a la entrada, así que el llamado
   los vuelve a escribir en `ctx` en **cada** salida aunque no los toque, y el llamador los recarga igual
   (solo r3/r1 vuelven en registros). Encima, el arreglo del 10-nov (todas las entradas `case` del `switch`
   marcan sucio lo pasado) hace que tras cada punto de reanudación (= tras cada llamada) los registros
   pasados de la propia función queden "quizá sucios" y se vuelquen en cada escape/retorno posterior.
   Por eso c2 es +8,6 % de código y +2,2 % ponderado, y en la consola peor que c1.
4. Comparar números absolutos de muestras entre corridas no sirve (el muestreador da 222 k-259 k muestras
   por corrida para la misma m2: `gentot.py` sobre J03/K02/G01); sí las proporciones dentro de un perfil y
   los ms/fotograma de `por_escena.py` (ruido entre corridas de m2: ±0,1-0,2 ms).
5. Dónde está el protocolo en e2 (`diag/pcst_e2.txt`, % de las muestras del código generado):
   `ctx.pc st` 2,6 % (1,2 % en los `ctx.pc = X` de cada sondeo de interrupciones, 0,5 % antes de `bl`,
   0,5 % en `ctx.pc = ctx.lr` de los `return`), `ctx.pc ld` 1,3 % (0,5 % justo tras `bl`: la comprobación
   `if (ctx.pc != R)`), `ctx.lr` 0,7 %, `ctx.inst_count` 1,9 %. Más las constantes `mov/movk` de esas
   direcciones (parte del 7,6 % de constantes).

Conclusión: para ganar hay que (a) no pagar un envoltorio con marco en los caminos de despacho, y
(b) quitar **almacenes** (y sus constantes) de las secuencias de llamada/retorno, no solo una carga/almacén
de `inst_count`; (c) no volcar en el llamado lo que el llamador ya tenía limpio (lo de c2 no se sigue).

## 2. c3 (`--proto 3` en `tools/apply_g6c.py`, función `emit_p3`)

Sobre el paso 1 (`inst_count` en registros), sin el paso 2 (r3-r10 siguen por `ctx`):
- **pc y lr son claves de G5** (`G5pc`, `G5lr`), como los GPR: viven en locales y se escriben en `ctx`
  solo antes de un escape (sondeo lento, llamada indirecta, función sin transformar, HLE, MMIO que lee el
  reloj, `return` hacia quien no es una llamada directa).
- Firma: `extern "C" uint64_t F_g6(CPUContext& ctx, uint64_t ic, uint64_t lr0x)`;
  `lr0x` = LR de entrada (bits 0-31) | `G6W` (bit 32: "entré desde fuera: deja `ctx` exacto al volver") |
  `G6CHK` (bit 33: "mira `ctx.pc` y usa el `switch` de entrada").
- **Envoltorio sin marco**: `void F(ctx) { F_g6(ctx, ctx.inst_count, ctx.lr | G6W | G6CHK); }` → GCC lo
  deja en `ldr; ldr; orr; b F_g6` (llamada de cola). Lo que hacía el envoltorio de c1 después de la llamada
  (guardar `inst_count`, `pc = lr`) lo hace el cuerpo en el camino lento de su `return` cuando ve `G6W`.
- **Llamada directa**: `ctx.lr = R; ctx.pc = F; F(ctx); if (ctx.pc != R) return;` →
  `G5lr = R; t = F_g6(ctx, G5ic, G5lr); if (t < 0) return g6_exact(..); G5ic = t; G5pc = G5lr;` y la
  comprobación siguiente se pliega sola (G5pc = R constante). Sin `str ctx.pc`, `str ctx.lr`,
  `str/ldr ctx.inst_count`, `ldr ctx.pc` + `mov/movk R` + `cmp` en el llamador; sin `ldr ctx.pc` +
  constante + `cmp` de la entrada (salta con `tbnz` del bit 33); sin `ldr ctx.lr`/`str ctx.pc` en el
  `return` del llamado.
- **Retorno rápido** (`ctx.pc = ctx.lr; return;`): vuelca lo sucio salvo ic/pc/lr y
  `return (uint64_t)G5lr == lr0x ? G5ic : g6_slowret(...)` (un `cmp` + `b.eq`). `g6_slowret` (caso raro o
  entrada desde fuera) escribe `inst_count`, `pc = lr = G5lr` y devuelve el bit 63 si `G5lr != lr0`.
- **Retornos exactos** (tras un escape, `if (ctx.pc != R) return;` tras una función sin transformar, el
  `switch`, el sondeo): vuelcan todo y `return g6_exact(ctx, lr0x)`: bit 63 libre solo si
  `ctx.pc == ctx.lr == lr0` (entonces el llamador directo sigue como en e2); si no, el llamador devuelve
  también `g6_exact` (= el `if (ctx.pc != R) return;` de antes, desenrollando hasta `run_game`).
- Llamadas de cola directas: si `G5lr == lr0` se pasa `lr0x` (con `G6W`, sin `G6CHK`) y es una llamada de
  cola de verdad; si no, `g6_tailfix`. Sin pila nativa: `ctx.pc = F`, volcado y `g6_exact`.
- **Sondeo**: el `ctx.pc = X;` de cada vuelta de bucle pasa a `G5pc = X` y solo se escribe en el camino
  lento. `poll_due()` lee `ctx.pc` (casos 0 / 0xFFFFFFFC del despachador): tras `ctx.pc = <dirección de
  código>` esa prueba es falsa, así que se usa `g6_poll_due` (= `poll_due` sin esa prueba; en
  `g6c_decls.h`, mantener al día con `cpu_context.h`); en los demás sondeos se escribe `ctx.pc` antes.
- `write_dec` (imprime pc/lr con `NWII_SAMPLE`) vuelca pc/lr antes.
- Exactitud: en todo punto donde algo de fuera puede mirar `ctx` (escapes, `return` a quien no es una
  llamada directa) `ctx` tiene lo mismo que e2. Entre medias pc/lr/inst_count viven en locales como los
  GPR. Única diferencia conocida, solo de depuración: los registros opcionales del runtime que leen
  `g_ctx_ptr->pc/lr` dentro de un acceso MMIO (`NWII_WGTRACE`, la ruta de segmentos de gx.cpp, el aviso
  "[HW DI] SUSPECT", las trampas de ax_async) pueden ver otro pc "aproximado" (en e2 tampoco era el pc
  de la instrucción: era el de la última llamada/sondeo).

## 3. c3b = c3 + `--closed` (llamados "cerrados")

Lo que sigue costando en c3 alrededor de cada llamada directa son los bloques de volcado/recarga de G5:
antes de la llamada se escriben en `ctx` **todos** los registros sucios (10-25 almacenes: GPR, los 4 bytes de
cada campo CR, ctr) y después se recargan todos los vivos (15-20 cargas), porque el llamado podría mirar
`ctx` en un escape (sondeo, llamada indirecta...) o escribir cualquier registro.
`--closed` calcula, entre las dos vueltas de la pasada, qué funciones `_g6` son **cerradas**: sin sondeo, sin
bloques "raw", sin lectores del reloj/MMIO de G5, y cuyos únicos escapes son llamadas directas (o de cola)
a funciones `_g6` también cerradas (punto fijo sobre el grafo de llamadas). Para ellas se guarda el conjunto
transitivo de registros que **pueden leer** (todas sus claves: se cargan a la entrada) y que **pueden
escribir** (las que alguna sentencia define). En una llamada directa a una función cerrada el llamador:
- vuelca solo los sucios que el llamado puede leer (p. ej. `PSVECCrossProduct`: r3, r4, r5 en vez de 26);
- recarga solo los que el llamado puede escribir (p. ej. `PSVECNormalize`, `PSMTXCopy`: ninguno);
- el resto sigue en registros del ARM (sucios) a través de la llamada;
- si el llamado no vuelve "normal" (solo `g6_slowret` con LR cambiado, o sin pila nativa) el llamador
  vuelca entonces lo que le quedaba y desenrolla (camino frío).
Exacto: nada de fuera puede mirar `ctx` mientras corre una función cerrada (no tiene escapes), así que lo
que no lee/escribe da igual dónde esté. 12 923 funciones cerradas; 32 149 de las 96 701 llamadas directas
van a una.

**Resultado: no compensa.** Primera versión (mantener en registros todo lo no leído/escrito): GCC guarda
esos valores en la pila nativa a través de la llamada (los registros callee-saved no dan para tanto):
`func_80248330` pasa de 586 a 773 accesos a pila. Versión final (`--closed-max 4`: como mucho 4 claves
sucias, vivas, ni leídas ni escritas por el llamado se quedan en registros; dos vueltas del análisis):
medida estática ponderada = c3 (`diag/static_mem_c3b.txt`: ins 1 900 vs 1 898, ctx ld 88,4 vs 89,8,
ctx st 119,8 vs 118,2, pila algo peor). No se mandó a compilar entero ni a la consola. Además la exportación
de c3b se generó con el fallo de vivacidad de pc del §5 (apartada en `stale/c3b_bugP`); queda la opción
en la pasada por si se quiere retomar.

## 4. Cómo rehacerlo

```
cd ~/nsmbw-port/r9/r30g6c3
# pasada (c3; c3b añade --closed --closed-max 4). ~1 min con G5_JOBS=3
mkdir -p c3 && cp -a ../r20g6/e2/export_pre c3/export && rm -rf c3/export/build
G5_JOBS=3 ~/nsmbw-port/scripts/lim python3 tools/apply_g6c.py c3/export --orig ../r20g6/e2/export_pre \
  --eager --entry-fns --exclude tools/g5_exclude_g6b.txt --inline-leaves --leaves-from ../r20g6/e2/export_pre --proto 3
# pruebas de CPU (288/288, salida = cputest/g6.out)
cd cputest && python3 ../tools/apply_g6c.py --eager --entry-fns --proto 3 --file gen_tests.inc gen_tests_g6c3.inc
~/nsmbw-port/scripts/lim g++ -std=c++20 -O2 -I../../r14g5/nWiiRuntime/include -I. -DGEN_INC='"gen_tests_g6c3.inc"' \
  test.cpp stub_gov.cpp ../../r14g5/nWiiRuntime/src/hle/interpreter.cpp -o test_g6c3 && ./test_g6c3 > g6c3.out; cmp g6c3.out g6.out
# objetos en la otra laptop (plantillas de compilación de r14g5 guardadas en tools/templates/)
~/nsmbw-port/scripts/remoto.sh push r9/r30g6c3/tools r9/r30g6c3/c3/export
~/nsmbw-port/scripts/remoto.sh run g6c3 'JOBS=11 r9/r30g6c3/tools/rbuild.sh c3 both'      # ~8 min nx + ~15 min pc
~/nsmbw-port/scripts/remoto.sh pull r9/r30g6c3/c3/nx_obj r9/r30g6c3/c3/pc_obj
# comparador en la otra laptop (10 procesos): A = objetos de PC de e2, B = c3, 2 semillas, --dispatch
~/nsmbw-port/scripts/remoto.sh push r9/r30g6c3/c3/pc_obj
~/nsmbw-port/scripts/remoto.sh run dt_c3 'r9/r30g6c3/tools/rdifftest.sh c3 10'
# aquí: medida estática, enlace con el runtime de m2, tabla fastmem y corridas de PC (cada una con .runlock)
REMOTE_DT=1 nohup tools/chain_c3.sh c3 &          # log: runs/chain_c3.log
python3 -I tools/static_mem.py e2=../r20g6/e2/nx_obj c3=c3/nx_obj   # medida por clases (ctx/pila/constantes)
```
Comprobación rápida de una sola unidad: `tools/one_file.py <output_N.cpp> <dir>` + `tools/compile1.sh nx <dir>/output_N.cpp x.o`
+ `tools/dis.sh x.o func_X_g6`.

## 5. Verificación de c3

Historia (para no repetir): (1) la primera exportación de c3 se hizo antes de dos arreglos (`g6_poll_due`;
vivacidad) y se tiró (`stale/c3_prepollfix`, sin objetos); (2) el comparador encontró un fallo real en la
segunda: al poner "pc no es uso de un `return`" se quitó también el uso real de `ctx.pc` en
`if (ctx.pc != R) return;` tras una llamada a una función **sin transformar**, así que `G5pc` no se
recargaba tras ella y la función volvía antes de tiempo (585+ diferencias en la semilla 1, p. ej.
`AIInit` terminaba con pc = lr = 0x8019F220). Las pruebas de CPU no lo veían (no tienen llamadas).
Arreglado (`u |= RETB & ~P`, sin quitar usos reales); exportación mala apartada en `stale/c3_bugP`,
corridas de PC hechas con ella en `stale/runs_bugP` (no valen).

Con la pasada final (`c3/export`, `c3/pass.log`):
- Pruebas de CPU: **288/288, salida idéntica** a la de G5/e2 (`cputest/g6c3.out` = `g6.out`).
- Comparador por función (otra laptop, 11 procesos, A = objetos de PC de e2, B = c3, todas las 40 031
  funciones, `--dispatch gen/jt.txt`): semilla 1 **40 030 iguales, 0 diferencias**, 1 no concluyente;
  semilla 2 **40 030 iguales, 0 diferencias**, 1 no concluyente (como c1/c2). `runs/difftest_c3.txt`,
  `difftest/runs/c3_s*_total.txt`.
- Tabla de fastmem del `.elf`: 0 mal formadas, 69 sin entrada (= e2). `runs/fmtab_c3.txt`.
- Corridas de PC (`tools/verify_c3.sh g6c3m checks sanity title cb4 long lista`): ver abajo.

### Medida estática (objetos de Switch; ponderada con las muestras de la consola, `fn_weights.tsv`)

`runs/static_c3.txt` (tamaño) y `diag/static_mem_c3.txt` (por clase, media ponderada por función):

| | ins | ctx ld | ctx st | ctx.pc | ctx.inst_count | ctx.gpr | ctx.cr | pila ld | pila st | mov/const | saltos |
|---|---|---|---|---|---|---|---|---|---|---|---|
| e2 | 1 960,7 | 111,0 | 148,3 | 28,5 | 20,1 | 102,6 | 39,6 | 103,5 | 116,0 | 257,7 | 167,3 |
| c1 | 1 962,6 | 107,1 | 141,3 | 28,4 | 9,2 | 102,7 | 39,6 | 100,9 | 115,2 | 271,2 | 167,2 |
| c2 | 2 004,3 | 111,3 | 136,9 | 28,5 | 8,9 | 102,5 | 39,7 | 105,2 | 119,9 | 282,9 | 168,2 |
| **c3** | **1 898,4 (−3,2 %)** | **90,8 (−18 %)** | **118,2 (−20 %)** | 6,1 | 8,0 | 93,2 | 35,3 | 99,7 | 105,7 (−9 %) | 235,8 (−8,5 %) | 172,8 |

Código total: 12 103 335 instrucciones (−0,7 % sobre e2; c1 +3,3 %, c2 +8,6 %). Es el primer paso de la
etapa 3 que baja la medida estática, y baja justo lo que el diagnóstico señala (almacenes en `ctx`).
Ojo: la más caliente, `func_800ECAE0` (2,5 % de las muestras), crece 10 985 → 11 732 instrucciones y sus
accesos a pila 1 286 → 1 549 (presión de registros: `G6lr0`/`G5lr`/`G5pc` vivos en bucles grandes con
sondeo); sus accesos a `ctx.pc/lr` bajan 166 → 31. Si la consola no gana, mirar esa función primero
(perfil con `tools/hostprof.py`, que ya cuenta los `_g6`).

### Corridas de PC (`runs/*_g6c3m.txt`, hojas `runs/hoja_*_g6c3m.jpg`, `runs/lista_g6c3m/hoja.jpg`)

| Prueba | c3 (g6c3m) | Referencia (m2) |
|---|---|---|
| `checks.sh` 1x + PINDEX/DLMEMO | title/map/walk/lvl11 **220/220 idénticas, `diff_lines=0`**; índice 0 diferencias; memo 0 diferencias; FASTMEM 0 | m2: `diff_lines=0` |
| sanity 165 s | título → mapa → 1-1, 59,7 fps, FASTMEM 0, resume_pruned 27 | m2 28 |
| title 200 s | MOVIE + demos, 59,7 fps, FASTMEM 0 | |
| cb4 200 s | MULTI_PLAY_COURSE_SELECT → nivel, 59,8 fps, FASTMEM 0 | |
| long 300 s | mapa → 1-1, 59,8 fps, FASTMEM 0, resume_pruned 28 | |
| lista 300 s | desierto4j → agua_pinguino → cielo_paratroopas → nubes_plataformas → castillo, "lista completa", 59,8 fps, FASTMEM 0 | igual |

Hoja de la lista revisada: imágenes limpias (los cuadros negros son las transiciones), sin vértices
disparados ni basura.

## 6. Binarios para medir (`bin/`, sha256 en `runs/bin_sha256_g6c3m.txt`)

Runtime de m2 (r7f, `r24m2/rt`), mismo enlace que m2 salvo los 87 `output_*.o` (`nx/link_g6c3m.cmd`),
orden L de m2 con cada `_g6` delante de su función (`nx/order/g6c3m.txt`). A/B directo contra m2.

| Archivo | sha256 |
|---|---|
| **`nsmbw_g6c3m.nro`** (el juego, GL; NACP "NSMBW PRUEBA g6c3m (m2 + G6 etapa 3 c3)") | `11b798056cc6e0178f3f40112a7ef2420ef8519a33e65f719fe29844632f13f4` |
| **`nsmbw_g6c3m.elf`** (para el perfil) | `1e92352d6495bb91a3fa8ed43a6c820db94c6fb852f4cd986c3783430d8b69ef` |
| `nsmbw_g6c3m_input_test.nro` (**solo pruebas, nunca como juego**) | `bf142d198f975230cd4931ab689015c96149fd98f30cee7fbf40ef2dbd25354b` |
| `NSMBW_g6c3m(_input_test)` (PC) | ver `runs/bin_sha256_g6c3m.txt` |

Qué esperar: la medida estática baja un 3,2 % de instrucciones y ~20 % de los accesos a `ctx` (y 9 % de
los almacenes en pila) en las funciones con muestras; si el coste está en las ráfagas de almacenes, del
orden de −0,5 a −1 ms/fotograma del hilo del juego frente a m2 (≈ 2-5 % del código generado). Lo que lo
puede comer: la presión de registros en funciones enormes con bucles (`func_800ECAE0`) y el camino
`g6_slowret` de las funciones que entran por `nwii_dispatch` (llamada de cola extra en cada `return`,
en vez del marco del envoltorio de c1/c2). Al medir, pedir el perfil (`profile_cpu.txt`) y pasar
`tools/prof3.sh`/`tools/fndelta.py` (adaptando las rutas) contra el de m2.

`stale/`: lo descartado (pasadas con fallo, sin objetos ni exportaciones; solo `pass.log` y las corridas
malas en `stale/runs_bugP`). Se puede borrar.
