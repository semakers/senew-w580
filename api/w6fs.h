/* w6fs.h — «W6FS»: NUESTRO sistema de ficheros de solo lectura para la NOR (versión 1).
 *
 * Por qué uno propio y tan simple (docs/sistema.md §9): el contenido del sistema (fondos, imágenes, textos)
 * no cambia en marcha —lo que cambia va al almacén clave→valor (almacen.h)—, la NOR está mapeada en
 * memoria y un fichero contiguo se lee sin capa de bloques ni caché (ni copia: w6fs_datos da el puntero).
 * Es una tabla de entradas ordenada de modo que los hijos de un directorio van seguidos, con CRC de la
 * tabla y de cada fichero. La imagen la construye en el PC herr/hacer_fs.py a partir de una carpeta.
 *
 * Imagen (little-endian; todo alineado a 4):
 *   +0  "W6FS"   +4 u32 versión (1)   +8 u32 nº de entradas   +12 u32 tamaño total de la imagen
 *   +16 u32 desplazamiento de la tabla (32)   +20 u32 desplazamiento de los nombres   +24 u32 CRC32 de
 *   tabla+nombres   +28 u32 CRC32 de los 28 bytes anteriores
 *   entrada (20 bytes): u32 nombre (desplazamiento en la zona de nombres, cadena UTF-8 acabada en 0)
 *                       u32 a   u32 b   u32 crc   u32 tipo (0 = fichero, 1 = directorio)
 *       fichero:    a = desplazamiento de los datos en la imagen, b = tamaño, crc = CRC32 de los datos
 *       directorio: a = índice de su primer hijo, b = nº de hijos (seguidos, ordenados por nombre)
 *   la entrada 0 es la raíz.
 * El montaje comprueba cabecera, CRC de la tabla y TODOS los rangos una vez: después nada puede salirse. */
#ifndef W6FS_H
#define W6FS_H
#include "hw.h"
typedef struct { const u8 *base; u32 n, tam, tabla, nombres, fin_nombres; } W6fs;
int  w6fs_montar(W6fs *fs, const u8 *base, u32 max);            /* 1 = hay un W6FS válido en «base» (max = bytes legibles) */
int  w6fs_buscar(const W6fs *fs, const char *ruta);             /* índice de la entrada o -1 («/» = 0) */
int  w6fs_es_dir(const W6fs *fs, int i);
u32  w6fs_tam(const W6fs *fs, int i);                           /* tamaño del fichero / nº de hijos del directorio */
const u8 *w6fs_datos(const W6fs *fs, int i);                    /* puntero a los datos del fichero (en la propia flash) */
int  w6fs_listar(const W6fs *fs, int dir, u32 desde, HwEntrada *e, u32 max);   /* como hw_fs_listar; e = NULL cuenta */
int  w6fs_comprobar(const W6fs *fs, int i);                     /* 1 = el CRC del fichero cuadra */
#endif
