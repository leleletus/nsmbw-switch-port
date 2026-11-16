# New Super Mario Bros. Wii — port nativo para Nintendo Switch (homebrew)

Versión de prueba. NSMBW (EE. UU., **SMNE01**) recompilado a código nativo de
Switch (NWiiRecomp + libnx, OpenGL con Mesa, SDL2). No es un emulador: el
código del juego corre directamente en la CPU de la Switch.

**No incluye el juego.** Necesitas tu propio disco de NSMBW de EE. UU. y
extraerlo (paso 2).

## Estado actual (léelo antes de probar) — NRO R6 del 28-oct-2026

Este NRO lleva el mismo código que la versión de PC `NSMBW_r6` (ronda 6):
CPU "cpu4" + gráficos R4 + rendimiento R5 (fiesta de Peach) + **R6**:
**multijugador de 2 a 4 jugadores** con Joy-Con / Pro Controller, el arreglo del
gráfico negro distorsionado que salía un instante a pantalla completa, y
accesos a memoria más rápidos (el código del juego va un 15-30 % más rápido en
una CPU lenta).

**Todavía no se ha probado en una Switch real ni en citron.** La versión de PC
con el mismo código sí (pruebas del 27 y 28-oct, sin cortes ni cuelgues): 5 min sin mando
(correa, título, fiesta) a 58,8 fps de media; 6 min en el 1-1 a 59,2; **Caza monedas a
4 jugadores** a 59,5; **modo historia a 2 jugadores** (Mario + Luigi en el 1-2) a 59,6.

| Funciona (en PC; en Switch el mismo código) | Falta / sin probar |
|---|---|
| Arranca, lee el disco, idioma automático (español) | Velocidad en la Switch real sin medir (el NRO anterior iba a 1-3 fps en el título en citron) |
| Título, fiesta, mapa del Mundo 1, niveles | Inclinación y sacudida por movimiento, applet de mandos y vibración HD: revisados en el código, sin probar en consola |
| Multijugador 2-4 (historia, Caza monedas) | La primera vez, unos segundos de pantalla negra al arrancar (se compilan los shaders guardados) |
| Partidas guardadas (`sdmc:/nsmbw/nand`) | |

Si lo pruebas, cuéntanos los fps (`NWII_LOG=1`, ver sección 8).

## 1. Requisitos

- Una Switch con **Atmosphère** (CFW) y el Homebrew Menu (hbmenu).
- Una microSD con unos **500 MB libres** (≈340 MB del juego + ≈115 MB del NRO + partidas).
- Tu disco **New Super Mario Bros. Wii de EE. UU. (SMNE01), revisión 1**
  (el código del port se generó de ese `main.dol` exacto; el NRO lo comprueba
  al arrancar con su SHA-256 y avisa si no coincide).
  - En Dolphin: clic derecho en el juego → Propiedades → Info → "Revisión: 1".

## 2. Extraer tu juego (en el PC)

Hacen falta las carpetas `sys/` y `files/` de la partición de datos.

**Con Dolphin** (Windows, Linux, macOS):
1. Clic derecho en *New Super Mario Bros. Wii* → **Propiedades** → pestaña **Sistema de archivos**.
2. Clic derecho en **Partición de datos** → **Extraer partición completa...** (en inglés: *Extract Entire Partition...*)
3. Elige una carpeta vacía llamada `SMNE01`. Dentro quedarán `sys/` y `files/`.

**Con Wiimms ISO Tools** (`wit`):
```
wit extract "New Super Mario Bros. Wii.wbfs" SMNE01 --psel data
```

Comprueba que existe `SMNE01/sys/main.dol` y `SMNE01/files/`.

## 3. Copiar a la microSD

```
microSD:/
├── switch/
│   └── nsmbw.nro                ← de este paquete
└── nsmbw/                       ← de este paquete
    ├── nsmbw.toml               (configuración)
    ├── env.txt                  (opciones avanzadas, opcional)
    ├── README_ES.md             (este archivo)
    └── SMNE01/                  ← TU juego extraído
        ├── sys/   (main.dol, boot.bin, bi2.bin, fst.bin, apploader.img)
        └── files/ (Effect, Env, HomeButton2, Layout, Stage, Sound, ...)
```

Se crean solos al jugar: `nsmbw/nand/` (partidas), `nsmbw/shadercache.bin` (caché de shaders), `nsmbw/nwii_err.log`
(errores). Si borras `nsmbw.toml`, el NRO lo vuelve a crear.

## 4. Abrirlo: usa "title takeover" (memoria)

Abre el Homebrew Menu **manteniendo pulsado R mientras abres cualquier juego
instalado** (no desde el Álbum) y elige **NSMBW Recomp**.

¿Por qué? Memoria:

| Cómo se abre | Memoria para el homebrew | ¿Alcanza? |
|---|---|---|
| Title takeover (R + juego) | ~3,2 GB | Sí, de sobra |
| Modo applet (desde el Álbum) | ~400-440 MB según firmware | No: el port necesita más de 430 MB |

Medido en el emulador citron: **431 MB en la pantalla de título** (110 MB de
código recompilado + 321 MB de memoria dinámica: la memoria de la Wii emulada,
88 MB; la tabla de despacho, 24 MB; la pila del hilo de CPU, 32 MB; texturas y
el driver OpenGL), y los niveles necesitarán algo más. Si lo abres en modo
applet, el NRO muestra un aviso (A = intentar igual, + = salir).

## 5. Controles y multijugador (1-4 jugadores)

**Jugar con varios, en resumen:**
1. Abre el juego con el mando del jugador 1 (o en modo portátil).
2. Conecta los demás mandos con el **applet de mandos de la Switch**: ábrelo
   **manteniendo L3 + R3** (los dos sticks pulsados) medio segundo. También se abre
   solo en la pantalla de jugadores al pulsar **+ ("Añadir control")**.
3. En el applet, cada jugador pulsa **SL + SR** con su Joy-Con suelto en horizontal
   (o **L + R** con un par de Joy-Con / Pro Controller) y se confirma en el applet.
4. Elige la partida → el juego pregunta el número de jugadores → cada jugador pulsa
   **2** (A o B) para unirse y se pulsa **Comenzar**.

Se juega como con el Wiimote **de lado**. El juego ve **cuatro Wiimotes**:
jugador 1 = modo portátil o el mando n.º 1; jugador N = mando n.º N. Vale un
**Joy-Con suelto en horizontal** por jugador, un Pro Controller o un par de Joy-Con.

| Wii (de lado) | Pro / portátil / par de Joy-Con | Joy-Con suelto en horizontal |
|---|---|---|
| Cruceta | Cruceta o stick izquierdo | Stick |
| 2 (saltar / aceptar) | A o B | Los dos botones de abajo/derecha |
| 1 (correr, bola de fuego / volver) | X o Y | Los dos botones de arriba/izquierda |
| A | L | SL |
| B | ZL | — |
| + (pausa) | + | + o − |
| − | − | Pulsar el stick |
| HOME del Wii | R3 (pulsar el stick derecho; el HOME de la Switch es del sistema) | — |
| Sacudir (giro, levantar) | R o ZR, o sacudir el mando | SR, o sacudir el Joy-Con |
| Inclinar | Stick derecho o girar el mando como un volante | Girar el Joy-Con como un volante |

**Vibración HD** en el mando de cada jugador.

### Varios jugadores

- **Applet de mandos de la Switch** (elegir quién juega con qué y emparejar Joy-Con):
  se abre solo cuando el juego muestra su pantalla **"Añadir control"** y cuando un
  jugador pierde su mando. A mano: mantén **L3 + R3** medio segundo (con un Joy-Con
  suelto: **SL + SR + pulsar el stick**). En el applet, cada jugador pulsa **SL + SR**
  para usar un Joy-Con en horizontal, o **L + R** para un par / Pro Controller.
  Para que no se abra solo: `nx_controller_applet = false` en `[input]` de `nsmbw.toml`.
- **Modo historia:** con 2 o más mandos, al elegir el archivo el juego pregunta el
  número de jugadores; cada jugador pulsa **2** para unirse (Luigi, Toad amarillo,
  Toad azul) y se pulsa **Comenzar**.
- **Caza monedas** y **Modo libre**: desde la pantalla de elegir archivo, igual.
- **"Añadir control"** (botón **+** en la pantalla de jugadores): se abre el applet,
  conecta el mando nuevo y pulsa **+ (Aceptar)** para volver.

### Ajustes (`env.txt`), solo si hace falta

- `NWII_NX_TILT_INVERT=1`: inclinación por movimiento al revés.
- `NWII_NX_NO_MOTION=1`: sin sensor de movimiento (ZR/R siguen sacudiendo).
- `NWII_NX_SIX_ROTATE=0`: si la inclinación de un Joy-Con suelto va en el eje equivocado.
- `NWII_NX_JOYCON=single` (cada Joy-Con un jugador) o `pair` (L+R juntos). Por defecto:
  jugador 1 con un par, jugadores 2-4 con Joy-Con sueltos.

## 6. Idioma y partidas

- **Idioma automático**: el de la consola si el disco de EE. UU. lo trae
  (español, francés, inglés); cualquier otro → inglés. Para forzarlo, en
  `nsmbw.toml`: `language = "es"` (o `"en"`, `"fr"`).
- **Partidas**: `sdmc:/nsmbw/nand/` (es la NAND de la Wii emulada; cópiala para
  hacer copia de seguridad).
- Pantalla: 1280×720 en portátil y en TV (la consola la escala a 1080p),
  imagen 16:9 a pantalla completa.

## 7. Rendimiento

Este NRO todavía no se ha medido. En el PC, con 2 núcleos lentos limitados al 150 %
(lo más parecido a una Switch que se puede probar), la versión R6 va un 15-30 % más
rápida que la anterior. En el 1-1 llega a 57 fps de media con el PC en modo
"rendimiento" (55,7 la versión anterior), pero en modo ahorro de energía baja a ~32 fps
y aún no llega a 60 fps en el mapa ni en la fiesta de Peach (el hilo que dibuja tiene
que hacer demasiado trabajo; es lo siguiente a mejorar).

Medido con el NRO anterior (27-oct 04:00) en citron (emula la CPU de la Switch en un PC), con `NWII_LOG=1`:
- Correa y avisos: 30-50 fps.
- Pantalla de título (nivel 3D detrás): **1-3 fps**. El cuello de botella es la
  CPU del juego (el hilo de sonido y la lógica), no el dibujo (≈15-30 ms por
  imagen en la GPU emulada).
- Memoria: ≈520 MB en el título (110 MB de código + ≈400 MB dinámicos).

En la consola real puede ser más rápido: aún no está medido. El reloj del
juego es real, así que si no llega a 60 fps el juego va más lento.

**Caché de shaders** (`sdmc:/nsmbw/shadercache.bin`): guarda el código de cada
shader que el juego usa. Al arrancar se compilan todos los ya vistos (en
citron ≈1 s por shader, unos 20-60 shaders → pantalla negra al inicio), y así
no hay tirones después. El driver de la Switch no guarda binarios, por eso se
recompilan en cada arranque. Si molesta la espera: `NWII_SHADER_PREWARM=0` en
`env.txt`. Si el archivo se daña (p. ej. al cerrar con HOME mientras se
escribía), el NRO lo repara solo.

Opcional y experimental: `NWII_NX_CPUBOOST=1` en `env.txt` sube la CPU a
1785 MHz (la GPU baja mientras tanto). Un overclock con sys-clk también ayuda.

## 8. Si algo falla

- El NRO muestra un mensaje si faltan los archivos del juego o si el
  `main.dol` no es el correcto.
- Errores: `sdmc:/nsmbw/nwii_err.log` (máximo 1 MB).
- Registro completo: pon `NWII_LOG=1` en `env.txt` → `sdmc:/nsmbw/nwii.log`
  (más lento; incluye fps y memoria cada 5 s). Con `NWII_LOG=nxlink` sale por
  red al PC (`nxlink -s nsmbw.nro`).
- Si se cuelga: botón HOME → cerrar el programa. Si la Switch muestra una
  pantalla de error de Atmosphère, el informe queda en
  `sdmc:/atmosphere/crash_reports/`.

## 9. Desinstalar

Borra `sdmc:/switch/nsmbw.nro` y la carpeta `sdmc:/nsmbw/` (copia antes
`nsmbw/nand/` si quieres conservar las partidas).
