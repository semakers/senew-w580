/* pc_usb_host.c — (MTP, ronda 3) SOLO PARA EL PC, en la variante «make pc-usb» (build/pc_usb/sistema_pc): el sistema ENTERO
 * con el controlador USB falso de USBD (herr/pruebas/usb_dc_falso.c) y un host MTP mínimo detrás, manejado desde el guion:
 *   usbhost enumerar        reset, SET_ADDRESS, SET_CONFIGURATION 1 (el teléfono pasa a «configurado»: pregunta el permiso)
 *   usbhost abrir           OpenSession(1); imprime «usbhost abrir -> 0xNNNN»
 *   usbhost storage         GetStorageIDs; imprime el código
 *   usbhost cerrar          CloseSession
 *   usbhost quitar          VBUS fuera (el cable)
 *   usbhost estado TEXTO    una línea: configurado, permiso, sesión, rutas de la interfaz y si la cima es la pantalla modal
 *   usbhost sen ...         (Fase 3b) el canal de gestión de SENUS, a bytes crudos: info, leer, poner K T V [K T V…], texto K TXT, reloj UTC ZONA,
 *                           vincular ID, bl, bt, apps; y «usbhost real», lo que hay de verdad en `aj`, para cotejar sin pasar por el canal
 * Sirve para ver, con la interfaz de verdad, la pregunta del permiso y la pantalla modal (docs/mtp.md §ronda 3). */
#include <stdio.h>
#include <string.h>
#include "shell.h"
#include "usb.h"
#include "mtp.h"
#include "usb_host.h"
extern u32 pc_reloj_ms;
void sh_usb_paso(int en_app);
static void bomba(void) { pc_reloj_ms += 1; sh_usb_paso(0); }
static u32 tid = 1;
static int ctl(u8 rt, u8 rq, u16 v)
{
    u8 s[8] = { rt, rq, (u8)v, (u8)(v >> 8), 0, 0, 0, 0 }, d[8]; int n = 0;
    return hst_control(s, d, &n);
}
static u32 g32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
/* una orden sin datos; devuelve el código de respuesta (0 = no llegó) */
static u32 orden(u16 op, u32 par, int np)
{
    u8 c[16], in[4096]; u32 hay = 0, o = 0; int k, vueltas;
    c[0] = (u8)(12 + 4 * np); c[1] = c[2] = c[3] = 0; c[4] = 1; c[5] = 0; c[6] = (u8)op; c[7] = (u8)(op >> 8);
    c[8] = (u8)tid; c[9] = (u8)(tid >> 8); c[10] = c[11] = 0;
    c[12] = (u8)par; c[13] = (u8)(par >> 8); c[14] = (u8)(par >> 16); c[15] = (u8)(par >> 24);
    tid++;
    if (hst_bulk_out(USB_EP_BULK_OUT, c, 12u + 4u * (u32)np, 0) < 0) return 0;
    for (vueltas = 0; vueltas < 200; vueltas++) {
        k = hst_bulk_in(USB_EP_BULK_IN, in + hay, sizeof in - hay);
        if (k > 0) hay += (u32)k;
        while (o + 12 <= hay) {
            u32 L = g32(in + o);
            if (L < 12 || o + L > hay) break;
            if (in[o + 4] == 3) return (u32)in[o + 6] | (u32)in[o + 7] << 8;
            o += L;
        }
        bomba();
    }
    return 0;
}
/* (Fase 3b) una transacción entera, con datos de la computadora o sin ellos: guarda la fase de datos y los parámetros de la respuesta */
static u8 sen_dat[4096]; static u32 sen_n, sen_par[3], sen_np;
static u32 trans(u16 op, u32 par, int np, const u8 *d, u32 n)
{
    static u8 in[8192], c[2048]; u32 hay = 0, o = 0, t = tid++; int k, vueltas;
    u32 L = 12u + 4u * (u32)np;
    memset(c, 0, 12); c[0] = (u8)L; c[4] = 1; c[6] = (u8)op; c[7] = (u8)(op >> 8); c[8] = (u8)t; c[9] = (u8)(t >> 8);
    c[12] = (u8)par; c[13] = (u8)(par >> 8); c[14] = (u8)(par >> 16); c[15] = (u8)(par >> 24);
    if (hst_bulk_out(USB_EP_BULK_OUT, c, L, 0) < 0) return 0;
    if (d) {
        L = 12u + n; memset(c, 0, 12); c[0] = (u8)L; c[1] = (u8)(L >> 8); c[4] = 2; c[6] = (u8)op; c[7] = (u8)(op >> 8); c[8] = (u8)t; c[9] = (u8)(t >> 8);
        memcpy(c + 12, d, n);
        if (hst_bulk_out(USB_EP_BULK_OUT, c, L, 0) < 0) return 0;
    }
    sen_n = 0; sen_np = 0;
    for (vueltas = 0; vueltas < 2000; vueltas++) {
        k = hst_bulk_in(USB_EP_BULK_IN, in + hay, sizeof in - hay);
        if (k > 0) hay += (u32)k;
        while (o + 12 <= hay) {
            u32 l = g32(in + o), i;
            if (l < 12 || o + l > hay) break;
            if (in[o + 4] == 2) { sen_n = l - 12 < sizeof sen_dat ? l - 12 : sizeof sen_dat; memcpy(sen_dat, in + o + 12, sen_n); }
            if (in[o + 4] == 3) { sen_np = (l - 12) / 4; for (i = 0; i < sen_np && i < 3; i++) sen_par[i] = g32(in + o + 12 + 4 * i); return (u32)in[o + 6] | (u32)in[o + 7] << 8; }
            o += l;
        }
        bomba();
    }
    return 0;
}
static u32 g16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static void sen_orden(char *l)
{
    int a, b, c; char t[128]; u8 d[1100]; u32 k, i, r;
    if (!strncmp(l, "info", 4)) {
        r = trans(0x9400, 0, 0, 0, 0);
        printf("usbhost sen info -> 0x%04x %u B formato=%u tam=%u build=%u api=%u.%u bl=%u bateria=%u banderas=0x%02x version=%s modelo=%s soc=%s compilacion=[%s] id=[%s] aj=%u\n", r, sen_n, g16(sen_dat), g16(sen_dat + 2), g32(sen_dat + 4),
               g16(sen_dat + 8), g16(sen_dat + 10), g16(sen_dat + 12), sen_dat[14], sen_dat[15], sen_dat + 24, sen_dat + 40, sen_dat + 48, sen_dat + 56, sen_dat + 80, g32(sen_dat + 96));
    } else if (!strncmp(l, "leer", 4)) {
        r = trans(0x9401, 0, 0, 0, 0);
        printf("usbhost sen leer -> 0x%04x %u B n=%u\n", r, sen_n, sen_n ? g16(sen_dat + 2) : 0);
        for (k = 4, i = 0; sen_n && i < g16(sen_dat + 2); i++) {
            u32 ti = sen_dat[k + 2], la = sen_dat[k + 3];
            if (ti == 4) printf("usbhost aj 0x%04x tipo=%u largo=%u min=%d max=%d v=[%.*s]\n", g16(sen_dat + k), ti, la, (short)g16(sen_dat + k + 4), (short)g16(sen_dat + k + 6), (int)la, sen_dat + k + 8);
            else printf("usbhost aj 0x%04x tipo=%u largo=%u min=%d max=%d v=%d\n", g16(sen_dat + k), ti, la, (short)g16(sen_dat + k + 4), (short)g16(sen_dat + k + 6), ti == 3 ? (short)g16(sen_dat + k + 8) : sen_dat[k + 8]);
            k += 8 + la;
        }
    } else if (!strncmp(l, "poner ", 6)) {
        int n = 0, en = 0, nb; char *p = l + 6;
        k = 4;
        while (sscanf(p, "%i %i %i%n", &a, &b, &c, &nb) == 3) {
            d[k] = (u8)a; d[k + 1] = (u8)(a >> 8); d[k + 2] = (u8)b; d[k + 3] = b == 3 ? 2 : 1; d[k + 4] = (u8)c; d[k + 5] = (u8)(c >> 8); k += 4u + d[k + 3]; p += nb; n++; en++;
        }
        d[0] = 1; d[1] = 0; d[2] = (u8)n; d[3] = 0;
        r = trans(0x9402, 0, 0, d, k);
        printf("usbhost sen poner -> 0x%04x aplicadas=%u fuera=%u desconocidas=%u\n", r, sen_par[0], sen_par[1], sen_par[2]);
    } else if (sscanf(l, "texto %i %127s", &a, t) == 2) {
        u32 n = (u32)strlen(t);
        if (!strcmp(t, "-")) n = 0;
        d[0] = 1; d[1] = 0; d[2] = 1; d[3] = 0; d[4] = (u8)a; d[5] = (u8)(a >> 8); d[6] = 4; d[7] = (u8)n; memcpy(d + 8, t, n);
        r = trans(0x9402, 0, 0, d, 8 + n);
        printf("usbhost sen texto -> 0x%04x aplicadas=%u fuera=%u desconocidas=%u\n", r, sen_par[0], sen_par[1], sen_par[2]);
    } else if (sscanf(l, "reloj %u %i", &k, &a) == 2) {
        d[0] = (u8)k; d[1] = (u8)(k >> 8); d[2] = (u8)(k >> 16); d[3] = (u8)(k >> 24); d[4] = (u8)a; d[5] = (u8)(a >> 8); d[6] = d[7] = 0;
        printf("usbhost sen reloj -> 0x%04x\n", trans(0x9403, 0, 0, d, 8));
    } else if (sscanf(l, "vincular %127s", t) == 1) {
        memset(d, 0, 16); memcpy(d, t, strlen(t) < 16 ? strlen(t) : 16);
        printf("usbhost sen vincular -> 0x%04x\n", trans(0x9404, 0, 0, d, 16));
    } else if (!strncmp(l, "bl", 2)) printf("usbhost sen bl -> 0x%04x\n", trans(0x9405, 0, 0, 0, 0));
    else if (!strncmp(l, "emparejar", 9)) {        /* SEN_BT_EMPAREJAR con un dataset que vale: en el PC no hay radio */
        u8 d[24] = { 0xDC, 0xA6, 0x32, 1, 2, 3, 4, '1', '2', '3', '4' };
        printf("usbhost sen emparejar -> 0x%04x\n", trans(0x9407, 0, 0, d, sizeof d));
    } else if (!strncmp(l, "bdaddr", 6)) {
        r = trans(0x9400, 0, 0, 0, 0);
        printf("usbhost sen bdaddr %02X:%02X:%02X:%02X:%02X:%02X -> 0x%04x\n", sen_dat[16], sen_dat[17], sen_dat[18], sen_dat[19], sen_dat[20], sen_dat[21], r);
    } else if (!strncmp(l, "bt", 2)) {
        r = trans(0x9406, 0, 0, 0, 0);
        printf("usbhost sen bt -> 0x%04x %u B encendido=%u puente=%u red=%u fallo=%u\n", r, sen_n, sen_dat[0], sen_dat[1], sen_dat[2], sen_dat[3]);
    } else if (!strncmp(l, "serie", 5)) {
        r = trans(0x1001, 0, 0, 0, 0);
        k = 8; k += 1u + 2u * sen_dat[k]; k += 2;
        for (i = 0; i < 4; i++) k += 4u + 2u * g32(sen_dat + k);
        k += 4u + 2u * g32(sen_dat + k);
        k += 1u + 2u * sen_dat[k]; k += 1u + 2u * sen_dat[k];
        printf("usbhost sen serie -> 0x%04x version=", r);
        for (i = 0; i + 1 < sen_dat[k]; i++) putchar(sen_dat[k + 1 + 2 * i]);
        k += 1u + 2u * sen_dat[k];
        printf(" serie=");
        for (i = 0; i + 1 < sen_dat[k]; i++) putchar(sen_dat[k + 1 + 2 * i]);
        printf("\n");
    } else if (!strncmp(l, "apps", 4)) {
        u32 desde = 0, ocup = 0, total = 0;
        do {
            r = trans(0x9408, desde, 1, 0, 0);
            if (r == 0x2019) { ocup++; continue; }
            if (r != 0x2001) break;
            total = g16(sen_dat + 2); k = g16(sen_dat + 4);
            for (i = 0; i < k; i++) { const u8 *e = sen_dat + 8 + i * 128;
                printf("usbhost app origen=%u banderas=%u perfil=%u api=%u.%u ver=%u tam=%u nombre=[%s] fichero=[%s]\n", e[0], e[1], e[2], g16(e + 6), g16(e + 8), g32(e + 12), g32(e + 16), e + 44, e + 74); }
            desde += k;
        } while (ocup < 5000 && (r == 0x2019 || (k && desde < total)));
        printf("usbhost sen apps -> 0x%04x total=%u dadas=%u ocupado=%u\n", r, total, desde, ocup);
    } else fprintf(stderr, "usbhost sen: orden desconocida: %s", l);
}
int pc_guion_extra(char *lin);
int pc_guion_extra(char *lin)
{
    static int puesto;
    char a[64];
    if (strncmp(lin, "usbhost ", 8)) return -1;
    if (!puesto) { hst_bomba = bomba; puesto = 1; }
    if (!strncmp(lin + 8, "enumerar", 8)) {
        int r; u32 i;
        for (i = 0; i < 400 && !hst_pullup(); i++) bomba();     /* el teléfono arranca el USB al ver VBUS */
        hst_reset(); bomba();
        r = ctl(0x00, 5, 7); bomba();
        r |= ctl(0x00, 9, 1); bomba();
        printf("usbhost enumerar -> %s (configurado %d)\n", r ? "MAL" : "bien", usb_conectado());
    } else if (!strncmp(lin + 8, "abrir", 5)) printf("usbhost abrir -> 0x%04x\n", orden(0x1002, 1, 1));
    else if (!strncmp(lin + 8, "storage", 7)) printf("usbhost storage -> 0x%04x\n", orden(0x1004, 0, 0));
    else if (!strncmp(lin + 8, "cerrar", 6)) printf("usbhost cerrar -> 0x%04x\n", orden(0x1003, 0, 0));
    else if (!strncmp(lin + 8, "quitar", 6)) { hst_vbus(0); printf("usbhost quitar\n"); }
    else if (!strncmp(lin + 8, "sen ", 4)) sen_orden(lin + 12);
    else if (!strncmp(lin + 8, "real", 4)) {
        HwFecha f; hw_rtc_leer(&f);
        printf("usbhost real tema=%d hora12=%d alarma=%d %d:%d zona=%d brillo=%d atenuar=%d apagar=%d bloqueo=%d tapa=%d volumen=%d tono=%d pasos=%d ahorro=%d estilo=%d efecto=%d intensidad=%d ejes=%d luces=%d color=%d latido=%d propio=%d giro=%d medios=%d fondo=[%s] rtc=%04d-%02d-%02d %02d:%02d\n",
               aj.tema, aj.hora12, aj.alarma_on, aj.alarma_h, aj.alarma_m, aj.zona_min, aj.brillo, aj.atenuar_s, aj.apagar_s, aj.bloqueo_auto, aj.tapa_apaga, aj.volumen, aj.tono_teclas, aj.pasos, aj.pasos_ahorro,
               aj.menu_estilo, aj.menu_efecto, aj.menu_intensidad, aj.menu_ejes, aj.luces, aj.luces_color, aj.luces_latido, aj.menu_propio, aj.giro, aj.medios_color, aj.fondo, f.anio, f.mes, f.dia, f.hora, f.min);
    }
    else if (sscanf(lin + 8, "estado %63s", a) == 1) {
        MtpInfo mi; mtp_info(&mi);
        printf("usbhost estado %-12s configurado=%d permiso=%d sesion=%d pide=%d rutas=%d emergente=%d\n", a, usb_conectado(), mtp_permiso(), mi.sesion,
               mi.pide_permiso, ui_rutas(), ui_cima_emergente());
    } else { fprintf(stderr, "usbhost: orden desconocida: %s", lin); return -1; }
    return 0;
}
