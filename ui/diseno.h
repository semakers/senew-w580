/* diseno.h — (MD) la capa 2 y la capa 3 del sistema de diseño. docs/material.md §1.
 *
 *   capa 1  NÚCLEO (ui.h, ui_nucleo.c, ui_widgets.c…): árbol, estado, disposición, foco, animación. Sin estética.
 *   capa 2  COMPONENTES SEMÁNTICOS + TOKENS (este fichero, la parte «dis_*» pública): lo que usan el shell y las apps.
 *           Los nombres dicen QUÉ es cada cosa (BarraSuperior, Fila, Botón principal…), nunca cómo se ve.
 *   capa 3  DISEÑOS intercambiables (DisenoTabla): una tabla de funciones que rellena los tokens y dibuja los componentes.
 *           De serie hay dos: «material3» (dis_material3.c) y «clasico» (dis_clasico.c, el aspecto de siempre).
 *
 * REGLA: en la capa 2 no entra ningún término de un diseño concreto (nada de «M3», «FAB», «elevación»): una app escrita contra
 * estos nombres funciona igual con cualquier diseño. La tabla tiene versión (DIS_INTERFAZ) para que un día un diseño pueda
 * llegar como FICHERO (cargable, como una app): docs/material.md §1.4. Hoy los diseños están compilados en el sistema. */
#ifndef DISENO_H
#define DISENO_H
#include "ui.h"

/* ---- tokens: lo que lee quien pinta algo propio (como Theme.of(context)) ------------------------------------------ */
typedef struct {
    Color primario, sobre_primario, contenedor_primario, sobre_contenedor_primario;
    Color secundario, contenedor_secundario, sobre_contenedor_secundario;
    Color terciario, contenedor_terciario, sobre_contenedor_terciario;
    Color error, sobre_error, contenedor_error, sobre_contenedor_error;
    Color superficie, sobre_superficie, sobre_superficie_variante;
    Color superficie_contenedor_minimo, superficie_contenedor_bajo, superficie_contenedor,
          superficie_contenedor_alto, superficie_contenedor_maximo;
    Color contorno, contorno_variante, superficie_inversa, sobre_superficie_inversa, primario_inverso;
    Color velo;                         /* el que oscurece lo de debajo de un diálogo */
} DisColores;
enum { DIS_COL_N = sizeof(DisColores) / sizeof(Color) };
typedef struct {
    u8 radio_peq, radio_med, radio_gra, radio_total;   /* esquinas (px); radio_total = píldora */
    u8 rejilla;                                        /* el paso de los espaciados (px) */
    u8 oscuro;                                         /* 1 = esquema oscuro */
    u16 anim_corta, anim_media;                        /* ms */
    u8 alto_barra_superior, alto_barra_teclas, alto_fila, alto_fila_doble;
    u8 alto_celda_menu;                                 /* la rejilla del menú principal */
    u8 alto_linea;                                      /* (estética) el paso de una línea de texto de cuerpo: lo que antes era LI_ALTO_LINEA (10) */
} DisMedidas;
typedef struct { DisColores c; DisMedidas m; } DisTokens;

/* tipografía semántica: el estilo va en las marcas del texto (bits 4..6), junto a F_MONO y F_BASTA.
 * Sin estilo (0) manda la escala, como siempre: 1 = cuerpo, 2 = titular, 3 = display, 4 = display grande.
 * (estética, 26-oct) DIS_T_PEQUENA: la talla pequeña en negrita del original (segundas líneas, la hora de la barra, ayudas). Va al
 * final para no mover los números de los demás (las apps de la API 1.3 los mandan en las marcas). */
enum { DIS_T_ESCALA, DIS_T_CUERPO, DIS_T_ETIQUETA, DIS_T_TITULO, DIS_T_TITULAR, DIS_T_DISPLAY, DIS_T_DISPLAY_G, DIS_T_PEQUENA, DIS_T_N };
#define F_ESTILO(e) ((e) << 4)
#define F_ESTILO_DE(m) (((m) >> 4) & 7)

/* ---- una fuente de mapas con grises (4 bits por píxel): la pone un diseño; NULL = el 5x7 de siempre ---------------- */
typedef struct { u8 w, h; signed char dx, dy; u8 avance, _r; u16 desp; } DisGlifo;   /* dy: desde arriba de la línea */
typedef struct { u8 alto, base, n; const DisGlifo *g; const u8 *datos; } DisFuente;

/* ---- símbolos: un icono del sistema se registra con su NOMBRE semántico; un diseño con símbolos propios lo pinta a su manera
 * (con grises) y uno sin ellos pinta el Icono de 1 bit de siempre. El orden es el de herr/diseno/hacer_simbolos.py. */
enum { DIS_S_ALARMA, DIS_S_CANDADO, DIS_S_ENCHUFE, DIS_S_NOTA, DIS_S_RAYO, DIS_S_CARPETA, DIS_S_FICHERO, DIS_S_PASOS,
       DIS_S_IZQ, DIS_S_DER, DIS_S_ARRIBA, DIS_S_ABAJO, DIS_S_HECHO,
       DIS_S_ARCHIVOS, DIS_S_IMAGENES, DIS_S_RELOJ, DIS_S_NOTAS, DIS_S_CALCULADORA, DIS_S_JUEGO, DIS_S_CONSOLA, DIS_S_TECLADO,
       DIS_S_SISTEMA, DIS_S_AJUSTES, DIS_S_TELEFONO, DIS_S_MUSICA, DIS_S_MOVIMIENTO, DIS_S_N };
void dis_icono_es(const Icono *ic, int simbolo);    /* lo llama quien tiene los iconos (shell/iconos.c) */
int  dis_simbolo_de(const Icono *ic);               /* DIS_S_* o -1 */

/* ---- componentes: lo que se le pide a un diseño ------------------------------------------------------------------- */
enum { DIS_PRINCIPAL, DIS_SECUNDARIO, DIS_TERCIARIO };          /* prioridad de un botón */
enum { DIS_ACC_NADA, DIS_ACC_VALOR, DIS_ACC_INTERRUPTOR, DIS_ACC_CASILLA, DIS_ACC_OPCION, DIS_ACC_FLECHA };
typedef struct {
    const char *titulo, *subtitulo;     /* subtítulo NULL = fila de una línea */
    const Icono *icono;                 /* a la izquierda (o NULL) */
    u8 accesorio;                       /* DIS_ACC_*: qué hay a la derecha */
    u8 activo;                          /* interruptor / casilla / opción: 1 = marcado */
    const char *valor;                  /* DIS_ACC_VALOR: el texto de la derecha */
} DisFila;
enum { DIS_DLG_AVISO, DIS_DLG_CONFIRMAR, DIS_DLG_OPCIONES };

#define DIS_INTERFAZ 2                  /* sube si cambia la forma de DisenoTabla (un diseño cargable diría cuál espera). 2: la
                                         * escala de letra nueva (estética): fuente_v1 y medidas_v1 para las apps de antes */
typedef struct DisenoTabla {
    u32 id;                             /* cuatro letras: 'MAT3', 'CLAS' */
    const char *nombre;                 /* «material3», «clasico» */
    u16 version, interfaz;              /* la del diseño · DIS_INTERFAZ con que se hizo */
    u8 n_paletas;                       /* cuántas variantes de color ofrece (claro, oscuro…) */
    const char *const *paleta_nombre;
    void (*tokens)(int paleta, DisTokens *t);
    const DisFuente *fuente[DIS_T_N];   /* por estilo; NULL = el 5x7 escalado */
    const DisFuente *fuente_v1[DIS_T_N];/* (estética) la letra de ANTES, para las .sen de API < 1.7 (modo v1: dis_v1) */
    DisMedidas medidas_v1;              /* (estética) y sus medidas (barras, filas, celdas); los colores son los mismos */
    const DisFuente *simbolos, *simbolos_g;   /* DIS_S_* en celdas de 12 y de 24 (escala 2); NULL = los iconos de 1 bit */
    /* los componentes que devuelven un árbol de widgets (se llaman dentro de un construir) */
    Widget *(*barra_superior)(const char *titulo, Widget *derecha);
    Widget *(*barra_teclas)(const char *izq, const char *der);
    Widget *(*andamio)(Widget *barra, Widget *cuerpo, Widget *teclas);
    Widget *(*fila)(const DisFila *f, int sel);
    Widget *(*dato)(const char *nombre, const char *valor);
    Widget *(*boton)(const char *texto, int prioridad, int enfocado);
    Widget *(*tarjeta)(Widget *hijo, int enfocada);
    Widget *(*chip)(const char *texto, int elegido, int enfocado);
    Widget *(*interruptor)(int activo, int enfocado);
    Widget *(*casilla)(int activo, int enfocado);
    Widget *(*opcion)(int activo, int enfocado);
    Widget *(*deslizador)(int valor, int max, int enfocado);
    Widget *(*dialogo)(int tipo, const char *titulo, const char *texto, Widget **botones, int n);   /* botones: ya hechos (Enfocables) */
    Widget *(*dialogo_boton)(const char *texto, int tipo, int i, int enfocado);
    Widget *(*aviso_breve)(const char *texto);                                       /* la tira de abajo */
    /* lo que pintan widgets del núcleo (render): progreso, barra de desplazamiento, campo de texto, selector */
    void (*pintar_progreso)(Lienzo *l, int x, int y, int w, int h, i32 valor, i32 max, Color c, Color fondo);
    void (*pintar_desp)(Lienzo *l, int x, int y, int w, int h, int pos, int largo, Color c);
    void (*pintar_campo)(Lienzo *l, int x, int y, int w, int h, int enfocado);
    Widget *(*selector)(const char *texto, int escala, int enfocado);
    /* la pantalla de espera (el fondo lo pone el sistema): el bloque del reloj y la tira de las teclas, sobre una foto */
    Widget *(*espera_reloj)(const char *hora, const char *seg, const char *fecha);
    Widget *(*espera_teclas)(const char *izq, const char *der);
    /* (segunda vuelta) la celda de la rejilla de apps (t = 0..256: la animación del foco), una tecla de un teclado en pantalla
     * (calculadora: rol DIS_TECLA_*), y el fondo de la espera cuando no hay foto */
    Widget *(*celda_menu)(const Icono *ic, const char *nombre, int enfocado, int t);
    Widget *(*tecla)(const char *texto, int rol, int enfocado);
    Color (*espera_fondo)(void);
} DisenoTabla;
enum { DIS_TECLA_NUMERO, DIS_TECLA_OPERACION, DIS_TECLA_ACCION };

/* ---- la capa 2: lo que se usa ------------------------------------------------------------------------------------- */
extern const DisenoTabla dis_material3, dis_clasico;
const DisenoTabla *dis_actual(void);
const DisTokens *dis_tokens(void);
int  dis_n(void);                                   /* diseños que trae el sistema */
const DisenoTabla *dis_de(int i);
const DisenoTabla *dis_buscar(const char *nombre);  /* NULL si no está */
void dis_poner(const DisenoTabla *d, int paleta);   /* el del sistema (Ajustes) */
void dis_forzar(const DisenoTabla *d);              /* el que pide una app mientras está en pantalla (NULL = soltar) */
const TemaDatos *dis_tema(void);                    /* los 6 colores de siempre, sacados de los tokens (apps de la 1.0–1.2) */
/* (estética, 26-oct) EL MODO V1: con él puesto, el diseño en uso pinta con la letra y las medidas de ANTES de la escala nueva (las
 * que dan por hechas las .sen de API < 1.7, que colocan cosas a mano con líneas de 10*k px). Lo pone el cargador mientras una
 * de esas apps está en pantalla y lo quita al volver. dis_escala_5x7(): a qué escala va el 5x7 de relleno (y el del «clasico»)
 * para una escala/estilo dados: en v1, la de siempre; en la nueva, el doble para el cuerpo. */
void dis_v1(int si);
int  dis_es_v1(void);
const DisFuente *dis_fuente(int estilo);            /* la del diseño en uso para ese estilo (NULL = el 5x7), con el modo v1 */
int  dis_escala_5x7(int escala, int marcas);
int  dis_alto_linea(void);                          /* el paso de una línea de cuerpo (antes LI_ALTO_LINEA = 10) */
int  dis_paso_linea(int marcas);                    /* el paso entre líneas de un texto de varias líneas con esas marcas (v1: 10) */
int  dis_ancho_valor(const DisFila *f, int an, int hueco);   /* el ancho del valor de una fila que no cabe (0 = cabe): ver diseno.c */
/* (estética, giro) dónde van las softkeys según la orientación (ui_giro): de lado, en una COLUMNA junto a las teclas físicas (con el
 * teclado a la derecha, a la derecha: la izquierda abajo y la derecha arriba; con el teclado a la izquierda, al revés); boca abajo, la
 * barra ARRIBA y con las etiquetas cambiadas de lado. Lo que ocupan, para quien calcula sitio a mano: */
#define DIS_ANCHO_COL_TECLAS 72
int  dis_teclas_columna(void);                      /* el ancho de la columna de las softkeys (0 = van en barra) */
int  dis_teclas_barra(void);                        /* el alto de la barra de las softkeys (0 = van en columna) */
Widget *dis_con_teclas(Widget *cuerpo, Widget *teclas);   /* coloca la barra/columna de softkeys donde toque (andamio, diálogos) */

/* componentes (atajos a la tabla en uso) */
Widget *BarraSuperior(const char *titulo, Widget *derecha);
Widget *BarraTeclas(const char *izq, const char *der);
Widget *DisFilaW(const DisFila *f, int sel);
Widget *Boton(const char *texto, int prioridad, int enfocado);
Widget *Tarjeta(Widget *hijo, int enfocada);
Widget *Chip(const char *texto, int elegido, int enfocado);
Widget *Interruptor(int activo, int enfocado);
Widget *Casilla(int activo, int enfocado);
Widget *Opcion(int activo, int enfocado);
Widget *Deslizador(int valor, int max, int enfocado);
void    ui_aviso_breve(const char *texto);          /* la tira que se va sola (snackbar) */

/* primitivas que los diseños comparten */
void li_rect_red(Lienzo *l, int x, int y, int w, int h, int radio, Color c);    /* rectángulo redondeado con bordes suavizados */
void li_marco_red(Lienzo *l, int x, int y, int w, int h, int radio, Color c);   /* su contorno (1 px) */
void li_circulo(Lienzo *l, int cx, int cy, int r, Color c);
void li_glifo4(Lienzo *l, int x, int y, const DisFuente *f, const DisGlifo *g, Color c);           /* ui_lienzo.c */
int  dis_alto_texto(int escala, int marcas, const char *s);                      /* lo que mide de alto una línea con ese estilo */
/* Caja redondeada: un WCaja con radio (widget del núcleo nuevo; lo usan los diseños) */
typedef struct { Widget base; Widget *hijo; i16 an, al; Color color, borde; u8 radio, mx, my, px, py; } WCajaRed;   /* mx/my: margen fuera; px/py: relleno dentro (ahorran dos Relleno por fila: la arena del cuadro es de 24 KiB) */
extern const Clase ClsWCajaRed;
#define CajaRed(...) UI_W(WCajaRed, .color = TRANSPARENTE, .borde = TRANSPARENTE, __VA_ARGS__)
#endif
