/* usrop.h — OPERACIONES COMPUESTAS sobre la zona de ficheros de usuario (W6U). Pista S6 del W580.
 *
 * «Copiar un fichero a la zona por trozos» y «renombrar», hechas SOLO con la API de api/usuario.h
 * (usr_empezar / usr_mas / usr_acabar / usr_borrar / usr_leer): NO tocan el formato ni la flash, y por eso
 * heredan sus garantías. Están aquí, en un fichero portable sin UI ni hardware, para que la prueba de cortes
 * del PC (herr/pruebas/prueba_usuario.c) ejercite EXACTAMENTE el mismo código que corre en el teléfono.
 *
 * Van POR PASOS (usro_paso): cada paso copia unos KB y vuelve, para que la pantalla pinte una barra de progreso
 * y se pueda cancelar. Solo una operación a la vez (es la única escritura a trozos que admite usuario.c).
 *
 * LO QUE QUEDA TRAS UN CORTE DE CORRIENTE, en cualquier punto (demostrado en la prueba de cortes):
 *  - COPIAR: el destino vale ENTERO lo de antes (o no existe, si no existía) o ENTERO lo nuevo. Nunca a medias.
 *  - RENOMBRAR (W6U no tiene «renombrar»: es copiar con el nombre nuevo y, YA COMPROMETIDA la copia, borrar el
 *    viejo): o solo el viejo, o los DOS (enteros e iguales), o solo el nuevo. NUNCA ninguno, nunca uno a medias.
 *    Si quedan los dos, el usuario ve el fichero dos veces y borra uno: no se ha perdido nada.
 *  - Los demás ficheros no cambian nunca.
 * Renombrar necesita sitio para una copia entera mientras dura (es lo que cuesta no perder el fichero).
 */
#ifndef USROP_H
#define USROP_H
#include "usuario.h"

/* La fuente de una copia: tiene que dejar en buf EXACTAMENTE los n bytes [desde, desde+n) y devolver n;
 * cualquier otra cosa es «no puedo» y la copia se abandona (el destino queda como estaba). */
typedef int (*UsroFuente)(void *dato, u32 desde, u8 *buf, u32 n);

enum { USRO_SIGUE = 1,                 /* usro_paso: queda trabajo */
       USRO_E_EXISTE = -20,            /* ya hay un fichero con el nombre de destino (y no se pidió reemplazar) */
       USRO_E_FUENTE = -21,            /* la fuente no dio los bytes pedidos */
       USRO_E_OCUPADA = -22,           /* ya hay una operación en marcha */
       USRO_E_MISMO = -23 };           /* renombrar a su mismo nombre */

typedef struct {
    u8  activa, renombra;
    char destino[USR_MAX_NOMBRE + 1], origen[USR_MAX_NOMBRE + 1];
    u32 tam, hecho;
    UsroFuente fuente; void *dato;
} UsroOp;

/* EMPEZAR. Todo lo que puede ir mal por culpa de quien llama se dice AQUÍ, antes de tocar la flash:
 * USR_E_ARG (nombre vacío, con «/» o de más de USR_MAX_NOMBRE bytes), USRO_E_EXISTE, USR_E_LLENO (no cabe),
 * USR_E_MUCHOS (ya hay USR_MAX_FICHEROS), USR_E_NO_HAY (renombrar algo que no existe), USR_E_FLASH (solo lectura). */
int  usro_copiar(UsroOp *o, const char *destino, u32 tam, u32 fecha, int reemplazar, UsroFuente f, void *dato);
int  usro_renombrar(UsroOp *o, const char *viejo, const char *nuevo);
/* UN PASO: copia hasta `max` bytes (mínimo, un trozo). USRO_SIGUE, USR_OK (acabada y comprometida) o el error;
 * tras un error o un USR_OK la operación ya no está activa. */
int  usro_paso(UsroOp *o, u32 max);
void usro_cancelar(UsroOp *o);         /* deja el destino como estaba (lo escrito hasta ahora no tiene compromiso: no existe) */
#endif
