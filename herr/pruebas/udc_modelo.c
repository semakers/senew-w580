/* (USBD) Un MODELO del UDC del DB2020 (TI OMAP1, docs/usb.md §2 y §9) a nivel de REGISTRO, para probar en el PC el driver
 * real (hal/usb_dc.c compilado con -DUSBDC_MODELO) con el mismo host simulado y las mismas pruebas que el núcleo
 * (herr/pruebas/prueba_usb.c). No sustituye al modelo de QEMU de la pista UEMU: es el banco de esta pista mientras no exista,
 * y lo que modela es lo que DICE docs/usb.md, no el aparato. Avisa (cuenta en hst.mal_uso y lo imprime) de lo que §2.1 y §9.7
 * prohíben: el bloque con la puerta 26 cerrada o el 20 en reset, DATA en un reset o sin VBUS, STAT_FLG/RXFSTAT sin EP_SEL,
 * RESET_EP en el EP0, configurar un EP con CFG_LOCK puesto y escribir en un FIFO IN ya armado. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "udc_modelo.h"
#include "usb_dc.h"
#include "usb_host.h"

HostCuentas hst;
int hst_prisa;
void (*hst_bomba)(void);
#define B 0xFC030000u
typedef struct { u8 d[64]; u16 n, rd; u8 armado, ack, halt, tog; } Fifo;
static Fifo fin[16], fout[16];
static u16 ep_num, syscon1, devstat, irq_en, irq_src, cfg[32], r108;
static u16 s18 = 0x03FF, s12 = 0x03DF;             /* SYSCON tras syscon_init(): puerta 26 cerrada, bloque 20 vivo */
/* (2026-10-20) LOS RELOJES DEL USB (tmp/usb_re/estatico.md, dinamico.md; medido en el aparato: sin ellos, leer el UDC CUELGA el
 * bus): +02 bit 12 a 0 y la puerta 37 (+1A bit 10) abierta. Valores tras nuestro syscon_init (dinamico.md: +02 = 0x1002,
 * +1A = 0x7FB8), y P4 (0xF9000028) */
static u16 s02 = 0x1002, s0a = 0x0000, s1a = 0x7FB8, p4 = 0x0000;
static u8 setup_d[8], setup_rd, ep0_stall, dir_hw, dir_pend, ctl_hw;
static u16 pend_tx, pend_rx;
static u8 htog_in[16], htog_out[16];
static int cpu_tapada;
static irq_fn linea[92]; static u8 tapada[92];

static void aviso(const char *q, u32 a) { hst.mal_uso++; if (hst.mal_uso < 20) printf("AVISO modelo: %s (0x%08X, EP_NUM %02X, DEVSTAT %02X)\n", q, a, ep_num, devstat); }
/* la puerta 26 / bloque 20 NO son del USB (UEMU, 07:52: el original enumera con ellos cerrados): no se vigila */
static void puerta(u32 a)
{
    (void)s18; (void)s12;
    if ((s1a & 0x0400) || (s02 & 0x1000)) aviso("acceso al UDC SIN sus relojes (puerta 37 cerrada o +02 bit 12 a 1): en el aparato CUELGA", a);
}
static int sel(void) { return ep_num & 0x20; }
static Fifo *ep_sel(void) { return (ep_num & 0x10) ? &fin[ep_num & 15] : &fout[ep_num & 15]; }
static int ep_valido(int n, int in) { return n == 0 || ((cfg[in ? 16 + n : n] & 0x8000) && (devstat & 0x08)); }
static u16 epn_stat(void)
{
    u16 v = 0; int i;
    for (i = 1; i < 16; i++) if (pend_tx & (1u << i)) { v |= (u16)i; break; }
    for (i = 1; i < 16; i++) if (pend_rx & (1u << i)) { v |= (u16)(i << 8); break; }
    return v;
}
static void ds_chg(void) { irq_src |= 0x08; }
/* (usb_ag, 2026-10-20) UDC_SIN_EPN_IRQ=1: el silicio que NO levanta EPN_TX/EPN_RX (hipótesis del MTP mudo): el driver tiene que
 * apañarse con su sondeo de STAT_FLG (hal/usb_dc.c, sondear_epn) */
static int sin_epn_irq(void) { static int v = -1; if (v < 0) v = getenv("UDC_SIN_EPN_IRQ") != 0; return v; }

u32 irq_cerrar(void) { int a = cpu_tapada; cpu_tapada = 1; return a ? 0x80 : 0; }
void irq_abrir(void) { cpu_tapada = 0; }
void intc_habilita_nivel(u32 l, irq_fn f, u32 p) { (void)p; linea[l] = f; tapada[l] = 0; }
void intc_tapar(u32 l) { tapada[l] = 1; }
void intc_habilita(u32 l, irq_fn f) { linea[l] = f; tapada[l] = 0; }
static void modelo_irq(void)
{
    int k;
    for (k = 0; k < 8; k++) {
        u16 en = (u16)(((irq_en & 1) ? 0x07 : 0) | (irq_en & 0xB8));
        if (cpu_tapada || !(irq_src & en)) return;
        if (linea[62] && !tapada[62]) linea[62]();
        else if (linea[63] && !tapada[63]) linea[63]();
        else return;
    }
}

u16 modelo_lee(u32 a)
{
    if (a == SYSCON + 0x18) return s18;
    if (a == SYSCON + 0x12) return s12;
    if (a == SYSCON + 0x02) return s02;
    if (a == SYSCON + 0x0A) return s0a;
    if (a == SYSCON + 0x1A) return s1a;
    if (a == 0xF9000028u) return p4;
    puerta(a);
    switch (a - B) {
    case 0x04: return ep_num;
    case 0x08:
        if (ep_num & 0x40) { u16 v = (u16)(setup_d[2 * (setup_rd & 3)] | setup_d[2 * (setup_rd & 3) + 1] << 8); setup_rd++; return v; }
        return (u16)(modelo_lee8(a) | modelo_lee8(a) << 8);
    case 0x10: {
        Fifo *f = ep_sel();
        u16 v = 0;
        if (!sel()) aviso("STAT_FLG sin EP_SEL", a);
        if (f->armado) v |= 0x04;
        if (f->ack) v |= 0x08;
        if (f->halt || ((ep_num & 15) == 0 && ep0_stall)) v |= 0x20;
        return v;
    }
    case 0x14: if (!sel()) aviso("RXFSTAT sin EP_SEL", a); return fout[ep_num & 15].n;
    case 0x18: return syscon1;
    case 0x1C: return 0;
    case 0x20: return devstat;
    case 0x28: return irq_en;
    case 0x30: return irq_src;
    case 0x34: return epn_stat();
    case 0x108: return r108;
    default:
        if (a - B >= 0x80 && a - B < 0x100) return cfg[(a - B - 0x80) / 4];
        aviso("lectura de un registro que el original no usa", a); return 0;
    }
}
u8 modelo_lee8(u32 a)
{
    Fifo *f = &fout[ep_num & 15];
    puerta(a);
    if (a != B + 0x08) { aviso("acceso de 8 bits fuera de DATA", a); return 0; }
    if ((devstat & 0x20) || !(devstat & 1)) aviso("DATA en un reset del bus o sin VBUS", a);
    if (ep_num & 0x10) { aviso("leer DATA con un EP IN seleccionado", a); return 0; }
    /* (2026-10-20, el MTP mudo del aparato) El ORIGINAL lee el FIFO OUT CON el EP seleccionado: FUN_44218b1c lee STAT_FLG y RXFSTAT,
     * deselecciona, y FUN_44218c50 vuelve a seleccionar (FUN_44218608, EP_NUM = ep|0x20) ANTES de FUN_44219278 (los ldrb de +08);
     * omap_udc.c (read_fifo) igual. El driver viejo leía con EP_SEL = 0: en el modelo de QEMU y aquí salían los datos buenos; en el
     * aparato el respondedor no contestaba nunca. Qué devuelve el silicio así es [sin saber]: aquí 0 y el FIFO no avanza. */
    if (!sel()) { aviso("leer DATA de un EP OUT SIN EP_SEL: el original selecciona antes (FUN_44218c50); en el aparato el MTP no contesta", a); return 0; }
    if (f->rd >= f->n) { aviso("leer DATA más allá del paquete", a); return 0; }
    return f->d[f->rd++];
}
void modelo_esc8(u32 a, u8 v)
{
    Fifo *f = &fin[ep_num & 15];
    puerta(a);
    if (a != B + 0x08) { aviso("acceso de 8 bits fuera de DATA", a); return; }
    if ((devstat & 0x20) || !(devstat & 1)) aviso("DATA en un reset del bus o sin VBUS", a);
    if (!(ep_num & 0x10) || !sel()) { aviso("escribir DATA sin un EP IN seleccionado", a); return; }
    if (f->armado) { aviso("escribir en un FIFO IN ya armado", a); return; }
    if (f->n >= 64) { aviso("FIFO IN desbordado", a); return; }
    f->d[f->n++] = v;
}
void modelo_esc(u32 a, u16 v)
{
    if (a == SYSCON + 0x18) { s18 = v; return; }
    if (a == SYSCON + 0x12) { s12 = v; return; }
    if (a == SYSCON + 0x02) { s02 = v; return; }
    if (a == SYSCON + 0x0A) { s0a = v; return; }
    if (a == SYSCON + 0x1A) { s1a = v; return; }
    if (a == 0xF9000028u) { p4 = v; return; }
    puerta(a);
    switch (a - B) {
    case 0x04: ep_num = v; if (v & 0x40) setup_rd = 0; return;
    case 0x08: modelo_esc8(a, (u8)v); modelo_esc8(a, (u8)(v >> 8)); return;
    case 0x0C: {
        Fifo *f = ep_sel();
        int n = ep_num & 15;
        if (v & 0x01) { if (n == 0) aviso("RESET_EP en el EP0", a); else { f->tog = 0; f->n = f->rd = 0; f->armado = f->ack = 0; } }
        if (v & 0x02) { f->n = f->rd = 0; f->ack = 0; }
        if (v & 0x08) f->tog = 0;
        if (v & 0x40) f->halt = 1;
        if (v & 0x80) { f->halt = 0; if (n == 0) ep0_stall = 0; }
        if (v & 0x04) {
            if (!(ep_num & 0x10)) { f->n = f->rd = 0; f->ack = 0; }     /* OUT: listo para recibir */
            f->armado = 1;
        }
        return;
    }
    case 0x18: syscon1 = v; return;
    case 0x1C:
        if (v & 0x20) ep0_stall = 1;
        if (v & 0x08) { if (!(devstat & 0x08)) { devstat |= 0x08; ds_chg(); } }
        if (v & 0x04) { if (devstat & 0x08) { devstat &= (u16)~0x08u; ds_chg(); } }
        return;
    case 0x28: irq_en = v; return;
    case 0x30: {
        int i;
        irq_src &= (u16)~v;
        if (v & 0x10) { for (i = 1; i < 16; i++) if (pend_tx & (1u << i)) { pend_tx &= (u16)~(1u << i); break; } if (pend_tx) irq_src |= 0x10; }
        if (v & 0x20) { for (i = 1; i < 16; i++) if (pend_rx & (1u << i)) { pend_rx &= (u16)~(1u << i); break; } if (pend_rx) irq_src |= 0x20; }
        return;
    }
    case 0x108: r108 = v; return;
    default:
        if (a - B >= 0x80 && a - B < 0x100) {
            if (syscon1 & 0x100) { aviso("configurar un EP con CFG_LOCK puesto", a); return; }
            cfg[(a - B - 0x80) / 4] = v;
            if ((v & 0x7FF) + (8u << ((v >> 12) & 7)) / 8 > 0x7F8 / 8 && (v & 0x8000)) aviso("EP fuera del FIFO de 2 KB", a);
            return;
        }
        aviso("escritura en un registro que el original no usa", a);
    }
}

/* ---------------------------------------------------------------------------------------------- el host */
int hst_pullup(void) { return (syscon1 & 1) && (devstat & 1); }
u8 hst_direccion(void) { return dir_hw; }
void hst_vbus(int si)
{
    if (si) { devstat |= 1; } else { devstat = 0; dir_hw = 0; }
    ds_chg(); modelo_irq();
}
void hst_vbus_mudo(int si) { if (si) devstat |= 1; else devstat = 0; }
void hst_reset(void)
{
    int i;
    devstat |= 0x20; ds_chg(); modelo_irq();
    devstat = (u16)((devstat & 1) | 0x02); dir_hw = 0;
    for (i = 0; i < 16; i++) { memset(&fin[i], 0, sizeof(Fifo)); memset(&fout[i], 0, sizeof(Fifo)); }
    ep0_stall = 0; pend_tx = pend_rx = 0; irq_src &= 0x08;
    memset(htog_in, 0, sizeof htog_in); memset(htog_out, 0, sizeof htog_out);
    ds_chg(); modelo_irq();
}
void hst_suspender(int si) { if (si) devstat |= 0x10; else devstat &= (u16)~0x10u; ds_chg(); modelo_irq(); }
void hst_tog_reset(u8 ep) { if (ep & 0x80) htog_in[ep & 15] = 0; else htog_out[ep & 15] = 0; }

#define VUELTAS 20000
static void bombear(void) { modelo_irq(); if (hst_bomba) hst_bomba(); modelo_irq(); }
static int paquete_out(u8 n_ep, const u8 *p, u32 n, int es_setup)
{
    Fifo *f = &fout[n_ep];
    int v;
    if (es_setup) {
        if (!hst_pullup()) return -2;
        memcpy(setup_d, p, 8);
        fin[0].armado = fin[0].ack = 0; fin[0].n = 0; fout[0].armado = fout[0].ack = 0;
        ep0_stall = 0; fin[0].tog = fout[0].tog = 1; htog_in[0] = htog_out[0] = 1;
        if (p[0] == 0x00 && p[1] == 5) {                        /* SET_ADDRESS: lo resuelve el hardware (docs/usb.md §0) */
            dir_pend = p[2] & 0x7F; ctl_hw = 1;
            fin[0].n = 0; fin[0].armado = 1;                    /* la fase de estado la pone él */
        } else { ctl_hw = 0; irq_src |= 0x04; }
        modelo_irq();
        return 0;
    }
    for (v = 0; v < VUELTAS; v++) {
        if (f->halt || (n_ep == 0 && ep0_stall)) { hst.stalls_vistos++; return -1; }
        if (!ep_valido(n_ep, 0)) { aviso("el host manda a un EP OUT no configurado", n_ep); return -2; }
        if (f->armado) {
            if (htog_out[n_ep] != f->tog && hst.toggles_mal++ < 3) printf("CONMUTADOR OUT ep%d host %d disp %d (controles %u)\n", n_ep, htog_out[n_ep], f->tog, hst.controles);
            htog_out[n_ep] ^= 1; f->tog ^= 1;
            if (n) memcpy(f->d, p, n);
            f->n = (u16)n; f->rd = 0; f->ack = 1; f->armado = 0;
            if (n_ep) { if (!sin_epn_irq()) { pend_rx |= (u16)(1u << n_ep); irq_src |= 0x20; } } else irq_src |= 0x02;
            modelo_irq();
            return 0;
        }
        bombear();
    }
    return -2;
}
static int paquete_in(u8 n_ep, u8 *p)
{
    Fifo *f = &fin[n_ep];
    int v;
    for (v = 0; v < VUELTAS; v++) {
        if (f->halt || (n_ep == 0 && ep0_stall)) { hst.stalls_vistos++; return -1; }
        if (n_ep && !ep_valido(n_ep, 1)) { aviso("el host pide a un EP IN no configurado", n_ep); return -2; }
        if (f->armado) {
            int n = f->n;
            if (htog_in[n_ep] != f->tog && hst.toggles_mal++ < 3) printf("CONMUTADOR IN ep%d host %d disp %d (controles %u)\n", n_ep, htog_in[n_ep], f->tog, hst.controles);
            htog_in[n_ep] ^= 1; f->tog ^= 1;
            memcpy(p, f->d, (u32)n); f->n = 0; f->armado = 0; f->ack = 1;
            if (n_ep == 0 && ctl_hw) { ctl_hw = 0; dir_hw = dir_pend; devstat |= 0x04; ds_chg(); }
            else if (n_ep) { if (!sin_epn_irq()) { pend_tx |= (u16)(1u << n_ep); irq_src |= 0x10; } }
            else irq_src |= 0x01;
            hst.paquetes_in++; if (n == 0) hst.zlps_in++;
            modelo_irq();
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
    if (paquete_out(0, s, 8, 1) < 0) return -2;
    if ((s[0] & 0x80) && largo) {
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
        if (hst_prisa) return 0;                                    /* (carrera) el SETUP siguiente, antes de que el dispositivo vea el estado */
        for (v = 0; v < 4; v++) bombear();
        return 0;
    }
    if (largo) {
        int i;
        for (i = 0; i < largo; i += 64) {
            int k = largo - i < 64 ? largo - i : 64;
            n = paquete_out(0, datos + i, (u32)k, 0);
            if (n < 0) return n;
        }
    }
    n = paquete_in(0, pk);
    if (n < 0) return n;
    if (n != 0) { hst.excesos++; return -3; }
    for (v = 0; v < 4; v++) bombear();
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
int hst_in_lleno(u8 ep) { return fin[ep & 15].armado; }
int hst_control_a_medias(const u8 *s) { u8 pk[64]; paquete_out(0, s, 8, 1); return paquete_in(0, pk); }
