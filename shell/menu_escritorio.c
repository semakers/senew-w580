/* menu_escritorio.c — EL ESCRITORIO: un estilo más del menú principal (Ajustes -> «Menú: estilo» -> «Escritorio»), a la manera de un
 * escritorio de ordenador clásico, con su ratón (la idea viene de seCursor, un elf de la comunidad que dibujaba un ratón en el escritorio).
 *
 * LO QUE SE VE (aj.menu_estilo == M3_ESCRITORIO). Siempre DE PIE (shell/giro.c no lo gira):
 *   - el paisaje (colina y cielo con nubes), los ACCESOS DIRECTOS (Doom, Música, Videos) en columnas desde arriba a la izquierda (Doom se
 *     busca en el menú, sh_apps, y en la TARJETA, cg_juegos_descubrir, por el nombre del fichero o de la app), la BARRA DE TAREAS con el
 *     botón Inicio y la bandeja (la batería y la hora: sh_reloj_barra), y el menú Inicio;
 *   - en el árbol de desarrollo hay además unos recursos pre-renderizados (shell/escritorio_rec.c); en este repositorio público ese fichero
 *     va VACÍO (rec() == 0) y todo se dibuja aquí con código y con la letra del sistema.
 * ES LA PANTALLA DE INICIO: con este estilo, el standby (shell/shell.c) muestra el escritorio en vez de la hora y «Menú».
 * LAS TECLAS: la CRUCETA elige (el acceso elegido, teñido de azul; bajando del último de una columna, el botón Inicio; arriba, de vuelta) y
 * el RATÓN se desliza hasta lo elegido; OK (o C) lo abre; Sel. izq. abre el menú Inicio. En el MENÚ INICIO (cabecera con el usuario, la
 * línea naranja, a la izquierda TODAS las apps del menú y los juegos de la tarjeta, que se desplaza, y «Todos los programas»; a la derecha
 * Archivos, Ajustes y Sistema; el pie con «Apagar», que pregunta): la cruceta elige, OK o C abren, Atrás lo cierra. Los atajos 1..9 0 #
 * siguen abriendo la celda i del menú, como en los demás estilos; «*» no (con Sel. izq. bloquea el teclado).
 * CÓMO SE PINTA: cada pieza es su propio objeto de render (el fondo, la raíz, coloca a sus hijos: las nubes, los accesos, la barra, el
 * menú Inicio y el ratón): mover el ratón repinta solo sus dos rectángulos. Todo lo que decide los píxeles va en la configuración de cada
 * pieza: un repintado parcial y uno completo dan lo mismo (lo comprueba el PC tras cada cuadro).
 * El temporizador (33 ms) corre con el escritorio arriba y la pantalla encendida mientras haya algo que animar (las nubes, el ratón). */
#include "shell.h"
#include "m3d.h"
#include "cargador.h"
#include "escritorio_rec.h"                                      /* los recursos hechos con Blender (herr/escritorio/hacer_recursos.py) */

extern const Clase ClsMenu;                                     /* shell/menu3d.c: el menú del que este es un estilo */

/* ---- medidas (de la pantalla de ahora; el escritorio va de pie) ------------------------------------------------------------------- */
#define ES_MAX 320                                               /* el lado mayor de la pantalla */
#define BARRA_AL 30
#define BARRA_Y (UI_ALTO - BARRA_AL)
#define INICIO_AN 86                                             /* el botón Inicio */
#define BANDEJA_AN 74
#define CEL_AN 76                                                /* un acceso: icono de 32 y su nombre en dos líneas */
#define CEL_AL cel_al()                                    /* el icono y dos líneas de su nombre: se mide con la letra */
#define ICO 32
#define CUR_AN 12
#define CUR_AL 19
/* el menú Inicio */
#define PAN_AN (UI_ANCHO - 12)
#define PAN_Y 18
#define PAN_AL (BARRA_Y - PAN_Y)
#define CAB_AL 40
#define PIE_AL 30
#define IZQ_AN (PAN_AN * 55 / 100)
#define FILA_AL 24
#define SITIO_AL 28
#define CUERPO_Y (CAB_AL + 2)
#define CUERPO_AL (PAN_AL - CUERPO_Y - PIE_AL)
#define VIS ((CUERPO_AL - 10) / FILA_AL - 1)                     /* filas que se desplazan (la última es «Todos los programas») */
#define MINI 20                                                  /* los iconos del menú Inicio */

/* ---- los colores (azul y verde clásicos) ------------------------------------------------------------------------------------------- */
#define C_BLANCO RGB(255, 255, 255)
#define C_SOMBRA RGB(10, 20, 40)
#define C_SEL RGB(49, 106, 197)                                  /* lo elegido */
#define C_AZUL_OSC RGB(0, 19, 107)
static TemaDatos tema_xp;

/* ---- los accesos directos ----------------------------------------------------------------------------------------------------------- */
enum { D_DOOM, D_MUSICA, D_VIDEO };
/* col, fila: dónde va en el escritorio (columnas de CEL_AN, filas de CEL_AL; caben 3 columnas y 3 filas) */
typedef struct { const char *nombre; u8 dibujo, juego, col, fila, er; const char *claves[3]; const char *id; const char *ruta[2]; } Acceso;   /* er: ER_* */
static const Acceso accesos[] = {
    { "Doom",            D_DOOM,   1, 0, 0, ER_DOOM,   { "doom", NULL, NULL },            NULL, { NULL, NULL } },
    { "Música",          D_MUSICA, 0, 0, 1, ER_MUSICA, { NULL, NULL, NULL },              SH_APP_MUSICA, { NULL, NULL } },
    { "Videos",          D_VIDEO,  0, 0, 2, ER_VIDEO,  { "videos.sen", NULL, NULL },      NULL, { "/tarjeta/apps/videos.sen", "/memoria/apps/videos.sen" } },
};
#define N_ACC ((int)(sizeof accesos / sizeof accesos[0]))
#define ES_JUEGOS 12
#define ES_ITEMS 28

typedef struct { const char *nombre; i16 app; signed char juego, dibujo, er; i16 malla; } EsItem;   /* malla -1: sin malla (el dibujo); er: ER_* o -1 */
static struct {
    u32 gen;                                                    /* sh_apps_gen con que se armó (0 = nunca) */
    u16 version;
    i16 acc_app[N_ACC];                                         /* qué abre cada acceso: celda de sh_apps, */
    signed char acc_juego[N_ACC];                               /* juego de la tarjeta, */
    i16 acc_malla[N_ACC];                                       /* y su malla 3D (-1 = la dibujada) */
    char jnom[ES_JUEGOS][30], jruta[ES_JUEGOS][96];
    i16 jmalla[ES_JUEGOS];
    u8 jn;
    EsItem it[ES_ITEMS]; u8 n_it; i16 todos;                    /* la columna izquierda del menú Inicio y la celda de «Todos los programas» */
    EsItem sitio[3]; u8 n_sitio;                                /* la derecha */
} es;

static int minus(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int contiene(const char *s, const char *k)               /* k dentro de s, sin mirar mayúsculas (ASCII) */
{
    int i, j;
    if (!s || !k) return 0;
    for (i = 0; s[i]; i++) {
        for (j = 0; k[j] && s[i + j] && minus((unsigned char)s[i + j]) == minus((unsigned char)k[j]); j++) {}
        if (!k[j]) return 1;
    }
    return 0;
}
static const char *base(const char *r) { const char *b = r; for (; r && *r; r++) if (*r == '/') b = r + 1; return b; }
static int es_de(const Acceso *a, const char *nombre, const char *ruta)
{
    int k;
    for (k = 0; k < 3; k++) if (a->claves[k] && (contiene(nombre, a->claves[k]) || contiene(base(ruta), a->claves[k]))) return 1;
    return 0;
}
static int malla_app(int i) { return sh_apps[i].malla ? sh_apps[i].malla - 1 : m3_malla_de_icono(sh_apps[i].icono); }
static int malla_mando(void) { int m = cg_juegos_malla(); return m ? m - 1 : m3_malla_de_icono(IC_JUEGO); }
static int acceso_de(const char *nombre, const char *ruta)       /* el acceso (juego) que es esa app, o -1 */
{
    int i;
    for (i = 0; i < N_ACC; i++) if (accesos[i].juego && es_de(&accesos[i], nombre, ruta)) return i;
    return -1;
}
static void poner(EsItem *v, u8 *n, int max, const char *nombre, int app, int juego, int acceso, int malla)
{
    if (*n >= max) return;
    v[(*n)++] = (EsItem){ nombre, (i16)app, (signed char)juego, (signed char)(acceso >= 0 ? accesos[acceso].dibujo : -1),
                          (signed char)(acceso >= 0 ? accesos[acceso].er : -1), (i16)malla };
}
static int acceso_app(int i) { int k; for (k = 0; k < N_ACC; k++) if (es.acc_app[k] == i) return k; return -1; }
/* Rehace qué abre cada acceso y la lista del menú Inicio, desde el menú de ahora. Los juegos de la tarjeta se descubren UNA vez por armado
 * del menú (sh_apps_gen), como en el de Medios: sus mallas cogen huecos del cargador hasta el próximo sh_apps_init. */
static void armar(void)
{
    int i, k, n, mallas[ES_JUEGOS];
    for (k = 0; k < N_ACC; k++) { es.acc_app[k] = -1; es.acc_juego[k] = -1; es.acc_malla[k] = -1; }
    n = cg_juegos_descubrir(mallas, ES_JUEGOS);
    es.jn = 0;
    for (i = 0; i < n && es.jn < ES_JUEGOS; i++) {
        CgInfo in;
        if (cg_app_info(i, &in)) continue;
        memcpy(es.jnom[es.jn], in.nombre, sizeof es.jnom[0]); es.jnom[es.jn][sizeof es.jnom[0] - 1] = 0;
        memcpy(es.jruta[es.jn], in.ruta, sizeof es.jruta[0]); es.jruta[es.jn][sizeof es.jruta[0] - 1] = 0;
        es.jmalla[es.jn] = (i16)(mallas[i] ? mallas[i] - 1 : -1);
        es.jn++;
    }
    for (k = 0; k < N_ACC; k++) {
        const Acceso *a = &accesos[k];
        for (i = 0; i < sh_n_apps && es.acc_app[k] < 0; i++)
            if ((a->id && sh_apps[i].id && !strcmp(sh_apps[i].id, a->id)) || es_de(a, sh_apps[i].nombre, sh_apps[i].id)) {
                es.acc_app[k] = (i16)i;
                if (a->juego && sh_apps[i].malla) es.acc_malla[k] = (i16)(sh_apps[i].malla - 1);   /* un juego del menú con malla propia */
            }
        if (es.acc_app[k] < 0 && a->juego)
            for (i = 0; i < es.jn; i++) if (es_de(a, es.jnom[i], es.jruta[i])) { es.acc_juego[k] = (signed char)i; es.acc_malla[k] = es.jmalla[i]; break; }
    }
    es.n_it = 0; es.n_sitio = 0; es.todos = -1;
    for (i = 0; i < sh_n_apps; i++) {
        const char *id = sh_apps[i].id;
        if (id && !strcmp(id, "@apps")) { es.todos = (i16)i; continue; }
        if (id && !strcmp(id, "@juegos") && es.jn) continue;    /* los juegos van uno a uno */
        if (id && (!strcmp(id, "@archivos") || !strcmp(id, "@ajustes") || !strcmp(id, "@sistema")))
            poner(es.sitio, &es.n_sitio, 3, sh_apps[i].nombre, i, -1, -1, malla_app(i));
        poner(es.it, &es.n_it, ES_ITEMS - 1, sh_apps[i].nombre, i, -1, acceso_app(i), malla_app(i));
    }
    for (i = 0; i < es.jn; i++) {
        const int d = acceso_de(es.jnom[i], es.jruta[i]);
        poner(es.it, &es.n_it, ES_ITEMS - 1, es.jnom[i], -1, i, d, es.jmalla[i] >= 0 ? es.jmalla[i] : d >= 0 ? -1 : malla_mando());
    }
    es.gen = sh_apps_gen;
    es.version++;
}

/* ---- pintar: utilidades ------------------------------------------------------------------------------------------------------------- */
static int recortar(const Lienzo *l, int *x0, int *y0, int *x1, int *y1)   /* [x0, x1) x [y0, y1) dentro del recorte */
{
    const Rect r = l->recorte;
    if (*x0 < r.x) *x0 = r.x;
    if (*y0 < r.y) *y0 = r.y;
    if (*x1 > r.x + r.w) *x1 = r.x + r.w;
    if (*y1 > r.y + r.h) *y1 = r.y + r.h;
    return *x0 < *x1 && *y0 < *y1;
}
static void gradiente(Lienzo *l, int x, int y, int w, int h, Color a, Color b)   /* de arriba (a) abajo (b) */
{
    int i;
    for (i = 0; i < h; i++) li_rect(l, x, y + i, w, 1, ui_mezcla(a, b, h > 1 ? i * 256 / (h - 1) : 0));
}
static void velo_color(Lienzo *l, int x, int y, int w, int h, Color c, int alfa)   /* c por encima de lo pintado, con alfa 0..256 */
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h, i, j;
    if (!recortar(l, &x0, &y0, &x1, &y1)) return;
    for (j = y0; j < y1; j++) for (i = x0; i < x1; i++) { Color *p = l->cuadro + j * UI_ANCHO + i; *p = ui_mezcla(*p, c, alfa); }
}
static void play(Lienzo *l, int x, int y, int w, int h, Color c)   /* un triángulo «play» que apunta a la derecha, en w x h */
{
    int i;
    for (i = 0; i < h; i++) {
        const int d = 2 * i - (h - 1), a = d < 0 ? -d : d, ancho = h > 1 ? w * (h - 1 - a) / (h - 1) : w;
        if (ancho > 0) li_rect(l, x, y + i, ancho, 1, c);
    }
}
static void texto_sombra(Lienzo *l, int x, int y, const char *s, int bytes, Color c, Color sombra, int marcas)
{
    if (sombra != TRANSPARENTE) li_texto_n(l, x + 1, y + 1, s, bytes, sombra, 1, marcas);
    li_texto_n(l, x, y, s, bytes, c, 1, marcas);
}
static void linea_desvanecida(Lienzo *l, int x, int y, int w, Color borde, Color centro)   /* una línea de 1 px: centro en medio, borde a los lados */
{
    int i;
    for (i = 0; i < w; i++) { const int d = 2 * i - w, a = 256 - (d < 0 ? -d : d) * 256 / (w > 0 ? w : 1); li_rect(l, x + i, y, 1, 1, ui_mezcla(borde, centro, a)); }
}
/* ---- los RECURSOS (shell/escritorio_rec.c, de Blender): son de 240x320, la pantalla del W580 de pie; en otra, el dibujo de aquí --------- */
static int rec(void) { return UI_ANCHO == er.fondo_an && UI_ALTO == er.fondo_al; }
static void pegar(Lienzo *l, int x, int y, const ErImg *im, int tinte)   /* una imagen con su alfa; tinte 0..256: hacia el azul de lo elegido */
{
    int x0 = x, y0 = y, x1 = x + im->w, y1 = y + im->h, i, j;
    if (!recortar(l, &x0, &y0, &x1, &y1)) return;
    for (j = y0; j < y1; j++) {
        const u16 *s = im->pix + (j - y) * im->w - x; const u8 *a = im->alfa + (j - y) * im->w - x;
        Color *p = l->cuadro + j * UI_ANCHO;
        for (i = x0; i < x1; i++) {
            const int al = a[i];
            Color c;
            if (!al) continue;
            c = (Color)s[i];
            if (tinte) c = ui_mezcla(c, C_SEL, tinte);
            p[i] = al == 255 ? c : ui_mezcla(p[i], c, al + (al >> 7));
        }
    }
}
static void mascara(Lienzo *l, int x, int y, const ErImg *im, Color c, int fuerza)   /* una máscara (un texto) del color c; fuerza 0..256 */
{
    int x0 = x, y0 = y, x1 = x + im->w, y1 = y + im->h, i, j;
    if (!recortar(l, &x0, &y0, &x1, &y1)) return;
    for (j = y0; j < y1; j++) {
        const u8 *a = im->alfa + (j - y) * im->w - x;
        Color *p = l->cuadro + j * UI_ANCHO;
        for (i = x0; i < x1; i++) { const int al = (a[i] + (a[i] >> 7)) * fuerza >> 8; if (al) p[i] = al >= 256 ? c : ui_mezcla(p[i], c, al); }
    }
}
static int cubre(int i, int j, int r)          /* cuánto (0..256) del píxel (i, j) de la esquina de arriba a la izquierda de un redondeo de radio r */
{
    const int c8 = 8 * r, r2 = c8 * c8;
    int s, t, n = 0;
    for (s = 0; s < 4; s++) for (t = 0; t < 4; t++) { const int dx = 8 * i + 2 * s + 1 - c8, dy = 8 * j + 2 * t + 1 - c8; if (dx * dx + dy * dy <= r2) n++; }
    return n * 16;
}
static void fila_redonda(Lienzo *l, int x, int y, int w, int j, int h, int r, int arriba, int abajo, Color c, int alfa)
{   /* la fila j (de h) de un rectángulo de w con esquinas de radio r (arriba / abajo), de c con alfa: las esquinas suavizadas */
    int i, dj = -1;
    if (arriba && j < r) dj = j; else if (abajo && j >= h - r) dj = h - 1 - j;
    if (dj < 0) { velo_color(l, x, y + j, w, 1, c, alfa); return; }
    for (i = 0; i < r; i++) { const int a = cubre(i, dj, r) * alfa >> 8; if (a) { velo_color(l, x + i, y + j, 1, 1, c, a); velo_color(l, x + w - 1 - i, y + j, 1, 1, c, a); } }
    velo_color(l, x + r, y + j, w - 2 * r, 1, c, alfa);
}
static int alto_pequena(void) { return dis_alto_texto(1, F_ESTILO(DIS_T_PEQUENA), NULL); }
static int cel_al(void) { return rec() ? 2 + ICO + 4 + 23 + 3 : 2 + ICO + 3 + 2 * alto_pequena() + 2; }   /* con los recursos: el nombre pre-renderizado (dos líneas, 23 px) */
/* un texto en una línea que no pase de `ancho`: si no cabe, se corta con «…» */
static void texto_corto(Lienzo *l, int x, int y, const char *s, int ancho, Color c, Color sombra, int marcas)
{
    int n = (int)strlen(s);
    if (li_ancho_texto_f(s, 1, marcas) <= ancho) { texto_sombra(l, x, y, s, n, c, sombra, marcas); return; }
    {
        const int pto = li_ancho_texto_f("…", 1, marcas);
        while (n > 0 && li_ancho_n(s, n, 1, marcas) + pto > ancho) { n--; while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; }
        texto_sombra(l, x, y, s, n, c, sombra, marcas);
        texto_sombra(l, x + li_ancho_n(s, n, 1, marcas), y, "…", (int)strlen("…"), c, sombra, marcas);
    }
}

/* ---- el FONDO: cielo, nubes y la colina ------------------------------------------------------------------------------------------- */
static struct {
    u8 gen; i16 an, al;
    i16 c1[ES_MAX], c2[ES_MAX];                                  /* la cresta de la colina de delante y de la de detrás */
    signed char sombra[ES_MAX];                                          /* lo que se adelanta la rampa de verde en cada columna (más clara a la izq.) */
    Color cielo[ES_MAX], verde1[ES_MAX], verde2[ES_MAX];
    struct { i16 x, y, r; u32 inv; u8 a; } nube[14]; u8 n_nubes; i16 nube_y0, nube_y1;
} fx;
static Color rampa(const u8 (*p)[4], int n, int v)               /* p: {posición, r, g, b} crecientes; v = posición */
{
    int k;
    if (v <= p[0][0]) return RGB(p[0][1], p[0][2], p[0][3]);
    for (k = 1; k < n; k++)
        if (v <= p[k][0]) {
            const int t = (v - p[k - 1][0]) * 256 / (p[k][0] - p[k - 1][0]);
            return ui_mezcla(RGB(p[k - 1][1], p[k - 1][2], p[k - 1][3]), RGB(p[k][1], p[k][2], p[k][3]), t);
        }
    return RGB(p[n - 1][1], p[n - 1][2], p[n - 1][3]);
}
void sh_escritorio_preparar(void)
{
    /* las nubes, en milésimas del ancho (x, r) y del alto (y), y su fuerza: dos bancos, alargados */
    static const i16 nubes[][4] = { { 560, 95, 120, 190 }, { 680, 80, 150, 210 }, { 800, 100, 115, 190 }, { 900, 90, 90, 170 },
                                    { 620, 120, 100, 150 }, { 150, 165, 95, 150 }, { 260, 150, 120, 170 }, { 360, 170, 90, 140 },
                                    { 80, 60, 70, 110 }, { 970, 175, 80, 120 } };
    static const u8 cielo[][4] = { { 0, 18, 76, 186 }, { 70, 54, 124, 216 }, { 150, 120, 176, 238 }, { 200, 182, 214, 246 } };
    static const u8 v1[][4] = { { 0, 156, 214, 80 }, { 5, 118, 192, 46 }, { 34, 82, 164, 28 }, { 90, 52, 132, 18 }, { 200, 30, 98, 12 } };
    static const u8 v2[][4] = { { 0, 104, 160, 84 }, { 30, 70, 128, 54 }, { 120, 50, 100, 40 } };
    const int W = UI_ANCHO, H = UI_ALTO, hb = H - BARRA_AL;
    int x, y, k;
    if (fx.gen && fx.an == W && fx.al == H) return;
    fx.an = (i16)W; fx.al = (i16)H;
    for (x = 0; x < W && x < ES_MAX; x++) {
        const int u = x * 1000 / W;                                /* 0..999 */
        /* la de delante: sube despacio desde la izquierda, llega arriba algo a la izquierda del centro y cae más deprisa a la derecha */
        const int d1 = u - 330, d2 = u - 980;
        fx.c1[x] = (i16)(hb * 52 / 100 + d1 * d1 * (u < 330 ? 55 : 60) / 1000 * hb / 100000);
        fx.c2[x] = (i16)(hb * 57 / 100 + d2 * d2 * 40 / 1000 * hb / 100000);   /* la de detrás asoma a la derecha */
        fx.sombra[x] = (signed char)((u - 380) * 26 / 1000);
    }
    {
        const int h0 = fx.c1[W * 33 / 100];                          /* el horizonte: lo más alto de la colina */
        for (y = 0; y < H && y < ES_MAX; y++) fx.cielo[y] = rampa(cielo, 4, y * 200 / (h0 > 1 ? h0 : 1));
    }
    for (y = 0; y < ES_MAX; y++) { fx.verde1[y] = rampa(v1, 5, y * 200 / (hb > 1 ? hb / 2 : 1)); fx.verde2[y] = rampa(v2, 3, y * 2); }
    fx.n_nubes = 0; fx.nube_y0 = (i16)H; fx.nube_y1 = 0;
    for (k = 0; k < (int)(sizeof nubes / sizeof nubes[0]); k++) {
        const int cx = nubes[k][0] * W / 1000, cy = nubes[k][1] * hb / 1000, r = nubes[k][2] * W / 1000;
        fx.nube[k].x = (i16)cx; fx.nube[k].y = (i16)cy; fx.nube[k].r = (i16)r; fx.nube[k].a = (u8)nubes[k][3];
        fx.nube[k].inv = (u32)((256u << 16) / (u32)(r * r > 0 ? r * r : 1));
        if (cy - r / 2 < fx.nube_y0) fx.nube_y0 = (i16)(cy - r / 2);
        if (cy + r / 2 > fx.nube_y1) fx.nube_y1 = (i16)(cy + r / 2);
        fx.n_nubes++;
    }
    fx.gen++; if (!fx.gen) fx.gen = 1;
}
/* El píxel (x, y) del fondo: una función pura de las tablas. Las nubes son manchas elípticas (el doble de anchas que altas) con caída
 * suave; manda la más fuerte. */
static void fondo_pintar_rect(Lienzo *l, int x0, int y0, int x1, int y1)
{
    int x, y, k;
    if (!recortar(l, &x0, &y0, &x1, &y1)) return;
    if (rec()) {                                                  /* el paisaje de Blender: 256 colores, también bajo la barra (es translúcida) */
        for (y = y0; y < y1; y++) {
            const u8 *s = er.fondo_idx + y * er.fondo_an + x0;
            Color *p = l->cuadro + y * UI_ANCHO + x0;
            for (x = x0; x < x1; x++) *p++ = (Color)er.fondo_pal[*s++];
        }
        return;
    }
    if (y1 > fx.al - BARRA_AL) y1 = fx.al - BARRA_AL;             /* debajo va la barra, que lo tapa entero */
    for (y = y0; y < y1; y++) {
        Color *p = l->cuadro + y * UI_ANCHO + x0;
        const int con_nubes = y >= fx.nube_y0 && y <= fx.nube_y1;
        for (x = x0; x < x1; x++, p++) {
            const int a1 = fx.c1[x], a2 = fx.c2[x];
            if (y >= a1) { int d = y - a1 + fx.sombra[x]; *p = fx.verde1[d < 0 ? 0 : d >= ES_MAX ? ES_MAX - 1 : d]; continue; }
            if (y >= a2) { *p = fx.verde2[y - a2 < ES_MAX ? y - a2 : ES_MAX - 1]; continue; }
            *p = fx.cielo[y];
            if (con_nubes) {
                int a = 0;
                for (k = 0; k < fx.n_nubes; k++) {
                    const int dx = x - fx.nube[k].x, dy = (y - fx.nube[k].y) * 2, r = fx.nube[k].r;
                    if (dx < -r || dx > r || dy < -r || dy > r) continue;
                    {
                        const u32 d2 = (u32)(dx * dx + dy * dy);
                        const int q = 256 - (int)((d2 * fx.nube[k].inv) >> 16);
                        if (q > 0) { const int v = (q * q >> 8) * fx.nube[k].a >> 8; if (v > a) a = v; }
                    }
                }
                if (a) *p = ui_mezcla(*p, RGB(252, 252, 255), a);
            }
        }
    }
}

/* ---- los ICONOS ------------------------------------------------------------------------------------------------------------------ */
/* Los dibujados, en un cuadrado de lado s con la esquina en (x, y): proporcionales, valen a 32 (escritorio) y a 20 (menú Inicio). */
static void letra_centrada(Lienzo *l, int cx, int cy, const char *t, Color c, int estilo)
{
    const int m = F_ESTILO(estilo), w = li_ancho_texto_f(t, 1, m), h = dis_alto_texto(1, m, NULL);
    li_texto_f(l, cx - w / 2, cy - h / 2, t, c, 1, m);
}
static void dibujar(Lienzo *l, int d, int x, int y, int s)
{
    const int cx = x + s / 2, cy = y + s / 2, grande = s >= 28;
    switch (d) {
    case D_DOOM:                                                 /* el rótulo en rojo y naranja sobre un fondo de metal oscuro */
        li_rect_red(l, x, y + s / 6, s, s * 2 / 3, s / 8, RGB(70, 30, 20));
        li_rect_red(l, x + 1, y + s / 6 + 1, s - 2, s * 2 / 3 - 2, s / 8, RGB(30, 14, 10));
        {   /* en pequeño (el menú Inicio) el rótulo no cabe: su D */
            const char *t = grande || li_ancho_texto_f("DOOM", 1, F_ESTILO(DIS_T_PEQUENA)) <= s - 2 ? "DOOM" : "D";   /* en el escritorio sobresale un poco: cabe en su casilla */
            letra_centrada(l, cx + 1, cy + 1, t, RGB(110, 20, 0), DIS_T_PEQUENA);
            letra_centrada(l, cx, cy, t, RGB(240, 110, 24), DIS_T_PEQUENA);
        }
        break;
    case D_MUSICA: {                                             /* una nota musical blanca sobre un cuadrado naranja redondeado */
        const int q = s / 9 + 1, ccx = x + s * 12 / 32, ccy = y + s * 22 / 32;
        li_rect_red(l, x + s / 16, y + s / 16, s - s / 8, s - s / 8, s / 5, RGB(200, 90, 0));
        li_rect_red(l, x + s / 16 + 1, y + s / 16 + 1, s - s / 8 - 2, s - s / 8 - 2, s / 5, RGB(255, 120, 0));
        li_circulo(l, ccx, ccy, s * 5 / 32 + 1, C_BLANCO);                          /* la cabeza de la nota */
        li_rect(l, ccx + s * 5 / 32 - q / 2, y + s * 7 / 32, q, ccy - (y + s * 7 / 32), C_BLANCO);   /* la plica */
        li_rect(l, ccx + s * 5 / 32 - q / 2, y + s * 7 / 32, s * 7 / 32, q + 1, C_BLANCO);           /* la bandera */
        break; }
    case D_VIDEO: {                                              /* una tira de película gris oscura con el triángulo «play» naranja */
        const int m = s / 8, hy = y + s / 6, hh = s * 2 / 3 + 1;
        int i;
        li_rect_red(l, x, hy, s, hh, s / 8, RGB(50, 52, 58));
        for (i = 0; i < 4; i++) {                                 /* las perforaciones, arriba y abajo */
            li_rect(l, x + m / 2 + i * (s - m) / 4 + 1, hy + 2, m, m / 2 + 1, RGB(200, 200, 205));
            li_rect(l, x + m / 2 + i * (s - m) / 4 + 1, hy + hh - m / 2 - 3, m, m / 2 + 1, RGB(200, 200, 205));
        }
        play(l, cx - s / 8, cy - s / 6, s * 9 / 32, s / 3 + 1, RGB(255, 120, 0));
        break; }
    }
}
/* Los 3D (las mallas del menú, con sus colores: el estilo «Cristal»), pintados FINOS cada vez, como las celdas del menú 3D. SIN caché a
 * propósito: la RAM que el sistema no usa es la de las apps, y a un juego grande (pide 10 616 KB) solo le sobraban 24 KB en la 0.29.3; una
 * caché de sprites de 31 KB lo dejaba sin abrir [medido en el emulador, 6-nov]. Se pintan solo al repintar su sitio (el menú Inicio se salta las
 * filas que no toca el daño). */
static void malla_pintar(Lienzo *l, int malla, int x, int y, int lado)    /* lado: ICO o MINI; (x, y) la esquina del cuadrado */
{
    M3Icono ic;
    memset(&ic, 0, sizeof ic);
    ic.malla = (u8)malla; ic.estilo = M3_CRISTAL; ic.luz = 255; ic.fino = 1;
    ic.guinada = -130; ic.cabeceo = 90; ic.radio = (u16)(lado * 256 * 11 / 32);
    m3_icono_pintar(l, (Rect){ (i16)x, (i16)y, (i16)lado, (i16)lado }, &ic);
}
static int fuera(const Lienzo *l, int y, int h) { return y + h <= l->recorte.y || y >= l->recorte.y + l->recorte.h; }   /* la franja no toca el daño */

/* ---- las piezas: objetos de render -------------------------------------------------------------------------------------------------- */
/* EL FONDO, la raíz: pinta el paisaje y COLOCA a sus hijos (los accesos, la barra, el menú Inicio y el cursor) */
typedef struct { Widget base; Widget **hijos; u8 gen; } EsFondo;
typedef struct { Widget base; u8 i, encima; i16 malla; } EsIcono;
typedef struct { Widget base; Widget *hijo; u8 abierto, encima; i16 minuto; } EsBarra;   /* minuto: hora*60+min de la bandeja (-1 = la pone el hijo) */
typedef struct { Widget base; u8 col, fila, ini, n_it, n_sitio; u16 version; } EsInicio;
typedef struct { Widget base; i16 x, y; } EsCursor;
extern const Clase ClsEsIcono, ClsEsBarra, ClsEsInicio, ClsEsCursor;

static void cel(int i, int *x, int *y) { *x = 2 + accesos[i].col * CEL_AN; *y = 4 + accesos[i].fila * CEL_AL; }   /* la esquina del acceso i */
static void fondo_disponer(Elem *e, Restr r)
{
    unsigned i;
    e->an = r.maxw < INF ? r.maxw : UI_ANCHO; e->al = r.maxh < INF ? r.maxh : UI_ALTO;
    for (i = 0; i < e->n; i++) {
        Elem *h = ui_hijo(e, i);
        Restr c = { 0, e->an, 0, e->al };
        int x = 0, y = 0;
        if (!h) continue;
        ui_disponer(h, c);
        if (h->cls == &ClsEsIcono) cel(((const EsIcono *)h->w)->i, &x, &y);
        else if (h->cls == &ClsEsBarra) y = BARRA_Y;
        else if (h->cls == &ClsEsInicio) y = PAN_Y;
        else if (h->cls == &ClsEsCursor) { x = ((const EsCursor *)h->w)->x; y = ((const EsCursor *)h->w)->y; }
        h->x = (i16)x; h->y = (i16)y;
    }
}
static void fondo_pintar(Elem *e, Lienzo *l, int x, int y) { fondo_pintar_rect(l, x, y, x + e->an, y + e->al); }
static int opaco(Elem *e) { (void)e; return 1; }
static const Clase ClsEsFondo = { .nombre = "EsFondo", .tipo = T_RENDER, .hijos = H_N, .tam = sizeof(EsFondo), .disponer = fondo_disponer,
                                  .pintar = fondo_pintar, .opaco = opaco };

/* UN ACCESO: el icono y, debajo, su nombre en hasta dos líneas, blanco con sombra (elegido: sobre azul) */
static void icono_disponer(Elem *e, Restr r) { (void)r; e->an = CEL_AN; e->al = CEL_AL; }
static void icono_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const EsIcono *w = (const EsIcono *)e->w;
    const Acceso *a = &accesos[w->i];
    const int m = F_ESTILO(DIS_T_PEQUENA), ha = alto_pequena(), ix = x + (CEL_AN - ICO) / 2, iy = y + 2;
    const char *s = a->nombre, *r, *t[2];
    int nb[2], ty = iy + ICO + 3, k;
    if (rec()) {                                                  /* el icono 3D (teñido de azul si está elegido) y su nombre pre-renderizado */
        const ErImg *et = &er.etiqueta[a->er];
        const int ex = x + (CEL_AN - et->w) / 2, ey = iy + ICO + 4;
        pegar(l, ix, iy, &er.ico_g[a->er], w->encima ? 110 : 0);
        if (w->encima) li_rect(l, ex - 2, ey - 2, et->w + 4, et->h + 4, C_SEL);
        else mascara(l, ex + 1, ey + 1, et, RGB(0, 0, 0), 200);
        mascara(l, ex, ey, et, C_BLANCO, 256);
        return;
    }
    if (w->malla >= 0) malla_pintar(l, w->malla, ix, iy, ICO); else dibujar(l, a->dibujo, ix, iy, ICO);
    if (w->encima) velo_color(l, ix, iy, ICO, ICO, C_SEL, 110);
    /* dos líneas: lo que cabe (cortando en un espacio) y el resto */
    nb[0] = li_partir(s, CEL_AN - 4, 1, m);
    r = s + nb[0]; while (*r == ' ') r++;
    while (nb[0] > 0 && s[nb[0] - 1] == ' ') nb[0]--;
    t[0] = s; t[1] = r; nb[1] = (int)strlen(r);
    for (k = 0; k < 2 && nb[k]; k++) {
        const int an = li_ancho_n(t[k], nb[k], 1, m), tx = x + (CEL_AN - (an < CEL_AN - 2 ? an : CEL_AN - 2)) / 2;
        if (w->encima) li_rect(l, tx - 2, ty + k * ha, (an < CEL_AN - 2 ? an : CEL_AN - 2) + 4, ha, C_SEL);
        if (an <= CEL_AN - 2) texto_sombra(l, tx, ty + k * ha, t[k], nb[k], C_BLANCO, w->encima ? TRANSPARENTE : C_SOMBRA, m);
        else texto_corto(l, tx, ty + k * ha, t[k], CEL_AN - 2, C_BLANCO, w->encima ? TRANSPARENTE : C_SOMBRA, m);
    }
}
const Clase ClsEsIcono = { .nombre = "EsIcono", .tipo = T_RENDER, .tam = sizeof(EsIcono), .disponer = icono_disponer, .pintar = icono_pintar };

/* LA BARRA DE TAREAS: azul, con el botón Inicio verde a la izquierda y la bandeja (batería y hora: su hijo) a la derecha */
/* (6-nov) LA HORA DE LA BANDEJA: con los dígitos pre-renderizados (er.digito) pegada a la derecha; a su izquierda, el hijo (la
 * batería y los avisos); si no caben en BANDEJA_AN, la bandeja crece hacia la izquierda. Antes era el sh_reloj_barra del sistema, con la
 * letra pequeña del sistema: demasiado grande, y con el rayo de la carga se cortaba la última cifra (el mantenedor). */
static void hora_txt(int minuto, char *t)                        /* «17:45» (o «5:45» con 12 h); sin hora: «» */
{
    int h, m, n = 0;
    t[0] = 0;
    if (minuto < 0) return;
    h = minuto / 60; m = minuto % 60;
    if (aj.hora12) h = h % 12 ? h % 12 : 12;
    if (h >= 10) t[n++] = (char)('0' + h / 10);
    t[n++] = (char)('0' + h % 10); t[n++] = ':'; t[n++] = (char)('0' + m / 10); t[n++] = (char)('0' + m % 10); t[n] = 0;
}
static const ErImg *glifo(char c) { return c == ':' ? &er.digito[10] : &er.digito[c - '0']; }
static int hora_ancho(int minuto)
{
    char t[8]; int i, w = 0;
    hora_txt(minuto, t);
    for (i = 0; t[i]; i++) w += glifo(t[i])->w + (i ? 1 : 0);
    return w;
}
static void bandeja(const Elem *e, int *bx, int *rx, int *ix)    /* dónde empieza la bandeja, la hora y los iconos (relativo a la barra) */
{
    const EsBarra *w = (const EsBarra *)e->w;
    const Elem *h = ui_hijo((Elem *)e, 0);
    const int ia = h ? h->an : 0;
    *rx = UI_ANCHO - 6 - hora_ancho(w->minuto);
    *ix = *rx - (ia ? ia + 5 : 0);
    *bx = *ix - 7 < UI_ANCHO - BANDEJA_AN ? *ix - 7 : UI_ANCHO - BANDEJA_AN;
}
static void barra_disponer(Elem *e, Restr r)
{
    Elem *h = ui_hijo(e, 0);
    (void)r;
    e->an = (i16)UI_ANCHO; e->al = BARRA_AL;
    if (h && ((const EsBarra *)e->w)->minuto >= 0) {            /* con la hora propia: el hijo son solo los iconos */
        Restr c = { 0, 120, 0, BARRA_AL };
        int bx, rx, ix;
        ui_disponer(h, c);
        bandeja(e, &bx, &rx, &ix);
        h->x = (i16)ix; h->y = (i16)((BARRA_AL - h->al) / 2);
    } else if (h) {
        Restr c = { 0, BANDEJA_AN - 8, 0, BARRA_AL };
        ui_disponer(h, c);
        h->x = (i16)(UI_ANCHO - BANDEJA_AN + (BANDEJA_AN - h->an) / 2); h->y = (i16)((BARRA_AL - h->al) / 2);
    }
}
static void bandera(Lienzo *l, int x, int y, int k)             /* el emblema del botón: un círculo blanco con el punto naranja de senew */
{
    li_circulo(l, x + k, y + k, k + 1, C_BLANCO);
    li_circulo(l, x + k, y + k, k > 3 ? k - 2 : 1, RGB(255, 120, 0));
}
static void boton_inicio(Lienzo *l, int x, int y, int estado)  /* verde, con brillo arriba y la derecha redonda; 1 = encima, 2 = abierto */
{
    const int w = INICIO_AN, h = BARRA_AL, r = 13;
    int j;
    for (j = 0; j < h; j++) {
        Color c;
        if (j == 0) c = RGB(150, 214, 136);
        else if (j == h - 1) c = RGB(22, 84, 24);
        else if (j < h / 2) c = ui_mezcla(RGB(106, 194, 94), RGB(64, 162, 60), (j - 1) * 256 / (h / 2 - 1));
        else c = ui_mezcla(RGB(52, 146, 50), RGB(32, 110, 34), (j - h / 2) * 256 / (h - 1 - h / 2));
        if (estado == 1) c = ui_mezcla(c, C_BLANCO, 46); else if (estado == 2) c = ui_mezcla(c, RGB(0, 36, 0), 76);
        /* la derecha redonda: el redondeo de la izquierda de un rectángulo de 2w empezado en x - w */
        fila_redonda(l, x - w, y, 2 * w, j, h, r, 1, 1, c, 256);
    }
    pegar(l, x + 6, y + (h - er.bandera.h) / 2, &er.bandera, 0);
    mascara(l, x + 27, y + (h - er.inicio.h) / 2 + 1, &er.inicio, RGB(10, 60, 12), 180);
    mascara(l, x + 26, y + (h - er.inicio.h) / 2, &er.inicio, C_BLANCO, 256);
}
static void barra_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const EsBarra *w = (const EsBarra *)e->w;
    if (rec()) {                                                  /* la barra azul TRANSLÚCIDA (se adivina el paisaje), la bandeja y el botón */
        int bx, rx, ix, j;
        bandeja(e, &bx, &rx, &ix); bx += x; (void)ix;
        for (j = 0; j < BARRA_AL; j++) {
            Color c; int a = 228;
            if (j == 0) { c = RGB(118, 170, 252); a = 245; }
            else if (j == 1) { c = RGB(76, 140, 244); a = 238; }
            else if (j >= BARRA_AL - 2) { c = j == BARRA_AL - 1 ? RGB(12, 40, 138) : RGB(20, 58, 168); a = 240; }
            else c = ui_mezcla(RGB(50, 116, 238), RGB(28, 78, 204), (j - 2) * 256 / (BARRA_AL - 5));
            velo_color(l, x, y + j, e->an, 1, c, a);
            velo_color(l, bx, y + j, BANDEJA_AN, 1, j == 0 ? RGB(130, 214, 255) : ui_mezcla(RGB(36, 170, 250), RGB(14, 122, 224), j * 256 / (BARRA_AL - 1)), 230);
        }
        li_rect(l, bx, y + 1, 1, BARRA_AL - 1, RGB(10, 72, 172)); velo_color(l, bx + 1, y + 1, 1, BARRA_AL - 2, RGB(140, 214, 255), 200);
        boton_inicio(l, x, y, w->abierto ? 2 : w->encima ? 1 : 0);
        if (w->minuto >= 0) {                                     /* la hora, en blanco, centrada en alto */
            char t[8]; int i, hx = x + rx;
            hora_txt(w->minuto, t);
            for (i = 0; t[i]; i++) { const ErImg *g = glifo(t[i]); mascara(l, hx, y + (BARRA_AL - g->h) / 2, g, C_BLANCO, 256); hx += g->w + 1; }
        }
        return;
    }
    const int bx = x + UI_ANCHO - BANDEJA_AN, m = F_ESTILO(DIS_T_ETIQUETA), h = dis_alto_texto(1, m, NULL);
    li_rect(l, x, y, e->an, 1, RGB(62, 132, 246));
    gradiente(l, x, y + 1, e->an, 3, RGB(52, 118, 238), RGB(38, 98, 222));
    gradiente(l, x, y + 4, e->an, BARRA_AL - 6, RGB(36, 94, 220), RGB(30, 82, 206));
    li_rect(l, x, y + BARRA_AL - 2, e->an, 2, RGB(22, 64, 178));
    /* la bandeja */
    li_rect(l, bx, y, BANDEJA_AN, BARRA_AL, RGB(18, 136, 232));
    gradiente(l, bx, y + 1, BANDEJA_AN, BARRA_AL / 2, RGB(32, 160, 244), RGB(18, 136, 232));
    li_rect(l, bx, y, 1, BARRA_AL, RGB(10, 76, 180)); li_rect(l, bx + 1, y, 1, BARRA_AL, RGB(90, 190, 255));
    /* el botón Inicio: verde, redondeado a la derecha (más claro con el cursor encima; hundido con el menú abierto) */
    {
        const Color base = w->abierto ? RGB(34, 120, 34) : w->encima ? RGB(76, 176, 70) : RGB(56, 156, 56);
        const Color alto = w->abierto ? RGB(48, 140, 48) : w->encima ? RGB(120, 210, 112) : RGB(96, 196, 92);
        li_rect_red(l, x - 14, y, INICIO_AN + 14, BARRA_AL, 12, RGB(24, 92, 26));
        li_rect_red(l, x - 14, y + 1, INICIO_AN + 13, BARRA_AL - 2, 12, base);
        li_rect_red(l, x - 14, y + 2, INICIO_AN + 8, BARRA_AL / 2 - 2, 9, alto);
        bandera(l, x + 7, y + BARRA_AL / 2 - 7, 6);
        li_texto_f(l, x + 26 + 1, y + (BARRA_AL - h) / 2 + 1, "Inicio", RGB(20, 70, 20), 1, m);
        li_texto_f(l, x + 26, y + (BARRA_AL - h) / 2, "Inicio", C_BLANCO, 1, m);
    }
}
static int barra_actualizar(Elem *e, const Widget *nv)
{
    EsBarra *a = (EsBarra *)e->w; const EsBarra *b = (const EsBarra *)nv;
    const int c = a->minuto != b->minuto ? C_DISPONER : a->abierto != b->abierto || a->encima != b->encima ? C_PINTAR : C_NADA;   /* otra hora: otro ancho */
    *a = *b;
    return c;
}
const Clase ClsEsBarra = { .nombre = "EsBarra", .tipo = T_RENDER, .hijos = H_1, .tam = sizeof(EsBarra), .actualizar = barra_actualizar,
                           .disponer = barra_disponer, .pintar = barra_pintar, .opaco = opaco };

/* EL MENÚ INICIO: cabecera con el usuario, la línea naranja, las dos columnas y el pie con «Apagar» */
enum { COL_IZQ, COL_DER, COL_PIE };
static int todos_hay(void) { return es.todos >= 0; }
static int filas_izq(void) { return es.n_it + todos_hay(); }    /* la última (si la hay) es «Todos los programas» */
static void pie_boton(int *bx, int *by, int *bw, int *bh) { *bw = 84; *bh = PIE_AL - 8; *bx = PAN_AN - *bw - 6; *by = PAN_AL - PIE_AL + 4; }
static void usuario(Lienzo *l, int x, int y, int s)             /* la foto del usuario: una flor naranja en su marco blanco */
{
    int k;
    static const signed char p[5][2] = { { 0, -6 }, { 6, -2 }, { 4, 5 }, { -4, 5 }, { -6, -2 } };
    li_rect_red(l, x, y, s, s, 4, C_BLANCO);
    gradiente(l, x + 2, y + 2, s - 4, s - 4, RGB(120, 190, 246), RGB(60, 150, 70));
    for (k = 0; k < 5; k++) li_circulo(l, x + s / 2 + p[k][0] * s / 30, y + s / 2 + p[k][1] * s / 30, s / 6, RGB(246, 140, 30));
    li_circulo(l, x + s / 2, y + s / 2, s / 8, RGB(255, 220, 60));
}
static void apagar_icono(Lienzo *l, int x, int y, int s)
{
    li_rect_red(l, x, y, s, s, 4, RGB(150, 40, 10));
    li_rect_red(l, x + 1, y + 1, s - 2, s - 2, 4, RGB(232, 90, 34));
    li_circulo(l, x + s / 2, y + s / 2 + 1, s / 3, C_BLANCO);
    li_circulo(l, x + s / 2, y + s / 2 + 1, s / 3 - 2, RGB(232, 90, 34));
    li_rect(l, x + s / 2 - 3, y + 2, 6, s / 2, RGB(232, 90, 34));
    li_rect(l, x + s / 2 - 1, y + 3, 2, s / 2, C_BLANCO);
}
#define SOMBRA 4                                                 /* la del menú Inicio, a la derecha (con los recursos) */
static void inicio_disponer(Elem *e, Restr r) { (void)r; e->an = (i16)(PAN_AN + (rec() ? SOMBRA : 0)); e->al = (i16)PAN_AL; }
static void inicio_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const EsInicio *w = (const EsInicio *)e->w;
    const int m = F_ESTILO(DIS_T_PEQUENA), ha = alto_pequena(), cy = y + CUERPO_Y;
    int i, bx, by, bw, bh;
    if (rec()) {                                                  /* la sombra, la cabecera redondeada con la foto y el nombre, los lados azules */
        int j;
        for (i = 0; i < SOMBRA; i++) velo_color(l, x + PAN_AN + i, y + 6 + i, 1, PAN_AL - 6 - i, RGB(0, 0, 0), 96 - i * 22);
        for (j = 0; j <= CAB_AL; j++)
            fila_redonda(l, x, y, PAN_AN, j, CAB_AL + 1, 7, 1, 0, j == 0 ? RGB(134, 190, 255) : ui_mezcla(RGB(72, 142, 246), RGB(26, 88, 212), (j - 1) * 256 / CAB_AL), 256);
        li_rect(l, x, y + CAB_AL, 2, PAN_AL - CAB_AL, RGB(36, 94, 220)); li_rect(l, x + PAN_AN - 2, y + CAB_AL, 2, PAN_AL - CAB_AL, RGB(36, 94, 220));
        pegar(l, x + 6, y + (CAB_AL - er.usuario.h) / 2, &er.usuario, 0);
        mascara(l, x + 44 + 1, y + (CAB_AL - er.usuario_txt.h) / 2 + 1, &er.usuario_txt, RGB(10, 40, 120), 200);
        mascara(l, x + 44, y + (CAB_AL - er.usuario_txt.h) / 2, &er.usuario_txt, C_BLANCO, 256);
    } else {
    li_rect_red(l, x, y, PAN_AN, PAN_AL, 6, RGB(0, 56, 160));
    gradiente(l, x + 1, y + 1, PAN_AN - 2, CAB_AL, RGB(24, 90, 206), RGB(64, 140, 240));
    li_rect_red(l, x + 1, y + 1, PAN_AN - 2, 6, 5, RGB(30, 96, 214));
    usuario(l, x + 6, y + 5, CAB_AL - 10);
    {
        const int mt = F_ESTILO(DIS_T_ETIQUETA), ht = dis_alto_texto(1, mt, NULL);
        li_texto_f(l, x + CAB_AL + 2 + 1, y + (CAB_AL - ht) / 2 + 2, "W580", RGB(10, 40, 120), 1, mt);
        li_texto_f(l, x + CAB_AL + 2, y + (CAB_AL - ht) / 2 + 1, "W580", C_BLANCO, 1, mt);
    }
    }
    /* la línea naranja: fuerte en el centro y desvaneciéndose a los lados */
    linea_desvanecida(l, x + 1, y + CAB_AL + 1, PAN_AN - 2, RGB(64, 140, 240), RGB(242, 150, 52));
    linea_desvanecida(l, x + 1, y + CAB_AL + 2, PAN_AN - 2, RGB(64, 140, 240), RGB(242, 150, 52));
    /* las dos columnas */
    li_rect(l, x + 1, cy + 1, IZQ_AN, CUERPO_AL - 1, C_BLANCO);
    li_rect(l, x + 1 + IZQ_AN, cy + 1, PAN_AN - 2 - IZQ_AN, CUERPO_AL - 1, RGB(211, 229, 250));
    li_rect(l, x + 1 + IZQ_AN, cy + 1, 1, CUERPO_AL - 1, RGB(149, 189, 238));
    /* la izquierda: las que se desplazan y, al pie, «Todos los programas» */
    for (i = w->ini; i < es.n_it && i < w->ini + VIS; i++) {
        const EsItem *it = &es.it[i];
        const int fy = cy + 4 + (i - w->ini) * FILA_AL, sel = w->col == COL_IZQ && w->fila == i;
        if (fuera(l, fy, FILA_AL)) continue;
        if (sel) li_rect(l, x + 3, fy, IZQ_AN - 4, FILA_AL, C_SEL);
        if (rec() && it->er >= 0) pegar(l, x + 6, fy + (FILA_AL - MINI) / 2, &er.ico_p[(int)it->er], 0);
        else if (it->malla >= 0) malla_pintar(l, it->malla, x + 6, fy + (FILA_AL - MINI) / 2, MINI);
        else if (it->dibujo >= 0) dibujar(l, it->dibujo, x + 6, fy + (FILA_AL - MINI) / 2, MINI);
        texto_corto(l, x + 10 + MINI, fy + (FILA_AL - ha) / 2, it->nombre, IZQ_AN - 16 - MINI, sel ? C_BLANCO : RGB(0, 0, 0), TRANSPARENTE, m);
    }
    if (w->ini > 0) play(l, x + IZQ_AN - 10, cy + 4, 7, 4, RGB(120, 140, 170));            /* hay más arriba (una marca pequeña) */
    if (w->ini + VIS < es.n_it) play(l, x + IZQ_AN - 10, cy + 4 + VIS * FILA_AL - 6, 7, 4, RGB(120, 140, 170));
    if (todos_hay()) {
        const int fy = cy + CUERPO_AL - FILA_AL - 3, sel = w->col == COL_IZQ && w->fila == es.n_it;
        linea_desvanecida(l, x + 8, fy - 3, IZQ_AN - 16, C_BLANCO, RGB(150, 170, 200));
        if (sel) li_rect(l, x + 3, fy, IZQ_AN - 4, FILA_AL, C_SEL);
        texto_corto(l, x + 12, fy + (FILA_AL - ha) / 2, "Todos los programas", IZQ_AN - 34, sel ? C_BLANCO : RGB(0, 0, 0), TRANSPARENTE, m);
        li_rect_red(l, x + IZQ_AN - 20, fy + (FILA_AL - 14) / 2, 14, 14, 3, RGB(46, 150, 40));
        play(l, x + IZQ_AN - 16, fy + (FILA_AL - 8) / 2, 6, 8, C_BLANCO);
    }
    /* la derecha: los sitios del sistema, en negrita azul oscuro */
    for (i = 0; i < es.n_sitio; i++) {
        const EsItem *it = &es.sitio[i];
        const int fy = cy + 6 + i * SITIO_AL, sx = x + IZQ_AN + 3, sel = w->col == COL_DER && w->fila == i;
        if (fuera(l, fy, SITIO_AL)) continue;
        if (sel) li_rect(l, sx, fy, PAN_AN - IZQ_AN - 6, SITIO_AL, C_SEL);
        malla_pintar(l, it->malla, sx + 3, fy + (SITIO_AL - MINI) / 2, MINI);
        texto_corto(l, sx + 6 + MINI, fy + (SITIO_AL - ha) / 2, it->nombre, PAN_AN - IZQ_AN - 14 - MINI, sel ? C_BLANCO : C_AZUL_OSC, TRANSPARENTE, m);
    }
    /* el pie con «Apagar» */
    pie_boton(&bx, &by, &bw, &bh);
    if (rec()) {
        li_rect(l, x, y + PAN_AL - PIE_AL, PAN_AN, 1, RGB(120, 176, 252));
        gradiente(l, x, y + PAN_AL - PIE_AL + 1, PAN_AN, PIE_AL - 1, RGB(70, 140, 246), RGB(30, 90, 212));
        if (w->col == COL_PIE) { int j; for (j = 0; j < bh + 2; j++) fila_redonda(l, x + bx - 3, y + by - 1, bw + 6, j, bh + 2, 4, 1, 1, RGB(150, 200, 255), 110); }
        pegar(l, x + bx, y + by + (bh - er.apagar.h) / 2, &er.apagar, 0);
        mascara(l, x + bx + 27, y + by + (bh - er.apagar_txt.h) / 2 + 1, &er.apagar_txt, RGB(10, 40, 120), 180);
        mascara(l, x + bx + 26, y + by + (bh - er.apagar_txt.h) / 2, &er.apagar_txt, C_BLANCO, 256);
        return;
    }
    gradiente(l, x + 1, y + PAN_AL - PIE_AL, PAN_AN - 2, PIE_AL - 1, RGB(64, 136, 244), RGB(36, 96, 216));
    if (w->col == COL_PIE) li_rect_red(l, x + bx - 2, y + by - 1, bw + 4, bh + 2, 4, RGB(90, 160, 250));
    apagar_icono(l, x + bx, y + by + (bh - 18) / 2, 18);
    li_texto_f(l, x + bx + 24, y + by + (bh - ha) / 2, "Apagar", C_BLANCO, 1, m);
}
const Clase ClsEsInicio = { .nombre = "EsInicio", .tipo = T_RENDER, .tam = sizeof(EsInicio), .disponer = inicio_disponer, .pintar = inicio_pintar };

/* EL CURSOR: una flecha (negra por fuera, blanca por dentro); su punta es (x, y) */
static const char *const flecha[CUR_AL] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
    "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ",
    "X     X..X  ", "      X..X  ", "       XX   " };
static void cursor_disponer(Elem *e, Restr r) { (void)r; e->an = (i16)(CUR_AN + (rec() ? 2 : 0)); e->al = (i16)(CUR_AL + (rec() ? 2 : 0)); }
static void cursor_pintar(Elem *e, Lienzo *l, int x, int y)
{
    int i, j;
    (void)e;
    if (rec())                                                    /* la sombra: la silueta, 2 px abajo a la derecha, suave */
        for (j = 0; j < CUR_AL; j++) for (i = 0; i < CUR_AN; i++) if (flecha[j][i] != ' ') velo_color(l, x + i + 2, y + j + 2, 1, 1, RGB(0, 0, 0), 84);
    for (j = 0; j < CUR_AL; j++)
        for (i = 0; i < CUR_AN; i++) {
            const char c = flecha[j][i];
            if (c == 'X') li_punto(l, x + i, y + j, RGB(0, 0, 0));
            else if (c == '.') li_punto(l, x + i, y + j, C_BLANCO);
        }
}
const Clase ClsEsCursor = { .nombre = "EsCursor", .tipo = T_RENDER, .tam = sizeof(EsCursor), .disponer = cursor_disponer, .pintar = cursor_pintar };

/* LAS NUBES que se mueven (con los recursos): la capa de Blender (2 pantallas de ancho, da la vuelta) sobre el cielo limpio del paisaje, que
 * se desliza hacia la izquierda como las partículas del XMB. Es su propio objeto de render (el primero de los hijos del fondo): moverla repinta
 * SOLO la franja del cielo. x = el desplazamiento de la capa, en píxeles. */
typedef struct { Widget base; u16 x; } EsNubes;
static void nubes_disponer(Elem *e, Restr r) { (void)r; e->an = (i16)UI_ANCHO; e->al = (i16)(rec() ? er.nubes_al : 0); }
static void nubes_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const EsNubes *w = (const EsNubes *)e->w;
    int x0 = x, y0 = y, x1 = x + e->an, y1 = y + e->al, i, j;
    if (!rec() || !recortar(l, &x0, &y0, &x1, &y1)) return;
    for (j = y0; j < y1; j++) {
        const u8 *fila = er.nubes + (j - y) * er.nubes_an;
        Color *p = l->cuadro + j * UI_ANCHO;
        int u = (int)w->x + (x0 - x);
        while (u >= er.nubes_an) u -= er.nubes_an;
        for (i = x0; i < x1; i++) {
            const u8 v = fila[u];
            if (v >= 16) { const int a = (v >> 4) * 17; p[i] = ui_mezcla(p[i], (Color)er.nubes_col[v & 15], a + (a >> 7)); }
            if (++u == er.nubes_an) u = 0;
        }
    }
}
const Clase ClsEsNubes = { .nombre = "EsNubes", .tipo = T_RENDER, .tam = sizeof(EsNubes), .disponer = nubes_disponer, .pintar = nubes_pintar };

/* ---- el componente: estado, la selección, teclas y el tic ---------------------------------------------------------------------------- */
/* (6-nov, el mantenedor: «así como lo hiciste queda genial; creo que no hace falta el acelerómetro: que se vaya moviendo entre los iconos con ese
 * sombreado azul, la tecla central abre el atajo, y al ir hasta abajo pase el mouse sobre el botón de Inicio») LA CRUCETA elige: entre los
 * accesos (por columnas y filas), y bajando del último de una columna, el botón Inicio. El ratón no se mueve con el sensor: se DESLIZA hasta
 * lo elegido (tau ~70 ms), como si alguien lo llevara. Sin el sensor: ni calibrar, ni temblor, ni gasto. */
typedef struct Escritorio { Widget base; } Escritorio;
#define SEL_INICIO 100
typedef struct {
    u8  activo, empezado, inicio;
    u8  sel, ult;                                                /* lo elegido en el escritorio (0..N_ACC-1 o SEL_INICIO) y el último acceso */
    u8  col, fila, ini;                                          /* lo elegido en el menú Inicio y su primera fila a la vista */
    i32 obj[2], pint[2];                                         /* el ratón: adónde va y dónde se pinta (píxeles x16) */
    u32 t_ult, t_nubes;                                          /* t_nubes: el reloj de las nubes (ms) */
    u16 nubes_x;
    i16 cx, cy;
    i16 minuto; u8 reloj;                                        /* la hora que se enseña y si su temporizador (1 s) está puesto */
} EsE;
extern const Clase ClsEscritorio;
/* (6-nov) el escritorio es la PANTALLA DE INICIO (shell/shell.c, standby_construir): vive en la ruta 0, dentro del standby; se ve cuando su
 * ruta es la de arriba */
static Ctx *raiz_de(Ctx *c) { while (c && c->padre) c = c->padre; return c; }
static Ctx *es_ctx;                                              /* el escritorio montado (para shell/giro.c) */
int sh_escritorio_visible(void) { return aj.menu_estilo == M3_ESCRITORIO && es_ctx && raiz_de(es_ctx) == ui_raiz(); }

static int acceso_en(int col, int fila) { int i; for (i = 0; i < N_ACC; i++) if (accesos[i].col == col && accesos[i].fila == fila) return i; return -1; }
static int filas_de(int col) { int f = 0; while (acceso_en(col, f) >= 0) f++; return f; }
static void mover(EsE *s, int k)                                  /* la cruceta en el escritorio */
{
    if (s->sel == SEL_INICIO) {
        if (k == K_ARRIBA) s->sel = s->ult < N_ACC ? s->ult : 0;   /* vuelve al acceso de donde se bajó */
        return;
    }
    {
        const int c = accesos[s->sel].col, f = accesos[s->sel].fila;
        int n = -1;
        if (k == K_ARRIBA && f > 0) n = acceso_en(c, f - 1);
        else if (k == K_ABAJO) { n = acceso_en(c, f + 1); if (n < 0) { s->ult = s->sel; s->sel = SEL_INICIO; return; } }   /* abajo del todo: Inicio */
        else if (k == K_IZQ || k == K_DER) {
            const int nc = c + (k == K_DER ? 1 : -1), nf = filas_de(nc);
            if (nc >= 0 && nf) n = acceso_en(nc, f < nf ? f : nf - 1);
        }
        if (n >= 0) s->sel = (u8)n;
    }
}
static void ver_fila(EsE *s)                                     /* que la fila elegida de la izquierda se vea */
{
    if (s->col != COL_IZQ || s->fila >= es.n_it) return;
    if (s->fila < s->ini) s->ini = s->fila;
    if (s->fila >= s->ini + VIS) s->ini = (u8)(s->fila - VIS + 1);
}
static void objetivo(const EsE *s, int *x, int *y)                /* adónde va la punta del ratón: abajo a la derecha del centro de lo elegido */
{
    if (s->inicio) {
        const int cy = PAN_Y + CUERPO_Y;
        int bx, by, bw, bh;
        if (s->col == COL_PIE) { pie_boton(&bx, &by, &bw, &bh); *x = bx + 34; *y = PAN_Y + by + bh / 2; return; }
        if (s->col == COL_DER) { *x = IZQ_AN + 44; *y = cy + 6 + s->fila * SITIO_AL + SITIO_AL / 2; return; }
        *x = 12 + MINI + 34;
        *y = s->fila >= es.n_it ? cy + CUERPO_AL - 3 - FILA_AL / 2 : cy + 4 + (s->fila - s->ini) * FILA_AL + FILA_AL / 2;
        return;
    }
    if (s->sel == SEL_INICIO) { *x = 46; *y = BARRA_Y + BARRA_AL / 2; return; }
    cel(s->sel, x, y); *x += (CEL_AN + ICO) / 2 - 5; *y += 2 + ICO - 8;   /* en la esquina de abajo a la derecha del icono: sin tapar el nombre */
}

static void es_tic(Ctx *c);
static int visible(Ctx *c) { return aj.menu_estilo == M3_ESCRITORIO && raiz_de(c) == ui_raiz() && sh_pantalla_estado() != EN_APAGADA; }
static void dormir(Ctx *c, EsE *s) { if (s->activo) { ui_cancelar(c, es_tic); s->activo = 0; } }
static void despertar(Ctx *c, EsE *s) { if (s->activo) return; s->activo = 1; s->t_ult = hw_ms(); ui_cada(c, 33, es_tic); }
static void es_tic(Ctx *c)
{
    EsE *s = ui_estado(c);
    const u32 t = hw_ms();
    int dt = (int)(t - s->t_ult), k, cambio = 0, quieto = 1, paso, ox, oy;
    s->t_ult = t;
    if (dt < 1) dt = 1;
    if (dt > 200) dt = 200;
    if (!visible(c)) { dormir(c, s); return; }
    objetivo(s, &ox, &oy);
    s->obj[0] = ox * 16; s->obj[1] = oy * 16;
    paso = dt * 256 / (dt + 70);                                 /* el ratón persigue a lo elegido: tau ~70 ms */
    for (k = 0; k < 2; k++) {
        const i32 d = s->obj[k] - s->pint[k];
        i32 mv = d * paso / 256;
        if (!mv && d) mv = d > 0 ? 1 : -1;
        s->pint[k] += mv;
        if (s->pint[k] != s->obj[k]) quieto = 0;
    }
    {
        const int cx = (int)(s->pint[0] / 16), cy = (int)(s->pint[1] / 16);
        if (cx != s->cx || cy != s->cy) { s->cx = (i16)cx; s->cy = (i16)cy; cambio = 1; }
    }
    if (rec() && !s->inicio && aj.menu_efecto != SH_EFECTO_QUIETO) {   /* las nubes: 1 px cada 90 ms hacia la izquierda (quietas con el menú Inicio) */
        u32 x;
        s->t_nubes += (u32)dt;
        x = (s->t_nubes / 90u) % er.nubes_an;
        if (x != s->nubes_x) { s->nubes_x = (u16)x; cambio = 1; }
        quieto = 0;
    }
    if (cambio) ui_sucio(c);
    if (quieto) dormir(c, s);                                    /* nada que animar: ni un temporizador (lo despierta la próxima tecla) */
}

static Widget *bandeja_iconos(void)                              /* la batería (con el rayo si carga) y los avisos, en blanco */
{
    const Color c = C_BLANCO;
    return Fila(.espacio = 3, .cruzado = P_CENTRO, .encoge = 1, .hijos = HIJOS(   /* .encoge: lo que mide, no todo el ancho */
        sh_bateria_de(c),
        sh_bloqueado() ? Icon(&sh_iconos[IC_CANDADO], .color = c) : NULL,
        sh_musica_estado() == MU_SONANDO ? Icon(&sh_iconos[IC_NOTA], .color = c) : NULL,
        aj.alarma_on ? Icon(&sh_iconos[IC_ALARMA], .color = c) : NULL));
}
static int minuto_ahora(void) { HwFecha f; return sh_ahora(&f) == HW_OK ? f.hora * 60 + f.min : -1; }
static void es_reloj(Ctx *c) { EsE *s = ui_estado(c); if (minuto_ahora() != s->minuto) ui_sucio(c); }   /* cada segundo: ¿otro minuto? */
static void abrir_juego(int j) { Widget *w = cg_abrir_ruta(es.jruta[j]); if (w) ui_empujar(w); }
static void abrir_acceso(int i)
{
    const Acceso *a = &accesos[i];
    int k;
    if (es.acc_app[i] >= 0) { sh_abrir_app(es.acc_app[i]); return; }
    if (es.acc_juego[i] >= 0) { abrir_juego(es.acc_juego[i]); return; }
    for (k = 0; k < 2; k++) if (a->ruta[k] && hw_fs_tam(a->ruta[k])) { Widget *w = cg_abrir_ruta(a->ruta[k]); if (w) ui_empujar(w); return; }
    ui_aviso(a->nombre, a->juego ? "No está en el teléfono: copia su .sen a la carpeta «juegos» de la tarjeta de memoria."
                                 : "Esta app no está en el teléfono.");
}
static void apagar_si(void *d) { (void)d; sh_apagar(); }
static void activar(Ctx *m, EsE *s)                              /* lo elegido en el menú Inicio */
{
    const int col = s->col, fila = s->fila;
    s->inicio = 0; ui_sucio(m);
    if (col == COL_PIE) { ui_confirmar("Apagar", "¿Apagar el teléfono?", apagar_si, NULL); return; }
    if (col == COL_DER) { if (fila < es.n_sitio) sh_abrir_app(es.sitio[fila].app); return; }
    if (fila == es.n_it && todos_hay()) { sh_abrir_app(es.todos); return; }
    if (fila < es.n_it) {
        const EsItem *it = &es.it[fila];
        if (it->app >= 0) sh_abrir_app(it->app);
        else if (it->juego >= 0) abrir_juego(it->juego);
    }
}
static void abrir_inicio(Ctx *m, EsE *s)
{
    if (!es.n_it && !es.n_sitio) return;
    s->inicio = 1; s->col = COL_IZQ; s->fila = 0; s->ini = 0;
    ui_sucio(m);
}
static int es_tecla(Ctx *c, int k, void *dato)
{
    static const u8 atajo[12] = { K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_AST, K_0, K_ALM };
    Ctx *m = ui_ancestro(c, &ClsEscritorio);
    EsE *s = ui_estado(m);
    int i;
    despertar(m, s);                                             /* el ratón va a moverse */
    if (s->inicio) {
        const int n = s->col == COL_IZQ ? filas_izq() : s->col == COL_DER ? es.n_sitio : 1;
        switch (k) {
        case K_ARRIBA:
            if (s->col == COL_PIE) { s->col = COL_IZQ; s->fila = (u8)(filas_izq() ? filas_izq() - 1 : 0); }
            else if (s->fila > 0) s->fila--;
            break;
        case K_ABAJO:
            if (s->col != COL_PIE && s->fila + 1 < n) s->fila++;
            else if (s->col != COL_PIE) { s->col = COL_PIE; s->fila = 0; }
            break;
        case K_IZQ: if (s->col == COL_DER) { s->col = COL_IZQ; s->fila = 0; } break;
        case K_DER: if (s->col == COL_IZQ && es.n_sitio) { s->col = COL_DER; s->fila = 0; } break;
        case K_OK: case K_C: case K_SEL_IZQ: activar(m, s); return 1;
        case K_ATRAS: case K_SEL_DER: s->inicio = 0; ui_sucio(m); return 1;
        default: return 1;
        }
        ver_fila(s); sh_luces_latido(); ui_sucio(m);
        return 1;
    }
    if (k == K_AST) return 0;                                    /* «*» + Sel. izq. bloquea el teclado (shell/energia.c): no es un atajo aquí */
    for (i = 0; i < 12 && i < sh_n_apps; i++) if (k == atajo[i]) { sh_abrir_app(i); return 1; }   /* los atajos de siempre */
    switch (k) {
    case K_OK: case K_C:                                         /* la tecla central (y la C, el «clic» de seCursor): abre lo elegido */
        if (s->sel == SEL_INICIO) abrir_inicio(m, s); else abrir_acceso(s->sel);
        return 1;
    case K_SEL_IZQ: abrir_inicio(m, s); return 1;
    case K_ARRIBA: case K_ABAJO: case K_IZQ: case K_DER:
        { const int antes = s->sel; mover(s, k); if (s->sel != antes) { sh_luces_latido(); ui_sucio(m); } }
        return 1;
    }
    return sh_teclas(c, k, dato);
}
static Widget *es_construir(Ctx *c)
{
    EsE *s = ui_estado(c);
    Widget *h[N_ACC + 5];
    int n = 0, i;
    const int primera = !s->empezado;
    if (es.gen != sh_apps_gen || !es.gen) armar();
    sh_escritorio_preparar();
    if (!s->empezado) {                                          /* el primero elegido, y el ratón ya encima */
        int x, y;
        s->empezado = 1; s->sel = 0; s->ult = 0;
        objetivo(s, &x, &y);
        s->obj[0] = s->pint[0] = x * 16; s->obj[1] = s->pint[1] = y * 16; s->cx = (i16)x; s->cy = (i16)y;
    }
    if (s->sel != SEL_INICIO && s->sel >= N_ACC) s->sel = 0;
    if (s->fila >= (s->col == COL_IZQ ? filas_izq() : s->col == COL_DER ? es.n_sitio : 1)) { s->col = COL_IZQ; s->fila = 0; s->ini = 0; }
    /* se nos reconstruye estando parados (una tecla, la vuelta de una app, la pantalla que se enciende) o es el PRIMER montaje: en ese, la
     * ruta del menú aún no es la de arriba (ui_nucleo.c crea la ruta entera antes de subirla), así que no se puede preguntar; el tic ya dirá */
    es_ctx = c;
    if (!s->activo && (primera || raiz_de(c) == ui_raiz())) despertar(c, s);
    s->minuto = (i16)minuto_ahora();
    if (!s->reloj) { s->reloj = 1; ui_cada(c, 1000, es_reloj); }
    tema_xp.fondo = RGB(0, 0, 0); tema_xp.texto = C_BLANCO; tema_xp.tenue = RGB(200, 220, 255); tema_xp.acento = C_BLANCO;
    tema_xp.sobre_acento = RGB(0, 0, 0); tema_xp.barra = RGB(36, 94, 220);
    if (rec()) h[n++] = ui_nuevo(&(EsNubes){ .base = { &ClsEsNubes, 30 }, .x = s->nubes_x }, sizeof(EsNubes));
    for (i = 0; i < N_ACC; i++)
        h[n++] = ui_nuevo(&(EsIcono){ .base = { &ClsEsIcono, (u32)i + 1 }, .i = (u8)i, .encima = !s->inicio && s->sel == i,
                                      .malla = es.acc_malla[i] }, sizeof(EsIcono));
    h[n++] = ui_nuevo(&(EsBarra){ .base = { &ClsEsBarra, 20 }, .hijo = rec() ? bandeja_iconos() : sh_reloj_barra(&tema_xp), .abierto = s->inicio,
                                  .encima = !s->inicio && s->sel == SEL_INICIO, .minuto = (i16)(rec() ? s->minuto : -1) }, sizeof(EsBarra));
    if (s->inicio)
        h[n++] = ui_nuevo(&(EsInicio){ .base = { &ClsEsInicio, 21 }, .col = s->col, .fila = s->fila, .ini = s->ini, .n_it = es.n_it,
                                       .n_sitio = es.n_sitio, .version = es.version }, sizeof(EsInicio));
    h[n++] = ui_nuevo(&(EsCursor){ .base = { &ClsEsCursor, 22 }, .x = s->cx, .y = s->cy }, sizeof(EsCursor));
    return Teclas(es_tecla, Tema(&tema_xp, ui_nuevo(&(EsFondo){ .base = { &ClsEsFondo, 0 }, .hijos = ui_lista(h, (size_t)n), .gen = fx.gen },
                                                    sizeof(EsFondo))));
}
static void es_desechar(Ctx *c) { EsE *s = ui_estado(c); dormir(c, s); if (s->reloj) { ui_cancelar(c, es_reloj); s->reloj = 0; } if (es_ctx == c) es_ctx = 0; }
COMPONENTE_CON_ESTADO(Escritorio, EsE, es_construir, NULL, .desechar = es_desechar);
Widget *sh_menu_escritorio(void) { return UI_W(Escritorio); }
