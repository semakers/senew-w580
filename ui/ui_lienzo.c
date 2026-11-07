/* Lienzo: primitivas de dibujo con recorte sobre el cuadro de 176x220. Sin hardware. */
#include "ui.h"
#include "diseno.h"                                   /* (MD) la fuente la pone el diseño en uso */

Color ui_mezcla(Color a, Color b, int t)
{
    u32 x = (u16)((a >> 8) | (a << 8)), y = (u16)((b >> 8) | (b << 8)), r, g, bl;
    if (t <= 0) return a;
    if (t >= 256) return b;
    r  = ((x >> 11) * (256 - t) + (y >> 11) * t) >> 8;
    g  = (((x >> 5) & 63) * (256 - t) + ((y >> 5) & 63) * t) >> 8;
    bl = ((x & 31) * (256 - t) + (y & 31) * t) >> 8;
    x = (r << 11) | (g << 5) | bl;
    return (Color)((x >> 8) | (x << 8));
}

void li_rect(Lienzo *l, int x, int y, int w, int h, Color c)
{
    int x1 = x + w, y1 = y + h, rx1 = l->recorte.x + l->recorte.w, ry1 = l->recorte.y + l->recorte.h, n;
    typedef u32 __attribute__((may_alias)) u32a;          /* (pista O) dos píxeles del cuadro de una vez (el cuadro es de u16) */
    u32 cc;
    if (c == TRANSPARENTE) return;
    if (x < l->recorte.x) x = l->recorte.x;
    if (y < l->recorte.y) y = l->recorte.y;
    if (x1 > rx1) x1 = rx1;
    if (y1 > ry1) y1 = ry1;
    if (x1 <= x) return;
    cc = (u32)c | ((u32)c << 16);
    for (; y < y1; y++) {
        Color *p = l->cuadro + y * UI_ANCHO + x;      /* (pista O) de dos en dos píxeles (una palabra de 32) y de 16 en 16 */
        u32a *q;
        n = x1 - x;
        if ((size_t)p & 2) { *p++ = c; n--; }
        q = (u32a *)(void *)p;
        for (; n >= 16; n -= 16, q += 8) { q[0] = cc; q[1] = cc; q[2] = cc; q[3] = cc; q[4] = cc; q[5] = cc; q[6] = cc; q[7] = cc; }
        for (; n >= 2; n -= 2) *q++ = cc;
        if (n) *(Color *)(void *)q = c;
    }
}

void li_marco(Lienzo *l, int x, int y, int w, int h, Color c)
{
    li_rect(l, x, y, w, 1, c); li_rect(l, x, y + h - 1, w, 1, c);
    li_rect(l, x, y, 1, h, c); li_rect(l, x + w - 1, y, 1, h, c);
}

static u32 utf8(const u8 **pp)
{
    const u8 *p = *pp; u32 cp = *p++;
    if ((cp & 0xE0) == 0xC0 && (*p & 0xC0) == 0x80) cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
    else if ((cp & 0xF0) == 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
        cp = ((cp & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2;
    }
    *pp = p;
    return cp;
}
u32 ui_utf8(const char **p) { return utf8((const u8 **)p); }

/* Métrica de un glifo: primera columna con tinta y ancho. Proporcional salvo los dígitos (tabulares). */
static int metrica(u32 cp, int marcas, int *ini)
{
    const u8 *g = fuente_5x7[fuente_indice(cp)];
    unsigned o = g[0] | g[1] | g[2] | g[3] | g[4] | g[5] | g[6];
    int a = 0, b = 4;
    *ini = 0;
    if ((marcas & F_MONO) || (cp >= '0' && cp <= '9')) return 5;
    if (!o) return 3;                                        /* espacio */
    while (!(o & (0x10u >> a))) a++;
    while (!(o & (0x10u >> b))) b--;
    *ini = a;
    return b - a + 1;
}

/* Las letras con descendente (g p q y, y la j) se dibujan en la tabla pegadas arriba y se pintan
 * bajadas: así ocupan las filas 2..8 y la «g» no parece un 9. Una línea de texto mide 9 de alto. */
static int baja(u32 cp) { return (cp == 'g' || cp == 'p' || cp == 'q' || cp == 'y') ? 2 : cp == 'j' ? 1 : 0; }
int li_descendentes(const char *s)
{
    const u8 *p = (const u8 *)s;
    while (*p) if (baja(utf8(&p))) return 1;
    return 0;
}


/* (MD) La fuente del diseño en uso para estas marcas y esta escala; NULL = el 5x7 de siempre (también con F_MONO). */
static const DisFuente *fuente_dis(int escala, int marcas)
{
    int e = F_ESTILO_DE(marcas);
    if (marcas & F_MONO) return NULL;
    if (!e) { if (escala < 1 || escala > 4) return NULL; e = escala == 1 ? DIS_T_CUERPO : escala == 2 ? DIS_T_TITULAR : escala == 3 ? DIS_T_DISPLAY : DIS_T_DISPLAY_G; }
    return dis_fuente(e);                /* (estética) la de la escala nueva, o la de antes en el modo v1 */
}
/* (estética) a qué escala va el 5x7 cuando no hay fuente del diseño: en el modo v1, la del widget, como siempre (bit a bit lo de
 * antes); con la escala nueva, la que diga dis_escala_5x7 (el cuerpo al doble). */
static int k5x7(int escala, int marcas) { return dis_es_v1() ? escala : dis_escala_5x7(escala, marcas); }
static int escala_de(const DisFuente *f) { return f->alto >= 20 ? f->alto / 10 : 1; }   /* para el 5x7 de relleno */
static int ancho_dis(const DisFuente *f, u32 cp)
{
    unsigned i = fuente_indice(cp); const DisGlifo *g;
    int ini;
    if (i >= f->n) i = 107;
    g = &f->g[i];
    if (!g->avance) return (metrica(cp, 0, &ini) + 1) * escala_de(f);       /* Roboto no lo tiene: el 5x7 */
    return g->avance;
}

int li_ancho_n(const char *s, int bytes, int escala, int marcas)
{
    const u8 *p = (const u8 *)s, *fin = p + bytes; int n = 0, ini;
    const DisFuente *f = fuente_dis(escala, marcas);
    if (f) { while (*p && p < fin) n += ancho_dis(f, utf8(&p)); return n; }
    escala = k5x7(escala, marcas);
    while (*p && p < fin) n += (metrica(utf8(&p), marcas, &ini) + 1) * escala;
    return n ? n - escala : 0;
}
int li_ancho_texto_f(const char *s, int escala, int marcas) { return li_ancho_n(s, 0x7FFFFFFF, escala, marcas); }
int li_ancho_texto(const char *s, int escala) { return li_ancho_n(s, 0x7FFFFFFF, escala, 0); }

/* Cuántos bytes de s caben en «ancho» píxeles: corta en el último espacio (que se come) o en '\n'
 * (que también se come). Siempre avanza al menos un carácter. */
int li_partir(const char *s, int ancho, int escala, int marcas)
{
    const u8 *p = (const u8 *)s, *ult = 0; int x = 0, ini;
    const DisFuente *f = fuente_dis(escala, marcas);
    const int resta = f ? 0 : (escala = k5x7(escala, marcas));
    while (*p) {
        const u8 *q = p; u32 cp = utf8(&q);
        if (cp == '\n') return (int)(q - (const u8 *)s);
        x += f ? ancho_dis(f, cp) : (metrica(cp, marcas, &ini) + 1) * escala;
        if (x - resta > ancho && p != (const u8 *)s) return (int)((ult ? ult : p) - (const u8 *)s);
        if (cp == ' ') ult = q;
        p = q;
    }
    return (int)(p - (const u8 *)s);
}

/* Un glifo a un mapa de bits de hasta 20x28 (escala 4), una palabra por fila, bit 31 = izquierda.
 * En las escalas 2 y 4 se amplía con EPX: cada píxel da 2x2 y las esquinas copian al vecino cuando
 * dos vecinos contiguos coinciden entre sí y no con los opuestos. */
/* (pista O) Las dos, por FILAS ENTERAS en vez de bit a bit, y dan exactamente lo mismo: con P la fila, A la de arriba,
 * D la de abajo, B = P << 1 (el vecino de la derecha) y C = P >> 1 (el de la izquierda), todas recortadas a w columnas
 * (fuera hay 0, como antes), cada esquina es «P, salvo donde se cumple su condición, que toma el vecino». */
static u32 esparce(u32 v)                                    /* los 16 bits altos de v: la columna x pasa al bit 30 - 2x */
{
    v >>= 16;
    v = (v | (v << 8)) & 0x00FF00FFu; v = (v | (v << 4)) & 0x0F0F0F0Fu;
    v = (v | (v << 2)) & 0x33333333u; v = (v | (v << 1)) & 0x55555555u;
    return v;
}
static void epx(const u32 *o, u32 *d, int w, int h)
{
    const u32 cols = w >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> w);
    int y;
    for (y = 0; y < h; y++) {
        const u32 P = o[y] & cols, A = y > 0 ? o[y - 1] & cols : 0, D = y + 1 < h ? o[y + 1] & cols : 0, B = P << 1, C = P >> 1;
        const u32 m1 = ~(C ^ A) & (C ^ D) & (A ^ B) & cols, m2 = ~(A ^ B) & (A ^ C) & (B ^ D) & cols,
                  m3 = ~(D ^ C) & (D ^ B) & (C ^ A) & cols, m4 = ~(B ^ D) & (B ^ A) & (D ^ C) & cols;
        d[2 * y]     = (esparce((P & ~m1) | (A & m1)) << 1) | esparce((P & ~m2) | (B & m2));
        d[2 * y + 1] = (esparce((P & ~m3) | (C & m3)) << 1) | esparce((P & ~m4) | (D & m4));
    }
}

/* pinta un mapa de bits (palabra por fila, bit 31 = izquierda) con píxeles de «e» de lado: cada tramo de bits seguidos, de una vez */
static void mapa(Lienzo *l, int x, int y, const u32 *m, int w, int h, Color c, int e)
{
    const u32 cols = w >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> w);
    int f, dentro;
    if (x >= l->recorte.x + l->recorte.w || x + w * e <= l->recorte.x ||
        y >= l->recorte.y + l->recorte.h || y + h * e <= l->recorte.y) return;
    dentro = x >= l->recorte.x && x + w * e <= l->recorte.x + l->recorte.w &&
             y >= l->recorte.y && y + h * e <= l->recorte.y + l->recorte.h;
    for (f = 0; f < h; f++) {
        u32 b = m[f] & cols;
        while (b) {
            const int ini = __builtin_clz(b);
            const u32 r = ~(b << ini);                       /* el tramo: los unos seguidos desde «ini» */
            const int k = ini + (r ? __builtin_clz(r) : 32);
            if (dentro) {
                Color *p = l->cuadro + (y + f * e) * UI_ANCHO + x + ini * e;
                int n = (k - ini) * e, i, j;
                for (j = 0; j < e; j++, p += UI_ANCHO) for (i = 0; i < n; i++) p[i] = c;
            } else li_rect(l, x + ini * e, y + f * e, (k - ini) * e, e, c);
            b = k >= 32 ? 0 : b & (0xFFFFFFFFu >> k);
        }
    }
}


/* (MD) un glifo de 4 bits por píxel: 15 = el color tal cual; 1..14 = mezclado con lo que ya hay debajo */
void li_glifo4(Lienzo *l, int x, int y, const DisFuente *f, const DisGlifo *g, Color c)
{
    const int bw = (g->w + 1) / 2, rx0 = l->recorte.x, rx1 = l->recorte.x + l->recorte.w, ry0 = l->recorte.y, ry1 = l->recorte.y + l->recorte.h;
    const u8 *d = f->datos + g->desp;
    int i, j, x0 = x + g->dx, y0 = y + g->dy;
    for (j = 0; j < g->h; j++, d += bw) {
        Color *p; int yy = y0 + j;
        if (yy < ry0 || yy >= ry1) continue;
        p = l->cuadro + yy * UI_ANCHO;
        for (i = 0; i < g->w; i++) {
            const int xx = x0 + i, a = (i & 1) ? d[i >> 1] & 15 : d[i >> 1] >> 4;
            if (!a || xx < rx0 || xx >= rx1) continue;
            p[xx] = a == 15 ? c : ui_mezcla(p[xx], c, a * 17);
        }
    }
}
static void texto_5x7(Lienzo *l, int x, int y, const u8 *p, const u8 *fin, Color c, int escala, int marcas);
static void texto_dis(Lienzo *l, int x, int y, const u8 *p, const u8 *fin, Color c, const DisFuente *f)
{
    if (y >= l->recorte.y + l->recorte.h || y + f->alto <= l->recorte.y) return;
    while (*p && p < fin) {
        const u8 *ini = p; u32 cp = utf8(&p); unsigned i = fuente_indice(cp); const DisGlifo *g;
        if (x >= l->recorte.x + l->recorte.w) break;
        if (i >= f->n) i = 107;
        g = &f->g[i];
        if (!g->avance) {                                    /* no está en la fuente: el 5x7, sobre la misma línea base */
            int e = escala_de(f), w = ancho_dis(f, cp);
            texto_5x7(l, x, y + (f->base - 7 * e), ini, p, c, e, F_MONO);
            x += w; continue;
        }
        if (g->w && x + g->dx + g->w > l->recorte.x) li_glifo4(l, x, y, f, g, c);
        x += g->avance;
    }
}

/* el 5x7 a una escala YA decidida (la usan li_texto_n y el relleno de los glifos que la fuente del diseño no tiene) */
static void texto_5x7(Lienzo *l, int x, int y, const u8 *p, const u8 *fin, Color c, int escala, int marcas)
{
    if (y >= l->recorte.y + l->recorte.h || y + 9 * escala <= l->recorte.y) return;
    while (*p && p < fin) {
        u32 cp = utf8(&p), m[28], t[14];
        int ini, w = metrica(cp, marcas, &ini), f, yg = y + baja(cp) * escala;
        const u8 *g;
        if (x >= l->recorte.x + l->recorte.w) break;
        if (cp == ' ' || cp == '\n' || x + w * escala <= l->recorte.x) { x += (w + 1) * escala; continue; }
        g = fuente_5x7[fuente_indice(cp)];
        for (f = 0; f < 7; f++) m[f] = ((u32)g[f] << (27 + ini));
        if (!(marcas & F_BASTA) && escala == 2) { epx(m, t, w, 7); mapa(l, x, yg, t, w * 2, 14, c, 1); }
        else if (!(marcas & F_BASTA) && escala == 4) { epx(m, t, w, 7); epx(t, m, w * 2, 14); mapa(l, x, yg, m, w * 4, 28, c, 1); }
        else mapa(l, x, yg, m, w, 7, c, escala);
        x += (w + 1) * escala;
    }
}
void li_texto_n(Lienzo *l, int x, int y, const char *s, int bytes, Color c, int escala, int marcas)
{
    const u8 *p = (const u8 *)s, *fin = p + bytes;
    const DisFuente *fd;
    if (c == TRANSPARENTE) return;
    if ((fd = fuente_dis(escala, marcas)) != NULL) { texto_dis(l, x, y, p, fin, c, fd); return; }
    texto_5x7(l, x, y, p, fin, c, k5x7(escala, marcas), marcas);
}
void li_texto_f(Lienzo *l, int x, int y, const char *s, Color c, int escala, int marcas) { li_texto_n(l, x, y, s, 0x7FFFFFFF, c, escala, marcas); }
void li_texto(Lienzo *l, int x, int y, const char *s, Color c, int escala) { li_texto_n(l, x, y, s, 0x7FFFFFFF, c, escala, 0); }

void li_linea_h(Lienzo *l, int x, int y, int w, Color c) { li_rect(l, x, y, w, 1, c); }
void li_punto(Lienzo *l, int x, int y, Color c) { li_rect(l, x, y, 1, 1, c); }

/* Velo: deja a la mitad de brillo lo que ya hay pintado debajo (para leer texto claro sobre una foto).
 * El píxel está con los bytes cambiados (RGB565_BE): en esa forma la máscara de «bit alto de cada
 * componente fuera tras desplazar» es 0xEF7B. */
void li_velo(Lienzo *l, int x, int y, int w, int h)
{
    typedef u32 __attribute__((may_alias)) u32a;          /* (pista O) dos píxeles del cuadro de una vez (el cuadro es de u16) */
    int x1 = x + w, y1 = y + h, rx1 = l->recorte.x + l->recorte.w, ry1 = l->recorte.y + l->recorte.h, i;
    if (x < l->recorte.x) x = l->recorte.x;
    if (y < l->recorte.y) y = l->recorte.y;
    if (x1 > rx1) x1 = rx1;
    if (y1 > ry1) y1 = ry1;
    /* (pista O) De dos en dos píxeles, sin deshacer el cambio de bytes: con el píxel girado (alto = p & 0xFF, bajo = p >> 8),
     * «(v >> 1) & 0x7BEF» es, por bytes, bajo' = (bajo >> 1 | alto << 7) & 0xEF y alto' = (alto >> 1) & 0x7B; es decir
     * p' = ((p >> 1) & 0x7F7F | (p << 15) & 0x8000) & 0xEF7B, y eso vale igual para dos píxeles en una palabra de 32. */
    for (; y < y1; y++) {
        Color *p = l->cuadro + y * UI_ANCHO + x;
        i = x1 - x;
        if (i > 0 && ((size_t)p & 2)) { u32 v = *p; *p++ = (Color)((((v >> 1) & 0x7F7Fu) | ((v << 15) & 0x8000u)) & 0xEF7Bu); i--; }
        for (; i >= 2; i -= 2, p += 2) {
            u32a *q = (u32a *)(void *)p; const u32 v = *q;
            *q = (((v >> 1) & 0x7F7F7F7Fu) | ((v << 15) & 0x80008000u)) & 0xEF7BEF7Bu;
        }
        if (i > 0) { u32 v = *p; *p = (Color)((((v >> 1) & 0x7F7Fu) | ((v << 15) & 0x8000u)) & 0xEF7Bu); }
    }
}

/* mapa de 1 bpp con filas de 16 bits (iconos): el bit (w-1) es la columna de la izquierda */
static void glifo(Lienzo *l, int x, int y, const u16 *filas16, int w, int h, Color c, int e)
{
    u32 m[16]; int f;
    for (f = 0; f < h && f < 16; f++) m[f] = (u32)filas16[f] << (32 - w);
    mapa(l, x, y, m, w, h < 16 ? h : 16, c, e);
}

void li_icono(Lienzo *l, int x, int y, const Icono *i, Color c, int escala)
{
    const DisFuente *sim = escala == 1 ? dis_actual()->simbolos : escala == 2 ? dis_actual()->simbolos_g : NULL; int k;
    if (sim && i->w == 12 && i->h == 12 && (k = dis_simbolo_de(i)) >= 0 && k < sim->n) {   /* (MD) el símbolo del diseño */
        li_glifo4(l, x, y, sim, &sim->g[k], c);
        return;
    }
    glifo(l, x, y, i->filas, i->w, i->h, c, escala);
}

void li_imagen(Lienzo *l, int x, int y, const Imagen *im)
{
    int x0 = x, y0 = y, x1 = x + im->w, y1 = y + im->h, i, j;
    const Color t = im->transparente;
    if (x0 < l->recorte.x) x0 = l->recorte.x;
    if (y0 < l->recorte.y) y0 = l->recorte.y;
    if (x1 > l->recorte.x + l->recorte.w) x1 = l->recorte.x + l->recorte.w;
    if (y1 > l->recorte.y + l->recorte.h) y1 = l->recorte.y + l->recorte.h;
    for (j = y0; j < y1; j++) {
        const Color *o = im->pix + (j - y) * im->w + (x0 - x);
        Color *d = l->cuadro + j * UI_ANCHO + x0;
        if (im->opaca) { if (x1 > x0) memcpy(d, o, (size_t)(x1 - x0) * sizeof(Color)); continue; }   /* (pista O) sin píxeles transparentes: la fila entera */
        for (i = x0; i < x1; i++, o++, d++) if (*o != t) *d = *o;
    }
}
