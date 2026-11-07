/* img_png.c — decodificador PNG PROPIO del sistema del W600 (contrato: img.h).
 *
 * Escrito a partir de las especificaciones (PNG, RFC 1950 zlib, RFC 1951 deflate). Sin libc (solo
 * memcpy/memset/memcmp/memmove), sin coma flotante, sin divisiones de 64 bits ni divisiones por un
 * divisor variable (no arrastra __aeabi_uidiv), sin setjmp, sin variables modificables estáticas.
 *
 * Cómo trabaja (todo en flujo, nunca el fichero descomprimido entero):
 *
 *   trozos IDAT ──► inflar() ──► ventana circular (≤ 32 KiB) ──► lineas() ──► fila_lista()
 *                                                                   │  desfiltra con la línea anterior
 *                                                                   ▼
 *                                                               emitir(): mezcla el alfa y
 *                                                               f = 1 : escribe el RGB565 en su sitio
 *                                                               f > 1 : suma en acumuladores de bloque
 *
 * Memoria pedida a la vez: estado (~5 KiB, con las tablas Huffman y la paleta) + ventana (32 KiB, o
 * menos si la imagen descomprimida entera es más pequeña) + 2 líneas de exploración + la salida RGB565
 * + (solo con reducción) los acumuladores: una fila de bloques (6 bytes por píxel de salida de ancho) o,
 * si además es entrelazada, 6 bytes por píxel de salida. El entrelazado Adam7 sin reducción no pide
 * nada más: cada píxel va directo a su sitio en la salida.
 *
 * Qué se comprueba: firma, CRC de IHDR, PLTE y tRNS, cabecera zlib y Adler-32 (si llegan sus 4 bytes).
 * El CRC de los IDAT NO se comprueba (el Adler-32 ya cubre los datos y así no se recorre dos veces).
 *
 * Resultado: IMG_OK si salieron todas las filas y el flujo no dio error; IMG_PARCIAL si salió al menos
 * una fila pero el flujo se cortó, estaba dañado o el Adler-32 no cuadra; IMG_E_DATOS si ni una fila.
 */
#include "img.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memmove(void *d, const void *s, size_t n);

#define VEN_MAX 32768u        /* ventana máxima de deflate */
#define RAP 9                 /* bits de la tabla rápida de Huffman */
#define PX_MAX 0x10000000u    /* más píxeles de salida que esto no se intentan (tamaños en u32) */

enum { Z_FIN = 0, Z_PARA = 1, Z_ERR = -1, Z_TIEMPO = -2 };
enum { CL_GRIS = 0, CL_PAL, CL_GRIS16, CL_GA, CL_RGB, CL_RGBA };

/* Un código de Huffman canónico: tabla rápida para los códigos de hasta RAP bits y, para los más
 * largos, la descripción canónica (cuántos códigos hay de cada longitud y los símbolos en orden). */
typedef struct {
    u16 rap[1 << RAP];        /* (símbolo << 4) | longitud; 0 = código largo o inexistente */
    u16 cuenta[16];
    u16 simb[288];
} Huff;

typedef struct {
    const ImgOpc *o;
    u32 t0;
    /* imagen */
    u32 w, h;
    u8  prof, tipo, entrel, clase;
    u8  bits_px, bpp, paso, clave;      /* bpp: bytes por píxel para los filtros; paso: bytes por muestra */
    u16 kr, kg, kb;                     /* color clave (tRNS en tipos 0 y 2) */
    u8  fr, fg, fb, f, sh;              /* fondo; factor de reducción y su log2 */
    u8  hay_plte, hay_trns;
    u8  pal[768];                       /* paleta; tras preparar(): ya mezclada con el fondo */
    u8  pal_a[256];
    /* salida */
    u16 *pix, *acum;
    u32 wo, ho, yo;
    /* líneas de exploración: [byte de filtro][datos] */
    u8  *act, *ant;
    u32 lin_bytes, lin_pos;
    u32 pase, npases, py, pw, ph, px0, py0, pxs, pys;
    u32 filas, sucia;
    /* entrada: cadena de trozos IDAT */
    const u8 *d;
    u32 n, pos;
    const u8 *p, *fin;
    /* ventana circular de salida de inflate */
    u8  *ven;
    u32 cap, wpos, env, llena;
    u32 ad_a, ad_b;
    u8  adler_mal;
    Huff ll, di;
    u8  lon[320];
} Png;

/* ------------------------------------------------------------------ utilidades */

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static int es(const u8 *t, char a, char b, char c, char d)
{
    return t[0] == (u8)a && t[1] == (u8)b && t[2] == (u8)c && t[3] == (u8)d;
}

/* CRC-32 (polinomio reflejado 0xEDB88320) de medio byte en medio byte: solo se usa en trozos pequeños. */
static u32 crc32_png(const u8 *p, u32 n)
{
    static const u32 T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
    };
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        c = T[c & 15] ^ (c >> 4);
        c = T[c & 15] ^ (c >> 4);
    }
    return ~c;
}

/* x / 255 exacto para 0 <= x < 65535 (lo máximo aquí es 255*255 + 127). */
static u32 div255(u32 x)
{
    return (x + 1 + (x >> 8)) >> 8;
}

/* c = (a*c + (255-a)*fondo + 127) / 255 */
static u32 mezclar(u32 c, u32 a, u32 fondo)
{
    return div255(a * c + (255 - a) * fondo + 127);
}

/* techo(2^22 / n) para n pequeño (1..64), por división larga bit a bit: con él,
 * (x * m) >> 22 == x / n para todo x <= 255*64 + 32 (comprobado exhaustivamente en probar_png.py). */
static u32 reciproco(u32 n)
{
    u32 q = 0, r = 0;
    int i;
    for (i = 22; i >= 0; i--) {
        r = (r << 1) | (i == 22 ? 1u : 0u);
        q <<= 1;
        if (r >= n) { r -= n; q |= 1; }
    }
    return q + (r != 0);
}

static u16 a565(u32 r, u32 g, u32 b)
{
    u32 v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (u16)((v >> 8) | (v << 8));
}

static int tiempo(const Png *s)
{
    const ImgOpc *o = s->o;
    if (!o->reloj || !o->max_ms) return 0;
    return (u32)(o->reloj() - s->t0) > o->max_ms;
}

/* ------------------------------------------------------------------ cabecera */

typedef struct { u32 w, h; u8 prof, tipo, entrel; } Cab;

static int cabecera(const u8 *d, u32 n, Cab *c)
{
    static const u8 firma[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    u32 i;
    int bien;
    c->w = c->h = 0;
    if (!d || n < 33) return IMG_E_FORMATO;
    for (i = 0; i < 8; i++) if (d[i] != firma[i]) return IMG_E_FORMATO;
    if (be32(d + 8) != 13 || !es(d + 12, 'I', 'H', 'D', 'R')) return IMG_E_FORMATO;
    if (crc32_png(d + 12, 17) != be32(d + 29)) return IMG_E_FORMATO;
    c->w = be32(d + 16);
    c->h = be32(d + 20);
    c->prof = d[24];
    c->tipo = d[25];
    c->entrel = d[28];
    if (c->w == 0 || c->h == 0 || d[26] != 0 || d[27] != 0 || c->entrel > 1) return IMG_E_FORMATO;
    switch (c->tipo) {
    case 0:  bien = c->prof == 1 || c->prof == 2 || c->prof == 4 || c->prof == 8 || c->prof == 16; break;
    case 3:  bien = c->prof == 1 || c->prof == 2 || c->prof == 4 || c->prof == 8; break;
    case 2: case 4: case 6: bien = c->prof == 8 || c->prof == 16; break;
    default: bien = 0;
    }
    if (!bien) return IMG_E_FORMATO;
    if (c->w > 65535u || c->h > 65535u) return IMG_E_GRANDE;   /* w_orig/h_orig son u16 */
    return IMG_OK;
}

static void salida_vacia(ImgSalida *s)
{
    s->w = s->h = s->w_orig = s->h_orig = 0;
    s->reduccion = 1;
    s->alfa = 0;
    s->mem_pico = 0;
    s->pix = NULL;
}

int img_png_info(const u8 *d, u32 n, ImgSalida *s)
{
    Cab c;
    u32 pos = 33;
    int r;
    if (!s) return IMG_E_FORMATO;
    salida_vacia(s);
    r = cabecera(d, n, &c);
    if (r != IMG_OK) return r;
    s->w_orig = (u16)c.w;
    s->h_orig = (u16)c.h;
    s->alfa = (c.tipo == 4 || c.tipo == 6);
    /* un tRNS antes del primer IDAT también es transparencia */
    while (!s->alfa && pos < n && n - pos >= 8) {
        u32 len = be32(d + pos), disp = n - pos - 8;
        const u8 *t = d + pos + 4;
        if (es(t, 'I', 'D', 'A', 'T') || es(t, 'I', 'E', 'N', 'D')) break;
        if (len > disp || disp - len < 4) break;
        if (es(t, 't', 'R', 'N', 'S') && c.tipo != 4 && c.tipo != 6) s->alfa = 1;
        pos += 12 + len;
    }
    return IMG_OK;
}

/* ------------------------------------------------------------------ Huffman */

/* Arma el código canónico de n símbolos con longitudes lon[] (0 = no se usa).
 * Devuelve -1 si las longitudes piden más códigos de los que existen. Un código incompleto se acepta:
 * los patrones sin símbolo dan error al decodificarlos. */
static int huff_armar(Huff *h, const u8 *lon, u32 n)
{
    u16 sig[16];
    u32 i, k, cod, l;
    int resto = 1;
    for (i = 0; i < 16; i++) h->cuenta[i] = 0;
    for (i = 0; i < n; i++) h->cuenta[lon[i]]++;
    h->cuenta[0] = 0;
    for (l = 1; l < 16; l++) {
        resto = resto * 2 - (int)h->cuenta[l];
        if (resto < 0) return -1;
    }
    sig[0] = sig[1] = 0;
    for (l = 1; l < 15; l++) sig[l + 1] = (u16)(sig[l] + h->cuenta[l]);
    for (i = 0; i < n; i++) if (lon[i]) h->simb[sig[lon[i]]++] = (u16)i;
    /* tabla rápida: cada código corto, con los bits al revés (deflate los manda del más alto al más
     * bajo, pero el flujo se lee del bit bajo al alto), repetido en todas las entradas que lo llevan */
    for (i = 0; i < (1u << RAP); i++) h->rap[i] = 0;
    cod = 0;
    k = 0;
    for (l = 1; l <= RAP; l++) {
        u32 j;
        for (j = 0; j < h->cuenta[l]; j++, k++, cod++) {
            u32 rev = 0, c = cod, b;
            for (b = 0; b < l; b++) { rev = (rev << 1) | (c & 1); c >>= 1; }
            for (; rev < (1u << RAP); rev += 1u << l) h->rap[rev] = (u16)((h->simb[k] << 4) | l);
        }
        cod <<= 1;
    }
    return 0;
}

/* Decodifica un símbolo de los bits dados (hay al menos 15, quizá de relleno). Deja la longitud en *l.
 * Devuelve -1 si el patrón no corresponde a ningún código. */
static int huff_decod(const Huff *h, u32 bits, int *l)
{
    u32 e = h->rap[bits & ((1u << RAP) - 1)];
    u32 cod = 0, prim = 0, ind = 0;
    int i;
    if (e) { *l = (int)(e & 15); return (int)(e >> 4); }
    /* código largo: se recorre longitud a longitud con la descripción canónica */
    for (i = 1; i <= 15; i++) {
        u32 cnt = h->cuenta[i];
        cod |= bits & 1;
        bits >>= 1;
        if (cod - prim < cnt) { *l = i; return h->simb[ind + (cod - prim)]; }
        ind += cnt;
        prim = (prim + cnt) << 1;
        cod <<= 1;
    }
    return -1;
}

/* ------------------------------------------------------------------ entrada: los IDAT */

/* Pasa al siguiente trozo IDAT con datos. 0 si no hay más (fin, IEND o fichero cortado). */
static int seg_sig(Png *s)
{
    for (;;) {
        u32 len, ini, disp;
        const u8 *t;
        int idat;
        if (s->pos >= s->n || s->n - s->pos < 8) return 0;
        len = be32(s->d + s->pos);
        t = s->d + s->pos + 4;
        ini = s->pos + 8;
        disp = s->n - ini;
        if (es(t, 'I', 'E', 'N', 'D')) { s->pos = s->n; return 0; }
        idat = es(t, 'I', 'D', 'A', 'T');
        if (len > disp) { len = disp; s->pos = s->n; }       /* longitud mentirosa o fichero cortado */
        else if (disp - len < 4) s->pos = s->n;
        else s->pos = ini + len + 4;
        if (idat && len) { s->p = s->d + ini; s->fin = s->p + len; return 1; }
    }
}

/* ------------------------------------------------------------------ salida de píxeles */

/* Prepara los acumuladores de la fila de bloques yo como si todo el bloque fuera fondo: cada píxel que
 * llega suma (color - fondo) en aritmética de 16 bits, así los que nunca llegan cuentan como fondo. */
static void acum_ini(const Png *s, u32 yo, u16 *ac)
{
    u32 f = s->f, bh = s->h - yo * f, xo, n;
    if (bh > f) bh = f;
    n = f * bh;
    for (xo = 0; xo < s->wo; xo++, ac += 3) {
        if (xo == s->wo - 1) n = (s->w - xo * f) * bh;
        ac[0] = (u16)(s->fr * n);
        ac[1] = (u16)(s->fg * n);
        ac[2] = (u16)(s->fb * n);
    }
}

/* Convierte una fila de bloques acumulada en píxeles: media redondeada (suma + n/2) / n. */
static void volcar(const Png *s, u32 yo, const u16 *ac)
{
    u32 f = s->f, bh = s->h - yo * f, xo, n, m, n2, m2;
    u16 *dst = s->pix + yo * s->wo;
    if (bh > f) bh = f;
    n = f * bh;
    m = reciproco(n);
    n2 = (s->w - (s->wo - 1) * f) * bh;
    m2 = reciproco(n2);
    for (xo = 0; xo < s->wo; xo++, ac += 3) {
        if (xo == s->wo - 1) { n = n2; m = m2; }
        dst[xo] = a565(((ac[0] + (n >> 1)) * m) >> 22, ((ac[1] + (n >> 1)) * m) >> 22,
                       ((ac[2] + (n >> 1)) * m) >> 22);
    }
}

/* Una línea ya desfiltrada (lin apunta a sus datos) de la fila Y de la imagen. */
static void emitir(Png *s, const u8 *lin, u32 Y)
{
    u32 i, X = s->px0, dx = 1u << s->pxs, pw = s->pw;
    u32 prof = s->prof, mask = (1u << (prof > 8 ? 8 : prof)) - 1, paso = s->paso, bpp = s->bpp;
    u32 esc = prof == 1 ? 255 : prof == 2 ? 85 : prof == 4 ? 17 : 1;
    u32 fr = s->fr, fg = s->fg, fb = s->fb;
    u16 *dst = NULL, *ac = NULL;
    if (s->f == 1) dst = s->pix + Y * s->w;
    else ac = s->acum + (s->entrel ? (Y >> s->sh) * s->wo * 3 : 0);
    /* (pista O) El caso de casi todas las fotos (RGB de 8 bits, sin color clave, sin reducir, sin entrelazar): el mismo
     * a565() por píxel, sin el reparto por clase ni la mezcla de alfa de cada vuelta (28 -> 10 instrucciones por píxel). */
    if (dst && s->clase == CL_RGB && paso == 1 && bpp == 3 && !s->clave && dx == 1) {
        const u8 *q = lin;
        u16 *d = dst + X;
        for (i = 0; i < pw; i++, q += 3) d[i] = a565(q[0], q[1], q[2]);
        return;
    }
    for (i = 0; i < pw; i++, X += dx) {
        u32 r, g, b, a = 255, bit;
        const u8 *q;
        switch (s->clase) {
        case CL_PAL:
            bit = i * prof;
            q = s->pal + 3 * ((lin[bit >> 3] >> (8 - prof - (bit & 7))) & mask);
            r = q[0]; g = q[1]; b = q[2];                    /* la paleta ya está mezclada con el fondo */
            break;
        case CL_GRIS:
            bit = i * prof;
            r = (lin[bit >> 3] >> (8 - prof - (bit & 7))) & mask;
            if (s->clave && r == s->kr) a = 0;
            r *= esc; g = b = r;
            break;
        case CL_GRIS16:
            q = lin + i * 2;
            r = g = b = q[0];
            if (s->clave && (((u32)q[0] << 8) | q[1]) == s->kr) a = 0;
            break;
        case CL_GA:
            q = lin + i * bpp;
            r = g = b = q[0];
            a = q[paso];
            break;
        case CL_RGB:
            q = lin + i * bpp;
            r = q[0]; g = q[paso]; b = q[2 * paso];
            if (s->clave) {
                if (paso == 1) { if (r == s->kr && g == s->kg && b == s->kb) a = 0; }
                else if ((((u32)q[0] << 8) | q[1]) == s->kr && (((u32)q[2] << 8) | q[3]) == s->kg &&
                         (((u32)q[4] << 8) | q[5]) == s->kb) a = 0;
            }
            break;
        default: /* CL_RGBA */
            q = lin + i * bpp;
            r = q[0]; g = q[paso]; b = q[2 * paso]; a = q[3 * paso];
            break;
        }
        if (a != 255) { r = mezclar(r, a, fr); g = mezclar(g, a, fg); b = mezclar(b, a, fb); }
        if (dst) dst[X] = a565(r, g, b);
        else {
            u16 *t = ac + (X >> s->sh) * 3;
            t[0] = (u16)(t[0] + r - fr);
            t[1] = (u16)(t[1] + g - fg);
            t[2] = (u16)(t[2] + b - fb);
        }
    }
}

/* ------------------------------------------------------------------ líneas de exploración */

/* Abre el pase s->pase (o el siguiente que tenga píxeles). Si no queda ninguno, s->pase == s->npases. */
static void pase_abrir(Png *s)
{
    static const u8 X0[7] = { 0, 4, 0, 2, 0, 1, 0 }, Y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const u8 XS[7] = { 3, 3, 2, 2, 1, 1, 0 }, YS[7] = { 3, 3, 3, 2, 2, 1, 1 };
    for (; s->pase < s->npases; s->pase++) {
        if (s->entrel) {
            s->px0 = X0[s->pase]; s->py0 = Y0[s->pase]; s->pxs = XS[s->pase]; s->pys = YS[s->pase];
        } else {
            s->px0 = s->py0 = s->pxs = s->pys = 0;
        }
        s->pw = s->w > s->px0 ? (s->w - s->px0 + (1u << s->pxs) - 1) >> s->pxs : 0;
        s->ph = s->h > s->py0 ? (s->h - s->py0 + (1u << s->pys) - 1) >> s->pys : 0;
        if (s->pw && s->ph) break;
    }
    if (s->pase >= s->npases) return;
    s->lin_bytes = (s->pw * s->bits_px + 7) >> 3;
    s->lin_pos = 0;
    s->py = 0;
    memset(s->ant, 0, s->lin_bytes + 1);       /* la primera línea de cada pase no tiene «arriba» */
}

/* Deshace el filtro de la línea a (con b la anterior ya desfiltrada). -1 si el tipo de filtro no existe. */
static int desfiltrar(u8 *a, const u8 *b, u32 n, u32 bpp)
{
    u8 *x = a + 1;
    const u8 *up = b + 1;
    u32 i;
    switch (a[0]) {
    case 0:
        break;
    case 1:                                                             /* Sub */
        for (i = bpp; i < n; i++) x[i] = (u8)(x[i] + x[i - bpp]);
        break;
    case 2:                                                             /* Up */
        for (i = 0; i < n; i++) x[i] = (u8)(x[i] + up[i]);
        break;
    case 3:                                                             /* Average */
        for (i = 0; i < bpp && i < n; i++) x[i] = (u8)(x[i] + (up[i] >> 1));
        for (; i < n; i++) x[i] = (u8)(x[i] + ((x[i - bpp] + up[i]) >> 1));
        break;
    case 4:                                                             /* Paeth */
        for (i = 0; i < bpp && i < n; i++) x[i] = (u8)(x[i] + up[i]);
        for (; i < n; i++) {
            int iz = x[i - bpp], ar = up[i], di = up[i - bpp];
            int pa = ar - di, pb = iz - di, pc = pa + pb;
            if (pa < 0) pa = -pa;
            if (pb < 0) pb = -pb;
            if (pc < 0) pc = -pc;
            x[i] = (u8)(x[i] + (pa <= pb && pa <= pc ? iz : pb <= pc ? ar : di));
        }
        break;
    default:
        return -1;
    }
    return 0;
}

/* La línea en s->act está completa. */
static int fila_lista(Png *s)
{
    u32 Y = s->py0 + (s->py << s->pys);
    u8 *t;
    if (desfiltrar(s->act, s->ant, s->lin_bytes, s->bpp)) return Z_ERR;
    emitir(s, s->act + 1, Y);
    s->filas++;
    if (s->f > 1 && !s->entrel) {
        s->sucia = 1;
        if (((Y + 1) & (s->f - 1u)) == 0 || Y + 1 == s->h) {
            volcar(s, s->yo, s->acum);
            s->sucia = 0;
            if (Y + 1 < s->h) acum_ini(s, ++s->yo, s->acum);
        }
    }
    t = s->act; s->act = s->ant; s->ant = t;
    s->lin_pos = 0;
    if (++s->py == s->ph) { s->pase++; pase_abrir(s); }
    if ((s->filas & 15) == 0 && tiempo(s)) return Z_TIEMPO;
    return 0;
}

/* Consumidor de los bytes descomprimidos. Z_PARA si llegan bytes con la imagen ya completa. */
static int lineas(Png *s, const u8 *q, u32 n)
{
    while (n) {
        u32 k;
        int r;
        if (s->pase >= s->npases) return Z_PARA;
        k = s->lin_bytes + 1 - s->lin_pos;
        if (k > n) k = n;
        memcpy(s->act + s->lin_pos, q, k);
        s->lin_pos += k;
        q += k;
        n -= k;
        if (s->lin_pos == s->lin_bytes + 1) {
            r = fila_lista(s);
            if (r) return r;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ inflate */

/* Entrega al consumidor lo escrito en la ventana desde la última vez y, si está llena, la rebobina. */
static int vaciar(Png *s)
{
    u32 n = s->wpos - s->env, a = s->ad_a, b = s->ad_b, k;
    const u8 *q = s->ven + s->env, *x = q;
    int r = 0;
    k = n;
    while (k) {                                 /* Adler-32: el módulo se aplaza 5552 bytes */
        u32 t = k > 5552 ? 5552 : k;
        k -= t;
        for (; t >= 8; t -= 8, x += 8) {            /* (pista O) de 8 en 8 bytes: las mismas sumas en el mismo orden */
            a += x[0]; b += a; a += x[1]; b += a; a += x[2]; b += a; a += x[3]; b += a;
            a += x[4]; b += a; a += x[5]; b += a; a += x[6]; b += a; a += x[7]; b += a;
        }
        while (t--) { a += *x++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    s->ad_a = a;
    s->ad_b = b;
    s->env = s->wpos;
    if (s->wpos == s->cap) { s->wpos = 0; s->env = 0; s->llena = 1; }
    if (n) r = lineas(s, q, n);
    if (!r && tiempo(s)) r = Z_TIEMPO;
    return r;
}

/* Descomprime el flujo zlib de los IDAT. Z_FIN: terminó bien (s->adler_mal si el Adler-32 no cuadra);
 * Z_PARA: el consumidor ya tiene la imagen y sobran datos; Z_ERR: dañado o cortado; Z_TIEMPO. */
static int inflar(Png *s)
{
    static const u8 orden[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    static const u16 lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                   67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const u16 dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                   769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    u32 bits = 0, i;
    int nb = 0, rell = 0, ultimo, r, l, sym;

    /* Al acabarse la entrada se rellena con ceros y se cuenta (rell): un código puede necesitar menos
     * bits de los que se miran. En cuanto se CONSUME un bit de relleno, el flujo estaba cortado. */
#define NECESITA(k) \
    while (nb < (int)(k)) { \
        if (s->p == s->fin && !seg_sig(s)) rell += 8; \
        else bits |= (u32)*s->p++ << nb; \
        nb += 8; \
    }
#define TOMAR(k) do { bits >>= (k); nb -= (int)(k); if (nb < rell) goto mal; } while (0)
#define SACAR(v) do { \
        s->ven[s->wpos++] = (u8)(v); \
        if (s->wpos == s->cap) { r = vaciar(s); if (r) return r; } \
    } while (0)

    /* cabecera zlib: método 8, ventana <= 32 KiB, comprobación mod 31, sin diccionario */
    NECESITA(16);
    {
        u32 cmf = bits & 255, flg = (bits >> 8) & 255;
        if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31u || (flg & 32)) goto mal;
    }
    TOMAR(16);

    do {
        u32 tipo;
        NECESITA(3);
        ultimo = (int)(bits & 1);
        tipo = (bits >> 1) & 3;
        TOMAR(3);
        if (tipo == 0) {                                    /* bloque almacenado */
            u32 len;
            TOMAR(nb & 7);
            NECESITA(32);
            len = bits & 0xFFFFu;
            if ((len ^ 0xFFFFu) != (bits >> 16)) goto mal;
            bits = 0;
            nb -= 32;
            if (nb < rell) goto mal;
            while (len) {
                u32 k;
                if (s->p == s->fin && !seg_sig(s)) goto mal;
                k = (u32)(s->fin - s->p);
                if (k > len) k = len;
                if (k > s->cap - s->wpos) k = s->cap - s->wpos;
                memcpy(s->ven + s->wpos, s->p, k);
                s->p += k;
                s->wpos += k;
                len -= k;
                if (s->wpos == s->cap) { r = vaciar(s); if (r) return r; }
            }
            continue;
        }
        if (tipo == 3) goto mal;
        if (tipo == 1) {                                    /* Huffman fijo */
            for (i = 0; i < 144; i++) s->lon[i] = 8;
            for (; i < 256; i++) s->lon[i] = 9;
            for (; i < 280; i++) s->lon[i] = 7;
            for (; i < 288; i++) s->lon[i] = 8;
            huff_armar(&s->ll, s->lon, 288);
            for (i = 0; i < 30; i++) s->lon[i] = 5;
            huff_armar(&s->di, s->lon, 30);
        } else {                                            /* Huffman dinámico */
            u32 hl, hd, hc;
            NECESITA(14);
            hl = (bits & 31) + 257;
            hd = ((bits >> 5) & 31) + 1;
            hc = ((bits >> 10) & 15) + 4;
            TOMAR(14);
            if (hl > 286 || hd > 30) goto mal;
            for (i = 0; i < 19; i++) s->lon[i] = 0;
            for (i = 0; i < hc; i++) {
                NECESITA(3);
                s->lon[orden[i]] = (u8)(bits & 7);
                TOMAR(3);
            }
            if (huff_armar(&s->ll, s->lon, 19)) goto mal;   /* el código de las longitudes, de prestado */
            i = 0;
            while (i < hl + hd) {
                NECESITA(15);
                sym = huff_decod(&s->ll, bits, &l);
                if (sym < 0) goto mal;
                TOMAR(l);
                if (sym < 16) s->lon[i++] = (u8)sym;
                else {
                    u32 rep;
                    u8 val = 0;
                    if (sym == 16) {
                        if (!i) goto mal;
                        val = s->lon[i - 1];
                        NECESITA(2); rep = 3 + (bits & 3); TOMAR(2);
                    } else if (sym == 17) {
                        NECESITA(3); rep = 3 + (bits & 7); TOMAR(3);
                    } else {
                        NECESITA(7); rep = 11 + (bits & 127); TOMAR(7);
                    }
                    if (i + rep > hl + hd) goto mal;
                    while (rep--) s->lon[i++] = val;
                }
            }
            if (huff_armar(&s->di, s->lon + hl, hd)) goto mal;
            if (huff_armar(&s->ll, s->lon, hl)) goto mal;
        }
        for (;;) {                                          /* símbolos del bloque */
            NECESITA(15);
            sym = huff_decod(&s->ll, bits, &l);
            if (sym < 0) goto mal;
            TOMAR(l);
            if (sym < 256) {
                SACAR(sym);
            } else if (sym == 256) {
                break;
            } else {
                u32 len, dist, eb, src;
                sym -= 257;
                if (sym >= 29) goto mal;
                eb = (sym < 8 || sym == 28) ? 0 : (u32)(sym - 4) >> 2;
                NECESITA(eb);
                len = lbase[sym] + (bits & ((1u << eb) - 1));
                TOMAR(eb);
                NECESITA(15);
                sym = huff_decod(&s->di, bits, &l);
                if (sym < 0 || sym >= 30) goto mal;
                TOMAR(l);
                eb = sym < 4 ? 0 : (u32)(sym - 2) >> 1;
                NECESITA(eb);
                dist = dbase[sym] + (bits & ((1u << eb) - 1));
                TOMAR(eb);
                if (dist > (s->llena ? s->cap : s->wpos)) goto mal;    /* más atrás de lo que existe */
                src = s->wpos >= dist ? s->wpos - dist : s->wpos + s->cap - dist;
                /* (pista O) por trozos: hasta que el origen o el destino lleguen al final de la ventana no hay nada que
                 * comprobar byte a byte. La copia sigue siendo hacia delante y de uno en uno (dist puede ser < len). */
                while (len) {
                    u32 k = len, i2;
                    u8 *ven = s->ven;
                    if (k > s->cap - src) k = s->cap - src;
                    if (k > s->cap - s->wpos) k = s->cap - s->wpos;
                    { u8 *d = ven + s->wpos; const u8 *o2 = ven + src; for (i2 = 0; i2 < k; i2++) d[i2] = o2[i2]; }
                    len -= k; src += k; s->wpos += k;
                    if (src == s->cap) src = 0;
                    if (s->wpos == s->cap) { r = vaciar(s); if (r) return r; }
                }
            }
        }
    } while (!ultimo);

    r = vaciar(s);
    if (r) return r;
    /* Adler-32 (4 bytes, el alto primero) tras alinear a byte; si no llegan los 4, no se comprueba */
    bits >>= nb & 7;
    nb -= nb & 7;
    NECESITA(32);
    if (!rell) {
        u32 ad = ((bits & 255) << 24) | ((bits >> 8 & 255) << 16) | ((bits >> 16 & 255) << 8) | (bits >> 24);
        if (ad != ((s->ad_b << 16) | s->ad_a)) s->adler_mal = 1;
    }
    return Z_FIN;

mal:
    r = vaciar(s);                                          /* lo ya descomprimido aún vale filas */
    return r == Z_TIEMPO ? Z_TIEMPO : Z_ERR;
#undef NECESITA
#undef TOMAR
#undef SACAR
}

/* ------------------------------------------------------------------ trozos previos a los IDAT */

/* Lee PLTE y tRNS y deja s->pos en el primer IDAT. */
static int previos(Png *s)
{
    const u8 *d = s->d;
    u32 n = s->n, pos = 33, i;
    for (;;) {
        u32 len, disp;
        const u8 *t;
        if (pos >= n || n - pos < 8) return IMG_E_DATOS;             /* no hay ningún IDAT */
        len = be32(d + pos);
        t = d + pos + 4;
        disp = n - pos - 8;
        if (es(t, 'I', 'D', 'A', 'T')) break;
        if (es(t, 'I', 'E', 'N', 'D')) return IMG_E_DATOS;
        if (len > disp || disp - len < 4) return IMG_E_DATOS;        /* trozo cortado antes de los datos */
        if (es(t, 'P', 'L', 'T', 'E') && s->tipo == 3 && !s->hay_plte) {
            if (len == 0 || len > 768 || len % 3u) return IMG_E_DATOS;
            if (crc32_png(t, len + 4) != be32(t + 4 + len)) return IMG_E_DATOS;
            memcpy(s->pal, t + 4, len);
            s->hay_plte = 1;
        } else if (es(t, 't', 'R', 'N', 'S') && s->tipo != 4 && s->tipo != 6 && !s->hay_trns) {
            if ((s->tipo == 3 && len > 256) || (s->tipo == 0 && len != 2) || (s->tipo == 2 && len != 6))
                return IMG_E_DATOS;
            if (crc32_png(t, len + 4) != be32(t + 4 + len)) return IMG_E_DATOS;
            if (s->tipo == 3) {
                for (i = 0; i < len; i++) s->pal_a[i] = t[4 + i];
            } else {
                /* de la clave solo cuentan los bits de la profundidad */
                u32 m = s->prof == 16 ? 0xFFFFu : (1u << s->prof) - 1;
                s->kr = (u16)((((u32)t[4] << 8) | t[5]) & m);
                if (s->tipo == 2) {
                    s->kg = (u16)((((u32)t[6] << 8) | t[7]) & m);
                    s->kb = (u16)((((u32)t[8] << 8) | t[9]) & m);
                }
                s->clave = 1;
            }
            s->hay_trns = 1;
        }
        pos += 12 + len;
    }
    if (s->tipo == 3 && !s->hay_plte) return IMG_E_DATOS;
    s->pos = pos;
    /* la paleta se mezcla con el fondo una sola vez */
    if (s->tipo == 3)
        for (i = 0; i < 256; i++) {
            u32 a = s->pal_a[i];
            if (a != 255) {
                s->pal[3 * i]     = (u8)mezclar(s->pal[3 * i], a, s->fr);
                s->pal[3 * i + 1] = (u8)mezclar(s->pal[3 * i + 1], a, s->fg);
                s->pal[3 * i + 2] = (u8)mezclar(s->pal[3 * i + 2], a, s->fb);
            }
        }
    return IMG_OK;
}

/* ------------------------------------------------------------------ entrada pública */

int img_png(const u8 *d, u32 n, const ImgOpc *o, ImgSalida *sal)
{
    Cab c;
    Png *s;
    u32 canales, bits_px, lin, fijo, cap, f, sh = 0, wo = 0, ho = 0, mem = 0, px, i, total;
    int r, z;

    if (!sal) return IMG_E_FORMATO;
    salida_vacia(sal);
    r = cabecera(d, n, &c);
    if (r != IMG_OK) return r;
    sal->w_orig = (u16)c.w;
    sal->h_orig = (u16)c.h;
    if (!o || !o->pedir || !o->soltar) return IMG_E_MEMORIA;

    canales = c.tipo == 2 ? 3 : c.tipo == 4 ? 2 : c.tipo == 6 ? 4 : 1;
    bits_px = canales * c.prof;
    lin = (c.w * bits_px + 7) >> 3;                      /* como mucho 8 * 65535 */

    /* ventana: 32 KiB, o menos si la imagen descomprimida entera (líneas + bytes de filtro) no llega */
    total = 0;
    if (c.entrel) {
        static const u8 X0[7] = { 0, 4, 0, 2, 0, 1, 0 }, Y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
        static const u8 XS[7] = { 3, 3, 2, 2, 1, 1, 0 }, YS[7] = { 3, 3, 3, 2, 2, 1, 1 };
        for (i = 0; i < 7 && total < VEN_MAX; i++) {
            u32 pw = c.w > X0[i] ? (c.w - X0[i] + (1u << XS[i]) - 1) >> XS[i] : 0;
            u32 ph = c.h > Y0[i] ? (c.h - Y0[i] + (1u << YS[i]) - 1) >> YS[i] : 0;
            u32 lb = ((pw * bits_px + 7) >> 3) + 1;
            if (!pw || !ph) continue;
            total = (lb >= VEN_MAX || ph >= VEN_MAX) ? VEN_MAX : total + lb * ph;
        }
    } else {
        total = (lin + 1 >= VEN_MAX || c.h >= VEN_MAX) ? VEN_MAX : (lin + 1) * c.h;
    }
    cap = total > VEN_MAX ? VEN_MAX : total;
    fijo = (u32)sizeof(Png) + cap + 2 * (lin + 1);

    /* la menor reducción (1, 2, 4, 8) que quepa en max_w x max_h y en max_mem (0 = sin tope) */
    for (f = 1; f <= 8; f <<= 1, sh++) {
        wo = (c.w + f - 1) >> sh;
        ho = (c.h + f - 1) >> sh;
        px = wo * ho;
        if ((o->max_w && wo > o->max_w) || (o->max_h && ho > o->max_h) || px > PX_MAX) continue;
        mem = fijo + px * 2;
        if (f > 1) mem += (c.entrel ? px : wo) * 6;
        if (o->max_mem && mem > o->max_mem) continue;
        break;
    }
    if (f > 8) return IMG_E_GRANDE;

    s = (Png *)o->pedir(sizeof(Png));
    if (!s) return IMG_E_MEMORIA;
    memset(s, 0, sizeof(Png));
    s->o = o;
    s->t0 = (o->reloj && o->max_ms) ? o->reloj() : 0;
    s->d = d; s->n = n;
    s->w = c.w; s->h = c.h; s->prof = c.prof; s->tipo = c.tipo; s->entrel = c.entrel;
    s->bits_px = (u8)bits_px;
    s->bpp = (u8)(bits_px < 8 ? 1 : bits_px >> 3);
    s->paso = (u8)(c.prof == 16 ? 2 : 1);
    s->clase = (u8)(c.tipo == 3 ? CL_PAL : c.tipo == 4 ? CL_GA : c.tipo == 2 ? CL_RGB : c.tipo == 6 ? CL_RGBA :
                    c.prof == 16 ? CL_GRIS16 : CL_GRIS);
    s->fr = o->fondo_r; s->fg = o->fondo_g; s->fb = o->fondo_b;
    s->f = (u8)f; s->sh = (u8)sh; s->wo = wo; s->ho = ho;
    s->cap = cap;
    s->npases = c.entrel ? 7 : 1;
    s->ad_a = 1;
    memset(s->pal_a, 255, 256);

    r = previos(s);
    if (r == IMG_OK) {
        s->pix = (u16 *)o->pedir((size_t)wo * ho * 2);
        s->ven = (u8 *)o->pedir(cap);
        s->act = (u8 *)o->pedir(lin + 1);
        s->ant = (u8 *)o->pedir(lin + 1);
        if (f > 1) s->acum = (u16 *)o->pedir((size_t)(c.entrel ? wo * ho : wo) * 6);
        if (!s->pix || !s->ven || !s->act || !s->ant || (f > 1 && !s->acum)) r = IMG_E_MEMORIA;
    }
    if (r == IMG_OK) {
        u16 fondo = a565(s->fr, s->fg, s->fb);
        for (i = 0; i < wo * ho; i++) s->pix[i] = fondo;
        if (f > 1) {
            if (c.entrel) for (i = 0; i < ho; i++) acum_ini(s, i, s->acum + i * wo * 3);
            else acum_ini(s, 0, s->acum);
        }
        pase_abrir(s);
        z = inflar(s);
        /* lo acumulado y aún sin volcar */
        if (f > 1 && z != Z_TIEMPO) {
            if (c.entrel) { if (s->filas) for (i = 0; i < ho; i++) volcar(s, i, s->acum + i * wo * 3); }
            else if (s->sucia) volcar(s, s->yo, s->acum);
        }
        if (z == Z_TIEMPO) r = IMG_E_TIEMPO;
        else if (!s->filas) r = IMG_E_DATOS;
        else if (s->pase >= s->npases && (z == Z_PARA || (z == Z_FIN && !s->adler_mal))) r = IMG_OK;
        else r = IMG_PARCIAL;
    }

    if (s->ven) o->soltar(s->ven);
    if (s->act) o->soltar(s->act);
    if (s->ant) o->soltar(s->ant);
    if (s->acum) o->soltar(s->acum);
    if (r == IMG_OK || r == IMG_PARCIAL) {
        sal->pix = s->pix;
        sal->w = (u16)wo;
        sal->h = (u16)ho;
        sal->reduccion = (u8)f;
        sal->alfa = (u8)(c.tipo == 4 || c.tipo == 6 || s->hay_trns);
        sal->mem_pico = mem;
    } else if (s->pix) {
        o->soltar(s->pix);
    }
    o->soltar(s);
    return r;
}
