#!/usr/bin/env python3
"""(MD) Rasteriza Roboto (variable, OFL 1.1, github.com/google/fonts ofl/roboto) a mapas de 4 bits por píxel en la COMPILACIÓN.
Saca ui/dis_fuente_datos.c con los estilos de la escala tipográfica del sistema de diseño (docs/material.md §3).
   python3 herr/diseno/hacer_fuente.py RUTA/Roboto[wdth,wght].ttf ui/dis_fuente_datos.c
Los caracteres son EXACTAMENTE los de fuente_5x7 (ui/fuente.c: ASCII + los 28 de extra); el que Roboto no tenga queda con
w = 0 y avance = 0 y el pintor usa el 5x7 (así ← → ↑ ↓ ✓ siguen saliendo)."""
import sys
from PIL import ImageFont

# (nombre C, px del cuadro de la eme, peso, alto de la línea en px, línea base desde arriba)
# ESCALA B (estética, 26-oct; docs/estetica.md §4): la letra del original medida en el emulador es Roboto 23-24 (mayúscula 17);
# el mantenedor eligió, mirando maquetas en el teléfono, el cuerpo a 20 px (mayúscula 14, x-height 11). Alto y base = ascent + descent
# de Roboto a ese tamaño: caben las mayúsculas acentuadas y las descendentes sin recortar. La «pequena» (14 px, 500) es la talla
# en negrita del original para segundas líneas, la hora de la barra y las ayudas.
ESTILOS = [
    ('cuerpo',    20, 400, 24, 19),
    ('etiqueta',  20, 500, 24, 19),
    ('titulo',    22, 500, 27, 22),
    ('titular',   28, 400, 33, 27),
    ('display',   36, 400, 42, 34),
    ('display_g', 44, 300, 51, 41),
    ('pequena',   14, 500, 17, 13),
]
# LA ESCALA DE ANTES (v1), para las .sen de API < 1.7 (modo de compatibilidad): una línea a escala k mide lo MISMO que la del
# 5x7 (10*k), que es lo que esas apps dan por hecho al colocar las cosas. Sus nombres llevan «_v1».
ESTILOS_V1 = [
    ('cuerpo_v1',    10, 400, 10, 7),
    ('etiqueta_v1',  10, 500, 10, 7),
    ('titulo_v1',    13, 500, 13, 10),
    ('titular_v1',   20, 400, 20, 14),
    ('display_v1',   30, 400, 30, 21),
    ('display_g_v1', 40, 300, 40, 28),
]
EXTRA = [0xE1,0xE9,0xED,0xF3,0xFA,0xF1,0xD1,0xBF,0xA1,0xAB,0xBB,0xB0,None,0xFC,0xDC,0xC1,0xC9,0xCD,0xD3,0xDA,
         0x2190,0x2192,0x2191,0x2193,0x2713,0x2022,0xB7,0x2026]   # índices 95..122 de fuente_indice (107 = «?» de relleno)
CPS = list(range(32, 127)) + [c if c is not None else 0x3F for c in EXTRA]

def main(ttf, salida):
    o = ['/* GENERADO por herr/diseno/hacer_fuente.py: no editar. Roboto (c) The Roboto Project Authors, SIL OFL 1.1:',
         ' * docs/licencias/roboto_OFL.txt. 4 bits por píxel, filas a byte, nibble alto = izquierda. */',
         '#include "diseno.h"', '']
    total = 0
    for nombre, px, peso, alto, base in ESTILOS + ESTILOS_V1:
        f = ImageFont.truetype(ttf, px)
        try: f.set_variation_by_axes([peso, 100])   # ejes: Weight, Width
        except Exception as e: print('aviso: sin ejes', e)
        cmap = None
        datos = bytearray(); glifos = []
        for cp in CPS:
            ch = chr(cp)
            mask, off = f.getmask2(ch, mode='L', anchor='ls')
            w, h = mask.size
            adv = round(f.getlength(ch))
            # ¿lo tiene Roboto? Un .notdef sale como caja: comparamos con un punto de código que seguro no está
            if cp > 0x2000:
                m2, o2 = f.getmask2('￿', mode='L', anchor='ls')
                if m2.size == mask.size and bytes(m2) == bytes(mask): w = h = 0; adv = 0
            if cp == 32: w = h = 0
            dy = base + off[1]                         # off[1] es la fila de arriba respecto a la línea base (negativa)
            dx = off[0]
            px_ = [mask.getpixel((x, y)) for y in range(h) for x in range(w)] if w and h else []
            # recorte a la caja de la línea (las mayúsculas acentuadas pierden la punta del acento en los estilos pequeños)
            y0 = max(0, -dy); y1 = min(h, alto - dy)
            if w and h and y1 > y0:
                filas = [px_[y * w:(y + 1) * w] for y in range(y0, y1)]
                dy += y0; h = y1 - y0
            else:
                filas = []; h = 0 if not w else h
                if w and y1 <= y0: w = h = 0
            ini = len(datos)
            for fila in filas:
                q = [min(15, (v * 15 + 127) // 255) for v in fila] + [0]
                for i in range(0, w, 2): datos.append((q[i] << 4) | q[i + 1])
            glifos.append((w, h, dx, dy, adv, ini))
        total += len(datos) + len(glifos) * 8
        o.append(f'static const u8 d_{nombre}[{max(1,len(datos))}] = {{')
        for i in range(0, len(datos), 24): o.append('    ' + ','.join(str(b) for b in datos[i:i + 24]) + ',')
        o.append('};')
        o.append(f'static const DisGlifo g_{nombre}[{len(glifos)}] = {{')
        for i in range(0, len(glifos), 4):
            o.append('    ' + ' '.join('{%d,%d,%d,%d,%d,0,%d},' % g for g in glifos[i:i + 4]))
        o.append('};')
        o.append(f'const DisFuente dis_roboto_{nombre} = {{ {alto}, {base}, {len(glifos)}, g_{nombre}, d_{nombre} }};')
        o.append('')
        print(f'{nombre:10s} {px:2d} px peso {peso}: {len(datos):6d} B de mapas + {len(glifos)*8} B de tabla')
    open(salida, 'w').write('\n'.join(o) + '\n')
    print(f'TOTAL {total} B en flash ({total/1024:.1f} KiB), 0 B de RAM (const)')

main(sys.argv[1], sys.argv[2])
