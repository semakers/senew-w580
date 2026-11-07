/* hal/mic_w580.h — el micrófono del W580i (docs/reconocimiento_original.md §4; hal/mic_w580.c). */
#ifndef MIC_W580_H
#define MIC_W580_H
#include "hal.h"
#define MIC_HZ 8000u                         /* una muestra de 16 bits con signo, mono, 8 kHz */
int  mic_iniciar(void);                      /* toma el DSP (lo reinicia), carga AAC_PLUS, abre el reloj PCM, enciende el códec y monta el grafo del mezclador: 0 = bien;
                                              * -1 dsp_arrancar -2 dsp_cargar -3 versión -4 códec -10-k el paso k del grafo (0..11) falló */
int  mic_leer(short *dst, u32 muestras, u32 plazo_ms);   /* BLOQUEA y copia `muestras` seguidas (sin huecos) o las que lleguen en plazo_ms; devuelve cuántas (≥ 0) o < 0: -1 no iniciado, -2 DSP dormido */
u32  mic_desbordes(void);                    /* veces que se tardó más de 8 ms entre sondeos (pudo perderse audio; la vuelta del anillo son 10 ms) */
int  mic_activo(void);
void mic_parar(void);                        /* apaga el códec y el DSP (dsp_apagar) */
/* (1.10) SIN ESPERAS (shell/micro_sis.c): el arranque troceado y las muestras recogidas por el tic (tic_micro = mic_tic) a un anillo de ~2 s */
int  mic_empezar(void);                      /* empieza el arranque (0); luego mic_paso desde el bucle */
int  mic_paso(void);                         /* 0 = sigue, 1 = listo y grabando, < 0 = falló (códigos de mic_iniciar; el DSP queda apagado) */
int  mic_preparando(void);                   /* 1 = a medio arrancar */
void mic_tic(void);                          /* desde el tic de 4 ms */
u32  mic_disponibles(void);                  /* muestras en el anillo del sistema */
u32  mic_sacar(short *dst, u32 max);         /* copia hasta max (sin esperar); devuelve cuántas */
u32  mic_perdidas(void);                     /* muestras tiradas porque nadie las leía (el anillo lleno) */
u32  mic_pico(void);                         /* el valor absoluto más alto de los últimos ~100-200 ms (0..32768) */
#endif
