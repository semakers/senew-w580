/* Más widgets de serie (pista S1): Párrafo, Velo, BarraDesp, Progreso, Pintura y la lista perezosa. */
#include "ui.h"
#include "diseno.h"                    /* (MD) lo que se ve lo pinta el diseño en uso */

static char *duplica(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    return memcpy(mo_pedir(n), s ? s : "", n);
}

/* ---- Párrafo ------------------------------------------------------------------------------------ */
static void parrafo_iniciar(Elem *e) { WParrafo *w = (WParrafo *)e->w; w->texto = duplica(w->texto); }
static void parrafo_desechar(Elem *e) { mo_soltar((void *)((WParrafo *)e->w)->texto); }
static int parrafo_actualizar(Elem *e, const Widget *nv)
{
    WParrafo *a = (WParrafo *)e->w; const WParrafo *b = (const WParrafo *)nv;
    const char *s = b->texto ? b->texto : "";
    int c = (a->max_lineas != b->max_lineas || a->marcas != b->marcas || a->centrado != b->centrado) ? C_DISPONER : a->color != b->color ? C_PINTAR : C_NADA;
    if (strcmp(a->texto, s)) { c = C_DISPONER; mo_soltar((void *)a->texto); a->texto = duplica(s); }
    a->color = b->color; a->max_lineas = b->max_lineas; a->marcas = b->marcas; a->centrado = b->centrado;
    return c;
}
static void parrafo_disponer(Elem *e, Restr r)
{
    const WParrafo *w = (const WParrafo *)e->w;
    const char *p = w->texto; int n = 0, an = r.maxw < INF ? r.maxw : UI_ANCHO, mayor = 0;
    while (*p && !(w->max_lineas && n >= w->max_lineas)) {
        int k = li_partir(p, an, 1, w->marcas), a = li_ancho_n(p, k, 1, w->marcas);
        if (a > mayor) mayor = a;
        p += k; n++;
    }
    e->an = (i16)(r.maxw < INF && (w->centrado || r.minw >= r.maxw) ? r.maxw : mayor);
    e->al = (i16)(n ? n * dis_paso_linea(w->marcas) - (dis_es_v1() ? 1 : 0) : 0);   /* (estética) el paso de la letra en uso */
}
static void parrafo_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const WParrafo *w = (const WParrafo *)e->w;
    const char *p = w->texto; int n = 0;
    Color c = w->color != TRANSPARENTE ? w->color : tema_de(e)->texto;
    while (*p && !(w->max_lineas && n >= w->max_lineas)) {
        int k = li_partir(p, e->an, 1, w->marcas), v = k;
        while (v && (p[v - 1] == ' ' || p[v - 1] == '\n')) v--;
        li_texto_n(l, x + (w->centrado ? (e->an - li_ancho_n(p, v, 1, w->marcas)) / 2 : 0), y + n * dis_paso_linea(w->marcas), p, v, c, 1, w->marcas);
        p += k; n++;
    }
}
const Clase ClsWParrafo = { .nombre = "Parrafo", .tipo = T_RENDER, .tam = sizeof(WParrafo), .iniciar = parrafo_iniciar,
                            .desechar = parrafo_desechar, .actualizar = parrafo_actualizar,
                            .disponer = parrafo_disponer, .pintar = parrafo_pintar };

/* ---- Velo ---------------------------------------------------------------------------------------- */
static void velo_disponer(Elem *e, Restr r)
{
    Elem *h = ui_hijo(e, 0);
    e->an = r.maxw < INF ? r.maxw : r.minw; e->al = r.maxh < INF ? r.maxh : r.minh;
    if (h) { ui_disponer(h, r); h->x = h->y = 0; e->an = h->an; e->al = h->al; }
}
static void velo_pintar(Elem *e, Lienzo *l, int x, int y) { li_velo(l, x, y, e->an, e->al); }
static int velo_actualizar(Elem *e, const Widget *nv) { *(WVelo *)e->w = *(const WVelo *)nv; return C_NADA; }
const Clase ClsWVelo = { .nombre = "Velo", .tipo = T_RENDER, .hijos = H_1, .tam = sizeof(WVelo), .actualizar = velo_actualizar,
                         .disponer = velo_disponer, .pintar = velo_pintar };

/* ---- Barra de desplazamiento y de progreso ---------------------------------------------------------- */
static void barra_disponer(Elem *e, Restr r) { e->an = 4; e->al = r.maxh < INF ? r.maxh : r.minh; }
static void barra_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const WBarraDesp *w = (const WBarraDesp *)e->w;
    const TemaDatos *t = tema_de(e);
    int h, p;
    if (w->total <= w->visibles || w->total <= 0) return;
    h = (int)((i32)e->al * w->visibles / w->total); if (h < 8) h = 8;
    p = (int)((i32)(e->al - h) * w->pos / (w->total - w->visibles));
    if (p > e->al - h) p = e->al - h;
    (void)t;
    dis_actual()->pintar_desp(l, x, y, e->an, e->al, p, h, w->color);
}
static int barra_actualizar(Elem *e, const Widget *nv)
{
    WBarraDesp *a = (WBarraDesp *)e->w; const WBarraDesp *b = (const WBarraDesp *)nv;
    int c = (a->total != b->total || a->visibles != b->visibles || a->pos != b->pos || a->color != b->color) ? C_PINTAR : C_NADA;
    *a = *b;
    return c;
}
const Clase ClsWBarraDesp = { .nombre = "BarraDesp", .tipo = T_RENDER, .tam = sizeof(WBarraDesp), .actualizar = barra_actualizar,
                              .disponer = barra_disponer, .pintar = barra_pintar };

static void progreso_disponer(Elem *e, Restr r) { e->an = r.maxw < INF ? r.maxw : 60; e->al = ((const WProgreso *)e->w)->al; }
static void progreso_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const WProgreso *w = (const WProgreso *)e->w;
    i32 v = w->valor < 0 ? 0 : w->valor > w->max ? w->max : w->valor;
    dis_actual()->pintar_progreso(l, x, y, e->an, e->al, v, w->max, w->color, w->fondo);
}
static int progreso_actualizar(Elem *e, const Widget *nv)
{
    WProgreso *a = (WProgreso *)e->w; const WProgreso *b = (const WProgreso *)nv;
    int c = a->al != b->al ? C_DISPONER : (a->valor != b->valor || a->max != b->max || a->color != b->color || a->fondo != b->fondo) ? C_PINTAR : C_NADA;
    *a = *b;
    return c;
}
const Clase ClsWProgreso = { .nombre = "Progreso", .tipo = T_RENDER, .tam = sizeof(WProgreso), .actualizar = progreso_actualizar,
                             .disponer = progreso_disponer, .pintar = progreso_pintar };

/* ---- Pintura -------------------------------------------------------------------------------------- */
static void pintura_disponer(Elem *e, Restr r)
{
    const WPintura *w = (const WPintura *)e->w;
    e->an = w->an ? w->an : r.maxw < INF ? r.maxw : r.minw;
    e->al = w->al ? w->al : r.maxh < INF ? r.maxh : r.minh;
}
static void pintura_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const WPintura *w = (const WPintura *)e->w;
    Rect r = { (i16)x, (i16)y, e->an, e->al }, g = l->recorte;
    int x1 = x + e->an < g.x + g.w ? x + e->an : g.x + g.w, y1 = y + e->al < g.y + g.h ? y + e->al : g.y + g.h;
    if (x > l->recorte.x) l->recorte.x = (i16)x;                      /* la aplicación no puede pintar fuera de lo suyo */
    if (y > l->recorte.y) l->recorte.y = (i16)y;
    l->recorte.w = (i16)(x1 - l->recorte.x); l->recorte.h = (i16)(y1 - l->recorte.y);
    if (l->recorte.w > 0 && l->recorte.h > 0 && w->pintar) w->pintar(l, r, w->dato);
    l->recorte = g;
}
static int pintura_actualizar(Elem *e, const Widget *nv)
{
    WPintura *a = (WPintura *)e->w; const WPintura *b = (const WPintura *)nv;
    int c = (a->an != b->an || a->al != b->al) ? C_DISPONER : (a->version != b->version || a->dato != b->dato || a->pintar != b->pintar) ? C_PINTAR : C_NADA;
    *a = *b;
    return c;
}
const Clase ClsWPintura = { .nombre = "Pintura", .tipo = T_RENDER, .tam = sizeof(WPintura), .actualizar = pintura_actualizar,
                            .disponer = pintura_disponer, .pintar = pintura_pintar };

/* ---- Lista perezosa -------------------------------------------------------------------------------
 * Un componente con estado (selección, primera fila visible, filas que caben) que construye solo las
 * filas a la vista dentro de una «ventana»: un render que las apila, recorta, y al disponerse mide
 * cuántas caben; si el número cambia, se lo apunta al componente y lo marca para reconstruir. */
typedef struct { i32 sel, primero; i16 visibles; } ListaPE;
typedef struct { Widget base; Widget **hijos; i16 alto_fila; i16 *visibles; Ctx *duenio; } WVentana;
static void ventana_disponer(Elem *e, Restr r)
{
    const WVentana *w = (const WVentana *)e->w;
    unsigned i; int y = 0, v;
    Restr c = { r.maxw, r.maxw, w->alto_fila, w->alto_fila };
    e->an = r.maxw; e->al = r.maxh < INF ? r.maxh : (i16)(e->n * w->alto_fila);
    for (i = 0; i < e->n; i++) { Elem *h = ui_hijo(e, i); if (h) { ui_disponer(h, c); h->x = 0; h->y = (i16)y; y += w->alto_fila; } }
    v = e->al / w->alto_fila; if (v < 1) v = 1;
    if (v != *w->visibles) { *w->visibles = (i16)v; ui_sucio(w->duenio); }
}
static int ventana_actualizar(Elem *e, const Widget *nv)
{
    WVentana *a = (WVentana *)e->w; const WVentana *b = (const WVentana *)nv;
    int c = a->alto_fila != b->alto_fila ? C_DISPONER : C_NADA;
    *a = *b;
    return c;
}
static const Clase ClsWVentana = { .nombre = "Ventana", .tipo = T_RENDER, .hijos = H_N, .recorta = 1, .tam = sizeof(WVentana),
                                   .actualizar = ventana_actualizar, .disponer = ventana_disponer };

static i32 *listap_sel(Ctx *c) { const WListaP *w = ui_widget(c); return w->sel ? w->sel : &((ListaPE *)ui_estado(c))->sel; }
static Widget *listap_construir(Ctx *c)
{
    const WListaP *w = ui_widget(c); ListaPE *s = ui_estado(c);
    i32 *sel = listap_sel(c), i, n = w->n, fin;
    Widget **filas;
    int enf = ui_foco() == c, af = w->alto_fila > 0 ? w->alto_fila : dis_tokens()->m.alto_fila;   /* (estética) 0 = el del diseño */
    if (*sel >= n) *sel = n - 1;
    if (*sel < 0) *sel = 0;
    if (!s->visibles) s->visibles = 8;
    if (*sel < s->primero) s->primero = *sel;
    if (*sel >= s->primero + s->visibles) s->primero = *sel - s->visibles + 1;
    if (s->primero > n - s->visibles) s->primero = n - s->visibles;
    if (s->primero < 0) s->primero = 0;
    fin = s->primero + s->visibles; if (fin > n) fin = n;
    filas = ui_arena((size_t)(fin - s->primero + 1) * sizeof *filas);
    for (i = s->primero; i < fin; i++)
        filas[i - s->primero] = UI_W(WCaja, CLAVE((u32)i + 1), .color = TRANSPARENTE, .borde = TRANSPARENTE, .an = INF, .al = (i16)af,
                                     .hijo = w->fila(c, (int)i, enf && i == *sel, w->dato));
    filas[fin - s->primero] = NULL;
    return Fila(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        Expandido(UI_W(WVentana, .hijos = filas, .alto_fila = (i16)af, .visibles = &s->visibles, .duenio = c)),
        n > s->visibles ? BarraDesp(.total = n, .visibles = s->visibles, .pos = s->primero, .color = w->barra) : NULL));
}
static void listap_iniciar(Ctx *c) { if (((const WListaP *)ui_widget(c))->autofoco && !ui_foco()) ui_enfocar(c); }
static int listap_tecla(Ctx *c, int k)
{
    const WListaP *w = ui_widget(c); ListaPE *s = ui_estado(c);
    i32 *sel = listap_sel(c), v = *sel, n = w->n;
    if (k == K_OK) { if (w->al_activar && n > 0) w->al_activar(c, (int)v, w->dato); return 1; }
    if (n <= 0) return 0;
    if (k == K_ABAJO) v = v + 1 < n ? v + 1 : 0;
    else if (k == K_ARRIBA) v = v > 0 ? v - 1 : n - 1;
    else if (k == K_VOL_MENOS) v = v + s->visibles < n ? v + s->visibles : n - 1;
    else if (k == K_VOL_MAS) v = v > s->visibles ? v - s->visibles : 0;
    else return 0;
    if (v != *sel) { *sel = v; ui_sucio(c); if (w->al_mover) w->al_mover(c, (int)v, w->dato); }
    return 1;
}
static void listap_foco(Ctx *c, int ganado) { (void)ganado; ui_sucio(c); }
const Clase ClsWListaP = { .nombre = "ListaP", .tipo = T_CON_ESTADO, .enfocable = 1, .tam = sizeof(WListaP),
                           .tam_estado = sizeof(ListaPE), .construir = listap_construir, .iniciar = listap_iniciar,
                           .tecla = listap_tecla, .foco = listap_foco };

/* ---- Selector numérico ---------------------------------------------------------------------------- */
typedef struct { u8 tecleando; } SelectorE;
static Widget *selector_construir(Ctx *c)
{
    const WSelector *w = ui_widget(c);
    int enf = ui_foco() == c;
    char f[8] = "%0_d"; f[2] = (char)('0' + w->digitos);
    return dis_actual()->selector(ui_fmt(f, *w->valor), w->escala, enf);
}
static void selector_iniciar(Ctx *c) { if (((const WSelector *)ui_widget(c))->autofoco && !ui_foco()) ui_enfocar(c); }
static int selector_tecla(Ctx *c, int k)
{
    const WSelector *w = ui_widget(c); SelectorE *s = ui_estado(c);
    i32 v = *w->valor;
    if (k == K_ARRIBA) { v = v < w->max ? v + 1 : w->min; s->tecleando = 0; }
    else if (k == K_ABAJO) { v = v > w->min ? v - 1 : w->max; s->tecleando = 0; }
    else if (k >= K_0 && k <= K_9) {
        v = s->tecleando ? v * 10 + (k - K_0) : (k - K_0);
        if (v > w->max) v = k - K_0;
        s->tecleando = 1;
        if (v < w->min && v * 10 > w->max) v = w->min;
    } else { s->tecleando = 0; return 0; }
    if (v != *w->valor) { *w->valor = (int)v; ui_sucio(c); if (w->al_cambiar) w->al_cambiar(c, w->dato); }
    return 1;
}
static void selector_foco(Ctx *c, int ganado)
{
    const WSelector *w = ui_widget(c);
    ((SelectorE *)ui_estado(c))->tecleando = 0;
    if (!ganado && *w->valor < w->min) { *w->valor = (int)w->min; if (w->al_cambiar) w->al_cambiar(c, w->dato); }
    ui_sucio(c);
}
const Clase ClsWSelector = { .nombre = "Selector", .tipo = T_CON_ESTADO, .enfocable = 1, .tam = sizeof(WSelector),
                             .tam_estado = sizeof(SelectorE), .construir = selector_construir, .iniciar = selector_iniciar,
                             .tecla = selector_tecla, .foco = selector_foco };
