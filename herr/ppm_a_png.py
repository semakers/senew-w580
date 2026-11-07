#!/usr/bin/env python3
"""PPM (P6) -> PNG, sin dependencias. Uso: ppm_a_png.py a.ppm [b.ppm ...] [--x N]; borra el .ppm."""
import os, struct, sys, zlib
esc = int(sys.argv[sys.argv.index('--x') + 1]) if '--x' in sys.argv else 1
for f in [a for a in sys.argv[1:] if a.endswith('.ppm')]:
    d = open(f, 'rb').read()
    p = d.split(b'\n', 3)
    w, h = map(int, p[1].split())
    px = p[3][:w * h * 3]
    filas = []
    for y in range(h):
        fila = px[y * w * 3:(y + 1) * w * 3]
        if esc > 1:
            fila = b''.join(fila[i:i + 3] * esc for i in range(0, len(fila), 3))
        filas += [b'\0' + fila] * esc
    ch = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(f[:-4] + '.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w * esc, h * esc, 8, 2, 0, 0, 0))
                                      + ch(b'IDAT', zlib.compress(b''.join(filas))) + ch(b'IEND', b''))
    os.remove(f)
