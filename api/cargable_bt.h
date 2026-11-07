/* cargable_bt.h — (1.4, pista BT) EL GRUPO BLUETOOTH de la tabla `w5`, visto desde el SISTEMA (docs/sistema.md §30).
 *
 * El lado de las apps es bt/sdk/w5_bt.h (se incluye detrás de w5.h). Este fichero repite, para el sistema, lo MISMO y con los
 * MISMOS números: las 15 funciones en el mismo orden y con la misma firma, W5_CAP_BT, W5_BT_*, W5_BTF_* y W5BtDisp. Que los
 * dos digan lo mismo lo comprueba «make probar-bt-abi» (herr/bt/comprobar_bt_abi.py): si alguien toca uno y no el otro, falla.
 *
 * Dónde va el grupo: DETRÁS de la tabla 1.3, en la casilla W5_BT_INDICE = el número de funciones de W5_API (lo que dice
 * w5_bt.h por omisión). W5_API (cargable.h) NO cambia: ni la tabla congelada, ni W5Api, ni w5_nombres.h, ni ninguna app ya
 * compilada. El sistema arma W5Api + este grupo en una sola estructura (CgApi, shell/cargador.h) y dice n = 1.3 + 15 y menor 4
 * (CG_API_MENOR). Una app que NO usa el Bluetooth se sigue compilando con W5_API_MENOR = 3 (cargable.h no se ha tocado) y carga
 * igual en un sistema 1.3 y en uno 1.4.
 *
 * Capacidad: W5_CAP_BT solo la da un sistema que trae el servicio (el W580: plat/bt_w580.c). W600, W810 y el PC tienen la
 * tabla ENTERA (las 15 casillas existen y contestan W5_NO_DISPONIBLE / W5_BT_APAGADO), pero no la capacidad. */
#ifndef CARGABLE_BT_H
#define CARGABLE_BT_H
#include "cargable.h"

#define W5_BT_MENOR 4                          /* la menor de la API que trae este grupo */
enum { W5_CAP_BT = 1 << 23 };                  /* hay radio Bluetooth para las apps (bits 0–14, 16, 17, 20–22 ya ocupados) */
enum { W5_BT_APAGADO = 0, W5_BT_ENCENDIENDO, W5_BT_LISTO, W5_BT_BUSCANDO, W5_BT_CONECTANDO, W5_BT_CONECTADO, W5_BT_FALLO };
enum { W5_BTF_OK = 0, W5_BTF_SIN_CHIP, W5_BTF_SIN_RESPUESTA, W5_BTF_PARCHE, W5_BTF_VELOCIDAD, W5_BTF_PIN, W5_BTF_SIN_SPP,
       W5_BTF_CONEXION, W5_BTF_CAIDA };
typedef struct {
    unsigned char addr[6];                     /* BD_ADDR, el byte más significativo primero */
    unsigned char res[2];                      /* a cero */
    unsigned int  clase;                       /* Class of Device (HC-05: 0x001F00) */
    char          nombre[32];                  /* acabado en 0; «» si no contestó al nombre */
} W5BtDisp;
typedef char w5_bt_disp_mide_44[sizeof(W5BtDisp) == 44 ? 1 : -1];

/* LAS MISMAS LÍNEAS que W5_API14 de bt/sdk/w5_bt.h (sin los comentarios largos: el contrato documentado está allí) */
#define W5_API14(F) \
    F(int, bt_encender, (void)) \
    F(void, bt_apagar, (void)) \
    F(int, bt_estado, (void)) \
    F(int, bt_fallo, (void)) \
    F(int, bt_buscar, (unsigned segundos)) \
    F(int, bt_encontrados, (void)) \
    F(int, bt_encontrado, (int i, W5BtDisp *d)) \
    F(void, bt_pin, (const char *pin)) \
    F(int, bt_emparejado, (const unsigned char addr[6])) \
    F(int, bt_conectar_spp, (const unsigned char addr[6])) \
    F(int, bt_enviar, (const void *d, unsigned n)) \
    F(int, bt_pendiente, (void)) \
    F(int, bt_recibir, (void *d, unsigned max)) \
    F(void, bt_desconectar, (void)) \
    F(int, bt_recordado, (unsigned char addr[6]))
#endif
