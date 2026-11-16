# R9 · medida de partida de r8 (31-oct-2026)

Referencia común de los frentes de R9: **mismos binarios de r8** (`bin/nsmbw_r8.nro` = `switch/bin/nsmbw_r8_20261029.nro`
y su `_input_test`), misma herramienta (`citron_scene.sh`), mismas escenas. Cada frente compara contra esta tabla
y, mejor aún, contra corridas de r8 alternadas con las suyas (A B B A).

## Resumen

| citron, portátil 1x, 300 s, hilos fijados (`PIN=1`) | **título** | **mapa del Mundo 1** | **1-1** |
|---|---|---|---|
| fps media (corrida A / B) | **17,7 / 16,6** | **14,5 / 15,0** | **16,2 / 15,6** |
| fps mín.–máx. de las ventanas de 5 s | 15,4–18,6 | 13,7–16,3 | 12,6–19,6 |
| hilo del juego (`CPUCore_1`) | 82–90 % | 90–91 % | 78–81 % (*) |
| hilo principal: render + GL (`CPUCore_0`) | 28–34 % | 69–76 % | 25–28 % |
| CPU del hilo del juego por frame | **50 ms** | **61 ms** | **50 ms** |
| CPU del hilo principal por frame | 18 ms | **49 ms** | 17 ms |
| `render_us/frame` (`[RPROF]`) | 13,1 ms | **41,7 ms** | 12,0 ms |
| dibujos GX / GL por frame | 2634 / 138 | **11 041 / 229** | ~2286 / 115 |
| líneas `[Gov]` en la escena | 1 / 1 (la de salida de la carga) | 1 / 1 | 1 / 1 |
| quién frena | **hilo del juego** | **hilo del juego, y el principal casi a la par** | **hilo del juego** |
| llega a la escena (desde que arranca citron) | 38 s | 60–66 s | 82–88 s |

(*) En el 1-1 el hilo del juego no llega al 95 % porque, al terminar un frame, espera al siguiente retrazado
vertical (VI): con frames de ~62 ms se pierde de media medio VI (~8 ms) por frame. Su trabajo real por frame es
~50 ms. El hilo principal está al 25-28 %: no es él.

Para 60 fps cada hilo tiene 16,7 ms por frame. **Falta ×3 en el hilo del juego en las tres escenas (×3,7 en el
mapa) y ×3 en el hilo principal en el mapa.** En el mapa, aunque el hilo del juego fuera infinitamente rápido, el
principal se quedaría en ~20 fps (49 ms por frame, de los que ~42 son `Render()`).

Escena extra que sale sola en las corridas del título: la **fiesta de Peach del modo demo** (escena MOVIE, a los 1000
frames sin tocar nada): 16-17 fps con el hilo principal al 82-86 % y el del juego al 77-85 %: ahí frena más el
render.

## Cómo se midió

```bash
~/nsmbw-port/r9/base/citron_scene.sh ~/nsmbw-port/r9/base/bin/nsmbw_r8.nro <title|map|lvl11> 300 runs/<escena>_pin_<a|b>
```

- citron en portátil y 1x (como está configurado `switch/citron-test/config`), perfil `performance` (vigilado
  durante toda la corrida), SD privada (respaldo y restauración de `switch/NSMBWnx`, salvo `data/`), partida con el
  Mundo 1 abierto (`citron/save_tpl`), caché de shaders de r8, capturas cada 600 frames.
- `fps` = líneas `[NX] game … fps` del NRO (cada 5 s). Se descartan las 2 primeras ventanas de la escena (carga).
- % de CPU por hilo = `/proc/<citron>/task/*/stat` cada 5 s, por nombre de hilo de citron; 100 % = un núcleo del PC.
  `CPUCore_0..3` son los 4 núcleos de la Switch emulada: en el 0 va el hilo principal del NRO (render, GX → GL y
  el controlador Mesa/nouveau, que es código del propio NRO) y en el 1 el hilo CPU del juego (código recompilado,
  AX/DSP, interrupciones). "CPU por frame" = % del hilo × 10 / fps (ms de CPU del PC por frame del juego): no depende
  de qué otro hilo frene ni del regulador.
- Las 6 corridas de la tabla están en `runs/{title,map,lvl11}_pin_{a,b}/` (`summary.txt`, `timeline.tsv`, `nwii.log`,
  `threads_raw.txt`, `shots/`).

### Hilos de citron que no son del juego

- **Emulación de audio de citron** (`audio`, `AudioRenderSyst`, `DSP_AudioRender`): ~150 % de CPU del PC en todas
  las escenas, en los E-cores. Es el servicio de audio de la consola emulada; en una Switch real lo hace el sistema,
  no el juego. No compite con los núcleos fijados del juego.
- `CPUCore_2`: ~40-46 % siempre, incluso en el arranque. No es código de NSMBW (r8 no pone nada suyo en el núcleo 2):
  probablemente el hilo de audio de SDL del NRO o un hilo del sistema. No se identificó.
- GPU emulada + Vulkan (`GPU`, `VulkanWorker`): 10-13 %. **La GPU no frena** en ninguna escena.

### Sin fijar los hilos (`PIN=0`, como el filtro de r8) los números no sirven para comparar

| sin fijar | título | mapa | 1-1 |
|---|---|---|---|
| fps media, corrida 1 / 2 | 17,2 / **9,2** | 9,7 / 7,4 | 9,3 / 8,1 |
| CPU del hilo del juego por frame | 52 / **101** ms | 97 / 128 ms | 101 / 118 ms |
| `[Gov]` en la escena | 2 / 2 | 1 / **24** | 1 / 1 |

(`runs/title_r8`, `title_r8_b`, `map_r8_try1`, `map_r8`, `lvl11_r8`, `lvl11_r8_b`.) La misma escena da de 9 a 17
fps y el hilo del juego gasta hasta el doble por frame: el planificador del PC lo pone en un P-core compartido con
otro hilo cargado (hermano de HT) o lo mueve a un E-core. Fijados, las dos corridas difieren un 3-7 %. Por eso
`citron_scene.sh` fija los hilos por defecto. El filtro final puede dar además el número sin fijar (más
conservador), pero **los A/B de los frentes se hacen fijados**.

Otras dos cosas que pasaron y que hay que vigilar:
- **Temperatura**: el portátil se pone a 92-96 °C a los pocos segundos de arrancar citron y se queda ahí; los
  P-cores bajan a ~2,8-3,3 GHz (máx. 4,5). Es igual en todas las corridas (el script espera a < 65 °C antes de
  empezar), pero conviene alternar A/B en vez de hacer todas las A seguidas.
- **El perfil de energía pasó solo a `power-saver`** a mitad de una corrida (la CPU a ~1,4 GHz, 3 fps; corrida
  descartada: `runs/INVALID_title_r8_b_powersaver`). No se sabe qué lo cambió. Ahora el muestreador lo comprueba cada
  5 s, lo vuelve a poner y deja un aviso en `summary.txt` ("WARNING: power profile left 'performance'").

## PC: perfil `perf` con CPU débil (perfil `sw`)

`pc/prof.sh ../bin/NSMBW_r8_input_test {map,lvl11}_sw {map,lvl11} 300 60 10`: sin ventana ni sonido, 1x, 2 núcleos
LP-E (16-17, ≤2,5 GHz) con cuota del 150 %, `perf record -F 1999` 60 s a partir de 10 s después de llegar a la escena.
Informes en `pc/prof/{map,lvl11}_sw.txt` (datos en `.data`).

| PC, perfil `sw` | mapa | 1-1 |
|---|---|---|
| fps (ventanas en la escena) | 59,1 | 59,7 |
| hilo principal (render) | 78 % de un núcleo LP-E = **13,2 ms/frame** | 38 % = 6,4 ms/frame |
| hilo CPU del juego | 53 % = **9,0 ms/frame** | 51 % = 8,5 ms/frame |
| `render_us/frame` | 9,1 ms | 4,7 ms |
| dibujos GX / GL por frame | 11 038 / 227 | 2722 / 118 |
| instrucciones del juego | 146 M/s | 152 M/s |
| reparto de muestras | principal 52 %, juego 37 %, hilo de Mesa 3 % | juego 51 %, principal 34 %, Mesa 5 % |

En el PC, con CPU débil, el mapa lo frena el **hilo principal** (78 %) y no el del juego; en citron es al revés
(el código del juego pasa por la JIT de citron y cuesta relativamente más).

### Hilo del juego (los dos casos se parecen)

| | mapa | 1-1 |
|---|---|---|
| código del juego (generado) | 65 % | 66 % |
| escritura al FIFO de GX (`wgp_write32`, `GX_WGPIPE_Write8/16`, `wgp_redirect`) | 11 % | 6 % |
| sondeo de interrupciones / base de tiempos (`process_pending_callbacks`, `clock_gettime`) | 8 % | 10 % |
| ayudas de memoria y paired-single (`psq_store`, `psq_load`, `MMU::write_f32_slow`) | 6,3 % | 4,9 % |
| AX/DSP (`ax::run_command_list`, `write_bus`) | 5,4 % | 6 % |

Funciones más caras (% del hilo, mapa / 1-1): `func_801A3880` (reverberación AXFX, código del juego) 9,9 / 10,3;
`process_pending_callbacks` 6,1 / 8,1; `wgp_write32` 6,0 / 2,9; `ax::run_command_list` 4,8 / 5,4;
`GX_WGPIPE_Write8` 3,2 / 1,8; `psq_store` 2,8 / 2,4; `psq_load` 2,0 / 1,6; `clock_gettime` 1,3 / 1,6;
`wgp_redirect` 1,2 / 0,8; luego código del juego repartido (<1 % cada una): `func_80250E70`, `func_802DCBFC`,
`func_801A2B10`, `PSMTXConcat`, `PSMTXCopy`, `func_801A5C10`, `func_802821E0`, `func_8024C960`, `func_80273560`,
`GXLoadTexObjPreLoaded`, `PSMTXIdentity`, `func_80082BE0`, `func_80119C50`.

**El audio pesa ~16-20 % del hilo del juego en el PC** (AXFX + mezclador AX + parte del sondeo) y es trabajo **por
segundo**, no por frame. En citron, a 15 fps, ese trabajo se reparte entre menos frames, así que parte de los
50-61 ms "por frame" del hilo del juego es audio fijo. Hipótesis para los frentes `threads`/`guestloop`: sacar el
audio del hilo del juego rinde en citron más que el 16-20 % del PC. Se puede medir con `NWII_*` del frente en
`EXTRA_DEBUG`.

### Hilo principal (render)

| | mapa | 1-1 |
|---|---|---|
| GX / renderer del runtime (incluye hash de texturas y copias EFB) | 79 % | 70 % |
| libc / libstdc++ (malloc/free, memcpy) | 11 % | 11 % |
| controlador GL (Mesa) | 5 % | 12 % |
| núcleo del SO | 1 % | 4 % |

Funciones más caras (mapa / 1-1): `gx::DrawPrimitive` **34,1 / 19,6**; `ProcessGXFifo` 10,5 / 8,2;
`RendererGL::Render` 7,2 / 8,4; `ExpandDisplayList` 6,1 / 4,9; `RangesHash` 3,3 / 1,2; `GetShaderHash` 2,9 / 3,6;
`DLContentHash` 2,3 / 1,4; `hle::hash_ram` 1,7 / **7,0**; `FifoParser::DecodeDraw` 1,6 / 1,6;
`TextureCache::efb_copy_done` 1,2 / 4,5; refcount de `shared_ptr` (`_M_release` + `operator=`) ~2,5 / 1,5;
`ParseStream` 1,4 / 2,3; `EmitIndices` 1,2 / 0,7; `SetupDrawState` 1,0 / 1,7; `vector::_M_range_insert` 1,0.
En el mapa el coste va casi todo con los 11 000 dibujos GX por frame (que ya se juntan en ~229 dibujos GL): el frente
`render` tiene que bajar el coste **por dibujo GX** (`DrawPrimitive`, `ProcessGXFifo`, `ExpandDisplayList`, hashes).

## Qué significa para cada frente

- `cpucode` y `guestloop`: el hilo del juego tiene que bajar de ~50-61 ms a ≤16,7 ms por frame en citron
  (×3-3,7). Lo más caro identificable: AXFX (`func_801A3880`), sondeo de interrupciones, escrituras al FIFO, ayudas
  paired-single. `[NX] … interp ~290-330/s` = entradas por segundo al intérprete (las 3 funciones de
  `[R8] resume_pruned`): mirar si cuestan en citron.
- `threads`: AX fuera del hilo del juego (el núcleo 2 de la Switch). Ver la nota del audio por segundo arriba.
- `render`: en el mapa el hilo principal necesita ×3 (49 → 16,7 ms; `Render()` ~42 ms con 11 041 dibujos GX).
  En título y 1-1 el principal va sobrado (17-18 ms/frame).
- Nadie tiene que mirar la GPU emulada, los shaders (0 nuevos en todas las corridas) ni la memoria (heap del host
  ~400-490 MB, estable).

## Cómo comparar un frente contra esto

1. Compilar el NRO del frente con su `build_nx.sh` (sale también `…_input_test.nro`).
2. Alternar r8 y el frente: `citron_scene.sh` con `bin/nsmbw_r8.nro` y con el NRO del frente, A B B A, en la misma
   escena (300 s). `EXTRA_DEBUG=<archivo>` para las claves `NWII_*` del frente.
3. Comparar `fps (5 s windows): mean` y, sobre todo, `CPUCore_1 per game frame` / `CPUCore_0 per game frame`
   (no se satura ni depende del regulador). Con los hilos fijados, dos corridas iguales difieren un 3-7 %: una
   mejora menor que eso no se puede afirmar con 2 corridas.
4. Mirar `WARNING` en `summary.txt` (perfil de energía, huecos del muestreador = la guardia de batería congeló el
   slice): si aparece, repetir.

## Opinión: ¿cuánto se parece citron a una Switch real?

Sin medida en la consola no se puede afirmar; lo que sí se ve:
- citron ejecuta el código ARM del NRO con una JIT (dynarmic) en los núcleos del PC a la velocidad que dé el PC, sin
  imitar los 1020 MHz de la Switch. Con los hilos fijados en P-cores a ~3,2 GHz, el hilo del juego gasta 50-61 ms por
  frame; en el PC nativo con 2 núcleos LP-E (Crestmont ≤2,5 GHz) gasta 8,5-9 ms por frame: **citron es ~6-7 veces más
  lento que el PC débil en el hilo del juego** y ~2,6-3,7 veces en el principal.
- Un A57 a 1020 MHz es, a ojo, unas 4-5 veces más lento que un núcleo LP-E a 2,5 GHz (la mitad de reloj y menos
  instrucciones por ciclo). Si fuera así, la Switch real gastaría ~35-45 ms por frame en el hilo del juego: algo
  mejor que citron (50-61 ms), pero del mismo orden. Es una estimación gruesa; la única forma de saberlo es medir
  r8/r9 en la consola real cuando citron se acerque a 60.
- citron añade un coste que la consola no tiene en la CPU del PC (emulación de audio ~150 %, servicios), pero va en
  otros núcleos del PC y no frena al juego mientras los hilos estén fijados.
