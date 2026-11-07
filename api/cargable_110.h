/* cargable_110.h — (1.10, 27-oct) EL MICRÓFONO PARA APPS, visto desde el SISTEMA (docs/apps_cargables.md §5.12; docs/dictado.md).
 *
 * El lado de las apps es cargables/sdk/w5_microfono.h. Este fichero repite, para el sistema, lo MISMO y con los MISMOS números (las funciones en el
 * mismo orden y con la misma firma, las constantes, la estructura y W5_MIC_MENOR); «make probar-mic-abi» lo comprueba.
 *
 * QUÉ TRAE LA 1.10: el micrófono (hal/mic_w580.c) SIN ESPERAS: el SISTEMA (shell/micro_sis.c) lo arranca por pasos desde su bucle (el DSP tarda ~2-3 s),
 * el tic de 4 ms vacía el anillo de 10 ms del DSP a uno del sistema de ~2 s, y la app toma lo acumulado con mic_tomar. Lo usan también el DICTADO del
 * sistema (shell/dictado.c): uno a la vez. La música por el DSP se PARA al abrirlo y vuelve al cerrarlo (sh_musica_ceder_dsp, decisión del mantenedor).
 * mic_wav_guardar escribe un WAV en /tarjeta/music (donde el original deja sus «Grabación (N).amr»). OPCIONAL, como la red: sin CG_PIDE.
 * En el banco de PC hay un micrófono de MENTIRA (un tono de 440 Hz, o el WAV de SIS_MIC_WAV en bucle).
 *
 * Dónde va el grupo: DETRÁS de la cámara (1.9). W5_API (cargable.h) NO cambia. El sistema dice menor 10 (CG_API_MENOR). Las envolturas, en shell/cg110.c. */
#ifndef CARGABLE_110_H
#define CARGABLE_110_H
#include "cargable_19.h"

#define W5_MIC_MENOR 10                        /* la menor de la API que trae este grupo */
#define W5_MIC_HZ 8000                         /* muestras por segundo: PCM de 16 bits con signo, mono */
#define W5_MIC_CARPETA "/tarjeta/music"        /* donde guarda mic_wav_guardar (la de las grabaciones del original); se lista con fs_listar */
#define W5_MIC_WAV_MAX (60 * W5_MIC_HZ)        /* lo más largo que guarda mic_wav_guardar: 60 s (960 KB) */
enum { W5_CAP_MICROFONO = 1 << 27 };           /* hay micrófono (el W580; en el banco de PC, uno de mentira) */
/* W5MicInfo.estado */
enum { W5_MIC_CERRADO = 0, W5_MIC_PREPARANDO, W5_MIC_GRABANDO, W5_MIC_FALLO };
typedef struct {
    unsigned char estado, mentira, musica, ocupado;   /* W5_MIC_*; 1 = el de mentira del PC; 1 = paró la música (vuelve al cerrar); 1 = lo tiene el dictado */
    int error;                                 /* el último fallo al prepararlo (< 0; 0 = ninguno) */
    unsigned muestras;                         /* las que ha tomado la app desde mic_abrir */
    unsigned desbordes, perdidas;              /* veces que el sistema llegó tarde al DSP (pudo perderse audio) y muestras tiradas porque nadie las tomaba */
    unsigned pico;                             /* el valor absoluto más alto de los últimos ~100-200 ms (0..32768): para un vúmetro */
    unsigned ms_preparar;                      /* lo que tardó en estar listo (0 mientras prepara) */
} W5MicInfo;

/* LAS MISMAS LÍNEAS que W5_API110 de cargables/sdk/w5_microfono.h */
#define W5_API110(F) \
    F(int, mic_abrir, (void))                            /* EMPIEZA a prepararlo (no espera). W5_OK (también si ya estaba) / W5_NO_DISPONIBLE / W5_ERROR (lo tiene el dictado) */ \
    F(int, mic_cerrar, (void))                           /* lo apaga (y la música vuelve, si la paró). W5_OK */ \
    F(int, mic_tomar, (short *dst, unsigned max))        /* copia las muestras acumuladas, sin esperar: cuántas (0 = aún nada); W5_ERROR (cerrado o dst no vale) */ \
    F(int, mic_info, (W5MicInfo *i))                     /* la COPIA a i. W5_OK / W5_ERROR */ \
    F(int, mic_wav_guardar, (const short *pcm, unsigned n, char *nombre))   /* n muestras (≤ W5_MIC_WAV_MAX) a W5_MIC_CARPETA como «Grabación (N).wav»; su nombre a nombre[32]. W5_OK / W5_NO_DISPONIBLE / W5_ERROR */

#endif
