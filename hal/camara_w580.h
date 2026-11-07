/* hal/camara_w580.h — la cámara del W580i (docs/reconocimiento_original.md §5; hal/camara_w580.c). */
#ifndef CAMARA_W580_H
#define CAMARA_W580_H
#include "hal.h"
int  cam_iniciar(void);                    /* 0 = bien; -1 el sensor no contesta; -2 no es el OV2640 REV2B; -3 la tabla falló; -4 CDI/CDICON no responden */
int  cam_capturar(u32 tope_ms);            /* espera un cuadro NUEVO y completo (flanco de bajada de CDI+04 bit 14) y PARA el flujo: 0 = bien; -1 apagada; -2 no llegó */
void cam_seguir(void);                     /* vuelve a poner el flujo en marcha tras cam_capturar */
void cam_apagar(void);
const u8 *cam_yuyv(int cual);              /* YUV 4:2:2 Y1 U Y2 V, 176×144, 352 B por fila. Tras cam_capturar el cuadro NUEVO y entero está SIEMPRE en el búfer 0 (el DMA reinicia ahí al reanudar; el 1 conserva uno viejo) [observado 27-oct] */
int  cam_ancho(void);
int  cam_alto(void);
void cam_a_rgb565(const u8 *yuyv, u16 *out);   /* out: cam_ancho() × cam_alto() u16 */
/* (1.9) sin esperas, para shell/camara_sis.c: el tic (tic_camara = cam_tic) para el flujo en el flanco «cuadro completo» y lo reanuda cuando la pantalla ya salió (PDICON libre) */
void cam_tic(void);                        /* desde el tic de 4 ms */
void cam_vigilar(int si);                  /* 1 = el tic vigila; 0 = no (cam_apagar ya lo hace) */
int  cam_listo(void);                      /* 1 = hay un cuadro nuevo y entero parado en el búfer 0 */
u32  cam_cuadros(void);                    /* cuadros enteros vistos por el tic */
int  cam_asentada(void);                   /* 1 = ya pasaron los ~1,2 s de la exposición automática */
const u8 *cam_tomar(void);                 /* el cuadro parado (caché invalidada) o NULL; el flujo sigue parado hasta cam_soltar */
void cam_soltar(void);                     /* el tic reanuda en cuanto la pantalla que lo enseña haya salido (o a los 60 ms) */
#endif
