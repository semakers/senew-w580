/* menu_medios.c — EL MENÚ DE MEDIOS (28-oct): un menú LINEAL en cruz (una fila de categorías y, debajo, la columna de lo que
 * hay en cada una). el mantenedor: «así como existe el menú en 2D y los menús en malla 3D, debe haber otro que siga la
 * estética del menú de medios; no suple a los que hay, es otra opción; los iconos deben ser los 3D, y era característico que hubiera
 * partículas de luces volando por detrás de los iconos: aprovechemos el framework 3D para que se vean geniales».
 *
 * LO QUE SE VE (aj.menu_estilo == M3_MEDIOS; Ajustes -> «Menú: estilo» -> «Medios»):
 *   - una FILA de categorías (Configuración, Fotos, Audio, Juegos, Organizador, Apps) con la elegida a un tercio de la pantalla;
 *   - debajo, la COLUMNA de lo que hay en esa categoría: la elegida justo bajo la fila, las anteriores suben por ENCIMA de la fila
 *     (como en el XMB) y las siguientes siguen hacia abajo. Izq./der. cambia de categoría, arriba/abajo de elemento, OK abre;
 *   - los iconos son las MALLAS 3D del menú en blanco (estilo M3_MEDIOS de m3d_dibujo.c); la elegida, mayor, con halo, y se mece;
 *   - detrás: el fondo rojo (m3_fondo_preparar), la OLA de tres hebras que se ondula y PARTÍCULAS de luz en 3D: puntos en un volumen
 *     que derivan (las cercanas más deprisa: paralaje), flotan, titilan y giran todas juntas un poco alrededor del eje vertical, con
 *     perspectiva (r3d_sen/r3d_cos). Se dibujan con mezcla ADITIVA y un brillo gaussiano por tamaño (más grandes las cercanas).
 * Las categorías salen del MENÚ de ahora (sh_apps: respeta lo que el usuario eligió en Ajustes -> «Menú principal»); «Juegos» se
 * despliega con los juegos de la TARJETA (cg_juegos_descubrir), como la pantalla «Juegos». Los atajos 1..9 * 0 # siguen abriendo la
 * celda i del menú, como en los demás estilos.
 *
 * CÓMO SE PINTA. Todo es UN objeto de render a pantalla completa (MdLienzo) cuya configuración lleva TODO lo que decide sus píxeles
 * (posiciones animadas, el tiempo de las partículas, la fase del mecerse): un repintado parcial y uno completo dan lo mismo (lo
 * comprueba el banco de PC). Como las partículas se mueven por detrás de todo, cada cuadro se repinta entero: los iconos SIN foco
 * se rasterizan una vez a un sprite (m3_icono_sprite, una caché pequeña) y se pegan; los de foco (dos) se pintan finos cada cuadro.
 * El temporizador (33 ms) solo corre con el menú arriba y la pantalla encendida; con el efecto «quieto» las partículas se quedan
 * quietas y el temporizador solo vive mientras dura una transición. */
#include "shell.h"
#include "m3d.h"
#include "r3d.h"
#include "cargador.h"

extern const Clase ClsMenu;                                     /* shell/menu3d.c: el menú del que este es un estilo */

/* ---- las categorías ------------------------------------------------------------------------------------------------------- */
enum { C_AJUSTES, C_FOTOS, C_MUSICA, C_JUEGOS, C_ORG, C_APPS, C_N };
/* (28-oct) ningún nombre de categoría puede ser el de una app suya: la etiqueta de la categoría y la del elemento con foco salían iguales una
 * encima de la otra («Ajustes» con la app Ajustes; «Música» con la app Música, que el mantenedor quiere que se llame así). */
static const char *const cat_nombre[C_N] = { "Configuración", "Fotos", "Audio", "Juegos", "Organizador", "Apps" };
#define MD_ITEMS 20
#define MD_JUEGOS 12
typedef struct { const char *nombre; i16 app; signed char juego; u8 malla; } MdItem;     /* app: celda de sh_apps; juego: de jj[]; los dos -1: aviso */
typedef struct { u8 id, n, sel, malla; MdItem it[MD_ITEMS]; } MdCat;
static struct {
    u32 gen;                                                    /* sh_apps_gen con que se armaron (0 = nunca) */
    u8 n_cat, cat_ini;
    MdCat c[C_N];
    char jnom[MD_JUEGOS][30], jruta[MD_JUEGOS][96];
    u8 jn;
    u16 version;                                                /* sube cuando cambian (va en la configuración del render) */
} md;

static int es(const char *id, const char *a) { return id && !strcmp(id, a); }
static int cat_de(const App *a)
{
    const char *id = a->id;
    if (es(id, "@ajustes") || es(id, "@sistema")) return C_AJUSTES;
    if (es(id, "/apps/imagenes.sen") || es(id, "/apps/camara.sen")) return C_FOTOS;
    if (es(id, SH_APP_MUSICA) || es(id, "/apps/grabadora.sen")) return C_MUSICA;
    if (es(id, "@juegos") || a->entrar) return C_JUEGOS;       /* (un juego puesto a mano en el menú: entra y no vuelve hasta salir) */
    if (es(id, "@archivos") || es(id, "/apps/reloj.sen") || es(id, "/apps/notas.sen") || es(id, "/apps/calculadora.sen") ||
        es(id, "/apps/movimiento.sen") || es(id, "/apps/pendiente.sen")) return C_ORG;
    return C_APPS;                                              /* «Apps», las .sen del usuario, las externas enlazadas */
}
static int malla_app(int i) { return sh_apps[i].malla ? sh_apps[i].malla - 1 : m3_malla_de_icono(sh_apps[i].icono); }
static int malla_mando(void) { int m = cg_juegos_malla(); return m ? m - 1 : m3_malla_de_icono(IC_JUEGO); }
static void poner_item(MdCat *c, const char *nombre, int app, int juego, int malla)
{
    if (c->n >= MD_ITEMS) return;
    c->it[c->n++] = (MdItem){ nombre, (i16)app, (signed char)juego, (u8)malla };
}
/* Rehace las categorías desde el menú de ahora. Los juegos de la tarjeta se descubren UNA vez por armado del menú (sh_apps_gen): sus
 * mallas cogen huecos del cargador hasta el próximo sh_apps_init. */
static void armar(void)
{
    MdCat tmp[C_N];
    int i, k, hay_juegos = 0, mallas[MD_JUEGOS];
    u8 sel_viejo[C_N];
    for (k = 0; k < C_N; k++) sel_viejo[k] = 0;
    for (k = 0; k < md.n_cat; k++) sel_viejo[md.c[k].id] = md.c[k].sel;
    memset(tmp, 0, sizeof tmp);
    for (k = 0; k < C_N; k++) tmp[k].id = (u8)k;
    for (i = 0; i < sh_n_apps; i++) {
        const int c = cat_de(&sh_apps[i]);
        if (es(sh_apps[i].id, "@juegos")) { hay_juegos = 1; continue; }
        if (es(sh_apps[i].id, "@apps")) continue;              /* «Todas las apps», al final de su categoría */
        poner_item(&tmp[c], sh_apps[i].nombre, i, -1, malla_app(i));
    }
    if (hay_juegos) {
        const int n = cg_juegos_descubrir(mallas, MD_JUEGOS);
        md.jn = 0;
        for (i = 0; i < n && md.jn < MD_JUEGOS; i++) {
            CgInfo in;
            if (cg_app_info(i, &in)) continue;
            memcpy(md.jnom[md.jn], in.nombre, sizeof md.jnom[0]); md.jnom[md.jn][sizeof md.jnom[0] - 1] = 0;
            memcpy(md.jruta[md.jn], in.ruta, sizeof md.jruta[0]); md.jruta[md.jn][sizeof md.jruta[0] - 1] = 0;
            poner_item(&tmp[C_JUEGOS], md.jnom[md.jn], -1, md.jn, mallas[i] ? mallas[i] - 1 : malla_mando());
            md.jn++;
        }
        if (!tmp[C_JUEGOS].n) poner_item(&tmp[C_JUEGOS], "Sin juegos en la tarjeta", -1, -1, malla_mando());
    }
    for (i = 0; i < sh_n_apps; i++) if (es(sh_apps[i].id, "@apps")) poner_item(&tmp[C_APPS], "Todas las apps", i, -1, malla_app(i));
    tmp[C_AJUSTES].malla = (u8)m3_malla_de_icono(IC_AJUSTES);
    tmp[C_FOTOS].malla = (u8)m3_malla_de_icono(IC_IMAGENES);
    tmp[C_MUSICA].malla = (u8)m3_malla_de_icono(IC_MUSICA);
    for (i = 0; i < sh_n_apps; i++) if (es(sh_apps[i].id, SH_APP_MUSICA)) tmp[C_MUSICA].malla = (u8)malla_app(i);   /* el disco de Música */
    tmp[C_JUEGOS].malla = (u8)malla_mando();
    tmp[C_ORG].malla = (u8)m3_malla_de_icono(IC_RELOJ);
    tmp[C_APPS].malla = (u8)m3_malla_de_icono(IC_APPS);
    md.n_cat = 0; md.cat_ini = 0;
    for (k = 0; k < C_N; k++) {
        if (!tmp[k].n) continue;                                /* una categoría vacía no sale */
        if (k == C_MUSICA) md.cat_ini = md.n_cat;               /* se empieza por Música */
        md.c[md.n_cat] = tmp[k];
        md.c[md.n_cat].sel = sel_viejo[k] < tmp[k].n ? sel_viejo[k] : 0;
        md.n_cat++;
    }
    md.gen = sh_apps_gen;
    md.version++;
}

/* ---- la geometría (de la pantalla de ahora: vale de pie y girado) ---------------------------------------------------------- */
#define FX (UI_ANCHO * 30 / 100)                 /* la x de la categoría elegida (y de su columna) */
#define DXC (UI_ANCHO > UI_ALTO ? 76 : 64)       /* de una categoría a la siguiente */
#define YC (UI_ALTO * 27 / 100)                  /* la fila de categorías */
#define YSEL (YC + (UI_ALTO > UI_ALTO_BAJO ? 66 : 54))   /* el elemento elegido */
#define UI_ALTO_BAJO 260
#define DYI (UI_ALTO > UI_ALTO_BAJO ? 46 : 38)   /* de un elemento al siguiente */
#define YARRIBA (YC - 50)                        /* el primero de los de ENCIMA de la fila */
#define R_CAT (13 * 256)                         /* lo que mide 1.0 de la malla: categoría sin foco, */
#define R_CAT_F (19 * 256)                       /* con foco, */
#define R_IT (11 * 256)                          /* elemento sin foco */
#define R_IT_F (16 * 256)                        /* y con foco */
#define POSE_GUI (-130)
#define POSE_CAB 90

static Color c_sombra_txt, c_item_arriba, c_item_abajo;      /* (28-oct) los colores de los textos y del halo, del color de ahora: tintes() */
static u32 c_halo565 = 0xFFDD;

/* ---- la caché de sprites (iconos SIN foco: radio y pose fijos) ------------------------------------------------------------- */
#define SPR_N 14
#define SPR_W 36
#define SPR_H 34
static struct { u8 ok, malla; u16 radio; u32 uso; u16 pix[SPR_W * SPR_H]; u8 cob[SPR_W * SPR_H]; } spr[SPR_N];
/* Y los DOS con foco (la categoría y el elemento): se mecen despacio, así que su pose cambia ~15 veces por segundo (la fase va a
 * pasos de 96, uno cada dos cuadros) y se rasterizan solo entonces; entre medias, se pegan. [medido: ~21 000 cuentas cada uno] */
#define SPF_W 60
#define SPF_H 56
static struct { u8 ok, malla; u16 radio; i16 gui, cab; u16 pix[SPF_W * SPF_H]; u8 cob[SPF_W * SPF_H]; } spf[2];
static u32 spr_reloj;
static int sprite(int malla, int radio)
{
    int i, viejo = 0;
    for (i = 0; i < SPR_N; i++) if (spr[i].ok && spr[i].malla == malla && spr[i].radio == radio) { spr[i].uso = ++spr_reloj; return i; }
    for (i = 1; i < SPR_N; i++) if (!spr[i].ok || (spr[viejo].ok && spr[i].uso < spr[viejo].uso)) viejo = i;
    {
        M3Icono ic;
        memset(&ic, 0, sizeof ic);
        ic.malla = (u8)malla; ic.estilo = M3_MEDIOS; ic.luz = 255; ic.fino = 1;
        ic.guinada = POSE_GUI; ic.cabeceo = POSE_CAB; ic.radio = (u16)radio;
        m3_icono_sprite(&ic, SPR_W, SPR_H, spr[viejo].pix, spr[viejo].cob);
    }
    spr[viejo].ok = 1; spr[viejo].malla = (u8)malla; spr[viejo].radio = (u16)radio; spr[viejo].uso = ++spr_reloj;
    return viejo;
}
/* Un icono centrado en (cx, cy): sin foco y a su radio de reposo, de la caché; con el foco entero y a su radio de foco, de su sprite
 * de foco (`hueco` 0 = la categoría, 1 = el elemento); en medio (una transición), fino y directo. Con foco, un halo debajo. */
static void icono(Lienzo *l, int malla, int cx, int cy, int radio, int reposo, int foco_radio, int foco, u16 fase, int alfa, int hueco)
{
    M3Icono ic;
    if (radio == reposo && !foco) {
        const int k = sprite(malla, radio);
        m3_sprite_pintar(l, cx - SPR_W / 2, cy - SPR_H / 2, SPR_W, SPR_H, spr[k].pix, spr[k].cob, alfa);
        return;
    }
    memset(&ic, 0, sizeof ic);
    ic.malla = (u8)malla; ic.estilo = M3_MEDIOS; ic.luz = (u8)(alfa >= 256 ? 255 : 120 + alfa / 2); ic.fino = 1;
    fase &= (u16)~95u;                                          /* la pose, a pasos: ~15 por segundo */
    ic.guinada = (i16)(POSE_GUI + ((((i32)r3d_sen(fase) * 170) >> 14) * foco >> 8));            /* se mece ±15° */
    ic.cabeceo = (i16)(POSE_CAB + ((((i32)r3d_sen((u32)fase * 2 + 1024) * 40) >> 14) * foco >> 8));
    ic.radio = (u16)radio;
    if (foco) m3_halo(l, cx, cy, 32, 29, c_halo565, (foco * 150 >> 8) / 8 * 8);   /* el halo, blanco cálido: el MISMO tamaño para los dos
                                                                               * y la fuerza a pasos de 8, para que su mapa se reutilice */
    if (foco >= 255 && radio == foco_radio && alfa >= 256) {
        if (!spf[hueco].ok || spf[hueco].malla != malla || spf[hueco].radio != radio || spf[hueco].gui != ic.guinada || spf[hueco].cab != ic.cabeceo) {
            m3_icono_sprite(&ic, SPF_W, SPF_H, spf[hueco].pix, spf[hueco].cob);
            spf[hueco].ok = 1; spf[hueco].malla = (u8)malla; spf[hueco].radio = (u16)radio; spf[hueco].gui = ic.guinada; spf[hueco].cab = ic.cabeceo;
        }
        m3_sprite_pintar(l, cx - SPF_W / 2, cy - SPF_H / 2, SPF_W, SPF_H, spf[hueco].pix, spf[hueco].cob, 256);
        return;
    }
    m3_icono_pintar(l, (Rect){ (i16)(cx - SPF_W / 2), (i16)(cy - SPF_H / 2), SPF_W, SPF_H }, &ic);
}

/* ---- lo aditivo: la ola y las partículas -------------------------------------------------------------------------------------- */
/* + color x a/256, saturando. El color, ya «abierto» (verde en la mitad alta: 0x07E0F81F) y multiplicado por cada alfa en una tabla;
 * la suma de los tres canales es UNA suma de 32 bits y la saturación sale de los bits de acarreo. p en formato de panel. [medido:
 * la ola costaba 28 400 cuentas por cuadro con la suma canal a canal] */
static inline void sumar_e(Color *p, u32 e)
{
    u32 d = *p, o;
    d = ((d >> 8) | (d << 8)) & 0xFFFF;
    d = ((d | (d << 16)) & 0x07E0F81Fu) + e;
    o = d & 0x08010020u;                                        /* acarreos: azul (bit 5), rojo (16), verde (27) */
    d = (d | (((o & 0x00010020u) - ((o & 0x00010020u) >> 5)) | ((o & 0x08000000u) - ((o & 0x08000000u) >> 6)))) & 0x07E0F81Fu;
    d = (d & 0xFFFF) | (d >> 16);
    *p = (Color)((d >> 8) | (d << 8));
}
static u32 tab_ola[256], tab_luz[256], tab_bokeh[256];
static void tabla(u32 *t, u32 r8, u32 g8, u32 b8)
{
    u32 a;
    for (a = 0; a < 256; a++) t[a] = (((g8 * a) >> 10) << 21) | (((r8 * a) >> 11) << 11) | ((b8 * a) >> 11);
}
/* La OLA del XMB: tres hebras que se ondulan a lo ancho (una ancha y tenue, una fina y brillante, una media), con su brillo cayendo
 * como una campana a cada lado. El centro de cada hebra va en 1/16 de píxel: la fina no hace escalones. t en ms. */
static void ola(Lienzo *l, Rect c, u32 t)
{
    static const struct { i16 off, amp, s; u16 pico, fx, vel, fase; } h[3] = {
        { 0, 20, 12, 40, 9, 7, 0 }, { 4, 15, 3, 150, 11, 9, 900 }, { -6, 11, 8, 48, 14, 5, 2300 } };   /* (la ancha: 18 -> 12 filas de radio) */
    static u8 campana[3][2 * 19 * 16 + 1], lista;              /* el alfa por distancia al centro (1/16 de píxel): sin dividir por píxel */
    const int base = UI_ALTO * 62 / 100;
    int x, k, y;
    if (!lista) {
        for (k = 0; k < 3; k++) {
            const int s16 = (h[k].s + 1) * 16;
            for (y = -s16; y <= s16; y++) {
                const int q = y * y >= s16 * s16 ? 0 : 256 - y * y * 256 / (s16 * s16);
                campana[k][y + s16] = (u8)(((u32)((q * q >> 8) * h[k].pico) >> 8) > 255 ? 255 : ((u32)((q * q >> 8) * h[k].pico) >> 8));
            }
        }
        lista = 1;
    }
    for (x = c.x; x < c.x + c.w; x++) {
        for (k = 0; k < 3; k++) {
            const u32 ang = ((u32)x * h[k].fx + (t * h[k].vel) / 64u + h[k].fase) & 4095u;
            const int yc16 = (base + h[k].off) * 16 + (int)(((i32)r3d_sen(ang) * h[k].amp * 16) >> 14) - (((x - UI_ANCHO / 2) * 2731) >> 10);   /* x 16/6, sin dividir */
            const int s16 = (h[k].s + 1) * 16, y0 = (yc16 - s16) / 16 < c.y ? c.y : (yc16 - s16) / 16;
            const int y1 = (yc16 + s16) / 16 >= c.y + c.h ? c.y + c.h - 1 : (yc16 + s16) / 16;
            for (y = y0; y <= y1; y++) {
                const int d = y * 16 + 8 - yc16;
                const u32 a = d < -s16 || d > s16 ? 0 : campana[k][d + s16];
                if (a) sumar_e(l->cuadro + y * UI_ANCHO + x, tab_ola[a]);
            }
        }
    }
}
/* Las PARTÍCULAS de luz: tres clases, como en el XMB y el Media Center:
 *   chispas  pequeñas y muy brillantes, con un núcleo blanco que titila fuerte;
 *   brillos  medianas y suaves;
 *   bokeh    grandes, muy tenues y lentas (luces desenfocadas al fondo).
 * Viven en un volumen 3D: derivan hacia la izquierda (las cercanas más deprisa: paralaje), flotan hacia arriba con un vaivén, y
 * todas giran juntas un poco alrededor del eje vertical; se proyectan con perspectiva y su tamaño y su brillo salen de la
 * profundidad. El brillo es una campana precalculada por tamaño, sumada (aditiva). */
#define NP 72
#define NIVELES 12
#define G_MAX 13                                  /* medio lado del mayor brillo */
#define LADO (2 * G_MAX + 1)
static u8 brillo[NIVELES][LADO * LADO];
static u8 g_med[NIVELES];                         /* medio lado de cada nivel: no se recorre lo que vale 0 */
enum { P_CHISPA, P_BRILLO, P_BOKEH };
static struct { i32 u0, v0, z, su, sv, amp, ph, tw, tf, br; u8 clase; } pt[NP];
static u8 hecho;
static u32 azar(u32 *s) { u32 x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x; }
static void preparar_particulas(void)
{
    u32 s = 0x2C1B3C6Du;
    int i, n, x, y;
    for (n = 0; n < NIVELES; n++) {                            /* radio = 0,6 + 0,55 n px; alfa = (1 - d²/(4R²))^4: una campana */
        const int r16 = 10 + 9 * n, lim = 4 * r16 * r16;
        int m = 0;
        for (y = -G_MAX; y <= G_MAX; y++)
            for (x = -G_MAX; x <= G_MAX; x++) {
                const int d2 = (x * x + y * y) * 256;
                int q = d2 >= lim ? 0 : 256 - d2 * 256 / lim, a;
                q = q * q >> 8; q = q * q >> 8;
                a = q > 255 ? 255 : q;
                if (!x && !y) a = 255;
                brillo[n][(y + G_MAX) * LADO + x + G_MAX] = (u8)a;
                if (a && (x > m || -x > m)) m = x > 0 ? x : -x;
            }
        g_med[n] = (u8)m;
    }
    for (i = 0; i < NP; i++) {
        const u32 a = azar(&s), b = azar(&s), c = azar(&s);
        pt[i].clase = (u8)(i % 9 == 0 ? P_BOKEH : i % 3 == 0 ? P_BRILLO : P_CHISPA);
        pt[i].z = 384 + (int)(a % 1100);                        /* profundidad Q8: 1,5 .. 5,8 */
        pt[i].u0 = (int)(b % 11200) - 5600;                     /* x normalizada Q12: ±1,37 */
        pt[i].v0 = (i & 1) ? (int)(c % 2600) - 2200 : (int)(c % 8000) - 4000;   /* la mitad, a lo largo de la ola */
        pt[i].su = -(int)(240 + azar(&s) % 420) * 768 / pt[i].z;  /* deriva a la izquierda: las cercanas, más deprisa */
        pt[i].sv = (int)(azar(&s) % 110) + 20;                  /* y suben un poco */
        pt[i].amp = 80 + (int)(azar(&s) % 360);
        pt[i].ph = (int)(azar(&s) & 4095);
        pt[i].tw = (int)(azar(&s) & 4095);
        pt[i].tf = 1 + (int)(azar(&s) % 4);
        pt[i].br = 150 + (int)(azar(&s) % 106);
        if (pt[i].clase == P_BOKEH) { pt[i].su /= 2; pt[i].sv /= 2; pt[i].br = 40 + (int)(azar(&s) % 40); }
    }
    hecho = 1;
}
static i32 envolver(i32 v, i32 lo, i32 span) { v = (v - lo) % span; if (v < 0) v += span; return v + lo; }
static void particulas(Lienzo *l, Rect c, u32 t)
{
    const i32 zc = 896, f = UI_ANCHO / 2;
    const u32 giro = (u32)(((i32)r3d_sen((t / 12u) & 4095u) * 130) >> 14) & 4095u;   /* ±11°, una vuelta del vaivén cada ~49 s */
    const i32 cs = r3d_cos(giro), sn = r3d_sen(giro);
    int i;
    if (!hecho) preparar_particulas();
    for (i = 0; i < NP; i++) {
        const i32 u = envolver(pt[i].u0 + (i32)((s64)pt[i].su * t / 1000), -5600, 11200);
        const i32 v = envolver(pt[i].v0 + (i32)((s64)pt[i].sv * t / 1000), -4600, 9200) + (((i32)r3d_sen((u32)(pt[i].ph + (i32)(t / 4u)) & 4095u) * pt[i].amp) >> 14);
        const i32 X = (u * pt[i].z) >> 12, Y = (v * pt[i].z) >> 12, Zd = pt[i].z - zc;   /* Q8 */
        const i32 Xr = (X * cs - Zd * sn) >> 14, Zr = zc + ((Zd * cs + X * sn) >> 14);
        const i32 tw = r3d_sen((u32)(pt[i].tw + (i32)(t * (u32)pt[i].tf / 3u)) & 4095u);   /* -16384..16384 */
        int sx, sy, n, a, x, y, x0, x1, y0, y1, g;
        const u32 *tc = tab_luz;
        if (Zr < 200) continue;
        sx = UI_ANCHO / 2 + Xr * f / Zr; sy = UI_ALTO * 55 / 100 - Y * f / Zr;
        if (pt[i].clase == P_BOKEH) { n = (int)(12 * 640 / Zr) + 3; a = pt[i].br; tc = tab_bokeh; }
        else {
            n = (int)((pt[i].clase == P_BRILLO ? 8 : 5) * 640 / Zr) - 1;   /* más grandes las cercanas */
            a = pt[i].br * (pt[i].clase == P_CHISPA ? 110 + ((tw * 146) >> 14) + ((tw > 14000) ? 120 : 0)   /* la chispa: titila fuerte */
                                                    : 170 + ((tw * 60) >> 14)) >> 8;
            a = a * (1400 - (Zr > 1400 ? 1400 : Zr) + 400) / 1400; /* las lejanas, más tenues */
        }
        if (n < 0) n = 0;
        if (n >= NIVELES) n = NIVELES - 1;
        if (a <= 6) continue;
        if (a > 255) a = 255;
        g = g_med[n];
        x0 = sx - g; x1 = sx + g; y0 = sy - g; y1 = sy + g;
        if (x0 < c.x) x0 = c.x;
        if (y0 < c.y) y0 = c.y;
        if (x1 >= c.x + c.w) x1 = c.x + c.w - 1;
        if (y1 >= c.y + c.h) y1 = c.y + c.h - 1;
        for (y = y0; y <= y1; y++) {
            const u8 *b = brillo[n] + (y - sy + G_MAX) * LADO + (x0 - sx + G_MAX);
            Color *p = l->cuadro + y * UI_ANCHO + x0;
            for (x = x0; x <= x1; x++, b++, p++) if (*b) sumar_e(p, tc[(u32)(*b * a) >> 8]);
        }
    }
}

/* ---- (28-oct) LOS TINTES con el color de Medios de ahora (Ajustes -> «  color de Medios»; la plantilla es el rojo: shell/m3d_dibujo.c).
 * Al cambiar de color se rehacen las tablas de la ola y de las partículas, los colores de los textos y del halo, y se vacían las cachés
 * de sprites (llevan el color dentro). Se llama al empezar a pintar (el menú y el fondo animado del standby). */
static int tinte_hecho = -1;
static void tintes(void)
{
    u8 o[3] = { 255, 236, 226 }, lz[3] = { 255, 232, 214 }, bk[3] = { 255, 190, 180 }, h[3] = { 255, 248, 232 };   /* h: el 0xFFDD de antes */
    int i;
    if (tinte_hecho == m3_medios_color_actual()) return;
    tinte_hecho = m3_medios_color_actual();
    m3_medios_rgb(o); m3_medios_rgb(lz); m3_medios_rgb(bk); m3_medios_rgb(h);
    tabla(tab_ola, o[0], o[1], o[2]); tabla(tab_luz, lz[0], lz[1], lz[2]); tabla(tab_bokeh, bk[0], bk[1], bk[2]);
    c_sombra_txt = m3_medios_c(RGB(70, 0, 0)); c_item_arriba = m3_medios_c(RGB(255, 170, 165)); c_item_abajo = m3_medios_c(RGB(255, 222, 218));
    c_halo565 = tinte_hecho ? ((u32)(h[0] >> 3) << 11 | (u32)(h[1] >> 2) << 5 | (u32)(h[2] >> 3)) : 0xFFDDu;
    for (i = 0; i < SPR_N; i++) spr[i].ok = 0;
    spf[0].ok = spf[1].ok = 0;
}

/* ---- el objeto de render: todo lo que decide los píxeles va aquí -------------------------------------------------------------- */
typedef struct { Widget base; Widget **hijos; i32 pos_c, pos_i; u32 t; u16 fase, version; u8 cat, n_cat; } MdLienzo;
static void texto_sombra(Lienzo *l, int x, int y, const char *s, Color c, int estilo)
{
#ifdef MD_SIN_TEXTO
    return;
#endif
    li_texto_f(l, x + 1, y + 1, s, c_sombra_txt, 1, F_ESTILO(estilo));
    li_texto_f(l, x, y, s, c, 1, F_ESTILO(estilo));
}
static void lienzo_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const MdLienzo *w = (const MdLienzo *)e->w;
    Rect r = { (i16)x, (i16)y, e->an, e->al };
    Rect c = l->recorte;
    int k, j, ab = 0;
    tintes();
    m3_fondo_pintar(l, r, 0, 0);
#ifndef MD_SIN_OLA                                              /* (MD_SIN_*: solo para medir cuánto cuesta cada capa, herr/medir.sh) */
    ola(l, c, w->t);
#endif
#ifndef MD_SIN_PARTICULAS
    particulas(l, c, w->t);
#endif
#ifdef MD_SIN_ICONOS
    return;
#endif
    if (!w->n_cat) return;
    /* la columna de la categoría elegida: se desliza con ella y se desvanece mientras se cambia de categoría */
    {
        const MdCat *ct = &md.c[w->cat];
        const i32 dc = (i32)w->cat * 256 - w->pos_c, cx = FX + dc * DXC / 256;
        const int alfa = 256 - (dc < 0 ? -dc : dc) * 2 > 0 ? 256 - (dc < 0 ? -dc : dc) * 2 : 0;
        if (alfa > 0)
            for (j = 0; j < ct->n; j++) {
                const i32 rel = (i32)j * 256 - w->pos_i, d = rel < 0 ? -rel : rel;
                int iy, radio, foco;
                if (rel >= 0) iy = YSEL + rel * DYI / 256;
                else if (rel > -256) iy = YSEL + rel * (YSEL - YARRIBA) / 256;
                else iy = YARRIBA + (rel + 256) * DYI / 256;
                if (iy < -30 || iy + 14 > UI_ALTO - (int)dis_tokens()->m.alto_barra_teclas) continue;   /* lo que caería bajo las teclas, no */
                radio = d >= 256 ? R_IT : R_IT_F - (R_IT_F - R_IT) * d / 256;
                radio = radio / 256 * 256;                       /* a píxeles enteros: la caché se reutiliza */
                foco = d < 128 ? (128 - d) * 2 : 0;
                if (foco > 255) foco = 255;
                icono(l, ct->it[j].malla, cx, iy, radio, R_IT, R_IT_F, foco * alfa >> 8, w->fase, rel < -128 ? alfa * 160 >> 8 : alfa, 1);
                if (alfa > 200) {
                    const int est = d < 128 ? DIS_T_TITULO : DIS_T_CUERPO, ha = dis_alto_texto(1, F_ESTILO(est), NULL);
                    texto_sombra(l, cx + 26, iy - ha / 2, ct->it[j].nombre, d < 128 ? RGB(255, 255, 255) : rel < 0 ? c_item_arriba : c_item_abajo, est);
                }
                ab = 1;
            }
    }
    (void)ab;
    /* la fila de categorías, encima de la columna */
    for (k = 0; k < w->n_cat; k++) {
        const i32 dc = (i32)k * 256 - w->pos_c, d = dc < 0 ? -dc : dc, cx = FX + dc * DXC / 256;
        int radio, foco;
        if (cx < -40 || cx > UI_ANCHO + 40) continue;
        radio = d >= 256 ? R_CAT : R_CAT_F - (R_CAT_F - R_CAT) * d / 256;
        radio = radio / 256 * 256;
        foco = d < 128 ? (128 - d) * 2 : 0;
        if (foco > 255) foco = 255;
        icono(l, md.c[k].malla, cx, YC, radio, R_CAT, R_CAT_F, foco, (u16)(w->fase + 1536), 256, 0);
        if (d < 96) {
            const char *s = cat_nombre[md.c[k].id];
            const int an = li_ancho_texto_f(s, 1, F_ESTILO(DIS_T_ETIQUETA));
            texto_sombra(l, cx - an / 2, YC + 24, s, RGB(255, 255, 255), DIS_T_ETIQUETA);
        }
    }
}
static void lienzo_disponer(Elem *e, Restr r)
{
    unsigned i;
    e->an = r.maxw < INF ? r.maxw : UI_ANCHO; e->al = r.maxh < INF ? r.maxh : UI_ALTO;
    for (i = 0; i < e->n; i++) { Elem *h = ui_hijo(e, i); Restr c = { e->an, e->an, e->al, e->al }; if (h) { ui_disponer(h, c); h->x = h->y = 0; } }
}
static int lienzo_actualizar(Elem *e, const Widget *nv)
{
    MdLienzo *a = (MdLienzo *)e->w; const MdLienzo *b = (const MdLienzo *)nv;
    const int c = (a->pos_c != b->pos_c || a->pos_i != b->pos_i || a->t != b->t || a->fase != b->fase || a->version != b->version ||
                   a->cat != b->cat || a->n_cat != b->n_cat) ? C_PINTAR : C_NADA;
    *a = *b;
    return c;
}
static int lienzo_opaco(Elem *e) { (void)e; return 1; }
static const Clase ClsMdLienzo = { .nombre = "MdLienzo", .tipo = T_RENDER, .hijos = H_N, .tam = sizeof(MdLienzo), .actualizar = lienzo_actualizar,
                                   .disponer = lienzo_disponer, .pintar = lienzo_pintar, .opaco = lienzo_opaco };

/* ---- el componente: estado, teclas y el tic --------------------------------------------------------------------------------- */
typedef struct Medios { Widget base; } Medios;
typedef struct {
    u8  cat, activo, empezado;
    i32 pos_c, pos_i;                                           /* lo que se pinta (x256), persigue a cat y a md.c[cat].sel */
    u32 t, t_ult;                                               /* t: el reloj de las partículas y la ola (ms) */
    u16 fase;                                                   /* el mecerse del foco */
    u16 pend;                                                   /* ms de animación aún sin pintar (el fondo va a ~15 cuadros/s) */
} MediosE;
extern const Clase ClsMedios;
static void md_tic(Ctx *c);
static int visible(Ctx *c) { Ctx *m = ui_ancestro(c, &ClsMenu); return aj.menu_estilo == M3_MEDIOS && m && ui_raiz() == m && sh_pantalla_estado() != EN_APAGADA; }
static void dormir(Ctx *c, MediosE *s) { if (s->activo) { ui_cancelar(c, md_tic); s->activo = 0; } }
static void despertar(Ctx *c, MediosE *s) { if (s->activo) return; s->activo = 1; s->t_ult = hw_ms(); ui_cada(c, 33, md_tic); }
static int perseguir(i32 *v, i32 obj, int dt)                  /* se acerca en ~150 ms, despacio al final */
{
    i32 d = obj - *v, paso;
    if (!d) return 0;
    paso = d * dt / 110;
    if (!paso) paso = d > 0 ? 1 : -1;
    if ((d > 0 && paso > d) || (d < 0 && paso < d)) paso = d;
    if (d > 0 && paso < 6 && d > 6) paso = 6;
    if (d < 0 && paso > -6 && d < -6) paso = -6;
    *v += paso;
    return 1;
}
static void md_tic(Ctx *c)
{
    MediosE *s = ui_estado(c);
    const u32 t = hw_ms();
    int dt = (int)(t - s->t_ult), cambio = 0;
    s->t_ult = t;
    if (dt < 1) dt = 1;
    if (dt > 200) dt = 200;
    if (!visible(c)) { dormir(c, s); return; }
    if (md.n_cat) {
        if (perseguir(&s->pos_c, (i32)s->cat * 256, dt)) cambio = 1;
        if (perseguir(&s->pos_i, (i32)md.c[s->cat].sel * 256, dt)) cambio = 1;
    }
    if (aj.menu_efecto != SH_EFECTO_QUIETO) {                  /* las partículas, la ola y el mecerse del foco: */
        s->pend = (u16)(s->pend + dt);
        if (cambio || s->pend >= 62) {                          /* con una transición, a cada tic; si no, uno de cada dos (~15/s): un
                                                                 * cuadro entero cuesta ~37 ms [medido en el emulador: 77 000 cuentas] */
            s->t += s->pend;
            s->fase = (u16)((s->fase + (u32)s->pend * 4096u / 2800u) & 4095);
            s->pend = 0;
            cambio = 1;
        }
        if (cambio) ui_sucio(c);
        return;                                                 /* el temporizador sigue: hay algo que animar */
    }
    if (cambio) ui_sucio(c);
    else dormir(c, s);                                          /* quieto y sin transición: ni un temporizador */
}
static void abrir_item(Ctx *c, MediosE *s)
{
    const MdItem *it;
    (void)c;
    if (!md.n_cat) return;
    it = &md.c[s->cat].it[md.c[s->cat].sel];
    if (it->app >= 0) { sh_abrir_app(it->app); return; }
    if (it->juego >= 0) { Widget *w = cg_abrir_ruta(md.jruta[(int)it->juego]); if (w) ui_empujar(w); return; }
    ui_aviso("Juegos", "No hay juegos en la tarjeta de memoria. Copia sus ficheros .sen a la carpeta «juegos» de la tarjeta.");
}
static int md_tecla(Ctx *c, int k, void *dato)
{
    static const u8 atajo[12] = { K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_AST, K_0, K_ALM };
    Ctx *m = ui_ancestro(c, &ClsMedios);
    MediosE *s = ui_estado(m);
    int i;
    for (i = 0; i < 12 && i < sh_n_apps; i++) if (k == atajo[i]) { sh_abrir_app(i); return 1; }   /* los atajos de siempre */
    if (!md.n_cat) return sh_teclas(c, k, dato);
    if (k == K_IZQ || k == K_DER) {
        const int n = k == K_DER ? s->cat + 1 : s->cat - 1;
        if (n >= 0 && n < md.n_cat) { s->cat = (u8)n; s->pos_i = (i32)md.c[n].sel * 256; sh_luces_latido(); }
        despertar(m, s); ui_sucio(m);
        return 1;
    }
    if (k == K_ARRIBA || k == K_ABAJO) {
        MdCat *ct = &md.c[s->cat];
        const int n = k == K_ABAJO ? ct->sel + 1 : ct->sel - 1;
        if (n >= 0 && n < ct->n) { ct->sel = (u8)n; sh_luces_latido(); }
        despertar(m, s); ui_sucio(m);
        return 1;
    }
    if (k == K_OK || k == K_SEL_IZQ) { abrir_item(m, s); return 1; }
    return sh_teclas(c, k, dato);
}
static TemaDatos tema_md;
static Widget *md_construir(Ctx *c)
{
    MediosE *s = ui_estado(c);
    const M3Tinta *tinta = m3_tinta(M3_MEDIOS);
    Widget *teclas;
    if (md.gen != sh_apps_gen || !md.gen) armar();
    if (!s->empezado || s->cat >= md.n_cat) {
        s->empezado = 1; s->cat = md.cat_ini < md.n_cat ? md.cat_ini : 0;
        s->pos_c = (i32)s->cat * 256; s->pos_i = md.n_cat ? (i32)md.c[s->cat].sel * 256 : 0;
    }
    m3_medios_color(sh_medios_tono());                         /* (28-oct) el color de Medios de ahora */
    m3_fondo_preparar(M3_MEDIOS, 0, (Rect){ 0, 0, (i16)UI_ANCHO, 0 }, 0);
    if (!s->activo && ui_raiz() == ui_ancestro(c, &ClsMenu)) despertar(c, s);
    tema_md.fondo = RGB(0, 0, 0); tema_md.texto = tinta->texto; tema_md.tenue = tinta->tenue; tema_md.acento = tinta->titulo;
    tema_md.sobre_acento = tinta->sombra; tema_md.barra = RGB(0, 0, 0);
    if (ui_giro() & 1) {                                        /* (giro) de lado: las teclas en columna, junto a las físicas */
        const char *arriba = ui_giro() == UI_GIRO_DER ? "Atrás" : "Abrir", *abajo = ui_giro() == UI_GIRO_DER ? "Abrir" : "Atrás";
        teclas = Caja(.an = DIS_ANCHO_COL_TECLAS, .al = INF, .hijo = RellenoXY(2, 8, Columna(.principal = P_ENTRE, .cruzado = P_CENTRO, .hijos = HIJOS(
            Texto(arriba, .marcas = F_ESTILO(DIS_T_PEQUENA)), Texto(abajo, .marcas = F_ESTILO(DIS_T_PEQUENA))))));
    } else teclas = Caja(.an = INF, .al = (i16)dis_tokens()->m.alto_barra_teclas, .hijo = RellenoXY(8, 0, Fila(.cruzado = P_CENTRO, .hijos = HIJOS(
        Texto(ui_giro() == UI_GIRO_180 ? "Atrás" : "Abrir", .marcas = F_ESTILO(DIS_T_ETIQUETA)), Hueco(),
        Texto(ui_giro() == UI_GIRO_180 ? "Abrir" : "Atrás", .marcas = F_ESTILO(DIS_T_ETIQUETA))))));
    return Teclas(md_tecla, Tema(&tema_md, ui_nuevo(&(MdLienzo){ .base = { &ClsMdLienzo, 0 },
        .pos_c = s->pos_c, .pos_i = s->pos_i, .t = s->t, .fase = s->fase, .version = md.version, .cat = s->cat, .n_cat = md.n_cat,
        .hijos = HIJOS(dis_con_teclas(Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
            Caja(.an = INF, .al = 22, .hijo = RellenoXY(6, 3, Fila(.cruzado = P_INICIO, .hijos = HIJOS(Hueco(), sh_reloj_barra(&tema_md))))),
            Hueco())), teclas)) }, sizeof(MdLienzo))));
}
static void md_desechar(Ctx *c) { dormir(c, ui_estado(c)); }
COMPONENTE_CON_ESTADO(Medios, MediosE, md_construir, NULL, .desechar = md_desechar);
Widget *sh_menu_medios(void) { return UI_W(Medios); }
void sh_medios_preparar(void) { m3_medios_color(sh_medios_tono()); m3_fondo_preparar(M3_MEDIOS, 0, (Rect){ 0, 0, (i16)UI_ANCHO, 0 }, 0); }

/* ---- (28-oct) EL FONDO DE MEDIOS, SOLO: el fondo animado del standby (Ajustes -> Fondo -> «Medios (animado)»; se pone solo al elegir el
 * menú de Medios). Pedido del mantenedor: «que las partículas se puedan poner como fondo de pantalla». Las MISMAS capas que el menú (el fondo,
 * la ola y las partículas, con el mismo color) y el mismo ritmo (~15 cuadros/s), sin iconos ni textos; lo de delante (el reloj) va de
 * hijo, como en el menú. El temporizador lo lleva el standby (shell/shell.c): solo anima mientras está arriba y la pantalla encendida;
 * con el efecto «quieto», un cuadro fijo. Cuesta lo que el menú quieto: un cuadro ~37 ms [medido en el emulador para el menú entero]. */
typedef struct { Widget base; Widget **hijos; u32 t; u8 color; } MfLienzo;
static void mf_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const MfLienzo *w = (const MfLienzo *)e->w;
    Rect r = { (i16)x, (i16)y, e->an, e->al };
    tintes();
    m3_fondo_pintar(l, r, 0, 0);
    ola(l, l->recorte, w->t);
    particulas(l, l->recorte, w->t);
}
static int mf_actualizar(Elem *e, const Widget *nv)
{
    MfLienzo *a = (MfLienzo *)e->w; const MfLienzo *b = (const MfLienzo *)nv;
    const int c = a->t != b->t || a->color != b->color ? C_PINTAR : C_NADA;
    *a = *b;
    return c;
}
static const Clase ClsMfLienzo = { .nombre = "MfLienzo", .tipo = T_RENDER, .hijos = H_N, .tam = sizeof(MfLienzo), .actualizar = mf_actualizar,
                                   .disponer = lienzo_disponer, .pintar = mf_pintar, .opaco = lienzo_opaco };
/* El elemento de pintado con «encima» de hijo y el reloj de la animación t (ms). Quien lo pone (el standby) lleva el temporizador y lo
 * vuelve a construir en cada cuadro, CON su hijo: un componente con estado propio que se rehiciera solo guardaría el hijo de una
 * construcción anterior, ya liberada [observado 28-oct en el PC: ASan, global-buffer-overflow en ui_nucleo.c crear]. */
Widget *sh_medios_fondo(Widget *encima, u32 t)
{
    m3_medios_color(sh_medios_tono());
    m3_fondo_preparar(M3_MEDIOS, 0, (Rect){ 0, 0, (i16)UI_ANCHO, 0 }, 0);
    return ui_nuevo(&(MfLienzo){ .base = { &ClsMfLienzo, 0 }, .t = t, .color = (u8)m3_medios_color_actual(), .hijos = HIJOS(encima) },
                    sizeof(MfLienzo));
}
