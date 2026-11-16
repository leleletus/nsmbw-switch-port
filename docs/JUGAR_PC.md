# Jugar en PC (Linux)

Este es el contenido de `LEEME.txt`, que va dentro del paquete `NSMBW-PC-rX.tar.zst`.
Para compilarlo tú mismo, ver [COMPILAR_PC_LINUX.md](COMPILAR_PC_LINUX.md).

```text
NEW SUPER MARIO BROS. Wii (port de PC para Linux, versión de prueba r6)
=======================================================================

Esto NO es un emulador: es el juego recompilado para correr directo en Linux.
Es una versión de PRUEBA: pueden salir fallos. Si ves alguno, tus reportes ayudan mucho.

LO QUE NECESITAS
- Tu propia copia de New Super Mario Bros. Wii, versión USA (código SMNE01),
  en formato .wbfs o .iso. Este paquete NO trae el juego.
- El programa "wit" para extraer el disco (una sola vez):
      sudo pacman -S wit
- Linux de 64 bits con OpenGL 4.x (probado en CachyOS con gráficos Intel Arc / Mesa).

CÓMO JUGAR
1. Descomprime la carpeta donde quieras.
2. Doble clic en "Jugar".
   La PRIMERA VEZ te pide elegir tu .wbfs/.iso y lo extrae (~1 minuto) en la
   carpeta "juego". Las siguientes veces abre el juego directamente.
   (Alternativa: copia el .wbfs/.iso dentro de "juego" antes de abrir. También
   puedes ejecutar ./jugar.sh desde una terminal.)
3. Con la partida nueva aparece "Se han creado datos..."; pulsa X (o A en el mando).

Tu partida se guarda en la carpeta "nand". Para empezar de cero, bórrala.
La primera vez puede haber pequeños tirones mientras se preparan los gráficos;
se guardan en shadercache.bin y desde la segunda vez ya no pasa.

CONTROLES (el juego cree que usas un Wiimote de lado)
  Teclado (jugador 1):
    Flechas ........ moverse
    X .............. 2 (saltar / aceptar)
    Z .............. 1 (correr, bola de fuego / volver)
    A / S .......... botones A / B del Wiimote
    Enter .......... + (pausa)        Retroceso .. -
    H .............. HOME
    C .............. agitar (giro en el aire, levantar objetos)
    Q / E .......... inclinar a la izquierda / derecha

  Mando (Xbox, PlayStation, 8BitDo, Pro Controller...):
    Cruceta o stick izquierdo ... moverse
    A o B ....................... saltar          X o Y ... correr
    LB / L ...................... A               LT / ZL . B
    Start / Select .............. + / -          Botón central ... HOME
    RB o RT ..................... agitar
    Stick derecho ............... inclinar (o el giroscopio si el mando tiene)

MULTIJUGADOR (2 a 4 jugadores)
  - Jugador 1 = teclado + el primer mando.
  - Cada mando que conectes es el siguiente jugador (2, 3, 4), también con el
    juego ya abierto.
  - Joy-Con por Bluetooth: un par L+R cuenta como un mando. Para que cada Joy-Con
    sea un jugador, abre nsmbw.toml y en [input] pon:   joycon_mode = "single"
  - Más opciones (2 jugadores en un teclado, reasignar botones) en nsmbw.toml.

SI VES UN FALLO
  - F12 = guarda los últimos ~2 segundos de imagen (púlsalo justo después del fallo).
  - F11 = guarda una captura de pantalla.
  Se guardan en la carpeta "capturas".
  - Después haz doble clic en "crear_reporte.sh" (o ejecútalo en terminal):
    crea reporte_nsmbw.tar.gz con los registros, las capturas y datos de tu PC
    (CPU, gráficos). NO incluye el juego ni tu partida. Envía ese archivo.
  - Cuéntame también: qué estabas haciendo, en qué nivel, y si iba lento (los FPS
    salen en el título de la ventana).

Archivos: NSMBW = el juego · Jugar = lanzador (solo ejecuta jugar.sh) ·
nsmbw.toml = configuración · logs/ = registros de la última partida.
```

## Resolución interna (`nsmbw.toml`)

Desde r7, en la sección `[graphics]` de `nsmbw.toml`:

```toml
[graphics]
internal_resolution = "auto"   # o 1, 2, 3, 4
```

| Valor | Efecto |
|---|---|
| `"auto"` (por defecto, también si falta la línea) | sigue el tamaño de la ventana: 720p → 2x, 1080p y 1440p → 3x, 4K → 4x |
| `1` | la resolución de la Wii (640x528), lo más ligero |
| `2`, `3`, `4` | fija: más nítido y más trabajo de la tarjeta gráfica |

Si el PC va justo, pon `1` o `2`. Las texturas del juego no cambian; se ven más limpios los bordes, el texto y
los efectos. Para pruebas, la variable `NWII_INTERNAL_RES=1..4` gana al toml (ver [DEPURAR.md](DEPURAR.md)).
