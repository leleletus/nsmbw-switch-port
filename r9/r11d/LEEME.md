# r11d — iris del título (`wipeCircle`) que parpadea / se ve negro

Estado: **causa encontrada y corregida** (parche listo). Faltan solo comprobaciones finales (ver "Pendiente").

## Qué se veía

Al aparecer el título ("pulsa 2"), el iris (`Layout/wipeCircle.arc`) debía abrir un agujero redondo que
crece y deja ver el escenario. En el port, entre los frames ~340 y ~372 todo era negro (el agujero no
mostraba nada, salvo un punto en un frame), luego se veía solo una franja de suelo abajo y después, de golpe,
el escenario entero.

## Referencia (Dolphin)

Capturé con el oráculo (`oracle.py start --video OGL --xfb-ram --auto2a`, `oracle.py xfb … --count 300 --every 2`)
los frames del título en Dolphin: punto pequeño → agujero que crece mostrando el **cielo** → el negro de
alrededor **se vuelve translúcido** hasta desaparecer.

**El negro translúcido del final es correcto**: lo hace el propio juego. La animación
`wipeCircle_00_outWindow.brlan` anima el alfa del panel `W_circle_00` (RLVC, alfa 254 hasta el frame 20 y
baja a 0 en el frame 60) además del tamaño. Dolphin hace lo mismo. Eso no era un fallo.

El material, la textura (I8 256x512, wrap S espejo / T clamp), el TEV, el blend (SRCALPHA/INVSRCALPHA) y
el alpha test del iris se traducían bien. El fallo no estaba en el iris sino en **la escena de debajo**.

## Causa real

- Durante esos frames el juego dibuja la escena **dos veces por frame** con **dos cámaras distintas**
  (una visible y otra desplazada ~2300 unidades, fuera de pantalla) y reutiliza **el mismo array de
  matrices de G3D** en RAM para las dos pasadas.
- Los modelos (G3D) cargan sus matrices con `LOAD_INDX` (comandos 0x20/0x28/0x30/0x38: la GPU lee la
  matriz del array en RAM **en el momento de ejecutar el comando**).
- En el port, el FIFO se analiza tarde, en el hilo anfitrión (hasta varios frames después). `fifo_parser.cpp`
  leía el array en ese momento (tanto en el flujo principal como en las listas, incluidas las de la caché
  de display lists, que "relee la RAM viva"). Para entonces el juego ya había escrito los valores de la
  otra pasada (o del frame siguiente): la pasada visible salía con la cámara desplazada → cielo, colinas y
  modelos fuera de pantalla → agujero negro; solo quedaban las piezas que usan matrices inmediatas
  (franja de suelo, sprites). Medido con un registro: la misma dirección del array daba tx=-2269 y tx=+35
  en las dos pasadas del mismo frame; y el resultado dependía de la carrera (por eso "un frame con el punto").
- Las copias de la lista (`GX_DL_Snapshot`) ya protegían los **bytes** de las display lists, pero no los
  **datos** que leen sus `LOAD_INDX`. Además el seam `GX_DL_Snapshot` no se llama en esta build; las
  llamadas a listas llegan por el FIFO (`0x40` + dir + tamaño).

## Arreglo (solo runtime, `nWiiRuntime/src/hle/gx.cpp` y `src/hle/gx/fifo_parser.cpp`)

Parche: `r9/r11d/wipe_fix.patch` contra `r9/r11base/nWiiRuntime` (aplicar con `patch -p1` desde la carpeta
que contiene `nWiiRuntime/`). No toca `renderer_gl.cpp` ni cabeceras.

1. **Hilo del juego (gx.cpp)**: en `GX_WGPIPE_Write8/16/32/F32/F64`, una pequeña máquina de estados sobre
   las llamadas reconoce lo que el SDK escribe como llamadas separadas:
   - CP `Write8(0x08) Write8(reg) Write32(v)` → recuerda base/stride de los arrays 12-15 (CP 0xAC-0xAF / 0xBC-0xBF);
   - `LOAD_INDX` `Write8(0x20|28|30|38) Write32(v)` → **copia en ese momento** los floats de la matriz;
   - `CALL_DL` `Write8(0x40) Write32(dir) Write32(tam)` → copia la parte de los arrays que esa lista leyó las
     veces anteriores (rango aprendido por lista; la primera vez no hay copia → RAM viva como antes).
   - Se salta el marcador `0x78` de los seams (si no, `GXSetCurrentMtx` parecía un `LOAD_INDX 0x30`).
   Los registros van a dos colas (en orden del flujo), con sello de frame.
2. **Hilo anfitrión (fifo_parser.cpp)**: en cada `LOAD_INDX` del flujo principal toma el registro que
   coincide (mismo array/dirección/tamaño, busca en los 64 primeros); en cada `CALL_DL` toma el registro de
   esa lista y lo usa para los `LOAD_INDX` dentro de ella (también en aciertos de la caché de listas).
   Si no hay registro, lee la RAM como antes. Registros no usados se descartan al quedar 16 frames atrás.
3. `NWII_IDXSNAP=0` desactiva todo (comportamiento anterior). `NWII_IDXSNAP_LOG=1` imprime contadores
   `[IDXSNAP] main used/live | lists with/without | in lists used/live | copied KB`.
   En el título: main live=0, lists without=0, solo ~0,3 % de lecturas en listas siguen siendo vivas
   (primera llamada de cada lista).

## Builds

- `r9/r11d/bin/NSMBW_fix2`, `NSMBW_fix2_input_test` (PC), `nsmbw_fix2.nro`, `nsmbw_fix2_input_test.nro` (Switch).
  **fix2 = versión final** (fix = igual sin la poda por antigüedad).
- `NSMBW_dbg*`: binarios de depuración (trazas extra), no usar.

## Reproducir / verificar

```
cd ~/nsmbw-port/r9/render
NWII_INPUT_PHASES=6 NWII_INPUT_SCRIPT_1= NWII_XFBDUMP_FROM=330 EVERY=2 RESIZE=40% PROFILE=free RES=1 \
  ~/nsmbw-port/scripts/lim pc/run.sh ~/nsmbw-port/r9/r11d/bin/NSMBW_fix2_input_test wipe_fix 16 mp '0-330:100:20:4'
```
Resultado: punto → agujero creciente con cielo → fundido; igual que Dolphin
(capturas de Dolphin: `/tmp/claude-1000/.../scratchpad/dol/a_000581..000660.png`, temporales).

Ojo: `pc/run.sh` usa la carpeta compartida `pc/run`: no lanzar dos corridas a la vez.

## Pendiente / cómo seguir

- Resultado de `RESS=1 test/checks.sh …/NSMBW_fix2_input_test wfix2 120` (ver abajo, "Resultados").
- Comparar frames título/mapa/1-1 antes/después (ver "Resultados").
- Probar en la Switch (nro fix2). El coste extra es pequeño: unas comparaciones por escritura al FIFO y
  copiar unos KB por frame (ver `copied KB`).
- Posible efecto colateral (bueno): cualquier modelo con matrices por índice ya no "adelanta" frames al
  desplazarse la cámara. Si al comparar 1-1 hay diferencias mínimas en modelos, es por eso.

## Resultados

`RESS=1 test/checks.sh /home/djvemo/nsmbw-port/r9/r11d/bin/NSMBW_fix2_input_test wfix2 120` (runs `pc/runs/chk_wfix2_*`):

```
chk_wfix2_title_1x  checks=200 identical=200 diff_lines=0 | mismatches=0
chk_wfix2_map_1x    checks=220 identical=220 diff_lines=0 | mismatches=0
chk_wfix2_walk_1x   checks=220 identical=220 diff_lines=0 | mismatches=0
chk_wfix2_lvl11_1x  checks=220 identical=220 diff_lines=0 | mismatches=0
```
("pass1 differs N" no es criterio: también sale distinto de 0 en chk_r10b / chk_r11a sin este parche.)

Comparación antes/después (`NWII_REALTIME=0`, un frame cada 60, runs `pc/runs/cmp_v1_{title,map,lvl11}_{base,fix,base2}`):
las corridas **no son deterministas ni entre dos corridas de la base** (título base vs base2: 3 de 25 frames
idénticos; base vs fix: 10 de 24), así que la identidad píxel a píxel entre corridas no sirve de criterio.
Mirados a mano (mapa 1140/2580, 1-1 1620/2640/4260/5400): mismo contenido, solo cambian la postura/posición
de Mario y las monedas por el reparto de tiempo, igual que entre dos corridas de la base. El criterio
determinista son los checks de arriba (todos a 0).

Contadores `NWII_IDXSNAP_LOG=1` (corridas de ~40-75 s):
- título: main used=432787 live=0 | listas con registro 100 % | en listas used=2244015 live=2874 | copiado 74,8 MB en total (~30 KB/frame)
- mapa:   main live=0 | en listas live=423 (de 276852) | 9,8 MB
- 1-1:    main live=0 | en listas live=816 (de 424440) | 15,3 MB

Estado al cerrar (12:20): **hecho**. Pendiente solo: probar el `.nro` fix2 en la Switch real/citron
(debe verse igual que en PC) y fusionar el parche con r11a/r11b (no tocan los mismos ficheros:
el parche no modifica `renderer_gl.cpp`).
