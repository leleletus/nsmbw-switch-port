# r42k: frente K — código frío fuera del camino caliente (encima del PGO) y sondeo de interrupciones — 15-nov-2026

Meta: bajar el hilo del juego (núcleo 1) en agua de 15,3 a ≤ 13,8 ms/fot (1020/460 MHz) sin cambiar el comportamiento.
Base: **m6a** del frente M (`r41m/nx/m6a`, `r41m/bin/m6a_pgo.nro`: runtime m6a sin perfil + objetos PGO del juego de
m5, perfil `r17pgo/profiles/m4_all`). Sin git. No se tocan carpetas de otros frentes.

## Herramientas (`tools/`)
- `build_k.sh <tag>`: copia `r41m/nx/m6a` (reflink) en la otra laptop y recompila los 88 fuentes del código
  generado que llevan `-fprofile-use` (output_*.cpp + fastdispatch.cpp) con las MISMAS órdenes de ninja + `KFLAGS`
  (y `KRE`/`KRE_FLAGS` por fuente, `KONLY` para probar uno, `KLINK` para el enlace). main_output.cpp y el runtime no
  se tocan. → `bin/<tag>.nro/.elf/.sha256`, registro `logs/build_<tag>.log`.
- `order_k.sh <tag|m6a> <nuevo> <perfil>@<elf> …`: orden L (r9/tools/hot_order.py) encima de una compilación, solo
  re-enlace (comprueba con hot_order_eqcheck que el código es el mismo).
- `tam_text.sh <tag>…`: KB de código generado en .text.hot / .text / .text.unlikely, partes .cold y .rodata.
- `encolar_k.sh <nro> <nombre> [extra]`: trabajo de consola de 400 s con el entorno fijo del frente.
- `test/demos_fc.sh` (copia de r41m con arnés propio `pc/`), `test/pc_poll.sh`.

## Bitácora
- 15:40 inicio. Leídos r41m, r17pgo, r32dx §0/§4, r35m4 §5/§11, r31t §5-§9, r16l. Base m6a (runtime estable:
  M02_m6a_on 15:45 IMAGEN OK).
- 15:44 **El perfil sigue valiendo** (comprobado, no supuesto): recompilar output_3/output_86 en otra carpeta con la
  misma orden + `-Wmissing-profile` da objetos **idénticos byte a byte** a los de m6a y ningún aviso (el `.gcda` se
  encuentra: `-fprofile-prefix-path` = carpeta de compilación). Con `-fno-jump-tables -freorder-blocks-and-partition`
  (y también con `-fno-profile-partial-training`) compila sin `coverage mismatch`: los dos flags actúan después de
  la instrumentación. **No hay que re-entrenar.**
- 16:00 k1 (`-fno-jump-tables`): compila y enlaza (548 s a -j6). Las tablas de saltos del código generado son
  poquísimas: `.rodata` de los 88 objetos 51 → 23 KB, `.text` +26 KB (+0,06 %). Su coste en tiempo no se puede ver
  en la consola (está por debajo del ruido); no se mide aparte.
- 16:10 k2 (`-fno-jump-tables -freorder-blocks-and-partition`, perfil y partial-training como m6a): compila, pero
  **el enlace falla**: 27 saltos condicionales (`b.cond`/`cbz`, `R_AARCH64_CONDBR19`, alcance ±1 MiB) van de la
  parte caliente a la fría de la misma función y switch.ld pone todo `.text.unlikely*` al principio de 59 MiB de
  `.text`; GNU ld no pone veneros para esos saltos (para `B`/`BL` sí). Por eso GCC no lo enciende por defecto en
  AArch64. (Lo de r17pgo, «invalid operands», era el otro problema: las tablas de saltos; con -fno-jump-tables ya no
  sale.) **Arreglo sin tocar el compilador**: `tools/cruces.py` lista esos saltos en los objetos y
  `tools/orden_pares.py` hace un `--section-ordering-file` que pone juntas la parte caliente y la fría de esas 27
  funciones; el resto lo coloca switch.ld como siempre. → **k2p** enlaza. ld comprueba el alcance de cada salto al
  enlazar: si alguno no llegara, el enlace falla (no hay error silencioso).
- 16:15 k0o = m6a + orden L (perfiles cpu/main/gxw de M02_m6a_on con su propio .elf): solo re-enlace; eqcheck:
  99 514 funciones, código igual salvo campos de reubicación (las 122 «distintas» son clones locales con el mismo
  nombre en varios objetos). k3 = k2 + orden L + pares.
- 16:07 K01 (control) cortada a los ~50 s por una pulsación externa (HOME; sin informe de crash ni reinicio); el
  coordinador restauró debug.toml y la reencoló (16:11). La de 16:03 no vale para medir.
- 16:20 **Encolar k2p/k3 lo bloqueó el sistema de permisos** (no pasaron la verificación de PC, que para un cambio
  solo de flags no es aplicable). Decide el usuario. No se buscó rodeo. Ver §2.
- 16:25 k2n (= k2 + `-fno-profile-partial-training`) y su enlace con pares (k2np) y con orden L (k3n). Ver §1.

## 1. Variantes y tamaños (código generado: 88 objetos con perfil; main_output y runtime iguales que m6a)

| variante | flags del código generado / enlace | .text.hot | .text normal | .text.unlikely (de ello partes .cold) | total | NRO |
|---|---|---|---|---|---|---|
| m6a (control) | como m5 | 4 046 KB | 40 641 | 1 (0) | 44 688 KB | `r41m/bin/m6a_pgo.nro` |
| k1 | `-fno-jump-tables` | 4 048 | 40 665 | 1 (0) | 44 714 | `bin/k1.nro` |
| k2 / **k2p** | k1 + `-freorder-blocks-and-partition`; k2p = enlace con pares | **3 033** | 40 452 | 1 624 (**1 623 KB en 2 271 partes .cold**) | 45 109 | `bin/k2p.nro` |
| **k3** | k2 + orden L (M02) + pares | 3 033 | 40 452 | 1 624 | 45 109 | `bin/k3.nro` |
| k2n / k2np | k2 + `-fno-profile-partial-training` | 2 628 | 7 148 | 31 503 (5 163 KB en 8 612 partes .cold) | 41 279 | `bin/k2np.nro` |
| k3n | k2n + orden L + pares | igual | | | | `bin/k3n.nro` |
| **k0o** | m6a + orden L (solo re-enlace) | = m6a | | | | `bin/k0o.nro` |

- Con partial-training (como m6a), k2 saca **1 MB de bloques nunca ejecutados de las funciones calientes**
  (`.text.hot` 4 046 → 3 033 KB) a 2 271 partes frías. Lo que no se entrenó sigue optimizado normal (-O2): el
  comportamiento ante código no entrenado es el de m6a.
- Sin partial-training (k2n), además 31 MB de funciones no ejecutadas en el entrenamiento pasan enteras a
  `.text.unlikely` **optimizadas para tamaño** (−7 % de código). Riesgo medido con el perfil de juego real con guion
  (mapa + niveles, `bench_M02_m2gen_juego_1112_0041`, 920 s, `tools/frio_vs_perfil.py`, `logs/riesgo_k2n.txt`): el
  **1,1 %** de las muestras del hilo del juego (4,2 % del código generado) cae en funciones que k2n deja frías. En las
  5 demos (M02_m6a_on): 0 %. O sea: k2n solo empeoraría ~1 % del tiempo de juego real, y no más que eso.
- Orden L (`hot_order.py`, perfiles M02): huella del hilo del juego que cubre el 95 % / 99 % de las muestras y nivel 1
  (cpu): k0o 1 161 / 3 208 / 2 747 KB · k3 967 / 2 528 / 2 089 KB · k3n 931 / 2 339 / 1 916 KB. (Estimación con
  muestras de m6a traducidas por nombre: en k3/k3n las funciones son más pequeñas, la densidad es aproximada.)
- Tabla de fastmem: 0 mal formadas en todas (k2p 870 167 entradas, k2np 745 046: el código optimizado para tamaño
  duplica menos accesos; m6a 866 754).

## 2. Verificación de k2p/k3 (y k2np/k3n): qué se comprobó y qué no

Comprobado:
1. Mismo perfil: objetos idénticos byte a byte a m6a recompilando sin los flags; 0 avisos `coverage mismatch` /
   `missing-profile` / warnings en los 88 fuentes con los flags. `.text.hot` presente (el perfil se aplicó).
2. Enlace sin errores con los pares (ld comprueba el alcance de cada salto condicional; el primer intento sin pares
   falla con «relocation truncated», que es justo la comprobación funcionando).
3. `hot_order_eqcheck.py` contra un enlace base de los MISMOS objetos (k2 sin orden, `--noinhibit-exec`): todas las
   funciones iguales palabra a palabra salvo campos de reubicación; las que salen «distintas» (27 en k2p/k3, 236 en
   k2np/k3n) son exactamente las que tienen un salto condicional a su parte fría (en el enlace base ese salto está
   truncado), comprobado nombre a nombre. Esto demuestra que el ENLACE no cambia nada; no dice nada de lo que hizo
   el compilador dentro de cada función.
4. Tabla de fastmem bien formada (`check_fm_tab_g5.py`, 0 mal formadas).

NO comprobado: que el código que GCC genera con `-freorder-blocks-and-partition`/`-fno-jump-tables` haga lo mismo
que el de m6a. La verificación de PC del frente M (demos_fc, checks) no se puede aplicar: el binario de PC no cambia
(los flags solo tocan el código AArch64 de la Switch), así que pasaría siempre y no probaría nada.

Vías para una verificación funcional fuera de la consola (ninguna disponible hoy):
- **citron** (emulador de Switch): `r9/base/citron_scene.sh` existe pero el emulador se borró (ESTADO 7-nov, «no llega
  a cargar») y no hay internet para reinstalarlo: necesita que el usuario lo vuelva a poner. Con él: correr control y
  k3 con las demos forzadas y comparar capturas por fotograma (~1 h de puesta a punto + ~10 min por corrida; además
  habría que comprobar primero que citron reproduce las demos de forma determinista).
- qemu-aarch64 / compilador cruzado AArch64-Linux: no están instalados en ninguna de las dos laptops.
- En la propia consola, una comparación fuerte sería un hash de la RAM del invitado en fotogramas fijos de cada demo
  (cambio de runtime, ~1 h), pero eso sigue siendo correr k3 en la consola.
Riesgo real (opinión): la partición es una pasada estándar de GCC (por defecto en x86 con perfil); en AArch64 viene
apagada por los saltos lejanos, que aquí detecta el enlazador. El riesgo restante es un fallo del compilador; en
consola se vería como cuelgue o imagen rota (veredicto de imagen de bench_title).

## 3. Sondeo de interrupciones (sin recompilar: `NWII_POLL_EVERY`, frente T)

Qué es la rama «clock»: el código generado (sondeo compacto de G7) solo cuenta atrás `poll_left` en cada sondeo; al
llegar a 0 entra en el camino lento (`process_pending_callbacks_slow`, hle/ios.cpp), que reinicia la cuenta a
`NWII_POLL_EVERY` (por defecto `kPollTbEvery` = 16), lee el reloj del anfitrión (`poll_tb_refresh`: tick del sistema →
reloj del invitado + gobernador + vaciado por edad del write-gather), avanza la agenda de eventos y mira decrementador
/ interrupciones / callbacks. «clock» cuenta esos refrescos; «full»/«tail» los casos en que hay algo que hacer. En la
consola casi todos los sondeos lentos son «clock» (p. ej. agua 224k/s, de ellos 222k «clock», 372 full, 315 tail):
es puro gasto periódico, no trabajo. Todo lo que levanta una interrupción o encola un evento ya fuerza el sondeo
siguiente (`poll_kick`), así que espaciar el refresco solo cambia **cuándo** (hasta ~N sondeos, decenas de µs) se ve
un evento por tiempo (alarma, retrazo), nunca el orden: la agenda dispara por hora. Con S2 el DMA de audio va en la
agenda de la CPU de sonido, así que ya no le afecta (en T03, sin S2, bajaba el audio a 31,3-31,7 kHz).
`NWII_POLL_EVERY` NO estaba activo en ningún binario/debug.toml actual (no está en el debug.toml de la SD ni en el
entorno de la cola); es solo debug.toml, valor por defecto 16.

PC (`test/pc_poll.sh`, binario de PC de m6a con reloj real `NWII_REALTIME=1` —sin él el interruptor no hace nada en
PC— y el entorno de V+D de la prueba (b) de M): control `test/fc_kctl.txt` y 128 `test/fc_kp128.txt`: **5/5 demos
FRAMECHECK idénticos** (440/420/380/480/400), coalesce 0 discrepancias, D40CHECK 237 lotes 0 distintos; sondeos
lentos 229-265k/s → 26-30k/s.

`tools/sondeos.py <corrida>`: por demo, sondeos lentos k/s, por fotograma, kHz de audio y listas AX/s.

## 5. Cómo repetir (sin IA)
```
cd ~/nsmbw-port/r9/r42k
KFLAGS='-fno-jump-tables -freorder-blocks-and-partition' tools/build_k.sh k2      # ~9 min en la otra laptop (-j6); el enlace FALLA a propósito (saltos lejanos)
tools/order_k.sh k2 k2p                                                         # enlace con los pares -> bin/k2p.nro
R=~/nsmbw-port/switch/real_logs/bench_M02_m6a_on_1115_1545; E=~/nsmbw-port/r9/r41m/bin/m6a_pgo.elf
tools/order_k.sh k2 k3 $R/profile_cpu.txt@$E $R/profile_main.txt@$E $R/profile_gxw.txt@$E   # + orden L -> bin/k3.nro
tools/order_k.sh m6a k0o $R/profile_cpu.txt@$E $R/profile_main.txt@$E $R/profile_gxw.txt@$E  # solo orden L
tools/tam_text.sh m6a k2                                                         # tamaños
test/pc_poll.sh                                                                  # PC: sondeo 128 contra control
tools/encolar_k.sh ../r41m/bin/m6a_pgo.nro K0x_nombre 'NWII_POLL_EVERY="128"'    # consola
r9/tools_comunes/linea.sh K01_k_m6a_ctl_1115_1611 K02_k_m6a_poll128              # (desde ~/nsmbw-port)
python3 tools/sondeos.py ~/nsmbw-port/switch/real_logs/bench_K02_*
```
Para llevarlo a otra base (otro runtime): basta con cambiar `M=` (carpeta de compilación de referencia) en
`build_k.sh`; el código generado y el perfil tienen que ser los de m5 (`r17pgo/profiles/m4_all`).

## 4. Receta para el frente M (k3 sobre otro runtime, p. ej. m6e) — probada: `bin/k3e.nro`
Los objetos del código generado de k2 (`r42k/nx/k2`, los 88 con perfil) no dependen del runtime (el código generado
y el perfil son los de m5): se ponen encima de la carpeta de compilación del runtime que toque y se enlaza con pares
+ orden L. Una orden (otra laptop, ~5 min, solo enlace):
```
cd ~/nsmbw-port/r9/r42k
R=~/nsmbw-port/switch/real_logs/<corrida del propio runtime, p. ej. bench_M10_m6e_def_*>; E=~/nsmbw-port/r9/r41m/bin/m6e_pgo.elf
KOBJ=k2 tools/order_k.sh r41m:m6e k3e $R/profile_cpu.txt@$E $R/profile_main.txt@$E $R/profile_gxw.txt@$E
#  -> bin/k3e.nro/.elf/.sha256; sin perfiles = solo pares (equivalente a k2p)
```
k3e de prueba (17:45, perfiles de M02/m6a traducidos por nombre; 41 muestras de runtime sin traducir): sha256
6359c06f…f4f, fastmem 0 mal formadas, eqcheck: diferencias solo en las 27 funciones con salto a su parte fría.
Para k2n/k3n: `KOBJ=k2n`. Si cambia el código generado o el perfil, hay que rehacer k2:
`KFLAGS='-fno-jump-tables -freorder-blocks-and-partition' tools/build_k.sh k2` (cambiando `M=` en build_k.sh a la
compilación PGO nueva). En r17pgo/cmake/pgo.cmake equivaldría a `NWII_GEN_FLAGS="-fno-jump-tables
-freorder-blocks-and-partition"` + el paso de pares en el enlace (sin él, el enlace falla).
Nota: la orden ssh de order_k.sh se quedó colgada al final una vez (el remoto había terminado); se mató el ssh y se
bajaron los archivos a mano.

## 6. Consola (Switch v1, 1020/460 MHz, lista fija de 5 demos, 400 s; ms juego/fot REAL · desierto agua cielo nubes castillo)

| corrida | binario / interruptor | ms juego/fot REAL | fps dibujados | render ms/fot | imagen · audio |
|---|---|---|---|---|---|
| K01_k_m6a_ctl_1115_1611 | m6a (control) | 11,6 · 15,4 · 14,4 · 11,4 · 6,3 | 58,6 · 44,5 · 58,6 · 59,9 · 54,5 | 14,2 · 16,2 · 14,6 · 10,5 · 13,8 | OK · 31,2-31,7 kHz, 77 underruns |
| K02_k_m6a_poll128_1115_1620 | m6a + `NWII_POLL_EVERY="128"` | **11,2 · 14,8 · 13,5 · 10,9 · 6,1** | 58,5 · 42,1 · 58,1 · 59,9 · 54,0 | 14,3 · 16,2 · 14,6 · 10,3 · 13,8 | OK · 31,3-31,9 kHz, 67 |
| K04_k_m6a_ctl2_1115_1731 | m6a (control) | 11,6 · 15,3 · 14,5 · 11,5 · 6,4 | 58,5 · 43,4 · 58,2 · 59,9 · 53,6 | 14,5 · 16,5 · 15,4 · 10,6 · 14,0 | OK · 71 |
| K08_k_k3_1115_1740 | **k3** (encolada por el coordinador) | **11,2 · 14,7 · 13,8 · 10,9 · 6,3** | 58,8 · 42,9 · 58,0 · 59,8 · 54,9 | **13,8 · 15,7 · 14,3 · 9,9 · 13,5** | OK (capturas miradas) · 31,3-31,9 kHz, 62; VIGIA 1 (el del arranque, sale en todas), NullCall 0 |
| K11_k_k3_poll128_1115_1807 | k3 + `NWII_POLL_EVERY="128"` (encolada por el coordinador; aún base vieja, `profile = true`) | **10,9 · 14,3 · 13,0 · 10,6 · 6,1** | 58,6 · 40,2 · 58,1 · 59,9 · 53,1 | 14,3 · 16,3 · 15,0 · 10,2 · 14,1 | OK · 31,2-31,8 kHz, 70 |

- Sondeo 128: sondeos lentos por fotograma 2 400-6 200 → 270-800; ahorro 0,4-0,9 ms/fot ⇒ ~0,15 µs (~150 ciclos) por
  sondeo lento evitado. Audio igual (con S2 el DMA de audio no depende del sondeo del juego).
- k3: −0,4/−0,6/−0,7/−0,6/−0,1 ms en el juego y además −0,5 a −1,1 ms en el render (el orden L también junta el
  código del hilo principal y del trabajador).
- k3 + 128 frente a K04: −0,7/−1,0/−1,5/−0,9/−0,3 ms (las dos cosas suman). Agua 14,3: aún 0,5 por encima de 13,8.
- Los fps dibujados de agua no suben con ninguno (42-45 en todas: el agua sigue limitada por render/sincronía, ver
  frentes V/R), aunque el juego en agua pase de 53,8-55,0 a 55,7-57,5 fot/s; incluso bajan un poco (44,5/43,4 → 42,1/42,9/40,2):
  más fotogramas del juego por segundo = más trabajo para un render que ya va al límite (16,2-16,5 ms/fot). Sin
  repetir no se puede separar del ruido, pero las tres variantes rápidas van en la misma dirección.
- 18:40 El coordinador canceló K03 (k0o), K09 (k2p), K010 (k3n), K05/K06 (sondeo 128 rep / 64) y K07: tras el cambio
  de base (profile = false, r9/tools_comunes/AVISO_BASE_1115_1810.md) quedaban sin control válido. M integró la
  receta en **m6g** = runtime m6f + objetos k2 + pares + orden L + `NWII_POLL_EVERY=128` por defecto (M15/M16).
  Sin medir en consola: k0o (orden L solo), k2p (partición sola), k3n/k2np (sin partial-training), k1.

## 7. Conclusión
- **Integrar (hecho en m6g)**: k3 = código generado de m5 recompilado con `-fno-jump-tables
  -freorder-blocks-and-partition` (mismo perfil m4_all, sin re-entrenar) + enlace con pares (obligatorio) + orden L;
  y `NWII_POLL_EVERY=128` (sin recompilar; en debug.toml o por defecto en el runtime). Medido juntos (K11 frente a
  K04, base vieja): agua 15,3 → 14,3 ms/fot de juego; −0,7 a −1,5 ms en las cinco escenas; audio igual; imagen OK.
  La meta de ≤ 13,8 en agua NO se alcanza (faltan ~0,5 ms).
- Lo que queda (estimación, sin medir): k3n daría como mucho ~0,1-0,3 ms más (huella del hilo del juego al 95 %:
  967 → 931 KB, nivel 1 2 089 → 1 916 KB, frente a 1 161 → 967 KB que dio k3 por ~0,6 ms), a cambio de optimizar para
  tamaño el código no entrenado (~1 % del tiempo del juego real). Sondeo 64 ganaría MENOS que 128; 256 ahorraría
  como mucho ~0,06 ms en agua (quedan ~760 sondeos lentos/fot × ~0,15 µs). Ninguno de los dos llega a 0,3 ms con lo
  que hay medido.
- Ojo: los fps dibujados de agua no suben (bajan un poco con el juego más rápido): agua la limita el render.
