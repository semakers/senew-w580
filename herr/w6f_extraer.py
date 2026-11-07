#!/usr/bin/env python3
"""w6f_extraer.py — LECTOR INDEPENDIENTE del formato W6F (no usa nada del código C; solo docs/fs_propio.md §2).

  w6f_extraer.py IMAGEN                       lista el árbol (ruta, tamaño, CRC32 del contenido)
  w6f_extraer.py IMAGEN --manifiesto          lo mismo en formato estable, para cotejar: «D ruta» / «F ruta tam crc32»
  w6f_extraer.py IMAGEN --extraer DIR         saca todos los ficheros a DIR
  opciones: --pag N --ppb N (si no, se deducen de una cabecera de bloque) --desde DESPL --bloques N (trozo de un volcado mayor)
            --max-obj N (por defecto, sin tope: cualquier id)

La IMAGEN son los bloques de la zona seguidos (un volcado de la NOR vale con --desde y --bloques).
Sale con 0 si todo lo que hay se pudo leer y pasó sus CRC; 1 si hay ficheros dañados; 2 si no hay volumen."""
import sys, struct, zlib, binascii, os, argparse

CAB = 28
MAGIA, MARCA = 0x3657, 0x4F6B
FUTURAS = []

class Cab:
    __slots__ = ('tipo', 'id', 'gen', 'idx', 'seq', 'dcrc', 'lote', 'danada')

def leer_cab(b):
    """None = blanco; False = basura; Cab = válida"""
    if b == b'\xff' * CAB: return None
    magia, tipo, ver, ident, marca, gen, idx, seq, dcrc, lote, hcrc = struct.unpack('<HBBHHIIIIHH', b)
    if magia != MAGIA or marca != MARCA: return False
    if hcrc != binascii.crc_hqx(b[:26], 0xFFFF): return False
    if ver != 2:
        FUTURAS.append(ver)                             # un volumen de OTRA versión del formato: no es basura, y no se interpreta
        return False
    danada, lote = bool(lote & 0x8000), lote & 0x7FFF  # bit alto: la recolección copió esta página sabiendo que ya venía rota
    if lote not in (0, 1, 3) or chr(tipo) not in 'BDNCFG': return False
    if chr(tipo) not in 'BF' and seq >= 0xFFFFFFF0: return False     # (M3) ninguna página normal llega a esa secuencia
    c = Cab(); c.tipo = chr(tipo); c.id = ident; c.gen = gen; c.idx = idx; c.seq = seq; c.dcrc = dcrc; c.lote = lote; c.danada = danada
    return c

def nombre_valido(n):
    if not (1 <= len(n) <= 255) or n in (b'.', b'..'): return False
    try: s = n.decode('utf-8')
    except UnicodeDecodeError: return False
    return not any(ord(ch) < 0x20 or ch in '/\x7f' for ch in s)

def deducir(img):
    for pag in (1024, 2048, 512, 4096):
        for off in range(0, min(len(img), 64 << 20), pag):
            c = leer_cab(img[off:off + CAB])
            if c and c.tipo == 'B':
                g = img[off + CAB:off + CAB + 20]
                if zlib.crc32(g) == c.dcrc and struct.unpack('<I', g[:4])[0] == pag:
                    ppb = struct.unpack('<I', g[4:8])[0]
                    if 4 <= ppb <= 4096 and off % (pag * ppb) == 0: return pag, ppb
    return None

def leer_volumen(img, pag, ppb, max_obj=None):
    carga = pag - CAB
    cola_max = carga - 20 - 255 - 1
    por_rango = carga // 4
    nblq = len(img) // (pag * ppb)
    P = lambda p: img[p * pag:(p + 1) * pag]
    nodos, datos, cps, cabeceras, formato, encogida, obj_formato = [], [], {}, 0, False, False, 0

    def nodo(p, c):
        b = P(p)[CAB:]
        padre, clase, nl, tam, fecha, mata, cola_len, aparte = struct.unpack('<HBBIIHHB', b[:17])
        if c.id == 0 or padre == c.id or nl == 0 or aparte > 1: return None
        if max_obj and (c.id >= max_obj or padre >= max_obj): return None
        if mata != 0xFFFF and (mata == 0 or (max_obj and mata >= max_obj)): return None
        if clase == 2:
            if tam or cola_len or aparte: return None
        elif clase == 1:
            r = tam % carga
            if aparte:
                if r == 0 or cola_len: return None
            elif cola_len != r or r > cola_max: return None
        else: return None
        usado = 20 + nl + cola_len
        if usado > carga or zlib.crc32(b[:usado]) != c.dcrc: return None
        nombre = b[20:20 + nl]
        if not nombre_valido(nombre): return None
        return dict(pag=p, id=c.id, gen=c.gen, seq=c.seq, padre=padre, dir=clase == 2, tam=tam, fecha=fecha, mata=mata,
                    cola=b[20 + nl:20 + nl + cola_len], aparte=aparte, nombre=nombre, nodo_malo=c.danada)

    def entera(p, c):
        b = P(p)[CAB:]
        if c.tipo == 'G': return True
        if c.tipo == 'D': return zlib.crc32(b) == c.dcrc
        if c.tipo == 'C': return zlib.crc32(b[:por_rango * 4]) == c.dcrc
        if c.tipo == 'N': return nodo(p, c) is not None
        return False

    for b in range(nblq):
        pend = []                      # copias de un lote que aún no tiene compromiso
        for k in range(ppb):
            p = b * ppb + k
            c = leer_cab(P(p)[:CAB])
            if not c: continue
            if c.tipo == 'B':
                g = P(p)[CAB:CAB + 20]
                if k == 0 and zlib.crc32(g) == c.dcrc and struct.unpack('<II', g[:8]) == (pag, ppb):
                    cabeceras += 1
                    if struct.unpack('<I', g[8:12])[0] > nblq: encogida = True      # (A3) se le dio formato con más bloques que los de esta imagen
                    obj_formato = max(obj_formato, struct.unpack('<I', g[16:20])[0])
                continue
            if c.lote == 3 and not entera(p, c): continue        # un compromiso a medias no es nada
            if c.tipo == 'F': formato = True; continue
            aceptar = []
            if c.lote == 0: aceptar = [(p, c)]
            else:
                pend.append((p, c))
                if c.lote == 3: aceptar, pend = pend, []
            for (q, cq) in aceptar:
                if cq.tipo == 'D': datos.append((q, cq))
                elif cq.tipo == 'N':
                    n = nodo(q, cq)
                    if n: nodos.append(n)
                elif cq.tipo == 'C':
                    body = P(q)[CAB:CAB + por_rango * 4]
                    if cq.gen == por_rango and zlib.crc32(body) == cq.dcrc and (cq.id not in cps or cq.seq > cps[cq.id][0]):
                        cps[cq.id] = (cq.seq, struct.unpack('<%dI' % por_rango, body))
    if formato: raise SystemExit('w6f: hay un formato a medias (marca F): el volumen no es válido hasta repetirlo')
    if FUTURAS: raise SystemExit('w6f: el volumen tiene páginas de la versión %d del formato; este lector solo entiende la 2' % max(FUTURAS))
    if not cabeceras: return None
    if encogida: raise SystemExit('w6f: a este volumen se le dio formato con más bloques de los que tiene la imagen: faltan trozos (¿--desde / --bloques?)')
    # por cada id, el nodo de mayor seq que respete su punto de control
    gana = {}
    for n in nodos:
        r = n['id'] // por_rango
        if r in cps:
            S, tabla = cps[r]
            e = tabla[n['id'] % por_rango]
            if n['seq'] < S and (e == 0 or n['seq'] < e): continue
        if n['id'] not in gana or n['seq'] > gana[n['id']]['seq']: gana[n['id']] = n
    # «mata»: un renombrado encima de otro cuyo punto de control no llegó a escribirse
    for n in list(gana.values()):
        m = n['mata']
        if m != 0xFFFF and m != n['id'] and m in gana and gana[m]['seq'] < n['seq']: gana[m]['condenado'] = True
    gana = {i: n for i, n in gana.items() if not n.get('condenado')}
    # huérfanos y ciclos: a la raíz
    for i, n in gana.items():
        x, pasos = i, 0
        while x and pasos <= 16:
            pa = gana[x]['padre'] if x == i else gana[x]['padre']
            if pa and (pa not in gana or not gana[pa]['dir']): break
            x = pa; pasos += 1
        n['huerfano'] = bool(x)
    for n in gana.values():
        if n['huerfano']: n['padre'] = 0
    # datos
    por_fichero = {}
    for (p, c) in datos:
        n = gana.get(c.id)
        if not n or n['dir'] or n['gen'] != c.gen: continue
        nd = n['tam'] // carga + n['aparte']
        if c.idx >= nd: continue
        por_fichero.setdefault(c.id, {}).setdefault(c.idx, []).append((c.seq, -p, p, c))
    malos = 0
    for i, n in gana.items():
        if n['dir']: continue
        nd = n['tam'] // carga + n['aparte']
        trozos, bien = [], not n['nodo_malo']
        for idx in range(nd):
            cand = sorted(por_fichero.get(i, {}).get(idx, []), reverse=True)
            if not cand: bien = False; break
            elegido = None
            for (_, _, p, c) in cand:
                if not c.danada and zlib.crc32(P(p)[CAB:]) == c.dcrc: elegido = p; break
            if elegido is None: bien = False; break
            trozos.append(P(elegido)[CAB:])
        if bien:
            n['contenido'] = (b''.join(trozos) + n['cola'])[:n['tam']] if not n['aparte'] else b''.join(trozos)[:n['tam']]
            if len(n['contenido']) != n['tam']: bien = False
        if not bien: n['contenido'] = None; malos += 1
    # rutas
    def ruta(i, prof=0):
        n = gana[i]
        if prof > 20: return '/?' 
        base = ruta(n['padre'], prof + 1) if n['padre'] else ''
        return base + '/' + n['nombre'].decode('utf-8')
    arbol = sorted(((ruta(i), n) for i, n in gana.items()), key=lambda t: t[0].encode('utf-8'))
    return arbol, malos, dict(pag=pag, ppb=ppb, bloques=nblq, cabeceras=cabeceras, nodos=len(nodos), datos=len(datos), puntos_de_control=len(cps))

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('imagen'); ap.add_argument('--manifiesto', action='store_true'); ap.add_argument('--extraer')
    ap.add_argument('--pag', type=int); ap.add_argument('--ppb', type=int)
    ap.add_argument('--desde', type=lambda x: int(x, 0), default=0); ap.add_argument('--bloques', type=int)
    ap.add_argument('--max-obj', type=int)
    a = ap.parse_args()
    img = open(a.imagen, 'rb').read()[a.desde:]
    if a.pag and a.ppb: pag, ppb = a.pag, a.ppb
    else:
        g = deducir(img)
        if not g: print('w6f: no encuentro ninguna cabecera de bloque', file=sys.stderr); return 2
        pag, ppb = g
    if a.bloques: img = img[:a.bloques * pag * ppb]
    v = leer_volumen(img, pag, ppb, a.max_obj)
    if v is None: print('w6f: no hay volumen', file=sys.stderr); return 2
    arbol, malos, info = v
    for r, n in arbol:
        if n['dir']: print('D %s' % r if a.manifiesto else '%-60s <dir>' % r)
        elif n['contenido'] is None: print('X %s %d' % (r, n['tam']) if a.manifiesto else '%-60s %9d DAÑADO' % (r, n['tam']))
        else:
            crc = zlib.crc32(n['contenido'])
            print('F %s %d %08x' % (r, n['tam'], crc) if a.manifiesto else '%-60s %9d %08x' % (r, n['tam'], crc))
    if a.extraer:
        for r, n in arbol:
            d = os.path.join(a.extraer, r.lstrip('/'))
            if n['dir']: os.makedirs(d, exist_ok=True)
            elif n['contenido'] is not None:
                os.makedirs(os.path.dirname(d), exist_ok=True)
                open(d, 'wb').write(n['contenido'])
    if not a.manifiesto: print('# %r, dañados: %d' % (info, malos), file=sys.stderr)
    return 1 if malos else 0

if __name__ == '__main__': sys.exit(main())
