/* cargable_red.h — (1.6) EL GRUPO DE RED de la tabla `w5`, visto desde el SISTEMA (docs/apps_cargables.md §5.9).
 *
 * El lado de las apps es cargables/sdk/w5_red.h (el contrato documentado está allí). Este fichero repite, para el sistema, lo MISMO
 * y con los MISMOS números: las funciones en el mismo orden y con la misma firma, las constantes y W5_RED_MENOR. Que los dos digan lo
 * mismo lo comprueba «make probar-red-abi» (herr/comprobar_red_abi.py).
 *
 * Dónde va el grupo: DETRÁS del de sonido (1.5), que va detrás del Bluetooth (1.4) y de la 1.3. Como con la 1.4 y la 1.5, W5_API
 * (cargable.h) NO cambia. El sistema dice menor 6 (CG_API_MENOR) y n = 1.3 + 15 + 2 + 18.
 * Lo que hay debajo: shell/red_sis.c (la sesión con el puente sobre el SPP del servicio Bluetooth) y bt/red/red.c (W5RED v1,
 * bt/puente_red/PROTOCOLO.md). Las envolturas, en shell/cg_red.c. */
#ifndef CARGABLE_RED_H
#define CARGABLE_RED_H
#include "cargable_15.h"

#define W5_RED_MENOR 6                         /* la menor de la API que trae este grupo */
enum { W5_CAP_RED = 1 << 24 };
#define W5_RED_CANALES 4
enum { W5_RED_APAGADA = 0, W5_RED_CONECTANDO, W5_RED_LISTA, W5_RED_FALLO };
enum { W5_REDF_OK = 0, W5_REDF_SIN_RADIO, W5_REDF_SIN_PUENTE /* no hay puente elegido (Ajustes -> Bluetooth) ni dirección */,
       W5_REDF_RADIO /* la radio no arrancó */, W5_REDF_ENLACE /* el puente no contesta, no tiene SPP o rechazó el PIN */,
       W5_REDF_SALUDO /* contestó, pero no es un puente W5RED */, W5_REDF_CAIDA /* se cortó estando conectada */,
       W5_REDF_OCUPADA /* el Bluetooth lo usa otra cosa (un robot conectado) */ };
enum { W5_RC_LIBRE = 0, W5_RC_ABRIENDO, W5_RC_ABIERTO, W5_RC_CERRADO, W5_RC_CERRANDO };
enum { W5_RCOD_BIEN = 0, W5_RCOD_DNS, W5_RCOD_RECHAZADA, W5_RCOD_PLAZO, W5_RCOD_TLS, W5_RCOD_HTTP, W5_RCOD_RED, W5_RCOD_CORTADA,
       W5_RCOD_PROHIBIDO, W5_RCOD_DEMASIADOS, W5_RCOD_PROTOCOLO, W5_RCOD_ABORTADA, W5_RCOD_INTERNO, W5_RCOD_SIN_SESION };
enum { W5_RED_TLS = 1 /* solo TCP: con TLS (el puente comprueba el certificado) */, W5_RED_SIN_REDIR = 2 /* HTTP: no seguir 3xx */,
       W5_RED_SIN_VERIFICAR = 4 /* no comprobar el certificado TLS */ };
#define W5_RED_PLAZO(s) (((unsigned)(s) & 255u) << 8)
enum { W5_RED_E_SIN_SESION = -1, W5_RED_E_OCUPADO = -2 /* reintentar en la vuelta siguiente */, W5_RED_E_ARG = -3,
       W5_RED_E_SIN_CANAL = -4 /* los W5_RED_CANALES en uso */, W5_RED_E_NO_DISPONIBLE = -5 };
#define W5_RED_CUERPO_A_TROZOS 0xFFFFFFFFu     /* red_http(…, cuerpo = 0, n = esto): el cuerpo irá con red_enviar() + red_terminar() */
#define W5_RED_CUERPO_MAX 4096                 /* lo más que se pasa ENTERO en red_http; más grande, a trozos */

/* LAS MISMAS LÍNEAS que W5_API16 de cargables/sdk/w5_red.h */
#define W5_API16(F) \
    F(int, red_conectar, (const unsigned char *addr))    /* arranca: radio + SPP con el puente + saludo. addr = 0: el elegido en Ajustes. W5_OK / W5_ERROR (mirar red_fallo) / W5_NO_DISPONIBLE */ \
    F(void, red_desconectar, (void))                     /* cierra todo (la radio sigue encendida) */ \
    F(int, red_estado, (void))                           /* W5_RED_* */ \
    F(int, red_fallo, (void))                            /* W5_REDF_* */ \
    F(int, red_http, (const char *metodo, const char *url, const char *cabeceras, const void *cuerpo, unsigned n, unsigned banderas)) /* canal o W5_RED_E_*; cabeceras «Nombre: valor\r\n»…; todo se COPIA */ \
    F(int, red_tcp, (const char *host, unsigned puerto, unsigned banderas))   /* canal o W5_RED_E_* */ \
    F(int, red_canal, (int c))                           /* W5_RC_* */ \
    F(int, red_codigo, (int c))                          /* W5_RCOD_* (con W5_RC_CERRADO) */ \
    F(int, red_texto, (int c, char *d, unsigned max))    /* COPIA el porqué del cierre («Name or service not known»…); bytes */ \
    F(int, red_http_estado, (int c))                     /* 200, 404…; 0 = aún no */ \
    F(int, red_cabeceras, (int c, char *d, unsigned max))   /* COPIA las cabeceras de la respuesta (TCP: «ip:puerto»); bytes */ \
    F(int, red_fin_recibido, (int c))                    /* 1 = el otro lado ya no mandará más: lo que quede en la cola es lo último */ \
    F(int, red_disponibles, (int c))                     /* bytes esperando en la cola de recepción */ \
    F(int, red_recibir, (int c, void *d, unsigned max))  /* COPIA y saca; no bloquea (0 = nada ahora) */ \
    F(int, red_enviar, (int c, const void *d, unsigned n))  /* bytes aceptados (hasta 4 KB por llamada; la cola del canal es de 16 KB); el resto, en otra vuelta */ \
    F(int, red_pendiente, (int c))                       /* bytes aún sin salir */ \
    F(int, red_terminar, (int c))                        /* FIN: no mandaré más (fin del cuerpo HTTP a trozos / medio cierre TCP) */ \
    F(void, red_cerrar, (int c))                         /* soltar el canal (si seguía abierto, se aborta). SIEMPRE al acabar */
#endif
