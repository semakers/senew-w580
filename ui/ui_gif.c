/* Imágenes desde ficheros: GIF → Imagen RGB565 en el montículo, una vez, al cargar.
 * El decodificador (LZW, paleta global y local, transparencia, entrelazado) es el de la pista P1
 * (propio/lib/gif.c, verificado allí píxel a píxel), adaptado para escribir en un búfer propio
 * en vez de en la pantalla. Cuadro a cuadro sobre un mismo lienzo: sirve para los animados (ui_gif_siguiente). */
#include "ui.h"
#include "img.h"

static u16 pref[4096];
static u8  suf[4096], pila[4096];
/* (R2, auditoría C2/A5) Un GIF es entrada AJENA y el fondo del standby se decodifica en cada arranque:
 *  - el rectángulo de cada cuadro se RECORTA al lienzo antes de guardarlo (venía en i16: con ix/iy >= 0x8000 quedaba
 *    negativo y «restaurar el fondo» escribía hasta 32 768 filas ANTES del lienzo);
 *  - un cuadro de 0 píxeles o de más píxeles que el mayor lienzo admitido se rechaza, y el LZW se deja en cuanto el
 *    cuadro está completo: el trabajo por cuadro queda acotado por GIF_MAX_PX (antes, 761 millones de vueltas con 301 KB);
 *  - además hay reloj: GIF_MAX_MS por cuadro (como PNG y JPEG), mirado cada 1024 códigos;
 *  - ninguna lectura pasa del final del fichero (extensión de control, paleta declarada más larga que el fichero). */
#define GIF_MAX_PX (2u * UI_PIXELES)
#define GIF_MAX_MS 15000u
int ui_img_error;

/* Decodifica el cuadro que empieza en g->pos sobre el lienzo (los píxeles transparentes dejan lo que
 * hubiera: así se componen las animaciones). Devuelve 1 si pintó un cuadro, 0 si no quedaban. */
static int gif_cuadro(UiGif *g)
{
    const u8 *d = g->fich; u32 n = g->n, p = g->pos;
    Color *dst = g->pix; u32 W = g->im.w, H = g->im.h;
    int transp = -1;
    u32 t0 = plat_ms(), cuenta = 0;
    g->espera_ms = 0;
    if (g->limpiar) {                                        /* el cuadro anterior pedía «restaurar el fondo» */
        u32 x, y, x1 = (u32)g->rx + g->rw, y1 = (u32)g->ry + g->rh;      /* ya recortado al lienzo al guardarlo; se vuelve a acotar */
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;
        for (y = g->ry; y < y1; y++) for (x = g->rx; x < x1; x++) dst[y * W + x] = TRANSPARENTE;
        g->limpiar = 0;
    }
    for (;;) {
        if (p + 1 >= n || d[p] == 0x3B) return 0;
        if (d[p] == 0x21) {                                  /* extensión */
            if (d[p + 1] == 0xF9 && p + 6 < n && d[p + 2] >= 4) {
                transp = (d[p + 3] & 1) ? d[p + 6] : -1;
                g->limpiar = ((d[p + 3] >> 2) & 7) == 2;
                g->espera_ms = (u16)((d[p + 4] | (d[p + 5] << 8)) * 10);
            }
            p += 2;
            while (p < n && d[p]) p += d[p] + 1u;
            p++;
            continue;
        }
        if (d[p] != 0x2C) return 0;
        break;
    }
    if (p + 10 >= n) return 0;
    {
        u32 ix = d[p + 1] | (d[p + 2] << 8), iy = d[p + 3] | (d[p + 4] << 8);
        u32 iw = d[p + 5] | (d[p + 6] << 8), ih = d[p + 7] | (d[p + 8] << 8);
        u32 marcas = d[p + 9], entrelazado = marcas & 0x40;
        const u8 *pal = g->pal_global, *fin_fich = d + n;
        u32 tam_min, borra, fin_cod, libre, tam, mascara, acum = 0, bits = 0, sub = 0;
        u32 x = 0, y = 0, pasada = 0;
        int viejo = -1, primero = 0, sp = 0;
        if (!iw || !ih || iw * ih > GIF_MAX_PX) return 0;    /* cuadro vacío o desmesurado: fichero dañado (iw, ih <= 65535: no desborda) */
        g->rx = (u16)(ix < W ? ix : W); g->ry = (u16)(iy < H ? iy : H);  /* el rectángulo, RECORTADO al lienzo */
        g->rw = (u16)(iw < W - g->rx ? iw : W - g->rx); g->rh = (u16)(ih < H - g->ry ? ih : H - g->ry);
        p += 10;
        if (marcas & 0x80) { pal = d + p; p += 3u << ((marcas & 7) + 1); }
        if (!pal || p >= n) return 0;
        tam_min = d[p++];
        if (tam_min < 2 || tam_min > 8) return 0;
        borra = 1u << tam_min; fin_cod = borra + 1; libre = borra + 2; tam = tam_min + 1; mascara = (1u << tam) - 1;
        for (;;) {
            u32 cod; int c;
            if (y >= ih) break;                              /* el cuadro ya está completo: lo que sobre no se decodifica */
            if (!(++cuenta & 1023) && (u32)(plat_ms() - t0) > GIF_MAX_MS) { ui_img_error = IMG_E_TIEMPO; return 0; }
            while (bits < tam) {
                if (!sub) {
                    if (p >= n || !d[p]) goto fin;
                    sub = d[p++];
                }
                if (p >= n) goto fin;
                acum |= (u32)d[p++] << bits; bits += 8; sub--;
            }
            cod = acum & mascara; acum >>= tam; bits -= tam;
            if (cod == borra) { libre = borra + 2; tam = tam_min + 1; mascara = (1u << tam) - 1; viejo = -1; continue; }
            if (cod == fin_cod) break;
            c = (int)cod;
            if (viejo < 0) { if (cod >= borra) break; pila[sp++] = (u8)cod; primero = (int)cod; }
            else {
                if (cod > libre) break;                      /* fichero corrupto */
                if (cod == libre) { pila[sp++] = (u8)primero; c = viejo; }
                while (c >= (int)borra && sp < 4095) { pila[sp++] = suf[c]; c = pref[c]; }
                pila[sp++] = (u8)c; primero = c;
                if (libre < 4096) {
                    pref[libre] = (u16)viejo; suf[libre] = (u8)c; libre++;
                    if (libre > mascara && tam < 12) { tam++; mascara = (1u << tam) - 1; }
                }
            }
            viejo = (int)cod;
            while (sp) {
                u32 i = pila[--sp];
                if (y < ih && (int)i != transp) {
                    u32 px = ix + x, py = iy + y;
                    if (px < W && py < H && pal + 3 * i + 2 < fin_fich) {    /* un índice más allá de la paleta que hay en el fichero no se pinta */
                        u32 v = ((u32)(pal[3 * i] >> 3) << 11) | ((u32)(pal[3 * i + 1] >> 2) << 5) | (pal[3 * i + 2] >> 3);
                        dst[py * W + px] = (Color)((v >> 8) | (v << 8));
                    }
                }
                if (++x == iw) {
                    x = 0;
                    if (!entrelazado) y++;
                    else {
                        static const u8 paso[4] = { 8, 8, 4, 2 }, ini[4] = { 0, 4, 2, 1 };
                        y += paso[pasada];
                        while (y >= ih && pasada < 3) y = ini[++pasada];
                    }
                }
            }
        }
fin:
        p += sub;                                            /* resto del sub-bloque en curso... */
        while (p < n && d[p]) p += d[p] + 1u;                /* ...y los que queden, hasta el terminador */
        p++;
    }
    g->pos = p;
    return 1;
}

/* Abre un GIF del sistema de ficheros: lee el fichero al montículo, reserva el lienzo y decodifica
 * el primer cuadro. NULL si no existe, no es un GIF o es mayor que 2 pantallas. */
static void *img_pedir(size_t n) { return mo_probar(n); }
static void img_soltar(void *p) { mo_soltar(p); }
/* PNG o JPEG ya leído en «fich»: lo decodifica (img/) a un UiGif de un solo cuadro. Suelta el fichero. */
/* (R2) Ni aquí ni en ui_gif_abrir se usa mo_pedir: si no hay memoria para una imagen, no hay imagen (antes: pánico). */
static u8 cubrir;                                         /* (W600) ui_gif_abrir_fondo: reducir lo justo para CUBRIR la pantalla */
static UiGif *abrir_fija(u8 *fich, u32 n, int jpeg)
{
    /* Normal: la reducción (1/2, 1/4, 1/8) más pequeña con la que la imagen CABE entera. Para un fondo (cubrir): la mayor con la
     * que todavía CUBRE la pantalla (se recorta al centrar): un fondo de 240x320 en el W600 (176x220) iba a 120x160 con bordes.
     * Con el tope «< 2 x pantalla» la reducción elegida deja cada lado >= la mitad de ese tope, o sea, ~>= la pantalla. */
    ImgOpc o = { .pedir = img_pedir, .soltar = img_soltar, .max_w = cubrir ? 2 * UI_ANCHO - 1 : UI_ANCHO, .max_h = cubrir ? 2 * UI_ALTO - 1 : UI_ALTO, .max_mem = 260 * 1024,
                 .reloj = plat_ms, .max_ms = 15000 };
    ImgSalida s; UiGif *g; u32 k; int r;
    r = jpeg ? img_jpeg(fich, n, &o, &s) : img_png(fich, n, &o, &s);
    if (r == IMG_E_GRANDE) {                                 /* ni a 1/8 cabe en la pantalla: lo mayor que quepa en memoria (se verá recortada) */
        o.max_w = o.max_h = 0; o.max_mem = 200 * 1024;
        r = jpeg ? img_jpeg(fich, n, &o, &s) : img_png(fich, n, &o, &s);
    }
    mo_soltar(fich);
    ui_img_error = r;
    if (r < 0) return NULL;
    if (!(g = mo_probar(sizeof *g))) { mo_soltar(s.pix); ui_img_error = IMG_E_MEMORIA; return NULL; }
    g->pix = s.pix; g->im.w = s.w; g->im.h = s.h; g->im.pix = s.pix; g->im.transparente = TRANSPARENTE; g->im.opaca = 1;   /* (pista O) el bucle de abajo lo garantiza */
    for (k = 0; k < (u32)s.w * s.h; k++) if (g->pix[k] == TRANSPARENTE) g->pix[k] = 0;   /* aquí no hay color «transparente» */
    g->formato = jpeg ? UI_F_JPEG : UI_F_PNG; g->reduccion = s.reduccion; g->parcial = r == IMG_PARCIAL;
    g->w_orig = s.w_orig; g->h_orig = s.h_orig; g->cuadro = 1;
    return g;
}
UiGif *ui_gif_abrir_fondo(const char *ruta) { UiGif *g; cubrir = 1; g = ui_gif_abrir(ruta); cubrir = 0; return g; }
UiGif *ui_gif_abrir(const char *ruta)
{
    UiGif *g; u8 *fich; u32 n, k, w, h;
    ui_img_error = 0;
    n = plat_fichero(ruta, NULL, 0);
    if (n < 13 || n > 300 * 1024) { if (n) ui_img_error = IMG_E_MEMORIA; return NULL; }
    if (!(fich = mo_probar(n))) { ui_img_error = IMG_E_MEMORIA; return NULL; }
    if (plat_fichero(ruta, fich, n) != n) { mo_soltar(fich); return NULL; }
    if (fich[0] == 0x89 && fich[1] == 'P' && fich[2] == 'N' && fich[3] == 'G') return abrir_fija(fich, n, 0);
    if (fich[0] == 0xFF && fich[1] == 0xD8) return abrir_fija(fich, n, 1);
    if (fich[0] != 'G' || fich[1] != 'I' || fich[2] != 'F') { mo_soltar(fich); return NULL; }
    w = (u32)fich[6] | ((u32)fich[7] << 8); h = (u32)fich[8] | ((u32)fich[9] << 8);      /* <= 65535: w * h cabe en u32 */
    if (!w || !h || w * h > GIF_MAX_PX) { mo_soltar(fich); ui_img_error = IMG_E_GRANDE; return NULL; }
    if (!(g = mo_probar(sizeof *g))) { mo_soltar(fich); ui_img_error = IMG_E_MEMORIA; return NULL; }
    if (!(g->pix = mo_probar((size_t)(w * h) * sizeof(Color)))) { mo_soltar(fich); mo_soltar(g); ui_img_error = IMG_E_MEMORIA; return NULL; }
    g->fich = fich; g->n = n;
    g->pal_global = (fich[10] & 0x80) ? fich + 13 : 0;
    g->inicio = g->pos = 13 + ((fich[10] & 0x80) ? 3u << ((fich[10] & 7) + 1) : 0);
    for (k = 0; k < w * h; k++) g->pix[k] = TRANSPARENTE;
    g->im.w = (u16)w; g->im.h = (u16)h; g->im.pix = g->pix; g->im.transparente = TRANSPARENTE; g->im.opaca = 0;   /* (pista O) un GIF sí deja píxeles sin pintar */
    g->w_orig = (u16)w; g->h_orig = (u16)h; g->reduccion = 1;
    if (!gif_cuadro(g)) { ui_gif_cerrar(g); return NULL; }
    g->cuadro = 1;
    return g;
}
/* Siguiente cuadro de la animación sobre el mismo lienzo; al acabar vuelve al primero. Devuelve los
 * milisegundos que pide ese cuadro (0 = el GIF no está animado: no hay que volver a llamar). */
int ui_gif_siguiente(UiGif *g)
{
    if (!g->fich) return 0;
    if (!gif_cuadro(g)) {
        if (g->cuadro <= 1) { ui_gif_soltar_fichero(g); return 0; }       /* un solo cuadro */
        g->pos = g->inicio; g->cuadro = 0; g->limpiar = 0;
        if (!gif_cuadro(g)) return 0;
    }
    g->cuadro++;
    return g->espera_ms ? g->espera_ms : 100;
}
void ui_gif_soltar_fichero(UiGif *g) { if (g->fich) { mo_soltar(g->fich); g->fich = NULL; } }
void ui_gif_cerrar(UiGif *g)
{
    if (!g) return;
    ui_gif_soltar_fichero(g);
    mo_soltar(g->pix); mo_soltar(g);
}

/* Carga (una vez; después sale de una tabla) la imagen de esa ruta del sistema de ficheros.
 * NULL si no hay FS, no existe o no es un GIF. La ruta debe ser una cadena estática. */
const Imagen *ui_imagen_fs(const char *ruta)
{
    static struct { const char *ruta; UiGif *g; } tabla[8];
    unsigned i;
    for (i = 0; i < 8 && tabla[i].ruta; i++) if (tabla[i].ruta == ruta) return tabla[i].g ? &tabla[i].g->im : NULL;
    if (i == 8) return NULL;
    tabla[i].ruta = ruta;
    tabla[i].g = ui_gif_abrir(ruta);
    if (tabla[i].g) ui_gif_soltar_fichero(tabla[i].g);
    return tabla[i].g ? &tabla[i].g->im : NULL;
}
