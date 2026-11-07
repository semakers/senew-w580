/* cargable_18.h — (1.8, 26-oct) LA MÚSICA POR LA RED Y LOS PÍXELES, vistos desde el SISTEMA (docs de la música por la red; docs/apps_cargables.md §5.10).
 *
 * El lado de las apps es cargables/sdk/w5_musica_red.h. Este fichero repite, para el sistema, lo MISMO y con los MISMOS números (las funciones en el
 * mismo orden y con la misma firma, las constantes, las estructuras y W5_MURED_MENOR); «make probar-mured-abi» lo comprueba.
 *
 * QUÉ TRAE LA 1.8:
 *   - una COLA de canciones por URL (MP3 por http:// o https://) que toca el SISTEMA (shell/musica_red.c): las baja por la red del puente
 *     Bluetooth (1.6) a su colchón de 192 KB y las da al DSP. SIGUE SONANDO CON LA APP CERRADA, y al acabar una pasa a la siguiente. Pausa,
 *     siguiente, anterior, parar y volumen son los de la música de siempre (mu_alternar, mu_saltar, mu_parar, mu_volumen de la 1.2; las teclas
 *     Play/Walkman/Vol±). Un corte del Bluetooth lo tapa el colchón (~12 s) y luego se reanuda desde el byte que falta (Range).
 *   - li_pixeles: pintar un bloque de píxeles RGB565 (el byte alto primero, como llegan de la red) en el lienzo, recortado.
 *
 * Dónde va el grupo: DETRÁS del de la pantalla (1.7). W5_API (cargable.h) NO cambia. El sistema dice menor 8 (CG_API_MENOR). Las envolturas, en
 * shell/cg18.c. */
#ifndef CARGABLE_18_H
#define CARGABLE_18_H
#include "cargable_17.h"

#define W5_MURED_MENOR 8                       /* la menor de la API que trae este grupo */
#define W5_MURED_MAX 32                        /* canciones en la cola */
#define W5_MURED_URL 192                       /* bytes de una URL, con el 0 */
#define W5_MURED_TITULO 100                    /* bytes del título, con el 0 */
#define W5_MURED_ARTISTA 68                    /* bytes del artista, con el 0 */
/* W5MuRed.fase: en qué va la descarga de la canción actual */
enum { W5_MURED_QUIETA = 0, W5_MURED_CONECTANDO, W5_MURED_PIDIENDO, W5_MURED_RESPUESTA, W5_MURED_BAJANDO, W5_MURED_ENTERA, W5_MURED_FALLO };
typedef struct {
    short actual, n;                           /* la canción de la cola que suena o se baja (-1 = ninguna) y cuántas hay */
    unsigned char fase, red, reintentos, tocando;   /* W5_MURED_*; W5_RED_* de la red; cortes seguidos; 1 = ya suena (0 = aún cargando) */
    unsigned char activa, rsv[3];              /* 1 = lo que toca la música ahora viene de esta cola (mu_info: estado, volumen, pos y dur) */
    int http;                                  /* el último estado HTTP (200, 206, 404…; 0 = aún no) */
    unsigned recibidos, total;                 /* bytes de esta canción ya en el colchón y su tamaño (0 = no se sabe) */
    unsigned colchon, colchon_ms;              /* bytes en el colchón sin dar al DSP, y cuánto audio son (ms) */
    unsigned pos_ms, dur_ms;                   /* por dónde va y cuánto dura (0 = no se sabe) */
    unsigned esperas, ms_esperando;            /* pausas por falta de datos (el colchón se vació) y ms esperando */
    char porque[48];                           /* el último fallo, en palabras («el servidor dice HTTP 404», «la red: se cortó»…) */
} W5MuRed;
typedef struct {
    char url[W5_MURED_URL];
    char titulo[W5_MURED_TITULO];
    char artista[W5_MURED_ARTISTA];
    unsigned dur_ms;
} W5MuRedPista;

/* LAS MISMAS LÍNEAS que W5_API18 de cargables/sdk/w5_musica_red.h */
#define W5_API18(F) \
    F(int, mu_red_vaciar, (void))                        /* la cola vacía (lo que suena sigue hasta que acabe o se pare). W5_OK / W5_NO_DISPONIBLE */ \
    F(int, mu_red_poner, (const char *url, const char *titulo, const char *artista, unsigned dur_ms))   /* al final de la cola: índice >= 0, W5_ERROR, W5_NO_DISPONIBLE */ \
    F(int, mu_red_tocar, (int i))                        /* toca la canción i de la cola (lo que sonara, fuera). W5_OK / W5_ERROR / W5_NO_DISPONIBLE */ \
    F(int, mu_red_info, (W5MuRed *i))                    /* la COPIA a i. W5_OK / W5_ERROR */ \
    F(int, mu_red_pista, (int i, W5MuRedPista *p))       /* la URL y los textos de la canción i de la cola. W5_OK / W5_ERROR */ \
    F(void, li_pixeles, (W5Lienzo l, int x, int y, int w, int h, const void *rgb565))   /* w×h píxeles RGB565 (byte alto primero), con la esquina en (x, y) */

#endif
