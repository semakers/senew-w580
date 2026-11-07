#!/usr/bin/env python3
"""Lee una zona de ficheros de usuario «W6U» (api/usuario.c) SIN usar el código del teléfono: otra implementación
del formato, en Python, para cotejar lo que el sistema dice haber guardado (pista S6 del W580).

  w6u_extraer.py IMAGEN [--base 0x1C00000] [--bloques 8] [--tam 131072] [--sacar DIR]

IMAGEN es o la zona sola (lo que escribe el PC con W600_USR=fichero: 8 x 128 KiB LÓGICOS) o una NOR entera volcada del
emulador (64 MiB; la zona está en 0x45C00000 - 0x44000000 = 0x1C00000: se detecta por el tamaño). En la NOR del
W580i (Intel M18) la zona va en MODO CONTROL (hal/m18.h): de cada segmento físico de 32 bytes solo valen los 16
primeros, así que 8 bloques de borrado de 256 KiB dan 8 bloques lógicos de 128 KiB; de un volcado se deshace aquí
(física = ((lógica * 2) & ~0x1F) + (lógica & 0xF)), también sin usar el código del teléfono.
Imprime una línea por fichero vigente: tamaño, CRC-32 (el del compromiso, comprobado contra los datos), nombre.
Con --sacar DIR deja cada fichero en DIR. Sale con 1 si algún fichero vigente no cuadra con su CRC.

El formato (resumen; entero en la cabecera de api/usuario.c): bloque = 'W6U1', secuencia, CRC de esos 8 bytes,
«retirado» (0xFFFFFFFF = en uso). Registro = marca u16 ('TZ' trozo, 'CM' compromiso, 'BR' borrado), n u16, tx u32,
a u32, CRC(carga) u32, CRC(cabecera) u32, carga (rellena a 4), y al final el SELLO = ~CRC(cabecera) & 0x7FFFFFFF.
Vale, para cada nombre, el último compromiso o borrado en orden de bloque (por secuencia) y posición."""
import os, struct, sys, zlib

def modo_control(fis):
    """Los bytes LÓGICOS de una zona física en modo control: los 16 primeros de cada segmento de 32."""
    return b''.join(fis[i:i + 16] for i in range(0, len(fis), 32))

def leer(img, nb, tb):
    blqs = []
    for b in range(nb):
        m = img[b * tb:(b + 1) * tb]
        magia, seq, crc, ret = struct.unpack_from('<IIII', m, 0)
        if magia == 0x31553657 and crc == zlib.crc32(m[:8]) and ret == 0xFFFFFFFF and seq != 0xFFFFFFFF:
            blqs.append((seq, b, m))
    blqs.sort()
    trozo_max = min(((tb - 16) // 8) & ~3, 4096)
    regs = []
    for seq, b, m in blqs:
        p = 16
        while p + 24 <= tb:
            marca, n, tx, a, crc, cc = struct.unpack_from('<HHIIII', m, p)
            tot = 20 + ((n + 3) & ~3) + 4
            ok = {0x5A54: 1 <= n <= trozo_max, 0x4D43: 9 < n <= 9 + 47, 0x5242: 1 < n <= 1 + 47}.get(marca, False)
            if ok and p + tot <= tb and cc == zlib.crc32(m[p:p + 16]):
                sello = struct.unpack_from('<I', m, p + tot - 4)[0]
                if sello == (~cc & 0x7FFFFFFF):
                    regs.append((marca, tx, a, m[p + 20:p + 20 + n]))
                p += tot
            else:
                p += 4
    return blqs, regs

def ficheros(regs):
    vig = {}
    for marca, tx, a, carga in regs:
        if marca == 0x4D43:
            fecha, crc, ln = struct.unpack_from('<IIB', carga, 0)
            vig[bytes(carga[9:9 + ln])] = (tx, a, fecha, crc)
        elif marca == 0x5242:
            vig.pop(bytes(carga[1:1 + carga[0]]), None)
    sal = []
    for nombre, (tx, tam, fecha, crc) in sorted(vig.items()):
        d = bytearray(tam)
        cub = 0
        tro = {}
        for marca, t, a, carga in regs:
            if marca == 0x5A54 and t == tx:
                tro[a] = carga                      # si está dos veces (una limpieza cortada), vale el último: son iguales
        for a, carga in tro.items():
            d[a:a + len(carga)] = carga; cub += len(carga)
        sal.append((nombre.decode('utf-8', 'replace'), tam, fecha, crc, bytes(d), cub == tam and zlib.crc32(bytes(d)) == crc))
    return sal

if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    img = open(a[0], 'rb').read()
    opc = lambda n, d: int(a[a.index(n) + 1], 0) if n in a else d
    nb, tb = opc('--bloques', 8), opc('--tam', 131072)
    base = opc('--base', 0x1C00000 if len(img) > nb * tb else 0)
    zona = img[base:base + nb * tb * (2 if len(img) > nb * tb else 1)]
    if len(img) > nb * tb:                                 # un volcado de la NOR: deshacer el modo control del M18
        zona = modo_control(zona)
    blqs, regs = leer(zona, nb, tb)
    fs = ficheros(regs)
    print('W6U en %s (+0x%X): %d bloques en el registro (secuencias %s), %d registros con sello, %d ficheros vigentes' % (
        os.path.basename(a[0]), base, len(blqs), ' '.join(str(s) for s, _, _ in blqs), len(regs), len(fs)))
    mal = 0
    for nombre, tam, fecha, crc, d, bien in fs:
        print('%8d  %08x  %s  %s' % (tam, crc, 'bien' if bien else 'MAL ', nombre))
        mal += not bien
        if '--sacar' in a:
            dd = a[a.index('--sacar') + 1]
            os.makedirs(dd, exist_ok=True)
            open(os.path.join(dd, nombre.replace('/', '_')), 'wb').write(d)
    sys.exit(1 if mal else 0)
