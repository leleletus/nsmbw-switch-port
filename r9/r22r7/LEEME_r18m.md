# r18m: m1 = todo junto (frente M, 8-nov-2026)

Una sola compilación con todas las mejoras que ya funcionaron en la consola. Sin commits.
No se tocó ninguna carpeta de otro frente: lo que hacía falta se copió aquí.

## Qué lleva y de dónde sale

| Pieza | De dónde | Cómo entró |
|---|---|---|
| Runtime base: R5 a13 (hilo de render, prioridad AX 0x2C) + deko3d "dkgl" + resolución interna fraccionaria (RES) | `r9/r16res/r5dkres/nWiiRuntime` (= `r15dk/r5dk` + `r16res/r5dkres/res_vs_r5dk.patch`; comprobado: aplicar el parche a una copia de r5dk da exactamente r5dkres) | copia con `cp -aL` (la carpeta `src/hle/gx/dk` es real, no un enlace; idéntica a `r15dk/nWiiRuntime/src/hle/gx/dk`) |
| W: entrega del write-gather de GX, anillos LOAD_INDX sin candado, perfilador wstack + ventana de muestreo (w3) + caminos en línea de Write32 (w4) | `r9/r16w/combo_r5/nWiiRuntime` | el diff de W frente a `r9/r14r5/nWiiRuntime` son 4 archivos: `src/hle/gx.cpp`, `src/hle/ios.cpp`, `src/hw/hw_cp.cpp`, `src/platform/switch/switch_profiler.cpp`. Ninguno lo toca la base (son idénticos a los de r14r5 en r5dkres), así que **se copiaron tal cual, sin conflictos** (el de `PipeNoteXfb()` ya lo resolvió W en combo_r5). Versión: ver "Versión de W" |
| G5: registros de la Wii en locales de C++ (v3: excluidas las 2 463 funciones de `g5_exclude.txt`, hojas triviales inlineadas) | `r9/r14g5/tools/apply_g5_regs.py` + `g5_exclude.txt` (copiados en `tools/`) y `r9/r14g5/nWiiRuntime/include/runtime/cpu_context.h` (`poll_due`, `NWII_FM_OPND`) | ver "Código generado" |
| A2: reverb AXFX en float (kernel 2 + prefetch) y delay AXFX nativo | `r9/r16a2/a2d_vs_r14r5.patch` (versión final, sha256 `89003979…`, `NWII_AXFX_PFNEXT` apagado por defecto) | ver "Código generado". Su parte de runtime (prioridad AX 0x2C) ya estaba en la base, idéntica |
| L: orden de enlace "lo caliente primero" | `r9/tools/hot_order.py` (+ `_eval`, `_eqcheck`) | re-enlace al final, ver "Orden de enlace" |
| F (PGO) | — | **no** entra (sigue en pruebas) |

Lo que **no** entró: el cambio opcional de `gx.cpp` de G5 (`NWII_WG_BPFLUSH=sync`, apagado por defecto en G5):
W hace lo mismo y más, y su `gx.cpp` es el que manda.

## Código generado (`export/`): G5 + A2

Los dos tocan `export/output_24.cpp`: A2 cambia el bloque nativo de la reverb (`nwii_r9_axfx_fast`, misma
firma; la llamada dentro de `func_801A3880` no cambia) y mete dos ganchos en `func_801A5C10` (delay nativo);
G5 reescribe el manejo de registros de **todas** las funciones `void func_X(CPUContext& ctx)` (y arregló el
`if (...) goto` sin llaves de `func_801A3880`).

**Decisión: primero A2 y después la pasada de G5 sobre el resultado** (la pasada trata los ganchos de A2 como
cualquier llamada que mira `ctx`: vuelca antes y recarga después, con el arreglo de las llaves):

1. `export_a2/export/` = copia de `r9/r13/export` (sin `build/`) + `patch -p1 < r9/r16a2/a2d_vs_r14r5.patch`.
   Comprobado: `output_24.cpp` sale **idéntico byte a byte** a `r9/r16a2/combo_r5/export_mod/output_24.cpp`
   (sha256 `7d04ac97…`).
2. `export/` = copia de `export_a2/export` + pasada G5 v3, con las mismas opciones de v3:
   `python3 tools/apply_g5_regs.py export --orig export_a2/export --eager --entry-fns --exclude tools/g5_exclude.txt --inline-leaves --leaves-from ../r13/export`
   (`--orig` apunta a la copia con A2 porque la pasada exige que cada archivo tenga el sha256 del original).
3. Comprobaciones:
   - La misma orden sobre `r9/r13/export` sin tocar reproduce `r9/r14g5/export` **byte a byte** (86/86 archivos).
   - En `export/`, 85 de 86 archivos son idénticos a los de `r9/r14g5/export`; solo cambia `output_24.cpp`.
   - `diff r14g5/export/output_24.cpp export/output_24.cpp` = exactamente las líneas del parche de A2, salvo
     los dos ganchos de `func_801A5C10`, que salen con su volcado/recarga de G5 (y la recarga tras
     `func_802DCD58` recarga 6 registros más porque ahora están vivos en el gancho). Contadores de la pasada:
     iguales a v3 salvo `esc` +2 (los dos ganchos).
   - **Comparador por función de G5** (`difftest/`, copia de `r9/r14g5/difftest`): A = `output_24` de r13 + A2
     (objeto de PC con las opciones y cabeceras de r13), B = `output_24` de G5 + A2 (el objeto de PC de esta
     compilación); el resto, objetos de r13 / r14g5. Las 369 funciones de `output_24` × semillas 1-3 y 11-12:
     **1 845 iguales, 0 diferencias, 0 NaN, 0 no concluyentes** (`difftest/runs/`). `func_801A5C10` y
     `func_801A3880` sueltas con 16 semillas: 16/16 iguales.
   - Tabla de fastmem (`r9/r14g5/tools/check_fm_tab.py`): 0 entradas mal formadas en PC y Switch; los mismos
     36 ids sin entrada que G5 v3 (código muerto).

## Compilación

Solo se compila el runtime y `output_24`; el resto del código del juego son los objetos ya compilados de G5 v3
(`r9/r14g5/nx/build` y `r9/r14g5/export/build`, que se usan **sin escribir nada**; sus fuentes son idénticas
a las de `export/` y su `cpu_context.h` es el mismo que el de aquí). sha256 de esos objetos al empezar:
`runs/r14g5_objs_sha.txt` (si G5 recompila, hay que comprobarlos o recompilar todo, ver abajo).

`tools/build_m.sh` = `r15dk/tools/variant_build_dk.sh` (runtime + uamlib + libdeko3d) + lo de A2
(`output_24` compilado con las opciones exactas de la base y cambiado en la línea de enlace) + guarda la orden
de enlace de la Switch en `nx/link_<nombre>.cmd`.

```
cd ~/nsmbw-port/r9
B=BASE=/home/djvemo/nsmbw-port/r9/r14g5
env $B JOBS=2 ~/nsmbw-port/scripts/lim r18m/tools/build_m.sh r18m m1 pc                      # bin/NSMBW_m1 (+_input_test)
env $B JOBS=2 ~/nsmbw-port/scripts/lim r18m/tools/build_m.sh r18m m1_dk3d nx                 # libdeko3dd (validación)
env $B DK_LIB=deko3d JOBS=2 ~/nsmbw-port/scripts/lim r18m/tools/build_m.sh r18m m1_dk3drel nx # libdeko3d de release
cd r18m && ~/nsmbw-port/scripts/lim tools/build_hot.sh m1_dk3drel $(cat nx/hot/profiles.txt)    # orden L
           ~/nsmbw-port/scripts/lim tools/build_hot.sh m1_dk3d    $(cat nx/hot/profiles.txt)
# nombres finales (bin/): las de build_m.sh van a sin_orden/; las _hot pasan a ser las principales
#   nsmbw_m1.nro/.elf/_input_test.nro  = copia de nsmbw_m1_dk3drel_hot*  (sin "dk3d": arranca con GL)
#   nsmbw_m1_dk3drel*                  = nsmbw_m1_dk3drel_hot*            (mismos bytes, deko3d por el nombre)
#   nsmbw_m1_dk3d*                     = nsmbw_m1_dk3d_hot*               (libdeko3dd)
tools/verify_m.sh m1 checks wcheck axfx sanity cb4 title off long   # verificación en PC (runs/<paso>_m1.txt)
```

Compilación completa sin depender de r14g5 (si hiciera falta): copiar `export/` y `nWiiRuntime/` a una copia
de `r9/base` y usar su `build_nx.sh` / `build_pc.sh` con `FULL=1 JOBS=2` dentro de `scripts/lim`
(mirar antes `MemAvailable`: `main_output.cpp` solo pide ~3,5 GB).

## Orden de enlace (frente L)

`tools/build_hot.sh <nombre> <perfiles>`: enlaza otra vez sin orden con mapa (comprueba que `.text` sale
idéntico al de `bin/nsmbw_<nombre>.elf`), saca el orden con `hot_order.py --by-thread` + raíces del worker
(como r16l), enlaza con `--section-ordering-file`, y comprueba con `hot_order_eval.py` (mismas 59 267
secciones, ninguna otra sección cambia) y `hot_order_eqcheck.py` (0 funciones distintas: solo cambian campos
de reubicación).

Perfiles (`nx/hot/profiles.txt`; cada uno traducido por nombre desde el `.elf` con el que se tomó, copiados en
`nx/hot/ref/`; cada `.elf` comprobado contra la NRO que corrió la consola por sha256 o por la marca de compilación):

| Perfil | Escena | `.elf` | fallos de traducción |
|---|---|---|---|
| `bench_g5v3_1108_2159` cpu | demo del título | G5 v3 | 8 |
| `bench_mapa_g5v3_1108_2219` cpu | mapa del Mundo 1 | G5 v3 `_input_test` (`r14g5/nx/build/fast_input`, reproduce la NRO 155ee93…) | 23 |
| `bench_w2r5_1108_2203` cpu | demo del título | W w2r5 | 0 |
| `bench_w3r5_desierto_1108_2215` cpu | demo (ventana del desierto) | W w3r5 | 0 |
| `bench_a2dr5_1108_2211` cpu | demo del título | A2 a2dr5 (el de las 21:40) | 19 |
| `bench_res_dk_1_1108_2224` cpu + main | demo del título con deko3d | RES r5dkres_rel | 2 + 2 |

Hilo de render (main): el perfil de deko3d de arriba (`bench_res_dk_1_1108_2224`).
Además, perfiles del hilo de render con GL (`bench_w2r5_1108_2203` main @ w2r5, `bench_a2dr5_1108_2211` main
@ a2dr5): m1 arranca con GL por defecto, así que el código caliente del render con GL (Mesa incluida) también va
delante. Resultado final: nivel 1 juego 3,9 MiB, render 380 KiB; 59 267 secciones iguales, 0 funciones
distintas (`nx/hot/order/*_eval.txt`, `*_eqcheck.txt`).

## Binarios (`bin/`)

| Archivo | Qué es |
|---|---|
| **`nsmbw_m1.nro`** (+ `nsmbw_m1.elf`) | **el juego**: todo junto + orden L, libdeko3d de release, **arranca con GL**. `NWII_BACKEND = "deko3d"` en debug.toml lo pasa a deko3d |
| `nsmbw_m1_dk3d.nro` (+ `.elf`) | lo mismo con libdeko3dd (validación), arranca con **deko3d** por llevar "dk3d" en el nombre (para comparar en la consola) |
| `nsmbw_m1_dk3drel.nro` (+ `.elf`) | **los mismos bytes** que `nsmbw_m1.nro`; solo el nombre hace que arranque con deko3d |
| `*_input_test.nro` | solo pruebas con entrada por guion. **Nunca como juego** |
| `sin_orden/` | las mismas NRO sin el orden L (para medir L por separado) |
| `NSMBW_m1`, `NSMBW_m1_input_test` | PC (GL; dkgl solo existe en la Switch) |
| `sd/cache/dkshcache.bin` | semilla opcional de la caché de shaders de deko3d (la de r5dk) |

sha256: `runs/bin_sha256.txt`. La NACP dice "NSMBW PRUEBA m1 (todo junto)".

**Por qué GL por defecto:** en la consola, deko3d tenía un fallo propio (vértices disparados en la demo
submarina del título). El frente deko3d lo arregló en `r15dk/nWiiRuntime/src/hle/gx/dk/dkgl.cpp` (22:35:
WaitForIdle + invalidar caché de texturas/L2 al empezar cada lista, `dsb sy` antes de enviar, viewport float y
blit de profundidad con escala). m1 lleva **ese** `dkgl.cpp` (sha256 en `runs/dkgl_sha.txt`; el de r5dkres era
el de las 21:22). GL sigue por defecto hasta que la consola confirme el arreglo. RES ya busca
`glViewportIndexedf` con `GX_GLProc(...)` (sirve con deko3d).

El binario de PC no se recompiló tras cambiar `dkgl.cpp`: todo `dkgl.cpp` va dentro de `#ifdef __SWITCH__`, así
que en PC no cambia nada.

## Versión de W

W cambió `gx.cpp`/`switch_profiler.cpp` mientras se hacía la fusión (w2r5 → w3r5 → w4r5). El coordinador pidió
la última que pasara sus pruebas de PC. m1 lleva **w4r5** = w2r5 (la medida en la consola) + ventana de
muestreo del perfilador (`NWII_NX_PROFILE_FROM`/`TO`/`SLOW_MS`, w3) + caminos en línea en `GX_WGPIPE_Write32`
(w4). W la dio por final: sus `checks` a 1x (4 escenas 220/220, `diff_lines=0`) y su corrida sin worker (145 s, 59,5 fps, FASTMEM 0 fallos, IDXSNAP 0 descartados) pasaron; md5 de los 4 archivos comprobados contra los que mandó W. Los 4 archivos de W en m1 son
idénticos a los de `r16w/combo_r5` en el momento de compilar (sha256 en `runs/w_snapshot_sha.txt`).

## Verificación en PC (8-nov 22:21-23:30, `tools/verify_m.sh`, cada corrida con `.runlock`)

Binario `bin/NSMBW_m1_input_test` (= runtime de m1 + G5 + A2; en PC no hay deko3d ni orden L).

| Prueba | Resultado |
|---|---|
| `checks.sh` 1x (FRAMECHECK, ECACHE) | title, map, walk, lvl11: **220/220 idénticas cada una, `diff_lines=0`** (los "pass1 differs 115" de lvl11 salen igual en w2r5/r13) — `runs/checks_m1.txt` |
| W: `NWII_WGP_CHECK` (título 100 s) | 117 M escrituras, **0 violaciones de la regla**, 0 discrepancias de redirección; flujo escrito = entregado (336 MB, prefijo igual) — `runs/wcheck_m1.txt` |
| A2: AX en línea, `NWII_AXFX_KCHECK=1`, `VERIFY=50`, `DVERIFY=20` (200 s) | kernel 2 frente a 1: **51 000/0**; reverb nativa frente al código generado (ya con G5): **1 051/0**; delay nativo: **1 301/0** — `runs/axfx_m1.txt` (+ `axfx_m1/audio.wav`) |
| sanity (título → mapa → 1-1, 165 s) | escenas como r13, 59,6 fps, FASTMEM 0 fallos, sin cuelgues |
| cb4 (Coin Battle 4 jugadores, 200 s) | MULTI_PLAY_COURSE_SELECT → nivel, 59,6 fps, FASTMEM 0 fallos |
| title (demos del título + MOVIE, 200 s) | 5 vueltas de demos, 59,4 fps, FASTMEM 0 fallos |
| off (`NWII_RENDER_WORKER=0 NWII_GPU_XF=0`, 120 s) | bien, 59,6 fps, FASTMEM 0 fallos |
| long (300 s) | bien, 59,8 fps, FASTMEM 0 fallos |

`resume_pruned` 27-28 líneas por corrida, en 0x801b119c / 0x801d0c10 / 0x801b116c, igual que r13 y G5. Autoprueba
de GPU XF bien en todas. Además, antes de compilar: comparador por función de `output_24` (0 diferencias) y tablas
de fastmem (0 mal formadas), ver arriba. **Sin consola**: la mide la sesión principal.

## Riesgos y cosas abiertas

- deko3d: m1 lleva el arreglo de coherencia de dkgl (22:35), pero la consola aún no lo confirmó → GL por defecto.
  Cuando el frente deko3d saque otra versión: copiar `r15dk/nWiiRuntime/src/hle/gx/dk/*` aquí y recompilar las
  dos NRO de Switch + orden L (el PC no cambia).
- m1 usa los objetos ya compilados de G5 v3 en `r9/r14g5` (sin copiarlos). Si G5 recompila allí, comprobar
  `runs/r14g5_objs_sha.txt` antes de volver a enlazar (o compilar todo, ver arriba).
- El orden L sale de perfiles de otras compilaciones traducidos por nombre (pocos fallos: 0-23 muestras por perfil).
  Lo mejor es tomar perfiles con m1 en la consola y regenerarlo (`tools/build_hot.sh`, poner los nuevos perfiles
  con `@bin/nsmbw_m1.elf`).
- W (w4) y G5 ya no se prueban por separado en m1: si algo falla en la consola, comparar con `nsmbw_w4r5.nro`,
  `nsmbw_r14g5_v3.nro`, `nsmbw_a2dr5.nro` y `bin/sin_orden/`.
- `NWII_AXFX_PFNEXT` apagado (el prefetch de la llamada siguiente salió peor en la consola).
