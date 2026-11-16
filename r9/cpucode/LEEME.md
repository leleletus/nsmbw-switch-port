# R9, frente "cpucode": código del juego más rápido en ARM (31-oct-2026)

Punto de partida: `r9/base` (r8). Todo está en `r9/cpucode/`. No se tocó `NWiiRecomp/`, `recomp/`, `switch/`, la
NAND del usuario ni la Switch real. Sin commits.

## Resultado en una línea

@@RESUMEN@@

## 1. Qué se cambió (todo es 1:1: mismas operaciones, mismos accesos a memoria, mismo orden)

### (a) Accesos a memoria del juego en ARM: de 9-10 instrucciones a 6 (`cpu_context.h`, `MemView`)

Mirando el ensamblador de devkitA64 de las funciones más caras (`func_801A3880`, `func_801A5C10`, `func_801A2B10`),
cada `lwz`/`stw` del juego costaba:

```
and  w0, wA, #0x3fffffff      ; dirección física
mov  w1, #0xffef              ; ┐ la constante 0x017FFFF0 (tamaño de MEM1 - 0x10)
movk w1, #0x17f, lsl #16      ; ┘ se volvía a construir antes de CADA acceso
sub  w3, w0, #0x10
cmp  w3, w1
b.hi lento
cbnz wWatch, lento            ; (escrituras) bandera de NWII_WATCH
add  x0, x20, w0, uxtw        ; base + desplazamiento (no se juntaba con la carga)
ldr  w0, [x0]
rev  w0, w0
```

En ARM `0x017FFFF0` no cabe como inmediato de `cmp`, y GCC prefiere reconstruirla con `mov+movk` en cada uno de los
~660 000 accesos del código generado. Ahora `MemView` (una por función, como en r8) guarda ese límite en un campo
que pasa por un `asm("" : "+r")` vacío: para GCC es un valor desconocido que deja en un registro toda la función.
Para las escrituras hay un segundo límite que vale 0 si hay un `NWII_WATCH` puesto (así la bandera de vigilancia
deja de costar una instrucción por escritura, y con vigilancia todas las escrituras siguen yendo al camino lento,
como antes). Cada ancho tiene su propia carga/guardado para MEM1 y para MEM2 (sin un puntero común), y GCC junta
base + desplazamiento en la propia instrucción (`ldr w0, [x21, w0, uxtw]`). MEM2 se prueba con `(paddr >> 26) == 4`
(inmediatos que sí caben). Resultado:

```
and w0, wA, #0x3fffffff ; sub w2, w0, #0x10 ; cmp wLim, w2 ; b.ls lento ; ldr w2, [x21, w0, uxtw] ; rev w2, w2
```

Las condiciones son exactamente las de r8 (mismo rango de MEM1 desde 0x10, mismo MEM2, mismo camino lento para
MMIO, la tubería de GX, la caché bloqueada, direcciones sin mapear y el caso 0xC), así que se leen y escriben los
mismos bytes en el mismo orden. En x86 el `asm` vacío no se pone (allí `cmp` admite la constante).

### (b) Sin pasar por la GOT (`cpu_context.h`, `os_hle_stubs.cpp`, `perf/apply_r9_cpucode.py`, cmake)

El NRO es un PIE y el `ld` de devkitA64 no relaja la GOT en AArch64: cada variable global externa es
`adrp + ldr (GOT) + ldr`. Cada función generada leía así `g_trace_calls` y `g_watch_addr` al entrar, cada llamada
indirecta del juego (`nwii_dispatch`: funciones virtuales, punteros a función) leía `g_disp_base`, `g_disp_span` y
`g_disp_table`, y antes de cada llamada recursiva `native_stack_ok()` comprobaba por la GOT la función de
inicialización TLS (débil) de `g_native_stack_limit`.
- `g_trace_calls` y `g_watch_addr`: visibilidad `hidden` (un solo módulo enlazado estáticamente).
- `g_native_stack_limit`: `constinit thread_local` (ya tenía inicializador constante): acceso TLS directo.
- `perf/apply_r9_cpucode.py` (pasada nueva sobre el código generado): `g_disp_base`/`g_disp_span` pasan a ser
  `inline constexpr` en `functions.h` (los mismos valores, sacados de `fastdispatch.cpp`) y `g_disp_table` `hidden`.
- Switch (`cmake/nsmbw-switch.cmake`): `-fvisibility=hidden` para todo lo que compilamos (en la biblioteca del
  runtime, que va con `-fPIC`, además deja de tratar sus propias funciones como interponibles) y
  `-fomit-frame-pointer` (devkitA64 guardaba `x29` como puntero de marco en cada función: un registro más libre).
  Se mantiene `-ffp-contract=off` (sin FMA nuevas: el FP sigue siendo 1:1).

Tamaño del código generado para Switch (suma de `.text` de los 89 objetos): r8 123,6 MB → **115,7 MB (−6,4 %)**;
NRO 135,9 → 128,0 MB. En `output_24` (la unidad de la reverb) −7 %; `func_801A3880` 5762 → 5226 instrucciones,
`func_801A5C10` 2072 → 1847, `func_801A2B10` 2178 → 2007 (y lo que se va es del camino rápido: los caminos lentos
no cambian).

### (c) Reverb AXFX nativa (`func_801A3880`, `perf/apply_r9_axfx.py`)

`func_801A3880` es el callback de reverberación AXFX del juego (96 muestras × 3 canales por llamada, ~330
llamadas/s; ~10 % del hilo del juego en el perfil del PC). En el código generado cada registro del juego vive en
`ctx` y cada escritura a memoria del juego puede apuntar a cualquier sitio, así que GCC recarga y vuelve a guardar
`ctx.gpr[]`/`fpr[]`/`ps1[]`/`cr` alrededor de cada acceso (~150 accesos a `ctx` por vuelta del bucle interior).

`perf/apply_r9_axfx.py` (pasada nueva) copia el cuerpo de la función desde `loc_801A3894` (justo después de la
llamada a `_savegpr`) hasta `loc_801A3D8C` (justo antes de `_restgpr`) a una función nueva,
`nwii_r9_axfx_native`, **traduciendo línea a línea** el C++ generado: `ctx.gpr[N]` → variable local `gN`,
`ctx.fpr[N]` → `fN`, `ctx.cr[0]` → `c0lt/gt/eq/so`, `ctr`, `xer`, `inst_count`; `fill/fmuls/fadds/fsubs/fctiwz`
con las mismas fórmulas que `CPUContext` (`ppc_single`, `ppc_force25`, `ppc_fctiw_image`); la memoria del juego por
el mismo `MemView` (mismos caminos rápidos y lentos, mismo orden). `ps1` se guarda aparte solo en los registros que
también se escriben sin él (`lfd`, `fmr`, `fctiwz`: f0, f2, f9); en los demás `ps1 = fpr` tras la última escritura.
Al salir se escriben en `ctx` todos los registros que el trozo puede cambiar, `pc` e `inst_count` (la misma suma que
el código generado), así que en `loc_801A3D8C` el estado es exactamente el mismo y el epílogo generado sigue igual.
No es una reimplementación a mano: es el mismo programa, sin `ctx` de por medio.

Lo único que no se copia son los dos sondeos de interrupciones de los bucles (`process_pending_callbacks` en
0x801A3B68 y 0x801A3D88). Con MSR[EE] apagado un sondeo no puede entregar ninguna interrupción; solo adelanta el
planificador de eventos, que entonces avanza en el siguiente sondeo del llamador (unos microsegundos después,
como si la función hubiera ido más rápido, que es lo que pasa). Por eso el cuerpo nativo **solo se usa con EE
apagado**; con EE encendido corre el código generado de siempre. En la práctica la reverb se llama siempre desde
la interrupción del DSP: en las corridas de PC, `generated(EE set)=0` de 25 000+ llamadas.
`NWII_AXFX_NATIVE=0` lo apaga. La pasada comprueba el sha256 del texto de la función y se niega si cambia.

## 2. Qué se probó y no se quedó

- **Quitar el alias entre la memoria del juego y `ctx`** (cada `memcpy` a `uint8_t*` puede tocar cualquier cosa, y
  GCC vuelve a leer `ctx.gpr[]` tras cada escritura del juego). Se probó un tipo propio para los accesos (unión de
  `enum class`, que en C++ tiene su propio conjunto de alias): funciona en ejemplos pequeños, pero en el código real
  no cambia nada porque la que obliga a recargar es la **llamada al camino lento** (una llamada puede cambiar
  cualquier memoria, `ctx` incluido) en cada unión rápido/lento. Con el camino lento llamado desde un `asm` con la
  lista exacta de registros y memoria que toca (sin "memory"), GCC deja de recargar, pero el código no sale más corto
  (5833 frente a 5762 instrucciones en la reverb): GCC está limitado por registros en estas funciones enormes y
  vuelve a cargar de `ctx` o de la pila igual. Además obliga a auditar que ningún camino lento toca registros del
  juego. **Descartado** (no gana y tiene riesgo). Cota superior medida: con los caminos lentos como `trap` (no
  válido) la unidad baja un 13 %.
- `__restrict` en el parámetro `ctx`: sin efecto (GCC lo pierde en cuanto `ctx` se pasa a otra función).
- `-mcpu=cortex-a57`: devkitPro ya pone `-march=armv8-a+crc+crypto -mtune=cortex-a57`, es lo mismo.
- `-fno-plt`: no aplica (enlace estático, no hay PLT).
- `-O3` en las unidades calientes: no se midió en citron por falta de tiempo (r8 ya vio que las funciones grandes
  están limitadas por la memoria y los registros; `-O3` sobre todo desenrolla y vectoriza, lo que aquí sube la
  presión de registros).
- **PGO cruzado** (perfil en el PC, uso con devkitA64): **no se adopta**. El GCC del PC es 16.2.1 y devkitA64 es
  16.1.0: el `.gcda` tiene otra versión (`B62*` frente a `B61*`) y devkitA64 lo rechaza. Cambiando a mano la versión
  en el archivo lo lee, pero el grafo de control no coincide entre x86 y AArch64 ni siquiera en un programa de 4
  líneas (`main`: "the control flow ... does not match its profile data"), porque las expansiones tempranas
  dependen del destino. En el código generado (y con los `#ifdef` de plataforma del runtime) el perfil se aplicaría
  solo a una parte de las funciones, sin aviso fiable. Haría falta un GCC 16.1 del PC y aun así no sería 1:1 en qué
  funciones reciben perfil. No compensa.

## 3. Números

@@NUMEROS@@

## 4. Verificación 1:1

- **Reverb nativa, bit a bit, con datos reales del juego** (`NWII_AXFX_VERIFY=n`, solo pruebas, código en la propia
  pasada): cada n-ésima llamada se copia toda la RAM del juego (MEM1+MEM2) y los registros, se ejecuta el cuerpo
  nativo, se guarda un hash de toda la RAM y todos los registros que deja (GPR, FPR y PS1 bit a bit, los 8 CR, CTR,
  XER, LR, PC, MSR, `inst_count`), se restaura la copia, se ejecuta el código generado de siempre y en
  `loc_801A3D8C` se compara. PC, `NSMBW_c2_input_test`, guion del 1-1 (título → selección → mapa → 1-1), NAND
  privada `mp`, sin ventana ni sonido:
  - `pc/runs/verify3_lvl11` (`n=40`, 400 s): **651 comparaciones, 651 iguales, 0 distintas** (25 389 llamadas al
    cuerpo nativo, 0 con EE encendido); llega al 1-1 (`phase 2 starts`).
  - `pc/runs/verify_lvl11` y `verify2_lvl11` (primera versión del verificador, con 4 copias de 88 MB por ronda):
    59 iguales, 0 distintas. Con la máquina cargada (media de carga 20-35: otro proceso del usuario con 10 núcleos
    al 100 % y tres juegos de otros frentes) esas copias dejaban el juego casi parado por presión de memoria del
    slice; por eso la versión final guarda una sola copia y compara hashes.
- **Accesos a memoria (`MemView`)**: las condiciones de cada camino son las mismas que en r8 (se comprobó una a
  una): MEM1 `paddr - 0x10 < 0x017FFFF0` con el mismo valor (solo que en un registro), MEM2
  `paddr - 0x10000000 < 0x04000000` ⇔ `paddr >> 26 == 4` (paddr < 2^30) y desplazamiento `paddr & 0x03FFFFFF` =
  `paddr - 0x10000000` dentro del rango; con `NWII_WATCH` el límite de escritura es 0 y MEM2 lleva `!watch`, así
  que toda escritura va al camino lento como antes. Mismos bytes, mismo orden, mismo camino lento.
- `hidden`, `constinit`, `inline constexpr` y `-fomit-frame-pointer`/`-fvisibility=hidden` no cambian qué se
  calcula: solo cómo se llega a la dirección de una variable y qué registro guarda el marco.
- **Humo en PC** (`pc/runs/hang_c2`, sin verificación, 75 s con la máquina cargada): `OK`, llega al 1-1 a los 31 s,
  45,8 fps de media (mín. 38,5) con 3 juegos más y demucs en marcha, 0 `[Gov]`.
@@CITRONOK@@

## 5. Archivos

| Qué | Dónde |
|---|---|
| parche del runtime (contra `r9/base/nWiiRuntime`, `patch -p1` desde `NWiiRecomp/`) | `r9_cpucode.patch` (`cpu_context.h`: `MemView`, `NWII_HIDDEN`, `constinit`; `os_hle_stubs.cpp`: `constinit`) |
| parche de la cadena del recompilador (desde `~/nsmbw-port`, `-p1`) | `r9_cpucode_recomp.patch`: añade `perf/apply_r9_cpucode.py` y `perf/apply_r9_axfx.py` y los llama en `recomp/build.sh regen` después de `apply_r8_codegen.py` |
| parche del cmake de Switch | `r9_cpucode_cmake.patch` (`switch/cmake/nsmbw-switch.cmake`: `-fomit-frame-pointer -fvisibility=hidden`) |
| código generado con las dos pasadas | `export/` (= `r9/base/export` + `apply_r9_cpucode.py` + `apply_r9_axfx.py`) |
| binarios congelados | `bin/` (sha256 en `bin/SHA256SUMS`) |
| herramientas | `tools/build_tu.sh` (compila una unidad para Switch como el build), `tools/seq_runs.sh` y `tools/runs_table.sh` (corridas de citron alternadas y su tabla), `tools/abba.sh` |
| corridas | `runs/` (citron), `pc/runs/` (PC) |

**Al integrar**: `cpu_context.h` cambia y `functions.h`/`fastdispatch.cpp`/`output_24.cpp` también, así que hay que
recompilar **todo** el código generado (PC y Switch, `FULL=1`). Las dos pasadas se aplican una sola vez (marcan los
archivos con `// [r9cpucode]` / `// [r9axfx]`). `apply_r9_axfx.py` comprueba el sha256 del texto de
`func_801A3880` y se niega si el recompilador genera otra cosa.
