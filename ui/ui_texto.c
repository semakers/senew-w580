/* Entrada de texto por teclado numérico: multipulsación con temporizador, modos con #, borrar con C,
 * acentos y ñ. El texto es de la aplicación (EntradaTexto); CampoTexto lo pinta y lo edita. */
#include "ui.h"
#include "diseno.h"

void et_iniciar(EntradaTexto *e, char *buf, unsigned cap)
{
    e->buf = buf; e->cap = (u16)cap; e->len = e->cur = 0; e->version++;
    if (cap) buf[0] = 0;
    if (e->solo_num) e->modo = ET_123;
}
void et_poner(EntradaTexto *e, const char *s)
{
    unsigned n = (unsigned)strlen(s);
    if (n > (unsigned)e->cap - 1) n = e->cap - 1u;
    while (n && ((u8)s[n] & 0xC0) == 0x80) n--;                 /* no cortar un carácter por la mitad */
    memcpy(e->buf, s, n); e->buf[n] = 0;
    e->len = e->cur = (u16)n; e->version++;
}
int et_insertar(EntradaTexto *e, const char *s)
{
    unsigned n = (unsigned)strlen(s), i;
    if (e->len + n + 1 > e->cap) return 0;
    for (i = e->len + 1u; i-- > e->cur; ) e->buf[i + n] = e->buf[i];
    memcpy(e->buf + e->cur, s, n);
    e->len = (u16)(e->len + n); e->cur = (u16)(e->cur + n); e->version++;
    return 1;
}
static unsigned anterior(const EntradaTexto *e, unsigned p) { if (p) p--; while (p && ((u8)e->buf[p] & 0xC0) == 0x80) p--; return p; }
static unsigned siguiente(const EntradaTexto *e, unsigned p) { if (p < e->len) p++; while (p < e->len && ((u8)e->buf[p] & 0xC0) == 0x80) p++; return p; }
int et_borrar(EntradaTexto *e)
{
    unsigned a = anterior(e, e->cur), n = e->cur - a, i;
    if (!n) return 0;
    for (i = a; i + n <= e->len; i++) e->buf[i] = e->buf[i + n];
    e->len = (u16)(e->len - n); e->cur = (u16)a; e->version++;
    return 1;
}
const char *et_modo_nombre(const EntradaTexto *e)
{
    static const char *const n[4] = { "abc", "Abc", "ABC", "123" };
    return n[e->modo & 3];
}

/* ciclo de cada tecla (K_0..K_9) en minúsculas; el dígito va dentro del ciclo.
 * (26-oct, el mantenedor: la # de su teléfono no funciona) la * CAMBIA EL MODO (abc, Abc, ABC, 123), como la #; los símbolos que daba la *
 * van al final de la tecla 1, detrás de la puntuación. */
static const char *const ciclo[10] = {
    " 0\n", ".,?!1-@:;()'\"/¿¡*+=%#<>_&$", "abc2á", "def3é", "ghi4í", "jkl5", "mno6ñó", "pqrs7", "tuv8úü", "wxyz9" };

static u32 mayus(u32 c)
{
    static const u16 par[][2] = { {0xE1,0xC1},{0xE9,0xC9},{0xED,0xCD},{0xF3,0xD3},{0xFA,0xDA},{0xF1,0xD1},{0xFC,0xDC} };
    unsigned i;
    if (c >= 'a' && c <= 'z') return c - 32;
    for (i = 0; i < sizeof par / sizeof par[0]; i++) if (par[i][0] == c) return par[i][1];
    return c;
}
static void a_utf8(u32 c, char *d)
{
    if (c < 0x80) { d[0] = (char)c; d[1] = 0; }
    else if (c < 0x800) { d[0] = (char)(0xC0 | (c >> 6)); d[1] = (char)(0x80 | (c & 63)); d[2] = 0; }
    else { d[0] = (char)(0xE0 | (c >> 12)); d[1] = (char)(0x80 | ((c >> 6) & 63)); d[2] = (char)(0x80 | (c & 63)); d[3] = 0; }
}
static u32 del_ciclo(int tecla, int i, int *n)              /* carácter i (módulo) del ciclo de esa tecla */
{
    const char *p = ciclo[tecla]; u32 c = 0; int total = 0, j;
    while (*p) { ui_utf8(&p); total++; }
    *n = total;
    for (p = ciclo[tecla], j = 0; j <= i % total; j++) c = ui_utf8(&p);
    return c;
}

typedef struct { u8 pend, pend_i, pend_tecla, parpadeo, largo_hecho, ast_modo; u32 pend_ms, ast_ms; i16 primera; } CampoE;
/* (E5, 27-oct) EL DICTADO: una pulsación LARGA de «*» en un campo de texto (no en uno de números, donde «*» es la coma) llama a este gancho, que pone el
 * shell (shell/dictado.c; sin él, «*» solo cambia el modo, como siempre). La pulsación corta sigue cambiando el modo al bajar: si resulta larga, se deshace.
 * El texto reconocido vuelve por ui_campo_insertar. Como el CampoTexto del SDK (W5C_CAMPO_TEXTO) ES este, las apps .sen lo reciben sin programar nada. */
int (*ui_dictado_gancho)(Ctx *campo);
void (*ui_campo_muere_gancho)(Ctx *campo);                       /* (E5) el campo se desmonta: el dictado que iba a él se cancela */
#define ET_CONFIRMA_MS 900
#define ET_LARGA_MS 600
#define MARGEN 3

static void cambio(Elem *e)
{
    const WCampoTexto *w = (const WCampoTexto *)e->w;
    ((CampoE *)e->estado)->parpadeo = 0;
    ui_marcar_pintar(e);
    if (w->al_cambiar) w->al_cambiar(e, w->dato);
}
static void confirmar(Elem *e)
{
    const WCampoTexto *w = (const WCampoTexto *)e->w; CampoE *s = e->estado;
    if (!s->pend) return;
    s->pend = 0;
    if (w->e->modo == ET_Abc) w->e->modo = ET_ABC;
    cambio(e);
}

/* la línea «cual» del texto partido al ancho dado: su inicio y sus bytes; devuelve 0 si no existe.
 * Con cual < 0 devuelve la línea donde está el cursor en *linea. */
static int linea(const EntradaTexto *t, int ancho, int cual, int *ini, int *n, int *linea_cursor)
{
    int p = 0, i = 0, lc = -1, k = 0;
    while (p < t->len) {
        k = li_partir(t->buf + p, ancho, 1, 0);
        if (t->cur >= p && (t->cur < p + k || (p + k == t->len && t->buf[t->len - 1] != '\n'))) lc = i;
        if (i == cual) { *ini = p; *n = k; if (linea_cursor) *linea_cursor = lc; return 1; }
        p += k; i++;
    }
    if (lc < 0) lc = i;                                       /* texto vacío o cursor tras un salto final */
    if (linea_cursor) *linea_cursor = lc;
    if (i == cual && lc == i) { *ini = p; *n = 0; return 1; }
    return 0;
}

static void campo_disponer(Elem *e, Restr r)
{
    const WCampoTexto *w = (const WCampoTexto *)e->w;
    e->an = r.maxw < INF ? r.maxw : 120;
    const int pl = dis_paso_linea(0);
    e->al = (i16)(w->lineas ? w->lineas * pl + 2 * MARGEN : r.maxh < INF ? r.maxh : 3 * pl + 2 * MARGEN);
}
static void campo_pintar(Elem *e, Lienzo *l, int x, int y)
{
    const WCampoTexto *w = (const WCampoTexto *)e->w; CampoE *s = e->estado;
    const EntradaTexto *t = w->e; const TemaDatos *tm = tema_de(e);
    const int pl = dis_paso_linea(0), v1 = dis_es_v1(), al_l = dis_alto_texto(1, 0, NULL);   /* (estética) el cursor y el subrayado, a la línea en uso */
    int enf = ui_foco() == e, ancho = e->an - 2 * MARGEN - 2, caben = (e->al - 2 * MARGEN + 2) / pl, i, ini, n, lc;
    Rect g = l->recorte, d = { (i16)(x + 1), (i16)(y + 1), (i16)(e->an - 2), (i16)(e->al - 2) };
    dis_actual()->pintar_campo(l, x, y, e->an, e->al, enf);      /* (MD) el marco lo pone el diseño */
    linea(t, ancho, -1, &ini, &n, &lc);
    if (caben < 1) caben = 1;
    if (lc < s->primera) s->primera = (i16)lc;
    if (lc >= s->primera + caben) s->primera = (i16)(lc - caben + 1);
    /* recorte al interior */
    if (d.x < g.x) { d.w = (i16)(d.w - (g.x - d.x)); d.x = g.x; }
    if (d.y < g.y) { d.h = (i16)(d.h - (g.y - d.y)); d.y = g.y; }
    if (d.x + d.w > g.x + g.w) d.w = (i16)(g.x + g.w - d.x);
    if (d.y + d.h > g.y + g.h) d.h = (i16)(g.y + g.h - d.y);
    if (d.w <= 0 || d.h <= 0) return;
    l->recorte = d;
    for (i = 0; i < caben; i++) {
        int ly = y + MARGEN + i * pl, v;
        if (!linea(t, ancho, s->primera + i, &ini, &n, NULL)) break;
        v = n; while (v && (t->buf[ini + v - 1] == '\n')) v--;
        li_texto_n(l, x + MARGEN, ly, t->buf + ini, v, tm->texto, 1, 0);
        if (enf && s->primera + i == lc) {
            int cx = x + MARGEN + li_ancho_n(t->buf + ini, t->cur - ini, 1, 0);
            if (s->pend) {                                   /* la letra pendiente, subrayada */
                unsigned a = anterior(t, t->cur);
                int ax = x + MARGEN + li_ancho_n(t->buf + ini, (int)a - ini, 1, 0);
                li_rect(l, ax, v1 ? ly + 9 : ly + al_l - 2, cx - ax > 0 ? cx - ax : 3, v1 ? 1 : 2, tm->acento);
            } else if (!s->parpadeo) li_rect(l, cx + (t->cur > ini ? 1 : 0), v1 ? ly - 1 : ly + 1, v1 ? 1 : 2, v1 ? 10 : al_l - 2, tm->acento);
        }
    }
    l->recorte = g;
    if (enf) {                                               /* el modo, abajo a la derecha */
        const char *m = et_modo_nombre(t);
        const DisTokens *k = dis_tokens();                   /* (MD) la marca del modo: una pastilla del diseño */
        if (v1) {
            int mw = li_ancho_texto(m, 1);
            li_rect_red(l, x + e->an - mw - 7, y + e->al - 12, mw + 6, 10, k->m.radio_peq, k->c.contenedor_primario);
            li_texto(l, x + e->an - mw - 4, y + e->al - 11, m, k->c.sobre_contenedor_primario, 1);
        } else {                                             /* (estética) en la talla pequeña, DENTRO del campo (asomaba por abajo) */
            const int pq = F_ESTILO(DIS_T_PEQUENA), mw = li_ancho_texto_f(m, 1, pq), hp = dis_alto_texto(1, pq, m);
            const int px = x + e->an - mw - 10, py = y + e->al - hp - 4;
            li_rect_red(l, px, py, mw + 8, hp + 2, k->m.radio_peq, k->c.contenedor_primario);
            li_texto_f(l, px + 4, py + 1, m, k->c.sobre_contenedor_primario, 1, pq);
        }
    }
}
static int campo_actualizar(Elem *e, const Widget *nv)
{
    WCampoTexto *a = (WCampoTexto *)e->w; const WCampoTexto *b = (const WCampoTexto *)nv;
    int c = a->lineas != b->lineas ? C_DISPONER : C_PINTAR;   /* el texto es de fuera: se repinta siempre que el padre reconstruye */
    *a = *b;
    return c;
}
static void campo_tic(Ctx *c)
{
    const WCampoTexto *w = (const WCampoTexto *)c->w; CampoE *s = c->estado;
    u32 t = plat_ms();
    static u8 fase;
    if (ui_foco() != c) { if (s->pend) confirmar(c); s->ast_ms = 0; return; }
    if (s->ast_ms && t - s->ast_ms >= ET_LARGA_MS) {                  /* (E5) «*» mantenido: dictar (y el cambio de modo de la pulsación, deshecho) */
        u32 abajo = plat_abajo(K_AST);
        s->ast_ms = 0;
        if (abajo && ui_dictado_gancho && !w->e->solo_num) { w->e->modo = s->ast_modo; cambio(c); ui_dictado_gancho(c); return; }
    }
    if (s->pend && !s->largo_hecho && s->pend_tecla <= 9 && t - s->pend_ms >= ET_LARGA_MS && plat_abajo(K_0 + s->pend_tecla)) {
        char d[2] = { (char)('0' + s->pend_tecla), 0 };       /* pulsación larga: el dígito */
        et_borrar(w->e); et_insertar(w->e, d);
        s->largo_hecho = 1; s->pend = 0; cambio(c);
        return;
    }
    if (s->pend && t - s->pend_ms >= ET_CONFIRMA_MS) { confirmar(c); return; }
    if (++fase >= 5) { fase = 0; if (!s->pend) { s->parpadeo ^= 1; ui_marcar_pintar(c); } }
}
static void campo_iniciar(Elem *e)
{
    if (((const WCampoTexto *)e->w)->autofoco && !ui_foco()) ui_enfocar(e);
    ui_cada(e, 100, campo_tic);
}
static void campo_desechar(Ctx *c) { if (ui_campo_muere_gancho) ui_campo_muere_gancho(c); }
static void campo_foco(Ctx *c, int ganado) { if (!ganado) confirmar(c); ui_marcar_pintar(c); }

static int campo_tecla(Ctx *c, int k)
{
    const WCampoTexto *w = (const WCampoTexto *)c->w; CampoE *s = c->estado;
    EntradaTexto *t = w->e;
    int tecla = k >= K_0 && k <= K_9 ? k - K_0 : k == K_AST && t->solo_num ? 10 : -1;   /* en un campo de números la * es la coma */
    if (tecla >= 0) {
        char d[4]; u32 cp; int n;
        if (t->modo == ET_123 && tecla <= 9) {
            confirmar(c);
            d[0] = (char)('0' + tecla); d[1] = 0;
            et_insertar(t, d); cambio(c);
            return 1;
        }
        if (t->solo_num) { if (tecla == 10) { confirmar(c); et_insertar(t, "."); cambio(c); } return 1; }
        if (s->pend && s->pend_tecla == tecla) { s->pend_i++; et_borrar(t); }
        else { confirmar(c); s->pend_i = 0; }
        cp = del_ciclo(tecla, s->pend_i, &n);
        if (cp == '\n' && w->lineas == 1) cp = del_ciclo(tecla, ++s->pend_i, &n);
        if (t->modo == ET_Abc || t->modo == ET_MAYUS) cp = mayus(cp);
        a_utf8(cp, d);
        if (et_insertar(t, d)) { s->pend = 1; s->pend_tecla = (u8)tecla; s->pend_ms = plat_ms(); s->largo_hecho = 0; }
        else s->pend = 0;
        cambio(c);
        return 1;
    }
    if (k == K_ALM || k == K_AST) {                        /* el modo: con la # o con la * */
        confirmar(c);
        if (k == K_AST) { s->ast_ms = plat_ms(); if (!s->ast_ms) s->ast_ms = 1; s->ast_modo = t->modo; }   /* (E5) si se mantiene, campo_tic dicta */
        if (!t->solo_num) t->modo = (u8)((t->modo + 1) & 3);
        cambio(c);
        return 1;
    }
    if (k == K_C) { s->pend = 0; et_borrar(t); cambio(c); return 1; }
    if (k == K_IZQ || k == K_DER) {
        confirmar(c);
        t->cur = (u16)(k == K_IZQ ? anterior(t, t->cur) : siguiente(t, t->cur));
        cambio(c);
        return 1;
    }
    if (k == K_ARRIBA || k == K_ABAJO) {
        int ancho = c->an - 2 * MARGEN - 2, ini, n, lc, x, p;
        confirmar(c);
        linea(t, ancho, -1, &ini, &n, &lc);
        if (!linea(t, ancho, lc, &ini, &n, NULL)) return 0;
        x = li_ancho_n(t->buf + ini, t->cur - ini, 1, 0);
        if (!linea(t, ancho, lc + (k == K_ABAJO ? 1 : -1), &ini, &n, NULL)) return 0;   /* no hay más: que suba (mueve el foco) */
        while (n && (t->buf[ini + n - 1] == '\n' || t->buf[ini + n - 1] == ' ')) n--;
        for (p = ini; p < ini + n && li_ancho_n(t->buf + ini, p - ini, 1, 0) < x; ) p = (int)siguiente(t, (unsigned)p);
        t->cur = (u16)p;
        cambio(c);
        return 1;
    }
    confirmar(c);
    return 0;
}
/* (E5) el texto DICTADO, en el cursor de ese campo (si sigue siendo un CampoTexto): lo que pasaría tecleándolo, con un espacio delante si hace falta.
 * 1 = puesto; 0 = no es un campo, no cabe entero (se pone lo que cabe, cortado en una palabra) o el texto está vacío. */
int ui_campo_insertar(Ctx *c, const char *utf8)
{
    const WCampoTexto *w;
    EntradaTexto *t;
    char b[8];
    unsigned n;
    if (!c || c->cls != &ClsWCampoTexto || !utf8) return 0;
    while (*utf8 == ' ') utf8++;
    if (!*utf8) return 0;
    w = (const WCampoTexto *)c->w; t = w->e;
    confirmar(c);
    if (t->cur > 0 && t->buf[t->cur - 1] != ' ' && t->buf[t->cur - 1] != '\n') { b[0] = ' '; b[1] = 0; et_insertar(t, b); }
    n = (unsigned)strlen(utf8);
    if (!et_insertar(t, utf8)) {                                       /* no cabe entero: palabra a palabra mientras quepa */
        const char *p = utf8;
        while (*p) {
            const char *q = p; char pal[64]; unsigned m;
            while (*q && *q != ' ') q++;
            m = (unsigned)(q - p) + (*q ? 1u : 0u);
            if (m >= sizeof pal) break;
            memcpy(pal, p, m); pal[m] = 0;
            if (!et_insertar(t, pal)) break;
            p += m;
        }
        cambio(c);
        return 0;
    }
    (void)n;
    cambio(c);
    return 1;
}
int ui_campo_lineas(Ctx *c) { return c && c->cls == &ClsWCampoTexto ? ((const WCampoTexto *)c->w)->lineas : -1; }   /* (E5) 1 = de una línea; 0 = sin tope */
const Clase ClsWCampoTexto = { .nombre = "CampoTexto", .tipo = T_RENDER, .enfocable = 1, .tam = sizeof(WCampoTexto),
                               .tam_estado = sizeof(CampoE), .iniciar = campo_iniciar, .desechar = campo_desechar, .tecla = campo_tecla, .foco = campo_foco,
                               .actualizar = campo_actualizar, .disponer = campo_disponer, .pintar = campo_pintar };
