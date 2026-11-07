/* menu_principal.c — (LANZ, 2026-10-16) Ajustes -> «Menú principal»: qué apps van en el menú y en qué orden.
 * el mantenedor: «esto no puede quedar automático, debe quedar a criterio del usuario desde Ajustes del sistema […] ahora el menú
 * principal es algo así como el launcher». La lista enseña TODAS las apps (las de fábrica, las externas enlazadas y las .sen
 * descubiertas en /apps, Mis archivos, la tarjeta y la Memoria) con su icono plano; las del menú van arriba, numeradas con su
 * celda. OK pone o quita; «Mover» (Sel. izq.) coge la fila y arriba/abajo la llevan; OK o «Soltar» la sueltan.
 * Ajustes y «Apps» (la lista de todo, siempre la última celda) son FIJAS: nadie se queda sin salida. Cupo: SH_CELDAS celdas
 * contando las dos. El modelo y lo que se guarda: shell/apps.c (sh_lz_*). Cada cambio se guarda en el acto; el menú se rehace
 * al salir de aquí. */
#include "shell.h"

typedef struct { Widget base; } MenuPrin;
typedef struct { i32 sel, ant; u8 mover; } MenuPrinE;
extern const Clase ClsMenuPrin;
static int n_filas(void) { int n = 0; while (sh_lz_app(n)) n++; return n; }

static Widget *fila(Ctx *l, int i, int sel, void *dato)
{
    MenuPrinE *s = ui_estado(ui_ancestro(l, &ClsMenuPrin));
    const ShLzApp *a = sh_lz_app(i);
    DisFila f = { 0 };
    (void)dato;
    if (!a) return sh_fila("Volver al de fábrica", NULL, sel);      /* la última fila: olvidar la elección */
    f.titulo = a->en_menu ? ui_fmt("%d. %s", sh_lz_celda(i), a->nombre) : a->nombre;
    f.icono = a->icono;
    if (s->mover && i == s->sel) { f.accesorio = DIS_ACC_VALOR; f.valor = "moviendo"; }
    else if (a->fijo) { f.accesorio = DIS_ACC_VALOR; f.valor = "siempre"; }
    else { f.accesorio = DIS_ACC_INTERRUPTOR; f.activo = a->en_menu; }
    return DisFilaW(&f, sel);
}
static void fabrica_si(void *d) { Ctx *c = d; MenuPrinE *s = ui_estado(c); sh_lz_fabrica(); s->sel = s->ant = 0; s->mover = 0; ui_sucio(c); }
static void activar(Ctx *l, int i, void *dato)
{
    Ctx *c = ui_ancestro(l, &ClsMenuPrin);
    MenuPrinE *s = ui_estado(c);
    int r;
    (void)dato;
    if (s->mover) { s->mover = 0; ui_sucio(c); return; }            /* OK con una fila cogida: soltarla */
    if (!sh_lz_app(i)) {
        ui_confirmar_no("Menú principal", "¿Volver al menú de fábrica? Las apps de fábrica en su sitio y, detrás, las demás por orden "
                     "(primero las apps, luego los juegos).", fabrica_si, c);
        return;
    }
    r = sh_lz_marcar(i);
    if (r == -1) ui_aviso("Ajustes va siempre", "Ajustes y «Apps» (la lista de todas las apps, siempre la última celda) no se pueden "
                          "quitar del menú: así nunca te quedas sin salida. Ajustes sí se puede mover.");
    else if (r == -2) ui_aviso("No caben más", ui_fmt("El menú tiene %d celdas y «Apps» ocupa la última: caben %d apps contando "
                               "Ajustes. Quita otra antes. Lo que no está en el menú sigue en «Apps».", SH_CELDAS, SH_LZ_MAX));
    ui_sucio(c);
}
/* Con una fila cogida, la lista ya ha movido la selección: la fila va detrás (se cambia con la vecina). Lo que no es un paso a la
 * vecina (dar la vuelta de la última a la primera, pasar página, ir a «Volver al de fábrica») no mueve nada. */
static void movida(Ctx *l, int i, void *dato)
{
    Ctx *c = ui_ancestro(l, &ClsMenuPrin);
    MenuPrinE *s = ui_estado(c);
    (void)dato;
    if (s->mover) {
        if ((i == s->ant + 1 || i == s->ant - 1) && sh_lz_app(i) && !sh_lz_mover(s->ant, i)) s->ant = i;
        else s->sel = s->ant;
        ui_sucio(l);
    } else s->ant = i;
    ui_sucio(c);
}
static int tecla(Ctx *t, int k, void *dato)
{
    Ctx *c = ui_ancestro(t, &ClsMenuPrin);
    MenuPrinE *s = ui_estado(c);
    (void)dato;
    if (k == K_SEL_IZQ) { if (sh_lz_app(s->sel)) { s->mover = (u8)!s->mover; s->ant = s->sel; } ui_sucio(c); return 1; }
    if (k == K_SEL_DER) { ui_sacar(); return 1; }
    return 0;
}
static void iniciar(Ctx *c) { MenuPrinE *s = ui_estado(c); sh_lz_abrir(); s->sel = s->ant = 0; }
static void desechar(Ctx *c) { (void)c; sh_lz_cerrar(); }            /* si algo cambió, el menú se rehace */
static Widget *construir(Ctx *c)
{
    MenuPrinE *s = ui_estado(c);
    int n = n_filas();
    if (!n) return Teclas(sh_teclas, Pantalla("Menú principal", Relleno(8, Parrafo(
        "En modo seguro no se carga ninguna app de fuera: el menú es el de fábrica y aquí no se puede cambiar.")), "", "Atrás"));
    return Teclas(tecla, Pantalla("Menú principal", Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        Expandido(ListaPerezosa(.n = n + 1, .fila = fila, .al_activar = activar, .al_mover = movida, .sel = &s->sel,
                                .alto_fila = dis_tokens()->m.alto_fila)),
        Caja(.an = INF, .color = sh_tema()->barra, .hijo = Relleno(4, Parrafo(s->mover ?
            "Arriba/abajo la llevan. OK o «Soltar» la deja ahí." :
            ui_fmt("En el menú: %d de %d. OK pone o quita; «Mover» cambia el orden. Ajustes y «Apps» (la última) van siempre.",
                   sh_lz_en_menu(), SH_LZ_MAX), .color = sh_tema()->tenue, .marcas = F_ESTILO(DIS_T_PEQUENA)))))),
        s->mover ? "Soltar" : "Mover", "Atrás"));
}
COMPONENTE_CON_ESTADO(MenuPrin, MenuPrinE, construir, iniciar, .desechar = desechar);
Widget *app_menu_principal(void) { return UI_W(MenuPrin); }
