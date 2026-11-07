#!/usr/bin/env python3
"""Genera el CONTENIDO PROPIO del sistema de ficheros (carpeta fs_propio/): fondos e imágenes dibujados por
programa (nada de terceros: ni fotos, ni logotipos, ni arte del FS de Sony Ericsson) y los textos.
Uso: hacer_contenido.py CARPETA"""
import math, os, sys
from PIL import Image, ImageDraw
D = sys.argv[1]
W, H = 240, 320                # el panel del W580i (en el W600 eran 176 x 220: los dibujos se escalan con X() e Y())
def X(v): return v * W / 176.0
def Y(v): return v * H / 220.0
for d in ('fondos', 'imagenes', 'docs'):
    os.makedirs(os.path.join(D, d), exist_ok=True)

def mezcla(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

# --- Amanecer: cielo en degradado, sol y tres cordilleras
im = Image.new('RGB', (W, H)); px = im.load()
for y in range(H):
    c = mezcla((20, 24, 70), (250, 140, 60), min(1, y / Y(150))) if y < Y(150) else (250, 140, 60)
    for x in range(W): px[x, y] = c
dr = ImageDraw.Draw(im)
dr.ellipse((X(88) - Y(30), Y(92), X(88) + Y(30), Y(152)), fill=(255, 226, 120))
for k, (col, base, amp, f) in enumerate((((120, 60, 90), 150, 14, 0.045), ((70, 36, 70), 170, 18, 0.03), ((26, 16, 40), 192, 12, 0.06))):
    dr.polygon([(0, H)] + [(x, Y(base) + Y(amp) * math.sin(x / X(1) * f + k * 1.7) + Y(6) * math.sin(x / X(1) * f * 2.3 + k)) for x in range(W + 1)] + [(W, H)], fill=col)
im.save(os.path.join(D, 'fondos', 'Amanecer.png'), optimize=True)

# --- Olas: bandas senoidales en azules (PNG con paleta)
im = Image.new('RGB', (W, H)); px = im.load()
for y in range(H):
    for x in range(W):
        v = math.sin(x / X(1) * 0.05 + math.sin(y / Y(1) * 0.035) * 3) + math.sin((x / X(1) + y / Y(1)) * 0.03)
        px[x, y] = mezcla((6, 30, 70), (80, 200, 230), (v + 2) / 4)
im.quantize(48).save(os.path.join(D, 'fondos', 'Olas.png'), optimize=True)

# --- Noche: cielo oscuro con estrellas (posiciones por un generador congruencial) y luna
im = Image.new('RGB', (W, H)); px = im.load()
for y in range(H):
    for x in range(W): px[x, y] = mezcla((4, 6, 20), (24, 30, 70), y / H)
s = 7
for _ in range(250):
    s = (s * 1103515245 + 12345) & 0x7FFFFFFF; x = (s >> 8) % W
    s = (s * 1103515245 + 12345) & 0x7FFFFFFF; y = (s >> 8) % H
    s = (s * 1103515245 + 12345) & 0x7FFFFFFF; b = 120 + (s >> 8) % 136
    px[x, y] = (b, b, min(255, b + 20))
dr = ImageDraw.Draw(im)
dr.ellipse((X(116), Y(28), X(116) + Y(34), Y(62)), fill=(236, 236, 210)); dr.ellipse((X(126), Y(24), X(126) + Y(32), Y(56)), fill=(10, 13, 34))
im.save(os.path.join(D, 'fondos', 'Noche.png'), optimize=True)

# --- Plasma: JPEG (4:2:0) con un campo de senos
im = Image.new('RGB', (W, H)); px = im.load()
for y in range(H):
    for x in range(W):
        v = math.sin(x / X(1) * 0.06) + math.sin(y / Y(1) * 0.045) + math.sin((x / X(1) + y / Y(1)) * 0.04) + math.sin(math.hypot(x / X(1) - 88, y / Y(1) - 110) * 0.07)
        px[x, y] = (int(128 + 127 * math.sin(v * 1.6)), int(128 + 127 * math.sin(v * 1.6 + 2.1)), int(128 + 127 * math.sin(v * 1.6 + 4.2)))
im.save(os.path.join(D, 'fondos', 'Plasma.jpg'), quality=88, subsampling=2)

# --- Rejilla: PNG con transparencia (se mezcla sobre negro) y GIF animado «Pulso»
im = Image.new('RGBA', (W, H), (0, 0, 0, 0)); dr = ImageDraw.Draw(im)
for i in range(0, W, 16): dr.line((i, 0, i, H), fill=(255, 120, 0, 90))
for j in range(0, H, 16): dr.line((0, j, W, j), fill=(255, 120, 0, 90))
dr.ellipse((W / 2 - 40, H / 2 - 40, W / 2 + 40, H / 2 + 40), fill=(255, 140, 20, 200)); dr.ellipse((W / 2 - 24, H / 2 - 24, W / 2 + 24, H / 2 + 24), fill=(20, 20, 30, 255))
im.save(os.path.join(D, 'imagenes', 'Rejilla (alfa).png'), optimize=True)
cuadros = []
for k in range(8):
    f = Image.new('P', (96, 96), 0); f.putpalette([8, 10, 24, 255, 120, 0, 255, 220, 120, 60, 70, 110] + [0] * (252 * 3))
    dr = ImageDraw.Draw(f); r = 6 + k * 5
    dr.ellipse((48 - r, 48 - r, 48 + r, 48 + r), outline=1, width=3)
    if r > 14: dr.ellipse((48 - r + 12, 48 - r + 12, 48 + r - 12, 48 + r - 12), outline=3, width=2)
    dr.ellipse((44, 44, 52, 52), fill=2)
    cuadros.append(f)
cuadros[0].save(os.path.join(D, 'imagenes', 'Pulso.gif'), save_all=True, append_images=cuadros[1:], duration=90, loop=0)
# --- una imagen grande (640x480) para ver la reducción al decodificar
im = Image.new('RGB', (640, 480)); px = im.load()
for y in range(480):
    for x in range(640):
        a = ((x // 40) + (y // 40)) & 1
        px[x, y] = mezcla((230, 230, 220), (40, 90, 160), a * 0.85 + 0.15 * math.sin(x * 0.02) ** 2)
ImageDraw.Draw(im).ellipse((220, 140, 420, 340), outline=(255, 120, 0), width=14)
im.save(os.path.join(D, 'imagenes', 'Grande 640x480.jpg'), quality=85)

open(os.path.join(D, 'docs', 'LEEME.txt'), 'w', encoding='utf-8').write(
"""Sistema del W580 - sistema de ficheros propio (W6FS)

Este contenido es nuestro: los fondos y las imagenes estan dibujados por programa
(herr/hacer_contenido.py) y la imagen del sistema de ficheros la construye herr/hacer_fs.py
a partir de la carpeta fs_propio/.

/fondos     fondos de pantalla (PNG y JPEG de 240 x 320)
/imagenes   imagenes de prueba: PNG con alfa, GIF animado, JPEG grande (se ve reducido)
/docs       estos textos
/original   el sistema de ficheros de fabrica del telefono, si esta en la flash (solo lectura)

Lo que cambia (ajustes, notas, alarma) no vive aqui sino en el almacen persistente de la NOR.
""")
open(os.path.join(D, 'docs', 'teclas.txt'), 'w', encoding='utf-8').write(
"""Teclas
sel. izq.  accion principal u Opciones
sel. der.  volver
OK         activar
C          borrar o volver
En el menu: 1..9 * 0 # abren las 12 entradas.
En Ajustes: izq./der. cambian la zona horaria y el brillo.
En el editor de notas: # cambia abc/Abc/ABC/123, mantener un digito lo escribe.
""")
print('contenido generado en', D)
