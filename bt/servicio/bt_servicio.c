/* bt_servicio.c — el servicio Bluetooth de senew sobre BTstack (ver bt_servicio.h y bt/DISENO.md).
 *
 * Capas, de abajo arriba:
 *   bt_hw_*            el aparato (bt_hw_w580.c) o el PC (bt_hw_pty.c)
 *   hal_uart_bloques   hal_uart_dma.h por sondeo
 *   BTstack            run loop embebido, H4, HCI, L2CAP, SDP cliente, RFCOMM; chipset propio stlc2500c
 *   este fichero       encendido (reset, P2.0), velocidad final, búsqueda, PIN, SDP→RFCOMM, colas de bytes */
#include "bt_servicio.h"
#include "bt_hw.h"
#include "btstack_config.h"
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "bluetooth_sdp.h"
#include "hci.h"
#include "gap.h"
#include "l2cap.h"
#include "classic/rfcomm.h"
#include "classic/sdp_client.h"
#include "classic/sdp_client_rfcomm.h"
#include "btstack_run_loop_embedded.h"
#include "btstack_uart_block.h"
#include "hci_transport_h4.h"
#include "classic/btstack_link_key_db_memory.h"
#include <string.h>
#ifdef BT_CHIPSET_BTSTACK                       /* SOLO para la comparación del LEEME: el chipset/stlc2500d de BTstack tal
                                                   cual, con su orden (velocidad antes del parche, hci_set_bd_addr) */
#include "btstack_chipset_stlc2500d.h"
#endif

int  hal_uart_bloques_sondear(void);
int  hal_uart_bloques_tx_ocupado(void);

#define MAX_ENCONTRADOS 8
/* (velocidad, 2026-10-25) colas grandes: el SPP es también la salida a la red de las apps (el ordenador con SENUS como puente). Potencias de 2.
 * Antes 256 B las dos: una trama RFCOMM del puente (~330 B) ya no cabía en la de recepción y se PERDÍAN bytes. */
#ifndef BT_COLA_TX
#define BT_COLA_TX 8192u
#endif
#ifndef BT_COLA_RX
#define BT_COLA_RX 16384u                       /* 16 créditos de tramas de 1 KB: una app a 30 cuadros/s recibe ~2 veces más que con 8 KB */
#endif
#define MASCARA_TX (BT_COLA_TX - 1u)
#define MASCARA_RX (BT_COLA_RX - 1u)
typedef char comprobar_colas[((BT_COLA_TX & MASCARA_TX) == 0 && (BT_COLA_RX & MASCARA_RX) == 0 && BT_COLA_RX <= 32768u) ? 1 : -1];

/* ---- estado ---- */
static bt_estado_t estado;
static bt_fallo_t  fallo;
static bt_info_t   info;
static uint8_t     sub;                        /* subpaso dentro del estado */
static uint32_t    t0;
static uint8_t     iniciado_btstack;

static char        pin[17] = "1234";
static uint8_t     mi_addr[6] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 };   /* el «fijo» del original si GDFS no tiene */
static uint8_t     hay_mi_addr;
static uint32_t    baud_final = 921600;
static stlc2500c_leer_t lector;

static bt_disp_t   encontrados[MAX_ENCONTRADOS];
static struct { uint8_t psrm; uint16_t reloj; } encontrados_pag[MAX_ENCONTRADOS];
static uint8_t     n_encontrados, nombre_sig;

static bd_addr_t   destino;
static uint16_t    rfcomm_cid;
static uint16_t    marco_max;
static uint8_t     canal_spp;
static uint8_t     prio_spp;                   /* (3-nov) de quién es canal_spp: 2 el puente, 1 otro puerto serie (sdp_manejador) */
/* (0.29.3) EL PUENTE QUE LLAMA. macOS 26 no deja a una app ser servidor Bluetooth clásico: su registro SDP no se anuncia y su canal RFCOMM
 * no contesta [medido en el teléfono del mantenedor, macOS 26.5.1]; lo que sí deja es LLAMAR. Así que, si la red busca su puente y el SDP no
 * trae ninguno («W5RED» o «puente de red»), el teléfono no abre ningún otro puerto serie: se queda CONECTABLE con su canal 7 y espera a que
 * SENUS lo llame (bt_puente_red lo pone red_sis.c justo antes de conectar; vale para UNA conexión). Los robots, como siempre. */
#define CANAL_PROPIO 7
uint8_t            bt_puente_red;              /* lo pone shell/red_sis.c antes de cada conectar_spp del puente; se consume aquí */
static uint8_t     solo_puente, esperando;
static void dejar_de_esperar(void) { if (esperando) { esperando = 0; gap_connectable_control(0); } }
static hci_con_handle_t enlace = HCI_CON_HANDLE_INVALID;
static uint8_t     soltar_enlace;              /* bt_desconectar: al cerrarse el canal, cortar también el ACL */

static uint8_t     tx_cola[BT_COLA_TX]; static uint16_t tx_ini, tx_n;
static uint8_t     rx_cola[BT_COLA_RX]; static uint16_t rx_ini, rx_n;
uint32_t           bt_rx_perdidos;
/* (velocidad) CONTROL DE FLUJO de punta a punta con los créditos de RFCOMM: la otra punta solo puede mandar tantas tramas como
 * créditos le demos, y solo se dan los que caben en la cola de recepción (cada trama gasta uno, sea del tamaño que sea, así que se
 * cuenta a tamaño máximo). Si la app no lee, la otra punta espera: no se pierde nada. */
static uint16_t    creditos_fuera;             /* dados y aún sin gastar */
#define CREDITOS_INICIALES ((uint8_t)(BT_COLA_RX / HCI_ACL_PAYLOAD_SIZE ? BT_COLA_RX / HCI_ACL_PAYLOAD_SIZE : 1))

static btstack_packet_callback_registration_t reg_hci;
static const hci_transport_t *transporte;
static hci_transport_config_uart_t config = {
    HCI_TRANSPORT_CONFIG_UART,
    115200,                                    /* baudrate_init: el chip arranca a 115 200 */
    0,                                         /* baudrate_main = 0: BTstack NO cambia la velocidad (la cambia este
                                                  fichero al final, como el original: DISENO §3.3) */
    1,                                         /* RTS/CTS */
    NULL, 0
};

/* 0xFCFC: ST_Set_Uart_Baud_Rate. No está en hci_cmd.c (es de fabricante). */
static const hci_cmd_t cmd_st_baudios = { 0xFCFC, "1" };

static void a_fallo(bt_fallo_t f) { dejar_de_esperar(); fallo = f; estado = BT_FALLO; }

static void log_(const char *s) { bt_hw_log(s); }

/* ---- colas ---- */
static void rx_meter(const uint8_t *d, uint16_t n)
{
    while (n--) {
        if (rx_n == BT_COLA_RX) { bt_rx_perdidos++; d++; continue; }   /* con los créditos no debe pasar nunca */
        rx_cola[(rx_ini + rx_n++) & MASCARA_RX] = *d++;
    }
}
static void dar_creditos(void)
{
    uint32_t m = marco_max ? marco_max : HCI_ACL_PAYLOAD_SIZE, caben = (BT_COLA_RX - rx_n) / m, k;
    if (!rfcomm_cid || estado != BT_CONECTADO || caben <= creditos_fuera) return;
    k = caben - creditos_fuera;
    if (k > 255) k = 255;
    if (k < 2 && creditos_fuera) return;         /* de dos en dos como poco: cada concesión es una trama vacía */
    if (rfcomm_grant_credits(rfcomm_cid, (uint8_t)k) == ERROR_CODE_SUCCESS) creditos_fuera = (uint16_t)(creditos_fuera + k);
}

static uint8_t parametro_baudios(uint32_t b)
{
    switch (b) { case 230400: return 0x12; case 460800: return 0x13; case 921600: return 0x14;
                 case 1843200: return 0x16; default: return 0x10; }
}

/* ---- RFCOMM y SDP ---- */
static void rfcomm_manejador(uint8_t tipo, uint16_t canal, uint8_t *p, uint16_t n)
{
    (void)canal;
    if (tipo == RFCOMM_DATA_PACKET) { if (creditos_fuera) creditos_fuera--; rx_meter(p, n); return; }
    if (tipo != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(p)) {
    case RFCOMM_EVENT_INCOMING_CONNECTION: {           /* (0.29.3) el puente que llama: solo el que esperamos, y solo mientras se espera */
        bd_addr_t a; uint16_t c = rfcomm_event_incoming_connection_get_rfcomm_cid(p);
        rfcomm_event_incoming_connection_get_bd_addr(p, a);
        if (esperando && estado == BT_CONECTANDO && !memcmp(a, destino, 6)) { log_("[bt] el puente llama al canal 7: aceptado"); rfcomm_accept_connection(c); }
        else { log_("[bt] alguien llama al canal 7 sin esperarlo: rechazado"); rfcomm_decline_connection(c); }
        break;
    }
    case RFCOMM_EVENT_CHANNEL_OPENED:
        dejar_de_esperar();
        if (rfcomm_event_channel_opened_get_status(p)) {
            log_("[bt] RFCOMM: no se abrió el canal");
            a_fallo(fallo == BT_F_PIN ? BT_F_PIN : BT_F_CONEXION);
            return;
        }
        rfcomm_cid = rfcomm_event_channel_opened_get_rfcomm_cid(p);
        marco_max  = rfcomm_event_channel_opened_get_max_frame_size(p);
        enlace     = rfcomm_event_channel_opened_get_con_handle(p);
        estado = BT_CONECTADO;
        log_("[bt] canal SPP abierto");
        dar_creditos();                          /* los iniciales eran para tramas del máximo posible; ahora se sabe el real */
        if (tx_n) rfcomm_request_can_send_now_event(rfcomm_cid);
        break;
    case RFCOMM_EVENT_CAN_SEND_NOW: {           /* (velocidad) una trama del tamaño máximo, escrita directamente en el búfer de
                                                 * salida de BTstack (antes: trozos de 64 B copiados dos veces) */
        uint16_t k = tx_n, i; uint8_t *b;
        if (!k) break;
        if (k > marco_max) k = marco_max;
        rfcomm_reserve_packet_buffer();
        b = rfcomm_get_outgoing_buffer();
        for (i = 0; i < k; i++) b[i] = tx_cola[(tx_ini + i) & MASCARA_TX];
        if (rfcomm_send_prepared(rfcomm_cid, k) == ERROR_CODE_SUCCESS) { tx_ini = (uint16_t)((tx_ini + k) & MASCARA_TX); tx_n -= k; }
        if (tx_n) rfcomm_request_can_send_now_event(rfcomm_cid);
        break;
    }
    case RFCOMM_EVENT_CHANNEL_CLOSED:
        log_("[bt] canal SPP cerrado");
        rfcomm_cid = 0;
        if (soltar_enlace && enlace != HCI_CON_HANDLE_INVALID) {
            gap_disconnect(enlace);              /* el HC-05 vuelve a «buscando» (LED rápido) y se puede reconectar */
            soltar_enlace = 0; enlace = HCI_CON_HANDLE_INVALID;
        }
        if (estado == BT_CONECTADO) a_fallo(BT_F_CAIDA);
        else if (estado == BT_CONECTANDO) a_fallo(fallo == BT_F_PIN ? BT_F_PIN : BT_F_CONEXION);
        break;
    default: break;
    }
}

/* ¿lleva `t` dentro? (sin libc: el sistema enlaza sin ella) */
static int nombre_tiene(const char *s, const char *t)
{
    int i, j;
    if (!s) return 0;
    for (i = 0; s[i]; i++) {
        for (j = 0; t[j] && s[i + j] == t[j]; j++) { }
        if (!t[j]) return 1;
    }
    return 0;
}

/* «[bt] SDP: canal 10 «SENUS W5RED»: el puente» (sin printf) */
static void log_sdp(uint8_t ch, const char *nm, uint8_t pr)
{
    static const char *const que[3] = { ": el de macOS, se salta", "", ": el puente" };
    char b[96]; unsigned k = 0; const char *t;
    for (t = "[bt] SDP: canal "; *t; t++) b[k++] = *t;
    if (ch >= 10) b[k++] = (char)('0' + ch / 10);
    b[k++] = (char)('0' + ch % 10);
    for (t = " \xc2\xab"; *t; t++) b[k++] = *t;
    for (t = nm ? nm : ""; *t && k < 60; t++) b[k++] = *t;
    for (t = "\xc2\xbb"; *t; t++) b[k++] = *t;
    for (t = que[pr]; *t && k < sizeof b - 1; t++) b[k++] = *t;
    b[k] = 0;
    log_(b);
}

static void sdp_manejador(uint8_t tipo, uint16_t canal, uint8_t *p, uint16_t n)
{
    (void)canal; (void)n;
    if (tipo != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(p)) {
    case SDP_EVENT_QUERY_RFCOMM_SERVICE: {
        /* (3-nov-2026) Un Mac anuncia DOS puertos serie con SENUS abierta: el suyo («Bluetooth-Incoming-Port», que al abrirlo saca
         * el diálogo de macOS pidiendo un código) y el de SENUS («SENUS W5RED»); con SENUS cerrada, solo el suyo. Antes se cogía el
         * primero: el teléfono acababa en el de macOS. Ahora manda el del puente (SENUS, o el puente de pruebas: «W580 puente de red»); si no hay,
         * el primero que no sea el de macOS (los robots: «Dev B», «SPP Dev»…). El de macOS, nunca. */
        const char *nm = sdp_event_query_rfcomm_service_get_name(p);
        /* BTstack corta el nombre a SDP_SERVICE_NAME_LEN (20): «Bluetooth-Incoming-Port» llega como «Bluetooth-Incoming-P» [medido en el emulador] */
        uint8_t ch = sdp_event_query_rfcomm_service_get_rfcomm_channel(p);
        uint8_t pr = nombre_tiene(nm, "W5RED") || nombre_tiene(nm, "puente de red") ? 2 : nombre_tiene(nm, "Incoming") ? 0 : 1;
        log_sdp(ch, nm, pr);
        if (pr > prio_spp) { prio_spp = pr; canal_spp = ch; }
        break;
    }
    case SDP_EVENT_QUERY_COMPLETE:
        if (estado != BT_CONECTANDO) break;
        if (solo_puente && !sdp_event_query_complete_get_status(p) && prio_spp < 2) {   /* (0.29.3) sin puente anunciado: que llame él */
            esperando = 1; gap_connectable_control(1);
            log_("[bt] SDP: sin puente W5RED; espero a que el puente llame (SENUS en macOS, canal 7)");
            break;
        }
        if (sdp_event_query_complete_get_status(p) || !canal_spp) {
            log_("[bt] SDP: sin puerto serie (en un Mac: SENUS cerrada o sin «Encender»)");
            a_fallo(fallo == BT_F_PIN ? BT_F_PIN : BT_F_SIN_SPP);
            break;
        }
        creditos_fuera = CREDITOS_INICIALES;
        if (rfcomm_create_channel_with_initial_credits(rfcomm_manejador, destino, canal_spp, CREDITOS_INICIALES, &rfcomm_cid) != ERROR_CODE_SUCCESS)
            a_fallo(BT_F_CONEXION);
        break;
    default: break;
    }
}

/* ---- búsqueda: tras el inquiry, pide los nombres de uno en uno ---- */
static void pedir_siguiente_nombre(void)
{
    while (nombre_sig < n_encontrados && encontrados[nombre_sig].nombre[0]) nombre_sig++;
    if (nombre_sig >= n_encontrados) { estado = BT_LISTO; return; }
    bd_addr_t a; memcpy(a, encontrados[nombre_sig].addr, 6);
    gap_remote_name_request(a, encontrados_pag[nombre_sig].psrm, (uint16_t)(encontrados_pag[nombre_sig].reloj | 0x8000));
}

static int indice_de(const uint8_t *a)
{
    int i;
    for (i = 0; i < n_encontrados; i++) if (!memcmp(encontrados[i].addr, a, 6)) return i;
    return -1;
}

/* ---- el manejador de HCI/GAP ---- */
static void hci_manejador(uint8_t tipo, uint16_t canal, uint8_t *p, uint16_t n)
{
    (void)canal;
    if (tipo != HCI_EVENT_PACKET) return;
    btstack_chipset_stlc2500c_evento(p, n);
    switch (hci_event_packet_get_type(p)) {
    case HCI_EVENT_COMMAND_COMPLETE: {
        uint16_t op = hci_event_command_complete_get_command_opcode(p);
        const uint8_t *r = hci_event_command_complete_get_return_parameters(p);
        if (op == HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION && n >= 14 && r[0] == 0) {
            info.lmp_version = r[4];
            info.hci_rev     = little_endian_read_16(r, 2);
            info.fabricante  = little_endian_read_16(r, 5);
            info.lmp_subver  = little_endian_read_16(r, 7);
            btstack_chipset_stlc2500c_version(info.hci_rev, info.lmp_subver);
        } else if (op == HCI_OPCODE_HCI_READ_BD_ADDR && n >= 12 && r[0] == 0) {
            reverse_bd_addr(r + 1, info.mi_addr);
        } else if (op == 0xFCFC && estado == BT_ENCENDIENDO && sub == 3) {
            info.baudios = r[0] == 0 ? baud_final : 0;
            if (r[0]) { a_fallo(BT_F_VELOCIDAD); break; }
            estado = BT_LISTO;
            log_("[bt] listo");
        }
        break;
    }
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(p) == HCI_STATE_WORKING && estado == BT_ENCENDIENDO) {
            const stlc2500c_informe_t *ci = btstack_chipset_stlc2500c_informe();
            memcpy(info.revision, ci->revision, sizeof info.revision);
            info.ptc = ci->fichero[0]; info.ssf = ci->fichero[1];
            info.baudios = 115200;
            if (ci->error) fallo = BT_F_PARCHE;      /* se avisa, pero se sigue */
#ifdef BT_CHIPSET_BTSTACK
            if (0) {
#else
            if (baud_final != 115200) {              /* paso 13 del original */
#endif
                sub = 2;
                hci_send_cmd(&cmd_st_baudios, parametro_baudios(baud_final));
            } else { estado = BT_LISTO; log_("[bt] listo"); }
        }
        break;
    case HCI_EVENT_PIN_CODE_REQUEST: {
        bd_addr_t a;
        hci_event_pin_code_request_get_bd_addr(p, a);
        log_("[bt] el robot pide PIN");
        gap_pin_code_response(a, pin);
        break;
    }
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        if (p[2] != 0) {
            fallo = BT_F_PIN;
            /* (clave) 0x06 PIN or Key Missing / 0x05 Authentication Failure con una clave guardada: el robot ya no la tiene
             * (AT+RMAAD, emparejado con otros móviles, otro módulo…). Fuera de BTstack, para que el siguiente intento haga
             * Link_Key_Request_Negative_Reply -> PIN_Code_Request y se use el PIN de verdad. Sin esto el teléfono seguía
             * mandando la clave vieja para siempre (bt/cadena/HALLAZGOS.md §1). El sistema la quita también de su almacén. */
            /* (sin mirar el estado: el Authentication_Complete puede llegar DESPUÉS de que RFCOMM haya dado el intento por fallido) */
            if ((p[2] == ERROR_CODE_PIN_OR_KEY_MISSING || p[2] == ERROR_CODE_AUTHENTICATION_FAILURE)) {
                link_key_t k; link_key_type_t t;
                if (gap_get_link_key_for_bd_addr(destino, k, &t)) {
                    log_("[bt] el robot rechazo la clave guardada: olvidada, el siguiente intento pide PIN");
                    gap_drop_link_key_for_bd_addr(destino);
                } else log_("[bt] PIN rechazado");
            } else log_("[bt] PIN rechazado");
        }
        break;
    case GAP_EVENT_INQUIRY_RESULT: {
        bd_addr_t a; int i;
        gap_event_inquiry_result_get_bd_addr(p, a);
        if ((i = indice_de(a)) < 0 && n_encontrados < MAX_ENCONTRADOS) {
            i = n_encontrados++;
            memset(&encontrados[i], 0, sizeof encontrados[i]);
            memcpy(encontrados[i].addr, a, 6);
            encontrados[i].clase = gap_event_inquiry_result_get_class_of_device(p);
            encontrados_pag[i].psrm  = gap_event_inquiry_result_get_page_scan_repetition_mode(p);
            encontrados_pag[i].reloj = gap_event_inquiry_result_get_clock_offset(p);
        }
        if (i >= 0 && gap_event_inquiry_result_get_name_available(p)) {
            unsigned k = gap_event_inquiry_result_get_name_len(p);
            if (k > 31) k = 31;
            memcpy(encontrados[i].nombre, gap_event_inquiry_result_get_name(p), k);
            encontrados[i].nombre[k] = 0;
        }
        break;
    }
    case GAP_EVENT_INQUIRY_COMPLETE:
        if (estado == BT_BUSCANDO) { nombre_sig = 0; pedir_siguiente_nombre(); }
        break;
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        if (estado == BT_BUSCANDO) {
            if (nombre_sig < n_encontrados) {
                if (hci_event_remote_name_request_complete_get_status(p) == 0)
                    strncpy(encontrados[nombre_sig].nombre, hci_event_remote_name_request_complete_get_remote_name(p), 31);
                else strcpy(encontrados[nombre_sig].nombre, "?");
                nombre_sig++;
            }
            pedir_siguiente_nombre();
        }
        break;
    default: break;
    }
}

/* ---- configuración que pone el sistema ---- */
void bt_config_ficheros(stlc2500c_leer_t f)     { lector = f; }
void bt_config_bd_addr(const uint8_t a[6])      { memcpy(mi_addr, a, 6); hay_mi_addr = 1; }
void bt_config_baudios(uint32_t b)              { baud_final = b; }
void bt_pin(const char *s)                      { strncpy(pin, s, sizeof pin - 1); pin[sizeof pin - 1] = 0; }

/* ---- ciclo de vida ---- */
static void iniciar_btstack(void)
{
    if (iniciado_btstack) return;              /* BTstack no se puede «des-iniciar»: una vez por arranque */
    iniciado_btstack = 1;
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());
    transporte = hci_transport_h4_instance_for_uart(btstack_uart_block_embedded_instance());
    hci_init(transporte, &config);
#ifdef BT_CHIPSET_BTSTACK
    config.baudrate_main = baud_final;
    hci_set_chipset(btstack_chipset_stlc2500d_instance());
    { bd_addr_t a; memcpy(a, mi_addr, 6); hci_set_bd_addr(a); }
#else
    hci_set_chipset(btstack_chipset_stlc2500c_instance());
#endif
    hci_set_link_key_db(btstack_link_key_db_memory_instance());
    reg_hci.callback = &hci_manejador;
    hci_add_event_handler(&reg_hci);
    l2cap_init();
    rfcomm_init();
    rfcomm_register_service_with_initial_credits(rfcomm_manejador, CANAL_PROPIO, 0xffff, CREDITOS_INICIALES);   /* (0.29.3) el puente que llama */
    sdp_client_init();
    gap_set_local_name("W580i senew");
    gap_set_class_of_device(0x5A020C);         /* teléfono inteligente (como un teléfono cualquiera) */
    gap_discoverable_control(0);               /* solo cliente: nadie nos tiene que encontrar */
    gap_connectable_control(0);
    gap_ssp_set_enable(0);                     /* BT 2.0: emparejamiento legado con PIN [ver DISENO §4.2] */
}

int bt_encender(void)
{
    if (estado != BT_APAGADO && estado != BT_FALLO) return -1;
    memset(&info, 0, sizeof info);
    fallo = BT_OK;
    iniciar_btstack();
#ifndef BT_CHIPSET_BTSTACK
    /* (prueba_emu) el chipset vuelve a empezar SIEMPRE: el chip sale de un reset y ha perdido parche, ajustes y BD_ADDR.
     * BTstack solo llama a su init() desde OFF; si un apagado anterior se quedó en HALTING, no lo haría. */
    hci_set_chipset(btstack_chipset_stlc2500c_instance());
#endif
    btstack_chipset_stlc2500c_set_lector(lector);
    btstack_chipset_stlc2500c_set_bd_addr(mi_addr);   /* siempre: si GDFS no tiene, 11:11:… como el original */
    (void)hay_mi_addr;
    estado = BT_ENCENDIENDO; sub = 0;
    bt_hw_reset(1);                            /* docs/bluetooth.md §3: pulso de 100 ms */
    t0 = bt_hw_ms();
    return 0;
}

void bt_apagar(void)
{
    if (estado == BT_APAGADO) return;
    if (iniciado_btstack) {
        /* (prueba_emu) hci_power_control(OFF) solo PIDE el apagado (HCI_STATE_HALTING); hay que dar vueltas al run loop hasta
         * que BTstack llegue a OFF (cierra el transporte). Si no, el siguiente ON entra por HALTING y se salta el init del
         * chipset: en el emulador, la segunda encendida no mandaba ni el parche ni la BD_ADDR. Tope: 500 ms. */
        uint32_t t = bt_hw_ms();
        hci_power_control(HCI_POWER_OFF);
        while (hci_get_state() != HCI_STATE_OFF && bt_hw_ms() - t < 500) {
            hal_uart_bloques_sondear();
            btstack_run_loop_embedded_execute_once();
        }
        if (hci_get_state() != HCI_STATE_OFF) log_("[bt] apagar: BTstack no llegó a OFF en 500 ms");
    }
    bt_hw_cerrar();
    esperando = 0;                                 /* el chip queda en reset: nada que apagar, y el próximo encendido lo deja no conectable */
    estado = BT_APAGADO; rfcomm_cid = 0; tx_n = rx_n = 0;
}

/* (1.8, 26-oct) INSTRUMENTADO: con la música por la red la descarga iba a ~33 KB/s (= lo que pide un MP3 de 256 kbps) y con una app
 * delante a 100-137. Cuántas veces se llama bt_paso, cuántas corta por el tope de tiempo y cuántas vueltas da; el tope, ajustable por el
 * puente (bt_paso_tope_ms; ojo: bt_hw_ms va a saltos de 4 ms, así que «4» es «hasta el próximo salto», ~2 ms de media). */
volatile uint32_t bt_paso_tope_ms = 4, bt_paso_llamadas, bt_paso_topes, bt_paso_vueltas, bt_paso_sin_trabajo;
void bt_paso(void)
{
    if (estado == BT_APAGADO) return;
    if (estado == BT_ENCENDIENDO) {
        switch (sub) {
        case 0:                                   /* reset bajo 100 ms */
            if (bt_hw_ms() - t0 < 100) return;
            if (bt_hw_abrir()) { a_fallo(BT_F_SIN_CHIP); return; }
            bt_hw_reset(0); t0 = bt_hw_ms(); sub = 1;
            return;
        case 1:                                   /* P2.0 a 1 y su RTS afirmado: el chip vive Y escucha (hasta 1 s) */
            if (!bt_hw_vivo() || !bt_hw_escucha()) { if (bt_hw_ms() - t0 > 1000) a_fallo(BT_F_SIN_CHIP); return; }
            t0 = bt_hw_ms(); sub = 5;
            return;
        case 5:                                   /* y 20 ms más. [observado en el aparato, 2026-10-25]: un HCI_Reset que esperaba en la FIFO
                                                   * y sale EN EL INSTANTE en que el chip afirma su RTS (~56 ms tras soltar el reset) recibe
                                                   * HARDWARE ERROR 0x20 y el chip ya no contesta; a 4 ms de la CTS, ya bien (60..150 ms: bien) */
            if (bt_hw_ms() - t0 < 20) return;
            { uint8_t tira[16]; while (bt_hw_rx(tira, sizeof tira) > 0) { } }   /* lo que entró al soltar el reset (el «break») */
            sdp_client_deinit();                  /* (28-oct) y el cliente SDP desde cero: si la radio se apagó a mitad de una consulta, seguía «ocupado» */
            hci_power_control(HCI_POWER_ON);      /* Reset, versión, nombre, chipset (FC0F, .ptc, .ssf, Reset, FC22, Reset)… */
            t0 = bt_hw_ms(); sub = 4;
            break;
        case 2:                                   /* 0xFCFC encolada: en cuanto salga entera, cambiar NUESTRA UART */
            if (hal_uart_bloques_tx_ocupado() || !bt_hw_tx_vacio()) break;
            transporte->set_baudrate(baud_final);
            t0 = bt_hw_ms(); sub = 3;
            break;
        case 3:                                   /* el Command Complete llega ya a la velocidad nueva */
            if (bt_hw_ms() - t0 > 2000) { a_fallo(BT_F_VELOCIDAD); return; }
            break;
        case 4:                                   /* init de BTstack en marcha */
            if (bt_hw_ms() - t0 > 30000) { a_fallo(BT_F_SIN_RESPUESTA); return; }   /* la pila del original espera 30 s */
            break;
        }
    }
    {   /* (velocidad) DRENAR: antes, una vuelta de UART + run loop por paso movía ~un bloque H4 (tipo, cabecera o carga): una app que
         * llama 30 veces por segundo se quedaba en nada. Ahora se repite mientras haya progreso, con tope de 4 ms (el tic) y de vueltas. */
        uint32_t t = bt_hw_ms(); int i;
        bt_paso_llamadas++;
        for (i = 0; i < 256; i++) {
            int algo = hal_uart_bloques_sondear();
            btstack_run_loop_embedded_execute_once();
            algo |= hal_uart_bloques_sondear();
            bt_paso_vueltas++;
            if (!algo) { if (!i) bt_paso_sin_trabajo++; break; }
            if (bt_hw_ms() - t >= bt_paso_tope_ms) { bt_paso_topes++; break; }
        }
    }
    dar_creditos();
}

bt_estado_t bt_estado(void) { return estado; }
bt_fallo_t  bt_fallo(void)  { return fallo; }
const bt_info_t *bt_info(void) { return &info; }

int bt_buscar(unsigned s)
{
    if (estado != BT_LISTO) return -1;
    if (s < 1) s = 1;
    if (s > 30) s = 30;
    n_encontrados = 0; nombre_sig = 0;
    estado = BT_BUSCANDO;
    return gap_inquiry_start((uint8_t)((s * 1000u + 1279u) / 1280u));
}

int bt_encontrados(void) { return n_encontrados; }
int bt_encontrado(int i, bt_disp_t *d)
{
    if (i < 0 || i >= n_encontrados) return -1;
    *d = encontrados[i];
    return 0;
}

int bt_emparejado(const uint8_t a[6])
{
    link_key_t k; link_key_type_t t; bd_addr_t b;
    memcpy(b, a, 6);
    return gap_get_link_key_for_bd_addr(b, k, &t) ? 1 : 0;
}

void bt_olvidar(const uint8_t a[6]) { bd_addr_t b; memcpy(b, a, 6); gap_drop_link_key_for_bd_addr(b); }

int bt_conectar_spp(const uint8_t a[6])
{
    if (estado != BT_LISTO) return -1;
    /* (28-oct) el cliente SDP de BTstack se queda OCUPADO si una conexión se cortó a mitad de su consulta (un page timeout, la radio apagada):
     * sdp_client_init() no hace nada y nadie lo reinicia; desde ahí CADA conexión fallaba al instante («el puente no contesta») hasta reiniciar el
     * teléfono [observado 28-oct, senew-RAM build/dic6]. Aquí no hay ninguna consulta nuestra en marcha (estado LISTO): se reinicia. */
    if (!sdp_client_ready()) sdp_client_deinit();
    memcpy(destino, a, 6);
    solo_puente = bt_puente_red; bt_puente_red = 0; dejar_de_esperar();
    canal_spp = 0; prio_spp = 0; tx_n = rx_n = 0; tx_ini = rx_ini = 0; fallo = BT_OK; creditos_fuera = 0;
    estado = BT_CONECTANDO;
    /* SDP: el canal RFCOMM del «Serial Port» (0x1101). El HC-05 lo anuncia en el 1, pero se pregunta igual. */
    if (sdp_client_query_rfcomm_channel_and_name_for_service_class_uuid(sdp_manejador, destino,
            BLUETOOTH_SERVICE_CLASS_SERIAL_PORT) != ERROR_CODE_SUCCESS) { a_fallo(BT_F_CONEXION); return -1; }
    return 0;
}

int bt_enviar(const void *d, unsigned n)
{
    const uint8_t *b = d; unsigned i;
    if (estado != BT_CONECTADO && estado != BT_CONECTANDO) return 0;
    for (i = 0; i < n && tx_n < BT_COLA_TX; i++) tx_cola[(tx_ini + tx_n++) & MASCARA_TX] = b[i];
    if (i && rfcomm_cid && estado == BT_CONECTADO) rfcomm_request_can_send_now_event(rfcomm_cid);
    return (int)i;
}

int bt_pendiente(void) { return tx_n; }

int bt_recibir(void *d, unsigned max)
{
    uint8_t *b = d; unsigned i;
    for (i = 0; i < max && rx_n; i++) { b[i] = rx_cola[rx_ini]; rx_ini = (uint16_t)((rx_ini + 1) & MASCARA_RX); rx_n--; }
    if (i) dar_creditos();                       /* hay sitio: que la otra punta siga mandando */
    return (int)i;
}

void bt_desconectar(void)
{
    dejar_de_esperar();
    if (rfcomm_cid) { soltar_enlace = 1; rfcomm_disconnect(rfcomm_cid); }
    rfcomm_cid = 0; tx_n = rx_n = 0; tx_ini = rx_ini = 0; creditos_fuera = 0;
    if (estado == BT_CONECTADO || estado == BT_CONECTANDO || estado == BT_FALLO) { estado = BT_LISTO; fallo = BT_OK; }
}
