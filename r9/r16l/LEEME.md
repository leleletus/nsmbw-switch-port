# r16l — orden de enlace "lo caliente primero" (frente L)

8-nov-2026. Sin commits. Base: **r13b** (los `.o` y `.a` de `r9/r13/nx/build`, que **no se tocan**). No se recompila
nada: solo se vuelve a enlazar con `-Wl,--section-ordering-file=` (GNU ld 2.46) para que el código que de verdad
se ejecuta (según los perfiles de la consola) quede junto al principio de `.text`, en vez de repartido por 68 MiB.
La meta es gastar menos i-caché (48 KiB L1i por núcleo, 2 MiB de L2 compartida) y menos iTLB (páginas de 4 KiB).

## Binarios (`bin/`)

| Archivo | Orden hecho con | Nombre en el hbmenu |
|---|---|---|
| **`nsmbw_l1.nro`** + `.elf` (recomendado) | perfiles del título r13b (`bench_r13b_1108_1813`, `r13b_titulo_1108`) + la sesión larga r13 (`r13_1108`: mapa y niveles, peso 0,5) | NSMBW PRUEBA r16l1 (orden) |
| `nsmbw_l2.nro` + `.elf` | solo los perfiles del título r13b | NSMBW PRUEBA r16l2 (orden) |

Usan la misma partida, configuración y logs que r13b (`switch/NSMBWnx/`). Para simbolizar perfiles de estas NRO hay
que usar **su** `.elf` (las direcciones del código cambian; las de los datos no).

## Qué es igual y qué cambia (comprobado)

- `link.sh` usa el comando de enlace exacto de r13b (`ninja -t commands nsmbw.elf` en `r9/r13/nx/build`, rutas
  absolutas en `build/link_args.txt`). Sin archivo de orden sale **idéntico bit a bit** al `nsmbw.elf` de r13b, y con
  `elf2nro` + la nacp de r13b, idéntico a la NRO instalada (sha256 `061af5d3…`). La salida se llama siempre
  `nsmbw.elf` porque `--nx-module-name` mete el nombre del archivo dentro de `.text`.
- `hot_order_eval.py`: las 56 077 secciones de entrada de `.text` son las mismas (nombre, archivo, tamaño) y todas las
  demás secciones (`.rodata`, `.data`, `.bss`, …) tienen la misma dirección y tamaño. `.text` mide 0x60 bytes menos
  (relleno de alineación). `.rodata` es idéntica byte a byte. Lo que cambia en los datos son punteros a código
  (vtables y `kEntries` en `.data.rel.ro`, `.got`, `.init_array`, `.eh_frame`, `nwii_fm_tab`): es lo esperado.
  `nwii_fm_tab` (fastmem) guarda direcciones relativas y el runtime **la ordena al arrancar**, así que el orden no
  le afecta. Las tablas de dispatch se indexan por la dirección PPC, así que tampoco les afecta.
- `hot_order_eqcheck.py`: compara palabra a palabra las 56 684 funciones. **0 distintas**. Las únicas palabras que
  cambian son campos de reubicación (B/BL al mismo símbolo+desplazamiento, ADRP, imm12 de ADD/LDR/STR), unas 660 000.
  No hay veneers ni parches de erratas (el enlace no lleva `--fix-cortex-a53-*`).
- Cabecera NRO: mismos tamaños de segmentos (text 0x4402000, ro 0xdf8000, data 0x49e000, bss 0x9f000), misma
  primera instrucción y el mismo MOD0.

## Cómo se arma el orden (`r9/tools/hot_order.py`)

1. Lee el mapa (`-Wl,-Map`) de un enlace **sin orden** de la misma compilación: nombre, archivo, dirección y tamaño
   de cada sección de entrada de `.text`. Todo se compila con `-ffunction-sections`, también Mesa (`libEGL.a`,
   9 663 secciones), libnx, SDL, libstdc++ y el runtime. Solo `libc.a` trae algunas `.text` enteras (memcpy, …),
   que se ordenan como una pieza.
2. Lee los perfiles (`count pc lr`, con `pc - base` = dirección en el `.elf`). Si un perfil es de otro `.elf`
   (`perfil@otro.elf`), traduce por nombre de símbolo + desplazamiento (r13 → r13b: 0 fallos). Cada perfil se
   normaliza a fracciones y se le puede dar un peso (`perfil:0.5`).
3. Peso de cada sección = muestras. Aristas llamador→llamado con `lr` (vale para funciones hoja).
4. Disposición:
   - `KEEP(*(.crt0))` primero (la cabecera de la NRO tiene que ir en 0).
   - Secciones calientes de más de 1 MiB **antes** (solo `run_game`, 8,3 MiB: es un `switch` gigante que solo está
     caliente al principio, el bucle de dispatch, y al final; así su final queda pegado al bloque caliente).
   - **Nivel 1** = las secciones más densas (muestras por byte) que suman el 99 % de las muestras, partidas por hilo:
     juego (`cpu`, núcleo 1), compartidas y render (`main`, núcleo 0). Dentro de cada parte va el orden C3
     ("call-chain clustering", como HFSort): cada función se pega detrás de su llamador más frecuente, con un máximo
     de 1 MiB por grupo, y luego los grupos se ordenan por densidad.
   - **Worker de render** (núcleo 2, que el perfilador no ve): lo que se alcanza por llamadas directas desde
     `StartWorker`/`WorkerMain` dentro del runtime, libc y libnx (`--static-root`). Son 157 funciones y 120 de ellas no
     tienen muestras (80 KiB).
   - **Nivel 2** = el resto de lo que tiene alguna muestra, también partido por hilo.
   - Detrás, todo lo demás en el orden de siempre (lo pone el `switch.ld` normal).
5. Escribe `.text : { KEEP(*(.crt0)) *libnx.a:svc.o(.text.svcSleepThread) */output_24.o(.text.func_…) … }`. Cada
   sección va con su archivo para no arrastrar homónimos.

## Tamaños (l1; `order/l1_log.txt`)

- Secciones con alguna muestra: 4 812 (17,3 MiB, pero 8,3 MiB son `run_game`).
- Muestras acumuladas por densidad: el 50 % cabe en 1 KiB (las esperas `svc*`), el 80 % en 123 KiB, el 90 % en
  538 KiB, el 95 % en 1,2 MiB, el 99 % en 3,4 MiB y el 99,9 % en 7,2 MiB.
- Nivel 1: juego 2,96 MiB, compartido 7,5 KiB, render 462 KiB. Con el worker (81 KiB) son unos 3,5 MiB, más que la
  L2 (2 MiB). El 95 % sí cabe en la L2.
- El hilo del juego ocupa tanto porque las funciones generadas son grandes (hay de 40-70 KiB) y solo tienen calientes
  algunos bloques. Esto no se puede arreglar al enlazar. Habría que separar los bloques fríos al compilar
  (`-freorder-blocks-and-partition` con perfil, o AutoFDO).
- Alineación: las funciones generadas van a 16 B y `.text` a 64 B; se respetan tal cual.

Medida aproximada sin consola (`order/l1_eval.txt`), con el perfil del título de r13b: páginas de 4 KiB distintas
que tocan las muestras.

| | base r13b | l1 | l2 |
|---|---|---|---|
| hilo del juego, 95 % de las muestras | 1 391 páginas (en 68 MiB) | 949 (en 15,8 MiB) | 920 |
| hilo del juego, 100 % | 1 496 | 1 007 | 971 |
| render, 95 % | 220 | 101 | 101 |
| render, 100 % | 436 | 195 | 189 |

Las líneas de 64 B distintas casi no cambian. Era de esperar: el orden no cambia lo que se ejecuta dentro de cada
función, solo lo junta.

Datos (`.rodata`/`.data`): no se ordenaron. Los perfiles solo tienen el PC, no las direcciones de los datos. Las
tablas de dispatch (`kEntries` 2,7 MiB, `kBounds`, `g_func_bounds`, más la tabla `g_disp_table` en el heap) se
indexan por la dirección PPC, así que ordenar secciones no las mejora. En el perfil, la búsqueda de `run_game`
(`upper_bound` sobre `g_func_bounds`) pesa un 0,07 %.

## Medidas en la consola

Pendiente: las mide la sesión principal (demo del título, 150 s). Referencia: r13b media 48,0 fps, mínimo 34,4.

| NRO | media fps | mín fps | notas |
|---|---|---|---|
| r13b (base) | 48,0 | 34,4 | `switch/real_logs/bench_r13b_1108_1813` |
| l1 | — | — | |
| l2 | — | — | |

## Cómo regenerar / aplicarlo a otra compilación (G5, deko3d, …)

En esta carpeta, `./make.sh` hace la base (y comprueba que es idéntica), luego l1 y l2: orden, enlace, comprobaciones,
nacp, NRO y `bin/`. `./make.sh l1` hace solo una.

Para otra compilación `X` (otra carpeta `nx/build`):

1. Sacar su comando de enlace: `python3 r9/tools/hot_order_linkargs.py X/nx/build CARPETA/build/link_args.txt`
   (usa `ninja -t commands nsmbw.elf`, quita `-o` y pone rutas absolutas). Copiar `link.sh` a CARPETA. Enlazar una
   vez **sin orden** y con `-Wl,-Map=…`, con salida `…/nsmbw.elf`.
2. `python3 r9/tools/hot_order.py --map base.map --elf base.elf --out orden.txt --by-thread
   --static-root '_ZZL11StartWorkervENUlPvE_4_FUNES_' --static-root '_ZZL10WorkerMainv' perfiles…`.
   Se pueden usar los perfiles de r13b aunque la compilación sea otra: hay que poner `perfil@elf_con_el_que_se_tomó`
   y se traduce por nombre (las `func_*` se llaman igual entre compilaciones). Lo que la compilación nueva cambie
   (por ejemplo, el renderer de deko3d) solo tendrá muestras cuando haya perfiles suyos. Lo mejor es tomar un perfil
   con la NRO nueva y volver a generar el orden.
3. Enlazar con `-Wl,--section-ordering-file=orden.txt -Wl,-Map=nuevo.map`, salida `…/nsmbw.elf`.
4. Comprobar: `hot_order_eval.py --base-map … --base-elf … --new-map … --new-elf … perfil.txt` (mismo conjunto
   de secciones y datos en su sitio) y `hot_order_eqcheck.py base.elf nuevo.elf` (0 distintas).
5. `elf2nro nuevo.elf nuevo.nro --icon=… --nacp=…`.

Integrarlo en CMake sería añadir a `target_link_options` `-Wl,--section-ordering-file=${…}/orden.txt`. Si en el
archivo de orden falta una sección (función nueva o con otro nombre), ld la coloca en su sitio de siempre: no es un
error. El orden se puede generar una vez y reusar mientras los nombres no cambien.

## Archivos

- `link.sh`: re-enlaza r13b con flags extra. `build/link_args.txt` y `build/link_r13.cmd`: el comando original.
- `make.sh`: todo el proceso.
- `order/l1.txt`, `order/l2.txt`: los archivos de orden. `*_informe.txt` lista el peso de cada sección caliente,
  `*_log.txt` la salida de `hot_order.py`, `*_eval.txt` la verificación y la medida, y `*_eqcheck.txt` la
  comparación de código.
- `build/{base,l1,l2}/`: `.elf`, `.map`, `.nro` y nacp de cada enlace.
- Herramientas en `r9/tools/`: `hot_order.py`, `hot_order_eval.py`, `hot_order_eqcheck.py`, `hot_order_linkargs.py`.


## Resultado en la consola (8-nov 19:30, sesión principal)

L1, demo del título 150 s con ráfaga (`switch/real_logs/bench_r16l1_*`): cinemática del castillo 34,3-38,0 fps con render 74-79 % frente a r13b 33,7-36,3 fps / 77-80 % en las mismas ventanas (`bench_r13b_rafaga_1108_1844`). Mejora pequeña (~1 fps, ~3 % del render) y gratuita. L2 pendiente.
