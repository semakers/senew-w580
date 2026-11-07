#!/usr/bin/env python3
"""empaquetar_db2020.py — Produce una nueva DB2020.zip con nuestro db2020.bin dentro de executor.jar.

Flujo del exploit en el teléfono (lo que descubrimos el 2026-10-30):
  AMS carga executor.jar (MIDlet 'eXploit') + executor.b (CAFEB00B, el que valida AMS).
  La MIDlet carga /cid52/db2020.bin del JAR, lo mete en un byte[] y usa un bug del verificador JVM para
  saltar al contenido del byte[] como código ARM. Los primeros 0x7C bytes de db2020.bin son el bootstrap
  del exploit (Thumb + branch a 0x7C). Desde 0x7C empieza el código ARM real (apaga MMU, pila, programación
  de flash). Nuestro db2020.bin preserva los primeros 0x7C bytes del original y a partir de 0x7C mete
  nuestro trampoline + bl.bin (modo installer).

Uso:
  python3 -I herr/bl/empaquetar_db2020.py                 # construye todo
  python3 -I herr/bl/empaquetar_db2020.py --no-make       # solo empaquetar lo que haya en build/installer/bl/

Sale:
  build/installer/db2020.bin        — el nuevo (prefijo original + trampoline + bl.bin)
  build/installer/executor.jar      — JAR con nuestro db2020.bin dentro
  build/installer/DB2020.zip        — zip plataforma con el JAR nuevo + customize_upgrade.xml

El executor.b (CAFEB00B, el que valida AMS) NO SE TOCA: debe ser el original de SE.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

AQUI = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BL_B = os.path.join(AQUI, 'build', 'installer', 'bl')
SALIDA = os.path.join(AQUI, 'build', 'installer')
TRAMPOLINE_S = os.path.join(AQUI, 'plat', 'bl', 'trampoline.S')
CC = 'arm-none-eabi-gcc'
OBJCOPY = 'arm-none-eabi-objcopy'

# Prefijo del db2020.bin original (bytes 0x00..0x7B) — el bootstrap que el exploit JVM necesita.
DB2020_ORIG_ZIP = os.environ.get('DB2020_ORIG_ZIP', os.path.join(AQUI, '..', 'seftool-senew', 'anycid', 'DB2020.zip'))   # el DB2020.zip ORIGINAL (no se distribuye aquí)
PREFIJO_LEN = 0x7C   # desde 0x7C arranca el código ARM real (apaga MMU...)
EJECUTOR_JAR_RUTA = '/tpa/preset/custom/executor.jar'

MARCADORES = {
    0xDEAD0001: 'bl_offset',
    0xDEAD0002: 'itcm_off',
    0xDEAD0003: 'itcm_size',
    0xDEAD0004: 'data_off',
    0xDEAD0005: 'data_size',
    0xDEAD0006: 'bss_ini',
    0xDEAD0007: 'bss_fin',
    0xDEAD0008: 'pila_exc',
    0xDEAD0009: 'pila_svc',
    0xDEAD000A: 'bl_main',
}


def leer_mapa(map_path):
    valores = {}
    with open(map_path) as f:
        for line in f:
            for nombre in ('_itcm_lma', '_itcm_ini', '_itcm_fin', '_data_lma', '_data',
                           '_edata', '_bss', '_ebss', '_pila_exc', '_pila_svc'):
                m = re.search(rf'(0x[0-9a-fA-F]+)\s+{re.escape(nombre)}\b', line)
                if m:
                    valores[nombre] = int(m.group(1), 16)
            m = re.search(r'(0x[0-9a-fA-F]+)\s+bl_main\b', line)
            if m:
                valores['bl_main'] = int(m.group(1), 16)
    return valores


def compilar_trampoline(build_dir):
    os.makedirs(build_dir, exist_ok=True)
    obj = os.path.join(build_dir, 'trampoline.o')
    raw = os.path.join(build_dir, 'trampoline.bin')
    subprocess.check_call([CC, '-c', '-mcpu=arm926ej-s', '-marm', '-nostdlib',
                           '-o', obj, TRAMPOLINE_S])
    subprocess.check_call([OBJCOPY, '-O', 'binary', '-j', '.text', obj, raw])
    return open(raw, 'rb').read()


def rellenar_constantes(trampoline, valores_mapa, bl_offset):
    BL_INI = 0x44140000
    itcm_lma = valores_mapa['_itcm_lma']
    itcm_ini = valores_mapa['_itcm_ini']
    itcm_fin = valores_mapa['_itcm_fin']
    data_lma = valores_mapa['_data_lma']
    data_ini = valores_mapa['_data']
    edata = valores_mapa['_edata']

    reemplazos = {
        0xDEAD0001: bl_offset,
        0xDEAD0002: itcm_lma - BL_INI,
        0xDEAD0003: itcm_fin - itcm_ini,
        0xDEAD0004: data_lma - BL_INI,
        0xDEAD0005: edata - data_ini,
        0xDEAD0006: valores_mapa['_bss'],
        0xDEAD0007: valores_mapa['_ebss'],
        0xDEAD0008: valores_mapa['_pila_exc'],
        0xDEAD0009: valores_mapa['_pila_svc'],
        0xDEAD000A: valores_mapa['bl_main'],
    }

    result = bytearray(trampoline)
    for marcador, valor in reemplazos.items():
        patron = struct.pack('<I', marcador)
        idx = result.find(patron)
        if idx < 0:
            sys.exit(f'Marcador 0x{marcador:08X} ({MARCADORES[marcador]}) no encontrado')
        if result.find(patron, idx + 4) >= 0:
            sys.exit(f'Marcador 0x{marcador:08X} aparece más de una vez')
        struct.pack_into('<I', result, idx, valor & 0xFFFFFFFF)
        print(f'  {MARCADORES[marcador]:12s} = 0x{valor:08X}')

    return bytes(result)


def extraer_prefijo_original():
    """Saca los primeros PREFIJO_LEN bytes del db2020.bin original (dentro de executor.jar dentro de DB2020.zip)."""
    with zipfile.ZipFile(DB2020_ORIG_ZIP) as zdb:
        jar_bytes = zdb.read('tpa/preset/custom/executor.jar')
    import io
    with zipfile.ZipFile(io.BytesIO(jar_bytes)) as zj:
        bin_bytes = zj.read('cid52/db2020.bin')
    assert len(bin_bytes) >= PREFIJO_LEN
    return bin_bytes[:PREFIJO_LEN], jar_bytes


def armar_jar_nuevo(jar_bytes_orig, db2020_bin_nuevo, out_jar):
    """Reescribe el JAR original con nuestro db2020.bin en /cid52/db2020.bin."""
    import io
    buf = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(jar_bytes_orig), 'r') as zin, \
         zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == 'cid52/db2020.bin':
                data = db2020_bin_nuevo
            zout.writestr(item, data)
    open(out_jar, 'wb').write(buf.getvalue())


def armar_db2020_zip(jar_path, out_zip):
    """DB2020.zip con 2 ficheros: tpa/preset/custom/customize_upgrade.xml + tpa/preset/custom/executor.jar."""
    with zipfile.ZipFile(DB2020_ORIG_ZIP) as zorig:
        xml_bytes = zorig.read('tpa/preset/custom/customize_upgrade.xml')
    jar_bytes = open(jar_path, 'rb').read()
    with zipfile.ZipFile(out_zip, 'w', zipfile.ZIP_STORED) as zout:
        zout.mkdir('tpa/')
        zout.mkdir('tpa/preset/')
        zout.mkdir('tpa/preset/custom/')
        zout.writestr('tpa/preset/custom/customize_upgrade.xml', xml_bytes)
        zout.writestr('tpa/preset/custom/executor.jar', jar_bytes)


def main():
    hacer = '--no-make' not in sys.argv[1:]
    if hacer:
        cmd = ['make', 'bl', 'BL_DEFS=-DBL_INSTALLER=1', f'BL_B={BL_B}']
        print('>', ' '.join(cmd))
        r = subprocess.run(cmd, cwd=AQUI)
        if r.returncode:
            sys.exit(f'make falló (rc={r.returncode})')

    bl_bin = os.path.join(BL_B, 'bl.bin')
    bl_map = os.path.join(BL_B, 'bl.map')
    for f in (bl_bin, bl_map):
        if not os.path.exists(f):
            sys.exit(f'falta {f}')

    bl = open(bl_bin, 'rb').read()
    print(f'\nbl.bin: {len(bl)} B')

    # Extraer prefijo original de db2020.bin
    print(f'\nExtrayendo primeros 0x{PREFIJO_LEN:X} bytes del db2020.bin original...')
    prefijo, jar_orig = extraer_prefijo_original()
    print(f'prefijo: {len(prefijo)} B, sha256 primeros 16: {prefijo[:16].hex()}')

    # Compilar trampoline
    print('\nCompilando trampoline...')
    trampoline_raw = compilar_trampoline(os.path.join(SALIDA, 'bl'))
    if len(trampoline_raw) % 4:
        trampoline_raw += b'\x00' * (4 - len(trampoline_raw) % 4)
    print(f'trampoline: {len(trampoline_raw)} B')

    # Offset de bl.bin dentro del db2020.bin, relativo al INICIO DEL TRAMPOLINE
    # El trampoline arranca en 0x7C, y bl.bin le sigue inmediatamente.
    # Desde la perspectiva del trampoline: bl.bin está en +len(trampoline).
    bl_offset_rel_tramp = len(trampoline_raw)

    print('\nConstantes del trampoline:')
    vals = leer_mapa(bl_map)
    trampoline_filled = rellenar_constantes(trampoline_raw, vals, bl_offset_rel_tramp)

    # Armar db2020.bin nuevo: prefijo (0..0x7C) + trampoline + bl.bin
    db2020_nuevo = prefijo + trampoline_filled + bl
    db2020_path = os.path.join(SALIDA, 'db2020.bin')
    open(db2020_path, 'wb').write(db2020_nuevo)
    print(f'\ndb2020.bin nuevo: {len(db2020_nuevo)} B ({len(db2020_nuevo)/1024:.1f} KiB)')
    print(f'  layout: [0..{PREFIJO_LEN}] prefijo original | [{PREFIJO_LEN}..{PREFIJO_LEN+len(trampoline_filled)}] trampoline | [{PREFIJO_LEN+len(trampoline_filled)}..{len(db2020_nuevo)}] bl.bin')

    # Reempacar JAR
    jar_salida = os.path.join(SALIDA, 'executor.jar')
    armar_jar_nuevo(jar_orig, db2020_nuevo, jar_salida)
    print(f'executor.jar: {os.path.getsize(jar_salida)} B')

    # Armar DB2020.zip
    zip_salida = os.path.join(SALIDA, 'DB2020.zip')
    armar_db2020_zip(jar_salida, zip_salida)
    print(f'DB2020.zip:   {os.path.getsize(zip_salida)} B')

    print(f'\nSalida en: {SALIDA}')
    print(f'  Para usarlo: cp {zip_salida} SEFTOOL_ANYCID/DB2020.zip')
    print(f'  Y asegúrate que SEFTOOL_ANYCID/R8BE001_CXC1123362_GENERIC_LA.zip es el ORIGINAL (3692 B dentro)')


if __name__ == '__main__':
    main()
