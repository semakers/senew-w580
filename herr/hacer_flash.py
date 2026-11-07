#!/usr/bin/env python3
"""Mete una imagen cruda enlazada en MAIN_INI (0x44180000 desde v19; del Makefile, en el entorno) en una NOR plana de 64 MiB
(base 0x44000000) para la máquina w580 de QEMU de la pista E («-bios»).
(BL, 2026-10-23) El emulador entra, como el cargador de fábrica, en BL_INI (0x44140000): ahí va el bootloader propio
(--bl build/bl/bl.bin) o, sin --bl, un TRAMPOLÍN que solo salta al main (8 vectores que van a los del main + el bloque EMIF
del main en +0x40). El trampolín es SOLO del emulador: así cualquier guion que se haga su flash con otro main sigue arrancando.
Con MAIN_INI=0x44140000 (un main de antes) no hay nada delante.
*** SOLO PARA EL EMULADOR. NO es una imagen para un teléfono: grabada entera borraría el cargador y el GDFS.
*** Lo que iría a un aparato lo hace herr/hacer_aparato.py («make w580-aparato»).
Uso: hacer_flash.py main.bin flash.bin [--fs fs_w6fs.bin] [--w6f memoria.bin] [--base flash_anterior.bin] [--fabrica region_45e00000.bin]
Todo lo demás queda a 0xFF: ni el main original, ni el FS de fábrica, ni GDFS (nada de Sony Ericsson en nuestras
imágenes), SALVO con --fabrica, que pone en 0x45E00000 la región cruda del FS de fábrica (la que saca
tools/unbabe_regiones.py del .fbn del propio teléfono) para probar el lector de api/fsfab.c: esa flash va a tmp/. --fs pone NUESTRO sistema de ficheros (W6FS) en 0x44400000 (o en el primer MiB libre tras el main).
--base conserva las ZONAS ESCRIBIBLES (desde I1: W6F_INI..0x45DFFFFF = la Memoria (W6F), el almacén y los ficheros de usuario)
de una flash volcada antes (propiedad volcar-nor del emulador): así se prueba «apagar y encender».
(I1) --w6f pone en W6F_INI (0x44500000 por omisión; del Makefile, en el entorno) la imagen de la Memoria YA FORMATEADA que hace
herr/w6f_imagen.c con el MISMO api/w6f.c (como sale «de fábrica»: así cada arranque del emulador no gasta 13–18 s en darle
formato). Sin --w6f la zona queda en blanco: el sistema le da formato él solo (con su pantalla), que es lo que se prueba aparte."""
import os, sys
ESCR_INI = int(os.environ.get('W6F_INI', '0x44500000'), 16)    # (I1) las zonas escribibles empiezan ahora en la de W6F
MAIN = int(os.environ.get('MAIN_INI', '0x44180000'), 16)        # (BL) del Makefile, en el entorno
MAIN_FIN = int(os.environ.get('MAIN_FIN', '0x443C0000'), 16)
BL_INI = 0x44140000
BASE, TAM, ESCR_FIN, W6FS_MIN = 0x44000000, 0x04000000, 0x45E00000, 0x44400000
W6F_FIN = 0x45B00000
a = sys.argv[1:]
img = open(a[0], 'rb').read()
f = bytearray(b'\xff') * TAM
assert MAIN + len(img) <= ESCR_INI, 'la imagen no cabe por debajo de las zonas escribibles'
# (BL) la dirección de «reset» (la primera de la tabla de +0x20) tiene que caer dentro de la imagen: un main enlazado en otro sitio
# (p. ej. uno de antes de v19, en 0x44140000) puesto aquí saltaría a la nada
_reset = int.from_bytes(img[0x20:0x24], 'little')
assert MAIN <= _reset < MAIN + len(img), 'el main está enlazado en otra dirección (reset = 0x%08X, MAIN_INI = 0x%08X)' % (_reset, MAIN)
f[MAIN - BASE:MAIN - BASE + len(img)] = img
nota = ''
if MAIN != BL_INI:
    assert MAIN + len(img) <= MAIN_FIN, 'el main pasa de MAIN_FIN (0x%08X): el bootloader no lo podría grabar' % MAIN_FIN
    if '--bl' in a:
        bl = open(a[a.index('--bl') + 1], 'rb').read()
        assert len(bl) <= MAIN - BL_INI, 'el bootloader no cabe en su bloque'
        nota += '; bootloader de %d bytes en 0x%08X' % (len(bl), BL_INI)
    else:
        bl = bytearray(b'\xff') * 0xB0
        for i in range(8):
            bl[4 * i:4 * i + 4] = (0xE59FF018).to_bytes(4, 'little')               # ldr pc, [pc, #0x18]
            bl[0x20 + 4 * i:0x24 + 4 * i] = (MAIN + 4 * i).to_bytes(4, 'little')    # al vector i del main
        bl[0x40:0xB0] = img[0x40:0xB0]                                              # el bloque EMIF, el del main
        nota += '; trampolín de emulador en 0x%08X' % BL_INI
    f[BL_INI - BASE:BL_INI - BASE + len(bl)] = bl
if '--fs' in a:
    fs = open(a[a.index('--fs') + 1], 'rb').read()
    donde = max(W6FS_MIN, (MAIN + len(img) + 0xFFFFF) & ~0xFFFFF)
    if donde + 0x100000 > ESCR_INI:      # (I1) la misma regla que plat/hw_w580.c (hwo_w6fs): si el megabyte no cabe, el primer bloque libre
        donde = (MAIN + len(img) + 0x3FFFF) & ~0x3FFFF
    assert donde >= W6FS_MIN and donde + len(fs) <= ESCR_INI, 'el FS no cabe'
    f[donde - BASE:donde - BASE + len(fs)] = fs
    nota += '; W6FS de %d bytes en 0x%08X' % (len(fs), donde)
# (I1) Sin --w6f, si junto al main está la imagen de la Memoria ya formateada que hace el Makefile (B/w6f_vacio.bin), se pone esa:
# es lo que lleva build/flash-w580.bin, y así las flashes que los guiones hacen por su cuenta con otro main (las de S7, R2, R3)
# no arrancan con la Memoria en blanco (13-18 s de formato al llegar al standby, con los guiones pulsando teclas encima).
# --w6f-blanco la deja en blanco a propósito (la pasada de W6F prueba así el formato del primer arranque).
_vacio = os.path.join(os.path.dirname(os.path.abspath(a[0])), 'w6f_vacio.bin')
if '--w6f' not in a and '--w6f-blanco' not in a and os.path.exists(_vacio):
    a += ['--w6f', _vacio]
if '--w6f' in a:
    w = open(a[a.index('--w6f') + 1], 'rb').read()
    assert len(w) == W6F_FIN - ESCR_INI, 'la imagen de la Memoria no mide la zona de W6F (%d)' % len(w)
    f[ESCR_INI - BASE:W6F_FIN - BASE] = w
    nota += '; Memoria (W6F) formateada en 0x%08X' % ESCR_INI
if '--base' in a:
    b = open(a[a.index('--base') + 1], 'rb').read()
    assert len(b) == TAM
    f[ESCR_INI - BASE:ESCR_FIN - BASE] = b[ESCR_INI - BASE:ESCR_FIN - BASE]
    nota += '; zonas escribibles de ' + a[a.index('--base') + 1]
if '--fabrica' in a:
    r = open(a[a.index('--fabrica') + 1], 'rb').read()
    FAB, FAB_FIN = 0x45E00000, 0x47E00000
    assert FAB + len(r) <= FAB_FIN, 'la región de fábrica no cabe en su segmento'
    f[FAB - BASE:FAB - BASE + len(r)] = r
    nota += '; FS de fábrica de %d bytes en 0x%08X' % (len(r), FAB)
open(a[1], 'wb').write(f)
print('%s: main de %d bytes en 0x%08X%s' % (a[1], len(img), MAIN, nota))
