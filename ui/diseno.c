/* diseno.c — (MD) la capa 2 del sistema de diseño: qué diseño manda, sus tokens, los atajos a sus componentes y las
 * primitivas que comparten (esquinas redondeadas suavizadas). docs/material.md §1. */
#include "diseno.h"

static const DisenoTabla *const disenos[] = { &dis_material3, &dis_clasico };
#define N_DIS ((int)(sizeof disenos / sizeof disenos[0]))
static const DisenoTabla *del_sistema = &dis_material3, *forzado;
static u8 paleta_sistema;
static u8 modo_v1;                       /* (estética) una .sen de API < 1.7 en pantalla: letra y medidas de antes */
static DisTokens tk;
static TemaDatos tema_compat;
static u8 listo;

static void calcular(void)
{
    const DisenoTabla *d = dis_actual();
    int p = paleta_sistema < d->n_paletas ? paleta_sistema : 0;
    d->tokens(p, &tk);
    if (modo_v1) { u8 o = tk.m.oscuro; tk.m = d->medidas_v1; tk.m.oscuro = o; }   /* colores de siempre; medidas de antes de la escala nueva */
    /* los seis colores de las apps 1.0–1.2 (W5_COL_*): sus roles, sacados de los tokens */
    tema_compat.fondo = tk.c.superficie;
    tema_compat.texto = tk.c.sobre_superficie;
    tema_compat.tenue = tk.c.sobre_superficie_variante;
    tema_compat.acento = tk.c.primario;
    tema_compat.sobre_acento = tk.c.sobre_primario;
    tema_compat.barra = tk.c.superficie_contenedor;
    listo = 1;
}
const DisenoTabla *dis_actual(void) { return forzado ? forzado : del_sistema; }
const DisTokens *dis_tokens(void) { if (!listo) calcular(); return &tk; }
const TemaDatos *dis_tema(void) { if (!listo) calcular(); return &tema_compat; }
int dis_n(void) { return N_DIS; }
const DisenoTabla *dis_de(int i) { return i >= 0 && i < N_DIS ? disenos[i] : NULL; }
const DisenoTabla *dis_buscar(const char *nombre)
{
    int i;
    for (i = 0; nombre && i < N_DIS; i++) if (!strcmp(disenos[i]->nombre, nombre)) return disenos[i];
    return NULL;
}
void dis_poner(const DisenoTabla *d, int paleta)
{
    if (d) del_sistema = d;
    paleta_sistema = (u8)(paleta < 0 ? 0 : paleta);
    calcular();
}
void dis_forzar(const DisenoTabla *d) { forzado = d; calcular(); }
void dis_v1(int si) { if (modo_v1 != (u8)(si != 0)) { modo_v1 = (u8)(si != 0); calcular(); } }
int dis_es_v1(void) { return modo_v1; }
const DisFuente *dis_fuente(int e)
{
    const DisenoTabla *d = dis_actual();
    if (e <= 0 || e >= DIS_T_N) return NULL;
    return modo_v1 ? d->fuente_v1[e] : d->fuente[e];
}
/* La escala del 5x7 para una escala/estilo cuando no hay fuente del diseño (el «clasico», F_MONO, o un glifo que Roboto no tiene).
 * En v1, la de siempre (cuerpo, etiqueta y título a 1). En la escala nueva el cuerpo va al DOBLE (mayúscula 14, como la de
 * Roboto 20) y cada paso de la escala sube uno: 1→2, 2→3, 3→4, 4→5. La pequeña, también al doble: el 5x7 no tiene un paso
 * intermedio y a 1 (mayúscula 7) es justo la letra que se quería dejar atrás. */
int dis_escala_5x7(int escala, int marcas)
{
    int e = F_ESTILO_DE(marcas), k;
    if (e == DIS_T_TITULO || e == DIS_T_CUERPO || e == DIS_T_ETIQUETA || e == DIS_T_PEQUENA) k = 1;
    else if (e == DIS_T_TITULAR) k = 2;
    else if (e == DIS_T_DISPLAY) k = 3;
    else if (e == DIS_T_DISPLAY_G) k = 4;
    else k = escala < 1 ? 1 : escala;
    if (modo_v1) return k;
    return k + 1;
}
int dis_alto_linea(void) { return dis_tokens()->m.alto_linea; }
int dis_teclas_columna(void) { return ui_giro() & 1 ? DIS_ANCHO_COL_TECLAS : 0; }
int dis_teclas_barra(void) { return ui_giro() & 1 ? 0 : dis_tokens()->m.alto_barra_teclas; }
Widget *dis_con_teclas(Widget *cuerpo, Widget *teclas)
{
    switch (ui_giro()) {
    case UI_GIRO_DER: return Fila(.cruzado = P_ESTIRAR, .hijos = HIJOS(Expandido(cuerpo), teclas));      /* las teclas, a la derecha */
    case UI_GIRO_IZQ: return Fila(.cruzado = P_ESTIRAR, .hijos = HIJOS(teclas, Expandido(cuerpo)));      /* a la izquierda */
    case UI_GIRO_180: return Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(teclas, Expandido(cuerpo)));   /* arriba */
    }
    return Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(Expandido(cuerpo), teclas));
}
/* (estética) Una fila con etiqueta y valor que no caben juntos: la etiqueta sale ENTERA y el valor se recorta con «…» (sin bajar de
 * 2/5 de lo que hay, ni subir de lo que mide). Devuelve el ancho que le toca al valor, o 0 si caben (o en el modo v1). an = lo que
 * hay para etiqueta + hueco + valor. */
int dis_ancho_valor(const DisFila *f, int an, int hueco)
{
    int at, av, m;
    if (modo_v1 || !f->valor) return 0;
    at = li_ancho_texto_f(f->titulo, 1, 0); av = li_ancho_texto_f(f->valor, 1, 0);
    if (at + hueco + av <= an) return 0;
    m = an - at - hueco; if (m < an * 2 / 5) m = an * 2 / 5; if (m > av) m = av;
    return m;
}
int dis_paso_linea(int marcas)
{
    const DisFuente *f; int e = F_ESTILO_DE(marcas), k;
    if (modo_v1) return LI_ALTO_LINEA;                     /* lo de siempre: 10, con cualquier estilo */
    f = (marcas & F_MONO) ? NULL : dis_fuente(e ? e : DIS_T_CUERPO);
    if (f) return f->alto;                                 /* la línea de Roboto ya lleva su interlineado */
    k = dis_escala_5x7(1, marcas);
    return 10 * k;                                         /* el 5x7: 9 de glifo + 1, por la escala */
}

static struct { const Icono *ic; u8 s; } reg_iconos[32];
static u8 n_reg;
void dis_icono_es(const Icono *ic, int s)
{
    int i;
    for (i = 0; i < n_reg; i++) if (reg_iconos[i].ic == ic) { reg_iconos[i].s = (u8)s; return; }
    if (n_reg < sizeof reg_iconos / sizeof reg_iconos[0] && s >= 0 && s < DIS_S_N) { reg_iconos[n_reg].ic = ic; reg_iconos[n_reg++].s = (u8)s; }
}
int dis_simbolo_de(const Icono *ic) { int i; for (i = 0; i < n_reg; i++) if (reg_iconos[i].ic == ic) return reg_iconos[i].s; return -1; }

/* ---- atajos: el componente lo dibuja el diseño en uso ------------------------------------------------------------ */
Widget *BarraSuperior(const char *titulo, Widget *derecha) { return dis_actual()->barra_superior(titulo, derecha); }
Widget *BarraTeclas(const char *izq, const char *der) { return dis_actual()->barra_teclas(izq, der); }
Widget *DisFilaW(const DisFila *f, int sel) { return dis_actual()->fila(f, sel); }
Widget *Boton(const char *texto, int prioridad, int enfocado) { return dis_actual()->boton(texto, prioridad, enfocado); }
Widget *Tarjeta(Widget *hijo, int enfocada) { return dis_actual()->tarjeta(hijo, enfocada); }
Widget *Chip(const char *texto, int elegido, int enfocado) { return dis_actual()->chip(texto, elegido, enfocado); }
Widget *Interruptor(int activo, int enfocado) { return dis_actual()->interruptor(activo, enfocado); }
Widget *Casilla(int activo, int enfocado) { return dis_actual()->casilla(activo, enfocado); }
Widget *Opcion(int activo, int enfocado) { return dis_actual()->opcion(activo, enfocado); }
Widget *Deslizador(int valor, int max, int enfocado) { return dis_actual()->deslizador(valor, max, enfocado); }

int dis_alto_texto(int escala, int marcas, const char *s)
{
    int e = F_ESTILO_DE(marcas), k;
    const DisFuente *f;
    if (modo_v1) {                       /* EXACTAMENTE lo de antes de la escala nueva (las .sen de API < 1.7 cuentan con ello) */
        f = e ? dis_fuente(e) : NULL;
        if (e && f) return f->alto;
        if (!e && escala == 1 && dis_fuente(DIS_T_CUERPO)) return dis_fuente(DIS_T_CUERPO)->alto;   /* la «g» de Roboto baja una fila más que la del 5x7 */
        k = dis_escala_5x7(escala, marcas);
        return (k == 1 || (s && li_descendentes(s)) ? 9 : 7) * k;
    }
    /* la escala nueva: con fuente del diseño, el alto de su línea (cabe todo: acentos y descendentes); si no, el 5x7 */
    f = (marcas & F_MONO) ? NULL : dis_fuente(e ? e : escala == 1 ? DIS_T_CUERPO : escala == 2 ? DIS_T_TITULAR
                                                    : escala == 3 ? DIS_T_DISPLAY : escala == 4 ? DIS_T_DISPLAY_G : 0);
    if (f) return f->alto;
    k = dis_escala_5x7(escala, marcas);
    return (k == 1 || (s && li_descendentes(s)) ? 9 : 7) * k;
}

/* ---- primitivas: esquinas redondeadas con el borde suavizado -------------------------------------------------------
 * La cobertura de un píxel de la esquina es la distancia de su centro al centro del arco, en dieciseisavos: 1 raíz entera
 * por píxel de esquina y NADA por los del interior (van con li_rect, de palabra en palabra). */
static u32 raiz(u32 v)
{
    u32 r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}
static void mezclar(Lienzo *l, int x, int y, Color c, int t)
{
    Color *p;
    if (x < l->recorte.x || y < l->recorte.y || x >= l->recorte.x + l->recorte.w || y >= l->recorte.y + l->recorte.h) return;
    p = l->cuadro + y * UI_ANCHO + x;
    *p = ui_mezcla(*p, c, t);
}
void li_rect_red(Lienzo *l, int x, int y, int w, int h, int r, Color c)
{
    int j;
    if (c == TRANSPARENTE || w <= 0 || h <= 0) return;
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    if (r <= 0) { li_rect(l, x, y, w, h, c); return; }
    if (y + h <= l->recorte.y || y >= l->recorte.y + l->recorte.h) return;
    li_rect(l, x, y + r, w, h - 2 * r, c);
    for (j = 0; j < r; j++) {
        int fy = (2 * (r - j) - 1) * 8, i, yb = y + h - 1 - j;
        for (i = 0; i < r; i++) {
            int fx = (2 * (r - i) - 1) * 8, t = (r * 16 - (int)raiz((u32)(fx * fx + fy * fy))) * 16 + 128;
            if (t >= 256) break;
            if (t > 0) { mezclar(l, x + i, y + j, c, t); mezclar(l, x + w - 1 - i, y + j, c, t);
                         mezclar(l, x + i, yb, c, t); mezclar(l, x + w - 1 - i, yb, c, t); }
        }
        li_rect(l, x + i, y + j, w - 2 * i, 1, c);
        if (yb != y + j) li_rect(l, x + i, yb, w - 2 * i, 1, c);
    }
}
void li_marco_red(Lienzo *l, int x, int y, int w, int h, int r, Color c)
{
    int j;
    if (c == TRANSPARENTE || w <= 0 || h <= 0) return;
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    if (r <= 0) { li_marco(l, x, y, w, h, c); return; }
    li_rect(l, x + r, y, w - 2 * r, 1, c); li_rect(l, x + r, y + h - 1, w - 2 * r, 1, c);
    li_rect(l, x, y + r, 1, h - 2 * r, c); li_rect(l, x + w - 1, y + r, 1, h - 2 * r, c);
    for (j = 0; j < r; j++) {
        int fy = (2 * (r - j) - 1) * 8, i, yb = y + h - 1 - j;
        for (i = 0; i < r; i++) {
            int fx = (2 * (r - i) - 1) * 8, d = (int)raiz((u32)(fx * fx + fy * fy)) - (r * 16 - 8), t;
            if (d < 0) d = -d;
            t = 256 - d * 16;
            if (t > 0) { mezclar(l, x + i, y + j, c, t); mezclar(l, x + w - 1 - i, y + j, c, t);
                         mezclar(l, x + i, yb, c, t); mezclar(l, x + w - 1 - i, yb, c, t); }
        }
    }
}
void li_circulo(Lienzo *l, int cx, int cy, int r, Color c) { li_rect_red(l, cx - r, cy - r, 2 * r, 2 * r, r, c); }

/* ---- CajaRed: la Caja con esquinas redondeadas ---------------------------------------------------------------------- */
static int tope(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static void cr_disponer(Elem *e, Restr r)
{
    const WCajaRed *w = (const WCajaRed *)e->w;
    Elem *h = ui_hijo(e, 0);
    const int ex = 2 * (w->mx + w->px), ey = 2 * (w->my + w->py);
    Restr c = r;
    if (w->an) { int a = w->an >= INF ? r.maxw : tope(w->an, r.minw, r.maxw); c.minw = c.maxw = (i16)a; }
    if (w->al) { int a = w->al >= INF ? r.maxh : tope(w->al, r.minh, r.maxh); c.minh = c.maxh = (i16)a; }
    e->an = c.minw; e->al = c.minh;
    if (h) {
        Restr d = { (i16)(c.minw > ex ? c.minw - ex : 0), (i16)(c.maxw >= INF ? INF : c.maxw > ex ? c.maxw - ex : 0),
                    (i16)(c.minh > ey ? c.minh - ey : 0), (i16)(c.maxh >= INF ? INF : c.maxh > ey ? c.maxh - ey : 0) };
        ui_disponer(h, d); h->x = (i16)(w->mx + w->px); h->y = (i16)(w->my + w->py);
        e->an = (i16)(h->an + ex); e->al = (i16)(h->al + ey);
    }
}
static void cr_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const WCajaRed *w = (const WCajaRed *)e->w;
    const int an = e->an - 2 * w->mx, al = e->al - 2 * w->my;
    li_rect_red(l, x + w->mx, y + w->my, an, al, w->radio, w->color);
    if (w->borde != TRANSPARENTE) li_marco_red(l, x + w->mx, y + w->my, an, al, w->radio, w->borde);
}
static int cr_actualizar(Elem *e, const Widget *nv)
{
    WCajaRed *a = (WCajaRed *)e->w; const WCajaRed *b = (const WCajaRed *)nv;
    int c = (a->an != b->an || a->al != b->al || a->mx != b->mx || a->my != b->my || a->px != b->px || a->py != b->py) ? C_DISPONER
          : (a->color != b->color || a->borde != b->borde || a->radio != b->radio) ? C_PINTAR : C_NADA;
    *a = *b;
    return c;
}
static int cr_opaco(Elem *e) { const WCajaRed *w = (const WCajaRed *)e->w; return w->color != TRANSPARENTE && !w->radio && !w->mx && !w->my; }
const Clase ClsWCajaRed = { .nombre = "CajaRed", .tipo = T_RENDER, .hijos = H_1, .tam = sizeof(WCajaRed), .opaco = cr_opaco,
                            .disponer = cr_disponer, .pintar = cr_pintar, .actualizar = cr_actualizar };

/* ---- el aviso breve (snackbar): una ruta emergente que se quita sola a los 3 s ------------------------------------- */
typedef struct { Widget base; const char *texto; } AvisoBreve;
typedef struct { char *texto; } AvisoBreveE;
extern const Clase ClsAvisoBreve;
static void ab_fin(Ctx *c) { (void)c; ui_quitar_ruta(&ClsAvisoBreve); }
static void ab_iniciar(Ctx *c)
{
    const AvisoBreve *w = ui_widget(c); AvisoBreveE *s = ui_estado(c);
    size_t n = strlen(w->texto ? w->texto : "") + 1;
    s->texto = memcpy(mo_pedir(n), w->texto ? w->texto : "", n);
    ui_tras(c, 3000, ab_fin);
}
static void ab_desechar(Ctx *c) { mo_soltar(((AvisoBreveE *)ui_estado(c))->texto); }
static int ab_tecla(Ctx *c, int k, void *d) { (void)k; (void)d; (void)c; ui_quitar_ruta(&ClsAvisoBreve); return 0; }
static Widget *ab_construir(Ctx *c)
{
    AvisoBreveE *s = ui_estado(c);
    return Teclas(ab_tecla, Alinear(0, 1, dis_actual()->aviso_breve(s->texto)));
}
COMPONENTE_CON_ESTADO(AvisoBreve, AvisoBreveE, ab_construir, ab_iniciar, .desechar = ab_desechar);
void ui_aviso_breve(const char *texto)
{
    if (ui_hay_ruta(&ClsAvisoBreve)) ui_quitar_ruta(&ClsAvisoBreve);
    ui_empujar_emergente(UI_W(AvisoBreve, .texto = texto));
}
