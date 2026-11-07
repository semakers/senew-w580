/* menu3d.c — EL MENÚ PRINCIPAL (pista V, docs/menu3d.md).
 *
 * Una rejilla de 3 columnas con una celda por aplicación, en el mismo orden y con los mismos atajos (1..9 * 0 #)
 * de siempre. Según aj.menu_estilo:
 *   - CLÁSICO: el menú de antes, tal cual (iconos de 1 bit sobre la Pantalla del tema).
 *   - WALKMAN, CRISTAL, NEÓN: iconos 3D (shell/m3d_dibujo.c) sobre un fondo en paralaje, que se inclinan con el
 *     teléfono (shell/inclina.c) o con un vaivén propio; el icono con foco es mayor, lleva halo y se mece.
 *
 * CÓMO ENCAJA EN EL FRAMEWORK. Cada celda es un Enfocable (el foco y las flechas son los del framework) cuyo
 * hijo es un objeto de render propio, M3Celda, con TODO lo que decide sus píxeles en la configuración (M3Icono).
 * El estado vivo (inclinación, fase del mecerse, cuánto foco tiene cada celda, la apertura) está en el estado
 * del componente Menu y solo cambia en m3_tic(); el tic marca el menú para reconstruir, y al reconstruir el
 * framework compara la configuración de cada celda con la anterior y repinta SOLO las que cambian:
 *   - teléfono quieto: cambia la celda del foco (se mece, ~15 pasos por segundo) -> un rectángulo de daño de 80 x 55;
 *   - al inclinarlo: cambian todas y el fondo -> el cuadro entero;
 *   - efecto «quieto» y nada pendiente: el temporizador se quita; no hay NADA latiendo.
 * El temporizador (33 ms) solo existe mientras el menú es la ruta de arriba y la pantalla está encendida: tapado
 * por una app o a oscuras se cancela y suelta el sensor, así que el reposo profundo queda como estaba. */
#include "shell.h"
#include "m3d.h"
#include "r3d.h"

/* (W600, el mantenedor: «el menú 3D en el 600 se ve incompleto») la celda sale de la PANTALLA: 55 px en el W580 (caben las 5 filas,
 * como siempre) y 43 en el W600 (176x220: caben 4 y la rejilla se DESPLAZA al bajar a la 5.ª). Los iconos, en proporción. */
/* (estética, 26-oct) con la letra nueva las medidas salen de ella: el título en el estilo «título» (+1 de sombra + 8), las teclas con
 * el alto de las softkeys del diseño, y 4 filas en el W580 (3 en el W600) que llenan lo que queda; la rejilla se desplaza al bajar
 * (lo que ya hacía el W600). Antes: título 28, teclas 17 y celdas de 55 (5 filas, todo a la vista). */
static int m3_alto_titulo(void) { return dis_alto_texto(1, F_ESTILO(DIS_T_TITULO), NULL) + 1 + 8; }
#define M3_ALTO_TITULO m3_alto_titulo()
#define M3_ALTO_TECLAS (dis_tokens()->m.alto_barra_teclas)
#define M3_FILAS (UI_ALTO >= 300 ? 4 : 3)
static int m3_alto_celda(void) { int a = (UI_ALTO - M3_ALTO_TITULO - M3_ALTO_TECLAS) / M3_FILAS; return a > 64 ? 64 : a; }   /* 64: el búfer del icono (m3d_dibujo.c, 80x64) */
#define M3_ALTO_CELDA m3_alto_celda()
#define RADIO_REPOSO ((18 * 256 + 128) * M3_ALTO_CELDA / 55)     /* lo que mide 1.0 de la malla: 18,5 px sin foco, */
#define RADIO_FOCO   ((23 * 256) * M3_ALTO_CELDA / 55)           /* 23 con foco */
#define RADIO_ABRIR  ((25 * 256) * M3_ALTO_CELDA / 55)           /* y 25 mientras se abre */

typedef struct { Widget base; } Menu;
typedef struct {
    int sel;
    /* ---- lo del menú 3D ---- */
    u8  activo;                  /* el temporizador está en marcha */
    u8  inclina;                 /* tenemos abierto el servicio de inclinación */
    u8  abriendo;                /* 1 + la app que se está abriendo (0 = ninguna) */
    u8  etapa;                   /* de esa apertura: A_SUBE (el icono se viene), A_ARRIBA (la app está abierta), A_VUELVE */
    u8  t_abrir;                 /* 0..255: la animación de abrir (sube) y la de volver (baja) */
    u8  gen_fondo;               /* sube cuando la imagen del fondo se rehace */
    u8  foco[SH_MAX_APPS];       /* 0..255: cuánto foco tiene cada celda (persigue al de verdad) */
    i16 tx, ty;                  /* la inclinación que se pinta */
    u16 fase;                    /* la fase del mecerse del foco, 0..4095 */
    u32 t_ult;
    u32 t_mov;                   /* hw_ms() de la última vez que cambió la inclinación */
    u32 t_mecer;                 /* y del último paso del mecerse */
    u8  moviendo;                /* la inclinación está cambiando: los iconos sin foco van «bastos» */
    u8  reenfocar;               /* hay pedido un «devuélvele el foco a la celda sel» */
    u8  fila0;                   /* (W600) la primera fila de la rejilla 3D que se ve: con pantalla baja no caben todas */
    u8  enfocar_sel;             /* (W600) tras desplazar: dar el foco a la celda sel en cuanto exista (la pide m3_construir) */
} MenuE;
extern const Clase ClsMenu;
enum { A_SUBE, A_ARRIBA, A_VUELVE };

static void menu_enfocada(Ctx *c, void *dato)
{
    Ctx *m = ui_ancestro(c, &ClsMenu);
    ((MenuE *)ui_estado(m))->sel = (int)(size_t)dato;
    sh_luces_latido();                                     /* (S5) las luces laterales laten al mover el foco */
    ui_sucio(m);
}
static int es3d(void) { return aj.menu_estilo != M3_CLASICO && aj.menu_estilo != M3_MEDIOS && aj.menu_estilo != M3_ESCRITORIO && aj.menu_estilo < M3_ESTILOS; }   /* (28-oct) Medios: otro menú (menu_medios.c) */

/* Abrir la app i. En 3D, con el menú animando, primero el icono se viene hacia delante (120 ms) y la abre el
 * tic; si no, en el acto (como siempre). */
static void abrir(Ctx *m, int i)
{
    MenuE *s = ui_estado(m);
    if (i < 0 || i >= sh_n_apps) return;
    if (!es3d()) { s->abriendo = 0; s->t_abrir = 0; sh_abrir_app(i); return; }   /* clásico: en el acto, como siempre */
    if (s->abriendo && s->etapa == A_SUBE) return;               /* ya hay una abriéndose */
    if (s->activo) { s->abriendo = (u8)(i + 1); s->etapa = A_SUBE; ui_sucio(m); }   /* (si otra estaba volviendo, se corta) */
    else { s->abriendo = 0; s->t_abrir = 0; sh_abrir_app(i); }
}
static void menu_abrir(Ctx *c, void *dato) { abrir(ui_ancestro(c, &ClsMenu), (int)(size_t)dato); }
static Elem *celda_de(Elem *e, u32 clave);
static void menu_enfocar_sel(Ctx *c) { MenuE *s = ui_estado(c); Elem *e = celda_de(c, (u32)s->sel + 1); if (e) ui_enfocar(e); }
static int menu_tecla(Ctx *c, int k, void *dato)
{
    static const u8 atajo[12] = { K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_AST, K_0, K_ALM };
    Ctx *m = ui_ancestro(c, &ClsMenu); MenuE *s = ui_estado(m);
    int i;
    if (s->abriendo && s->etapa == A_SUBE) return 1;             /* a medio abrir: esa tecla no es para nadie */
    for (i = 0; i < 12 && i < sh_n_apps; i++) if (k == atajo[i]) { abrir(m, i); return 1; }   /* 1..9 * 0 # = abrir directo */
    if (k == K_WEB && es3d()) { sh_inclina_centrar(); return 1; }   /* «Actividad»: plano = la postura de ahora */
    if (es3d() && (k == K_ABAJO || k == K_ARRIBA)) {               /* (W600) en el borde de lo que se ve, la rejilla se desplaza */
        const int n = sh_n_apps < 15 ? sh_n_apps : 15, fila = s->sel / M3_COLS;
        int nueva = -1;
        if (k == K_ABAJO && fila == s->fila0 + M3_FILAS - 1 && (fila + 1) * M3_COLS < n) nueva = s->sel + M3_COLS < n ? s->sel + M3_COLS : n - 1;
        if (k == K_ARRIBA && fila == s->fila0 && s->fila0 > 0) nueva = s->sel - M3_COLS;
        if (nueva >= 0) { s->sel = nueva; s->enfocar_sel = 1; ui_sucio(m); sh_luces_latido(); return 1; }
    }
    return sh_teclas(c, k, dato);
}

/* ---- el menú CLÁSICO: el de antes de la pista V, sin tocar ---------------------------------------------------- */
/* (MD) la celda de la REJILLA la dibuja el diseño en uso (DisenoTabla.celda_menu).
 * PUNTO DE ENGANCHE DEL ICONO PROPIO (pista ICO): aquí se decide QUÉ icono lleva la celda de una app: hoy su icono 2D propio
 * (App.arte, de la .sen) o, si no trae, el genérico (sh_iconos[App.icono], que el diseño pinta con su símbolo). Un icono propio
 * nuevo (plano o malla) se elige AQUÍ y se le pasa a celda_menu; el diseño solo lo enmarca. */
static const Icono *icono_de_app(const App *a) { return a->arte ? a->arte : &sh_iconos[a->icono]; }
static Widget *menu_celda(Ctx *c, const Foco *f, void *dato)
{
    const App *a = &sh_apps[(size_t)dato];
    (void)c;
    return dis_actual()->celda_menu(icono_de_app(a), a->nombre, f->enfocado, f->t);
}
static Widget *clasico_construir(MenuE *s)
{
    Widget *celdas[16];
    size_t i;
    for (i = 0; i < (size_t)sh_n_apps && i < 15; i++)
        celdas[i] = Enfocable(CLAVE((u32)i + 1), .constructor = menu_celda, .dato = (void *)i, .al_activar = menu_abrir,
                              .al_enfocar = menu_enfocada, .anim_ms = 120, .autofoco = i == 0);
    return Teclas(menu_tecla, Pantalla(sh_apps[s->sel].nombre,
        Relleno(3, Lista(.hijos = HIJOS(Rejilla(.columnas = (u8)M3_COLS, .alto_celda = dis_tokens()->m.alto_celda_menu, .hijos = ui_lista(celdas, i))))),   /* (estética) se desplaza: con la letra nueva caben 4 filas */
        "Abrir", "Atrás"));
}

/* ---- los dos objetos de render del menú 3D -------------------------------------------------------------------- */
typedef struct { Widget base; M3Icono ic; } M3Celda;
static void celda_disponer(Elem *e, Restr r) { e->an = r.maxw < INF ? r.maxw : 80; e->al = r.maxh < INF ? r.maxh : 55; }
static void celda_pintar(Elem *e, Lienzo *l, int x, int y)
{
    Rect r = { (i16)x, (i16)y, e->an, e->al };
    m3_icono_pintar(l, r, &((const M3Celda *)e->w)->ic);
}
static int celda_actualizar(Elem *e, const Widget *nv)
{
    M3Celda *a = (M3Celda *)e->w; const M3Celda *b = (const M3Celda *)nv;
    int c = memcmp(&a->ic, &b->ic, sizeof a->ic) ? C_PINTAR : C_NADA;
    a->ic = b->ic;
    return c;
}
static const Clase ClsM3Celda = { .nombre = "M3Celda", .tipo = T_RENDER, .tam = sizeof(M3Celda), .actualizar = celda_actualizar,
                                  .disponer = celda_disponer, .pintar = celda_pintar };

/* El fondo ES la raíz de la pantalla: pinta la imagen desplazada y encima van sus hijos (la columna con el
 * título, la rejilla y las teclas). Es opaco: el framework se ahorra el borrado a negro de lo dañado. */
typedef struct { Widget base; Widget **hijos; i16 ox, oy; u8 estilo, gen; } M3Fondo;
static void fondo_disponer(Elem *e, Restr r)
{
    unsigned i;
    e->an = r.maxw < INF ? r.maxw : UI_ANCHO; e->al = r.maxh < INF ? r.maxh : UI_ALTO;
    for (i = 0; i < e->n; i++) {
        Elem *h = ui_hijo(e, i);
        Restr c = { e->an, e->an, e->al, e->al };
        if (h) { ui_disponer(h, c); h->x = h->y = 0; }
    }
}
static void fondo_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const M3Fondo *w = (const M3Fondo *)e->w;
    Rect r = { (i16)x, (i16)y, e->an, e->al };
    m3_fondo_pintar(l, r, w->ox, w->oy);
}
static int fondo_actualizar(Elem *e, const Widget *nv)
{
    M3Fondo *a = (M3Fondo *)e->w; const M3Fondo *b = (const M3Fondo *)nv;
    int c = (a->ox != b->ox || a->oy != b->oy || a->estilo != b->estilo || a->gen != b->gen) ? C_PINTAR : C_NADA;
    *a = *b;
    return c;
}
static int fondo_opaco(Elem *e) { (void)e; return 1; }
static const Clase ClsM3Fondo = { .nombre = "M3Fondo", .tipo = T_RENDER, .hijos = H_N, .tam = sizeof(M3Fondo), .actualizar = fondo_actualizar,
                                  .disponer = fondo_disponer, .pintar = fondo_pintar, .opaco = fondo_opaco };

/* ---- geometría y pose del menú 3D ------------------------------------------------------------------------------ */
#define POSE_GUINADA (-182)               /* la pose de reposo: tres cuartos (-16°, +11°). De frente no se ve el canto */
#define POSE_CABECEO 125
#define MECER_GUINADA 205                 /* el foco se mece ±18° */
#define MECER_CABECEO 46                  /* y cabecea ±4° al doble de ritmo */
#define MECER_MS 2600
#define FOCO_MS 140
#define ABRIR_MS 120
#define QUIETO_MS 160                 /* sin cambiar la inclinación este rato: se vuelve a pintar fino */
#define MECER_PASO_MS 48
#define RAPIDO_MS 110

static int suave(int t) { return (t * t * (765 - 2 * t)) / 65025; }      /* 0..255 -> 0..255, entra y sale despacio */

/* Lo que se pinta en la celda i, a partir del estado: una función pura (ver la regla de m3d.h). */
static M3Icono icono_de(const MenuE *s, int i)
{
    M3Icono ic;
    const int f = suave(s->foco[i]), ab = suave(s->t_abrir), esta = s->abriendo == i + 1;
    int radio = RADIO_REPOSO + (RADIO_FOCO - RADIO_REPOSO) * f / 255, gui = POSE_GUINADA + s->tx, cab = POSE_CABECEO + s->ty;
    int luz = 255, cerca = 256 + f / 2;                       /* el que tiene el foco está más cerca: se desplaza más */
    memset(&ic, 0, sizeof ic);
    if (f && aj.menu_efecto != SH_EFECTO_QUIETO) {
        gui += (int)(((i32)r3d_sen(s->fase) * MECER_GUINADA) >> 14) * f / 255;
        cab += (int)(((i32)r3d_sen((u32)s->fase * 2 + 1024) * MECER_CABECEO) >> 14) * f / 255;
        radio += (int)(((i32)r3d_sen((u32)s->fase * 2) * 160) >> 14) * f / 255;      /* respira: ±0,6 px */
    }
    if (s->abriendo) {
        if (esta) { radio += (RADIO_ABRIR - radio) * ab / 255; gui += (0 - gui) * ab / 255; cab += (0 - cab) * ab / 255; }
        else luz = 255 - 170 * ab / 255;                      /* los demás se apagan */
    }
    ic.malla = (u8)(sh_apps[i].malla ? sh_apps[i].malla - 1 : m3_malla_de_icono(sh_apps[i].icono)); ic.estilo = aj.menu_estilo;
    ic.foco = (u8)(esta ? (f > ab ? f : ab) : s->abriendo ? f * (255 - ab) / 255 : f);
    ic.luz = (u8)luz;
    ic.fino = (u8)(!s->moviendo || f > 128 || esta);          /* el del foco, siempre fino: es donde se mira */
    ic.guinada = (i16)gui; ic.cabeceo = (i16)cab;
    ic.dx = (i16)(s->tx * 3 * 16 * cerca / (SH_INCLINA_MAX * 256));       /* ±3 px (±4,5 el del foco) al tope */
    ic.dy = (i16)(s->ty * 3 * 16 * cerca / (SH_INCLINA_MAX * 256));
    ic.radio = (u16)radio;
    return ic;
}

static Widget *m3_celda(Ctx *c, const Foco *f, void *dato)
{
    const MenuE *s = ui_estado(ui_ancestro(c, &ClsMenu));
    M3Celda w = { .base = { &ClsM3Celda, 0 }, .ic = icono_de(s, (int)(size_t)dato) };
    (void)f;
    return ui_nuevo(&w, sizeof w);
}

/* ---- el tic: lo único que cambia el estado ---------------------------------------------------------------------- */
static int visible(Ctx *c) { return es3d() && ui_raiz() == c && sh_pantalla_estado() != EN_APAGADA; }
static void m3_tic(Ctx *c);
static void dormir(Ctx *c, MenuE *s)
{
    if (s->inclina) { sh_inclina_cerrar(); s->inclina = 0; }
    if (s->activo) { ui_cancelar(c, m3_tic); s->activo = 0; }
}
static void despertar(Ctx *c, MenuE *s)
{
    if (s->activo) return;
    s->activo = 1; s->t_ult = hw_ms();
    ui_cada(c, 33, m3_tic);
}
static int hacia(u8 *v, int objetivo, int paso)                /* 1 si se ha movido */
{
    int a = *v;
    if (a == objetivo) return 0;
    if (a < objetivo) { a += paso; if (a > objetivo) a = objetivo; }
    else { a -= paso; if (a < objetivo) a = objetivo; }
    *v = (u8)a;
    return 1;
}
static void m3_tic(Ctx *c)
{
    MenuE *s = ui_estado(c);
    const u32 t = hw_ms();
    int dt = (int)(t - s->t_ult), cambio = 0, pendiente = 0, i;
    i16 tx, ty;
    s->t_ult = t;
    if (dt < 1) dt = 1;
    if (dt > 200) dt = 200;
    if (!visible(c)) {                                     /* tapado por una app, a oscuras, o en clásico: a dormir */
        if (s->abriendo && s->etapa == A_SUBE) { s->abriendo = 0; s->t_abrir = 0; }   /* (lo que estuviera a medio abrir se olvida) */
        dormir(c, s);
        return;
    }
    if (!s->inclina) {                                     /* se nos ve (otra vez): «plano» es la postura de ahora */
        sh_inclina_abrir(); s->inclina = 1;
    }
    if (s->abriendo && s->etapa == A_ARRIBA) s->etapa = A_VUELVE;   /* se nos ve y la app ya no está: la animación, al revés */
    if (sh_inclina_paso(dt, &tx, &ty)) {
        /* Dos cambios seguidos en menos de RAPIDO_MS = se está inclinando deprisa: los iconos sin foco van bastos.
         * Un cambio suelto (una deriva lenta, el vaivén) se pinta fino: despacio es cuando se verían los dientes. */
        if (aj.menu_efecto == SH_EFECTO_INCLINA && t - s->t_mov < RAPIDO_MS) s->moviendo = 1;
        s->tx = tx; s->ty = ty; cambio = 1; s->t_mov = t;
        if (aj.menu_efecto == SH_EFECTO_INCLINA) sh_luces_inclina(tx, ty);   /* (S5) y al inclinar (no con el vaivén, que se mueve solo) */
    }
    else if (s->moviendo && t - s->t_mov >= QUIETO_MS) { s->moviendo = 0; cambio = 1; }      /* se ha parado: un cuadro entero, fino */
    if (s->moviendo) pendiente = 1;
    for (i = 0; i < sh_n_apps && i < SH_MAX_APPS; i++) {
        const int obj = (s->abriendo ? s->abriendo - 1 == i : s->sel == i) ? 255 : 0;
        if (hacia(&s->foco[i], obj, dt * 255 / FOCO_MS + 1)) cambio = 1;
        if (s->foco[i] != obj) pendiente = 1;
    }
    if (aj.menu_efecto != SH_EFECTO_QUIETO) {              /* el foco se mece (solo cambia SU celda) */
        if (t - s->t_mecer >= MECER_PASO_MS) {             /* un tic sí y otro no (~15 por segundo): es UN icono fino entero cada vez */
            s->fase = (u16)((s->fase + (t - s->t_mecer > 200 ? 200 : t - s->t_mecer) * 4096u / MECER_MS) & 4095);
            s->t_mecer = t; cambio = 1;
        }
        pendiente = 1;
    }
    if (s->abriendo) {
        pendiente = 1;
        if (s->etapa == A_SUBE) {
            if (hacia(&s->t_abrir, 255, dt * 255 / ABRIR_MS + 1)) cambio = 1;
            if (s->t_abrir == 255) {
                s->etapa = A_ARRIBA;
                ui_sucio(c);
                sh_abrir_app(s->abriendo - 1);             /* una app de casa: empuja su ruta y el tic siguiente nos duerme. Una
                                                            * EXTERNA no vuelve de aquí hasta que el usuario sale de ella */
                s->t_ult = hw_ms();
                return;
            }
        } else if (s->etapa == A_VUELVE) {
            if (hacia(&s->t_abrir, 0, dt * 255 / ABRIR_MS + 1)) cambio = 1;
            if (!s->t_abrir) s->abriendo = 0;
        }
    }
    if (cambio) ui_sucio(c);
    if (!pendiente && aj.menu_efecto == SH_EFECTO_QUIETO) dormir(c, s);     /* nada que animar: ni un temporizador */
}

/* ---- construir -------------------------------------------------------------------------------------------------- */
static TemaDatos temas3d[M3_ESTILOS];
static Widget *m3_construir(Ctx *c, MenuE *s)
{
    const int estilo = aj.menu_estilo;
    const M3Tinta *tinta = m3_tinta(estilo);
    TemaDatos *t = &temas3d[estilo];
    Rect rej = { (i16)(ui_giro() == UI_GIRO_IZQ ? dis_teclas_columna() : 0), (i16)(M3_ALTO_TITULO + (ui_giro() == UI_GIRO_180 ? M3_ALTO_TECLAS : 0)),
                 (i16)(UI_ANCHO - dis_teclas_columna()), 0 };   /* (giro) la rejilla, al lado de la columna de las teclas (o debajo de su barra) */
    Widget *celdas[SH_MAX_APPS + 1], *teclas;
    size_t i, n = (size_t)(sh_n_apps < 15 ? sh_n_apps : 15), i0, i1;
    if (s->sel >= (int)n) s->sel = 0;
    {   /* (giro) con otras columnas la fila de arriba puede no valer: nunca más abajo de la última que deja la rejilla LLENA (de pie, bajado
         * hasta el final, y girado: se quedaba una fila más abajo, con un hueco vacío debajo) */
        int ft = ((int)n + M3_COLS - 1) / M3_COLS, mx = ft > M3_FILAS ? ft - M3_FILAS : 0;
        if (s->fila0 > mx) s->fila0 = (u8)mx;
    }
    if (s->sel / M3_COLS < s->fila0) s->fila0 = (u8)(s->sel / M3_COLS);           /* (W600) que la fila del foco se vea */
    if (s->sel / M3_COLS >= s->fila0 + M3_FILAS) s->fila0 = (u8)(s->sel / M3_COLS - M3_FILAS + 1);
    i0 = (size_t)s->fila0 * M3_COLS; i1 = i0 + (size_t)M3_FILAS * M3_COLS; if (i1 > n) i1 = n;
    t->fondo = RGB(0, 0, 0); t->texto = tinta->texto; t->tenue = tinta->tenue; t->acento = tinta->titulo;
    t->sobre_acento = tinta->sombra; t->barra = RGB(0, 0, 0);
    rej.h = (i16)(((i1 - i0 + M3_COLS - 1) / M3_COLS) * M3_ALTO_CELDA);
    if (m3_fondo_preparar(estilo, (int)(i1 - i0), rej, M3_ALTO_CELDA)) s->gen_fondo++;
    if (!s->activo && (ui_raiz() == c || !s->t_ult)) {      /* se nos reconstruye estando parados: una tecla, la vuelta de una
                                                             * app, la pantalla que se enciende… (o es el primer montaje) */
        if (!s->t_ult) { s->foco[s->sel] = 255; s->t_ult = 1; }      /* el primer cuadro ya sale con su foco */
        despertar(c, s);
    }
    for (i = i0; i < i1; i++)
        celdas[i - i0] = Enfocable(CLAVE((u32)i + 1), .constructor = m3_celda, .dato = (void *)i, .al_activar = menu_abrir,
                                   .al_enfocar = menu_enfocada, .autofoco = i == 0);
    if (ui_giro() & 1) {                                 /* (giro) de lado: las teclas en columna, junto a las físicas (ui/diseno.h) */
        const char *arriba = ui_giro() == UI_GIRO_DER ? "Atrás" : "Abrir", *abajo = ui_giro() == UI_GIRO_DER ? "Abrir" : "Atrás";
        teclas = Caja(.an = DIS_ANCHO_COL_TECLAS, .al = INF, .hijo = RellenoXY(2, 8, Columna(.principal = P_ENTRE, .cruzado = P_CENTRO, .hijos = HIJOS(
            Texto(arriba, .marcas = F_ESTILO(DIS_T_PEQUENA)), Texto(abajo, .marcas = F_ESTILO(DIS_T_PEQUENA))))));
    } else teclas = Caja(.an = INF, .al = (i16)M3_ALTO_TECLAS, .hijo = RellenoXY(8, 0, Fila(.cruzado = P_CENTRO, .hijos = HIJOS(   /* (estética) como las softkeys del diseño */
        Texto(ui_giro() == UI_GIRO_180 ? "Atrás" : "Abrir", .marcas = F_ESTILO(DIS_T_ETIQUETA)), Hueco(),
        Texto(ui_giro() == UI_GIRO_180 ? "Abrir" : "Atrás", .marcas = F_ESTILO(DIS_T_ETIQUETA))))));
    return Teclas(menu_tecla, Tema(t, ui_nuevo(&(M3Fondo){ .base = { &ClsM3Fondo, 0 },
        .ox = (i16)(s->tx * M3_MARGEN / SH_INCLINA_MAX), .oy = (i16)(s->ty * M3_MARGEN / SH_INCLINA_MAX), .estilo = (u8)estilo, .gen = s->gen_fondo,
        .hijos = HIJOS(dis_con_teclas(Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(   /* (giro) las teclas, en barra o en columna */
            Caja(.an = INF, .al = (i16)M3_ALTO_TITULO, .hijo = RellenoXY(6, 4, Fila(.espacio = 4, .cruzado = P_INICIO, .hijos = HIJOS(
                Expandido(Texto(sh_apps[s->abriendo ? s->abriendo - 1 : s->sel].nombre, .marcas = F_ESTILO(DIS_T_TITULO), .color = tinta->titulo, .sombra = tinta->sombra)),
                sh_reloj_barra(t))))),
            Rejilla(.columnas = (u8)M3_COLS, .alto_celda = M3_ALTO_CELDA, .hijos = ui_lista(celdas, i1 - i0)),
            Hueco())),
            tinta->velo ? Velo(teclas) : teclas)) }, sizeof(M3Fondo))));
}

/* Al pasar de clásico a 3D (o al revés) con el menú debajo de Ajustes, el árbol de widgets es otro: las celdas se
 * montan de nuevo y el foco de esta ruta se pierde (el autofoco no vale: en ese momento el foco es de Ajustes). Al
 * volver, se le devuelve a la celda que lo tenía. */
static Elem *celda_de(Elem *e, u32 clave)
{
    unsigned i;
    if (e->cls == &ClsWEnfocable && e->clave == clave) return e;
    for (i = 0; i < e->n; i++) { Elem *r = celda_de(e->hijos[i], clave); if (r) return r; }
    return NULL;
}
static void menu_reenfocar(Ctx *c)
{
    MenuE *s = ui_estado(c);
    s->reenfocar = 0;
    if (ui_raiz() == c && !ui_foco()) { Elem *e = celda_de(c, (u32)s->sel + 1); if (e) ui_enfocar(e); }
}
static Widget *menu_construir(Ctx *c)
{
    MenuE *s = ui_estado(c);
    if (ui_raiz() == c && !ui_foco() && !s->reenfocar) { s->reenfocar = 1; ui_tras(c, 0, menu_reenfocar); }
    if (aj.menu_estilo == M3_MEDIOS) { dormir(c, s); s->abriendo = 0; s->t_abrir = 0; return sh_menu_medios(); }   /* (28-oct) el de MEDIOS */
    if (aj.menu_estilo == M3_ESCRITORIO) { dormir(c, s); s->abriendo = 0; s->t_abrir = 0; return sh_menu_escritorio(); }   /* (6-nov) el ESCRITORIO */
    if (!es3d()) { dormir(c, s); s->abriendo = 0; s->t_abrir = 0; return clasico_construir(s); }   /* (nada a medio abrir que heredar del 3D) */
    if (s->enfocar_sel) { s->enfocar_sel = 0; ui_tras(c, 0, menu_enfocar_sel); }   /* (W600) después de ESTE montaje: la celda ya existe */
    return m3_construir(c, s);
}
static void menu_desechar(Ctx *c) { dormir(c, ui_estado(c)); }
COMPONENTE_CON_ESTADO(Menu, MenuE, menu_construir, NULL, .desechar = menu_desechar);
Widget *sh_menu(void) { return UI_W(Menu); }
/* La imagen del fondo cuesta ~15 M de instrucciones (0,1–0,3 s): se hace en el ARRANQUE, con la pantalla de
 * arranque puesta, para que la primera vez que se abre el menú no dé un tirón. Con otro estilo se rehace al volver
 * de Ajustes. */
void sh_menu_preparar(void)
{
    const int n = sh_n_apps < 15 ? (sh_n_apps < M3_FILAS * M3_COLS ? sh_n_apps : M3_FILAS * M3_COLS) : M3_FILAS * M3_COLS < 15 ? M3_FILAS * M3_COLS : 15;
    Rect rej = { (i16)(ui_giro() == UI_GIRO_IZQ ? dis_teclas_columna() : 0), (i16)(M3_ALTO_TITULO + (ui_giro() == UI_GIRO_180 ? M3_ALTO_TECLAS : 0)),
                 (i16)(UI_ANCHO - dis_teclas_columna()), (i16)(((n + M3_COLS - 1) / M3_COLS) * M3_ALTO_CELDA) };
    if (es3d()) m3_fondo_preparar(aj.menu_estilo, n, rej, M3_ALTO_CELDA);
    if (aj.menu_estilo == M3_MEDIOS) sh_medios_preparar();
    if (aj.menu_estilo == M3_ESCRITORIO) sh_escritorio_preparar();
}
