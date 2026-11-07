#!/usr/bin/env python3
"""Las mallas de los iconos 3D del menú (pista V, docs/menu3d.md) -> shell/mallas3d.h

Todo se hace AQUÍ, por programa: extrusiones de polígonos (con bisel), discos, anillos, casquetes y poco más.
No se lee ningún fichero de fuera (ni modelos, ni iconos, ni fuentes).

Convenios:
  - x a la derecha, y ARRIBA, z hacia quien mira. El icono cabe en |x|, |y| <= 1; se guarda en s8 con 1.0 = 100.
  - Solo se generan las caras que se pueden ver: el menú nunca gira un icono más de ~40°, así que no hay tapas
    traseras.
  - El material de una cara es un PAPEL (0 cuerpo, 1 secundario, 2 claro, 3 oscuro, 4 acento); el color lo pone
    el estilo. Cada icono trae además sus tres colores «naturales» (cuerpo, secundario, acento).
  - Normal por vértice: la de la cara en las aristas vivas, promediada donde el ángulo entre caras es pequeño.

Uso: python3 herr/hacer_mallas.py > shell/mallas3d.h
"""
import math, sys

CUERPO, SEC, CLARO, OSCURO, ACENTO = range(5)
ESC = 100.0


def norm(v):
    l = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / l for c in v)


def sub(a, b): return tuple(x - y for x, y in zip(a, b))
def cruz(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def punto(a, b): return sum(x * y for x, y in zip(a, b))


def rot_x(g):
    c, s = math.cos(math.radians(g)), math.sin(math.radians(g))
    return ((1, 0, 0), (0, c, -s), (0, s, c))


def rot_y(g):
    c, s = math.cos(math.radians(g)), math.sin(math.radians(g))
    return ((c, 0, s), (0, 1, 0), (-s, 0, c))


def rot_z(g):
    c, s = math.cos(math.radians(g)), math.sin(math.radians(g))
    return ((c, -s, 0), (s, c, 0), (0, 0, 1))


def mul(m, v): return tuple(sum(m[i][k] * v[k] for k in range(3)) for i in range(3))
def mmul(a, b): return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))


class Malla:
    def __init__(self, nombre, natural):
        self.nombre, self.natural = nombre, natural       # natural: (cuerpo, secundario, acento) en RGB
        self.pos, self.ipos, self.nor, self.inor, self.tris = [], {}, [], {}, [[] for _ in range(5)]
        self.m, self.t = ((1, 0, 0), (0, 1, 0), (0, 0, 1)), (0.0, 0.0, 0.0)

    # --- transformación en curso (giro + traslación) para lo que se añada a continuación
    def poner(self, m=None, t=(0, 0, 0)):
        self.m = m or ((1, 0, 0), (0, 1, 0), (0, 0, 1)); self.t = t

    def _v(self, p, n):
        """Una esquina = (índice de posición, índice de normal). Posiciones y normales se guardan aparte y sin
        repetir: una arista viva repite la posición con otra normal, y casi todas las caras comparten normales."""
        p = tuple(a + b for a, b in zip(mul(self.m, p), self.t)); n = norm(mul(self.m, n))
        qp = tuple(int(round(c * ESC)) for c in p); qn = tuple(int(round(c * 127)) for c in n)
        if max(abs(c) for c in qp) > 127: raise SystemExit("%s: vértice fuera de s8: %r" % (self.nombre, p))
        i = self.ipos.get(qp)
        if i is None: i = self.ipos[qp] = len(self.pos); self.pos.append(qp)
        j = self.inor.get(qn)
        if j is None: j = self.inor[qn] = len(self.nor); self.nor.append(qn)
        return (i, j), p

    def tri(self, a, b, c, mat):
        """a, b, c = (posición, normal). El sentido se arregla solo para que la cara mire hacia sus normales."""
        (ia, pa), (ib, pb), (ic, pc) = self._v(*a), self._v(*b), self._v(*c)
        if len({ia[0], ib[0], ic[0]}) < 3: return
        g = cruz(sub(pb, pa), sub(pc, pa))
        if punto(g, g) < 1e-12: return
        nm = norm(tuple(x + y + z for x, y, z in zip(mul(self.m, a[1]), mul(self.m, b[1]), mul(self.m, c[1]))))
        if punto(g, nm) < 0: ib, ic = ic, ib
        self.tris[mat].append((ia[0], ib[0], ic[0], ia[1], ib[1], ic[1]))

    def quad(self, a, b, c, d, mat):
        self.tri(a, b, c, mat); self.tri(a, c, d, mat)


# ---- polígonos 2D -------------------------------------------------------------------------------------------
def area2(p): return sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1] for i in range(len(p)))
def ccw(p): return list(p) if area2(p) > 0 else list(reversed(p))


def circ(cx, cy, r, n, ry=None, a0=0.0):
    ry = r if ry is None else ry
    return [(cx + r * math.cos(a0 + 2 * math.pi * i / n), cy + ry * math.sin(a0 + 2 * math.pi * i / n)) for i in range(n)]


def rrect(x0, y0, x1, y1, r, n=3):
    """Rectángulo de esquinas redondeadas (n tramos por esquina)."""
    if r <= 0: return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    p = []
    for cx, cy, a in ((x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180)):
        for i in range(n + 1):
            g = math.radians(a + 90.0 * i / n)
            p.append((cx + r * math.cos(g), cy + r * math.sin(g)))
    return p


def rect(x0, y0, x1, y1): return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def girar2(p, g, cx=0.0, cy=0.0):
    c, s = math.cos(math.radians(g)), math.sin(math.radians(g))
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in p]


def mover2(p, dx, dy): return [(x + dx, y + dy) for x, y in p]


def arco(cx, cy, r0, r1, a0, a1, n):
    """Banda de arco entre los radios r0 < r1, de a0 a a1 grados."""
    ext = [(cx + r1 * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + r1 * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]
    inte = [(cx + r0 * math.cos(math.radians(a1 - (a1 - a0) * i / n)), cy + r0 * math.sin(math.radians(a1 - (a1 - a0) * i / n))) for i in range(n + 1)]
    return ext + inte


def camino(pts, ancho):
    """Polígono de una línea quebrada con grosor (uniones a inglete)."""
    h = ancho / 2.0; izq, der = [], []
    for i, (x, y) in enumerate(pts):
        d0 = norm(sub(pts[i], pts[i - 1]) + (0,))[:2] if i else None
        d1 = norm(sub(pts[i + 1], pts[i]) + (0,))[:2] if i + 1 < len(pts) else None
        d0, d1 = d0 or d1, d1 or d0
        n0, n1 = (-d0[1], d0[0]), (-d1[1], d1[0])
        b = norm((n0[0] + n1[0], n0[1] + n1[1], 0))[:2]
        k = h / max(0.3, b[0] * n0[0] + b[1] * n0[1])
        izq.append((x + b[0] * k, y + b[1] * k)); der.append((x - b[0] * k, y - b[1] * k))
    return der + list(reversed(izq))


def meter(p, d):
    """El polígono (CCW) encogido d hacia dentro (inglete)."""
    n = len(p); r = []
    for i in range(n):
        a, b, c = p[i - 1], p[i], p[(i + 1) % n]
        e0 = norm((b[0] - a[0], b[1] - a[1], 0)); e1 = norm((c[0] - b[0], c[1] - b[1], 0))
        n0, n1 = (-e0[1], e0[0]), (-e1[1], e1[0])                    # hacia dentro en un CCW
        bx, by = n0[0] + n1[0], n0[1] + n1[1]
        l = math.hypot(bx, by) or 1.0; bx, by = bx / l, by / l
        k = d / max(0.35, bx * n0[0] + by * n0[1])
        r.append((b[0] + bx * k, b[1] + by * k))
    return r


def triangula(p):
    """Recorte de orejas de un polígono simple CCW -> lista de (i, j, k)."""
    idx = list(range(len(p))); out = []

    def dentro(a, b, c, q):
        def s(u, v, w): return (v[0] - u[0]) * (w[1] - u[1]) - (v[1] - u[1]) * (w[0] - u[0])
        return s(a, b, q) > 1e-9 and s(b, c, q) > 1e-9 and s(c, a, q) > 1e-9
    guarda = 0
    while len(idx) > 3 and guarda < 10000:
        guarda += 1; hecho = False
        for k in range(len(idx)):
            i, j, l = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = p[i], p[j], p[l]
            if (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) <= 1e-9: continue
            if any(dentro(a, b, c, p[m]) for m in idx if m not in (i, j, l)): continue
            out.append((i, j, l)); del idx[k]; hecho = True
            break
        if not hecho: del idx[0]                                        # degenerado: se suelta un vértice
    if len(idx) == 3: out.append(tuple(idx))
    return out


def normales_lado(p, suave):
    """Por cada arista i (p[i] -> p[i+1]) las normales en sus dos extremos: la de la arista, o la media con la
    arista vecina si el ángulo entre las dos es menor que `suave` grados."""
    n = len(p); ne = []
    for i in range(n):
        a, b = p[i], p[(i + 1) % n]
        ne.append(norm((b[1] - a[1], -(b[0] - a[0]), 0.0)))             # hacia fuera en un CCW
    cs = math.cos(math.radians(suave)) if suave else 2.0
    r = []
    for i in range(n):
        ant, sig = ne[i - 1], ne[(i + 1) % n]
        n0 = norm(tuple(x + y for x, y in zip(ne[i], ant))) if punto(ne[i], ant) >= cs else ne[i]
        n1 = norm(tuple(x + y for x, y in zip(ne[i], sig))) if punto(ne[i], sig) >= cs else ne[i]
        r.append((n0, n1))
    return r


# ---- primitivas ---------------------------------------------------------------------------------------------
def prisma(m, poli, z0, z1, mat, lado=None, bisel=0.0, suave=40, mat_bisel=None, tapa=True, lados=True):
    """Extrusión de un polígono de z0 (atrás) a z1 (delante). Con bisel, la tapa se encoge y la une al contorno
    un chaflán a 45°."""
    p = ccw(poli); n = len(p)
    lado = mat if lado is None else lado
    mat_bisel = mat if mat_bisel is None else mat_bisel
    arriba = meter(p, bisel) if bisel > 0 else p
    zs = z1 - bisel if bisel > 0 else z1
    if tapa:
        for i, j, k in triangula(arriba):
            m.tri(((arriba[i][0], arriba[i][1], z1), (0, 0, 1)), ((arriba[j][0], arriba[j][1], z1), (0, 0, 1)),
                  ((arriba[k][0], arriba[k][1], z1), (0, 0, 1)), mat)
    nl = normales_lado(p, suave)
    for i in range(n):
        j = (i + 1) % n
        (n0, n1) = nl[i]
        if bisel > 0:
            b0, b1 = norm((n0[0], n0[1], 1.0)), norm((n1[0], n1[1], 1.0))
            m.quad(((p[i][0], p[i][1], zs), b0), ((p[j][0], p[j][1], zs), b1),
                   ((arriba[j][0], arriba[j][1], z1), b1), ((arriba[i][0], arriba[i][1], z1), b0), mat_bisel)
        if lados and zs > z0 + 1e-6:
            m.quad(((p[i][0], p[i][1], z0), n0), ((p[j][0], p[j][1], z0), n1),
                   ((p[j][0], p[j][1], zs), n1), ((p[i][0], p[i][1], zs), n0), lado)


def caja(m, x0, y0, x1, y1, z0, z1, mat, **k): prisma(m, rect(x0, y0, x1, y1), z0, z1, mat, **k)


def barra(m, x0, y0, x1, y1, ancho, z0, z1, mat, **k):
    """Una barra recta de (x0, y0) a (x1, y1) con ese ancho."""
    prisma(m, camino([(x0, y0), (x1, y1)], ancho), z0, z1, mat, **k)


def disco(m, cx, cy, r, z0, z1, mat, n=14, **k): prisma(m, circ(cx, cy, r, n), z0, z1, mat, suave=60, **k)


def corona(m, fuera, dentro, z0, z1, mat, lado=None, lado_dentro=None, suave=40):
    """Tapa con agujero entre dos contornos CCW (cualquier número de puntos cada uno) + sus lados."""
    fuera, dentro = ccw(fuera), ccw(dentro)
    lado = mat if lado is None else lado
    lado_dentro = lado if lado_dentro is None else lado_dentro
    cx = sum(x for x, _ in dentro) / len(dentro); cy = sum(y for _, y in dentro) / len(dentro)
    ang = lambda q: math.atan2(q[1] - cy, q[0] - cx) % (2 * math.pi)
    A = sorted(range(len(fuera)), key=lambda i: ang(fuera[i])); B = sorted(range(len(dentro)), key=lambda i: ang(dentro[i]))
    A = [fuera[i] for i in A]; B = [dentro[i] for i in B]
    i = j = 0; na, nb = len(A), len(B)
    P = lambda q: ((q[0], q[1], z1), (0, 0, 1))
    while i < na or j < nb:
        aa = ang(A[(i + 1) % na]) + (2 * math.pi if i + 1 >= na else 0)
        bb = ang(B[(j + 1) % nb]) + (2 * math.pi if j + 1 >= nb else 0)
        if j >= nb or (i < na and aa <= bb):
            m.tri(P(A[i % na]), P(A[(i + 1) % na]), P(B[j % nb]), mat); i += 1
        else:
            m.tri(P(B[j % nb]), P(A[i % na]), P(B[(j + 1) % nb]), mat); j += 1
    for cont, signo, ml in ((A, 1, lado), (B, -1, lado_dentro)):
        nl = normales_lado(cont, suave)
        for k in range(len(cont)):
            l = (k + 1) % len(cont); n0, n1 = nl[k]
            n0, n1 = tuple(signo * c for c in n0), tuple(signo * c for c in n1)
            m.quad(((cont[k][0], cont[k][1], z0), n0), ((cont[l][0], cont[l][1], z0), n1),
                   ((cont[l][0], cont[l][1], z1), n1), ((cont[k][0], cont[k][1], z1), n0), ml)


def anillo(m, cx, cy, r1, r0, z0, z1, mat, n=16, **k):
    corona(m, circ(cx, cy, r1, n), circ(cx, cy, r0, n), z0, z1, mat, suave=60, **k)


def casquete(m, cx, cy, cz, r, mat, rz=None, anillos=3, gajos=10):
    """Media esfera (la mitad que mira al espectador), achatada si rz < r. Normales suaves."""
    rz = r if rz is None else rz

    def pt(i, j):
        a = (math.pi / 2) * i / anillos; g = 2 * math.pi * j / gajos
        d = (math.sin(a) * math.cos(g), math.sin(a) * math.sin(g), math.cos(a))
        return ((cx + r * d[0], cy + r * d[1], cz + rz * d[2]), norm((d[0] / r, d[1] / r, d[2] / rz)))
    for j in range(gajos):
        m.tri(pt(0, 0), pt(1, j), pt(1, j + 1), mat)
        for i in range(1, anillos):
            m.quad(pt(i, j), pt(i + 1, j), pt(i + 1, j + 1), pt(i, j + 1), mat)


# ---- los iconos ---------------------------------------------------------------------------------------------
def archivos():
    m = Malla("archivos", ((250, 186, 44), (214, 138, 24), (255, 240, 200)))
    lomo = [(-0.92, -0.72), (0.92, -0.72), (0.92, 0.52), (0.02, 0.52), (-0.14, 0.74), (-0.76, 0.74), (-0.92, 0.58)]
    prisma(m, lomo, -0.18, -0.04, SEC, bisel=0.04)
    prisma(m, rrect(-0.74, -0.3, 0.74, 0.44, 0.04, 1), -0.04, 0.03, CLARO)                 # la hoja que asoma
    prisma(m, rrect(-0.95, -0.76, 0.95, 0.24, 0.1, 2), 0.03, 0.2, CUERPO, bisel=0.05)     # la tapa de delante
    return m


def imagenes():
    m = Malla("imagenes", ((72, 176, 96), (255, 208, 60), (96, 176, 255)))
    prisma(m, rrect(-0.95, -0.78, 0.95, 0.78, 0.1, 2), -0.12, 0.08, CLARO, bisel=0.05)   # el marco
    caja(m, -0.79, -0.62, 0.79, 0.62, 0.08, 0.1, ACENTO, lados=False)                     # el cielo
    disco(m, 0.38, 0.28, 0.19, 0.1, 0.17, SEC, n=10, bisel=0.04)                          # el sol
    prisma(m, [(-0.79, -0.62), (0.79, -0.62), (0.79, -0.38), (0.42, 0.02), (0.14, -0.28), (-0.24, 0.2), (-0.79, -0.4)],
           0.1, 0.2, CUERPO, bisel=0.04, suave=0)                                          # las montañas
    return m


def reloj():
    m = Malla("reloj", ((236, 84, 60), (255, 200, 64), (60, 70, 90)))
    anillo(m, 0, 0, 0.96, 0.76, -0.14, 0.16, CUERPO, n=18)
    disco(m, 0, 0, 0.77, -0.14, 0.04, CLARO, n=18, lados=False)
    for a in range(0, 360, 90):                                                            # las cuatro marcas
        c, s = math.cos(math.radians(a)), math.sin(math.radians(a))
        barra(m, 0.56 * c, 0.56 * s, 0.7 * c, 0.7 * s, 0.09, 0.04, 0.07, OSCURO, lados=False)
    barra(m, 0, 0, -0.26, 0.36, 0.13, 0.04, 0.1, OSCURO)                                   # las diez y diez
    barra(m, 0, 0, 0.5, 0.3, 0.1, 0.04, 0.13, OSCURO)
    disco(m, 0, 0, 0.11, 0.04, 0.19, CUERPO, n=8, bisel=0.03)
    return m


def notas():
    m = Malla("notas", ((250, 204, 56), (70, 130, 235), (255, 150, 60)))
    prisma(m, rrect(-0.78, -0.95, 0.62, 0.9, 0.08, 1), -0.14, 0.02, CLARO, bisel=0.03)    # la libreta
    prisma(m, rrect(-0.78, 0.55, 0.62, 0.9, 0.08, 1), 0.02, 0.1, CUERPO, bisel=0.03)      # el lomo de arriba
    for y in (0.3, 0.05, -0.2):
        caja(m, -0.56, y - 0.045, 0.4 if y > -0.1 else 0.05, y + 0.045, 0.02, 0.04, OSCURO, lados=False)
    m.poner(rot_z(-38), (0.42, -0.42, 0.0))                                                # el lápiz, cruzado
    prisma(m, rect(-0.13, -0.38, 0.13, 0.52), 0.1, 0.3, SEC, bisel=0.08)
    prisma(m, rect(-0.13, 0.52, 0.13, 0.7), 0.1, 0.3, ACENTO, bisel=0.08)                 # la goma
    prisma(m, [(-0.13, -0.38), (0.13, -0.38), (0.0, -0.68)], 0.1, 0.26, CLARO, bisel=0.06, suave=0)   # la madera
    prisma(m, [(-0.05, -0.56), (0.05, -0.56), (0.0, -0.69)], 0.1, 0.28, OSCURO, suave=0)  # la mina
    m.poner()
    return m


def calculadora():
    m = Malla("calculadora", ((84, 104, 148), (226, 230, 240), (255, 140, 40)))
    prisma(m, rrect(-0.72, -0.95, 0.72, 0.95, 0.12, 2), -0.14, 0.08, CUERPO, bisel=0.05)
    caja(m, -0.52, 0.42, 0.52, 0.76, 0.08, 0.11, CLARO, lados=False)                      # la pantalla
    caja(m, 0.0, 0.5, 0.42, 0.66, 0.11, 0.12, OSCURO, lados=False)                        # una cifra
    for fi in range(3):
        for co in range(3):
            x = -0.52 + co * 0.37; y = 0.02 - fi * 0.36
            mat = ACENTO if (co == 2 and fi == 2) else SEC
            prisma(m, rect(x, y, x + 0.3, y + 0.27), 0.08, 0.18, mat, bisel=0.04)
    return m


def serpiente():
    """La del juego: bloques sobre una cuadrícula de 5 x 5, con la cabeza y la manzana."""
    m = Malla("serpiente", ((76, 196, 88), (44, 150, 70), (236, 64, 64)))
    c = 0.37
    cuerpo = [(0, 0), (1, 0), (2, 0), (3, 0), (3, 1), (3, 2), (2, 2), (1, 2), (1, 3)]
    for i, (co, fi) in enumerate(cuerpo):
        x, y = -0.925 + co * c, -0.925 + fi * c
        prisma(m, rect(x + 0.02, y + 0.02, x + c - 0.02, y + c - 0.02), -0.1, 0.1, SEC if i % 2 else CUERPO, bisel=0.06)
    x, y = -0.925 + 1 * c, -0.925 + 4 * c                                                # la cabeza, más alta
    prisma(m, rrect(x - 0.03, y, x + c + 0.03, y + c + 0.03, 0.08, 1), -0.1, 0.2, CUERPO, bisel=0.07)
    disco(m, x + 0.08, y + 0.24, 0.065, 0.2, 0.23, CLARO, n=6, lados=False)
    disco(m, x + c - 0.08, y + 0.24, 0.065, 0.2, 0.23, CLARO, n=6, lados=False)
    casquete(m, -0.925 + 3.5 * c, -0.925 + 4.45 * c, -0.02, 0.2, ACENTO, anillos=2, gajos=8)   # la manzana
    barra(m, -0.925 + 3.5 * c, -0.925 + 4.9 * c, -0.925 + 3.7 * c, -0.925 + 5.1 * c, 0.07, 0.0, 0.08, SEC)   # el rabito
    return m


def consola():
    m = Malla("consola", ((70, 78, 100), (150, 160, 180), (96, 236, 128)))
    prisma(m, rrect(-0.95, -0.74, 0.95, 0.76, 0.1, 2), -0.14, 0.08, CUERPO, bisel=0.05)
    caja(m, -0.8, -0.58, 0.8, 0.62, 0.08, 0.1, OSCURO, lados=False)                       # la pantalla
    prisma(m, camino([(-0.56, 0.36), (-0.22, 0.1), (-0.56, -0.16)], 0.13), 0.1, 0.17, ACENTO, suave=0)   # >
    caja(m, -0.08, -0.26, 0.4, -0.14, 0.1, 0.17, ACENTO)                                   # _
    return m


def teclas():
    m = Malla("teclas", ((236, 240, 248), (60, 68, 88), (255, 140, 40)))
    prisma(m, rrect(-0.95, -0.95, 0.95, 0.95, 0.12, 2), -0.14, 0.0, OSCURO, bisel=0.04)
    for fi in range(3):
        for co in range(3):
            x = -0.82 + co * 0.58; y = 0.34 - fi * 0.58
            mat = ACENTO if (co == 1 and fi == 1) else CUERPO
            prisma(m, rect(x, y, x + 0.48, y + 0.48), 0.0, 0.16, mat, bisel=0.06)
    return m


def sistema():
    m = Malla("sistema", ((64, 74, 96), (206, 212, 224), (60, 208, 196)))
    for i in range(3):                                                                     # las patas
        d = -0.36 + i * 0.36
        for x0, y0, x1, y1 in ((d - 0.08, 0.6, d + 0.08, 0.95), (d - 0.08, -0.95, d + 0.08, -0.6),
                               (0.6, d - 0.08, 0.95, d + 0.08), (-0.95, d - 0.08, -0.6, d + 0.08)):
            caja(m, x0, y0, x1, y1, -0.1, 0.0, SEC)
    prisma(m, rrect(-0.66, -0.66, 0.66, 0.66, 0.08, 1), -0.12, 0.16, CUERPO, bisel=0.06)
    prisma(m, rrect(-0.32, -0.32, 0.32, 0.32, 0.05, 1), 0.16, 0.22, ACENTO, bisel=0.03)
    disco(m, -0.47, 0.47, 0.06, 0.16, 0.18, SEC, n=6, lados=False)
    return m


def ajustes():
    m = Malla("ajustes", ((140, 156, 180), (226, 232, 240), (255, 150, 40)))
    dientes, pts = 8, []
    for d in range(dientes):
        a = 360.0 * d / dientes
        for da, r in ((-13, 0.72), (-8, 0.96), (8, 0.96), (13, 0.72)):
            g = math.radians(a + da); pts.append((r * math.cos(g), r * math.sin(g)))
    corona(m, pts, circ(0, 0, 0.3, 12), -0.13, 0.11, CUERPO, suave=30)
    anillo(m, 0, 0, 0.52, 0.3, 0.11, 0.19, SEC, n=12)
    return m


def musica():
    m = Malla("musica", ((236, 72, 152), (255, 190, 220), (255, 220, 80)))
    k = dict(bisel=0.06)
    prisma(m, girar2(circ(-0.5, -0.62, 0.31, 10, ry=0.23), 18, -0.5, -0.62), -0.1, 0.18, CUERPO, suave=60, **k)
    prisma(m, girar2(circ(0.42, -0.44, 0.31, 10, ry=0.23), 18, 0.42, -0.44), -0.1, 0.18, CUERPO, suave=60, **k)
    prisma(m, rect(-0.34, -0.6, -0.2, 0.6), -0.08, 0.1, CUERPO, bisel=0.04)
    prisma(m, rect(0.58, -0.42, 0.72, 0.78), -0.08, 0.1, CUERPO, bisel=0.04)
    prisma(m, [(-0.34, 0.5), (0.72, 0.68), (0.72, 0.95), (-0.34, 0.77)], -0.08, 0.14, CUERPO, bisel=0.05, suave=0)
    return m


def telefono():
    m = Malla("telefono", ((64, 200, 116), (30, 130, 70), (255, 255, 255)))
    p = [(-0.95, -0.42), (-0.45, -0.42), (-0.45, 0.0), (0.45, 0.0), (0.45, -0.42), (0.95, -0.42),
         (0.95, 0.14), (0.86, 0.3), (0.7, 0.38), (-0.7, 0.38), (-0.86, 0.3), (-0.95, 0.14)]
    m.poner(rot_z(38))
    prisma(m, p, -0.14, 0.14, CUERPO, bisel=0.07, suave=50)
    for x in (-0.82, 0.58):                                                                # las rejillas
        caja(m, x, -0.44, x + 0.24, -0.42, -0.05, 0.05, SEC)
    m.poner()
    return m


def movimiento():
    m = Malla("movimiento", ((96, 108, 228), (200, 226, 255), (255, 150, 40)))
    m.poner(mmul(rot_z(-16), rot_y(22)), (0.02, 0, 0))                                     # un teléfono ya inclinado
    prisma(m, rrect(-0.44, -0.8, 0.44, 0.8, 0.1, 2), -0.1, 0.06, CUERPO, bisel=0.04)
    caja(m, -0.34, -0.3, 0.34, 0.66, 0.06, 0.08, SEC, lados=False)
    disco(m, 0, -0.55, 0.1, 0.06, 0.09, SEC, n=8, lados=False)
    m.poner()
    for r, z in ((0.74, 0.0), (0.93, -0.04)):                                              # las ondas del movimiento
        prisma(m, arco(0, 0, r - 0.045, r + 0.045, 152, 208, 5), z - 0.04, z + 0.04, ACENTO, suave=60)
        prisma(m, arco(0, 0, r - 0.045, r + 0.045, -28, 28, 5), z - 0.04, z + 0.04, ACENTO, suave=60)
    return m


def apps():
    """(EST3) La celda «Apps» (todas las .sen): cuatro baldosas, cada una de un material. Antes salía la carpeta de Archivos."""
    m = Malla("apps", ((72, 140, 240), (255, 196, 60), (240, 90, 110)))
    for (x, y), mat in (((-0.9, 0.08), CUERPO), ((0.08, 0.08), SEC), ((-0.9, -0.9), ACENTO), ((0.08, -0.9), CLARO)):
        prisma(m, rrect(x, y, x + 0.82, y + 0.82, 0.16, 2), -0.12, 0.14, mat, bisel=0.06)
    return m


def enchufe():
    """(EST3) Un enchufe (W5_IC_ENCHUFE: la app Red y las .sen que lo pidan): el cuerpo, las dos patas y el cable."""
    m = Malla("enchufe", ((70, 170, 120), (226, 232, 240), (255, 196, 60)))
    m.poner(rot_z(-30))
    prisma(m, rrect(-0.42, -0.3, 0.42, 0.42, 0.14, 2), -0.16, 0.16, CUERPO, bisel=0.06)                 # el cuerpo
    for x in (-0.24, 0.14):
        caja(m, x, 0.42, x + 0.1, 0.84, -0.05, 0.05, SEC, bisel=0.02)                                    # las patas
    prisma(m, rect(-0.12, -0.58, 0.12, -0.3), -0.09, 0.09, OSCURO, bisel=0.03)                          # la funda
    prisma(m, camino([(0.0, -0.58), (0.0, -0.78), (0.26, -0.94)], 0.1), -0.05, 0.05, OSCURO, suave=60)   # el cable
    m.poner()
    return m


def generico():
    """Para una app sin icono 3D propio (una app externa): una estrella facetada."""
    m = Malla("generico", ((255, 200, 48), (255, 236, 150), (255, 255, 255)))
    pts = []
    for i in range(10):
        r = 0.96 if i % 2 == 0 else 0.42
        g = math.radians(90 + 36 * i); pts.append((r * math.cos(g), r * math.sin(g)))
    for i in range(10):
        a, b = pts[i], pts[(i + 1) % 10]
        pa, pb, pc = (a[0], a[1], -0.05), (b[0], b[1], -0.05), (0.0, 0.0, 0.3)
        n = norm(cruz(sub(pb, pa), sub(pc, pa)))
        if n[2] < 0: n = tuple(-c for c in n)
        m.tri((pa, n), (pb, n), (pc, n), CUERPO if i % 2 else SEC)
    prisma(m, pts, -0.16, -0.05, CUERPO, tapa=False, suave=0)
    return m


ICONOS = [archivos, imagenes, reloj, notas, calculadora, serpiente, consola, teclas, sistema, ajustes,
          telefono, musica, movimiento, apps, enchufe, generico]   # el orden es el del enum IC_* del shell hasta la música; luego
                                                                     # movimiento, apps y enchufe (m3_malla_de_icono) y el genérico


def main():
    o = sys.stdout
    o.write("/* GENERADO por herr/hacer_mallas.py: las mallas de los iconos 3D del menú. No editar a mano.\n"
            " * pos: s8 x, y, z (1.0 = 100; y arriba, z hacia quien mira). nor: s8 x, y, z (x127). tri: 6 bytes por\n"
            " * triángulo (tres índices de posición y tres de normal), agrupados por material (0 cuerpo, 1 secundario,\n"
            " * 2 claro, 3 oscuro, 4 acento). `natural`: RGB de cuerpo, secundario y acento para los estilos que dan a\n"
            " * cada icono su color. */\n")
    tp = tn = tt = 0; filas = []
    for f in ICONOS:
        m = f()
        np_, nn = len(m.pos), len(m.nor); nt = [len(t) for t in m.tris]
        if np_ > 255 or nn > 255: raise SystemExit("%s: %d posiciones, %d normales (el tope es 255)" % (m.nombre, np_, nn))
        tp += np_; tn += nn; tt += sum(nt)
        for nombre, lista in (("p", m.pos), ("n", m.nor)):
            o.write("static const s8 m3%s_%s[%d] = {\n" % (nombre, m.nombre, 3 * len(lista)))
            for i in range(0, len(lista), 10):
                o.write("    " + " ".join("%d,%d,%d," % v for v in lista[i:i + 10]) + "\n")
            o.write("};\n")
        o.write("static const u8 m3t_%s[%d] = {\n" % (m.nombre, 6 * sum(nt)))
        plano = [tri for t in m.tris for tri in t]
        for i in range(0, len(plano), 6):
            o.write("    " + " ".join("%d,%d,%d,%d,%d,%d," % t for t in plano[i:i + 6]) + "\n")
        o.write("};\n")
        filas.append("    { m3p_%s, m3n_%s, m3t_%s, %d, %d, { %s }, { %s } },   /* %s: %d triángulos */" % (
            m.nombre, m.nombre, m.nombre, np_, nn, ", ".join(str(x) for x in nt),
            ", ".join("{ %d, %d, %d }" % c for c in m.natural), m.nombre, sum(nt)))
        sys.stderr.write("%-12s %3d posiciones %3d normales %3d triángulos %s\n" % (m.nombre, np_, nn, sum(nt), nt))
    o.write("#define M3_MALLAS %d\n#define M3_MALLA_GENERICA %d\nstatic const M3Malla m3_mallas[M3_MALLAS] = {\n%s\n};\n" % (
        len(ICONOS), len(ICONOS) - 1, "\n".join(filas)))
    sys.stderr.write("total: %d posiciones, %d normales, %d triángulos, %d bytes\n" % (tp, tn, tt, 3 * (tp + tn) + 6 * tt))


if __name__ == "__main__":
    main()
