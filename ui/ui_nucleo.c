/* Núcleo: elementos, reconciliación, disposición con caché, daño y pintado, foco, rutas,
 * temporizadores, animaciones y tareas. Diseño: docs/ui.md. */
#include "ui.h"

int  ui_en_arena(const void *p);
void ui_arena_vaciar(void);

#define W_HIJO(w)  (*(Widget **)((Widget *)(w) + 1))
#define W_HIJOS(w) (*(Widget ***)((Widget *)(w) + 1))
#define ES_COMP(k) ((k)->tipo == T_SIN_ESTADO || (k)->tipo == T_CON_ESTADO)

/* (PM) El búfer del panel, ALINEADO A 4 KiB y con la reserva redondeada a páginas enteras: así, cuando se le presta a una
 * app cargable en modo usuario (W5_JG_PANEL o las bandas de un juego, shell/cg_tabla.c -> cg_pm_abrir), ocupa páginas PROPIAS
 * y no deja al descubierto ni un byte del .bss vecino. Los píxeles de relleno del final son del propio búfer (no del sistema).
 * Sin protección es inofensivo (solo alinea una variable). */
#define UI_PANT_N ((((UI_PIXELES * (int)sizeof(Color)) + 4095) & ~4095) / (int)sizeof(Color))
short ui_ancho_ = PLACA_ANCHO, ui_alto_ = PLACA_ALTO;   /* (giro) el tamaño lógico de ahora */
static u8 giro_ui;
Color ui_pantalla[UI_PANT_N] __attribute__((aligned(4096)));
const unsigned ui_pantalla_tam = (unsigned)sizeof ui_pantalla;   /* (PM/AUD9) la reserva real, alineada y redondeada a página: lo que cg_pm_abrir abre al prestar el panel */
UiMedidas ui_med;

#define MAX_RUTAS 10
#define N_TEMPOS 32
#define N_ANIMS 12
#define N_TAREAS 8
static struct { Elem *raiz, *foco; u8 emergente; } rutas[MAX_RUTAS];
static int n_rutas;
static Elem *sucios[64]; static int n_sucios;
static Elem *foco;
static u8 hay_disponer, hay_pintar, todo, mostrar_foco;
static u32 ahora;
static struct { u8 activa; u32 inicio; } trans;
static int ultimo_dx;
#define TRANS_MS 160
static struct { u8 tipo; Widget *w; const Clase *cls; } nav[6]; static int n_nav;
#define MAX_NAV 6
static int (*filtro)(int k);                       /* ui_filtro: ve cada tecla antes que nadie; 1 = se la queda */
#define UI_PERIODO_A_OSCURAS 1000u
static u8 pantalla_off;                            /* ui_pantalla_activa(0): se construye y dispone, pero no se pinta ni se vuelca */
static struct { Elem *d; Retro f; u32 vence, periodo; } tempos[N_TEMPOS];
static struct { Elem *d; Anim *a; } anims[N_ANIMS];
static struct { Elem *d; Tarea f; void *dato; } tareas[N_TAREAS];

/* ---- rectángulos ---------------------------------------------------------------------------- */
static int vacio(Rect r) { return r.w <= 0 || r.h <= 0; }
static Rect inter(Rect a, Rect b)
{
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    Rect r = { (i16)x0, (i16)y0, (i16)(x1 > x0 ? x1 - x0 : 0), (i16)(y1 > y0 ? y1 - y0 : 0) };
    return r;
}
/* El daño es una lista corta de rectángulos que no se tocan: dos cambios lejanos (el título y
 * una celda) no obligan a repintar todo lo que queda entre ellos. */
#define MAX_DANO 6
static Rect danos[MAX_DANO]; static int n_danos;
static Rect une(Rect a, Rect b)
{
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    Rect r = { (i16)x0, (i16)y0, (i16)(x1 - x0), (i16)(y1 - y0) };
    return r;
}
static void danar(Rect r)
{
    const Rect pant = { 0, 0, (i16)UI_ANCHO, (i16)UI_ALTO };   /* (giro) el tamaño lógico de ahora */
    int i, cambio;
    r = inter(r, pant);
    if (vacio(r)) return;
    do {
        for (cambio = 0, i = 0; i < n_danos; i++) {
            Rect g = { (i16)(danos[i].x - 2), (i16)(danos[i].y - 2), (i16)(danos[i].w + 4), (i16)(danos[i].h + 4) };
            if (vacio(inter(g, r))) continue;
            r = une(danos[i], r); danos[i] = danos[--n_danos]; cambio = 1;
            break;
        }
        if (!cambio && n_danos == MAX_DANO) {             /* no cabe: se funde con el que menos área añade */
            int mejor = 0; i32 coste = 0x7FFFFFFF;
            for (i = 0; i < n_danos; i++) {
                Rect u = une(danos[i], r);
                i32 c = (i32)u.w * u.h - (i32)danos[i].w * danos[i].h;
                if (c < coste) { coste = c; mejor = i; }
            }
            r = une(danos[mejor], r); danos[mejor] = danos[--n_danos]; cambio = 1;
        }
    } while (cambio);
    danos[n_danos++] = r;
}

/* ---- marcas --------------------------------------------------------------------------------- */
void ui_marcar_pintar(Elem *e)
{
    Elem *p;
    e->marcas |= M_PINTAR; hay_pintar = 1;
    for (p = e->padre; p && !(p->marcas & M_SUB); p = p->padre) p->marcas |= M_SUB;
}

void ui_marcar_disponer(Elem *e)
{
    Elem *p;
    hay_disponer = 1;
    ui_marcar_pintar(e);
    e->marcas |= M_DISPONER | M_SUB;
    for (p = e->padre; p; p = p->padre) {
        if (p->cls->tipo != T_RENDER) continue;
        if (p->marcas & M_DISPONER) break;
        p->marcas |= M_DISPONER;
    }
}

void ui_sucio(Ctx *c)
{
    Elem *e = c;
    if (e->cls->tipo == T_RENDER) { ui_marcar_disponer(e); return; }
    /* quien recibe widgets ya hechos (proxies) no puede reconstruirse solo: esos widgets eran de
     * la arena de otro cuadro. Sube hasta el componente que los fabricó. */
    while (!(ES_COMP(e->cls) && e->cls->hijos == H_0) && e->padre) e = e->padre;
    if (e->marcas & M_SUCIO) return;
    if (n_sucios == (int)(sizeof sucios / sizeof sucios[0])) return;       /* no cabe: se queda sin marcar */
    e->marcas |= M_SUCIO;
    sucios[n_sucios++] = e;
}

Elem *ui_render(Elem *e)
{
    while (e && e->cls->tipo != T_RENDER) e = e->n ? e->hijos[0] : NULL;
    return e;
}
Elem *ui_hijo(Elem *e, unsigned i) { return i < e->n ? ui_render(e->hijos[i]) : NULL; }

Rect ui_rect_abs(Elem *e)
{
    Elem *r = ui_render(e), *p;
    Rect a = { 0, 0, 0, 0 };
    if (!r) return a;
    a.w = r->an; a.h = r->al;
    for (p = r; p; p = p->padre) if (p->cls->tipo == T_RENDER) { a.x += p->x; a.y += p->y; }
    return a;
}

const void *ui_heredado(Ctx *c, const Clase *cls)
{
    Elem *p;
    for (p = c->padre; p; p = p->padre) if (p->cls == cls) return p->w;
    return NULL;
}

Ctx *ui_ancestro(Ctx *c, const Clase *cls)
{
    Elem *p;
    for (p = c->padre; p; p = p->padre) if (p->cls == cls) return p;
    return NULL;
}

/* ---- montar, actualizar, desmontar ------------------------------------------------------------ */
static Elem *crear(Elem *padre, const Widget *w);
static void actualizar(Elem *e, const Widget *w);

static void desmontar(Elem *e)
{
    unsigned i;
    for (i = 0; i < e->n; i++) desmontar(e->hijos[i]);
    if (e->cls->desechar) e->cls->desechar(e);
    if (e->cls->tipo == T_RENDER) danar(e->pintado);
    if (foco == e) foco = NULL;
    for (i = 0; i < MAX_RUTAS; i++) if (rutas[i].foco == e) rutas[i].foco = NULL;
    for (i = 0; i < (unsigned)n_sucios; i++) if (sucios[i] == e) sucios[i] = NULL;
    for (i = 0; i < N_TEMPOS; i++) if (tempos[i].d == e) tempos[i].d = NULL;
    for (i = 0; i < N_ANIMS; i++) if (anims[i].d == e) anims[i].d = NULL;
    for (i = 0; i < N_TAREAS; i++) if (tareas[i].d == e) tareas[i].d = NULL;
    if (e->hijos != &e->uno) mo_soltar(e->hijos);
    ui_med.elementos--;
    mo_soltar(e);
}

/* Reconciliación de la lista de hijos: mismo sitio + misma clase + misma clave → se conserva el
 * elemento (y su estado); con clave, se busca también en otro sitio; lo que sobra se desmonta. */
static void reconciliar(Elem *e, Widget *const *nv, unsigned n)
{
    unsigned i, j, vn = e->n;
    Elem **v = e->hijos, **nuevo;
    if (n == 0 && vn == 0) return;
    nuevo = ui_arena((n + 1) * sizeof *nuevo);
    for (i = 0; i < n; i++) {
        nuevo[i] = NULL;
        if (i < vn && v[i] && v[i]->cls == nv[i]->cls && v[i]->clave == nv[i]->clave) { nuevo[i] = v[i]; v[i] = NULL; }
        else if (nv[i]->clave)
            for (j = 0; j < vn; j++)
                if (v[j] && v[j]->cls == nv[i]->cls && v[j]->clave == nv[i]->clave) {
                    nuevo[i] = v[j]; v[j] = NULL; ui_marcar_disponer(e);      /* cambió de sitio */
                    break;
                }
    }
    for (j = 0; j < vn; j++) if (v[j]) { desmontar(v[j]); hay_disponer = 1; ui_marcar_disponer(e); }
    e->n = 0;
    if (n > e->cap) {
        if (e->hijos != &e->uno) mo_soltar(e->hijos);
        e->hijos = mo_pedir(n * sizeof(Elem *)); e->cap = (u16)n;
    }
    for (i = 0; i < n; i++) {
        if (nuevo[i]) actualizar(nuevo[i], nv[i]);
        else nuevo[i] = crear(e, nv[i]);
        e->hijos[i] = nuevo[i];
        e->n = (u16)(i + 1);
    }
}

static void poner_hijos(Elem *e)
{
    const Clase *k = e->cls;
    e->marcas &= (u16)~M_SUCIO;
    if (ES_COMP(k)) {
        Widget *h = k->construir(e);
        ui_med.construidos++;
        reconciliar(e, &h, h ? 1 : 0);
    } else if (k->hijos == H_1) {
        Widget *h = W_HIJO(e->w);
        reconciliar(e, &h, h ? 1 : 0);
    } else if (k->hijos == H_N) {
        Widget **l = W_HIJOS(e->w); unsigned n = 0;
        while (l && l[n]) n++;
        reconciliar(e, l, n);
    }
}

static Elem *crear(Elem *padre, const Widget *w)
{
    const Clase *k = w->cls;
    size_t tw = ((size_t)k->tam + 7) & ~(size_t)7, te = ((size_t)k->tam_estado + 7) & ~(size_t)7;
    Elem *e = mo_pedir(sizeof(Elem) + tw + te);
    e->cls = k; e->clave = w->clave; e->padre = padre;
    e->w = (Widget *)(e + 1);
    memcpy(e->w, w, k->tam);
    e->origen = w;
    e->estado = te ? (u8 *)(e + 1) + tw : NULL;
    e->hijos = &e->uno; e->cap = 1;
    ui_med.elementos++;
    if (k->tipo == T_RENDER) ui_marcar_disponer(e);
    if (k->iniciar) k->iniciar(e);
    poner_hijos(e);
    return e;
}

static void marcar_renders(Elem *e)
{
    unsigned i;
    e->marcas |= M_SUB;
    if (e->cls->tipo == T_RENDER) { e->marcas |= M_DISPONER | M_PINTAR; hay_disponer = 1; }
    for (i = 0; i < e->n; i++) marcar_renders(e->hijos[i]);
}

static void actualizar(Elem *e, const Widget *w)
{
    const Clase *k = e->cls;
    size_t ini = sizeof(Widget) + (k->hijos != H_0 ? sizeof(void *) : 0);
    if (w == e->origen && !ui_en_arena(w)) return;       /* el mismo widget estático: nada que hacer */
    e->origen = w;
    if (k->tipo == T_RENDER) {
        int c;
        if (k->actualizar) c = k->actualizar(e, w);
        else {
            c = memcmp((const u8 *)e->w + ini, (const u8 *)w + ini, k->tam - ini) ? C_DISPONER : C_NADA;
            memcpy(e->w, w, k->tam);
        }
        if (c & 2) ui_marcar_disponer(e);
        else if (c & 1) ui_marcar_pintar(e);
    } else if (k->tipo == T_PROXY) {             /* sus structs no tienen relleno: punteros nada más */
        int c = memcmp((const u8 *)e->w + ini, (const u8 *)w + ini, k->tam - ini);
        memcpy(e->w, w, k->tam);
        if (c) { marcar_renders(e); ui_marcar_disponer(ui_render(e) ? ui_render(e) : e); }
    } else memcpy(e->w, w, k->tam);
    poner_hijos(e);
}

/* ---- disposición ------------------------------------------------------------------------------ */
void ui_disponer(Elem *e, Restr r)
{
    if (!e) return;
    if (!(e->marcas & M_DISPONER) && !memcmp(&r, &e->r, sizeof r)) return;
    e->r = r;
    e->marcas &= (u16)~M_DISPONER;
    e->marcas |= M_SUB;                               /* sus hijos pueden haberse movido */
    e->cls->disponer(e, r);
    if (e->an < r.minw) e->an = r.minw;
    if (e->an > r.maxw) e->an = r.maxw;
    if (e->al < r.minh) e->al = r.minh;
    if (e->al > r.maxh) e->al = r.maxh;
    ui_med.dispuestos++;
}

/* ---- daño y pintado ---------------------------------------------------------------------------- */
/* Solo baja por donde hay algo marcado (M_SUB sube hasta la raíz al marcar) o se ha movido algo. */
static void pasada_dano(Elem *e, int ax, int ay, Rect recorte, int forzar)
{
    Rect r = { (i16)ax, (i16)ay, e->an, e->al }, v;
    unsigned i;
    int movido;
    v = inter(r, recorte);
    movido = memcmp(&v, &e->pintado, sizeof v) != 0;
    if (!forzar && !movido && !(e->marcas & (M_PINTAR | M_SUB))) return;
    if ((e->marcas & M_PINTAR) || movido) { danar(e->pintado); danar(v); }
    e->pintado = v;
    forzar |= movido;
    if (e->cls->recorta) recorte = v;
    for (i = 0; i < e->n; i++) {
        Elem *h = e->hijos[i];
        for (; h && h->cls->tipo != T_RENDER; h = h->n ? h->hijos[0] : NULL) h->marcas &= (u16)~M_SUB;   /* de paso */
        if (h) pasada_dano(h, ax + h->x, ay + h->y, recorte, forzar);
    }
    e->marcas &= (u16)~(M_PINTAR | M_SUB);
}

static void pintar_arbol(Elem *e, Lienzo *l, int ax, int ay)
{
    Rect r = { (i16)ax, (i16)ay, e->an, e->al }, guarda = l->recorte, v = inter(r, guarda);
    unsigned i;
    if (vacio(v)) {
        /* (I1) fuera del daño. Si recorta, nada suyo se ve ahí: se acaba. Si no, un hijo puede salirse de él (la 5.ª fila
         * del menú clásico a 176x220 asoma por debajo de la rejilla) y la pasada de daño sí lo cuenta: se baja a buscarlo,
         * o el pintado por regiones se lo salta y no da lo mismo que un repintado completo. */
        if (e->cls->recorta) return;
        for (i = 0; i < e->n; i++) {
            Elem *h = ui_render(e->hijos[i]);
            if (h) pintar_arbol(h, l, ax + h->x, ay + h->y);
        }
        return;
    }
    ui_med.pintados++;
    if (e->cls->pintar) e->cls->pintar(e, l, ax, ay);
    if (e->cls->recorta) l->recorte = v;
    for (i = 0; i < e->n; i++) {
        Elem *h = ui_render(e->hijos[i]);
        if (h) pintar_arbol(h, l, ax + h->x, ay + h->y);
    }
    l->recorte = guarda;
    if (e->cls->pintar_encima) e->cls->pintar_encima(e, l, ax, ay);
}

/* ---- foco ------------------------------------------------------------------------------------- */
Elem *ui_foco(void) { return foco; }

void ui_enfocar(Elem *e)
{
    Elem *antes = foco;
    if (e == foco) return;
    foco = e;
    if (antes) { ui_sucio(antes); if (antes->cls->foco) antes->cls->foco(antes, 0); }
    if (e) { ui_sucio(e); if (e->cls->foco) e->cls->foco(e, 1); mostrar_foco = 1; }
}

static unsigned recoger(Elem *e, Elem **l, unsigned n, unsigned max)
{
    unsigned i;
    if (e->cls->enfocable && n < max) l[n++] = e;
    for (i = 0; i < e->n; i++) n = recoger(e->hijos[i], l, n, max);
    return n;
}

static void mover_foco(int k)
{
    Elem *l[96], *mejor = NULL;
    unsigned n = recoger(rutas[n_rutas - 1].raiz, l, 0, 96), i;
    i32 nota = 0x7FFFFFFF;
    Rect a;
    if (!n) return;
    if (!foco) { ui_enfocar(l[0]); return; }
    a = ui_rect_abs(foco);
    for (i = 0; i < n; i++) {
        Rect b; i32 dx, dy, prim, sec, s;
        if (l[i] == foco) continue;
        b = ui_rect_abs(l[i]);
        dx = (2 * b.x + b.w) - (2 * a.x + a.w); dy = (2 * b.y + b.h) - (2 * a.y + a.h);
        prim = k == K_ABAJO ? dy : k == K_ARRIBA ? -dy : k == K_DER ? dx : -dx;
        sec = (k == K_ABAJO || k == K_ARRIBA) ? dx : dy;
        if (sec < 0) sec = -sec;
        if (prim <= 0) continue;
        if ((k == K_IZQ || k == K_DER) && (b.y >= a.y + a.h || b.y + b.h <= a.y)) continue;   /* a los lados: solo lo de su misma fila */
        s = prim + 3 * sec;
        if (s < nota) { nota = s; mejor = l[i]; }
    }
    if (!mejor && (k == K_IZQ || k == K_DER)) {           /* fin de la fila: el siguiente (o el anterior) en el orden del árbol */
        for (i = 0; i < n && l[i] != foco; i++) ;
        if (i < n) mejor = k == K_DER ? (i + 1 < n ? l[i + 1] : NULL) : (i > 0 ? l[i - 1] : NULL);
    }
    if (mejor) ui_enfocar(mejor);
}

/* Tras disponer: pide a los contenedores con desplazamiento que dejen el foco a la vista. */
static void hacer_visible(Elem *e)
{
    Elem *r = ui_render(e), *p;
    Rect a;
    if (!r) return;
    a.x = r->x; a.y = r->y; a.w = r->an; a.h = r->al;
    for (p = r->padre; p; p = p->padre) {
        if (p->cls->tipo != T_RENDER) continue;
        if (p->cls->mostrar) p->cls->mostrar(p, a);
        a.x += p->x; a.y += p->y;
    }
}

void ui_tecla(int k)
{
    Elem *e = foco;
    if (!n_rutas) return;
    if (!e) for (e = rutas[n_rutas - 1].raiz; e->cls->tipo != T_RENDER && e->n; ) e = e->hijos[0];
    for (; e; e = e->padre) if (e->cls->tecla && e->cls->tecla(e, k)) return;
    if (k == K_ARRIBA || k == K_ABAJO || k == K_IZQ || k == K_DER) mover_foco(k);
    else if ((k == K_ATRAS || k == K_C) && n_rutas > 1) ui_sacar();
}

/* ---- rutas ------------------------------------------------------------------------------------- */
int  ui_rutas(void) { return n_rutas; }
void ui_empujar(Widget *w) { if (n_nav < MAX_NAV) { nav[n_nav].tipo = 1; nav[n_nav++].w = w; } }
void ui_empujar_emergente(Widget *w) { if (n_nav < MAX_NAV) { nav[n_nav].tipo = 3; nav[n_nav++].w = w; } }
void ui_quitar_ruta(const Clase *cls) { if (n_nav < MAX_NAV) { nav[n_nav].tipo = 4; nav[n_nav++].cls = cls; } }
int  ui_hay_ruta(const Clase *cls) { int i; for (i = 0; i < n_rutas; i++) if (rutas[i].raiz->cls == cls) return 1; return 0; }
int  ui_cima_emergente(void) { return n_rutas > 0 && rutas[n_rutas - 1].emergente; }
void ui_filtro(int (*f)(int k)) { filtro = f; }
void ui_pantalla_activa(int si) { if (pantalla_off && si) todo = 1; pantalla_off = (u8)!si; }
void ui_reconstruir_todo(void) { int i; for (i = 0; i < n_rutas; i++) ui_sucio(rutas[i].raiz); todo = 1; }
/* (MD) al cambiar de DISEÑO cambian las medidas de la letra: un texto con la misma cadena no se habría vuelto a medir */
void ui_redisponer_todo(void) { int i; for (i = 0; i < n_rutas; i++) { ui_sucio(rutas[i].raiz); marcar_renders(rutas[i].raiz); } todo = 1; }
void ui_sacar(void) { if (n_nav < MAX_NAV) nav[n_nav++].tipo = 2; }

static void aplicar_nav(void)
{
    int i;
    for (i = 0; i < n_nav; i++) {
        if ((nav[i].tipo == 1 || nav[i].tipo == 3) && n_rutas < MAX_RUTAS) {
            if (n_rutas) rutas[n_rutas - 1].foco = foco;
            foco = NULL;
            rutas[n_rutas].foco = NULL;
            rutas[n_rutas].emergente = nav[i].tipo == 3;
            rutas[n_rutas].raiz = crear(NULL, nav[i].w);
            n_rutas++;
            trans.activa = 0;
            if (n_rutas > 1 && nav[i].tipo == 1) { trans.activa = 1; trans.inicio = ahora; }
        } else if (nav[i].tipo == 2 && n_rutas > 1) {
            n_rutas--;
            desmontar(rutas[n_rutas].raiz);
            foco = rutas[n_rutas - 1].foco;
            trans.activa = 0;
            ui_sucio(rutas[n_rutas - 1].raiz);            /* la que queda a la vista se reconstruye: lo de encima pudo cambiar sus datos */
        } else if (nav[i].tipo == 4) {                    /* quitar la ruta de esa clase, esté donde esté (nunca la 0) */
            int j, k;
            for (j = n_rutas - 1; j > 0 && rutas[j].raiz->cls != nav[i].cls; j--) ;
            if (j > 0) {
                if (j == n_rutas - 1) foco = NULL; else rutas[n_rutas - 1].foco = foco;
                desmontar(rutas[j].raiz);
                for (k = j; k < n_rutas - 1; k++) rutas[k] = rutas[k + 1];
                n_rutas--;
                foco = rutas[n_rutas - 1].foco;
                trans.activa = 0;
                ui_sucio(rutas[n_rutas - 1].raiz);
            }
        }
        todo = 1; hay_disponer = 1;
    }
    n_nav = 0;
}

/* ---- tiempo ------------------------------------------------------------------------------------ */
static void tempo(Ctx *c, u32 ms, Retro f, u32 periodo)
{
    unsigned i;
    for (i = 0; i < N_TEMPOS; i++) if (!tempos[i].d) {
        tempos[i].d = c; tempos[i].f = f; tempos[i].vence = plat_ms() + ms; tempos[i].periodo = periodo;
        return;
    }
}
void ui_cada(Ctx *c, u32 ms, Retro f) { tempo(c, ms, f, ms); }
void ui_tras(Ctx *c, u32 ms, Retro f) { tempo(c, ms, f, 0); }
void ui_cancelar(Ctx *c, Retro f)                 /* quita los temporizadores de c con esa retrollamada (vale desde la propia retrollamada) */
{
    unsigned i;
    for (i = 0; i < N_TEMPOS; i++) if (tempos[i].d == c && tempos[i].f == f) tempos[i].d = NULL;
}

void ui_anim(Ctx *c, Anim *a, u32 ms)
{
    unsigned i, libre = N_ANIMS;
    a->inicio = plat_ms(); a->dur = (u16)ms; a->activa = 1;
    for (i = 0; i < N_ANIMS; i++) { if (anims[i].d && anims[i].a == a) return; if (!anims[i].d) libre = i; }
    if (libre < N_ANIMS) { anims[libre].d = c; anims[libre].a = a; } else a->activa = 0;
}
int ui_anim_t(const Anim *a)
{
    u32 t;
    if (!a->activa || !a->dur) return 256;
    t = (ahora - a->inicio) * 256 / a->dur;
    return t > 256 ? 256 : (int)t;
}
int ui_curva_sal(int t) { int u = 256 - t; return 256 - ((u * u * u) >> 16); }
int ui_curva_entra_sal(int t) { return (t * t * (768 - 2 * t)) >> 16; }

void ui_tarea(Ctx *c, Tarea f, void *dato)
{
    unsigned i;
    for (i = 0; i < N_TAREAS; i++) if (!tareas[i].d) { tareas[i].d = c; tareas[i].f = f; tareas[i].dato = dato; return; }
}

/* ---- el cuadro ---------------------------------------------------------------------------------- */
/* La ruta más baja que hace falta pintar para ver la ruta i: ella misma si es opaca; si es un
 * emergente, la primera opaca de debajo. */
static int base_de(int i) { while (i > 0 && rutas[i].emergente) i--; return i; }

/* Pinta en el lienzo todo lo que se ve: durante una transición (dx != 0) lo que había antes y la
 * ruta nueva desplazada; si no, de la base a la cima. */
static void pintar_rutas(Lienzo *l, int dx)
{
    int v, cima = n_rutas - 1;
    Elem *e;
    if (dx && cima > 0) {
        for (v = base_de(cima - 1); v < cima; v++) if ((e = ui_render(rutas[v].raiz)) != NULL) pintar_arbol(e, l, 0, 0);
        if ((e = ui_render(rutas[cima].raiz)) != NULL) pintar_arbol(e, l, dx, 0);
        return;
    }
    for (v = base_de(cima); v <= cima; v++) if ((e = ui_render(rutas[v].raiz)) != NULL) pintar_arbol(e, l, 0, 0);
}

int ui_cuadro(void)
{
    const Rect pant = { 0, 0, (i16)UI_ANCHO, (i16)UI_ALTO };
    u32 t0 = plat_cuenta(), t1, t2, t3;
    unsigned i;
    Elem *r;
    int dx = 0, base, v;

    ahora = plat_ms();
    aplicar_nav();
    for (i = 0; i < N_ANIMS; i++) if (anims[i].d) {
        ui_sucio(anims[i].d);
        if (ahora - anims[i].a->inicio >= anims[i].a->dur) { anims[i].a->activa = 0; anims[i].d = NULL; }
    }
    if (!n_sucios && !hay_disponer && !((hay_pintar || todo || trans.activa) && !pantalla_off) && !mostrar_foco) { ui_arena_vaciar(); return 0; }

    /* 1. construir + reconciliar: los sucios, de arriba abajo */
    for (;;) {
        Elem *e = NULL; int prof = 0x7FFF, j, p; Elem *q;
        for (j = 0; j < n_sucios; j++) {
            if (!sucios[j] || !(sucios[j]->marcas & M_SUCIO)) { sucios[j] = NULL; continue; }
            for (p = 0, q = sucios[j]; q; q = q->padre) p++;
            if (p < prof) { prof = p; e = sucios[j]; }
        }
        if (!e) break;
        poner_hijos(e);
    }
    n_sucios = 0;
    t1 = plat_cuenta();

    /* 2. disponer: las restricciones bajan, los tamaños suben. Se disponen todas las rutas que se
     * ven: la de arriba y, si es un emergente, las de debajo hasta la primera opaca. */
    r = ui_render(rutas[n_rutas - 1].raiz);
    if (!r) { ui_arena_vaciar(); return 0; }
    base = base_de(n_rutas - 1);
    {
        Restr fija = { UI_ANCHO, UI_ANCHO, UI_ALTO, UI_ALTO };
        if (hay_disponer) for (v = base; v < n_rutas; v++) ui_disponer(ui_render(rutas[v].raiz), fija);
        if (mostrar_foco && foco) { hacer_visible(foco); ui_disponer(r, fija); }
        hay_disponer = 0; mostrar_foco = 0;
    }
    t2 = plat_cuenta();
    if (pantalla_off) {                                   /* luz apagada: ni se pinta ni se vuelca; al encender, todo */
        todo = 1; trans.activa = 0;
        ui_arena_vaciar();
        return 0;
    }

    /* 3. daño, pintado de lo dañado y volcado de esas filas */
    if (trans.activa) {
        u32 t = (ahora - trans.inicio) * 256 / TRANS_MS;
        if (t >= 256) { trans.activa = 0; t = 256; }
        dx = UI_ANCHO - (UI_ANCHO * ui_curva_sal((int)t)) / 256;
        todo = 1;
    }
    ultimo_dx = dx;
    for (v = base; v < n_rutas; v++) {
        Elem *q, *rv = ui_render(rutas[v].raiz);
        for (q = rutas[v].raiz; q && q != rv; q = q->n ? q->hijos[0] : NULL) q->marcas &= (u16)~M_SUB;
        if (rv) pasada_dano(rv, v == n_rutas - 1 ? dx : 0, 0, pant, 0);
    }
    if (todo) { danos[0] = pant; n_danos = 1; }
    hay_pintar = 0; todo = 0;
    ui_med.dano_px = 0; ui_med.dano_rects = (u32)n_danos; ui_med.dano.w = ui_med.dano.h = 0;
    if (n_danos) {
        static u8 fila[UI_LADO_MAX];
        int d, y, opaco = !dx && base == n_rutas - 1 && r->cls->opaco && r->cls->opaco(r);
        memset(fila, 0, sizeof fila);
        for (d = 0; d < n_danos; d++) {
            Lienzo l = { ui_pantalla, danos[d] };
            if (!opaco) li_rect(&l, danos[d].x, danos[d].y, danos[d].w, danos[d].h, 0);
            pintar_rutas(&l, dx);
            memset(fila + danos[d].y, 1, (size_t)danos[d].h);
            ui_med.dano_px += (u32)(danos[d].w * danos[d].h);
            ui_med.dano = d ? une(ui_med.dano, danos[d]) : danos[d];
        }
        t3 = plat_cuenta();
        if (giro_ui) {                                    /* (giro) de lado o boca abajo el PDICON manda el cuadro ENTERO: UN volcado (de la
                                                           * primera a la última fila sucias, para convertirlas) en vez de uno por tramo */
            int a = -1, b = -1;
            for (y = 0; y < UI_ALTO; y++) if (fila[y]) { if (a < 0) a = y; b = y; }
            if (a >= 0) plat_volcar(ui_pantalla, a, b - a + 1);
        } else for (y = 0; y < UI_ALTO; y++) {           /* el panel se vuelca por tramos de filas enteras */
            int y0 = y;
            if (!fila[y]) continue;
            while (y < UI_ALTO && fila[y]) y++;
            plat_volcar(ui_pantalla, y0, y - y0);
        }
    } else t3 = plat_cuenta();
    n_danos = 0;
    ui_arena_vaciar();
    ui_med.cuadros++;
    ui_med.t_construir = t1 - t0; ui_med.t_disponer = t2 - t1; ui_med.t_pintar = t3 - t2;
    ui_med.t_volcar = plat_cuenta() - t3;
    return 1;
}

void ui_repintar_en(Color *destino)
{
    Lienzo l = { destino, { 0, 0, UI_ANCHO, UI_ALTO } };
    li_rect(&l, 0, 0, UI_ANCHO, UI_ALTO, 0);
    if (n_rutas) pintar_rutas(&l, ultimo_dx);
}

static int tecla_girada(int k);
int ui_paso(void)
{
    int k, trabajo = 0;
    unsigned i;
    u32 t = plat_ms();
    while ((k = plat_tecla()) != K_NADA) {
        trabajo = 1;
        if (giro_ui && k >= K_ARRIBA && k <= K_DER) k = tecla_girada(k);   /* (giro) el joystick, en el sentido de la pantalla */
        if (!filtro || !filtro(k)) ui_tecla(k);
    }
    for (i = 0; i < N_TEMPOS; i++) if (tempos[i].d && (i32)(t - tempos[i].vence) >= 0) {
        Elem *d = tempos[i].d; Retro f = tempos[i].f;
        if (tempos[i].periodo) {
            /* (R2; auditoría del sistema, M10) CON LA PANTALLA APAGADA un temporizador periódico no late más deprisa que una vez
             * por segundo: Consola, Movimiento y Teclas sondean cada 40 ms, Luces cada 80, Reloj cada 100–250, y con una de
             * ellas abierta y la pantalla apagada la CPU no dormía a fondo casi nunca (7,6 % de las vueltas, medido por la
             * auditoría; 96,4 % en el standby). Los de un solo disparo (ui_tras) no se tocan. */
            u32 per = pantalla_off && tempos[i].periodo < UI_PERIODO_A_OSCURAS ? UI_PERIODO_A_OSCURAS : tempos[i].periodo;
            tempos[i].vence += per;
            if ((i32)(t - tempos[i].vence) >= 0) tempos[i].vence = t + per;
        } else tempos[i].d = NULL;
        f(d);
    }
    for (i = 0; i < N_TAREAS; i++) if (tareas[i].d) {
        trabajo = 1;
        if (!tareas[i].f(tareas[i].d, tareas[i].dato)) tareas[i].d = NULL;
    }
    trabajo |= ui_cuadro();
    for (i = 0; i < N_ANIMS; i++) if (anims[i].d) trabajo = 1;
    if (pantalla_off) return n_nav != 0;                  /* a oscuras no hay animaciones que justifiquen no dormir */
    return trabajo || trans.activa || n_nav;
}
int ui_ocupada(void)                                      /* ¿hay tareas o animaciones en marcha? (para no dormir a fondo) */
{
    unsigned i;
    for (i = 0; i < N_TAREAS; i++) if (tareas[i].d) return 1;
    for (i = 0; i < N_ANIMS; i++) if (anims[i].d) return 1;
    return trans.activa || n_nav || n_sucios;
}
u32 ui_proximo_ms(void)                                   /* ms hasta el temporizador más cercano (0xFFFFFFFF = ninguno) */
{
    u32 t = plat_ms(), mejor = 0xFFFFFFFFu; unsigned i;
    for (i = 0; i < N_TEMPOS; i++) if (tempos[i].d) {
        i32 d = (i32)(tempos[i].vence - t);
        if (d < 0) d = 0;
        if ((u32)d < mejor) mejor = (u32)d;
    }
    return mejor;
}

void ui_forzar_todo(void) { todo = 1; }
/* (estética, giro) cambiar de orientación: el tamaño lógico, la plataforma (hw_pantalla_girar) y TODO redispuesto y repintado. Los
 * elementos NO se rehacen: foco, desplazamiento, texto escrito y diálogos abiertos siguen donde estaban (solo cambian las medidas). */
int ui_giro(void) { return giro_ui; }
/* El joystick es físico: de lado, empujarlo hacia el arriba DEL TELÉFONO es ir hacia el lado de la pantalla donde quedó el arriba del
 * teléfono. Con el teclado a la derecha (giro 1) el arriba del teléfono queda a la izquierda: arriba -> izquierda, derecha -> arriba,
 * abajo -> derecha, izquierda -> abajo. Con el teclado a la izquierda, al revés; boca abajo, todo dado la vuelta. */
static int tecla_girada(int k)
{
    static const u8 gira[4][4] = {      /* [giro][arriba, abajo, izq, der] */
        { K_ARRIBA, K_ABAJO, K_IZQ, K_DER },
        { K_IZQ, K_DER, K_ABAJO, K_ARRIBA },
        { K_ABAJO, K_ARRIBA, K_DER, K_IZQ },
        { K_DER, K_IZQ, K_ARRIBA, K_ABAJO } };
    int i = k == K_ARRIBA ? 0 : k == K_ABAJO ? 1 : k == K_IZQ ? 2 : 3;
    return gira[giro_ui & 3][i];
}
int ui_poner_giro(int g)
{
    g &= 3;
    if (g == giro_ui) return 1;
    if (plat_girar(g) != 0) return 0;
    giro_ui = (u8)g;
    ui_ancho_ = (short)(g & 1 ? PLACA_ALTO : PLACA_ANCHO); ui_alto_ = (short)(g & 1 ? PLACA_ANCHO : PLACA_ALTO);
    ui_redisponer_todo();
    return 1;
}
Ctx *ui_raiz(void) { return n_rutas ? rutas[n_rutas - 1].raiz : NULL; }

void ui_init(Widget *(*raiz)(void))
{
    ui_arena_vaciar();
    ui_empujar(raiz());
}
