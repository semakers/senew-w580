/* El núcleo USB: capítulo 9 (EP0), los descriptores de UNA interfaz MTP y el «tubo» que usa el respondedor MTP.
 * Pista USBD. Contrato: docs/usb_contrato.md §2; lo hecho y lo que falta: docs/usb_driver.md.
 * Por debajo, hal/usb_dc.h (el teléfono) o herr/pruebas/usb_dc_falso.c (el PC). Nada aquí bloquea: usb_paso() atiende como
 * mucho USB_EVENTOS_POR_PASO eventos y vuelve. */
#ifndef USB_H
#define USB_H
/* (USB600) el modelo en las cadenas USB y MTP («W580 sistema propio», el número de serie…): lo da la placa (el W600, por -D en
 * placas/w600i/placa.mk); sin nada, el W580 de siempre */
#ifndef USB_TEXTO_MODELO
#define USB_TEXTO_MODELO "W580"
#endif
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;

/* ---- el contrato (§2) ---- */
int  usb_iniciar(void);          /* arranca el controlador y pone el pull-up. 0 = bien; < 0 = el controlador no contesta */
void usb_paso(void);             /* desde el bucle principal: atiende eventos; NUNCA bloquea más de unos ms (perro) */
int  usb_conectado(void);        /* configurado por el host (SET_CONFIGURATION 1) */
int  usb_carga_ma(void);         /* (CARGA) mA que el PC concede para cargar: 500 configurado y despierto; 0 si no (api/carga.h) */
/* el «tubo» de la interfaz MTP (bulk) */
int  usb_tubo_leer(u8 *p, u32 max);          /* no bloquea: bytes recibidos (0 si nada) */
int  usb_tubo_escribir(const u8 *p, u32 n);  /* no bloquea: bytes aceptados (puede ser menos que n) */
int  usb_tubo_evento(const u8 *p, u32 n);    /* interrupt IN (eventos MTP): n si salió a la cola, 0 si el anterior aún no se ha ido */
int  usb_tubo_libre(void);                   /* cuánto cabe ahora en el envío */
/* CÓMO SE CORTAN LAS TRANSFERENCIAS DE ENTRADA (IN): lo que se escribe en el tubo es una sucesión de CONTENEDORES MTP (cabecera
 * de 12 bytes con la longitud total en los 4 primeros, little-endian). El núcleo lee esa longitud y termina cada contenedor como
 * pide USB (y MTP, «USB Still Image Capture» §6 / MTP 1.1 Apéndice D): con un paquete corto, o con un ZLP si la longitud es
 * múltiplo de 64. El respondedor no tiene que hacer nada más que escribir contenedores enteros, en los trozos que quiera.
 * Longitud 0xFFFFFFFF (objetos de más de 4 GiB) no se admite: aquí no caben. Una longitud < 4 (malformada) se trata como 4. */

/* ---- (USBD) lo que añade, «Propuesta de cambio» en el contrato ---- */
void usb_parar(void);            /* pull-up fuera, controlador parado; el tubo se vacía */
void usb_poner_serie(const char *s);   /* (Fase 3b) el iSerialNumber: la serie derivada del id vinculado (api/gestion.c); «» = la fija. Vale desde la próxima enumeración */
/* Vigilar el cable: llamarla desde el bucle con los ms del sistema. Lee VBUS (PMU 23 bit 7, SOLO lectura) como mucho cada
 * USB_VBUS_MS; si aparece arranca (usb_iniciar), si se va lo para todo (usb_parar). Devuelve 1 si el USB está en marcha. */
int  usb_vigilar(u32 ahora_ms);
int  usb_activo(void);           /* 1 entre usb_iniciar() y usb_parar() */
/* AVISOS para el respondedor: por qué se ha tirado lo que había en el tubo (0 = ninguno). (AUD9) Se acumulan: devuelve el más
 * FUERTE pendiente (BUS > RESET_MTP > CANCELAR) y lo borra con los más débiles, que ya van dentro. Mientras haya alguno sin
 * leer, la petición de clase «Get Device Status» contesta Device_Busy (0x2019). Qué los da y qué tiene que hacer el respondedor:
 *   CANCELAR  — Cancel de la clase (0x64); SET_INTERFACE(0); CLEAR_FEATURE(HALT) del bulk IN con un contenedor a medias.
 *               Acabar la transacción en curso (sin respuesta) y SEGUIR con la sesión abierta.
 *   RESET_MTP — Device Reset de la clase (0x66). Acabar la transacción y CERRAR la sesión.
 *   BUS       — reset del bus, SET_CONFIGURATION (0, o 1 estando ya configurado), cable fuera. Acabar y CERRAR la sesión.
 * En todos, el tubo ya está vacío en los dos sentidos y lo siguiente que se escriba empieza un contenedor nuevo. */
enum { USB_AVISO_NADA = 0, USB_AVISO_CANCELAR = 1, USB_AVISO_RESET_MTP = 2, USB_AVISO_BUS = 3 };
int  usb_tubo_aviso(void);
/* El estado, para la pantalla «Conectado por USB» y la app Sistema */
enum { USB_EST_PARADO = 0, USB_EST_ESPERA, USB_EST_DIRECCION, USB_EST_CONFIGURADO, USB_EST_SUSPENDIDO };
typedef struct { u8 estado, direccion, configuracion; u32 resets, setups, stalls, rx_bytes, tx_bytes, contenedores_in, avisos; } UsbInfo;
void usb_info(UsbInfo *i);

#define USB_EVENTOS_POR_PASO 32
#define USB_VBUS_MS 300

/* Los endpoints de la interfaz MTP: los de la interfaz PTP (PictBridge, 6/1/1) del ORIGINAL, «SIC Device», en 0x44281D70
 * [observado por USB0, docs/usb.md §8]: bulk OUT 0x02, interrupt IN 0x81 (intervalo 16), bulk IN 0x82, todos de 64 B. */
#define USB_EP_BULK_IN   0x82
#define USB_EP_BULK_OUT  0x02
#define USB_EP_INT_IN    0x81
#define USB_TAM_EP0      64
#define USB_TAM_BULK     64
#define USB_TAM_INT      64
/* VID/PID DE PRUEBAS: 0x1209 es pid.codes; 0x0001 es su «Test PID» (https://pid.codes/1209/0001/), solo para uso privado
 * y pruebas. [sin saber] si un PID propio hará falta: nunca para el uso del mantenedor. */
#define USB_VID 0x1209
#define USB_PID 0x0001
#define USB_VENDOR_MS 0x54        /* el bMS_VendorCode que se anuncia en la cadena 0xEE («MSFT100») */
#endif
