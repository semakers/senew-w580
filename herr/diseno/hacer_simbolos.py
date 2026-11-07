#!/usr/bin/env python3
"""(MD) Rasteriza SOLO los símbolos que usa el sistema de Material Symbols Rounded (Apache 2.0,
github.com/google/material-design-icons, variablefont/) a 4 bits por píxel, en la compilación. Saca ui/dis_simbolos_datos.c.
   python3 herr/diseno/hacer_simbolos.py RUTA/MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf RUTA/….codepoints ui/dis_simbolos_datos.c
El ORDEN de la lista es el de enum DIS_S_* de ui/diseno.h (los nombres de aquí son los de Material; los de allí, los nuestros)."""
import sys
from PIL import ImageFont

SIMBOLOS = ['alarm', 'lock', 'power', 'music_note', 'bolt', 'folder', 'description', 'directions_walk',
            'arrow_back', 'arrow_forward', 'arrow_upward', 'arrow_downward', 'check',
            # (segunda vuelta) los de las apps del menú
            'folder_open', 'image', 'schedule', 'sticky_note_2', 'calculate', 'sports_esports', 'terminal', 'keyboard',
            'memory', 'settings', 'call', 'headphones', 'vibration']
TAMS = (12, 24)             # 12: la barra de estado (sh_iconos, 12x12); 24: el icono a escala 2 (la rejilla del menú)

def una(ttf, cp, TAM):
    f = ImageFont.truetype(ttf, TAM)
    f.set_variation_by_axes([1, 0, 20, 500])     # relleno, grado 0, tamaño óptico 20 (el menor), peso 500: lo que mejor aguanta 12 px
    datos = bytearray(); gl = []
    for n in SIMBOLOS:
        ch = chr(int(cp[n], 16))
        m, off = f.getmask2(ch, mode='L')
        w, h = m.size
        # centrado en la celda de TAM x TAM
        bx, by = f.getbbox(ch)[:2]
        dx = (TAM - w) // 2; dy = (TAM - h) // 2
        ini = len(datos)
        for y in range(h):
            fila = [min(15, (m.getpixel((x, y)) * 15 + 127) // 255) for x in range(w)] + [0]
            for x in range(0, w, 2): datos.append((fila[x] << 4) | fila[x + 1])
        gl.append((w, h, dx, dy, TAM, ini))
    o = ['static const u8 d%d[%d] = {' % (TAM, len(datos))]
    for i in range(0, len(datos), 24): o.append('    ' + ','.join(map(str, datos[i:i + 24])) + ',')
    o.append('};')
    o.append('static const DisGlifo g%d[%d] = {' % (TAM, len(gl)))
    for n, x in zip(SIMBOLOS, gl): o.append('    {%d,%d,%d,%d,%d,0,%d},   /* %s */' % (x + (n,)))
    o.append('};')
    o.append('const DisFuente dis_simbolos_%d = { %d, %d, %d, g%d, d%d };' % (TAM, TAM, TAM, len(gl), TAM, TAM))
    print('%d px: %d símbolos, %d B de mapas + %d B de tabla' % (TAM, len(gl), len(datos), 8 * len(gl)))
    return o

def main(ttf, cps, salida):
    cp = dict(l.split() for l in open(cps) if l.strip())
    o = ['/* GENERADO por herr/diseno/hacer_simbolos.py: no editar. Material Symbols Rounded (c) Google, Apache 2.0:',
         ' * docs/licencias/material_symbols_LICENSE.txt. */', '#include "diseno.h"']
    for t in TAMS: o += una(ttf, cp, t)
    open(salida, 'w').write('\n'.join(o) + '\n')

main(*sys.argv[1:4])
