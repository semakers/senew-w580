/* Pruebas del núcleo USB (api/usb.c) contra el host simulado de usb_dc_falso.c. Pista USBD. docs/usb_driver.md §3.
 *   prueba_usb            -> todas las pruebas; 0 = todas bien
 *   prueba_usb azar N S   -> N peticiones de control al azar (semilla S): nunca más de wLength, nunca un cuelgue
 * Se compila con ASan y UBSan (Makefile: probar-usb). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usb.h"
#include "usb_dc.h"
#include "usb_host.h"

static int fallos, comprobaciones;
#define OK(c, ...) do { comprobaciones++; if (!(c)) { fallos++; printf("MAL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- el lado del dispositivo: usb_paso + un eco de contenedores por el tubo ---- */
static u8 eco[8192]; static u32 eco_n; static int eco_activo = 1;
static u32 ms;
static int ultimo_aviso;
static u8 lento[9000]; static u32 lento_n; static int lento_activo;
static void eco_paso(void)
{
    int a;
    if (!eco_activo && !lento_activo) return;                         /* «la app» no está mirando: el aviso se queda sin leer */
    a = usb_tubo_aviso();
    if (a) { ultimo_aviso = a; eco_n = 0; }
    if (lento_activo) { if (lento_n < sizeof lento) lento_n += (u32)usb_tubo_leer(lento + lento_n, sizeof lento - lento_n < 10 ? sizeof lento - lento_n : 10); return; }
    if (!eco_activo) return;
    if (eco_n) {
        int w = usb_tubo_escribir(eco, eco_n);
        if (w > 0) { memmove(eco, eco + w, eco_n - (u32)w); eco_n -= (u32)w; }
    }
    if (eco_n < sizeof eco) eco_n += (u32)usb_tubo_leer(eco + eco_n, (u32)(sizeof eco - eco_n));
}
static void bomba(void) { ms += 1; usb_vigilar(ms); eco_paso(); }

static int ctl(u8 tipo, u8 pet, u16 valor, u16 indice, u16 largo, u8 *d, int *n)
{
    u8 s[8] = { tipo, pet, (u8)valor, (u8)(valor >> 8), (u8)indice, (u8)(indice >> 8), (u8)largo, (u8)(largo >> 8) };
    return hst_control(s, d, n);
}
static int desc(u8 tipo, u8 idx, u16 lang, u16 largo, u8 *d)
{
    int n = -1, r = ctl(0x80, 6, (u16)(tipo << 8 | idx), lang, largo, d, &n);
    return r < 0 ? r : n;
}
static void conectar(void)
{
    int i;
    hst_vbus(1);
    for (i = 0; i < 400 && !hst_pullup(); i++) bomba();
}

/* comprueba el descriptor de configuración entero: longitudes, total, interfaz MTP y sus 3 endpoints */
static void validar_config(const u8 *c, int n)
{
    int i, eps = 0, hay_bin = 0, hay_bout = 0, hay_int = 0;
    OK(n >= 9 && c[0] == 9 && c[1] == 2, "cabecera de configuración");
    OK(n >= 9 && c[7] == 0x80 && c[8] == 0xFA, "alimentado por el bus, 500 mA (CARGA: como el original; sin esto el PC no concede la corriente)");
    OK((c[2] | c[3] << 8) == n, "wTotalLength %d != %d", c[2] | c[3] << 8, n);
    for (i = 0; i < n; i += c[i]) {
        OK(c[i] >= 2 && i + c[i] <= n, "descriptor de longitud %d en %d", c[i], i);
        if (c[i] < 2) return;
        if (c[i + 1] == 4) OK(c[i + 5] == 6 && c[i + 6] == 1 && c[i + 7] == 1 && c[i + 4] == 3, "interfaz no es 6/1/1 con 3 endpoints");
        if (c[i + 1] == 5) {
            u16 mp = (u16)(c[i + 4] | c[i + 5] << 8);
            eps++;
            if (c[i + 3] == 2 && (c[i + 2] & 0x80)) { hay_bin = 1; OK(mp == 64, "bulk IN de %d", mp); }
            if (c[i + 3] == 2 && !(c[i + 2] & 0x80)) { hay_bout = 1; OK(mp == 64, "bulk OUT de %d", mp); }
            if (c[i + 3] == 3 && (c[i + 2] & 0x80)) { hay_int = 1; OK(mp <= 64 && c[i + 6] >= 1, "interrupt IN"); }
        }
    }
    OK(eps == 3 && hay_bin && hay_bout && hay_int, "endpoints: %d", eps);
}
static void cadena_es(const u8 *d, int n, const char *s)
{
    int i, l = (int)strlen(s);
    OK(n == 2 + 2 * l && d[0] == n && d[1] == 3, "cadena «%s»: n=%d", s, n);
    for (i = 0; i < l && 2 + 2 * i + 1 < n; i++) OK(d[2 + 2 * i] == (u8)s[i] && d[3 + 2 * i] == 0, "cadena «%s» en %d", s, i);
}

/* Linux (usb/core/hub.c, «new scheme»): 64 del dispositivo, reset, SET_ADDRESS, 18, config 9, config entera, cadenas */
static void enumerar_linux(void)
{
    u8 d[512]; int n, tot;
    hst_reset(); bomba();
    n = desc(1, 0, 0, 64, d); OK(n == 18 && d[7] == 64, "dispositivo (64): n=%d", n);
    hst_reset(); bomba();
    OK(ctl(0x00, 5, 7, 0, 0, 0, 0) == 0, "SET_ADDRESS");
    OK(hst_direccion() == 7, "la dirección no se aplicó tras el estado: %d", hst_direccion());
    n = desc(1, 0, 0, 18, d); OK(n == 18 && d[0] == 18 && d[1] == 1 && (d[8] | d[9] << 8) == USB_VID, "dispositivo (18)");
    n = desc(2, 0, 0, 9, d); OK(n == 9, "config (9): %d", n);
    tot = d[2] | d[3] << 8;
    n = desc(2, 0, 0, (u16)tot, d); validar_config(d, n);
    n = desc(3, 0, 0, 255, d); OK(n == 4 && d[2] == 0x09 && d[3] == 0x04, "idiomas");
    n = desc(3, 2, 0x0409, 255, d); cadena_es(d, n, "W580 sistema propio");
    n = desc(3, 1, 0x0409, 255, d); cadena_es(d, n, "W580 homebrew");
    n = desc(3, 3, 0x0409, 255, d); cadena_es(d, n, "W580S10000001");
    OK(ctl(0x00, 9, 1, 0, 0, 0, 0) == 0 && usb_conectado(), "SET_CONFIGURATION 1");
    n = -1; OK(ctl(0x80, 8, 0, 0, 1, d, &n) == 0 && n == 1 && d[0] == 1, "GET_CONFIGURATION");
}
/* Windows (usbhub/ucx): 64, reset, dirección, 18, config 9 y 255, la cadena 0xEE, la compat ID, el calificador (STALL) */
static void enumerar_windows(void)
{
    u8 d[512]; int n;
    hst_reset(); bomba();
    n = desc(1, 0, 0, 64, d); OK(n == 18, "dispositivo (64)");
    hst_reset(); bomba();
    OK(ctl(0x00, 5, 3, 0, 0, 0, 0) == 0 && hst_direccion() == 3, "SET_ADDRESS");
    n = desc(1, 0, 0, 18, d); OK(n == 18, "dispositivo (18)");
    n = desc(2, 0, 0, 9, d); OK(n == 9, "config (9)");
    n = desc(2, 0, 0, 255, d); validar_config(d, n);
    n = desc(6, 0, 0, 10, d); OK(n == -1, "el calificador tiene que dar STALL: %d", n);
    n = desc(0x0F, 0, 0, 5, d); OK(n == -1, "BOS: STALL");
    n = desc(3, 0, 0, 255, d); OK(n == 4, "idiomas");
    n = desc(3, 3, 0x0409, 255, d); cadena_es(d, n, "W580S10000001");
    n = desc(3, 0xEE, 0, 0x12, d);
    OK(n == 18 && d[16] == USB_VENDOR_MS && !memcmp(d + 2, "M\0S\0F\0T\0" "1\0" "0\0" "0\0", 14), "cadena 0xEE MSFT100");
    n = -1; OK(ctl(0xC0, USB_VENDOR_MS, 0, 4, 16, d, &n) == 0 && n == 16 && (d[0] | d[1] << 8) == 40, "compat ID (16)");
    n = -1; OK(ctl(0xC0, USB_VENDOR_MS, 0, 4, 40, d, &n) == 0 && n == 40 && !memcmp(d + 18, "WINUSB\0\0", 8), "compat ID «WINUSB»");
    /* Extended Properties: Windows pide primero la cabecera (10) y luego el largo que dice; una sola, DeviceInterfaceGUID, REG_SZ */
    n = -1; OK(ctl(0xC1, USB_VENDOR_MS, 0, 5, 10, d, &n) == 0 && n == 10 && (d[0] | d[1] << 8) == 142 && d[6] == 5 && d[8] == 1,
               "extended properties (10): %d", n);
    n = -1; OK(ctl(0xC1, USB_VENDOR_MS, 0, 5, 142, d, &n) == 0 && n == 142, "extended properties (142): %d", n);
    {
        static const char guid[] = "{75E2684D-1404-4935-9498-B79143AFBA85}", nombre[] = "DeviceInterfaceGUID";
        int i, bien = (d[10] | d[11] << 8) == 132 && d[14] == 1 && (d[18] | d[19] << 8) == 40 && (d[60] | d[61] << 8) == 78;
        for (i = 0; i < 20; i++) bien = bien && d[20 + 2 * i] == (u8)nombre[i] && d[21 + 2 * i] == 0;
        for (i = 0; i < 39; i++) bien = bien && d[64 + 2 * i] == (u8)guid[i] && d[65 + 2 * i] == 0;
        OK(bien, "extended properties: DeviceInterfaceGUID = %s", guid);
    }
    n = desc(3, 4, 0x0409, 255, d); cadena_es(d, n, "MTP");
    n = desc(3, 2, 0x0409, 255, d); cadena_es(d, n, "W580 sistema propio");
    n = -1; OK(ctl(0x80, 0, 0, 0, 2, d, &n) == 0 && n == 2 && d[0] == 0, "GET_STATUS del dispositivo (CARGA: alimentado por el bus, como el descriptor)");
    OK(ctl(0x00, 9, 1, 0, 0, 0, 0) == 0 && usb_conectado(), "SET_CONFIGURATION 1");
}

static u8 contenedor[8192];
static u32 hacer_contenedor(u32 L, u32 sem)
{
    u32 i;
    contenedor[0] = (u8)L; contenedor[1] = (u8)(L >> 8); contenedor[2] = (u8)(L >> 16); contenedor[3] = (u8)(L >> 24);
    contenedor[4] = 2; contenedor[5] = 0; contenedor[6] = 1; contenedor[7] = 0x10;
    for (i = 8; i < L; i++) contenedor[i] = (u8)(i * 7 + sem);
    return L;
}
static void eco_de(u32 L, const char *qué)
{
    static u8 vuelta[8192];
    int n;
    hacer_contenedor(L, L);
    OK(hst_bulk_out(USB_EP_BULK_OUT, contenedor, L, 1) == (int)L, "%s: OUT de %u", qué, L);
    n = hst_bulk_in(USB_EP_BULK_IN, vuelta, sizeof vuelta);
    OK(n == (int)L && !memcmp(vuelta, contenedor, L), "%s: eco de %u volvió %d", qué, L, n);
}
static void eco_varios(void)
{
    static const u32 tam[] = { 12, 13, 63, 64, 65, 127, 128, 129, 500, 512, 1000, 1024, 4095, 4096, 4097, 8000 };
    unsigned i;
    u32 z0 = hst.zlps_in;
    for (i = 0; i < sizeof tam / sizeof tam[0]; i++) eco_de(tam[i], "eco");
    OK(hst.zlps_in - z0 == 6, "ZLPs tras los múltiplos de 64 (64, 128, 512, 1024, 4096, 8000): %u", hst.zlps_in - z0);
}

static void prueba_basica(void)
{
    conectar();
    OK(hst_pullup(), "sin pull-up con VBUS");
    enumerar_linux();
    eco_varios();
}

static void prueba_windows(void) { enumerar_windows(); eco_de(64, "eco tras Windows"); eco_de(12, "eco tras Windows"); }

/* reset del bus en mitad de una transferencia IN: el tubo se vacía, aviso BUS, y se vuelve a enumerar y a hacer eco */
static void prueba_reset_a_mitad(void)
{
    u8 b[64]; int n;
    hacer_contenedor(3000, 1);
    hst_bulk_out(USB_EP_BULK_OUT, contenedor, 3000, 1);
    n = hst_int_in(USB_EP_BULK_IN, b); OK(n == 64, "primer paquete del eco: %d", n);   /* se lleva un paquete y corta */
    hst_reset(); bomba(); bomba();
    OK(!usb_conectado(), "sigue configurado tras el reset");
    OK(ultimo_aviso == USB_AVISO_BUS, "aviso tras el reset: %d", ultimo_aviso);
    enumerar_linux();
    eco_de(200, "eco tras reset");
    eco_de(4096, "eco tras reset");
}
/* SET_FEATURE(HALT) en el bulk IN, GET_STATUS, CLEAR_FEATURE con los conmutadores puestos a DATA0 por los dos lados */
static void prueba_halt(void)
{
    u8 d[64]; int n;
    eco_de(65, "antes del halt");                                     /* deja los conmutadores en un valor cualquiera */
    OK(ctl(0x02, 3, 0, USB_EP_BULK_IN, 0, 0, 0) == 0, "SET_FEATURE halt");
    n = -1; OK(ctl(0x82, 0, 0, USB_EP_BULK_IN, 2, d, &n) == 0 && n == 2 && d[0] == 1, "GET_STATUS ep parado");
    hacer_contenedor(20, 9);
    hst_bulk_out(USB_EP_BULK_OUT, contenedor, 20, 1);
    n = hst_bulk_in(USB_EP_BULK_IN, d, 16); OK(n == -1, "el IN parado tenía que dar STALL: %d", n);
    n = -1; OK(ctl(0xA1, 0x67, 0, 0, 16, d, &n) == 0 && n == 8 && d[2] == 0x1F && d[4] == USB_EP_BULK_IN, "Get Device Status con el endpoint parado: n=%d", n);
    OK(ctl(0x02, 1, 0, USB_EP_BULK_IN, 0, 0, 0) == 0, "CLEAR_FEATURE halt"); hst_tog_reset(USB_EP_BULK_IN);
    n = -1; OK(ctl(0x82, 0, 0, USB_EP_BULK_IN, 2, d, &n) == 0 && d[0] == 0, "GET_STATUS ep libre");
    { static u8 v[64]; n = hst_bulk_in(USB_EP_BULK_IN, v, 64); OK(n == 20 && !memcmp(v, contenedor, 20), "el eco retenido sale tras el CLEAR: %d", n); }
    n = desc(9, 0, 0, 10, d); OK(n == -1, "descriptor desconocido: STALL");
    n = -1; OK(ctl(0x80, 0x55, 0, 0, 2, d, &n) == -1, "petición desconocida: STALL");
    OK(ctl(0x02, 3, 0, 0x05, 0, 0, 0) == -1, "halt de un endpoint que no existe: STALL");
    n = desc(1, 0, 0, 18, d); OK(n == 18, "el EP0 sigue vivo tras los STALL");
    eco_de(77, "tras STALL");
}
/* peticiones de clase: Cancel (con sus 6 bytes), Get Device Status ocupado/OK, Device Reset */
static void prueba_clase(void)
{
    u8 d[64]; int n;
    u8 cancel[6] = { 0x01, 0x40, 5, 0, 0, 0 };
    eco_activo = 0;                                                   /* lo que mande el host se queda en el tubo */
    hacer_contenedor(300, 3); hst_bulk_out(USB_EP_BULK_OUT, contenedor, 300, 1);
    ultimo_aviso = 0;
    OK(ctl(0x21, 0x64, 0, 0, 6, cancel, 0) == 0, "Cancel");
    n = -1; OK(ctl(0xA1, 0x67, 0, 0, 16, d, &n) == 0 && n == 4 && d[2] == 0x19 && d[3] == 0x20, "Device_Busy antes de leer el aviso: %d %02X", n, d[2]);
    eco_activo = 1; bomba();
    OK(ultimo_aviso == USB_AVISO_CANCELAR, "aviso Cancel: %d", ultimo_aviso);
    n = -1; OK(ctl(0xA1, 0x67, 0, 0, 16, d, &n) == 0 && n == 4 && d[2] == 0x01, "OK tras leer el aviso");
    { u8 malo[6] = { 0x02, 0x40 }; OK(ctl(0x21, 0x64, 0, 0, 6, malo, 0) == -1, "Cancel con código malo: STALL"); }
    OK(ctl(0x21, 0x66, 0, 0, 0, 0, 0) == 0, "Device Reset"); bomba();
    OK(ultimo_aviso == USB_AVISO_RESET_MTP, "aviso Device Reset");
    hst_tog_reset(USB_EP_BULK_IN); hst_tog_reset(USB_EP_BULK_OUT); hst_tog_reset(USB_EP_INT_IN);
    eco_de(100, "tras Device Reset");
    /* el evento por el interrupt IN */
    { u8 ev[24] = { 24, 0, 0, 0, 4, 0, 0x02, 0x40 }, r[64];
      OK(usb_tubo_evento(ev, 24) == 24, "evento");
      OK(usb_tubo_evento(ev, 24) == 0, "el segundo evento espera al primero");
      n = hst_int_in(USB_EP_INT_IN, r); OK(n == 24 && !memcmp(r, ev, 24), "evento recibido %d", n);
      bomba(); OK(usb_tubo_evento(ev, 12) == 12, "tras salir, otro"); hst_int_in(USB_EP_INT_IN, r); }
}
/* control de flujo: el host manda 8 KiB sin que nadie lea; nada se pierde y el host ve NAK, no errores */
static void prueba_flujo(void)
{
    u32 i;
    eco_activo = 0; lento_activo = 1; lento_n = 0;
    hacer_contenedor(8000, 5);
    /* el host manda 8000 bytes y la «app» lee de 10 en 10: el endpoint da NAK mientras el tubo está lleno, nada se pierde */
    OK(hst_bulk_out(USB_EP_BULK_OUT, contenedor, 8000, 1) == 8000, "OUT de 8000 con lector lento");
    for (i = 0; i < 2000 && lento_n < 8000; i++) bomba();
    OK(lento_n == 8000 && !memcmp(lento, contenedor, 8000), "llegaron %u", lento_n);
    lento_activo = 0;
    /* el envío: un contenedor más grande que el tubo, escrito a trozos según usb_tubo_libre() */
    { static u8 v[9000]; u32 puesto = 0; int vueltas = 0;
      hacer_contenedor(7000, 6);
      while (puesto < 7000 && vueltas++ < 100000) {
          int libre = usb_tubo_libre();
          if (libre > 0) { u32 k = (u32)libre < 7000 - puesto ? (u32)libre : 7000 - puesto; if (k > 700) k = 700; puesto += (u32)usb_tubo_escribir(contenedor + puesto, k); }
          if (hst_in_lleno(USB_EP_BULK_IN)) break;                    /* ya hay algo saliendo: el host empieza a leer */
          bomba();
      }
      /* el host lee mientras se sigue escribiendo: lo hace el bombeo */
      { u32 tot = 0; u8 pk[64]; int k;
        for (vueltas = 0; vueltas < 100000; vueltas++) {
            if (puesto < 7000) { int libre = usb_tubo_libre(); if (libre > 0) { u32 kk = (u32)libre < 7000 - puesto ? (u32)libre : 7000 - puesto; puesto += (u32)usb_tubo_escribir(contenedor + puesto, kk); } }
            if (hst_in_lleno(USB_EP_BULK_IN)) { k = hst_int_in(USB_EP_BULK_IN, pk); memcpy(v + tot, pk, (u32)k); tot += (u32)k; if (k < 64) break; }
            bomba();
        }
        OK(tot == 7000 && !memcmp(v, contenedor, 7000), "contenedor de 7000 por trozos: %u", tot); }
    }
    eco_activo = 1;
}
/* paquetes cortos y ZLP del host: un contenedor de 128 llega con ZLP detrás, otro de 10 corto; los dos vuelven bien */
static void prueba_cortos(void)
{
    eco_de(128, "128 con ZLP del host");
    eco_de(10, "contenedor de 10 (< 12: la cabecera dice 10)");
}
/* suspensión y desconexión: se para todo; al volver VBUS se arranca de cero */
static void prueba_desconexion(void)
{
    UsbInfo i; int k;
    hst_suspender(1); bomba(); usb_info(&i); OK(i.estado == USB_EST_SUSPENDIDO, "suspendido");
    hst_suspender(0); bomba(); usb_info(&i); OK(i.estado == USB_EST_CONFIGURADO, "reanudado");
    eco_activo = 0;
    hacer_contenedor(700, 2); hst_bulk_out(USB_EP_BULK_OUT, contenedor, 700, 1);
    hst_vbus(0);
    for (k = 0; k < 2; k++) bomba();
    OK(!usb_activo() && !usb_conectado() && !hst_pullup(), "sigue activo tras quitar el cable");
    OK(usb_tubo_leer(contenedor, 100) == 0 && usb_tubo_escribir(contenedor, 10) == 0 && usb_tubo_libre() == 0, "el tubo no está muerto tras quitar el cable");
    for (k = 0; k < 1000; k++) bomba();
    OK(!usb_activo(), "arrancó sin VBUS");
    eco_activo = 1;
    conectar();
    for (k = 0; k < 400 && !usb_activo(); k++) bomba();
    OK(usb_activo() && hst_pullup(), "no arrancó al volver VBUS");
    enumerar_windows();
    eco_de(333, "tras reconectar");
    /* VBUS que cae sin evento (el driver no lo vio): lo pilla usb_vigilar por la lectura */
    {
        hst_vbus_mudo(0);
        for (k = 0; k < USB_VBUS_MS + 10; k++) bomba();
        OK(!usb_activo(), "sin VBUS por la lectura y sigue activo");
        hst_vbus_mudo(1);
    }
}
/* un control que el host abandona: SETUP de lectura, se lleva un paquete y manda otro SETUP */
static void prueba_setup_encima(void)
{
    u8 d[256]; int n;
    conectar(); enumerar_linux();
    { u8 s[8] = { 0x80, 6, 0, 2, 0, 0, 0xFF, 0 }; hst_control_a_medias(s); }
    n = desc(1, 0, 0, 18, d); OK(n == 18, "el control siguiente al abandonado: %d", n);
#ifndef CON_MODELO                                                    /* en el UDC, SET_ADDRESS lo contesta el hardware */
    OK(ctl(0x00, 5, 9, 0, 0, 0, 0) == -1, "SET_ADDRESS configurado: STALL");
#endif
    n = desc(2, 0, 0, 39, d); OK(n == 39, "config: lo pedido");
    n = desc(2, 0, 0, 1000, d); OK(n == 39, "config: todo con más wLength");
    n = desc(3, 9, 0x0409, 255, d); OK(n == -1, "cadena que no existe: STALL");
}
/* LA CARRERA del emulador de UEMU (08:07): el estado OUT de una lectura sin atender y el SETUP siguiente ya encima */
static void prueba_prisa(void)
{
    u8 d[256]; int n, i;
    hst_prisa = 1;
    for (i = 0; i < 20; i++) {
        n = desc(3, 0, 0, 255, d); OK(n == 4, "con prisa, idiomas: %d (vuelta %d)", n, i);
        n = desc(1, 0, 0, 18, d); OK(n == 18, "con prisa, dispositivo: %d", n);
        n = desc(3, 2, 0x0409, 255, d); OK(n == 40, "con prisa, producto: %d", n);
    }
    hst_prisa = 0;
    eco_de(500, "tras la prisa");
}
/* (AUD9, docs/auditoria_ronda2.md §2) los reproductores de work/aud9/usbd/ hechos comprobaciones: U1 y los BAJO a–e */
#ifdef CON_MODELO
u16 modelo_lee(u32 a);
#endif
static void reconectar(void) { int k; eco_activo = 1; hst_reset(); bomba(); enumerar_linux(); for (k = 0; k < 5; k++) bomba(); }
static void prueba_aud9(void)
{
    static u8 c[4096], r[8192];
    u8 getdesc[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
#ifdef CON_MODELO
    u8 getst[8] = { 0x80, 0, 0, 0, 0, 0, 2, 0 };
#endif
    u8 cancel_d[6] = { 0x01, 0x40, 0x07, 0, 0, 0 }, reset_mtp[8] = { 0x21, 0x66, 0, 0, 0, 0, 0, 0 };
    int i, a, k;
    void (*b)(void) = hst_bomba;
    /* U1 caso A: un GET_DESCRIPTOR abandonado y un Cancel detrás, con el bucle ocupado */
    reconectar(); eco_activo = 0; while (usb_tubo_aviso()) ;
    hst_bomba = 0; hst_control_a_medias(getdesc); ctl(0x21, 0x64, 0, 0, 6, cancel_d, 0); hst_bomba = b;
    for (i = 0; i < 20; i++) bomba();
    a = usb_tubo_aviso(); k = hst_in_lleno(0x80);
    OK(a == USB_AVISO_CANCELAR && k == 1, "U1 A: Cancel tras un control abandonado: aviso %d, ZLP de estado %d (1 y 1)", a, k);
    /* U1 caso B: los 8 bytes de DATOS de una petición son «21 66…»: no se ejecutan como SETUP */
    reconectar(); eco_activo = 0; while (usb_tubo_aviso()) ;
    hst_bomba = 0; hst_control_a_medias(getdesc); ctl(0x21, 0x99, 0, 0, 8, reset_mtp, 0); hst_bomba = b;
    for (i = 0; i < 20; i++) bomba();
    a = usb_tubo_aviso();
    OK(a != USB_AVISO_RESET_MTP, "U1 B: datos OUT ejecutados como SETUP (aviso %d)", a);
#ifdef CON_MODELO
    /* (a) 64 SETUP con el bucle ocupado no se comen el TX_HECHO del bulk IN */
    reconectar(); eco_activo = 0;
    c[0] = 100; memset(c + 4, 0xA1, 96); c[1] = c[2] = c[3] = 0; usb_tubo_escribir(c, 100);
    hst_bomba = 0; for (i = 0; i < 64; i++) hst_control_a_medias(getst);
    k = hst_int_in(USB_EP_BULK_IN, r); hst_bomba = b; for (i = 0; i < 50; i++) bomba();
    a = hst_bulk_in(USB_EP_BULK_IN, r, sizeof r);
    OK(k == 64 && a == 36 && udc_cta.perdidos == 0, "(a) 64 SETUP: %d y %d B (64 y 36), perdidos %u", k, a, udc_cta.perdidos);
#endif
    /* (b) SET_INTERFACE a mitad de un contenedor y SET_CONFIGURATION(1) repetido: con aviso */
    reconectar(); eco_activo = 0; while (usb_tubo_aviso()) ;
    memset(c, 0xB2, 300); c[0] = 0x2C; c[1] = 1; c[2] = c[3] = 0; usb_tubo_escribir(c, 200);
    OK(ctl(0x01, 11, 0, 0, 0, 0, 0) == 0, "SET_INTERFACE"); for (i = 0; i < 5; i++) bomba();
    a = usb_tubo_aviso(); OK(a == USB_AVISO_CANCELAR, "(b) SET_INTERFACE a mitad: aviso %d (1)", a);
    hst_tog_reset(USB_EP_BULK_IN); hst_tog_reset(USB_EP_BULK_OUT); hst_tog_reset(USB_EP_INT_IN);
    OK(ctl(0x00, 9, 1, 0, 0, 0, 0) == 0, "SET_CONFIGURATION(1) otra vez"); for (i = 0; i < 5; i++) bomba();
    a = usb_tubo_aviso(); OK(a == USB_AVISO_BUS, "(b) SET_CONFIGURATION(1) repetido: aviso %d (3)", a);
    hst_tog_reset(USB_EP_BULK_IN); hst_tog_reset(USB_EP_BULK_OUT); hst_tog_reset(USB_EP_INT_IN);
    /* (c) CLEAR_FEATURE(HALT) del bulk IN sin halt, a mitad de un contenedor: se tira el resto CON aviso, y el siguiente sale entero */
    memset(c, 0xC3, 200); c[0] = 200; c[1] = c[2] = c[3] = 0; usb_tubo_escribir(c, 200);
    for (i = 0; i < 3; i++) bomba();
    OK(ctl(0x02, 1, 0, USB_EP_BULK_IN, 0, 0, 0) == 0, "CLEAR_FEATURE"); hst_tog_reset(USB_EP_BULK_IN);
    for (i = 0; i < 5; i++) bomba();
    a = usb_tubo_aviso(); OK(a == USB_AVISO_CANCELAR, "(c) CLEAR_FEATURE(HALT) a mitad: aviso %d (1)", a);
    memset(c, 0xC4, 90); c[0] = 90; c[1] = c[2] = c[3] = 0; usb_tubo_escribir(c, 90); for (i = 0; i < 3; i++) bomba();
    k = hst_bulk_in(USB_EP_BULK_IN, r, sizeof r);
    OK(k == 90 && r[5] == 0xC4, "(c) el contenedor siguiente sale entero: %d B (90), byte %02X", k, r[5]);
    /* (d) SET_CONFIGURATION 1, 1, 0: el UDC queda desconfigurado */
    OK(ctl(0x00, 9, 0, 0, 0, 0, 0) == 0, "SET_CONFIGURATION(0)"); for (i = 0; i < 5; i++) bomba();
#ifdef CON_MODELO
    OK(!(modelo_lee(0xFC030020u) & 8), "(d) tras 1,1,0 el UDC sigue configurado: DEVSTAT %04X", modelo_lee(0xFC030020u));
#endif
    OK(!usb_conectado(), "(d) el núcleo sigue configurado");
    /* (e) Device Reset y Cancel antes de que el respondedor mire: gana el Device Reset */
    reconectar(); eco_activo = 0; while (usb_tubo_aviso()) ;
    ctl(0x21, 0x66, 0, 0, 0, 0, 0); ctl(0x21, 0x64, 0, 0, 6, cancel_d, 0);
    a = usb_tubo_aviso(); k = usb_tubo_aviso();
    OK(a == USB_AVISO_RESET_MTP && k == 0, "(e) Device Reset + Cancel: avisos %d y %d (2 y 0)", a, k);
    /* wIndex que no cabe en un byte: STALL, no el endpoint de su byte bajo */
    a = -1; OK(ctl(0x82, 0, 0, 0x0100 | USB_EP_BULK_IN, 2, r, &a) == -1, "GET_STATUS con wIndex 0x0182: STALL");
    reconectar();
}
static void azar(unsigned n, unsigned semilla)
{
    unsigned i;
    srand(semilla);
    conectar(); enumerar_windows();
    for (i = 0; i < n; i++) {
        u8 s[8], d[1024]; int k, r;
        for (k = 0; k < 8; k++) s[k] = (u8)rand();
        if (rand() % 3) s[0] &= 0xE3;                              /* tipos casi válidos */
        if (rand() % 2) s[7] = 0;
        if (!(s[0] & 0x80) && (s[6] | s[7] << 8) > 64) s[7] = 0, s[6] &= 0x3F;
        if (!(s[0] & 0x80)) for (k = 0; k < 64; k++) d[k] = (u8)rand();
        if (s[1] == 5 || s[1] == 9 || s[1] == 0x66) continue;      /* dirección y configuración, aparte */
        r = hst_control(s, d, &k);
        if (r == -2) { printf("MAL azar: cuelgue con %02X %02X %02X%02X %02X%02X %02X%02X\n", s[0], s[1], s[3], s[2], s[5], s[4], s[7], s[6]); fallos++; break; }
        if (rand() % 50 == 0) { hst_reset(); bomba(); enumerar_windows(); }
        if (rand() % 20 == 0) eco_de((u32)(12 + rand() % 3000), "eco al azar");
    }
    OK(hst.excesos == 0 && hst.mal_uso == 0 && hst.toggles_mal == 0, "azar: excesos %u, mal uso %u, toggles %u", hst.excesos, hst.mal_uso, hst.toggles_mal);
}

int main(int argc, char **argv)
{
    hst_bomba = bomba;
    if (argc >= 2 && !strcmp(argv[1], "azar")) {
        azar(argc > 2 ? (unsigned)atoi(argv[2]) : 10000, argc > 3 ? (unsigned)atoi(argv[3]) : 1);
    } else {
        prueba_basica();
        prueba_windows();
        prueba_reset_a_mitad();
        prueba_halt();
        prueba_clase();
        prueba_flujo();
        prueba_cortos();
        prueba_desconexion();
        prueba_setup_encima();
        prueba_prisa();
        prueba_aud9();
        OK(hst.excesos == 0 && hst.mal_uso == 0 && hst.toggles_mal == 0, "host: excesos %u, mal uso %u, conmutadores mal %u", hst.excesos, hst.mal_uso, hst.toggles_mal);
    }
    { UsbInfo i; usb_info(&i);
      printf("usb: %u resets, %u setups, %u stalls, %u B recibidos, %u B enviados, %u contenedores, %u avisos; host: %u controles, %u paquetes IN (%u ZLP), %u STALL vistos\n",
             i.resets, i.setups, i.stalls, i.rx_bytes, i.tx_bytes, i.contenedores_in, i.avisos, hst.controles, hst.paquetes_in, hst.zlps_in, hst.stalls_vistos); }
    printf("%s: %d comprobaciones, %d mal\n", fallos ? "FALLA" : "BIEN", comprobaciones, fallos);
    return fallos != 0;
}
