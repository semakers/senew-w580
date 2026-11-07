/* bt_sis.h — (pista BT) EL BLUETOOTH DESDE EL SISTEMA: lo que ven el shell, Ajustes y la tabla de las apps (docs/sistema.md §30).
 *
 * DORMIDO POR OMISIÓN. Nada de esto toca el chip al arrancar: el chip (STLC2500, UART 3) solo se enciende cuando una app lo pide
 * (bt_encender de la tabla 1.4) o desde Ajustes -> «Bluetooth» (la pantalla de diagnóstico). Al irse quien lo encendió (la app
 * se descarga, se sale de la pantalla) el sistema lo apaga: el chip vuelve a quedar en reset.
 * (0.29.1) La única excepción: un teléfono que SENUS emparejó (NV_RED con su bandera) hace UNA llamada por arranque, a los 8 s: shell/red_sis.c, sh_red_arranque_paso.
 *
 * Dos capas:
 *   - el CONTROLADOR (BtDriver): lo da quien tiene radio. Hoy solo plat/bt_w580.c (BTstack + bt/servicio + bt/port, compilados
 *     desde ../bt). Los demás (W600, W810, el PC) no enlazan ninguno y bt_driver() da NULL: shell/bt.c lo declara débil.
 *   - este fichero (shell/bt.c): quién es el dueño, el paso del bucle, no dormir a fondo con la radio encendida, y lo que se
 *     suelta al descargar una app. Es portable: compila igual en los cuatro.
 * Los números de estado y fallo son los de bt/servicio/bt_servicio.h (= W5_BT_* y W5_BTF_* de api/cargable_bt.h). */
#ifndef BT_SIS_H
#define BT_SIS_H
#include "hw.h"
#include "ui.h"

enum { SH_BT_NADIE, SH_BT_SISTEMA, SH_BT_APP, SH_BT_MUSICA, SH_BT_DICTADO, SH_BT_DESCARGA };   /* (E5) DICTADO: el dictado por voz (shell/dictado.c); (1.11) DESCARGA: una canción a la tarjeta (shell/descarga_red.c) */   /* quién lo encendió (el que se lleva el apagado al irse). (1.8) MUSICA: la música
                                                                 * por la red (shell/musica_red.c), que sigue con la app cerrada */
enum { SH_BT_APAGADO = 0, SH_BT_ENCENDIENDO, SH_BT_LISTO, SH_BT_BUSCANDO, SH_BT_CONECTANDO, SH_BT_CONECTADO, SH_BT_FALLO };
enum { SH_BTF_OK = 0, SH_BTF_SIN_CHIP, SH_BTF_SIN_RESPUESTA, SH_BTF_PARCHE, SH_BTF_VELOCIDAD, SH_BTF_PIN, SH_BTF_SIN_SPP,
       SH_BTF_CONEXION, SH_BTF_CAIDA };
enum { SH_BT_ADDR_ALEATORIA, SH_BT_ADDR_GUARDADA };       /* de dónde sale la BD_ADDR propia (no se sabe leer el GDFS: §30.4) */

typedef struct { u8 addr[6]; u32 clase; char nombre[32]; } ShBtDisp;   /* un aparato encontrado (el MSB de la dirección primero) */

/* Lo que enseña la pantalla de diagnóstico, paso a paso (y lo que se dice por la UART 0). -1 = aún no se sabe. */
typedef struct {
    u8  hay;                    /* 0 = este sistema no trae Bluetooth */
    u8  estado, fallo;          /* SH_BT_*, SH_BTF_* */
    u8  dueno;                  /* SH_BT_NADIE / SISTEMA / APP */
    u32 t_encender;             /* hw_ms() de la última orden de encender */
    i32 ms_reset;               /* 1: el pulso de nRESET (100 ms): ms desde encender hasta soltarlo (-1: aún no) */
    i32 ms_p20;                 /* 2: P2.0 alto, ms desde que se soltó el reset (-1: aún no; -2: no subió en 1 s) */
    i32 ms_hci_reset;           /* 3: el PRIMER HCI_Reset contestado (Command Complete), ms desde soltar el reset */
    signed char hci_reset_estado;       /*    su byte de estado (-1: sin respuesta) */
    u8  hay_version;            /* 4: Read_Local_Version contestó */
    u16 hci_rev, lmp_subver, fabricante; u8 lmp_version;
    char revision[64];          /*    la cadena de 0xFC0F (Ericsson_Read_Revision_Information) */
    signed char ptc, ssf;               /* 5: 1 cargado, 0 no había fichero para esta revisión, -1 falló la descarga (-2: aún no) */
    char origen[32];            /*    de dónde se leyeron («/original/ifs/corrections» o «/tarjeta») */
    u8  addr[6], addr_origen;   /* 6: la BD_ADDR que se le da al chip (0xFC22) y de dónde sale (SH_BT_ADDR_*) */
    u8  addr_leida[6], hay_addr_leida;   /* la que devuelve Read_BD_ADDR después */
    u32 baudios, baudios_final; /* 7: la velocidad de ahora y la que se pide al final (115 200 = no se cambia) */
    u32 tx, rx;                 /* bytes por la UART 3 desde que se encendió por primera vez */
    u8  claves, claves_pendientes;   /* claves de enlace guardadas; 1 = hay una nueva que aún no se pudo escribir en la NOR */
    u8  hay_recordado, recordado[6];
} ShBtDiag;

/* EL CONTROLADOR: lo da la plataforma con radio. Todas las funciones existen (ninguna es NULL). */
typedef struct BtDriver {
    int  (*encender)(void);                       /* 0 = arrancado (asíncrono) */
    void (*apagar)(void);
    void (*paso)(void);                           /* del bucle (solo se llama con la radio encendida) */
    int  (*estado)(void);                         /* SH_BT_* */
    int  (*fallo)(void);                          /* SH_BTF_* */
    void (*diag)(ShBtDiag *d);
    int  (*buscar)(unsigned segundos);            /* 0 = arrancada */
    int  (*encontrados)(void);
    int  (*encontrado)(int i, ShBtDisp *d);       /* 0 = bien */
    void (*pin)(const char *pin);                 /* NULL = «1234» */
    int  (*emparejado)(const u8 addr[6]);
    void (*olvidar_todo)(void);                   /* borra las claves de enlace guardadas (RAM y almacén) */
    int  (*conectar_spp)(const u8 addr[6]);       /* 0 = arrancada */
    int  (*enviar)(const void *d, unsigned n);
    int  (*pendiente)(void);
    int  (*recibir)(void *d, unsigned max);
    void (*desconectar)(void);
    int  (*recordado)(u8 addr[6]);                /* 1 si hay un último robot */
    void (*velocidad)(u32 baudios);               /* la final para el PRÓXIMO encendido (115 200 o 921 600) */
    void (*guardar_pendiente)(void);              /* reintenta escribir las claves que no se pudieron (con una app delante el almacén solo deja las suyas) */
    int  (*traza)(int si);                        /* cada paquete HCI por la UART 0: 1/0 la pone o la quita, < 0 solo pregunta */
    void (*propia)(u8 addr[6]);                   /* (0.29.1) la BD_ADDR con la que este teléfono llamará, aunque la radio esté apagada y sin encenderla */
} BtDriver;
const BtDriver *bt_driver(void);                  /* NULL = sin Bluetooth (la de shell/bt.c es débil; plat/bt_w580.c da la de verdad) */

/* ---- lo que usa el resto del sistema ---- */
int  sh_bt_hay(void);
int  sh_bt_encender(int dueno);                   /* SH_BT_SISTEMA o SH_BT_APP; 0 = arrancado; -1 ya encendido o sin radio */
void sh_bt_apagar(void);
void sh_bt_paso(void);                            /* del bucle principal y de app_latido: no hace nada con la radio apagada */
int  sh_bt_activo(void);                          /* 1 si la radio está encendida (no se duerme a fondo: plat_reposo sí) */
int  sh_bt_dueno(void);
void sh_bt_soltar_app(void);                      /* al descargar una app: si era la dueña, desconecta y apaga; PIN a «1234» */
void sh_bt_soltar(int quien);                     /* (1.8) quien se va: si la radio era suya y la red sigue con OTRO dueño, la radio pasa a
                                                    * ese dueño (no se corta su red); si no, desconecta y apaga */
void sh_bt_diag(ShBtDiag *d);
const char *sh_bt_estado_nombre(int estado);
const char *sh_bt_fallo_nombre(int fallo);
Widget *app_bluetooth(void);                      /* Ajustes -> «Bluetooth» (apps/ajustes/bluetooth.c) */
#endif
