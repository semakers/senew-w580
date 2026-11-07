/* dis_material3.c — (MD) el diseño «material3», el de serie: Material Design 3 adaptado a 240x320 (y 176x220), RGB565 y
 * teclado sin táctil. Las decisiones (por qué cada medida) están en docs/material.md §2–§5. Los colores salen del algoritmo
 * de tonos HCT con la semilla naranja de siempre (herr/diseno/hacer_colores.py); la letra es Roboto rasterizada con grises
 * (herr/diseno/hacer_fuente.py). Lo que un diseño de M3 haría con el tacto (la onda, el estado «hover») aquí lo hace el FOCO. */
#include "diseno.h"
#include "dis_colores_datos.h"

extern const DisFuente dis_simbolos_12, dis_simbolos_24, dis_roboto_cuerpo, dis_roboto_etiqueta, dis_roboto_titulo, dis_roboto_titular, dis_roboto_display, dis_roboto_display_g,
                       dis_roboto_pequena, dis_roboto_cuerpo_v1, dis_roboto_etiqueta_v1, dis_roboto_titulo_v1, dis_roboto_titular_v1,
                       dis_roboto_display_v1, dis_roboto_display_g_v1;
static const char *const paletas[] = { "oscuro", "claro" };
static const DisColores esquema[2] = { DIS_M3_OSCURO, DIS_M3_CLARO };

static void tokens(int p, DisTokens *t)
{
    t->c = esquema[p & 1];
    /* (estética, 26-oct) LA ESCALA B: cuerpo 20 px (línea 24), título 22 (27), pequeña 14 (17). Fila = línea + 4; barra superior =
     * título + 5; softkeys = etiqueta + 4; fila doble = cuerpo + pequeña + 3. docs/estetica.md §4. */
    t->m = (DisMedidas){ .radio_peq = 4, .radio_med = 8, .radio_gra = 12, .radio_total = 255, .rejilla = 4, .oscuro = !(p & 1),
                         .anim_corta = 100, .anim_media = 200, .alto_barra_superior = 32, .alto_barra_teclas = 28,
                         .alto_fila = 28, .alto_fila_doble = 44, .alto_celda_menu = 63, .alto_linea = 24 };
}
#define M (dis_tokens()->m)
#define V1 (dis_es_v1())
#define C (dis_tokens()->c)
#define ET F_ESTILO(DIS_T_ETIQUETA)
#define TI F_ESTILO(DIS_T_TITULO)

static Widget *barra_superior(const char *titulo, Widget *derecha)
{
    return CajaRed(.an = INF, .al = M.alto_barra_superior, .px = 8, .color = C.superficie_contenedor, .hijo = Fila(.espacio = 4, .hijos = HIJOS(
        Expandido(Texto(titulo, .color = C.sobre_superficie, .marcas = TI)), derecha)));
}
static Widget *barra_teclas(const char *izq, const char *der)
{
    const int g = ui_giro();
    if (g == UI_GIRO_DER || g == UI_GIRO_IZQ) {   /* (giro) de lado: una COLUMNA junto a las teclas físicas, cada etiqueta a la altura de su tecla */
        const char *arriba = g == UI_GIRO_DER ? der : izq, *abajo = g == UI_GIRO_DER ? izq : der;
        const int pq = F_ESTILO(DIS_T_PEQUENA);
        return CajaRed(.an = DIS_ANCHO_COL_TECLAS, .al = INF, .px = 4, .py = 8, .color = C.superficie_contenedor, .hijo = Columna(.principal = P_ENTRE,
            .cruzado = P_CENTRO, .hijos = HIJOS(Texto(arriba ? arriba : "", .color = C.primario, .marcas = (u8)pq), Texto(abajo ? abajo : "", .color = C.primario, .marcas = (u8)pq))));
    }
    if (g == UI_GIRO_180) { const char *t = izq; izq = der; der = t; }   /* boca abajo: la izquierda física queda a la derecha */
    {
    const int an = UI_ANCHO - dis_teclas_columna() - 16, wi = li_ancho_texto_f(izq ? izq : "", 1, ET), wd = li_ancho_texto_f(der ? der : "", 1, ET);
    Widget *ti = Texto(izq ? izq : "", .color = C.primario, .marcas = ET), *td = Texto(der ? der : "", .color = C.primario, .marcas = ET);
    if (!V1 && wi + 8 + wd > an) {   /* (estética) no caben las dos: se reparten el ancho (la corta entera) y la otra se recorta con «…» */
        int mi = an / 2 - 4, md = an / 2 - 4;
        if (wi < mi) md = an - 8 - wi; else if (wd < md) mi = an - 8 - wd;
        ti = Caja(.an = (i16)mi, .hijo = ti); td = Caja(.an = (i16)md, .hijo = Alinear(1, 0, td));
    }
    return CajaRed(.an = INF, .al = M.alto_barra_teclas, .px = 8, .color = C.superficie_contenedor, .hijo = Fila(.cruzado = P_CENTRO, .hijos = HIJOS(
        ti, Hueco(), td)));
    }
}
static Widget *andamio(Widget *barra, Widget *cuerpo, Widget *teclas)
{
    return Tema(dis_tema(), Caja(.an = INF, .al = INF, .color = C.superficie, .hijo = dis_con_teclas(Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        barra, Expandido(cuerpo))), teclas)));   /* (giro) las softkeys, en barra o en columna según la orientación */
}

/* ---- los controles pequeños, pintados (Pintura): el estado va en .dato, así un cambio repinta solo eso ------------- */
#define BITS(a, e) ((void *)(size_t)(((a) ? 1u : 0u) | ((e) ? 2u : 0u)))
/* (estética) las medidas de los controles: las de siempre en el modo v1 (pista del interruptor 26x14) y un tercio más con la
 * escala nueva (36x20), a juego con la fila de 28 */
static void p_interruptor(Lienzo *l, Rect r, void *d)
{
    const int on = (int)(size_t)d & 1, enf = (int)(size_t)d & 2, h = V1 ? 14 : 20, w = V1 ? 26 : 36, x = r.x, y = r.y + (r.h - h) / 2;
    if (enf) li_rect_red(l, x - 2, y - 2, w + 4, h + 4, h / 2 + 2, C.secundario);        /* el anillo de foco */
    if (enf) li_rect_red(l, x - 1, y - 1, w + 2, h + 2, h / 2 + 1, C.superficie);
    if (on) { li_rect_red(l, x, y, w, h, h / 2, C.primario); li_circulo(l, x + w - h / 2, y + h / 2, h / 2 - 2, C.sobre_primario); }
    else { li_rect_red(l, x, y, w, h, h / 2, C.contorno); li_rect_red(l, x + 1, y + 1, w - 2, h - 2, h / 2 - 1, C.superficie_contenedor_maximo);
           li_circulo(l, x + h / 2, y + h / 2, h / 2 - 3, C.contorno); }
}
static void p_casilla(Lienzo *l, Rect r, void *d)
{
    const int on = (int)(size_t)d & 1, enf = (int)(size_t)d & 2, n = V1 ? 12 : 16, k = V1 ? 1 : 2, x = r.x + 1, y = r.y + (r.h - n) / 2;
    if (enf) li_rect_red(l, x - 3, y - 3, n + 6, n + 6, n / 2 + 3, C.contenedor_secundario);
    if (on) {
        int i;
        li_rect_red(l, x, y, n, n, 2, C.primario);
        for (i = 0; i < 3 * k; i++) li_rect(l, x + 2 * k + i, y + 5 * k + i, 2, 2, C.sobre_primario);   /* la marca: dos trazos */
        for (i = 0; i < 5 * k; i++) li_rect(l, x + 5 * k + i, y + 7 * k - i, 2, 2, C.sobre_primario);
    } else { li_rect_red(l, x, y, n, n, 2, C.sobre_superficie_variante); li_rect_red(l, x + 2, y + 2, n - 4, n - 4, 1, enf ? C.contenedor_secundario : C.superficie); }
}
static void p_opcion(Lienzo *l, Rect r, void *d)
{
    const int on = (int)(size_t)d & 1, enf = (int)(size_t)d & 2, k = V1 ? 0 : 2, cx = r.x + 7 + k, cy = r.y + r.h / 2;
    Color fondo = enf ? C.contenedor_secundario : C.superficie;
    if (enf) li_circulo(l, cx, cy, 9 + k, fondo);
    li_circulo(l, cx, cy, 6 + k, on ? C.primario : C.sobre_superficie_variante);
    li_circulo(l, cx, cy, 4 + k, fondo);
    if (on) li_circulo(l, cx, cy, 3 + k, C.primario);
}
static void p_deslizador(Lienzo *l, Rect r, void *d)
{
    const u32 v = (u32)(size_t)d; const int enf = v & 2, t = (int)(v >> 2);              /* t = 0..1024 */
    const int x = r.x + 2, w = r.w - 4, cy = r.y + r.h / 2, px = x + (w * t) / 1024;
    li_rect_red(l, x, cy - 2, w, 4, 2, C.contenedor_secundario);
    li_rect_red(l, x, cy - 2, px - x, 4, 2, C.primario);
    li_rect_red(l, px - 1, cy - (enf ? 8 : 6) - (V1 ? 0 : 3), 3, (enf ? 16 : 12) + (V1 ? 0 : 6), 1, C.primario);   /* el asa (la de M3 de 2024: una barra) */
}
static Widget *interruptor(int a, int e) { return Pintura(p_interruptor, .dato = BITS(a, e), .an = V1 ? 28 : 38, .al = V1 ? 16 : 22); }
static Widget *casilla(int a, int e) { return Pintura(p_casilla, .dato = BITS(a, e), .an = V1 ? 16 : 20, .al = V1 ? 16 : 20); }
static Widget *opcion(int a, int e) { return Pintura(p_opcion, .dato = BITS(a, e), .an = V1 ? 16 : 20, .al = V1 ? 16 : 20); }
static Widget *deslizador(int valor, int max, int e)
{
    u32 t = max > 0 ? (u32)((valor < 0 ? 0 : valor > max ? max : valor) * 1024 / max) : 0;
    return Pintura(p_deslizador, .dato = (void *)(size_t)((t << 2) | (e ? 2u : 0u)), .al = V1 ? 18 : 24);
}

static Widget *derecha(const DisFila *f, int sel)
{
    switch (f->accesorio) {
    case DIS_ACC_VALOR:
        if (!f->valor) return NULL;
        {   /* (estética) si etiqueta y valor no caben, la etiqueta sale entera y el VALOR se recorta con «…» (dis_ancho_valor) */
            const int m = dis_ancho_valor(f, UI_ANCHO - dis_teclas_columna() - 2 * 4 - 2 * 6 - 8 /* la barra de desplazamiento */ - (f->icono ? f->icono->w + 8 : 0), 8);
            if (m) return Caja(.an = (i16)m, .hijo = Alinear(1, 0, Texto(f->valor, .color = sel ? C.sobre_contenedor_secundario : C.sobre_superficie_variante)));
        }
        return Texto(f->valor, .color = sel ? C.sobre_contenedor_secundario : C.sobre_superficie_variante);
    case DIS_ACC_INTERRUPTOR: return interruptor(f->activo, 0);
    case DIS_ACC_CASILLA: return casilla(f->activo, 0);
    case DIS_ACC_OPCION: return opcion(f->activo, 0);
    case DIS_ACC_FLECHA: return Texto("→", .color = C.sobre_superficie_variante);
    }
    return NULL;
}
static Widget *fila(const DisFila *f, int sel)
{
    Color tx = sel ? C.sobre_contenedor_secundario : C.sobre_superficie, tn = sel ? C.sobre_contenedor_secundario : C.sobre_superficie_variante;
    return CajaRed(.an = INF, .al = INF, .radio = 8, .mx = 4, .my = 1, .px = 6, .color = sel ? C.contenedor_secundario : TRANSPARENTE,
        .hijo = Fila(.espacio = 8, .hijos = HIJOS(
            f->icono ? Icon(f->icono, .color = sel ? tx : C.sobre_superficie_variante) : NULL,
            Expandido(f->subtitulo ? Columna(.principal = P_CENTRO, .espacio = 1, .hijos = HIJOS(Texto(f->titulo, .color = tx), Texto(f->subtitulo, .color = tn)))
                                   : Texto(f->titulo, .color = tx)),
            derecha(f, sel))));
}
static Widget *dato(const char *nombre, const char *valor)
{
    return RellenoXY(8, 2, Fila(.espacio = 8, .hijos = HIJOS(Texto(nombre, .color = C.sobre_superficie_variante), Hueco(),
                                                             Texto(valor, .color = C.sobre_superficie))));
}
static Widget *boton(const char *texto, int prio, int enf)
{
    Color fondo = prio == DIS_PRINCIPAL ? C.primario : prio == DIS_SECUNDARIO ? C.contenedor_secundario : TRANSPARENTE;
    Color tx = prio == DIS_PRINCIPAL ? C.sobre_primario : prio == DIS_SECUNDARIO ? C.sobre_contenedor_secundario : C.primario;
    return CajaRed(.radio = 255, .color = fondo, .borde = enf ? C.sobre_superficie : prio == DIS_TERCIARIO ? TRANSPARENTE : TRANSPARENTE,
                   .px = 10, .py = 4, .hijo = Texto(texto, .color = tx, .marcas = ET));
}
static Widget *tarjeta(Widget *hijo, int enf)
{
    return CajaRed(.an = INF, .radio = 8, .color = C.superficie_contenedor_alto, .borde = enf ? C.primario : TRANSPARENTE, .px = 8, .py = 8, .hijo = hijo);
}
static Widget *chip(const char *texto, int elegido, int enf)
{
    return CajaRed(.radio = 6, .color = elegido ? C.contenedor_secundario : TRANSPARENTE,
                   .borde = enf ? C.primario : elegido ? TRANSPARENTE : C.contorno,
                   .px = 8, .py = 3, .hijo = (Texto(elegido ? ui_fmt("✓ %s", texto) : texto, .color = elegido ? C.sobre_contenedor_secundario : C.sobre_superficie_variante, .marcas = ET)));
}
static Widget *dialogo(int tipo, const char *titulo, const char *texto, Widget **botones, int n)
{
    Widget *v[16]; int i, k = 0, mt = 0;
    v[k++] = RellenoXY(8, 6, Texto(titulo, .color = C.sobre_superficie, .marcas = TI));
    if (!V1 && texto && texto[0]) {   /* (estética) un texto que no cabe con el cuerpo (p. ej. «Detalles» de Música) va en la talla pequeña */
        const int an = UI_ANCHO - dis_teclas_columna() - 2 * 4 - 2 * 8, cabe = UI_ALTO - dis_teclas_barra() - 12 - 8 - (dis_alto_texto(1, TI, NULL) + 12) - n * (dis_alto_texto(1, ET, NULL) + 6) - 6;
        const char *p = texto; int nl = 0;
        while (*p && nl < 64) { p += li_partir(p, an, 1, 0); nl++; }
        if (nl * dis_paso_linea(0) > cabe) mt = F_ESTILO(DIS_T_PEQUENA);
    }
    if (texto && texto[0]) v[k++] = UI_W(WRelleno, .izq = 8, .der = 8, .aba = 6, .hijo = Parrafo(texto, .color = C.sobre_superficie_variante, .marcas = (u8)mt));
    if (!V1) {   /* (estética) con la letra nueva caben ~7 opciones: si son más, van en una Lista que se desplaza con el foco */
        const int fila = dis_alto_texto(1, ET, NULL) + 6, lim = UI_ALTO - dis_teclas_barra() - 12 - 8 - (dis_alto_texto(1, TI, NULL) + 12)
                         - (texto && texto[0] ? 3 * dis_paso_linea(0) + 6 : 0);
        if (n * fila > lim) {
            v[k++] = Caja(.an = INF, .al = (i16)(lim / fila * fila), .hijo = Lista(.hijos = ui_lista(botones, (size_t)n), .barra = C.contorno));
            n = 0;
        }
    }
    for (i = 0; i < n && k < 15; i++) v[k++] = botones[i];
    return Velo(dis_con_teclas(   /* (giro) las softkeys del diálogo, donde estén las de la pantalla */
        Alinear(0, tipo == DIS_DLG_OPCIONES ? 1 : 0, RellenoXY(V1 ? 12 : 4, 6,   /* (estética) casi a todo el ancho, como los emergentes del original */
            CajaRed(.an = INF, .radio = 12, .color = C.superficie_contenedor_alto,
                    .hijo = UI_W(WRelleno, .arr = 2, .aba = 6, .hijo = Columna(.cruzado = P_ESTIRAR, .encoge = 1, .hijos = ui_lista(v, (size_t)k)))))),
        barra_teclas(tipo == DIS_DLG_AVISO ? "Aceptar" : "Elegir", tipo == DIS_DLG_AVISO ? "" : "Cancelar")));
}
static Widget *dialogo_boton(const char *texto, int tipo, int i, int enf)
{
    (void)tipo; (void)i;
    return CajaRed(.an = INF, .radio = 255, .mx = V1 ? 6 : 4, .my = 1, .px = V1 ? 10 : 8, .py = V1 ? 4 : 2, .color = enf ? C.contenedor_secundario : TRANSPARENTE,
        .hijo = Texto(texto, .color = enf ? C.sobre_contenedor_secundario : C.primario, .marcas = ET));
}
static Widget *aviso_breve(const char *texto)
{
    return UI_W(WRelleno, .izq = 8, .der = 8, .aba = (u8)(M.alto_barra_teclas + 6), .hijo = CajaRed(.an = INF, .radio = 4, .color = C.superficie_inversa,
        .hijo = RellenoXY(10, 7, Parrafo(texto, .color = C.sobre_superficie_inversa))));
}
static void pintar_progreso(Lienzo *l, int x, int y, int w, int h, i32 v, i32 max, Color c, Color fondo)
{
    int n = max > 0 ? (int)((i32)w * v / max) : 0, hh = h > 4 ? 4 : h, yy = y + (h - hh) / 2;
    li_rect_red(l, x, yy, w, hh, hh / 2, fondo != TRANSPARENTE ? fondo : C.contenedor_secundario);
    if (n > 0) li_rect_red(l, x, yy, n < hh ? hh : n, hh, hh / 2, c != TRANSPARENTE ? c : C.primario);
}
static void pintar_desp(Lienzo *l, int x, int y, int w, int h, int pos, int largo, Color c)
{
    (void)w; (void)h;
    li_rect_red(l, x + 1, y + pos, 2, largo, 1, c != TRANSPARENTE ? c : C.contorno);
}
static void pintar_campo(Lienzo *l, int x, int y, int w, int h, int enf)
{
    li_rect_red(l, x, y, w, h, 4, C.superficie_contenedor_maximo);
    li_rect(l, x, y + h - (enf ? 2 : 1), w, enf ? 2 : 1, enf ? C.primario : C.sobre_superficie_variante);
}
static Widget *selector(const char *texto, int escala, int enf)
{
    return CajaRed(.radio = 4, .color = enf ? C.contenedor_primario : C.superficie_contenedor_maximo, .borde = enf ? C.primario : TRANSPARENTE,
                   .px = 4, .py = 3, .hijo = Texto(texto, .escala = (u8)escala, .color = enf ? C.sobre_contenedor_primario : C.sobre_superficie));
}
/* La espera: la foto manda. El reloj en display grande (Roboto Light), los segundos en el tono claro del primario y la fecha en
 * estilo título, sobre una tarjeta-velo redondeada de lado a lado; las teclas, como etiquetas sobre el velo. */
static Widget *espera_reloj(const char *hora, const char *seg, const char *fecha)
{
    const Color blanco = RGB(255, 255, 255), negro = RGB(0, 0, 0);
    return (Velo(RellenoXY(0, 8, Columna(.cruzado = P_CENTRO, .espacio = 4, .hijos = HIJOS(
        Fila(.cruzado = P_FIN, .encoge = 1, .espacio = 4, .hijos = HIJOS(
            Texto(hora, .escala = 4, .color = blanco, .sombra = TRANSPARENTE),
            Texto(seg, .escala = 2, .color = esquema[0].primario))),
        Texto(fecha, .color = esquema[0].sobre_superficie, .marcas = TI))))));
}
static Widget *espera_teclas(const char *izq, const char *der)
{
    const int g = ui_giro();
    if (g & 1) {   /* (giro) de lado: en columna, junto a las teclas */
        const char *arriba = g == UI_GIRO_DER ? der : izq, *abajo = g == UI_GIRO_DER ? izq : der;
        return Velo(Caja(.an = DIS_ANCHO_COL_TECLAS, .al = INF, .hijo = RellenoXY(2, 8, Columna(.principal = P_ENTRE, .cruzado = P_CENTRO, .hijos = HIJOS(
            Texto(arriba, .color = RGB(255, 255, 255), .marcas = F_ESTILO(DIS_T_PEQUENA)), Texto(abajo, .color = RGB(255, 255, 255), .marcas = F_ESTILO(DIS_T_PEQUENA)))))));
    }
    if (g == UI_GIRO_180) { const char *t = izq; izq = der; der = t; }
    return Velo(RellenoXY(8, 4, Fila(.hijos = HIJOS(Texto(izq, .color = RGB(255, 255, 255), .marcas = ET), Hueco(),
                                                    Texto(der, .color = RGB(255, 255, 255), .marcas = ET)))));
}
/* la rejilla de apps: un cuadrado redondeado (radio grande) con el símbolo de 24 px y el nombre debajo; el foco lo llena de
 * contenedor_primario (con la animación del núcleo: t mezcla del contenedor normal al del foco) */
static Widget *celda_menu(const Icono *ic, const char *nombre, int enf, int t)
{
    Color fondo = enf ? ui_mezcla(C.superficie_contenedor, C.contenedor_primario, t) : C.superficie_contenedor;
    return CajaRed(.an = INF, .al = INF, .radio = 12, .mx = 3, .my = 2, .color = fondo,
        .hijo = Columna(.principal = P_CENTRO, .cruzado = P_CENTRO, .espacio = 3, .hijos = HIJOS(
            Icon(ic, .escala = 2, .color = enf ? C.sobre_contenedor_primario : C.primario),
            Texto(nombre, .marcas = V1 ? ET : F_ESTILO(DIS_T_PEQUENA), .color = enf ? C.sobre_contenedor_primario : C.sobre_superficie_variante))));   /* (estética) el nombre, en la pequeña: cabe en la celda */
}
/* una tecla de un teclado en pantalla: número = tonal, operación = contenedor terciario, acción = primario; la elegida, primario */
static Widget *tecla(const char *texto, int rol, int enf)
{
    Color f = enf ? C.primario : rol == DIS_TECLA_OPERACION ? C.contenedor_terciario : rol == DIS_TECLA_ACCION ? C.primario : C.contenedor_secundario;
    Color tx = enf ? C.sobre_primario : rol == DIS_TECLA_OPERACION ? C.sobre_contenedor_terciario : rol == DIS_TECLA_ACCION ? C.sobre_primario : C.sobre_contenedor_secundario;
    return CajaRed(.an = INF, .al = INF, .radio = 255, .color = f, .hijo = Centro(Texto(texto, .marcas = F_ESTILO(DIS_T_TITULAR), .color = tx)));
}
/* sin foto, la espera es SIEMPRE oscura (un velo sobre una superficie clara salía gris): la superficie del esquema oscuro */
static Color espera_fondo(void) { return esquema[0].superficie_contenedor_minimo; }

const DisenoTabla dis_material3 = {
    .id = 0x4D415433u /* MAT3 */, .nombre = "material3", .version = 1, .interfaz = DIS_INTERFAZ,
    .n_paletas = 2, .paleta_nombre = paletas, .tokens = tokens,
    .fuente = { NULL, &dis_roboto_cuerpo, &dis_roboto_etiqueta, &dis_roboto_titulo, &dis_roboto_titular, &dis_roboto_display, &dis_roboto_display_g,
                &dis_roboto_pequena },
    .fuente_v1 = { NULL, &dis_roboto_cuerpo_v1, &dis_roboto_etiqueta_v1, &dis_roboto_titulo_v1, &dis_roboto_titular_v1, &dis_roboto_display_v1,
                   &dis_roboto_display_g_v1, &dis_roboto_cuerpo_v1 },
    .medidas_v1 = { .radio_peq = 4, .radio_med = 8, .radio_gra = 12, .radio_total = 255, .rejilla = 4,
                    .anim_corta = 100, .anim_media = 200, .alto_barra_superior = 24, .alto_barra_teclas = 18,
                    .alto_fila = 18, .alto_fila_doble = 30, .alto_celda_menu = 52, .alto_linea = 10 },
    .simbolos = &dis_simbolos_12, .simbolos_g = &dis_simbolos_24,
    .barra_superior = barra_superior, .barra_teclas = barra_teclas, .andamio = andamio, .fila = fila, .dato = dato,
    .boton = boton, .tarjeta = tarjeta, .chip = chip, .interruptor = interruptor, .casilla = casilla, .opcion = opcion,
    .deslizador = deslizador, .dialogo = dialogo, .dialogo_boton = dialogo_boton, .aviso_breve = aviso_breve,
    .pintar_progreso = pintar_progreso, .pintar_desp = pintar_desp, .pintar_campo = pintar_campo, .selector = selector,
    .espera_reloj = espera_reloj, .espera_teclas = espera_teclas,
    .celda_menu = celda_menu, .tecla = tecla, .espera_fondo = espera_fondo };
