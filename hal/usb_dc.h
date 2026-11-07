/* El controlador USB de DISPOSITIVO del DB2020 (0xFC030000). Pista USBD. Contrato: docs/usb_contrato.md §1;
 * el mapa de registros: docs/usb.md (pista USB0); lo hecho y lo que falta: docs/usb_driver.md.
 *
 * Lo usa SOLO api/usb.c. Hay dos implementaciones: hal/usb_dc.c (el teléfono) y herr/pruebas/usb_dc_falso.c (el PC: un
 * host simulado detrás, para las pruebas de enumeración y del tubo). Este fichero no depende de hal.h, para que el núcleo
 * (api/usb.c) se compile igual en el PC.
 *
 * SEMÁNTICA QUE EL CONTRATO NO DECÍA Y QUE AMBAS IMPLEMENTACIONES CUMPLEN:
 *  - Un PAQUETE por llamada. usbdc_ep_escribir(ep, p, n) con n <= tam_max del endpoint encola UN paquete (n = 0 = paquete de
 *    longitud cero, ZLP) y devuelve n; si el endpoint aún tiene uno pendiente devuelve -1 y no toca nada. Al salir el paquete
 *    (el host lo ha aceptado) llega UDC_EV_TX_HECHO con ese ep. El núcleo trocea y decide los paquetes cortos y los ZLP.
 *  - usbdc_ep_leer(ep, p, max) devuelve UN paquete (0 = ZLP) tras UDC_EV_RX, o -1 si no hay. Hasta que se lee, el endpoint
 *    contesta NAK al host (el control de flujo del tubo es ese). max >= tam_max; si no, se pierde la cola y devuelve max.
 *  - En el EP0 la fase de SETUP llega como UDC_EV_SETUP; los 8 bytes, con usbdc_ep_leer(UDC_EP_SETUP, p, 8) (devuelve 8, o -1
 *    si no hay SETUP: NUNCA devuelve el paquete de datos del EP0; usbdc_ep_leer(0, …) devuelve SOLO datos; AUD9 U1). Un SETUP
 *    nuevo anula lo que quedara pendiente en el EP0 —también un UDC_EV_SETUP anterior aún en la cola— y quita el STALL del EP0.
 *  - usbdc_ep_stall(ep, 0) además pone el conmutador de datos (DATA0/1) a DATA0: es lo que pide CLEAR_FEATURE(HALT).
 *  - usbdc_direccion() se llama DESPUÉS de la fase de estado de SET_ADDRESS. Si el hardware la aplica solo, no hace nada.
 *  - UDC_EV_RESET: reset del bus; el driver ya ha vuelto a la dirección 0 y ha desconfigurado los endpoints != 0.
 *  - UDC_EV_DESCONECTA: VBUS se fue (o el driver lo vio caer); después de él no llega nada más hasta usbdc_iniciar(). */
#ifndef USB_DC_H
#define USB_DC_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;

typedef enum { UDC_EV_RESET = 1, UDC_EV_SETUP, UDC_EV_RX, UDC_EV_TX_HECHO, UDC_EV_SUSPENDE, UDC_EV_REANUDA, UDC_EV_DESCONECTA } UdcEvento;

/* tipos de endpoint, como en bmAttributes del descriptor */
#define UDC_EP_CONTROL 0
#define UDC_EP_ISO     1
#define UDC_EP_BULK    2
#define UDC_EP_INT     3
#define UDC_EP_SETUP   0x40   /* (AUD9 U1) para usbdc_ep_leer: «el SETUP del EP0», nunca sus datos */

int  usbdc_iniciar(void);                    /* puerta de reloj, reset del bloque, IRQ. 0 = bien; < 0 = el bloque no contesta */
void usbdc_conectar(int si);                 /* pull-up D+ (que el PC nos vea) */
int  usbdc_vbus(void);                       /* 1 si hay VBUS (PMU 23 bit 7), 0 si no, -1 si no se pudo leer. SOLO lee */
void usbdc_direccion(u8 dir);
int  usbdc_ep_configurar(u8 ep, u8 tipo, u16 tam_max);   /* ep con bit 7 = IN; tipo como en el descriptor; tam_max 0 = quitarlo. 0 = bien */
int  usbdc_ep_escribir(u8 ep, const u8 *p, u32 n);       /* UN paquete (n <= tam_max); -1 = ocupado; UDC_EV_TX_HECHO al salir */
int  usbdc_ep_leer(u8 ep, u8 *p, u32 max);               /* UN paquete recibido (tras UDC_EV_RX); -1 = no hay */
void usbdc_ep_stall(u8 ep, int si);
int  usbdc_evento(UdcEvento *ev, u8 *ep);    /* saca el siguiente evento (lo encola la IRQ); 0 si no hay */
void usbdc_parar(void);                      /* pull-up fuera, IRQ tapadas, bloque a reset, puerta cerrada */

/* (USBD) contadores para la pantalla de Sistema y las pruebas */
typedef struct { u32 irqs, eventos, perdidos, setups, rx, tx, stalls, resets, errores; } UdcCuentas;
extern UdcCuentas udc_cta;

/* (2026-10-20, el MTP mudo en el aparato) La foto de la pantalla «Diagnóstico USB» (apps/sistema/usb_diag.c). Solo la da
 * hal/usb_dc.c (débil para las demás implementaciones). Los registros «vivo» se releen al pedirla, y SOLO si el bloque tiene sus
 * relojes (si no, leerlo cuelga el bus): `vivo` = 0 → no se han leído. Índices por endpoint [0..3] = EP0..EP3; *_sin_ep = una
 * IRQ EPN_TX/EPN_RX con EPN_STAT sin número. */
typedef struct {
    u8  vivo, modo;                                   /* modo: 0 = leer el FIFO OUT CON EP_SEL (como el original), 1 = sin (el viejo) */
    u16 devstat, syscon1, irq_en, irq_src, epn_stat, ep_num;                 /* vivo */
    u16 cfg_ep0, cfg_rx2, cfg_tx2, cfg_tx1;                                   /* vivo: +80, EP_RX(2), EP_TX(2), EP_TX(1) */
    u16 n_cfg, cfg_devstat, cfg_irq_en;               /* configuraciones cerradas; DEVSTAT antes de DEV_CFG e IRQ_EN después */
    u8  setup[8];                                     /* el último SETUP */
    u32 irq62, irq63, red;                            /* entradas por cada línea; fuentes que atendió la red de seguridad */
    u32 src[8];                                       /* veces que se vio cada bit de IRQ_SRC */
    u32 rx_irq[4], tx_irq[4], rx_sin_ep, tx_sin_ep;   /* EPN_RX / EPN_TX por endpoint */
    u32 leidos[4], vacios[4], escritos[4], ocupados[4];   /* paquetes; lecturas sin nada (FIFO_EN o sin ACK); escrituras rechazadas */
    u16 st_rx, st_tx, rx_n;                           /* STAT_FLG del último leer/escribir en un EPn; RXFSTAT del último paquete EPn */
    u8  rx_cab[8], tx_cab[8];                         /* los 8 primeros bytes del último paquete EPn recibido / enviado */
    u32 sondeo_tx, sondeo_rx, sin_ack;                /* sucesos de EPn que dio el sondeo de STAT_FLG (sin IRQ); OUT tomados sin ACK */
} UdcDiag;
void usbdc_diag(UdcDiag *d);
void usbdc_diag_modo(int modo);
#endif
