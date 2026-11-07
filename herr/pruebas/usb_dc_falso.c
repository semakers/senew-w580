/* Un controlador USB de dispositivo FALSO para el PC (pista USBD), con la interfaz de hal/usb_dc.h, y el HOST simulado que
 * hay detrás (usb_host.h). Modela lo que importa al núcleo: un paquete por endpoint y sentido (NAK mientras no se lee o no se
 * ha recogido), conmutadores DATA0/DATA1 por los dos lados (si no cuadran, el host lo cuenta como error), STALL, SETUP que
 * anula el control en curso, reset del bus y caída de VBUS. Lo usan herr/pruebas/prueba_usb.c y, si quiere, la pista MTP. */
#include <stdio.h>
#include <string.h>
#include "usb_dc.h"
#include "usb_host.h"

UdcCuentas udc_cta;
typedef struct { u16 tam; u8 tipo, stall, tog, lleno; u8 n; u8 d[64]; } Ep;
static Ep ep_in[16], ep_out[16];
static u8 setup_d[8], setup_hay;
static struct { UdcEvento ev; u8 ep, gen; } cola[256];
static u8 ep0_gen;                                  /* como hal/usb_dc.c: lo del EP0 encolado antes de un SETUP nuevo se tira */
static unsigned c_ini, c_n;
static int iniciado, pullup, vbus = 1;
static u8 direccion;
HostCuentas hst;
int hst_prisa;
void (*hst_bomba)(void);

static void encolar(UdcEvento ev, u8 ep)
{
    if (c_n == 256) { udc_cta.perdidos++; return; }
    cola[(c_ini + c_n++) & 255].ev = ev; cola[(c_ini + c_n - 1) & 255].ep = ep; cola[(c_ini + c_n - 1) & 255].gen = ep0_gen;
    udc_cta.eventos++;
}
static Ep *ep_de(u8 ep) { return (ep & 0x80) ? &ep_in[ep & 15] : &ep_out[ep & 15]; }

/* ---------------------------------------------------------------------------------------------- lado del dispositivo */
int usbdc_iniciar(void) { memset(ep_in, 0, sizeof ep_in); memset(ep_out, 0, sizeof ep_out); c_n = 0; iniciado = 1; direccion = 0; hst.inicios++; return hst.fallar_inicio ? -1 : 0; }
void usbdc_conectar(int si) { pullup = si; }
int usbdc_vbus(void) { hst.lecturas_vbus++; return vbus; }
void usbdc_direccion(u8 d) { direccion = d; hst.direccion_puesta = d; }
int usbdc_ep_configurar(u8 ep, u8 tipo, u16 tam)
{
    Ep *e = ep_de(ep);
    if (tam > 64) return -1;
    memset(e, 0, sizeof *e); e->tam = tam; e->tipo = tipo;
    if ((ep & 15) == 0) { ep_in[0].tam = ep_out[0].tam = tam; }
    return 0;
}
int usbdc_ep_escribir(u8 ep, const u8 *p, u32 n)
{
    Ep *e = &ep_in[ep & 15];
    if (!iniciado || !(ep & 0x80) || !e->tam) { hst.mal_uso++; return -1; }
    if (e->lleno) return -1;
    if (n > e->tam) { hst.mal_uso++; return -1; }
    if (n) memcpy(e->d, p, n);
    e->n = (u8)n; e->lleno = 1; udc_cta.tx++;
    return (int)n;
}
int usbdc_ep_leer(u8 ep, u8 *p, u32 max)
{
    Ep *e = &ep_out[ep & 15];
    if (ep == UDC_EP_SETUP) {                           /* (AUD9 U1) solo el SETUP; usbdc_ep_leer(0, …) solo datos */
        if (max < 8 || !setup_hay) return -1;
        memcpy(p, setup_d, 8); setup_hay = 0; return 8;
    }
    if (!e->lleno) return -1;
    if (e->n > max) { hst.mal_uso++; memcpy(p, e->d, max); e->lleno = 0; return (int)max; }
    memcpy(p, e->d, e->n); e->lleno = 0; udc_cta.rx++;
    return e->n;
}
void usbdc_ep_stall(u8 ep, int si)
{
    Ep *e = ep_de(ep);
    e->stall = (u8)si;
    if (!si) e->tog = 0;
    if ((ep & 15) && e->lleno) { e->lleno = 0; }       /* como RESET_EP en el UDC (hal/usb_dc.c §5.3): el paquete en el FIFO se pierde */
    if (si) udc_cta.stalls++;
}
int usbdc_evento(UdcEvento *ev, u8 *ep)
{
    while (c_n) {
        UdcEvento e = cola[c_ini].ev; u8 p = cola[c_ini].ep, g = cola[c_ini].gen;
        c_ini = (c_ini + 1) & 255; c_n--;
        if ((e == UDC_EV_RX || e == UDC_EV_TX_HECHO || e == UDC_EV_SETUP) && (p & 15) == 0 && g != ep0_gen) continue;   /* (AUD9 U1) también el SETUP */
        *ev = e; *ep = p;
        return 1;
    }
    return 0;
}
void usbdc_parar(void) { iniciado = 0; pullup = 0; c_n = 0; hst.paradas++; }

/* ---------------------------------------------------------------------------------------------- lado del host */
static u8 htog_in[16], htog_out[16];
int hst_pullup(void) { return iniciado && pullup; }
u8 hst_direccion(void) { return direccion; }
void hst_vbus(int si) { vbus = si; if (!si && iniciado) encolar(UDC_EV_DESCONECTA, 0); }
void hst_reset(void)
{
    int i;
    for (i = 1; i < 16; i++) { memset(&ep_in[i], 0, sizeof(Ep)); memset(&ep_out[i], 0, sizeof(Ep)); }
    ep_in[0].lleno = ep_out[0].lleno = ep_in[0].stall = ep_out[0].stall = 0; setup_hay = 0;
    memset(htog_in, 0, sizeof htog_in); memset(htog_out, 0, sizeof htog_out);
    direccion = 0; c_n = 0;
    encolar(UDC_EV_RESET, 0);
}
void hst_suspender(int si) { encolar(si ? UDC_EV_SUSPENDE : UDC_EV_REANUDA, 0); }
void hst_tog_reset(u8 ep) { if (ep & 0x80) htog_in[ep & 15] = 0; else htog_out[ep & 15] = 0; }

#define VUELTAS 20000
static void bombear(void) { if (hst_bomba) hst_bomba(); }
/* deposita un paquete OUT; espera a que el endpoint esté libre (NAK). 0 = bien, -1 STALL, -2 tope */
static int paquete_out(u8 n_ep, const u8 *p, u32 n, int es_setup)
{
    Ep *e = &ep_out[n_ep];
    int v;
    if (es_setup) {
        memcpy(setup_d, p, 8); setup_hay = 1; ep0_gen++;
        ep_in[0].lleno = 0; ep_out[0].lleno = 0; ep_in[0].stall = ep_out[0].stall = 0;
        ep_in[0].tog = 1; ep_out[0].tog = 1; htog_in[0] = 1; htog_out[0] = 1;   /* tras SETUP, la fase de datos empieza en DATA1 */
        encolar(UDC_EV_SETUP, 0);
        return 0;
    }
    for (v = 0; v < VUELTAS; v++) {
        if (e->stall) { hst.stalls_vistos++; return -1; }
        if (!e->tam && n_ep) { hst.mal_uso++; return -2; }
        if (!e->lleno) {
            if (htog_out[n_ep] != e->tog) hst.toggles_mal++;
            htog_out[n_ep] ^= 1; e->tog ^= 1;
            if (n) memcpy(e->d, p, n);
            e->n = (u8)n; e->lleno = 1;
            encolar(UDC_EV_RX, n_ep);
            return 0;
        }
        bombear();
    }
    return -2;
}
/* recoge un paquete IN. Devuelve su longitud, -1 STALL, -2 tope */
static int paquete_in(u8 n_ep, u8 *p)
{
    Ep *e = &ep_in[n_ep];
    int v;
    for (v = 0; v < VUELTAS; v++) {
        if (e->stall) { hst.stalls_vistos++; return -1; }
        if (e->lleno) {
            int n = e->n;
            if (htog_in[n_ep] != e->tog) hst.toggles_mal++;
            htog_in[n_ep] ^= 1; e->tog ^= 1;
            memcpy(p, e->d, (u32)n); e->lleno = 0;
            encolar(UDC_EV_TX_HECHO, (u8)(0x80 | n_ep));
            hst.paquetes_in++; if (n == 0) hst.zlps_in++;
            return n;
        }
        bombear();
    }
    return -2;
}

int hst_control(const u8 s[8], u8 *datos, int *n_datos)
{
    u16 largo = (u16)(s[6] | s[7] << 8);
    u8 pk[64];
    int total = 0, n, v;
    hst.controles++;
    paquete_out(0, s, 8, 1);
    if ((s[0] & 0x80) && largo) {                                   /* lectura: datos IN, estado OUT */
        for (;;) {
            n = paquete_in(0, pk);
            if (n < 0) return n;
            if (n > 64 || total + n > largo) { hst.excesos++; return -3; }
            memcpy(datos + total, pk, (u32)n); total += n;
            if (n < 64 || total == largo) break;
        }
        if (n_datos) *n_datos = total;
        n = paquete_out(0, pk, 0, 0);
        if (n < 0) return n;
        if (hst_prisa) return 0;                                    /* (carrera) el SETUP siguiente llega antes de que el dispositivo vea el estado */
        for (v = 0; v < VUELTAS && ep_out[0].lleno; v++) bombear();   /* que el dispositivo vea el estado */
        return ep_out[0].lleno ? -2 : 0;
    }
    if (largo) {                                                    /* escritura con datos */
        int i;
        for (i = 0; i < largo; i += 64) {
            int k = largo - i < 64 ? largo - i : 64;
            n = paquete_out(0, datos + i, (u32)k, 0);
            if (n < 0) return n;
        }
    }
    n = paquete_in(0, pk);                                          /* estado: un ZLP del dispositivo */
    if (n < 0) return n;
    if (n != 0) { hst.excesos++; return -3; }
    for (v = 0; v < 4; v++) bombear();                              /* que aplique lo pendiente (SET_ADDRESS) */
    if (n_datos) *n_datos = 0;
    return 0;
}

int hst_bulk_out(u8 ep, const u8 *p, u32 n, int zlp)
{
    u32 i = 0;
    int r;
    while (i < n) {
        u32 k = n - i < 64 ? n - i : 64;
        if ((r = paquete_out(ep & 15, p + i, k, 0)) < 0) return r;
        i += k;
    }
    if (zlp && (n % 64) == 0) if ((r = paquete_out(ep & 15, p, 0, 0)) < 0) return r;
    return (int)n;
}
/* una transferencia IN entera (hasta un paquete corto). -1 STALL, -2 tope, -3 desborda max */
int hst_bulk_in(u8 ep, u8 *p, u32 max)
{
    u8 pk[64];
    u32 total = 0;
    for (;;) {
        int n = paquete_in(ep & 15, pk);
        if (n < 0) return n;
        if (total + (u32)n > max) { hst.excesos++; return -3; }
        memcpy(p + total, pk, (u32)n); total += (u32)n;
        if (n < 64) return (int)total;
    }
}
int hst_int_in(u8 ep, u8 *p) { return paquete_in(ep & 15, p); }
int hst_in_lleno(u8 ep) { return ep_in[ep & 15].lleno; }
/* VBUS cae sin que el driver lo vea (sin evento): solo lo descubre la lectura de usb_vigilar */
void hst_vbus_mudo(int si) { vbus = si; }
/* el host manda un SETUP de lectura, se lleva el primer paquete y lo abandona (lo siguiente será otro SETUP) */
int hst_control_a_medias(const u8 *s)
{
    u8 pk[64];
    paquete_out(0, s, 8, 1);
    return paquete_in(0, pk);
}
