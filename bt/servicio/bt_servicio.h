/* bt_servicio.h — EL SERVICIO BLUETOOTH de senew: lo que el sistema ofrece a las apps (y, a través de la tabla
 * w5, a las .sen; ver bt/DISENO.md §5). Debajo: BTstack (HCI, L2CAP, cliente SDP, RFCOMM) sobre el STLC2500.
 *
 * Todo es ASÍNCRONO y nada bloquea: las órdenes arrancan algo y bt_estado() dice en qué va. El sistema llama a
 * bt_paso() en cada vuelta de su bucle (o en cada tic) mientras el servicio esté encendido.
 *
 * Un solo robot a la vez (un enlace ACL, un canal RFCOMM). Emparejamiento LEGADO con PIN (BT 2.0: ni el chip
 * ni el HC-05 tienen SSP); la clave de enlace se guarda y la segunda vez ya no se pide PIN. */
#ifndef BT_SERVICIO_H
#define BT_SERVICIO_H
#include <stdint.h>

typedef enum {
    BT_APAGADO = 0,
    BT_ENCENDIENDO,          /* pulso de reset, esperar P2.0, init HCI, parche, BD_ADDR, velocidad */
    BT_LISTO,                /* el chip contesta: se puede buscar o conectar */
    BT_BUSCANDO,             /* inquiry + nombres */
    BT_CONECTANDO,           /* ACL, PIN, SDP, RFCOMM */
    BT_CONECTADO,            /* el canal SPP está abierto: bt_enviar / bt_recibir */
    BT_FALLO                 /* ver bt_fallo(); bt_apagar() y volver a encender */
} bt_estado_t;

typedef enum {
    BT_OK = 0,
    BT_F_SIN_CHIP,           /* P2.0 no subió tras el reset (chip sin alimentación / reloj / pines) */
    BT_F_SIN_RESPUESTA,      /* el chip no contestó a HCI_Reset */
    BT_F_PARCHE,             /* la descarga del .ptc/.ssf se cortó (sigue andando con su ROM) — no es fatal */
    BT_F_VELOCIDAD,          /* 0xFCFC sin respuesta a la nueva velocidad */
    BT_F_PIN,                /* el robot rechazó el PIN */
    BT_F_SIN_SPP,            /* el aparato no anuncia el puerto serie (SDP) */
    BT_F_CONEXION,           /* no se pudo abrir el enlace o el canal */
    BT_F_CAIDA               /* el enlace se cayó estando conectado */
} bt_fallo_t;

typedef struct {
    uint8_t  addr[6];        /* BD_ADDR, el byte más significativo primero (como se escribe «98:D3:31:…») */
    uint32_t clase;          /* Class of Device (HC-05: 0x001F00) */
    char     nombre[32];     /* «HC-05»; vacío si no contestó al nombre */
} bt_disp_t;

typedef struct {             /* para la pantalla de diagnóstico y el LEEME */
    uint16_t hci_rev, lmp_subver, fabricante;
    uint8_t  lmp_version;    /* 3 = BT 2.0 */
    char     revision[64];   /* cadena de 0xFC0F */
    int8_t   ptc, ssf;       /* 1 cargado, 0 no había, -1 falló */
    uint32_t baudios;
    uint8_t  mi_addr[6];
} bt_info_t;

/* ---- ciclo de vida ---- */
int         bt_encender(void);                   /* 0 = arrancado (mirar bt_estado) */
void        bt_apagar(void);                     /* cierra todo y deja el chip en reset */
void        bt_paso(void);                       /* del bucle del sistema */
bt_estado_t bt_estado(void);
bt_fallo_t  bt_fallo(void);
const bt_info_t *bt_info(void);

/* ---- buscar ---- */
int  bt_buscar(unsigned segundos);               /* 1..30; acaba solo (bt_estado vuelve a BT_LISTO) */
int  bt_encontrados(void);                       /* cuántos hay (crece mientras busca) */
int  bt_encontrado(int i, bt_disp_t *d);         /* 0 = bien */

/* ---- emparejar y conectar ---- */
void bt_pin(const char *pin);                    /* el que se dará si el robot lo pide; por omisión «1234» */
int  bt_emparejado(const uint8_t addr[6]);       /* 1 si hay clave de enlace guardada */
void bt_olvidar(const uint8_t addr[6]);
int  bt_conectar_spp(const uint8_t addr[6]);     /* SDP (canal del puerto serie) + RFCOMM; empareja si hace falta */

/* ---- el canal ---- */
int  bt_enviar(const void *d, unsigned n);       /* bytes aceptados en la cola (256 B); salen en orden */
int  bt_pendiente(void);                         /* bytes aún en la cola de envío */
int  bt_recibir(void *d, unsigned max);          /* no bloquea */
void bt_desconectar(void);                       /* vuelve a BT_LISTO */

/* ---- lo que pone el sistema (no las apps) ---- */
#include "btstack_chipset_stlc2500c.h"
void bt_config_ficheros(stlc2500c_leer_t f);     /* de dónde salen .ptc/.ssf (fsfab: /ifs/corrections) */
void bt_config_bd_addr(const uint8_t addr[6]);   /* la del GDFS; si no hay, 11:11:11:11:11:11 como el original */
void bt_config_baudios(uint32_t b);              /* velocidad final: 921 600 por omisión; 115 200 = no cambiar */

#endif
