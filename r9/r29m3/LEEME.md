# r29m3: troncal m3 = m2 + frente X1 + frente D1 (10-nov-2026)

> **Resumen.** m3 = **m2** (`r9/r24m2`: runtime r7f + código generado G6 e2 + orden L) + las **22 nativas de X1**
> (`r9/r26x1`: nw4r/SDK/MTX/MSL) + las **3 nativas de D1** con el índice ID → nodo (`r9/r28d1`: searchNodeByID,
> searchBaseByID, searchNodeByProfName). Verificado en PC: checks 1x idénticos (`diff_lines=0` en las 4), sanity/title/cb4
> sin cuelgues, FASTMEM 0, lista de las 5 demos completa, `NWII_NATIVE_VERIFY` **5 131 comparaciones / 0 malas**,
> `NWII_D1_CHECK` **9,30 M búsquedas / 0 malas** y ProfName **62 951 / 0 malas**; capturas limpias (iguales a las de m2).
> NRO: **`bin/nsmbw_m3.nro`** (GL por defecto). Sin medir en la consola.

Sin commits. No se tocó ninguna otra carpeta de `r9/` (solo lectura), ni `~/nsmbw-port/nand`, ni la consola. Todo lo
compilado/enlazado pasó por `scripts/lim` con `JOBS=2`.

## 1. Qué hay aquí

| Ruta | Qué es |
|---|---|
| `nWiiRuntime/` | runtime r7f (`r24m2/nWiiRuntime_r7f_src` = `r22r7/nWiiRuntime`) + X1 + D1. Diferencias: `m3_runtime_vs_r7f.patch` |
| `export/` | los 7 `output_*.cpp` con ganchos; el resto son enlaces a `r20g6/e2/export` (sin cambios). Diferencias: `m3_export_vs_g6e2.patch` |
| `nx/build`, `pc_build`, `pcproj` | compilación del runtime (Switch / PC), misma configuración cmake que r22r7/r26x1/r28d1 (toolchain y export de r14g5) |
| `nx_obj/`, `pc_obj/` | los 7 objetos del juego recompilados (`output_21 25 26 27 34 43 44`), con el comando exacto de `r20g6/e2/{nx_obj,pc_obj}/compile.cmd` |
| `nx/order/m3.txt` | orden L (ver §3) |
| `nx/link_m3.cmd`, `nx/pc_link_m3.cmd`, `nx/m3/game/nsmbw.map` | líneas de enlace y mapa |
| `tools/` | `build_m3.sh`, `mk_order.py`, `verify_m3.sh`, `verify_g5_copy.sh`, copias de `apply_x1.py` y `apply_d1.py` |
| `runs/` | resultados de verificación, `build_m3.log`, `bin_sha256_m3.txt`, `rt_sha256_m3.txt`, `mods.txt` |

## 2. Cómo se juntó

### Runtime (sin conflictos reales)
- Los `native_n1.h` / `native_n1.cpp` de X1 y de D1 son **idénticos** (los dos copiados de r19n): uno solo.
- Copiados de X1: `native_n1_fp.h`, `native_n1.h`, `native_n1.cpp`, `native_n2.cpp`, `native_x1.cpp`; de D1: `native_d1.h`,
  `native_d1.cpp`. `CMakeLists.txt`: las 4 fuentes (`native_n1/n2/x1/d1.cpp`).
- `switch_platform.cpp` (`keeps_zero`): la unión de los dos: `key.rfind("NWII_NATIVE", 0) == 0 || key.rfind("NWII_D1", 0) == 0`
  (cubre `NWII_NATIVE`, todas las `NWII_NATIVE_<NOMBRE>`, `_VERIFY/_STATS/_TIME`, y `NWII_D1_INDEX/_CHECK/_STATS`): en la
  Switch un "0" sí apaga cualquiera de las 25 nativas o el índice.
- Registro: no hay tabla central; cada gancho lleva su `N1Slot` estático en el `output_*.cpp`. X1 excluye 80162C30, así que
  `SEARCHNODEBYID` solo lo engancha D1 (`d1h_80162C30`); los nombres de las 25 ranuras no se repiten (ver `runs/lista_m3.txt`:
  las 25 aparecen en `[N1] ... final`).
- Cabeceras que incluye el código generado (`cpu_context.h`, `config.h`, `nwii_env.h`, `event_scheduler.h`, `hw/hw.h`):
  idénticas a r14g5 (el guion lo comprueba).
- Comprobación de la compilación (`nx/check/lib_*_vs_r7f.txt`): en el `libnwiiruntime.a` nuevo, **todos** los miembros son
  idénticos byte a byte a los de r7f (`r24m2/rt/`) salvo los `native_*` nuevos, `switch_platform.o` (keeps_zero) y, en la
  Switch, `switch_profiler.o` (solo lleva `__DATE__/__TIME__` de la compilación). `main.o` (Switch) y `main.cpp.o` (PC)
  idénticos a r7f; `libglad.a`: su `glad.o` es idéntico (el archivo .a difiere solo en metadatos).

### Código generado (copia de `r20g6/e2/export`)
1. `python3 tools/apply_x1.py export --orig ""` → los mismos 22 ganchos que en r26x1 (output_25/26/27/34/43/44).
2. `python3 tools/apply_d1.py export` → 3 ganchos + 6 `d1_touch` en `output_21.cpp`. **G6 e2 conserva lo que necesita D1**:
   las etiquetas `loc_80161020` (×2), `loc_80160FE0` (×2), `loc_80160F60`, `loc_80160F10` están en las mismas funciones que en
   r22r7 (`func_80161020/80160FE0/80160F60` y las copias fusionadas en `func_80162B40`), con `G5r3/G5r4/G5r5` declarados (compila
   sin cambios). No hubo que adaptar los ganchos.
3. **Ojo con el "inline" de G6 e2** (comprobado con las marcas `[g6i]`):
   - Las 4 operaciones de lista (addTop/addLast/insert/removeLineNode) **no** se copian en ningún llamador (sus instrucciones
     `// 0x80161020:` etc. solo aparecen en `output_21.cpp`, igual que en r22r7) → todas las altas/bajas pasan por `d1_touch`;
     el índice sigue exacto (guard = 0, check 0 malas).
   - En cambio G6 copió **searchBaseByID (80162D00) en 22 sitios, searchNodeByID (80162C30) en 2** (uno de ellos dentro del
     propio 80162D00), **ProfName (80163090) en 1** y getTreeNext (801616C0) en 5. Esas copias son código generado: correctas
     (solo leen las listas), pero **no pasan por la nativa**. Igual que X1 con `__cvt_fp2unsigned` (81 sitios) y DC* (15/10).
     Se deja así (no se regeneró e2). Si se quiere el ahorro completo de D1: quitar `func_80162D00`, `func_80162C30`,
     `func_80163090` (y `__cvt_fp2unsigned`, `DCInvalidateRange`, `DCFlushRange`) de `r20g6/tools/inline_callees.txt` al
     regenerar.
   - Las paradas de reanudación que usan las nativas (0x80162C50, 0x80162D24, 0x801630B4/D8/DC, 0x801616E0, 0x80161734)
     siguen como `case` en los `switch` de entrada de e2.

### Enlace
- La línea de m2 (`r24m2/nx/link_m2.cmd`, `pc_link_m2.cmd`) con main.o / libnwiiruntime.a / libglad.a nuevos y los 7 objetos
  nuevos en el mismo lugar (mismo nombre `output_N.o`, así el orden L los reconoce). El `_input_test` usa los
  `input_script*.o` de m2.

## 3. Orden L

`nx/order/m3.txt` = `r24m2/nx/order/g6e2.txt` + **17 secciones** de `libnwiiruntime.a` que el orden de r7f
(`r24m2/nx/ref/r7f_dk3drel.txt`) tiene y g6e2 no (el worker de render de R7: `ReplayMemo`, `FifoParser::Parse` nueva,
`RangesHashSpan`, `Retire`, `DLMemoOn`, `SetFrameSeq`, `ReleaseRetired`, `push_back` de `GXCommand`, `TrimBuffers`,
`RetractToPrefix`, `wprof::Stage::publish`, varios `vector`...) insertadas **justo después de `PrepFrame`**, en el orden de
r7f (`tools/mk_order.py`). Resultado: `ReplayMemo` en 0xD285C0 junto a `PrepFrame` (0xD22FE0), en m2 estaba en 0x3885850.
(El LEEME de m2 hablaba de 19; en el orden de r7f son 17 las que faltan — `ChunksHash` ya era fría en r7f.)
Las nativas de X1/D1 quedan fuera del orden (≈0x388xxxx), igual que en r26x1 y r28d1 (donde se midieron así).
El resto del orden es exactamente el de m2.

## 4. Compilar

```
cd ~/nsmbw-port/r9/r29m3
JOBS=2 ~/nsmbw-port/scripts/lim /home/djvemo/nsmbw-port/r9/r29m3/tools/build_m3.sh     # ~10 min; log en runs/build_m3.log
```
(Para reaplicar desde cero: copiar `r20g6/e2/export` a `export/`, correr `apply_x1.py export --orig ""` y `apply_d1.py export`.)

## 5. Binarios (`bin/`, sha256 en `runs/bin_sha256_m3.txt`)

| Archivo | Bytes | sha256 |
|---|---|---|
| **`nsmbw_m3.nro`** (el juego, GL; NACP "NSMBW PRUEBA m3 (m2 + X1 + D1)") | 79 927 493 | `ba5444342f5f4394d8dee6ca5dfe04a39cc9bd09593d1af81713d0f17939b038` |
| `nsmbw_m3.elf` | 111 457 312 | `bb6a22359f4b506a4c77a963852bd7d80c2949bf817ab16f7af83fe61d29f5a2` |
| `nsmbw_m3_input_test.nro` (**solo pruebas, nunca como juego**) | 79 935 685 | `f3c32578c5ad81b9cde541a183b249b5f904e83ca9d16235117a91c67e875159` |
| `NSMBW_m3` (PC) | 94 786 728 | `7e3c9129ebb223d614eb16c87446fc45b2b52903144fac4e55dcdcc0a8dd8fe0` |
| `NSMBW_m3_input_test` (PC) | 94 806 832 | `fa155d2e27a9887b9abc50ed194b61fc0d2db09cc048dbcc40c5e5d4decbe73a` |

## 6. Verificación en PC (`tools/verify_m3.sh m3 checks sanity title cb4 lista vnative d1check`)

Mismo arnés que m2 (`render/pc/run.sh`, NAND privada `mp`, `.runlock`). Resultados en `runs/<paso>_m3.txt` y `runs/<paso>_m3/`.

| Prueba | m3 | m2 |
|---|---|---|
| `checks.sh` 1x + `NWII_PINDEX_CHECK` + `NWII_DLMEMO_CHECK` | title 140/140, map 80/80, walk 80/80, lvl11 200/200 idénticas, **`diff_lines=0`** en las 4 ("pass1 differs 100" en lvl11, igual que m2); índice 0 diferencias; memo 173 M dibujos, 0 diferencias; FASTMEM 0 | idéntico (también 100 en lvl11) |
| sanity (165 s) | título → mapa → 1-1, 54,5 fps, FASTMEM 0, sin cuelgues (longest_no_frame 0 s), resume_pruned 28 | 54,2 fps, 28 |
| title (200 s) | MOVIE + 4 demos, 54,0 fps, FASTMEM 0 | 55,7 |
| cb4 (200 s) | MULTI_PLAY_COURSE_SELECT → nivel, 57,8 fps, FASTMEM 0 | 55,1 |
| lista (forcedemo, LOOP, NO_SAVE, 300 s, `NWII_NATIVE_STATS` + `NWII_D1_STATS`) | desierto4j → agua_pinguino → cielo_paratroopas → nubes_plataformas → castillo, **"lista completa"**; 56,0 fps, FASTMEM 0; las 25 nativas en uso (p. ej. PSMTXConcat 5,89 M, SinFIdx 13,7 M, searchBaseByID 8,82 M nativas); índice: 9,30 M búsquedas, k medio 8,5, 546 reconstrucciones, **guard 0** | igual recorrido, 59,6 fps |
| vnative (`NWII_NATIVE_VERIFY=2000`, `NWII_AX_THREAD=0`, 300 s, ~10 fps) | **5 131 comparaciones, 0 malas** (las 25 ranuras; SEARCHBASEBYID 603, SEARCHNODEBYID 49, STRCMP 1 668...; GXLoad*MtxImm no se comparan por diseño: escriben el FIFO). Por la lentitud solo llega a desierto4j | — |
| d1check (`NWII_D1_CHECK=1`, 300 s) | **9 302 405 búsquedas comparadas con el recorrido, 0 malas**; 5,52 M sondeos lentos en estado exacto, diverged 0; **ProfName 62 951 ok / 0 malas**; lista de 5 demos completa, 56,6 fps | — |

Capturas revisadas (`runs/lista_m3/hoja.jpg`, `d1check_m3/hoja.jpg`, `hoja_title_m3.jpg`, `hoja_cb4_m3.jpg`,
`hoja_sanity_m3.jpg`, `vnative_m3/hoja.jpg`): limpias, sin vértices disparados ni basura; la hoja de la lista es igual cuadro
a cuadro a la de m2 (los negros son cargas). Las diferencias de fps entre corridas son ruido de carga de la PC.

## 7. Ojo / pendiente

- **Sin medir en la consola.** Lo esperado: X1 ~0,5-1 ms/frame + D1 ~0,1 ms (menos que en r28d1, porque las 22 copias de
  searchBaseByID de G6 no pasan por la nativa). A/B con el mismo NRO: `NWII_NATIVE="0"` apaga las 25; `NWII_D1_INDEX="0"` solo
  el índice; `NWII_NATIVE_<NOMBRE>="0"` una.
- Copias de G6 que saltan la nativa (§2.3): se pueden recuperar regenerando e2 sin esos callees en `inline_callees.txt`.
- Las nativas quedan frías en el orden L; si la consola muestra fallos de i-caché/TLB en ellas, añadir sus secciones
  (`native_n2.o`, `native_x1.o`, `native_d1.o`, `native_n1.o`) al orden.
