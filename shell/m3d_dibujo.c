/* m3d_dibujo.c — el dibujo del menú 3D (pista V, docs/menu3d.md §1). Ver m3d.h.
 *
 * UN ICONO: su malla (shell/mallas3d.h, generada) se gira, se ilumina y se proyecta aquí, en enteros; los
 * triángulos van al rasterizador r3d (sistema/r3d/, el nuestro del W600) en modo RAMPA con z-buffer, sobre un
 * búfer propio al DOBLE de resolución; después se baja 2x2 mezclando con lo que haya en pantalla (la cobertura
 * hace de alfa). Como el icono se rasteriza siempre ENTERO en su búfer y solo la bajada mira el recorte, lo que
 * sale no depende del recorte.
 *
 * Costes: posiciones y normales van aparte y sin repetir; la luz se calcula por NORMAL (una malla tiene 20–60) y
 * la proyección por POSICIÓN (50–150). No hay divisiones por vértice: la perspectiva es de primer orden
 * (1 + z/D), que para objetos de ±0,3 de fondo vistos a 4,5 radios se distingue mal de la exacta. */
#include "m3d.h"
#include "shell.h"
#include "r3d.h"

#include "mallas3d.h"

const char *const m3_estilo_nombre[M3_ESTILOS] = { "Rejilla", "Naranja", "Cristal", "Neón", "Medios", "Escritorio" };

/* ---- estilos ---------------------------------------------------------------------------------------------- */
typedef struct {
    u8 natural;                 /* 1 = cuerpo, secundario y acento son los de cada icono; 0 = los de pal[] */
    u8 pal[5][3];               /* RGB por papel: cuerpo, secundario, claro, oscuro, acento */
    u8 sombra_pct;              /* cuánto queda del color en la sombra más honda (%) */
    u8 tinte[3], tinte_pct;     /* hacia qué color tira la sombra, y cuánto (%) */
    u8 brillo[3];               /* el color del brillo especular (los últimos niveles de la rampa) */
    u8 amb, dif, esp;           /* luz: ambiente (0..255), difusa y especular (x/128) */
    u8 halo[3], halo_pct;       /* el resplandor de detrás del icono con foco */
    u8 suelo[3], suelo_pct;     /* la sombra que el icono deja debajo (0 = no hay) */
    M3Tinta tinta;
} Estilo;
#define C565(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
static const Estilo estilos[M3_ESTILOS] = {
    [M3_WALKMAN] = { .natural = 0,
        .pal = { { 255, 122, 0 }, { 255, 184, 70 }, { 255, 246, 232 }, { 52, 46, 44 }, { 255, 255, 255 } },
        .sombra_pct = 22, .tinte = { 60, 10, 0 }, .tinte_pct = 35, .brillo = { 255, 250, 224 },
        .amb = 70, .dif = 146, .esp = 100, .halo = { 255, 128, 0 }, .halo_pct = 84, .suelo_pct = 0,
        .tinta = { RGB(255, 140, 20), RGB(255, 255, 255), RGB(190, 150, 110), RGB(0, 0, 0), 1 } },
    [M3_CRISTAL] = { .natural = 1,
        .pal = { { 0, 0, 0 }, { 0, 0, 0 }, { 252, 253, 255 }, { 58, 68, 92 }, { 0, 0, 0 } },
        .sombra_pct = 46, .tinte = { 40, 60, 120 }, .tinte_pct = 30, .brillo = { 255, 255, 255 },
        .amb = 100, .dif = 118, .esp = 76, .halo = { 70, 150, 255 }, .halo_pct = 58,
        .suelo = { 30, 50, 100 }, .suelo_pct = 34,
        .tinta = { RGB(24, 60, 140), RGB(20, 36, 80), RGB(80, 100, 150), RGB(255, 255, 255), 0 } },
    [M3_NEON] = { .natural = 0,
        .pal = { { 0, 226, 255 }, { 255, 64, 208 }, { 232, 250, 255 }, { 54, 40, 120 }, { 255, 234, 64 } },
        .sombra_pct = 26, .tinte = { 110, 0, 170 }, .tinte_pct = 62, .brillo = { 255, 255, 255 },
        .amb = 64, .dif = 154, .esp = 110, .halo = { 255, 40, 200 }, .halo_pct = 76, .suelo_pct = 0,
        .tinta = { RGB(0, 230, 255), RGB(255, 255, 255), RGB(190, 120, 230), RGB(20, 0, 40), 1 } },
    [M3_MEDIOS] = { .natural = 0,
        .pal = { { 246, 240, 238 }, { 222, 206, 204 }, { 255, 255, 255 }, { 120, 44, 40 }, { 255, 255, 255 } },
        .sombra_pct = 52, .tinte = { 110, 10, 12 }, .tinte_pct = 45, .brillo = { 255, 255, 255 },
        .amb = 92, .dif = 150, .esp = 96, .halo = { 255, 236, 226 }, .halo_pct = 60, .suelo_pct = 0,
        .tinta = { RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 196, 190), RGB(70, 0, 0), 0 } },   /* (28-oct) MEDIOS: los iconos en BLANCO, como los del XMB; sombras teñidas del rojo del fondo */
};
/* ---- (28-oct) EL COLOR DE MEDIOS (Ajustes -> «  color de Medios», pedido del mantenedor: «que no sea siempre rojo»). La plantilla es el rojo
 * del XMB (estilos[M3_MEDIOS], fondos[M3_MEDIOS] y los tintes de shell/menu_medios.c); cada color sale GIRANDO EL TONO de cada color de la
 * plantilla, con su saturación y su valor: la misma luz y la misma hondura que el rojo. Plata y Negro le quitan la saturación. */
static int md_tono;                                            /* SH_MEDIOS_ROJO..SH_MEDIOS_NEGRO */
static const struct { i16 giro; u8 sat, val; } md_tonos[] = { /* giro del tono (1536 = la vuelta entera), saturación y valor (%) */
    { 0, 100, 100 },       /* rojo: la plantilla tal cual */
    { 102, 100, 100 },     /* naranja (+24°) */
    { 188, 95, 100 },      /* oro (+44°) */
    { 491, 90, 88 },       /* verde (+115°: el verde se ve más claro con el mismo valor) */
    { 747, 95, 88 },       /* turquesa (+175°) */
    { 930, 100, 100 },     /* azul (+218°) */
    { 1160, 90, 100 },     /* morado (+272°) */
    { 1387, 85, 108 },     /* rosa (+325°) */
    { 0, 6, 118 },         /* plata */
    { 0, 0, 42 },          /* negro */
};
#define MD_TONOS ((int)(sizeof md_tonos / sizeof md_tonos[0]))
void m3_medios_rgb(u8 *c)
{
    int r = c[0], g = c[1], b = c[2], mx, mn, d, h, s, v, k, f, p, q, t;
    if (!md_tono) return;
    mx = r > g ? (r > b ? r : b) : (g > b ? g : b); mn = r < g ? (r < b ? r : b) : (g < b ? g : b); d = mx - mn;
    v = mx; s = mx ? d * 255 / mx : 0;
    if (!d) h = 0;
    else if (mx == r) h = ((g - b) * 256 / d + 1536) % 1536;
    else if (mx == g) h = (b - r) * 256 / d + 512;
    else h = (r - g) * 256 / d + 1024;
    h = (h + md_tonos[md_tono].giro) % 1536;
    s = s * md_tonos[md_tono].sat / 100; if (s > 255) s = 255;
    v = v * md_tonos[md_tono].val / 100; if (v > 255) v = 255;
    k = h >> 8; f = h & 255;
    p = v * (255 - s) / 255; q = v * (255 - s * f / 256) / 255; t = v * (255 - s * (255 - f) / 256) / 255;
    switch (k) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    c[0] = (u8)r; c[1] = (u8)g; c[2] = (u8)b;
}
Color m3_medios_c(Color c)
{
    const u32 v = ((u32)c >> 8 | (u32)c << 8) & 0xFFFFu;
    u8 rgb[3];
    if (!md_tono) return c;
    rgb[0] = (u8)((v >> 11) << 3); rgb[1] = (u8)(((v >> 5) & 63) << 2); rgb[2] = (u8)((v & 31) << 3);
    m3_medios_rgb(rgb);
    return RGB(rgb[0], rgb[1], rgb[2]);
}
static Estilo est_md;                                          /* estilos[M3_MEDIOS] con el color de ahora (si no es el rojo) */
static u32 fondo_clave;
static u8 rampas_sucias;                                       /* el color cambió: las rampas de color de los iconos, otra vez */
static const Estilo *est(int estilo)
{
    if (estilo <= 0 || estilo >= M3_ESTILOS || estilo == M3_ESCRITORIO) estilo = M3_WALKMAN;   /* (6-nov) el escritorio no tiene estilo de malla */
    return estilo == M3_MEDIOS && md_tono ? &est_md : &estilos[estilo];
}
int m3_medios_color_actual(void) { return md_tono; }
void m3_medios_color(int t)
{
    int i;
    if (t < 0 || t >= MD_TONOS) t = 0;
    if (t == md_tono) return;
    md_tono = t;
    est_md = estilos[M3_MEDIOS];
    for (i = 0; i < 5; i++) m3_medios_rgb(est_md.pal[i]);
    m3_medios_rgb(est_md.tinte); m3_medios_rgb(est_md.brillo); m3_medios_rgb(est_md.halo); m3_medios_rgb(est_md.suelo);
    est_md.tinta.titulo = m3_medios_c(est_md.tinta.titulo); est_md.tinta.texto = m3_medios_c(est_md.tinta.texto);
    est_md.tinta.tenue = m3_medios_c(est_md.tinta.tenue); est_md.tinta.sombra = m3_medios_c(est_md.tinta.sombra);
    fondo_clave = 0;                                           /* el fondo, otra vez, con el color nuevo */
    rampas_sucias = 1;
}
const M3Tinta *m3_tinta(int estilo) { return &est(estilo)->tinta; }

int m3_malla_de_icono(int ic)
{
    if (ic >= IC_ARCHIVOS && ic <= IC_MUSICA) return ic;      /* las doce primeras van en el orden del enum */
    if (ic == IC_MOVIMIENTO) return 12;
    if (ic == IC_APPS) return 13;                             /* (EST3) la celda «Apps»: cuatro baldosas (antes, la carpeta de Archivos) */
    if (ic == IC_ENCHUFE) return 14;                          /* (EST3) un enchufe: Red y las .sen que lo pidan */
    return M3_MALLA_GENERICA;
}

/* Las mallas de las apps EXTERNAS (pista M2): no están en mallas3d.h, las trae cada app en sus datos
 * (AppExterna.malla). Aquí solo se guarda el puntero; su índice es M3_MALLAS + hueco. */
static const M3Malla *m3_ext[APP_MALLAS_MAX];
int m3_malla_externa(const M3Malla *m)
{
    int i;
    if (!m || !m->pos || !m->nor || !m->tri || !m->npos || !m->nnor) return -1;
    for (i = 0; i < APP_MALLAS_MAX; i++) {
        if (m3_ext[i] == m) return M3_MALLAS + i;
        if (!m3_ext[i]) { m3_ext[i] = m; return M3_MALLAS + i; }
    }
    return -1;
}
/* (ICO) las de las .sen: un hueco por app del menú, detrás de las externas. El contenido de un hueco cambia si cambian las
 * apps descubiertas: por eso poner una malla borra sus rampas (rampas_de). */
#define M3_TODAS (M3_MALLAS + APP_MALLAS_MAX + M3_SEN_MAX)
static const M3Malla *m3_sen[M3_SEN_MAX];
static u8 rampas_de[M3_TODAS];   /* estilo + 1 con el que se hicieron (0 = sin hacer) */
int m3_malla_sen(int hueco, const M3Malla *m)
{
    if (hueco < 0 || hueco >= M3_SEN_MAX) return -1;
    m3_sen[hueco] = m && m->pos && m->nor && m->tri && m->npos && m->nnor ? m : 0;
    rampas_de[M3_MALLAS + APP_MALLAS_MAX + hueco] = 0;
    return m3_sen[hueco] ? M3_MALLAS + APP_MALLAS_MAX + hueco : -1;
}
static const M3Malla *malla_de(int *i)                        /* la malla del índice; uno que no existe es la genérica */
{
    if (*i >= M3_MALLAS && *i < M3_MALLAS + APP_MALLAS_MAX && m3_ext[*i - M3_MALLAS]) return m3_ext[*i - M3_MALLAS];
    if (*i >= M3_MALLAS + APP_MALLAS_MAX && *i < M3_TODAS && m3_sen[*i - M3_MALLAS - APP_MALLAS_MAX]) return m3_sen[*i - M3_MALLAS - APP_MALLAS_MAX];
    if (*i < 0 || *i >= M3_MALLAS) *i = M3_MALLA_GENERICA;
    return &m3_mallas[*i];
}

/* ---- rampas: 32 colores por (estilo, malla, papel): sombra -> color (nivel 23) -> brillo (nivel 31).
 * Dos copias: NATIVA (para el búfer del icono, que luego se promedia) y en formato de PANEL (para pintar directo). */
#define NIVEL_COLOR 23
static u16 rampas[2][M3_TODAS][5][32];

static const u16 *rampa(int estilo, int malla, int papel, int panel)
{
    if (rampas_sucias) { memset(rampas_de, 0, sizeof rampas_de); rampas_sucias = 0; }   /* (28-oct) el color de Medios cambió */
    if (rampas_de[malla] != estilo + 1) {
        const Estilo *e = estilo == M3_MEDIOS && md_tono ? &est_md : &estilos[estilo]; const M3Malla *m = malla_de(&malla);
        int p, i, k;
        for (p = 0; p < 5; p++) {
            const u8 *base = e->pal[p]; int c[3], s[3];
            if (e->natural && p == 0) base = m->natural[0];
            if (e->natural && p == 1) base = m->natural[1];
            if (e->natural && p == 4) base = m->natural[2];
            for (k = 0; k < 3; k++) {
                c[k] = base[k];
                s[k] = (c[k] * e->sombra_pct * (100 - e->tinte_pct) + e->tinte[k] * e->tinte_pct * 100) / 10000;
            }
            for (i = 0; i < 32; i++) {
                int v[3]; u16 q;
                for (k = 0; k < 3; k++)
                    v[k] = i <= NIVEL_COLOR ? s[k] + (c[k] - s[k]) * i / NIVEL_COLOR
                                            : c[k] + (e->brillo[k] - c[k]) * (i - NIVEL_COLOR) * 7 / (8 * (31 - NIVEL_COLOR));
                q = C565(v[0], v[1], v[2]);
                rampas[0][malla][p][i] = q; rampas[1][malla][p][i] = (u16)((q >> 8) | (q << 8));
            }
        }
        rampas_de[malla] = (u8)(estilo + 1);
    }
    return rampas[panel ? 1 : 0][malla][papel];
}

/* ---- mezclas sobre la pantalla (formato de panel: RGB565 con el byte alto primero) --------------------------- */
static const u8 bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
#define BAYER(x, y) bayer[(((y) & 3) << 2) | ((x) & 3)]
static inline u32 mezcla565(u32 d, u32 c, u32 a)        /* nativos; a = 0..32 */
{
    u32 rb = ((d & 0xF81F) * (32 - a) + (c & 0xF81F) * a) >> 5, g = ((d & 0x07E0) * (32 - a) + (c & 0x07E0) * a) >> 5;
    return (rb & 0xF81F) | (g & 0x07E0);
}
static Rect corta(Rect a, Rect b)
{
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    Rect r = { (i16)x0, (i16)y0, (i16)(x1 > x0 ? x1 - x0 : 0), (i16)(y1 > y0 ? y1 - y0 : 0) };
    return r;
}

/* Una mancha elíptica difusa de un color: alfa = fuerza x (1 - d²)^perfil, tramada (el alfa tiene 33 niveles y
 * sin trama se verían los anillos). Solo depende de las coordenadas de pantalla, no del recorte. */
static void mancha(Lienzo *l, Rect rec, int cx, int cy, int rx, int ry, u32 color, int fuerza, int perfil)
{
    static u32 col[2 * 64 + 1];
    int x, y, x0 = cx - rx, x1 = cx + rx + 1, y0 = cy - ry, y1 = cy + ry + 1;
    u32 kx, ky;
    if (rx < 1 || ry < 1 || rx > 64 || fuerza <= 0) return;
    kx = (65536u << 8) / (u32)(rx * rx); ky = (65536u << 8) / (u32)(ry * ry);
    for (x = 0; x <= 2 * rx; x++) col[x] = ((u32)((x - rx) * (x - rx)) * kx) >> 8;
    if (x0 < rec.x) x0 = rec.x;
    if (y0 < rec.y) y0 = rec.y;
    if (x1 > rec.x + rec.w) x1 = rec.x + rec.w;
    if (y1 > rec.y + rec.h) y1 = rec.y + rec.h;
    for (y = y0; y < y1; y++) {
        const u32 qy = ((u32)((y - cy) * (y - cy)) * ky) >> 8;
        Color *p = l->cuadro + y * UI_ANCHO + x0;
        if (qy >= 65536) continue;
        for (x = x0; x < x1; x++, p++) {
            u32 q = qy + col[x - cx + rx], g, a;
            if (q >= 65536) continue;
            g = 65536 - q;
            if (perfil == 2) g = (g >> 8) * (g >> 8);
            a = ((g >> 8) * (u32)fuerza) >> 8;                              /* 0..256 */
            a = (a + (BAYER(x, y) >> 1)) >> 3;
            if (!a) continue;
            if (a > 32) a = 32;
            { u32 d = *p; d = mezcla565(((d >> 8) | (d << 8)) & 0xFFFF, color, a); *p = (Color)((d >> 8) | (d << 8)); }
        }
    }
}

/* ---- el icono ------------------------------------------------------------------------------------------------
 * Dos calidades (M3Icono.fino):
 *   FINO: se rasteriza al DOBLE en el búfer del icono y se baja 2x2 mezclando con lo que hay (bordes suavizados).
 *   BASTO: a resolución simple y DIRECTO a la pantalla, por la ventana del recorte. Es lo que se usa mientras la
 *          inclinación está cambiando: cuesta menos de la mitad. Se puede porque el triángulo de r3d da los mismos
 *          píxeles con cualquier ventana (su aritmética es exacta frente a traslaciones enteras del marco y al
 *          recorte por filas y columnas), así que un repintado parcial y uno completo siguen coincidiendo. */
#define SP_AN 160                       /* el búfer del icono: una celda de 80 x 64 al doble (estética, 26-oct: antes 80 x 56; con la letra */
#define SP_AL 128                       /* nueva el menú tiene 4 filas de 64 en el W580) */
static u16 sp_pix[SP_AN * SP_AL], sp_z[SP_AN * SP_AL];
static s32 vx[256], vy[256]; static u32 vz[256];
static s32 nluz[256], vg[256];
/* la luz, en el espacio de la pantalla (y arriba, z hacia quien mira), Q7: viene de arriba a la izquierda */
#define LUZ_X (-51)
#define LUZ_Y 70
#define LUZ_Z 93
#define MED_X (-27)                     /* el vector medio entre la luz y la mirada: donde salta el brillo */
#define MED_Y 38
#define MED_Z 118

/* El halo del foco como mapa de alfa (0..255). (pista O) El mapa va en coordenadas del CENTRO del halo (dx = -rx..rx,
 * dy = -ry..ry; fuera de ahí el alfa es 0 con la fórmula de siempre), así que solo se rehace si cambian los radios o la
 * fuerza: antes iba en coordenadas de la celda y se rehacía (4480 píxeles) cada vez que el icono se mecía un píxel.
 * Y el color del halo por cada alfa (33 niveles) se multiplica una vez, no en cada píxel. Da los mismos píxeles. */
static u8 halo_a[80 * 64];
static struct { i16 rx, ry, fuerza; u8 ok; } halo_k;
static void halo(Lienzo *l, Rect celda, Rect rec, int cx, int cy, int rx, int ry, u32 color, int fuerza)
{
    static u32 c_rb[33], c_g[33], c_de = 0xFFFFFFFFu;
    const int an = 2 * rx + 1;
    int x, y, x0, x1, y0, y1;
    if (celda.w > 80 || celda.h > 64 || rx < 1 || ry < 1 || rx > 64 || fuerza <= 0 || an * (2 * ry + 1) > (int)sizeof halo_a) return;
    if (!halo_k.ok || halo_k.rx != rx || halo_k.ry != ry || halo_k.fuerza != fuerza) {
        const u32 kx = (65536u << 8) / (u32)(rx * rx), ky = (65536u << 8) / (u32)(ry * ry);
        for (y = -ry; y <= ry; y++) {
            const u32 qy = ((u32)(y * y) * ky) >> 8;
            u8 *p = halo_a + (y + ry) * an;
            for (x = -rx; x <= rx; x++) {
                const u32 q = qy + (((u32)(x * x) * kx) >> 8);
                u32 g = q < 65536u ? 65536u - q : 0;
                g = (g >> 8) * (g >> 8);
                g = ((g >> 8) * (u32)fuerza) >> 8;
                p[x + rx] = (u8)(g > 255 ? 255 : g);
            }
        }
        halo_k.rx = (i16)rx; halo_k.ry = (i16)ry; halo_k.fuerza = (i16)fuerza; halo_k.ok = 1;
    }
    if (c_de != color) { u32 a; for (a = 0; a <= 32; a++) { c_rb[a] = (color & 0xF81F) * a; c_g[a] = (color & 0x07E0) * a; } c_de = color; }
    x0 = rec.x > cx - rx ? rec.x : cx - rx; x1 = rec.x + rec.w < cx + rx + 1 ? rec.x + rec.w : cx + rx + 1;
    y0 = rec.y > cy - ry ? rec.y : cy - ry; y1 = rec.y + rec.h < cy + ry + 1 ? rec.y + rec.h : cy + ry + 1;
    for (y = y0; y < y1; y++) {
        const u8 *a8 = halo_a + (y - cy + ry) * an + (x0 - cx + rx);
        Color *p = l->cuadro + y * UI_ANCHO + x0;
        for (x = x0; x < x1; x++, p++, a8++) {
            u32 a = *a8, d;
            if (!a) continue;
            a = (a + (BAYER(x, y) >> 1)) >> 3;                               /* 33 niveles, tramados */
            if (!a) continue;
            if (a > 32) a = 32;
            d = *p; d = ((d >> 8) | (d << 8)) & 0xFFFF;
            d = ((((d & 0xF81F) * (32 - a) + c_rb[a]) >> 5) & 0xF81F) | ((((d & 0x07E0) * (32 - a) + c_g[a]) >> 5) & 0x07E0);   /* = mezcla565(d, color, a) */
            *p = (Color)((d >> 8) | (d << 8));
        }
    }
}

/* (28-oct, Medios) pasos 2-5: el icono a la trama, con z. FINO: la celda entera al doble en el búfer del icono (sp_pix, sp_z);
 * BASTO: la ventana `rec` directamente en la pantalla de `l`. Devuelve 0 si no sale nada; si sale, deja en *pan el ancho del marco
 * y en caja[] la caja de lo pintado (minx, miny, maxx, maxy, en píxeles del marco). La usan m3_icono_pintar y m3_icono_sprite. */
static int icono_trama(Lienzo *l, Rect celda, Rect rec, const M3Icono *ic, int fino, int *pan, s32 caja[4])
{
    const Estilo *e = est(ic->estilo);
    const int estilo = ic->estilo > 0 && ic->estilo < M3_ESTILOS && ic->estilo != M3_ESCRITORIO ? ic->estilo : M3_WALKMAN;   /* (28-oct) no «e - estilos»: con el color de
                                                                                                * Medios, e es una copia fuera de la tabla */
    int malla = ic->malla;
    const M3Malla *m = malla_de(&malla);
    s32 r[3][3], f, pk, gk, zk, cxq, cyq, minx = 0x7FFFFFFF, maxx = -0x7FFFFFFF, miny = 0x7FFFFFFF, maxy = -0x7FFFFFFF;
    int i, p, an, al;
    const u8 *t;
    /* 2. el giro: R = Rx(cabeceo) · Ry(guiñada), Q14 */
    {
        const s32 cy_ = r3d_cos((u32)ic->guinada), sy_ = r3d_sen((u32)ic->guinada);
        const s32 cx_ = r3d_cos((u32)ic->cabeceo), sx_ = r3d_sen((u32)ic->cabeceo);
        r[0][0] = cy_;                 r[0][1] = 0;    r[0][2] = sy_;
        r[1][0] = (sx_ * sy_) >> 14;   r[1][1] = cx_;  r[1][2] = -((sx_ * cy_) >> 14);
        r[2][0] = -((cx_ * sy_) >> 14); r[2][1] = sx_; r[2][2] = (cx_ * cy_) >> 14;
    }
    /* El marco: FINO, la celda entera al doble, en el búfer; BASTO, la ventana del recorte, en la pantalla.
     * Las unidades son 1/16 de píxel DEL MARCO: en fino un píxel de pantalla son 32. */
    f = (s32)(((u32)ic->radio * 83886u) >> 8);             /* Q14 x unidades de malla -> 1/16 de píxel del marco, x 2^32 */
    pk = 1864135 / (s32)ic->radio;                         /* perspectiva: 4096 x 256 / D, con D = 4,5 radios */
    gk = ((22 * 65536 * 8) / (s32)ic->radio * ic->luz) >> 8;   /* ±22 de 255 de luz entre el borde de arriba y el de abajo */
    zk = 16;
    if (fino) {
        an = celda.w * 2; al = celda.h * 2;
        cxq = an * 8 + ic->dx * 2; cyq = al * 8 + ic->dy * 2;
    } else {
        an = rec.w; al = rec.h;
        f >>= 1; pk <<= 1; gk <<= 1; zk = 32;
        cxq = (celda.x - rec.x) * 16 + celda.w * 8 + ic->dx; cyq = (celda.y - rec.y) * 16 + celda.h * 8 + ic->dy;
    }

    /* 3. posiciones: girar, escalar, perspectiva de primer orden */
    for (i = 0; i < m->npos; i++) {
        const s32 x = m->pos[3 * i], y = m->pos[3 * i + 1], z = m->pos[3 * i + 2];
        const s32 X = (s32)(((s64)(r[0][0] * x + r[0][1] * y + r[0][2] * z) * f) >> 32);
        const s32 Y = (s32)(((s64)(r[1][0] * x + r[1][1] * y + r[1][2] * z) * f) >> 32);
        const s32 Z = (s32)(((s64)(r[2][0] * x + r[2][1] * y + r[2][2] * z) * f) >> 32);
        const s32 k = 4096 + ((Z * pk) >> 8);
        const s32 sx = cxq + ((X * k) >> 12), sy = cyq - ((Y * k) >> 12);
        vx[i] = sx; vy[i] = sy; vz[i] = (u32)(32768 - Z * zk) << 15;
        vg[i] = (Y * gk) >> 8;                             /* más luz arriba que abajo: las caras planas no salen lisas */
        if (sx < minx) minx = sx;
        if (sx > maxx) maxx = sx;
        if (sy < miny) miny = sy;
        if (sy > maxy) maxy = sy;
    }
    /* 4. la luz, por normal */
    for (i = 0; i < m->nnor; i++) {
        const s32 x = m->nor[3 * i], y = m->nor[3 * i + 1], z = m->nor[3 * i + 2];
        const s32 X = (r[0][0] * x + r[0][1] * y + r[0][2] * z) >> 14;
        const s32 Y = (r[1][0] * x + r[1][1] * y + r[1][2] * z) >> 14;
        const s32 Z = (r[2][0] * x + r[2][1] * y + r[2][2] * z) >> 14;
        s32 d = (X * LUZ_X + Y * LUZ_Y + Z * LUZ_Z) >> 7, h = (X * MED_X + Y * MED_Y + Z * MED_Z) >> 7, v;
        if (d < 0) d = 0;
        if (h < 0) h = 0;
        if (h > 127) h = 127;
        h = (h * h) >> 7; h = (h * h) >> 7; h = (h * h) >> 7; h = (h * h) >> 7; h = (h * h) >> 7;      /* ^32 */
        v = e->amb + ((d * e->dif) >> 7) + ((h * e->esp) >> 7);
        v = (v * ic->luz) >> 8;
        if (v > 255) v = 255;
        nluz[i] = v << 16;
    }

    /* 5. a la trama, con z. Solo se borra la z del rectángulo que ocupa el icono */
    minx = (minx >> 4) & ~1; miny = (miny >> 4) & ~1; maxx = ((maxx >> 4) + 3) & ~1; maxy = ((maxy >> 4) + 3) & ~1;   /* pares: la bajada va de 2 en 2 */
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > an) maxx = an;                              /* (R3; calidad) las dos ramas del «if (fino)» eran idénticas: an y al ya son los del marco que toque */
    if (maxy > al) maxy = al;
    if (minx >= maxx || miny >= maxy) return 0;
    r3d_memoria_z(sp_z, sizeof sp_z);
    if (fino) r3d_iniciar_marco(sp_pix, an, an, al);
    else r3d_iniciar_marco((u16 *)(l->cuadro + rec.y * UI_ANCHO + rec.x), UI_ANCHO, an, al);
    r3d_borrar_z_rect((int)minx, (int)miny, (int)maxx, (int)maxy);
    r3d_area_minima(fino ? 32 * 256 : 8 * 256);            /* por debajo de 4 píxeles de pantalla: profundidad y luz constantes */
    for (p = 0, t = m->tri; p < 5; p++) {
        R3dMaterial mat = { .modo = R3D_RAMPA, .z = R3D_Z_SI };
        int n = m->nt[p];
        if (!n) continue;
        mat.rampa = rampa(estilo, malla, p, !fino);
        r3d_material(&mat);
        for (; n--; t += 6) {
            const int a = t[0], b = t[1], c = t[2];
            R3dPV pa, pb, pc;
            if ((vx[b] - vx[a]) * (vy[c] - vy[a]) - (vx[c] - vx[a]) * (vy[b] - vy[a]) >= 0) continue;   /* de espaldas */
#define LUZ(n, v) ({ s32 l_ = nluz[n] + vg[v]; l_ < 0 ? 0 : l_ > (255 << 16) ? (255 << 16) : l_; })
            pa.x = vx[a]; pa.y = vy[a]; pa.z = vz[a]; pa.l = LUZ(t[3], a);
            pb.x = vx[b]; pb.y = vy[b]; pb.z = vz[b]; pb.l = LUZ(t[4], b);
            pc.x = vx[c]; pc.y = vy[c]; pc.z = vz[c]; pc.l = LUZ(t[5], c);
#undef LUZ
            pa.u = pa.v = pb.u = pb.v = pc.u = pc.v = 0; pa.w = pb.w = pc.w = 0;
            r3d_triangulo(&pa, &pb, &pc);
        }
    }
    *pan = an; caja[0] = minx; caja[1] = miny; caja[2] = maxx; caja[3] = maxy;
    return 1;
}

void m3_icono_pintar(Lienzo *l, Rect celda, const M3Icono *ic)
{
    const Estilo *e = est(ic->estilo);
    const int fino = ic->fino != 0;
    Rect rec = corta(l->recorte, celda);
    s32 caja[4], minx, miny, maxx, maxy;
    int an;
    if (rec.w <= 0 || rec.h <= 0 || ic->radio < 256 || celda.w * 2 > SP_AN || celda.h * 2 > SP_AL) return;
    /* 1. lo que va DEBAJO del icono: la sombra en el «suelo» y el halo del foco */
    {
        const int cx = celda.x + celda.w / 2 + ic->dx / 16, cy = celda.y + celda.h / 2 + ic->dy / 16, rp = ic->radio >> 8;
        if (e->suelo_pct)
            mancha(l, rec, cx + 2 - ic->dx / 8, cy + rp + 1, rp, rp / 4 + 1, C565(e->suelo[0], e->suelo[1], e->suelo[2]),
                   e->suelo_pct * 256 / 100, 1);
        if (ic->foco)
            halo(l, celda, rec, cx, cy, 34, 28, C565(e->halo[0], e->halo[1], e->halo[2]), e->halo_pct * ic->foco / 100);
    }

    if (!icono_trama(l, celda, rec, ic, fino, &an, caja)) return;
    minx = caja[0]; miny = caja[1]; maxx = caja[2]; maxy = caja[3];
    if (!fino) return;                                     /* basto: ya está en la pantalla */

    /* 6. la bajada 2x2 a la pantalla, dentro del recorte. Sin cubrir (z = 0xFFFF en las cuatro): no se toca;
     * cubierto entero: la media; a medias: lo cubierto más lo que había, a partes iguales. */
    {
        int X, Y, x0 = celda.x + (int)(minx >> 1), x1 = celda.x + (int)(maxx >> 1);
        int y0 = celda.y + (int)(miny >> 1), y1 = celda.y + (int)(maxy >> 1);
        if (x0 < rec.x) x0 = rec.x;
        if (y0 < rec.y) y0 = rec.y;
        if (x1 > rec.x + rec.w) x1 = rec.x + rec.w;
        if (y1 > rec.y + rec.h) y1 = rec.y + rec.h;
        for (Y = y0; Y < y1; Y++) {
            const int sy = 2 * (Y - celda.y), sx = 2 * (x0 - celda.x);
            const u16 *za = sp_z + sy * an + sx, *zb = za + an, *ca = sp_pix + sy * an + sx, *cb = ca + an;
            Color *d = l->cuadro + Y * UI_ANCHO + x0;
            for (X = x0; X < x1; X++, za += 2, zb += 2, ca += 2, cb += 2, d++) {
                const u32 z0 = za[0], z1 = za[1], z2 = zb[0], z3 = zb[1];
                u32 c0, c1, c2, c3, o;
                if ((z0 & z1 & z2 & z3) == 0xFFFF) continue;
                c0 = ca[0]; c1 = ca[1]; c2 = cb[0]; c3 = cb[1];
                if (z0 != 0xFFFF && z1 != 0xFFFF && z2 != 0xFFFF && z3 != 0xFFFF) {
                    if (c0 == c1 && c2 == c3 && c0 == c2) o = c0;
                    else o = ((((c0 & 0xF81F) + (c1 & 0xF81F) + (c2 & 0xF81F) + (c3 & 0xF81F)) >> 2) & 0xF81F) |
                             ((((c0 & 0x07E0) + (c1 & 0x07E0) + (c2 & 0x07E0) + (c3 & 0x07E0)) >> 2) & 0x07E0);
                } else {
                    u32 f0 = *d, rb, g;
                    f0 = ((f0 >> 8) | (f0 << 8)) & 0xFFFF;
                    if (z0 == 0xFFFF) c0 = f0;
                    if (z1 == 0xFFFF) c1 = f0;
                    if (z2 == 0xFFFF) c2 = f0;
                    if (z3 == 0xFFFF) c3 = f0;
                    rb = (((c0 & 0xF81F) + (c1 & 0xF81F) + (c2 & 0xF81F) + (c3 & 0xF81F)) >> 2) & 0xF81F;
                    g = (((c0 & 0x07E0) + (c1 & 0x07E0) + (c2 & 0x07E0) + (c3 & 0x07E0)) >> 2) & 0x07E0;
                    o = rb | g;
                }
                *d = (Color)((o >> 8) | (o << 8));
            }
        }
    }
}

/* ---- (28-oct) MEDIOS: el icono a un sprite, y el sprite a la pantalla ---------------------------------------------------------- */
int m3_icono_sprite(const M3Icono *ic, int w, int h, u16 *pix, u8 *cob)
{
    Rect celda = { 0, 0, (i16)w, (i16)h };
    s32 caja[4];
    int an, X, Y, x0, x1, y0, y1;
    memset(cob, 0, (size_t)(w * h));
    if (ic->radio < 256 || w * 2 > SP_AN || h * 2 > SP_AL || w <= 0 || h <= 0) return 0;
    if (!icono_trama(NULL, celda, celda, ic, 1, &an, caja)) return 0;
    x0 = (int)(caja[0] >> 1); x1 = (int)(caja[2] >> 1); y0 = (int)(caja[1] >> 1); y1 = (int)(caja[3] >> 1);
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (Y = y0; Y < y1; Y++) {
        const u16 *za = sp_z + 2 * Y * an + 2 * x0, *zb = za + an, *ca = sp_pix + 2 * Y * an + 2 * x0, *cb = ca + an;
        for (X = x0; X < x1; X++, za += 2, zb += 2, ca += 2, cb += 2) {
            const u16 z[4] = { za[0], za[1], zb[0], zb[1] }, c[4] = { ca[0], ca[1], cb[0], cb[1] };
            u32 k = 0, r = 0, g = 0, b = 0, i;
            for (i = 0; i < 4; i++) if (z[i] != 0xFFFF) { k++; r += c[i] >> 11; g += (c[i] >> 5) & 63; b += c[i] & 31; }
            if (!k) continue;
            pix[Y * w + X] = (u16)(((r / k) << 11) | ((g / k) << 5) | (b / k));
            cob[Y * w + X] = (u8)k;
        }
    }
    return 1;
}
void m3_sprite_pintar(Lienzo *l, int x, int y, int w, int h, const u16 *pix, const u8 *cob, int alfa)
{
    Rect c = corta(l->recorte, (Rect){ (i16)x, (i16)y, (i16)w, (i16)h });
    int X, Y;
    if (c.w <= 0 || c.h <= 0 || alfa <= 0) return;
    if (alfa > 256) alfa = 256;
    for (Y = c.y; Y < c.y + c.h; Y++) {
        const u8 *k = cob + (Y - y) * w + (c.x - x);
        const u16 *s = pix + (Y - y) * w + (c.x - x);
        Color *p = l->cuadro + Y * UI_ANCHO + c.x;
        for (X = 0; X < c.w; X++, k++, s++, p++) {
            u32 a, d;
            if (!*k) continue;
            a = ((u32)*k * 8u * (u32)alfa) >> 8;                       /* 0..32 */
            if (a >= 32) { *p = (Color)((*s >> 8) | (*s << 8)); continue; }
            if (!a) continue;
            d = *p; d = mezcla565(((d >> 8) | (d << 8)) & 0xFFFF, *s, a); *p = (Color)((d >> 8) | (d << 8));
        }
    }
}
void m3_resplandor(Lienzo *l, int cx, int cy, int rx, int ry, u32 color565, int fuerza) { mancha(l, l->recorte, cx, cy, rx, ry, color565, fuerza, 2); }
/* el halo del foco con su mapa de alfa GUARDADO (el de m3_icono_pintar): con los mismos radios y fuerza no se recalcula [medido] */
void m3_halo(Lienzo *l, int cx, int cy, int rx, int ry, u32 color565, int fuerza)
{
    const Rect celda = { (i16)(cx - 40), (i16)(cy - 32), 80, 64 };
    halo(l, celda, l->recorte, cx, cy, rx, ry, color565, fuerza);
}

/* ---- el fondo ------------------------------------------------------------------------------------------------
 * Una imagen por estilo, de (240 + 2·M3_MARGEN) x (320 + 2·M3_MARGEN), en formato de panel y TRAMADA (un
 * degradado suave en RGB565 sin trama hace escalones). Se hace una vez (al abrir el menú por primera vez o al
 * cambiar de estilo) y pintarla es copiar filas con un desplazamiento: eso es lo que se mueve al revés que los
 * iconos. */
#define F_AN (UI_ANCHO + 2 * M3_MARGEN)      /* (giro) el tamaño lógico de ahora; la reserva, la de las dos orientaciones (es la misma) */
#define F_AL (UI_ALTO + 2 * M3_MARGEN)
static Color fondo[(PLACA_ANCHO + 2 * M3_MARGEN) * (PLACA_ALTO + 2 * M3_MARGEN)];

typedef struct { i16 cx, cy, rx, ry; u8 c[3], fuerza, perfil; } Luz;      /* coordenadas de PANTALLA */
typedef struct { u8 arriba[3], medio[3], abajo[3]; i16 y_medio; const Luz *luces; int n; } Fondo;
static const Luz luces_walkman[] = {
    { 120, 150, 210, 230, { 255, 96, 0 }, 92, 2 },        /* el resplandor de en medio */
    { 30, 30, 120, 100, { 255, 150, 40 }, 60, 2 },
    { 220, 300, 130, 110, { 255, 70, 0 }, 84, 2 },
    { 196, 64, 30, 30, { 255, 170, 80 }, 40, 1 },         /* luces difusas, desenfocadas */
    { 52, 236, 38, 38, { 255, 140, 40 }, 34, 1 },
    { 150, 196, 20, 20, { 255, 190, 110 }, 34, 1 },
    { 92, 102, 14, 14, { 255, 200, 130 }, 30, 1 },
    { 224, 170, 24, 24, { 255, 120, 30 }, 30, 1 },
};
static const Luz luces_cristal[] = {
    { 50, 40, 190, 170, { 255, 255, 255 }, 170, 2 },
    { 210, 300, 170, 150, { 176, 226, 255 }, 140, 2 },
    { 230, 70, 100, 100, { 255, 226, 246 }, 110, 2 },
    { 20, 250, 90, 90, { 214, 255, 240 }, 100, 2 },
};
static const Luz luces_neon[] = {
    { 120, 214, 230, 54, { 255, 60, 200 }, 120, 2 },      /* el resplandor del horizonte */
    { 120, 214, 120, 20, { 255, 190, 240 }, 120, 2 },
    { 200, 60, 110, 90, { 40, 90, 255 }, 60, 2 },
};
static const Luz luces_medios[] = {                     /* (28-oct) MEDIOS: el rojo del XMB, con una claridad arriba a la derecha y */
    { 200, 40, 230, 170, { 255, 120, 110 }, 84, 2 },      /* la parte de abajo más honda, como en la PS3 */
    { 30, 300, 190, 150, { 90, 0, 8 }, 110, 2 },
    { 120, 196, 260, 60, { 230, 50, 50 }, 70, 2 },        /* donde va la ola: un poco más claro */
};
static const Fondo fondos[M3_ESTILOS] = {
    [M3_WALKMAN] = { { 5, 4, 4 }, { 16, 8, 3 }, { 34, 14, 2 }, 160, luces_walkman, 8 },
    [M3_CRISTAL] = { { 226, 240, 255 }, { 178, 212, 250 }, { 128, 176, 238 }, 170, luces_cristal, 4 },
    [M3_NEON]    = { { 5, 5, 26 }, { 40, 8, 78 }, { 22, 6, 46 }, 214, luces_neon, 3 },
    [M3_MEDIOS]  = { { 150, 16, 22 }, { 196, 30, 34 }, { 112, 6, 12 }, 190, luces_medios, 3 },
};

int m3_fondo_preparar(int estilo, int celdas, Rect rej, int alto_celda)
{
    static u32 kxs[8], kys[8], colq[8][UI_LADO_MAX + 2 * M3_MARGEN];
    const Fondo *fo; int x, y, i, k;
    u32 clave;
    if (estilo <= 0 || estilo >= M3_ESTILOS || estilo == M3_ESCRITORIO) estilo = M3_WALKMAN;
    clave = 0x80000000u | (u32)estilo | ((u32)celdas << 4) | ((u32)(rej.y & 0x1FF) << 9) | ((u32)(alto_celda & 0xFF) << 18) | ((u32)(UI_ANCHO > UI_ALTO) << 26) | ((u32)(rej.x != 0) << 27);   /* (giro) de lado, otro fondo */
    if (clave == fondo_clave) return 0;
    fondo_clave = clave;
    fo = &fondos[estilo];
    if (estilo == M3_MEDIOS && md_tono) {                       /* (28-oct) el color de Medios: la plantilla roja, con su tono girado */
        static Fondo fo_md; static Luz luces_md[8];
        fo_md = *fo;
        m3_medios_rgb(fo_md.arriba); m3_medios_rgb(fo_md.medio); m3_medios_rgb(fo_md.abajo);
        for (i = 0; i < fo->n && i < 8; i++) { luces_md[i] = fo->luces[i]; m3_medios_rgb(luces_md[i].c); }
        fo_md.luces = luces_md;
        fo = &fo_md;
    }
    for (i = 0; i < fo->n; i++) { kxs[i] = (65536u << 8) / (u32)(fo->luces[i].rx * fo->luces[i].rx); kys[i] = (65536u << 8) / (u32)(fo->luces[i].ry * fo->luces[i].ry); }
    for (i = 0; i < fo->n; i++) {                             /* (pista O) la parte de d² que pone la COLUMNA, por luz: una vez por imagen, no por píxel */
        const Luz *z = &fo->luces[i];
        for (x = 0; x < F_AN; x++) { const int dx = x - M3_MARGEN - z->cx; colq[i][x] = (dx <= -z->rx || dx >= z->rx) ? 65536u : ((u32)(dx * dx) * kxs[i]) >> 8; }
    }
    /* (pista O) El mismo cálculo, con la misma aritmética y en el mismo orden (da la misma imagen bit a bit), pero con
     * lo de cada luz que es constante en la FILA puesto en una tabla pequeña (color x 256, la parte de d² de la fila,
     * fuerza, perfil) y los tres canales en locales: antes cada píxel releía la luz por puntero y guardaba c[] en la pila. */
    for (y = 0; y < F_AL; y++) {
        struct { const u32 *col; u32 qy, fuerza; int c0, c1, c2, perfil2; } lf[8], *f;
        const int v = y - M3_MARGEN;
        Color *p = fondo + y * F_AN;
        int base[3], nv = 0, j;
        for (k = 0; k < 3; k++) {
            if (v <= fo->y_medio) { int t = v < 0 ? 0 : v; base[k] = fo->arriba[k] + (fo->medio[k] - fo->arriba[k]) * t / fo->y_medio; }
            else { int t = v - fo->y_medio, n = UI_ALTO - fo->y_medio; if (t > n) t = n; base[k] = fo->medio[k] + (fo->abajo[k] - fo->medio[k]) * t / n; }
        }
        for (i = 0; i < fo->n; i++) {                         /* las luces que tocan esta fila (las pequeñas, casi ninguna) */
            const Luz *z = &fo->luces[i]; const int dy = v - z->cy;
            const u32 qy = (dy > -z->ry && dy < z->ry) ? ((u32)(dy * dy) * kys[i]) >> 8 : 65536u;
            if (qy < 65536u) {
                f = &lf[nv++]; f->col = colq[i]; f->qy = qy; f->fuerza = z->fuerza;
                f->c0 = z->c[0] * 256; f->c1 = z->c[1] * 256; f->c2 = z->c[2] * 256; f->perfil2 = z->perfil == 2;
            }
        }
#define VELA(T0, T1, T2, a) { c0 += (((T0) * 256 - c0) * (a)) >> 8; c1 += (((T1) * 256 - c1) * (a)) >> 8; c2 += (((T2) * 256 - c2) * (a)) >> 8; }
        for (x = 0; x < F_AN; x++) {
            const int u = x - M3_MARGEN, b = BAYER(x, y);
            int c0 = base[0] * 256, c1 = base[1] * 256, c2 = base[2] * 256;
            u32 o;
            for (j = 0, f = lf; j < nv; j++, f++) {
                u32 g; const u32 q = f->qy + f->col[x];
                if (q >= 65536u) continue;
                g = 65536u - q;
                if (f->perfil2) g = (g >> 8) * (g >> 8);
                g = ((g >> 8) * f->fuerza) >> 8;              /* 0..255 */
                if (!g) continue;                             /* (con g = 0 las tres sumas son + 0) */
                c0 += ((f->c0 - c0) * (int)g) >> 8; c1 += ((f->c1 - c1) * (int)g) >> 8; c2 += ((f->c2 - c2) * (int)g) >> 8;
            }
            if (estilo == M3_NEON) {
                /* el suelo: una rejilla en perspectiva por debajo del horizonte; arriba, unas pocas estrellas */
                const int h = 214;
                if (v > h + 1) {
                    const int d = v - h, lx = ((u - 120) * 96) / d + 4096, ly = 9000 / d;
                    const int fx = lx & 31;
                    int a = 0;
                    if (fx < 2) a = 90; else if (fx == 2 || fx == 31) a = 34;
                    if ((9000 / (d + 1)) / 16 != ly / 16) a = a > 110 ? a : 110;
                    if (d < 14) a = a * d / 14;
                    VELA(255, 70, 220, a)
                } else if (v < 150) {
                    u32 hsh = (u32)(x * 7919 + y * 104729) * 2654435761u;
                    if ((hsh >> 22) == 5) { int a = 80 + (int)((hsh >> 12) & 127); VELA(235, 235, 235, a) }
                }
            }
            if (estilo == M3_CRISTAL) {
                /* una loseta de vidrio debajo de cada celda con app: aclara, con el filo de arriba más claro */
                /* (W600, el mantenedor: «los fondos de los ítems quedaron más grandes») la columna es un TERCIO de la pantalla: 80 px en el
                 * W580 como siempre, 58 en el W600 (antes 80 fijo: las losetas se montaban y la 3.ª se salía) */
                const int cw = rej.w / M3_COLS, uu = u - rej.x;   /* (giro) la rejilla puede no empezar en 0 ni ocupar todo el ancho */
                const int fy = v - rej.y, fila = alto_celda > 0 && fy >= 0 ? fy / alto_celda : -1, col = uu >= 0 && uu < cw * M3_COLS ? uu / cw : -1;
                if (fila >= 0 && col >= 0 && fila * M3_COLS + col < celdas) {
                    const int ix = uu - col * cw, iy = fy - fila * alto_celda, m = 5, an = cw - 2 * m, al = alto_celda - 2 * m;
                    const int jx = ix - m, jy = iy - m;
                    if (jx >= 0 && jy >= 0 && jx < an && jy < al) {
                        const int ex = jx < an - 1 - jx ? jx : an - 1 - jx, ey = jy < al - 1 - jy ? jy : al - 1 - jy;
                        int a = 96, rr = 7;
                        if (ex < rr && ey < rr) {                                  /* la esquina, redondeada */
                            const int ddx = rr - ex, ddy = rr - ey, d2 = ddx * ddx + ddy * ddy;
                            if (d2 > rr * rr + rr) a = 0; else if (d2 > (rr - 1) * (rr - 1)) a = 150;
                        } else if (jy == 0 || jx == 0) a = 190;
                        else if (jy == al - 1 || jx == an - 1) a = 40;
                        VELA(255, 255, 255, a)
                    }
                }
            }
            if (c0 < 0) c0 = 0;
            if (c0 > 255 * 256) c0 = 255 * 256;
            if (c1 < 0) c1 = 0;
            if (c1 > 255 * 256) c1 = 255 * 256;
            if (c2 < 0) c2 = 0;
            if (c2 > 255 * 256) c2 = 255 * 256;
            {   /* a 5-6-5 con trama ordenada de 4x4 (sin dividir: c x 257/256 lleva 255 a 65535) */
                const u32 r8 = (u32)c0 + ((u32)c0 >> 8), g8 = (u32)c1 + ((u32)c1 >> 8), b8 = (u32)c2 + ((u32)c2 >> 8);
                const u32 rq = (r8 * 31 + (u32)b * 4096 + 31) >> 16, gq = (g8 * 63 + (u32)b * 4096 + 63) >> 16,
                          bq = (b8 * 31 + (u32)(15 - b) * 4096 + 31) >> 16;
                o = ((rq > 31 ? 31 : rq) << 11) | ((gq > 63 ? 63 : gq) << 5) | (bq > 31 ? 31 : bq);
            }
            p[x] = (Color)((o >> 8) | (o << 8));
        }
#undef VELA
    }
    return 1;
}

void m3_fondo_pintar(Lienzo *l, Rect r, int ox, int oy)
{
    Rect c = corta(l->recorte, r);
    int y;
    if (c.w <= 0 || c.h <= 0) return;
    if (ox < -M3_MARGEN) ox = -M3_MARGEN;
    if (ox > M3_MARGEN) ox = M3_MARGEN;
    if (oy < -M3_MARGEN) oy = -M3_MARGEN;
    if (oy > M3_MARGEN) oy = M3_MARGEN;
    for (y = c.y; y < c.y + c.h; y++) {
        const int fy = y - r.y + M3_MARGEN + oy, fx = c.x - r.x + M3_MARGEN + ox;
        if (fy < 0 || fy >= F_AL || fx < 0 || fx + c.w > F_AN) continue;
        memcpy(l->cuadro + y * UI_ANCHO + c.x, fondo + fy * F_AN + fx, (size_t)c.w * sizeof(Color));
    }
}
