/* dis_clasico.c — (MD) el diseño «clasico»: el aspecto de siempre (el de la prueba de concepto del W600), portado a la
 * interfaz de diseños. Píxel a píxel lo que pintaban Pantalla, sh_fila, sh_boton, los diálogos, el Progreso, la barra de
 * desplazamiento, el CampoTexto y el Selector antes de la pista MD (las capturas de «make probar» con este diseño son las
 * de antes: docs/material.md §7). Las piezas que antes NO existían (interruptor, casilla, tarjeta…) se dibujan con el mismo
 * vocabulario: rectángulos, marcos y el 5x7. */
#include "diseno.h"

static const char *const paletas[] = { "oscuro", "claro", "fósforo" };
static const TemaDatos viejos[3] = {
    { .fondo = RGB(12, 14, 24), .texto = RGB(255, 255, 255), .tenue = RGB(140, 144, 160),
      .acento = RGB(255, 120, 0), .sobre_acento = RGB(0, 0, 0), .barra = RGB(36, 40, 60) },
    { .fondo = RGB(240, 240, 236), .texto = RGB(20, 20, 28), .tenue = RGB(104, 108, 120),
      .acento = RGB(0, 96, 208), .sobre_acento = RGB(255, 255, 255), .barra = RGB(204, 208, 220) },
    { .fondo = RGB(0, 16, 4), .texto = RGB(128, 255, 128), .tenue = RGB(48, 144, 64),
      .acento = RGB(176, 255, 64), .sobre_acento = RGB(0, 0, 0), .barra = RGB(0, 48, 16) } };

static void tokens(int p, DisTokens *t)
{
    const TemaDatos *v = &viejos[p % 3];
    Color *c = (Color *)&t->c; int i;
    for (i = 0; i < DIS_COL_N; i++) c[i] = v->texto;         /* lo que no tiene equivalente: el texto */
    t->c.primario = t->c.secundario = t->c.terciario = v->acento;
    t->c.sobre_primario = t->c.sobre_contenedor_primario = v->sobre_acento;
    t->c.contenedor_primario = t->c.contenedor_secundario = t->c.contenedor_terciario = v->acento;
    t->c.sobre_contenedor_secundario = t->c.sobre_contenedor_terciario = v->sobre_acento;
    t->c.error = RGB(255, 64, 48); t->c.sobre_error = RGB(0, 0, 0); t->c.contenedor_error = RGB(255, 64, 48); t->c.sobre_contenedor_error = RGB(0, 0, 0);
    t->c.superficie = t->c.superficie_contenedor_minimo = t->c.superficie_contenedor_bajo = v->fondo;
    t->c.sobre_superficie = v->texto; t->c.sobre_superficie_variante = v->tenue;
    t->c.superficie_contenedor = t->c.superficie_contenedor_alto = t->c.superficie_contenedor_maximo = v->barra;
    t->c.contorno = v->tenue; t->c.contorno_variante = v->barra;
    t->c.superficie_inversa = v->texto; t->c.sobre_superficie_inversa = v->fondo; t->c.primario_inverso = v->acento;
    t->c.velo = RGB(0, 0, 0);
    /* (estética, 26-oct) con la escala nueva el 5x7 del cuerpo va al doble (línea de 18): barras y filas = 18 + 6 */
    t->m = (DisMedidas){ .radio_peq = 0, .radio_med = 0, .radio_gra = 0, .radio_total = 0, .rejilla = 2, .oscuro = p != 1,
                         .anim_corta = 120, .anim_media = 250, .alto_barra_superior = 24, .alto_barra_teclas = 24,
                         .alto_fila = 24, .alto_fila_doble = 36, .alto_celda_menu = 64, .alto_linea = 20 };
}
#define T (dis_tema())

static Widget *barra_superior(const char *titulo, Widget *derecha)
{
    return Caja(.an = INF, .color = T->barra, .hijo = RellenoXY(4, 3, Fila(.espacio = 4, .hijos = HIJOS(
        Expandido(Texto(titulo, .color = T->acento)), derecha))));
}
static Widget *barra_teclas(const char *izq, const char *der)
{
    const int g = ui_giro();
    if (g == UI_GIRO_DER || g == UI_GIRO_IZQ) {   /* (giro) de lado: una columna junto a las teclas físicas (ui/diseno.h) */
        const char *arriba = g == UI_GIRO_DER ? der : izq, *abajo = g == UI_GIRO_DER ? izq : der;
        return Caja(.an = DIS_ANCHO_COL_TECLAS, .al = INF, .color = T->barra, .hijo = RellenoXY(2, 6, Columna(.principal = P_ENTRE, .cruzado = P_CENTRO,
            .hijos = HIJOS(Texto(arriba ? arriba : ""), Texto(abajo ? abajo : "")))));
    }
    if (g == UI_GIRO_180) { const char *t = izq; izq = der; der = t; }   /* boca abajo: la izquierda física queda a la derecha */
    return Caja(.an = INF, .color = T->barra, .hijo = RellenoXY(4, 3, Fila(.hijos = HIJOS(
        Texto(izq ? izq : ""), Hueco(), Texto(der ? der : "")))));
}
static Widget *andamio(Widget *barra, Widget *cuerpo, Widget *teclas)
{
    return Tema(T, Caja(.an = INF, .al = INF, .color = T->fondo, .hijo = dis_con_teclas(Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        barra, Expandido(cuerpo))), teclas)));   /* (giro) */
}
static Widget *marca(int acc, int activo, Color c)            /* lo de la derecha de una fila, con texto */
{
    if (acc == DIS_ACC_INTERRUPTOR || acc == DIS_ACC_CASILLA) return Texto(activo ? "sí" : "no", .color = c);
    if (acc == DIS_ACC_OPCION) return Texto(activo ? "•" : "", .color = c);
    if (acc == DIS_ACC_FLECHA) return Texto("→", .color = c);
    return NULL;
}
static Widget *valor(const DisFila *f, Color c)              /* (estética) la etiqueta entera; el valor, recortado si no caben */
{
    const int m = dis_ancho_valor(f, UI_ANCHO - dis_teclas_columna() - 2 * 4 - 6 - 8 /* la barra de desplazamiento */ - (f->icono ? f->icono->w * 2 + 6 : 0), 6);
    return m ? Caja(.an = (i16)m, .hijo = Alinear(1, 0, Texto(f->valor, .color = c))) : Texto(f->valor, .color = c);
}
static Widget *fila(const DisFila *f, int sel)
{
    Color tx = sel ? T->sobre_acento : T->texto, tn = sel ? T->sobre_acento : T->tenue;
    return Caja(.an = INF, .al = INF, .color = sel ? T->acento : TRANSPARENTE, .hijo = RellenoXY(4, 0, Fila(.espacio = 6, .hijos = HIJOS(
        f->icono ? Icon(f->icono, .color = sel ? tx : T->acento) : NULL,
        Expandido(f->subtitulo ? Columna(.principal = P_CENTRO, .hijos = HIJOS(Texto(f->titulo, .color = tx), Texto(f->subtitulo, .color = tn)))
                               : Texto(f->titulo, .color = tx)),
        f->accesorio == DIS_ACC_VALOR ? (f->valor ? valor(f, tn) : NULL) : marca(f->accesorio, f->activo, tn)))));
}
static Widget *dato(const char *nombre, const char *valor)
{
    return RellenoXY(4, 2, Fila(.espacio = 6, .hijos = HIJOS(Texto(nombre, .color = T->tenue), Hueco(), Texto(valor))));
}
static Widget *boton(const char *texto, int prioridad, int enf)
{
    (void)prioridad;
    return Caja(.color = enf ? T->acento : T->barra, .hijo = RellenoXY(6, 4, Texto(texto, .color = enf ? T->sobre_acento : T->texto)));
}
static Widget *tarjeta(Widget *hijo, int enf)
{
    return Caja(.an = INF, .color = T->barra, .borde = enf ? T->acento : TRANSPARENTE, .hijo = Relleno(4, hijo));
}
static Widget *chip(const char *texto, int elegido, int enf)
{
    return Caja(.color = elegido ? T->acento : T->barra, .borde = enf ? T->texto : TRANSPARENTE,
                .hijo = RellenoXY(4, 2, Texto(texto, .color = elegido ? T->sobre_acento : T->texto)));
}
static Widget *interruptor(int activo, int enf)
{
    return Caja(.color = enf ? T->acento : T->barra, .hijo = RellenoXY(3, 1, Texto(activo ? "SÍ" : "NO", .color = enf ? T->sobre_acento : T->texto)));
}
static Widget *casilla(int activo, int enf)
{
    return Caja(.an = dis_es_v1() ? 9 : 14, .al = dis_es_v1() ? 9 : 14, .color = activo ? T->acento : T->fondo, .borde = enf ? T->texto : T->tenue);
}
static Widget *opcion(int activo, int enf) { return casilla(activo, enf); }
static Widget *deslizador(int valor, int max, int enf)
{
    return Progreso(.valor = valor, .max = max, .color = enf ? T->acento : T->tenue);
}
static Widget *dialogo(int tipo, const char *titulo, const char *texto, Widget **botones, int n)
{
    Widget *v[16]; int i, k = 0;
    v[k++] = Caja(.an = INF, .color = T->barra, .hijo = RellenoXY(6, 4, Texto(titulo, .color = T->acento)));
    if (texto && texto[0]) v[k++] = Relleno(6, Parrafo(texto));
    if (!dis_es_v1()) {   /* (estética) las opciones que no caben, en una Lista que se desplaza con el foco */
        const int fila = dis_alto_texto(1, 0, NULL) + 8, lim = UI_ALTO - dis_teclas_barra() - 8 - 2 - (dis_alto_texto(1, 0, NULL) + 8)
                         - (texto && texto[0] ? 3 * dis_paso_linea(0) + 12 : 0);
        if (n * fila > lim) {
            v[k++] = Caja(.an = INF, .al = (i16)(lim / fila * fila), .hijo = Lista(.hijos = ui_lista(botones, (size_t)n), .barra = T->tenue));
            n = 0;
        }
    }
    for (i = 0; i < n && k < 15; i++) v[k++] = botones[i];
    return Velo(dis_con_teclas(   /* (giro) */
        Alinear(0, tipo == DIS_DLG_OPCIONES ? 1 : 0, RellenoXY(10, 4,
            Caja(.an = INF, .color = T->fondo, .borde = T->acento, .hijo = Relleno(1, Columna(.cruzado = P_ESTIRAR, .encoge = 1, .hijos = ui_lista(v, (size_t)k)))))),
        barra_teclas(tipo == DIS_DLG_AVISO ? "Aceptar" : "Elegir", tipo == DIS_DLG_AVISO ? "" : "Cancelar")));
}
static Widget *dialogo_boton(const char *texto, int tipo, int i, int enf)
{
    (void)tipo; (void)i;
    return Caja(.an = INF, .color = enf ? T->acento : T->fondo,
                .hijo = RellenoXY(6, 4, Texto(texto, .color = enf ? T->sobre_acento : T->texto)));
}
static Widget *aviso_breve(const char *texto)
{
    return RellenoXY(6, 20, Caja(.an = INF, .color = T->texto, .hijo = Relleno(4, Parrafo(texto, .color = T->fondo))));
}
static void pintar_progreso(Lienzo *l, int x, int y, int w, int h, i32 v, i32 max, Color c, Color fondo)
{
    int n = max > 0 ? (int)((i32)(w - 2) * v / max) : 0;
    li_rect(l, x, y, w, h, fondo != TRANSPARENTE ? fondo : T->barra);
    li_rect(l, x + 1, y + 1, n, h - 2, c != TRANSPARENTE ? c : T->acento);
}
static void pintar_desp(Lienzo *l, int x, int y, int w, int h, int pos, int largo, Color c)
{
    (void)w;
    li_rect(l, x + 1, y, 1, h, T->barra);
    li_rect(l, x, y + pos, 3, largo, c != TRANSPARENTE ? c : T->tenue);
}
static void pintar_campo(Lienzo *l, int x, int y, int w, int h, int enf)
{
    li_rect(l, x, y, w, h, T->barra);
    li_marco(l, x, y, w, h, enf ? T->acento : T->tenue);
}
static Widget *selector(const char *texto, int escala, int enf)
{
    return Caja(.color = enf ? T->acento : T->barra, .hijo = RellenoXY(3, 3,
        Texto(texto, .escala = (u8)escala, .color = enf ? T->sobre_acento : T->texto)));
}
static Widget *espera_reloj(const char *hora, const char *seg, const char *fecha)
{
    const Color blanco = RGB(255, 255, 255), negro = RGB(0, 0, 0);
    return Velo(RellenoXY(0, 7, Columna(.cruzado = P_CENTRO, .espacio = 6, .hijos = HIJOS(
        Fila(.cruzado = P_FIN, .encoge = 1, .espacio = 3, .hijos = HIJOS(
            Texto(hora, .escala = 4, .color = blanco, .sombra = negro),
            Texto(seg, .escala = 2, .color = RGB(255, 176, 64), .sombra = negro))),
        Texto(fecha, .color = blanco, .sombra = negro)))));
}
static Widget *espera_teclas(const char *izq, const char *der)
{
    const int g = ui_giro();
    if (g & 1) {   /* (giro) de lado: en columna, junto a las teclas */
        const char *arriba = g == UI_GIRO_DER ? der : izq, *abajo = g == UI_GIRO_DER ? izq : der;
        return Velo(Caja(.an = DIS_ANCHO_COL_TECLAS, .al = INF, .hijo = RellenoXY(2, 6, Columna(.principal = P_ENTRE, .cruzado = P_CENTRO, .hijos = HIJOS(
            Texto(arriba, .color = RGB(255, 255, 255)), Texto(abajo, .color = RGB(255, 255, 255)))))));
    }
    if (g == UI_GIRO_180) { const char *t = izq; izq = der; der = t; }
    return Velo(RellenoXY(4, 3, Fila(.hijos = HIJOS(Texto(izq, .color = RGB(255, 255, 255)), Hueco(), Texto(der, .color = RGB(255, 255, 255))))));
}
static Widget *celda_menu(const Icono *ic, const char *nombre, int enf, int t)
{
    (void)nombre;
    return Relleno(2, Caja(.color = enf ? ui_mezcla(T->barra, T->acento, t) : T->barra,
                           .hijo = Centro(Icon(ic, .escala = 2, .color = enf ? T->sobre_acento : T->texto))));
}
static Widget *tecla(const char *texto, int rol, int enf)
{
    (void)rol;
    return Caja(.an = INF, .al = INF, .color = enf ? T->acento : T->barra,
                .hijo = Centro(Texto(texto, .escala = 2, .color = enf ? T->sobre_acento : T->texto)));
}
static Color espera_fondo(void) { return T->fondo; }

const DisenoTabla dis_clasico = {
    .id = 0x434C4153u /* CLAS */, .nombre = "clasico", .version = 1, .interfaz = DIS_INTERFAZ,
    .n_paletas = 3, .paleta_nombre = paletas, .tokens = tokens,
    .medidas_v1 = { .rejilla = 2, .anim_corta = 120, .anim_media = 250, .alto_barra_superior = 15, .alto_barra_teclas = 15,
                    .alto_fila = 15, .alto_fila_doble = 24, .alto_celda_menu = 44, .alto_linea = 10 },
    .barra_superior = barra_superior, .barra_teclas = barra_teclas, .andamio = andamio, .fila = fila, .dato = dato,
    .boton = boton, .tarjeta = tarjeta, .chip = chip, .interruptor = interruptor, .casilla = casilla, .opcion = opcion,
    .deslizador = deslizador, .dialogo = dialogo, .dialogo_boton = dialogo_boton, .aviso_breve = aviso_breve,
    .pintar_progreso = pintar_progreso, .pintar_desp = pintar_desp, .pintar_campo = pintar_campo, .selector = selector,
    .espera_reloj = espera_reloj, .espera_teclas = espera_teclas,
    .celda_menu = celda_menu, .tecla = tecla, .espera_fondo = espera_fondo };
