#!/usr/bin/env python3
"""Construye una imagen W6FS (nuestro sistema de ficheros de solo lectura, formato en api/w6fs.h) desde una carpeta.

  hacer_fs.py CARPETA salida.bin                 la imagen sola (para 0x44400000 o para el hueco del FS)
  hacer_fs.py CARPETA salida.bin --con-main main.bin
        un solo fichero «main + FS»: el main crudo (0x440A0000), relleno a 0xFF y el FS DONDE EL SISTEMA LO
        BUSCA: en el primer MiB libre detrás del final del main, y nunca por debajo de 0x44400000 (la misma
        cuenta que plat/hw_w600.c hace con _fin_flash: docs/sistema.md §11.7). Con el sistema solo (160 KB)
        eso es 0x44400000, como siempre; con una app externa de 9 MB dentro, 0x44A00000.
        Es lo que se sube al banco web como main (crudo, empieza por un salto ARM).
  hacer_fs.py CARPETA salida.bin --segmento      7 MiB justos (0x45800000..0x45EFFFFF), para subirlo como «FS»
  hacer_fs.py --listar imagen.bin                lista una imagen y comprueba todos los CRC
"""
import os, struct, sys, zlib
MAIN, FS_DIR, SEG = 0x440A0000, 0x44400000, 7 << 20
FIN_MAIN = 0x45600000                                  # hasta aquí llega el hueco del main (hal/enlace.ld); detrás van el almacén y la zona de usuario


def dir_fs(tam_main):
    """Dónde va el W6FS detrás de un main de ese tamaño: lo MISMO que calcula hwo_w6fs() en plat/hw_w600.c."""
    tras = (MAIN + tam_main + 0xFFFFF) & ~0xFFFFF
    return max(tras, FS_DIR)


def rec_ventana(d):
    """(M5X, api 1.1) Si `d` es un .sen que pide la VENTANA DE RECURSOS (W5_CAP_REC_VENTANA, sistema/api/cargable.h),
    el desplazamiento de sus recursos dentro del fichero; si no, None. El sistema los pone con la MMU en páginas de 4 KiB:
    tienen que empezar en una frontera de 4 KiB de la flash. La imagen del FS va siempre en un MiB o en un bloque de
    256 KiB (hwo_w6fs), así que basta con alinearlos dentro de la imagen."""
    if len(d) < 128 or d[:4] != b'W5X\x01': return None
    caps = struct.unpack_from('<I', d, 12)[0]
    if not caps & (1 << 10): return None
    cod, dat, _, _, _, _, nreub = struct.unpack_from('<7I', d, 24)
    return 128 + cod + dat + nreub * 4


ULTIMOS_RAIZ = ('apps',)
def construir(carpeta):
    ents = []                                          # [nombre, tipo, a, b, crc, datos]
    def dir_(ruta, nombre):
        i = len(ents); ents.append([nombre, 1, 0, 0, 0, None]); return i
    raiz = dir_(carpeta, '')
    cola = [(raiz, carpeta)]
    while cola:                                        # por niveles: los hijos de cada directorio quedan seguidos y tras él
        i, ruta = cola.pop(0)
        hijos = sorted(n for n in os.listdir(ruta) if not n.startswith('.'))
        if i == raiz:                                  # (R3) en la RAÍZ, lo del usuario primero (docs, fondos, imágenes, música) y «apps» AL FINAL:
            hijos.sort(key=lambda n: n in ULTIMOS_RAIZ)    # las apps ya tienen su sitio en el menú; el orden de la imagen es el que enseña Archivos
        ents[i][2], ents[i][3] = len(ents), len(hijos)
        for n in hijos:
            r = os.path.join(ruta, n)
            assert len(n.encode()) < 64, 'nombre demasiado largo: ' + n
            if os.path.isdir(r):
                cola.append((dir_(r, n), r))
            else:
                d = open(r, 'rb').read(); ents.append([n, 0, 0, len(d), zlib.crc32(d), d])
    nombres, pos_nom = b'', []
    for e in ents:
        pos_nom.append(len(nombres)); nombres += e[0].encode() + b'\0'
    nombres += b'\0' * (-len(nombres) % 4)
    tabla = 32; off_nom = tabla + 20 * len(ents); pos = off_nom + len(nombres)
    datos = b''
    primero = True
    for e in ents:
        if e[1] == 0:
            ri = rec_ventana(e[5])                     # (M5X, api 1.1) los recursos de una app con ventana, a 4 KiB en la flash
            es_app = e[0].endswith('.sen') and not primero
            if ri is not None:
                assert not primero, 'una app con ventana de recursos no puede ser el primer fichero del FS'
                datos += b'\xff' * (-(pos + len(datos) + ri) % 4096)
            elif es_app:                               # (AUD9) toda .sen empieza en página propia (la MMU abre por páginas)
                datos += b'\xff' * (-(pos + len(datos)) % 4096)
            e[2] = pos + len(datos) if e[3] else pos + len(datos)
            datos += e[5] + b'\xff' * (-len(e[5]) % 4)
            if es_app:                                 # (AUD9, fuga del borde) y acaba en página propia: lo que la MMU abre de su
                datos += b'\xff' * (-(pos + len(datos)) % 4096)   # última página es relleno 0xFF, no otro fichero
            primero = False
    t = b''.join(struct.pack('<5I', pos_nom[i], e[2], e[3], e[4], e[1]) for i, e in enumerate(ents))
    total = pos + len(datos)
    cab = struct.pack('<4s6I', b'W6FS', 1, len(ents), total, tabla, off_nom, zlib.crc32(t + nombres))
    cab += struct.pack('<I', zlib.crc32(cab))
    return cab + t + nombres + datos, ents


def listar(img):
    assert img[:4] == b'W6FS' and zlib.crc32(img[:28]) == struct.unpack_from('<I', img, 28)[0], 'cabecera mala'
    _, n, total, tabla, off_nom, _ = struct.unpack_from('<6I', img, 4)
    def ent(i): return struct.unpack_from('<5I', img, tabla + 20 * i)
    def nom(i): o = off_nom + ent(i)[0]; return img[o:img.index(b'\0', o)].decode()
    mal = 0
    def rec(i, pre):
        nonlocal mal
        _, a, b, crc, tipo = ent(i)
        if tipo == 1:
            for k in range(a, a + b): rec(k, pre + nom(i) + '/' if i else '/')
        else:
            ok = zlib.crc32(img[a:a + b]) == crc; mal += not ok
            print('%8d  %s  %s%s' % (b, 'bien' if ok else 'CRC MALO', pre, nom(i)))
    rec(0, '')
    print('%d entradas, %d bytes, %d CRC malos' % (n, total, mal))
    return mal


if __name__ == '__main__':
    if sys.argv[1] == '--listar':
        sys.exit(1 if listar(open(sys.argv[2], 'rb').read()) else 0)
    img, ents = construir(sys.argv[1])
    if '--con-main' in sys.argv:
        m = open(sys.argv[sys.argv.index('--con-main') + 1], 'rb').read()
        d = dir_fs(len(m))
        if d + len(img) > FIN_MAIN:
            sys.exit('no cabe: el main acaba en 0x%08X, el FS iría en 0x%08X y con sus %d bytes pasaría de 0x%08X'
                     % (MAIN + len(m), d, len(img), FIN_MAIN))
        print('main de %d bytes (hasta 0x%08X): el W6FS va en 0x%08X' % (len(m), MAIN + len(m), d))
        img = m + b'\xff' * (d - MAIN - len(m)) + img
    elif '--segmento' in sys.argv:
        assert len(img) <= SEG; img += b'\xff' * (SEG - len(img))
    open(sys.argv[2], 'wb').write(img)
    print('%s: %d bytes, %d entradas (%d ficheros)' % (sys.argv[2], len(img), len(ents), sum(1 for e in ents if e[1] == 0)))
