/* cg13.c — (1.3, pista MD) las envolturas del grupo 13 de la tabla: el sistema de diseño para las apps cargables.
 * Mismas reglas que el resto (docs/proteccion_memoria.md, docs/ronda2.md §3.3): toda cadena se valida y se COPIA a la arena
 * del sistema, el registro W5FilaDatos se copia antes de mirarlo (no puede cambiar entre la comprobación y el uso), el icono se
 * valida y se copia como en ui_w, los widgets se comprueban con cg_widget, y nada acaba en pánico: lo que no vale cierra la
 * app con un mensaje (cg_matar) y el sistema sigue. docs/material.md §8. */
#include "cargador.h"
#include "diseno.h"

#define MUERE(m, r) do { cg_matar(m); return r; } while (0)
static int vivo(unsigned n) { if (cg_estado() != CG_VIVA || cg_perfil() != W5_PERFIL_WIDGETS) return 0; if (ui_arena_libre() >= n + 3072u) return 1; cg_matar("La app agotó la memoria de un cuadro (demasiados widgets o texto)."); return 0; }
static const char *copia(const char *s)              /* NULL sigue siendo NULL; si no, una cadena ya validada, a la arena */
{
    unsigned n = 0; char *d;
    if (!s) return NULL;
    while (n < 512u && s[n]) n++;
    d = ui_arena(n + 1); memcpy(d, s, n); d[n] = 0;
    return d;
}
static const Icono *icono(const W5Icono *ic, int *mal)
{
    Icono *c; unsigned short *fl;
    *mal = 0;
    if (!ic) return NULL;
    if (!cg_legible_n(ic, sizeof *ic) || ic->w > 16 || !ic->h || ic->h > 64 || !cg_legible_n(ic->filas, (size_t)ic->h * sizeof(unsigned short))) { *mal = 1; return NULL; }
    c = ui_arena(sizeof *c); fl = ui_arena((unsigned)ic->h * sizeof(unsigned short));
    memcpy(fl, ic->filas, (size_t)ic->h * sizeof(unsigned short));
    c->w = ic->w; c->h = ic->h; c->filas = fl;
    return c;
}

W5W t_ui_fila_de(const W5FilaDatos *f, int sel)
{
    W5FilaDatos k; DisFila d; int mal;
    if (!vivo(1024)) return NULL;
    if (!f || !cg_legible_n(f, sizeof *f)) MUERE("ui_fila_de: la fila no es de la app.", NULL);
    k = *f;                                                   /* la copia: lo que se mira es lo que se usa */
    if (!k.titulo || !cg_legible(k.titulo) || !cg_cadena(k.subtitulo) || !cg_cadena(k.valor) || k.accesorio >= W5_ACC_N)
        MUERE("ui_fila_de: argumentos que no valen.", NULL);
    d.titulo = copia(k.titulo); d.subtitulo = copia(k.subtitulo); d.valor = copia(k.valor);
    d.icono = icono(k.icono, &mal);
    if (mal) MUERE("ui_fila_de: icono que no vale.", NULL);
    d.accesorio = k.accesorio; d.activo = k.activo != 0;
    return (W5W)DisFilaW(&d, sel != 0);
}
W5W t_ui_boton(const char *texto, int prio, int enf)
{
    if (!vivo(512)) return NULL;
    if (!texto || !cg_legible(texto) || prio < W5_PRIO_PRINCIPAL || prio > W5_PRIO_TERCIARIO) MUERE("ui_boton: argumentos que no valen.", NULL);
    return (W5W)Boton(copia(texto), prio, enf != 0);
}
W5W t_ui_tarjeta(W5W hijo, int enf)
{
    Widget *h = cg_widget(hijo);
    if (!vivo(512)) return NULL;
    if (hijo && !h) MUERE("ui_tarjeta: el hijo no es un widget.", NULL);
    return (W5W)Tarjeta(h, enf != 0);
}
W5W t_ui_chip(const char *texto, int elegido, int enf)
{
    if (!vivo(512)) return NULL;
    if (!texto || !cg_legible(texto)) MUERE("ui_chip: texto que no vale.", NULL);
    return (W5W)Chip(copia(texto), elegido != 0, enf != 0);
}
W5W t_ui_control(int tipo, int activo, int enf)
{
    if (!vivo(256)) return NULL;
    switch (tipo) {
    case W5_CTL_INTERRUPTOR: return (W5W)Interruptor(activo != 0, enf != 0);
    case W5_CTL_CASILLA: return (W5W)Casilla(activo != 0, enf != 0);
    case W5_CTL_OPCION: return (W5W)Opcion(activo != 0, enf != 0);
    }
    MUERE("ui_control: tipo que no existe.", NULL);
}
W5W t_ui_deslizador(int valor, int max, int enf)
{
    if (!vivo(256)) return NULL;
    if (max <= 0 || max > 1000000) MUERE("ui_deslizador: máximo que no vale.", NULL);
    return (W5W)Deslizador(valor, max, enf != 0);
}
void t_ui_aviso_breve(const char *texto)
{
    if (!vivo(256)) return;
    if (!texto || !cg_legible(texto)) MUERE("ui_aviso_breve: texto que no vale.", );
    ui_aviso_breve(texto);                                    /* el emergente copia el texto a su estado */
}
static unsigned rgb(Color c)
{
    unsigned v = (unsigned)((c >> 8) | (c << 8)) & 0xFFFF, r = v >> 11, g = (v >> 5) & 63, b = v & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}
unsigned t_ui_token(int k)
{
    const DisTokens *t = dis_tokens();
    if (k >= 0 && k < DIS_COL_N && k <= W5_TK_VELO) return rgb(((const Color *)&t->c)[k]);
    switch (k) {
    case W5_TK_RADIO_PEQ: return t->m.radio_peq;
    case W5_TK_RADIO_MED: return t->m.radio_med;
    case W5_TK_RADIO_GRA: return t->m.radio_gra;
    case W5_TK_REJILLA: return t->m.rejilla;
    case W5_TK_OSCURO: return t->m.oscuro;
    case W5_TK_ANIM_CORTA: return t->m.anim_corta;
    case W5_TK_ANIM_MEDIA: return t->m.anim_media;
    case W5_TK_ALTO_FILA: return t->m.alto_fila;
    case W5_TK_ALTO_FILA_DOBLE: return t->m.alto_fila_doble;
    case W5_TK_ALTO_BARRA_SUPERIOR: return t->m.alto_barra_superior;   /* (1.7) */
    case W5_TK_ALTO_BARRA_TECLAS: return t->m.alto_barra_teclas;
    case W5_TK_ALTO_LINEA: return t->m.alto_linea;
    }
    return 0;
}
int t_ui_diseno(char *buf, unsigned max)
{
    const char *n = dis_actual()->nombre; unsigned l = (unsigned)strlen(n);
    if (!buf || !max || !cg_en_app(buf, max)) return W5_ERROR;
    if (l > max - 1) l = max - 1;
    memcpy(buf, n, l); buf[l] = 0;
    return (int)l;
}
/* Un diseño PEDIDO por la app vale mientras esté cargada: cg13_soltar() lo quita al descargarla (cargador.c). */
static u8 pedido;
void cg13_soltar(void);
int t_ui_diseno_pedir(const char *nombre)
{
    char k[32]; unsigned n = 0; const DisenoTabla *d;
    if (cg_estado() != CG_VIVA) return W5_ERROR;
    if (!nombre) { cg13_soltar(); return W5_OK; }
    if (!cg_legible(nombre)) MUERE("ui_diseno_pedir: nombre que no vale.", W5_ERROR);
    while (n < sizeof k - 1 && nombre[n]) { k[n] = nombre[n]; n++; }
    k[n] = 0;
    if (!(d = dis_buscar(k))) return W5_NO_DISPONIBLE;
    pedido = 1; dis_forzar(d); ui_tema_global(dis_tema()); ui_redisponer_todo();
    return W5_OK;
}
void cg13_soltar(void) { if (pedido) { pedido = 0; dis_forzar(NULL); ui_tema_global(dis_tema()); ui_redisponer_todo(); } }
W5W t_ui_tecla_boton(const char *texto, int rol, int elegida)
{
    if (!vivo(512)) return NULL;
    if (!texto || !cg_legible(texto) || rol < W5_TECLA_NUMERO || rol > W5_TECLA_ACCION) MUERE("ui_tecla_boton: argumentos que no valen.", NULL);
    return (W5W)dis_actual()->tecla(copia(texto), rol, elegida != 0);
}
