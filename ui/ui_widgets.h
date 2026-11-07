/* ui_widgets.h — los widgets de serie. Cada uno: su struct de configuración, su clase y una
 * macro constructora con parámetros con nombre y valores por defecto (inicializadores designados). */
#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

/* ---- disposición --------------------------------------------------------------------------- */
enum { P_INICIO, P_CENTRO, P_FIN, P_ENTRE, P_ESTIRAR };
typedef struct { Widget base; Widget **hijos; u8 vertical, principal, cruzado, encoge; i16 espacio; } WFlex;   /* .encoge = MainAxisSize.min */
extern const Clase ClsWFlex;
#define Columna(...) UI_W(WFlex, .vertical = 1, __VA_ARGS__)
#define Fila(...)    UI_W(WFlex, .vertical = 0, .cruzado = P_CENTRO, __VA_ARGS__)

typedef struct { Widget base; Widget *hijo; u8 flex; } WExpandido;   /* solo dentro de Fila/Columna */
extern const Clase ClsWExpandido;
#define Expandido(h, ...) UI_W(WExpandido, .hijo = (h), .flex = 1, __VA_ARGS__)
#define Hueco()           UI_W(WExpandido, .flex = 1)

typedef struct { Widget base; Widget *hijo; i16 izq, arr, der, aba; } WRelleno;
extern const Clase ClsWRelleno;
#define Relleno(n, h)            UI_W(WRelleno, .hijo = (h), .izq = (n), .arr = (n), .der = (n), .aba = (n))
#define RellenoXY(nx, ny, h)     UI_W(WRelleno, .hijo = (h), .izq = (nx), .arr = (ny), .der = (nx), .aba = (ny))

typedef struct { Widget base; Widget *hijo; signed char ax, ay; } WAlinear;      /* -1, 0, 1 */
extern const Clase ClsWAlinear;
#define Centro(h)          UI_W(WAlinear, .hijo = (h))
#define Alinear(x_, y_, h) UI_W(WAlinear, .hijo = (h), .ax = (x_), .ay = (y_))

/* Caja = SizedBox + Container: tamaño fijo opcional (0 = el del hijo; INF = todo lo que haya),
 * color de fondo y borde opcionales. */
typedef struct { Widget base; Widget *hijo; i16 an, al; Color color, borde; } WCaja;
extern const Clase ClsWCaja;
#define Caja(...) UI_W(WCaja, .color = TRANSPARENTE, .borde = TRANSPARENTE, __VA_ARGS__)

typedef struct { Widget base; Widget **hijos; } WPila;               /* Stack: uno encima de otro */
extern const Clase ClsWPila;
#define Pila(...) UI_W(WPila, .hijos = HIJOS(__VA_ARGS__))

typedef struct { Widget base; Widget **hijos; u8 columnas; i16 alto_celda, espacio; } WRejilla;
extern const Clase ClsWRejilla;
#define Rejilla(...) UI_W(WRejilla, .columnas = 3, __VA_ARGS__)

/* Lista: columna con desplazamiento vertical; sigue al foco. */
typedef struct { Widget base; Widget **hijos; Color barra; } WLista;
extern const Clase ClsWLista;
#define Lista(...) UI_W(WLista, .barra = TRANSPARENTE, __VA_ARGS__)

/* ---- contenido ----------------------------------------------------------------------------- */
/* Texto: una línea. .color TRANSPARENTE = el del tema; .sombra = color de una sombra a (+1,+1) por
 * escala (para leer sobre una foto); .marcas = F_MONO (celda fija) | F_BASTA (sin suavizar). */
typedef struct { Widget base; const char *texto; Color color, sombra; u8 escala, marcas; } WTexto;
extern const Clase ClsWTexto;
#define Texto(s, ...) UI_W(WTexto, .texto = (s), .color = TRANSPARENTE, .sombra = TRANSPARENTE, .escala = 1, __VA_ARGS__)

/* Párrafo: varias líneas, partidas por palabras al ancho que le den. .max_lineas 0 = las que salgan. */
typedef struct { Widget base; const char *texto; Color color; u8 max_lineas, marcas, centrado; } WParrafo;
extern const Clase ClsWParrafo;
#define Parrafo(s, ...) UI_W(WParrafo, .texto = (s), .color = TRANSPARENTE, __VA_ARGS__)

/* Velo: oscurece a la mitad lo que haya pintado debajo de su rectángulo (dentro de una Pila) o, con
 * hijo, debajo del hijo. Es lo que hace legible un texto claro sobre un fondo claro. */
typedef struct { Widget base; Widget *hijo; } WVelo;
extern const Clase ClsWVelo;
#define Velo(h) UI_W(WVelo, .hijo = (h))

/* Barra de desplazamiento vertical suelta (la Lista y la ListaPerezosa ya traen la suya). */
typedef struct { Widget base; i32 total, visibles, pos; Color color; } WBarraDesp;
extern const Clase ClsWBarraDesp;
#define BarraDesp(...) UI_W(WBarraDesp, .color = TRANSPARENTE, __VA_ARGS__)

/* Barra de progreso horizontal: ocupa el ancho disponible. */
typedef struct { Widget base; i32 valor, max; Color color, fondo; i16 al; } WProgreso;
extern const Clase ClsWProgreso;
#define Progreso(...) UI_W(WProgreso, .color = TRANSPARENTE, .fondo = TRANSPARENTE, .al = 6, __VA_ARGS__)

/* Pintura (CustomPaint): la aplicación dibuja con el lienzo dentro del rectángulo r. Se repinta
 * cuando cambia .version (o .dato). .an/.al 0 = todo lo disponible. */
typedef struct { Widget base; void (*pintar)(Lienzo *l, Rect r, void *dato); void *dato; u32 version; i16 an, al; } WPintura;
extern const Clase ClsWPintura;
#define Pintura(f, ...) UI_W(WPintura, .pintar = (f), __VA_ARGS__)

/* Lista perezosa (ListView.builder): .n filas de alto fijo; solo se construyen las que se ven.
 * La selección es suya (un solo enfocable): arriba/abajo mueven, volumen +/- pasan página, OK activa.
 * .fila(c, i, sel, dato) devuelve la fila i (sel = 1 si es la seleccionada y la lista tiene el foco).
 * .sel (opcional) apunta a un entero de la aplicación donde vive el índice seleccionado. */
typedef struct {
    Widget base; i32 n;
    Widget *(*fila)(Ctx *c, int i, int sel, void *dato);
    void (*al_activar)(Ctx *c, int i, void *dato);
    void (*al_mover)(Ctx *c, int i, void *dato);
    void *dato; i32 *sel; i16 alto_fila; u8 autofoco; Color barra;
} WListaP;
extern const Clase ClsWListaP;
#define ListaPerezosa(...) UI_W(WListaP, .alto_fila = 0, .autofoco = 1, .barra = TRANSPARENTE, __VA_ARGS__)   /* (estética) alto_fila 0 = el del diseño en uso (dis_tokens()->m.alto_fila; 18 en el modo v1, como antes) */

/* Selector numérico (un «NumberPicker» de teclado): enfocable; arriba/abajo suman y restan (da la
 * vuelta), los dígitos se teclean. El valor es de la aplicación (.valor apunta a un int suyo). */
typedef struct { Widget base; int *valor; i32 min, max; void (*al_cambiar)(Ctx *c, void *dato); void *dato; u8 digitos, escala, autofoco; } WSelector;
extern const Clase ClsWSelector;
#define Selector(v, ...) UI_W(WSelector, .valor = (v), .max = 99, .digitos = 2, .escala = 2, __VA_ARGS__)

/* ---- entrada de texto por teclado numérico (multipulsación) ------------------------------------
 * El texto vive en un EntradaTexto de la aplicación (como un TextEditingController): un búfer UTF-8
 * suyo, la longitud y el cursor. CampoTexto lo edita:
 *   2..9 = letras por multipulsación (con á é í ó ú ñ ü al final del ciclo), 1 = puntuación,
 *   0 = espacio, 0 y salto de línea; * = símbolos; # = modo abc -> Abc -> ABC -> 123;
 *   C = borrar (mantener = repetir); izq./der. = cursor; mantener un dígito = ese dígito.
 * La letra pendiente se confirma al pasar 900 ms o al pulsar otra tecla. */
enum { ET_ABC, ET_Abc, ET_MAYUS, ET_123 };
typedef struct { char *buf; u16 cap, len, cur; u8 modo, solo_num; u16 version; } EntradaTexto;
void et_iniciar(EntradaTexto *e, char *buf, unsigned cap);   /* búfer vacío */
void et_poner(EntradaTexto *e, const char *s);               /* sustituye el texto; cursor al final */
int  et_insertar(EntradaTexto *e, const char *utf8);         /* en el cursor; 0 = no cabe */
int  et_borrar(EntradaTexto *e);                             /* el carácter de antes del cursor */
const char *et_modo_nombre(const EntradaTexto *e);
typedef struct {
    Widget base; EntradaTexto *e; void (*al_cambiar)(Ctx *c, void *dato); void *dato;
    u8 lineas, autofoco;                                     /* lineas 0 = todo el alto disponible */
} WCampoTexto;
extern const Clase ClsWCampoTexto;
extern int (*ui_dictado_gancho)(Ctx *campo);                   /* (E5) la pulsación larga de «*» en un campo: lo pone shell/dictado.c */
extern void (*ui_campo_muere_gancho)(Ctx *campo);             /* (E5) un CampoTexto se desmonta */
int ui_campo_insertar(Ctx *c, const char *utf8);               /* (E5) el texto dictado en el cursor de ese campo (1 = puesto entero) */
int ui_campo_lineas(Ctx *c);                                   /* (E5) las líneas del campo (1 = de una línea: buscadores, nombres), -1 si no es un campo */
#define CampoTexto(e_, ...) UI_W(WCampoTexto, .e = (e_), .lineas = 1, __VA_ARGS__)

/* ---- emergentes: avisos, confirmaciones y menú de opciones (rutas no opacas) --------------------
 * Los textos se copian; las opciones deben ser cadenas estáticas. La retrollamada llega después de
 * cerrarse el emergente (puede abrir otra pantalla). Atrás, C o la tecla derecha cancelan. */
void ui_aviso(const char *titulo, const char *texto);
void ui_confirmar(const char *titulo, const char *texto, void (*si)(void *dato), void *dato);
void ui_confirmar_no(const char *titulo, const char *texto, void (*si)(void *dato), void *dato);   /* (R2) lo mismo con el foco en «No»: para lo destructivo */
void ui_opciones(const char *titulo, const char *const *ops, int n, void (*elegir)(int i, void *dato), void *dato);

typedef struct { Widget base; const Icono *icono; Color color; u8 escala; } WIcono;
extern const Clase ClsWIcono;
#define Icon(i, ...) UI_W(WIcono, .icono = (i), .color = TRANSPARENTE, .escala = 1, __VA_ARGS__)

typedef struct { Widget base; const Imagen *imagen; } WImagen;
extern const Clase ClsWImagen;
#define Image(im) UI_W(WImagen, .imagen = (im))

/* ---- tema (InheritedWidget) ------------------------------------------------------------------ */
typedef struct { Color fondo, texto, tenue, acento, sobre_acento, barra; } TemaDatos;
typedef struct { Widget base; Widget *hijo; const TemaDatos *datos; } WTema;
extern const Clase ClsWTema;
#define Tema(d, h) UI_W(WTema, .hijo = (h), .datos = (d))
const TemaDatos *tema_de(Ctx *c);
void ui_tema_global(const TemaDatos *t);                             /* el de quien no tiene un Tema encima (emergentes) */

/* ---- foco y teclas ---------------------------------------------------------------------------- */
typedef struct { u8 enfocado; i16 t; } Foco;                         /* t: 0..256, animado si .anim_ms */
typedef struct {
    Widget base;
    Widget *(*constructor)(Ctx *c, const Foco *f, void *dato);
    void (*al_activar)(Ctx *c, void *dato);                          /* tecla OK */
    void (*al_enfocar)(Ctx *c, void *dato);
    void *dato; u16 anim_ms; u8 autofoco;
} WEnfocable;
extern const Clase ClsWEnfocable;
#define Enfocable(...) UI_W(WEnfocable, __VA_ARGS__)

typedef struct { Widget base; Widget *hijo; int (*al_tecla)(Ctx *c, int tecla, void *dato); void *dato; } WTeclas;
extern const Clase ClsWTeclas;
#define Teclas(f, h, ...) UI_W(WTeclas, .hijo = (h), .al_tecla = (f), __VA_ARGS__)

/* ---- para declarar los componentes de la aplicación ------------------------------------------- */
#define COMPONENTE(Nombre, construir_)                                                             \
    const Clase Cls##Nombre = { .nombre = #Nombre, .tipo = T_SIN_ESTADO, .tam = sizeof(Nombre),    \
                                .construir = (construir_) }
#define COMPONENTE_CON_ESTADO(Nombre, Estado, construir_, iniciar_, ...)                           \
    const Clase Cls##Nombre = { .nombre = #Nombre, .tipo = T_CON_ESTADO, .tam = sizeof(Nombre),    \
                                .tam_estado = sizeof(Estado), .construir = (construir_),           \
                                .iniciar = (iniciar_), __VA_ARGS__ }
#endif
