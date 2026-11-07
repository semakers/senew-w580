/* El núcleo USB del sistema (pista USBD): capítulo 9 sobre el EP0, los descriptores de UNA interfaz MTP (clase 6/1/1, la de
 * «Still Image Capture» que usa MTP: bulk IN, bulk OUT e interrupt IN), los descriptores «MS OS 1.0» con los que Windows le pone
 * WinUSB sin .inf (no su MTP: ver desc_msos_compat), las peticiones de clase de PTP/MTP (Cancel, Device Reset, Get Device Status) y
 * el «tubo».
 * Contrato: docs/usb_contrato.md §2. Qué está probado y qué falta: docs/usb_driver.md.
 *
 * Fuentes PÚBLICAS de lo que hay aquí (nada sale del firmware original, que usa la pila MCCI y no MTP):
 *  - USB 2.0, capítulo 9 (peticiones estándar, descriptores, fases del control);
 *  - «Universal Serial Bus Still Image Capture Device Definition» 1.0 (2000), §5 (peticiones de clase 0x64..0x67);
 *  - «Media Transfer Protocol» 1.1, Apéndice D (MTP sobre USB: contenedores y paquetes cortos / ZLP);
 *  - Microsoft, «Microsoft OS 1.0 Descriptors Specification» (cadena 0xEE «MSFT100», Extended Compat ID «WINUSB» y Extended
 *    Properties «DeviceInterfaceGUID»), y «WinUSB Device» (WinUSB sin .inf desde Windows 8).
 * Se compila igual en el teléfono (sobre hal/usb_dc.c) y en el PC (sobre herr/pruebas/usb_dc_falso.c). */
#include <string.h>
#include "usb.h"
#ifdef PUENTE
#include "puente.h"                 /* (v16) el puente de depuración: peticiones de vendedor por el EP0 (docs/puente.md) */
#endif
#include "../hal/usb_dc.h"           /* la interfaz del driver vive en hal/; el PC la cumple con un falso (plat/usb_dc_pc.c, herr/pruebas/usb_dc_falso.c) */

/* ------------------------------------------------------------------------------------------------ descriptores */
#define LE16(x) (u8)((x) & 0xFF), (u8)((x) >> 8)
#define LE32(x) LE16(x), LE16((x) >> 16)
static const u8 desc_dispositivo[18] = {
    18, 1, LE16(0x0200),            /* USB 2.0 (velocidad completa: sin BOS ni calificador) */
    0, 0, 0,                        /* la clase va en la interfaz */
    USB_TAM_EP0, LE16(USB_VID), LE16(USB_PID), LE16(0x0030),   /* bcdDevice: Windows guarda lo que leyó de los MS OS por
                                     * VID/PID/bcdDevice (usbflags) y no vuelve a preguntar. Pasó de 0x0019 a 0x0030 (senew 0.30.3)
                                     * con la compat ID «WINUSB»; súbelo cada vez que cambien los descriptores MS OS */
    1, 2, 3,                        /* fabricante, producto, número de serie */
    1                               /* una configuración */
};
#define CONF_LARGO (9 + 9 + 7 + 7 + 7)
static const u8 desc_configuracion[CONF_LARGO] = {
    9, 2, LE16(CONF_LARGO), 1, 1, 0,
    0x80, 0xFA,                     /* (CARGA, 25-oct-2026) alimentado por el bus, 500 mA: como el original (parchea su descriptor con la unidad
                                     * 0x05E6 = FA de su GDFS: work/b/usb_carga_RE.md §3). Con la configuración puesta y el bus despierto, el PC
                                     * concede los 500 mA y api/carga.h carga la batería (antes: autoalimentado, 2 mA, sin cargar) */
    9, 4, 0, 0, 3, 6, 1, 1, 4,      /* interfaz 0, 3 endpoints, clase 6 (imagen) / 1 / 1 (PTP: MTP), cadena 4 = «MTP» */
    7, 5, USB_EP_BULK_IN,  UDC_EP_BULK, LE16(USB_TAM_BULK), 0,
    7, 5, USB_EP_BULK_OUT, UDC_EP_BULK, LE16(USB_TAM_BULK), 0,
    7, 5, USB_EP_INT_IN,   UDC_EP_INT,  LE16(USB_TAM_INT),  16    /* cada 16 ms, como la interfaz PTP del original */
};
static const char *const cadenas[] = { 0, USB_TEXTO_MODELO " homebrew", USB_TEXTO_MODELO " sistema propio", USB_TEXTO_MODELO "S10000001", "MTP" };
#define CADENA_SERIE 3
static char serie_propia[40];       /* (Fase 3b) con un id vinculado, la serie derivada: el contrato del canal §7. Vacía: la fija de `cadenas` */
void usb_poner_serie(const char *s)
{
    u32 i;
    for (i = 0; s[i] && i + 1 < sizeof serie_propia; i++) serie_propia[i] = s[i];
    serie_propia[i] = 0;
}
/* MS OS 1.0: la cadena 0xEE y el «Extended Compat ID» (wIndex = 4) que dice «a esta interfaz, WinUSB». Windows 8 y posteriores la
 * ponen solos, sin .inf ni permisos, y SENUS la abre con libusb igual que en la Mac. (6-nov-2026) Antes decía «MTP», y medido en
 * Windows 10: su MTP abre sesión nada más enumerar, antes del «Sí» del teléfono; recibe Device_Busy y se rinde (código 10, sin
 * reintentar), y aunque arrancara, libusb no puede abrir un aparato con ese driver. Lo decidió el mantenedor: en Windows el teléfono no
 * sale en el Explorador; todo va por SENUS. macOS y Linux no leen estos descriptores. */
static const u8 desc_msos_cadena[18] = { 18, 3, 'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0, USB_VENDOR_MS, 0 };
static const u8 desc_msos_compat[40] = {
    LE16(40), 0, 0, LE16(0x0100), LE16(4), 1, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 'W', 'I', 'N', 'U', 'S', 'B', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
/* MS OS 1.0: «Extended Properties» (wIndex = 5), una sola: DeviceInterfaceGUID (REG_SZ en UTF-16), la puerta por la que un programa
 * encuentra el aparato con la API de WinUSB. El GUID es el de senew en todos los teléfonos: NO cambiarlo. */
#define MSOS_PROPS_LARGO (10 + 14 + 40 + 78)
static const u8 desc_msos_props[MSOS_PROPS_LARGO] = {
    LE32(MSOS_PROPS_LARGO), LE16(0x0100), LE16(5), LE16(1),
    LE32(14 + 40 + 78), LE32(1), LE16(40),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 0, 0,
    LE32(78),
    '{', 0, '7', 0, '5', 0, 'E', 0, '2', 0, '6', 0, '8', 0, '4', 0, 'D', 0, '-', 0, '1', 0, '4', 0, '0', 0, '4', 0, '-', 0,
    '4', 0, '9', 0, '3', 0, '5', 0, '-', 0, '9', 0, '4', 0, '9', 0, '8', 0, '-', 0,
    'B', 0, '7', 0, '9', 0, '1', 0, '4', 0, '3', 0, 'A', 0, 'F', 0, 'B', 0, 'A', 0, '8', 0, '5', 0, '}', 0, 0, 0
};

/* ------------------------------------------------------------------------------------------------ estado */
enum { F_LIBRE = 0, F_DATOS_IN, F_DATOS_OUT, F_ESTADO_IN, F_ESTADO_OUT };
static struct {
    u8 activo, estado, estado_antes, dir, dir_pend, config;
    u8 fase, ep0_ocupado, ep0_zlp;          /* fase del control en curso */
    u8 setup[8];
    const u8 *in_p; u16 in_n, in_pos;       /* fase de datos IN del EP0 */
    u8 out_buf[64]; u16 out_n, out_esperado;/* fase de datos OUT del EP0 (solo peticiones de clase: Cancel lleva 6 bytes) */
#ifdef PUENTE
    u8 *out_dst; u8 vend;                   /* (v16) la fase de datos OUT es de una petición de vendedor del puente: se recibe en out_dst (no en out_buf) */
#endif
    u8 ctl[256];                            /* donde se arman las respuestas que no son constantes */
    u8 halt_in, halt_out, halt_int;
    /* el tubo */
    u8 in_ocupado, zlp_pend, int_ocupado, rx_pend;
    u32 cont_resta;                         /* bytes que faltan del contenedor IN en curso (0 = hay que leer otra cabecera) */
    u8 aviso;
    u32 ultimo_vbus; u8 vbus_visto;
    UsbInfo cta;
} u;

#define TX_TAM 4096u                        /* potencias de 2 */
#define RX_TAM 1024u
static u8 tx_buf[TX_TAM], rx_buf[RX_TAM];
static u32 tx_ini, tx_cnt, rx_ini, rx_cnt;

static void tubo_vaciar(void) { tx_ini = tx_cnt = rx_ini = rx_cnt = 0; u.cont_resta = 0; u.zlp_pend = 0; }
/* (AUD9 BAJO e) Los avisos son BITS, no una casilla: un Device Reset seguido de un Cancel antes de que el respondedor mire no
 * se pierde. usb_tubo_aviso() da el más fuerte (BUS > RESET_MTP > CANCELAR) y borra ese y los más débiles, que ya van dentro. */
static void avisar(u8 a) { u.aviso |= (u8)(1u << a); u.cta.avisos++; }

/* ------------------------------------------------------------------------------------------------ EP0 */
static void ep0_stall(void)
{
    usbdc_ep_stall(0x80, 1); usbdc_ep_stall(0x00, 1);
    u.fase = F_LIBRE; u.cta.stalls++;
}
static void ep0_bombear(void)                /* el siguiente paquete de la fase de datos IN */
{
    u32 n;
    if (u.fase != F_DATOS_IN || u.ep0_ocupado) return;
    n = u.in_n - u.in_pos;
    if (n == 0 && !u.ep0_zlp) { u.fase = F_ESTADO_OUT; return; }
    if (n > USB_TAM_EP0) n = USB_TAM_EP0;
    if (usbdc_ep_escribir(0x80, u.in_p + u.in_pos, n) < 0) return;
    u.ep0_ocupado = 1;
    u.in_pos += n;
    if (n == 0) u.ep0_zlp = 0;               /* el ZLP que cierra una respuesta múltiplo de 64 más corta que wLength */
}
static void responder(const u8 *p, u32 n)
{
    u32 pedido = u.setup[6] | u.setup[7] << 8;
    if (n > pedido) n = pedido;
    u.in_p = p; u.in_n = (u16)n; u.in_pos = 0;
    u.ep0_zlp = (n < pedido && (n % USB_TAM_EP0) == 0 && n > 0);
    if (n == 0) u.ep0_zlp = 1;               /* fase de datos vacía: un ZLP */
    u.fase = F_DATOS_IN;
    ep0_bombear();
}
static void estado_in(void)                  /* la fase de estado de una petición sin datos (o de datos OUT): un ZLP */
{
    u.fase = F_ESTADO_IN;
    if (!u.ep0_ocupado && usbdc_ep_escribir(0x80, 0, 0) >= 0) u.ep0_ocupado = 1;
}
static u32 cadena_usb(u8 *d, const char *s)
{
    u32 n = 2;
    while (*s && n < 254) { d[n++] = (u8)*s++; d[n++] = 0; }
    d[0] = (u8)n; d[1] = 3;
    return n;
}

static void configurar_eps(int si)
{
    usbdc_ep_configurar(USB_EP_BULK_IN,  UDC_EP_BULK, si ? USB_TAM_BULK : 0);
    usbdc_ep_configurar(USB_EP_BULK_OUT, UDC_EP_BULK, si ? USB_TAM_BULK : 0);
    usbdc_ep_configurar(USB_EP_INT_IN,   UDC_EP_INT,  si ? USB_TAM_INT : 0);
    u.halt_in = u.halt_out = u.halt_int = 0;
    u.in_ocupado = u.int_ocupado = u.rx_pend = 0;
    tubo_vaciar();
}
static u8 *halt_de(u8 ep)
{
    if (!u.config) return 0;
    if (ep == USB_EP_BULK_IN) return &u.halt_in;
    if (ep == USB_EP_BULK_OUT) return &u.halt_out;
    if (ep == USB_EP_INT_IN) return &u.halt_int;
    return 0;
}

static void peticion_estandar(u8 tipo, u8 pet, u16 valor, u16 indice)
{
    u8 dest = tipo & 0x1F;
    switch (pet) {
    case 0x06:                                                      /* GET_DESCRIPTOR */
        if (!(tipo & 0x80)) break;
        switch (valor >> 8) {
        case 1: responder(desc_dispositivo, sizeof desc_dispositivo); return;
        case 2: if ((valor & 0xFF) != 0) break; responder(desc_configuracion, sizeof desc_configuracion); return;
        case 3: {
            u8 i = valor & 0xFF;
            if (i == 0) { static const u8 idiomas[4] = { 4, 3, LE16(0x0409) }; responder(idiomas, 4); return; }
            if (i == 0xEE) { responder(desc_msos_cadena, sizeof desc_msos_cadena); return; }
            if (i == CADENA_SERIE && serie_propia[0]) { responder(u.ctl, cadena_usb(u.ctl, serie_propia)); return; }
            if (i < sizeof cadenas / sizeof cadenas[0]) { responder(u.ctl, cadena_usb(u.ctl, cadenas[i])); return; }  /* cualquier idioma: el de 0x0409 */
            break;
        }
        default: break;                                             /* 6 (calificador), 7 (otra velocidad), 0x0F (BOS): STALL, como pide §9.6.2 a un dispositivo solo de velocidad completa */
        }
        break;
    case 0x05:                                                      /* SET_ADDRESS: se aplica tras la fase de estado (§9.4.6) */
        if (tipo != 0x00 || valor > 127 || u.estado == USB_EST_CONFIGURADO) break;
        u.dir_pend = (u8)valor | 0x80;
        estado_in();
        return;
    case 0x09:                                                      /* SET_CONFIGURATION */
        /* sin exigir el estado «con dirección»: en el UDC del DB2020 SET_ADDRESS lo resuelve el HARDWARE y no llega aquí (docs/usb.md §0) */
        if (tipo != 0x00 || (valor & 0xFF) > 1) break;
        if (u.config) avisar(USB_AVISO_BUS);                       /* (AUD9 BAJO b) también SET_CONFIGURATION(1) repetido: vacía el tubo */
        u.config = valor & 0xFF;
        configurar_eps(u.config);
        u.estado = u.config ? USB_EST_CONFIGURADO : USB_EST_DIRECCION;
        u.cta.configuracion = u.config;
        estado_in();
        return;
    case 0x08:                                                      /* GET_CONFIGURATION */
        if (tipo != 0x80) break;
        u.ctl[0] = u.config; responder(u.ctl, 1); return;
    case 0x0A:                                                      /* GET_INTERFACE */
        if (tipo != 0x81 || !u.config || indice != 0) break;
        u.ctl[0] = 0; responder(u.ctl, 1); return;
    case 0x0B:                                                      /* SET_INTERFACE: solo hay el ajuste 0 */
        if (tipo != 0x01 || !u.config || indice != 0 || valor != 0) break;
        avisar(USB_AVISO_CANCELAR);                                 /* (AUD9 BAJO b) el tubo se vacía: la transacción en curso se acaba */
        configurar_eps(1);                                          /* §9.1.1.5: reponer conmutadores y quitar halts */
        estado_in(); return;
    case 0x00:                                                      /* GET_STATUS */
        if (!(tipo & 0x80)) break;
        u.ctl[0] = u.ctl[1] = 0;
        if (dest == 0) u.ctl[0] = 0;                                /* (CARGA) alimentado por el bus (bit 0 = 0); sin despertar remoto */
        else if (dest == 1) { if (!u.config || indice != 0) break; }
        else if (dest == 2) {
            u8 *h = indice > 0xFF ? 0 : halt_de((u8)indice);       /* (AUD9) wIndex entero, sin truncar a u8 */
            if (indice == 0 || indice == 0x80) ;
            else if (!h) break;
            else u.ctl[0] = *h;
        } else break;
        responder(u.ctl, 2); return;
    case 0x01: case 0x03: {                                         /* CLEAR_FEATURE / SET_FEATURE */
        int si = pet == 0x03;
        if (dest == 2 && valor == 0) {                              /* ENDPOINT_HALT */
            u8 *h = indice > 0xFF ? 0 : halt_de((u8)indice);
            if (indice == 0 || indice == 0x80) { estado_in(); return; }   /* el EP0 no se para así */
            if (!h) break;
            *h = (u8)si;
            usbdc_ep_stall((u8)indice, si);                         /* quitarlo pone además el conmutador a DATA0 (usb_dc.h) */
            if (!si && (u8)indice == USB_EP_BULK_IN) {
                /* (AUD9 BAJO c) RESET_EP vacía el FIFO: el paquete en vuelo se ha perdido. Si había un contenedor a medias, ya no se
                 * puede terminar bien: se tira el resto y se avisa como un Cancel (el respondedor acaba la transacción). */
                /* (Si no había nada a medias —nada en vuelo y ningún contenedor empezado— lo del tubo sigue entero y se queda.) */
                if (u.in_ocupado || u.cont_resta || u.zlp_pend) { tx_ini = tx_cnt = 0; u.cont_resta = 0; u.zlp_pend = 0; avisar(USB_AVISO_CANCELAR); }
                u.in_ocupado = 0;
            }
            estado_in(); return;
        }
        if (dest == 0 && valor == 1) { estado_in(); return; }      /* DEVICE_REMOTE_WAKEUP: no lo anunciamos; se acepta y no hace nada */
        break;                                                      /* TEST_MODE (2) no existe a velocidad completa */
    }
    default: break;
    }
    ep0_stall();
}

/* Peticiones de clase de «Still Image Capture» §5.2 (las de MTP sobre USB). */
static void peticion_clase(u8 tipo, u8 pet, u16 valor, u16 indice, u16 largo)
{
    (void)valor;
    if ((tipo & 0x1F) != 1 || indice != 0 || !u.config) { ep0_stall(); return; }
    switch (pet) {
    case 0x64:                                                      /* Cancel Request: 6 bytes de datos (0x4001 + id de transacción) */
        if (tipo != 0x21 || largo != 6) break;
        u.fase = F_DATOS_OUT; u.out_n = 0; u.out_esperado = 6;
        return;
    case 0x66:                                                      /* Device Reset Request */
        if (tipo != 0x21 || largo != 0) break;
        configurar_eps(1);
        avisar(USB_AVISO_RESET_MTP);
        estado_in(); return;
    case 0x67: {                                                    /* Get Device Status: wLength, código, y los endpoints parados */
        u32 n = 4;
        if (tipo != 0xA1) break;
        u.ctl[2] = 0x01; u.ctl[3] = 0x20;                           /* 0x2001 OK */
        if (u.aviso) { u.ctl[2] = 0x19; u.ctl[3] = 0x20; }          /* 0x2019 Device_Busy: el respondedor aún no se ha enterado */
        if (u.halt_in)  { u.ctl[n] = USB_EP_BULK_IN;  u.ctl[n + 1] = u.ctl[n + 2] = u.ctl[n + 3] = 0; n += 4; }
        if (u.halt_out) { u.ctl[n] = USB_EP_BULK_OUT; u.ctl[n + 1] = u.ctl[n + 2] = u.ctl[n + 3] = 0; n += 4; }
        if (n > 4) { u.ctl[2] = 0x1F; u.ctl[3] = 0x20; }            /* 0x201F Transaction_Cancelled con los endpoints parados (Still Image §5.2.4) */
        u.ctl[0] = (u8)n; u.ctl[1] = 0;
        responder(u.ctl, n); return;
    }
    default: break;                                                 /* 0x65 Get Extended Event Data: opcional; STALL */
    }
    ep0_stall();
}
static void fin_datos_out(void)
{
#ifdef PUENTE
    if (u.vend) {                                                   /* (v16) acabó la fase de datos de una petición del puente */
        u.vend = 0;
        if (puente_ep0_fin(u.setup, u.out_dst, u.out_n) < 0) ep0_stall(); else estado_in();
        return;
    }
#endif
    if (u.setup[1] == 0x64 && u.out_n == 6 && u.out_buf[0] == 0x01 && u.out_buf[1] == 0x40) {
        tubo_vaciar();
        avisar(USB_AVISO_CANCELAR);
        estado_in();
    } else ep0_stall();
}

static void peticion_vendedor(u8 tipo, u8 pet, u16 valor, u16 indice)
{
    (void)valor;
#ifdef PUENTE
    {   /* (v16) las peticiones del puente (0xB0..0xB5): IN = devolver lo que diga; OUT = recibir la fase de datos donde diga */
        const u8 *p = 0; u32 n = 0;
        int r = puente_ep0(u.setup, &p, &n);
        if (r == PU_USB_IN) { responder(p, n); return; }
        if (r == PU_USB_OUT) { u.fase = F_DATOS_OUT; u.out_n = 0; u.out_esperado = (u16)n; u.out_dst = (u8 *)(void *)p; u.vend = 1; return; }
        if (r == PU_USB_LISTO) { if (puente_ep0_fin(u.setup, 0, 0) < 0) ep0_stall(); else estado_in(); return; }
        if (r == PU_USB_STALL) { ep0_stall(); return; }
    }
#endif
    /* MS OS 1.0: Extended Compat ID. Windows la pide como dispositivo (0xC0); algunas versiones, como interfaz (0xC1). */
    if (pet == USB_VENDOR_MS && (tipo == 0xC0 || tipo == 0xC1) && indice == 4) { responder(desc_msos_compat, sizeof desc_msos_compat); return; }
    /* MS OS 1.0: Extended Properties. Windows la pide como interfaz (0xC1, wValue = la interfaz 0); se acepta también como dispositivo. */
    if (pet == USB_VENDOR_MS && (tipo == 0xC0 || tipo == 0xC1) && indice == 5) { responder(desc_msos_props, sizeof desc_msos_props); return; }
    ep0_stall();
}

static void setup_llega(void)
{
    u8 tipo; u16 valor, indice, largo;
    if (usbdc_ep_leer(UDC_EP_SETUP, u.setup, 8) != 8) { u.cta.stalls++; return; }   /* (AUD9 U1) solo el SETUP, nunca los datos del EP0 */
    u.cta.setups++;
    u.fase = F_LIBRE; u.ep0_ocupado = 0; u.ep0_zlp = 0;            /* un SETUP anula el control anterior (usb_dc.h) */
    tipo = u.setup[0];
    valor = u.setup[2] | u.setup[3] << 8; indice = u.setup[4] | u.setup[5] << 8; largo = u.setup[6] | u.setup[7] << 8;
#ifdef PUENTE
    u.vend = 0;                                                    /* (v16) un SETUP nuevo anula una recepción del puente a medias */
#endif
    if (!(tipo & 0x80) && largo > sizeof u.out_buf && (tipo & 0x60) != 0x00
#ifdef PUENTE
        && (tipo & 0x60) != 0x40                                   /* (v16) el puente recibe hasta PU_BUF bytes en una petición de vendedor */
#endif
       ) { ep0_stall(); return; }
    switch (tipo & 0x60) {
    case 0x00: if (!(tipo & 0x80) && largo) ep0_stall(); else peticion_estandar(tipo, u.setup[1], valor, indice); break;
    case 0x20: peticion_clase(tipo, u.setup[1], valor, indice, largo); break;
    case 0x40: peticion_vendedor(tipo, u.setup[1], valor, indice); break;
    default: ep0_stall(); break;
    }
}
static void ep0_rx(void)
{
    u8 b[64];
    int n = usbdc_ep_leer(0, b, sizeof b);
    if (n < 0) return;
    if (u.fase == F_DATOS_OUT) {
        if ((u32)n > (u32)(u.out_esperado - u.out_n)) { ep0_stall(); return; }
#ifdef PUENTE
        memcpy((u.vend ? u.out_dst : u.out_buf) + u.out_n, b, (u32)n); u.out_n += (u16)n;   /* (v16) el puente recibe en su búfer */
#else
        memcpy(u.out_buf + u.out_n, b, (u32)n); u.out_n += (u16)n;
#endif
        if (u.out_n == u.out_esperado || n < USB_TAM_EP0) fin_datos_out();
        return;
    }
    /* la fase de estado OUT de una lectura (el host puede cortarla antes de tiempo: también vale) */
    if (u.fase == F_ESTADO_OUT || u.fase == F_DATOS_IN) { u.fase = F_LIBRE; return; }
}
static void ep0_tx(void)
{
    u.ep0_ocupado = 0;
    if (u.fase == F_DATOS_IN) { ep0_bombear(); return; }
    if (u.fase == F_ESTADO_IN) {
        u.fase = F_LIBRE;
        if (u.dir_pend) {
            u.dir = u.dir_pend & 0x7F; u.dir_pend = 0;
            usbdc_direccion(u.dir);
            u.estado = u.dir ? USB_EST_DIRECCION : USB_EST_ESPERA;
            u.cta.direccion = u.dir;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ el tubo */
static void tx_bombear(void)
{
    u8 b[USB_TAM_BULK] = { 0 };
    u32 n, i;
    if (!u.config || u.in_ocupado || u.halt_in) return;
    if (u.zlp_pend) {
        if (usbdc_ep_escribir(USB_EP_BULK_IN, b, 0) < 0) return;
        u.zlp_pend = 0; u.in_ocupado = 1; return;
    }
    if (u.cont_resta == 0) {                                        /* la cabecera del contenedor siguiente */
        u32 L = 0;
        if (tx_cnt < 4) return;
        for (i = 0; i < 4; i++) L |= (u32)tx_buf[(tx_ini + i) & (TX_TAM - 1)] << (8 * i);
        if (L < 4) L = 4;                                           /* malformado (MTP nunca manda < 12): al menos no se atasca */
        u.cont_resta = L; u.cta.contenedores_in++;
    }
    n = u.cont_resta < USB_TAM_BULK ? u.cont_resta : USB_TAM_BULK;
    if (tx_cnt < n) return;                                         /* un paquete corto solo al final del contenedor */
    for (i = 0; i < n; i++) b[i] = tx_buf[(tx_ini + i) & (TX_TAM - 1)];
    if (usbdc_ep_escribir(USB_EP_BULK_IN, b, n) < 0) return;
    tx_ini = (tx_ini + n) & (TX_TAM - 1); tx_cnt -= n;
    u.in_ocupado = 1; u.cta.tx_bytes += n;
    u.cont_resta -= n;
    if (u.cont_resta == 0 && n == USB_TAM_BULK) u.zlp_pend = 1;   /* múltiplo de 64: el ZLP termina la transferencia */
}
static void rx_bombear(void)
{
    u8 b[USB_TAM_BULK];
    int n, i;
    if (!u.rx_pend || RX_TAM - rx_cnt < USB_TAM_BULK) return;       /* sin sitio: el endpoint sigue en NAK */
    n = usbdc_ep_leer(USB_EP_BULK_OUT, b, sizeof b);
    u.rx_pend = 0;
    if (n <= 0) return;                                             /* un ZLP no lleva datos: el respondedor se guía por la longitud del contenedor */
    for (i = 0; i < n; i++) rx_buf[(rx_ini + rx_cnt + (u32)i) & (RX_TAM - 1)] = b[i];
    rx_cnt += (u32)n; u.cta.rx_bytes += (u32)n;
}

int usb_tubo_leer(u8 *p, u32 max)
{
    u32 n = 0;
    if (!u.config) return 0;
    while (n < max && rx_cnt) { p[n++] = rx_buf[rx_ini]; rx_ini = (rx_ini + 1) & (RX_TAM - 1); rx_cnt--; }
    rx_bombear();
    while (n < max && rx_cnt) { p[n++] = rx_buf[rx_ini]; rx_ini = (rx_ini + 1) & (RX_TAM - 1); rx_cnt--; }
    return (int)n;
}
int usb_tubo_escribir(const u8 *p, u32 n)
{
    u32 i, libre;
    if (!u.config) return 0;
    libre = TX_TAM - tx_cnt;
    if (n > libre) n = libre;
    for (i = 0; i < n; i++) tx_buf[(tx_ini + tx_cnt + i) & (TX_TAM - 1)] = p[i];
    tx_cnt += n;
    tx_bombear();
    return (int)n;
}
int usb_tubo_libre(void) { return u.config ? (int)(TX_TAM - tx_cnt) : 0; }
int usb_tubo_evento(const u8 *p, u32 n)
{
    if (!u.config || u.int_ocupado || u.halt_int) return 0;
    if (n > USB_TAM_INT) n = USB_TAM_INT;                          /* un evento MTP cabe (12..24 bytes) */
    if (usbdc_ep_escribir(USB_EP_INT_IN, p, n) < 0) return 0;
    u.int_ocupado = 1;
    return (int)n;
}
int usb_tubo_aviso(void)
{
    int a;
    for (a = USB_AVISO_BUS; a >= USB_AVISO_CANCELAR; a--)
        if (u.aviso & (1u << a)) { u.aviso = 0; return a; }      /* a es el más fuerte que hay: se van él y los débiles */
    return USB_AVISO_NADA;
}

/* ------------------------------------------------------------------------------------------------ marcha */
static void reset_bus(void)
{
    if (u.config) avisar(USB_AVISO_BUS);
    u.config = 0; u.dir = 0; u.dir_pend = 0; u.fase = F_LIBRE; u.ep0_ocupado = 0;
    u.estado = USB_EST_ESPERA; u.cta.resets++; u.cta.direccion = 0; u.cta.configuracion = 0;
    configurar_eps(0);
}
int usb_iniciar(void)
{
    UsbInfo c = u.cta;
    int r;
    memset(&u, 0, sizeof u);
    u.cta = c;
    tubo_vaciar();
    r = usbdc_iniciar();
    if (r < 0) return r;
    usbdc_ep_configurar(0x00, UDC_EP_CONTROL, USB_TAM_EP0);
    usbdc_ep_configurar(0x80, UDC_EP_CONTROL, USB_TAM_EP0);
    u.activo = 1; u.estado = USB_EST_ESPERA; u.vbus_visto = 1;
    usbdc_conectar(1);
    return 0;
}
void usb_parar(void)
{
    if (u.activo) { usbdc_conectar(0); usbdc_parar(); }
    if (u.config) avisar(USB_AVISO_BUS);
    u.activo = 0; u.config = 0; u.dir = 0; u.estado = USB_EST_PARADO; u.fase = F_LIBRE;
    u.in_ocupado = u.int_ocupado = u.rx_pend = 0;
    u.cta.direccion = 0; u.cta.configuracion = 0;
    tubo_vaciar();
}
void usb_paso(void)
{
    UdcEvento ev; u8 ep; int k;
    if (!u.activo) return;
    for (k = 0; k < USB_EVENTOS_POR_PASO && usbdc_evento(&ev, &ep); k++) {
        switch (ev) {
        case UDC_EV_RESET: reset_bus(); break;
        case UDC_EV_SETUP: setup_llega(); break;
        case UDC_EV_RX:
            if ((ep & 0x0F) == 0) ep0_rx();
            else if (ep == USB_EP_BULK_OUT && u.config) { u.rx_pend = 1; rx_bombear(); }
            break;
        case UDC_EV_TX_HECHO:
            if ((ep & 0x0F) == 0) ep0_tx();
            else if (ep == USB_EP_BULK_IN) { u.in_ocupado = 0; tx_bombear(); }
            else if (ep == USB_EP_INT_IN) u.int_ocupado = 0;
            break;
        case UDC_EV_SUSPENDE: if (u.estado != USB_EST_SUSPENDIDO) { u.estado_antes = u.estado; u.estado = USB_EST_SUSPENDIDO; } break;
        case UDC_EV_REANUDA: if (u.estado == USB_EST_SUSPENDIDO) u.estado = u.estado_antes; break;
        case UDC_EV_DESCONECTA: usb_parar(); return;
        }
    }
    rx_bombear();
    tx_bombear();
}
int usb_conectado(void) { return u.activo && u.config != 0; }
/* (CARGA) lo que el PC ha concedido para cargar: los 500 mA del descriptor con la configuración puesta y el bus despierto; 0 sin
 * configurar, suspendido o parado (USB 2.0 §7.2.1: sin configurar, 100 mA; suspendido, 2,5 mA: el original tampoco carga así). */
int usb_carga_ma(void) { return u.activo && u.config && u.estado == USB_EST_CONFIGURADO ? 500 : 0; }
int usb_activo(void) { return u.activo; }
int usb_vigilar(u32 ahora)
{
    if (!u.vbus_visto || (u32)(ahora - u.ultimo_vbus) >= USB_VBUS_MS) {
        int v = usbdc_vbus();
        u.ultimo_vbus = ahora; u.vbus_visto = 1;
        if (v == 1 && !u.activo) { if (usb_iniciar() < 0) usb_parar(); u.ultimo_vbus = ahora; }
        else if (v == 0 && u.activo) usb_parar();
    }
    if (u.activo) usb_paso();
    return u.activo;
}
void usb_info(UsbInfo *i)
{
    *i = u.cta;
    i->estado = u.estado; i->direccion = u.dir; i->configuracion = u.config;
}
