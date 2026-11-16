# r45p: frente P — el mapa del mundo y el selector de mundos («−») — 15/16-nov-2026

Base: binario **m6i** (`r9/r41m/bin/m6i.nro`, runtime `r9/r41m/nWiiRuntime_m6i`). Sin git. No se editan otras carpetas
(las pruebas de PC usan el arnés `r9/r41m/pc` con su `.runlock`).

## Herramientas (repetir sin IA)
- `tools/nro_input.sh <dir_enlace_omen> <src_runtime_omen> <nombre>`: NRO de PRUEBA con entrada guionizada = el mismo
  enlace final con orden L de un NRO ya hecho (`k_cmds.txt` + `order/order.txt`) + `input_script_nx.o` + `--wrap` de
  KPADRead (como `r17pgo/tools/test_nro.sh`). El m6i.nro normal NO lleva `NWII_INPUT_SCRIPT` (solo FORCE_DEMO/LEVEL).
  `tools/nro_input.sh /home/djvemo/nsmbw-port/r9/r41m/nx/m6i /home/djvemo/nsmbw-port/r9/r41m/src_m6irt m6i` →
  `bin/m6i_input_test.nro` sha256 350fb6f1…d23e (elf a444b8bc…5b71), fastmem 871 162 entradas, 0 mal formadas.
  Solo para el banco (nxlink); nunca instalarlo como juego.
- Guion (WPAD: 2 = 0x100, MINUS = 0x1000): `NWII_INPUT_SCRIPT="0-100000:100:20:4"` (pulsa «2» por los menús: título →
  partida → mapa) + `NWII_INPUT_PHASES="3"` (fase 1 al llegar a WORLD_MAP) y, para el selector,
  `NWII_INPUT_SCRIPT_1="300-306:1000:100:6"` («−» 5 s después de llegar al mapa; se queda en «Selecciona un mundo»).
  Siempre `NWII_NO_SAVE="1"`. En PC llega al mapa en ~18 s (fotograma ~960).
- `tools/pantalla.py [--sel 300] <bench_dir|run.log>...`: diagnóstico por tramo (menús / mapa / selector) de las
  líneas [NX] [PIPE] [RPROF] [WPROF] [TCSTAT] (columnas explicadas en la cabecera del guion).
- `tools/pc_perf.sh <bin_input_test> <nombre> mapa|selector`: perf por hilo en PC.
- `tools/pc_pant_fc.sh <bin_input_test> <tag>`: FRAMECHECK=30 en mapa y selector a 1.5789x.
- `tools/pc_verif_mrt.sh <bin_input_test> <tag> ENV=val…`: FC 5 demos (1.5789x) + checks 1x + mapa/selector.

## Bitácora
- 22:15 inicio. m6i.nro no trae entrada guionizada → `tools/nro_input.sh` (arriba). PC: el guion llega al mapa (fot. ~960)
  y «−» abre «Selecciona un mundo» (`r41m/pc/runs/p45_menos`, mirado).
- 23:01-23:16 **consola, m6i_input_test, 1020/460, partida del usuario (Mundo 2, desierto), capturas miradas: bien**:

| corrida | tramo | dib | juego | ms juego | render ms/fot | trabajador ms | draws GX→GL | copias EFB/fot | desc./2 s |
|---|---|---|---|---|---|---|---|---|---|
| P01_m6i_mapa | mapa | 59,9 | 59,8 | 10,1 | 13,1 | 8,3 | 7 343→221 | 21 | 0,2 |
| P02_m6i_selector | selector | **43,5** | 59,9 | 8,0 | **20,7** (95 %) | 11,3 (57 %) | 10 181→283 | 60 | 33 |
| P03 (+TPROF+NX_PROFILE) | selector | 40,5 | 59,9 | 8,0 | 21,0 | 12,0 | igual | 60 | 39 |
| P08 (+RPHASE+GPUTIME) | selector | 41,4 | 59,9 | 8,2 | 21,1 | 11,3 | igual | 60 | 37 |
| P09 (+RPHASE+GPUTIME+COPYMRT) | selector | 44,5 | 59,9 | 8,1 | 20,0 | 11,2 | igual | 60 | 31 |

- **Cuello: el hilo de render (CPU)**. GPU: 9,6 ms por fotograma (GPUTIME, máx 16,6) → la GPU a 460 MHz no limita;
  swap 0,3 ms. El juego va a 60 (8 ms) y el trabajador tiene ~37 % libre.
- Render del selector por fases (P08, `NWII_RPHASE=1`, µs por fotograma dibujado, total 21 026): regs 1 700 · **state
  5 270** (bucle de órdenes por draw + prueba de recorte + preámbulo de DrawPrimitive: 0,5 µs × 10 181 draws) · tex 1 125 ·
  **emit 5 270** (xf::Emit, 0,52 µs/draw) · upload 340 · **gldraw 4 500** (283 draws GL × 15,3 µs de Mesa/nouveau:
  validación de texturas y programa) · **copy 2 600** (60 copias de 32x32: setup 410 draw 1 020 back 230 done 180 clear
  600) · frame 240.
- Perfil del hilo de render (P03, NX_PROFILE, ms por fotograma del juego; ×1,5 por dibujado): EmitPrimitive 1,73 ·
  **espera «sin fotograma que dibujar» 1,58** · RenderOnce 1,51 · xf::Culled 0,73 (en el selector no se recorta nada:
  `culled 0`) · PalPosNrm 0,57 · hash_ram_ 0,54 · DrawPrimitive 0,51 · SetupDrawState 0,47 · liberar órdenes 0,45 ·
  fence GPU 0,40 · Mesa 29 % del hilo · IPC GPU 4 %.
- Lo que tiene el selector: el modelo 3D de los mundos son **~10 000 draws pequeños** por fotograma (9 980 de display
  lists, todos por GPU XF, 67 000 vértices) en 283 lotes GL, más **60 copias EFB de 32x32** (3 tandas de 20 de la franja
  y=456, como las baldosas animadas) y 139 texturas hasheadas por fotograma. Frente al mapa (+2 900 draws, +40 copias):
  +7,6 ms de render.
- La espera de 1,58 ms/fot del render: con dos paquetes, cuando hay un fotograma listo sin tomar y el trabajador termina
  de analizar otro, se lo QUITA al render (RetractToPrefix en `acquire`) y lo rellena; hasta entregar el nuevo (retracción
  destruyendo ~30 000 órdenes, split, decode: `clear` 2,4 ms/fot de media en WPROF) no hay nada que dibujar. Con el render
  como lado lento (33 retiradas por ventana de 2 s) cae a menudo en esa ventana.
- 23:30-23:46 **COPYMRT (de R2, ya en m6i, apagado) en consola**, contiguas, m6i_input_test: selector P04 MRT **44,8** frente
  a P05 control 40,2 (render 19,4 frente a 21,0 ms; P02, el mismo control antes, dio 43,5: las corridas iguales varían
  ±2 fps); mapa P06 MRT 59,8 frente a P07 59,9 (render 13,0 = 13,0: sin efecto en el mapa). Con RPHASE (P09 frente a P08):
  copy 2 600 → 1 530 µs/fot, total 21 026 → 20 020.
- Exactitud de MRT: en PC la FC dio 1 fotograma distinto de 540 (agua, fundido, 56 px máx 5) y luego 1 de 1 460 (máx 1);
  el control sin MRT 0 de 1 520. Nueva comprobación directa **`NWII_P_MRT_CHECK=1`** (renderer_gl.cpp): cada copia
  encolada se hace TAMBIÉN en el momento de encolarla con el programa de una copia (lo que haría el camino viejo con el
  EFB de ese momento) y al hacer la cola se comparan byte a byte: **agua 210 000 copias, castillo 245 000, selector
  295 000: 0 distintas** (`test/mrt_check_b.txt`, `tools/mrt_check.sh`); en esas mismas corridas FRAMECHECK agua
  1 020/1 020, selector 520/520, castillo 1 178/1 180 (2 fotogramas de 2 px; sale también sin MRT a veces: ruido de las
  pasadas). Conclusión: las copias MRT son idénticas; los fotogramas sueltos de la FC son el ruido de las 3 pasadas.
- **Reproducción de m6i con mis guiones** (`tools/nro_completo.sh p45m6i nWiiRuntime_m6i_base`: build_nro.sh + order_p.sh
  con KOBJ k2a43 y los perfiles de M10 + nro_input.sh): `.text` idéntico byte a byte al de `r41m/bin/m6i.elf`
  (62 536 632 bytes), fastmem 871 162, eqcheck 27 distintas (= m6i); el .nro difiere solo en rutas de datos. Así la
  variante p45a sale del mismo proceso que m6i.
- Hallazgo aparte (no es de P): el fondo del selector (una captura del mapa que el juego copia del EFB una vez al pulsar
  «−» y luego dibuja en los bordes ondulados) sale a veces NEGRO en PC, también con m6i sin cambios (p45_menos,
  tri_a_ctl, brd_*_ctl): si el fotograma de esa copia se descarta (copia «stale»: solo su borrado), la textura no se hace
  nunca. En consola las 3 capturas mostraban el desierto (bien). Probando con NWII_NOFRAMESKIP=1.
- 00:09-00:45 **Selector en consola, combinaciones** (180 s cada una, todas 1020/460, capturas bien):
  p45a (m6i + interruptores, todo apagado por defecto): P10 control **44,3** · P11 TRIPLE 45,3 · P12 MRT+CULLSKIP
  **49,6** (render 17,9 ms) · P13 TRIPLE+MRT+CULLSKIP **50,8** (render 18,1, vuelta 19,7) · P14 control 44,3.
  p45b (con enganches de diagnóstico en el bucle de órdenes: +0,8 ms de render; quitados en p45g): P19 todo 48,2 ·
  P20 MRT+CULLSKIP 47,3. TRIPLE v1 (retracción con s_ready quitado: ventana sin fotograma 3,7 ms en consola) → v2
  (copia de las órdenes guardadas mientras sigue disponible, luego empalme: ventana 0,6 ms).
- **MRT y el agua (punto 2 del coordinador)**: FRAMECHECK=2 400 s en agua: con MRT 2 de 4 240 fotogramas distintos,
  SIN MRT 46 de 7 000 — los mismos fotogramas (fundido de entrada de cada vuelta de la demo, ±1 en toda la imagen). Es
  ruido de las 3 pasadas de FRAMECHECK, no de MRT. MRT_CHECK: 785 000 copias, 0 distintas.
- **Fondo negro del selector (punto 3)**: al pulsar «−» el juego hace UNA copia del EFB entero (640x456) a otra
  dirección (11A33520 en PC; el mapa copia cada fotograma a 11A7A920). Con NWII_P_STALELOG: en 5/5 corridas negras esa
  copia única salió STALE (su fotograma se descartó: solo el borrado) y en 4/4 visibles salió hecha. Arreglo:
  **`NWII_P_KEEPCOPY=1`** (con TRIPLE): un fotograma en espera con una copia a textura cuyo destino (BP 0x4B) el nuevo
  no repite no se descarta; el trabajador espera (máx. 100 ms) a que el render lo tome. Pendiente: los descartes
  dentro de un lote (DROPFAST, trabajador atrasado) no lo cubren.
- **Punto 4: `NWII_P_DLMERGE=1`** (fifo_parser.cpp ReplayMemo + DLFuseGroup, DecodedVerts::parts, EmitIndices por partes
  en renderer_gl.cpp): en PC, selector 9 660 draws de listas fundidos en 291 por fotograma, castillo 11 605 en 236.
  Comprobación fundido↔partido (`NWII_P_DLMERGE_CHECK=5`, reutiliza el D40MERGE_CHECK de D): **selector 1 080/1 080,
  castillo 1 220/1 220 idénticas**; FRAMECHECK 420/420 y 480/480.
- 01:20-02:30 **Consola**: desierto aislado (p45g, 150 s): P27 control 59,1 · P28 MRT **47,2** · P29 TRIPLE 59,2 ·
  P36 MRT+`NWII_P_MRT_SMALL=32` 59,1 · P37 MRT+RPHASE+GPUTIME 47,1 (GPU 12,5 ms/fot). Selector: P31 MRT+CULL+DLMERGE 52,8 ·
  P32 MRT+CULL 44,5 · **P33 solo DLMERGE 56,6** (render 14,7, vuelta 17,7) · P38 candidata (DLMERGE+CULL+TRIPLE+MRT32+
  KEEPCOPY) 52,8 (render 13,7, vuelta 18,9, hilo de render ocupado 75 %) · P39 control 39,8. 5 demos P40 candidata /
  P41 control: desierto 59,6/59,6 · agua 57,5/57,6 · cielo **58,0/59,4** · nubes 60/60 · castillo **58,9/56,6**; render
  más bajo en todas (castillo 12,2 → 9,2 ms, agua 12,9 → 10,9, cielo 12,6 → 10,2). Algo de MRT/CULLSKIP añade espera
  fuera del draw (P31 frente a P33: vuelta 18,9 frente a 17,7 con draw 13,5 frente a 14,7) → tanda P42-P48.
- PC: DLMERGE con devolución de ranuras del arena (p45m): castillo arena 1 242 452/1 572 864, 0 llenos (antes 1 257),
  fundido↔partido 1 240/1 240. KEEPCOPY con descartes solo por retirada: borde visible 2/3 (sin él 0/3).
- **m6p** = árbol con valores por defecto nuevos en `m6_defaults()` (grupo `NWII_P_OFF`): DLMERGE, CULLSKIP, TRIPLE,
  KEEPCOPY encendidos; COPYMRT y MRT_SMALL=32 preparados. `bin/m6p.nro` (sha en bin/m6p.sha256). PC con m6p sin
  variables: FC 5 demos 400-440/400-440 (agua 1 fotograma ±1 en el fundido de la vuelta: el ruido conocido).
- 02:45-03:40 **Tanda de aislamiento en el selector** (p45l, contiguas, 150 s): P42 DLMERGE 59,7 (render 15,6) · P43
  DLMERGE+CULLSKIP **59,8 (render 13,9)** · P44 +TRIPLE 59,8 · P45 +TRIPLE+KEEPCOPY 59,8 · P46 DLMERGE+MRT32 59,8 (14,2) ·
  P47 control 39,8 (21,7) · P48 candidata completa +TPROF 59,8 (la P38 igual había dado 52,8: variación entre corridas).
  **Decisión del coordinador: por defecto DLMERGE + CULLSKIP; TRIPLE, KEEPCOPY, COPYMRT/MRT_SMALL preparados.**
- **NRO definitivo m6q** = m6i + este árbol (`m6q_vs_m6i.patch`, sha256 2dd8bd93…84fe4; = la fuente de la Omen
  `src_m6qrt`, comprobado con rsync -n): `bin/m6q.nro` sha256 5c0a27bfa686b06e162265e54e74d2ecfc127644f4e5f2baba062114d9db2c6b,
  elf 611634d5…f8f8, `bin/m6q_input_test.nro` aa25ef21…2f4d. Mismo proceso que m6i (k2a43 + pares + orden L de M10;
  fastmem y eqcheck como m6i). Incluye la devolución de ranuras del arena.
- **PC m6q sin variables** (`tools/verif_def.sh bin/NSMBW_m6q m6q`, NEST=2 NESTL=1): FC 5 demos idénticas
  (400/380/400/420/440); DLMERGE_CHECK 0 píxeles en 5 demos (600-880 cada una), checks 1x (960-1 280) y mapa/selector
  (940/940); checks 1x 160/140/140/160 idénticas; mapa/selector 1.5789x 120/120; estrés 2×150 + 1×150 lento: 0 rotas,
  0 NullCall, vigía 0. (m6p, con TRIPLE+KEEPCOPY encendidos: igual, estrés 3+2.)
- Corridas de consola de m6q/m6p: Q01-Q03 encoladas por el coordinador (m6p + _OFF de TRIPLE/KEEPCOPY = m6q).
