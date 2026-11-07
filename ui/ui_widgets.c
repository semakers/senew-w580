/* Los widgets de serie. Disposición con enteros: las restricciones bajan, los tamaños suben. */
#include "ui.h"
#include "diseno.h"

static int tope(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* «¿Qué cambia?» campo a campo. No se comparan los bytes del struct: el relleno entre campos de un
 * literal compuesto no está definido y daba cambios falsos (medido: repintaba la rejilla entera). */
#define ACTUALIZAR(Tipo, redispone, repinta)                                                        \
    static int Tipo##_actualizar(Elem *e, const Widget *nv)                                         \
    {                                                                                               \
        Tipo *a = (Tipo *)e->w; const Tipo *b = (const Tipo *)nv;                                   \
        int c = (redispone) ? C_DISPONER : (repinta) ? C_PINTAR : C_NADA;                           \
        *a = *b;                                                                                    \
        return c;                                                                                   \
    }
ACTUALIZAR(WFlex, a->vertical != b->vertical || a->principal != b->principal || a->cruzado != b->cruzado || a->espacio != b->espacio || a->encoge != b->encoge, 0)
ACTUALIZAR(WExpandido, a->flex != b->flex, 0)
ACTUALIZAR(WRelleno, a->izq != b->izq || a->arr != b->arr || a->der != b->der || a->aba != b->aba, 0)
ACTUALIZAR(WAlinear, a->ax != b->ax || a->ay != b->ay, 0)
ACTUALIZAR(WPila, 0, 0)
ACTUALIZAR(WRejilla, a->columnas != b->columnas || a->alto_celda != b->alto_celda || a->espacio != b->espacio, 0)
ACTUALIZAR(WLista, 0, a->barra != b->barra)
ACTUALIZAR(WImagen, a->imagen != b->imagen, 0)

/* ---- Fila / Columna --------------------------------------------------------------------------- */
static void flex_disponer(Elem *e, Restr r)
{
    const WFlex *w = (const WFlex *)e->w;
    int V = w->vertical, maxp = V ? r.maxh : r.maxw, maxc = V ? r.maxw : r.maxh;
    int usado = 0, cruz = 0, flex = 0, libre, pos, sep = w->espacio, n = 0, resto;
    unsigned i;
    for (i = 0; i < e->n; i++) {
        Elem *h = ui_hijo(e, i);
        Restr c;
        if (!h) continue;
        n++;
        if (h->cls == &ClsWExpandido) { flex += ((const WExpandido *)h->w)->flex; continue; }
        c.minw = c.minh = 0; c.maxw = V ? (i16)maxc : INF; c.maxh = V ? INF : (i16)maxc;
        if (w->cruzado == P_ESTIRAR && maxc < INF) { if (V) c.minw = (i16)maxc; else c.minh = (i16)maxc; }
        ui_disponer(h, c);
        usado += V ? h->al : h->an;
        if ((V ? h->an : h->al) > cruz) cruz = V ? h->an : h->al;
    }
    usado += n > 1 ? sep * (n - 1) : 0;
    libre = maxp < INF ? maxp - usado : 0;
    if (libre < 0) libre = 0;
    resto = libre;
    for (i = 0; i < e->n && flex; i++) {
        Elem *h = ui_hijo(e, i);
        Restr c;
        int t;
        if (!h || h->cls != &ClsWExpandido) continue;
        t = libre * ((const WExpandido *)h->w)->flex / flex;
        resto -= t;
        if (V) { c.minh = c.maxh = (i16)t; c.minw = 0; c.maxw = (i16)maxc; if (w->cruzado == P_ESTIRAR && maxc < INF) c.minw = (i16)maxc; }
        else   { c.minw = c.maxw = (i16)t; c.minh = 0; c.maxh = (i16)maxc; if (w->cruzado == P_ESTIRAR && maxc < INF) c.minh = (i16)maxc; }
        ui_disponer(h, c);
        if ((V ? h->an : h->al) > cruz) cruz = V ? h->an : h->al;
    }
    if (flex) usado = maxp < INF ? maxp - resto : usado;    /* (R3; calidad) «libre = resto» no se leía después */
    if (w->cruzado == P_ESTIRAR && maxc < INF) cruz = maxc;
    cruz = tope(cruz, V ? r.minw : r.minh, maxc);
    {
        int total = (maxp < INF && (!w->encoge || flex)) ? maxp : usado, hueco = total - usado;
        pos = w->principal == P_CENTRO ? hueco / 2 : w->principal == P_FIN ? hueco : 0;
        if (w->principal == P_ENTRE && n > 1) sep += hueco / (n - 1);
        if (V) { e->al = (i16)total; e->an = (i16)cruz; } else { e->an = (i16)total; e->al = (i16)cruz; }
    }
    for (i = 0; i < e->n; i++) {
        Elem *h = ui_hijo(e, i);
        int hc, pc;
        if (!h) continue;
        hc = V ? h->an : h->al;
        pc = w->cruzado == P_CENTRO ? (cruz - hc) / 2 : w->cruzado == P_FIN ? cruz - hc : 0;
        if (V) { h->x = (i16)pc; h->y = (i16)pos; pos += h->al + sep; }
        else   { h->x = (i16)pos; h->y = (i16)pc; pos += h->an + sep; }
    }
}
const Clase ClsWFlex = { .nombre = "Flex", .tipo = T_RENDER, .hijos = H_N, .tam = sizeof(WFlex), .actualizar = WFlex_actualizar, .disponer = flex_disponer };

/* ---- proxies de disposición: Expandido, Relleno, Alinear ----------------------------------------- */
static void expandido_disponer(Elem *e, Restr r)
{
    Elem *h = ui_hijo(e, 0);
    e->an = r.minw; e->al = r.minh;
    if (h) { ui_disponer(h, r); h->x = h->y = 0; e->an = h->an; e->al = h->al; }
}
const Clase ClsWExpandido = { .nombre = "Expandido", .tipo = T_RENDER, .hijos = H_1, .tam = sizeof(WExpandido), .actualizar = WExpandido_actualizar, .disponer = expandido_disponer };

static void relleno_disponer(Elem *e, Restr r)
{
    const WRelleno *w = (const WRelleno *)e->w;
    Elem *h = ui_hijo(e, 0);
    int px = w->izq + w->der, py = w->arr + w->aba;
    e->an = (i16)px; e->al = (i16)py;
    if (h) {
        Restr c;
        c.minw = (i16)tope(r.minw - px, 0, INF); c.minh = (i16)tope(r.minh - py, 0, INF);
        c.maxw = r.maxw >= INF ? INF : (i16)tope(r.maxw - px, 0, INF);
        c.maxh = r.maxh >= INF ? INF : (i16)tope(r.maxh - py, 0, INF);
        ui_disponer(h, c);
        h->x = w->izq; h->y = w->arr;
        e->an = (i16)(h->an + px); e->al = (i16)(h->al + py);
    }
}
const Clase ClsWRelleno = { .nombre = "Relleno", .tipo = T_RENDER, .hijos = H_1, .tam = sizeof(WRelleno), .actualizar = WRelleno_actualizar, .disponer = relleno_disponer };

static void alinear_disponer(Elem *e, Restr r)
{
    const WAlinear *w = (const WAlinear *)e->w;
    Elem *h = ui_hijo(e, 0);
    Restr c = { 0, r.maxw, 0, r.maxh };
    if (h) ui_disponer(h, c);
    e->an = r.maxw < INF ? r.maxw : (h ? h->an : 0);
    e->al = r.maxh < INF ? r.maxh : (h ? h->al : 0);
    if (h) { h->x = (i16)((e->an - h->an) * (w->ax + 1) / 2); h->y = (i16)((e->al - h->al) * (w->ay + 1) / 2); }
}
const Clase ClsWAlinear = { .nombre = "Alinear", .tipo = T_RENDER, .hijos = H_1, .tam = sizeof(WAlinear), .actualizar = WAlinear_actualizar, .disponer = alinear_disponer };

/* ---- Caja ---------------------------------------------------------------------------------------- */
static void caja_disponer(Elem *e, Restr r)
{
    const WCaja *w = (const WCaja *)e->w;
    Elem *h = ui_hijo(e, 0);
    Restr c = r;
    if (w->an) { int a = w->an >= INF ? r.maxw : tope(w->an, r.minw, r.maxw); c.minw = c.maxw = (i16)a; }
    if (w->al) { int a = w->al >= INF ? r.maxh : tope(w->al, r.minh, r.maxh); c.minh = c.maxh = (i16)a; }
    e->an = c.minw; e->al = c.minh;
    if (h) { ui_disponer(h, c); h->x = h->y = 0; e->an = h->an; e->al = h->al; }
}
static void caja_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const WCaja *w = (const WCaja *)e->w;
    li_rect(l, x, y, e->an, e->al, w->color);
    if (w->borde != TRANSPARENTE) li_marco(l, x, y, e->an, e->al, w->borde);
}
static int caja_actualizar(Elem *e, const Widget *nv)
{
    WCaja *a = (WCaja *)e->w; const WCaja *b = (const WCaja *)nv;
    int c = (a->an != b->an || a->al != b->al) ? C_DISPONER : (a->color != b->color || a->borde != b->borde) ? C_PINTAR : C_NADA;
    *a = *b;
    return c;
}
static int caja_opaco(Elem *e) { return ((const WCaja *)e->w)->color != TRANSPARENTE; }
const Clase ClsWCaja = { .nombre = "Caja", .tipo = T_RENDER, .hijos = H_1, .tam = sizeof(WCaja), .opaco = caja_opaco,
                         .disponer = caja_disponer, .pintar = caja_pintar, .actualizar = caja_actualizar };

/* ---- Pila, Rejilla -------------------------------------------------------------------------------- */
static void pila_disponer(Elem *e, Restr r)
{
    unsigned i;
    Restr c = { 0, r.maxw, 0, r.maxh };
    e->an = r.minw; e->al = r.minh;
    for (i = 0; i < e->n; i++) {
        Elem *h = ui_hijo(e, i);
        if (!h) continue;
        ui_disponer(h, c); h->x = h->y = 0;
        if (h->an > e->an) e->an = h->an;
        if (h->al > e->al) e->al = h->al;
    }
}
const Clase ClsWPila = { .nombre = "Pila", .tipo = T_RENDER, .hijos = H_N, .tam = sizeof(WPila), .actualizar = WPila_actualizar, .disponer = pila_disponer };

static void rejilla_disponer(Elem *e, Restr r)
{
    const WRejilla *w = (const WRejilla *)e->w;
    int cols = w->columnas ? w->columnas : 1, filas = ((int)e->n + cols - 1) / cols, sep = w->espacio;
    int an = r.maxw < INF ? r.maxw : UI_ANCHO, ca = (an - sep * (cols - 1)) / cols;
    int cl = w->alto_celda ? w->alto_celda : (r.maxh < INF && filas) ? (r.maxh - sep * (filas - 1)) / filas : ca;
    unsigned i;
    for (i = 0; i < e->n; i++) {
        Elem *h = ui_hijo(e, i);
        Restr c = { (i16)ca, (i16)ca, (i16)cl, (i16)cl };
        if (!h) continue;
        ui_disponer(h, c);
        h->x = (i16)((int)(i % (unsigned)cols) * (ca + sep)); h->y = (i16)((int)(i / (unsigned)cols) * (cl + sep));
    }
    e->an = (i16)an; e->al = (i16)(filas ? filas * cl + (filas - 1) * sep : 0);
}
const Clase ClsWRejilla = { .nombre = "Rejilla", .tipo = T_RENDER, .hijos = H_N, .tam = sizeof(WRejilla), .actualizar = WRejilla_actualizar, .disponer = rejilla_disponer };

/* ---- Lista con desplazamiento ---------------------------------------------------------------------- */
typedef struct { i16 scroll, contenido; } ListaE;
static void lista_disponer(Elem *e, Restr r)
{
    ListaE *s = e->estado;
    unsigned i; int y = 0, max;
    Restr c = { r.maxw, r.maxw, 0, INF };
    for (i = 0; i < e->n; i++) { Elem *h = ui_hijo(e, i); if (h) { ui_disponer(h, c); y += h->al; } }
    s->contenido = (i16)y;
    e->an = r.maxw; e->al = r.maxh < INF ? r.maxh : (i16)y;
    max = y - e->al;
    s->scroll = (i16)tope(s->scroll, 0, max > 0 ? max : 0);
    y = -s->scroll;
    for (i = 0; i < e->n; i++) { Elem *h = ui_hijo(e, i); if (h) { h->x = 0; h->y = (i16)y; y += h->al; } }
}
static void lista_mostrar(Elem *e, Rect a)
{
    ListaE *s = e->estado;
    int d = a.y < 0 ? a.y : a.y + a.h > e->al ? a.y + a.h - e->al : 0;
    if (!d) return;
    s->scroll = (i16)(s->scroll + d);
    ui_marcar_disponer(e);
}
static void lista_encima(Elem *e, Lienzo *l, int x, int y)
{
    const WLista *w = (const WLista *)e->w; ListaE *s = e->estado;
    int h, p;
    if (w->barra == TRANSPARENTE || s->contenido <= e->al) return;
    h = e->al * e->al / s->contenido; if (h < 6) h = 6;
    p = (e->al - h) * s->scroll / (s->contenido - e->al);
    li_rect(l, x + e->an - 3, y + p, 3, h, w->barra);
}
const Clase ClsWLista = { .nombre = "Lista", .tipo = T_RENDER, .hijos = H_N, .recorta = 1, .tam = sizeof(WLista), .actualizar = WLista_actualizar,
                          .tam_estado = sizeof(ListaE), .disponer = lista_disponer, .mostrar = lista_mostrar,
                          .pintar_encima = lista_encima };

/* ---- Tema ------------------------------------------------------------------------------------------ */
static const TemaDatos tema_defecto = { RGB(0, 0, 0), RGB(255, 255, 255), RGB(150, 150, 150), RGB(255, 130, 0), RGB(0, 0, 0), RGB(40, 40, 40) };
const Clase ClsWTema = { .nombre = "Tema", .tipo = T_PROXY, .hijos = H_1, .tam = sizeof(WTema) };
static const TemaDatos *tema_glob = &tema_defecto;
void ui_tema_global(const TemaDatos *t) { tema_glob = t ? t : &tema_defecto; }
const TemaDatos *tema_de(Ctx *c)
{
    const WTema *t = ui_heredado(c, &ClsWTema);
    return t ? t->datos : tema_glob;
}

/* ---- Texto: el objeto de render guarda su copia de la cadena (la del widget muere con el cuadro) ---- */
static char *duplica(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(mo_pedir(n), s, n);
}
static void texto_iniciar(Elem *e) { WTexto *w = (WTexto *)e->w; w->texto = duplica(w->texto ? w->texto : ""); }
static void texto_desechar(Elem *e) { mo_soltar((void *)((WTexto *)e->w)->texto); }
static int texto_actualizar(Elem *e, const Widget *nv)
{
    WTexto *a = (WTexto *)e->w; const WTexto *b = (const WTexto *)nv;
    const char *s = b->texto ? b->texto : "";
    int c = C_NADA;
    if (a->escala != b->escala || a->marcas != b->marcas || (a->sombra == TRANSPARENTE) != (b->sombra == TRANSPARENTE)) c = C_DISPONER;
    else if (a->color != b->color || a->sombra != b->sombra) c = C_PINTAR;
    if (strcmp(a->texto, s)) {
        c = (li_ancho_texto_f(a->texto, 1, b->marcas) == li_ancho_texto_f(s, 1, b->marcas) && li_descendentes(a->texto) == li_descendentes(s) && c != C_DISPONER) ? C_PINTAR : C_DISPONER;
        mo_soltar((void *)a->texto);
        a->texto = duplica(s);
    }
    a->color = b->color; a->escala = b->escala; a->marcas = b->marcas; a->sombra = b->sombra; a->base.clave = b->base.clave;
    return c;
}
static void texto_disponer(Elem *e, Restr r)
{
    const WTexto *w = (const WTexto *)e->w;
    (void)r;
    int s = w->sombra != TRANSPARENTE ? w->escala : 0;
    e->an = (i16)(li_ancho_texto_f(w->texto, w->escala, w->marcas) + s);
    e->al = (i16)(dis_alto_texto(w->escala, w->marcas, w->texto) + s);   /* (MD) lo que diga el diseño; sin letra propia, como siempre: a escala 1 siempre 9 (las filas no bailan) */
    if (!dis_es_v1() && r.maxw < INF && e->an > r.maxw) e->an = r.maxw;  /* (estética) no cabe: se queda en lo que le dejan y se pinta con «…» */
}
static void texto_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const WTexto *w = (const WTexto *)e->w;
    Rect g = l->recorte;
    if (x + e->an < g.x + g.w) l->recorte.w = (i16)(x + e->an - g.x);     /* corta lo que no cupo */
    /* (MD) y nada fuera de su caja por ningún lado: un glifo con grises que asoma (la pata de la «j», un acento) se pintaría
     * en un repintado completo y no en uno parcial del vecino: el panel y el repintado entero no coincidirían */
    if (x > l->recorte.x) { l->recorte.w = (i16)(l->recorte.w - (x - l->recorte.x)); l->recorte.x = (i16)x; }
    if (y > l->recorte.y) { l->recorte.h = (i16)(l->recorte.h - (y - l->recorte.y)); l->recorte.y = (i16)y; }
    if (y + e->al < l->recorte.y + l->recorte.h) l->recorte.h = (i16)(y + e->al - l->recorte.y);
    if (l->recorte.w <= 0 || l->recorte.h <= 0) { l->recorte = g; return; }
    if (!dis_es_v1() && li_ancho_texto_f(w->texto, w->escala, w->marcas) > e->an) {     /* (estética) recortado: «Brillo de pant…» */
        const char *t = w->texto; const int pe = li_ancho_texto_f("…", w->escala, w->marcas);
        int n = (int)strlen(t);
        Color c = w->color != TRANSPARENTE ? w->color : tema_de(e)->texto;
        while (n > 0 && li_ancho_n(t, n, w->escala, w->marcas) + pe > e->an) { n--; while (n > 0 && ((u8)t[n] & 0xC0) == 0x80) n--; }
        while (n > 0 && t[n - 1] == ' ') n--;
        if (w->sombra != TRANSPARENTE) li_texto_n(l, x + w->escala, y + w->escala, t, n, w->sombra, w->escala, w->marcas);
        li_texto_n(l, x, y, t, n, c, w->escala, w->marcas);
        li_texto_f(l, x + li_ancho_n(t, n, w->escala, w->marcas), y, "…", c, w->escala, w->marcas);
        l->recorte = g; return;
    }
    if (w->sombra != TRANSPARENTE) li_texto_f(l, x + w->escala, y + w->escala, w->texto, w->sombra, w->escala, w->marcas);
    li_texto_f(l, x, y, w->texto, w->color != TRANSPARENTE ? w->color : tema_de(e)->texto, w->escala, w->marcas);
    l->recorte = g;
}
const Clase ClsWTexto = { .nombre = "Texto", .tipo = T_RENDER, .tam = sizeof(WTexto), .iniciar = texto_iniciar,
                          .desechar = texto_desechar, .actualizar = texto_actualizar,
                          .disponer = texto_disponer, .pintar = texto_pintar };

/* ---- Icono, Imagen ---------------------------------------------------------------------------------- */
static void icono_disponer(Elem *e, Restr r)
{
    const WIcono *w = (const WIcono *)e->w;
    (void)r;
    e->an = (i16)(w->icono->w * w->escala); e->al = (i16)(w->icono->h * w->escala);
}
static void icono_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const WIcono *w = (const WIcono *)e->w;
    li_icono(l, x, y, w->icono, w->color != TRANSPARENTE ? w->color : tema_de(e)->texto, w->escala);
}
static int icono_actualizar(Elem *e, const Widget *nv)
{
    WIcono *a = (WIcono *)e->w; const WIcono *b = (const WIcono *)nv;
    int c = (a->icono != b->icono || a->escala != b->escala) ? C_DISPONER : a->color != b->color ? C_PINTAR : C_NADA;
    *a = *b;
    return c;
}
const Clase ClsWIcono = { .nombre = "Icono", .tipo = T_RENDER, .tam = sizeof(WIcono), .disponer = icono_disponer,
                          .pintar = icono_pintar, .actualizar = icono_actualizar };

static void imagen_disponer(Elem *e, Restr r)
{
    const WImagen *w = (const WImagen *)e->w;
    (void)r;
    e->an = (i16)w->imagen->w; e->al = (i16)w->imagen->h;
}
static void imagen_pintar(Elem *e, Lienzo *l, int x, int y) { li_imagen(l, x, y, ((const WImagen *)e->w)->imagen); }
const Clase ClsWImagen = { .nombre = "Imagen", .tipo = T_RENDER, .tam = sizeof(WImagen), .actualizar = WImagen_actualizar, .disponer = imagen_disponer, .pintar = imagen_pintar };

/* ---- Enfocable y Teclas ------------------------------------------------------------------------------ */
typedef struct { Anim a; Foco f; } EnfocableE;
static Widget *enfocable_construir(Ctx *c)
{
    const WEnfocable *w = ui_widget(c); EnfocableE *s = ui_estado(c);
    s->f.enfocado = ui_foco() == c;
    s->f.t = (i16)(s->f.enfocado ? ui_curva_sal(ui_anim_t(&s->a)) : 0);
    return w->constructor(c, &s->f, w->dato);
}
static void enfocable_iniciar(Ctx *c) { if (((const WEnfocable *)ui_widget(c))->autofoco && !ui_foco()) ui_enfocar(c); }
static int enfocable_tecla(Ctx *c, int k)
{
    const WEnfocable *w = ui_widget(c);
    if (k != K_OK || !w->al_activar) return 0;
    w->al_activar(c, w->dato);
    return 1;
}
static void enfocable_foco(Ctx *c, int ganado)
{
    const WEnfocable *w = ui_widget(c); EnfocableE *s = ui_estado(c);
    if (!ganado) return;
    if (w->anim_ms) ui_anim(c, &s->a, w->anim_ms);
    if (w->al_enfocar) w->al_enfocar(c, w->dato);
}
const Clase ClsWEnfocable = { .nombre = "Enfocable", .tipo = T_CON_ESTADO, .enfocable = 1, .tam = sizeof(WEnfocable),
                              .tam_estado = sizeof(EnfocableE), .construir = enfocable_construir,
                              .iniciar = enfocable_iniciar, .tecla = enfocable_tecla, .foco = enfocable_foco };

static int teclas_tecla(Ctx *c, int k)
{
    const WTeclas *w = ui_widget(c);
    return w->al_tecla ? w->al_tecla(c, k, w->dato) : 0;
}
const Clase ClsWTeclas = { .nombre = "Teclas", .tipo = T_PROXY, .hijos = H_1, .tam = sizeof(WTeclas), .tecla = teclas_tecla };
