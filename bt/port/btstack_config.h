/* btstack_config.h — configuración de BTstack para el servicio Bluetooth de senew (W580i, STLC2500 por H4).
 * Solo Classic: HCI + L2CAP + cliente SDP + RFCOMM (SPP de cliente). Sin LE, sin SCO, sin malloc: todo en
 * pools estáticos dimensionados para UN robot. Lo mismo compila en el PC (prueba con el chip falso) y para ARM.
 * Diseño: bt/DISENO.md. */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* --- port embebido: run loop propio con hal_time_ms() y hal_uart_dma.h por sondeo --- */
#define HAVE_EMBEDDED_TIME_MS
/* (sin HAVE_HAL_UART_BUFFERS: la HAL de bt/port/hal_uart_bloques.c es la variante "void") */

/* --- qué se compila --- */
#define ENABLE_CLASSIC
#ifndef BT_SIN_LOG                   /* make tamanos SUF=-sinlog OPT="-Os -DBT_SIN_LOG": sin los textos de error */
#define ENABLE_LOG_ERROR
#endif
#ifdef BT_PC                         /* en el PC, todo el registro; en el teléfono solo errores (y por la UART 0) */
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP
#endif

/* --- tamaños --- */
/* L2CAP MTU: RFCOMM necesita >= 48; el robot va a 9600 baudios, no hace falta más. 339 = un DH5 de BR. */
/* (velocidad, 2026-10-25) 1021 = el búfer ACL del STLC2500 con el parche (Read_Buffer_Size) y el de un 3-DH5. BTstack elige los
 * tipos de paquete del aire por este tamaño: con 339 descartaba 2-DH3, 3-DH3, 2-DH5 y 3-DH5 (Create_Connection con 0xFF18) y
 * las tramas RFCOMM no pasaban de ~330 B. */
#ifndef HCI_ACL_PAYLOAD_SIZE
#define HCI_ACL_PAYLOAD_SIZE 1021
#endif
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4

#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_L2CAP_SERVICES  1     /* RFCOMM la registra (aunque solo se use de cliente) */
#define MAX_NR_L2CAP_CHANNELS  2     /* SDP (un rato) + RFCOMM */
#define MAX_NR_RFCOMM_MULTIPLEXERS 1
#define MAX_NR_RFCOMM_SERVICES 1     /* (0.29.3) el canal 7 propio: un Mac con SENUS llama al teléfono (bt_servicio.c, «el puente que llama») */
#define MAX_NR_RFCOMM_CHANNELS 1
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 4   /* claves de enlace en RAM; el servicio las vuelca a su fichero */
#define MAX_NR_SERVICE_RECORD_ITEMS 0
#define MAX_NR_BNEP_SERVICES 0
#define MAX_NR_BNEP_CHANNELS 0
#define MAX_NR_HFP_CONNECTIONS 0
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 0
#define MAX_NR_AVDTP_CONNECTIONS 0
#define MAX_NR_AVRCP_CONNECTIONS 0
#define MAX_NR_HID_HOST_CONNECTIONS 0
#define MAX_NR_HIDS_HOSTS 0
#define MAX_NR_GOEP_SERVER_SERVICES 0
#define MAX_NR_GOEP_SERVER_CONNECTIONS 0
#define MAX_NR_GATT_CLIENTS 0
#define MAX_NR_WHITELIST_ENTRIES 0
#define MAX_NR_SM_LOOKUP_ENTRIES 0
#define MAX_NR_LE_DEVICE_DB_ENTRIES 0
#define NVM_NUM_LINK_KEYS 4
#define NVM_NUM_DEVICE_DB_ENTRIES 0

#endif
