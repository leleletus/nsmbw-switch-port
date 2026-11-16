# NSMBWnx: New Super Mario Bros. Wii nativo en Nintendo Switch

Versión de prueba **R6 (29-oct-2026)**. Es el juego recompilado a código de Switch, no un emulador.
Trae lo mismo que la versión de PC `NSMBW_r6`:
- multijugador de 2 a 4 jugadores con Joy-Con o Pro Controller;
- el arreglo del gráfico negro que salía a pantalla completa con la primera moneda o el primer Goomba;
- código más rápido.

> **Todavía no se ha probado en una Switch real.** En PC funciona sin cortes a ~59 fps.
> Si lo pruebas, apunta los fps y si funcionan la vibración, la inclinación y el applet de mandos.

> **Versión en desarrollo R8 (31-oct-2026, `nsmbw_r8_20261029.nro`)**: shaders precompilados al arrancar (sin
> tirones de 2 s por shader), memoria del host estable (~0,5 GB en vez de crecer hasta ~3 GB) y código generado
> más rápido. En el emulador citron va a 7-22 fps (7 en el mapa del Mundo 1): **todavía no llega a 60**, así que
> aún no es la versión para probar en la consola.

## 1. Cómo debe quedar la microSD

Todo en **una sola carpeta**, junto al NRO. Solo copias 2 cosas, el resto se crea solo:

```
SD:/switch/NSMBWnx/
├── NSMBWnx.nro     ← el port
├── cache/shadercache.bin  ← (desde R8) los gráficos ya preparados que vienen con el port: cópialo también
├── data/           ← TU disco NSMBW USA extraído
│   ├── sys/   (main.dol, boot.bin, bi2.bin, fst.bin, apploader.img)
│   └── files/ (Effect, Env, Layout, Stage, Sound, ...)
│
│   (se crean al abrirlo)
├── config.toml     ← configuración, comentada en español
├── save/           ← partidas guardadas (cópiala para tener respaldo)
├── cache/          ← gráficos ya preparados (se puede borrar; ver arriba)
└── logs/           ← solo si hay errores (nwii_err.log)
```

Si la carpeta no se llama así o el homebrew no recibe su ruta, el NRO busca `sdmc:/switch/NSMBWnx/` y luego la instalación antigua `sdmc:/nsmbw/`.
Los nombres viejos (`SMNE01/`, `nsmbw.toml`, `nand/`, `env.txt`) siguen funcionando.

**Juego necesario:** New Super Mario Bros. Wii **USA (SMNE01), revisión 1**. El NRO comprueba el `main.dol` al arrancar.
Para extraerlo en Linux:
```
wit extract --psel DATA "TU_JUEGO.wbfs" SMNE01
```
Después copia `SMNE01/sys` y `SMNE01/files` dentro de `data/`.

## 2. Abrirlo (importante: memoria)

**Mantén pulsado R mientras abres cualquier juego instalado.** Se abre el Homebrew Menu con toda la memoria (~3,2 GB).
Ahí elige **NSMBW Recomp**.
Desde el Álbum (modo applet) solo hay ~400 MB y no alcanza. El NRO avisa si es el caso.

Al arrancar hay unos segundos de pantalla negra mientras se preparan los gráficos de `cache/shadercache.bin`
(en el emulador, ~7-10 s; pasa en cada arranque). A cambio, el juego no se para a mitad de partida para
prepararlos. Si se borra `cache/`, el juego funciona igual, pero con pequeños tirones la primera vez que sale
cada efecto.

## 3. Controles (el juego ve un Wiimote de lado)

| Wii (de lado) | Pro / portátil / par de Joy-Con | Joy-Con suelto en horizontal |
|---|---|---|
| Moverse | Cruceta o stick izquierdo | Stick |
| 2 (saltar / aceptar) | A o B | Los dos botones de abajo/derecha |
| 1 (correr, bola de fuego / volver) | X o Y | Los dos botones de arriba/izquierda |
| A | L | SL |
| B | ZL | — |
| + (pausa) | + | + o − |
| − | − | Pulsar el stick |
| HOME del Wii | R3 (pulsar el stick derecho) | — |
| Sacudir (giro, levantar objetos) | R o ZR, o sacudir el mando | SR, o sacudir |
| Inclinar | Stick derecho o girar el mando como un volante | Girar el Joy-Con |

Cada mando tiene su propia vibración HD.

## 4. Multijugador (2 a 4)

1. Abre el juego con el mando del jugador 1 (o en modo portátil).
2. Abre el **applet de mandos de la Switch**: mantén **L3 + R3** (los dos sticks pulsados) medio segundo.
   También se abre solo en la pantalla "Añadir control" del juego (botón +).
3. En el applet, cada jugador pulsa **SL + SR** con su Joy-Con en horizontal (o **L + R** con un par o un Pro Controller).
4. Elige la partida. El juego pregunta cuántos juegan: cada jugador pulsa **2** para unirse y luego **Comenzar**.

Esto vale para el modo historia, la Caza monedas y el Modo libre.

Si el jugador 1 juega en **modo portátil**, al añadir jugadores el applet puede pedir quitar los Joy-Con de la consola.

## 5. Configuración (`config.toml`)

Se crea sola la primera vez y cada opción viene explicada. Para jugar no hace falta tocar nada.

| Opción | Para qué |
|---|---|
| `[system] language = "es"` | forzar idioma (`"en"`, `"fr"`); por defecto, el de la consola |
| `[input] joycon_mode = "single"` / `"pair"` | cada Joy-Con es un jugador / L+R juntos |
| `[input] players = 2` | máximo de jugadores |
| `[switch] motion = false` | sin sensor de movimiento (R/ZR siguen sacudiendo) |
| `[switch] tilt_invert = true` | inclinación al revés (desde m6r el valor por defecto, `false`, ya es el sentido correcto; antes hacía falta `true`) |
| `[switch] cpu_mhz = 1224` | desde m6r: CPU que el juego pide a sys-clk si está instalado (1020, 1122, 1224, 1326, 1428, 1581, 1683, 1785); `0` = no pedir nada |
| `[graphics] internal_resolution = "auto"` | resolución interna: `"auto"` = 1x (la de la Wii, 640x528) en portátil y 2x (1280x1056) en el dock (salida 1080p); `1` = la de la Wii (lo más ligero); `2`, `3`, `4` = más nítido y más trabajo de GPU. Un número aquí vale para los dos modos |
| `[graphics] internal_resolution_handheld = 2` | solo en modo portátil (viene comentada con `#`): por ejemplo `2` para probar más nitidez en portátil (más trabajo de GPU). Cambia en marcha al quitar o poner el dock |

Un `config.toml` creado por una versión anterior no tiene estas líneas: vale `"auto"`. Puedes añadirlas en la
sección `[graphics]` o borrar el archivo para que se cree de nuevo.

Para desarrolladores: `debug.toml` (registro de fps, etc.) está en [DEPURAR.md](DEPURAR.md).

## 6. Si algo falla

- Si faltan archivos del juego o el `main.dol` no es el correcto, el NRO lo dice en pantalla.
- Pásame `logs/nwii_err.log` (solo existe si hubo un error), y `logs/nwii.log` si usaste `debug.toml`.
- Si se cuelga: botón HOME → cerrar. Si sale un error de Atmosphère, el informe queda en `SD:/atmosphere/crash_reports/`.
