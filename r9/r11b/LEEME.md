# r11b: transformación de vértices y texgen en la GPU (frente B, 8-nov-2026)

**Estado:** hecho y verificado en PC (imagen idéntica píxel a píxel y valores idénticos bit a bit).
**Pendiente:** probarlo en la Switch real (no se instaló nada). Números de PC: ver §4.

Binarios (en `r9/r11b/bin/`):

| Archivo | Qué es |
|---|---|
| `nsmbw_gpuxf.nro` (+ `.elf`, `_input_test.nro`) | **el que hay que instalar en la Switch** (GPU XF activado por defecto; sha256 `431ff89b…7c1769`) |
| `NSMBW_gpuxf`, `NSMBW_gpuxf_input_test` | PC |
| `NSMBW_xf1..xf4*`, `nsmbw_xf2*` | versiones intermedias (no usar) |

Compilado con `tools/variant_build.sh r11b <nombre> both` (solo runtime, enlaza con el código del juego de r10c).
Base de comparación: `r9/r11base` (= runtime de r10c).

## 1. Qué cambió

Archivos tocados (nada más): `src/hle/gx/renderer_gl.cpp`, `src/hle/gx/tev_shader_gen.cpp`,
`include/runtime/gx/tev_shader_gen.h`. No se tocó `gx.cpp`, `fifo_parser.cpp` ni cabeceras del código generado
(fácil de fusionar con r11a, que trabaja en `gx.cpp`/`ProcessGXFifo`).

Antes (r10c): por cada vértice la CPU transformaba posición y normal (`EmitPos4`), calculaba las coordenadas de
textura (`TexGen4`, con raíz y divisiones) y escribía un `BatchVertex` de 72 bytes en memoria de GPU. En la Switch
eso era ~20 % del hilo de render ocupado (`EmitPos4` 5,1 % + `TexGen4` 4,2 % de todas las muestras de
`profile_main.txt` de r10e, con el hilo ~50 % dormido). La caché de emisión de R9 evitaba repetir el trabajo solo
si las matrices no cambiaban; en el mapa quieto ~3 700 dibujos por frame fallaban igual (texturas animadas).

Ahora (`NWII_GPU_XF`, activado por defecto):

- **Un solo vertex shader nuevo** (`GenerateVertexShaderXF`) que *saca* los vértices de un buffer (dos buffer
  textures sobre el mismo buffer persistente: palabras y bloques de 16 B) y hace en la GPU la transformación de
  posición y normal y el texgen completo (matriz de textura, normalización, post-matriz, división STQ). Después
  sigue el texto del shader clásico tal cual (proyección, canales de color/luces, varyings): los fragment shaders
  (los 201 TEV) **no cambian**.
- **Índices = (dibujo del lote << 16) | vértice.** Cada lote sigue siendo un solo `glDrawElements` (~230 por
  frame en el mapa, igual que antes); el uniform `uXDraw` dice dónde empiezan las entradas de dibujo del lote.
- **Entrada por dibujo GX (16 B):** dónde están sus vértices, su formato, su matriz de posición/normal y su
  registro de texgen.
- **Vértices crudos:** posición (3 floats), normal (3, si hay), color RGBA8 (si hay), texcoords presentes, y una
  palabra de "rangos de matriz" si el dibujo usa más de una matriz (personajes con skinning). Un dibujo de display
  list deja sus vértices **en una arena para siempre** (mientras vivan sus vértices decodificados): por frame solo
  escribe su entrada (16 B) y sus índices, aunque la cámara se mueva o la textura esté animada. Los demás escriben
  sus vértices compactos en el anillo.
- **Matrices por frame en el anillo:** cada matriz (3x4 de posición + 3x3 de normal, matrices de textura, de
  post-transform) se escribe una vez por frame mientras no cambie (se compara con la última copia escrita).
  Registros de texgen: 64 B, solo cuando cambian.
- **Modo CPU dentro del mismo shader:** si un dibujo no se puede hacer en la GPU (texgen con fuente de color,
  más de 16 matrices distintas, `NWII_PREXF`), la CPU lo calcula exactamente como antes (`EmitRange`) y el shader
  pasa su `BatchVertex` sin tocarlo. En el título, mapa y 1-1 esto **no ocurre nunca** (`cpu=0` en `[GPUXF]`).
- `NWII_GPU_XF=0` → el camino de CPU de siempre (vbuf/caché de emisión de R9, o el r8 con `NWII_NORING=1`). Con
  GPU XF no se crean los buffers de vbuf (63+8 MB); el de GPU XF ocupa 48 MB (24 arena + 24 anillo).
- GPU XF **no depende de `NWII_NORING`** (tiene su propio buffer persistente, como el vbuf que ya funcionó en la
  Switch con r10e).

## 2. Cómo se garantiza el mismo resultado bit a bit

Cómo redondea la CPU: todo en `float` IEEE, sin FMA (la Switch compila con `-ffp-contract=off`; x86-64 sin
`-march` no tiene FMA), sumas de izquierda a derecha: `((x*m0 + y*m1) + z*m2) + m3`; `sqrt` y `/` correctamente
redondeados (SSE/NEON). El shader repite exactamente esas operaciones en ese orden:

- `precise` (ARB_gpu_shader5) en todas las cuentas: el compilador no puede fusionar mul+add en FMA ni reasociar.
  Variante 1 de reserva: cada producto pasa por un XOR con un uniform 0 (ningún compilador puede fusionarlo).
- División y raíz **correctamente redondeadas** (`div_rn`, `sqrt_rn`): aproximación del hardware + una corrección
  con `fma` (cociente/raíz fiel) + decisión exacta entre los dos candidatos con el resto calculado por `fma`
  (exacto para un candidato fiel) frente al punto medio (potencia de dos: exacto). Se hace sobre mantisas en
  [0,5, 2) (`frexp`/`ldexp`, exactos) para que ningún resto caiga en subnormales (la GPU los aplasta a 0: la
  primera versión fallaba el 1 % de las raíces de x < 2^-80; encontrado con la prueba exhaustiva y corregido).
- El color se sube como RGBA8: el shader clásico solo lo usa como `round(c*255)` y ningún formato de color GX da
  un .5 exacto, así que da el mismo entero.

**Autoprueba al arrancar** (siempre, ~20-60 ms): 65 536 casos de división, raíz y fila de matriz contra la CPU
(un cuarto cerca de un punto medio de redondeo, la mitad en todo el rango de exponentes). Si algo difiere prueba
la variante 1; si también falla, **apaga GPU XF** y usa el camino de CPU. Log: `[GPUXF] self-test variant ...`.

## 3. Verificación (PC: Intel Arc MTL, Mesa 26.2.5)

| Prueba | Resultado |
|---|---|
| `test/checks.sh` (FRAMECHECK: cada 30 frames se dibuja el frame con GPU XF y con el camino r8 de CPU y se comparan los píxeles) xf3, 1x y 2x, título/mapa/paseo/1-1 | **8/8 escenas, 1 740 comparaciones, 0 píxeles distintos** (`test/checks_xf3.txt`; también xf2 1x: `checks_xf2_1x.txt`). El "pass1 differs" del 1-1 (116) es igual en la base (efecto conocido de copias EFB, `chk_base_lvl11_1x`). |
| `NWII_GPU_XF_CHECK=20` (transform feedback: cada dibujo GPU de cada 20.º frame se ejecuta en la GPU y se compara con la CPU valor a valor: posición, normal, 4 texcoords, color cuantizado) | modo separable (como la Switch): paseo por el mapa **551 151 dibujos / 3 834 604 vértices**, 1-1 **650 375 dibujos / 4 626 404 vértices**: **0 valores distintos** (`pc/runs/sso_chk_walk`, `sso_chk_lvl11b`) |
| Autoprueba extendida (`NWII_GPU_XF_SELFTEST=2000 NWII_GPU_XF_SQRT_ALL=1`) | variantes 0 y 1: **131 072 000 casos div/sqrt/fila y la raíz de los 2 130 706 432 floats normales positivos: 0 diferencias** |
| Modo separable (`NWII_SHADER_SSO=1`, el de la Switch) + FRAMECHECK | paseo 60/60 y 1-1 180/180 (gpuxf) y 220/220 (xf4) frames idénticos; xf4 1-1: otros 726 069 dibujos / 5 163 991 vértices por transform feedback, 0 distintos |
| Binario final `gpuxf` (= fuentes de xf4: xf3 + `div_rn`/`sqrt_rn` escalados con frexp/ldexp) | `checks.sh` 1x: título/mapa/paseo/1-1 **4/4 escenas, 880 comparaciones, 0 píxeles distintos** (`test/checks_gpuxf_1x.txt`), más el 1-1 separable y el A/B |

Diferencias posibles conocidas (no vistas en ninguna prueba): resultados **subnormales** (|x| < 1,2e-38) en
productos o cocientes: la CPU los conserva y la GPU los aplasta a 0. Valores así no aparecen en coordenadas de
textura ni posiciones reales.

## 4. Rendimiento en PC (perfil de CPU `sw`, 2 núcleos LP-E con cuota del 150 %, 1x)

Medido con `test/ab_gpuxf.sh` (bajo el candado `r9/render/pc/.runlock`, orden A/B/B/A, `pc_ab.sh`: media de las
líneas `[RPROF]` tras llegar a la escena; resultado completo en `test/ab_gpuxf.txt`). `render_cpu` = µs de CPU del
hilo de render por frame.

| Escena | base r11base (A) | gpuxf (B) | gpuxf (B) | base (A) | cambio |
|---|---|---|---|---|---|
| Mapa del Mundo 1 quieto (11 038 dibujos GX, 228 GL) | 6 919 | 6 230 | 6 282 | 8 422 (*) | **−10 %** frente a la base buena (−18 % frente a la media) |
| 1-1 (2 550 dibujos GX, 117 GL) | 4 532 | 3 890 | 3 902 | 4 248 | **−11 %** |

(*) la 4.ª corrida del mapa salió más lenta (ruido de la máquina; drain también subió), por eso doy la cifra
conservadora. Los fps son ~60 en todos los casos: con este perfil el PC llega a 60 y lo que cambia es la CPU
libre del hilo de render.

Datos que no dependen del ruido (`NWII_RPHASE=1`, mapa quieto, por frame):

| | base | gpuxf |
|---|---|---|
| Bytes escritos a memoria de GPU | **2 420 KB** (72 B por vértice transformado + índices) | **798 KB** (índices 623 KB + entradas 176 KB + matrices) |
| Fase "emit" de RPHASE (4 ventanas de 120 frames) | 3,27-5,90 ms (corrida de las 13:18 bajo el candado; muy ruidosa: total 8,0-14,5 ms) | 1,70-1,79 ms (total 6,8-7,0 ms) |
| Vértices transformados en CPU | ~25 000 (los fallos de la caché de emisión) | **0** |

En la Switch la ganancia debería ser mayor que en el PC: allí `EmitPos4` + `TexGen4` eran ~20 % del tiempo ocupado
del hilo de render en el perfil real de r10e, y cada byte va a memoria sin caché (GART). Falta medirlo en la consola.

Lo que queda en el hilo de render (gpuxf, RPHASE mapa): state 2,1 ms (prólogo de `DrawPrimitive`/clave de lote),
emit 1,7 ms (entrada por dibujo, matrices, índices), regs 0,75, gldraw 0,55, copy 0,47, frame 0,8.

## 5. Caché de shaders

- **Switch (programas separables, la caché solo guarda fuentes de fragment shaders):** la clave en disco sigue
  siendo la de antes (FS + texto del vertex shader clásico), así que **la `cache/shadercache.bin` de 201
  programas sirve tal cual y no crece**. El arranque compila esos 201 FS como siempre y, en vez del vertex shader
  clásico (808 ms en r10e), compila el de GPU XF (PC: 270-330 ms frente a 23-27 ms del clásico; en la Switch estimo
  1-3 s, una sola vez por arranque). El clásico solo se compila si hace falta (`NWII_GPU_XF=0` o FRAMECHECK).
- **PC (programas enlazados con binarios):** la clave en memoria y en disco incluye el texto del vertex shader, así
  que los programas GPU XF son entradas nuevas (se compilan y se añaden la primera vez; los binarios viejos no se
  usan con el shader nuevo).

## 6. Riesgos y qué mirar en la Switch

- Mesa 20.1 nouveau: si no respeta `precise`, la autoprueba lo detecta (fila distinta) y usa la variante 1; si
  `fma`/`frexp`/`ldexp` no son exactos, apaga GPU XF. Mirar en `nwii.log` las líneas `[GPUXF]`.
- Buffer textures sobre un buffer persistente coherente en GART: nvc0 invalida la caché de texturas por draw para
  buffers coherentes (como hace con vbuf). Si se viera basura, probar `NWII_GPU_XF=0`.
- Coste de GPU: unas 30-40 lecturas de buffer y ~500 instrucciones por vértice (~0,5-1 ms/frame en el mapa a
  307 MHz, estimado). La GPU estaba casi ociosa.
- La primera vez que aparece un dibujo de display list se escriben sus vértices en la arena (24 MB; mapa ~8 MB).

En `debug.toml` de la Switch: `NWII_GPU_XF = "0"` para A/B; `NWII_GPU_XF_CHECK = "60"` hace la comprobación
por transform feedback en la consola (lento, solo para verificar; resultado en la línea `[GPUXF] ... check:`).
Línea `[GPUXF] per frame:` cada 600 frames: dibujos GPU/CPU, vértices escritos, KB del anillo por frame.

## 7. Cómo seguir

- Probar `nsmbw_gpuxf.nro` en la Switch (mapa del Mundo 1 y 1-1), con y sin `NWII_GPU_XF=0`.
- Lo que queda caro por frame en el hilo de render: índices (≈155 000 por frame en el mapa, 620 KB a memoria sin
  caché), la fase "state" de `DrawPrimitive` y los registros XF/BP. Siguiente paso posible: índices estáticos por
  dibujo con `glMultiDrawElementsIndirect` + `baseInstance` (el dibujo de display list no escribiría nada).
- Scripts de prueba propios en `r9/r11b/{pc,test,tools}` (copias de `r9/render`, usan su propia carpeta
  `r11b/pc/run`): `test/checks.sh` (FRAMECHECK), `test/sso_lvl11.sh` (1-1 separable + transform feedback),
  `test/ab_gpuxf.sh` (A/B), `test/rphase_base.sh` (RPHASE de la base, `pc/runs/base_map_rphase2`), `tools/shcache_count.py` (cuenta registros de una shadercache.bin).
- Para comparar en el mismo momento la fase "emit": `flock r9/render/pc/.runlock scripts/lim test/rphase_base.sh`.
- Caché de shaders comprobada en modo separable: plantilla de 82 programas → 85 registros tras el mapa, 85 claves
  = 85 fuentes distintas (los 3 nuevos son programas nuevos vistos en el juego, ningún duplicado).
