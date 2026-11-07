/* camara_sis.h — (1.9, 27-oct) LA CÁMARA para las apps, vista desde el sistema (api/cargable_19.h; docs/apps_cargables.md §5.11).
 * En el W580: hal/camara_w580.c (el sensor, CDI/CDICON y el cuadro parado por el tic). En el banco de PC: una cámara de MENTIRA (barras de
 * colores que se mueven, ~7 cuadros por segundo) para hacer las apps sin el teléfono. Una sola dueña: la app cargada (o el sistema). */
#ifndef CAMARA_SIS_H
#define CAMARA_SIS_H
#include "hw.h"
enum { SH_CAM_CERRADA = 0, SH_CAM_ASENTANDO, SH_CAM_EN_VIVO, SH_CAM_FALLO };    /* = W5_CAM_* */
typedef struct { u8 estado, mentira; int error; u32 cuadros, entregados, ms_abrir, ms_ultimo; } ShCamInfo;
int  sh_cam_hay(void);                         /* 1 si hay cámara (el driver del W580, o la de mentira del PC) */
int  sh_cam_abrir(void);                       /* HW_OK (también si ya estaba), HW_NO_DISPONIBLE, HW_ERROR (ShCamInfo.error); tarda ~0,75 s en el W580 */
void sh_cam_cerrar(void);
int  sh_cam_cuadro(void *dst, int formato);    /* 1 = cuadro nuevo copiado (W5_CAM_RGB565 | W5_CAM_GIRO(g), o W5_CAM_YUYV); 0 = aún no; < 0 cerrada */
void sh_cam_info(ShCamInfo *i);
int  sh_cam_activa(void);                      /* 1 = abierta (no se duerme a fondo) */
void sh_cam_soltar_app(void);                  /* al descargarse la app: apagada */
int  sh_cam_foto_guardar(const u8 *d, u32 n, char nombre[16]);   /* el JPEG a /tarjeta/DCIM/100MSDCF/DSCnnnnn.JPG (el número siguiente); HW_OK / HW_NO_DISPONIBLE / HW_ERROR */
int  sh_cam_foto_borrar(const char *nombre);                      /* solo «DSCnnnnn.JPG» de esa carpeta */
#endif
