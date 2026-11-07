# senew para el Sony Ericsson W580i — el Makefile del sistema. GPL-3.0-or-later (ver LICENSE y NOTICE).
# El MISMO código (ui/ + shell/ + apps/ sobre api/) se compila para el teléfono (arm-none-eabi-gcc) y para el PC (gcc/clang, hardware
# simulado). Documentación: README.md y docs/.
#   make w580           -> build/sistema.bin (el main crudo, en MAIN_INI = 0x44180000) y build/flash-w580.bin (NOR de 64 MiB SOLO
#                          PARA EL EMULADOR, «-bios»: main + nuestro FS + la Memoria formateada; nada del fabricante)
#   make bl             -> build/bl/bl.bin, el bootloader propio (en 0x44140000)
#   make bl-installer   -> build/installer/bl/bl.bin, el mismo BL en modo instalador (pantalla y barra), el que usa SENUS
#   make pc             -> build/sistema_pc (el sistema entero en el PC con ASan/UBSan; se maneja con guiones: herr/guiones/)
#   make flash-apps SEN="a.sen b.sen"  -> build/flash-w580-apps.bin: la flash del emulador con esas apps en la Memoria (/apps)
#   make emu            -> arranca build/flash-w580.bin en el emulador (QEMU=ruta/a/qemu-system-arm) y guarda una captura
#   make paneles DISPDRIVER=…/dispdriver0.dat    -> hal/lcd_paneles.h desde el fichero de pantalla de TU teléfono (ver README)
#   make camara-tabla CAMDRIVER=…/camdriver0.dat -> hal/camara_w580_tab.c desde el fichero de cámara de TU teléfono
#   make probar-rapido  -> las pruebas de PC que no necesitan nada de fuera (placa, almacén, W6FS, USB, NOR, RTC, I2C, carga, montículo, MS)
#   make w580 DEFS=-DHITO=n B=build/hito_n       -> el main mínimo del hito n (2 UART, 3 pantalla, 4 tic, 5 teclas, 6 luces): plat/hitos_w580.c
#   make w580 BT=0      -> sin Bluetooth (no hace falta BTstack); con BT=1 (por omisión) hace falta herr/traer_btstack.sh una vez
CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
OPT    ?= -O2
# (P600, paso 2) MODELO = qué placa da lo de COMPILACIÓN (placas/$(MODELO)/placa_conf.h: el tamaño de pantalla). El teléfono
# del W580 es siempre w580i; «make pc MODELO=w600i» da el PC a 176x220 (por omisión en build/pc-w600i).
MODELO ?= w580i
INC     = -Iapi -Iui -Ishell -Iplat -Iimg -Ir3d -Iplacas/$(MODELO)
COMUN   = -std=gnu11 -ffp-contract=off -Wall -Wextra -Wno-override-init -Wno-missing-field-initializers $(INC) $(COMUN_EXTRA)
CFLAGS  = -mcpu=arm926ej-s -mfloat-abi=soft -marm -mthumb-interwork -ffreestanding -fno-builtin \
          -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections -g $(COMUN) -Ihal -Iplat/tel -DW580=1 -DTELEFONO=1 $(DEFS)
# (I1; W6F) dónde empieza la zona de W6F: UN solo sitio; de aquí lo ven el C (-D), el enlace (--defsym, para sus ASSERT) y los
# guiones (W6F_INI en el entorno: herr/hacer_flash.py, comprobar_ramfn.py).
# (AUD7 B3) NO se toma del entorno sin más: un W6F_INI que se quedó exportado en la shell movería la zona en silencio. Para moverla
# A PROPÓSITO hay que decirlo dos veces: make … W6F_INI=0x… W6F_MOVER=1 (y el MANIFIESTO lo apunta).
# (2026-10-14, el mantenedor: sin juegos en el firmware, la Memoria crece a 22 MiB; antes 0x44B00000)
W6F_INI_FABRICA = 0x44500000
ifeq ($(origin W6F_INI),environment)
ifneq ($(W6F_INI),$(W6F_INI_FABRICA))
ifeq ($(W6F_MOVER),)
$(error W6F_INI=$(W6F_INI) viene del ENTORNO y no es el de fábrica ($(W6F_INI_FABRICA)): moverlo es mover la Memoria de un aparato ya formateado. Si es a propósito, W6F_MOVER=1; si no, unset W6F_INI)
endif
endif
endif
ifneq ($(origin W6F_INI),environment)
ifneq ($(origin W6F_INI),undefined)
ifneq ($(W6F_INI),$(W6F_INI_FABRICA))
ifeq ($(W6F_MOVER),)
$(error W6F_INI=$(W6F_INI) no es el de fábrica ($(W6F_INI_FABRICA)): para mover la Memoria a propósito, añade W6F_MOVER=1)
endif
endif
endif
endif
W6F_INI ?= $(W6F_INI_FABRICA)
export W6F_INI
CFLAGS += -DNOR_W6F_INI=$(W6F_INI)u
# (BL, 2026-10-23) LO BAJO DE LA NOR, también en UN sitio: el cargador de fábrica entra SIEMPRE en BL_INI, que desde v19 es el
# bootloader propio (plat/bl/, un bloque de 256 KiB); el main de senew va detrás, de MAIN_INI a MAIN_FIN (9 bloques), y el BL solo
# borra y programa ese tramo. MAIN_INI=0x44140000 da el main de antes BYTE A BYTE (sin BL delante: la prueba de no regresión).
BL_INI    = 0x44140000
MAIN_INI ?= 0x44180000
MAIN_FIN  = 0x443C0000
export BL_INI MAIN_INI MAIN_FIN
CFLAGS += -DBL_INI=$(BL_INI)u -DMAIN_INI=$(MAIN_INI)u -DMAIN_FIN=$(MAIN_FIN)u
ENLACE ?= hal/enlace.ld                  # (27-oct) senew EN RAM: ENLACE=hal/enlace_ram.ld
LDFLAGS = -nostdlib -nostartfiles -T $(ENLACE) -Wl,--gc-sections -Wl,--defsym=NOR_W6F_INI=$(W6F_INI) \
          -Wl,--defsym=MAIN_INI=$(MAIN_INI) -Wl,--defsym=MAIN_FIN=$(MAIN_FIN) -Wl,--defsym=BL_INI=$(BL_INI) $(LDEXTRA)
# LDEXTRA: opciones de enlace extra (27-oct): p. ej. LDEXTRA="-Wl,-u,cam_iniciar -Wl,-u,cam_capturar …" conserva el driver de la cámara (hal/camara_w580.c) para llamarlo por el puente aunque ninguna app lo use
B       = build
ifneq ($(MODELO),w580i)
ifeq ($(origin B),file)
B := build/pc-$(MODELO)
endif
ifneq ($(filter w580 w580-aparato w580-medida hitos-w580 probar-w580 all,$(MAKECMDGOALS)),)
$(error MODELO=$(MODELO): el teléfono de este Makefile es el W580i; MODELO solo cambia el PC («make pc MODELO=w600i»))
endif
endif
UI      = $(wildcard ui/*.c)
SHELL_C = $(wildcard shell/*.c)
APPS    = $(wildcard apps/*/*.c)
API     = api/hw_comun.c api/ui_sobre_hw.c api/almacen.c api/hw_fs.c api/w6fs.c api/fat.c api/tarjeta.c api/mezcla.c api/usuario.c api/usrop.c api/hw_usr.c api/fsfab.c api/luces.c api/w6f.c api/hw_w6f.c api/serie_rx.c api/mtp.c api/gestion.c
IMG     = img/png/img_png.c img/jpeg/img_jpeg.c
# (ms810) hal/ms_db2010.c es el anfitrión de Memory Stick del DB2010 (W810i): NO entra en el W580 (filter-out en HAL e HITO_SRC).
# El rasterizador del menú 3D (pista V): copia recortada del r3d del W600. El .S (tramo caliente) solo va al teléfono.
R3D     = r3d/r3d_ras.c r3d/r3d_mat.c r3d/r3d_seno.c
# (USBD) el núcleo USB (capítulo 9 + MTP + tubo).
API    += api/usb.c
# (v16) el puente de depuración por USB: vacío sin -DPUENTE, así que no cambia ningún binario normal
API    += api/puente.c
PORTABLE = $(UI) $(SHELL_C) $(APPS) $(API) $(IMG) $(R3D)
# (P600, paso 1) LA DESCRIPCIÓN DE PLACA (api/placa.h): una estructura constante por modelo en placas/<m>/placa.c. El PC es una placa
# SIMULADA: enlaza todas y placas/pc/placa_pc.c da la del MODELO del entorno. (En este repositorio solo se publica el port al W580i;
# de los W600i y W810i quedan sus descripciones de placa, que usa el PC.)
PLACAS_PC = placas/pc/placa_pc.c placas/w580i/placa.c placas/w600i/placa.c placas/w810i/placa.c
HAL     = hal/reloj.S $(wildcard hal/*.c) placas/w580i/placa.c plat/tel/perro.c plat/hw_w580.c plat/tarjeta_w580.c plat/main_w580.c plat/mem.c r3d/r3d_arm.S
# EL SERVICIO BLUETOOTH, solo en el TELÉFONO (el STLC2500 por la UART 3). DORMIDO por omisión: lo enciende una app o Ajustes -> «Bluetooth».
# Nuestro código está en bt/ (servicio, chipset, port y la red W5RED); la pila es BTstack 1.8.3 de BlueKitchen, que NO se copia en
# este repositorio (su licencia es otra: BSD con una cláusula de uso NO comercial; ver NOTICE): herr/traer_btstack.sh la descarga en
# bt/terceros/btstack. «make w580 BT=0» compila sin Bluetooth (bt_driver() débil de shell/bt.c = sin radio, como el PC).
BT      ?= 1
BTD      = bt
BTS      = $(BTD)/terceros/btstack
BT_TERC  = $(addprefix $(BTS)/,src/btstack_linked_list.c src/btstack_memory.c src/btstack_memory_pool.c src/btstack_run_loop.c \
           src/btstack_util.c src/ad_parser.c src/hci.c src/hci_cmd.c src/hci_dump.c src/hci_transport_h4.c src/l2cap.c \
           src/l2cap_signaling.c src/classic/rfcomm.c src/classic/sdp_client.c src/classic/sdp_client_rfcomm.c src/classic/sdp_util.c \
           src/classic/btstack_link_key_db_memory.c platform/embedded/btstack_run_loop_embedded.c platform/embedded/btstack_uart_block_embedded.c)
BT_NUES  = $(BTD)/chipset/btstack_chipset_stlc2500c.c $(BTD)/port/hal_uart_bloques.c $(BTD)/servicio/bt_servicio.c \
           $(BTD)/servicio/bt_indices.c $(BTD)/port/bt_hw_w580.c
BT_INC   = -I$(BTD)/port -I$(BTD)/chipset -I$(BTD)/servicio -I$(BTS)/src -I$(BTS)/platform/embedded
BT_CAB   = $(wildcard $(BTD)/port/*.h $(BTD)/chipset/*.h $(BTD)/servicio/*.h)
BT_OPC   = -Os -DNDEBUG -Wno-unused-parameter -Wno-sign-compare
ifeq ($(BT),1)
HAL     += $(BT_TERC) $(BT_NUES) plat/bt_w580.c
ifneq ($(filter w580 all bl-installer emu flash-apps,$(if $(MAKECMDGOALS),$(MAKECMDGOALS),w580)),)
ifeq ($(wildcard $(BTS)/src/hci.c),)
$(error Falta BTstack en $(BTS) (no se distribuye con senew; ver NOTICE). Descárgala una vez con «sh herr/traer_btstack.sh», o compila sin Bluetooth con «make w580 BT=0»)
endif
endif
endif
HITO_SRC = hal/arranque.S hal/reloj.S $(wildcard hal/*.c) plat/hitos_w580.c plat/mem.c
CAB     = $(wildcard plat/tel/*.h placas/*/*.h ui/*.h api/*.h shell/*.h hal/*.h plat/*.h img/*.h apps/*/*.h r3d/*.h r3d/*.inc) $(wildcard bt/red/red.h bt/red/red.c)   # (1.6) shell/red_nucleo.c incluye bt/red/red.c
GUIONES = $(sort $(wildcard herr/guiones/*.txt))

# APPS EXTERNAS: código y datos que viven FUERA de sistema/ y que se enlazan
# dentro del main. Cada directorio de APPS_EXT trae un app.mk que solo añade (+=) a estas cinco
# variables. Sin APPS_EXT no cambia absolutamente nada (el binario sale idéntico byte a byte).
#   make w580 APPS_EXT=herr/app_ext_ejemplo B=build/con_ext
#   APPEXT_SRC  sus .c y .S (ruta relativa a sistema/)      APPEXT_INC  sus -I
#   APPEXT_DEF  sus -D y opciones solo de sus objetos       APPEXT_OPT  -O2, -Os… (por omisión -O2)
#   APPEXT_LIB  bibliotecas para el ENLACE (-L… -lm, o un .a): van detrás de los objetos y delante de -lgcc
#   APPEXT_PC := 1  (opcional) la app también se compila para el PC; sin ella, «make pc APPS_EXT=…» se niega
# Dónde caen sus objetos: como los de casa, en $(B)/w580/<ruta del fuente>.o. Con un fuente
# «../otra/x.c» eso es $(B)/w580/../otra/x.c.o = $(B)/otra/x.c.o (un nivel, no más:
# un fuente con «../../» o absoluto se saldría de $(B) y el Makefile se niega).
# Con APPS_EXT y sin B= en la línea de órdenes, B pasa a ser build/con_ext: el firmware de trabajo
# (build/sistema.bin) y la entrega NUNCA llevan una app externa por descuido.
APPS_EXT ?=
APPEXT_SRC :=
APPEXT_INC :=
APPEXT_DEF :=
APPEXT_OPT :=
APPEXT_LIB :=
APPEXT_SOLO_TEL :=
define INCLUIR_EXT
APPEXT_PC :=
include $(1)/app.mk
ifeq ($$(strip $$(APPEXT_PC)),)
APPEXT_SOLO_TEL += $(1)
endif
endef
$(foreach d,$(APPS_EXT),$(eval $(call INCLUIR_EXT,$(d))))
APPEXT_OPT := $(if $(APPEXT_OPT),$(APPEXT_OPT),-O2)
ifneq ($(strip $(APPS_EXT)),)
ifeq ($(origin B),file)
B := build/con_ext
endif
ifneq ($(filter ../../% /%,$(APPEXT_SRC)),)
$(error APPS_EXT: estos fuentes caerían fuera de $(B) (ruta absoluta o con más de un «../»): $(filter ../../% /%,$(APPEXT_SRC)))
endif
endif

# (R3) HITO_SIN_LLAVE quita de hal/arranque.S la llave del perro de hardware: SOLO vale para el main de MEDIDA del perro
# (-DHITO=8). Con el sistema o con cualquier otro hito, el Makefile se niega (condición del coordinador para tocar arranque.S).
ifneq ($(findstring HITO_SIN_LLAVE,$(DEFS) $(CFLAGS) $(COMUN_EXTRA) $(APPEXT_DEF)),)
ifeq ($(filter -DHITO=8,$(DEFS)),)
$(error HITO_SIN_LLAVE quita la llave del perro: solo para el main de MEDIDA, y se compila con «make w580-medida» (va a build/hito_8a, aparte). Con el sistema o con otro hito, NO)
endif
endif
.DEFAULT_GOAL := w580
all: pc w580
# El PC solo compila las apps externas que lo declaran (APPEXT_PC := 1 en su app.mk).
ifeq ($(strip $(APPEXT_SOLO_TEL)),)
pc: $(B)/sistema_pc
else
pc:
	@echo "make pc: estas apps externas solo existen para el teléfono (su app.mk no declara APPEXT_PC := 1): $(APPEXT_SOLO_TEL)"; \
	 echo "         usa «make w580 APPS_EXT=…», o «make pc» sin APPS_EXT."; exit 1
endif
w580: $(B)/sistema.bin $(if $(findstring -DHITO,$(DEFS)),,$(B)/flash-w580.bin)
# (BL, v19) El bootloader propio (plat/bl/): main en 0x44140000 estándar, pero no es un main de senew — es la entrada por donde
# el cargador de fábrica llega siempre, decide y salta al main de senew en 0x44180000 o se queda en modo flasheo USB. Compila
# aparte: ningún fuente compartido con senew salvo plat/bl/bl.h (contrato).
# (installer, 2026-10-29) con -DBL_INSTALLER=1 se añade plat/bl/bl_pantalla.c (pantalla + barra) y aparecen las órdenes 0xD4..0xD6
# (WRITE_PROPIO, INSTALADOR_INI, LISTO). Sin la bandera el BL se compila byte-equivalente al de siempre (salvo __DATE__/__TIME__).
BL_SRC = plat/bl/bl_arranque.S plat/bl/bl_main.c plat/bl/bl_usb.c plat/bl/bl_nor.c plat/bl/bl_syscon.c
ifneq (,$(findstring BL_INSTALLER,$(BL_DEFS)))
BL_SRC += plat/bl/bl_pantalla.c
endif
BL_B   = $(B)/bl
BL_OBJ = $(patsubst %,$(BL_B)/%.o,$(BL_SRC))
BL_CFLAGS = -mcpu=arm926ej-s -mfloat-abi=soft -marm -mthumb-interwork -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -Os -g \
            -std=gnu11 -Wall -Wextra -Iplat/bl -DBL=1 $(BL_DEFS)
BL_LDFLAGS = -nostdlib -nostartfiles -T plat/bl/bl_enlace.ld -Wl,--gc-sections \
             -Wl,--defsym=BL_INI=$(BL_INI) -Wl,--defsym=MAIN_INI=$(MAIN_INI) -Wl,--defsym=MAIN_FIN=$(MAIN_FIN)
bl: $(BL_B)/bl.bin
$(BL_B)/%.o: % plat/bl/bl.h | $(BL_B)    # (27-oct) y bl.h: un cambio en las órdenes dejaba bl_usb.c.o viejo (STALL de la orden 5)
	@mkdir -p $(dir $@)
	$(CC) $(BL_CFLAGS) -c -o $@ $<
$(BL_B)/bl.elf: $(BL_OBJ) plat/bl/bl_enlace.ld
	$(CC) $(BL_CFLAGS) $(BL_LDFLAGS) -Wl,-Map=$(@:.elf=.map) -o $@ $(BL_OBJ) -lgcc
$(BL_B)/bl.bin: $(BL_B)/bl.elf
	$(OBJCOPY) -O binary $< $@
	@python3 -c "import sys;d=open('$@','rb').read();print('$@: %d bytes; primera palabra %08X' % (len(d), int.from_bytes(d[:4],'little')))"
$(BL_B):
	@mkdir -p $(BL_B)

$(B)/sistema_pc: $(PORTABLE) plat/hw_pc.c plat/pc_main.c plat/pc_nor.c plat/tarjeta_pc.c plat/usb_dc_pc.c $(PLACAS_PC) $(APPEXT_SRC) $(CAB) | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(COMUN) -DPLACA_SIMULADA -DMODELO_PC='"$(MODELO)"' $(APPEXT_INC) $(APPEXT_DEF) -o $@ $(PORTABLE) plat/hw_pc.c plat/pc_main.c plat/pc_nor.c plat/tarjeta_pc.c plat/usb_dc_pc.c $(PLACAS_PC) $(APPEXT_SRC) -lm

# Teléfono: un objeto por fuente. El framework y la HAL van con -O2 (son el camino caliente: pintar);
# el shell y las apps con -Os (son sobre todo literales de widgets: -Os los deja en ~2/3).
# Con -DHITO=n el binario es el main mínimo del hito sobre la HAL sola (ni framework ni plataforma).
FUENTES_W = $(if $(findstring -DHITO,$(DEFS)),$(HITO_SRC),hal/arranque.S $(PORTABLE) $(HAL) $(APPEXT_SRC))
OBJ   = $(patsubst %,$(B)/w580/%.o,$(FUENTES_W))
# (el dibujo del menú 3D está en shell/ pero es camino caliente: va con -O2)
opt   = $(if $(filter-out shell/m3d_dibujo.c,$(filter apps/% shell/%,$(1))),-Os,$(OPT))
# Los objetos de una app externa se compilan con SUS opciones y SUS -I, nunca con los del shell.
esext = $(if $(filter $(APPEXT_SRC),$(1)),1,)
# (R3; AUDITORÍA AUD6, M2) EL SELLO DE OPCIONES: los objetos dependen de $(B)/opciones.lst, que solo se reescribe si cambian
# las opciones de compilar (CFLAGS, con DEFS dentro). Sin él, un objeto compilado con unas DEFS se reutilizaba con otras en el
# mismo B: tras «make w580 DEFS="-DHITO=8 -DHITO_SIN_LLAVE"» sin B, el sistema se enlazaba con un arranque SIN la llave.
$(B)/opciones.lst: FORCE | $(B)
	@printf '%s\n' '$(CFLAGS) | $(APPEXT_INC) $(APPEXT_DEF) $(APPEXT_OPT) | BT=$(BT) $(BT_OPC)' | cmp -s - $@ || printf '%s\n' '$(CFLAGS) | $(APPEXT_INC) $(APPEXT_DEF) $(APPEXT_OPT) | BT=$(BT) $(BT_OPC)' > $@
$(B)/w580/%.o: % $(CAB) $(B)/opciones.lst
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(call esext,$<),$(APPEXT_INC) $(APPEXT_DEF) $(APPEXT_OPT),$(call opt,$<)) -c -o $@ $<
# (pista BT) lo de ../bt: con SUS -I delante (que ningún .h del sistema tape uno de BTstack), -Os, y BTstack (de terceros) sin avisos
$(B)/w580/$(BTD)/%.o: $(BTD)/% $(BT_CAB) $(B)/opciones.lst
	@mkdir -p $(dir $@)
	$(CC) $(BT_INC) $(CFLAGS) $(BT_OPC) $(if $(filter $(BTS)/%,$<),-w) -c -o $@ $<
$(B)/w580/plat/bt_w580.c.o: plat/bt_w580.c $(CAB) $(BT_CAB) $(B)/opciones.lst
	@mkdir -p $(dir $@)
	$(CC) $(BT_INC) $(CFLAGS) $(BT_OPC) -c -o $@ $<
# $(B)/enlace.lst = QUÉ se enlaza (objetos, bibliotecas y banderas de las apps externas). Solo se reescribe
# si cambia, y el .elf depende de él: así quitar o poner APPS_EXT sobre el mismo B SIEMPRE vuelve a enlazar
# (sin esto, «make w580» tras un «make w580 APPS_EXT=…» dejaría el binario con la app dentro).
$(B)/enlace.lst: FORCE | $(B)
	@printf '%s\n' '$(OBJ) | $(APPEXT_LIB) | $(APPEXT_DEF) $(APPEXT_OPT) $(APPEXT_INC) | $(DEFS)' | cmp -s - $@ || \
	 printf '%s\n' '$(OBJ) | $(APPEXT_LIB) | $(APPEXT_DEF) $(APPEXT_OPT) $(APPEXT_INC) | $(DEFS)' > $@
FORCE:
$(B)/sistema.elf: $(OBJ) hal/enlace.ld $(B)/enlace.lst
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-Map=$(@:.elf=.map) -o $@ $(OBJ) $(APPEXT_LIB) -lgcc
	python3 herr/comprobar_ramfn.py $@ $(COMPROBAR)
$(B)/sistema.bin: $(B)/sistema.elf
	$(OBJCOPY) -O binary $< $@
	@python3 -c "import sys;d=open('$@','rb').read();print('$@: %d bytes; primera palabra %08X' % (len(d), int.from_bytes(d[:4],'little')))"
# La flash para el emulador (senew-emu, máquina w580, «-bios»): NOR plana de 64 MiB, base 0x44000000, TODO a 0xFF salvo nuestro
# main, nuestro FS (W6FS) y la Memoria (W6F) formateada. Nada del fabricante.
$(B)/flash-w580.bin: $(B)/sistema.bin $(if $(findstring -DHITO,$(DEFS)),,$(B)/fs_w580.bin $(B)/w6f_vacio.bin) herr/hacer_flash.py
	python3 herr/hacer_flash.py $< $@ $(if $(findstring -DHITO,$(DEFS)),,--fs $(B)/fs_w580.bin --w6f $(B)/w6f_vacio.bin)
# (I1) la Memoria (W6F) ya formateada para la flash del emulador: el MISMO api/w6f.c y la misma configuración (api/w6f_sistema.h)
$(B)/w6f_imagen: herr/w6f_imagen.c api/w6f.c api/w6f.h api/w6f_sistema.h | $(B)
	gcc -O1 -g -std=gnu11 -Wall -Wextra -Iapi -Iplacas/$(MODELO) -o $@ herr/w6f_imagen.c api/w6f.c
$(B)/w6f_vacio.bin: $(B)/w6f_imagen
	$(B)/w6f_imagen $@ $$(( (0x45B00000 - $(W6F_INI)) / 0x40000 )) > /dev/null   # (2026-10-14) los bloques de la zona: 88 desde 0x44500000
	@printf '%s\n' 'flash-w580.bin es una NOR ENTERA de 64 MiB SOLO PARA EL EMULADOR (qemu -bios): tiene 0xFF donde un' 'teléfono tiene el cargador (0x44000000), el FS de fábrica (0x45E00000) y el GDFS (0x47E00000).' 'NO GRABAR EN UN APARATO: borraría el cargador y el GDFS. senew se instala en un teléfono con SENUS.' > $(B)/flash-w580.bin.SOLO-EMULADOR.txt
$(B)/flash-sin-fs.bin: $(B)/sistema.bin herr/hacer_flash.py
	python3 herr/hacer_flash.py $< $@
# NUESTRO sistema de ficheros (W6FS, solo lectura): el contenido se dibuja por programa (herr/hacer_contenido.py -> fs_propio/) y
# herr/hacer_fs.py hace la imagen. De qué depende: del LISTADO de fs_propio/ (nombre, tamaño y fecha de cada fichero), no de los
# ficheros uno a uno (hay nombres con espacios y make parte las dependencias por los espacios).
# fs_propio/apps/*.sen son las utilidades de fábrica, compiladas desde sus repositorios públicos (github.com/semakers/senew-app-*).
# Las mallas de los iconos 3D del menú se hacen por programa; el .h generado está en el árbol.
shell/mallas3d.h: | herr/hacer_mallas.py
	@mkdir -p tmp; python3 herr/hacer_mallas.py > $@.tmp 2> tmp/mallas.log && mv $@.tmp $@
# fs_propio/ (fondos, imágenes, textos) ya está en el repositorio: solo se regenera si falta LEEME.txt (la dependencia del guion es
# «solo de orden»: tras un `git clone` las fechas son arbitrarias y no debe volver a generarse). `make contenido` lo regenera
# a propósito (necesita Pillow: `pip install pillow`).
fs_propio/docs/LEEME.txt: | herr/hacer_contenido.py
	python3 herr/hacer_contenido.py fs_propio
contenido:
	python3 herr/hacer_contenido.py fs_propio
.PHONY: contenido
$(B)/fs.lst: FORCE fs_propio/docs/LEEME.txt | $(B)
	@python3 -c "import os,sys;print(''.join(sorted('%s|%d|%.6f\n'%(p,os.path.getsize(p),os.path.getmtime(p)) for d,_,fs in os.walk('fs_propio') for p in [os.path.join(d,f) for f in fs])),end='')" > $@.tmp; cmp -s $@.tmp $@ || mv $@.tmp $@; rm -f $@.tmp
$(B)/fs_w580.bin: fs_propio/docs/LEEME.txt $(B)/fs.lst herr/hacer_fs.py | $(B)
	python3 herr/hacer_fs.py fs_propio $@
	python3 herr/hacer_fs.py --listar $@ | tail -1
fs: $(B)/fs_w580.bin
# LA PANTALLA Y LA CÁMARA DESDE TU TELÉFONO. Las tablas de inicio del LCD (microcódigo del PDI y guiones de los paneles) y del sensor
# de la cámara salen de dos ficheros de configuración del FS de fábrica del propio teléfono. Son datos del fabricante: NO van en este
# repositorio. Sin ellos se compila con hal/lcd_paneles_stub.h (el emulador arranca; un teléfono real se queda con la pantalla sin
# iniciar) y sin tabla de cámara (hal/fabricante_ausente.c). Con ellos, estas dos reglas escriben los ficheros que git ignora.
DISPDRIVER ?=
CAMDRIVER ?=
paneles:
	@[ -n "$(DISPDRIVER)" ] && [ -f "$(DISPDRIVER)" ] || { echo "make paneles DISPDRIVER=/ruta/a/dispdriver0.dat   (el /ifs/settings/display/dispdriver0.dat de TU teléfono)"; exit 1; }
	python3 herr/hacer_paneles.py "$(DISPDRIVER)" > hal/lcd_paneles.h.tmp && mv hal/lcd_paneles.h.tmp hal/lcd_paneles.h
	@echo "hal/lcd_paneles.h escrito: recompila (make w580) para usarlo"
camara-tabla:
	@[ -n "$(CAMDRIVER)" ] && [ -f "$(CAMDRIVER)" ] || { echo "make camara-tabla CAMDRIVER=/ruta/a/camdriver0.dat   (el /ifs/settings/camera/camdriver0.dat de TU teléfono)"; exit 1; }
	CAMDRIVER="$(CAMDRIVER)" python3 herr/camara/gen_tabla_c.py > hal/camara_w580_tab.c.tmp && mv hal/camara_w580_tab.c.tmp hal/camara_w580_tab.c
	@echo "hal/camara_w580_tab.c escrito: recompila (make w580) para usarlo"
ifeq ($(wildcard hal/lcd_paneles.h),)
ifneq ($(filter w580 all bl-installer,$(if $(MAKECMDGOALS),$(MAKECMDGOALS),w580)),)
$(info AVISO: sin hal/lcd_paneles.h: se compila con hal/lcd_paneles_stub.h. Vale para el EMULADOR; en un teléfono real la pantalla NO se iniciaría. Ver «make paneles».)
endif
endif
# PROBAR APPS .sen EN EL EMULADOR: la flash con las .sen que digas en la Memoria (/apps), como si el usuario las hubiera subido.
#   make flash-apps SEN="../senew-app-serpiente/serpiente.sen ../senew-sdk/template/hello/hello.sen"
SEN ?=
$(B)/w6f_apps.bin: $(B)/w6f_imagen $(SEN) FORCE
	@[ -n "$(strip $(SEN))" ] || { echo 'make flash-apps SEN="ruta/a/app.sen …"'; exit 1; }
	$(B)/w6f_imagen $@ $$(( (0x45B00000 - $(W6F_INI)) / 0x40000 )) $(foreach a,$(SEN),--con /apps/$(notdir $(a)) $(a)) > /dev/null
$(B)/flash-w580-apps.bin: $(B)/sistema.bin $(B)/fs_w580.bin $(B)/w6f_apps.bin herr/hacer_flash.py
	python3 herr/hacer_flash.py $(B)/sistema.bin $@ --fs $(B)/fs_w580.bin --w6f $(B)/w6f_apps.bin
flash-apps: $(B)/flash-w580-apps.bin
# EL EMULADOR (github.com/semakers/senew-emu): arranca una flash, espera, quita el menú de encendido y guarda una captura en PNG.
#   make emu QEMU=…/qemu-system-arm [FLASH=build/flash-w580-apps.bin] [CAPTURA=build/pantalla.png]
QEMU ?= qemu-system-arm
FLASH ?= $(B)/flash-w580.bin
CAPTURA ?= $(B)/pantalla.png
emu: $(if $(filter $(B)/flash-w580.bin,$(FLASH)),$(FLASH))
	@[ -f $(FLASH) ] || { echo "no existe $(FLASH) (make w580, o make flash-apps SEN=…)"; exit 1; }
	python3 herr/emu.py --qemu "$(QEMU)" --flash $(FLASH) guion herr/emu_arranque.txt --captura $(CAPTURA)
# El FS del PC (W600_FS): /apps con las .sen compiladas PARA EL PC, si las hay (SEN_PC_DIR); si no, vacío.
SEN_PC_DIR ?=
$(B)/fs_pc: FORCE | $(B)
	@rm -rf $@; mkdir -p $@/apps; $(if $(SEN_PC_DIR),cp $(SEN_PC_DIR)/*.sen $@/apps/,true)
export W600_FS ?= $(abspath $(B)/fs_pc)
$(B):
	mkdir -p $(B)
# EL BL EN MODO INSTALADOR (el que lleva el paquete de primera instalación de SENUS): pantalla, barra y las órdenes de instalar.
bl-installer:
	@$(MAKE) --no-print-directory bl B=build/installer BL_DEFS=-DBL_INSTALLER=1

# El almacén con cortes de alimentación en cada operación de flash (modelo estricto de NOR): miles de casos.
$(B)/prueba_almacen: herr/pruebas/prueba_almacen.c api/almacen.c plat/pc_nor.c api/almacen.h plat/pc_nor.h hal/m18.h | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -Iapi -Iplacas/$(MODELO) -Iplat -o $@ herr/pruebas/prueba_almacen.c api/almacen.c plat/pc_nor.c
probar-almacen: $(B)/prueba_almacen
	@mkdir -p evidencia/almacen; for c in "3000 4 131072 1" "4000 2 4096 7" "4000 4 4096 99" "3000 3 8192 5" "2500 8 65536 3" "3000 2 262144 11"; do \
	  echo "== $$c"; ASAN_OPTIONS=detect_leaks=0 $(B)/prueba_almacen $$c || exit 1; done 2>&1 | tee evidencia/almacen/cortes.txt
# Sin escritura: -DNOR_ESCRITURA=0 no deja en el binario NINGUNA rutina que borre o programe la NOR.
sin-escritura:
	$(MAKE) B=build/sin_escritura DEFS=-DNOR_ESCRITURA=0 build/sin_escritura/sistema.elf COMPROBAR=--sin-escritura
$(B)/prueba_w6fs: herr/pruebas/prueba_w6fs.c api/w6fs.c api/almacen.c api/w6fs.h | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -Iapi -Iplacas/$(MODELO) -o $@ herr/pruebas/prueba_w6fs.c api/w6fs.c api/almacen.c
probar-fs: $(B)/sistema_pc $(B)/prueba_w6fs $(B)/fs_w580.bin $(B)/fs_pc
	ASAN_OPTIONS=detect_leaks=0 $(B)/prueba_w6fs $(B)/fs_w580.bin 40000
	@mkdir -p evidencia/pc_fs_propio; W600_W6FS=$(B)/fs_w580.bin ASAN_OPTIONS=detect_leaks=0 $(B)/sistema_pc herr/guiones_fs/1_propio.txt evidencia/pc_fs_propio | tail -n 1; python3 herr/ppm_a_png.py evidencia/pc_fs_propio/*.ppm
$(B)/prueba_ms: herr/pruebas/prueba_ms.c hal/ms.c hal/ms.h | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -Ihal -o $@ herr/pruebas/prueba_ms.c hal/ms.c
$(B)/prueba_ms_serie: herr/pruebas/prueba_ms.c hal/ms.c hal/ms.h | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -Ihal -DMS_SOLO_SERIE -o $@ herr/pruebas/prueba_ms.c hal/ms.c
probar-ms-serie: $(B)/prueba_ms_serie
	@mkdir -p evidencia/ms; ASAN_OPTIONS=detect_leaks=0 nice $(B)/prueba_ms_serie | tee evidencia/ms/prueba_ms_serie.txt; grep -q 'PROBAR-MS: BIEN' evidencia/ms/prueba_ms_serie.txt
probar-ms: $(B)/prueba_ms
	@mkdir -p evidencia/ms; ASAN_OPTIONS=detect_leaks=0 nice $(B)/prueba_ms | tee evidencia/ms/prueba_ms.txt
puertas-tarjeta: $(B)/sistema.elf
	python3 herr/comprobar_tarjeta_sl.py $(B)
# (USBD) el núcleo USB (api/usb.c) contra un host simulado (herr/pruebas/usb_dc_falso.c): enumeración Linux y Windows, eco de
# contenedores MTP, reset a mitad, STALL/halt, peticiones de clase, flujo, desconexión y control al azar.
$(B)/prueba_usb: herr/pruebas/prueba_usb.c herr/pruebas/usb_dc_falso.c herr/pruebas/usb_host.h api/usb.c api/usb.h hal/usb_dc.h | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -Iapi -Iplacas/$(MODELO) -Ihal -Iherr/pruebas -o $@ herr/pruebas/prueba_usb.c herr/pruebas/usb_dc_falso.c api/usb.c
# lo mismo con el driver REAL (hal/usb_dc.c) sobre un modelo del UDC a nivel de registro (herr/pruebas/udc_modelo.c)
$(B)/prueba_usb_modelo: herr/pruebas/prueba_usb.c herr/pruebas/udc_modelo.c herr/pruebas/udc_modelo.h herr/pruebas/usb_host.h hal/usb_dc.c api/usb.c api/usb.h hal/usb_dc.h | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -DCON_MODELO -DUSBDC_MODELO -Iapi -Iplacas/$(MODELO) -Ihal -Iherr/pruebas -o $@ herr/pruebas/prueba_usb.c herr/pruebas/udc_modelo.c hal/usb_dc.c api/usb.c

# (v16) las MISMAS pruebas del núcleo USB con el puente compilado dentro (api/usb.c con -DPUENTE + api/puente.c): el MTP no se estropea
$(B)/prueba_usb_puente: herr/pruebas/prueba_usb.c herr/pruebas/usb_dc_falso.c herr/pruebas/usb_host.h herr/pruebas/stubs_puente_pc.c api/usb.c api/usb.h api/puente.c api/puente.h hal/usb_dc.h | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -DPUENTE -Iapi -Iplacas/$(MODELO) -Ihal -Iherr/pruebas -o $@ herr/pruebas/prueba_usb.c herr/pruebas/usb_dc_falso.c api/usb.c api/puente.c herr/pruebas/stubs_puente_pc.c
probar-usb-puente: $(B)/prueba_usb_puente
	@mkdir -p evidencia/usb
	@$(B)/prueba_usb_puente > evidencia/usb/prueba_usb_puente.txt; r=$$?; tail -2 evidencia/usb/prueba_usb_puente.txt; [ $$r = 0 ] || { grep MAL evidencia/usb/prueba_usb_puente.txt; exit $$r; }
	@for s in 1 2 3; do $(B)/prueba_usb_puente azar 20000 $$s | tail -1; done
probar-usb: $(B)/prueba_usb $(B)/prueba_usb_modelo
	@mkdir -p evidencia/usb
	@$(B)/prueba_usb > evidencia/usb/prueba_usb.txt; r=$$?; tail -2 evidencia/usb/prueba_usb.txt; [ $$r = 0 ] || { grep MAL evidencia/usb/prueba_usb.txt; exit $$r; }
	@for s in 1 2 3 4 5; do $(B)/prueba_usb azar 20000 $$s | tail -1; done | tee evidencia/usb/azar.txt; ! grep -q FALLA evidencia/usb/azar.txt
	@$(B)/prueba_usb_modelo > evidencia/usb/prueba_usb_modelo.txt; r=$$?; tail -2 evidencia/usb/prueba_usb_modelo.txt; [ $$r = 0 ] || { grep -E 'MAL|AVISO' evidencia/usb/prueba_usb_modelo.txt; exit $$r; }
	@for s in 1 2 3 4 5; do $(B)/prueba_usb_modelo azar 20000 $$s | tail -1; done | tee evidencia/usb/azar_modelo.txt; ! grep -q FALLA evidencia/usb/azar_modelo.txt
	@# (usb_ag, 2026-10-20) un UDC que NO levanta EPN_TX/EPN_RX: el driver tiene que salir adelante con su sondeo de STAT_FLG
	@UDC_SIN_EPN_IRQ=1 $(B)/prueba_usb_modelo > evidencia/usb/prueba_usb_modelo_sin_epn.txt; r=$$?; tail -1 evidencia/usb/prueba_usb_modelo_sin_epn.txt; [ $$r = 0 ] || { grep -E 'MAL|AVISO' evidencia/usb/prueba_usb_modelo_sin_epn.txt; exit $$r; }
# (P600, paso 1) Prueba de conformidad n.º 1: toda descripción de placa es coherente consigo misma y con las reglas de
# seguridad (zonas escribibles fuera del main y del FS de fábrica, carga_permitida = 0, umbrales…). Entra en «make probar».
$(B)/coherencia_placa: pruebas/conformidad/coherencia_placa.c $(PLACAS_PC) api/placa.h ui/ui.h | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(COMUN) -DPLACA_SIMULADA -o $@ pruebas/conformidad/coherencia_placa.c $(PLACAS_PC)
probar-placa: $(B)/coherencia_placa
	@$(B)/coherencia_placa
# (CARGA) la máquina de la carga por USB (api/carga.h) contra una batería y un PMU simulados con lo medido en el aparato (docs/bateria.md §12)
$(B)/prueba_carga: herr/pruebas/prueba_carga.c api/carga.h | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -Iapi -o $@ herr/pruebas/prueba_carga.c -lm
probar-carga: $(B)/prueba_carga
	@mkdir -p evidencia/carga; $(B)/prueba_carga > evidencia/carga/prueba_carga.txt 2>&1; r=$$?; cat evidencia/carga/prueba_carga.txt; exit $$r
# (R2) hal/i2c.c de verdad contra un modelo del controlador y del PMU con fallos inyectados, y las reglas de la tecla de
# encendido (api/enc.h): y sus negativas.
$(B)/prueba_i2c: herr/pruebas/prueba_i2c.c hal/i2c.c hal/hal.h api/enc.h | $(B)
	gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -DI2C_PRUEBA -Ihal -Iapi -Iplacas/$(MODELO) -o $@ herr/pruebas/prueba_i2c.c hal/i2c.c
probar-i2c: $(B)/prueba_i2c
	@mkdir -p evidencia/pc_auditoria_hal; $(B)/prueba_i2c | tee evidencia/pc_auditoria_hal/i2c_y_tecla.txt; tail -1 evidencia/pc_auditoria_hal/i2c_y_tecla.txt | grep -q "TODO BIEN"
$(B)/prueba_monticulo: herr/pruebas/prueba_monticulo.c ui/ui_mem.c $(CAB) | $(B)
	gcc -O1 -g -fsanitize=undefined -std=gnu11 -Wall -Wextra -Wno-override-init -Wno-missing-field-initializers $(INC) -o $@ herr/pruebas/prueba_monticulo.c ui/ui_mem.c
# (R3; P1, P2 y AUD3-M2) hal/nor.c DE VERDAD (-DNOR_PRUEBA) contra un modelo del chip con tiempo, suspensión y violaciones:
# borrado suspendido que sobrevive a un reinicio, IRQ siempre pendiente, chip que tarda en avanzar tras reanudar, chip más
# lento que el tope, error de SR, chip que no es el de la placa (se lee, no se escribe), sin CFI, y programa en modo control.
# Y la NEGATIVA de los sondeos de gracia (-DNOR_SONDEOS_SIN_MIRAR=0). 
NOR_CASOS = modelo normal w6f w6f_susp irq avance suspendido rango_sanear sin_registro susp_ajeno susp_fuera otro_susp ocupado ocupado_siempre lento error otro_id sin_qry cfi_tam programa programa_irq
NOR_CC = gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -DNOR_PRUEBA -Ihal
$(B)/prueba_nor: herr/pruebas/prueba_nor.c hal/nor.c hal/hal.h hal/m18.h hal/nor_placa.h | $(B)
	$(NOR_CC) -o $@ herr/pruebas/prueba_nor.c hal/nor.c
	$(NOR_CC) -DNOR_SONDEOS_SIN_MIRAR=0 -o $@_sin_gracia herr/pruebas/prueba_nor.c hal/nor.c
probar-nor: $(B)/prueba_nor
	@mkdir -p evidencia/pc_auditoria_hal; { for c in $(NOR_CASOS); do $(B)/prueba_nor $$c || exit 1; done; $(B)/prueba_nor_sin_gracia avance || exit 1; echo "NOR (hal/nor.c contra el modelo): TODO BIEN"; } > evidencia/pc_auditoria_hal/nor.txt 2>&1; r=$$?; grep -v "^  bien" evidencia/pc_auditoria_hal/nor.txt; exit $$r
# (I1) hal/rtc.c DE VERDAD (-DRTC_PRUEBA) contra un modelo del RTC con el acarreo propagándose campo a campo: lecturas
# sembradas en todos los vuelcos (minuto, hora, día, mes, año, bisiestos), el camino corto lejos del vuelco (sin retardo),
# el oscilador parado, y la NEGATIVA (el camino corto aceptado recién volcado TIENE que dar una hora rota).
RTC_CC = gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -DRTC_PRUEBA -Ihal
$(B)/prueba_rtc: herr/pruebas/prueba_rtc.c hal/rtc.c hal/hal.h | $(B)
	$(RTC_CC) -o $@ herr/pruebas/prueba_rtc.c hal/rtc.c
	$(RTC_CC) -Wno-type-limits -DFRAC_LEJOS=0u -o $@_negativa herr/pruebas/prueba_rtc.c hal/rtc.c
	$(RTC_CC) -DFRAC_SALTO=128u -DRTC_UNA_LECTURA -o $@_negativa_pausas herr/pruebas/prueba_rtc.c hal/rtc.c
probar-rtc: $(B)/prueba_rtc
	@mkdir -p evidencia/pc_auditoria_hal; { $(B)/prueba_rtc && $(B)/prueba_rtc_negativa negativa && $(B)/prueba_rtc_negativa_pausas negativa_pausas; } > evidencia/pc_auditoria_hal/rtc.txt 2>&1; r=$$?; cat evidencia/pc_auditoria_hal/rtc.txt; exit $$r
probar-monticulo: $(B)/prueba_monticulo
	@mkdir -p evidencia/pc_auditoria_hal; $(B)/prueba_monticulo | tee evidencia/pc_auditoria_hal/monticulo.txt; tail -1 evidencia/pc_auditoria_hal/monticulo.txt | grep -q "TODO BIEN"
clean:
	rm -rf $(B)

# Las pruebas de PC que no necesitan nada de fuera de este árbol (unos minutos; dejan su salida en evidencia/). Probadas en macOS y Linux.
# Solo Linux (el sistema entero para el PC usa ensamblador en línea de ELF; ui.h choca con las cabeceras de Apple): pc, probar-fs, probar-monticulo.
# el contador de pasos (shell/podometro.c) con señales sintéticas, en el PC (CC del anfitrión)
probar-podometro: herr/pruebas/prueba_podometro.c shell/podometro.c shell/podometro.h
	@mkdir -p $(B)
	cc -O1 -Wall -Wextra -Ishell herr/pruebas/prueba_podometro.c shell/podometro.c -lm -o $(B)/prueba_podometro
	$(B)/prueba_podometro
probar-rapido: probar-placa probar-almacen probar-ms probar-ms-serie probar-usb probar-usb-puente probar-nor probar-rtc probar-i2c probar-carga puertas-tarjeta
	@echo "PRUEBAS RÁPIDAS DEL PC: TODAS BIEN"
.PHONY: all pc w580 bl bl-installer fs flash-apps emu paneles camara-tabla sin-escritura clean FORCE probar-rapido probar-placa probar-almacen \
        probar-fs probar-ms probar-ms-serie probar-usb probar-usb-puente probar-nor probar-rtc probar-i2c probar-carga probar-monticulo puertas-tarjeta
.SECONDARY:
