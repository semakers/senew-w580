#!/usr/bin/env python3
"""(MD) El esquema de color del diseño «material3» a partir de UN color semilla, con el algoritmo de tonos HCT de M3
(tono = L* de CIELAB; matiz y croma = los de CAM16 con las condiciones de vista por omisión de material-color-utilities).
Reescrito aquí a partir de la descripción pública del algoritmo (no se copia código). Saca ui/dis_colores_datos.h.
   python3 herr/diseno/hacer_colores.py FF7800 ui/dis_colores_datos.h
Variante de esquema: «tonal spot» (la de serie de Android 13+): primario C=36,
secundario C=16, terciario matiz+60 C=24, neutro C=6, neutro variante C=8, error matiz 25 C=84."""
import math, sys

def lin(c):  # sRGB 0..255 -> lineal 0..100
    c /= 255.0
    return (c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4) * 100.0
def delin(c):  # lineal 0..100 -> sRGB 0..255 (sin redondear)
    c /= 100.0
    c = c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055
    return c * 255.0
M = [[0.41233895, 0.35762064, 0.18051042], [0.2126, 0.7152, 0.0722], [0.01932141, 0.11916382, 0.95034478]]
MI = [[3.2413774792388685, -1.5376652402851851, -0.49885366846268053],
      [-0.9691452513005321, 1.8758853451067872, 0.04156585616912061],
      [0.05562093689691305, -0.20395524564742123, 1.0571799111220335]]
def mul(m, v): return [sum(m[i][j] * v[j] for j in range(3)) for i in range(3)]
def y_de_l(L): return 100.0 * (((L + 16) / 116) ** 3 if L > 8 else L / 903.2962962)
def l_de_y(Y):
    y = Y / 100.0
    return 116 * y ** (1 / 3) - 16 if y > 216 / 24389 else 903.2962962 * y

WP = [95.047, 100.0, 108.883]
M16 = [[0.401288, 0.650173, -0.051461], [-0.250268, 1.204414, 0.045854], [-0.002079, 0.048952, 0.953127]]
M16I = [[1.8620678, -1.0112547, 0.14918678], [0.38752654, 0.62144744, -0.00897398], [-0.01584150, -0.03412294, 1.0499644]]
# condiciones de vista por omisión
La = 200 / math.pi * y_de_l(50) / 100; Yb = y_de_l(50); F, c_, Nc = 1.0, 0.69, 1.0
k = 1 / (5 * La + 1); k4 = k ** 4
FL = k4 * La + 0.1 * (1 - k4) ** 2 * (5 * La) ** (1 / 3)
n = Yb / WP[1]; z = 1.48 + math.sqrt(n); Nbb = 0.725 / n ** 0.2; Ncb = Nbb
D = max(0, min(1, F * (1 - (1 / 3.6) * math.exp((-La - 42) / 92))))
rgbW = mul(M16, WP); DRGB = [D * 100 / v + 1 - D for v in rgbW]
def ad(v): p = (FL * abs(v) / 100) ** 0.42; return math.copysign(400 * p / (p + 27.13), v)
rgbAW = [ad(DRGB[i] * rgbW[i]) for i in range(3)]
Aw = (2 * rgbAW[0] + rgbAW[1] + 0.05 * rgbAW[2]) * Nbb

def cam(rgb):
    xyz = mul(M, [lin(v) for v in rgb])
    r, g, b = [ad(DRGB[i] * v) for i, v in enumerate(mul(M16, xyz))]
    a = (11 * r - 12 * g + b) / 11; bb = (r + g - 2 * b) / 9
    u = (20 * r + 20 * g + 21 * b) / 20; p2 = (40 * r + 20 * g + b) / 20
    h = math.degrees(math.atan2(bb, a)) % 360
    A = p2 * Nbb; J = 100 * (A / Aw) ** (c_ * z)
    et = 0.25 * (math.cos(math.radians(h if h >= 20.14 else h + 360) + 2) + 3.8)
    t = 50000 / 13 * et * Nc * Ncb * math.hypot(a, bb) / (u + 0.305)
    alpha = t ** 0.9 * (1.64 - 0.29 ** n) ** 0.73
    return h, alpha * math.sqrt(J / 100), J

def jch_a_xyz(J, C, h):
    alpha = 0 if J == 0 else C / math.sqrt(J / 100)
    t = (alpha / (1.64 - 0.29 ** n) ** 0.73) ** (1 / 0.9)
    hr = math.radians(h)
    et = 0.25 * (math.cos(hr + 2) + 3.8)
    A = Aw * (J / 100) ** (1 / c_ / z)
    p1 = et * (50000 / 13) * Nc * Ncb; p2 = A / Nbb
    hs, hc = math.sin(hr), math.cos(hr)
    gamma = 23 * (p2 + 0.305) * t / (23 * p1 + 11 * t * hc + 108 * t * hs)
    a = gamma * hc; b = gamma * hs
    ra = (460 * p2 + 451 * a + 288 * b) / 1403; ga = (460 * p2 - 891 * a - 261 * b) / 1403; ba = (460 * p2 - 220 * a - 6300 * b) / 1403
    def inv(v): base = max(0, 27.13 * abs(v) / (400 - abs(v))); return math.copysign(100 / FL * base ** (1 / 0.42), v)
    rc = [inv(v) / DRGB[i] for i, v in enumerate((ra, ga, ba))]
    return mul(M16I, rc)

def hct(h, C, T):
    """el sRGB de (matiz, croma, tono); si no cabe, baja el croma hasta que quepa (como el solucionador de M3)"""
    if T <= 0.0001: return (0, 0, 0)
    if T >= 99.9999: return (255, 255, 255)
    Y = y_de_l(T)
    c = C
    while c >= 0:
        J = math.sqrt(Y) * 11
        for _ in range(8):
            xyz = jch_a_xyz(J, c, h)
            if xyz[1] <= 0: break
            if abs(xyz[1] - Y) < 0.002: break
            J = J - (xyz[1] - Y) * J / (2 * xyz[1])
            if J <= 0: break
        rgb = [delin(v) for v in mul(MI, xyz)]
        if all(-0.5 <= v <= 255.5 for v in rgb) and abs(xyz[1] - Y) < 0.05:
            return tuple(max(0, min(255, round(v))) for v in rgb)
        c -= 0.5
    g = round(delin(Y)); return (g, g, g)

def esquema(semilla):
    h, C, _ = cam(semilla)
    pal = {'P': (h, 36), 'S': (h, 16), 'T': ((h + 60) % 360, 24),
           'N': (h, 6), 'V': (h, 8), 'E': (25, 84)}
    tono = lambda p, t: hct(pal[p][0], pal[p][1], t)
    claro = dict(primario=('P', 40), sobre_primario=('P', 100), contenedor_primario=('P', 90), sobre_contenedor_primario=('P', 10),
                 secundario=('S', 40), contenedor_secundario=('S', 90), sobre_contenedor_secundario=('S', 10),
                 terciario=('T', 40), contenedor_terciario=('T', 90), sobre_contenedor_terciario=('T', 10),
                 error=('E', 40), sobre_error=('E', 100), contenedor_error=('E', 90), sobre_contenedor_error=('E', 10),
                 superficie=('N', 98), sobre_superficie=('N', 10), sobre_superficie_variante=('V', 30),
                 superficie_contenedor_minimo=('N', 100), superficie_contenedor_bajo=('N', 96), superficie_contenedor=('N', 94),
                 superficie_contenedor_alto=('N', 92), superficie_contenedor_maximo=('N', 90),
                 contorno=('V', 50), contorno_variante=('V', 80), superficie_inversa=('N', 20), sobre_superficie_inversa=('N', 95),
                 primario_inverso=('P', 80), velo=('N', 0))
    oscuro = dict(primario=('P', 80), sobre_primario=('P', 20), contenedor_primario=('P', 30), sobre_contenedor_primario=('P', 90),
                  secundario=('S', 80), contenedor_secundario=('S', 30), sobre_contenedor_secundario=('S', 90),
                  terciario=('T', 80), contenedor_terciario=('T', 30), sobre_contenedor_terciario=('T', 90),
                  error=('E', 80), sobre_error=('E', 20), contenedor_error=('E', 30), sobre_contenedor_error=('E', 90),
                  superficie=('N', 6), sobre_superficie=('N', 90), sobre_superficie_variante=('V', 80),
                  superficie_contenedor_minimo=('N', 4), superficie_contenedor_bajo=('N', 10), superficie_contenedor=('N', 12),
                  superficie_contenedor_alto=('N', 17), superficie_contenedor_maximo=('N', 22),
                  contorno=('V', 60), contorno_variante=('V', 30), superficie_inversa=('N', 90), sobre_superficie_inversa=('N', 20),
                  primario_inverso=('P', 40), velo=('N', 0))
    return (h, C), {k: tono(*v) for k, v in claro.items()}, {k: tono(*v) for k, v in oscuro.items()}, list(claro)

def a565(rgb):
    r, g, b = rgb
    return ((r * 31 + 127) // 255, (g * 63 + 127) // 255, (b * 31 + 127) // 255)

def main(hexa, salida):
    semilla = tuple(int(hexa[i:i + 2], 16) for i in (0, 2, 4))
    (h, C), cl, os_, orden = esquema(semilla)
    o = ['/* GENERADO por herr/diseno/hacer_colores.py %s: no editar. Semilla #%s: matiz %.1f, croma %.1f (CAM16). */' % (hexa, hexa, h, C)]
    for nom, d in (('CLARO', cl), ('OSCURO', os_)):
        o.append('#define DIS_M3_%s { \\' % nom)
        for k in orden:
            r, g, b = d[k]; q = a565(d[k])
            # RGB() de ui.h trunca (>> 3, >> 2): se le da el valor que, truncado, es el más cercano en 565
            o.append('    .%s = RGB(%d, %d, %d), /* #%02X%02X%02X */ \\' % (k, q[0] * 8 + 4, q[1] * 4 + 2, q[2] * 8 + 4, r, g, b))
        o.append('}')
    open(salida, 'w').write('\n'.join(o) + '\n')
    print('semilla #%s -> matiz %.1f croma %.1f' % (hexa, h, C))
    for k in orden: print('%-30s claro #%02X%02X%02X   oscuro #%02X%02X%02X' % ((k,) + cl[k] + os_[k]))

main(sys.argv[1], sys.argv[2])
