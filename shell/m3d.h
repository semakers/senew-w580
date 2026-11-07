/* m3d.h — el dibujo del menú 3D (pista V, docs/menu3d.md). Interno del shell.
 *   m3d_dibujo.c  los estilos, las rampas de color, el icono (malla -> rasterizador r3d -> 2x2 -> pantalla),
 *                 el halo del foco y el fondo en paralaje. No sabe de widgets ni de teclas.
 *   menu3d.c      el menú: widgets, foco, animación y de dónde sale la inclinación.
 *   inclina.c     del acelerómetro al vector de inclinación.
 * REGLA: todo lo que decide qué píxeles salen va en M3Icono / en los argumentos; el pintado no lee nada «vivo».
 * Así un repintado parcial y uno completo dan lo mismo (lo comprueba el PC tras cada cuadro). */
#ifndef M3D_H
#define M3D_H
#include "ui.h"
#include "app_externa.h"            /* AppMalla: el formato de malla es público desde la pista M2 */

enum { M3_CLASICO, M3_WALKMAN, M3_CRISTAL, M3_NEON, M3_MEDIOS, M3_ESCRITORIO, M3_ESTILOS };   /* (6-nov) M3_ESCRITORIO: el escritorio con
                                                                                * cursor (shell/menu_escritorio.c): otro menú, sin mallas de estilo propio */   /* (28-oct) M3_MEDIOS: el menú de MEDIOS (shell/menu_medios.c),
                                                                                * el lineal de Sony (Media Center del A200 -> XMB): otro menú,
                                                                                * no una rejilla; sus iconos son estas mallas en blanco */
extern const char *const m3_estilo_nombre[M3_ESTILOS];
/* (28-oct) EL COLOR DE MEDIOS: la plantilla es el rojo del XMB; los demás colores salen girando su TONO (Plata y Negro, quitándole la
 * saturación). t = SH_MEDIOS_ROJO..SH_MEDIOS_NEGRO (shell/shell.h; sin «según el mes»: eso lo resuelve sh_medios_tono). */
void  m3_medios_color(int t);
int   m3_medios_color_actual(void);
void  m3_medios_rgb(u8 *rgb);           /* un color de la plantilla roja, al color de ahora (en su sitio) */
Color m3_medios_c(Color c);             /* lo mismo para un Color del panel */

typedef AppMalla M3Malla;              /* (api/app_externa.h) pos x, y, z (1.0 = 100) · nor x127 · tri: 6 bytes por triángulo
                                        * (3 posiciones, 3 normales) agrupados por material · nt[5]: triángulos de cuerpo,
                                        * secundario, claro, oscuro, acento · natural: RGB de cuerpo, secundario, acento */

typedef struct {
    u8  malla, estilo;
    u8  foco;                          /* 0..255: halo y sombra del icono con foco */
    u8  luz;                           /* 255 = normal; menos = apagado (mientras se abre otra app) */
    u8  fino;                          /* 1 = suavizado (2x2); 0 = basto y directo a la pantalla (mientras se inclina) */
    u8  _r;
    i16 guinada, cabeceo;              /* giro sobre el eje vertical / horizontal, en 1/4096 de vuelta */
    i16 dx, dy;                        /* desplazamiento del centro, en 1/16 de píxel */
    u16 radio;                         /* píxeles (x256) que mide 1.0 de la malla */
} M3Icono;

int  m3_malla_de_icono(int icono_del_shell);          /* IC_* -> índice de malla (la genérica si no tiene) */
int  m3_malla_externa(const M3Malla *m);              /* (M2) apunta la malla de una app externa y da su índice; -1 si no
                                                       * vale o no quedan huecos (APP_MALLAS_MAX). La misma malla, el mismo índice */
#define M3_SEN_MAX 7                                     /* (ICO) huecos para las mallas de las .sen del menú (= CG_MENU). (28-oct) 4 -> 7:
                                                       * + las 3 de fábrica que traen la suya (Cámara, YT Music, Grabadora) */
int  m3_malla_sen(int hueco, const M3Malla *m);      /* (ICO) pone (o quita, con NULL) la malla del hueco y da su índice; -1 si
                                                       * no hay. La malla ya VALIDADA (shell/cg_icono.c); cambiarla rehace sus rampas */
void m3_icono_pintar(Lienzo *l, Rect celda, const M3Icono *ic);
/* (28-oct, Medios) El icono FINO a un sprite de w x h (w, h <= 80 x 64) en vez de a la pantalla: pix = el color de lo cubierto (RGB565
 * NATIVO), cob = cuántas de las 4 muestras del 2x2 cubre (0 = nada). Devuelve 0 si no sale nada. Y su pegado sobre lo que hay,
 * con alfa 0..256 (el borde se mezcla como en m3_icono_pintar: lo cubierto más lo que había, por cuartos). Sin sombra ni halo. */
int  m3_icono_sprite(const M3Icono *ic, int w, int h, u16 *pix, u8 *cob);
void m3_sprite_pintar(Lienzo *l, int x, int y, int w, int h, const u16 *pix, const u8 *cob, int alfa);
void m3_resplandor(Lienzo *l, int cx, int cy, int rx, int ry, u32 color565, int fuerza);   /* una mancha difusa (el halo), 0..256 */
void m3_halo(Lienzo *l, int cx, int cy, int rx, int ry, u32 color565, int fuerza);         /* el halo del foco, con su mapa guardado */

/* El fondo: una imagen por estilo algo mayor que la pantalla (M3_MARGEN por cada lado), hecha una vez.
 * `celdas`: cuántas celdas de la rejilla tienen app (algún estilo dibuja algo debajo de cada una);
 * `rejilla`: el rectángulo de la rejilla en pantalla. m3_fondo_preparar devuelve 1 si la ha rehecho. */
#define M3_MARGEN 8
#define M3_COLS (UI_ANCHO > UI_ALTO ? 5 : 3)                     /* (giro) de lado, 5 columnas x 3 filas: caben las 15 sin desplazar */
int  m3_fondo_preparar(int estilo, int celdas, Rect rejilla, int alto_celda);
void m3_fondo_pintar(Lienzo *l, Rect r, int ox, int oy);   /* ox, oy en -M3_MARGEN..M3_MARGEN */

/* Los colores de la interfaz de cada estilo (título, textos de las barras). */
typedef struct { Color titulo, texto, tenue, sombra; u8 velo; } M3Tinta;
const M3Tinta *m3_tinta(int estilo);
#endif
