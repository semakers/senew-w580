/* almacen.h — el almacén persistente del sistema: pares clave → valor en unos pocos bloques de flash NOR.
 *
 * Portable: no sabe de registros; habla con la flash por un «puerto» de cuatro funciones (el teléfono:
 * hal/nor.c; el PC: plat/pc_nor.c, un modelo ESTRICTO de NOR con inyección de cortes de alimentación).
 *
 * Garantías (demostradas por herr/prueba_almacen.c cortando la alimentación en CADA operación de flash):
 *  - alm_guardar es atómica: tras un corte en cualquier punto, cada clave vale lo de antes o lo nuevo;
 *    las demás claves no cambian nunca.
 *  - Ninguna palabra de flash se programa dos veces ni se pide subir un bit: lo nuevo se escribe en
 *    sitio borrado y se valida con su CRC, que es lo último que se escribe.
 *  - El montaje tolera cualquier contenido (basura, bloques a medio borrar, registros a medias).
 *  - Desgaste repartido: los registros se añaden uno tras otro; al llenarse el bloque activo se compacta
 *    en el SIGUIENTE bloque del anillo y solo entonces se borra el viejo: cada bloque se borra una vez
 *    por vuelta completa.
 *
 * Formato (todo little-endian, alineado a 4):
 *   bloque:   +0 u32 'W6NV'   +4 u32 generación   +8 u32 CRC32(los 8 bytes)   +12 u32 confirmación 'ACTV'
 *             (la confirmación se programa al final de la compactación: sin ella el bloque no cuenta)
 *   registro: +0 u16 marca (0x5652 valor, 0x5842 borrado)  +2 u16 clave  +4 u16 longitud
 *             +6 u16 CRC16 de los 6 bytes  +8 datos, relleno 0xFF hasta múltiplo de 4,
 *             y u32 CRC32(cabecera + datos sin relleno)
 *   vale el último registro válido de cada clave en el bloque activo (el de mayor generación confirmado).
 */
#ifndef ALMACEN_H
#define ALMACEN_H
#ifndef HW_H
#ifndef HAL_H
#ifndef UI_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
#endif
#endif
#endif

#define ALM_MAX_VALOR  2048u          /* bytes por valor */
#define ALM_MAX_CLAVES 96

typedef struct {
    u32 bloques, tam_bloque;                                   /* ≥ 2 bloques; tam múltiplo de 4 */
    const u8 *(*leer)(u32 bloque);                             /* el bloque, mapeado en memoria */
    int (*programar)(u32 bloque, u32 desplaz, const void *d, u32 n);   /* 0 = bien. Solo sobre lo borrado */
    int (*borrar)(u32 bloque);                                 /* 0 = bien */
} AlmPuerto;

typedef struct {
    u8  montado, solo_lectura;
    u32 bloques, tam_bloque, activo, generacion, usado, vivos, claves, formateos, compactaciones, errores;
} AlmEstado;

enum { ALM_OK = 0, ALM_E_NO_HAY = -1, ALM_E_ARG = -2, ALM_E_LLENO = -3, ALM_E_FLASH = -4, ALM_E_SIN_MONTAR = -5 };

int  alm_montar(const AlmPuerto *p, int puede_escribir);
int  alm_firma(const AlmPuerto *p);                            /* (AUD2 B4) 1 si algún bloque lleva NUESTRA marca ('W6NV'), monte o no */       /* busca el bloque activo (o da formato si no hay ninguno y puede escribir) */
int  alm_cargar(u16 clave, void *buf, u32 max);                /* bytes del valor (copia hasta max), o ALM_E_NO_HAY */
int  alm_tam(u16 clave);                                       /* bytes del valor o ALM_E_NO_HAY */
int  alm_guardar(u16 clave, const void *d, u32 n);             /* ALM_OK (si el valor ya era ese, no escribe nada) */
int  alm_borrar(u16 clave);
void alm_estado(AlmEstado *e);
u32  alm_crc32(u32 crc, const void *d, u32 n);                 /* CRC-32 (IEEE, reflejado); crc inicial 0 */
#endif
