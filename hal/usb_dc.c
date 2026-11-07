/* El controlador USB de dispositivo del DB2020 (0xFC030000) para api/usb.c. Pista USBD. Interfaz: hal/usb_dc.h.
 *
 * DE DÓNDE SALE: docs/usb.md (pista USB0): el mapa y las secuencias del ORIGINAL (pila MCCI, 0x44217950..0x442196E8),
 * que mandan. El controlador es, con evidencia fuerte, el UDC de TI de la familia OMAP1 [inferido por USB0]; donde el
 * original no dice algo se sigue drivers/usb/gadget/udc/omap_udc.c de Linux y el TRM del OMAP5912 («USB Function
 * Controller»), y se marca. Solo PIO por la ventana +08 (el original no usa el DMA: docs/usb.md §6).
 *
 * NADA DE ESTO SE HA EJECUTADO EN UN TELÉFONO. Lo que solo dirá el aparato: docs/usb_driver.md §5.
 *
 * Reparto del trabajo: la IRQ (62 «geni» y 63 «non_iso», la misma rutina, como la parte baja del original 0x44218FB8)
 * lee IRQ_SRC, borra lo que ve, lee el SETUP (que no espera) y ENCOLA eventos; los datos de los endpoints se mueven fuera
 * de la IRQ, desde usb_paso() (usbdc_ep_leer / usbdc_ep_escribir), con las IRQ cortadas mientras se usa EP_NUM (la IRQ
 * también lo usa). Mientras un OUT no se lee, el endpoint no se rearma (SET_FIFO_EN) y el host recibe NAK.
 *
 * Política de carga: este fichero NO toca el PMU en absoluto (ni la carga, 61/63, ni nada): docs/bateria.md §8.A. */
#ifndef USBDC_MODELO
/* (USB600) la placa puede dar su HAL (el W600: <hal.h> = soc/db2010/hal.h; con comillas saldría hal/hal.h, el del W580) */
#ifdef USBDC_HAL
#include USBDC_HAL
#else
#include "hal.h"
#endif
/* Todos los accesos al bloque (y a SYSCON) pasan por aquí: en el PC, herr/pruebas/udc_modelo.h los manda a un modelo del UDC */
#define LEE(a)     R16(a)
#define ESC(a, v)  (R16(a) = (u16)(v))
#define LEE8(a)    R8(a)
#define ESC8(a, v) (R8(a) = (u8)(v))
#else
#include "udc_modelo.h"
#endif
#include "usb_dc.h"

/* (USB600) EL MISMO bloque en los dos chips (docs/usb_w600.md): en el DB2020 en 0xFC030000, en el DB2010 en 0xFD000000. La placa
 * que no es el W580 da UDC_BASE, y si su original no escribe +108 (el W600 no), UDC_SIN_R108; y su encendido (puerta y pines),
 * USBDC_PLACA_ENCENDER + usbdc_placa_encender(). Sin nada de eso, esto es el W580 de siempre. */
#ifndef UDC_BASE
#define UDC_BASE 0xFC030000u
#endif
#define UDC UDC_BASE
#define EP_NUM   (UDC + 0x04)
#define DATA     (UDC + 0x08)
#define CTRL     (UDC + 0x0C)
#define STAT_FLG (UDC + 0x10)
#define RXFSTAT  (UDC + 0x14)
#define SYSCON1  (UDC + 0x18)
#define SYSCON2  (UDC + 0x1C)
#define DEVSTAT  (UDC + 0x20)
#define IRQ_EN   (UDC + 0x28)
#define IRQ_SRC  (UDC + 0x30)
#define EPN_STAT (UDC + 0x34)
#define EP0_CFG  (UDC + 0x80)
#define EP_RX(n) (UDC + 0x80 + 4 * (n))
#define EP_TX(n) (UDC + 0xC0 + 4 * (n))
#define R108     (UDC + 0x108)
/* bits [observado por USB0 en el original, nombres de omap_udc.h] */
#define N_SEL 0x20u
#define N_DIR 0x10u
#define N_SETUP 0x40u
#define C_RESET_EP 0x01u
#define C_CLR_EP 0x02u
#define C_FIFO_EN 0x04u
#define C_SET_HALT 0x40u
#define C_CLR_HALT 0x80u
#define S_FIFO_EN 0x04u
#define S_ACK 0x08u
#define S_STALL 0x20u
#define S1_PULLUP 0x0001u
#define S1_SELF_PWR 0x0004u
#define S1_CFG_LOCK 0x0100u
#define S2_CLR_CFG 0x04u
#define S2_DEV_CFG 0x08u
#define S2_STALL 0x20u
#define D_ATT 0x01u
#define D_DEF 0x02u
#define D_ADD 0x04u
#define D_CFG 0x08u
#define D_SUS 0x10u
#define D_RESET 0x20u
#define I_EP0_TX 0x01u
#define I_EP0_RX 0x02u
#define I_SETUP 0x04u
#define I_DS_CHG 0x08u
#define I_EPN_TX 0x10u
#define I_EPN_RX 0x20u
#define I_SOF 0x80u

UdcCuentas udc_cta;
static UdcDiag dg;               /* (2026-10-20) para la pantalla «Diagnóstico USB»: lo que separa las hipótesis del MTP mudo */
#define EPI(n) ((n) & 3)         /* índice de dg por endpoint (EP0..EP3: los que usamos) */
static u8 encendido;             /* la puerta y el reset ya se hicieron (no se deshacen: docs/usb.md §1) */
static u8 iniciado, quiere_pullup, en_reset, cfg_pendiente, n_validos;
static u16 devstat_antes;
static u16 tam_in[16], tam_out[16];
static volatile u8 setup_hay;
static u8 setup_d[8];
static u8 ep0_rx_pend;           /* llegó un paquete al EP0 OUT (la IRQ lo vio): STAT_FLG.ACK solo no basta, se queda a 1 */
static u16 ep0_out_esperado;     /* el EP0 OUT está armado (SET_FIFO_EN): fase de datos o de estado */

/* la cola de eventos: la llena la IRQ, la vacía usbdc_evento() (un productor y un consumidor: índices volátiles) */
#define COLA 64u
static volatile struct { u8 ev, ep, gen; } cola[COLA];   /* (AUD9) volátil: la escribe la IRQ */
/* (AUD9 U1 y BAJO a) Como mucho UN SETUP en la cola: si llega otro antes de que el bucle lo saque, se queda la casilla y sube
 * su generación (los bytes son siempre los del último, setup_d). Así 64 SETUP con el bucle ocupado no llenan la cola. */
static volatile u8 setup_en_cola;
static volatile u32 setup_casilla;
/* (AUD9 BAJO a) Los TX_HECHO/RX de los endpoints 1..15 no van a la cola: son bits (no se pierden nunca; uno por endpoint
 * basta porque cada endpoint tiene como mucho un paquete en vuelo). Se entregan cuando la cola está vacía. */
static volatile u16 epn_tx, epn_rx;
/* (2026-10-20, red de seguridad de los EPn) Lo que el driver sabe que está en el aire: un IN escrito y armado aún sin EPN_TX
 * (tx_vuelo) y un OUT armado aún sin EPN_RX (rx_armado); rx_visto = un OUT del que la IRQ (o el sondeo) dijo «ha llegado». Con
 * ellos: una EPN_TX/EPN_RX con EPN_STAT sin número se atribuye si solo hay un candidato, y si las IRQ de los EPn no llegan nunca,
 * usbdc_evento() mira STAT_FLG.FIFO_EN de los que están en el aire (como hace el original al atenderlos) y da el suceso. Un suceso
 * de más es inofensivo: escribir rehúsa si el FIFO sigue ocupado y leer da -1 si no hay paquete. Las cuentas, en dg. */
static volatile u16 tx_vuelo, rx_armado, rx_visto;
static volatile u8 epn_irq_visto;   /* ya entró alguna IRQ EPN_TX/EPN_RX desde usbdc_iniciar: entonces NO se sondea (el sondeo selecciona EPs) */
static int un_bit(u16 m) { return m && !(m & (m - 1)); }
static u8 bit_num(u16 m) { u8 n = 0; while (m > 1) { m >>= 1; n++; } return n; }
/* La «generación» del EP0: sube con cada SETUP. Un EP0_RX/EP0_TX encolado ANTES de un SETUP nuevo es del control anterior y
 * usbdc_evento() lo tira (usb_dc.h: «un SETUP nuevo anula lo que quedara pendiente en el EP0»). Sin esto, el ZLP de estado del
 * control anterior, aún en la cola, se leía como datos y se llevaba los 8 bytes del SETUP nuevo: el host recibía NAK para
 * siempre (visto en el emulador de UEMU con su anfitrión, 08:07, en la petición de la cadena 0). */
static volatile u8 ep0_gen;
static volatile u32 c_pon, c_saca;
static void encolar(u8 ev, u8 ep)
{
    if (c_pon - c_saca >= COLA) { udc_cta.perdidos++; return; }
    cola[c_pon % COLA].ev = ev; cola[c_pon % COLA].ep = ep; cola[c_pon % COLA].gen = ep0_gen;
    c_pon++; udc_cta.eventos++;
}

/* vaciar las fuentes pendientes, como §3.1 paso 4 (con tope: si el bloque no contesta, no se cuelga) */
static void vaciar_fuentes(void)
{
    int i;
    ESC(EP_NUM, N_SETUP); ESC(EP_NUM, 0);
    for (i = 0; i < 16 && LEE(IRQ_SRC); i++) ESC(IRQ_SRC, 0xFFFF);
}
/* LA PUERTA. docs/hal.md §2.1 dice «USB = puerta 26 / bloque 20»; USB0 (docs/usb.md §1) la vio gobernada por el arranque del
 * DSP, no por la pila USB; y UEMU (work/bitacora_UEMU.md, 07:52) vio al ORIGINAL conectar el pull-up y enumerar con la puerta 26
 * CERRADA (F9090018 bit 1 = 1) y el bloque 20 EN RESET (F9090012 bit 1 = 0) [observado en el emulador], y el coordinador pide
 * reproducir ESE estado (calibración de UEMU). Nuestro syscon_init() deja el bloque 20 VIVO (+12 = 0x03DF), así que aquí se pone
 * como el original: puerta 26 cerrada y bloque 20 a reset, una vez, y no se deshace. Con -DUSB_PUERTA_26=1, en cambio, la
 * secuencia de FUN_4459f428 (bloque 20 a reset, puerta 26 cerrada y abierta en «modo 4», bloque 20 vivo). Qué hace falta de
 * verdad (y qué más cuelga del bloque 20: USB0 lo ve ligado al arranque del DSP, que no usamos): solo el aparato. */
#ifndef USB_PUERTA_26
#define USB_PUERTA_26 0
#endif
/* (2026-10-20, MEDIDO EN EL APARATO) Las dos secuencias de antes (puerta 26 / bloque 20) COLGABAN el bus al leer DEVSTAT: la 26 y
 * el bloque 20 son del PCM (FA020000), no del USB. Lo que el ORIGINAL enciende antes de tocar el UDC (tmp/usb_re/estatico.md y
 * dinamico.md: FUN_4422c3e8 al iniciar, FUN_4422c340 antes del pull-up; [observado] en el desensamblado y en sus trazas) son
 * TRES relojes de SYSCON que nuestro syscon_init deja apagados: +02 bit 12 a 0, la puerta 37 (+1A bit 10) y la 38 (+1A bit 11);
 * más +0A bits 0..2 a 0 y P4.2 = 0. En sus 66 accesos al UDC nunca está la combinación nuestra (+02 = 0x1002, +1A = 0x7FB8).
 * La 38 ya aquí (el original la abre con VBUS, antes del pull-up) es [inferido]: es el estado de toda su enumeración. La espera
 * es [inferido]: el original no espera. Devuelve -1 (y NO se toca 0xFC03xxxx) si SYSCON no tomó los bits. Con -DUSB_PUERTA_26=1
 * o 2, las secuencias viejas (para comparar; las dos colgaban en el W580). */
static int encender_bloque(void)
{
    if (encendido) return 0;
#ifdef USBDC_PLACA_ENCENDER
    usbdc_placa_encender();                                      /* (USB600) la puerta y los pines de ESA placa */
#else
    if (USB_PUERTA_26 == 0) {
        ESC(SYSCON + 0x02, LEE(SYSCON + 0x02) & (u16)~0x1000u);      /* FUN_44235a34(1,1), pc 0x44235AB6 */
        ESC(SYSCON + 0x1A, LEE(SYSCON + 0x1A) & (u16)~0x0400u);      /* puerta 37: EnableClock(0x25,1) */
        ESC(SYSCON + 0x1A, LEE(SYSCON + 0x1A) & (u16)~0x0800u);      /* puerta 38: EnableClock(0x26,1) */
        ESC(SYSCON + 0x0A, LEE(SYSCON + 0x0A) & (u16)~0x0007u);      /* FUN_44235b08(0x10,0) */
        ESC(0xF9000028, LEE(0xF9000028) & (u16)~0x0004u);            /* P4.2 = 0 (gpio_write(4,4,0); gpio_init ya la deja a 0) */
        espera_ms(2);
        if ((LEE(SYSCON + 0x1A) & 0x0C00u) || (LEE(SYSCON + 0x02) & 0x1000u)) return -1;
    } else if (USB_PUERTA_26 == 1) {
        ESC(SYSCON + 0x12, LEE(SYSCON + 0x12) & ((u16)~0x0002u));
        ESC(SYSCON + 0x18, LEE(SYSCON + 0x18) | (0x0002));
        ESC(SYSCON + 0x18, LEE(SYSCON + 0x18) & ((u16)~0x0002u));
        ESC(SYSCON + 0x12, LEE(SYSCON + 0x12) | (0x0002));
    } else {
        ESC(SYSCON + 0x18, LEE(SYSCON + 0x18) | 0x0002u);
        ESC(SYSCON + 0x12, LEE(SYSCON + 0x12) & (u16)~0x0002u);
    }
#endif
    encendido = 1;
    return 0;
}

/* ---------------------------------------------------------------------------------------------- la IRQ */
static void leer_setup(void)                     /* §4.3, y omap_udc.c: se relee mientras llegue otro */
{
    int i, k;
    u16 w[4];
    ESC(IRQ_SRC, I_EP0_TX | I_EP0_RX);          /* lo viejo del EP0 ya no vale */
    for (k = 0; k < 4; k++) {
        ESC(EP_NUM, N_SETUP);
        for (i = 0; i < 4; i++) w[i] = LEE(DATA);
        ESC(EP_NUM, 0);
        ESC(IRQ_SRC, I_SETUP);                  /* [inferido de OMAP] leerlo lo borra; se borra además por si acaso (escribir 1) */
        if (!(LEE(IRQ_SRC) & I_SETUP)) break;
    }
    for (i = 0; i < 4; i++) { setup_d[2 * i] = (u8)w[i]; setup_d[2 * i + 1] = (u8)(w[i] >> 8); }
    for (i = 0; i < 8; i++) dg.setup[i] = setup_d[i];
    setup_hay = 1; ep0_rx_pend = 0; udc_cta.setups++;
    ep0_gen++;

    /* El EP0 OUT se arma ya: para la fase de estado de una lectura (como el original, §4.3 paso 4) o para la fase de datos
     * de una escritura con datos (lo hará la pila del original más tarde; aquí de una vez). */
    ep0_out_esperado = (setup_d[0] & 0x80) || setup_d[6] || setup_d[7];
    if (ep0_out_esperado) { ESC(EP_NUM, 0); ESC(CTRL, C_FIFO_EN); }
    if (setup_en_cola) { cola[setup_casilla % COLA].gen = ep0_gen; return; }   /* ya hay uno en la cola: ahora vale por este (usbdc_evento
                                                                              * saca con las IRQ cortadas: no hay carrera) */
    if (c_pon - c_saca < COLA) { setup_casilla = c_pon; setup_en_cola = 1; }
    encolar(UDC_EV_SETUP, 0);
}
static void cambio_estado(void)                  /* §4.2 paso 1 */
{
    u16 d = LEE(DEVSTAT), x = d ^ devstat_antes;
    devstat_antes = d;
    ESC(IRQ_SRC, I_DS_CHG);
    if (x & D_ATT) {
        if (d & D_ATT) { if (quiere_pullup) ESC(SYSCON1, LEE(SYSCON1) | (S1_PULLUP)); }
        else { ESC(SYSCON1, LEE(SYSCON1) & ((u16)~S1_PULLUP)); encolar(UDC_EV_DESCONECTA, 0); return; }
        /* el original pone un antirrebote de 50 tics a la desconexión: aquí lo hace usb_vigilar (VBUS cada 300 ms) */
    }
    if (x & (D_DEF | D_RESET)) {                 /* §3.4 */
        if (d & D_RESET) { en_reset = 1; }
        else {
            en_reset = 0;
            ESC(IRQ_EN, 0x09);                  /* DS_CHG_IE | EP0_IE, como el original */
            n_validos = 0; cfg_pendiente = 0;
            { int k; for (k = 1; k < 16; k++) tam_in[k] = tam_out[k] = 0; }   /* (AUD9) nada se escribe en un EPn hasta reconfigurar */
            epn_tx = epn_rx = 0; tx_vuelo = rx_armado = rx_visto = 0;
            setup_hay = 0; ep0_gen++; setup_en_cola = 0;   /* (AUD9) un SETUP de antes del reset ya no vale (su casilla se tira por la
                                                             * generación; el siguiente va DETRÁS del RESET en la cola) */
            udc_cta.resets++;
            encolar(UDC_EV_RESET, 0);
        }
    }
    if (x & D_SUS) encolar((d & D_SUS) ? UDC_EV_SUSPENDE : UDC_EV_REANUDA, 0);
}
static void usb_irq(void)
{
    int vueltas;
    udc_cta.irqs++;
    for (vueltas = 0; vueltas < 8; vueltas++) {
        u16 s = LEE(IRQ_SRC);
        if (!s) break;
        { int b; for (b = 0; b < 8; b++) if (s & (1u << b)) dg.src[b]++; }
        if (s & I_DS_CHG) cambio_estado();
        if (s & I_SETUP) leer_setup();           /* un SETUP nuevo deja viejos EP0_TX/EP0_RX que vinieran con él (§4.3 paso 1) */
        else {
            if (s & I_EP0_TX) { ESC(IRQ_SRC, I_EP0_TX); encolar(UDC_EV_TX_HECHO, 0x80); }
            if (s & I_EP0_RX) { ESC(IRQ_SRC, I_EP0_RX); ep0_rx_pend = 1; encolar(UDC_EV_RX, 0x00); }
        }
        if (s & I_EPN_TX) {
            u16 e = LEE(EPN_STAT); ESC(IRQ_SRC, I_EPN_TX);
            epn_irq_visto = 1;
            if (!(e & 0xF)) { dg.tx_sin_ep++; if (un_bit(tx_vuelo)) e = bit_num(tx_vuelo); }   /* sin número: el único IN en el aire */
            if (e & 0xF) { u16 b = (u16)(1u << (e & 0xF)); epn_tx |= b; tx_vuelo &= (u16)~b; if ((e & 0xF) < 4) dg.tx_irq[e & 0xF]++; }
            udc_cta.eventos++;
        }
        if (s & I_EPN_RX) {
            u16 e = LEE(EPN_STAT); ESC(IRQ_SRC, I_EPN_RX);
            epn_irq_visto = 1;
            if (!((e >> 8) & 0xF)) { dg.rx_sin_ep++; if (un_bit(rx_armado)) e = (u16)(bit_num(rx_armado) << 8); }   /* el único OUT armado */
            if ((e >> 8) & 0xF) { u16 b = (u16)(1u << ((e >> 8) & 0xF)); epn_rx |= b; rx_armado &= (u16)~b; rx_visto |= b; if (((e >> 8) & 0xF) < 4) dg.rx_irq[(e >> 8) & 0xF]++; }
            udc_cta.eventos++;
        }
        if (s & I_SOF) ESC(IRQ_SRC, I_SOF);
        if (s & ~(u16)(I_DS_CHG | I_EP0_TX | I_EP0_RX | I_SETUP | I_EPN_TX | I_EPN_RX | I_SOF)) { ESC(IRQ_SRC, (u16)(s & ~(u16)(I_DS_CHG | I_EP0_TX | I_EP0_RX | I_SETUP | I_EPN_TX | I_EPN_RX | I_SOF))); udc_cta.errores++; }
    }
}

/* (2026-10-20) la misma rutina en las dos líneas, contadas aparte: dice si la 63 («non_iso», la de los EPn en OMAP) entra */
static void usb_irq62(void) { dg.irq62++; usb_irq(); }
static void usb_irq63(void) { dg.irq63++; usb_irq(); }

/* ---------------------------------------------------------------------------------------------- la interfaz */
#if defined(MIGAS) && !defined(USBDC_MODELO)
void sh_miga(const char *paso);                  /* (DIAG 2026-10-20) shell/energia.c: el arranque normal se colgaba aquí en el aparato */
static u8 vbus_migas;
#define MIGA_USB(p) do { if (vbus_migas < 1) sh_miga(p); } while (0)
#else
#define MIGA_USB(p) ((void)0)
#endif
int usbdc_vbus(void)                             /* §7: DEVSTAT.ATT, como el original (el PMU 23 bit 7 es solo contraste) */
{
    int v;
    MIGA_USB("usb: encender relojes (37/38, +02.12)");
    if (encender_bloque()) { MIGA_USB("usb: SYSCON NO tomo los relojes: no se toca"); return 0; }
    MIGA_USB("usb: leer DEVSTAT");
    v = LEE(DEVSTAT) & D_ATT;
    MIGA_USB(v ? "usb: DEVSTAT leido, ATT=1" : "usb: DEVSTAT leido, ATT=0");
#if defined(MIGAS) && !defined(USBDC_MODELO)
    vbus_migas = 1;
#endif
    return v;
}
int usbdc_iniciar(void)
{
    u32 e, n;
    MIGA_USB("usb: iniciar (hay VBUS)");
    if (encender_bloque()) return -1;
    e = irq_cerrar();
    ESC(IRQ_EN, 0);                             /* §3.1 */
#ifndef UDC_SIN_R108
    ESC(R108, 0);                               /* [sin saber] qué es; el original escribe 0 (16 bits, como todo el bloque: docs/usb.md §1) */
#endif
    ESC(SYSCON1, LEE(SYSCON1) & ((u16)~S1_PULLUP));
    vaciar_fuentes();
    /* §3.6: lo que escribe el original sin ningún endpoint: EP0 de 64 B en el puntero 1; EP_RX/EP_TX(1..15) = 0x0009 (sin VALID) */
    ESC(SYSCON1, LEE(SYSCON1) & ((u16)~S1_CFG_LOCK));
    ESC(EP0_CFG, 0x3001);
    for (n = 1; n < 16; n++) { ESC(EP_RX(n), 0x0009); ESC(EP_TX(n), 0x0009); }
    ESC(SYSCON1, LEE(SYSCON1) | (S1_CFG_LOCK));
    ESC(SYSCON1, LEE(SYSCON1) | (S1_SELF_PWR));                 /* el original lo pone antes de configurar nada (§3.6); somos autoalimentados */
    for (n = 0; n < 16; n++) tam_in[n] = tam_out[n] = 0;
    tam_in[0] = tam_out[0] = 64;
    c_pon = c_saca = 0; setup_hay = 0; en_reset = 0; cfg_pendiente = 0; n_validos = 0; quiere_pullup = 0;
    setup_en_cola = 0; epn_tx = epn_rx = 0; tx_vuelo = rx_armado = rx_visto = 0; epn_irq_visto = 0;
    devstat_antes = LEE(DEVSTAT);
    vaciar_fuentes();                            /* §3.2 */
    ESC(IRQ_EN, I_DS_CHG);                      /* 0x08: esperando conexión */
    /* POR FLANCO, como el original (ILR = 0x78: prioridad 30, flanco; UEMU, 07:59 [observado en el emulador]). Con flanco, una
     * fuente que quede pendiente al salir de la IRQ no da otro flanco: usbdc_evento() lo mira y la atiende (red de seguridad). */
    intc_habilita(62, usb_irq62);                /* «geni» */
    intc_habilita(63, usb_irq63);                /* «non_iso» */
    iniciado = 1;
    if (!(e)) irq_abrir();
    /* ¿contesta el bloque? SELF_PWR se lee como se ha escrito (el original lo relee: §3.6, «leído 4») */
    if (!(LEE(SYSCON1) & S1_SELF_PWR)) { usbdc_parar(); udc_cta.errores++; return -1; }
    return 0;
}
void usbdc_conectar(int si)
{
    u32 e = irq_cerrar();
    quiere_pullup = (u8)si;
    if (si && (LEE(DEVSTAT) & D_ATT)) ESC(SYSCON1, LEE(SYSCON1) | (S1_PULLUP));     /* §3.2: el pull-up solo con ATT = 1 */
    if (!si) ESC(SYSCON1, LEE(SYSCON1) & ((u16)~S1_PULLUP));
    if (!e) irq_abrir();
}
void usbdc_direccion(u8 dir) { (void)dir; }      /* SET_ADDRESS lo resuelve el hardware: el original nunca escribe la dirección */

static u16 codigo_tam(u16 tam) { u16 c = 0; while ((8u << c) < tam && c < 6) c++; return c; }
/* El FIFO de 2 KB, en unidades de 8 bytes: el EP0 en el puntero 1 (64 B) y un hueco FIJO de 64 B por endpoint y sentido,
 * desde el puntero 9 (byte 72), como el original reparte detrás del EP0. 30 huecos: el último acaba en 1992 <= 0x7F8. */
static u16 puntero(u8 ep) { u16 hueco = (u16)(((ep & 15) - 1) * 2 + ((ep & 0x80) ? 1 : 0)); return (u16)(9 + 8 * hueco); }
int usbdc_ep_configurar(u8 ep, u8 tipo, u16 tam)
{
    u8 n = ep & 15;
    u32 e;
    (void)tipo;                                  /* [inferido] el UDC no distingue bulk de interrupt en el registro (OMAP: bit ISO aparte) */
    if (n == 0) return tam == 64 || tam == 0 ? 0 : -1;   /* el EP0 es 0x3001 desde usbdc_iniciar */
    if (tam > 64 || (tam & 7)) return -1;
    e = irq_cerrar();
    ESC(SYSCON1, LEE(SYSCON1) & ((u16)~S1_CFG_LOCK));           /* §3.5 */
    if (ep & 0x80) { ESC(EP_TX(n), tam ? (u16)(0x8000u | codigo_tam(tam) << 12 | puntero(ep)) : 0x0009); tam_in[n] = tam; }
    else           { ESC(EP_RX(n), tam ? (u16)(0x8000u | codigo_tam(tam) << 12 | puntero(ep)) : 0x0009); tam_out[n] = tam; }
    ESC(SYSCON1, LEE(SYSCON1) | (S1_CFG_LOCK));
    { u8 k; n_validos = 0; for (k = 1; k < 16; k++) n_validos = (u8)(n_validos + (tam_in[k] != 0) + (tam_out[k] != 0)); }   /* (AUD9 BAJO d) contados, no sumados */
    if (tam) cfg_pendiente = 1;
    if (!tam && !n_validos && (LEE(DEVSTAT) & D_CFG)) {   /* desconfigurar (§3.5 caso 1) */
        ESC(SYSCON2, S2_CLR_CFG);
        ESC(IRQ_EN, LEE(IRQ_EN) & ((u16)~0xB0u));
        cfg_pendiente = 0;
    }
    if (!e) irq_abrir();
    return 0;
}
/* §3.5 paso 5: DEV_CFG cuando ya están todos los endpoints, justo antes de la fase de estado de SET_CONFIGURATION (el
 * núcleo configura los endpoints y en seguida manda ese ZLP), y armar la recepción de los OUT. */
static void cerrar_configuracion(void)
{
    u8 n;
    if (!cfg_pendiente) return;
    cfg_pendiente = 0;
    /* (2026-10-20) Las IRQ de los EPn, SIEMPRE: el original (FUN_44217db8 caso 0) solo las enciende con DEVSTAT.CFG = 0, junto con
     * DEV_CFG; si el silicio ya diera CFG = 1 aquí (no se ha visto), se quedaban apagadas y nadie se enteraba de los paquetes bulk. */
    dg.n_cfg++; dg.cfg_devstat = LEE(DEVSTAT);
    ESC(IRQ_EN, LEE(IRQ_EN) | (I_EPN_TX | I_EPN_RX));
    if (!(dg.cfg_devstat & D_CFG)) ESC(SYSCON2, S2_DEV_CFG);
    dg.cfg_irq_en = LEE(IRQ_EN);
    /* Cada endpoint: RESET_EP (vacía y deja el conmutador en DATA0, como pide USB tras SET_CONFIGURATION, SET_INTERFACE o el
     * Device Reset de MTP [inferido de OMAP; el original usa RESET_EP igual al quitar un halt, §5.3]) y los OUT, armados. */
    for (n = 1; n < 16; n++) {
        if (tam_in[n]) { ESC(EP_NUM, N_SEL | N_DIR | n); ESC(CTRL, C_RESET_EP); ESC(EP_NUM, N_DIR | n); }
        if (tam_out[n]) { ESC(EP_NUM, N_SEL | n); ESC(CTRL, C_RESET_EP); ESC(EP_NUM, n); ESC(CTRL, C_FIFO_EN); rx_armado |= (u16)(1u << n); }
    }
    tx_vuelo = 0; rx_visto = 0;
}
static int fifo_vale(void)                       /* §2.1: el FIFO no se toca en un reset del bus ni sin VBUS */
{
    u16 d = LEE(DEVSTAT);
    return !en_reset && !(d & D_RESET) && (d & D_ATT);
}
int usbdc_ep_escribir(u8 ep, const u8 *p, u32 n)
{
    u8 num = ep & 15;
    u32 e, i;
    if (!iniciado || !(ep & 0x80) || n > (num ? tam_in[num] : 64u) || (num && !tam_in[num])) { udc_cta.errores++; return -1; }
    e = irq_cerrar();
    if (num == 0) cerrar_configuracion();
    if (!fifo_vale()) { if (!e) irq_abrir(); return -1; }
    ESC(EP_NUM, (u16)(N_SEL | N_DIR | num));                     /* §5.1 */
    { u16 st = LEE(STAT_FLG);
      if (num) dg.st_tx = st;
      if (st & S_FIFO_EN) { ESC(EP_NUM, (u16)(N_DIR | num)); dg.ocupados[EPI(num)]++; if (!e) irq_abrir(); return -1; } }   /* aún hay uno sin salir */
    if (num) { for (i = 0; i < 8; i++) dg.tx_cab[i] = i < n ? p[i] : 0; }
    dg.escritos[EPI(num)]++;
    for (i = 0; i + 1 < n; i += 2) ESC(DATA, (u16)(p[i] | p[i + 1] << 8));
    if (i < n) ESC8(DATA, p[i]);                                   /* el byte suelto, de 8 bits, como el original */
    ESC(EP_NUM, (u16)(N_DIR | num));
    ESC(CTRL, C_FIFO_EN);                                        /* listo para el próximo IN del host (n = 0: ZLP) */
    if (num) tx_vuelo |= (u16)(1u << num);
    udc_cta.tx++;
    if (!e) irq_abrir();
    return (int)n;
}
int usbdc_ep_leer(u8 ep, u8 *p, u32 max)
{
    u8 num = ep & 15;
    u32 e, n, i;
    u16 st;
    if (ep == UDC_EP_SETUP) {                    /* (AUD9 U1) SOLO el SETUP: nunca el paquete de datos del EP0 */
        if (max < 8) return -1;
        e = irq_cerrar();
        if (!setup_hay) { if (!e) irq_abrir(); return -1; }
        for (i = 0; i < 8; i++) p[i] = setup_d[i];
        setup_hay = 0;
        if (!e) irq_abrir();
        return 8;
    }
    if (!iniciado || (num && !tam_out[num])) return -1;
    e = irq_cerrar();
    if (!num) {                                  /* (AUD9) la marca se mira y se borra con las IRQ cortadas */
        if (!ep0_rx_pend) { if (!e) irq_abrir(); return -1; }
        ep0_rx_pend = 0;
    }
    if (!fifo_vale()) { if (!e) irq_abrir(); return -1; }
    ESC(EP_NUM, (u16)(N_SEL | num));                             /* §5.2 */
    st = LEE(STAT_FLG);
    if (num) dg.st_rx = st;
    /* nada recibido: FIFO_EN aún a 1, o sin ACK. (2026-10-20) Salvo que la IRQ (o el sondeo) dijera que llegó uno a ESTE EP: entonces
     * se toma igual y se cuenta (sin_ack): si el silicio borrara el ACK por algo, el EP no se quedaría en NAK para siempre. */
    if ((st & S_FIFO_EN) || (!(st & S_ACK) && !(num && (rx_visto & (1u << num))))) { ESC(EP_NUM, num); dg.vacios[EPI(num)]++; if (!e) irq_abrir(); return -1; }
    if (!(st & S_ACK)) dg.sin_ack++;
    if (num) rx_visto &= (u16)~(1u << num);
    n = LEE(RXFSTAT) & 0x3FFu;
    ESC(EP_NUM, num);                                            /* FUN_44218b1c: deselecciona tras STAT_FLG y RXFSTAT */
    /* (2026-10-20, EL MTP MUDO DEL APARATO) y FUN_44218c50 VUELVE A SELECCIONAR (FUN_44218608: EP_NUM = ep | EP_SEL) ANTES de leer el
     * FIFO (FUN_44219278, ldrb de +08) y deselecciona después (FUN_44218648); omap_udc.c (read_fifo) también lee con EP_SEL. El driver
     * viejo leía con EP_SEL = 0 (docs/usb.md §5.2 paso 3 lo contaba mal): el modelo de QEMU lo dejaba pasar, el aparato no. El modo 1
     * (tecla de «Diagnóstico USB») repite lo viejo, para comparar en el aparato. */
    if (!dg.modo) ESC(EP_NUM, (u16)(N_SEL | num));
    if (n > max) { udc_cta.errores++; }
    for (i = 0; i < n; i++) { u8 b = LEE8(DATA); if (i < max) p[i] = b; }
    ESC(EP_NUM, num);
    dg.leidos[EPI(num)]++;
    if (num) { dg.rx_n = (u16)n; for (i = 0; i < 8; i++) dg.rx_cab[i] = i < n && i < max ? p[i] : 0; }
    if (num) {
        ESC(EP_NUM, (u16)(N_SEL | num)); ESC(CTRL, C_CLR_EP); ESC(EP_NUM, num);   /* §5.2 paso 4 */
        ESC(CTRL, C_FIFO_EN);                                    /* rearmar: el siguiente paquete */
        rx_armado |= (u16)(1u << num);
    } else if (n == 64) {
        ESC(CTRL, C_FIFO_EN);                                    /* el EP0 solo se rearma si la fase de datos puede seguir */
    }
    udc_cta.rx++;
    if (!e) irq_abrir();
    return (int)(n > max ? max : n);
}
void usbdc_ep_stall(u8 ep, int si)
{
    u8 num = ep & 15;
    u32 e = irq_cerrar();
    if (num == 0) {                                               /* el núcleo para los dos sentidos del EP0: basta una orden */
        if (si && (ep & 0x80)) { ESC(SYSCON2, S2_STALL); udc_cta.stalls++; }   /* §4.3; el siguiente SETUP lo quita */
    } else if (si) {                                              /* §5.3 poner */
        ESC(EP_NUM, (u16)(N_SEL | (ep & 0x80 ? N_DIR : 0) | num));
        ESC(CTRL, C_RESET_EP);
        ESC(EP_NUM, (u16)((ep & 0x80 ? N_DIR : 0) | num));
        ESC(CTRL, C_SET_HALT);
        if (ep & 0x80) { if (tx_vuelo & (1u << num)) epn_tx |= (u16)(1u << num); tx_vuelo &= (u16)~(1u << num); }   /* el que estaba en el aire */
        else { rx_armado &= (u16)~(1u << num); rx_visto &= (u16)~(1u << num); }                 /* ya no saldrá: que el núcleo no lo espere */
        udc_cta.stalls++;
    } else {                                                      /* §5.3 quitar. [inferido de OMAP] RESET_EP vuelve el conmutador a DATA0 */
        ESC(EP_NUM, (u16)(N_SEL | (ep & 0x80 ? N_DIR : 0) | num));
        ESC(CTRL, C_RESET_EP);
        (void)LEE(STAT_FLG);
        ESC(EP_NUM, (u16)((ep & 0x80 ? N_DIR : 0) | num));
        ESC(CTRL, C_CLR_HALT);                                   /* [inferido de OMAP]: el original no lo nombra para EPn; RESET_EP podría bastar */
        if (!(ep & 0x80)) { ESC(CTRL, C_FIFO_EN); rx_armado |= (u16)(1u << num); rx_visto &= (u16)~(1u << num); }   /* y vuelve a armar la recepción */
        else { if (tx_vuelo & (1u << num)) epn_tx |= (u16)(1u << num); tx_vuelo &= (u16)~(1u << num); }
    }
    if (!e) irq_abrir();
}
/* (2026-10-20) el sondeo de los EPn en el aire (ver tx_vuelo): con las IRQ cortadas, nada en la cola, y el FIFO usable. Seleccionar y
 * deseleccionar para leer STAT_FLG es lo que hace el original (FUN_44218b1c) antes de tocar los datos. */
static void sondear_epn(void)
{
    u8 n;
    for (n = 1; n < 16; n++) {
        u16 b = (u16)(1u << n), st;
        if (tx_vuelo & b) {
            ESC(EP_NUM, (u16)(N_SEL | N_DIR | n)); st = LEE(STAT_FLG); ESC(EP_NUM, (u16)(N_DIR | n));
            if (!(st & S_FIFO_EN)) { tx_vuelo &= (u16)~b; epn_tx |= b; dg.sondeo_tx++; }
        }
        if (rx_armado & b) {
            ESC(EP_NUM, (u16)(N_SEL | n)); st = LEE(STAT_FLG); ESC(EP_NUM, n);
            if (!(st & S_FIFO_EN)) { rx_armado &= (u16)~b; rx_visto |= b; epn_rx |= b; dg.sondeo_rx++; }
        }
    }
}
int usbdc_evento(UdcEvento *ev, u8 *ep)
{
    u32 cerr;
    int n;
    if (c_saca == c_pon && !epn_tx && !epn_rx && iniciado) {   /* la red de seguridad de la IRQ por flanco: algo pendiente y habilitado que nadie atendió */
        u32 e = irq_cerrar();
        u16 en = LEE(IRQ_EN), m = (u16)(((en & 1) ? 0x07 : 0) | (en & 0xB8));
        if (LEE(IRQ_SRC) & m) { dg.red++; usb_irq(); }
        if (!epn_irq_visto && c_saca == c_pon && !epn_tx && !epn_rx && (tx_vuelo || rx_armado) && fifo_vale()) sondear_epn();   /* solo si las IRQ de los EPn no dan señales */
        if (!e) irq_abrir();
    }
    cerr = irq_cerrar();                         /* (AUD9) se saca con las IRQ cortadas: la IRQ puede retocar la casilla del SETUP */
    while (c_saca != c_pon) {
        u32 i = c_saca % COLA;
        u8 e = cola[i].ev, p = cola[i].ep, g = cola[i].gen;
        if (e == UDC_EV_SETUP && setup_en_cola && setup_casilla == c_saca) setup_en_cola = 0;
        c_saca++;
        /* del control anterior (o de antes de un reset): EP0_RX/EP0_TX y también el SETUP (AUD9 U1) */
        if ((e == UDC_EV_RX || e == UDC_EV_TX_HECHO || e == UDC_EV_SETUP) && (p & 15) == 0 && g != ep0_gen) continue;
        *ev = (UdcEvento)e; *ep = p;
        if (!cerr) irq_abrir();
        return 1;
    }
    for (n = 1; n < 16; n++) {                   /* los de los endpoints 1..15, de los bits */
        if (epn_tx & (1u << n)) { epn_tx &= (u16)~(1u << n); *ev = UDC_EV_TX_HECHO; *ep = (u8)(0x80 | n); if (!cerr) irq_abrir(); return 1; }
        if (epn_rx & (1u << n)) { epn_rx &= (u16)~(1u << n); *ev = UDC_EV_RX; *ep = (u8)n; if (!cerr) irq_abrir(); return 1; }
    }
    if (!cerr) irq_abrir();
    return 0;
}
void usbdc_parar(void)                           /* §3.3; la puerta 26 y el bloque 20 se quedan como están (§1) */
{
    u32 e = irq_cerrar();
    if (encendido) {
        ESC(IRQ_EN, 0);
        ESC(SYSCON1, LEE(SYSCON1) & ((u16)~S1_PULLUP));
        if (LEE(DEVSTAT) & D_CFG) ESC(SYSCON2, S2_CLR_CFG);
        vaciar_fuentes();
    }
    intc_tapar(62); intc_tapar(63);
    iniciado = 0; quiere_pullup = 0; setup_hay = 0; c_pon = c_saca = 0; setup_en_cola = 0; epn_tx = epn_rx = 0; tx_vuelo = rx_armado = rx_visto = 0;
    if (!e) irq_abrir();
}

/* (2026-10-20) La foto para «Diagnóstico USB». Los registros vivos, solo con los relojes del bloque puestos (si no, leer cuelga) y
 * sin seleccionar ningún EP (seleccionar/deseleccionar podría borrar el ACK de un paquete aún sin leer): STAT_FLG va de lo último
 * que leyó el propio driver. */
void usbdc_diag(UdcDiag *d)
{
    u32 e = irq_cerrar();
    dg.vivo = encendido;
    if (encendido) {
        dg.devstat = LEE(DEVSTAT); dg.syscon1 = LEE(SYSCON1); dg.irq_en = LEE(IRQ_EN); dg.irq_src = LEE(IRQ_SRC);
        dg.epn_stat = LEE(EPN_STAT); dg.ep_num = LEE(EP_NUM);
        dg.cfg_ep0 = LEE(EP0_CFG); dg.cfg_rx2 = LEE(EP_RX(2)); dg.cfg_tx2 = LEE(EP_TX(2)); dg.cfg_tx1 = LEE(EP_TX(1));
    }
    *d = dg;
    if (!e) irq_abrir();
}
void usbdc_diag_modo(int modo) { dg.modo = (u8)(modo ? 1 : 0); }
