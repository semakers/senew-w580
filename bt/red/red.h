/* red.h — EL LADO DEL TELÉFONO de W5RED v1 (bt/puente_red/PROTOCOLO.md): la red de senew por el «puente» (SENUS, en el ordenador).
 *
 * Portátil (C99, sin malloc, sin libc más allá de memcpy/memset): corre igual en el W580 (el tubo es el SPP del servicio Bluetooth)
 * y en el PC (el tubo es un socket TCP a la puerta de pruebas del puente: bt/red/prueba_pc.c). Todo por SONDEO: red_paso() mueve
 * lo que haya en los dos sentidos y no bloquea nunca; las órdenes de la app arrancan algo y red_canal() dice en qué va.
 *
 * Un CANAL es una petición HTTP (cualquier verbo; http y https: el TLS lo hace el puente) o un socket TCP (con TLS opcional). Hay
 * RED_CANALES a la vez; cada uno con su cola de recepción (RED_RX) y de envío (RED_TX) y su ventana: el puente no manda a un canal
 * más de lo que cabe en su cola, así que una app que no lee no pierde nada (el otro lado espera). */
#ifndef RED_H
#define RED_H
#include <stdint.h>

typedef struct {
    int      (*enviar)(const void *d, unsigned n);   /* bytes aceptados (0 = ahora no hay sitio) */
    int      (*recibir)(void *d, unsigned max);      /* bytes leídos (0 = nada) */
} red_tubo_t;

#ifndef RED_CANALES
#define RED_CANALES 4
#endif
#define RED_CARGA   2048                             /* la carga máxima que pedimos (el puente da hasta 4096: vale la menor) */
#define RED_RX      16384                            /* cola de recepción de cada canal = la ventana que se le da al puente (16 KB: una app a
                                                        30 cuadros/s baja ~2 veces más que con 8) */
#define RED_TX      16384                            /* cola de envío de cada canal (16 KB: una app a 30 cuadros/s deja más por cuadro) */
#define RED_CAB     768                              /* cabeceras de la respuesta HTTP (o «ip:puerto» de un TCP) */

/* la sesión */
enum { RED_APAGADA = 0, RED_SALUDANDO, RED_LISTA, RED_CAIDA };
/* un canal */
enum { RED_C_LIBRE = 0, RED_C_ABRIENDO, RED_C_ABIERTO, RED_C_CERRADO, RED_C_CERRANDO };
/* códigos de cierre (los de PROTOCOLO.md §3) */
enum { RED_BIEN = 0, RED_DNS, RED_RECHAZADA, RED_PLAZO, RED_TLS, RED_HTTP, RED_RED, RED_CORTADA, RED_PROHIBIDO, RED_DEMASIADOS,
       RED_PROTOCOLO, RED_ABORTADA, RED_INTERNO, RED_SIN_SESION };
/* banderas de red_http / red_tcp */
enum { RED_B_TLS = 1, RED_B_SIN_REDIR = 2, RED_B_SIN_VERIFICAR = 4 };
/* errores al abrir (negativos) */
enum { RED_E_SIN_SESION = -1, RED_E_OCUPADO = -2, RED_E_ARG = -3, RED_E_SIN_CANAL = -4 };

void red_empezar(const red_tubo_t *t, const char *cliente);   /* el tubo se acaba de abrir: manda el HOLA */
void red_parar(void);                                          /* el tubo se cortó: los canales abiertos quedan CERRADOS (RED_SIN_SESION) */
void red_paso(void);                                           /* mover lo que haya; no bloquea */
int  red_estado(void);                                         /* RED_APAGADA..RED_CAIDA */
const char *red_nombre_puente(void);                           /* lo que dijo el HOLA_OK */

/* abrir: devuelve el canal (1..RED_CANALES) o un RED_E_*. Todo se COPIA. cabeceras: líneas «Nombre: valor\r\n» o NULL. cuerpo: el de
 * la petición (si n > 0) entero aquí, hasta RED_TX bytes; para uno más largo, cuerpo = NULL, n = RED_CUERPO_A_TROZOS y luego
 * red_enviar() + red_terminar(). plazo_s: 0 = 30 s. */
#define RED_CUERPO_A_TROZOS 0xFFFFFFFFu
int  red_http(const char *metodo, const char *url, const char *cabeceras, const void *cuerpo, unsigned n, unsigned banderas, unsigned plazo_s);
int  red_tcp(const char *host, unsigned puerto, unsigned banderas, unsigned plazo_s);

int  red_canal(int c);                                         /* RED_C_* */
int  red_codigo(int c);                                        /* el de cierre (con RED_C_CERRADO) */
const char *red_texto(int c);                                  /* el texto del cierre («Name or service not known»…) */
int  red_http_estado(int c);                                   /* 200, 404…; 0 = aún no ha llegado la respuesta */
int  red_cabeceras(int c, char *d, unsigned max);              /* COPIA las cabeceras de la respuesta (TCP: «ip:puerto»); bytes copiados */
int  red_fin_recibido(int c);                                  /* 1 = la otra punta ya no mandará más (y lo que haya en la cola es lo último) */
int  red_disponibles(int c);                                   /* bytes en la cola de recepción */
int  red_recibir(int c, void *d, unsigned max);                /* COPIA y saca; 0 = nada ahora */
int  red_enviar(int c, const void *d, unsigned n);             /* bytes aceptados en la cola de envío */
int  red_pendiente(int c);                                     /* bytes aún sin salir de la cola de envío */
int  red_terminar(int c);                                      /* FIN: ya no se mandará más (fin del cuerpo HTTP / medio cierre TCP) */
void red_cerrar(int c);                                        /* soltar el canal (si seguía abierto, se aborta) */

/* cuentas, para el diagnóstico */
typedef struct { uint32_t tramas_rx, tramas_tx, bytes_rx, bytes_tx, errores, ventana_violada; } red_cuentas_t;
const red_cuentas_t *red_cuentas(void);
#endif
