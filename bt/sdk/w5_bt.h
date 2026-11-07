/* w5_bt.h — LA API BLUETOOTH PARA APPS (.sen): la ampliación 1.4 de la tabla `w5` que propone bt/DISENO.md §5.2.
 *
 * ESTADO: PROPUESTA. El sistema de hoy (API 1.3) NO la tiene. Este fichero vive en bt/sdk/ y se incluye DESPUÉS de
 * w5.h; no toca cargable.h. Cuando el servicio entre en sistema/ (DISENO §6), W5_API14 se pega al final de W5_API en
 * sistema/api/cargable.h, W5_API_MENOR pasa a 4 y W5_CAP_BT entra en lo que el sistema ofrece en W5_Q_CAPS.
 *
 * Reglas que sigue (las de cargable.h): la tabla SOLO crece por el final; nada cambia de número ni de significado; las
 * estructuras que cruzan van por puntero, fijadas campo a campo, y el sistema las COPIA; la app es DUEÑA del servicio
 * mientras está cargada (como la serie): al descargarla el sistema desconecta y apaga la radio. Todo es ASÍNCRONO: las
 * órdenes arrancan algo y bt_estado() dice en qué va; la app lo mira en un ui_cada. bt_paso() lo llama el sistema.
 *
 * CÓMO SE USA COMO «OPCIONAL» (una app que funciona con y sin Bluetooth, como una app de robots):
 *   - NO poner W5_CAP_BT en .caps de W5_APP (si no, el cargador la rechaza en un sistema sin radio);
 *   - compilar con la API que haya (hoy 1.3: la app declara api_menor = 3 y carga en 1.3 y en 1.4);
 *   - antes de llamar a nada de esto: if (w5_bt_hay()) … — mira que la tabla de ESTE sistema es lo bastante larga
 *     (w5->n), que su menor es >= 4 y que el aparato tiene la radio (W5_CAP_BT en W5_Q_CAPS). Sin eso, NO se llama.
 *
 * HUECOS DE DISENO.md §5.2 QUE ESTE FICHERO RELLENA (decisiones mías, a revisar al integrar):
 *   1. W5BtDisp con relleno EXPLÍCITO (res[2]) y tamaño fijo 44 B (= bt_disp_t del servicio en ARM y en AArch64).
 *   2. Las constantes W5_BT_* y W5_BTF_* con los mismos números que bt_estado_t y bt_fallo_t.
 *   3. Los códigos de vuelta: W5_OK / W5_ERROR (orden fuera de estado o argumento malo) / W5_NO_DISPONIBLE (sin W5_CAP_BT).
 *   4. bt_pin: copia como mucho 16 cifras ASCII; NULL o «» = «1234». Se recuerda hasta descargar la app.
 *   5. bt_buscar: 1..30 s (fuera de rango se recorta, como el servicio).
 *   6. W5_BT_INDICE: en qué casilla de la tabla empieza el grupo. Por omisión, justo detrás de la 1.3. Si al integrar
 *      entrara otra cosa antes, se cambia SOLO este número (y la menor mínima, W5_BT_MENOR).
 *   7. Lo que DISENO no dice y el servicio hace: un robot APAGADO (page timeout) llega como W5_BTF_SIN_SPP, no como
 *      W5_BTF_CONEXION (bt_servicio.c: el fallo de la consulta SDP tapa el de la conexión). La app trata los dos igual:
 *      «el robot no contesta».
 * Lo que NO se da a las apps (DISENO §5.2): bt_paso, la BD_ADDR propia, los ficheros de parche, la velocidad del chip y
 * olvidar claves de enlace (eso va en Ajustes del sistema). */
#ifndef W5_BT_H
#define W5_BT_H
#include "w5.h"

#define W5_BT_MENOR 4                          /* la menor de la API que trae este grupo */
enum { W5_CAP_BT = 1 << 23 };                  /* hay radio Bluetooth para las apps (bits 0–14, 16, 17, 20–22 ya ocupados) */

/* bt_estado(): los números de bt_estado_t (bt/servicio/bt_servicio.h) */
enum { W5_BT_APAGADO = 0, W5_BT_ENCENDIENDO, W5_BT_LISTO, W5_BT_BUSCANDO, W5_BT_CONECTANDO, W5_BT_CONECTADO, W5_BT_FALLO };
/* bt_fallo(): los números de bt_fallo_t */
enum { W5_BTF_OK = 0, W5_BTF_SIN_CHIP, W5_BTF_SIN_RESPUESTA, W5_BTF_PARCHE, W5_BTF_VELOCIDAD, W5_BTF_PIN, W5_BTF_SIN_SPP,
       W5_BTF_CONEXION, W5_BTF_CAIDA };

/* un aparato encontrado (bt_encontrado): 44 bytes, campo a campo, sin punteros; el sistema lo COPIA */
typedef struct {
    unsigned char addr[6];                     /* BD_ADDR, el byte más significativo primero («98:D3:31:…») */
    unsigned char res[2];                      /* a cero */
    unsigned int  clase;                       /* Class of Device (HC-05: 0x001F00) */
    char          nombre[32];                  /* acabado en 0; «» si no contestó al nombre */
} W5BtDisp;
typedef char w5_bt_disp_mide_44[sizeof(W5BtDisp) == 44 ? 1 : -1];

#define W5_API14(F) \
    F(int, bt_encender, (void))                          /* arranca la radio (asíncrono: mirar bt_estado); W5_OK; W5_ERROR si ya está encendida; W5_NO_DISPONIBLE sin W5_CAP_BT */ \
    F(void, bt_apagar, (void))                           /* cierra todo y apaga la radio */ \
    F(int, bt_estado, (void))                            /* W5_BT_* */ \
    F(int, bt_fallo, (void))                             /* W5_BTF_*: por qué está en W5_BT_FALLO (o el aviso W5_BTF_PARCHE) */ \
    F(int, bt_buscar, (unsigned segundos))               /* inquiry + nombres (1..30 s); W5_OK, o W5_ERROR si no está W5_BT_LISTO */ \
    F(int, bt_encontrados, (void))                       /* cuántos van (crece mientras busca) */ \
    F(int, bt_encontrado, (int i, W5BtDisp *d))          /* COPIA el i-ésimo a d: W5_OK o W5_ERROR */ \
    F(void, bt_pin, (const char *pin))                   /* el PIN que se dará si el robot lo pide (COPIA, <= 16 cifras); NULL = «1234» */ \
    F(int, bt_emparejado, (const unsigned char addr[6])) /* 1 si hay clave de enlace guardada (no pedirá PIN) */ \
    F(int, bt_conectar_spp, (const unsigned char addr[6]))   /* SDP (0x1101) + RFCOMM; empareja si hace falta. W5_OK = arrancado (mirar bt_estado); W5_ERROR si no está W5_BT_LISTO */ \
    F(int, bt_enviar, (const void *d, unsigned n))       /* bytes aceptados en la cola (256 B); salen en orden. 0 si no está conectado */ \
    F(int, bt_pendiente, (void))                         /* bytes aún en la cola de envío */ \
    F(int, bt_recibir, (void *d, unsigned max))          /* no bloquea: bytes copiados (0 = nada) */ \
    F(void, bt_desconectar, (void))                      /* cierra el canal Y el enlace; vuelve a W5_BT_LISTO (también desde W5_BT_FALLO) */ \
    F(int, bt_recordado, (unsigned char addr[6]))        /* COPIA el último robot conectado (del almacén del sistema): 1 si hay, 0 si no */

/* El número de funciones de la tabla 1.3 (contadas, no escritas a mano) y el sitio del grupo. */
#define W5_BT_CUENTA_(r, n, a) +1
enum { W5_BT_N13 = 0 W5_API(W5_BT_CUENTA_), W5_BT_N14 = 0 W5_API14(W5_BT_CUENTA_) };
#ifndef W5_BT_INDICE
#define W5_BT_INDICE W5_BT_N13                 /* casilla de bt_encender en la tabla (hueco 6 de arriba) */
#endif

/* La tabla vista con el grupo: la de siempre, (hueco si lo hubiera) y las 15 de arriba. Mismo orden que tendrá en W5Api. */
typedef struct W5ApiBt {
    W5Api base;
#if W5_BT_INDICE > W5_BT_N13
    void (*hueco_[W5_BT_INDICE - W5_BT_N13])(void);
#endif
#define W5_BT_F_(r, nombre, args) r (*nombre) args;
    W5_API14(W5_BT_F_)
#undef W5_BT_F_
} W5ApiBt;
#define W5BT ((const W5ApiBt *)w5)

/* ¿se puede llamar a esto en ESTE sistema? (la API es opcional: ver arriba) */
static inline int w5_bt_hay(void)
{
    int api;
    if (!w5 || w5->n < (unsigned)(W5_BT_INDICE + W5_BT_N14)) return 0;
    api = ap_consulta(W5_Q_API);
    if (api < 0 || ((api >> 16) & 0xFFFF) != W5_API_MAYOR || (api & 0xFFFF) < W5_BT_MENOR) return 0;
    return (ap_consulta(W5_Q_CAPS) & W5_CAP_BT) != 0;
}

/* Los nombres cortos (bt_estado() es W5BT->bt_estado()), como w5_nombres.h. Quien enlace también el servicio del
 * sistema (el arnés del PC) define W5_BT_SIN_NOMBRES para que no choquen con los de bt_servicio.h. */
#ifndef W5_BT_SIN_NOMBRES
#define bt_encender (W5BT->bt_encender)
#define bt_apagar (W5BT->bt_apagar)
#define bt_estado (W5BT->bt_estado)
#define bt_fallo (W5BT->bt_fallo)
#define bt_buscar (W5BT->bt_buscar)
#define bt_encontrados (W5BT->bt_encontrados)
#define bt_encontrado (W5BT->bt_encontrado)
#define bt_pin (W5BT->bt_pin)
#define bt_emparejado (W5BT->bt_emparejado)
#define bt_conectar_spp (W5BT->bt_conectar_spp)
#define bt_enviar (W5BT->bt_enviar)
#define bt_pendiente (W5BT->bt_pendiente)
#define bt_recibir (W5BT->bt_recibir)
#define bt_desconectar (W5BT->bt_desconectar)
#define bt_recordado (W5BT->bt_recordado)
#endif
#endif
