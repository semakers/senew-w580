/* La tabla de aplicaciones del menú (rejilla 3x4; los atajos 1..9 * 0 # siguen este orden).
 * (APX, 2026-10-14) Regla del mantenedor: ninguna app que no sea del sistema vive en el sistema. Las utilidades de fábrica
 * (Imágenes, Reloj, Notas, Calculadora, Teclas, Música, «Lo que falta», Movimiento) son .sen del W6FS (/apps,
 * hechas en los repositorios senew-app-*) y aquí solo queda SU SITIO: la disposición del menú de fábrica es cosa del sistema (una app
 * no decide dónde sale). Nombre e icono los da la cabecera del .sen; si el fichero no está o no vale, la celda no sale.
 * Las de casa de verdad (Archivos, Sistema, Ajustes) siguen enlazadas. Ningún JUEGO viene de fábrica: la Serpiente es
 * contenido del usuario (/memoria/apps) y sale con las demás descubiertas, detrás. docs/apps_externas.md.
 * Las apps EXTERNAS enlazadas (docs/sistema.md §11) se añaden solas al final: el enlazador deja sus descriptores
 * en la sección .apps_ext y sh_apps_init() los recorre. Como el menú es de SH_LZ_MAX celdas (más «Apps»), por cada externa se cae
 * la ÚLTIMA de casa.
 * (LANZ, 2026-10-16) el mantenedor: «esto no puede quedar automático, debe quedar a criterio del usuario desde Ajustes»: el menú
 * principal es el LANZADOR. Mientras el usuario no toque nada (aj.menu_propio = 0) sale EXACTAMENTE lo de siempre (la disposición
 * de fábrica y el orden automático de §28). En cuanto elige algo en Ajustes -> «Menú principal», el menú son SUS apps en SU orden,
 * guardadas por id (App.id: la ruta de la .sen o «@archivos», «@sistema», «@ajustes», «@ext:Nombre») en NV_MENU; Ajustes está
 * siempre y «Apps» va siempre la última. Una .sen nueva no entra sola (va a «Apps»); una elegida que ya no está se ignora. */
#include "shell.h"
#include "app_externa.h"
#include "m3d.h"
#include "cargador.h"

#define LZ_AJUSTES "@ajustes"
Widget *app_archivos(void);    Widget *app_sistema(void);   Widget *app_ajustes(void);

#define W5(nombre, icono, fich) { nombre, icono, 0, 0, .ruta = "/apps/" fich ".sen", .id = "/apps/" fich ".sen" }
static const App de_casa[] = {
    { "Archivos",    IC_ARCHIVOS, app_archivos,    0, .id = "@archivos" },
    W5("Imágenes",    IC_IMAGENES, "imagenes"),
    W5("Reloj",       IC_RELOJ,    "reloj"),
    W5("Notas",       IC_NOTAS,    "notas"),
    W5("Calculadora", IC_CALC,     "calculadora"),
    W5("Movimiento",  IC_MOVIMIENTO, "movimiento"),       /* (APX) la celda 6, la que dejó la Serpiente (un juego: ya no viene de fábrica).
                                                           * Así ninguna otra cambia de sitio ni de atajo; antes iba en la fila de más */
    /* (EST3, 26-oct) la Consola ya no es de fábrica (el mantenedor: «ahora mismo no tiene sentido; guárdala como está hecha, pero no la
     * pongas más en el sistema»): su código sigue en apps_ext/consola (se compila) y abre si el usuario la sube a /memoria/apps.
     * Las de detrás suben un sitio (y un atajo) */
    { "Juegos",      IC_JUEGO,    cg_abrir_juegos, 0, .id = "@juegos" },   /* (28-oct) en el sitio de Teclas (retirada: «ya no tiene sentido»,
                                                           * el mantenedor). Los juegos de la TARJETA (shell/cargador.c); su icono, un mando */
    { "Sistema",     IC_SISTEMA,  app_sistema,     0, .id = "@sistema" },
    { "Ajustes",     IC_AJUSTES,  app_ajustes,     0, .id = LZ_AJUSTES },   /* (LANZ) siempre en el menú: nadie se queda sin salida */
    W5("Música",      IC_MUSICA,   "musica"),             /* (28-oct) el mantenedor: «la app de música anterior debe desaparecer y solo quedar la de
                                                           * yt music, ahora nombrada Música», con SU icono (el disco con «reproducir»). Busca
                                                           * y toca por la red y lo descargado sin ella; ya es musica.sen (la de antes, walkman).
                                                           * El servicio (shell/musica.c) sigue aquí */
    W5("Lo que falta", IC_TELEFONO, "pendiente"),         /* pantalla honesta: qué hardware falta */
    /* (28-oct) las de la cámara y el micrófono ya son del sistema (vienen en el W6FS): con su celda, DETRÁS de las de siempre
     * (ninguna cambia de sitio ni de atajo; la Cámara se queda la «#»). Descubiertas, el orden automático de §28 las echaba a
     * «Apps» en cuanto el usuario subía otras apps a la tarjeta. Con ellas el menú de fábrica son 13 celdas (+ «Apps») */
    W5("Cámara",      IC_IMAGENES, "camara"),
    W5("Grabadora",   IC_NOTA,     "grabadora"),          /* la ÚLTIMA: es la que cae si hay externas enlazadas */
};
#define N_CASA ((int)(sizeof de_casa / sizeof de_casa[0]))
#define N_RUTAS N_CASA

App sh_apps[SH_MAX_APPS];
int sh_n_apps;
u32 sh_apps_gen;                                           /* (28-oct, Medios) sube cada vez que se arma el menú */
static struct { char nombre[30]; u16 bits[12]; Icono arte; } fab[N_RUTAS];   /* el nombre que trae la cabecera de cada .sen de fábrica y
                                                                             * (28-oct) su arte de 1 bit, si trae uno propio */

/* ¿Es una de las de la disposición? (el cargador no la vuelve a poner entre las descubiertas) */
/* De fábrica pero SIN celda: se abren desde otra pantalla del sistema (Luces, desde Ajustes) y no salen en el menú, como antes */
static const char *const sin_celda[] = { "/apps/luces.sen" };
/* (EST3) RETIRADAS: siguen en el W6FS de los aparatos ya grabados (/apps no se puede reescribir sin Far), pero ya no son del
 * sistema: no salen en el menú, ni en «Apps», ni en Ajustes -> «Menú principal». Una copia del usuario en /memoria/apps sí sale */
static const char *const retiradas[] = { "/apps/consola.sen", "/apps/walkman.sen", "/apps/teclas.sen" };   /* (28-oct) + la Música de antes y Teclas */
int sh_ruta_retirada(const char *ruta)
{
    int i;
    for (i = 0; i < (int)(sizeof retiradas / sizeof retiradas[0]); i++) if (!strcmp(retiradas[i], ruta)) return 1;
    return 0;
}
static int empieza(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
int sh_ruta_de_fabrica(const char *ruta)
{
    int i;
    if (empieza(ruta, HW_FS_MEMORIA "/apps/")) {   /* (EST3) la copia NUEVA de una de fábrica (cg_ruta_nueva) */
        const char *r = ruta + sizeof HW_FS_MEMORIA - 1;                      /* "/apps/x.sen" */
        if (sh_ruta_retirada(r)) return 0;                                   /* (la de una retirada es del usuario: sale) */
        ruta = r;
    }
    for (i = 0; i < (int)(sizeof sin_celda / sizeof sin_celda[0]); i++) if (!strcmp(sin_celda[i], ruta)) return 1;
    for (i = 0; i < N_CASA; i++) if (de_casa[i].ruta && !strcmp(de_casa[i].ruta, ruta)) return 1;
    return 0;
}
/* Una celda de la disposición: la de casa tal cual; la de fuera solo si su .sen está y vale (con su nombre e icono). */
static int poner(const App *a, int k)
{
    int ic;
    if (!a->ruta) {
        sh_apps[sh_n_apps] = *a;
        if (a->abrir == cg_abrir_juegos) { sh_apps[sh_n_apps].arte = cg_juegos_arte(); sh_apps[sh_n_apps].malla = cg_juegos_malla(); }   /* (28-oct) el mando */
        sh_n_apps++;
        return 1;
    }
    if (cg_de_fabrica(a->ruta, fab[k].nombre, sizeof fab[k].nombre, &ic)) return 0;
    sh_apps[sh_n_apps] = *a;
    sh_apps[sh_n_apps].nombre = fab[k].nombre;
    sh_apps[sh_n_apps].icono = ic;
    sh_apps[sh_n_apps].malla = cg_icono_fabrica(a->ruta, &fab[k].arte, fab[k].bits, 1);   /* (28-oct) la que trae icono propio (malla y arte): */
    if (fab[k].arte.w) sh_apps[sh_n_apps].arte = &fab[k].arte;                        /* YT Music no sale con la nota de Música */
    sh_n_apps++;
    return 1;
}

/* (LANZ) las externas ENLAZADAS (§11) también se pueden elegir: su id es «@ext:» y su nombre (las LZ_EXT primeras) */
#define LZ_EXT 4
static char ext_id[LZ_EXT][36];
static void poner_cad(char *d, size_t nd, const char *a, const char *b)     /* a y b, juntas y cortadas a nd - 1 */
{
    size_t n = 0;
    while (*a && n + 1 < nd) d[n++] = *a++;
    while (b && *b && n + 1 < nd) d[n++] = *b++;
    d[n] = 0;
}
static const char *ext_ident(int k, const AppExterna *e)
{
    if (k < 0 || k >= LZ_EXT) return NULL;
    poner_cad(ext_id[k], sizeof ext_id[k], "@ext:", e->nombre);
    return ext_id[k];
}
static App externa(const AppExterna *e, int k)
{
    return (App){ e->nombre, e->icono, 0, e->entrar,
                  .arte = sh_icono_externo(e->arte),            /* (M2) su icono 2D y su malla, */
                  .malla = m3_malla_externa(e->malla) + 1,      /* si los trae y valen */
                  .id = ext_ident(k, e) };
}

/* ---- (LANZ) la elección guardada: [versión 1][n][n x (largo, id sin cero)] en NV_MENU, y en RAM tal cual ---------------------- */
#define LZ_VERSION 1
#define LZ_ID 96                                                    /* = la ruta de una .sen descubierta (shell/cargador.c) */
static u8 lz_reg[2 + SH_LZ_MAX * LZ_ID];                            /* 1 346 B */
static u8 lz_leido;                                                 /* 0 sin mirar; 1 vale; 2 no hay o no vale */
static int lz_leer(void)                                            /* 1 = hay una elección guardada y vale */
{
    int n, i, p;
    if (lz_leido) return lz_leido == 1;
    n = hw_cargar(NV_MENU, lz_reg, sizeof lz_reg);
    lz_leido = 2;
    if (n < 2 || n > (int)sizeof lz_reg || lz_reg[0] != LZ_VERSION || lz_reg[1] > SH_LZ_MAX) return 0;
    for (i = 0, p = 2; i < lz_reg[1]; i++) { if (p >= n || !lz_reg[p] || lz_reg[p] >= LZ_ID || p + 1 + lz_reg[p] > n) return 0; p += 1 + lz_reg[p]; }
    if (p != n) return 0;
    lz_leido = 1;
    return 1;
}
static const u8 *lz_id(const u8 *p, char *id)                       /* copia el id de ese elemento (con su cero) y da el siguiente */
{
    memcpy(id, p + 1, p[0]); id[p[0]] = 0;
    return p + 1 + p[0];
}
static int ya_esta(const char *id) { int i; for (i = 0; i < sh_n_apps; i++) if (sh_apps[i].id && !strcmp(sh_apps[i].id, id)) return 1; return 0; }
/* Una celda por id: de casa (si su .sen está y vale), externa enlazada o .sen descubierta (a la celda *k de las del cargador). */
static int poner_id(const char *id, const AppExterna *const *ext, int n_ext, int *k)
{
    int i;
    if (ya_esta(id)) return 0;
    for (i = 0; i < N_CASA; i++) if (!strcmp(de_casa[i].id, id)) return poner(&de_casa[i], i);
    for (i = 0; i < n_ext && i < LZ_EXT; i++) if (!strcmp(ext_ident(i, ext[i]), id)) { sh_apps[sh_n_apps++] = externa(ext[i], i); return 1; }
    if (id[0] == '/' && *k < CG_CELDAS && !cg_menu_sen(&sh_apps[sh_n_apps], *k, id)) { sh_n_apps++; (*k)++; return 1; }
    return 0;                                                       /* ya no está (la borró, sacó la tarjeta) o no vale: se ignora */
}
static void menu_elegido(const AppExterna *const *ext, int n_ext)
{
    const u8 *p = lz_reg + 2;
    char id[LZ_ID];
    int i, k = 0, ajustes = 0;
    cg_menu_empezar();
    for (i = 0; i < lz_reg[1]; i++) {
        p = lz_id(p, id);
        if (!strcmp(id, LZ_AJUSTES)) ajustes = 1;
        else if (sh_n_apps >= SH_LZ_MAX - !ajustes) continue;       /* el sitio de Ajustes, guardado aunque venga detrás */
        poner_id(id, ext, n_ext, &k);
    }
    if (!ya_esta(LZ_AJUSTES)) poner_id(LZ_AJUSTES, ext, n_ext, &k); /* un registro sin Ajustes no deja a nadie sin salida */
    cg_menu_terminar();
    if (!hw_modo_seguro()) sh_apps[sh_n_apps++] = cg_celda_apps();  /* «Apps», siempre la última */
}

void sh_apps_init(void)
{
    const AppExterna *ext[SH_MAX_APPS];
    int n_ext = sh_apps_externas(ext, SH_MAX_APPS), i, k = 0;
    if (n_ext > SH_MAX_APPS - 1) n_ext = SH_MAX_APPS - 1;        /* siempre queda sitio para lo de casa */
    sh_n_apps = 0;
    sh_apps_gen++;
    cg_menu_empezar();                                           /* (28-oct) los huecos de malla, desde cero: las de casa también cogen */
    if (aj.menu_propio && lz_leer()) { menu_elegido(ext, n_ext); return; }   /* (LANZ) el que eligió el usuario */
    for (i = 0; i < N_CASA; i++, k++) poner(&de_casa[i], k);
    if (sh_n_apps + n_ext > SH_LZ_MAX) sh_n_apps = SH_LZ_MAX - n_ext;   /* (28-oct) el menú son SH_LZ_MAX celdas + «Apps» (antes 12: con 14 de
                                                                  * casa cortaba dos sin haber externas): por cada externa se cae la última */
    if (sh_n_apps < 1) sh_n_apps = 1;
    for (i = 0; i < n_ext; i++) sh_apps[sh_n_apps++] = externa(ext[i], i);
    sh_n_apps = cg_al_menu(sh_apps, sh_n_apps, 15);              /* (C) las apps CARGABLES descubiertas (W580i: hasta 15 celdas, la fila de más) (.sen de /apps, Mis archivos, la tarjeta, la Memoria) */
}

/* ---- (LANZ) la lista de la pantalla Ajustes -> «Menú principal» ----------------------------------------------------------------
 * TODAS las apps: las de la disposición de fábrica (las que están y valen), las externas enlazadas y las .sen descubiertas. Las del
 * menú van primero, en su orden; detrás las demás, en el orden de siempre. Los punteros de las .sen apuntan a lo que descubrió el
 * cargador (cg_app_info): valen mientras la pantalla esté abierta (nada vuelve a descubrir debajo de ella: el menú se rehace en el
 * standby o al cerrarla, y la lista «Apps» no se abre desde aquí). */
#define LZ_FILAS (N_CASA + LZ_EXT + CG_MAX)
static ShLzApp lz[LZ_FILAS];
static int lz_n;
static u8 lz_cambio;
static void lz_poner(const char *id, const char *nombre, const Icono *ic, int juego)
{
    if (lz_n < LZ_FILAS && id) lz[lz_n++] = (ShLzApp){ id, nombre, ic, (u8)juego, (u8)!strcmp(id, LZ_AJUSTES), 0, 0 };
}
int sh_lz_abrir(void)
{
    const AppExterna *ext[SH_MAX_APPS];
    int n_ext = sh_apps_externas(ext, SH_MAX_APPS), n_sen, i, j, k, ic, arriba = 0;
    CgInfo in;
    lz_n = 0; lz_cambio = 0;
    if (hw_modo_seguro()) return 0;                                  /* en modo seguro no se carga nada de fuera: el menú no se toca */
    for (i = 0; i < N_CASA; i++)
        if (!de_casa[i].ruta) lz_poner(de_casa[i].id, de_casa[i].nombre, de_casa[i].abrir == cg_abrir_juegos ? cg_juegos_arte() : &sh_iconos[de_casa[i].icono], 0);
        else if (!cg_de_fabrica(de_casa[i].ruta, fab[i].nombre, sizeof fab[i].nombre, &ic)) {
            cg_icono_fabrica(de_casa[i].ruta, &fab[i].arte, fab[i].bits, 0);         /* (28-oct) su arte propio, si lo trae */
            lz_poner(de_casa[i].id, fab[i].nombre, fab[i].arte.w ? &fab[i].arte : &sh_iconos[ic], 0);
        }
    for (i = 0; i < n_ext && i < LZ_EXT; i++) lz_poner(ext_ident(i, ext[i]), ext[i]->nombre, &sh_iconos[ext[i]->icono], 0);
    n_sen = cg_descubrir();
    for (i = 0; i < n_sen; i++) if (!cg_app_info(i, &in)) lz_poner(in.ruta, in.nombre, in.icono, in.juego);
    /* las del menú de ahora, arriba y en su orden (el resto conserva el suyo) */
    for (i = 0; i < sh_n_apps; i++) {
        if (!sh_apps[i].id) continue;
        for (j = arriba; j < lz_n && strcmp(lz[j].id, sh_apps[i].id); j++) ;
        if (j == lz_n) continue;                                     /* «Apps», o una que ya no está */
        { ShLzApp t = lz[j]; for (k = j; k > arriba; k--) lz[k] = lz[k - 1]; t.en_menu = 1; lz[arriba++] = t; }
    }
    return lz_n;
}
const ShLzApp *sh_lz_app(int i) { return i >= 0 && i < lz_n ? &lz[i] : NULL; }
int sh_lz_en_menu(void) { int i, n = 0; for (i = 0; i < lz_n; i++) n += lz[i].en_menu; return n; }
int sh_lz_celda(int i) { int j, n = 0; if (i < 0 || i >= lz_n || !lz[i].en_menu) return 0; for (j = 0; j <= i; j++) n += lz[j].en_menu; return n; }
static void lz_guardar(void)                                         /* cada cambio, en el acto (como el resto de Ajustes) */
{
    u8 *p = lz_reg + 2;
    int i, r;
    lz_reg[0] = LZ_VERSION; lz_reg[1] = 0;
    for (i = 0; i < lz_n; i++) {
        size_t n = strlen(lz[i].id);
        if (!lz[i].en_menu || !n || n >= LZ_ID || lz_reg[1] >= SH_LZ_MAX) continue;
        *p++ = (u8)n; memcpy(p, lz[i].id, n); p += n; lz_reg[1]++;
    }
    lz_leido = 1; lz_cambio = 1;
    r = hw_guardar(NV_MENU, lz_reg, (u32)(p - lz_reg));
    if (r != HW_OK) hw_log("[lanzador] la elección del menú NO se ha podido guardar: vale hasta apagar\n");
    aj.menu_propio = 1;
    sh_guardar_ajustes();                                            /* (dice él, una vez, si no se puede guardar) */
}
int sh_lz_marcar(int i)
{
    if (i < 0 || i >= lz_n) return -3;
    if (lz[i].fijo) return -1;
    if (!lz[i].en_menu && sh_lz_en_menu() >= SH_LZ_MAX) return -2;
    lz[i].en_menu = (u8)!lz[i].en_menu;
    lz_guardar();
    return 0;
}
int sh_lz_mover(int de, int a)
{
    ShLzApp t;
    if (de < 0 || a < 0 || de >= lz_n || a >= lz_n || de == a) return -1;
    t = lz[de]; lz[de] = lz[a]; lz[a] = t;
    if (lz[de].en_menu || lz[a].en_menu) lz_guardar();              /* el orden de las que no van no se guarda: no cambia nada */
    return 0;
}
void sh_lz_fabrica(void)
{
    aj.menu_propio = 0; lz_leido = 2;
    hw_olvidar(NV_MENU);
    sh_guardar_ajustes();
    sh_apps_init();                                                  /* el de fábrica, y la lista otra vez desde él */
    sh_lz_abrir();
    lz_cambio = 1;
}
void sh_lz_cerrar(void)
{
    lz_n = 0;
    if (!lz_cambio) return;
    lz_cambio = 0;
    sh_apps_init();
    ui_reconstruir_todo();
}
