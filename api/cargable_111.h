/* cargable_111.h — (1.11, 28-oct) DESCARGAR CANCIONES DE LA RED A LA TARJETA, visto desde el SISTEMA (docs/apps_cargables.md §5.13).
 *
 * El lado de las apps es cargables/sdk/w5_descargas.h. Este fichero repite, para el sistema, lo MISMO y con los MISMOS números; «make probar-desc-abi» lo
 * comprueba. La descarga la hace shell/descarga_red.c (su propio dueño de la red, SH_BT_DESCARGA; la recepción a trozos de la tarjeta, HW_RX_RED).
 * Dónde va el grupo: DETRÁS del micrófono (1.10). El sistema dice menor 11 (CG_API_MENOR). Las envolturas, en shell/cg111.c. */
#ifndef CARGABLE_111_H
#define CARGABLE_111_H
#include "cargable_110.h"

#define W5_DESC_MENOR 11                       /* la menor de la API que trae este grupo */
#define W5_DESC_CARPETA "/tarjeta/music"       /* donde quedan las canciones descargadas (la carpeta de música del original); se lista con fs_listar */
#define W5_DESC_NOMBRE 64                      /* el nombre del fichero con su 0 */
/* W5MuDescarga.fase */
enum { W5_DESC_QUIETA = 0, W5_DESC_CONECTANDO, W5_DESC_PIDIENDO, W5_DESC_RESPUESTA, W5_DESC_BAJANDO, W5_DESC_HECHA, W5_DESC_FALLO };
typedef struct {
    unsigned char fase, ya_estaba, rsv[2];     /* W5_DESC_*; 1 = ya estaba descargada (no se bajó otra vez) */
    int http;                                  /* el estado HTTP (200; 0 = aún no) */
    unsigned bajados, total, esperado;         /* bytes ya escritos, los del Content-Length (0 = no se sabe) y los que se esperan por la duración */
    char nombre[W5_DESC_NOMBRE];               /* «Artista - Título.mp3» (en W5_DESC_CARPETA) */
    char porque[48];                           /* el último fallo, en palabras */
} W5MuDescarga;

/* LAS MISMAS LÍNEAS que W5_API111 de cargables/sdk/w5_descargas.h */
#define W5_API111(F) \
    F(int, mu_red_descargar, (int i))                    /* la canción i de la cola de la música por la red, a W5_DESC_CARPETA como «Artista - Título.mp3»: la baja el SISTEMA (sigue con la app cerrada). W5_OK (también si ya estaba) / W5_ERROR (otra en curso, o i no vale) / W5_NO_DISPONIBLE */ \
    F(int, mu_red_descarga, (W5MuDescarga *d))           /* la COPIA del estado de la descarga en curso o de la última. W5_OK / W5_ERROR */ \
    F(int, mu_red_descarga_cancelar, (void))             /* la deja (no queda nada a medias en la tarjeta). W5_OK */

#endif
