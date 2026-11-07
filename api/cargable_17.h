/* cargable_17.h — (1.7) LA PANTALLA: la letra nueva y el tamaño de la pantalla, vistos desde el SISTEMA (docs/estetica.md §6).
 *
 * El lado de las apps es cargables/sdk/w5_pantalla.h. Este fichero repite, para el sistema, lo MISMO y con los MISMOS números
 * (las funciones en el mismo orden y con la misma firma, las constantes y W5_PANT_MENOR).
 *
 * QUÉ CAMBIA CON LA 1.7: la escala de la letra (estética, 26-oct). Una app que declara `.api_menor >= 7` dice «sé medir el texto»:
 * pinta con la letra NUEVA (cuerpo Roboto 20 px, línea de 24) y no debe dar por hecha ninguna altura: la pregunta con
 * ui_paso_linea / ui_alto_texto (nuevas) y li_ancho_texto (la de siempre, de la 1.0). Una app de menor < 7 (todas las de antes) corre en el MODO V1: el sistema le pone
 * la letra y las medidas de antes (líneas de 10*k px, filas de 18), con las que colocó sus cosas (ui/diseno.h: dis_v1).
 *
 * Dónde va el grupo: DETRÁS del de red (1.6). Como con la 1.4–1.6, W5_API (cargable.h) NO cambia. El sistema dice menor 7
 * (CG_API_MENOR). Las envolturas, en shell/cg17.c. */
#ifndef CARGABLE_17_H
#define CARGABLE_17_H
#include "cargable_red.h"

#define W5_PANT_MENOR 7                        /* la menor de la API que trae este grupo: la letra nueva */
enum { W5_GIRO = 5 };                         /* (1.7) suceso: la pantalla giró (a = el giro nuevo, 0..3). Solo a las apps con W5_CAP_GIRA; sus
                                                * pantallas ya se vuelven a construir solas con el tamaño nuevo: esto es para quien pinta a mano */
enum { W5_CAP_GIRA = 1 << 25 };                /* (1.7) la app SABE GIRAR: se recoloca de lado y boca abajo. Sin esto, mientras está
                                                * cargada la pantalla va de pie (todas las de antes van así) */

/* LAS MISMAS LÍNEAS que W5_API17 de cargables/sdk/w5_pantalla.h */
#define W5_API17(F) \
    F(int, ui_paso_linea, (int marcas))                  /* el paso entre líneas de un texto de varias líneas con esas marcas (24 con el cuerpo) */ \
    F(int, ui_alto_texto, (int escala, int marcas))      /* el alto de UNA línea de texto con esa escala y marcas (lo que ocupa un Texto) */ \
    F(int, pantalla_tam, (void))                         /* (ancho << 16) | alto de la pantalla LÓGICA que ve la app ahora */ \
    F(int, pantalla_giro, (void))                        /* 0 de pie, 1 de lado con el teclado a la derecha, 2 boca abajo, 3 con el teclado a la izquierda */

#endif
