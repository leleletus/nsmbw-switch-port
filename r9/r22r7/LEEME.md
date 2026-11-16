# r22r7: el "worker" de render más barato (frente R7, 9-nov-2026)

**Qué es:** copia de `r9/r18m` (m1, lo que juega el usuario: GL, 720p) con cambios **solo en el runtime**
(`nWiiRuntime/`) para que el hilo "gxw" (worker de render, núcleo 2) gaste mucho menos por frame. Encima
lleva `r9/r21s/forcedemo.patch` (opciones `NWII_FORCE_DEMO`, `NWII_NO_SAVE`, ...) para poder repetir la
cinemática del castillo y el desierto en la consola. Sin commits. No se tocó ninguna carpeta de otro frente.

**Estado (9-nov 06:30):** r7d medida en la consola por el coordinador: **castillo 46,3 → 59,4 fps** (núcleo 2
71 % → 29 %), imagen limpia en las 54 capturas; se instala r7d como `NSMBWnx.nro`. r7e (réplica más barata) y
r7f (= r7e + arreglo de los interruptores en la Switch, ver §5) verificadas en PC, sin medir aún en la consola.

## 1. Dónde se iba el tiempo del worker

El worker (`PrepFrame` en `src/hle/gx.cpp`) toma los bytes del FIFO, los interpreta (expandiendo las
display lists desde la caché de DL), busca el fin de frame, decodifica los dibujos y prepara sus entradas de
la caché de emisión (R5), y entrega el frame al hilo de render.

Medido en PC (perfil `sw`, castillo = escena MOVIE del título, ~12 200 dibujos GX y ~27 400 comandos por
frame; línea nueva `[WPROF]`) y en la consola (`profile_gxw.txt` de m1, `bench_m1_gxw_castillo_1109_0359`):

| Parte | PC m1 (ms/frame) | Por qué |
|---|---|---|
| Réplica de las DL desde la caché (copiar ~20 300 `GXCommand` de 136 B + arreglos de estado) | 2,4-2,9 | escribir ~2,7 MB por frame |
| Bucle decode/prep por dibujo (12 200) | 2,3-2,8 | fallos de caché: `DrawRaw`, vector de rangos, contador del `DecodedVerts` (todos aciertan la caché de decodificación: no hay vértices nuevos) |
| Destruir el frame anterior | 0,7-0,9 | releer 3,7 MB |
| Re-escanear el frame buscando dibujos y fin de frame | 0,6-0,7 | releer lo recién escrito (ya fuera de caché) |
| Hash del contenido de las DL (715 KB/frame) | ~0,5 | hash con multiplicaciones |

No hay una etapa "de vértices" cara: el coste es tráfico de memoria por comando y por dibujo (en la Switch,
con más latencia de memoria, pesa todavía más).

## 2. Cambios

| | Qué | Dónde | Interruptor (por defecto encendido) |
|---|---|---|---|
| r7w | Línea `[WPROF]` cada 2 s: tiempo del worker por frame y por etapa (lock, parse, scan, clear, split, decode, prep, hand), réplicas de DL, y cuántas veces el hilo de render liberó el frame | `gx.cpp` | `NWII_WPROF=2` separa decode/prep por dibujo (2 lecturas de reloj por dibujo) |
| a | **Memo de dibujos por display list**: cada entrada de la caché de DL guarda, por dibujo, sus vértices decodificados y su entrada de la caché de emisión, válidos mientras no cambie la memoria que leen (la misma prueba que la caché de decodificación: hash de sus rangos). Al replicar la DL, el dibujo se escribe desde esa tabla compacta, sin tocar `DrawRaw` ni contadores de referencia, y su entrada queda anotada: el bucle de fin de frame ya no visita esos dibujos | `fifo_parser.cpp` (`DLDrawMemo`, `ReplayMemo`) | `NWII_DLMEMO=0` |
| a | Los comandos apuntan a esos vértices **sin ser dueños** (puntero sin bloque de control); la tabla es la dueña, y lo que se reemplaza o expulsa va a un "cementerio" que se libera cuando ningún frame vivo puede apuntarle (número de frame por paquete) | `fifo_parser.cpp`, `gx.cpp` | — |
| a | **Índice del parser**: el parser anota la posición de cada dibujo y de cada fin de frame (BP 0x52) mientras escribe los comandos: se acabó el re-escaneo del frame | `fifo_parser.cpp`, `gx.cpp` | `NWII_PINDEX=0` (vuelve al escaneo y apaga el memo) |
| a | `DLStateHash` solo se recalcula si una escritura CP cambia el valor | `fifo_parser.cpp` | — |
| a | La época del memo de hashes de memoria (`RangesHash`) avanza también al terminar cada frame: las réplicas del frame siguiente miran la memoria de nuevo | `gx.cpp` | — |
| c | Hash de contenido de las DL y de los trozos de 1 KB con **CRC32C** (instrucción `crc32cx` de ARMv8 / SSE4.2), 4 carriles | `fifo_parser.cpp` | `NWII_DLHASH=fnv` |
| c | **Validación por lista**: si ningún trozo de 1 KB que usan los dibujos de una DL cambió desde que todos sus memos eran válidos, no se prueba dibujo por dibujo | `fifo_parser.cpp` | `NWII_DLMEMO_LIST=0` |
| c | **Reparto adaptativo**: el hilo de render (núcleo 0) libera los comandos del frame que acaba de dibujar si no hay ya otro frame esperando (es decir, cuando el lento es el worker) y ningún comando comparte contadores con el worker; si no, lo hace el worker como antes | `gx.cpp` | `NWII_RENDER_FREE=0` |
| d | `GXCommand` de 136 a **128 bytes** (tipo de 1 byte, campos reordenados) | `include/runtime/gx_command.h` | — |
| d | `forcedemo.patch` del frente S | 4 archivos | — |

Pruebas nuevas (solo para PC): `NWII_PINDEX_CHECK=1` (el índice del parser debe ser igual al escaneo
antiguo), `NWII_DLMEMO_CHECK=1` (cada dibujo escrito desde el memo se decodifica otra vez desde sus bytes,
sin caché, y se compara vértice a vértice).

Todo apagado (`NWII_DLMEMO=0 NWII_PINDEX=0 NWII_RENDER_FREE=0 NWII_DLHASH=fnv`) = m1 (salvo el tamaño de
`GXCommand` y el recálculo de `DLStateHash`, que no cambian el resultado): sirve para A/B con el mismo binario.

## 3. Compilar

Igual que r18m (solo runtime y `output_24`; el resto, objetos de G5 v3 en `r9/r14g5`):

```
cd ~/nsmbw-port/r9
B=BASE=/home/djvemo/nsmbw-port/r9/r14g5
env $B JOBS=2 ~/nsmbw-port/scripts/lim r22r7/tools/build_m.sh r22r7 r7d pc
env $B DK_LIB=deko3d JOBS=2 ~/nsmbw-port/scripts/lim r22r7/tools/build_m.sh r22r7 r7d_dk3drel nx
cd r22r7 && ~/nsmbw-port/scripts/lim tools/build_hot.sh r7d_dk3drel $(cat nx/hot/profiles.txt)   # orden L
# bin/nsmbw_r7d.nro/.elf/_input_test.nro = copias de bin/nsmbw_r7d_dk3drel_hot* (sin "dk3d": arranca con GL)
```

`tools/build_hot.sh` es el de r18m con `M` apuntando aquí y la NACP "NSMBW PRUEBA r7". `export/` son enlaces
duros a `r18m/export` (no editar ahí).

Medir en PC: `test/prof.sh <bin> <nombre> castle|map|lvl11 [s] [espera] [record|stat]` (toma `.runlock`;
`record` = perf record + informe por hilo, `stat` = ciclos por frame por hilo). Verificar:
`tools/verify_m.sh r7d wcheck title off sanity` y `render/test/checks.sh` (con `flock`).

## 4. Resultados

### PC (perfil `sw`, `perf stat` por hilo, ciclos del worker por frame; mismo binario encendido/apagado)

| Escena | m1 | r7d apagado | r7a (memo+índice) | r7d | r7e |
|---|---|---|---|---|---|
| Castillo (12 200 dibujos/frame) | 12,6 M | 12,5 M | 7,9 M | 5,8 M | **5,4 M (−57 %)** |
| Mapa Mundo 1 (11 000 dibujos/frame) | — | 11,9 M | — | **5,6 M (−54 %)** | — |

`[WPROF]` castillo r7d: ~2,5-2,8 ms/frame (m1 ~7,5-8): réplica de DL ~1,8, parse del flujo principal ~0,4,
el resto <0,3. El hilo de render libera el frame anterior en ~120 de 121 frames (~0,4 ms en `sw`).
Archivos: `prof/*.stat.txt`, `prof/*_castle*.txt` (perf record), `prof/*.window.txt`.

### Consola (las corrió el coordinador; `switch/real_logs/bench_r7d_lista_1109_0532`, OC 1785, 720p, GL)

| Escena (lista fija `NWII_FORCE_DEMO`) | m1 | r7d |
|---|---|---|
| Castillo | 46,3 fps, gxw 88-91 % | **59,4 fps** (estable 60), gxw 36-41 %, worker ~6,2-7,4 ms/frame |
| Desierto 4J | 40,0 | 42,1 |
| Cielo (paratroopas) | 42,7 | 45,6 |
| Agua (pingüino) | 35,3 | 36,2 |

En el castillo **el límite pasa a ser el hilo de render** (núcleo 0, main 81-87 %, dibujo 13-14 ms/frame en
`[PIPE]`). Las demos de 4 jugadores siguen limitadas por el hilo del juego (cpu 67-75 %).
Ojo: `bench_r7d_off_1109_0543` NO es un A/B: los interruptores a "0" no se aplicaron (§5).

### Verificación en PC (`runs/`)

| Prueba | Resultado |
|---|---|
| `checks.sh` 1x title/map/walk/lvl11, r7d y r7e (con `NWII_PINDEX_CHECK` y `NWII_DLMEMO_CHECK`) | 220/220 idénticas cada una, `diff_lines=0`; índice 0 diferencias; memo: 194 M (r7d) y 200 M (r7e) dibujos re-decodificados, 0 diferencias |
| `checks.sh` 1,5789x (720p), r7d | 220/220 idénticas, `diff_lines=0` en las 4 escenas |
| `NWII_WGP_CHECK` (título 100 s), r7d | 117 M escrituras, 0 violaciones, flujo escrito = entregado (336 MB) |
| título 200 s / worker apagado 120 s / sanity 165 s, r7d | 59,7 fps, FASTMEM 0 fallos, sin cuelgues |

("pass1 differs" del título sale 0-20 según la corrida, también en otros frentes: depende del momento de las
copias EFB; no es una diferencia de imagen.) r7f solo cambia `switch_platform.cpp` (código solo de Switch):
en PC es r7e.

## 5. Lección: interruptores "0" en la Switch

`apply_env()` (`src/platform/switch/switch_platform.cpp`) **borra** la variable si el valor de debug.toml es
"0", "off", "false", "no" o vacío, salvo las claves de la lista `keeps_zero`. Un interruptor "encendido por
defecto, 0 lo apaga" no se puede apagar en la consola si no está en esa lista. r7f añade `NWII_DLMEMO`,
`NWII_PINDEX`, `NWII_RENDER_FREE`, `NWII_DLMEMO_LIST`, `NWII_WPROF`. (`NWII_DLHASH=fnv` sí funcionaba.)
Cualquier frente nuevo con interruptores así debe añadirlos ahí.

## 6. Binarios (`bin/`)

| Archivo | Qué es |
|---|---|
| `nsmbw_r7d.nro` (+ `.elf`) | **la medida en la consola** (memo, índice, CRC, liberación en render, GXCommand 128 B, forcedemo) |
| `nsmbw_r7e.nro` (+ `.elf`) | r7d + réplica sin leer los comandos cacheados de los dibujos con memo (−7 % de worker en PC) |
| `nsmbw_r7f.nro` (+ `.elf`) | r7e + interruptores apagables en la Switch (para el A/B en la consola) |
| `nsmbw_r7w.nro` | m1 + solo la línea `[WPROF]` |
| `*_input_test.nro` | solo pruebas con entrada por guion. **Nunca como juego** |
| `NSMBW_r7*_input_test` | PC |

Todas con GL por defecto (sin "dk3d" en el nombre) y orden L (59 29x secciones iguales, 0 funciones distintas).
sha256: `runs/bin_sha256_r7*.txt`.

## 7. Siguientes pasos

1. Consola: A/B limpio con r7f (con y sin `NWII_DLMEMO="0" NWII_PINDEX="0" NWII_RENDER_FREE="0" NWII_DLHASH="fnv"`)
   en el castillo y el mapa guiado; luego r7f como sucesor de r7d si no hay sorpresas.
2. **El castillo ahora lo limita el hilo de render** (núcleo 0, 13-14 ms de dibujo por frame con ~12 000
   dibujos GX → ~230 de GL): ese es el siguiente frente (o deko3d, que en el castillo dibujaba mucho más barato).
   La liberación del frame en el render solo ocurre cuando no hay otro frame esperando, así que no le roba
   tiempo cuando el render es el lento.
3. Lo que queda en el worker (~6-7 ms/frame en la consola): copiar ~8 000 comandos no-dibujo de las DL y
   ~7 000 del flujo principal por frame, y el hash de ~715 KB de DL por frame (memoria; cada DL se llama ~1 vez
   por frame, así que no hay repeticiones que ahorrar). Ideas: comandos aún más pequeños (sacar `vertices` a
   un puntero: 128 → 112 B) o que el render recorra los comandos de las DL sin copiarlos (mucho más invasivo).
4. Medir el mapa en la consola con `[WPROF]` (en PC baja igual que el castillo).
