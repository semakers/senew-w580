/* serie_rx.h — (SER) «RECIBIR POR SERIE»: el protocolo con el que el PC (herr/enviar_serie.py) manda UN fichero a la Memoria.
 * C99 portable, sin malloc y sin hardware: el motor recibe bytes (srx_meter), el reloj (ahora_ms) y una interfaz de
 * almacén (SrxAlmacen). Lo usan la pantalla del teléfono (apps/recibir) y la prueba del PC (herr/pruebas/prueba_serie_rx.c).
 * Documento: docs/recibir_por_serie.md.
 *
 * TRAMA (las dos direcciones):  'W' '5' | tipo u8 | largo u16 LE | carga[largo] | CRC32 u32 LE  (IEEE reflejado, de tipo+largo+carga)
 * PC -> teléfono:
 *   'H' cabecera: total u32 | crc32 del fichero u32 | marcas u8 (bit 0 = reemplazar) | ruta (sin 0 final)
 *   'D' datos:    desplazamiento u32 | 1..SRX_DATOS bytes
 *   'F' fin (sin carga)            'X' el PC abandona (sin carga)
 * teléfono -> PC:
 *   'A' bien:     siguiente u32 (el desplazamiento que espera)           (a 'H', 'D' y 'F')
 *   'N' repite:   siguiente u32 (la trama llegó rota o fuera de orden: no es grave)
 *   'E' negativa: código u8 | texto. La sesión se acaba; lo recibido NO queda con el nombre bueno.
 * Una trama cada vez (parar y esperar): el PC no manda la siguiente hasta tener la respuesta, así que nunca hay más de
 * ~530 bytes en vuelo y el anillo de 1 KiB de la UART no se desborda aunque la interfaz tarde en sondear. */
#ifndef SERIE_RX_H
#define SERIE_RX_H
#include <stdint.h>

#define SRX_DATOS      512u                    /* bytes de fichero por trama 'D' */
#define SRX_CARGA_MAX  (4u + SRX_DATOS)        /* la carga más larga que se admite */
#define SRX_TAM_MAX    (8u * 1024u * 1024u)    /* el fichero más grande que se acepta (además de que quepa) */
#ifndef SRX_PLAZO_MS
#define SRX_PLAZO_MS   10000u                  /* sin una trama buena en este tiempo, a media recepción: cable cortado */
#endif
#define SRX_RUTA_MAX   96u
#define SRX_TEMPORAL   "/.recibiendo"          /* el temporal, en la raíz de la Memoria; el nombre bueno solo llega al renombrar */

enum { SRX_ESPERA = 0, SRX_RECIBIENDO, SRX_HECHO, SRX_FALLO };
enum { SRX_E_NADA = 0, SRX_E_NOMBRE = 1, SRX_E_EXISTE = 2, SRX_E_GRANDE = 3, SRX_E_LLENO = 4, SRX_E_TAMANO = 5, SRX_E_CRC = 6,
       SRX_E_ESCRIBIR = 7, SRX_E_PLAZO = 8, SRX_E_CANCELADO = 9, SRX_E_ORDEN = 10, SRX_E_ABANDONO = 11 };

typedef struct {                               /* rutas DENTRO de la Memoria («/apps/x.sen»). 0 = bien; < 0 = mal */
    void *ctx;
    int      (*info)(void *ctx, const char *ruta, int *es_dir);    /* 1 = existe, 0 = no */
    uint32_t (*libre)(void *ctx);                                   /* el fichero nuevo más grande que cabe ahora */
    int      (*mkdir)(void *ctx, const char *ruta);
    int      (*abrir)(void *ctx, const char *ruta);                 /* escritura a trozos (nada existe hasta cerrar) */
    int      (*escribir)(void *ctx, const void *d, uint32_t n);
    int      (*cerrar)(void *ctx);
    void     (*abortar)(void *ctx);
    int      (*renombrar)(void *ctx, const char *de, const char *a, int reemplazar);   /* atómico */
    int      (*borrar)(void *ctx, const char *ruta);
} SrxAlmacen;

typedef struct {
    const SrxAlmacen *al;
    uint8_t  estado, error, reemplazar, abierto, es_app;
    uint32_t total, hecho, crc_decl, crc, ultimo_ms, tramas, rotas, repetidas;
    char     ruta[SRX_RUTA_MAX];
    char     texto[80];                        /* la última negativa, para la pantalla */
    uint8_t  ent[8 + SRX_CARGA_MAX + 8]; uint32_t n_ent;
    uint8_t  sal[256]; uint32_t n_sal;         /* respuestas por mandar (quien llama las saca y pone n_sal = 0) */
} Srx;

uint32_t srx_crc32(uint32_t crc, const void *d, uint32_t n);         /* crc inicial 0 */
void srx_iniciar(Srx *s, const SrxAlmacen *al, uint32_t ahora_ms);
void srx_meter(Srx *s, const uint8_t *d, uint32_t n, uint32_t ahora_ms);
void srx_tic(Srx *s, uint32_t ahora_ms);                             /* el plazo (cable cortado) */
void srx_cancelar(Srx *s, int codigo, const char *texto);           /* el usuario (Atrás) o quien sea: aborta y lo dice al PC */
int  srx_ruta_valida(const char *ruta);                              /* 1 = la ruta es admisible (ver serie_rx.c) */
const char *srx_texto_error(int e);
#endif
