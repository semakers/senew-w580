# senew-w580 — an open operating system for the Sony Ericsson W580i

*Español más abajo — [ir a la versión en español](#español).*

> [!WARNING]
> **senew is a new project, in beta. Use it at your own risk.** Installing senew **replaces the phone's software**.
> It is installed with **SENUS** (<https://github.com/semakers/senus>), which makes a backup of the phone first; keep that
> backup. Try senew in the emulator before putting it on a phone. Nobody can promise that it will work on yours.

**senew** is a complete operating system written from scratch for the **W580i** (DB2020 system-on-chip, ARM926EJ-S core,
64 MiB NOR flash, 240x320 screen): its own bootloader, drivers, a graphical shell with several menu styles, a file system,
USB (MTP and a management channel for SENUS), Bluetooth, and a loader for third-party apps (`.sen` files). This repository
is the system: everything needed to build it, plus the tools to package it and to try it in an emulator.
Website: <https://semakers.com>.

| Repository | What |
|---|---|
| **senew-w580** (this one) | The operating system, its bootloader and its build tools (GPL-3.0) |
| [senus](https://github.com/semakers/senus) | The desktop app (Windows, macOS) that installs and updates senew, and uploads apps |
| [senew-sdk](https://github.com/semakers/senew-sdk) | Everything to write your own `.sen` apps (MIT) |
| [senew-emu](https://github.com/semakers/senew-emu) | The QEMU machine model of the W580i, to run senew without a phone |
| [`senew-app-*`](https://github.com/semakers?tab=repositories&q=senew-app) | The apps (calculator, notes, clock, camera, music…), one repository each |

## Status

* Version **0.30.3** (build 303). It boots and runs on real W580i phones and in the emulator. It is young: expect rough edges.
* The binary that SENUS installs today (0.30.3) was built from the development tree and still has a few personal shortcuts in
  its "Escritorio" menu style. **The next release will be built from this tree.**
* What works, in short: menus (grid, 3D, media bar, desktop), files (own read-only FS, the user "Memoria" with folders, the
  memory card with FAT read/write), settings, clock and alarm, lights, accelerometer and step counter, USB (MTP + SENUS
  channel), Bluetooth (pairing, serial port profile, the network bridge through SENUS), and the `.sen` app loader with memory
  protection.

### What this public tree does not include (and what you lose)

senew reads some data from the phone's **own factory file system** at run time, and in the development tree a few more
tables are **generated from files of the original firmware**. That material belongs to the manufacturer and **is not in this
repository**. In its place there are empty stand-ins, so that the system builds and boots:

| Missing here | Stand-in | Effect | Can you make your own? |
|---|---|---|---|
| LCD panel initialisation scripts and PDI microcode (`hal/lcd_paneles.h`) | `hal/lcd_paneles_stub.h` | **Works in the emulator; on a real phone the screen stays dark.** Do not flash such a build. | **Yes**: `make paneles DISPDRIVER=/path/dispdriver0.dat` — the file is `/ifs/settings/display/dispdriver0.dat` of *your* phone's factory file system |
| Camera sensor start-up table (`hal/camara_w580_tab.c`) | empty (`hal/fabricante_ausente.c`) | the camera does not start (`cam_iniciar` fails) | **Yes**: `make camara-tabla CAMDRIVER=/path/camdriver0.dat` (`/ifs/settings/camera/camdriver0.dat`) |
| DSP code images (`hal/dsp_w580_img*.c`) | empty | no MP3 playback through the DSP, no microphone (AAC+ module), "DSP diagnostics" fails | not yet: the extractor is not published |
| MP3 player message templates (`hal/mp3_w580_dat.c`) | empty | (only used with the DSP images) | no: they come from traces of the original firmware |
| Spare copies of the acoustic files (`hal/mp3_w580_apfdat.c`) | empty | none: senew reads them from the phone (`/ifs/settings/acoustic/`) | — |
| Pre-rendered artwork of the "Escritorio" menu style (`shell/escritorio_rec.c`) | empty | that style is drawn entirely by code | — |
| The manufacturer's step-counting algorithm | replaced by a simple threshold counter of our own (`shell/podometro.c`) | counts less accurately, no "power saving" mode (the sensor never sleeps); the Movement app works | **Pending a decision**: the version of the published 0.30.3 binary is not distributed here (it was derived from the original firmware's code); a better detector of our own can replace this one |
| BTstack, the Bluetooth stack (BlueKitchen) | fetched by `herr/traer_btstack.sh` | without it, build with `BT=0` (no Bluetooth, no network) | — |

The generated files are listed in `.gitignore`: they stay on your computer. The tools that generate them read only the file you
give them; the output of `herr/hacer_paneles.py` and `herr/camara/gen_tabla_c.py` was checked to be identical, byte for byte,
to the tables of the development tree. Everything else (MIDI and tone sounds, the system font, icons, the UI) is senew's own.

## Build

Tested on **macOS 26 (Apple silicon)** with:

* **Arm GNU Toolchain 15.2.Rel1** (`arm-none-eabi-gcc 15.2.1 20251203`), from
  <https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads> — on Linux, your distribution's `gcc-arm-none-eabi`
  (a recent one) or the same Arm release for x86_64/aarch64;
* **GNU make 4.x** (macOS ships 3.81: `brew install make` and put `$(brew --prefix make)/libexec/gnubin` first in `PATH`);
* **Python 3.8+** (3.9.6 tried), standard library only;
* **git** (once, to fetch BTstack).

```sh
git clone https://github.com/semakers/senew-w580 && cd senew-w580
sh herr/traer_btstack.sh          # once: BTstack at the tested commit + senew's patch, into bt/terceros/btstack
make -j3 w580                      # -> build/sistema.bin (1 046 616 bytes) and build/flash-w580.bin (64 MiB, emulator only)
make bl                            # -> build/bl/bl.bin, the bootloader (5 912 bytes)
make bl-installer                  # -> build/installer/bl/bl.bin, the bootloader in installer mode (10 908 bytes)
```

Other targets: `make w580 BT=0` (no Bluetooth, no BTstack needed), `make sin-escritura` (a build with no NOR write routines at
all, checked on the ELF), `make w580 DEFS=-DHITO=3 B=build/hito_3` (minimal boot milestones, `plat/hitos_w580.c`),
`make probar-rapido` (host-side tests: board descriptions, the flash store with power cuts, USB core against a simulated host and
a register-level UDC model, NOR driver against a chip model, RTC, I²C, charging, Memory Stick protocol; a few minutes),
`make pc` (the whole system on the host with ASan/UBSan, driven by scripts in `herr/guiones/`; **Linux only**: it uses ELF
inline assembly; `probar-fs` and `probar-monticulo` too), `make clean`.
During the build you will see `AVISO: sin hal/lcd_paneles.h…` until you generate your own panel table (see above).

`build/flash-w580.bin` is a whole 64 MiB NOR image **for the emulator only** (`-bios`): it has `0xFF` where a phone has its boot
loader and factory data. **Never write it to a phone.** Phones are installed and updated with SENUS.

## Install on a phone

With **SENUS** (<https://github.com/semakers/senus/releases>). The first installation goes through the phone's service mode
("anyCID" packages + a small trampoline that starts senew's bootloader in installer mode); SENUS backs the phone up first. After
that, updates go through senew's own bootloader over USB, without service mode. SENUS also uploads `.sen` apps.

## Try it in the emulator

You need the emulator from [senew-emu](https://github.com/semakers/senew-emu) (`scripts/build.sh` → `build/qemu-system-arm`).

```sh
make emu QEMU=../senew-emu/build/qemu-system-arm          # boots build/flash-w580.bin, dismisses the power menu,
                                                          # saves build/pantalla.png
```

`herr/emu.py` drives one emulator instance through QMP (Python standard library only):

```sh
python3 herr/emu.py arrancar --qemu ../senew-emu/build/qemu-system-arm --flash build/flash-w580.bin
python3 herr/emu.py tecla sel_der        # wait ~15 s after starting: dismiss the power menu (right soft key)
python3 herr/emu.py tecla sel_izq        # "Menú"
python3 herr/emu.py tecla abajo 2        # keys: arriba abajo izq der ok sel_izq sel_der atras c 0-9 * # (or up down left right back…)
python3 herr/emu.py captura screen.png   # 240x320 PNG
python3 herr/emu.py parar
```

### Trying an app in the emulator

Build the app with the SDK, then make a flash image that has it in the user's Memoria (`/apps`), as if it had been uploaded:

```sh
git clone https://github.com/semakers/senew-sdk ../senew-sdk
git clone https://github.com/semakers/senew-app-serpiente ../senew-app-serpiente
make -C ../senew-app-serpiente SENEW_SDK=../senew-sdk                 # -> ../senew-app-serpiente/build/serpiente.sen
make flash-apps SEN="../senew-app-serpiente/build/serpiente.sen"      # -> build/flash-w580-apps.bin
make emu QEMU=../senew-emu/build/qemu-system-arm FLASH=build/flash-w580-apps.bin
```

Then, with `emu.py`: `tecla sel_der`, `tecla sel_izq` (menu), move to the app's cell (here, "Serpiente" appears at the end of the
grid) and `tecla ok`. Several apps: `SEN="a.sen b.sen"`.

## Architecture in brief

```
apps/    built-in apps: Settings, Files, System (diagnostics), Receive
shell/   the shell: menus and their styles, status bar, standby, music service, network sessions, the .sen loader (cargador.c,
         cg_*.c: one file per API group, every pointer and size coming from an app is checked)
ui/      the UI framework: widgets, layout, partial repaint, text (Roboto), two design systems (Material 3 and classic)
api/     the portable core between ui/shell and the hardware (hw.h), file systems (own read-only W6FS, the "Memoria" W6F with
         power-cut safety, FAT for the card, the factory FS reader), USB core + MTP + SENUS management channel, the app contract
         (cargable*.h: the same as include/ in senew-sdk)
hal/     drivers for the DB2020: boot, clocks, interrupts, timer, keypad, LCD/PDI, I²C + PMU, NOR (Intel M18), RTC, UART, USB
         device controller, Memory Stick, camera, DSP
plat/    the W580 platform glue (plat/hw_w580.c) and the PC platform (plat/hw_pc.c…), plat/bl/: senew's own bootloader
placas/  board descriptions (screen size, keys, flash zones); bt/: senew's Bluetooth service, chipset driver and network protocol
r3d/     a small fixed-point 3D rasteriser for the 3D menu icons      img/: PNG and JPEG decoders      herr/: build and test tools
```

* **Flash layout**: bootloader at `0x44140000` (one 256 KiB block; the phone's boot ROM always enters there), senew's main at
  `0x44180000`–`0x443BFFFF`, senew's read-only file system at `0x44400000`, the user Memoria from `0x44500000` (22 MiB).
  The manufacturer's factory file system and calibration data stay untouched where they are; senew only reads them.
* **Apps (`.sen`)** are position-independent ARM files with relocations, loaded into their own memory and run in user mode with
  the MMU protecting the system; they call the system only through a numbered function table. See
  [senew-sdk](https://github.com/semakers/senew-sdk).
* **Network**: the phone has no data connection of its own in senew. An app opens a "network" channel; senew carries it over
  Bluetooth (serial port profile) to SENUS on the computer, which talks to the app's own server (W5RED protocol, `bt/red/`).
* More: [`docs/en/senew.md`](docs/en/senew.md) (Spanish: [`docs/es/senew.md`](docs/es/senew.md)). Comments in the sources are in
  Spanish, the project's working language.

## License

senew is **GPL-3.0-or-later** — see [`LICENSE`](LICENSE). Third parties and their licenses are listed in [`NOTICE`](NOTICE):
the Roboto font (SIL OFL 1.1) and Material Symbols (Apache-2.0), rasterised into `ui/`; **BTstack** (BlueKitchen GmbH), which is
**not** distributed here and has its own license (BSD-style **with a non-commercial clause**). Because senew is linked with BTstack
when built with Bluetooth, `NOTICE` adds a GPL-3.0 section 7 permission to do so *(pending the author's confirmation)*.
Apps (`.sen`) talk to the system only through the function table, so they can use any license.
Sony Ericsson, W580i, Walkman and other names are trademarks of their respective owners and are used only to say which hardware this
is about; this project is not affiliated with them and contains no software of theirs.

---

<a id="español"></a>

# senew-w580 — un sistema operativo abierto para el Sony Ericsson W580i

> [!WARNING]
> **senew es un proyecto nuevo, en beta. Úsalo bajo tu propio riesgo.** Instalar senew **sustituye el software del
> teléfono**. Se instala con **SENUS** (<https://github.com/semakers/senus>), que antes hace una copia de seguridad del
> teléfono: guárdala. Prueba senew en el emulador antes de ponerlo en un teléfono. Nadie puede prometer que funcione en el tuyo.

**senew** es un sistema operativo completo, escrito desde cero para el **W580i** (chip DB2020, núcleo ARM926EJ-S, 64 MiB de
NOR, pantalla de 240x320): su propio bootloader, los drivers, un shell gráfico con varios estilos de menú, sistema de ficheros,
USB (MTP y un canal de gestión para SENUS), Bluetooth y un cargador de apps de terceros (ficheros `.sen`). Este repositorio es el
sistema: todo lo necesario para compilarlo, y las herramientas para empaquetarlo y probarlo en un emulador.
Web: <https://semakers.com>.

| Repositorio | Qué es |
|---|---|
| **senew-w580** (este) | El sistema operativo, su bootloader y sus herramientas (GPL-3.0) |
| [senus](https://github.com/semakers/senus) | La app de escritorio (Windows, macOS) que instala y actualiza senew y sube apps |
| [senew-sdk](https://github.com/semakers/senew-sdk) | Todo para escribir tus propias apps `.sen` (MIT) |
| [senew-emu](https://github.com/semakers/senew-emu) | El modelo QEMU del W580i, para usar senew sin teléfono |
| [`senew-app-*`](https://github.com/semakers?tab=repositories&q=senew-app) | Las apps (calculadora, notas, reloj, cámara, música…), un repositorio cada una |

## Estado

* Versión **0.30.3** (build 303). Arranca y funciona en teléfonos W580i reales y en el emulador. Es joven: habrá asperezas.
* El binario que instala hoy SENUS (0.30.3) se compiló desde el árbol de desarrollo y aún lleva algunos accesos personales en su
  estilo de menú «Escritorio». **La próxima versión saldrá de este árbol.**
* Lo que funciona, en corto: menús (rejilla, 3D, barra de medios, escritorio), archivos (FS propio de solo lectura, la «Memoria»
  del usuario con carpetas, la tarjeta con FAT de lectura y escritura), ajustes, reloj y alarma, luces, acelerómetro y podómetro,
  USB (MTP + canal de SENUS), Bluetooth (emparejar, perfil de puerto serie, el puente de red a través de SENUS) y el cargador de
  apps `.sen` con protección de memoria.

### Lo que NO trae este árbol público (y lo que se pierde)

senew lee algunos datos del **propio FS de fábrica del teléfono** mientras funciona, y en el árbol de desarrollo hay además unas
tablas **generadas a partir de ficheros del firmware original**. Ese material es del fabricante y **no está en este
repositorio**. En su lugar hay sustitutos vacíos, para que el sistema compile y arranque:

| Lo que falta | Sustituto | Efecto | ¿Puedes hacer el tuyo? |
|---|---|---|---|
| Guiones de inicio de los paneles y microcódigo del PDI (`hal/lcd_paneles.h`) | `hal/lcd_paneles_stub.h` | **En el emulador funciona; en un teléfono real la pantalla se queda apagada.** No grabes un senew así. | **Sí**: `make paneles DISPDRIVER=/ruta/dispdriver0.dat` — el fichero es `/ifs/settings/display/dispdriver0.dat` del FS de fábrica de *tu* teléfono |
| Tabla de arranque del sensor de la cámara (`hal/camara_w580_tab.c`) | vacía (`hal/fabricante_ausente.c`) | la cámara no arranca (`cam_iniciar` falla) | **Sí**: `make camara-tabla CAMDRIVER=/ruta/camdriver0.dat` (`/ifs/settings/camera/camdriver0.dat`) |
| Imágenes de código del DSP (`hal/dsp_w580_img*.c`) | vacías | sin MP3 por el DSP, sin micrófono (módulo AAC+); «Diagnóstico DSP» falla | todavía no: el extractor no está publicado |
| Plantillas de mensajes del reproductor MP3 (`hal/mp3_w580_dat.c`) | vacías | (solo se usan con las imágenes del DSP) | no: salen de trazas del firmware original |
| Copias de reserva de los ficheros acústicos (`hal/mp3_w580_apfdat.c`) | vacías | ninguno: senew los lee del teléfono (`/ifs/settings/acoustic/`) | — |
| Dibujos pre-renderizados del estilo de menú «Escritorio» (`shell/escritorio_rec.c`) | vacío | ese estilo se dibuja entero con código | — |
| El algoritmo de pasos del fabricante | sustituido por un contador por umbral propio y sencillo (`shell/podometro.c`) | cuenta con menos exactitud y no tiene «ahorro en reposo» (el sensor no duerme); la app Movimiento funciona | **Pendiente de decisión**: la versión del binario 0.30.3 publicado no se distribuye aquí (derivaba del código del firmware original); un detector propio mejor puede sustituir a este |
| BTstack, la pila Bluetooth (BlueKitchen) | la trae `herr/traer_btstack.sh` | sin ella, compila con `BT=0` (sin Bluetooth ni red) | — |

Los ficheros generados están en `.gitignore`: se quedan en tu ordenador. Las herramientas que los generan solo leen el fichero
que les das; se comprobó que la salida de `herr/hacer_paneles.py` y de `herr/camara/gen_tabla_c.py` es idéntica, byte a byte, a
las tablas del árbol de desarrollo. Todo lo demás (sonidos MIDI y tonos, la letra del sistema, iconos, la interfaz) es de senew.

## Compilar

Probado en **macOS 26 (Apple silicon)** con:

* **Arm GNU Toolchain 15.2.Rel1** (`arm-none-eabi-gcc 15.2.1 20251203`), de
  <https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads> — en Linux, el `gcc-arm-none-eabi` de tu distribución
  (uno reciente) o la misma versión de Arm para x86_64/aarch64;
* **GNU make 4.x** (macOS trae la 3.81: `brew install make` y pon `$(brew --prefix make)/libexec/gnubin` delante en el `PATH`);
* **Python 3.8+** (probado con 3.9.6), solo la biblioteca estándar;
* **git** (una vez, para traer BTstack).

```sh
git clone https://github.com/semakers/senew-w580 && cd senew-w580
sh herr/traer_btstack.sh          # una vez: BTstack en el commit probado + el parche de senew, en bt/terceros/btstack
make -j3 w580                      # -> build/sistema.bin (1 046 616 bytes) y build/flash-w580.bin (64 MiB, solo emulador)
make bl                            # -> build/bl/bl.bin, el bootloader (5 912 bytes)
make bl-installer                  # -> build/installer/bl/bl.bin, el bootloader en modo instalador (10 908 bytes)
```

Otros objetivos: `make w580 BT=0` (sin Bluetooth: no hace falta BTstack), `make sin-escritura` (un binario SIN ninguna rutina que
escriba la NOR, comprobado sobre el ELF), `make w580 DEFS=-DHITO=3 B=build/hito_3` (los hitos mínimos de arranque,
`plat/hitos_w580.c`), `make probar-rapido` (pruebas en el ordenador: descripciones de placa, el almacén con cortes de corriente, el
núcleo USB contra un anfitrión simulado y un modelo del UDC a nivel de registro, el driver de la NOR contra un modelo del chip, RTC,
I²C, carga, protocolo de Memory Stick; unos minutos), `make pc` (el sistema entero en el ordenador con ASan/UBSan, manejado con los
guiones de `herr/guiones/`; **solo Linux**: usa ensamblador en línea de ELF; también `probar-fs` y `probar-monticulo`), `make clean`.
Al compilar verás `AVISO: sin hal/lcd_paneles.h…` hasta que generes tu tabla de la pantalla (ver arriba).

`build/flash-w580.bin` es una NOR entera de 64 MiB **solo para el emulador** (`-bios`): tiene `0xFF` donde un teléfono tiene su
cargador y sus datos de fábrica. **No la grabes nunca en un teléfono.** Los teléfonos se instalan y actualizan con SENUS.

## Instalar en un teléfono

Con **SENUS** (<https://github.com/semakers/senus/releases>). La primera instalación pasa por el modo de servicio del teléfono
(paquetes «anyCID» + un pequeño trampolín que arranca el bootloader de senew en modo instalador); SENUS hace antes la copia de
seguridad. Después, las actualizaciones van por el bootloader propio de senew por USB, sin modo de servicio. SENUS también sube
apps `.sen`.

## Probarlo en el emulador

Hace falta el emulador de [senew-emu](https://github.com/semakers/senew-emu) (`scripts/build.sh` → `build/qemu-system-arm`).

```sh
make emu QEMU=../senew-emu/build/qemu-system-arm          # arranca build/flash-w580.bin, quita el menú de encendido
                                                          # y guarda build/pantalla.png
```

`herr/emu.py` maneja una instancia del emulador por QMP (solo la biblioteca estándar de Python):

```sh
python3 herr/emu.py arrancar --qemu ../senew-emu/build/qemu-system-arm --flash build/flash-w580.bin
python3 herr/emu.py tecla sel_der        # unos 15 s después de arrancar: quita el menú de encendido (tecla de selección derecha)
python3 herr/emu.py tecla sel_izq        # «Menú»
python3 herr/emu.py tecla abajo 2        # teclas: arriba abajo izq der ok sel_izq sel_der atras c 0-9 * #
python3 herr/emu.py captura pantalla.png # PNG de 240x320
python3 herr/emu.py parar
```

### Probar una app en el emulador

Compila la app con el SDK y haz una imagen de flash que la lleve en la Memoria del usuario (`/apps`), como si la hubieras subido:

```sh
git clone https://github.com/semakers/senew-sdk ../senew-sdk
git clone https://github.com/semakers/senew-app-serpiente ../senew-app-serpiente
make -C ../senew-app-serpiente SENEW_SDK=../senew-sdk                 # -> ../senew-app-serpiente/build/serpiente.sen
make flash-apps SEN="../senew-app-serpiente/build/serpiente.sen"      # -> build/flash-w580-apps.bin
make emu QEMU=../senew-emu/build/qemu-system-arm FLASH=build/flash-w580-apps.bin
```

Después, con `emu.py`: `tecla sel_der`, `tecla sel_izq` (el menú), ve a la celda de la app (aquí «Serpiente» sale al final de la
rejilla) y `tecla ok`. Varias apps: `SEN="a.sen b.sen"`.

## Arquitectura en breve

```
apps/    apps de serie: Ajustes, Archivos, Sistema (diagnósticos), Recibir
shell/   el shell: los menús y sus estilos, barra de estado, espera, servicio de música, sesiones de red, el cargador de .sen
         (cargador.c, cg_*.c: un fichero por grupo de la API; todo puntero y tamaño que llega de una app se comprueba)
ui/      el framework de interfaz: widgets, disposición, repintado parcial, texto (Roboto), dos sistemas de diseño (Material 3 y clásico)
api/     el núcleo portable entre ui/shell y el hardware (hw.h), los sistemas de ficheros (W6FS propio de solo lectura, la «Memoria»
         W6F a prueba de cortes de corriente, FAT para la tarjeta, el lector del FS de fábrica), núcleo USB + MTP + canal de gestión
         de SENUS, el contrato de las apps (cargable*.h: lo mismo que include/ en senew-sdk)
hal/     drivers del DB2020: arranque, relojes, interrupciones, tic, teclado, LCD/PDI, I²C + PMU, NOR (Intel M18), RTC, UART,
         controlador USB de dispositivo, Memory Stick, cámara, DSP
plat/    la plataforma W580 (plat/hw_w580.c) y la del PC (plat/hw_pc.c…); plat/bl/: el bootloader propio de senew
placas/  descripciones de placa (pantalla, teclas, zonas de la flash); bt/: el servicio Bluetooth de senew, el driver del chip y el
         protocolo de red
r3d/     un rasterizador 3D pequeño en coma fija para los iconos del menú 3D     img/: decodificadores PNG y JPEG     herr/: herramientas
```

* **La flash**: el bootloader en `0x44140000` (un bloque de 256 KiB; la ROM de arranque del teléfono entra siempre ahí), el main de
  senew en `0x44180000`–`0x443BFFFF`, el FS de solo lectura de senew en `0x44400000`, la Memoria del usuario desde `0x44500000`
  (22 MiB). El FS de fábrica y los datos de calibración del fabricante se quedan donde están, sin tocar; senew solo los lee.
* **Las apps (`.sen`)** son ficheros ARM independientes de la posición, con reubicaciones; se cargan en su propia memoria y corren en
  modo usuario, con la MMU protegiendo el sistema; solo llaman al sistema por una tabla numerada de funciones. Ver
  [senew-sdk](https://github.com/semakers/senew-sdk).
* **La red**: en senew el teléfono no tiene conexión de datos propia. Una app abre un canal de «red»; senew lo lleva por Bluetooth
  (perfil de puerto serie) hasta SENUS en el ordenador, que habla con el servidor propio de la app (protocolo W5RED, `bt/red/`).
* Más: [`docs/es/senew.md`](docs/es/senew.md) (inglés: [`docs/en/senew.md`](docs/en/senew.md)). Los comentarios del código están en
  español, la lengua de trabajo del proyecto.

## Licencia

senew es **GPL-3.0-or-later** — ver [`LICENSE`](LICENSE). Los terceros y sus licencias están en [`NOTICE`](NOTICE): la letra Roboto
(SIL OFL 1.1) y Material Symbols (Apache-2.0), rasterizadas en `ui/`; **BTstack** (BlueKitchen GmbH), que **no** se distribuye aquí y
tiene su propia licencia (tipo BSD **con una cláusula de uso no comercial**). Como senew se enlaza con BTstack cuando se compila con
Bluetooth, `NOTICE` añade un permiso de la sección 7 de la GPL-3.0 para hacerlo *(pendiente de confirmar por el autor)*.
Las apps (`.sen`) solo hablan con el sistema por la tabla de funciones, así que pueden tener cualquier licencia.
Sony Ericsson, W580i, Walkman y otros nombres son marcas de sus dueños y se usan solo para decir de qué hardware se trata; este
proyecto no tiene relación con ellos y no contiene software suyo.
