/* bt_w580.c — (pista BT) EL CONTROLADOR BLUETOOTH DEL W580i para el sistema (shell/bt_sis.h, docs/sistema.md §30).
 *
 * Debajo, SIN COPIAR: bt/servicio/bt_servicio.c (el servicio), bt/chipset/btstack_chipset_stlc2500c.c, bt/port/hal_uart_bloques.c,
 * bt/port/bt_hw_w580.c (UART 3, nRESET en SYSCON+04 bit 14, P2.0) y BTstack 1.8.3 (bt/terceros/btstack), compilados desde su
 * sitio por el Makefile (BT=1, solo el teléfono W580). Lo que pone ESTE fichero, que el servicio deja al sistema:
 *   - de dónde salen los parches: /tarjeta/BT_PatchInfo.txt si existe (como el original), si no /original/ifs/corrections (el FS
 *     de fábrica, api/fsfab: sin distinguir mayúsculas, que el índice dice «.ssf» y el fichero es «.SSF»);
 *   - la BD_ADDR propia: el sistema NO sabe leer el GDFS (no hay lector: docs/arranque.md §6 solo describe los bloques), así que
 *     se usa la guardada en el almacén (NV_BT) o, si no hay, una ALEATORIA «localmente administrada» 02:53:4E:xx:xx:xx que se
 *     queda fija la primera vez que se empareja (se escribe con la clave: el robot la recuerda por esa dirección);
 *   - las claves de enlace (4, como el servicio) y el último robot, en el almacén, clave NV_BT = 0x0500. Es lo ÚNICO que se escribe
 *     en la NOR, y solo al EMPAREJAR (Link_Key_Notification), al «Olvidar emparejados» de la pantalla de diagnóstico o cuando
 *     el robot RECHAZA la clave guardada (Authentication_Complete 0x05/0x06: se olvida esa y se vuelve a pedir PIN);
 *   - la velocidad final: 115 200 por omisión (no se sube a 921 600 hasta que el aparato lo confirme); la pantalla la cambia para el
 *     PRÓXIMO encendido, solo en RAM;
 *   - el diagnóstico paso a paso (ShBtDiag) y su eco por la UART 0 («[bt] …»), y los errores de BTstack por la misma UART.
 * DORMIDO: nada de aquí corre en el arranque. La primera llamada es drv_encender(), desde una app o desde Ajustes -> «Bluetooth». */
#include "hw.h"
#include "hal.h"                                            /* gpio_get: P2.2 (aparato) */
#include "shell.h"
#include "bt_sis.h"
#include "bt_servicio.h"
#include "bt_hw.h"
#include "btstack_config.h"
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_util.h"
#include "hci.h"
#include "gap.h"
#include "hci_dump.h"
#include <stdarg.h>
#ifndef BT_BAUDIOS_OMISION
#define BT_BAUDIOS_OMISION 1843200u                          /* (velocidad) entre el teléfono y el chip si nadie pide otra: la máxima de la
                                                              * escala rápida (divisor 1), medida en el aparato (docs/bluetooth.md §11) */
#endif

int bt_indice_buscar(const char *txt, unsigned n, uint16_t rev, uint16_t sub, char *nombre, unsigned max);   /* bt/servicio/bt_indices.c */
extern uint32_t bt_hal_tx_bytes, bt_hal_rx_bytes, bt_hal_rx_tirados;                                      /* bt/port/hal_uart_bloques.c */
extern uint32_t bt_hw_rx_errores;                                                                         /* bt/port/bt_hw_w580.c */

/* ---- el registro por la UART 0 ---------------------------------------------------------------------------------------------- */
static char lin[256];
static void cat(char **p, const char *s) { while (*s && *p < lin + sizeof lin - 2) *(*p)++ = *s++; }
static void num(char **p, u32 v, unsigned base, int ancho, int cero)
{
    char d[12]; int k = 0;
    do { d[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v && k < 11);
    while (ancho-- > k && *p < lin + sizeof lin - 2) *(*p)++ = cero ? '0' : ' ';
    while (k && *p < lin + sizeof lin - 2) *(*p)++ = d[--k];
}
/* printf mínimo para los textos de BTstack (log_error) y los nuestros: %d %i %u %x %X %p %s %c, «0», ancho y «l» (se ignora) */
static void vlog(const char *pre, const char *f, va_list ap)
{
    char *p = lin;
    cat(&p, pre);
    for (; *f && p < lin + sizeof lin - 2; f++) {
        int cero = 0, ancho = 0;
        if (*f != '%') { *p++ = *f; continue; }
        f++;
        if (*f == '0') { cero = 1; f++; }
        while (*f >= '0' && *f <= '9') ancho = ancho * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'h' || *f == 'z') f++;
        if (!*f) break;
        switch (*f) {
        case 's': { const char *s = va_arg(ap, const char *); cat(&p, s ? s : "(null)"); break; }
        case 'c': *p++ = (char)va_arg(ap, int); break;
        case 'd': case 'i': { int v = va_arg(ap, int); if (v < 0) { *p++ = '-'; v = -v; } num(&p, (u32)v, 10, ancho, cero); break; }
        case 'u': num(&p, va_arg(ap, unsigned), 10, ancho, cero); break;
        case 'x': case 'X': num(&p, va_arg(ap, unsigned), 16, ancho, cero); break;
        case 'p': cat(&p, "0x"); num(&p, (u32)(size_t)va_arg(ap, void *), 16, 8, 1); break;
        default: *p++ = *f; break;
        }
    }
    *p++ = '\n'; *p = 0;
    hw_log(lin);
}
static void logf_(const char *f, ...) { va_list ap; va_start(ap, f); vlog("[bt] ", f, ap); va_end(ap); }

/* BTstack -> UART 0: sus log_error siempre; los paquetes, solo con la traza encendida (pantalla de diagnóstico) */
static u8 traza;
static void dump_msg(int nivel, const char *f, va_list ap) { (void)nivel; vlog("[btstack] ", f, ap); }
static void dump_paq(uint8_t tipo, uint8_t entra, uint8_t *p, uint16_t n)
{
    char *q = lin; unsigned i;
    if (!traza) return;
    cat(&q, entra ? "[hci] < " : "[hci] > ");
    num(&q, tipo, 16, 2, 1); cat(&q, " |");
    for (i = 0; i < n && i < 24; i++) { cat(&q, " "); num(&q, p[i], 16, 2, 1); }
    if (n > 24) { cat(&q, " … ("); num(&q, n, 10, 0, 0); cat(&q, " B)"); }
    *q++ = '\n'; *q = 0;
    hw_log(lin);
}
static const hci_dump_t mi_dump = { 0, dump_paq, dump_msg };

/* ---- 1. los parches: índice y fichero, de la tarjeta o del FS de fábrica ---------------------------------------------------- */
static void strcat_(char *d, const char *s, unsigned cap)
{
    unsigned n = 0;
    while (n < cap && d[n]) n++;
    while (*s && n + 1 < cap) d[n++] = *s++;
    if (n < cap) d[n] = 0;
}
static char origen[32];
static struct { u8 mirado, hay; u16 rev, sub; char ruta[112]; } fich[2];
static char indice[2048];
static void elegir_origen(void)
{
    /* (aparato, 2026-10-25) la TARJETA manda si trae el índice (como /card/ en el original); si no, la MEMORIA de senew
     * (/memoria/bluetooth: en la NOR, no depende de que la Memory Stick monte —en el aparato a veces no lo hace—, se pone por MTP o
     * con herr/puente/bt/memoria_escribir.py); y si tampoco, el FS de fábrica (que api/fsfab no monta en un teléfono USADO). */
    const char *d = hw_fs_tam("/tarjeta/BT_PatchInfo.txt") ? "/tarjeta"
                  : hw_fs_tam("/memoria/bluetooth/BT_PatchInfo.txt") ? "/memoria/bluetooth" : "/original/ifs/corrections";
    unsigned i;
    for (i = 0; d[i] && i < sizeof origen - 1; i++) origen[i] = d[i];
    origen[i] = 0;
    fich[0].mirado = fich[1].mirado = 0;
}
static void resolver(int cual, u16 rev, u16 sub)
{
    static const char *const idx[2] = { "BT_PatchInfo.txt", "BT_FactorySettingsInfo.txt" };   /* en el FS: «BT_FactorysettingsInfo.txt» */
    char ruta[112], nombre[64];
    u32 n;
    fich[cual].mirado = 1; fich[cual].hay = 0; fich[cual].rev = rev; fich[cual].sub = sub;
    memcpy(ruta, origen, sizeof origen); ruta[sizeof origen - 1] = 0;
    strcat_(ruta, "/", sizeof ruta); strcat_(ruta, idx[cual], sizeof ruta);
    n = hw_fs_leer(ruta, (u8 *)indice, sizeof indice);
    if (!n) { logf_("%s: no esta (sin %s)", ruta, cual ? ".ssf" : ".ptc"); return; }
    if (!bt_indice_buscar(indice, n, rev, sub, nombre, sizeof nombre)) { logf_("%s: nada para HCI 0x%04x / LMP 0x%04x", ruta, rev, sub); return; }
    memcpy(fich[cual].ruta, origen, sizeof origen); fich[cual].ruta[sizeof origen - 1] = 0;
    strcat_(fich[cual].ruta, "/", sizeof fich[cual].ruta); strcat_(fich[cual].ruta, nombre, sizeof fich[cual].ruta);
    n = hw_fs_tam(fich[cual].ruta);
    if (!n) { logf_("%s: el indice lo nombra y no esta", fich[cual].ruta); return; }
    fich[cual].hay = 1;
    logf_("5. %s: %s (%u B)", cual ? "ajustes" : "parche", fich[cual].ruta, n);
}
static int leer_corr(int cual, uint16_t rev, uint16_t sub, uint32_t off, uint8_t *buf, uint16_t max)
{
    if (cual < 0 || cual > 1) return -1;
    if (!fich[cual].mirado || fich[cual].rev != rev || fich[cual].sub != sub) resolver(cual, rev, sub);
    if (!fich[cual].hay) return -1;
    return (int)hw_fs_leer_desde(fich[cual].ruta, off, buf, max);
}

/* ---- 2. lo que se guarda: NV_BT = [1][banderas][addr 6][recordado 6][n][n x (addr 6, clave 16, tipo 1)] ---------------------- */
#define CLAVES 4
#define NV_TAM (15 + 1 + CLAVES * 23)
typedef struct { u8 addr[6], clave[16], tipo; } Clave;
static struct { u8 cargado, hay_addr, hay_rec, n, pendiente; u8 addr[6], rec[6]; Clave c[CLAVES]; } nv;
static void nv_cargar(void)
{
    u8 b[NV_TAM]; int n, i;
    if (nv.cargado) return;
    nv.cargado = 1;
    n = hw_cargar(NV_BT, b, sizeof b);
    /* 15 B = sin claves (lo que deja «Olvidar emparejados», o una dirección propia sola): antes pedía 16 y la dirección guardada
     * se perdía al reiniciar (2026-10-25, leyendo para escribir la BD_ADDR del GDFS) */
    if (n < 15 || b[0] != 1) {
        char t[24];
        if (n > 0) logf_("almacen: registro NV_BT que no entiendo (%d B): se ignora", n);
        /* (E5, 27-oct) SIN ALMACÉN (senew en RAM: la NOR no se escribe y NV_BT no está) la dirección PROPIA puede venir de la tarjeta:
         * «/tarjeta/bt_addr.txt» = «AA:BB:CC:DD:EE:FF» (p. ej. la de fábrica del teléfono, la que ya conocen el ordenador y los robots). Sin claves: se emparejan otra vez y
         * viven solo en la RAM. Con una aleatoria, el ordenador puente rechaza la llamada (HCI 0x0F) [observado 27-oct]. */
        if (hw_fs_leer("/tarjeta/bt_addr.txt", (u8 *)t, sizeof t) >= 17) {
            for (i = 0; i < 6; i++) {
                int h = t[3 * i], l = t[3 * i + 1];
                h = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                l = l >= '0' && l <= '9' ? l - '0' : l >= 'a' && l <= 'f' ? l - 'a' + 10 : l >= 'A' && l <= 'F' ? l - 'A' + 10 : -1;
                if (h < 0 || l < 0 || (i < 5 && t[3 * i + 2] != ':')) break;
                nv.addr[i] = (u8)(h << 4 | l);
            }
            if (i == 6) { nv.hay_addr = 1; logf_("almacen: sin NV_BT; la direccion propia, de /tarjeta/bt_addr.txt"); }
        }
        return;
    }
    nv.hay_addr = b[1] & 1; nv.hay_rec = (b[1] >> 1) & 1;
    memcpy(nv.addr, b + 2, 6); memcpy(nv.rec, b + 8, 6);
    nv.n = b[14] > CLAVES ? CLAVES : b[14];
    if (n < 15 + nv.n * 23) nv.n = (u8)((n - 15) / 23);
    for (i = 0; i < nv.n; i++) { memcpy(nv.c[i].addr, b + 15 + i * 23, 6); memcpy(nv.c[i].clave, b + 21 + i * 23, 16); nv.c[i].tipo = b[37 + i * 23]; }
}
static void nv_guardar(void)
{
    u8 b[NV_TAM]; int i, r;
    b[0] = 1; b[1] = (u8)(nv.hay_addr | (nv.hay_rec << 1));
    memcpy(b + 2, nv.addr, 6); memcpy(b + 8, nv.rec, 6); b[14] = nv.n;
    for (i = 0; i < nv.n; i++) { memcpy(b + 15 + i * 23, nv.c[i].addr, 6); memcpy(b + 21 + i * 23, nv.c[i].clave, 16); b[37 + i * 23] = nv.c[i].tipo; }
    r = hw_guardar(NV_BT, b, 15u + nv.n * 23u);
    nv.pendiente = r != HW_OK;
    logf_(r == HW_OK ? "almacen: %u claves guardadas (NV_BT)" : "almacen: NO se pudo guardar ahora (%d); queda pendiente", r == HW_OK ? nv.n : (u32)r);
}
static void drv_guardar_pendiente(void) { if (nv.pendiente) nv_guardar(); }

/* ---- 3. la BD_ADDR propia ------------------------------------------------------------------------------------------------- */
static u8 mi_addr[6], addr_hecha;
static void addr_elegir(void)
{
    HwFecha f; u32 h = 2166136261u, x[4]; int i;
    if (nv.hay_addr) { memcpy(mi_addr, nv.addr, 6); addr_hecha = 1; return; }
    if (addr_hecha) return;                                  /* la aleatoria de este arranque, hasta que se empareje */
    hw_rtc_leer(&f);
    x[0] = hw_ms(); x[1] = hw_cuenta(); x[2] = hw_fecha_seg(&f); x[3] = bt_hal_tx_bytes ^ (u32)(size_t)&f;
    for (i = 0; i < 16; i++) { h ^= ((const u8 *)x)[i]; h *= 16777619u; }
    mi_addr[0] = 0x02; mi_addr[1] = 0x53; mi_addr[2] = 0x4E;  /* 0x02: localmente administrada, unicast; «SN» de senew */
    mi_addr[3] = (u8)(h >> 16); mi_addr[4] = (u8)(h >> 8); mi_addr[5] = (u8)h;
    if (mi_addr[3] == 0x9E && mi_addr[4] == 0x8B && mi_addr[5] < 0x40) mi_addr[3] = 0x9D;   /* fuera de los LAP de búsqueda (GIAC/DIAC) */
    addr_hecha = 1;
}

/* (0.29.1) La dirección que SENUS lee por USB antes de la llamada, para ponerla en sus permitidos. Sin encender nada: la del almacén si la hay. Si no, la
 * aleatoria se elige AQUÍ: es la misma que usará drv_encender en este arranque (addr_hecha) y queda fija al emparejar (clave_nueva). No se escribe en la NOR al leerla. */
static void drv_propia(u8 a[6]) { nv_cargar(); addr_elegir(); memcpy(a, mi_addr, 6); }

/* ---- 4. el diagnóstico: lo que se ve paso a paso ------------------------------------------------------------------------- */
static int drv_fallo(void);
static ShBtDiag dg;
static u32 t_suelto;
static i32 ms_cts;                                          /* (aparato) P2.2 = RTS del chip -> CTS nuestro, activo a 0: listo para recibir */
static u32 baud_final_sig = BT_BAUDIOS_OMISION;            /* la velocidad final del PRÓXIMO encendido (solo en RAM) */
static u8 ultimo_estado, ultimo_fallo;
static u8 destino[6], rec_ram[6], hay_rec_ram;
static u8 conexion_mal;                                     /* estado de Connection_Complete si falló (0x04 = page timeout: robot apagado) */
static btstack_packet_callback_registration_t reg;
static u8 registrado;
static void addr_txt(char *t, const u8 *a) { int i; for (i = 0; i < 6; i++) { t[i * 3] = "0123456789ABCDEF"[a[i] >> 4]; t[i * 3 + 1] = "0123456789ABCDEF"[a[i] & 15]; t[i * 3 + 2] = i < 5 ? ':' : 0; } }

static void clave_nueva(const u8 *a)
{
    link_key_t k; link_key_type_t t; bd_addr_t b; int i;
    memcpy(b, a, 6);
    if (!gap_get_link_key_for_bd_addr(b, k, &t)) return;     /* BTstack no la quiso guardar (nula, sin bonding): nosotros tampoco */
    for (i = 0; i < nv.n && memcmp(nv.c[i].addr, a, 6); i++) ;
    if (i == nv.n) {
        if (nv.n == CLAVES) { memmove(&nv.c[0], &nv.c[1], sizeof nv.c[0] * (CLAVES - 1)); i = CLAVES - 1; }   /* la más vieja fuera */
        else nv.n++;
    }
    memcpy(nv.c[i].addr, a, 6); memcpy(nv.c[i].clave, k, 16); nv.c[i].tipo = (u8)t;
    memcpy(nv.addr, mi_addr, 6); nv.hay_addr = 1;            /* desde ahora nuestra dirección es FIJA: el robot nos recuerda por ella */
    memcpy(nv.rec, a, 6); nv.hay_rec = 1; memcpy(rec_ram, a, 6); hay_rec_ram = 1;
    dg.addr_origen = SH_BT_ADDR_GUARDADA;
    nv_guardar();                                            /* LA escritura en la NOR, en el acto. Solo si una app ha TOMADO EL PANEL
                                                              * (juegos: hwo_claves_app) el almacén no deja: queda pendiente hasta que
                                                              * se descargue. Con una app de widgets se escribe ya. */
}

/* (clave) el robot rechazó la clave guardada (Authentication_Complete 0x05/0x06): el servicio ya la ha quitado de BTstack;
 * aquí, de NV_BT, para que no vuelva de la NOR en el próximo arranque. El «último robot» y la dirección propia se quedan. */
static void clave_rechazada(const u8 *a, u8 estado)
{
    char t[18]; int i;
    nv_cargar();
    for (i = 0; i < nv.n && memcmp(nv.c[i].addr, a, 6); i++) ;
    if (i == nv.n) return;                                   /* no había clave: era un PIN malo, nada que olvidar */
    memmove(&nv.c[i], &nv.c[i + 1], sizeof nv.c[0] * (unsigned)(nv.n - 1 - i));
    nv.n--;
    addr_txt(t, a);
    logf_("el robot %s rechazo la clave guardada (0x%02x): olvidada; se le pedira el PIN", t, estado);
    nv_guardar();
}

static void evento(uint8_t tipo, uint16_t canal, uint8_t *p, uint16_t n)
{
    (void)canal;
    if (tipo != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(p)) {
    case HCI_EVENT_COMMAND_COMPLETE: {
        u16 op = hci_event_command_complete_get_command_opcode(p);
        const u8 *r = hci_event_command_complete_get_return_parameters(p);
        if (op == HCI_OPCODE_HCI_RESET && dg.hci_reset_estado < 0 && n >= 6) {
            dg.ms_hci_reset = (i32)(bt_hw_ms() - t_suelto); dg.hci_reset_estado = (signed char)r[0];
            logf_("3. HCI_Reset -> Command Complete, estado %u, a los %d ms", r[0], dg.ms_hci_reset);
        } else if (op == HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION && n >= 14 && r[0] == 0 && !dg.hay_version) {
            dg.hay_version = 1; dg.lmp_version = r[4]; dg.hci_rev = little_endian_read_16(r, 2);
            dg.fabricante = little_endian_read_16(r, 5); dg.lmp_subver = little_endian_read_16(r, 7);
            logf_("4. version: HCI_Revision 0x%04x, LMP %u / 0x%04x, fabricante 0x%04x", dg.hci_rev, dg.lmp_version, dg.lmp_subver, dg.fabricante);
        } else if (op == HCI_OPCODE_HCI_READ_BD_ADDR && n >= 12 && r[0] == 0) {
            char t[18];
            reverse_bd_addr(r + 1, dg.addr_leida); dg.hay_addr_leida = 1;
            addr_txt(t, dg.addr_leida); logf_("6. Read_BD_ADDR: %s", t);
        }
        break;
    }
    case HCI_EVENT_CONNECTION_COMPLETE:                      /* (apps de robots) el ACL hacia el robot: si no se abre, es «no se pudo conectar», no «sin SPP» */
        if (n >= 3 && p[2]) { conexion_mal = p[2]; logf_("el enlace no se abrio: estado 0x%02x%s", p[2], p[2] == 0x04 ? " (page timeout: el robot no contesta)" : ""); }
        break;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:                  /* (clave) el robot rechaza la clave guardada: fuera también del almacén */
        if (n >= 3 && (p[2] == ERROR_CODE_PIN_OR_KEY_MISSING || p[2] == ERROR_CODE_AUTHENTICATION_FAILURE)) clave_rechazada(destino, p[2]);
        break;
    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        if (n >= 25) { bd_addr_t a; reverse_bd_addr(p + 2, a); logf_("emparejado: clave de enlace nueva"); clave_nueva(a); }
        break;
    case HCI_EVENT_HARDWARE_ERROR:
        logf_("el chip dice HARDWARE ERROR 0x%02x", n > 2 ? p[2] : 0);
        break;
    default: break;
    }
}

static void diag_cero(void)
{
    memset(&dg, 0, sizeof dg);
    dg.ms_reset = dg.ms_p20 = dg.ms_hci_reset = -1; dg.hci_reset_estado = -1; dg.ptc = dg.ssf = -2; ms_cts = -1;
}

/* ---- 5. el controlador -------------------------------------------------------------------------------------------------- */
static int drv_encender(void)
{
    char t[18];
    if (bt_estado() != BT_APAGADO && bt_estado() != BT_FALLO) return -1;
    if (bt_estado() == BT_FALLO) bt_apagar();
    nv_cargar();
    addr_elegir();
    elegir_origen();
    diag_cero();
    dg.t_encender = hw_ms();
    memcpy(dg.addr, mi_addr, 6); dg.addr_origen = nv.hay_addr ? SH_BT_ADDR_GUARDADA : SH_BT_ADDR_ALEATORIA;
    dg.baudios_final = baud_final_sig;
    memcpy(dg.origen, origen, sizeof dg.origen);
    bt_config_ficheros(leer_corr);
    bt_config_bd_addr(mi_addr);
    bt_config_baudios(dg.baudios_final);
    if (!registrado) hci_dump_init(&mi_dump);                /* antes del primer encendido: los log_error de hci_init ya salen */
    if (bt_encender()) return -1;
    if (!registrado) {                                       /* BTstack ya está iniciado (bt_encender lo hace la primera vez) */
        int i; registrado = 1;
        reg.callback = &evento;
        hci_add_event_handler(&reg);
        gap_set_link_supervision_timeout(6400);              /* (apps de robots) 6400 x 0,625 ms = 4 s (BTstack: 20 s): un robot que se apaga se nota pronto.
                                                              * BTstack manda Write_Link_Supervision_Timeout él solo al abrir el ACL como maestro */
        for (i = 0; i < nv.n; i++) { bd_addr_t a; memcpy(a, nv.c[i].addr, 6); gap_store_link_key_for_bd_addr(a, nv.c[i].clave, (link_key_type_t)nv.c[i].tipo); }
        if (nv.hay_rec) { memcpy(rec_ram, nv.rec, 6); hay_rec_ram = 1; }
    }
    t_suelto = bt_hw_ms(); ultimo_estado = BT_ENCENDIENDO; ultimo_fallo = BT_OK;
    addr_txt(t, mi_addr);
    logf_("1. encender: nRESET a 0 (SYSCON+04 bit 14), 100 ms; BD_ADDR %s (%s); velocidad final %u; parches de %s",
          t, nv.hay_addr ? "guardada" : "aleatoria, se fija al emparejar", dg.baudios_final, origen);
    return 0;
}
static void drv_apagar(void) { bt_apagar(); ultimo_estado = BT_APAGADO; }
static void drv_paso(void)
{
    u32 ahora;
    bt_paso();
    ahora = bt_hw_ms();
    if (bt_estado() == BT_ENCENDIENDO) {
        if (dg.ms_reset < 0 && ahora - dg.t_encender >= 100) {   /* bt_paso suelta el reset en la primera vuelta tras 100 ms: esta */
            /* (el reloj de bt_hw_ms y hw_ms es el mismo tic; la diferencia es el grano de 4 ms) */
            dg.ms_reset = (i32)(ahora - (u32)dg.t_encender); t_suelto = ahora;
            logf_("1. nRESET suelto a los %d ms; abierta la UART 3 a 115200 8N1 con RTS/CTS", dg.ms_reset);
        }
        if (dg.ms_reset >= 0 && dg.ms_p20 == -1 && bt_hw_vivo()) {
            dg.ms_p20 = (i32)(ahora - t_suelto);
            logf_("2. P2.0 alto (el chip vive) a los %d ms de soltar el reset", dg.ms_p20);
        }
        /* [observado en el aparato, 2026-10-25] P2.0 sube en cuanto se suelta el reset; lo que dice «ya escucho» es P2.2 (su RTS) a
         * 0, ~56 ms después. Con RTS/CTS (+16 = 0xC0) el HCI_Reset espera solo en la FIFO hasta entonces: solo se apunta. */
        if (dg.ms_reset >= 0 && ms_cts < 0 && !gpio_get(2, 0x04)) {
            ms_cts = (i32)(ahora - t_suelto);
            logf_("2. P2.2 (CTS: el chip acepta datos) a 0 a los %d ms de soltar el reset", ms_cts);
        }
    }
    if (bt_estado() != ultimo_estado || drv_fallo() != ultimo_fallo) {
        const bt_info_t *in = bt_info();
        ultimo_estado = (u8)bt_estado(); ultimo_fallo = (u8)drv_fallo();
        if (ultimo_estado == BT_LISTO && dg.ptc == -2) {
            dg.ptc = in->ptc; dg.ssf = in->ssf; dg.baudios = in->baudios;
            {   /* [observado en el aparato] la cadena de 0xFC0F empieza por «\r\n » («\r\n STLC2500 R4.2 12102005 12:40:14 ZAV1000…»):
                 * fuera los caracteres de control y espacios del principio; los de en medio, a espacio (la pantalla pintaba «??») */
                const char *r = in->revision; unsigned k = 0;
                while (*r && (u8)*r <= ' ' && r < in->revision + sizeof in->revision) r++;
                for (; *r && k < sizeof dg.revision - 1 && r < in->revision + sizeof in->revision; r++) dg.revision[k++] = (u8)*r < ' ' ? ' ' : *r;
                dg.revision[k] = 0;
            }
            logf_("4. revision (0xFC0F): %s", dg.revision[0] ? dg.revision : "(vacia)");
            logf_("5. parche .ptc: %s; ajustes .ssf: %s", in->ptc > 0 ? "cargado" : in->ptc < 0 ? "FALLO" : "no habia",
                  in->ssf > 0 ? "cargados" : in->ssf < 0 ? "FALLO" : "no habia");
            logf_("7. velocidad: %u baudios. LISTO en %u ms", in->baudios, ahora - (u32)dg.t_encender);
            logf_("   UART 3: %u B enviados, %u recibidos; tirados: %u con marca de error, %u que no eran tipo H4",
                  bt_hal_tx_bytes, bt_hal_rx_bytes, bt_hw_rx_errores, bt_hal_rx_tirados);
        }
        if (ultimo_estado == BT_FALLO) {
            if (ultimo_fallo == BT_F_SIN_CHIP && dg.ms_p20 == -1) dg.ms_p20 = -2;
            logf_("FALLO %u: %s (UART 3: %u B enviados, %u recibidos, %u con error, %u no tipo H4)", ultimo_fallo, sh_bt_fallo_nombre(ultimo_fallo),
                  bt_hal_tx_bytes, bt_hal_rx_bytes, bt_hw_rx_errores, bt_hal_rx_tirados);
        } else logf_("estado: %s", sh_bt_estado_nombre(ultimo_estado));
        if (ultimo_estado == BT_CONECTADO) { memcpy(rec_ram, destino, 6); hay_rec_ram = 1; }   /* el último robot (en RAM; a la NOR solo al emparejar) */
    }
}
static int  drv_estado(void) { return bt_estado(); }
/* (apps de robots) bt_servicio.c da SIN_SPP cuando el robot está APAGADO (el fallo de la consulta SDP tapa el del enlace). Sin tocar el
 * servicio: si el Connection_Complete de este intento falló, el fallo es CONEXION. */
static int  drv_fallo(void) { int f = bt_fallo(); return f == BT_F_SIN_SPP && conexion_mal ? BT_F_CONEXION : f; }
static void drv_diag(ShBtDiag *d)
{
    static u8 hecho;
    if (!hecho && bt_estado() == BT_APAGADO && !dg.t_encender) { hecho = 1; diag_cero(); }   /* (aparato) antes del 1.er encendido la
                                                              * pantalla decía «P2.0 a los 0 ms» y «Command Complete (0)»: eran ceros */
    *d = dg;
    d->estado = (u8)bt_estado(); d->fallo = (u8)drv_fallo();
    if (d->estado != SH_BT_APAGADO) d->baudios = bt_info()->baudios ? bt_info()->baudios : 115200u;
    d->baudios_final = baud_final_sig;
    d->tx = bt_hal_tx_bytes; d->rx = bt_hal_rx_bytes;
    d->claves = nv.n; d->claves_pendientes = nv.pendiente;
    d->hay_recordado = hay_rec_ram; memcpy(d->recordado, rec_ram, 6);
}
static int  drv_buscar(unsigned s) { return bt_buscar(s); }
static int  drv_encontrados(void) { return bt_encontrados(); }
static int  drv_encontrado(int i, ShBtDisp *d)
{
    bt_disp_t x;
    if (bt_encontrado(i, &x)) return -1;
    memcpy(d->addr, x.addr, 6); d->clase = x.clase; memcpy(d->nombre, x.nombre, sizeof d->nombre); d->nombre[sizeof d->nombre - 1] = 0;
    return 0;
}
static void drv_pin(const char *p) { bt_pin(p && p[0] ? p : "1234"); }
static int  drv_emparejado(const u8 a[6])                   /* de NUESTRA tabla (la misma que la de BTstack): vale con la radio apagada */
{
    int i;
    nv_cargar();
    for (i = 0; i < nv.n; i++) if (!memcmp(nv.c[i].addr, a, 6)) return 1;
    return 0;
}
static void drv_olvidar_todo(void)
{
    int i;
    if (registrado) for (i = 0; i < nv.n; i++) bt_olvidar(nv.c[i].addr);
    nv_cargar();
    nv.n = 0; nv.hay_rec = 0; hay_rec_ram = 0;
    nv_guardar();                                            /* la dirección propia se queda (si se cambiara, los robots no nos reconocerían) */
}
static int  drv_conectar(const u8 a[6]) { memcpy(destino, a, 6); conexion_mal = 0; return bt_conectar_spp(a); }
static int  drv_enviar(const void *d, unsigned n) { return bt_enviar(d, n); }
static int  drv_pendiente(void) { return bt_pendiente(); }
static int  drv_recibir(void *d, unsigned m) { return bt_recibir(d, m); }
static void drv_desconectar(void) { if (bt_estado() != BT_APAGADO) bt_desconectar(); }
static int  drv_recordado(u8 a[6])
{
    nv_cargar();
    if (!hay_rec_ram && nv.hay_rec) { memcpy(rec_ram, nv.rec, 6); hay_rec_ram = 1; }
    if (!hay_rec_ram) return 0;
    memcpy(a, rec_ram, 6);
    return 1;
}
static void drv_velocidad(u32 b) { if (b == 115200u || b == 921600u || b == 1843200u) baud_final_sig = b; }   /* 0xFCFC 0x10 / 0x14 / 0x16 */
static int  drv_traza(int si) { if (si >= 0) traza = (u8)(si != 0); return traza; }

static const BtDriver drv = { .encender = drv_encender, .apagar = drv_apagar, .paso = drv_paso, .estado = drv_estado, .fallo = drv_fallo,
    .diag = drv_diag, .buscar = drv_buscar, .encontrados = drv_encontrados, .encontrado = drv_encontrado, .pin = drv_pin,
    .emparejado = drv_emparejado, .olvidar_todo = drv_olvidar_todo, .conectar_spp = drv_conectar, .enviar = drv_enviar,
    .pendiente = drv_pendiente, .recibir = drv_recibir, .desconectar = drv_desconectar, .recordado = drv_recordado,
    .velocidad = drv_velocidad, .guardar_pendiente = drv_guardar_pendiente, .traza = drv_traza, .propia = drv_propia };
const BtDriver *bt_driver(void) { return &drv; }

/* lo único de la libc que pide el servicio y el sistema no tenía (el sistema enlaza sin libc: plat/mem.c da memcpy y compañía) */
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
