# r28d1: piloto D1 — búsquedas de fBase en nativo + índice exacto ID → nodo (frente D, 10-nov-2026)

> **Resumen.** Base: **r7f** (`r9/r22r7`; `r9/r24m2` no estaba terminado: `bin/` vacío y sin LEEME). Tres funciones
> del juego (decompiladas en NSMBW-Decomp) pasan a nativo **exacto**: `fLiMgBa_c::searchNodeByID` (80162C30) con un
> **índice ID → nodo** que devuelve el mismo nodo, en la misma posición, que la búsqueda lineal; `fManager_c::searchBaseByID`
> (80162D00) que lo usa; y `fTrMgBa_c::searchNodeByProfName` (80163090, recorrido del árbol fusionado con sus dos
> llamadas). **Verificado en PC: 0 diferencias** (≈46 M búsquedas comparadas con el recorrido real, 4 821 llamadas
> comparadas registro a registro y RAM con el código generado, 120 600 llamadas de ProfName comparadas con el
> generado). **PC: searchBaseByID 391 → 194 ns/llamada** (lineal nativa 246), ProfName 6,39 → 6,06 µs. Ahorro estimado
> en la consola: **~0,1-0,15 ms/frame** (la mitad del ~1 % que pesan; el resto son los sondeos de interrupción, que
> son semántica y se mantienen). NRO: **`bin/nsmbw_d1.nro`** (+ `.elf`), orden L, GL por defecto.

Sin commits. No se tocó `r9/r26x1` ni `r9/r27x2` (ni nw4r/SDK math ni escritura GX), ni `nand/`, ni la consola.
Todo lo compilado/enlazado pasó por `scripts/lim` con `JOBS=2`.

## 1. Qué hacen estas funciones

`fManager_c::m_searchManage[8]` (USA `0x80377A70`, 8 `cLineMg_c {first, last}`) guarda el `mSearchNode` de cada
`fBase_c` en la lista `id & 7`; el constructor de `fBase_c` lo mete con `addTopLineNode` y `postDelete` lo saca con
`removeLineNode`. `searchNodeByID` recorre la lista desde `first` hasta el primer nodo con `owner->mUniqueID == id`.
Medido (PC, demos forzadas): ~36 000 búsquedas/s (≈600 por frame), 92 % aciertos, **posición media 7-9**, fallos tras
7-9 nodos; las listas cambian poco (≈600 reconstrucciones del índice en 5 min). `searchNodeByProfName` recorre el
árbol de conexión en preorden (≈250 llamadas/s, pero ~6 µs cada una: recorre cientos de nodos).

## 2. Cómo se reemplazan (patrón del frente N, `r9/r19n`)

- `tools/apply_d1.py export` (sobre la `output_21.cpp` limpia de r22r7 = r18m, sha256 `da1ec7bf…`; se niega a correr
  dos veces) pone al principio de las 3 funciones generadas el gancho `N1Slot` de r19n
  (`if (ctx.pc == ENTRY && n1_hook(slot, ctx)) return;`) y, **justo después de cada etiqueta** de
  `cLineMg_c::addTopLineNode / addLastLineNode / insertLineNode / removeLineNode` (`loc_80161020`, `80160FE0`,
  `80160F10`, `80160F60`: las funciones y sus copias fusionadas dentro de `func_80162B40`), `d1_touch(this, node[, prev])`.
  Todas las llamadas del resto del juego a esas funciones pasan por ellas (comprobado: ninguna copia "inlined" de G5
  en otros archivos). Solo cambia `output_21.cpp` (MODS = `output_21 output_24`).
- Runtime: `native_n1.h/.cpp` copiados de r19n (gancho, contadores, `NWII_NATIVE_VERIFY`, `NWII_NATIVE_TIME`, y la
  búsqueda lineal a mano `n1h_80162C30`) + **`native_d1.h/.cpp`** (nuevo). `cpu_context.h` y demás cabeceras del código
  generado **sin tocar**.

### El índice (exacto por construcción)

- Por lista: la lista tal como está en la RAM del invitado (nodo, owner, id de arriba abajo) y un hash
  id → posición del **primer** nodo con ese id (con ids repetidos gana el más cercano a `first`, como el recorrido).
- Coherencia: `d1_touch` marca la lista como sucia si `this` es una de las 8; se reconstruye **perezosamente** en la
  siguiente búsqueda de esa lista (los cuerpos de esas 4 funciones no tienen sondeo de interrupción: ninguna búsqueda
  puede caer a mitad de una operación). Si una operación de **otra** lista usa un nodo de búsqueda (o lo usa como
  `prev`), también marca su lista. Guardas en cada búsqueda: `list->first` debe ser el nodo indexado arriba y, en un
  acierto, `node->owner` y `owner->id` deben seguir siendo los indexados; si no, se reconstruye (contador `guard`:
  **0** en todas las corridas). Punteros fuera de RAM normal o > 65 536 nodos → la lista no se indexa (búsqueda lineal).
- Resultado = todo lo que deja el código generado: hit en k → r3 = nodo, r5 = owner, r0 = id, CR0 eq,
  `inst_count` +8+7k; fallo tras n nodos → r3 = 0, r0/r5 del último nodo (o intactos si n = 0), +6+7n; `pc = lr`.
- **Sondeos de interrupción** (k+1 o n+1, en 0x80162C50): los que la cuenta atrás haría "en silencio" se descuentan en
  bloque; con el reloj de pared hay uno lento cada 16 (≈0,5 por búsqueda). Ese se hace **de verdad** con ctx en el
  estado exacto de esa prueba (r3 = nodo t, r0/r5 del nodo t−1, `inst_count`, CR0); si entrega una interrupción, el
  código generado reanuda en 0x80162C50 desde ahí. Si un sondeo lento cambiara registros o la lista, el resto sigue con
  la semántica del bucle generado sobre la lista real (`diverged`: **0** en todas las corridas).
- `searchBaseByID`: prólogo (stwu, LR en la pila), la búsqueda de arriba con `lr = 0x80162D24`, y epílogo; si la
  búsqueda tomó una interrupción, vuelve como el generado y este reanuda en D24.
- `searchNodeByProfName`: **sin índice** (el orden del árbol cambia con cada alta/baja); `getTreeNext` (801616C0, con su
  marco de pila) y `getTreeNextNotChild` (80161700) fusionados en un bucle nativo con los registros en locales, las
  mismas escrituras de pila en el mismo orden, el `inst_count` por bloque de G5 y los sondeos (0x801630DC, 0x80161734);
  un sondeo lento deja ctx exactamente como el generado (dentro de las llamadas anidadas: marcos escritos,
  `lr = 0x801616E0`), y se reanuda por los `case` de los generados.

### Interruptores

| Clave | Efecto |
|---|---|
| `NWII_NATIVE_SEARCHNODEBYID / _SEARCHBASEBYID / _SEARCHNODEBYPROFNAME = "0"` | esa función vuelve al código generado; `NWII_NATIVE="0"` todas |
| `NWII_D1_INDEX = "0"` | sin índice: búsqueda lineal nativa (r19n) |
| `NWII_D1_CHECK = "1"` | cada búsqueda indexada recorre además la lista y compara nodo, k, n y último nodo (`[D1] CHECK MISMATCH`); cada ProfName corre nativo y generado sobre una copia de registros + 40 B de pila y compara (`[D1] searchNodeByProfName CHECK MISMATCH`) |
| `NWII_D1_STATS = "1"` | contadores (búsquedas, aciertos, k medio, reconstrucciones, guardas, sondeos lentos) |
| `NWII_NATIVE_STATS / _VERIFY / _TIME` | los de r19n (verify: instantánea de toda la RAM, solo PC) |

En la Switch, todas las claves `NWII_NATIVE*` y `NWII_D1*` están en `keeps_zero` (`switch_platform.cpp`): "0" sí apaga.

## 3. Verificación (PC, `runs/`)

Lista fija de demos de S (`desierto4j, agua_pinguino, cielo_paratroopas, nubes_plataformas, castillo`, sin título entre
demos), `tools/d1_run.sh` (= `r21s/tools/demo_run.sh`, toma `.runlock`).

| Corrida | Resultado |
|---|---|
| `check1` (300 s, versión 1) | 14,76 M búsquedas comparadas con el recorrido: **0 malas**; guard 0 |
| `verify1` (`NWII_NATIVE_VERIFY=300`, `NWII_AX_THREAD=0`, 300 s; ~12 fps por las instantáneas) | SEARCHBASEBYID **4 791 ok / 0 malas**, SEARCHNODEBYID 28 / 0, ProfName 2 / 0 (ver nota) ; + 2,07 M búsquedas del índice comparadas: 0 malas |
| `check2` (320 s) | 12,67 M búsquedas: **0 malas**; 6,49 M sondeos lentos hechos en estado exacto, 0 divergencias; 59,1 fps |
| `check3` (330 s, versión final) | 13,10 M búsquedas: **0 malas**; ProfName **91 206 ok / 0 malas** (88 538 de ellas con sondeos lentos) |
| `check_lvl11` (150 s, versión final; las fases del guion no se aplicaron: título + demos naturales) | 3,72 M búsquedas: 0 malas; ProfName 29 421 ok / 0 malas |
| capturas (`runs/*/hoja.jpg`) | revisadas: demos y título normales (los negros son cargas) |

Nota sobre `NWII_NATIVE_VERIFY` con ProfName: la instantánea de 88 MB tarda tanto que el siguiente sondeo lento entrega una
interrupción dentro del recorrido (cientos de nodos) y esas llamadas no se pueden comparar; por eso ProfName tiene su
propia comparación ligera en `NWII_D1_CHECK` (copia de registros y de los 40 B de pila que puede escribir).
En `check2` las 1 155 "malas" de ProfName eran relleno sin inicializar de la estructura comparada (corregido; `check3` = 0).

## 4. Tiempos en PC (`NWII_NATIVE_TIME=1`, 300 s cada uno, mismo binario, perfil `free`)

| Función | generado | nativo lineal (`NWII_D1_INDEX=0`) | **nativo + índice** |
|---|---|---|---|
| searchBaseByID (≈36 000/s) | 391 ns ¹ | 246 ns | **194 ns** |
| searchNodeByID llamada directa (≈56/s) | 256 ns ² | 509 ns | 298 ns |
| searchNodeByProfName (≈250/s) | 6 392 ns | — | 6 060 ns (×1,05) |
| **total por segundo** (base + ProfName) | ≈15,8 ms/s ¹ | ≈10,5 ms/s | **≈8,6 ms/s** |

¹ en "generado" el tiempo de searchBaseByID incluye el gancho cronometrado de la búsqueda anidada (dos lecturas de reloj
+ `n1_hook_slow`, ~40-50 ns): el generado real ronda 340-350 ns. ² media de todas las llamadas (directas y anidadas).
Todas las cifras llevan ~40 ns de las dos lecturas de reloj. Lo que queda en los 194 ns es sobre todo el sondeo lento
(≈0,5 por búsqueda: `process_pending_callbacks_slow` refresca el reloj), igual que en el generado.

Por frame en PC: ≈600 búsquedas → de ~0,2 ms a ~0,12 ms. **Consola (estimación)**: el perfil de r7f a 1020 MHz pone estas
tres en 1,21 % del hilo del juego ocupado (searchNodeByID 0,87 + searchBaseByID 0,14 + ProfName 0,20) ≈ 0,3 ms/frame; con
la proporción de PC (búsquedas por ID ~45 % menos; ProfName ~5 %) el ahorro esperado es **~0,1-0,15 ms/frame**. Para
medirlo: A/B del mismo NRO con `NWII_NATIVE="0"` (o `NWII_D1_INDEX="0"`) y `NWII_NATIVE_TIME="1"` en las dos.

## 5. Compilar / reaplicar

```
cd ~/nsmbw-port/r9
python3 r28d1/tools/apply_d1.py r28d1/export          # sobre output_21.cpp limpia de r22r7 (ya hecho)
env BASE=$PWD/r14g5 MODS="output_21 output_24" JOBS=2 ~/nsmbw-port/scripts/lim r28d1/tools/build_m.sh r28d1 d1 pc
env BASE=$PWD/r14g5 DK_LIB=deko3d MODS="output_21 output_24" JOBS=2 ~/nsmbw-port/scripts/lim r28d1/tools/build_m.sh r28d1 d1_dk3drel nx
cd r28d1 && M=$PWD NACP_NAME="NSMBW PRUEBA d1" ~/nsmbw-port/scripts/lim tools/build_hot.sh d1_dk3drel $(cat nx/hot/profiles.txt)
# bin/nsmbw_d1.nro/.elf/_input_test.nro = copias de bin/nsmbw_d1_dk3drel_hot* (orden L; eqcheck: 0 funciones distintas)
```

Parches: `d1_runtime_vs_r22r7.patch` (`diff -ruN r22r7/nWiiRuntime r28d1/nWiiRuntime`: CMakeLists, `native_n1.*`,
`native_d1.*`, `switch_platform.cpp`) y `d1_export_vs_r22r7.patch` (`output_21.cpp`; mejor `tools/apply_d1.py`, que busca
por etiqueta/dirección). **Sobre m2 (G6 e2)**: el runtime se aplica igual; `apply_d1.py` necesita que el export G6 conserve
las etiquetas `loc_8016…` y los locales `G5r3/G5r4/G5r5` en esas funciones (si G6 los renombra, ajustar los argumentos de
`d1_touch`); el resto vive en el runtime mientras el contrato de `CPUContext` (registros, `inst_count`, sondeo) no cambie.

## 6. Binarios (`bin/`)

| Archivo | Qué es |
|---|---|
| **`nsmbw_d1.nro`** (+ `nsmbw_d1.elf`) | **r7f + D1** (índice + 3 nativas), orden L, GL por defecto. Para medir en la consola |
| `nsmbw_d1_input_test.nro` | solo pruebas con entrada por guion. **Nunca como juego** |
| `nsmbw_d1_dk3drel*.nro/.elf` | lo mismo sin renombrar / sin orden |
| `NSMBW_d1`, `NSMBW_d1_input_test` | PC |

sha256: `runs/bin_sha256_d1.txt`.

## 7. Conclusión y siguientes pasos

- La idea funciona y es exacta, pero el techo es pequeño: estas tres funciones son ~1,2 % del hilo; el índice quita el
  recorrido (k≈8 nodos) pero **no los sondeos de interrupción** (uno lento cada 16), que son parte de la semántica.
  Coincide con r25dec: D no es un frente principal.
- ProfName apenas gana (×1,05): el coste es perseguir punteros por cientos de nodos. Un índice exacto (primer nodo en
  preorden con ese perfil a partir de una posición) necesitaría ganchos en `cTreeMg_c::addTreeNode/removeTreeNode`
  (ojo: `func_80161380` está "inlined" en `output_10.cpp`), posiciones en preorden con sumas de `inst_count` y sondeos, y
  materializar el estado anidado en cada sondeo lento: más trabajo para ~0,05 ms/frame.
- Mismo patrón (índice + ganchos en las operaciones de lista) sirve para `countNodeByProfName` o búsquedas por grupo si
  algún perfil de escena las muestra.
