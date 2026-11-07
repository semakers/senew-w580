#!/usr/bin/env python3
"""Genera hal/lcd_paneles.h a partir del fichero de configuración de pantalla del propio teléfono
(/ifs/settings/display/dispdriver0.dat; la copia limpia de la pista H2 es work/h2/dispdriver0_limpio.txt).

  hacer_paneles.py [dispdriver0_limpio.txt] > hal/lcd_paneles.h
  hacer_paneles.py --texto [fichero]        las transferencias de cada guion, una por línea (para cotejar con una traza)

El fichero trae cinco descriptores { … }. El primero («SWRESET sequence only») no casa con ningún panel: da el reset
por software y el microcódigo del PDI. Los otros cuatro son el mismo cristal de 240x320 con distinto fabricante; los
tres primeros se eligen por el registro 0x0028 del panel (`reg="…" 0028 2 000f 1 VALOR`: (R28 & 0x000F) == VALOR) y
el último no trae `reg` [inferido: es el de «si ninguno casa»].

Cómo se trocean los guiones en TRANSFERENCIAS del PDI (lo que hace el original, [observado] en la traza e23 de la
pista E, work/emu/trazas/e23.qemu.log):
  - `$..$` son bytes de ORDEN (RS = 0), lo demás bytes de DATO (RS = 1); `%wNN` espera NN (hex) ms;
  - una transferencia lleva como mucho 24 bytes y no parte un grupo «orden + sus datos»;
  - `%w` y `%d` cierran la transferencia en curso;
  - lo que venga detrás de un `%w` SIN un `%d` delante NO SE MANDA: el final de standby_from_normal
    (`%w1E $00 10$ 00 00`) no sale nunca por el PDI en la traza del original.
Formato de un guion en C: [n, rs0, rs1, rs2, rs3, n bytes] por transferencia (rs = el valor de PDI+11C, little-endian),
[0xFE, ms] una espera, [0xFF] fin."""
import re, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
if not args: sys.exit('uso: hacer_paneles.py RUTA/dispdriver0.dat > hal/lcd_paneles.h   (el fichero de pantalla de TU teléfono)')
FICH = args[0]
TEXTO = '--texto' in sys.argv


def lineas(ruta):
    """Las líneas lógicas: el .dat del teléfono (CRLF, cada sentencia acaba en «;» y puede ocupar varias líneas) o una copia ya
    limpia (una por línea, con o sin el prefijo «NNN:» del número de línea)."""
    t = open(ruta, encoding='latin-1').read().replace('\r', '')
    if not (';\n' in t and not re.match(r'\s*\d+:', t)):
        return t.split('\n')
    out, acum = [], []
    for l in t.split('\n'):
        s = l.strip()
        if not acum and s in ('{', '}', ''):
            if s: out.append(s)
            continue
        acum.append(s)
        if s.endswith(';'):
            out.append(' '.join(acum)); acum = []
    if acum: out.append(' '.join(acum))
    return out


def descriptores(ruta):
    out, cur = [], None
    for l in lineas(ruta):
        l = re.sub(r'^\s*\d+:\s?', '', l.rstrip('\n'))
        s = l.split('#')[0].strip() if not l.strip().startswith('#') else ''
        s = s.rstrip(';').strip() if s in ('{;', '};') else s
        if s == '{':
            cur = {}
        elif s == '}':
            out.append(cur); cur = None
        elif cur is not None and '=' in s:
            k, v = s.split('=', 1)
            cur[k.strip()] = v.strip().rstrip(';').strip()
    return out


def guion(texto):
    """-> lista de ('T', bytes, rs) y ('W', ms)."""
    ops, grupos, grupo = [], [], None       # grupo = [(byte, es_dato), …]
    en_orden, activo = False, True          # activo = hay un %d vigente (tras un %w hace falta otro)

    def cerrar():
        nonlocal grupos, grupo
        if grupo:
            grupos.append(grupo)
        grupo = None
        trozo = []
        for g in grupos:
            if trozo and len(trozo) + len(g) > 24:
                ops.append(('T', trozo)); trozo = []
            trozo = trozo + g
        if trozo:
            ops.append(('T', trozo))
        grupos = []

    i, n = 0, len(texto)
    while i < n:
        c = texto[i]
        if c in ' \t':
            i += 1
        elif c == '$':
            en_orden = not en_orden
            if en_orden and activo:          # empieza un grupo nuevo (orden + sus datos)
                if grupo:
                    grupos.append(grupo)
                grupo = []
            i += 1
        elif c == '%':
            if texto[i + 1] == 'd':
                cerrar(); activo = True; i += 2
            elif texto[i + 1] == 'w':
                m = re.match(r'[0-9a-fA-F]+', texto[i + 2:])
                cerrar(); ops.append(('W', int(m.group(0), 16))); activo = False
                i += 2 + len(m.group(0))
            else:
                raise ValueError('escape desconocido en ' + texto[i:i + 12])
        else:
            m = re.match(r'[0-9a-fA-F]{2}', texto[i:])
            if not m:
                raise ValueError('no entiendo ' + texto[i:i + 12])
            if activo:
                if grupo is None:
                    grupo = []
                grupo.append((int(m.group(0), 16), not en_orden))
            i += 2
    cerrar()
    return ops


def rs_de(trozo):
    r, n = 0, len(trozo)
    for k, (_, dato) in enumerate(trozo):
        if dato:
            r |= 1 << (32 - n + k)
    return r


def a_c(ops):
    b = []
    for o in ops:
        if o[0] == 'W':
            assert 0 < o[1] < 256
            b += [0xFE, o[1]]
        else:
            t = o[1]; rs = rs_de(t)
            b += [len(t), rs & 255, rs >> 8 & 255, rs >> 16 & 255, rs >> 24] + [x for x, _ in t]
    return b + [0xFF]


def hexs(b):
    return ','.join('0x%02X' % x for x in b)


D = descriptores(FICH)
assert len(D) == 5 and D[0]['name'].startswith('SWRESET'), [d.get('name') for d in D]
PGM = {}
for k in ('pdipgmwrite', 'pdipgmgc', 'pdipgmreadhigh', 'pdipgmread'):
    v = [d[k].lower() for d in D]
    assert len(set(v)) == 1, k + ': el microcódigo no es el mismo en los cinco descriptores'
    PGM[k] = [0 if t.startswith('%cnt') else int(t, 16) for t in v[0].split()]
for k in ('pdirs', 'pdinibbleswap', 'pdipgmgcspeed', 'pdipgmwritespeed', 'pdipgmreadspeed', 'pdidataarea', 'pdidisplayreg',
          'pdidisplaycolour1', 'pdidisplaycolour2', 'pdidisplaycolour3', 'pdidisplaycolour4', 'pdidisplaycolour5', 'pdidisplaycolour6'):
    assert len({d[k] for d in D}) == 1, k
for k in ('cap_resx', 'cap_resy', 'cap_bpp', 'init2', 'standby_from_normal', 'normal_from_standby', 'shutdown', 'set_bpp_mode', 'pre_update',
          'init_seq', 'standby_from_normal_seq', 'normal_from_standby_seq', 'shutdown_seq'):
    assert len({d[k].lower() for d in D[1:]}) == 1, k
REG = []
for d in D:
    m = re.match(r'"([^"]*)"\s+([0-9a-fA-F]+)\s+(\d+)\s+([0-9a-fA-F]+)\s+(\d+)\s+([0-9a-fA-F]+)', d.get('reg', ''))
    REG.append((int(m.group(2), 16), int(m.group(4), 16), int(m.group(6), 16)) if m else None)
assert REG[0] == (0xA0, 0xF, 0xFF) and [r[0] for r in REG[1:4]] == [0x28] * 3 and REG[4] is None, REG
GUIONES = [('swreset', D[0]['swreset'])] + [('init_%d' % i, D[i]['init']) for i in range(1, 5)] + \
          [(k, D[1][k]) for k in ('init2', 'standby_from_normal', 'normal_from_standby', 'shutdown', 'pre_update')]

if TEXTO:
    for nombre, t in GUIONES:
        print('== ' + nombre + (' (%s)' % D[int(nombre[5])]['name'] if nombre.startswith('init_') else ''))
        for o in guion(t):
            if o[0] == 'W':
                print('  espera %d ms' % o[1])
            else:
                print('  T %2d  rs=0x%08x  %s' % (len(o[1]), rs_de(o[1]), ''.join('%02x' % x for x, _ in o[1])))
    sys.exit(0)

print('/* GENERADO por herr/hacer_paneles.py a partir de %s (el dispdriver0.dat del teléfono). NO EDITAR A MANO.' % FICH.split('/')[-1])
print(' * Microcódigo del PDI, guiones del panel troceados en transferencias como las hace el original, y los cuatro')
print(' * descriptores de panel con su valor del registro 0x0028. Formato de un guion: [n, rs (4 bytes LE), n bytes] por')
print(' * transferencia; [0xFE, ms] espera; [0xFF] fin. */')
for k, nom in (('pdipgmwrite', 'pgm_write'), ('pdipgmgc', 'pgm_gc'), ('pdipgmreadhigh', 'pgm_readhigh'), ('pdipgmread', 'pgm_read')):
    print('static const u8 %s[%d] = { %s };' % (nom, len(PGM[k]), hexs(PGM[k])))
print('#define PGM_WRITE_EN    0x00   /* dónde deja el original cada programa en la memoria del PDI (hal.md §4.2) */')
print('#define PGM_GC_EN       0x1C')
print('#define PGM_READHIGH_EN 0x70')
print('#define PGM_READ_EN     0x9C')
print('#define PDI_VEL_GC    0x%s' % D[1]['pdipgmgcspeed'])
print('#define PDI_VEL_WRITE 0x%s' % D[1]['pdipgmwritespeed'])
print('#define PDI_VEL_READ  0x%s' % D[1]['pdipgmreadspeed'])
print('#define PANEL_REG_SONDA 0x%04X   /* «dummy read» del primer descriptor: se lee y no casa nunca (valor 0x%04X > máscara) */' % (REG[0][0], REG[0][2]))
print('#define PANEL_REG_ID    0x%04X' % REG[1][0])
print('#define PANEL_REG_MASC  0x%04X' % REG[1][1])
for nombre, t in GUIONES:
    b = a_c(guion(t))
    print('static const u8 g_%s[%d] = { %s };' % (nombre, len(b), hexs(b)))
print('typedef struct { const char *nombre; short id; const u8 *init; } lcd_panel;   /* id < 0: sin `reg`, el de «si ninguno casa» */')
print('static const lcd_panel lcd_paneles[4] = {')
for i in range(1, 5):
    print('    { "%s", %d, g_init_%d },' % (re.sub(r'\s+', ' ', D[i]['name'].replace('Lindsay LCD ', '')), REG[i][2] if REG[i] else -1, i))
print('};')
