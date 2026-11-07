#!/usr/bin/env python3
"""(1.2, APX) juntar_cargable.py cargable.h salida.h — mete los grupos api/cargable12_*.h DENTRO de cargable.h: cada
`#include "cargable12_x.h"` se sustituye por su contenido (los `#define W5_API12_X(F)` se quedan: el cuerpo de W5_API los cita
en su orden, y herr/hacer_api.py los lee en su sitio). Así cargable.h vuelve a ser UN fichero, el que comparte el SDK."""
import os, re, sys
ent, sal = sys.argv[1], sys.argv[2]
d = os.path.dirname(os.path.abspath(ent))
t = open(ent, encoding='utf-8').read()
grupos = {}
def incluir(m):
    f = open(os.path.join(d, m.group(1)), encoding='utf-8').read()
    g = re.search(r'#define (W5_API12_\w)\(F\)((?:[^\n]*\\\n)*[^\n]*)\n', f)
    cuerpo = g.group(2)
    lineas = [l.rstrip().rstrip('\\').rstrip() for l in cuerpo.split('\n')]
    grupos[g.group(1)] = [l for l in lineas if l.strip()]
    return f
t = re.sub(r'#include "(cargable12_\w\.h)"\n', incluir, t)
def expandir(m):
    out = []
    for nombre in re.findall(r'(W5_API12_\w)\(F\)', m.group(0)):
        out += ['    ' + l.strip() + ' \\' for l in grupos.get(nombre, [])]
    if out: out[-1] = out[-1][:-2]
    return '\n'.join(out) + '\n' if out else '\n'
t = t.replace('/* (1.2, en desarrollo: herr/juntar_cargable.py los mete aquí dentro al entregar) */\n', '')
open(sal, 'w', encoding='utf-8').write(t)
