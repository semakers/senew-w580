/* Sistema -> OK en las filas «USB» / «datos» -> «Diagnóstico USB» (2026-10-20): en el aparato (v3/v4) el teléfono ENUMERA pero el MTP
 * no contesta nada por los bulk (en el emulador sí). Una FOTO de esta pantalla separa las hipótesis (tmp/usb_ag/informe.md):
 *  - ¿llegan los paquetes OUT (EP2 OUT: irq / leídos) y qué bytes trae el último (un 0C 00 00 00 01 00 01 10… es una orden MTP;
 *    ceros = el FIFO se leyó mal)? ¿sale algo por el EP2 IN (escritos / ocupados, y sus bytes)?
 *  - ¿entra la línea 63? ¿se encendieron las IRQ de los EPn (IRQ_EN con 0x30) y qué DEVSTAT había al cerrar la configuración?
 *  - los registros del bloque releídos y el último SETUP; y la pila (UsbInfo) y el respondedor (MtpInfo).
 * Tecla 1: el modo de lectura del FIFO OUT (0 = CON EP_SEL, como el original; 1 = SIN, el driver viejo) para comparar con la sonda.
 * Lo da hal/usb_dc.c (débil: en el PC dice «no disponible»). Leer no selecciona ningún EP. */
#include "shell.h"
#include "usb.h"
#include "mtp.h"
#include "../../hal/usb_dc.h"

typedef struct { Widget base; } UsbDiag;
typedef struct { i32 sel; UdcDiag d; UsbInfo u; MtpInfo m; int hay, hay_mtp; } UsbDiagE;
extern const Clase ClsUsbDiag;
void usbdc_diag(UdcDiag *d) __attribute__((weak));
void usbdc_diag_modo(int modo) __attribute__((weak));
void mtp_info(MtpInfo *i) __attribute__((weak));
int  mtp_permiso(void) __attribute__((weak));
#define N_FILAS 24

static void leer(Ctx *c)
{
    UsbDiagE *s = ui_estado(c);
    s->hay = usbdc_diag != 0;
    if (s->hay) usbdc_diag(&s->d);
    usb_info(&s->u);
    s->hay_mtp = mtp_info != 0;
    if (s->hay_mtp) mtp_info(&s->m);
    ui_sucio(c);
}
static void iniciar(Ctx *c) { leer(c); ui_cada(c, 500, leer); }
static const char *hex8(const u8 *b) { return ui_fmt("%02X%02X%02X%02X %02X%02X%02X%02X", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]); }

static Widget *fila(Ctx *l, int i, int sel, void *dato)
{
    UsbDiagE *s = ui_estado(ui_ancestro(l, &ClsUsbDiag));
    const UdcDiag *d = &s->d;
    const char *n = "", *v = "";
    (void)dato;
    if (!s->hay) return sh_fila(i ? "" : "USB", i ? "" : "no disponible en esta plataforma", sel);
    switch (i) {                                  /* lo que más separa, en la primera pantalla */
    case 0:  n = "Tecla 1"; v = ui_fmt("modo %u: FIFO OUT %s", d->modo, d->modo ? "SIN EP_SEL (viejo)" : "con EP_SEL"); break;
    case 1:  n = "EP2 OUT"; v = ui_fmt("irq %u lei %u vac %u", d->rx_irq[2], d->leidos[2], d->vacios[2]); break;
    case 2:  n = "  bytes"; v = hex8(d->rx_cab); break;
    case 3:  n = "  ultimo"; v = ui_fmt("st %04X n %u", d->st_rx, d->rx_n); break;
    case 4:  n = "EP2 IN"; v = ui_fmt("irq %u esc %u ocu %u", d->tx_irq[2], d->escritos[2], d->ocupados[2]); break;
    case 5:  n = "  bytes"; v = hex8(d->tx_cab); break;
    case 6:  n = "  ultimo"; v = ui_fmt("st %04X", d->st_tx); break;
    case 7:  n = "Pila"; v = ui_fmt("rx %u tx %u cont %u st %u", s->u.rx_bytes, s->u.tx_bytes, s->u.contenedores_in, s->u.stalls); break;
    case 8:  n = "MTP"; v = !s->hay_mtp ? "-" : ui_fmt("ses %u op %04X tr %u err %u", s->m.sesion, s->m.op, s->m.transacciones, s->m.errores); break;
    case 9:  n = "  permiso"; v = !s->hay_mtp ? "-" : ui_fmt("%s, pidio %u", !mtp_permiso ? "-" : mtp_permiso() == 1 ? "SI" : mtp_permiso() == 2 ? "NO" : "sin contestar", s->m.pide_permiso); break;
    case 10: n = "  estado"; v = ui_fmt("est %u dir %u cfg %u rst %u", s->u.estado, s->u.direccion, s->u.configuracion, s->u.resets); break;
    case 11: n = "Bloque"; v = !d->vivo ? "sin relojes: no se lee" : ui_fmt("DEV %02X S1 %03X EN %02X", d->devstat, d->syscon1, d->irq_en); break;
    case 12: n = "  SRC/EPN"; v = !d->vivo ? "-" : ui_fmt("%02X %04X EPNUM %02X", d->irq_src, d->epn_stat, d->ep_num); break;
    case 13: n = "  EP cfg"; v = !d->vivo ? "-" : ui_fmt("%04X %04X %04X %04X", d->cfg_ep0, d->cfg_rx2, d->cfg_tx2, d->cfg_tx1); break;
    case 14: n = "Config"; v = ui_fmt("%u, DEV %02X, EN %02X", d->n_cfg, d->cfg_devstat, d->cfg_irq_en); break;
    case 15: n = "SETUP"; v = hex8(d->setup); break;
    case 16: n = "IRQ 62 / 63"; v = ui_fmt("%u / %u, red %u", d->irq62, d->irq63, d->red); break;
    case 17: n = "  fuentes"; v = ui_fmt("t%u r%u s%u d%u", d->src[0], d->src[1], d->src[2], d->src[3]); break;
    case 18: n = "  EPn"; v = ui_fmt("TX %u RX %u SOF %u ?%u", d->src[4], d->src[5], d->src[7], d->src[6]); break;
    case 19: n = "EP0"; v = ui_fmt("lei %u vac %u esc %u ocu %u", d->leidos[0], d->vacios[0], d->escritos[0], d->ocupados[0]); break;
    case 20: n = "EP1 IN (int)"; v = ui_fmt("irq %u esc %u ocu %u", d->tx_irq[1], d->escritos[1], d->ocupados[1]); break;
    case 21: n = "EPN sin EP"; v = ui_fmt("rx %u tx %u (EP1 OUT %u)", d->rx_sin_ep, d->tx_sin_ep, d->rx_irq[1]); break;
    case 23: n = "Almacenes MTP"; v = !s->hay_mtp ? "-" : ui_fmt("anuncia %u; tarjeta %s", s->m.almacenes, s->m.tarjeta == 1 ? "LISTA" : s->m.tarjeta == 0 ? "fuera" : s->m.tarjeta == 4 ? "preparando" : s->m.tarjeta == 2 ? "sin FAT" : "mala"); break;
    case 22: n = "Sondeo"; v = ui_fmt("rx %u tx %u, sin ACK %u", d->sondeo_rx, d->sondeo_tx, d->sin_ack); break;
    }
    return sh_fila(n, v, sel);
}
static int tecla(Ctx *t, int k, void *dato)
{
    Ctx *c = ui_ancestro(t, &ClsUsbDiag);
    UsbDiagE *s = ui_estado(c);
    if (k == K_1) { if (usbdc_diag_modo) usbdc_diag_modo(!s->d.modo); leer(c); return 1; }
    if (k == K_OK || k == K_SEL_IZQ) return 1;
    return sh_teclas(t, k, dato);
}
static Widget *construir(Ctx *c)
{
    UsbDiagE *s = ui_estado(c);
    return Teclas(tecla, Pantalla("Diagnóstico USB", ListaPerezosa(.n = N_FILAS, .fila = fila, .sel = &s->sel, .alto_fila = dis_tokens()->m.alto_fila), "", "Atrás"));
}
COMPONENTE_CON_ESTADO(UsbDiag, UsbDiagE, construir, iniciar);
Widget *app_usb_diag(void) { return UI_W(UsbDiag); }
