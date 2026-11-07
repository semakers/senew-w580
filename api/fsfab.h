/* Lector (SOLO LECTURA) del sistema de ficheros de FÁBRICA del W580i: el «SEMC FS» sobre la NOR
 * (docs/fs.md). Portable: recibe un puntero a la región cruda (en el teléfono, la propia NOR en
 * 0x45E00000; en el PC, un fichero cargado en memoria) y no toca nada más. No escribe NUNCA. */
#ifndef FSFAB_H
#define FSFAB_H
#include "hw.h"

#define FF_BLK    1024u                 /* bloque físico = bloque lógico = cluster de los volúmenes */
#define FF_GRUPO  250u                  /* bloques físicos que describe un bloque de cabecera */
#define FF_MAX_FIS 32768u               /* el segmento declarado: 0x45E00000..0x47DFFFFF = 32 MiB */

typedef struct {
    u8  montado;
    u32 fisicos;                        /* bloques físicos mirados */
    u32 cabeceras, cabeceras_malas;     /* bloques de cabecera con la suma buena / mala */
    u32 validos;                        /* lógicos con bloque */
    u32 duplicados;                     /* lógicos que salían en dos cabeceras (gana la de secuencia mayor) */
    u32 desconocidos;                   /* entradas de cabecera con un estado que este lector no conoce */
    u32 fuera;                          /* entradas válidas que apuntaban fuera de lo mirado */
    u32 montajes, volumenes;            /* entradas de la tabla de montaje / de ellas, volúmenes FAT que montan */
    u32 lecturas;                       /* bloques lógicos pedidos */
    u8  marca_fin;                      /* 1 = está la marca de «fin de lo precargado» (flash recién grabada) */
} FfEstado;

int  ff_montar(const u8 *region, u32 bytes);     /* devuelve los lógicos válidos; 0 = ahí no hay un SEMC FS */
int  ff_listo(void);
const FfEstado *ff_estado(void);
int  ff_montaje(unsigned i, char nombre[28], u8 *tipo, u32 *inicio, u32 *bloques);   /* entrada i de la tabla de montaje; 0 = no hay más */
/* Lo que pide la API (hw.h): rutas absolutas del teléfono («/tpa/user/music/x.mid», «/ifs/settings/…»).
 * Los nombres se comparan sin distinguir mayúsculas, como hace FAT. */
int  ff_listar(const char *dir, u32 desde, HwEntrada *e, u32 max);   /* e == NULL: cuenta. HW_ERROR si no existe o no es directorio */
u32  ff_tam(const char *ruta);                   /* 0 si no existe o es un directorio */
u32  ff_leer(const char *ruta, u8 *buf, u32 max);
u32  ff_leer_desde(const char *ruta, u32 desde, u8 *buf, u32 max);   /* los bytes [desde, desde+max): devuelve los leídos (0 al final) */
#endif
