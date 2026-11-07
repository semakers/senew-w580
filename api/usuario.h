/* usuario.h — «W6U»: la zona de FICHEROS DE USUARIO del sistema del W600 (pista S3).
 *
 * Un puñado de ficheros (notas largas, capturas de pantalla, contactos, agenda…) que se pueden crear,
 * reemplazar y borrar, en unos pocos bloques de flash NOR. Es un registro circular (log) con limpieza;
 * NO es el almacén clave→valor (api/almacen.h), que sigue sirviendo para los ajustes pequeños.
 *
 * Portable: habla con la flash por el mismo «puerto» que el almacén (AlmPuerto: leer / programar / borrar);
 * en el teléfono lo pone hal/nor.c sobre la zona 0x45C00000..0x45DFFFFF (8 bloques lógicos de 128 KiB; hal/hal.h) y en el PC
 * plat/pc_nor.c (modelo ESTRICTO con cortes de alimentación).
 *
 * GARANTÍAS (las demuestra herr/pruebas/prueba_usuario.c cortando la alimentación en CADA operación de flash):
 *  - usr_escribir y usr_borrar son ATÓMICAS por fichero: tras un corte en cualquier punto, ese fichero vale
 *    ENTERO lo de antes o ENTERO lo nuevo (o no existe, si antes no existía / si lo nuevo era borrarlo);
 *    los demás ficheros no cambian nunca.
 *  - Ninguna palabra de flash se programa dos veces ni se pide subir un bit (el modelo lo cuenta como violación).
 *  - El montaje tolera CUALQUIER contenido (basura, bloques a medio borrar, registros a medias).
 *  - Desgaste repartido: la cabeza del registro da vueltas por todos los bloques.
 *  - Lleno = error limpio (USR_E_LLENO) ANTES de tocar nada; nunca deja el volumen a medias.
 *
 * Límites: nombres de 1..USR_MAX_NOMBRE bytes (UTF-8, sin '/' ni NUL), USR_MAX_FICHEROS ficheros, tamaño de
 * un fichero limitado solo por el sitio. Sin directorios.
 */
#ifndef USUARIO_H
#define USUARIO_H
#include "almacen.h"                   /* AlmPuerto, alm_crc32, u8/u16/u32 */

#define USR_MAX_NOMBRE   47
#define USR_MAX_FICHEROS 64

enum { USR_OK = 0, USR_E_NO_HAY = -1, USR_E_ARG = -2, USR_E_LLENO = -3, USR_E_FLASH = -4, USR_E_SIN_MONTAR = -5,
       USR_E_MUCHOS = -6 /* ya hay USR_MAX_FICHEROS */ };

typedef struct {
    char nombre[USR_MAX_NOMBRE + 1];
    u32  tam;
    u32  fecha;                        /* la que dio quien lo escribió (el sistema usa segundos desde 2000-01-01 UTC); 0 = sin fecha */
} UsrEntrada;

typedef struct {
    u8  montado, solo_lectura;
    u32 bloques, tam_bloque;
    u32 ficheros;                      /* cuántos hay */
    u32 ocupado;                       /* bytes de datos vivos (suma de tamaños + cabeceras) */
    u32 libre;                         /* bytes que se pueden escribir todavía en UN fichero nuevo (aprox., por lo bajo) */
    u32 formateos, limpiezas, errores;
} UsrEstado;

int  usr_montar(const AlmPuerto *p, int puede_escribir);   /* USR_OK; si no hay volumen y puede escribir, da formato */
int  usr_firma(const AlmPuerto *p);                        /* (AUD2 B4) 1 si algún bloque lleva NUESTRA marca ('W6U1'), monte o no */
int  usr_contar(void);                                     /* n.º de ficheros (0 si no está montado) */
int  usr_listar(u32 desde, UsrEntrada *e, u32 max);        /* orden estable (alfabético por bytes); devuelve cuántas */
int  usr_info(const char *nombre, UsrEntrada *e);          /* USR_OK o USR_E_NO_HAY */
int  usr_leer(const char *nombre, u32 desde, void *buf, u32 max);   /* bytes leídos (0 al final) o USR_E_* */

/* Escritura de un fichero ENTERO (reemplaza al que hubiera con ese nombre). Dos maneras:
 *  - de una vez: usr_escribir;
 *  - a trozos (para lo que no cabe en RAM): usr_empezar / usr_mas (las veces que haga falta, sumando justo «tam») / usr_acabar.
 *    Hasta usr_acabar el fichero anterior sigue intacto y es el que se lee; usr_cancelar (o un corte) lo deja como estaba.
 *    Solo una escritura a trozos a la vez. */
int  usr_escribir(const char *nombre, const void *d, u32 n, u32 fecha);
int  usr_empezar(const char *nombre, u32 tam, u32 fecha);  /* USR_E_LLENO si no va a caber (comprobado aquí, antes de escribir nada) */
int  usr_mas(const void *d, u32 n);
int  usr_acabar(void);                                     /* el compromiso: desde aquí vale lo nuevo */
void usr_cancelar(void);
int  usr_borrar(const char *nombre);                       /* USR_OK o USR_E_NO_HAY */
void usr_estado(UsrEstado *e);
/* (R3; AUD3-M1) «Sigo viva»: usuario.c llama a f tras cada registro copiado o grabado y antes de cada borrado de bloque. En una
 * limpieza, hecho/total son los bytes copiados y los que hay que copiar; en lo demás, 0/0. Sirve para dar de comer al perro y
 * pintar un progreso; f NO puede llamar a usr_*. NULL = nadie. */
typedef void (*UsrPaso)(u32 hecho, u32 total);
void usr_al_paso(UsrPaso f);
#endif
