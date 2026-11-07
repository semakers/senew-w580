# senew — documentación técnica

> [!WARNING]
> **senew es un proyecto nuevo, en beta: úsalo bajo tu propio riesgo.** Instalar senew sustituye el software del teléfono.
> Se instala con SENUS, que antes hace una copia de seguridad; guárdala. Prueba primero en el emulador.

senew es un sistema operativo abierto, escrito desde cero, para el **Sony Ericsson W580i**: chip **DB2020** con núcleo
**ARM926EJ-S** (ARMv5TE), **64 MiB de NOR** Intel M18, pantalla de **240x320**, teclado, acelerómetro, luces, USB, ranura
Memory Stick Micro (M2) y Bluetooth (chip STLC2500). Código: <https://github.com/semakers/senew-w580> (GPL-3.0).
Versión actual: **0.30.3** (build 303).

No es una modificación del firmware original: senew sustituye entero el programa principal del teléfono por el suyo, con su
propio bootloader. Lo único del fabricante que queda en el aparato es lo que senew no toca (la ROM de arranque, el cargador de
fábrica, el FS de fábrica y los datos de calibración), y de ahí senew solo **lee**.

## 1. Las capas

```
apps/  ui/  shell/         lo que se ve: apps de serie, el framework de interfaz, el shell (menús, barra, espera, cargador de .sen)
        │
api/  (hw.h)               el núcleo PORTABLE: sistemas de ficheros, USB + MTP, contrato de las apps, audio MIDI, luces…
        │
hal/  plat/  placas/  bt/  el hardware: drivers del DB2020, la plataforma W580, la descripción de placa, el Bluetooth
```

El mismo código de `ui/`, `shell/`, `apps/` y `api/` se compila **para el teléfono** (`arm-none-eabi-gcc`) y **para el
ordenador** (`gcc` con ASan/UBSan, con el hardware simulado en `plat/hw_pc.c`), donde se prueba con guiones (`herr/guiones/`):
teclas, esperas y capturas de pantalla.

### `hal/` — los drivers

Arranque (`arranque.S`: vectores, pilas, MMU y cachés), relojes y PLL (`reloj.S`, `reloj.c`), interrupciones (`intcon.c`), el tic
(`timer.c`), teclado (`kbd.c`), pantalla (`lcd.c`: el PDI del DB2020, que ejecuta un microcódigo, y el panel), I²C y el PMU
(`i2c.c`: batería, carga, tecla de encendido, acelerómetro por el ADC), NOR (`nor.c`, `m18.h`: borrado y programación en modo de
control del M18, con suspensión y topes de tiempo), RTC (`rtc.c`), UART (`uart.c`), el controlador USB de dispositivo
(`usb_dc.c`), Memory Stick (`ms.c`, `ms_db2020.c`), cámara (`camara_w580.c`, sensor OV2640 por SCCB), DSP (`dsp_w580*.c`) y
reproductor MP3 por el DSP (`mp3_w580*.c`).

### `api/` — el núcleo portable

* `hw.h`: la frontera entre el sistema y el hardware (pantalla, teclas, sonido, almacenamiento, energía…).
* Sistemas de ficheros:
  * **W6FS** (`w6fs.c`): el FS de SOLO LECTURA de senew, con las utilidades de fábrica, fondos, imágenes y MIDI de muestra. Lo
    construye `herr/hacer_fs.py` desde `fs_propio/` y va en `0x44400000`.
  * **W6F, la «Memoria»** (`w6f.c`, `hw_w6f.c`): la zona del usuario, 22 MiB desde `0x44500000`, con carpetas: un FS de registro (nunca
    reescribe en el sitio) a prueba de cortes de corriente, con su propia batería de pruebas de cortes en el ordenador.
  * **FAT12/16/32** (`fat.c`, `tarjeta.c`): la tarjeta de memoria, montada como `/tarjeta`, con lectura y escritura.
  * El lector del **FS de fábrica** (`fsfab.c`): solo lectura; de ahí senew toma, mientras funciona, ficheros como los ajustes del
    chip Bluetooth, los acústicos o los efectos de luz.
  * El **almacén** de ajustes (`almacen.c`) y la zona de ficheros de usuario (`usuario.c`), con pruebas de cortes en cada operación.
* USB (`usb.c`): capítulo 9, un respondedor **MTP** (`mtp.c`) y el **canal de gestión** de SENUS (`gestion.c`: información del
  aparato, vincular, entrar al bootloader…). Lleva descriptores «MS OS 1.0» para que Windows le ponga WinUSB sin driver.
* Audio (`mezcla.c`: lector de MIDI y mezcla por el generador de tonos o PCM), luces (`luces.c`: intérprete de efectos `.ble`).
* El contrato de las apps: `cargable.h` y `cargable_*.h` (ver §4).

### `shell/` y `ui/` — lo que se ve

* `ui/`: widgets, disposición, **repintado parcial** (solo lo que cambia), texto con **Roboto** rasterizada a 4 bits, símbolos de
  Material Symbols, y dos sistemas de diseño intercambiables (Material 3 y clásico), con la paleta generada desde un color semilla
  (`#FF7800`).
* `shell/`: el menú principal y sus **estilos** (Ajustes → «Menú: estilo»): *Rejilla* (2D); *Naranja*, *Cristal* y *Neón* (iconos
  3D con `r3d/`, que se inclinan con el acelerómetro); *Medios* (una barra de categorías en cruz con partículas) y *Escritorio*
  (accesos directos y un ratón). También la barra de estado, la pantalla de espera, el servicio de música (sigue sonando con la app
  cerrada), la energía y el modo seguro, y el cargador de apps.
* `apps/`: Ajustes (también Bluetooth y el menú principal), Archivos, Sistema (diagnósticos de energía, USB, tarjeta, DSP…) y
  Recibir (por serie). Las demás utilidades (calculadora, notas, reloj, imágenes, música, cámara, grabadora, luces, movimiento y
  «Lo que falta») son apps `.sen` normales, cada una en su repositorio `senew-app-*`.

senew **no hace llamadas ni usa la red móvil**; la app «Lo que falta» dice qué hardware no está soportado todavía.

## 2. La flash y el arranque

```
0x44000000  cargador de fábrica (no se toca)
0x44140000  BOOTLOADER de senew (1 bloque de 256 KiB)        <- la ROM de arranque entra SIEMPRE aquí
0x44180000  main de senew (hasta 0x443BFFFF: 9 bloques)
0x44400000  W6FS de senew (solo lectura)
0x44500000  W6F, la «Memoria» del usuario (22 MiB, hasta 0x45AFFFFF)
0x45E00000  FS de fábrica (solo lectura para senew)
0x47E00000  datos de calibración de fábrica (no se tocan)
```

**El bootloader** (`plat/bl/`, ~6 KB) decide en `bl_arranque.S`, desde la NOR y sin tocar nada más: si en la iRAM está la bandera
`BL_MAGIA` («BLOA») y su complemento, o si el main no tiene vectores válidos (un flasheo cortado), se queda en **modo flasheo**; si
no, salta a `0x44180000`. En modo flasheo corre entero desde la ITCM y la DTCM, sube el reloj como senew y habla USB con el mismo
núcleo y el mismo driver que el sistema, por peticiones de vendedor en el EP0 (VID:PID `1209:0002`): `ID`, `RES`, `PUT`, `ARG`,
`GO`, `GET`… Solo sabe borrar y programar el tramo del main; jamás su propio bloque, el W6FS, la Memoria, el FS de fábrica ni los
datos de calibración (la comprobación está en las propias rutinas de la NOR). SENUS actualiza senew así: unos 40 s, sin modo de
servicio.

**La primera instalación** (cuando el teléfono aún tiene el firmware original) va por el **anyCID**: con el teléfono apagado y la
tecla C pulsada al enchufar el cable entra en modo de servicio (`0fce:adde`); SENUS, con `seftool-senew`, sube dos paquetes: el de
plataforma (`DB2020.zip`, con una MIDlet de la comunidad que aprovecha un fallo del verificador de Java) y el de producto
(`R8BE001_*.zip`). El usuario abre la MIDlet en el menú de juegos del firmware original; esta salta a código ARM, nuestro
**trampolín** (`plat/bl/trampoline.S`) copia el bootloader en **modo instalador** (`make bl-installer`: con pantalla y barra de
progreso) a la ITCM/DTCM y lo arranca; el teléfono reaparece por USB como `1209:0003` y SENUS graba el bootloader y el main. Las
herramientas que arman esos paquetes están en `herr/bl/` (leen los paquetes originales que tenga el usuario; no los traen).

## 3. Bluetooth y red

* `bt/servicio/`: el servicio Bluetooth de senew sobre **BTstack** (BlueKitchen; no se distribuye con senew, ver `NOTICE`): encender
  el chip **STLC2500** por la UART 3, cargarle su parche y sus ajustes (que senew lee del FS de fábrica del propio teléfono), buscar,
  emparejar (PIN) y abrir el **perfil de puerto serie** (RFCOMM). Está dormido hasta que una app o Ajustes lo encienden.
* `bt/red/`: **W5RED v1**, el lado del teléfono de la red de senew. El teléfono no tiene conexión de datos propia: una app abre un
  canal (`red_conectar`, `red_http` o `red_tcp`, `red_enviar`, `red_recibir`…), senew lo lleva por el puerto serie Bluetooth hasta **SENUS**, en el ordenador, que hace de
  puente y habla con el servidor propio de la app. La guía para hacer apps con internet está en `senew-sdk` (`docs/*/apps-con-internet.md`).

## 4. Las apps `.sen`

Una app es un fichero ARM **independiente de la posición** con cabecera (`W5X\x01`, versión de API, perfil, capacidades, pila y
montón que pide, icono, CRC), código, datos, **reubicaciones** y, si quiere, recursos. El cargador (`shell/cargador.c`) la comprueba
entera antes de ejecutarla, la copia en SU memoria, aplica las reubicaciones y la arranca en **modo usuario**, con la MMU protegiendo
la memoria del sistema. La app no enlaza con senew: llama al sistema por una **tabla numerada de funciones** (`w5`, `api/cargable.h`),
que solo crece por el final. Cada grupo de la API vive en su `shell/cg_*.c`, y todo puntero y tamaño que llega de la app se valida
(en caso de error se devuelve un código; nada acaba en pánico). Grupos: pantalla y widgets, teclas, sonido y vibrador, ficheros (Mis
archivos, la Memoria, la tarjeta), sistema de diseño, Bluetooth, red, música por la red, cámara, micrófono, descargas.

Las apps se hacen con **senew-sdk** (<https://github.com/semakers/senew-sdk>) y se suben con SENUS (Apps → «Subir .sen»).

## 5. Compilar

Probado en macOS 26 (Apple silicon) con Arm GNU Toolchain **15.2.Rel1**, GNU make 4.4.1 y Python 3.9.6:

```sh
git clone https://github.com/semakers/senew-w580 && cd senew-w580
sh herr/traer_btstack.sh      # una vez: BTstack (commit fijo) + el parche de senew
make -j3 w580                  # build/sistema.bin (el main) y build/flash-w580.bin (64 MiB, SOLO para el emulador)
make bl                        # build/bl/bl.bin (el bootloader)
make bl-installer              # build/installer/bl/bl.bin (el bootloader en modo instalador)
make w580 BT=0                 # sin Bluetooth (sin BTstack)
```

Este árbol público no trae las tablas que salen de ficheros del fabricante (la pantalla, la cámara, las imágenes del DSP): compila
con sustitutos vacíos. Con ellos **el emulador funciona**, pero en un teléfono real la pantalla no se iniciaría. La tabla de la
pantalla y la de la cámara se generan desde los ficheros de **tu** teléfono:

```sh
make paneles DISPDRIVER=/ruta/dispdriver0.dat         # /ifs/settings/display/dispdriver0.dat del FS de fábrica
make camara-tabla CAMDRIVER=/ruta/camdriver0.dat      # /ifs/settings/camera/camdriver0.dat
```

Sin las imágenes del DSP no hay MP3 por el DSP ni micrófono.

## 6. Probar

* **En el ordenador**: `make probar-rapido` (descripciones de placa; el almacén con miles de cortes de corriente; el núcleo USB contra
  un anfitrión simulado, también con el driver real sobre un modelo del UDC a nivel de registro; el driver de la NOR contra un modelo
  del chip con suspensión y fallos; el RTC; el I²C con fallos inyectados; la carga; el protocolo de Memory Stick; y que el binario
  solo escriba en la tarjeta por el camino permitido). `make sin-escritura` comprueba sobre el ELF que una compilación con
  `-DNOR_ESCRITURA=0` no tiene ninguna rutina que borre o programe la NOR. `make pc` compila el sistema entero para el ordenador
  (solo Linux) y se maneja con guiones: `build/sistema_pc herr/guiones/00_shell.txt evidencia/pc/00`.
* **En el emulador** ([senew-emu](https://github.com/semakers/senew-emu), la máquina `w580` de QEMU):

  ```sh
  make emu QEMU=../senew-emu/build/qemu-system-arm      # arranca, quita el menú de encendido y guarda build/pantalla.png
  python3 herr/emu.py arrancar --qemu ../senew-emu/build/qemu-system-arm --flash build/flash-w580.bin
  python3 herr/emu.py tecla sel_der ; python3 herr/emu.py tecla sel_izq ; python3 herr/emu.py captura menu.png
  python3 herr/emu.py parar
  ```

  Una app en el emulador: `make flash-apps SEN="ruta/app.sen"` hace `build/flash-w580-apps.bin` con la app en la Memoria (`/apps`);
  luego `make emu FLASH=build/flash-w580-apps.bin QEMU=…`.

## 7. Límites conocidos

* Proyecto joven, en beta. Probado sobre todo en un W580i (variante GENERIC_LA) y en el emulador.
* Sin telefonía ni datos móviles; la red de las apps necesita SENUS en un ordenador cerca (Bluetooth).
* El árbol público no trae datos del fabricante: sin ellos no hay MP3 por el DSP, micrófono ni cámara, y la pantalla de un teléfono
  real necesita la tabla generada desde su propio `dispdriver0.dat`.
* `make pc` y algunas pruebas del ordenador solo compilan en Linux.
* El binario 0.30.3 que instala SENUS hoy salió del árbol de desarrollo; la próxima versión saldrá de este.
