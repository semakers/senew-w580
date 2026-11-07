/* micro_sis.h — (1.10, 27-oct) EL MICRÓFONO, visto desde el sistema (api/cargable_110.h; docs/dictado.md). En el W580: hal/mic_w580.c (el DSP con el grafo de la
 * grabadora del original; el arranque troceado por mic_paso y las muestras recogidas por el tic). En el banco de PC: uno de MENTIRA (un tono de 440 Hz o el WAV de
 * SIS_MIC_WAV, 8 kHz mono 16 bits, en bucle). UN DUEÑO a la vez: la app cargada o el dictado del sistema. Al abrirlo, la música por el DSP se para (y vuelve al cerrar). */
#ifndef MICRO_SIS_H
#define MICRO_SIS_H
#include "hw.h"
enum { SH_MIC_NADIE = 0, SH_MIC_APP, SH_MIC_DICTADO };
enum { SH_MIC_CERRADO = 0, SH_MIC_PREPARANDO, SH_MIC_GRABANDO, SH_MIC_FALLO };    /* = W5_MIC_* */
typedef struct { u8 estado, dueno, mentira, musica; int error; u32 tomadas, desbordes, perdidas, pico, ms_preparar; } ShMicInfo;
int  sh_mic_hay(void);
int  sh_mic_abrir(int dueno);                  /* EMPIEZA (no espera): HW_OK (o ya era suyo), HW_ERROR (otro dueño), HW_NO_DISPONIBLE */
void sh_mic_cerrar(int dueno);                 /* solo si es de ese dueño; la música vuelve si se paró */
int  sh_mic_tomar(int dueno, short *dst, u32 max);   /* las acumuladas, sin esperar: cuántas (≥ 0); < 0 si no es suyo o está cerrado */
void sh_mic_info(ShMicInfo *i);
int  sh_mic_dueno(void);
int  sh_mic_activo(void);                      /* 1 = preparando o grabando (no se duerme a fondo: el tic recoge) */
void sh_mic_paso(void);                        /* desde el bucle y app_latido: avanza el arranque */
void sh_mic_soltar_app(void);                  /* al descargarse la app */
int  sh_mic_wav_guardar(const short *pcm, u32 n, char nombre[32]);   /* /tarjeta/music/«Grabación (N).wav»; HW_OK / HW_NO_DISPONIBLE / HW_ERROR */
#endif
