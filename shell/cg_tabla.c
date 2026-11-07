/* cg_tabla.c — LA TABLA de funciones que ve una app cargable (API v1; api/cargable.h) y sus envolturas.
 *
 * Dos clases de entrada:
 *  - DIRECTAS: funciones puras o que solo escriben donde la app les dice (memcpy, ui_fmt). Un puntero malo ahí es
 *    un fallo de la CPU con código de la app en curso: el cargador la cierra y el sistema sigue.
 *  - ENVUELTAS (todas las demás): comprueban lo que el sistema va a GUARDAR o a ESCRIBIR (punteros dentro de la
 *    memoria de la app, cadenas legibles, widgets que son widgets, retrollamadas dentro de su código), traducen
 *    colores y teclas, ponen el prefijo a los ficheros y ocultan todo lo que es de este modelo de teléfono.
 * Aquí NO hay: flash, PMU, GPIO, I²C, reinicio, apagado, pánico, gancho del tic, ajustes del sistema, ni datos globales. */
#include "cargador.h"
#include "app_externa.h"
#include <stddef.h>
#include <stdarg.h>

#define MUERE(m, r) do { cg_matar(m); return r; } while (0)
static int et_del_deposito(const EntradaTexto *t);
static int arena_hay(u32 n) { if (ui_arena_libre() >= n + 3072u) return 1; cg_matar("La app agotó la memoria de un cuadro (demasiados widgets o texto)."); return 0; }

static Color col(unsigned c) { return (c >> 24) ? TRANSPARENTE : RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
/* (AUD9 A5) copiar a la ARENA DEL SISTEMA (que con PM la app solo lee): lo validado no se puede cambiar después. */
static const char *dup_cad_sys(const char *s)
{
    unsigned n = 0; char *d;
    if (!s) return "";
    while (n < 4096u && s[n]) n++;
    if (!arena_hay(n + 1)) return NULL;
    d = ui_arena(n + 1); memcpy(d, s, n); d[n] = 0;
    return d;
}
static unsigned col_a_rgb(Color c)
{
    unsigned v = (unsigned)((c >> 8) | (c << 8)) & 0xFFFF, r = v >> 11, g = (v >> 5) & 63, b = v & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

/* ---- los widgets que la app puede devolver ------------------------------------------------------------------------ */
static const Clase *const conocidas[] = { &ClsWFlex, &ClsWExpandido, &ClsWRelleno, &ClsWAlinear, &ClsWCaja, &ClsWPila, &ClsWRejilla,
    &ClsWLista, &ClsWTexto, &ClsWParrafo, &ClsWVelo, &ClsWBarraDesp, &ClsWProgreso, &ClsWPintura, &ClsWListaP, &ClsWSelector,
    &ClsWCampoTexto, &ClsWIcono, &ClsWEnfocable, &ClsWTeclas, &ClsWTema, &ClsWCgPant, &ClsWCajaRed };   /* (MD) CajaRed: la devuelven los componentes 1.3 */
Widget *cg_widget(void *p)
{
    Widget *w = p;
    unsigned i;
    if (!w || ((size_t)w & (sizeof(void *) - 1)) || !ui_en_arena(w)) return NULL;
    for (i = 0; i < sizeof conocidas / sizeof conocidas[0]; i++) if (w->cls == conocidas[i]) return w;
    return NULL;
}
int cg_ctx(const void *p)
{
    const Elem *e = p;
    unsigned i;
    if (!e || ((size_t)e & (sizeof(void *) - 1))) return 0;
    if (cg_pm_activa() && (cg_en_app(e, sizeof *e) || !cg_sis_leible(e, sizeof *e))) return 0;   /* (PM/AUD9) un contexto es del sistema (arena o su RAM): ni fabricado en la app ni una dirección cualquiera (un registro se borraría al leerlo) */
    for (i = 0; i < sizeof conocidas / sizeof conocidas[0]; i++) if (e->cls == conocidas[i]) return 1;   /* (un puntero disparatado da fallo AQUÍ, con la app en curso) */
    return 0;
}
#define CTX(c, r) do { if (!cg_ctx(c)) MUERE("La app dio un contexto que no lo es.", r); } while (0)

/* ---- los trampolines: lo que el framework llama en vez de la función de la app -------------------------------------- */
#define P(x) ((W5Pal)(size_t)(x))
/* (AUD9 C1) El W5Lienzo que la app recibe en th_pintar es un puntero del SISTEMA: las funciones li_* corren privilegiadas
 * tras la puerta y escriben en `l->cuadro`. Si se aceptara cualquier `l`, la app fabricaría uno con `cuadro` apuntando al
 * sistema y escribiría ahí. Por eso solo se acepta EXACTAMENTE el lienzo vigente: el que th_pintar acaba de entregar. */
static const Lienzo *cg_lienzo_ok;
static int li_vale(W5Lienzo l) { if (!cg_pm_activa()) return 1; if ((const Lienzo *)l == cg_lienzo_ok && cg_lienzo_ok) return 1; cg_matar("La app dio un lienzo que no es el suyo."); return 0; }
int cg_li_vale(W5Lienzo l) { return li_vale(l); }   /* (INT2) el mismo control para la 1.2 (shell/cg12_c.c: im_pintar) */
static void th_pintar(Lienzo *l, Rect r, void *dato)
{
    CgRet *t = cg_ret_de(dato);
    W5Rect rr = { r.x, r.y, r.w, r.h }, *pr = &rr;
    const Lienzo *ant = cg_lienzo_ok;
    if (t && cg_pm_activa()) { if (!(pr = cg_arena_app(sizeof rr))) return; *pr = rr; }   /* (PM) la app no ve la pila del sistema: el rectángulo, en SU memoria */
    cg_lienzo_ok = l;                                        /* (AUD9 C1) válido solo mientras dura esta retrollamada de pintar */
    if (t) cg_llamar(t->fn[0], P(l), P(pr), P(t->dato), 0);
    cg_lienzo_ok = ant;
}
static int th_tecla(Ctx *c, int k, void *dato) { CgRet *t = cg_ret_de(dato); return t ? (int)cg_llamar(t->fn[0], P(c), P(k), P(t->dato), 0) : 0; }
static Widget *th_fila(Ctx *c, int i, int sel, void *dato)
{
    CgRet *t = cg_ret_de(dato);
    Widget *w = t ? cg_widget((void *)cg_llamar(t->fn[0], P(c), P(i), P(sel), P(t->dato))) : NULL;
    return w ? w : Caja();
}
static void th_fila_activar(Ctx *c, int i, void *dato) { CgRet *t = cg_ret_de(dato); if (t) cg_llamar(t->fn[1], P(c), P(i), P(t->dato), 0); }
static void th_fila_mover(Ctx *c, int i, void *dato) { CgRet *t = cg_ret_de(dato); if (t) cg_llamar(t->fn[2], P(c), P(i), P(t->dato), 0); }
static void th_cambiar(Ctx *c, void *dato) { CgRet *t = cg_ret_de(dato); if (t) cg_llamar(t->fn[0], P(c), P(t->dato), 0, 0); }
static Widget *th_enfocado(Ctx *c, const Foco *f, void *dato)
{
    CgRet *t = cg_ret_de(dato);
    Widget *w = t ? cg_widget((void *)cg_llamar(t->fn[0], P(c), P(f->enfocado), P((int)f->t), P(t->dato))) : NULL;
    return w ? w : Caja();
}
static void th_activar(Ctx *c, void *dato) { CgRet *t = cg_ret_de(dato); if (t) cg_llamar(t->fn[1], P(c), P(t->dato), 0, 0); }
static void th_enfocar(Ctx *c, void *dato) { CgRet *t = cg_ret_de(dato); if (t) cg_llamar(t->fn[2], P(c), P(t->dato), 0, 0); }
static void th_si(void *dato) { CgRet *t = cg_ret_de(dato); if (t) cg_llamar(t->fn[0], P(t->dato), 0, 0, 0); }
static void th_elegir(int i, void *dato) { CgRet *t = cg_ret_de(dato); if (t) cg_llamar(t->fn[0], P(i), P(t->dato), 0, 0); }

/* ---- ui_w: una clase y una lista de { atributo, valor } ------------------------------------------------------------- */
enum { T_I16, T_U8, T_S8, T_I32, T_U16, T_COL, T_CAD, T_W, T_WS, T_FN0, T_FN1, T_FN2, T_DATO, T_PINT, T_ET, T_ICO };
typedef struct { u8 atr, tipo; u16 off; } Atr;
#define A(atr, tipo, T, campo) { atr, tipo, (u16)offsetof(T, campo) }
#define FN(atr, k) { atr, T_FN0 + (k), 0 }
static const Atr a_flex[] = { A(W5A_HIJOS, T_WS, WFlex, hijos), A(W5A_VERTICAL, T_U8, WFlex, vertical), A(W5A_PRINCIPAL, T_U8, WFlex, principal),
                              A(W5A_CRUZADO, T_U8, WFlex, cruzado), A(W5A_ENCOGE, T_U8, WFlex, encoge), A(W5A_ESPACIO, T_I16, WFlex, espacio) };
static const Atr a_expandido[] = { A(W5A_HIJO, T_W, WExpandido, hijo), A(W5A_FLEX, T_U8, WExpandido, flex) };
static const Atr a_relleno[] = { A(W5A_HIJO, T_W, WRelleno, hijo), A(W5A_IZQ, T_I16, WRelleno, izq), A(W5A_ARR, T_I16, WRelleno, arr),
                                 A(W5A_DER, T_I16, WRelleno, der), A(W5A_ABA, T_I16, WRelleno, aba) };
static const Atr a_alinear[] = { A(W5A_HIJO, T_W, WAlinear, hijo), A(W5A_AX, T_S8, WAlinear, ax), A(W5A_AY, T_S8, WAlinear, ay) };
static const Atr a_caja[] = { A(W5A_HIJO, T_W, WCaja, hijo), A(W5A_AN, T_I16, WCaja, an), A(W5A_AL, T_I16, WCaja, al),
                              A(W5A_COLOR, T_COL, WCaja, color), A(W5A_BORDE, T_COL, WCaja, borde) };
static const Atr a_pila[] = { A(W5A_HIJOS, T_WS, WPila, hijos) };
static const Atr a_rejilla[] = { A(W5A_HIJOS, T_WS, WRejilla, hijos), A(W5A_COLUMNAS, T_U8, WRejilla, columnas),
                                 A(W5A_ALTO_CELDA, T_I16, WRejilla, alto_celda), A(W5A_ESPACIO, T_I16, WRejilla, espacio) };
static const Atr a_lista[] = { A(W5A_HIJOS, T_WS, WLista, hijos), A(W5A_BARRA, T_COL, WLista, barra) };
static const Atr a_texto[] = { A(W5A_TEXTO, T_CAD, WTexto, texto), A(W5A_COLOR, T_COL, WTexto, color), A(W5A_SOMBRA, T_COL, WTexto, sombra),
                               A(W5A_ESCALA, T_U8, WTexto, escala), A(W5A_MARCAS, T_U8, WTexto, marcas) };
static const Atr a_parrafo[] = { A(W5A_TEXTO, T_CAD, WParrafo, texto), A(W5A_COLOR, T_COL, WParrafo, color), A(W5A_MAX_LINEAS, T_U8, WParrafo, max_lineas),
                                 A(W5A_MARCAS, T_U8, WParrafo, marcas), A(W5A_CENTRADO, T_U8, WParrafo, centrado) };
static const Atr a_velo[] = { A(W5A_HIJO, T_W, WVelo, hijo) };
static const Atr a_barra[] = { A(W5A_TOTAL, T_I32, WBarraDesp, total), A(W5A_VISIBLES, T_I32, WBarraDesp, visibles), A(W5A_POS, T_I32, WBarraDesp, pos),
                               A(W5A_COLOR, T_COL, WBarraDesp, color) };
static const Atr a_progreso[] = { A(W5A_VALOR, T_I32, WProgreso, valor), A(W5A_MAX, T_I32, WProgreso, max), A(W5A_COLOR, T_COL, WProgreso, color),
                                  A(W5A_FONDO, T_COL, WProgreso, fondo), A(W5A_AL, T_I16, WProgreso, al) };
static const Atr a_pintura[] = { FN(W5A_PINTAR, 0), { W5A_DATO, T_DATO, 0 }, A(W5A_VERSION, T_I32, WPintura, version),
                                 A(W5A_AN, T_I16, WPintura, an), A(W5A_AL, T_I16, WPintura, al) };
static const Atr a_listap[] = { A(W5A_N, T_I32, WListaP, n), FN(W5A_FILA, 0), FN(W5A_AL_ACTIVAR, 1), FN(W5A_AL_MOVER, 2), { W5A_DATO, T_DATO, 0 },
                                A(W5A_SEL, T_PINT, WListaP, sel), A(W5A_ALTO_FILA, T_I16, WListaP, alto_fila), A(W5A_AUTOFOCO, T_U8, WListaP, autofoco),
                                A(W5A_BARRA, T_COL, WListaP, barra) };
static const Atr a_selector[] = { A(W5A_VALOR, T_PINT, WSelector, valor), A(W5A_MIN, T_I32, WSelector, min), A(W5A_MAX, T_I32, WSelector, max),
                                  FN(W5A_AL_CAMBIAR, 0), { W5A_DATO, T_DATO, 0 }, A(W5A_DIGITOS, T_U8, WSelector, digitos),
                                  A(W5A_ESCALA, T_U8, WSelector, escala), A(W5A_AUTOFOCO, T_U8, WSelector, autofoco) };
static const Atr a_campo[] = { A(W5A_ENTRADA, T_ET, WCampoTexto, e), FN(W5A_AL_CAMBIAR, 0), { W5A_DATO, T_DATO, 0 },
                               A(W5A_LINEAS, T_U8, WCampoTexto, lineas), A(W5A_AUTOFOCO, T_U8, WCampoTexto, autofoco) };
static const Atr a_icono[] = { A(W5A_ICONO, T_ICO, WIcono, icono), A(W5A_COLOR, T_COL, WIcono, color), A(W5A_ESCALA, T_U8, WIcono, escala) };
static const Atr a_enfocable[] = { FN(W5A_CONSTRUCTOR, 0), FN(W5A_AL_ACTIVAR, 1), FN(W5A_AL_ENFOCAR, 2), { W5A_DATO, T_DATO, 0 },
                                   A(W5A_ANIM_MS, T_U16, WEnfocable, anim_ms), A(W5A_AUTOFOCO, T_U8, WEnfocable, autofoco) };
static const Atr a_teclas[] = { A(W5A_HIJO, T_W, WTeclas, hijo), FN(W5A_AL_TECLA, 0), { W5A_DATO, T_DATO, 0 } };

/* los valores por omisión: los mismos que dan las macros de ui_widgets.h */
static const WFlex d_flex = { .vertical = 1 };
static const WExpandido d_expandido = { .flex = 1 };
static const WCaja d_caja = { .color = TRANSPARENTE, .borde = TRANSPARENTE };
static const WRejilla d_rejilla = { .columnas = 3 };
static const WLista d_lista = { .barra = TRANSPARENTE };
static const WTexto d_texto = { .texto = "", .color = TRANSPARENTE, .sombra = TRANSPARENTE, .escala = 1 };
static const WParrafo d_parrafo = { .texto = "", .color = TRANSPARENTE };
static const WBarraDesp d_barra = { .color = TRANSPARENTE };
static const WProgreso d_progreso = { .color = TRANSPARENTE, .fondo = TRANSPARENTE, .al = 6 };
static const WListaP d_listap = { .alto_fila = 0, .autofoco = 1, .barra = TRANSPARENTE };   /* (estética) 0 = el del diseño (18 en el modo v1, como antes) */
static const WSelector d_selector = { .max = 99, .digitos = 2, .escala = 2 };
static const WCampoTexto d_campo = { .lineas = 1 };
static const WIcono d_icono = { .color = TRANSPARENTE, .escala = 1 };
typedef union { Widget base; WFlex f; WExpandido x; WRelleno r; WAlinear a; WCaja c; WPila p; WRejilla j; WLista l; WTexto t; WParrafo pa; WVelo v;
                WBarraDesp b; WProgreso pr; WPintura pi; WListaP lp; WSelector s; WCampoTexto ct; WIcono i; WEnfocable e; WTeclas k; } Cualquiera;
typedef struct { const Clase *cls; const void *def; u16 tam; u8 n; const Atr *a; } Cls;
#define C(Tipo, cls_, def_, atr) { &cls_, def_, sizeof(Tipo), sizeof atr / sizeof atr[0], atr }
static const Cls clases[W5C_N] = {
    [W5C_FLEX] = C(WFlex, ClsWFlex, &d_flex, a_flex), [W5C_EXPANDIDO] = C(WExpandido, ClsWExpandido, &d_expandido, a_expandido),
    [W5C_RELLENO] = C(WRelleno, ClsWRelleno, NULL, a_relleno), [W5C_ALINEAR] = C(WAlinear, ClsWAlinear, NULL, a_alinear),
    [W5C_CAJA] = C(WCaja, ClsWCaja, &d_caja, a_caja), [W5C_PILA] = C(WPila, ClsWPila, NULL, a_pila),
    [W5C_REJILLA] = C(WRejilla, ClsWRejilla, &d_rejilla, a_rejilla), [W5C_LISTA] = C(WLista, ClsWLista, &d_lista, a_lista),
    [W5C_TEXTO] = C(WTexto, ClsWTexto, &d_texto, a_texto), [W5C_PARRAFO] = C(WParrafo, ClsWParrafo, &d_parrafo, a_parrafo),
    [W5C_VELO] = C(WVelo, ClsWVelo, NULL, a_velo), [W5C_BARRA_DESP] = C(WBarraDesp, ClsWBarraDesp, &d_barra, a_barra),
    [W5C_PROGRESO] = C(WProgreso, ClsWProgreso, &d_progreso, a_progreso), [W5C_PINTURA] = C(WPintura, ClsWPintura, NULL, a_pintura),
    [W5C_LISTA_PEREZOSA] = C(WListaP, ClsWListaP, &d_listap, a_listap), [W5C_SELECTOR] = C(WSelector, ClsWSelector, &d_selector, a_selector),
    [W5C_CAMPO_TEXTO] = C(WCampoTexto, ClsWCampoTexto, &d_campo, a_campo), [W5C_ICONO] = C(WIcono, ClsWIcono, &d_icono, a_icono),
    [W5C_ENFOCABLE] = C(WEnfocable, ClsWEnfocable, NULL, a_enfocable), [W5C_TECLAS] = C(WTeclas, ClsWTeclas, NULL, a_teclas),
};

static W5W t_ui_w(unsigned clase, const W5Pal *pares, unsigned n)
{
    Cualquiera w;
    const Cls *k;
    void *fn[3] = { 0, 0, 0 }, *dato = NULL, *nuevo;
    unsigned i, j;
    if (cg_estado() != CG_VIVA) return NULL;
    if (clase < 1 || clase >= W5C_N) MUERE("ui_w: clase de widget que no existe.", NULL);
    if (n > 32 || (n && !cg_legible(pares))) MUERE("ui_w: lista de atributos que no vale.", NULL);
    if (!arena_hay(sizeof w)) return NULL;
    k = &clases[clase];
    memset(&w, 0, sizeof w);
    if (k->def) memcpy(&w, k->def, k->tam);
    for (i = 0; i < n; i++) {
        const unsigned atr = (unsigned)pares[2 * i];
        const W5Pal v = pares[2 * i + 1];
        u8 *d;
        if (atr == W5A_CLAVE) { w.base.clave = (u32)v; continue; }
        for (j = 0; j < k->n && k->a[j].atr != atr; j++) ;
        if (j == k->n) MUERE(ui_fmt("ui_w: el widget %s no tiene el atributo %u.", k->cls->nombre, atr), NULL);
        d = (u8 *)&w + k->a[j].off;
        switch (k->a[j].tipo) {
        case T_I16: { i16 x = (i16)(int)v; memcpy(d, &x, 2); break; }
        case T_U16: { u16 x = (u16)v; memcpy(d, &x, 2); break; }
        case T_U8:  *d = (u8)v; break;
        case T_S8:  *d = (u8)(signed char)(int)v; break;
        case T_I32: { i32 x = (i32)v; memcpy(d, &x, 4); break; }
        case T_COL: { Color x = col((unsigned)v); memcpy(d, &x, sizeof x); break; }
        case T_CAD: { const char *s = (const char *)(size_t)v; if (!s) s = "";
                      if (!cg_legible(s)) MUERE("ui_w: texto fuera de la memoria de la app.", NULL);
                      if (cg_pm_activa() && !(s = dup_cad_sys(s))) return NULL;   /* (AUD9 A5) copia del texto a memoria del sistema */
                      memcpy(d, &s, sizeof s); break; }
        case T_W:   { Widget *h = (Widget *)(size_t)v; if (h && !cg_widget(h)) MUERE("ui_w: el hijo no es un widget de este cuadro.", NULL); memcpy(d, &h, sizeof h); break; }
        case T_WS:  { Widget **h = (Widget **)(size_t)v; unsigned m;
                      if (h) { if (!ui_en_arena(h)) MUERE("ui_w: la lista de hijos no viene de ui_lista.", NULL);
                               for (m = 0; h[m]; m++) if (m > 255 || !cg_widget(h[m])) MUERE("ui_w: un hijo no es un widget de este cuadro.", NULL); }
                      memcpy(d, &h, sizeof h); break; }
        case T_FN0: case T_FN1: case T_FN2: fn[k->a[j].tipo - T_FN0] = (void *)(size_t)v; break;
        case T_DATO: dato = (void *)(size_t)v; break;
        case T_PINT: { void *p = (void *)(size_t)v; if (p && (!cg_en_app(p, 4) || ((size_t)p & 3))) MUERE("ui_w: puntero a un entero fuera de la memoria de la app.", NULL); memcpy(d, &p, sizeof p); break; }
        case T_ET:  { EntradaTexto *e = (EntradaTexto *)(size_t)v;
                      if (cg_pm_activa()) { if (!et_del_deposito(e)) MUERE("ui_w: la entrada de texto no viene de et_nueva.", NULL); }   /* (PM) del depósito del sistema, no fabricada en la app */
                      else if (!e || !cg_en_app(e, sizeof *e) || !cg_en_app(e->buf, e->cap)) MUERE("ui_w: la entrada de texto no viene de et_nueva.", NULL);
                      memcpy(d, &e, sizeof e); break; }
        case T_ICO: { const Icono *ic = (const Icono *)(size_t)v;
                      if (ic && (!cg_legible_n(ic, sizeof *ic) || ic->w > 16 || !ic->h || ic->h > 64 || !cg_legible_n(ic->filas, (size_t)ic->h * sizeof(unsigned short)))) MUERE("ui_w: icono que no vale.", NULL);
                      if (ic && cg_pm_activa()) {            /* (AUD9 A5) copia del icono (cabecera + filas) a memoria del sistema */
                          unsigned short *fl; Icono *c2;
                          if (!arena_hay(sizeof *c2 + (unsigned)ic->h * sizeof(unsigned short))) return NULL;
                          c2 = ui_arena(sizeof *c2); fl = ui_arena((unsigned)ic->h * sizeof(unsigned short));
                          memcpy(fl, ic->filas, (size_t)ic->h * sizeof(unsigned short));
                          *c2 = *ic; c2->filas = fl; ic = c2;
                      }
                      memcpy(d, &ic, sizeof ic); break; }
        }
    }
    /* las clases con retrollamadas: el widget se queda con el trampolín del sistema y con el registro internado */
    switch (clase) {
    case W5C_PINTURA:  if (!fn[0]) MUERE("Pintura sin función de pintar.", NULL);
                       w.pi.pintar = th_pintar; w.pi.dato = cg_ret(fn[0], 0, 0, dato); break;
    case W5C_TECLAS:   if (fn[0]) { w.k.al_tecla = th_tecla; w.k.dato = cg_ret(fn[0], 0, 0, dato); } break;
    case W5C_LISTA_PEREZOSA: if (!fn[0]) MUERE("ListaPerezosa sin función de fila.", NULL);
                       if (w.lp.n < 0 || w.lp.alto_fila < 0) MUERE("ListaPerezosa con medidas que no valen.", NULL);
                       w.lp.fila = th_fila; if (fn[1]) w.lp.al_activar = th_fila_activar; if (fn[2]) w.lp.al_mover = th_fila_mover;
                       w.lp.dato = cg_ret(fn[0], fn[1], fn[2], dato); break;
    case W5C_SELECTOR: if (!w.s.valor) MUERE("Selector sin valor.", NULL);
                       if (fn[0]) { w.s.al_cambiar = th_cambiar; w.s.dato = cg_ret(fn[0], 0, 0, dato); } break;
    case W5C_CAMPO_TEXTO: if (!w.ct.e) MUERE("CampoTexto sin entrada.", NULL);
                       if (fn[0]) { w.ct.al_cambiar = th_cambiar; w.ct.dato = cg_ret(fn[0], 0, 0, dato); } break;
    case W5C_ENFOCABLE: if (!fn[0]) MUERE("Enfocable sin constructor.", NULL);
                       w.e.constructor = th_enfocado; if (fn[1]) w.e.al_activar = th_activar; if (fn[2]) w.e.al_enfocar = th_enfocar;
                       w.e.dato = cg_ret(fn[0], fn[1], fn[2], dato); break;
    case W5C_REJILLA:  if (!w.j.columnas) MUERE("Rejilla sin columnas.", NULL); break;
    case W5C_TEXTO:    if (w.t.escala < 1 || w.t.escala > 8) MUERE("Texto con una escala que no vale.", NULL); break;
    case W5C_ICONO:    if (!w.i.icono) MUERE("Icono sin icono.", NULL); break;
    default: break;
    }
    if (cg_estado() != CG_VIVA) return NULL;
    w.base.cls = k->cls;
    nuevo = ui_nuevo(&w, k->tam);
    return (W5W)nuevo;
}
static W5W *t_ui_lista(const W5W *v, unsigned n)
{
    unsigned i;
    if (cg_estado() != CG_VIVA) return NULL;
    if (n > 255 || (n && !cg_legible(v))) MUERE("ui_lista: lista que no vale.", NULL);
    if (!arena_hay((n + 1) * (u32)sizeof(void *))) return NULL;
    for (i = 0; i < n; i++) if (v[i] && !cg_widget(v[i])) MUERE("ui_lista: un elemento no es un widget de este cuadro.", NULL);
    return (W5W *)ui_lista((Widget *const *)v, n);
}
static void *t_ui_arena(unsigned n) { if (cg_estado() != CG_VIVA || n > 8192) return NULL; if (cg_pm_activa()) return cg_arena_app(n); if (!arena_hay(n)) return NULL; return ui_arena(n); }   /* (PM) con protección, de SU memoria: la del sistema la app solo la lee */
/* ui_fmt va DIRECTA (es de argumentos variables): antes de cada construir() el cargador ya ha comprobado el margen,
 * y un resultado no pasa de 320 bytes. */
static W5W t_ui_pantalla(W5Construir construir, W5Vida iniciar, W5Vida desechar, void *dato)
{
    if (cg_estado() != CG_VIVA || !arena_hay(64)) return NULL;
    if (cg_perfil() != W5_PERFIL_WIDGETS) MUERE("ui_pantalla en una app de perfil juego.", NULL);
    return (W5W)cg_pantalla((void *)construir, (void *)iniciar, (void *)desechar, dato);
}
static W5W t_ui_andamio(const char *titulo, W5W cuerpo, const char *izq, const char *der)
{
    if (cg_estado() != CG_VIVA || !arena_hay(1024)) return NULL;
    if (!cg_cadena(titulo) || !cg_cadena(izq) || !cg_cadena(der) || !cg_widget(cuerpo)) MUERE("ui_andamio: argumentos que no valen.", NULL);
    return (W5W)Pantalla(titulo ? titulo : "", (Widget *)cuerpo, izq ? izq : "", der ? der : "");
}
static W5W t_ui_fila(const char *texto, const char *detalle, int sel)
{
    if (cg_estado() != CG_VIVA || !arena_hay(512)) return NULL;
    if (!texto || !cg_legible(texto) || !cg_cadena(detalle)) MUERE("ui_fila: texto que no vale.", NULL);
    return (W5W)sh_fila(texto, detalle, sel);
}
static W5W t_ui_dato(const char *nombre, const char *valor)
{
    if (cg_estado() != CG_VIVA || !arena_hay(512)) return NULL;
    if (!nombre || !valor || !cg_legible(nombre) || !cg_legible(valor)) MUERE("ui_dato: texto que no vale.", NULL);
    return (W5W)sh_dato(nombre, valor);
}
static unsigned t_ui_color(int rol)
{
    const TemaDatos *t = sh_tema();
    const Color v[W5_COL_N] = { t->fondo, t->texto, t->tenue, t->acento, t->sobre_acento, t->barra };
    return rol >= 0 && rol < W5_COL_N ? col_a_rgb(v[rol]) : 0;
}
static void t_ui_sucio(W5Ctx c) { CTX(c, ); ui_sucio((Ctx *)c); }
static W5Ctx t_ui_mi_pantalla(W5Ctx c) { CTX(c, NULL); return (W5Ctx)cg_mi_pantalla((Ctx *)c); }
static int t_ui_cada(W5Ctx c, unsigned ms, W5Retro f)
{
    Retro r;
    CTX(c, W5_ERROR);
    if (ms < 10) ms = 10;                                    /* una app no late más deprisa que 100 Hz */
    if (!(r = cg_retro(f, 1))) return W5_ERROR;
    ui_cancelar((Ctx *)c, r);                                /* la misma función en el mismo contexto: se sustituye, no se apila */
    ui_cada((Ctx *)c, ms, r);
    return W5_OK;
}
static int t_ui_tras(W5Ctx c, unsigned ms, W5Retro f)
{
    Retro r;
    CTX(c, W5_ERROR);
    if (!(r = cg_retro(f, 1))) return W5_ERROR;
    ui_cancelar((Ctx *)c, r);
    ui_tras((Ctx *)c, ms, r);
    return W5_OK;
}
static void t_ui_cancelar(W5Ctx c, W5Retro f) { Retro r; CTX(c, ); if ((r = cg_retro(f, 0)) != NULL) ui_cancelar((Ctx *)c, r); }
static void t_ui_empujar(W5W p)
{
    Widget *w = cg_widget(p);
    if (cg_estado() != CG_VIVA) return;
    if (!w || w->cls != &ClsWCgPant) MUERE("ui_empujar: eso no es una pantalla hecha con ui_pantalla.", );
    ui_empujar(w);
}
static void t_ui_sacar(void) { if (cg_estado() == CG_VIVA && cg_perfil() == W5_PERFIL_WIDGETS) ui_sacar(); }
static void t_ui_tecla(int k) { if (cg_estado() == CG_VIVA && cg_perfil() == W5_PERFIL_WIDGETS && k > K_NADA && k < K_N) ui_tecla(k); }
static int t_ui_teclas_defecto(W5Ctx c, int k)
{
    CTX(c, 0);
    if (k == K_ATRAS) { ui_sacar(); return 1; }
    return sh_teclas((Ctx *)c, k, NULL);
}
static void t_ui_aviso(const char *titulo, const char *texto)
{
    if (cg_estado() != CG_VIVA || cg_perfil() != W5_PERFIL_WIDGETS) return;
    if (!titulo || !texto || !cg_legible(titulo) || !cg_legible(texto)) MUERE("ui_aviso: texto que no vale.", );
    ui_aviso(titulo, texto);
}
static void t_ui_confirmar(const char *titulo, const char *texto, W5Si si, void *dato)
{
    if (cg_estado() != CG_VIVA || cg_perfil() != W5_PERFIL_WIDGETS) return;
    if (!titulo || !texto || !cg_legible(titulo) || !cg_legible(texto) || !si) MUERE("ui_confirmar: argumentos que no valen.", );
    ui_confirmar(titulo, texto, th_si, cg_ret((void *)si, 0, 0, dato));
}
static void t_ui_opciones(const char *titulo, const char *const *ops, int n, W5Elegir elegir, void *dato)
{
    int i;
    if (cg_estado() != CG_VIVA || cg_perfil() != W5_PERFIL_WIDGETS) return;
    if (!titulo || !cg_legible(titulo) || n < 1 || n > 12 || !cg_en_app(ops, (size_t)n * sizeof *ops) || !elegir) MUERE("ui_opciones: argumentos que no valen.", );
    for (i = 0; i < n; i++) if (!ops[i] || !cg_en_app(ops[i], 1)) MUERE("ui_opciones: una opción no es una cadena de la app.", );
    ui_opciones(titulo, ops, n, th_elegir, cg_ret((void *)elegir, 0, 0, dato));
}

/* ---- el lienzo: las del framework, con el color traducido ----------------------------------------------------------- */
#define L(l) ((Lienzo *)(l))
static void t_li_rect(W5Lienzo l, int x, int y, int w, int h, unsigned c) { if (li_vale(l)) li_rect(L(l), x, y, w, h, col(c)); }
static void t_li_marco(W5Lienzo l, int x, int y, int w, int h, unsigned c) { if (li_vale(l)) li_marco(L(l), x, y, w, h, col(c)); }
static void t_li_linea_h(W5Lienzo l, int x, int y, int w, unsigned c) { if (li_vale(l)) li_linea_h(L(l), x, y, w, col(c)); }
static void t_li_punto(W5Lienzo l, int x, int y, unsigned c) { if (li_vale(l)) li_punto(L(l), x, y, col(c)); }
static int escala_vale(int e) { return e >= 1 && e <= 8; }
/* (1.7) las marcas que llegan al lienzo: hasta la 1.6 solo F_MONO y F_BASTA (lo de siempre); desde la 1.7, también el estilo (bits 4..6) */
static int mk(int m) { return m & (cg_api_menor() >= W5_PANT_MENOR ? 0x73 : 3); }
static void t_li_texto(W5Lienzo l, int x, int y, const char *s, unsigned c, int e, int m) { if (li_vale(l) && s && cg_legible(s) && escala_vale(e)) li_texto_f(L(l), x, y, s, col(c), e, mk(m)); }
static void t_li_texto_n(W5Lienzo l, int x, int y, const char *s, int n, unsigned c, int e, int m) { if (li_vale(l) && s && n > 0 && cg_legible_n(s, (size_t)n) && escala_vale(e)) li_texto_n(L(l), x, y, s, n, col(c), e, mk(m)); }
static int t_li_ancho_texto(const char *s, int e, int m) { return s && cg_legible(s) && escala_vale(e) ? li_ancho_texto_f(s, e, mk(m)) : 0; }
static int t_li_partir(const char *s, int ancho, int e, int m) { return s && cg_legible(s) && escala_vale(e) ? li_partir(s, ancho, e, mk(m)) : 0; }
static void t_li_velo(W5Lienzo l, int x, int y, int w, int h) { if (li_vale(l)) li_velo(L(l), x, y, w, h); }
static void t_li_icono(W5Lienzo l, int x, int y, const W5Icono *i, unsigned c, int e)
{
    Icono ic;
    if (!li_vale(l) || !i || !cg_legible_n(i, sizeof *i) || i->w > 16 || !i->h || i->h > 64 || !i->filas || !cg_legible_n(i->filas, (size_t)i->h * sizeof(unsigned short)) || !escala_vale(e)) return;
    ic = *(const Icono *)i;                                  /* (AUD9 A5) copia de la cabecera; las filas se copian abajo si hace falta */
    li_icono(L(l), x, y, &ic, col(c), e);
}
typedef char w5_icono_es_icono[sizeof(W5Icono) == sizeof(Icono) && offsetof(W5Icono, filas) == offsetof(Icono, filas) ? 1 : -1];

/* ---- la entrada de texto -------------------------------------------------------------------------------------------------
 * Sin protección vive en el montón de la app (la app no ve qué hay dentro: es un manejador opaco). CON protección NO puede
 * vivir ahí: el framework (en modo sistema) escribe por `buf` en cada tecla, y la app podría cambiar `buf` entre cuadros para
 * que el sistema escribiese donde ella quisiera. Por eso con protección la entrada y su búfer viven en un DEPÓSITO DEL SISTEMA
 * (cg_et_dep): el manejador que la app recibe es opaco; el texto se le COPIA a su arena en et_texto. */
#define CG_ET_DEP 3
#define CG_ET_CAP 4096
static EntradaTexto cg_et_dep[CG_ET_DEP];
static char cg_et_dep_buf[CG_ET_DEP][CG_ET_CAP];
static u8  cg_et_dep_usada[CG_ET_DEP];
void cg_et_reset(void) { unsigned i; for (i = 0; i < CG_ET_DEP; i++) cg_et_dep_usada[i] = 0; }   /* al descargar la app (shell/cargador.c) */
static int et_del_deposito(const EntradaTexto *t) { return t >= cg_et_dep && t < cg_et_dep + CG_ET_DEP && cg_et_dep_usada[t - cg_et_dep]; }
static EntradaTexto *et_de(W5Et e)
{
    EntradaTexto *t = (EntradaTexto *)e;
    if (cg_pm_activa()) {                                    /* (PM) solo del depósito del sistema */
        if (!et_del_deposito(t) || t->buf != cg_et_dep_buf[t - cg_et_dep] || t->len >= t->cap || t->cur > t->len) { cg_matar("La app dio una entrada de texto que no lo es."); return NULL; }
        return t;
    }
    if (!t || !cg_en_app(t, sizeof *t) || !cg_en_app(t->buf, t->cap) || t->len >= t->cap || t->cur > t->len) { cg_matar("La app dio una entrada de texto que no lo es."); return NULL; }
    return t;
}
EntradaTexto *cg_et_de(W5Et e) { return et_de(e); }   /* (INT2) el mismo control para la 1.2 (shell/cg12_c.c: et_poner_modo) */
static W5Et t_et_nueva(unsigned cap)
{
    EntradaTexto *t;
    if (cap < 2) return NULL;
    if (cg_pm_activa()) {                                    /* (PM) del depósito del sistema: la app no escribe ahí */
        int i;
        if (cap > CG_ET_CAP) cap = CG_ET_CAP;
        for (i = 0; i < CG_ET_DEP; i++) if (!cg_et_dep_usada[i]) break;
        if (i == CG_ET_DEP) return NULL;
        cg_et_dep_usada[i] = 1;
        et_iniciar(&cg_et_dep[i], cg_et_dep_buf[i], cap);
        return (W5Et)&cg_et_dep[i];
    }
    if (cap > 16384 || !(t = cg_monton_pedir((u32)sizeof *t + cap))) return NULL;
    et_iniciar(t, (char *)(t + 1), cap);
    return (W5Et)t;
}
static void t_et_poner(W5Et e, const char *s) { EntradaTexto *t = et_de(e); if (t && s && cg_legible(s)) et_poner(t, s); }
static const char *t_et_texto(W5Et e)
{
    EntradaTexto *t = et_de(e);
    if (!t) return "";
    if (cg_pm_activa()) {                                    /* (PM) el búfer es del sistema: una copia en la arena de la app */
        char *c = cg_arena_app(t->len + 1); u32 i;
        if (!c) return NULL;
        for (i = 0; i < t->len; i++) c[i] = t->buf[i];
        c[t->len] = 0;
        return c;
    }
    return t->buf;
}
static unsigned t_et_len(W5Et e) { EntradaTexto *t = et_de(e); return t ? t->len : 0; }
static int t_et_insertar(W5Et e, const char *s) { EntradaTexto *t = et_de(e); return t && s && cg_legible(s) ? et_insertar(t, s) : 0; }
static int t_et_borrar(W5Et e) { EntradaTexto *t = et_de(e); return t ? et_borrar(t) : 0; }
static const char *t_et_modo(W5Et e)
{
    EntradaTexto *t = et_de(e);
    const char *m = t ? et_modo_nombre(t) : "";
    if (cg_pm_activa()) {                                    /* (PM) es una constante del sistema, que la app no puede leer: una copia en SU memoria */
        char *c = cg_arena_app(8); size_t i;
        if (!c) return NULL;
        for (i = 0; i < 7 && m[i]; i++) c[i] = m[i];
        return c;
    }
    return m;
}

/* ---- la app y el aparato ---------------------------------------------------------------------------------------------- */
static int t_ap_consulta(int que)
{
    switch (que) {
    case W5_Q_API: return (W5_API_MAYOR << 16) | CG_API_MENOR;     /* (BT) 1.4; (1.5) sonido y vibrador */
    case W5_Q_ANCHO: return UI_ANCHO;
    case W5_Q_ALTO: return UI_ALTO;
    case W5_Q_CAPS: return (int)CG_CAPS;
    case W5_Q_TECLAS_A: return (int)HW_TECLAS_A;
    case W5_Q_TECLAS_B: return (int)HW_TECLAS_B;
    case W5_Q_ALTO_LINEA: return dis_paso_linea(0);              /* (1.7) el de la letra en uso: 10 en el modo v1 (lo de siempre), 24 con la nueva */
    case W5_Q_MODELO: return HW_MODELO;
    case W5_Q_PRIMER_PLANO: return cg_primer_plano();
    case W5_Q_PILA_LIBRE: return (int)cg_pila_libre();
    case W5_Q_ACEL_1G: return HW_ACEL_1G;
    case W5_Q_PIXEL: return W5_PIX_RGB565_BE;
    case W5_Q_AVISOS: return app_avisos() & 7;
    }
    return -1;
}
static void t_ap_log(const char *s)
{
    unsigned n = 0;
    if (!s || !cg_legible(s)) return;
    hw_log("[app] "); hw_log(s);
    while (s[n]) n++;
    if (!n || s[n - 1] != '\n') hw_log("\n");     /* una línea por llamada: el registro se filtra por líneas */
}
static int t_ap_fecha(W5Fecha *f)
{
    HwFecha h;
    int r;
    if (!cg_en_app(f, sizeof *f)) return W5_ERROR;
    r = sh_ahora(&h);
    f->anio = h.anio; f->mes = h.mes; f->dia = h.dia; f->hora = h.hora; f->min = h.min; f->seg = h.seg;
    f->dia_semana = (unsigned char)hw_dia_semana(h.anio, h.mes, h.dia);
    return r == HW_OK ? W5_OK : W5_ERROR;
}
static char *t_ap_hora_texto(int con_segundos) { HwFecha h; if (!arena_hay(64)) return NULL; sh_ahora(&h); return sh_hora(&h, con_segundos); }
static void *t_ap_mem_pedir(unsigned n) { return cg_monton_pedir(n); }

/* (28-oct) los de una app de la TARJETA, en la tarjeta junto a su .sen (cg_fich_tarjeta); los demás, en Mis archivos como siempre */
static int t_ap_fich_tam(const char *nombre)
{
    const char *t = cg_fich_tarjeta(nombre), *n;
    u32 x;
    if (t) return hw_fs_mirar(t, &x) == HW_OK ? (int)x : W5_NO_DISPONIBLE;
    n = cg_nombre_fich(nombre);
    return n ? hw_usr_tam(n) : W5_ERROR;
}
static int t_ap_fich_leer(const char *nombre, unsigned desde, void *buf, unsigned max)
{
    const char *t = cg_fich_tarjeta(nombre), *n = cg_nombre_fich(nombre);
    u32 x;
    if (!n || !cg_en_app(buf, max)) return W5_ERROR;
    if (t) return hw_fs_mirar(t, &x) != HW_OK ? W5_NO_DISPONIBLE : (int)hw_fs_leer_desde(t, desde, buf, max);
    return hw_usr_leer(n, desde, buf, max);
}
static int t_ap_fich_escribir(const char *nombre, const void *d, unsigned n_)
{
    const char *t = cg_fich_tarjeta(nombre), *n = cg_nombre_fich(nombre);
    if (!n || (n_ && !cg_en_app(d, n_))) return W5_ERROR;
    if (t) return hw_fs_escribir(t, (const u8 *)d, n_) == HW_OK ? W5_OK : W5_ERROR;   /* la tarjeta: sin el tope de la flash */
    if (!cg_escritura_permitida()) return W5_ERROR;          /* el desgaste de la flash: 20 escrituras por minuto */
    return hw_usr_escribir(n, d, n_);
}
static int t_ap_fich_borrar(const char *nombre)
{
    const char *t = cg_fich_tarjeta(nombre), *n = cg_nombre_fich(nombre);
    if (!n) return W5_ERROR;
    if (t) return hw_fs_borrar(t) == HW_OK ? W5_OK : W5_NO_DISPONIBLE;
    if (!cg_escritura_permitida()) return W5_ERROR;
    return hw_usr_borrar(n);
}
static int t_fs_tam(const char *ruta) { u32 t; if (!ruta || !cg_legible(ruta)) return W5_ERROR; t = hw_fs_tam(ruta); return t ? (int)t : W5_NO_DISPONIBLE; }
static int t_fs_leer(const char *ruta, unsigned desde, void *buf, unsigned max)
{
    if (!ruta || !cg_legible(ruta) || !cg_en_app(buf, max)) return W5_ERROR;
    return (int)hw_fs_leer_desde(ruta, desde, buf, max);
}
static int t_ap_tecla_abajo(int k) { return k > K_NADA && k < K_N ? hw_tecla_abajo(k) : 0; }
static int t_ap_acel_leer(int *x, int *y)
{
    int a, b, r;
    if (!cg_en_app(x, sizeof *x) || !cg_en_app(y, sizeof *y)) return W5_ERROR;
    if (!(HW_CAPS & W5_CAP_ACELEROMETRO)) return W5_NO_DISPONIBLE;
    /* (juego de carreras) la ÚLTIMA MUESTRA VÁLIDA del servicio del podómetro (shell/pasos.c), que es quien alimenta el sensor: lo
     * enciende ~20 ms antes de cada muestra y lo apaga después. Leer el ADC directamente a otra hora daba el sensor
     * APAGADO (0 crudo = -125) en ~48 % de las lecturas [observado]. Con ap_acel(1)
     * el servicio está retenido y muestrea cada 40 ms sin modo vigilar. Sin ninguna muestra todavía, como antes. */
    {   PasosInfo i;
        sh_pasos_info(&i);
        if (i.muestras && i.hay_sensor) { *x = (int)i.x - HW_ACEL_CERO; *y = (int)i.y - HW_ACEL_CERO; return W5_OK; }
    }
    r = hw_acel_leer(&a, &b);
    if (r != HW_OK) return W5_NO_DISPONIBLE;
    *x = a - HW_ACEL_CERO; *y = b - HW_ACEL_CERO;
    return W5_OK;
}
static int t_ap_vibrar(int ms) { if (!(HW_CAPS & W5_CAP_VIBRADOR)) return W5_NO_DISPONIBLE; return hw_vibrar(ms < 0 ? 0 : ms > 1000 ? 1000 : ms) == HW_OK ? W5_OK : W5_NO_DISPONIBLE; }
static int t_ap_tono(unsigned hz, unsigned ms)
{
    int ancho = sh_sonido_ancho();
    if (!ancho) return W5_OK;                                /* en silencio: calla, y no es un error */
    if (ms > 2000) ms = 2000;
    return hw_tono_ms(hz, ancho, ms) == HW_OK ? W5_OK : W5_ERROR;
}
static int t_ap_sonar(int cual) { if (cual != W5_SN_TECLA && cual != W5_SN_ERROR && cual != W5_SN_AVISO) return W5_ERROR; sh_sonar(cual); return W5_OK; }
static int t_ap_bateria(void) { int p = hw_bateria_pct(); return p < 0 ? W5_NO_DISPONIBLE : p; }
typedef char w5_sonidos[(int)W5_SN_TECLA == (int)SN_TECLA && (int)W5_SN_ERROR == (int)SN_ERROR && (int)W5_SN_AVISO == (int)SN_PRUEBA ? 1 : -1];
typedef char w5_teclas[(int)W5K_N == (int)K_N && (int)W5K_ENCENDIDO == (int)K_ENCENDIDO && (int)W5K_ALM == (int)K_ALM ? 1 : -1];   /* los códigos del framework SON los abstractos */

/* ---- recursos ---------------------------------------------------------------------------------------------------------- */
static int t_ap_rec_leer(unsigned desde, void *buf, unsigned max) { return cg_rec_leer(desde, buf, max); }

/* ---- PERFIL JUEGO: el contrato de api/app_externa.h, por la tabla -------------------------------------------------------- */
static u8 jg_panel;                                    /* (1.1) W5_JG_PANEL tomado */
static void *t_jg_tomar(int modo, int *an, int *al, int *paso)
{
    AppMarco m;
    if (cg_perfil() != W5_PERFIL_JUEGO) MUERE("jg_tomar en una app que no es de perfil juego.", NULL);
    if (!cg_en_app(an, sizeof *an) || !cg_en_app(al, sizeof *al) || !cg_en_app(paso, sizeof *paso)) return NULL;
    if (modo != W5_JG_BANDAS && modo != W5_JG_GIRADA && modo != W5_JG_PANEL) return NULL;
    if (modo == W5_JG_PANEL && cg_api_menor() < 1) return NULL;          /* (1.1) */
    if (app_pantalla_tomar(&m, modo == W5_JG_GIRADA ? APP_GIRADA : APP_BANDAS) != HW_OK) return NULL;
    jg_panel = modo == W5_JG_PANEL;                      /* (1.1) el panel entero (ui_pantalla), sin copia: tomado en bandas */
    if (jg_panel) { m.pix = ui_pantalla; m.an = UI_ANCHO; m.al = UI_ALTO; m.paso = UI_ANCHO; }
    *an = m.an; *al = m.al; *paso = m.paso;
    if (cg_pm_activa()) {
        /* (AUD9 C3) bandas: el marco cae DENTRO de ui_pantalla; se abre el búfer ENTERO, alineado a 4 KiB y sin .bss vecino
         * en los bordes (ui_nucleo.c). Girado: su propio búfer, que tiene que venir alineado a página; si no, cg_pm_abrir lo
         * rechaza y el juego no toma la pantalla (en vez de abrir una página con las tablas de la ventana de M5X). */
        const void *base = (modo == W5_JG_GIRADA) ? (const void *)m.pix : (const void *)ui_pantalla;
        u32 bytes = (modo == W5_JG_GIRADA) ? (((u32)m.al * (u32)m.paso * (u32)sizeof(Color) + 4095u) & ~4095u) : ui_pantalla_tam;
        if (!cg_pm_abrir(base, bytes, 1)) { app_pantalla_soltar(); MUERE("jg_tomar: el marco no está alineado a 4 KiB (no se puede aislar).", NULL); }
    }
    return m.pix;
}
static void t_jg_volcar(void) { if (cg_perfil() != W5_PERFIL_JUEGO) return; if (cg_pm_activa()) cg_arena_reset(); if (jg_panel) app_pantalla_volcar_entera(); else app_pantalla_volcar(); }   /* (AUD9 A4) nuevo fotograma: la arena de la app se vacía (ui_fmt por cuadro no la agota) */
static void t_jg_soltar(void)
{
    if (cg_perfil() != W5_PERFIL_JUEGO) return;
    if (cg_pm_activa()) cg_pm_cerrar(ui_pantalla, ui_pantalla_tam);   /* (AUD9) retira los permisos del panel al soltarlo (no solo al descargar) */
    jg_panel = 0;
    app_pantalla_soltar();
}
static void t_jg_espera(void) { if (cg_perfil() == W5_PERFIL_JUEGO) hw_reposo(); }
static int t_jg_teclas(unsigned *a, unsigned *b)
{
    int k, n = 0;
    if (!cg_en_app(a, sizeof *a) || !cg_en_app(b, sizeof *b)) return 0;
    *a = *b = 0;
    for (k = 1; k < K_N; k++) if (hw_tecla_abajo(k)) { if (k < 32) *a |= 1u << k; else *b |= 1u << (k - 32); n++; }
    return n;
}
typedef char w5_nota[sizeof(W5Nota) == sizeof(AppNota) ? 1 : -1];
static int t_jg_melodia(const W5Nota *notas, unsigned n, int repetir)
{
    if (cg_perfil() != W5_PERFIL_JUEGO) return W5_ERROR;
    if (notas && (!n || n > 65536 || !cg_en_app(notas, (size_t)n * sizeof *notas))) return W5_ERROR;
    return app_melodia((const AppNota *)notas, notas ? n : 0, repetir) == HW_OK ? W5_OK : W5_ERROR;
}

/* (AUD9 A4) ui_fmt de la tabla: valida los punteros de %s (si no, «(?)»: nada de lectura arbitraria de la RAM del sistema ni de
 * registros de E/S que se borran al leerse), acota a 320 B y NO entra en pánico: el resultado va a memoria legible por la app
 * (su arena con PM) y, si no cabe, devuelve NULL (los widgets y ap_log tratan NULL como «sin texto»). La `ui_fmt` cruda del
 * sistema no se toca; esta es solo la que ve la app. */
static char *t_ui_fmt(const char *f, ...)
{
    char tmp[320]; unsigned n = 0; va_list ap; char *out;
    if (cg_estado() != CG_VIVA || !f || !cg_legible(f)) return NULL;
    va_start(ap, f);
    for (; *f && cg_legible(f) && n < sizeof tmp - 12; f++) {
        int cero = 0, ancho = 0;
        if (*f != '%') { tmp[n++] = *f; continue; }
        f++;
        if (*f == '0') { cero = 1; f++; }
        while (*f >= '0' && *f <= '9') ancho = ancho * 10 + (*f++ - '0');
        if (!*f) break;
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s || !cg_legible(s)) { const char *q = "(?)"; while (*q && n < sizeof tmp - 12) tmp[n++] = *q++; }   /* (A4) puntero que no es de la app */
            else while (*s && cg_legible(s) && n < sizeof tmp - 12) tmp[n++] = *s++;
        } else if (*f == 'c') tmp[n++] = (char)va_arg(ap, int);
        else if (*f == '%') tmp[n++] = '%';
        else {
            char d[11]; int k = 0, neg = 0; unsigned base = (*f == 'x' || *f == 'X') ? 16 : 10;
            u32 v = va_arg(ap, u32);
            if (*f == 'd' && (i32)v < 0) { neg = 1; v = 0u - v; }
            do { d[k++] = "0123456789ABCDEF"[v % base]; v /= base; } while (v);
            if (neg && cero) { tmp[n++] = '-'; ancho--; }
            else if (neg) d[k++] = '-';
            for (; ancho > k && n < sizeof tmp - 12; ancho--) tmp[n++] = cero ? '0' : ' ';
            while (k) tmp[n++] = d[--k];
        }
    }
    va_end(ap);
    tmp[n++] = 0;
    out = cg_pm_activa() ? cg_arena_app(n) : ui_arena(n);     /* (A4) a memoria legible por la app, SIN pánico */
    if (!out) return NULL;
    memcpy(out, tmp, n);
    return out;
}

/* (1.2) ap_jit_sincronizar (77) y fs_listar (78), de la pista JIT: en shell/cg12_d.c y shell/cg12_c.c (INT2, ronda 2) */

/* ---- la tabla ------------------------------------------------------------------------------------------------------------- */
#define t_memcpy memcpy
#define t_memset memset
#define t_memmove memmove
#define t_memcmp memcmp
#define t_strlen strlen
#define t_strcmp strcmp
#define t_ap_salir cg_salir
#define t_ap_latido cg_latido
#define t_ap_mem_soltar cg_monton_soltar
#define t_ap_mem_libre cg_monton_libre
#define t_ap_ms hw_ms
#define t_ap_acel cg_acel
/* t_ui_fmt: función validada arriba (AUD9 A4) */
#define t_ap_rec_tam cg_rec_tam
#define t_ap_rec_mapa cg_rec_mapa
#define t_ap_rec_comprobar cg_rec_comprobar
#define W5_PROTO12(r, n, a) r t_##n a;   /* (1.2) las envolturas de shell/cg12_*.c */
W5_API12_0(W5_PROTO12) W5_API12_A(W5_PROTO12) W5_API12_B(W5_PROTO12) W5_API12_C(W5_PROTO12) W5_API12_D(W5_PROTO12) W5_API13(W5_PROTO12)
W5_API14(W5_PROTO12)                    /* (1.4, pista BT) shell/cg_bt.c */
W5_API15(W5_PROTO12)                    /* (1.5) shell/cg15.c */
W5_API16(W5_PROTO12)                    /* (1.6) shell/cg_red.c */
W5_API17(W5_PROTO12)                    /* (1.7) shell/cg17.c */
W5_API18(W5_PROTO12)                    /* (1.8) shell/cg18.c */
W5_API19(W5_PROTO12)                    /* (1.9) shell/cg19.c */
W5_API110(W5_PROTO12)                   /* (1.10) shell/cg110.c */
W5_API111(W5_PROTO12)                   /* (1.11) shell/cg111.c */
#define W5_CUENTA(r, n, a) +1
const CgApi cg_api_todo = { {
    W5_API_MAGIA, W5_API_MAYOR, CG_API_MENOR, 0 W5_API(W5_CUENTA) W5_API14(W5_CUENTA) W5_API15(W5_CUENTA) W5_API16(W5_CUENTA) W5_API17(W5_CUENTA) W5_API18(W5_CUENTA) W5_API19(W5_CUENTA) W5_API110(W5_CUENTA) W5_API111(W5_CUENTA), sizeof(void *),
#define W5_PONER(r, n, a) t_##n,
    W5_API(W5_PONER) },
    W5_API14(W5_PONER)                  /* (1.4) detrás de la 1.3: casilla W5_BT_INDICE de bt/sdk/w5_bt.h */
    W5_API15(W5_PONER)                  /* (1.5) detrás del Bluetooth: casilla W5_SON_INDICE de cargables/sdk/w5_sonido.h */
    W5_API16(W5_PONER)                  /* (1.6) detrás del sonido: casilla W5_RED_INDICE de cargables/sdk/w5_red.h */
    W5_API17(W5_PONER)                  /* (1.7) detrás de la red: casilla W5_PANT_INDICE de cargables/sdk/w5_pantalla.h */
    W5_API18(W5_PONER)                  /* (1.8) detrás de la pantalla: casilla W5_MURED_INDICE de cargables/sdk/w5_musica_red.h */
    W5_API19(W5_PONER)                  /* (1.9) detrás de la música por la red: casilla W5_CAM_INDICE de cargables/sdk/w5_camara.h */
    W5_API110(W5_PONER)                 /* (1.10) detrás de la cámara: casilla W5_MIC_INDICE de cargables/sdk/w5_microfono.h */
    W5_API111(W5_PONER)                 /* (1.11) detrás del micrófono: casilla W5_DESC_INDICE de cargables/sdk/w5_descargas.h */
};
/* la tabla y los grupos, PEGADOS: la app indexa w5 como un vector de punteros (W5ApiBt de w5_bt.h, W5ApiSon de w5_sonido.h) */
typedef char cg_bt_pegado[offsetof(CgApi, bt_encender) == sizeof(W5Api) ? 1 : -1];
#define W5_CUENTA14(r, n, a) +1
typedef char cg_son_pegado[offsetof(CgApi, mu_tocar_ruta) == sizeof(W5Api) + (0 W5_API14(W5_CUENTA14)) * sizeof(void *) &&
                           (0 W5_API14(W5_CUENTA14)) == 15 ? 1 : -1];   /* w5_sonido.h cuenta 15 del Bluetooth (W5_SON_N14) */
typedef char cg_red_pegado[offsetof(CgApi, red_conectar) == sizeof(W5Api) + (0 W5_API14(W5_CUENTA14) W5_API15(W5_CUENTA14)) * sizeof(void *) &&
                           (0 W5_API15(W5_CUENTA14)) == 2 ? 1 : -1];    /* (1.6) w5_red.h cuenta 15 del Bluetooth y 2 del sonido */
typedef char cg_pant_pegado[offsetof(CgApi, ui_paso_linea) == sizeof(W5Api) + (0 W5_API14(W5_CUENTA14) W5_API15(W5_CUENTA14) W5_API16(W5_CUENTA14)) * sizeof(void *) &&
                            (0 W5_API16(W5_CUENTA14)) == 18 ? 1 : -1];  /* (1.7) w5_pantalla.h cuenta 15 + 2 + 18 */
typedef char cg_mured_pegado[offsetof(CgApi, mu_red_vaciar) == sizeof(W5Api) + (0 W5_API14(W5_CUENTA14) W5_API15(W5_CUENTA14) W5_API16(W5_CUENTA14)
                             W5_API17(W5_CUENTA14)) * sizeof(void *) && (0 W5_API17(W5_CUENTA14)) == 4 ? 1 : -1];   /* (1.8) w5_musica_red.h cuenta 15 + 2 + 18 + 4 */
typedef char cg_cam_pegado[offsetof(CgApi, cam_abrir) == sizeof(W5Api) + (0 W5_API14(W5_CUENTA14) W5_API15(W5_CUENTA14) W5_API16(W5_CUENTA14)
                           W5_API17(W5_CUENTA14) W5_API18(W5_CUENTA14)) * sizeof(void *) && (0 W5_API18(W5_CUENTA14)) == 6 ? 1 : -1];   /* (1.9) w5_camara.h cuenta 15 + 2 + 18 + 4 + 6 */
typedef char cg_mic_pegado[offsetof(CgApi, mic_abrir) == sizeof(W5Api) + (0 W5_API14(W5_CUENTA14) W5_API15(W5_CUENTA14) W5_API16(W5_CUENTA14)
                           W5_API17(W5_CUENTA14) W5_API18(W5_CUENTA14) W5_API19(W5_CUENTA14)) * sizeof(void *) && (0 W5_API19(W5_CUENTA14)) == 6 ? 1 : -1];   /* (1.10) w5_microfono.h cuenta 15 + 2 + 18 + 4 + 6 + 6 */
typedef char cg_desc_pegado[offsetof(CgApi, mu_red_descargar) == sizeof(W5Api) + (0 W5_API14(W5_CUENTA14) W5_API15(W5_CUENTA14) W5_API16(W5_CUENTA14)
                            W5_API17(W5_CUENTA14) W5_API18(W5_CUENTA14) W5_API19(W5_CUENTA14) W5_API110(W5_CUENTA14)) * sizeof(void *) && (0 W5_API110(W5_CUENTA14)) == 5 ? 1 : -1];   /* (1.11) w5_descargas.h cuenta 15 + 2 + 18 + 4 + 6 + 6 + 5 */
