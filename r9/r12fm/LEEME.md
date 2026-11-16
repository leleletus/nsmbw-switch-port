# R12fm — fastmem (PC y Switch) sobre r10c

8-nov-2026. Implementación del diseño de `r9/r11c/DISENO_FASTMEM.md`. Base: `r9/r10c` (lo que está en la
consola). No se tocó r10c/r11*/recomp/NWiiRecomp/switch/NAND/consola. Sin commits.

## Resumen (13:35)

- **fmtest.nro listo** (`tools/fmtest/fmtest.nro`): para correr en la consola **antes** de probar el NRO del juego.
- **PC: hecho y verificado.** `bin/NSMBW_r12fm(_input_test)`: compilación completa (FULL=1, JOBS=2).
  Tabla de excepciones: **682 614 entradas, 0 mal formadas** (`check_fm_tab.py`: cada entrada apunta a un
  `mov` de la forma fija con el ancho y la dirección de su tipo). `.text` 146,7 → **95,3 MB (−35 %)**.
- **Switch: compila, SIN PROBAR.** `bin/nsmbw_r12fm.nro` (+ `_input_test.nro`): 681 661 entradas, 0 mal
  formadas; `.text` 126,0 → **85,9 MB (−32 %)**, NRO 130,9 → 90,8 MB. **No mandar a la consola hasta que fmtest
  salga bien** (si las excepciones no llegan al NRO, el juego se cuelga en el primer acceso MMIO no listado; con
  la lista de MMIO casi no hay, pero los hay en arranques/menús no perfilados).
- **Fallos en el juego (PC): 0.** Cuatro corridas largas con guion (Coin Battle 4J 195 s, menús→mapa→1-1 195 s,
  título/demos 145 s, mapa andando 95 s) y tres deterministas: **0 fallos, 0 sitios, 0 no manejados**, sin
  cuelgues (`hang_summary`: OK), las mismas escenas que r10c, 59,4–59,7 fps. Es decir: con la lista estática +
  perfil de r11c todo lo que no es RAM va por llamada explícita (`_io`, 5 657 sitios) y nada cae en el manejador.
  El manejador está probado aparte (`test/fm_unit.cpp`: MMIO 8/16/32, tubería GX con 8/16/32/f32/f64 en orden,
  sin mapear, cruce del final de MEM1, espejos; **todo bien**; coste en PC ≈ 1,3 µs por fallo).
- **A/B en PC (ABBA, r10c vs r12fm, perfil débil `e2`: 2 E-cores a 3,6 GHz, 150 %; carpeta privada):**
  1-1: **hilo de la CPU 30,1 % → 23,3 % (−23 %)** con el mismo trabajo (60 fps, ~133 M instr. del juego/s).
  Mapa del mundo 1: **30,1 % → 26,0 % (−13 %)**, también a 60 fps (149–152 M instr./s). Detalle en
  `runs/ab_e2.txt`. Con 60 fps en los dos, la ganancia sale como tiempo libre del hilo, no como fps.
  Repetición en 1-1 con un perfil más duro (`e1`: las mismas 2 E-cores al 70 %): **27,8 % → 22,9 % (−18 %)**,
  aún a 60 fps (`runs/ab_e1.txt`). No se pudo usar el perfil `sw` (CPUs 16–17): ver Notas.
- **Diferencial:** `det.sh` (reloj virtual) da las **mismas imágenes que r10c hasta la copia 350** en título,
  mapa y 1-1 (1 750 copias XFB), y diverge en la 350 **igual que r10c contra sí mismo** (también con el
  mezclador AX en línea, `NWII_AX_THREAD=0`): ese arnés no es determinista más allá, así que no puede decir más
  (ya lo vio r11a). El diferencial por función del diseño (§7.2) queda pendiente.


## Qué hay

| Qué | Dónde |
|---|---|
| prueba aislada para la Switch (coste de un fallo, valores, espejos, 3 hilos, registros) | `tools/fmtest/fmtest.nro` (fuente `tools/fmtest/source/main.cpp`) |
| reescritura de la cabecera (`MemBlock`, `MemView` con plantillas, psq) | `tools/fm_header.py` (sha256 de la de r10c comprobado, marca `[r12fm-header]`) |
| pasada sobre el código generado | `tools/apply_r12_fastmem.py` → `sites.tsv` (id de sitio → pc PowerPC, tipo, función) |
| runtime: reserva, vistas, manejador PC (SIGSEGV) y Switch (entrada de excepción propia) | `nWiiRuntime/src/core/fastmem_impl.inc` (incluido al final de `src/hle/memory.cpp`, así no cambia ningún CMakeLists) |
| prueba unitaria PC (hardware simulado, 0 fallos) | `test/fm_unit.cpp`, `test/build_unit.sh` |
| comprobación de la tabla de excepciones de un binario (x86-64 y AArch64) | `tools/check_fm_tab.py <elf> sites.tsv` |
| verificación PC (humo, diferencial determinista, A/B `sw`) | `tools/verify_pc.sh smoke|det|ab` → `runs/*.txt` |
| informe de fallos por sitio (une el volcado `[FASTMEM]` con `sites.tsv`) | `tools/fault_report.py sites.tsv <run.log>` |
| compilar una unidad suelta | `tools/tu_pc.sh`, `tools/tu_nx.sh` |
| binarios | `bin/NSMBW_r12fm`, `bin/NSMBW_r12fm_input_test`, `bin/nsmbw_r12fm.nro` (+ `_input_test.nro`) |

## Cómo funciona

- **Memoria.** `MMU::mem1/mem2/lc` ya no son `std::vector` sino `MemBlock` (puntero + tamaño, con `data()`,
  `size()`, `[]`): mismos tamaños (+8 de relleno), así que `get_ptr`, DSP, AX, GX, IPC... no cambian.
  `fm_alloc()` (desde `MMU()`) crea los bloques canónicos y los vuelve a mapear en una reserva de 4 GiB + 64 KiB
  (`g_fm_base`): MEM1 en 0x00/0x40/0x80/0xC0000000, MEM2 en 0x10/0x50/0x90/0xD0000000, LC en 0xE0000000.
  PC: un `memfd` por región. Switch: receta de melonDS (un bloque de 98 MiB → `svcMapProcessCodeMemory` + RW,
  `virtmemFindAslr` + `virtmemAddReservation`, 9 `svcMapProcessMemory`).
- **Código generado.** La pasada numera cada acceso: `mv_.read32(ea)` → `mv_.read32<ID>(ea)` (669 412 sitios) y
  `psq_l_fast(ctx, mv_, …)` → `psq_l_fast<ID>(…)`. Los sitios cuyo pc está en `r9/r11c/prof_mmio_list.tsv`
  (clasificador estático + perfil) pasan a `mv_.write32_io(ea, v)` = llamada directa a `MMU::*_slow`
  (5 657 sitios). Con `NWII_FASTMEM` cada acceso es **un `asm volatile`** de forma fija
  (`movl (%b,%i)` / `ldr w, [x, w, uxtw]`, más el `bswap`/`rev` fuera) con una entrada en la sección
  `nwii_fm_tab` = `{pc − &entrada, tipo | id << 8}`, y un operando `"m"` falso para que GCC sepa qué bytes toca
  (no hay `"memory"`: puede seguir teniendo `ctx` en registros).
- **Fallo.** Todo lo que no es RAM no está mapeado → fallo. PC: `SIGSEGV` → busca el pc en la tabla ordenada,
  decodifica la instrucción (decodificador del prototipo de r11c), hace el acceso con el `*_slow` del **tipo del
  sitio** (así `write_f32` sigue yendo a `GX_WGPIPE_WriteF32`), escribe el registro destino, salta la instrucción.
  Switch: `__libnx_exception_entry` propia (la de `r9/r11c/proto/fastmem_nx.cpp`, la misma que prueba fmtest) con
  `a64_decode.h` y `svcReturnFromException(0)`. Lo que no es de la tabla → fallo de verdad (en PC se encadena al
  manejador anterior / muere como siempre; en Switch `svcReturnFromException(1)` → informe de Atmosphère).
- **Contadores.** Fallos por sitio; volcado `[FASTMEM] dump …` + `[FASTMEM] site <id> kind <k> faults <n>
  last_ea <ea>` al salir, con SIGTERM/SIGINT (el `timeout` de las corridas) y cada `NWII_FASTMEM_DUMP_S` s
  (30 por defecto; 0 = nunca) en PC; en Switch solo al salir.
- **Interruptor.** `NWII_FASTMEM` es de **compilación** (cabecera: 1 por defecto en Linux x86-64; en Switch lo
  pone `cmake/nsmbw-switch.cmake` y `build_nx.sh`). Con `-DNWII_FASTMEM=0` el mismo `export/` compila con el
  MemView de r10c (las plantillas reenvían a él). **No hay `NWII_FASTMEM=0` en tiempo de ejecución**: con el
  código sin comprobaciones no se puede volver atrás sin un segundo sabor del código generado (para depurar con
  gdb, usar el binario de r10c o compilar con `-DNWII_FASTMEM=0`).

## Diferencias de comportamiento conocidas (todas previstas en el diseño)

- Página 0: 0x0000000C ya no es especial (antes `read32` daba 0 y `write32` se descartaba). En los perfiles de
  r11c nunca se tocó. Pendiente: comprobar en cada VI que 0xC–0xF siguen a 0.
- `NWII_WATCH` no ve los guardados del código generado que van por fastmem (sí los del runtime y los `_io`).
- El manejador ejecuta `MMU::*_slow` dentro del fallo: si un `HW_Reg_Write*` escribiera en `ctx` o en algo que el
  código generado tenga en registros, no se vería (riesgo 1 del diseño). Revisado por encima: los registros que
  avisan a la CPU usan `poll_kick_global()` (un atómico, como los otros hilos); no vi escrituras a `ctx`.

## fmtest (Switch, para el usuario)

`tools/fmtest/fmtest.nro` → `sdmc:/switch/fmtest/`. A = pruebas, + = salir. Escribe
`sdmc:/switch/fmtest/resultado_N.txt` línea a línea (con `fsync`: si la consola se cuelga, la última línea
dice el paso). Hace: (0) entorno (depurador conectado, `exosphere.ini`/`disable_user_exception_handlers`,
región ASLR) y la reserva + 9 vistas como el juego (con el tiempo de los 9 `svcMapProcessMemory`);
(1) un primer fallo controlado (si se cuelga ahí: las excepciones no llegan al NRO); (b) cargas 8/16/32/64 y
guardados 8/16/32/64 que fallan: valor emulado / valor que recibe el manejador; (c) las 4 vistas de MEM1, las
4 de MEM2 y la LC son la misma memoria, bordes mapeados no fallan y lo de fuera sí; (f) x9–x17, d16, d31
intactos a través de un fallo; (a) coste: acceso mapeado, llamada explícita, 3 × 10 000 fallos de carga y de
guardado (ns y ciclos con el reloj de `clkrst` CpuBus); (e) 3 hilos fallando a la vez (núcleos 0–2; el área
global de la entrada se apoya en el candado del núcleo); (g) cruce del final de MEM1. Al salir deshace los
mapeos (si no, hbloader/hbmenu quedarían con el heap en estado *AliasCode*). Lo que hay que mirar: `RESULTADO
pasada 1: TODO BIEN` y las líneas de coste. Umbral del diseño: si un fallo cuesta >10 µs, todo sitio con
>10 fallos/s va a la lista (ya lo están).

## Cómo retomar

1. **Resultado de fmtest** (coste de un fallo en la Switch; si las excepciones llegan). Si falla el paso (1),
   no usar el NRO fastmem.
2. Si fmtest va bien: probar `bin/nsmbw_r12fm.nro` en la consola (sin OC primero) y hacer el A/B contra r10c.
   El NRO vuelca `[FASTMEM] dump` solo al salir (stdout/nxlink).
3. Pendiente (por orden): diferencial por función (§7.2 del diseño: dos sabores de cada unidad en el mismo
   binario, grabar los accesos lentos y comparar RAM/registros) o la repetición determinista (§7.3, necesita
   arreglar el no-determinismo de `det.sh` desde la copia 350); comprobar 0xC–0xF en cada VI; `NWII_WATCH` con
   `mprotect`; leer de la SD al heap y copiar (no a la memoria canónica en estado CodeMemory, SISTEMA §6) —
   `hle/ios.cpp` (~l. 230–254) lee de archivo directo a la RAM del juego: en Switch funciona pero libnx lo parte
   en trozos de 4 KB (más lento al cargar); volcado periódico de fallos en Switch; G5.
4. Reconstruir desde cero: copiar `r9/r10c/{nWiiRuntime,export,cmake,tools,build_*.sh}`, luego
   `python3 tools/fm_header.py nWiiRuntime/include/runtime/cpu_context.h <sha256 de la de r10c>`, copiar
   `src/core/fastmem_impl.inc` y `a64_decode.h`, añadir el `#include` al final de `src/hle/memory.cpp`,
   `python3 tools/apply_r12_fastmem.py export ../r11c/prof_mmio_list.tsv sites.tsv --orig ../r10c/export`,
   `-DNWII_FASTMEM=1` en `cmake/nsmbw-switch.cmake` y en `build_nx.sh`; después
   `FULL=1 JOBS=2 ~/nsmbw-port/scripts/lim ./build_pc.sh bin/NSMBW_r12fm` (~22 min) y `build_nx.sh` (~28 min).
5. Verificar: `test/build_unit.sh`, `tools/check_fm_tab.py bin/NSMBW_r12fm_input_test sites.tsv`,
   `tools/verify_pc.sh smoke|det|det2`, `tools/ab_private.sh lvl11|map` (`PROFILE=e2|e1`).

## Notas de las corridas

- `r9/render/pc/.runlock` estaba ocupado todo el rato por los A/B de otros frentes (`ab_gpuxf.sh`,
  `ab_locked.sh`, `rphase_base.sh`), así que **no usé la carpeta compartida**: copias privadas del arnés en
  `pc/` (humo) y `pc_ab/` (A/B), con `limc` ampliado con conjuntos de CPU fijos (`q0` 0–3, `q1` 4–7, `q2` 8–11,
  `q3` 12–15, `e1` 14–15 al 70 %) para no tocar las CPUs 16–17 del perfil `sw` de los demás. `det.sh` también
  en copia privada (`tools/det.sh`, salidas en `test/runs/`). Resultados de texto en `runs/`.
- Las velocidades entre conjuntos de CPU distintos no se comparan (el mismo r10c dio 930 copias en `q0` y 1 173
  en `q3`): solo cuentan los A/B ABBA en el mismo conjunto.
