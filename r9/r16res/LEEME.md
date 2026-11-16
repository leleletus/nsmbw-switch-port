# r16res — resolución interna fraccionaria (frente RES, 8-nov-2026)

**Qué es:** la resolución interna (el EFB de la Wii, 640×528, multiplicado por una escala) ya no tiene que ser
1, 2, 3 o 4: vale cualquier número entre 1 y 4 (`1.3636`, `1.5`…) o una altura de imagen (`"720p"`, `"1080p"`).
Sin commits. Base: runtime de `r9/r13` (copiado en `nWiiRuntime/`), compilado solo-runtime con
`tools/variant_build.sh` (`BASE=r9/r13`). Parche limpio contra r13: **`res.patch`**.
**Versión para la consola (la buena): `r5dkres/`** = runtime de r5dk (R5 a13 + dkgl/deko3d de r15dk) + este
parche; parche contra r5dk: `r5dkres/res_vs_r5dk.patch`.

## 1. Cómo se usa

En `config.toml` (`[graphics]`) o, para pruebas, como variable en `debug.toml` (manda sobre las dos claves):

| Valor | Qué hace |
|---|---|
| `internal_resolution = "auto"` | PC: la escala con la que la imagen del juego ocupa justo la altura de la ventana (1:1). Switch: `"1080p"` en el dock y `"720p"` en portátil |
| `1` … `4` (número o texto) | escala entera, como antes (1 = la de la Wii) |
| `"1.3636"`, `1.5`, `"1.5x"` | cualquier escala entre 1 y 4 |
| `"720p"`, `"1080p"`, `"900p"` | la escala con la que el XFB que se ve (456 líneas en SMNE01) mide exactamente esas líneas: **720p = 1,5789** (EFB 1010×834), **1080p = 2,3684** |
| `internal_resolution_handheld = …` | igual, solo en modo portátil (sin la clave = la de arriba) |
| `NWII_INTERNAL_RES = "1.3636"` (debug.toml / entorno) | manda sobre las dos claves de config.toml; misma sintaxis |
| `NWII_INTERNAL_RES_CYCLE = "<n>"` + `NWII_INTERNAL_RES_CYCLE_FRAC = "1"` | pruebas: 1 → 1,3636 → 2 → 1,5789 cada n frames (cambio de tamaño en marcha) |
| `NWII_RES_GPUPROF = "1"` | perfil de GPU por tramos (§4): líneas `[RESGPU]` cada 300 frames (solo GL: con deko3d da 0) |
| `NWII_NO_FLOAT_VIEWPORT = "1"` | pruebas: viewport entero redondeado borde a borde en vez de en float |

Ojo con "720p": la cuenta del encargo (720/528 = 1,3636) usa la altura del EFB, pero en pantalla solo se ve el XFB
(456 de las 528 líneas). Con 1,3636 el XFB sale de 873×622 y se estira ×1,16 en vertical hasta 720; para que sea 1:1
hace falta 720/456 = 1,5789 (2,49 veces los píxeles de 1x, frente a 1,86 de 1,3636). Las dos se pueden pedir;
`"720p"` es la 1:1. En `[Config] Internal resolution:` y `[GL] internal resolution 1.3636x (EFB 873x720)` del
log se ve qué se usa.

**Aviso:** "auto" en portátil = 720p real. Si la GPU de una Switch v1 sin OC no llega (ver §5), la línea
`internal_resolution_handheld = 1` (o `"1.3636"`) en config.toml lo arregla sin tocar código.

## 2. Qué cambia (archivos)

| Archivo | Cambio |
|---|---|
| `include/runtime/res_scale.h` (nuevo, solo runtime) | `res::Spec` + `Parse` / `Describe` / `Resolve` (todo inline: el CMake de la Switch es compartido y no admite archivos nuevos). |
| `src/core/config.cpp` | lee las dos claves también como `res::Spec` en `res::g_cfg[2]` (global del runtime). **`config.h` no cambia** (lo incluye el código generado): los `int` de siempre se siguen rellenando. |
| `src/core/main.cpp` | `DesiredInternalScale` devuelve `float`; `GX_CreateEfb/GX_ResizeEfb(float)`, `float GX_GetEfbScale()`, nuevos `GX_GetEfbSize(w,h)` y `GX_GetXfbLines()`; los `glViewport(640*s…)` pasan a `GX_GetEfbSize`. |
| `src/hle/gx/renderer_gl.cpp` | la regla de escala (abajo), viewport, scissor, borrados, copias EFB→textura y EFB→XFB, rectángulo de presentación, creación/cambio del EFB, capturas, `PixWatch`, copias "nativas" de los tiles y el perfil `[RESGPU]`. |
| `include/runtime/texture_cache_gl.h`, `src/hle/texture_cache_gl.cpp` | `acquire_efb_copy(addr, w, h, gl_w, gl_h)` (antes `int scale`); `EfbCopy::stitched` + `efb_copy_stitched(addr)`. |
| `src/platform/switch/switch_platform.cpp` | texto del config.toml por defecto. |

### La regla (por qué no hay costuras ni líneas perdidas)

- Coordenadas GX → escaladas: X = s·x (la geometría, con viewport en float: `glViewportIndexedf`, GL 4.1; glad es
  3.3, así que se pide por nombre).
- Un píxel escalado p (centro p+0,5) pertenece al píxel nativo floor((p+0,5)/s).
- Todo borde de rectángulo GX x (scissor, borrados, origen de copias, XFB, presentación) pasa a
  **SE(x) = ceil(s·x − 0,5)**: el primer píxel escalado cuyo centro está en s·x o más allá. Dos rectángulos que
  comparten borde lo comparten exacto (ni hueco ni solape) y cubren los mismos píxeles que el rasterizador da a
  la geometría con ese borde. El EFB mide SE(640)×SE(528); la inversión de filas usa esa altura, no 528·s.
- Escalas enteras: SE(x) = x·s y el viewport sigue siendo el entero de antes ⇒ **1x y 2x son bit a bit lo de
  antes** (solo cambian sus copias de tiles a 2x, ver abajo).
- Copia EFB→textura: el rectángulo fuente SE a SE, 1:1 (las de media escala, a la mitad entera). Su tamaño GL
  puede variar ±1 según la posición (32×32 → 51×51 o 50×51 a 1,5789): la caché lo guarda tal cual.
- Copias "nativas" (nuevo): las copias que la caché de texturas ya pegó (`stitch`) en una textura de RAM (los
  tiles animados: ~20 copias de 32×32 por frame en la demo del título) solo se usan a tamaño nativo. A escala ≠ 1
  ahora se hacen directamente a tamaño nativo con un filtro de caja en el shader de copia (`uDown`: media de los
  píxeles escalados de cada píxel nativo), en una pasada, en vez de copia escalada + blit de reducción. A 1x no
  cambia nada (uDown = 0).
- Copias a la RAM, peeks/pokes del EFB, anchos de línea/punto y copias de Z: **no existen en este renderer** (las
  copias se quedan en la GPU, no hay EFB peek, las líneas/puntos no se dibujan, la copia de Z copia color), así que
  no hay nada que escalar.
- Presentación: el XFB escalado (p. ej. 1010×720 a 720p) se lleva a la ventana con el blit de siempre (a 720p en
  portátil, 1:1 en vertical; en horizontal el XFB anamórfico de 640 de ancho se estira a 16:9 como siempre).
- Volcados (`NWII_XFBDUMP`, F11): a tamaño nativo con filtro de caja (también fraccionario) salvo
  `NWII_DUMP_SCALED=1`.

## 3. Verificación en PC (Intel Arc, Mesa; arnés privado `pc/` bajo `flock r9/render/pc/.runlock`)

Hecha sobre la compilación fusionada `r5dkres` (PC: `r5dkres/bin/NSMBW_r5dkres_input_test`, camino GL), que
lleva exactamente el mismo código de este frente que `res.patch`.

| Prueba | Resultado |
|---|---|
| `checks.sh` 1x (FRAMECHECK cada 30 frames: GPU XF contra el camino r8, píxel a píxel; `test/checks_priv.sh`) | título, mapa, paseo, 1-1: **4 × 220/220 idénticos, diff_lines=0, mismatches=0** (`test/checks_r5res_1x.txt`; igual que R5 a6/a13) |
| FRAMECHECK a escala fraccionaria | título 1,3636: 220/220; 1-1 1,3636: 220/220; título 720p (1,5789): 220/220 (`test/checks_r5res_frac.txt`) |
| Cambio de escala en marcha (`NWII_INTERNAL_RES_CYCLE=600` + `_FRAC=1`, mapa, 240 s) | 23 cambios 1 → 1,3636 → 2 → 1,5789 → 1…, viewport en float en las fraccionarias, sin cuelgues, 59,8 fps; las imágenes de cada escala coinciden (`pc/runs/cycle_map`) |
| Demo del título 330 s a 1,3636 (`pc/runs/hunt_title_1.3636`, volcado cada 60 frames) | sin artefactos (cielo, 1-1, torre, castillo); no salió la demo submarina |
| Humo 720p (título con la cinemática del castillo) | correcto, bloom incluido (`pc/runs/smoke_720p_title`) |

Capturas en **`shots/`** (volcados del XFB a la resolución interna, `NWII_DUMP_SCALED=1`):
`castillo_{1,1.3636,720p_1.5789,2}.png` (cinemática del castillo, con bloom), `nivel11_{1,1.3636,2}.png`,
`map_{1x,1.3636,720p_1.5789,2x}.png`, y comparaciones del mismo rectángulo GX ampliado ×3 sin filtro:
`castillo_bloom_1x_1.36x_720p_2x.png`, `lvl11_1x_1.36x_2x.png`, `lvl11_hud_1x_1.36x_2x.png`,
`cmp_1x_vs_1.36x.png`, `hud_*_zoom3.png`. Bordes alineados en todas las escalas, sin huecos ni líneas de más
o de menos en los paneles del HUD (hechos de varios cuadros), el bloom/halo igual.

## 4. Coste: CPU del hilo de render y GPU

### CPU: no hay coste por píxel

- El coste de 2x en la CPU que se vio con r10 ya **no está**: con el código actual la escala no hace nada
  proporcional a los píxeles en la CPU (no hay copias a la RAM, ni lecturas del EFB, ni conversiones en la CPU;
  el único `glReadPixels` por frame son los volcados de prueba). Consola (coordinador, r5a10 GL, castillo): render
  43-46 % a 1x y 43-47 % a 2x. Consola con esta NRO (deko3d, OC, demo del título; `switch/real_logs/bench_res_dk_*`):
  hilo de render 24 % (1x), 26 % (1,3636), 26 % (2x); fps mediana 57,7 / 56,9 / 55,5 (las demos salen al azar).
- PC, misma corrida alternando la escala cada 600 frames en el mapa quieto (`test/cycle_sum.py`;
  `pc/runs/cycle_map_nodump`, sin volcados): render_cpu mediana 2,48 / 2,59 / 2,45 / 2,66 ms/frame a
  1 / 1,3636 / 1,5789 / 2x, dentro del ruido (p25 2,20 / 2,30 / 2,30 / 2,36). Como mucho +0,1-0,2 ms y sin
  relación con los píxeles (×1,86 / ×2,49 / ×4).
- Lo que sí quita este frente en la CPU: las copias de tiles pegadas ya no hacen el blit de reducción (una
  llamada GL y un cambio de destino menos por copia, ~20 por frame en la demo del título) a cualquier escala ≠ 1.
- Ojo al medir: los volcados `NWII_XFBDUMP` van dentro de `Render()` y a escala alta leen más píxeles: una corrida
  con volcado cada 150 frames sale +5/+7/+10 % de CPU solo por eso.

### GPU (`NWII_RES_GPUPROF=1`, marcas de tiempo GL por tramo; PC Intel Arc, ms de GPU por frame)

| Escena | 1x | 1,3636 | 2x | reparto a 1x → 2x |
|---|---|---|---|---|
| Demo del título | 1,22 | 1,46 | 1,81 | dibujos 0,72 → 1,04; copias a textura 0,43 → 0,55; copia XFB 0,06 → 0,20 |
| 1-1 | 1,27 | 1,70 | 2,35 | dibujos 0,61 → 1,11; copias 0,59 → 1,02; XFB 0,06 → 0,21 |
| Mapa (misma corrida, `cycle_map_nodump`) | 1,51 | 1,91 | 2,65 | (1,5789: 2,20) |

- La GPU sube ×1,25-1,35 a 1,3636, ×1,45 a 720p y ×1,5-1,8 a 2x (píxeles ×1,86 / ×2,49 / ×4): la escena no está
  limitada por el relleno; lo que crece como los píxeles son las copias grandes (XFB y copias a pantalla completa).
- **Lo que más pesa a cualquier escala son las copias EFB pequeñas:** ~20-25 copias de 32×32 por frame (tiles
  animados) cuestan ~0,02-0,03 ms de GPU **cada una** (cambio de destino + limpieza, no píxeles): 0,43-0,59 ms a 1x,
  un tercio o más del frame de GPU. En 1-1 hay además dos copias de pantalla completa (640×456 con borrado) por
  frame: 0,13 ms a 1x, 0,29 a 1,3636. Con deko3d estas copias podrían ir por el motor 2D (`dkCmdBufBlitImage`
  con `FlipY`, sin cambiar de destino) en vez de un dibujo con el shader de copia: es lo que más GPU ahorraría,
  a cualquier escala (para R5/r15dk).
- La cinemática del castillo a 720p: escena 1,07 ms, XFB 0,14, 20 copias de tiles 0,47 ms (`smoke_720p_title`).

## 5. Consola

NRO: **`r5dkres/bin/nsmbw_r5dkres_rel.nro`** (+ `.elf`; libdeko3d de release). Como el nombre no lleva "dk3d",
arranca con GL salvo que se pida deko3d. Todo en `debug.toml` (sin tocar config.toml):

```toml
NWII_BACKEND = "deko3d"
NWII_INTERNAL_RES = "1"        # luego "1.3636", "2" (y "720p" = 1,5789: 1:1 en la pantalla portátil)
#NWII_RES_GPUPROF = "1"        # reparto de GPU por tramos: solo con NWII_BACKEND = "gl"
```

En `nwii.log`: `[GL] internal resolution 1.3636x (EFB 873x720)`.
Resultados del coordinador (portátil, OC 1785/1152, demo del título; `switch/real_logs/bench_res_dk_{1,13636,2}_1108_22*`):
fps mediana 57,7 / 56,9 / 55,5, hilo de render 24 / 26 / 26 %. Las corridas de las 21:51/21:55 no valen (el
debug.toml tenía restos de otro trabajo: `NWII_REALTIME=0`, volcados, 7 fps).
Extrapolación a la v1 sin OC en portátil (GPU 307-384 MHz, ~3-3,75 veces más lenta): con la GPU al 17-27 % a 1x
(medida del coordinador), 1,3636 (×1,3) cabe justo y 2x no; "720p" (×1,45) es el límite.

**Fallo visto en la consola (deko3d, 1,3636, demo submarina, `02_title_140.jpg`):** un cuadro con textura estirado
desde la esquina y un triángulo azul: vértices basura. No sale de este frente: todo lo que toca RES es igual para
todos los vértices de un dibujo (viewport, scissor), líneas/puntos no se dibujan, el blit de Z con escala solo
corre al cambiar de escala (esa corrida no cambió) y las copias no tocan vértices. Lo más probable es una carrera
de reutilización de memoria (anillo de vértices / arena de GPU XF / uXDraw) en dkgl que se nota más cuando la GPU
tarda más por frame. Para aislarlo: deko3d 1,3636 con `NWII_GPU_XF=0`, y la misma demo a 1x. En PC (GL) no se pudo
repetir (5,5 min de demos a 1,3636 sin la submarina, sin artefactos).

## 6. deko3d (r15dk / r5dk): qué hace falta

El backend deko3d es dkgl (GL emulado sobre deko3d) debajo del mismo `renderer_gl.cpp`, así que el código de
este frente corre igual. Lo que necesita (ya está en `r5dkres/`):

1. `glViewportIndexedf` en dkgl (`DkViewport` ya es float). **r15dk ya lo añadió** (21:22, junto con
   `glQueryCounter` vacío y el blit de profundidad con escala de `GX_ResizeEfb`).
2. En el renderer la función se pide con **`GX_GLProc("glViewportIndexedf")`**, no con `SDL_GL_GetProcAddress`
   (con deko3d SDL no tiene contexto GL y no llegaría a dkgl). `res.patch` (base r13, solo GL) usa SDL porque r13
   no tiene `GX_GLProc`; `r5dkres/res_vs_r5dk.patch` ya usa `GX_GLProc`.
3. Si dkgl no diera la función (puntero nulo), el renderer redondea el viewport borde a borde (correcto, con hasta
   medio píxel de error en la geometría).
4. Lo demás que usa este frente ya existe en dkgl: `glUniform1f` (uDown del shader de copia, que usa `ceil` y
   bucles dinámicos), `glBlitFramebuffer` con escala (presentación, XFB, reducción de copias no pegadas) y
   `glQueryCounter` (sin datos: `[RESGPU]` da 0 con deko3d; para medir la GPU usar `NWII_BACKEND="gl"`).
5. Si algún día el renderer deja de pasar por dkgl y habla deko3d directamente: viewport = s·x en float, scissor y
   rectángulos con SE(x) = ceil(s·x − 0,5), EFB de SE(640)×SE(528), inversión de filas con esa altura.

## 7. Fusión (notas)

- Con R5 / r5dk: `r5dkres/` es la fusión hecha (git merge-file, base r13). Conflictos: `texture_cache_gl.h` (los
  campos `seen_hash/seen_frame` de R5 y `stitched` de RES van juntos) y `main.cpp` (el bloque `HostDrawableSize/
  HostSwap` de r15dk + la `DesiredInternalScale` nueva, que en PC usa `HostDrawableSize`). En `renderer_gl.cpp` no
  hubo conflicto; solo hay que cambiar `SDL_GL_GetProcAddress("glViewportIndexedf")` por `GX_GLProc(...)`.
  `r5dkres/res_vs_r5dk.patch` = esos cambios contra `r15dk/r5dk` (sin `dk/`: dkgl es el de r15dk).
- `config.h` no cambia; `res_scale.h` es nuevo y solo lo incluye el runtime. Sin archivos `.cpp` nuevos.
- Funciones con firma nueva (para `dk_rename.h` si dkgl renombra símbolos): `GX_CreateEfb(float, …)`,
  `GX_ResizeEfb(float, …)`, `float GX_GetEfbScale()`, nuevas `GX_GetEfbSize(int*, int*)` y `GX_GetXfbLines()`.

## 8. Compilar y probar

- r13 + RES (solo GL): `cd r9/r16res && BASE=/home/djvemo/nsmbw-port/r9/r13 JOBS=2 ~/nsmbw-port/scripts/lim
  ./tools/variant_build.sh . res2 pc|nx` → `bin/NSMBW_res2*`, `bin/nsmbw_res2.nro` (+`.elf`).
- r5dk + RES: `cd r9 && DK_LIB=deko3d BASE=/home/djvemo/nsmbw-port/r9/r13 JOBS=2 ~/nsmbw-port/scripts/lim
  r15dk/tools/variant_build_dk.sh r16res/r5dkres r5dkres_rel nx` (PC: `… r5dkres pc`). Ojo: si se copia `dk/`
  con `cp -a`, tocar los `.cpp` (`touch`) para que ninja los recompile.
- Pruebas: `test/checks_priv.sh <bin> <tag>` (RESS="1 1.3636 720p", SCENES=…), `test/res_runs.sh` (CPU, GPU y
  volcados por escala y escena), `test/meas.sh`, `test/cycle_sum.py <run.log>` (A/B en una corrida),
  `test/resgpu_sum.py <run.log>…`, `test/crops.sh` (comparaciones de capturas).
