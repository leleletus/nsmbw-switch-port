# r41m: frente M — integración m6 (m5 + V + C + D [+ R2]) — 15-nov-2026

Base: `r9/r35m4/nWiiRuntime_m5` (runtime de m5). NRO = objetos PGO del juego de m5 (`r17pgo/nx/m5_pgo`) + runtime
sin perfil (no se rehace el PGO). Runtime propio en `nWiiRuntime/`. No se editan las carpetas de otros frentes ni
r35m4. Sin git.

## Qué entra en m6a
| frente | fuente exacta usada | interruptores (apagados salvo lo indicado) |
|---|---|---|
| V v37a | `r40d/base_v37a/nWiiRuntime` (= r37v/remote_src a las 13:06, antes de v37b) | `NWII_V_WAKE`, `NWII_V_LATE`, `NWII_V_TL`, `NWII_V_SLOWTEST` (PC) |
| D d2 | `r40d/nWiiRuntime` (= v37a + `r40d_vs_v37a.patch`) | `NWII_D_DROPFAST`, `_CHECK`, `NWII_D_SLOWWORKER` (PC) |
| C c1 | `r17pgo/c38src_c1` de la otra laptop (fuentes exactas del NRO c1) | arreglo de esperas y vigilante ENCENDIDOS (`NWII_C_SPIN_OFF=1`, `NWII_C_VIGIA_OFF=1` los quitan); `NWII_C_PCSCHED`, estrés (PC/pruebas) |
| C c2 | `r17pgo/c38src_c2` (= r38c/nWiiRuntime actual) | `NWII_C_PRIO_HI`, `NWII_C_LATE_US`, `NWII_C_CATCHUP`, `NWII_C_CATCHUP_GAP_US` |
NO entra: `NWII_V_DROPFAST` (v37b; duplica a D, se queda D, orden del coordinador).

Parches por frente en `parches/` (diff -ruN de src/): `v37a_vs_m5.diff`, `d2_vs_v37a.diff`, `c1_vs_m5.diff`,
`c2_vs_c1.diff`; parche único de m6a contra m5: `m6a_vs_m5.patch`.

## Herramientas
- `tools/build_m6.sh <n>`: PC (`bin/NSMBW_<n>[_input_test]`), runtime de aquí + objetos m4 de r35m4 (copia de build_d.sh).
- `tools/build_pgo_m6.sh <tag> [src]`: NRO en la otra laptop; recompila con las órdenes exactas de ninja de m5_pgo las
  fuentes que difieren de m5src y las que incluyen una cabecera que difiere (`s2.h`, `switch_platform.h`).
- `test/demos_fc.sh`, `test/checks.sh` + arnés `pc/` (copias de r40d).

## Bitácora
- 14:00 inicio. Leídos LEEME de r35m4 §11, r37v, r38c, r40d. r38c/nWiiRuntime = c2 exacto; c1 traído de la otra laptop.
- 14:10 Unión: V+D (D aplica limpio sobre v37a, por construcción) y C encima con diff3 (base m5). main.cpp sin
  conflicto (V: `vtl_init` antes de `s2::init`; C: `pc_sched(0)` después; las dos se quedan). gx.cpp: UN conflicto,
  trivial: V (`VSlowTest`, solo PC) y C (`s_c38_xfb`, contador del vigilante) declaran cosas nuevas en el mismo
  sitio, antes de `PipeNoteXfb` → se quedan las dos. Comprobado que las líneas que la unión añade sobre V+D son
  exactamente las de c1 sobre m5 (gx.cpp y main.cpp). Resto de archivos de C sin solapes.
- c2 sobre c1: aplica limpio. Va todo detrás de interruptores apagados (`g_prio_hi = 0` → `boost`/`unboost` vuelven
  sin hacer nada; `NWII_C_CATCHUP` 0 → `s_c38_theo_next = 0` y el camino de m5) SALVO la línea `[C38]` del registro
  de S2 (una vez por segundo, sin coste apreciable). Se integra.
- Revisión de esperas nuevas (riesgo del cuelgue de m5): V no añade ninguna; D solo añade `NWII_D_SLOWWORKER`, que
  no existe en el NRO (`#if !defined(__SWITCH__)`).
- 14:05 PC `bin/NSMBW_m6a[_input_test]` compilado (sin avisos nuevos). Parche único `m6a_vs_m5.patch`
  (`cd <copia de nWiiRuntime_m5> && patch -p2 < m6a_vs_m5.patch`; comprobado que reproduce `nWiiRuntime/`).
- 14:15 **Comprobación de `build_pgo_m6.sh`** reconstruyendo los NRO de los frentes:
  * v37a (fuentes `r40d/base_v37a`): `bin/rv37a_pgo.nro/.elf` **byte a byte** = `r37v/bin/v37a_pgo.*`
    (3694589…0903a / 7f78255…e882). Las fuentes de D-base son, pues, las de v37a.
  * c1 (fuentes `r17pgo/c38src_c1`): igual salvo 32 bytes: las tres cadenas `__DATE__ __TIME__` de
    switch_platform.cpp/main (hora de compilación) y la `GNU build-id` (hash del contenido). Comprobado con un
    script que esas son las ÚNICAS diferencias. (v37a no recompila switch_platform.cpp → sale idéntico.)
- 14:06 **NRO m6a** `bin/m6a_pgo.nro` sha256 d83d9f83753cb364de7bbbac9d4ebdd8845d21ba4712d4e1b19720b0792bf005,
  elf 53e1ad58…8dd41 (`bin/m6a_pgo.sha256`); recompiladas 12 fuentes (las 11 cambiadas + includers de s2.h /
  switch_platform.h); fastmem 866 754 entradas, **0 mal formadas** (= m5). Un aviso (Pal3Slow, ya en m5). En el ELF
  están NWII_V_WAKE/LATE, D_DROPFAST, C_* y `[VIGIA]`; NO está NWII_V_DROPFAST.
- 14:08 Encoladas (detrás de V08, ~10 trabajos delante; se retiran si falla la verificación de PC):
  M01_m6a_off (entorno de C02), M02_m6a_on (entorno de D02), M03_m6a_on_largo (1500 s, capturas cada 20 s).
  `tools/encolar_m.sh`.
- 14:20 **(a) PC, todo apagado** (`tools/pc_checks_m6.sh m6a off` → `test/checks_off.txt`; checks.sh 1x con
  `NWII_S2_CHECK=1`, S2 y R por defecto como en m5/c1): title/map/walk/lvl11 **220/220 FRAMECHECK idénticas**,
  `diff_lines=0`, rcoal 0 discrepancias, S2 ACTIVO(2) 0 trampas/asserts, vigía 0 avisos, ninguna línea `[V]`,
  salida normal (exit 124 = timeout). = c1 (`r38c/runs/checks_c1.txt`: 220/220 las cuatro, pass1 differs 15/0/0/115;
  aquí 21/0/0/115). cmp_png contra c1: map 2,0 %, lvl11 2,2 %, title ≤2,6 % salvo t_006300 (otra demo de atracción,
  igual que c1 contra m5: 98,9 %), walk 34 % desde t_001800 = cámara del mapa desplazada (conocido desde s2a; mirado:
  misma escena). Una diferencia con c1: walk «perdidos 1» (aviso de AX descartado en una carga; ver r35m4 §11, inocuo).
- 14:35 **(b) PC, encendido** (`test/fc_m6a_on.txt`; `test/demos_fc.sh` 5 demos 1,5789x 100 s, FRAMECHECK cada 10,
  `NWII_V_WAKE=1 NWII_V_LATE=8 NWII_D_DROPFAST=1 NWII_D_DROPFAST_CHECK=1 NWII_D_SLOWWORKER=6
  NWII_R11_SLOW_RENDER_US=12000` + COALESCE_CHECK): **5/5 idénticas** (desierto 540/540, agua 520/520, cielo
  540/540, nubes 560/560, castillo 520/520), coalesce 0 discrepancias, **D40CHECK 185 lotes / 4,76 M órdenes, 0
  distintos**, todos los lotes con varios finales por el camino barato. (= d2: 5/5, 299 lotes 0 distintos.)
- 14:30 **(c) PC con prioridades de Horizon + estrés** (`tools/estres_m6.sh`, `test/estres_m6a.txt`: `NWII_C_PCSCHED=1
  NWII_C_STRESS_FIFO_US=300 NWII_C_STRESS_SFIFO=1`, desierto4j↔agua, 150 s; corridas en `pc/runs/estres_*`):
  * encendido (V+D): 150 s sin cuelgue, 58,7 fps, AID 333/s, vigía 0.
  * control con las esperas de m5 (`NWII_C_SPIN_OFF=1`): se cuelga al arrancar (vigía: S del juego en una
    interrupción, la CPU de sonido espera S 5,6 s, candado del FIFO tomado) = el cuelgue de m5 que C arregla. La
    prueba sigue siendo capaz de provocarlo.
  * **apagado: CUELGUE a los ~85 s (desierto4j)**, de OTRO tipo: la CPU de sonido tiene S dentro de una interrupción
    y no sale de un bucle del invitado en 0x8027B91C-0x8027B938 (recorre una lista enlazada con centinela en
    0x80412F78 = objeto estático 0x80412BF0 + 0x388 —cuyo mutex 0x80412F44 es el que sale en la petición pendiente
    de S2— y llama al método virtual +0x10 de cada nodo): 122 267 `[NullCall] site 0x8027b930` = un nodo sin
    tabla virtual → la lista está rota (nodo que no vuelve al centinela). El juego espera S para siempre; al deshacer
    PCSCHED (válvula del vigía) NO se recupera → no es inversión de prioridades. AID 0/s. El sitio 0x8027b930 no
    aparece en NINGÚN otro registro de r9 ni de switch/real_logs.
  * Repetición (`tools/repro_null.sh`, misma prueba, `test/repro_null*.txt`): 150 s c1 ×2, m6a ×2 y 300 s c2/m6a/c1 ×2:
    **ningún otro cuelgue** (total: m6a 1 cuelgue en 5 corridas/1 050 s; c1 0 en 4/900 s; c2 0 en 2/600 s). Raro; m6a con todo apagado ejecuta el mismo código que c2 (V y D
    solo dejan un contador atómico y comprobaciones de interruptor), así que lo atribuyo (INFERIDO) a S2/C y no a la
    unión. Hipótesis (no demostrada): la función de 0x8027B8xx es la llamada de AX de `nw4r::snd::detail::SoundThread`
    (justo antes de `SoundThreadProc` 0x8027B960; hace OSSendMessage a la cola +0x320 y recorre la lista +0x388); en
    la consola original corre con interrupciones apagadas y nadie más toca la lista a la vez; con S2 corre en la CPU
    de sonido con S tomado, y un nodo destruido/reutilizado por el juego sin S (memoria liberada desde código
    clasificado como SDK-no-sonido, `snd_class`) la rompe. Para el frente C.
- 14:45 **m6b** (segunda etapa): R2 midió r2g en consola (R2_04/R2_05: render −2-3 ms/fot con «los 8») y su parche
  actual `r39r2_vs_m5.patch` (14:02) es **r2k** (comprobado: m5 + parche con `build_pgo_m6.sh` = `r2_r2k.nro` byte a
  byte, d9999952…0b7e). Copia: `parches/r2k_vs_m5.patch`. Sobre m6a aplica limpio (solo desplazamientos).
  Revisión de choques R2×D: (1) LATEFREE devuelve en ProcessGXFifo DESPUÉS de `RenderPacket`, que ya pone y
  quita `g_render_start` alrededor de `Render()` → nada queda colgado; (2) LATEFREE libera el vector entero como
  FREECHUNK (D ya lo revisó: válido con `start`); si para a medias, el trabajador reutiliza el paquete y PrepFrame
  vuelve a fijar `start`; `RetractToPrefix` solo ve paquetes listos, nunca el retenido; (3) los añadidos de R2 dentro
  de RenderOnce van en el bucle que D hace empezar en `g_render_start`; PREHASH/TEXMEMO no recorren órdenes.
  R2×C/V: sin código común. Árbol `nWiiRuntime_m6b/`, PC `bin/NSMBW_m6b[_input_test]`
  (`SUF=_m6b RTD=nWiiRuntime_m6b tools/build_m6.sh m6b`).
- 15:05 **NRO m6b** `bin/m6b_pgo.nro` sha256 547460ce8dc3daf9a1b78d4bb535af69ccfc274aa0bbcf551a4ee8960ac0aad6, elf
  10dcd609…c57a (`bin/m6b_pgo.sha256`); fastmem 0 mal formadas. Parches: `m6b_vs_m5.patch` (único contra m5),
  `m6b_vs_m6a.patch` (= las líneas de r2k exactas, comprobado).
- 15:10 **PC m6b** (`tools/pc_m6b.sh`): (a) todo apagado `test/checks_m6b_off.txt`: 220/220 las cuatro, diff_lines=0,
  S2 0 trampas, vigía 0, salida normal (= m6a). (b) `test/fc_m6b_on.txt`: V+D+R2 «todo11» con las comprobaciones de D
  y R2, modo uniforme + SSO como en la Switch, trabajador ×6 y render +12 ms: **5/5 idénticas** (440/380/420/460/420),
  coalesce 0, **D40CHECK 418 lotes / 10,6 M órdenes 0 distintos**, PREHASH_CHECK 401 k comparaciones 0 distintas,
  CLEARTEX_CHECK 0 píxeles distintos.
- 15:20 Encolada **M04_m6b_on** (V+D+R2 «todo11», S2 por defecto; detrás de M03 y de R2_10/11).
- 15:30 (b2) checks 1x con TODO encendido (V+D+R2 «todo11» + comprobaciones; `test/checks_m6b_on.txt`): 220/220 las
  cuatro, diff_lines=0, D40CHECK 0 distintos, coalesce 0, S2 0 trampas, vigía 0, salida normal; cmp_png contra c1
  ≤2,1 % salvo title t_006300 (otra demo de atracción) y walk (cámara desplazada, conocido).
- 15:40 (c) m6b con TODO encendido (V+D+R2) + PCSCHED + estrés, 300 s (`test/estres_m6b.txt`): sin cuelgue, 59,4 fps, AID 333/s, vigía 0, NullCall 0.
- 15:50 **CONSOLA: la misma firma en D07** (`bench_D07_d7_allp_1115_1536`, NRO d7 de D = v37a + D, sin el arreglo de C):
  928 917 `[NullCall] site 0x8027b930 lr=0x8027b934` desde la línea 1830 (poco después de empezar desierto4j), registro
  cortado por el límite de tamaño, 6 ventanas. Confirma que el cuelgue existe en la consola y que no lo introduce la
  unión (está en el linaje m5/S2). Avisado al coordinador.

## RECETA de la prueba (c) — cuelgue NullCall 0x8027b930 (para el frente C)
Binario de PC: `r9/r41m/bin/NSMBW_m6a` (o el de C: `r9/r38c/bin/NSMBW_c1` / `_c2`; con todo apagado ejecutan el mismo
código de S2). Arnés: `r9/r41m/tools/pcrun.sh` (= r38c/tools/pcrun.sh con NAND/toml de `r41m/pc/`; offscreen,
`SDL_AUDIODRIVER=dummy`, `NWII_FPS=1`, `NWII_REALTIME=1`, sin volcado de audio). Orden exacta:
```
cd /home/djvemo/nsmbw-port/r9/r41m
flock pc/.runlock tools/pcrun.sh bin/NSMBW_m6a <nombre> 150 \
  NWII_NO_SAVE=1 NWII_FORCE_DEMO=desierto4j,agua_pinguino NWII_FORCE_DEMO_LOOP=1 \
  NWII_C_PCSCHED=1 NWII_C_STRESS_FIFO_US=300 NWII_C_STRESS_SFIFO=1
```
(PCSCHED con las CPUs por defecto 2,3,4.) Se repite con `tools/repro_null.sh <n>` (`BINS="c1:../r38c/bin/NSMBW_c1
m6a:bin/NSMBW_m6a" SECS=150 TAGR=x`). Señal: `grep -c NullCall pc/runs/<nombre>/run.log` > 0 y `[VIGIA] #1`.
Caso cogido: `pc/runs/estres_off/run.log` (cuelgue a los ~85 s, 38 s después de empezar desierto4j; primera NullCall
en la línea 2375; vigía en 2401-2418). Frecuencia en PC: 1 de 11 corridas (m6a 1/5, c1 0/4, c2 0/2).
En consola: `switch/real_logs/bench_D07_d7_allp_1115_1536` (d7 de D, sin el arreglo de C), línea 1830.
Estado en el cuelgue (vigía, PC): S=2 (sonido), sonido `S 1 irq 1`, contexto de sonido pc 0x8027B938 lr 0x8027B934;
juego en pc 0x801B119C esperando S; petición S2 pendiente op 2 sobre 0x80412F44 (mutex del objeto 0x80412BF0, que
también es un OSThread: `th=0x80412bf0 prio=4 waitq=0x80412f18` en el volcado de hilos de D07). Bucle del invitado
(ppcdis 8027B8E0-8027B940): `lwzu r31,0x388(r30)` (r30 = 0x80412BF0) … `lwz r12,8(r3); lwz r12,0x10(r12); bctrl` y
`cmplw r31,r30` → recorre la lista con centinela 0x80412F78 llamando al virtual +0x10; un nodo con vtable 0 = lista rota.

## Bitácora (sigue, 15:45-)
- 15:45 Coordinador: R2 cerró con **r2m** (`r39r2_vs_m5.patch` sha256 6d29f7d5…f774; COPYMRT/HASHPF nuevos, sin medir).
  m6b rehecho con r2m (`parches/r2m_vs_m5.patch`; gx.cpp de R2 igual que en r2k, cambian renderer_gl/texture_cache):
  comprobado que m5 + parche con `build_pgo_m6.sh` = `r2_r2m.nro` byte a byte (d860f98d…198a). **NRO m6b (r2m)**
  `bin/m6b_pgo.nro` sha256 dfe61c47051e02121c076e6e65356d3b73c554334cd653be989a5d788cd44e12, elf 10186f34…b8f6,
  fastmem 0 mal formadas. El de r2k queda como `bin/m6b_r2k_pgo.*` (547460ce…). M04 re-apuntado al NRO r2m (misma
  posición; mismo entorno «todo11», sin COPYMRT/HASHPF). Pruebas de PC de m6b(r2k) renombradas `test/*_r2k.txt`;
  se repiten con r2m (`tools/pc_m6b.sh`).
- 15:50 Receta de la prueba (c) para el frente C: sección «RECETA» arriba.
- 15:55 Encargo LOGBG: `frame_tick()` hace cada 2 s fflush(stdout) + `g_out_sink.flush()` (fwrite hasta 256 KB + fsync
  a la SD) en el hilo de render con el mutex del sumidero tomado; además, sin el interruptor, cualquier `write()` que
  llene el búfer de 256 KB del FILE escribe a la SD en el hilo que imprime. **m6c** = m6b + (solo switch_platform.cpp,
  `parches/m6c_vs_m6b.patch`):
  * medida SIEMPRE (línea `[LOGF]` cada 5 s): vaciado en render n/media/máx ms, `write()` máx (pared, espera del mutex
    incluida; lo que ve cualquier hilo que imprime), hilo de fondo n/media/máx/KB y bytes tirados.
  * `NWII_M_LOGBG="1"`: `write()` solo copia a `pend` bajo el mutex (tope 8 MB; si se pasa, tira y lo cuenta); hilo
    `logbg` (libnx, prioridad 0x3B, núcleo ideal 2 con máscara de los núcleos permitidos 0-2) cada 2 s o al avisarle
    frame_tick intercambia `pend` por `wbuf` y hace fwrite + fflush + fsync con un mutex propio (`fm`), SIN el de los
    escritores. `flush()` (vigía `c38_sink_flush`, frame_tick sin fondo) y `close()` vacían de forma síncrona, así que
    el vigía sigue llegando a la SD aunque el hilo de fondo no corra. Sin el interruptor el camino es el de siempre.
  * Solo existe en la Switch (switch_platform.cpp no se compila en PC): verificado compilando el NRO.
  **NRO m6c** `bin/m6c_pgo.nro` sha256 8967c4dc7d3a9bb9888cde29dc90fb08275cf2f77015fafb9f97b7c44d83b061, fastmem 0.
  Encoladas al final (detrás de M02/M04/M03, R2, M01, K01/K02): **M05_m6c_logsync** / **M06_m6c_logbg**, contiguas,
  entorno de M04 (V+D+R2 «todo11»), 400 s.
- 15:58 **M02_m6a_on** (`bench_M02_m6a_on_1115_1545`; V_WAKE=1, V_LATE=8, D_DROPFAST=1): 80 ventanas, media 54,3, mín.
  36,5; 7 demos, las 5 escenas; 84/84 líneas a 1020/460 MHz; IMAGEN OK 2/36; hoja `capturas.jpg` mirada: limpia
  (negras = fundidos/iris). Dibujados · juego · ms juego REAL · render: desierto 58,5·59,8·11,6·14,4 · agua
  44,2·53,7·15,5·16,2 · cielo 58,6·59,9·14,6·14,8 · nubes 59,9·59,9·11,5·10,5 · castillo 53,5·59,5·6,5·13,9 · fiesta
  47,9·59,9·–·18,7 (trabajador 16,6). **= D02** (58,6 · 43,4 · 58,6 · 59,9 · 54,1; fiesta 48,4) dentro del ruido
  entre corridas (D vio agua 39,9 y 43,4 con la misma configuración). Audio: AID 330-332/s, underruns 59 (D02 65,
  C02 57). Vigía: 1 aviso al arrancar (render fot 1 = la carga inicial; igual en C02-C08), 0 NullCall.
- 16:05 **M04_m6b_on** (`bench_M04_m6b_on_1115_1554`; NRO m6b r2m, V+D+R2 «todo11»): 80 ventanas, media 54,7, mín. 32,8;
  84/84 a 1020/460; IMAGEN OK 3/37; hoja mirada: limpia (tiles animados del desierto/agua bien; negras = fundidos/iris).
  Dibujados · juego · ms juego REAL · render (M02 → M04): desierto 58,5→**59,2** · 59,8 · 11,3 · 14,4→**10,8** | agua
  44,2→44,0 · 54,6 · 15,4 · 16,2→**12,0** | cielo 58,6→59,1 · 59,9 · 14,5 · 14,8→12,3 | nubes 59,9 · 11,2 · 10,5→8,0 |
  castillo 53,5→**55,6** · 59,5 · 6,4 · 13,9→11,7 | fiesta 47,9→**51,7** (render 18,7→16,0; trabajador 16,6→16,0).
  Audio AID 329-332/s, underruns 63. Vigía: solo el aviso de arranque. 0 NullCall. Lectura (inferida): R2 baja el render
  2,5-4 ms/fot; agua no sube porque el límite ahí es el trabajador (16 ms/fot preparado; terreno de D: d5/d7).
  Tirones (`tools/tirones.py`, intervalos del juego > 30 ms por minuto): desierto 11,1→12,5, agua 133→103, castillo
  16,2→16,2.
- 16:05 Coordinador: M03 cancelada → M09_m6b_on_largo (1500 s, entorno de M04) y M07_m6b_snd0 (las analiza él).
  Siguiente objetivo **m6d** = m6b + arreglo de C (NullCall) + d5/d7 de D + COPYMRT (si R2_10/11 ganan), con los
  interruptores elegidos ENCENDIDOS POR DEFECTO.
- 16:10 **Mecanismo de «encendido por defecto»** (árbol `nWiiRuntime_m6d` = m6c + esto; main.cpp `m6_defaults()`,
  llamado tras `early_init` (debug.toml/env.txt ya aplicados) y antes de cualquier lectura): por cada entrada de la
  tabla, si la variable NO está, `setenv(valor)`; un valor explícito manda. Apagar: `<NOMBRE>_OFF="1"` (uno),
  `NWII_V_OFF` / `NWII_D_OFF` / `NWII_R2_OFF` (grupo; `NWII_R_OFF` de m5 también quita CULL=2), `NWII_M6_OFF` (todos).
  Línea `[M6] por defecto: … | apagados: …` al arrancar. Tabla actual = entorno de M04: V_WAKE=1, V_LATE=8,
  D_DROPFAST=1, R2 LATEFREE/CLEARTEX/COPYFBO/LAZYNAT/COPYUNI/COPYST/LAZYEFB/PREHASH/TEXMEMO/XDUBO=1, R_CULL=2.
  Comprobado que todos los lectores de esas variables están dentro de funciones (ninguno en inicialización estática).
  NRO de prueba **m6d0** (`bin/m6d0_pgo.nro` 121d749b…5ebb, fastmem 0); PC `bin/NSMBW_m6d0`. Pruebas: `tools/pc_m6d.sh`.
- 16:10 PC m6b (r2m) repetidas (`test/checks_m6b_off.txt`, `fc_m6b_on.txt`, `checks_m6b_on.txt`): todas idénticas
  (FC 5/5; menos comprobaciones por la máquina cargada con compilaciones).
- 16:30 **PC m6d0 sin variables** (`test/fc_m6d0_def.txt`, `test/checks_m6d0_def.txt`): la línea `[M6]` pone los 14;
  FC 5/5 idénticas (400/340/400/460/380), coalesce 0, **D40CHECK 855 lotes / 22,1 M órdenes 0 distintos**, PREHASH
  0 distintas, CLEARTEX 0 píxeles; checks 1x 220/220 las cuatro, S2 0 trampas/perdidos, vigía 0. Apagados:
  `NWII_M6_OFF=1` → ninguno (sin [V] ni [R2]); `NWII_V_LATE_OFF=1 NWII_R2_OFF=1` → quedan V_WAKE, D_DROPFAST, CULL=2;
  `NWII_V_LATE=4 NWII_R_OFF=1` → V_LATE=4 «explícito», CULL=2 apagado por R_OFF. (Nota: en `checks_m6b_on.txt` de r2m
  salieron «perdidos» 212/127/20/5 de S2 con la máquina cargada por dos compilaciones a la vez; en m6d0 con la misma
  configuración y la máquina libre, 0. Avisos de AX descartados por cola llena = carga del PC, inocuo.)

## RESUMEN (16:35)
| | NRO | sha256 | qué es |
|---|---|---|---|
| m6a | `bin/m6a_pgo.nro` | d83d9f83…f005 | m5 + V(v37a) + D(d2) + C(c1+c2); nuevos apagados salvo arreglo de esperas + vigía de C |
| m6b | `bin/m6b_pgo.nro` | dfe61c47…4e12 | m6a + R2 r2m (apagados) |
| m6c | `bin/m6c_pgo.nro` | 8967c4dc…b061 | m6b + `[LOGF]` + `NWII_M_LOGBG` |
| m6d0 | `bin/m6d0_pgo.nro` | 121d749b…5ebb | m6c + `m6_defaults()` (V+D+R2 «todo11» ENCENDIDOS por defecto) — falta C/D/COPYMRT |
Parches únicos contra m5: `m6a_vs_m5.patch`, `m6b_vs_m5.patch`; deltas `m6b_vs_m6a.patch`, `parches/m6c_vs_m6b.patch`.
Pendiente: M05/M06 (A/B de LOGBG, al final de la cola); M01 (m6a apagado); m6d = m6d0 + arreglo de C para NullCall +
d5/d7 de D + COPYMRT (si R2_10/11 ganan) → añadir sus interruptores a la tabla de `m6_defaults()` y recompilar con
`tools/build_pgo_m6.sh m6d nWiiRuntime_m6d` + `SUF=_m6d RTD=nWiiRuntime_m6d tools/build_m6.sh m6d` + `tools/pc_m6d.sh m6d`.

## m6e (16:50-): m6d0 + arreglo de C para el cuelgue NullCall (c5)
- Coordinador: integrar c5 (`r9/r38c/c5_vs_m6a.patch`, sha256 02cbaf54…dcee, 16:41). El parche trae por error un
  `os_hle_stubs.cpp.orig` nuevo: quitado (`parches/c5_vs_m6a_sin_orig.patch`, solo s2.cpp + os_hle_stubs.cpp).
  s2.cpp/os_hle_stubs.cpp de m6d0 = los de m6a → aplica exacto (sin desplazamientos). Resultado: s2.cpp y s2.h =
  `r38c/nWiiRuntime` (c5); os_hle_stubs.cpp = c5 + lo de V. Arreglo encendido por defecto en su código
  (`NWII_C_AXLIST_OFF="1"` lo quita); `[NullCall]` limitado a 20 líneas + contador; ALARMA del vigía.
- `m6_defaults()`: nueva entrada PREPARADA pero apagada `NWII_POLL_EVERY=128` (grupo `NWII_K_OFF`; campo `def=false`;
  sale en `[M6] … apagados: NWII_POLL_EVERY(preparado)`). LOGBG fuera de la tabla hasta ver M05/M06.
- **NRO m6e** `bin/m6e_pgo.nro` sha256 dc7cc0f957d8e9b902eee80a6a134557a83b81a89bf8f10a158dba755cf1fb05, elf
  35c8189b…ded9, fastmem 866 754 entradas 0 mal formadas. PC `bin/NSMBW_m6e[_input_test]`. Parches: `m6e_vs_m5.patch`
  (único contra m5), `parches/m6e_vs_m6b.patch`.
- Encoladas al final (detrás de C10/C11): **M10_m6e_def** (SIN interruptores: solo BACKEND, NO_SAVE, FORCE_DEMO,
  FORCE_DEMO_LOOP; 400 s; debe igualar a M04) y **M11_m6e_def_largo** (1500 s).
- PC en marcha: `tools/pc_m6d.sh m6e` (FC 5/5 por defecto + comprobaciones, checks 1x, líneas [M6]) y
  `tools/estres_m6e.sh bin/NSMBW_m6e m6e 5` (prueba (c) + `NWII_C_STRESS_IRQ_US=100` + `NWII_C_AXLIST_CHECK=1`, PCSCHED
  en CPUs 5,6,7 porque C corre su cadena en 2,3,4 a la vez).
- 17:10 **PC m6e, valores por defecto** (`test/fc_m6e_def.txt`, `test/checks_m6e_def.txt`): FC 5/5 idénticas
  (400/380/420/440/400), coalesce 0, D40CHECK 664 lotes / 16,8 M órdenes 0 distintos, PREHASH 0 distintas, CLEARTEX 0
  px; `[M6]` = los 14 + `NWII_POLL_EVERY(preparado)` apagado. Checks 1x 220/220 las cuatro, diff_lines=0, S2 0
  trampas (walk «perdidos 12»: avisos AX en carga, inocuo), vigía 0.
- Cadena de C (c4, `r38c/runs`): checks 220/220 las cuatro; axl6 off/on 3+3 sin cuelgue. Su comparación de audio
  falló (buscaba los .wav en r34s2/runs; `audio_run.sh` los deja en r35m4/runs): hecha aquí con `wavq.py`: c4 con
  arreglo frente a `NWII_C_AXLIST_OFF=1`, envolvente 0,996, nivel mediana 0,01 dB (máx. 1,79), espectral mediana 0,999
  (mín. 0,989), 0 huecos, clics 22 frente a 25 (= ruido entre corridas; m5 dio 0,975/0,999).
- 17:05 Coordinador adelantó M10/M11 (detrás de D08-D10) y canceló C10/C11. Guarda puesta: si M10 empieza antes de
  acabar el estrés de PC, M11 pasa a `.job.retenido`.
- 17:30 **Estrés m6e** (`test/estres_m6e.txt`; prueba (c) + `NWII_C_STRESS_IRQ_US=100` + `NWII_C_AXLIST_CHECK=1`, valores
  por defecto, 5 × 150 s): **0 cuelgues, 0 listas rotas, 0 NullCall, vigía 0**; altas/bajas 9-15/s, 48-57 con la CPU
  de sonido en su interrupción por corrida, «sin S» 0, espera de S del juego 0,09-0,16 ms/s, AID 332-333/s, 58,6-59,4 fps.
  (Comparación: sin arreglo C vio 1 cuelgue de 3 con este estrés de bucle.) M11 NO se retuvo: el PC acabó antes de M10.
- 17:40 **Audio PC m6e** (`test/audio_m6e.txt`, título + demos 100 s, tiempo real, `wavq.py`): arreglo encendido frente a
  `NWII_C_AXLIST_OFF=1`: envolvente 0,973-0,975, nivel mediana 0,04-0,08 dB, espectral mediana 0,996-0,999, 0 huecos,
  clics 20-26 en todas. Ruido entre dos corridas IGUALES (encendido ×2): envolvente 0,979, espectral 0,998, ventanas
  peores en los mismos segundos (64-66 s, donde las demos de atracción divergen). Lectura: igual dentro del ruido
  (inferido; no hay comparación determinista posible con S2 en tiempo real).

## m6f (17:35-): m6e + fase 2 de D
- Parche `r40d/m6e1_vs_m6e.patch` (17:28, sha256 a35168a0…9d28; copia en `parches/`): fifo_parser.h (solo
  declaraciones estáticas nuevas: sin cambio de disposición), fifo_parser.cpp, renderer_gl.cpp, gx.cpp. Aplica exacto
  sobre `nWiiRuntime_m6e`. `build_pgo_m6.sh` ahora admite include/ cambiado (recompila los includers con -I del
  include de la fuente, solo si cambia; como `r40d/tools/build_pgo_m6d.sh`).
- **OJO**: m6e + este parche NO reproduce `r40d/bin/m6e1_pgo.nro` (430bee2b…, ELF 17:11): 848 bytes de diferencia
  (PrepFrame 0x4c28 frente a 0x4a00; fin del TLS 0x540 frente a 0x580). El NRO de D (el de D11/D12) es de una versión
  anterior del parche; el árbol de D sigue cambiando (MERGE_CHECK a las 17:32). Avisado al coordinador. Integro el parche.
- `m6_defaults()` añade ENCENDIDOS `NWII_D_PARSESTOP`, `NWII_D_LIGHTPARSE`, `NWII_D_FASTDECODE`, `NWII_D_FASTPREP`
  (grupo `NWII_D_OFF`); `NWII_D_RENDERPREP` PREPARADO apagado; `NWII_D_EAGERDECODE` fuera; `NWII_POLL_EVERY` preparado.
- **NRO m6f** `bin/m6f_pgo.nro` sha256 8945edb69329b9cd2cb37b48901f2730cba608f1550bb30b1b3a24fb7a60e4f0, elf
  1e65d046…1ac0, fastmem 0 mal formadas. PC `bin/NSMBW_m6f[_input_test]`. Parches `m6f_vs_m5.patch`,
  `parches/m6f_vs_m6e.patch`. Encoladas al final: **M13_m6f_def** (400 s) y **M14_m6f_def_largo** (1500 s), sin interruptores.
- PC en marcha: `tools/pc_m6f.sh m6f` (FC con todas las comprobaciones de D y R2; cielo ×6 de 120 s con FRAMECHECK y
  trabajador lento por el riesgo que anotó D; checks 1x; estrés (c) + STRESS_IRQ 5×150 s).
- 17:55 **PC m6f FC** (`test/fc_m6f_def.txt`, valores por defecto + comprobaciones de D y R2, trabajador ×6, render
  +12 ms; el portátil estaba en perfil «power-saver» → menos comprobaciones): 5/5 FRAMECHECK idénticas
  (240/240/260/260/240), coalesce 0, D40CHECK 3 656 lotes / 73 M órdenes 0 distintos, DEC_CHECK 0, PREP_CHECK 0,
  predicción de LIGHTPARSE fallida 0, vigía 0. **Nuevo**: castillo PREHASH_CHECK «35 874 comparados, 21 distintos»
  (resto de demos y m6b/m6d0/m6e: 0): RAM de una textura escrita por el juego entre el hash en el tiempo ocioso del
  render y su uso → esa textura podría ir 1 fotograma atrasada. FRAMECHECK siguió idéntico. INFERIDO: la fase 2 de D
  entrega antes los fotogramas y el render tiene más tiempo ocioso para pre-hashear antes de que el juego acabe de
  escribir; es un efecto de R2 PREHASH (no de D). Lo vigilo en las demás corridas de m6f.
- 17:55 **m6g** (encargo nuevo): runtime `nWiiRuntime_m6g` = m6f + `NWII_POLL_EVERY=128` ENCENDIDO por defecto; base de
  enlace `nx/m6grt` (`bin/m6grt_pgo.nro` 8d49e943…0989, fastmem 0). Herramienta `tools/order_m6.sh` = copia de
  `r42k/tools/order_k.sh` con salidas en r41m (lee los objetos k2 de `r42k/nx/k2` sin tocarlos); + `tools/cruces.py`,
  `tools/orden_pares.py` (copias). Pendiente: perfiles de M10 (m6e) para el orden L.
- 18:10 **M10_m6e_def** (`bench_M10_m6e_def_1115_1758`, m6e SIN interruptores): `[M6]` aplica los 14; 80 ventanas, media
  54,3, mín. 31,6; 84/84 a 1020/460; IMAGEN OK 1/34; hoja mirada: limpia. Frente a M04 (mismo entorno puesto a mano):
  desierto 59,1 (59,2) · agua 43,2 (44,0) · cielo 58,3 (59,1) · nubes 60,0 · castillo 54,3 (55,6) · fiesta 50,9 (51,7);
  render 11,3/12,2/13,0/8,2/11,9 (10,8/12,0/12,3/8,0/11,7); ms juego REAL iguales. = M04 dentro del ruido. Audio AID
  329-333/s, underruns 70 (63). Vigía: solo el de arranque; 0 NullCall. **Los valores por defecto funcionan en consola.**
- 18:05 **cielo ×6** (m6f, `test/cielo_m6f.txt`, 120 s, FRAMECHECK, trabajador ×6, render +12 ms, todas las
  comprobaciones): 6/6 idénticas (200/200/160/200/200/200), vigía 0, SIGSEGV 0, comprobaciones de D 0, PREHASH 0
  distintas. El fallo que vio D en cielo NO aparece.
- 18:20 **m6g** = runtime m6g (m6f + POLL_EVERY=128 por defecto; base `nx/m6grt`) + objetos k2 de K (88 con perfil,
  `-fno-jump-tables -freorder-blocks-and-partition`) + pares + orden L con los perfiles de **M10** (m6e, el último con
  perfilador; 113 muestras de runtime sin traducir por nombre, como las 41 de K): `KOBJ=k2 tools/order_m6.sh r41m:m6grt
  m6g …@bin/m6e_pgo.elf` (la ssh se quedó colgada al final, como avisaba K: el remoto había terminado; bajados a mano).
  Comprobaciones: fastmem 870 167 entradas **0 mal formadas**; eqcheck palabra a palabra contra el enlace base: 101 858
  funciones, 101 825 iguales, 30 distintas, 0 faltan, y las 19 «DIFERENCIA REAL» son **exactamente las mismas 19
  funciones que en k3e de K** (las que saltan a su parte fría); «.text distinto del base» también sale en k3e (los
  objetos k2 no son los del base). **NRO m6g** `bin/m6g.nro` sha256
  696106746ed63537815fa0ba48fe02c82284c01cfc0671bcfb2b0ebcea6badb2, elf 12fd1eeb…61b2; orden e informe en `order/m6g/`.
  En PC no cambia nada (solo banderas de la Switch). Encoladas al final: **M15_m6g_def** y **M16_m6g_def_largo**.
- 18:40 **Estrés m6f** (`test/estres_m6f.txt`, 5×150 s, prueba (c) + STRESS_IRQ + AXLIST_CHECK, valores por defecto):
  0 cuelgues, 0 listas rotas, 0 NullCall, vigía 0, AID 333/s, 58,9-59,1 fps. Checks 1x m6f (`test/checks_m6f_def.txt`):
  220/220 las cuatro, D40CHECK 0, S2 0 trampas, vigía 0. **m6f verificado en PC.**
- 18:40 PREHASH (pregunta del coordinador): el hash adelantado se USA TAL CUAL (`texture_cache_gl.cpp`:
  `if (r2_pre_take(...)) e.data_hash = ph;` para texturas y `c.ram_hash = ph` para copias EFB; `PREHASH_CHECK` solo
  cuenta). Un «distinto» = el juego escribió esa RAM entre el hash ocioso y el uso → si el contenido viejo coincide con
  la textura en caché, se dibuja la textura vieja ese fotograma (al siguiente se rehace). Matiz: el hash en línea
  tampoco es «el bueno» (el juego ya va por el fotograma siguiente y la RAM se lee después), pero no puedo demostrar
  que sea inocuo → en el próximo build (m6h) `NWII_R2_PREHASH` pasa a «preparado, apagado». Encolada
  **M17_m6g_noprehash** (= M15 + `NWII_R2_PREHASH_OFF="1"`, 400 s). En marcha: FC de castillo ×3 con m6e y ×3 con m6f
  (`tools/castillo_prehash.sh`, `test/castillo_m6{e,f}.txt`).
- 19:05 **FC castillo ×3** (`test/castillo_m6e.txt`, `test/castillo_m6f.txt`, PREHASH_CHECK): m6e 21 · 21 · 0 distintos
  (de 74-82 k); m6f 0 · 0 · 0 (de 62-64 k); FRAMECHECK idéntico en las 6. Es decir: NO lo trae la fase 2 de D; es de
  R2 PREHASH y ya estaba en m6e (el 0 anterior era suerte). Siempre exactamente 21 cuando sale → INFERIDO: un suceso
  concreto del castillo (una textura que el juego reescribe en un momento dado), no ruido. Decisión pendiente del
  coordinador: PREHASH «preparado, apagado» en m6h; M17 mide cuánto cuesta.

## m6h (19:00-): m6g + MERGE (D) + W + A + LOGBG, todo nuevo «preparado, apagado»; PREHASH apagado
- Consola (coordinador): M13 m6f 59,5·52,7·58,9·60,0·57,7; M15 m6g 59,5·52,0·57,9·60,0·57,9 (ms juego 10,4·14,2·
  12,8·10,5·5,7); M17 m6g sin PREHASH 59,5·51,8·58,1·60,0·58,0 (render +1 ms, 0 fps) → **PREHASH pasa a preparado/apagado**.
  Cola de consola PARADA a propósito (el usuario usa la consola): solo se encola, no se arranca nada.
- **Árbol `nWiiRuntime_m6h`**:
  1. Base: `r40d/fuentes_fase2_final/nWiiRuntime` (FASE 2 CONGELADA de D = lo compilado en m6e1; = mi m6f salvo el
     diagnóstico `[D40M]` de PrepFrame, que el parche de las 17:28 llevaba de más: m6f/m6g lo llevan, m6h no) +
     `main.cpp` de m6g (tabla de valores por defecto). Diferencias con m6g antes de los parches: solo ese bloque de gx.cpp.
  2. `parches/merge_vs_fase2.patch` (D, MERGE): aplica exacto (-p2).
  3. `parches/w2_runtime_vs_m6e.patch` (W): main.cpp y gx.cpp, solo desplazamientos (-p2). + la unidad generada
     `w_agua/w_agua_gen.cpp` (`tools/gen_w_agua.py` = copia del de W, del export de m4 = el mismo de k3) con
     `w_agua_cola.h` y `w_agua_hook.inc` (copias). `build_m6.sh` y `build_pgo_m6.sh` con `WAGUA=1` la compilan con la
     orden de los objetos del juego (sin perfil) y la meten en el enlace (y en `cmds.txt`, que lee `order_m6.sh`).
  4. `parches/a_vs_m6e.patch` (A; sha256 bd0d666a…e94f): s2.cpp, ax_hle.cpp, switch_profiler.cpp, exacto (-p1). Revisado
     el solape: A partió de m6e (ya con c5), así que su s2.cpp lleva el arreglo de C dentro; sus contadores usan
     `q[9]` (C lo amplió a `q[10]`) y `p[18..19]` (`p[20]`): sin desbordes. A añade una tercera línea `[A43]` al
     registro de S2: amplío el búfer `s2l` de 1400 a 2048 (main.cpp y switch_platform.cpp) para que no se corte.
     LOGBG está en switch_platform.cpp, A toca switch_profiler.cpp: sin solape.
  5. Tabla `m6_defaults()`: PREHASH → preparado; nuevos preparados/apagados `NWII_D_MERGE`, `NWII_W_AGUA`,
     `NWII_W_WGP`, `NWII_A_MIX`(1), `NWII_A_NOSPIN`(1), `NWII_A_REQSPIN`(200), `NWII_M_LOGBG` (grupos NWII_W_OFF,
     NWII_A_OFF, NWII_M_OFF). OJO para el futuro: `NWII_M_LOGBG` se lee en `setup_logging` (dentro de early_init),
     ANTES de `m6_defaults()`: encenderlo por defecto exigirá leerlo más tarde o ponerlo en early_init.
- 19:20 **NRO m6h** = runtime m6h (base `nx/m6hrt`, `WAGUA=1`; `bin/m6hrt_pgo.nro` 757c3b42…fbec2, fastmem 867 827 = m6f
  + 1 073 sitios del agua nativa, 0 mal formadas) + objetos k2 + pares + orden L (perfiles de M10):
  `bin/m6h.nro` sha256 f36d72511c0b69f3a196dfb37fb38eb49fd5d1eee40b18e9da99e4f8f44278b7, elf 5cf698a6…78b7;
  fastmem 871 240 entradas 0 mal formadas; eqcheck 101 902 funciones, 30 distintas, 0 faltan, las 19 «DIFERENCIA REAL»
  = las de k3e; `[WAGUA]` en el ELF. (ssh colgada otra vez al final; bajado a mano.) Parches: `m6h_vs_m5.patch`,
  `parches/m6h_vs_m6g.patch`.
- 19:22 Encoladas (cola PARADA, no se arranca): **M22_m6h_def**, **M23_m6h_merge** (+MERGE), **M24_m6h_agua**
  (+AGUA+WGP), **M25_m6h_todo** (+MERGE+AGUA+WGP), **M26_m6h_todo_logbg** (+LOGBG), **M27_m6h_todo_audio** (M25 +
  A_MIX=1, A_NOSPIN=1, A_REQSPIN=200), contiguas, 400 s.
- PC en marcha: `tools/pc_m6h.sh m6h` (P1 por defecto: FC + checks; P2 todo: FC con MERGE_CHECK + checks + estrés
  5×150; P3 audio: + AX_VERIFY, FC + checks + estrés 5×150; P4 AGUA_CHECK real/nulo 200 s + WGP_SELFTEST).
- 20:15 **PC m6h P1 (por defecto)**: FC 5/5 idénticas (280/260/280/300/260), D40CHECK/DEC/PREP/predicción 0, vigía 0,
  `[M6]` con D_MERGE/W/A/LOGBG/PREHASH «preparado»; checks 1x 220/220 ×4. **P2 «todo» (+MERGE+AGUA+WGP)**: FC 5/5
  idénticas (280/240/280/280/240), **D40MERGE_CHECK 0 píxeles** en todas (60-140 comparaciones por demo), D 0, `[WAGUA]
  NATIVO` enganchado (agua: 45 llamadas/s, 1 100 cuerpos/llamada), `[WGP] caminos cortos SI`; checks 1x 220/200/200/200
  idénticas (walk 63 % frente a c1 = cámara desplazada, mirado); estrés 5×150 s: 0 cuelgues, 0 rotas, 0 NullCall,
  vigía 0, AID 333/s, 59,2-59,6 fps.
- 21:00 **PC m6h P3 «audio»** (todo + `NWII_A_MIX=1 NWII_A_NOSPIN=1 NWII_A_REQSPIN=200` + `NWII_AX_VERIFY=1`; `[A43] … nospin 1
  reqspin 200`): FC 5/5 idénticas (400/360/340/340/380), MERGE_CHECK 0 píxeles, D 0, **AXV 21 000-30 000 listas por demo,
  0 distintas, 0 MISMATCH**; checks 1x 220/220/200/200 idénticas, AXV 24-33 k listas 0 distintas; estrés 5×150 s: 0
  cuelgues, 0 rotas, 0 NullCall, vigía 0, AID 333/s. Nota: en estos checks cmp_png contra c1 sube (map 5 %, lvl11 18 %,
  walk 78 %): FRAMECHECK idéntico y la imagen mirada (lvl11 t_006300) es la misma escena con Mario en el mismo sitio;
  el juego va con otra cadencia (AX_VERIFY duplica la mezcla: irq DSP 364 ms/s, 60 líneas [V] frente a 7-9) → fases de
  animación distintas en el mismo fotograma. Se separa con lvl11 sin AX_VERIFY (abajo).
- 21:30 **PC m6h P4 agua** (`test/agua_m6h.txt`, `NWII_W_AGUA_CHECK` con S2 apagado, agua_pinguino 200 s): real 2 287
  llamadas / 122 M escrituras al pipe: **pipe 0, registros 0**, memoria 15; control nulo (generada contra generada)
  2 450 llamadas: pipe 0, registros 0, memoria 21 → las diferencias de memoria son ruido del método (misma zona
  8037xxxx-80384590, también en el control; = conclusión de W). `NWII_W_WGP_SELFTEST`: 4 000 secuencias, 0 distintas.
  lvl11 1x sin AX_VERIFY: con A (MIX=1+NOSPIN+REQSPIN) 220/220 y cmp_png contra c1 3,2 %; «todo» sin A 2,3 % → el 18 %
  de antes era la cadencia distinta por AX_VERIFY, no el mezclador.
  **m6h verificado en PC** (P1-P4). Pendiente: A_MIX=2 (lo que llevan M27/M29; yo probé MIX=1) → `tools/pc_m6h_mix2.sh`.
- Consola (coordinador): M22 def 59,4·52,2·58,1·59,7·57,7 · M23 +MERGE agua 55,8 · M24 +AGUA+WGP juego agua 14,5→13,6 ·
  M25 todo 59,5·56,3·59,4·60,0·58,2 · M26 +LOGBG (vaciado en render 7 → 0,01 ms) · M27 +audio (MIX=2) 59,6·57,4·59,4·60,0·57,5.
- 21:15 **PC m6h con la configuración de M29** (MERGE+AGUA+WGP+`A_MIX=2`+NOSPIN+REQSPIN=200; `tools/pc_m6h_mix2.sh`):
  FC 5/5 idénticas (360/220/220/240/220), MERGE_CHECK 0 píxeles, **AXV 9-24 k listas por demo 0 distintas, 0 MISMATCH**;
  checks 1x 160/180/180/180 idénticas, AXV 15-18 k listas 0 distintas; estrés 5×150 s: 0 cuelgues/rotas/NullCall, vigía
  0, AID 333/s (56,8-57,5 fps en PC; con MIX=1 fueron 59,2-59,5: medido con la máquina compilando m6i a la vez, no es
  una medida de rendimiento). S2 «perdidos» 75-241 en los checks (AX_VERIFY + compilación a la vez; avisos de AX
  descartados, inocuos).

## m6i (20:30-): el binario definitivo de esta ronda
- Consola (coordinador): M23 +MERGE agua 55,8; M25 todo 59,5·56,3·59,4·60,0·58,2; M26 +LOGBG (vaciado en render 7 → 0,01 ms);
  M27 +audio MIX=2 59,6·57,4·59,4·60,0·57,5 (hilo de sonido 28 → 24 % en agua, mezcla 166 → 90 µs); M28 MIX=1 = M27.
- **Runtime `nWiiRuntime_m6i`** = m6h + (`parches/m6i_vs_m6h.patch`, `m6i_vs_m5.patch`):
  * ENCENDIDOS por defecto: `NWII_D_MERGE`, `NWII_W_AGUA`, `NWII_W_WGP`, `NWII_A_MIX=2`, `NWII_A_NOSPIN=1`,
    `NWII_A_REQSPIN=200`, `NWII_M_LOGBG`. Preparados/apagados: PREHASH, RENDERPREP, `NWII_AXFX_KERNEL=4`. POLL_EVERY sigue.
  * LOGBG: `logbg_start` (en early_init, antes de `m6_defaults()`) aplica la misma regla: explícito manda;
    `NWII_M_LOGBG_OFF` / `NWII_M_OFF` / `NWII_M6_OFF` lo apagan.
- **Gancho del agua w3 EXACTO**: `w_agua/` = copia de `nsmbw-build:r9/r44w/wagua_w3/` (lo que compiló `w3_pgo.nro`
  847083a3…): gen.cpp 3d0e59ab…, hook.inc 001bc348… (5 rutas, válvulas `NWII_W_AGUA_SOLO0` / `_TODOS`), cola.h (igual).
  El hook/generador actuales de W (20:00-20:16) ya traen `NWII_W_SIM` (olas, posterior a w3): NO usados. Marca
  `w_agua/FIJO`: los guiones ya no regeneran; los de m6h (w2) quedan en `w_agua_w2/`.
- **Reverb de A** (RECETA de r43a): `nx/k2a43` (otra laptop) = copia de `r42k/nx/k2` con output_24.o recompilado con la orden
  de K + `-Wno-error=coverage-mismatch` desde `gen_k2a43/output_24.cpp` (= `r43a/gen/output_24.cpp` sha256 17cf37b3…);
  avisos de perfil SOLO en nwii_r9_axfx_fast / nwii_a2_axfx_kcheck / nwii_a2_axfx_report. `order_m6.sh` con
  `KOBJDIR=…/r41m/nx/k2a43`. PC: `GEN24=1 tools/build_m6.sh` enlaza `r43a/gen/pc/output_24.o`.
- **NRO m6i** = base `nx/m6irt` (`bin/m6irt_pgo.nro` 810df034…b81d, fastmem 0) + objetos k2a43 + pares + orden L (perfiles
  de M10): `bin/m6i.nro` sha256 d016ba111c67be650450e77ac7395d50cdd1b47a31f866147264c31348e1ed4c, elf 389bdcee…97f3;
  fastmem 871 162 entradas 0 mal formadas; `[WAGUA]` y `nwii_a43_axfx_k4` en el ELF; eqcheck 101 873 funciones, 27
  distintas (todas saltos condicionales `b.cond` a partes frías, como k3e), 0 faltan.
- Encoladas y **RETENIDAS** (`.job.retenido`; por su prefijo iban detrás de M29, antes de verificar en PC): M30_m6i_def,
  M31_m6i_kernel4 (+`NWII_AXFX_KERNEL="4"`), M32_m6i_def_largo (1500 s).
- PC: `tools/pc_m6i.sh m6i` (por defecto + MERGE_CHECK + AX_VERIFY: FC, checks, estrés; kernel 4 con KCHECK; AGUA_CHECK
  en agua_pinguino, 8-C, 2-C, 5-4, 2-5).
- 21:50 **PC m6i** (`tools/pc_m6i.sh`; el portátil a load 30-40 por conversiones de vídeo del usuario: menos fotogramas y
  más «perdidos» de S2, sin efecto en las comprobaciones):
  * por defecto (`[M6]` con MERGE, AGUA, WGP, A_MIX=2, NOSPIN, REQSPIN=200, LOGBG): **FC 5/5 idénticas**
    (260/220/240/260/220), D40CHECK 0, MERGE_CHECK 0 píxeles, **AXV 12-15 k listas por demo 0 distintas**, `[WAGUA]`
    NATIVO; checks 1x 180/180/160/140 idénticas (S2 perdidos 139-507 por la carga); **estrés 5×150 s**: 0 cuelgues, 0
    rotas, 0 NullCall, vigía 0, AID 333/s.
  * reverb `NWII_AXFX_KERNEL=4 NWII_AXFX_KCHECK=1` (castillo y agua, 150 s): **kcheck 153 000 llamadas, 0 distintas**.
  * `NWII_W_AGUA_CHECK` (+S2_OFF) por ruta: agua_pinguino B530 1 630 llamadas, 8-C BA70 1 890, 2-C C5C0 2 199, 2-5 C020
    1 929: **pipe 0 y registros 0 en todas**; memoria 12-84, todas dentro de la zona de ruido 80378d20-80384590 (la
    escribe otro hilo; más con la máquina cargada; = control nulo de W). 5-4 (CA50, pantano): sin llamadas en 150 s
    con la carga (la ruta no llegó a dibujarse) → repetida a 300 s (abajo). W ya comprobó CA50 con este mismo gancho w3.
  M30-M32 LIBERADAS (quitado `.retenido`). La cola estaba parada (inactive) al liberarlas: no la arranco.
- 22:15 Ruta CA50 (pantano): `NWII_FORCE_LEVEL=5-4` no llega a dibujarla ni en 300 s (Mario quieto); con la demo
  `jungla` (= 5-4, como hizo W): **2 368 llamadas, pipe 0, registros 0**, memoria 37 en la zona de ruido. **Las 5 rutas del
  gancho w3 comprobadas en m6i. m6i verificado en PC.**
