/* bl_pantalla.h — pantalla MÍNIMA del modo installer del BL (plat/bl/bl.h).
 *
 * SOLO se compila con -DBL_INSTALLER=1 (BL_SRC del Makefile incluye bl_pantalla.c tras `ifneq (,$(findstring BL_INSTALLER…`).
 * Si el panel no contesta en bl_pantalla_ini(), bl_pantalla_hay() devuelve 0 y los pintados posteriores no hacen nada
 * (el instalador USB sigue funcionando: el anfitrión ve el progreso por el log). */
#ifndef BL_PANTALLA_H
#define BL_PANTALLA_H
#include "bl.h"

#ifdef BL_INSTALLER

int  bl_pantalla_ini(void);                               /* 0 = panel OK y cuadro inicial enviado; <0 = no hay panel */
int  bl_pantalla_hay(void);                               /* 1 cuando bl_pantalla_ini devolvió 0 */
void bl_mostrar(const char *linea1, const char *linea2); /* dos líneas en el tercio superior; borra la barra */
void bl_barra(u32 hecho, u32 total);                      /* dibuja «INSTALANDO...» + porcentaje y la barra */
void bl_listo(void);                                      /* «LISTO. REINICIA.» en medio, sin barra */

#else                                                     /* en compilación clásica, los puntos de llamada se quedan vacíos sin #ifdef por todas partes */
#define bl_pantalla_ini()        0
#define bl_pantalla_hay()        0
#define bl_mostrar(a, b)         ((void)0)
#define bl_barra(h, t)           ((void)(h), (void)(t))
#define bl_listo()               ((void)0)
#endif

#endif
