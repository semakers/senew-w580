/* ui.h — «un Flutter nuestro en C» para el W600. API pública. Diseño: docs/ui.md.
 * C99 (gnu99), sin libc, sin coma flotante, sin recolector. Portable: teléfono y PC. */
#ifndef UI_H
#define UI_H
#include <stddef.h>
#ifndef HAL_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
#endif
typedef signed short i16; typedef signed int i32;

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);

/* ---- pantalla y color ------------------------------------------------------------------- */
#include "placa_conf.h"                  /* (P600, paso 2) el tamaño de pantalla, de UN sitio: placas/<modelo>/placa_conf.h */
/* (estética, giro, 26-oct) UI_ANCHO y UI_ALTO son el tamaño LÓGICO de ahora: 240x320 derecho o boca abajo, 320x240 de lado
 * (ui_poner_giro). El cuadro ocupa los mismos píxeles en las cuatro orientaciones y su paso de fila es UI_ANCHO. Lo que necesita
 * un tamaño de compilación (reservas, tablas) usa UI_PIXELES o UI_LADO_MAX. */
extern short ui_ancho_, ui_alto_;
#define UI_ANCHO ((int)ui_ancho_)
#define UI_ALTO  ((int)ui_alto_)
#define UI_PIXELES (PLACA_ANCHO * PLACA_ALTO)
#define UI_LADO_MAX (PLACA_ANCHO > PLACA_ALTO ? PLACA_ANCHO : PLACA_ALTO)
enum { UI_GIRO_0, UI_GIRO_DER, UI_GIRO_180, UI_GIRO_IZQ };   /* hacia dónde queda el ARRIBA del cuadro en el panel (hw_pantalla_girar) */
int  ui_giro(void);
int  ui_poner_giro(int giro);           /* 1 = hecho (lo redispone y lo repinta todo); 0 = la plataforma no sabe girar */
typedef u16 Color;                      /* RGB565 con el byte alto primero, como lo quiere el panel */
#define RGB(r, g, b) ((Color)((((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)) >> 8) | \
                              (((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)) << 8)))
#define TRANSPARENTE ((Color)0x0020)    /* un verde casi negro que no se usa: «sin color» */
Color ui_mezcla(Color a, Color b, int t);          /* t = 0..256 */

typedef struct { i16 x, y, w, h; } Rect;
typedef struct { i16 minw, maxw, minh, maxh; } Restr;   /* restricciones de caja */
#define INF 0x3FFF

/* ---- lienzo: dibujo con recorte sobre el cuadro ------------------------------------------- */
typedef struct { u8 w, h; const u16 *filas; } Icono;                 /* 1 bit por píxel, bit 15 = izquierda */
typedef struct { u16 w, h; const Color *pix; Color transparente; u8 opaca; } Imagen;   /* RGB565_BE; opaca = 1: quien la hizo garantiza que NINGÚN píxel vale «transparente» (li_imagen copia filas enteras) */
typedef struct Lienzo { Color *cuadro; Rect recorte; } Lienzo;
void li_rect(Lienzo *l, int x, int y, int w, int h, Color c);
void li_marco(Lienzo *l, int x, int y, int w, int h, Color c);
/* Texto. La fuente es una sola (5x7 dibujada a mano) con dos mejoras hechas al pintar:
 *  - PROPORCIONAL por defecto: cada glifo ocupa lo que mide (la «i» 1 columna, la «m» 5); los dígitos
 *    conservan las 5 columnas para que un reloj no baile. F_MONO vuelve a la celda fija de 6.
 *  - SUAVIZADA en las escalas 2 y 4 (algoritmo EPX: las diagonales salen redondeadas, no a escalones).
 *    F_BASTA lo quita. */
enum { F_MONO = 1, F_BASTA = 2 };
void li_texto(Lienzo *l, int x, int y, const char *utf8, Color c, int escala);
int  li_ancho_texto(const char *utf8, int escala);
void li_texto_f(Lienzo *l, int x, int y, const char *utf8, Color c, int escala, int marcas);
int  li_ancho_texto_f(const char *utf8, int escala, int marcas);
int  li_ancho_n(const char *utf8, int bytes, int escala, int marcas);     /* ancho de los primeros «bytes» */
int  li_partir(const char *utf8, int ancho, int escala, int marcas);     /* bytes que caben en una línea (corta en espacio o en \n) */
void li_texto_n(Lienzo *l, int x, int y, const char *utf8, int bytes, Color c, int escala, int marcas);
void li_velo(Lienzo *l, int x, int y, int w, int h);                     /* oscurece a la mitad lo ya pintado */
void li_linea_h(Lienzo *l, int x, int y, int w, Color c);
void li_punto(Lienzo *l, int x, int y, Color c);
u32  ui_utf8(const char **p);                                            /* siguiente punto de código; avanza */
#define LI_ALTO_LINEA 10                                                 /* 9 de glifo (7 + 2 de descendente) + 1 */
int  li_descendentes(const char *utf8);                                  /* 1 si el texto tiene g, j, p, q o y */
void li_icono(Lienzo *l, int x, int y, const Icono *i, Color c, int escala);
void li_imagen(Lienzo *l, int x, int y, const Imagen *im);
const Imagen *ui_imagen_fs(const char *ruta_estatica);              /* GIF del sistema de ficheros → Imagen (o NULL) */
/* GIF con vida propia: se abre, se pinta g->im, se avanza con ui_gif_siguiente y se cierra (libera). */
typedef struct UiGif { u8 *fich; const u8 *pal_global; u32 n, pos, inicio; Imagen im; Color *pix;
                       u16 espera_ms, cuadro; u16 rx, ry, rw, rh; u8 limpiar;   /* rx..rh: el cuadro anterior, YA recortado al lienzo */
                       u8 formato, reduccion, parcial; u16 w_orig, h_orig; } UiGif;      /* formato: UI_F_* */
enum { UI_F_GIF, UI_F_PNG, UI_F_JPEG };
/* Abre una imagen del sistema de ficheros: GIF (animados incluidos), PNG o JPEG base (por su firma, no por
 * la extensión). PNG y JPEG se decodifican con img/ a como mucho la pantalla (reducidos 1/2, 1/4 o 1/8 si son
 * mayores; el alfa, mezclado sobre negro). NULL si no se puede; el motivo queda en ui_img_error (IMG_E_* de img.h, 0 = no es una imagen). */
UiGif *ui_gif_abrir(const char *ruta);
UiGif *ui_gif_abrir_fondo(const char *ruta);   /* (W600) para un fondo de pantalla: la imagen CUBRE la pantalla (se recorta) */
extern int ui_img_error;
int    ui_gif_siguiente(UiGif *g);                                  /* ms del cuadro nuevo; 0 = no está animado */
void   ui_gif_soltar_fichero(UiGif *g);                             /* ya no se va a animar: libera el fichero */
void   ui_gif_cerrar(UiGif *g);
extern const u8 fuente_5x7[][7];
unsigned fuente_indice(u32 punto_de_codigo);

/* ---- las tres capas ------------------------------------------------------------------------ */
typedef struct Clase Clase;
typedef struct Elem Elem;
typedef Elem Ctx;                       /* como en Flutter: el BuildContext ES el elemento */

/* 1. Widget: configuración inmutable y barata. Vive en la ARENA DEL CUADRO: dura un cuadro. */
typedef struct Widget { const Clase *cls; u32 clave; } Widget;

enum { T_SIN_ESTADO, T_CON_ESTADO, T_PROXY, T_RENDER };
enum { H_0, H_1, H_N };                 /* hijos tras la base: ninguno, «Widget *hijo» o «Widget **hijos» */
enum { C_NADA = 0, C_PINTAR = 1, C_DISPONER = 3 };

struct Clase {
    const char *nombre;
    u8  tipo, hijos, recorta, enfocable;
    u16 tam;                            /* sizeof del widget */
    u16 tam_estado;                     /* estado del componente / datos del objeto de render */
    /* componentes */
    Widget *(*construir)(Ctx *c);
    void (*iniciar)(Ctx *c);            /* también para render: tras montar */
    void (*desechar)(Ctx *c);
    int  (*tecla)(Ctx *c, int tecla);   /* 1 = consumida; si no, sube al padre */
    void (*foco)(Ctx *c, int ganado);   /* el foco entra o sale de este elemento */
    void (*mostrar)(Elem *e, Rect r);   /* contenedor con desplazamiento: «haz visible r» (relativo a e) */
    /* objeto de render */
    void (*disponer)(Elem *e, Restr r); /* fija e->an, e->al y la posición de sus hijos */
    void (*pintar)(Elem *e, Lienzo *l, int x, int y);          /* solo lo suyo; los hijos van después */
    void (*pintar_encima)(Elem *e, Lienzo *l, int x, int y);   /* tras los hijos (p. ej. barra de desplazamiento) */
    int  (*actualizar)(Elem *e, const Widget *nuevo);          /* NULL = comparar bytes; devuelve C_* y copia */
    int  (*opaco)(Elem *e);             /* 1 = tapa todo su rectángulo (ahorra el borrado previo) */
};

/* 2. Elemento: la identidad que persiste entre cuadros. Montículo, vida explícita.
 * 3. Objeto de render: geometría y pintado; vive incrustado en su elemento (tipo T_RENDER). */
struct Elem {
    const Clase *cls; u32 clave;
    Elem *padre;
    Widget *w;                          /* copia propia de la configuración */
    const Widget *origen;               /* el widget original (atajo de identidad si es estático) */
    void *estado;
    Elem **hijos; Elem *uno; u16 n, cap;
    u16 marcas;
    i16 x, y, an, al;                   /* render: posición relativa al render padre y tamaño */
    Restr r;                            /* render: últimas restricciones */
    Rect pintado;                       /* render: rectángulo visible en el último pintado */
};
enum { M_SUCIO = 1, M_DISPONER = 2, M_PINTAR = 4, M_SUB = 8 };      /* M_SUB: algo de debajo hay que repasarlo al calcular el daño */

/* ---- construir widgets --------------------------------------------------------------------- */
void    *ui_nuevo(const void *plantilla, size_t tam);                /* copia a la arena del cuadro */
Widget **ui_lista(Widget *const *v, size_t n);                       /* ídem; quita los NULL y termina en NULL */
char    *ui_fmt(const char *f, ...);                                 /* %d %u %x %s %c, con 0 y ancho; arena */
void    *ui_arena(size_t n);
#define UI_W(Tipo, ...) ((Widget *)ui_nuevo(&(Tipo){ .base = { &Cls##Tipo, 0 }, __VA_ARGS__ }, sizeof(Tipo)))
#define HIJOS(...) ui_lista((Widget *[]){ __VA_ARGS__ }, sizeof((Widget *[]){ __VA_ARGS__ }) / sizeof(Widget *))
#define CLAVE(k) .base.clave = (k)

/* ---- dentro de construir(): estado, configuración, herencia --------------------------------- */
#define ui_estado(c)  ((c)->estado)
#define ui_widget(c)  ((const void *)(c)->w)
void ui_sucio(Ctx *c);                                               /* el setState: «he cambiado, reconstrúyeme» */
const void *ui_heredado(Ctx *c, const Clase *cls);
Ctx *ui_ancestro(Ctx *c, const Clase *cls);                         /* el componente de esa clase que me contiene */                   /* el widget heredado más cercano o NULL */

/* para clases de render */
Elem *ui_render(Elem *e);                                            /* primer render bajando por componentes */
Elem *ui_hijo(Elem *e, unsigned i);                                  /* render del hijo i (o NULL) */
void  ui_disponer(Elem *hijo, Restr r);                              /* con caché: no baja si nada cambió */
void  ui_marcar_disponer(Elem *e);
void  ui_marcar_pintar(Elem *e);
Rect  ui_rect_abs(Elem *e);

/* ---- teclas, foco, rutas ------------------------------------------------------------------- */
enum { K_NADA, K_ARRIBA, K_ABAJO, K_IZQ, K_DER, K_OK, K_SEL_IZQ, K_SEL_DER, K_ATRAS, K_C,
       K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_AST, K_ALM,
       K_VOL_MAS, K_VOL_MENOS, K_WEB, K_PLAY, K_JUEGO_A, K_JUEGO_B, K_WALKMAN, K_CAMARA, K_BLOQUEO, K_ENCENDIDO, K_N };
void ui_tecla(int k);                   /* entrega: foco → ancestros → acciones por defecto */
void ui_enfocar(Elem *e);
Elem *ui_foco(void);
void ui_empujar(Widget *pantalla);      /* se aplican al empezar el cuadro siguiente */
void ui_sacar(void);
int  ui_rutas(void);
void ui_empujar_emergente(Widget *w);   /* ruta NO opaca: se pinta encima de la anterior, que sigue viva (diálogos, menús) */
void ui_reconstruir_todo(void);         /* marca la raíz de todas las rutas (p. ej. al cambiar el tema) */
void ui_redisponer_todo(void);          /* (MD) ídem y además vuelve a medir TODO (al cambiar de diseño: cambia la letra) */
void ui_quitar_ruta(const Clase *cls);  /* quita la ruta cuya raíz es un componente de esa clase, esté arriba o debajo de otras (nunca la primera) */
int  ui_hay_ruta(const Clase *cls);
int  ui_cima_emergente(void);           /* 1 si la ruta de arriba es un emergente (diálogo, menú de opciones) */
void ui_filtro(int (*f)(int k));        /* f ve CADA tecla de la plataforma antes que el foco; si devuelve 1, se la queda (bloqueo, combinaciones globales) */
void ui_pantalla_activa(int si);        /* 0 = pantalla apagada: el estado sigue al día pero no se pinta ni se vuelca; 1 = repinta todo */
void ui_forzar_todo(void);              /* el cuadro siguiente se repinta ENTERO (al volver de una app a pantalla completa) */
int  ui_ocupada(void);                  /* 1 si hay tareas, animaciones o navegación pendientes */
Ctx *ui_raiz(void);                     /* el elemento raíz de la ruta de arriba (NULL si no hay rutas) */
u32  ui_proximo_ms(void);               /* ms hasta el temporizador más cercano; 0xFFFFFFFF = ninguno */

/* ---- tiempo: temporizadores, animaciones, tareas (mueren con su dueño) ----------------------- */
typedef void (*Retro)(Ctx *c);
void ui_cada(Ctx *c, u32 ms, Retro f);
void ui_tras(Ctx *c, u32 ms, Retro f);
void ui_cancelar(Ctx *c, Retro f);      /* (pista V) quita los ui_cada / ui_tras de c con esa retrollamada: quien anima solo a ratos no deja un temporizador latiendo */
typedef struct { u32 inicio; u16 dur; u8 activa, _r; } Anim;
void ui_anim(Ctx *c, Anim *a, u32 ms);  /* arranca; mientras dure, c se reconstruye en cada cuadro */
int  ui_anim_t(const Anim *a);          /* 0..256 lineal; 256 si ya acabó o no arrancó */
int  ui_curva_sal(int t);               /* frena al final (cúbica) */
int  ui_curva_entra_sal(int t);
#define ui_interp(a, b, t) ((a) + (((b) - (a)) * (t)) / 256)
typedef int (*Tarea)(Ctx *c, void *dato);          /* 1 = sigo; 0 = he terminado */
void ui_tarea(Ctx *c, Tarea f, void *dato);

/* ---- el bucle ------------------------------------------------------------------------------ */
void ui_init(Widget *(*raiz)(void));    /* monta la primera ruta */
int  ui_paso(void);                     /* teclas + temporizadores + tareas + un cuadro si hace falta; 0 = nada que hacer */
int  ui_cuadro(void);                   /* 0 = no había nada sucio */
extern Color ui_pantalla[];
extern const unsigned ui_pantalla_tam;   /* UI_ANCHO*UI_ALTO píxeles útiles; la reserva real va redondeada a página y alineada a 4 KiB (ui_nucleo.c, PM) */
void ui_repintar_en(Color *destino);    /* pruebas: pinta la ruta de arriba ENTERA en otro cuadro */

/* ---- memoria y medidas --------------------------------------------------------------------- */
void *mo_pedir(size_t n);               /* montículo propio; si se agota, para el sistema */
void *mo_probar(size_t n);              /* ídem, pero devuelve NULL (para lo grande y prescindible: imágenes, ficheros) */
void  mo_soltar(void *p);
typedef struct {
    u32 arena_pico, arena_tam, monticulo_usado, monticulo_pico, monticulo_tam, bloques, elementos;
    u32 cuadros, construidos, dispuestos, pintados;          /* acumulados */
    Rect dano; u32 dano_px, dano_rects;                      /* del último cuadro: envolvente, píxeles y n.º de rectángulos */
    u32 t_construir, t_disponer, t_pintar, t_volcar;         /* cuentas de plat_cuenta() del último cuadro */
} UiMedidas;
extern UiMedidas ui_med;

/* ---- lo que pone la plataforma (plat/) ------------------------------------------------------ */
void plat_init(void);
u32  ui_arena_libre(void);              /* (C/R3) bytes que quedan en la arena de este cuadro (lo usa el cargador de apps) */
u32  plat_ms(void);
int  plat_tecla(void);                  /* siguiente pulsación (K_*) o K_NADA */
int  plat_abajo(int k);                 /* ¿sigue pulsada ahora mismo esa tecla? (pulsación larga, juegos) */
void plat_volcar(const Color *cuadro, int y0, int filas);
int  plat_girar(int giro);              /* (giro) 0 = hecho (hw_pantalla_girar) */
void plat_reposo(void);
void plat_log(const char *s);
void plat_panico(const char *que);      /* fallo sin salida (memoria agotada…): pantalla de fallo y reinicio. No vuelve */
u32  plat_cuenta(void);                 /* contador de medida (0 si no hay) */
u32  plat_fichero(const char *ruta, u8 *buf, u32 max);   /* lee del FS; con buf NULL, el tamaño; 0 = no hay */

#include "ui_widgets.h"
#endif
