# senew — technical documentation

> [!WARNING]
> **senew is a new project, in beta: use it at your own risk.** Installing senew replaces the phone's software.
> It is installed with SENUS, which makes a backup first; keep it. Try it in the emulator first.

senew is an open operating system, written from scratch, for the **Sony Ericsson W580i**: **DB2020** system-on-chip with an
**ARM926EJ-S** core (ARMv5TE), **64 MiB of Intel M18 NOR flash**, a **240x320** screen, keypad, accelerometer, lights, USB, a
Memory Stick Micro (M2) slot and Bluetooth (STLC2500 chip). Code: <https://github.com/semakers/senew-w580> (GPL-3.0).
Current version: **0.30.3** (build 303).

It is not a modification of the original firmware: senew replaces the phone's whole main program with its own, with its own
bootloader. The only parts of the manufacturer's software left on the device are those senew does not touch (the boot ROM, the
factory loader, the factory file system and the calibration data), and senew only **reads** from them.

## 1. The layers

```
apps/  ui/  shell/         what you see: built-in apps, the UI framework, the shell (menus, status bar, standby, .sen loader)
        │
api/  (hw.h)               the PORTABLE core: file systems, USB + MTP, the app contract, MIDI audio, lights…
        │
hal/  plat/  placas/  bt/  the hardware: DB2020 drivers, the W580 platform, the board description, Bluetooth
```

The same code in `ui/`, `shell/`, `apps/` and `api/` is built **for the phone** (`arm-none-eabi-gcc`) and **for the host**
(`gcc` with ASan/UBSan, with the hardware simulated in `plat/hw_pc.c`), where it is tested with scripts (`herr/guiones/`): keys,
waits and screenshots.

### `hal/` — drivers

Boot (`arranque.S`: vectors, stacks, MMU and caches), clocks and PLL (`reloj.S`, `reloj.c`), interrupts (`intcon.c`), the tick
(`timer.c`), keypad (`kbd.c`), screen (`lcd.c`: the DB2020's PDI, which runs microcode, and the panel), I²C and the PMU (`i2c.c`:
battery, charging, power key, accelerometer through the ADC), NOR (`nor.c`, `m18.h`: erase and program in the M18's control mode,
with suspend and time limits), RTC (`rtc.c`), UART (`uart.c`), the USB device controller (`usb_dc.c`), Memory Stick (`ms.c`,
`ms_db2020.c`), camera (`camara_w580.c`, OV2640 sensor over SCCB), DSP (`dsp_w580*.c`) and the MP3 player through the DSP
(`mp3_w580*.c`).

### `api/` — the portable core

* `hw.h`: the boundary between the system and the hardware (screen, keys, sound, storage, power…).
* File systems:
  * **W6FS** (`w6fs.c`): senew's READ-ONLY file system, holding the factory utilities, wallpapers, sample images and MIDI files.
    `herr/hacer_fs.py` builds it from `fs_propio/`; it lives at `0x44400000`.
  * **W6F, the "Memoria"** (`w6f.c`, `hw_w6f.c`): the user area, 22 MiB from `0x44500000`, with folders: a log-structured file
    system (it never rewrites in place) that survives power cuts, with its own power-cut test suite on the host.
  * **FAT12/16/32** (`fat.c`, `tarjeta.c`): the memory card, mounted as `/tarjeta`, read and write.
  * The **factory file system** reader (`fsfab.c`): read-only; at run time senew takes from it files such as the Bluetooth chip
    settings, the acoustic settings or the light effects.
  * The settings **store** (`almacen.c`) and the user file area (`usuario.c`), tested with power cuts at every operation.
* USB (`usb.c`): chapter 9, an **MTP** responder (`mtp.c`) and SENUS's **management channel** (`gestion.c`: device information,
  pairing with a computer, entering the bootloader…). It has "MS OS 1.0" descriptors so that Windows binds WinUSB with no driver.
* Audio (`mezcla.c`: MIDI reader and mixing to the tone generator or PCM), lights (`luces.c`: interpreter of `.ble` effects).
* The app contract: `cargable.h` and `cargable_*.h` (see §4).

### `shell/` and `ui/` — what you see

* `ui/`: widgets, layout, **partial repaint** (only what changes), text in **Roboto** rasterised at 4 bits per pixel, Material
  Symbols, and two interchangeable design systems (Material 3 and classic), with the palette generated from a seed colour (`#FF7800`).
* `shell/`: the main menu and its **styles** (Settings → "Menú: estilo"): *Rejilla* (2D grid); *Naranja*, *Cristal* and *Neón* (3D
  icons drawn with `r3d/`, which tilt with the accelerometer); *Medios* (a cross-shaped media bar with particles) and *Escritorio*
  (desktop shortcuts and a mouse pointer). Also the status bar, standby screen, the music service (keeps playing with the app
  closed), power management and safe mode, and the app loader.
* `apps/`: Settings (including Bluetooth and the main menu), Files, System (power, USB, card, DSP diagnostics…) and Receive (over
  serial). The other utilities (calculator, notes, clock, images, music, camera, recorder, lights, motion and "Lo que falta") are
  ordinary `.sen` apps, each in its own `senew-app-*` repository.

senew **does not make calls or use the mobile network**; the "Lo que falta" app lists the hardware that is not supported yet.

## 2. Flash layout and boot

```
0x44000000  factory loader (not touched)
0x44140000  senew BOOTLOADER (one 256 KiB block)            <- the boot ROM ALWAYS enters here
0x44180000  senew main (up to 0x443BFFFF: 9 blocks)
0x44400000  senew W6FS (read-only)
0x44500000  W6F, the user "Memoria" (22 MiB, up to 0x45AFFFFF)
0x45E00000  factory file system (read-only for senew)
0x47E00000  factory calibration data (not touched)
```

**The bootloader** (`plat/bl/`, ~6 KB) decides in `bl_arranque.S`, running from NOR and touching nothing else: if the iRAM holds the
`BL_MAGIA` flag ("BLOA") and its complement, or if the main has no valid vectors (an interrupted flash), it stays in **flash
mode**; otherwise it jumps to `0x44180000`. In flash mode it runs entirely from ITCM and DTCM, raises the clock like senew, and
speaks USB with the same core and driver as the system, through vendor requests on EP0 (VID:PID `1209:0002`): `ID`, `RES`, `PUT`,
`ARG`, `GO`, `GET`… It can only erase and program the main's range: never its own block, the W6FS, the Memoria, the factory file
system or the calibration data (the check lives in the NOR routines themselves). SENUS updates senew this way: about 40 s, no
service mode.

**The first installation** (while the phone still runs the original firmware) goes through **anyCID**: with the phone off and the C
key held while plugging the cable, it enters service mode (`0fce:adde`); SENUS, through `seftool-senew`, uploads two packages: the
platform one (`DB2020.zip`, with a community MIDlet that uses a flaw in the Java verifier) and the product one (`R8BE001_*.zip`). The
user opens the MIDlet from the original firmware's games menu; it jumps to ARM code, and senew's **trampoline**
(`plat/bl/trampoline.S`) copies the bootloader in **installer mode** (`make bl-installer`: with a screen and a progress bar) into
ITCM/DTCM and starts it; the phone comes back on USB as `1209:0003` and SENUS writes the bootloader and the main. The tools that
assemble those packages are in `herr/bl/` (they read the original packages the user has; they do not ship them).

## 3. Bluetooth and network

* `bt/servicio/`: senew's Bluetooth service on top of **BTstack** (BlueKitchen; not distributed with senew, see `NOTICE`): power up
  the **STLC2500** chip on UART 3, load its patch and settings (which senew reads from the phone's own factory file system), scan,
  pair (PIN) and open the **serial port profile** (RFCOMM). It sleeps until an app or Settings turns it on.
* `bt/red/`: **W5RED v1**, the phone side of senew's network. The phone has no data connection of its own: an app opens a channel
  (`red_conectar`, `red_http` or `red_tcp`, `red_enviar`, `red_recibir`…), senew carries it over the Bluetooth serial port to
  **SENUS** on the computer, which acts as a bridge and talks to the app's own server. The guide to apps that use the internet is in
  `senew-sdk` (`docs/*/apps-con-internet.md`).

## 4. `.sen` apps

An app is a **position-independent** ARM file with a header (`W5X\x01`, API version, profile, capabilities, the stack and heap it
asks for, icon, CRCs), code, data, **relocations** and, optionally, resources. The loader (`shell/cargador.c`) checks the whole file
before running it, copies it into ITS own memory, applies the relocations and starts it in **user mode**, with the MMU protecting the
system's memory. The app does not link with senew: it calls the system through a **numbered function table** (`w5`,
`api/cargable.h`), which only grows at the end. Each API group lives in its own `shell/cg_*.c`, and every pointer and size coming from
the app is validated (errors return a code; nothing ends in a panic). Groups: screen and widgets, keys, sound and vibration, files (My
files, the Memoria, the card), design system, Bluetooth, network, music over the network, camera, microphone, downloads.

Apps are made with **senew-sdk** (<https://github.com/semakers/senew-sdk>) and uploaded with SENUS (Apps → "Subir .sen").

## 5. Build

Tested on macOS 26 (Apple silicon) with Arm GNU Toolchain **15.2.Rel1**, GNU make 4.4.1 and Python 3.9.6:

```sh
git clone https://github.com/semakers/senew-w580 && cd senew-w580
sh herr/traer_btstack.sh      # once: BTstack (fixed commit) + senew's patch
make -j3 w580                  # build/sistema.bin (the main) and build/flash-w580.bin (64 MiB, ONLY for the emulator)
make bl                        # build/bl/bl.bin (the bootloader)
make bl-installer              # build/installer/bl/bl.bin (the bootloader in installer mode)
make w580 BT=0                 # without Bluetooth (no BTstack)
```

This public tree does not include the tables that come from the manufacturer's files (screen, camera, DSP images): it builds with
empty stand-ins. With them **the emulator works**, but on a real phone the screen would not start. The screen and camera tables are
generated from the files of **your** phone:

```sh
make paneles DISPDRIVER=/path/dispdriver0.dat         # /ifs/settings/display/dispdriver0.dat of the factory file system
make camara-tabla CAMDRIVER=/path/camdriver0.dat      # /ifs/settings/camera/camdriver0.dat
```

Without the DSP images there is no MP3 playback through the DSP and no microphone.

## 6. Testing

* **On the host**: `make probar-rapido` (board descriptions; the store with thousands of power cuts; the USB core against a simulated
  host, also the real driver on a register-level model of the UDC; the NOR driver against a chip model with suspend and faults; the
  RTC; I²C with injected faults; charging; the Memory Stick protocol; and that the binary only writes to the card through the allowed
  path). `make sin-escritura` checks on the ELF that a build with `-DNOR_ESCRITURA=0` has no routine that erases or programs the NOR.
  `make pc` builds the whole system for the host (Linux only) and is driven by scripts:
  `build/sistema_pc herr/guiones/00_shell.txt evidencia/pc/00`.
* **In the emulator** ([senew-emu](https://github.com/semakers/senew-emu), QEMU's `w580` machine):

  ```sh
  make emu QEMU=../senew-emu/build/qemu-system-arm      # boots, dismisses the power menu, saves build/pantalla.png
  python3 herr/emu.py arrancar --qemu ../senew-emu/build/qemu-system-arm --flash build/flash-w580.bin
  python3 herr/emu.py tecla sel_der ; python3 herr/emu.py tecla sel_izq ; python3 herr/emu.py captura menu.png
  python3 herr/emu.py parar
  ```

  An app in the emulator: `make flash-apps SEN="path/app.sen"` makes `build/flash-w580-apps.bin` with the app in the Memoria
  (`/apps`); then `make emu FLASH=build/flash-w580-apps.bin QEMU=…`.

## 7. Known limits

* Young project, in beta. Tested mostly on one W580i (GENERIC_LA variant) and in the emulator.
* No telephony or mobile data; apps' network needs SENUS on a nearby computer (Bluetooth).
* The public tree has no manufacturer data: without it there is no MP3 through the DSP, no microphone and no camera, and a real
  phone's screen needs the table generated from its own `dispdriver0.dat`.
* `make pc` and some host tests only build on Linux.
* The 0.30.3 binary that SENUS installs today came from the development tree; the next version will come from this one.
