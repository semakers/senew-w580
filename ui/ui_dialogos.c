/* Emergentes: aviso, confirmación y menú de opciones. Son rutas NO opacas (ui_empujar_emergente):
 * lo de debajo se sigue viendo, oscurecido por un Velo, y sigue vivo. */
#include "ui.h"
#include "diseno.h"

typedef struct { Widget base; const char *titulo, *texto; const char *const *ops; int n;
                 void (*elegir)(int i, void *dato); void (*si)(void *dato); void *dato; u8 tipo, por_no; } Dialogo;
typedef struct { char *titulo, *texto; } DialogoE;
extern const Clase ClsDialogo;
enum { D_AVISO, D_CONFIRMAR, D_OPCIONES };

static char *dup(const char *s) { size_t n = strlen(s ? s : "") + 1; return memcpy(mo_pedir(n), s ? s : "", n); }
static void dlg_iniciar(Ctx *c)
{
    const Dialogo *w = ui_widget(c); DialogoE *s = ui_estado(c);
    s->titulo = dup(w->titulo); s->texto = dup(w->texto);
}
static void dlg_desechar(Ctx *c) { DialogoE *s = ui_estado(c); mo_soltar(s->titulo); mo_soltar(s->texto); }

static void dlg_elegir(Ctx *c, void *dato)
{
    Dialogo w = *(const Dialogo *)ui_widget(ui_ancestro(c, &ClsDialogo));    /* copia: al sacar la ruta se desmonta */
    int i = (int)(size_t)dato;
    ui_sacar();
    if (w.tipo == D_OPCIONES && w.elegir) w.elegir(i, w.dato);
    if (w.tipo == D_CONFIRMAR && i == 0 && w.si) w.si(w.dato);
}
static Widget *dlg_opcion(Ctx *c, const Foco *f, void *dato)
{
    const Dialogo *w = ui_widget(ui_ancestro(c, &ClsDialogo));
    static const char *const sino[2] = { "Sí", "No" };
    const char *s = w->tipo == D_OPCIONES ? w->ops[(size_t)dato] : w->tipo == D_CONFIRMAR ? sino[(size_t)dato] : "Aceptar";
    (void)c;
    return dis_actual()->dialogo_boton(s, w->tipo, (int)(size_t)dato, f->enfocado);     /* (MD) lo dibuja el diseño */
}
static int dlg_tecla(Ctx *c, int k, void *dato)
{
    (void)c; (void)dato;
    if (k == K_SEL_IZQ) { ui_tecla(K_OK); return 1; }
    if (k == K_SEL_DER) { ui_sacar(); return 1; }
    return 0;
}
static Widget *dlg_construir(Ctx *c)
{
    const Dialogo *w = ui_widget(c); DialogoE *s = ui_estado(c);
    Widget *v[13]; int i, nops = w->tipo == D_OPCIONES ? w->n : w->tipo == D_CONFIRMAR ? 2 : 1;
    if (nops > 12) nops = 12;                            /* (I1) antes 9: la Memoria de Archivos tiene 11 (caben: 12 filas de 17 px) */
    for (i = 0; i < nops; i++)
        v[i] = Enfocable(CLAVE((u32)i + 1), .constructor = dlg_opcion, .dato = (void *)(size_t)i, .al_activar = dlg_elegir, .autofoco = i == (w->tipo == D_CONFIRMAR && w->por_no ? 1 : 0));
    /* (MD) el marco, el título, el texto y la barra de teclas los pone el diseño en uso (DIS_DLG_* = D_*) */
    return Teclas(dlg_tecla, dis_actual()->dialogo(w->tipo, s->titulo, s->texto, v, nops));
}
COMPONENTE_CON_ESTADO(Dialogo, DialogoE, dlg_construir, dlg_iniciar, .desechar = dlg_desechar);

void ui_aviso(const char *titulo, const char *texto) { ui_empujar_emergente(UI_W(Dialogo, .titulo = titulo, .texto = texto, .tipo = D_AVISO)); }
void ui_confirmar(const char *titulo, const char *texto, void (*si)(void *dato), void *dato)
{
    ui_empujar_emergente(UI_W(Dialogo, .titulo = titulo, .texto = texto, .si = si, .dato = dato, .tipo = D_CONFIRMAR));
}
/* (R2; auditoría de S7, M8) Para lo DESTRUCTIVO o lo que sale sin que nadie lo pida: el foco empieza en «No», así un OK dado
 * para otra cosa no confirma nada. */
void ui_confirmar_no(const char *titulo, const char *texto, void (*si)(void *dato), void *dato)
{
    ui_empujar_emergente(UI_W(Dialogo, .titulo = titulo, .texto = texto, .si = si, .dato = dato, .tipo = D_CONFIRMAR, .por_no = 1));
}
void ui_opciones(const char *titulo, const char *const *ops, int n, void (*elegir)(int i, void *dato), void *dato)
{
    ui_empujar_emergente(UI_W(Dialogo, .titulo = titulo, .texto = "", .ops = ops, .n = n, .elegir = elegir, .dato = dato, .tipo = D_OPCIONES));
}
