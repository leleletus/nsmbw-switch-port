# preparar_sd: prepara la tarjeta SD para NSMBWnx

NSMBWnx es un port nativo de **New Super Mario Bros. Wii** para Nintendo Switch
(homebrew). El archivo `NSMBWnx.nro` **solo contiene el código del port, no el juego**:
los gráficos, sonidos y niveles los lee de la tarjeta SD, y salen de **tu propia copia**
del disco.

`preparar_sd.py` toma tu copia (imagen `.iso` / `.wbfs` o una carpeta ya extraída),
comprueba que sea la versión correcta y deja en la SD la carpeta que el port necesita.

> **Hace falta una copia propia y legal del juego**, hecha de tu disco. Esta herramienta
> no descarga nada, no incluye ningún archivo del juego ni claves de Nintendo y no se
> conecta a internet.

## Qué juego vale

Solo **New Super Mario Bros. Wii de EE. UU.**, ID **SMNE01**, **revisión 1**.
El código del port se generó a partir de ese `main.dol` exacto, así que la herramienta
(y el propio NRO al arrancar) comprueban su SHA-256. Con la versión europea (SMNP01),
japonesa (SMNJ01) u otra revisión el juego fallaría, y la herramienta te lo dice.

Para ver la revisión en Dolphin: clic derecho en el juego → Propiedades → Info.

## Requisitos

- **Python 3.7 o más nuevo** (Windows, Linux o macOS). No hace falta instalar nada con pip.
  - Windows: instálalo desde python.org o la Microsoft Store y marca «Add python.exe to PATH».
- Tu juego en uno de estos formatos:
  - **Carpeta extraída** con Dolphin o wit: funciona siempre, sin nada más.
  - **Imagen `.iso` o `.wbfs`** (y otros formatos que entiendan las herramientas, como
    `.rvz`): necesita **wit** (Wiimms ISO Tools) o **DolphinTool** (viene con Dolphin)
    para leer el disco. La herramienta los busca sola en el PATH y en las carpetas
    habituales de instalación.
- Una Switch con **Atmosphère** y el **Homebrew Menu**, y unos **550 MB libres** en la SD
  (352 MB del juego + unos 150 MB del NRO + partidas y caché).

### Conseguir wit o DolphinTool (solo para imágenes)

- **wit**: https://wit.wiimm.de → descarga la versión para tu sistema (en Windows hay
  instalador; en Linux suele estar en el gestor de paquetes como `wit`; en macOS, en el
  paquete de la web).
- **DolphinTool**: viene dentro de Dolphin (https://dolphin-emu.org). En Windows es
  `DolphinTool.exe`, en la misma carpeta que `Dolphin.exe`.
- Si los tienes en otra carpeta: `--herramienta "RUTA\wit.exe"` o
  `--herramienta "RUTA\DolphinTool.exe"`.

### Sin wit ni DolphinTool: extraer con Dolphin

1. Abre Dolphin y añade la carpeta donde está tu juego.
2. Clic derecho en *New Super Mario Bros. Wii* → **Propiedades** → pestaña **Sistema de archivos**.
3. Clic derecho en el disco (primera línea) → **Extraer disco entero…**
   (o en **Partición de datos** → **Extraer partición completa…**).
4. Elige una carpeta vacía, por ejemplo `C:\NSMBW_extraido`.
5. Pasa esa carpeta a la herramienta (ver abajo). Da igual si `sys/` y `files/` quedaron
   dentro de una subcarpeta como `DATA`: la herramienta la encuentra.

## Uso en Windows (PowerShell)

Abre PowerShell en la carpeta donde está `preparar_sd.py` (en el Explorador:
Archivo → Abrir Windows PowerShell, o Mayús + clic derecho → «Abrir PowerShell aquí»).

Directo a la tarjeta SD (letra `E:`):

```powershell
python preparar_sd.py "C:\Juegos\New Super Mario Bros. Wii.wbfs" --sd E:
```

Desde una carpeta extraída con Dolphin:

```powershell
python preparar_sd.py "C:\NSMBW_extraido" --sd E:
```

Sin la SD a mano (deja el resultado en una carpeta para copiarla después):

```powershell
python preparar_sd.py "C:\Juegos\NSMBW.iso" --salida "C:\NSMBW_para_SD"
```

Si `python` no se reconoce, prueba con `py` en su lugar (`py preparar_sd.py ...`).

## Uso en Linux y macOS

```bash
python3 preparar_sd.py ~/Juegos/NSMBW.wbfs --sd /run/media/$USER/SD     # Linux
python3 preparar_sd.py ~/Juegos/NSMBW.wbfs --sd /Volumes/SD              # macOS
python3 preparar_sd.py ~/NSMBW_extraido --salida ~/NSMBW_para_SD         # a una carpeta
```

## Opciones

| Opción | Qué hace |
|---|---|
| `--sd LETRA_O_RUTA` | Copia directamente a `<SD>/switch/NSMBWnx/`. |
| `--salida DIR` | Deja el resultado en `DIR/switch/NSMBWnx/` (por defecto `NSMBWnx_SD` en la carpeta actual). |
| `--nro RUTA` | Copia también `NSMBWnx.nro` (si está junto a `preparar_sd.py`, lo copia solo). |
| `--herramienta RUTA` | Ruta de `wit(.exe)` o `DolphinTool(.exe)`. |
| `--comprobar` | Compara por SHA-256 también lo que ya estaba copiado (más lento; útil si sospechas de la SD). |
| `--autotest` | Pruebas internas con datos falsos diminutos (no necesita el juego). |

## Qué se copia y cuánto ocupa

```
SD:/
└── switch/
    └── NSMBWnx/
        ├── NSMBWnx.nro      ← el port (lo copias tú o con --nro)
        ├── data/
        │   ├── sys/         main.dol, boot.bin, bi2.bin, fst.bin, apploader.img (3,8 MB)
        │   └── files/       los 1184 archivos del disco (348 MB)
        ├── config.toml      ajustes (solo si no existía)
        └── save/            partidas guardadas (solo si no existía)
```

Solo se copian los archivos que lista el propio disco (`fst.bin`); lo demás que deja la
extracción (`disc/`, `ticket.bin`, `tmd.bin`, `cert.bin`, la partición de actualización…)
no hace falta. En total, unos **352 MB**. Al jugar, el NRO crea `cache/` (gráficos ya
preparados) y `logs/`.

La herramienta:

- comprueba el ID del disco (SMNE01), la revisión y el SHA-256 de `main.dol`, y que no
  falte ningún archivo ni tenga un tamaño distinto del que dice el disco;
- copia mostrando el progreso y luego **relee y compara cada archivo copiado** (SHA-256);
- se puede ejecutar las veces que quieras: lo que ya está bien no se vuelve a copiar, y si
  se cortó a medias, sigue donde lo dejó;
- **nunca borra nada tuyo**: no toca `save/` (ni `nand/` de versiones antiguas) ni tu
  `config.toml`; solo escribe dentro de `switch/NSMBWnx/data/` y, si no existían,
  `config.toml` y `save/`. Al leer una imagen usa una carpeta temporal que borra al acabar.

## Copiar el NRO y arrancar

1. Si no usaste `--nro`, copia `NSMBWnx.nro` a `SD:/switch/NSMBWnx/NSMBWnx.nro`
   (en la misma carpeta que `data/`).
2. Si usaste `--salida`, copia el **contenido** de esa carpeta (la carpeta `switch`) a la
   **raíz** de la SD; se fusiona con la `switch/` que ya tengas.
3. Pon la SD en la Switch y abre el Homebrew Menu **manteniendo pulsado R mientras abres
   cualquier juego instalado** («title takeover»). Desde el Álbum (modo applet) no hay
   memoria suficiente.
4. Elige **NSMBWnx**. La primera vez puede haber unos segundos de pantalla negra mientras
   se preparan los gráficos.

## Problemas frecuentes

- **«Este disco es New Super Mario Bros. Wii de Europa (SMNP01)»** (u otra región): el
  port solo funciona con el disco de EE. UU. No hay forma de convertirlo.
- **«El main.dol no es el de SMNE01 revisión 1»**: es otra revisión del disco de EE. UU.,
  o la extracción está dañada. Vuelve a extraer o volcar el disco.
- **«La copia del juego está incompleta o dañada»**: faltan archivos o tienen otro tamaño;
  vuelve a extraer el disco entero (no solo algunas carpetas).
- **«No encontré ninguna herramienta para leer imágenes»**: instala wit o Dolphin, o
  extrae el disco con Dolphin y pasa la carpeta (ver arriba).
- **«No encuentro la tarjeta SD en …»**: comprueba la letra en el Explorador (por ejemplo
  `E:`) o la ruta donde está montada en Linux/macOS.
- **«No hay espacio»**: libera espacio en la SD (hacen falta unos 352 MB para el juego).
- **«Tras copiar, no coinciden N archivos»**: la SD está llena, protegida contra escritura
  o dañada. Prueba con `--comprobar` o con otra tarjeta.
- **Tenía una versión anterior en `SD:/nsmbw/`**: la herramienta te avisa. Si quieres
  conservar las partidas, copia el contenido de `SD:/nsmbw/nand/` dentro de
  `SD:/switch/NSMBWnx/save/`. La carpeta antigua la puedes borrar tú cuando quieras.
- **En la Switch sale «No se encontraron los archivos del juego»**: falta
  `SD:/switch/NSMBWnx/data/sys/main.dol`, o el NRO no está en `SD:/switch/NSMBWnx/`.
- **Rutas largas en Windows**: la herramienta las admite; si aun así ves errores de ruta,
  usa una carpeta de salida corta, como `C:\NSMBW_para_SD`.
