# r43a: frente A — abaratar la CPU de sonido S2 (15-nov-2026)

Base: runtime **m6e** (`r9/r41m/nWiiRuntime_m6e`, copia intacta en `orig/nWiiRuntime_m6e`); mi árbol: `nWiiRuntime/`.
NRO = objetos PGO del juego de m5 + este runtime sin perfil (`tools/build_pgo.sh <tag>` = build_pgo_m6.sh de r41m con
rutas de r43a). PC: `tools/build_pc.sh <n>` (= build_m6.sh). Sin git; no se toca PrepFrame/FifoParser ni sondeos del
hilo del juego ni el código generado.

## RESUMEN (19:45)
- **Dónde se va la CPU de sonido** (agua, consola M04: 26 % del núcleo 2 = ~260 ms/s; [S2] irq 160-180 + hilo 81-88 ms/s):
  mezcla AX nativa 68 ms/s (205 µs/lista; 10,8× más lenta que en PC por funciones no integradas en -O2 aarch64), reverb
  AXFX nativa ~29 ms/s (44 µs × ~650/s), el resto (~160 ms/s) código invitado nw4r::snd/AX + sobrecoste S2 (en PC: invitado
  ~40 % del hilo, núcleo/futex 9 %, S2 5 %, reloj 4 %, cesiones en bucle 280-3000/s). El invitado no lo baja el runtime.
- **Cambios** (todos con interruptor, apagados por defecto): `NWII_A_MIX=1|2` (mezclador), `NWII_AXFX_KERNEL=4` (reverb,
  código generado: `gen/output_24.cpp`), `NWII_A_NOSPIN=1`, `NWII_A_REQSPIN=n`, `NWII_A_PROF_SND=1`, línea `[A43]`.
- **Parches**: `parches/a_vs_m6e.patch` (runtime; aplica igual en m6g; ya integrado por M en m6h);
  `parches/output_24_a_vs_m5.patch` (kernel 4; receta con objetos de K al final). NRO de prueba (base m6e): `bin/a3_pgo.nro`
  (runtime), `bin/a5_pgo.nro` (runtime + MIX=2 + kernel 4).
- **PC**: todo verificado (AXV 0 distintas, kcheck 0 distintas, FC 5/5, checks 220/220, estrés sin cuelgue, audio en el ruido).
- **Consola**: sin medir aquí (cola parada por el usuario; A01-A07 retiradas por el coordinador; lo mide él como M25/M27
  sobre m6h).

## Interruptores nuevos (todos apagados por defecto; encender con "1" o un número)
| variable | qué hace |
|---|---|
| `NWII_A_MIX=1` | mezclador AX rápido (bit a bit igual; `ax_hle.cpp`) |
| `NWII_A_NOSPIN=1` | esperas cortas (< 50 µs al siguiente evento) de la CPU de sonido durmiendo en vez de ceder en bucle |
| `NWII_A_REQSPIN=n` | vueltas de giro de una petición al juego antes de dormir (m6e: 1500) |
| `NWII_A_PROF_SND=1` | (medida) el perfilador de muestras de la consola muestrea también la CPU de sonido → `profile_snd.txt` |
Línea nueva `[A43]` junto a `[S2]`/`[C38]`: esperas/cesiones/siestas del hilo de sonido, sondeos, irq DSP vs AID, peticiones.

## Herramientas
- `tools/pcrun.sh`, `tools/audio_run.sh`, `test/checks.sh`, `test/demos_fc.sh`, `tools/pc_checks.sh`, `tools/estres.sh`:
  copias de las de r41m con arnés propio (`pc/`, candado `pc/.runlock`).
- `tools/pc_verif.sh <n> <tag> [env]`: audio determinista con/sin (cmp byte a byte), FC 5 demos, checks 1x, estrés ×2.
- `tools/perf_snd.sh <bin> <nombre> <demo> [env]`: perf de PC del proceso; `tools/cat_perf.py <perf.data> <tid>`: reparto
  del hilo de sonido por categorías.

## Bitácora
- 17:10 Lectura de r34s2, r38c, r41m (m6e), r32dx §0. tprof (`r35m4/tools/tprof_m4.py`) NO cubre el hilo de sonido (solo
  muestrea `g_ctx_ptr`, el contexto del juego) → perfil de PC con `perf` (hilo de sonido) y, en consola, el perfilador de
  muestras ampliado (`NWII_A_PROF_SND=1`, ver tabla).
- 17:20 OJO: la primera medida de perf cogió por error el proceso de la prueba de estrés de M (mismo binario, `pgrep -n`)
  → 22 % en `snd_poll` = el bucle de `NWII_C_STRESS_IRQ_US`. Descartada; `perf_snd.sh` busca ahora el proceso por su
  carpeta de trabajo.
- 17:25 **Medidas de partida**. Consola (M04 = m6b «todo11», 1020/460, agua): hilo snd 26 % del núcleo 2; línea [S2]:
  irq 160-180 ms/s + hilo 81-88 ms/s; mezcla AX nativa `mix 205 us/lista` en agua (desierto 172, nubes 170, cielo 163,
  castillo 113, título 60) × 330 listas/s = **68 ms/s ≈ 26 % del hilo**; reverb AXFX nativa (kernel 2) 44 µs/llamada ×
  ~650/s ≈ **29 ms/s**. PC (perf, agua, hilo de sonido, %): nw4r::snd invitado ~32, mezcla nativa ~24, AXFX nativa ~12,
  AX invitado ~6, núcleo (futex/sueño) ~9, S2 ~5, reloj ~4, ceder ~1. La mezcla en consola va **10,8× más lenta que en
  PC** (205 frente a 19 µs; lo normal es 4-6×, p. ej. AXFX 44/7,7): en el ELF de la Switch (-O2, cortex-a57)
  `Accelerator::read_sample` y `aram8` NO estaban integrados (dos llamadas con marco por muestra, `aram8` de 664 bytes con
  el registro de lecturas dentro, globales recargados con adrp). En PC sí (process_voice de 20 KB).
- 17:28 **a1/a2/a3**: `NWII_A_MIX=1` (process_voice_fast con plantillas por formato, todo integrado; voces paradas sin leer el
  PB entero). En el ELF de a1: `process_voice_fast<0,0>` 0x6f8 bytes, bucle de ADPCM ~40 instrucciones por muestra sin
  llamadas. **PC NWII_AX_VERIFY=1 (contra la mezcla r7b de referencia): 45 000 listas, 0 distintas** (5 demos).
  a2 = a1 + línea [A43] + `NWII_A_NOSPIN`, `NWII_A_REQSPIN`; a3 = a2 + `NWII_A_PROF_SND` (muestreador a 0x2B para poder
  muestrear a la CPU de sonido, que está a 0x2C en el mismo núcleo) + contadores de S y ganchos.
  PC (agua, a3): [A43] esperas cv 680-990/s, **cesiones 390-3150/s** (bucles de `yield` cuando el siguiente evento está a
  < 50 µs: en Horizon no sueltan el núcleo al trabajador), sondeos 13-15 k/s, S tomado 1,4-5 k/s, ganchos SO 2 k/s,
  peticiones 667/s.
- 17:40 En cola (al final, contiguas, 400 s, `NWII_A_PROF_SND=1` en las tres): **A01_a3_ctl**, **A02_a3_mix**
  (`NWII_A_MIX=1`), **A03_a3_mixspin** (+ `NWII_A_NOSPIN=1 NWII_A_REQSPIN=200`). Se retiran si falla la verificación de PC
  (en marcha: `test/verif_a1mix.txt`, `test/verif_a2spin.txt`, `test/aud_mix.txt`).
- 17:45 **a4/a5: reverb AXFX, kernel 4** (`NWII_AXFX_KERNEL=4`, código generado: `gen/output_24.cpp` = r35m4/export/output_24.cpp
  = r17pgo/m4src/export/output_24.cpp + kernel 4 AÑADIDO AL FINAL; dentro del archivo solo se tocan, sin añadir líneas, la
  selección de kernel, kcheck y el informe; primera línea `#line 1 "<ruta original>"` para que los números de línea y el
  nombre del archivo, que entran en las sumas de control del PGO, no cambien). Mismas operaciones en float que el kernel 2
  y en el mismo orden POR CANAL, pero L/R/S en los carriles de un vector de 4 (las líneas de retardo de cada canal son
  regiones disjuntas: el llamador ya lo comprueba para usar el kernel 2). El kernel 2 en el ELF: 265 ldr / 136 str (derrames)
  y la cadena de dependencias de cada canal en serie. PGO: `GEN24=1 tools/build_pgo.sh` recompila output_24 con LA MISMA orden
  de m5_pgo (+ `-Wno-error=coverage-mismatch`); avisos de perfil SOLO en las 3 funciones tocadas (nwii_r9_axfx_fast, kcheck,
  report); comparado con nm: todas las demás funciones del ELF tienen el mismo tamaño que en a3. PC: `GEN24=1
  tools/build_pc.sh` enlaza `gen/pc/output_24.o` (orden de r35m4/m4/pc_obj/compile.cmd).
  **PC kcheck (kernel 4 contra kernel 1 en CADA llamada, 5 demos): 63 000 llamadas, 0 distintas** (a4).
- 17:50 **`NWII_A_MIX=2`**: además, las muestras ADPCM de entrada se decodifican en lote antes del remuestreo (cuántas pide
  el remuestreo solo depende de la fracción, la razón y el tipo); dentro de una trama de 8 bytes y lejos del final, bucle
  corto (coeficientes, escala y puntero fijos); el nibble que cierra la trama, el final y lo raro, por `read<0,0>`.
- 17:55 Audio DET (reloj virtual, S2 apagado): NO es determinista ni control contra control (se separan a los ~18 s, en el
  título) → `tools/aud.sh` mide el primer byte distinto de control-control y de control-variante; la exactitud del
  mezclador la da NWII_AX_VERIFY. a1 MIX=1: control-variante primer byte distinto en 2,32-2,46 MB (18-19 s) = el mismo
  punto que control-control (2,39 MB).
- 17:58 PC (agua, máquina cargada y en power-saver; solo orientativo, la Switch es otra cosa): reverb kernel 2 23,6 →
  kernel 4 20,1 µs/llamada; mezcla MIX=0 33-35 → MIX=2 27-29 µs/lista. (En x86 sin SSSE3 el barajado de bytes del kernel 4
  salía carísimo — 86 µs —: en x86 se usa bswap escalar, en aarch64 rev32; las dos formas dan lo mismo, la del barajado es
  la que pasó el kcheck de a4.) a6 = a5 con ese cambio solo para x86 (el NRO a5 vale).
- 18:00 En cola, detrás de A01-A03 (con dos trabajos ajenos en medio), contiguas: **A04_a5_ctl** (sin interruptores) y
  **A05_a5_all** (`NWII_A_MIX=2 NWII_AXFX_KERNEL=4 NWII_A_NOSPIN=1 NWII_A_REQSPIN=200`), ambas con `NWII_A_PROF_SND=1`.
  Verificación de PC de a5 en marcha (`test/verif_a5all.txt`, `pc/runs/a5chk` = AXV + kcheck juntos).
- 18:15 **Cambio de base (r9/tools_comunes/AVISO_BASE_1115_1810.md)**: el maestro lleva `profile = false` desde las 18:10.
  A01-A05 corren todas después → son corridas de fps limpias SIN perfilador (`NWII_A_PROF_SND=1` queda inerte: el
  perfilador no arranca); su desglose sale de las líneas [S2]/[A43]/mezcla/[A2]. Para el desglose por funciones de la
  CPU de sonido en consola, par aparte con el perfilador encendido (`NWII_NX_PROFILE="1"` + `NWII_A_PROF_SND="1"`):
  **A06_a5_ctl_perfil** y **A07_a5_all_perfil** (contiguas, al final). No comparar sus fps con A01-A05.
- 18:50 **Verificación de PC de A01-A03: PASA** (`test/verif_a1mix.txt`, `test/verif_a2spin.txt`, `test/aud_mix.txt`):
  * MIX=1: AXV 45 000 listas 0 distintas; MIX=2: AXV 69 000 listas (5 demos) 0 distintas; kernel 4: kcheck 66 000 0 distintas.
  * FC 5 demos (FRAMECHECK 540/520/520/540/520-540, coalesce 0) y checks 1x title/map/walk/lvl11 220/220 diff_lines=0, S2
    0 trampas/asserts/perdidos, vigía 0, en MIX=1 y en MIX=1+NOSPIN+REQSPIN=200. cmp_png contra c1/m5 = lo mismo que m6e
    (título: otra demo de atracción; walk: cámara del mapa; map/lvl11 ≤ 2,3 %).
  * Estrés (prueba c + `NWII_C_STRESS_IRQ_US=100` + AXLIST_CHECK, PCSCHED en 8,9,10, 2×150 s cada una): 0 cuelgues, 0
    NullCall, 0 listas rotas, vigía 0, AID 333/s.
  * Audio tiempo real MIX=1 (100 s, título+atracción): control-variante correlación espectral mediana 0,998 / mín. 0,947,
    nivel mediana 0,03 dB; control-control 0,998 / 0,888, 0,02 dB; 0 huecos; clics 30 frente a 27-31 → dentro del ruido.
- 19:10 Coordinador: la cola está PARADA (el usuario usa la consola); A01-A07 siguen encoladas. La base de referencia es
  ahora m6g (m6f + objetos k2 de K + POLL_EVERY; agua: gxw 63 %, snd 28 %, 52 dibujados). Mis tres archivos (s2.cpp,
  ax_hle.cpp, switch_profiler.cpp) son iguales en m6e y m6g: `parches/a_vs_m6e.patch` aplica limpio en m6g (dry-run).
  El kernel 4 va en el código generado: con los objetos de K hay que recompilar `gen/output_24.cpp` con las banderas de K.
  Informe intermedio enviado al coordinador.
- 19:20 **Verificación de PC de a5 «todo» (MIX=2 + KERNEL=4 + NOSPIN + REQSPIN=200): PASA** (`test/verif_a5all.txt`): FC 5/5
  (560/560/560/580/560, coalesce 0), checks 1x 220/220 diff_lines=0 (lvl11 cmp_png 3,2 % = animación al ritmo de la
  música, FRAMECHECK idéntico), S2 0 trampas, vigía 0; estrés 2×150 s: 0 cuelgues, 0 NullCall, 0 rotas; audio dentro del
  ruido (arriba).
- 19:40 Coordinador: el parche de runtime ya está en **m6h** (M; interruptores apagados). Mide M25 (m6h) frente a M27 (M25 +
  `NWII_A_MIX="1";NWII_A_NOSPIN="1";NWII_A_REQSPIN="200"`). **A01-A07 RETIRADAS de la cola por el coordinador** (eran
  sobre m6e): no volver a encolarlas. El kernel 4 de la reverb (código generado) queda para otra ronda: receta abajo.
- 19:40 PC perf con todo (`runs/p6_all`, agua; orientativo, x86 cargado): reparto del hilo de sonido mezcla 52 → 47,
  AXFX 26 → 22 (kernel 4 18,4 frente a kernel 2 21 µs/llamada), cesiones 965-2627/s → 0 (siestas cortas 55/s); el resto
  igual. En x86 la mezcla y la reverb ya iban integradas/rápidas: la ganancia de verdad se espera en la consola.
  OJO para quien repita: en zsh `$var` sin comillas NO se parte en palabras (la primera tanda de `p6_all` corrió solo con
  MIX=2 por eso); las medidas buenas son las de `bash -c`.

## RECETA: kernel 4 de la reverb (gen/output_24.cpp) con los objetos k2 de K y el enlace de M
El objeto que cambia es UNO: output_24 (código generado, con perfil). Se recompila con la orden EXACTA de K para ese
fuente (`r42k/nx/k2/k_cmds.txt`, banderas k2 `-fno-jump-tables -freorder-blocks-and-partition` + perfil m4_all) cambiando
solo la fuente y añadiendo `-Wno-error=coverage-mismatch`; la primera línea de `gen/output_24.cpp` (`#line 1 "<ruta
original>"`) mantiene nombre y números de línea, y el kernel nuevo va al FINAL del archivo, así que el perfil casa con
todo salvo las 3 funciones tocadas (aviso esperado SOLO para nwii_r9_axfx_fast, nwii_a2_axfx_kcheck, nwii_a2_axfx_report).
En la otra laptop (no tocar r42k: copiar a una carpeta de M, p. ej. r41m/nx/k2a43):
```
L=/home/djvemo/nsmbw-port/r9; K=$L/r42k/nx/k2; N=$L/r41m/nx/k2a43          # N: carpeta nueva de M
rsync -a $L/r43a/gen/output_24.cpp nsmbw-build:$L/r43a/gen_k2/output_24.cpp  # (desde esta laptop)
ssh nsmbw-build                                                              # lo que sigue, alli:
cp -a --reflink=auto $K $N && cd $N
grep -E -- "-c $L/r17pgo/m4src/export/output_24\.cpp\$" k_cmds.txt \
 | sed -e "s#$K#$N#g" -e "s#-c $L/r17pgo/m4src/export/output_24\.cpp\$#-Wno-error=coverage-mismatch -c $L/r43a/gen_k2/output_24.cpp#" > a43_24.sh
cat a43_24.sh   # una sola orden; -o igual que antes (CMakeFiles/.../m4src/export/output_24.o), -fprofile-prefix-path=$N
export DEVKITPRO=/opt/devkitpro; bash a43_24.sh 2>&1 | grep -c coverage   # avisos solo de las 3 funciones de arriba
```
Luego el enlace con orden L/pares como m6g/m6h (`KOBJ=k2a43 tools/order_m6.sh …`, con `KOBJ` apuntando a esa carpeta;
order_m6.sh lee hoy `r42k/nx/<KOBJ>`: o se crea la copia allí con permiso de K o se cambia esa ruta). Comprobaciones:
fastmem 0 mal formadas; `nm -S` del ELF nuevo frente al de m6h: solo cambian de tamaño esas 3 funciones y aparecen
`nwii_a43_axfx_k4` y su lambda de prefetch. Se enciende con `NWII_AXFX_KERNEL="4"`; la línea `[A43] axfx kernel4 X us/call`
sale junto a la `[A2]`. Comprobación de PC: `NWII_AXFX_KERNEL=4 NWII_AXFX_KCHECK=1` (kernel 4 frente al 1 en cada
llamada; `GEN24=1 tools/build_pc.sh <n>` enlaza `gen/pc/output_24.o`). Hecho en a4/a5 con los objetos de m5:
66 000 llamadas, 0 distintas.
