# construir_nro: NSMBWnx.nro desde tu copia del juego, con un solo comando

`construir_nro.py` hace toda la cadena del port en tu máquina, partiendo de **tu propia copia** de New Super Mario
Bros. Wii (EE. UU., `SMNE01`, revisión 1):

1. valida el juego y, si es una imagen `.iso`/`.wbfs`, lo extrae;
2. recompila el código del juego (PowerPC → C++) con el recompilador NWiiRecomp y los cambios del port;
3. aplica las pasadas del port al código generado (las mismas que llevan los binarios publicados);
4. compila y enlaza el NRO con devkitPro (devkitA64 + libnx) y prepara la carpeta de la SD.

Esta herramienta **no trae nada del juego**: ni código generado, ni objetos, ni claves. Todo lo que sale del juego
se genera en tu carpeta de trabajo. Lo que trae son los guiones del port, listas de direcciones/funciones, parches
escritos a mano (guardados sin contexto, para que no lleven ni una línea de código generado), las sumas SHA-256
del código que tiene que salir y los perfiles de PGO (contadores de ejecución, ver más abajo).

```
python3 construir_nro.py --juego <carpeta|juego.iso|juego.wbfs> --trabajo <carpeta> [--jobs N] [--sin-pgo] [--docker]
```

Al terminar dice dónde están `NSMBWnx.nro` (con su SHA-256) y la carpeta de la SD:

```
<trabajo>/salida/NSMBWnx.nro            el port
<trabajo>/salida/NSMBWnx.elf            el mismo, con símbolos (para depurar)
<trabajo>/salida/sd/switch/NSMBWnx/     copia esta carpeta a la SD (NRO + data/ con los archivos del juego)
<trabajo>/registros/<etapa>.log         todo lo que hizo cada etapa
```

## Requisitos

| | Linux (nativo) | Windows | 
|---|---|---|
| Python | 3.7 o más nuevo | 3.7 o más nuevo (solo para lanzar; la compilación va dentro de Docker) |
| devkitPro | `devkitA64`, `libnx` y las portlibs `switch-sdl2`, `switch-mesa`, `switch-glm`, `deko3d` (`sudo dkp-pacman -S switch-dev switch-sdl2 switch-mesa switch-glm deko3d`), con `DEVKITPRO` exportado | `--docker` (imagen oficial `devkitpro/devkita64` + Docker Desktop con WSL2) |
| Otros | `git`, `cmake` ≥ 3.20, `ninja`, un compilador de C++ para el PC (`g++`), `xz` | Docker Desktop. Para leer `.iso`/`.wbfs`: `wit` o `DolphinTool` (o extrae el disco antes con Dolphin) |
| Disco | ~3 GB libres en `--trabajo` (medido abajo) + ~2 GB de la imagen de Docker si la usas | igual |
| RAM | 8 GB (con `--jobs 4`); `main_output.cpp` sola pide ~3,5 GB | igual (asigna RAM a WSL2) |

La primera vez descarga de GitHub, si no le das copias locales: el recompilador NWiiRecomp (en su commit
`595f176`), la base de firmas de Dolphin `totaldb.dsy` (comprobada por SHA-256), toml++ v3.4.0 y devkitPro/uam
(commit `5a5afc2`). Para no usar la red: `--recompilador <dir> --uam <dir> --tomlplusplus <dir>`.

### Linux

```
export DEVKITPRO=/opt/devkitpro
python3 tools/construir_nro/construir_nro.py --juego ~/juegos/NSMBW --trabajo ~/nsmbw_construir --jobs 8
```

### Windows (Docker Desktop + WSL2)

1. Instala Docker Desktop (con el motor WSL2) y Python 3 para Windows.
2. Desde la carpeta del repositorio:
   ```
   python tools\construir_nro\construir_nro.py --juego D:\juegos\NSMBW.wbfs --trabajo D:\nsmbw_construir --docker --jobs 6
   ```
   La etapa 1 (leer y validar el juego) corre en Windows con `preparar_sd.py` (para `.iso`/`.wbfs` necesita `wit` o
   `DolphinTool`); el resto corre dentro de la imagen `construir_nro_nsmbw:1`, que se construye sola a partir de
   `devkitpro/devkita64` con `docker/Dockerfile` (añade python3, git, cmake, ninja y g++).
   Con WSL2 y sin Docker también vale la vía de Linux dentro de la distribución.

¿Por qué no un AppImage o un ejecutable único? El compilador cruzado de devkitPro (GCC para AArch64 + newlib +
libnx + Mesa/SDL de las portlibs) pesa varios GB y tiene su propio instalador y repositorio de paquetes; meterlo en
un AppImage sería redistribuirlo entero, mantenerlo al día a mano y seguiría sin servir en Windows. La imagen
oficial de Docker de devkitPro ya es exactamente ese entorno, versionado y mantenido por ellos, y funciona igual en
Linux y en Windows (WSL2).

## Etapas

Cada etapa guarda en `<trabajo>/estado/<etapa>.json` una huella de sus entradas; si vuelves a lanzar la misma
orden, las etapas hechas cuyas entradas no cambiaron se saltan (la construcción se puede reanudar tras un corte).
`--hasta <etapa>` para en esa etapa; `--rehacer <etapa>` la repite (y las siguientes).

| etapa | qué hace | tiempo medido (*) |
|---|---|---|
| `juego` | valida con `tools/preparar_sd/preparar_sd.py` (ID, revisión, SHA-256 de `main.dol`, archivos según `fst.bin`); extrae `.iso`/`.wbfs`; descomprime los 4 REL (`files/rels/*.rel.LZ`, LZ77 de Nintendo) | < 1 s (carpeta) |
| `herramientas` | copia/clona NWiiRecomp, aplica `recompilador/cambios_locales_completos.patch` (o ve que ya está aplicado) y compila `nwiirecomp` y `jtdump` con el compilador del PC (solo recompilador + analizador + cargador: sin SDL ni interfaz) | ~20 s |
| `recompilar` | `main.dol` + 4 REL → `export/` (87 `output_N.cpp` + `main_output.cpp`), pasadas base `apply_fastdispatch.py` y `apply_r8_codegen.py --prune-only --memview`, tabla de saltos `jt.txt` | ~15 s |
| `pasadas` | las pasadas del port, en este orden: cpucode (`apply_r9_cpucode/axfx/psq/axfx_fast`), fastmem (`apply_r12_fastmem.py`), A2 (reverb, `output_24`), G6 entry/notrace/inline, G6c (`apply_g6c.py --proto 3`), G7 (`--feat stfs,psq,fp,poll,icm,disp,cr`), S2; después A43 (kernel 4 de la reverb) y la unidad del agua nativa (`gen_w_agua.py`, W). **Cada paso se compara con `referencia/*.sha256`** | ~2,5 min |
| `uamlib` | compila uamlib (compilador de shaders de deko3d: devkitPro/uam + `material/uam_r15dk.patch`) y la deja como `libuamlib_ns.a` (todo local salvo `uamlib::Compile`) | ~30 s con `-j8` |
| `vista` | árbol de compilación: `export/` + runtime (`runtime/nWiiRuntime`, el de m6s = m6q + inclinación corregida + petición de CPU a sys-clk, quitada al perder el foco) + GLAD; cada fuente y cabecera empieza por `#line 1 "<ruta original>"` (ver PGO) | < 5 s |
| `compilar` | CMake + ninja con devkitA64: `main_output.cpp` sola y luego todo con `-jN`; el agua nativa; enlace (con PGO: pares caliente/frío + orden L de m6q); `nacptool` + `elf2nro`; comprueba la tabla fastmem (0 mal formadas) y que esté `[WAGUA]` | ~11,5 min con PGO, ~7,5 sin PGO (`-j8`) |
| `sd` | `preparar_sd.py` copia los datos del juego y el NRO a `salida/sd/switch/NSMBWnx/` | ~10 s |

(*) En un portátil de 12 hilos y 32 GB (Linux, devkitA64 GCC 16.1.0, `--jobs 8`), 16-nov-2026, desde una carpeta
de trabajo vacía y con el juego ya extraído: **14,7 min en total** (881 s). RAM: ~7 GB por encima de lo que ya usaba
el sistema (pico de `main_output.cpp` + 7 compilaciones a la vez); con `--jobs 4` basta con 8 GB.
Espacio en `--trabajo`: **2,9 GB de pico, 2,1 GB al terminar** (código generado 0,6 GB, vista 0,3 GB, objetos 0,4 GB,
uamlib 0,25 GB, salida 0,6 GB de la que 0,34 GB es la copia de los datos del juego para la SD). Docker: + ~2 GB de imagen.

## PGO: qué es y por qué se puede usar aquí

La PGO (optimización guiada por perfil) compila el código del juego sabiendo qué ramas se ejecutan de verdad en la
consola. Los perfiles son archivos `.gcda`: **contadores** de ejecución por función y por salto, no código. Vienen
en `perfiles/m4_all_codigo_generado.tar.xz` (88 archivos, 9,2 MB sin comprimir, **1,1 MB** comprimidos) y son los
que usa el binario publicado m6q.

Pero cada contador está atado bit a bit al código generado: GCC lo asocia a cada función por una suma de su grafo de
control y de su nombre/ruta de archivo, así que solo sirven si los fuentes son exactamente los mismos. Por eso:

- **La herramienta comprueba cada paso** contra las sumas SHA-256 de `referencia/` (sacadas de los árboles con los que
  se hicieron m4/m5/m6q). Probado el 16-nov-2026 desde cero en una máquina nueva (`game/` de la copia del usuario):
  la recompilación y las 9 pasadas dan **los mismos 96 archivos byte a byte** que `r9/r17pgo/m4src/export`, y
  `output_24` (A43) y `w_agua_gen.cpp` (W) los mismos que m6q.
- GCC mete la ruta del archivo en la suma de cada función; la etapa `vista` pone `#line 1 "/home/…/r17pgo/m4src/…"`
  al principio de cada fuente y cabecera (el código compilado no cambia; es solo el nombre que «ve» GCC) y la etapa
  `compilar` renombra los `.gcda` a la ruta de los objetos de tu compilación. Comprobado: el objeto de `output_10`
  compilado así desde otra carpeta es idéntico (desensamblado completo) al de m6q.
- Los `.gcda` llevan la versión de GCC: **la PGO solo se usa con `aarch64-none-elf-gcc` 16.1.0** (el de devkitA64
  con que se tomaron). Con otra versión, o si algo no casa con la referencia, la herramienta compila **sin PGO**
  y lo dice (`PGO: no (motivo)`). Fuérzalo con `--sin-pgo`.
- Sin PGO se pierde rendimiento en el hilo del juego: la PGO valía **~3,5-4 ms por fotograma** (Switch v1 a 1020 MHz,
  medidas de r17pgo/r33g7); sin ella el juego se queda por debajo de 60 fps en las escenas cargadas (agua, castillo).
  También se omite el orden de enlace L y los flags del frente K (`-fno-jump-tables -freorder-blocks-and-partition`),
  que solo tienen sentido con perfil (`--orden` pone el orden L también sin PGO; no medido).
- La herramienta no usa `symbols/` ni `NSMBW-Decomp/` (terceros): los nombres de funciones del SDK salen de la base de
  firmas de Dolphin (`totaldb.dsy`), que el recompilador lee en tu máquina.
- Para perfiles nuevos (otro compilador u otro código): `r9/r17pgo/pgo_release.sh prep|finish` del proyecto
  (NRO instrumentada → entrenar 900 s en la consola → juntar `.gcda`), y sustituir el `.tar.xz`.

## Qué está probado y qué no

Probado el 16-nov-2026, de principio a fin, en Linux (CachyOS, devkitA64 GCC 16.1.0, cmake 4.4, ninja 1.13, Python
3.14), partiendo de la carpeta extraída del disco y de una carpeta de trabajo vacía, con copias locales del
recompilador, uam y toml++ (`--recompilador`, `--uam`, `--tomlplusplus`):

| qué | resultado |
|---|---|
| validación del juego y REL descomprimidos | bien (los REL dan el mismo código que los de `bin_usa/`) |
| recompilación + 9 pasadas + A43 + agua | **los 96 fuentes, byte a byte, iguales a los de m5/m6q** (`r17pgo/m4src/export`), comprobado contra `referencia/` en cada paso |
| NRO por defecto (PGO + K + pares + orden L, con la comprobación del `main.dol`) | sale; tabla fastmem **871 162 entradas, 0 mal formadas** (= m6q); `[WAGUA]` presente; de las 101 893 funciones de m6q, **101 884 iguales**; distintas solo `early_init`, `LogSink::write` (y la cabecera `__nx_mod0`), más 3 de `std::string`: lo que añade la comprobación del `main.dol` (ver abajo) |
| NRO con `--sin-comprobar-dol` | **`.text`, `.data` y tabla fastmem idénticos byte a byte a m6q** (`r9/r45p/bin/m6q.elf`); `.rodata` difiere en 15 bytes: la fecha y hora de compilación (`__DATE__`/`__TIME__`) |
| repetición desde cero | el mismo código (solo cambia la fecha de compilación) |
| `--sin-pgo` | sale (compilación 7,4 min con `-j8`); fastmem 723 842 entradas, 0 mal formadas; `.text` 57,7 MB (con PGO 62,6 MB). **Rendimiento sin medir en consola** |
| reanudar | probado a mano en varias tandas (etapas hechas se saltan; ninja y los objetos del código generado solo se rehacen si cambian) |

**m6r** (16-nov-2026, runtime de m6q + dos cambios solo de Switch, parche en `r9/r47final/m6r_vs_m6q.patch`):
inclinación por movimiento corregida de fábrica (`tilt_invert = false` da ahora el sentido que antes daba `true`) y
petición de CPU a sys-clk (`src/platform/switch/sysclk_client.cpp`, `[switch] cpu_mhz = 1224`, `0` = nada;
`NWII_NX_SYSCLK_OFF=1` lo apaga). Construido con esta herramienta desde una carpeta vacía (PGO + orden L, 12,8 min):
los 12 pasos del código generado `= referencia`; fastmem 871 162 entradas, 0 mal formadas; frente al NRO por defecto
de m6q, de 101 895 funciones **101 887 iguales** (solo cambian campos de reubicación) y distintas solo las del runtime
tocado (`early_init`, `shutdown`, `apply_env`, `poll_pad` con la inclinación, `__nx_mod0`) más las nuevas
`sysclk_start`/`sysclk_stop`. NRO sha256 `cdf3085581ce7f09d2e50394566e6f629cb1d83687a2f9641a9aa447c8677279`.
Probado en consola (Horizon OC, `hoc:clk` API 2): pide 1224 MHz; pero HOME → cerrar programa mata el proceso sin
`shutdown()`/`atexit` y el override se quedaba puesto. **m6s** lo arregla: con un gancho de applet quita el override al
perder el foco (HOME, dormir) y lo vuelve a pedir al recuperarlo (`[NX] sys-clk: quitado (foco perdido)` /
`pedido (foco recuperado)`); parche `r9/r47final/m6s_vs_m6q.patch`. En consola el gancho de foco no llegó a ejecutarse antes del cierre, así que
**m6t** además bloquea la salida (`appletLockExit`) mientras hay sesión con sys-clk: HOME → cerrar llega como
ExitRequested, el juego quita el override (`quitado (salida pedida)`), vacía el registro y sale por `shutdown()`
(`appletUnlockExit`); un hilo de guarda desbloquea la salida a los 3 s y termina el proceso a los 6 s pase lo que pase.
Parche `r9/r47final/m6t_vs_m6q.patch`; m6t sin probar aún en consola.

Lo que **no** está probado:
- **Ningún NRO de esta herramienta se ha ejecutado aún en la consola.** El de `--sin-comprobar-dol` tiene el mismo
  código que m6q (medido y verificado en consola), así que se espera que se comporte igual; el de por defecto añade
  la comprobación del `main.dol` al arrancar.
- **`--docker` no está probado** (no había Docker en las máquinas de prueba ni red para bajar la imagen). Tampoco
  Windows. La imagen `devkitpro/devkita64:latest` puede traer otro GCC que 16.1.0: entonces la herramienta compila
  sin PGO (lo avisa).
- **La descarga de dependencias** (clonar NWiiRecomp, uam y toml++, bajar `totaldb.dsy`) no está probada: las
  pruebas usaron copias locales. En particular, que el parche `recompilador/cambios_locales_completos.patch` se
  aplique limpio sobre el commit `595f176` recién clonado.
- La lectura de `.iso`/`.wbfs` (usa la de `preparar_sd.py`, probada aparte con su `--autotest`).
- `--orden` sin PGO, `--permitir-distinto`.

Sobre la comprobación del `main.dol`: el runtime trae una comprobación (el NRO se niega a arrancar, con un mensaje,
si el `main.dol` de la SD no es el de SMNE01 rev 1) que se activa al compilar con la ruta del `main.dol`. **m6q no la
lleva**: en su compilación la definición no llegó al archivo (`switch_platform.cpp` se compiló sin
`NWII_NX_EXPECTED_DOL_SHA256`). Esta herramienta la pone por defecto; `--sin-comprobar-dol` la quita y da el mismo
código que m6q.

Otra rareza de m6q que la herramienta reproduce a propósito: `output_24` (kernel 4 de la reverb, A43) se compiló con
PGO pero **sin** los flags del frente K (`-fno-jump-tables -freorder-blocks-and-partition`), porque la receta de A43
copió la orden de `k_cmds.txt`, que no los lleva. Se deja igual para que el binario sea el medido.

## Problemas frecuentes

- **«no encuentro devkitA64»**: instala devkitPro y exporta `DEVKITPRO=/opt/devkitpro`, o usa `--docker`.
- **«El main.dol no es el de SMNE01 revisión 1»**: el port solo funciona con el disco de EE. UU. revisión 1 (el
  código se generó de ese `main.dol` exacto). Las revisiones 0/2 y las versiones europea/japonesa no valen.
- **«la recompilación no reproduce el código de referencia» / «la pasada X no da los fuentes de referencia»**: algo
  de la cadena no es lo esperado (otra versión del recompilador o del parche, un REL dañado). Mira
  `registros/recompilar.log` o `registros/pasadas.log`. `--permitir-distinto` sigue igualmente, sin PGO y sin garantías.
- **Se queda sin memoria compilando**: baja `--jobs` (cada compilación del código generado pide ~0,9 GB; con PGO algo
  más). `main_output.cpp` va sola y pide ~3,5 GB.
- **Sin red**: pasa `--recompilador`, `--uam` y `--tomlplusplus` con copias locales (la base de firmas
  `totaldb.dsy` se toma de `<recompilador>/third_party/dolphin-sigdb/` si está).
- **Reanudar**: vuelve a lanzar la misma orden; `--rehacer compilar` recompila solo el NRO.
- **Windows sin Docker**: no está soportado (la compilación necesita las herramientas de Linux de devkitPro); usa
  `--docker` o la vía de Linux dentro de WSL2.

## Archivos

| ruta | qué es |
|---|---|
| `construir_nro.py` | la herramienta (solo biblioteca estándar de Python) |
| `pasadas/` | guiones del port sobre el código generado (copias de `perf/`, `r9/cpucode`, `r9/r12fm`, `r9/r20g6`, `r9/r30g6c3`, `r9/r33g7`, `r9/r35m4`, `r9/r44w`), `aplicar_u0.py` (aplica parches sin contexto), `jtdump.cpp`, `g7.h` (la de la variante d, con el arreglo de `ctr==0`), `cruces.py`/`orden_pares.py` (orden de enlace), `check_fm_tab_g5.py` |
| `material/` | listas de direcciones/funciones (`prof_mmio_list.tsv`, `inline_*.txt`, `g5_exclude_g6b.txt`), parches escritos a mano sin contexto (`a2_`/`a43_output_24.U0.patch`), el gancho del agua (`w_agua_hook.inc`, `w_agua_cola.h`), el orden L de m6q, la configuración del recompilador y el parche de uam |
| `referencia/` | SHA-256 de lo que tiene que salir en cada paso (`00_recomp` … `09_s2`) |
| `perfiles/` | perfiles de PGO del código generado (m4_all) |
| `compilacion/` | proyecto CMake del NRO (copia del de `switch/` y `r9/r17pgo/cmake`), uamlib |
| `terceros/` | archivos generados de terceros que no hace falta regenerar: GLAD (cargador de GL 3.3 core, glad v0.1.36) y `uam_gen` (bison/flex/mako de Mesa para uam) |
| `docker/Dockerfile` | imagen para `--docker` |

Licencias: el recompilador NWiiRecomp tiene licencia propia de uso no comercial (ver `recompilador/UPSTREAM.md` y
`LICENSE_NWiiRecomp` del repositorio); uam/Mesa y GLAD tienen las suyas (MIT y similares).
