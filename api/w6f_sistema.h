/* w6f_sistema.h — (I1) LA CONFIGURACIÓN de W6F en este sistema, en UN sitio: la usan api/hw_w6f.c (el volumen de la Memoria),
 * plat/hw_pc.c (la imagen de fábrica del PC) y herr/w6f_imagen.c (la imagen ya formateada de la flash del emulador). Cambiar
 * max_obj o el tamaño de la zona hace que un volumen hecho antes dé W6F_E_GEOMETRIA (subir max_obj y AMPLIAR la zona sí se
 * admite sin migrar: docs/fs_propio.md §10). fs2/INTEGRACION.md §3. */
#ifndef W6F_SISTEMA_H
#define W6F_SISTEMA_H
#include "w6f.h"
#define W6F_SIS_PAG       1024u        /* una región del M18 en modo objeto */
#define W6F_SIS_PPB       256u         /* regiones por bloque de 256 KiB */
#define W6F_SIS_MAX_OBJ   1100u        /* ficheros + carpetas, contando la raíz */
#define W6F_SIS_RESERVA   4u           /* bloques de reserva (mínimo 3) */
#define W6F_SIS_TOPE      32u          /* programas por paso de recolección */
#define W6F_SIS_MEM_U32   17000u       /* RAM de trabajo: 68 000 bytes (w6f_mem_necesaria pide 63 024 con 88 bloques y 1100 objetos; 50 544 con 64) */
static inline W6fConf w6f_sis_conf(int solo_lectura)
{
    W6fConf c = { 0 };
    c.max_obj = W6F_SIS_MAX_OBJ; c.reserva_bloques = W6F_SIS_RESERVA; c.tope_paso = W6F_SIS_TOPE;
    c.solo_lectura = (uint32_t)(solo_lectura != 0); c.relecturas = 0; c.reusar_blanco = 0;
    return c;
}
/* lo que pone la plataforma (plat/hw_w580.c, plat/hw_pc.c) para api/hw_w6f.c */
const W6fFlash *hwo_w6f_flash(int *puede_escribir);   /* el puerto, o NULL si no hay zona (o modo seguro) */
void hwo_w6f_tras_escribir(void);
int  hwo_w6f_fallo_de_prueba(void);                   /* (AUD7 M1) el PC con PC_W6F_FALLO: 1 = hacer como si el montaje fallara; el teléfono, 0 */                     /* el PC vuelca su fichero; el teléfono, nada */
#endif
