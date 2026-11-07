#!/usr/bin/env python3
"""empaquetar_installer.py — Compila el BL con -DBL_INSTALLER=1 y lo empaqueta como executor.b del AnyCID.

El executor.b resultante tiene dos partes:
  1. TRAMPOLINE (position-independent, ARM): el exploit Java del AnyCID lo carga en SDRAM y salta al inicio.
     El trampoline copia las secciones del BL a ITCM/DTCM y salta a bl_main(2) = modo installer.
  2. BL.BIN: el bootloader installer completo, enlazado a 0x44140000.

El trampoline lee las constantes (offsets y tamaños de las secciones) de literals al final de su código,
que este script rellena a partir del mapa del BL (bl.map).

Sale un executor.b de 256 KiB (un bloque de NOR del M18), padded con 0xFF.

Uso:
  python3 -I herr/bl/empaquetar_installer.py            # make bl BL_DEFS=-DBL_INSTALLER=1 + empaquetar
  python3 -I herr/bl/empaquetar_installer.py --no-make  # solo empaquetar lo que haya en build/installer/bl/
"""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys

AQUI = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BL_B = os.path.join(AQUI, 'build', 'installer', 'bl')
SALIDA = os.path.join(AQUI, 'build', 'installer')
TOPE_HOLGURA = 128 * 1024
BLOQUE = 256 * 1024
PAQ_GEN = os.environ.get('PAQ_GEN', os.path.join(AQUI, 'build', 'installer', 'paquete'))   # dónde dejar el paquete (SENUS lo lleva en sus assets)
TRAMPOLINE_S = os.path.join(AQUI, 'plat', 'bl', 'trampoline.S')
CC = 'arm-none-eabi-gcc'
OBJCOPY = 'arm-none-eabi-objcopy'

# Marcadores para las constantes del trampoline (ver trampoline.S)
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
    """Lee las direcciones relevantes del bl.map."""
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
    """Compila trampoline.S y extrae el .text crudo."""
    os.makedirs(build_dir, exist_ok=True)
    obj = os.path.join(build_dir, 'trampoline.o')
    raw = os.path.join(build_dir, 'trampoline.bin')
    subprocess.check_call([CC, '-c', '-mcpu=arm926ej-s', '-marm', '-nostdlib',
                           '-o', obj, TRAMPOLINE_S])
    subprocess.check_call([OBJCOPY, '-O', 'binary', '-j', '.text', obj, raw])
    return open(raw, 'rb').read()


def rellenar_constantes(trampoline, valores_mapa, bl_offset):
    """Reemplaza los marcadores DEAD000x con los valores reales."""
    BL_INI = 0x44140000
    itcm_lma = valores_mapa['_itcm_lma']
    itcm_ini = valores_mapa['_itcm_ini']
    itcm_fin = valores_mapa['_itcm_fin']
    data_lma = valores_mapa['_data_lma']
    data_ini = valores_mapa['_data']
    edata = valores_mapa['_edata']

    reemplazos = {
        0xDEAD0001: bl_offset,
        0xDEAD0002: itcm_lma - BL_INI,         # offset de .itcm dentro de bl.bin
        0xDEAD0003: itcm_fin - itcm_ini,        # tamaño de .itcm
        0xDEAD0004: data_lma - BL_INI,           # offset de .data dentro de bl.bin
        0xDEAD0005: edata - data_ini,            # tamaño de .data
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
            sys.exit(f'Marcador 0x{marcador:08X} ({MARCADORES[marcador]}) no encontrado en trampoline')
        if result.find(patron, idx + 4) >= 0:
            sys.exit(f'Marcador 0x{marcador:08X} aparece más de una vez')
        struct.pack_into('<I', result, idx, valor & 0xFFFFFFFF)
        print(f'  {MARCADORES[marcador]:12s} = 0x{valor:08X}')

    return bytes(result)


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
    if not os.path.exists(bl_bin):
        sys.exit(f'no existe {bl_bin}; corre sin --no-make')
    if not os.path.exists(bl_map):
        sys.exit(f'no existe {bl_map}')

    with open(bl_bin, 'rb') as f:
        bl = f.read()
    print(f'\nbl.bin: {len(bl)} B')

    # Compilar trampoline
    print('\nCompilando trampoline...')
    trampoline_raw = compilar_trampoline(os.path.join(SALIDA, 'bl'))

    # Alinear el trampoline a 4 bytes
    if len(trampoline_raw) % 4:
        trampoline_raw += b'\x00' * (4 - len(trampoline_raw) % 4)
    bl_offset = len(trampoline_raw)
    print(f'trampoline: {len(trampoline_raw)} B, bl.bin empieza en offset 0x{bl_offset:X}')

    # Leer mapa y rellenar constantes
    print('\nConstantes del mapa:')
    vals = leer_mapa(bl_map)
    trampoline_filled = rellenar_constantes(trampoline_raw, vals, bl_offset)

    # Armar executor.b = trampoline + bl.bin + 0xFF hasta 256 KiB
    payload = trampoline_filled + bl
    if len(payload) > TOPE_HOLGURA:
        sys.exit(f'payload {len(payload)} B > tope {TOPE_HOLGURA} B')
    if len(payload) > BLOQUE:
        sys.exit(f'payload {len(payload)} B > bloque {BLOQUE} B')

    executor_b = payload + b'\xff' * (BLOQUE - len(payload))
    assert len(executor_b) == BLOQUE

    os.makedirs(SALIDA, exist_ok=True)
    ejecutor = os.path.join(SALIDA, 'executor.b')
    with open(ejecutor, 'wb') as f:
        f.write(executor_b)
    sha = hashlib.sha256(executor_b).hexdigest()

    # Copia los ficheros del AnyCID genérico
    copiados = []
    for nom in ('customize_upgrade.xml', 'executor.jar'):
        orig = os.path.join(PAQ_GEN, nom)
        if not os.path.exists(orig):
            print(f'  AVISO: {orig} no existe')
            continue
        dst = os.path.join(SALIDA, nom)
        shutil.copyfile(orig, dst)
        copiados.append((nom, os.path.getsize(dst)))

    print()
    print(f'trampoline:  {len(trampoline_filled):>8} B')
    print(f'bl.bin:      {len(bl):>8} B  (holgura {TOPE_HOLGURA - len(payload):>7} B)')
    print(f'executor.b:  {len(executor_b):>8} B  sha256 {sha}')
    for nom, tam in copiados:
        print(f'{nom:<13} {tam:>8} B')
    print(f'\nsalida en: {SALIDA}')


if __name__ == '__main__':
    main()
