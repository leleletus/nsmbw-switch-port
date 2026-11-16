# Benchmark en Switch real (nxbench, 8-nov-2026)

Medido con `tools/nxbench` (código y método en `tools/nxbench/source/main.cpp`) en la consola del usuario:
aplicación con toda la RAM (3189 MB), núcleos 0-2 permitidos (máscara 0x7), **con overclock**: CPU a
**1785 MHz** en la pasada normal y **1963,5 MHz** con `appletSetCpuBoostMode(FastLoad)`. Resultados crudos:
`switch/real_logs/nxbench_1108/resultados_1.txt`.

Los **ciclos** no dependen del reloj (las dos pasadas dan los mismos ciclos), así que valen para una Switch v1
sin overclock: para pasar a tiempo a 1020 MHz, `ns = ciclos / 1,02`.

> **Objetivo del proyecto: 60 fps en Switch v1 sin overclock, modo portátil (CPU 1020 MHz).**
> Presupuesto por frame y por hilo: 16,7 ms = **17 M ciclos**.

## 1. Lo más importante para el port

| Hallazgo | Medida | Qué significa |
|---|---|---|
| **Registros de la Wii en memoria (`ctx.gpr[N]`) frente a registros ARM** | 2 operaciones del juego: **15 ciclos** con `ctx` en memoria, **2 ciclos** en registros | ×7,5. Cada operación que lee un registro recién escrito en memoria paga la ida y vuelta por la memoria (almacenar → cargar ≈ 6-7 ciclos en el A57). **Es la diferencia principal con el JIT de Dolphin** y la palanca más grande del código traducido. |
| **"fadds" al estilo PowerPC** (sumar en double y redondear a single) | **15 ciclos** de latencia frente a **5** de un `fadd` normal | Cada redondeo `double → float → double` (`fcvt` ida y vuelta) cuesta 10 ciclos. Cuando los operandos ya son floats (tras `lfs`, `fadds`, `fmuls`…), sumar/restar/multiplicar/dividir directamente en float da **exactamente el mismo resultado** (teorema del doble redondeo: 53 ≥ 2·24+2) → ×3 en cadenas de coma flotante. |
| `fmadd` (multiplicar-sumar fusionado) | 10 ciclos de latencia | No es más rápido que `fmul` (6) + `fadd` (5). En el A57 no hay ventaja por fusionar. |
| `malloc + free` (newlib) | **423 ciclos** (64 B) | Muy caro: el hilo de render hace miles por frame. Usar pools o búferes reutilizados. |
| Una llamada al sistema (svc) | ~340 ciclos (190 ns a 1785 MHz) | Nada de svc en caminos calientes. |
| `armGetSystemTick()` | 8 ciclos | El reloj barato. `clock_gettime` = 30 ciclos, `steady_clock::now()` = 41. |
| mutex sin contención | 78 ciclos (libnx), 89 (`std::mutex`) | Un mutex por escritura FIFO o por dibujo es caro. |
| `std::shared_ptr` copia + destrucción | 73 ciclos | Por los atómicos (`ldxr/stxr`, ARMv8.0 sin LSE): `fetch_add` = 21-29 ciclos. Usar conteo no atómico donde solo toca un hilo. |
| Despertar un hilo en otro núcleo (evento ida y vuelta) | **~8 400 ciclos** (4,7 µs) | Repartir trabajo entre núcleos sirve para bloques grandes (un frame, una lista de audio), no para tareas pequeñas. |
| Ida y vuelta de una línea de caché entre núcleos | ~270 ciclos | Las variables compartidas entre hilos cuestan; separarlas en líneas de 64 B. |
| Fallo de predicción de salto | ~21 ciclos | Llamada indirecta con destino variable: ~28 ciclos; con destino fijo, 6. |

## 2. Memoria

| Tamaño del bloque recorrido al azar | Latencia | Nivel |
|---|---|---|
| 8-24 KB | 4 ciclos | L1D (32 KB) |
| 48-512 KB | 22-24 ciclos | L2 (2 MB compartida) |
| 1,5 MB | ~50 ciclos | L2 casi llena (compartida con los otros núcleos) |
| 3-64 MB | **~245-265 ciclos** (~135 ns) | RAM LPDDR4 |

- Ancho de banda de **un núcleo**: `memcpy` 3,7 GB/s, lectura secuencial con NEON 4,9 GB/s, `memset` 10,6 GB/s.
  Copiar 1 MB cuesta ~270 µs. Mover datos gordos (p. ej. `VertexData` de ~170 bytes por vértice × 77 000
  vértices por frame en el mapa) es caro: adelgazar estructuras y evitar copias.
- La RAM de la Wii (24 MB MEM1 + 64 MB MEM2) no cabe en la L2: los accesos del juego dispersos pagan RAM.

## 3. Enteros (latencia en ciclos)

`add`, `rev` (bswap), `ror`, `ubfx`, `clz`: 1 · `bfi`, `cmp+csel`: 2 · `crc32w`: 3 · `mul` 32 bits: 3 ·
`mul`/`madd` 64 bits: 5 · `udiv` 32/64: ~6 · `add` independientes: 2 por ciclo.

→ El intercambio de bytes (`rev`) es gratis comparado con todo lo demás. Preferir operaciones de 32 bits para
multiplicar.

## 4. Coma flotante y NEON (latencia en ciclos)

| Operación | float32 | float64 |
|---|---|---|
| `fadd` | 5 | 5 |
| `fmul` | 6 | 6 |
| `fmadd` | 10 | 10 |
| `fdiv` | 18 | 32 |
| `fsqrt` | ~17 | ~7 (*) |
| rendimiento `fadd`/`fmul` independientes | ~1,3-1,6 por op | |

- `fcvt` double ↔ float: 5 cada sentido. FP16 (`fcvt` s ↔ h): solo conversión, 10 ida y vuelta: **el A57 no
  calcula en FP16** (eso es de la GPU).
- `fmov` entre registros enteros y de coma flotante: ~4,5 por sentido. `scvtf`/`fcvtzs`: ~12 por sentido.
- Denormales: **sin penalización** en `fmul` (6 ciclos igual).
- NEON: `fmla` de 4 floats = 4 ciclos de latencia y **1 por ciclo** de rendimiento (4 multiplicaciones-suma por
  ciclo). Ojo: la versión de 64 bits (`2s`) dio 10 de latencia en esta prueba (operando acumulado en sí mismo;
  revisar). `fmul` de 2 doubles: 6. `rev32` de 16 bytes (4 bswap a la vez): 3.

(*) `fsqrt d` dio menos que `fsqrt s` porque el valor converge a 1,0 y el A57 termina antes; tomarlo como
orientativo.

## 5. Qué NO midió bien esta versión del benchmark (para la próxima)

- "lectura guest con/sin comprobación": las direcciones caían al azar en 24 MB y la prueba midió la **latencia de
  la RAM** (~250 ciclos), no el coste de la comprobación. Repetir con un bloque que quepa en L1.
- Falta: una cadena real de "fadds/fmuls en float" frente a la emulación actual con los mismos números, y el
  coste de `ppc_force25`.

## 6. Conclusiones para el port (Switch v1, 1020 MHz)

Medido en la consola del usuario en el 1-1 (ver `switch/real_logs/r9combo_1108/`): el hilo del juego gasta
~25 ms por frame **a 1785 MHz** = ~45 M ciclos. A 1020 MHz serían ~44 ms (≈ 20-23 fps). El hilo de render,
~20 ms a 1785 = ~36 M ciclos. **Para 60 fps en una v1 hace falta bajar ~×2,7 el hilo del juego y ~×2,1 el de
render** (más en el mapa del Mundo 1).

Orden de ataque según estos datos:
1. Interrupciones del juego (21 % del hilo en el perfil real): inline barato (r10a, en curso).
2. **Registros de la Wii en registros ARM** dentro de cada función (escribir a `ctx` solo en llamadas, sondeos y
   salidas): hasta ×7 en secuencias de enteros.
3. **Floats directos** cuando los operandos ya son single (×3 en latencia de coma flotante, resultado idéntico).
4. Render: sin `malloc`/`shared_ptr` atómicos por dibujo (r10b), vértices más delgados, transformar vértices en
   la GPU, y después deko3d.
5. Repartir: render dividido entre dos núcleos (decodificar GX en uno y enviar a la GPU en otro), bloques de un
   frame entero por la latencia de despertar (~8 400 ciclos).
