/* Modelo ESTRICTO de una flash NOR para el PC (pruebas del almacén y simulación en hw_pc.c):
 *  - programar solo baja bits (1 -> 0): pedir que suba uno es una VIOLACIÓN (se cuenta y falla);
 *  - programar dos veces la misma palabra es otra violación (el almacén promete no hacerlo);
 *  - como en hal/nor.c, un «programar» se parte en ventanas de 64 bytes: cada ventana y cada borrado
 *    es UNA operación de flash, y se puede cortar la alimentación en la operación n.º «corte_en»:
 *    esa operación queda a medias (bits al azar a medio programar, bloque a medio borrar, basura) y
 *    todas las siguientes fallan sin hacer nada, hasta pc_nor_encender(). */
#ifndef PC_NOR_H
#define PC_NOR_H
#include "almacen.h"
typedef struct {
    u8 *mem, *prog;                    /* contenido; prog = 1 por palabra de 16 bits ya programada desde el último borrado */
    u32 bloques, tam;
    long ops, corte_en;                /* operaciones hechas; n.º de la que se corta (-1 = ninguna) */
    long fallo_en; int fallo_n;        /* fallo TRANSITORIO (pista S6): las operaciones fallo_en … fallo_en+fallo_n-1 fallan
                                          dejando su sitio a medias como en un corte, pero la corriente SIGUE (un sitio malo
                                          de la flash); -1 = ninguna */
    int cortado, modo_corte;           /* modo_corte: -1 = al azar; 0.. = fijo */
    u32 semilla;
    long n_prog, n_borr, violaciones;
    u32 borrados_por_bloque[64];
} PcNor;
extern PcNor pc_nor;
void pc_nor_crear(u32 bloques, u32 tam);
void pc_nor_encender(void);            /* «vuelve la corriente»: quita el corte */
const AlmPuerto *pc_nor_puerto(void);
u32  pc_nor_azar(void);
#endif
