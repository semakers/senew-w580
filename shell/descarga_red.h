/* descarga_red.h — (1.11, 28-oct) DESCARGAR una canción de la red a la TARJETA para oírla sin red (pedido del mantenedor: «cuando me lleve el teléfono y no
 * tenga acceso a internet por Bluetooth, pueda acceder a las canciones que decidí que se queden descargadas»). La baja el SISTEMA (sigue con la app cerrada)
 * a /tarjeta/music/«Artista - Título.mp3», la carpeta de música del original, que la Música del sistema ya mira. Una a la vez. */
#ifndef DESCARGA_RED_H
#define DESCARGA_RED_H
#include "hw.h"
enum { DESC_QUIETA = 0, DESC_CONECTANDO, DESC_PIDIENDO, DESC_RESPUESTA, DESC_BAJANDO, DESC_HECHA, DESC_FALLO };
#define DESC_NOMBRE 64                          /* el nombre del fichero con su 0 (lo que cabe en HwEntrada) */
typedef struct { u8 fase, ya_estaba; int http; u32 bajados, total, esperado; char nombre[DESC_NOMBRE]; char porque[48]; } DescInfo;
int  sh_desc_hay(void);                         /* 1 si hay radio (la red) */
int  sh_desc_empezar(const char *url, const char *titulo, const char *artista, u32 dur_ms);   /* HW_OK (o ya estaba), HW_ERROR (otra en curso), HW_NO_DISPONIBLE (sin tarjeta) */
void sh_desc_cancelar(void);
void sh_desc_info(DescInfo *i);
void sh_desc_paso(void);                        /* desde sh_bt_paso (el bucle y el latido de las apps) */
#endif
