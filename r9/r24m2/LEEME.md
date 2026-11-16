# r24m2: troncal m2 = runtime de r7f + código generado de G6 e2 (10-nov-2026)

Sin commits. No se tocó ninguna otra carpeta de `r9/` (lo necesario se copió aquí), ni `~/nsmbw-port/nand`,
ni la consola. Solo se enlazó: no se compiló nada (las piezas del runtime son las de r7f ya compiladas).

## Qué es

- **Runtime:** r7f (`r9/r22r7`) = s1 (m1 + forcedemo) + memo/índice del worker de render de R7 + sus claves en
  `keeps_zero` de `switch_platform.cpp`. Piezas copiadas en `rt/`:
  `rt/nx/{main.o, libnwiiruntime.a, libglad.a, input_script_nx.o}` y `rt/pc/{main.cpp.o, libnwiiruntime.a,
  input_script.o}` (sha256 en `runs/rt_sha256.txt`; fuente de referencia en `nWiiRuntime_r7f_src/`).
- **Código del juego:** G6 e2 (`r20g6/e2/nx_obj`, `pc_obj`, 87 `output_*.o`, solo lectura); el resto
  (fastdispatch, main_output) de `r14g5`, igual que g6e2s.
- **Orden L:** el mismo de g6e2s (`r20g6/nx/hot/order/g6e2.txt`, copiado en `nx/order/`; idéntico al de `r21s`).
- Las cabeceras que incluye el código generado (`cpu_context.h`, `config.h`, `nwii_env.h`, `event_scheduler.h`,
  `hw/hw.h`) son idénticas entre r22r7 y r20g6; R7 solo cambió `gx_command.h` y `fifo_parser.h`, que el código
  generado no incluye.

## Cómo se hizo (`tools/build_m2.sh`, igual que `r21s/tools/build_g6s.sh` pero con el runtime de r7f)

```
cd ~/nsmbw-port/r9/r24m2 && JOBS=2 ~/nsmbw-port/scripts/lim /home/djvemo/nsmbw-port/r9/r24m2/tools/build_m2.sh
```

1. Comprobación de que `rt/` es r7f: con las copias se re-enlaza la línea de r7f (`nx/ref/link_r7f_dk3drel.cmd`
   + su orden) → **idéntico byte a byte a `r22r7/bin/nsmbw_r7f.elf`**; en PC la línea de r7e (en PC r7f = r7e)
   → **idéntico byte a byte a `r22r7/bin/NSMBW_r7e`**. (Ojo: libnx guarda el nombre del archivo de salida en
   `.nx-module-name`; por eso el `.elf` se enlaza siempre como `nsmbw.elf`.)
2. Switch: `r20g6/nx_link_g6e2.cmd` con main.o/libnwiiruntime.a/libglad.a de m1 cambiados por los de `rt/nx`
   + `--section-ordering-file=nx/order/g6e2.txt`. El guion comprueba que, quitando los `output_*.o`, la línea
   de m2 es **exactamente** la de r7f. Línea guardada en `nx/link_m2.cmd`, mapa en `nx/m2/game/nsmbw.map`.
3. PC: `r20g6/pc_link_g6e2.cmd` con las piezas de `rt/pc` (`nx/pc_link_m2.cmd`).

Comprobado además con `nm`: el orden de todas las `func_*` en `nsmbw_m2.elf` es idéntico al de
`r21s/bin/nsmbw_g6e2s.elf` (p. ej. `func_800ECAE0` en la misma dirección); el binario tiene las cadenas de las
claves de R7 (`NWII_DLMEMO_LIST`, `NWII_PINDEX`, `NWII_RENDER_FREE`) y de forcedemo.

## Binarios (`bin/`, sha256 en `runs/bin_sha256_m2.txt`)

| Archivo | Bytes | sha256 |
|---|---|---|
| **`nsmbw_m2.nro`** (el juego, GL; NACP "NSMBW PRUEBA m2 (r7f + G6 e2)") | 79 845 573 | `beb5962def186620827fc2675e73166b34e9a172966b16f288d1a39dcaade93b` |
| `nsmbw_m2.elf` | 111 358 616 | `8cf61b99d579d016a83c0c5e8a749e0540710fd486082da39393306eca4e6eaa` |
| `nsmbw_m2_input_test.nro` (**solo pruebas, nunca como juego**) | 79 857 861 | `02df3384a0c73b749b82d4f2818f3a134664cacae4f1dc6ce0b8ddb59a42b88f` |
| `NSMBW_m2` (PC) | 94 634 928 | `89dcfe4492612d6f33f70311787d68ed6c19007381780945e8cf3c68418d65af` |
| `NSMBW_m2_input_test` (PC) | 94 655 032 | `4788d7df07e1c8839c721e0c91a7a17f3181b22725f348604fcbfc62462e9e94` |

## Verificación en PC (`tools/verify_m2.sh m2 checks sanity title cb4 off wcheck lista`)

Mismo arnés que antes (`render/pc/run.sh`, NAND privada `nand_tpl_mp`, cada corrida con `render/pc/.runlock`).
Resultados en `runs/<paso>_m2.txt`, hojas de capturas `runs/hoja_*_m2.jpg` y `runs/lista_m2/hoja.jpg`.

| Prueba | m2 | Referencia |
|---|---|---|
| `checks.sh` 1x (FRAMECHECK+ECACHE) + `NWII_PINDEX_CHECK` + `NWII_DLMEMO_CHECK` | title 140/140, map 80/80, walk 80/80, lvl11 200/200 idénticas, **`diff_lines=0`** en las 4; índice 0 diferencias; memo 166 M dibujos re-decodificados, **0 diferencias**; FASTMEM 0 fallos | r7e y g6e2: 200-220/220, `diff_lines=0` |
| sanity (165 s) | mismas escenas (título → mapa → 1-1), 54,2 fps, FASTMEM 0, sin cuelgues, resume_pruned 28 | g6e2: 56,5 fps, 27 |
| title (200 s) | mismas escenas (MOVIE + 4 demos), 55,7 fps, FASTMEM 0 | g6e2: 58,7 |
| cb4 (200 s) | MULTI_PLAY_COURSE_SELECT → nivel, 55,1 fps, FASTMEM 0 | g6e2: 59,7 (llegó a una 2.ª selección) |
| off (worker y GPUXF apagados, 120 s) | mismas escenas, 59,2 fps, FASTMEM 0 | — |
| wcheck (`NWII_WGP_CHECK`, 100 s) | 117 M escrituras, 0 violaciones; escrito = entregado (336 008 355 B, prefijo igual) | r7d: igual (336 MB) |
| lista (forcedemo, LOOP, NO_SAVE, 300 s) | desierto4j → agua_pinguino → cielo_paratroopas → nubes_plataformas → castillo, "lista completa"; 59,6 fps, FASTMEM 0 | g6e1s_lista: igual |

Imágenes revisadas (lista, title, cb4): limpias, sin vértices disparados ni basura.

Menos frames comparados en `checks` y fps algo más bajos en sanity/title/cb4: la PC estaba cargada durante las
corridas (otra cadena de compilación `nsmbw-g6c1`, ffmpeg al 576 % de CPU, otras corridas del juego); `off`,
`wcheck` y `lista` (más tarde, con menos carga) dan 59-60 fps. No es una diferencia de imagen ni de estado.

## Ojo / pendiente

- **El orden L de g6e2 no conoce las funciones nuevas del worker de R7** (19 funciones: `ReplayMemo`,
  `FifoParser::Parse` nueva, `ChunksHash`, `Retire`, `DLMemoOn`, `RangesHashSpan`, `push_back` de `GXCommand`…).
  En m2 quedan al final, frías (~0x388xxxx), mientras que en r7f iban junto a `PrepFrame` (~0xC9xxxx). Funciona
  igual, pero el hilo gxw de la Switch podría rendir algo menos que en r7f. Si la consola lo nota: hacer un orden
  m2 propio (`build_hot.sh` con los perfiles de `r22r7/nx/hot/profiles.txt` + los de G6, o insertar esas
  secciones tras `PrepFrame` en `g6e2.txt`); solo es un re-enlace.
- Sin medir en la consola.
