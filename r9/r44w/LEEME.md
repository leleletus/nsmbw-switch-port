# r44w: frente W — el fotograma del agua (agua_pinguino) — 15-nov-2026

Base: runtime **m6e** (`r9/r41m/nWiiRuntime_m6e`), copiado en `nWiiRuntime/`. NRO = objetos PGO del juego de m5
(`r17pgo/nx/m5_pgo`) + este runtime + `w_agua_gen.o` (nuevo, ver §3). Sin git. No se tocan otras carpetas.

## 1. Auditoría (informe intermedio mandado al coordinador a las ~17:45)
Fuentes: hilo del juego = **Z09** (m4 PGO + S2 + TPROF; mismos objetos del juego que m6e; ms juego agua 15,1 frente a
15,4 en M04). Otros hilos = registros de **M04** (m6b) + `hilos` de Z09. R2_08 NO sirve: lleva `NWII_S2_OFF=1`.
Herramientas: `tools/tprof_w.py <bench> <nm> propio|wgp|rtfase [--escena]` (tiempo propio por categoría, quién
escribe al pipe), `tools/stats_escena.py <bench>` (medias por escena de las líneas [NX]/[WPROF]/[RPROF]/[GXPROF]/[S2]).
La corrida propia con el binario actual, **W01_w_m6e_tprof** (m6e + TPROF + `NWII_V_TL`), está al final de la cola.

### Hilo del juego (núcleo 1), agua, ms por fotograma del juego (ocupado 15,0)
| qué | ms/fot |
|---|---|
| código generado (invitado, incl. clones constprop 0,75) | ~10,7 |
| runtime: **escritura al pipe GX** (Write8 0,73 · Write16 0,32 · Write32 0,27 + wgp_write32 0,15 · WriteF32 0,19 · write*_slow 0,33 · flush 0,05) | **2,03** (desierto 1,05, cielo 1,20) |
| runtime: sondeo y reloj (pending_callbacks_slow 0,38 · poll_tb_refresh 0,20 · tick/wall_timebase 0,08) — frente K | 0,66 |
| ayudantes de g7 (en g7.h = código generado): psq/ps 0,74 · g7_bctrl 0,31 | 1,05 |
| libc (memcpy) · driver (svcSendSyncRequest, lecturas) | 0,06 · 0,08 |
| espera: OSLockMutex del mutex de nw4r::snd que tiene la CPU de sonido (frente A/C) | 0,52 (en espera) |
Por fases: actores 6,44 · DrawOpa/Xlu→GX 2,63 (de eso, **8000D000 = 1,28**: 0,94 escrituras al pipe + 0,34 propio) ·
CalcView 1,64 · efectos dibujo 1,22 + cálculo 0,65 · CalcMaterial 0,70 · resto de painter 0,62 · alta de modelos 0,63.

### Otros hilos (M04)
Trabajador gxw (núcleo 2, con la CPU de sonido al 28 %): split 4,4 ms/fot (0,2-0,45 en las demás escenas),
decode 7,6 (0,5-1,9), parse 10,4 (4-5,6). Render (núcleo 0) 13,1 ms/fot; 23 fotogramas descartados por intervalo (≈3).

### Lo que tiene el agua
Un chorro de vértices en modo inmediato: **8000D000** (ScnProc de la superficie del agua; drawProc 80164FA0 → thunk del
REL 807B6E50/60 `b 8000D000`; tipo en this+0xCE94, cuerpos en this+0xCE90). En agua_pinguino: **1 llamada por
fotograma, tipo 0, 1 100 quads**, cada uno `GXBegin(GX_QUADS, VTXFMT0, 4)` + 4 vértices × 11 escrituras al pipe.
Escrituras al pipe 4,64 M/s ≈ 85 000 por fotograma (1,1-2,1 M/s en las demás; OJO: no 1,2 M/s), bytes de draws
en el FIFO 13,7 MB por intervalo (1-2,5), draws 7 370/fot. Copias EFB, efectos: como el desierto.

## 2. Palancas (ver informe intermedio) y decisión del coordinador
(a) runtime: **a2 = 8000D000 nativo por g_disp_table (`NWII_W_AGUA`)**, aprobado como excepción; **a1 = caminos
cortos de escritura al pipe (`NWII_W_WGP`)**. K (sondeo) y A/C (mutex de sonido) no son de aquí.
(b) código generado (solo si sobra un regenerar+PGO): rutas rápidas en línea de psq_l/psq_st cuantizados y ps_madds
(0,74 → ≈ −0,3), caché de destino por sitio en bctrl (0,31 → ≈ −0,15). La feat `io`/X2 no (medido ≈0: K01/K04).

## 3. `NWII_W_AGUA="1"`: el renderizador del agua «nativo» (exacto por construcción)
- `tools/gen_w_agua.py r9/r35m4/export w_agua/w_agua_gen.cpp` copia el CÓDIGO GENERADO exacto de func_8000D000_g6
  (output_1.cpp, = r17pgo/m4src) y de los thunks 807B6E50/60 (output_48.cpp) y SOLO cambia, dentro de los 5 bucles de
  vértices (8000B89C-BA48, BE20-BFE8, C3C8-C594, C91C-CA24, CE00-CFC8), las 270 escrituras al pipe
  (`mv_.write8/16/f32_io(rX - 0x8000)` con base r23/r28/r29/r30 = 0xCC010000 de `lis rX,0xcc01`) por un añadido a una
  cola local (`w_agua/w_agua_cola.h`: bytes big endian + cinta de tipos; si la base no fuera 0xCC010000, la llamada de
  siempre). Antes de cualquier otra sentencia de los bucles (GXBegin, sondeo, return) y al salir de cada bucle,
  `W_COMMIT()` → `GX_WGPIPE_CommitTape` (gx.cpp): copia el bloque al búfer write-gather y deja s_bp_armed, el
  sniffer (s_sn_st/s_sn_a) y el contador exactamente como las llamadas una a una; si no está en el camino rápido, las
  repite una a una. Resultado: 1 entrega de 96 B por quad en vez de 44 llamadas.
- Enganche: `w_agua_install` (main.cpp, antes del hilo de la CPU, o sea antes de que g7 monte su tabla hash de bctrl)
  cambia las entradas 0x8000D000, 0x807B6E50, 0x807B6E60 de g_disp_table (solo si apuntan a las funciones generadas).
  Línea `[WAGUA]` al arrancar y cada 5 s (llamadas, tipos, cuerpos/llamada, entregas).
- `NWII_W_AGUA_CHECK="1"` (PC, con `NWII_S2_OFF=1`): en cada llamada ejecuta la ORIGINAL con el pipe capturado
  (`g_w_cap`, caminos completos), deshace sus escrituras en RAM (solo las palabras que cambió) y los registros, ejecuta
  la NATIVA igual y compara escrituras al pipe (tipo+valor en orden), registros (gpr/fpr/ps1/cr/pc/lr/ctr/xer/msr/…)
  y RAM (MEM1/MEM2/LC) tocada; luego manda al pipe las de la nativa. MSR[EE]=0 durante las dos (si no, una
  interrupción entregada en la original se perdía y el juego se colgaba). `NWII_W_AGUA_CHECK_NULL="1"`: control
  del método (original contra original).

## 4. `NWII_W_WGP="1"`: caminos cortos de Write8/16/32/F32 (gx.cpp)
Una bandera decidida en wg_init_mode (write-gather en línea, búfer de 4 KB, sniffer encendido) + la redirección; el
sniffer de Write8 por tabla de 256 cuando está en reposo; límite del búfer constante. Además (siempre, también sin el
interruptor) la declaración de `hw::g_wgp_redirect_on` es oculta: se lee sin pasar por la GOT.
`NWII_W_WGP_SELFTEST=1` (PC): 4 000 secuencias × 200 escrituras aleatorias por los dos caminos, comparando tras cada
escritura búfer, sniffer, BP armado y contador.

## 5. Para D (lo que se ve desde el hilo del juego)
- Formato del vértice del agua (setup de 8000B530): GXClearVtxDesc; POS, CLR0, TEX0, TEX1 DIRECTOS; VTXFMT0: POS XYZ
  F32, CLR0 RGBA8, TEX0 y TEX1 ST **S16 con 10 bits de fracción** → **24 B por vértice** (12 + 4 + 4 + 4).
- Por fotograma (agua_pinguino): **1 100 draws `GX_QUADS` de 4 vértices** (99 B cada uno: opcode 0x80 + u16 4 + 96 B),
  = 108 900 B; es casi todo el «draw_bytes» de GXPROF del agua (13,7 MB por intervalo).
- **Entre dos quads seguidos no hay NINGUNA escritura de estado**: la captura del modo comprobación da por llamada
  exactamente setup (~680 B) + 1 100 × (Write8 opcode + Write16 cuenta + 44 escrituras de vértice); GXBegin no
  ensucia nada después del primero. Se pueden fundir los 1 100 en UN draw de QUADS de 4 400 vértices (mismo VAT,
  misma VCD, mismas matrices/TEV/texturas), o decodificar el bloque de una vez (formato fijo, sin índices).
- Contenido: 4 filas de posiciones (this+0x48 + k·13 200, paso 12 B), dos colores RGBA fijos por fila
  (this+0x10000-12664 y -12660), texcoords s = this+0x10000-12648/-12646 (s16) + (i mod 11)·93 (+92), t = r27/1023.
- Los otros ~6 000 draws/fot del agua no salen de aquí (el agua tiene 7 370 frente a 2 800-4 400 en las demás).

## 6. PARA M: cómo integrar w2 en otro runtime (m6h)
El parche `w2_runtime_vs_m6e.patch` (`diff -ruN r41m/nWiiRuntime_m6e r44w/nWiiRuntime`: solo `src/core/main.cpp` y
`src/hle/gx.cpp`; `include/` NO cambia) **no basta solo**: main.cpp llama a `w_agua_install`, que vive en una unidad
NUEVA fuera del runtime. Pasos:
1. `cd <copia de tu runtime> && patch -p2 < r9/r44w/w2_runtime_vs_m6e.patch` (aplica sobre m6e; si tu gx.cpp/main.cpp
   cambiaron, los trozos son independientes: captura en los 4 caminos completos, `GX_WGPIPE_CommitTape` +
   `GX_W_ModeSwap` + selftest antes de `GX_WgpStatsLine`, caminos cortos al principio de Write8/16/32/F32, la
   declaración oculta de `hw::g_wgp_redirect_on` y `s_w1_fast` en `wg_init_mode`; en main.cpp la declaración y la
   llamada justo después de `::vtl_init()`).
2. Generar la unidad nativa del agua DEL MISMO export que tus objetos del juego (k3 = mismas fuentes que m4/m5
   = `r9/r35m4/export` = `r17pgo/m4src/export`, solo cambian las opciones):
   `python3 -I r9/r44w/tools/gen_w_agua.py <export> <dir>/w_agua_gen.cpp` y copiar al lado `r9/r44w/w_agua/w_agua_cola.h`
   y `r9/r44w/w_agua/w_agua_hook.inc`. El generador falla (assert/StopIteration) si no encuentra las funciones.
3. Compilarla con la orden de ninja de `output_1.o` SIN las opciones de perfil (`-fprofile-*`, `-Wno-missing-profile`)
   ni el `-MD…`, con `-I<dir>`, y añadir el objeto al enlace antes de `-o nsmbw.elf`. Hecho en
   `r9/r44w/tools/build_pgo_w.sh` (líneas «r44w: w_agua_gen.o»): cópialo en tu build_pgo. PC: `tools/build_w.sh`
   (la orden de `pc_obj/compile.cmd` de m4 + `-I`).
4. Comprobar que el ELF tiene `[WAGUA]` (`strings -a … | grep -c WAGUA` ≥ 1) y fastmem 0 mal formadas
   (la unidad nueva añade ~1 070 sitios de fastmem: 866 754 → 867 827 en w2).
Interruptores (ninguno cambia nada si no está): `NWII_W_AGUA`, `NWII_W_WGP`; de PC: `NWII_W_AGUA_CHECK`
(+`NWII_S2_OFF=1`), `NWII_W_AGUA_CHECK_NULL`, `NWII_W_AGUA_STATS`, `NWII_W_WGP_SELFTEST`. Con el gancho instalado
sale `[WAGUA] renderizador del agua: NATIVO …` al arrancar y una línea `[WAGUA] nativo: …` cada 5 s mientras se dibuja
agua. Si g_disp_table no apunta a las funciones generadas esperadas (otro export), NO se engancha y lo dice.
Nota: la declaración oculta de `g_wgp_redirect_on` cambia también el camino de siempre (una carga menos); el resto
de gx.cpp con los interruptores apagados ejecuta lo mismo que m6e.

## 7. Qué más queda en el hilo del juego en agua (después de AGUA + WGP)
Estimación previa a medir: AGUA quita ~0,94 ms de llamadas al pipe del agua, pero suma ~0,15-0,2 ms de entregas
(1 100 por fotograma) → **≈ −0,7 ms**; WGP ≈ −0,2-0,3 ms en el resto de escrituras al pipe (~35 000/fot en agua).
Lo que queda (Z09, ms/fot, agua), por orden:
1. **Simulación de la superficie del agua**: actor del REL 807B5240 = 1,51 ms (cielo no lo tiene), de eso
   **800ECAE0 0,97 (0,83 propio)** —función grande de coma flotante (544 B de pila, guarda f18-f31 con psq_st),
   el cálculo de las olas que luego dibuja 8000D000— y SinFIdx 0,33. Es coma flotante de Broadway emulada en
   double: el método N (r19n) mostró que eso solo baja reescribiéndolo en single exacto, a mano. Candidato de (b)
   o de un frente N específico (~−0,5 ms si se lograra la mitad). NO lo he tocado.
2. Sondeo y reloj 0,66 (K: `NWII_POLL_EVERY=128` midió −0,4 en K11).
3. Ayudantes de g7 (psq/ps 0,74, bctrl 0,31): código generado, (b).
4. Actores en general (6,4 ms; jugadores 1,2, 80830720 1,37): código generado del juego, sin nada específico del agua
   salvo el punto 1.
5. La espera del mutex de sonido (0,5 ms) no cuenta como «ms juego» (es tiempo ocioso del SO invitado), pero alarga
   el fotograma: A/C.
Para leer W01 (si llega a correr: m6e + TPROF + NX_PROFILE + V_TL): `python3 r9/r35m4/tools/tprof_m4.py <bench> <nm de
r41m/bin/m6e_pgo.elf> fases|esperas|hilos --escena agua_pinguino` y `python3 r9/r44w/tools/tprof_w.py <bench> <nm>
propio|wgp --escena agua_pinguino`; la traza V: `r9/r37v/tools/vtl_escenas.sh <bench> <bench>/tprof_vtl_W01.bin`.

## 8. Los otros líquidos (tipos 1, 2, 4) — comprobados en PC (w3)
8000D000 salta según el tipo (this+0xCE94) a 5 rutas; la del tipo 1 depende de mundo/nivel/zona (bytes 4620/4621/4622
del objeto en r13-21688). Instrumentado (`[WAGUA] rutas acumuladas`) y comprobado con `NWII_W_AGUA_CHECK`
(`tools/chk_demos.sh`, `tools/chk_niveles.sh` con `NWII_FORCE_LEVEL`; `test/chk_w3todos.txt`, `test/chk_niv.txt`):
| ruta | tipo | niveles donde sale (PC) | llamadas comprobadas | pipe · registros · memoria distintas |
|---|---|---|---|---|
| 8000B530 | 0 (agua) | agua_pinguino 1-4, playa_hielo 4-1, playa_yoshi 4-5, casa_oscura 5-G | 5 276 + 1 641 + 1 695 + 1 160 | 0 · 0 · ruido* |
| 8000BA70 | 1 (lava) | castillo6 6-C, lava 8-7, 8-C | 1 573 + 1 212 + 1 160 | 0 · 0 · 0 |
| 8000C5C0 | 1, caso W2-C zona 0 / W8-3 / W8-C zona 3 | 2-C, 8-3 | 1 489 + 1 275 (+1 694) | 0 · 0 · 0 (ruido* 3 en la repetición) |
| 8000CA50 | 2 (pantano venenoso) | jungla 5-4 | 1 403 | 0 · 0 · 0 |
| 8000C020 | 4 (arena movediza) | 2-5 | 1 699 | 0 · 0 · 0 |
*ruido = la zona 80378d20-80384590 que otro hilo escribe (el control nulo da lo mismo, §bitácora).
No pasan por ella: desierto4j, cielo_paratroopas, nubes_plataformas, castillo (película), pradera, nieve, torre3,
desierto_helices, cielo_hoja, 2-2, 2-3, 2-6.
**Por defecto (w3) la versión nativa se usa en las 5 rutas** (todas comprobadas); el tipo sin dibujo (ruta «ninguna»)
va a la generada. Válvulas: `NWII_W_AGUA_SOLO0="1"` = nativa solo para el tipo 0 (el resto, generada; probado en
2-C: 2 685 llamadas al generado), `NWII_W_AGUA_TODOS="1"` = también la ruta «ninguna».
**Para M**: el parche de runtime NO cambia respecto a w2 (`w3_runtime_vs_m6e.patch` = `w2_runtime_vs_m6e.patch`); solo
cambia `w_agua/w_agua_hook.inc` (rutas, válvulas, estadística): vuelve a copiarlo junto a w_agua_gen.cpp.
NRO w3 `bin/w3_pgo.nro` (sha256 en `bin/w3_pgo.sha256`).

## 9. Plan (b): «regenerar + PGO» final con rutas rápidas psq/ps y caché de bctrl (NO hecho; coste estimado)
Qué se toca (todo en la pasada G7, sin tocar el recompilador ni el runtime salvo lo dicho):
1. `r9/r33g7/tools/g7.h` (cabecera del código generado; va en el export, no en el runtime):
   - psq_l/psq_st **cuantizados** (GQR u16/s16/u8/s8 con escala, hoy `psq_l_gen/psq_st_gen` fuera de línea: 0,18 +
     0,18 ms en agua): ruta en línea para el GQR que el juego usa de verdad (GQR3 u16 escala 0, el de
     `OSInitFastCast`, y los 2-5 que fija nw4r), probando `ctx.gqr[I]` contra la constante y cayendo al general si
     no; misma conversión `(float)u16 * scale` y misma `ppc_quantize` → exacto por construcción.
   - `ps_madds0/1`, `ps_madd`, `ps_msub` (hoy G7_NOINL, 0,38 ms): versión en línea SOLO del camino sin empate
     (`madd_s` con `fma` + prueba del empate, que ya es en línea) y la rama de empate fuera; hoy se dejaron fuera de
     línea por tamaño (+30-50 % en las funciones de matrices) → hacerlo solo en las ~40 funciones calientes de la
     lista del perfil (PSMTXConcat 801C0540, PSMTXConcatArray 801C0610, PSMTXInverse 801C07A0, animación g3d
     802403C0, CalcWorld 80250E70…), con una lista de funciones en `apply_g7.py --feat psil` (patrón de `inl_sel.tsv`).
   - `g7_bctrl_g6` (0,31 ms): caché de un destino por sitio de bctrl (`static uint32_t last_pc; static body_fn last_fn;`
     por sitio, comparar y llamar; fallo → la tabla hash de siempre). Exacto: misma función destino. Riesgo: la tabla
     hash se monta con `g_disp_table` (el gancho de W tiene que instalarse antes, ya lo hace).
2. `r9/r33g7/tools/apply_g7.py`: dos feats nuevas (`psil`, `bcache`) que emiten esas llamadas en los sitios.
3. Cadena: `apply_g7.py` sobre el export de m4 (r35m4/export) → `apply_s2.py` (pasada S2) → compilar.
Coste (con las cifras de r33g7/r35m4/r17pgo):
- escribir y probar las dos feats + cputest (gen_tests G7) + comparador por función (difftest, 2 semillas): **3-4 h**;
- export nuevo + objetos de PC (`build_m4obj.sh`, ~6 min JOBS 5) + checks 1x/FC: **1 h**;
- PGO: instrumentada en la otra laptop (~35-40 min PJOBS 8), **2 entrenamientos en consola de 900 s** (Z01/Z02,
  con S2 la instrumentada va a ~6 fot/s), finish (~11-15 min JOBS 11), orden L + pares de K (k3: re-enlace, ~10 min):
  **~2,5-3 h de reloj, ~35 min de consola**;
- medida A/B en consola (control + variante, 2 × 9 min).
Total **≈ 7-8 h** de reloj (≈ 1 h de consola). Ganancia estimada en agua: −0,4 a −0,6 ms (psq/ps −0,3, bctrl −0,15).
Riesgos: (1) el perfil nuevo cambia TODO el código generado (nuevas decisiones de GCC): puede mover ±0,3 ms en
cualquier escena, por eso hay que medir las 5; (2) una feat mal hecha rompe exactitud en sitios raros de GQR → la
verificación (cputest + difftest + checks + FC) es obligatoria antes de entrenar; (3) entrenar con S2 encendido a ~6
fot/s sesga el perfil hacia el sonido (visto en m4); (4) la integración con k3 (orden L y pares) y con la fusión de
D hay que rehacerla sobre los objetos nuevos (las herramientas de K ya lo hacen con KOBJ=…).

## 10. `NWII_W_SIM="1"`: simulación de las olas (807B5240 → 800ECAE0) — exacta, ganancia SIN medir en consola
Qué hace el código del juego (agua_pinguino, por fotograma): 800ECAE0 recorre los 1 101 vértices de la superficie
(bucle 800ED8C4) y, por vértice, 2 SinFIdx + ~60 operaciones single + un bucle de 20 «huecos» (800ED9B8; casi todos
vacíos: lbz + cmp + salto) + guardar el vértice (8000D0D0/8000D0A0). En PC: ~780 µs por llamada.
Cómo (sin regenerar, mismo esquema que el agua): `tools/gen_w_sim.py <export> w_agua/w_sim_gen.inc --fpr` copia el
código generado exacto de 807B5240 (execute del actor del REL; llega por g_disp_table; llama a 800ECAE0 DIRECTAMENTE,
por eso hay que copiar también el llamador) y de 800ECAE0, y en 800ECAE0 pasa **los 32 FPR y sus ps1 a variables
locales** (`w_agua/w_sim_fpr.h`: mismas expresiones de cpu_context.h/g7.h para fill/fadds/fsubs/fdivs/frsp/fctiwz/
fmuls/psq_l/psq_st), con volcado completo a ctx antes de cada llamada, sondeo lento y return, y recarga después
(también antes del `goto` en las llamadas que acaban saltando: era el fallo de f5 de la primera versión).
NO cambia ninguna operación: redondeos, FPSCR (no modelado), desnormales, orden y pares quedan como en el generado.
Gancho: `w_agua_install` engancha 0x807B5240 si `NWII_W_SIM`/`NWII_W_SIM_CHECK` (no toca main.cpp). `[WSIM]` cada 5 s
(llamadas, µs/llamada). `NWII_W_SIM_GEN=1`: gancho con la generada (para medir el tiempo de la misma forma).
Verificación (PC, `tools/pc_s2.sh s2`, máquina cargada: load 28-29 de otros trabajos):
- `NWII_W_SIM_CHECK` (generada contra nativa, MSR[EE]=0, 300 s de agua): **4 244 llamadas, pipe 0, registros 0,
  memoria 11** (todas en la zona ruidosa 8037xxxx-80384590); control nulo (generada contra generada): 2 915 llamadas,
  memoria **89** en la misma zona → ruido del método, no la nativa (`test/chk_sim_s2.txt`).
- FRAMECHECK 5/5 idénticas con AGUA+WGP+SIM y las comprobaciones de M (240/220/240/240/240), D40CHECK 0, coalesce 0
  (`test/fc_s2_on.txt`); checks 1x idénticas las 4 (180/160/180/180 con la máquina cargada: menos fotogramas), diff_lines=0,
  S2 0 trampas, vigía 0; «perdidos» de S2 97-257 = carga de la máquina (lo mismo vio M con dos compilaciones a la vez).
- **Tiempo en PC (x86): sin diferencia** (generada 700-850 µs/llamada, nativa 740-825; ruido). En x86 el reenvío
  almacén→carga es barato; en el A57 puede no serlo, pero NO tengo medida: la ganancia en consola es desconocida
  (mi estimación: 0 a −0,3 ms). Para −0,5 ms o más haría falta reescribir el cuerpo por vértice en nativo de verdad
  (single del anfitrión exacto: operandos single → fmul/fadd de float = resultado de Broadway por el teorema de doble
  redondeo con 53 ≥ 2·24+2; SinFIdx en línea; los 20 huecos sin el protocolo de llamada) con todos sus efectos
  (pila 80-140(r1), r0/r3-r7/r19/r20/r24, cr, ps1, sondeos): ~1 día de trabajo con la comprobación ya hecha.
NRO **s2** = w3 + SIM: `bin/s2_pgo.nro` 4cd2aa8b…8283 (elf 736e84e8…b739, fastmem 868 500, 0 mal formadas).
A/B propuesto (mismo NRO, contiguas): control `NWII_W_AGUA="1";NWII_W_WGP="1"` / variante `+ NWII_W_SIM="1"`.
**Para M**: el parche de runtime sigue igual que w2; hay 2 ficheros más en w_agua/ (`w_sim_fpr.h`, `w_sim_gen.inc`
generado con `tools/gen_w_sim.py r9/r35m4/export <dir>/w_sim_gen.inc --fpr`), que w_agua_gen.cpp incluye (vuelve a
generar w_agua_gen.cpp con gen_w_agua.py y copia los 5: w_agua_gen.cpp, w_agua_cola.h, w_agua_hook.inc, w_sim_gen.inc,
w_sim_fpr.h). `tools/build_pgo_w.sh` / `build_w.sh` ya lo hacen.

## 11. `NWII_W_SIM="2"` (s3): SinFIdx integrado y psq cuantizado en línea — exacta
- v1 (`NWII_W_SIM="1"`) vuelca/recarga los 64 FPR+ps1 en cada llamada: con 2 SinFIdx por vértice son ~280 000 accesos
  a memoria más por fotograma → en el A57 puede ir MÁS LENTA que el generado (sin medir).
- v2 (`NWII_W_SIM="2"`, prefijo `w_sim2`, `gen_w_sim.py --fpr --inline=80237BD0,8000D0D0,8000D0A0`): el código
  generado de SinFIdx (hoja, 108 líneas) va DENTRO de 800ECAE0 como lambda `always_inline` sobre los mismos FPR
  locales (sin volcados; su sondeo de la reducción vuelca/recarga; su salida G6BIT, que solo llega tras un sondeo que
  ya volcó y puede haber cambiado de hilo, devuelve sin volcar, como el generado). 4 llamadas integradas
  (8000D0D0/8000D0A0 ya los había integrado G6). Además `W_PSQ_L/W_PSQ_ST` con GQR cuantizado usan copias EN LÍNEA
  de `psq_l_gen/psq_st_gen` (`w_psq_l_q/w_psq_st_q`), igual en PC y en la Switch (la comprobación de PC prueba ese
  mismo código).
- Comprobación (PC, máquina con load 30-36): v1 1 171 llamadas, v2 1 258 + 1 802: **pipe 0, registros 0**; memoria
  solo en la zona ruidosa 8037xxxx-80384590 (0 fuera).
- FRAMECHECK 5/5 idénticas con AGUA+WGP+SIM=2 y las comprobaciones de M (240/220/220/220/200; load 34), D40CHECK 0
  distintos, coalesce 0 (`test/fc_s3_on.txt`). Checks 1x (`test/checks_s3_on.txt`): la máquina NO bajó de load 6 en 4 h (otros trabajos; se corrió con load 57):
  title/map/walk/lvl11 **idénticas 180/160/160/180** (no 220/220: con esa carga el arnés hace menos fotogramas en su
  tiempo), diff_lines=0, S2 0 trampas, vigía 0, cmp_png map 1,8 %, lvl11 4,0 %, title t_006300 (otra demo de atracción)
  y walk (cámara desplazada) como siempre; «perdidos» de S2 = carga.
- `tools/gen_w_sim_all.sh <export> <salida>` genera las dos versiones en `w_sim_gen.inc`; `[WSIM] … versión N`.
- **NRO s3** `bin/s3_pgo.nro` sha256 8dfdc7b885b1044af76abc013f128f0560b0d640f49b7263ec35e894c30e5610 (elf baaf29ae…68b9,
  fastmem 869 222, 0 mal formadas). A/B: control `NWII_W_AGUA="1";NWII_W_WGP="1"` / variante `+ NWII_W_SIM="2"`.
- **Receta para M (árbol m6i, r9/r41m)**: tu `w_agua/` está FIJO en w3. Para SIM: sustituir los 3 ficheros por los 5
  que compiló s3 (copia exacta en `nsmbw-build:r9/r44w/wagua_s3/`: w_agua_gen.cpp, w_agua_cola.h (igual),
  w_agua_hook.inc, w_sim_gen.inc, w_sim_fpr.h; también en `r9/r44w/w_agua/`). El parche de runtime NO cambia (es el
  de w2; el gancho de SIM vive en w_agua_hook.inc, llamado por el mismo `w_agua_install`). Sin `NWII_W_SIM` el hook nuevo
  hace lo mismo que el de w3 (las rutas, válvulas y estadística del agua no cambian; lo demás nuevo solo actúa en
  los modos de comprobación). Para encenderla por defecto: `NWII_W_SIM=2` en `m6_defaults()`.

## Consola (medido por el coordinador sobre m6h, 1020/460)
- M22 m6h por defecto: agua 14,5 ms juego. **M24 m6h + AGUA + WGP: agua 14,5 → 13,6 ms juego (−0,9)**. Con todo
  (M27/M28, + MERGE de D) el agua dibuja 57,4 fot/s.

## Bitácora
- 17:15 W01_w_m6e_tprof encolada al final (m6e + `NWII_TPROF` + `NWII_V_TL=…/tprof_vtl_W01.bin`, PULL_PREFIX=tprof).
- 17:45 informe intermedio (auditoría + palancas). Coordinador: sí a a2 (`NWII_W_AGUA`) y a1 (`NWII_W_WGP`).
- 18:xx PC: `NWII_W_AGUA=1` agua_pinguino 100 s: 60 llamadas/s, tipo 0, 1 100 cuerpos/llamada, 66 000 entregas/s
  (2,9 M escrituras/s por bloque), 0 lentas, 60 fot/s. Primer intento de comprobación colgaba el juego (interrupción
  entregada en la primera ejecución y perdida): arreglado con EE=0 durante la comprobación.
- Comprobación (w1): 1 538 llamadas: pipe 0, registros 0, memoria 2; 2 148 llamadas: pipe 0, registros 0, memoria 1
  (427 palabras desde 80378d20).
- **Comprobación w2, 400 s de agua (`test/chk_agua_w2.txt`)**: nativa contra generada **5 276 llamadas, 281,7 M
  escrituras al pipe: pipe 0, registros 0, memoria 1** (498 palabras en 80378d20-803841d0, todas «tocadas por la
  original»). **Control nulo** (generada contra generada, `NWII_W_AGUA_CHECK_NULL=1`): 5 247 llamadas, pipe 0,
  registros 0, **memoria 2** (446 y 595 palabras en 80379xxx-80384590). Misma zona y misma frecuencia → es ruido del
  método (otro hilo escribe esa zona de MEM1 durante la primera ejecución; la función no la toca: lo que toca de
  verdad son ~47-55 palabras de __GXData en 8038fcxx), no la versión nativa.
- (18:30) Aviso del coordinador: maestro con `profile = false` desde las 18:10. TPROF cuelga del muestreador →
  añadido `NWII_NX_PROFILE="1"` a la línea DEBUG_EXTRA de W01 en la cola (solo texto) y anotado en AVISO_BASE.
- Selftest de WGP: 4 000 secuencias, 800 000 escrituras por camino, 0 distintas.
- PC w2 con AGUA+WGP y las comprobaciones de M: **FRAMECHECK 5/5 idénticas** (260/240/260/280/260), D40CHECK 0
  distintos, coalesce 0 (`test/fc_w2_on.txt`); **checks 1x 220/220** title/lvl11 (map/walk 200/200), diff_lines=0, S2 0
  trampas, vigía 0 (`test/checks_w2_on.txt`); cmp_png: map 2,0 %, lvl11 3,5 %, title t_006300 = otra demo de atracción
  (mirada: limpia), walk = cámara desplazada conocida. Las otras 4 demos no pasan por 8000D000 (`test/chk_w1.txt`).
- Encoladas W02 (m6e ctl) / W03 agua / W04 wgp / W05 ambos; el coordinador las retira: la consola está ocupada por el
  usuario y M integra w2 en m6h (mide M22-M26). Integración: §6.
- NRO w1 (solo AGUA) 602bc67c…1214; **NRO w2 (AGUA + WGP)** `bin/w2_pgo.nro` aa8ca000…8cdd, elf 0432dc1e…0680; fastmem 0 mal formadas.
