/* btstack_chipset_stlc2500c.c — ver el .h. Fichero nuevo del proyecto (no es de BTstack); usa su interfaz
 * btstack_chipset_t. Los dos órdenes de fabricante que ya trae chipset/stlc2500d de BTstack (0xFCFC y 0xFC22)
 * se reescriben aquí igual, para no depender de su fichero (que además se llama «BCM» por error). */
#include "btstack_chipset_stlc2500c.h"
#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_util.h"
#include "hci.h"
#include <string.h>

#define OP_LEER_REVISION 0xFC0Fu       /* Ericsson_Read_Revision_Information */
#define OP_DESCARGA      0xFC2Eu       /* Ericsson_Write/Download (BlueZ: ocf 0x2e) */
#define OP_NVDS          0xFC22u       /* ST_Store_In_NVDS (= Ericsson_Store_In_Flash) */
#define OP_BAUDIOS       0xFCFCu       /* ST_Set_Uart_Baud_Rate (el W580: 01 FC FC 01 14) */
#define OP_RESET         0x0C03u

enum { P_REVISION, P_FICHERO, P_RESET, P_BDADDR, P_RESET2, P_HECHO };

static stlc2500c_leer_t lector;
static stlc2500c_informe_t inf;
static uint8_t paso, cual, seq, esperando_seq;
static uint32_t off;
static bd_addr_t bdaddr; static uint8_t bdaddr_puesta;

void btstack_chipset_stlc2500c_set_bd_addr(const bd_addr_t a) { memcpy(bdaddr, a, 6); bdaddr_puesta = 1; }

void btstack_chipset_stlc2500c_set_lector(stlc2500c_leer_t f) { lector = f; }
const stlc2500c_informe_t *btstack_chipset_stlc2500c_informe(void) { return &inf; }

void btstack_chipset_stlc2500c_version(uint16_t hci_rev, uint16_t lmp_subver)
{
    inf.hci_rev = hci_rev; inf.lmp_subver = lmp_subver;
}

static void chipset_set_bd_addr_command(bd_addr_t addr, uint8_t *b);
static void cmd(uint8_t *b, uint16_t op, uint8_t n) { little_endian_store_16(b, 0, op); b[2] = n; }

static void chipset_init(const void *config)
{
    (void)config;
    uint16_t r = inf.hci_rev, s = inf.lmp_subver;
    memset(&inf, 0, sizeof inf);
    inf.hci_rev = r; inf.lmp_subver = s;
    paso = P_REVISION; cual = 0; seq = 0; off = 0; esperando_seq = 0;
}

void btstack_chipset_stlc2500c_evento(const uint8_t *ev, uint16_t n)
{
    uint16_t op;
    if (n < 6 || ev[0] != HCI_EVENT_COMMAND_COMPLETE) return;
    op = little_endian_read_16(ev, 3);
    if (op == OP_LEER_REVISION) {
        uint16_t k = (uint16_t)(n - 6);               /* ev[5] = estado, luego la cadena */
        if (k >= sizeof inf.revision) k = sizeof inf.revision - 1;
        memcpy(inf.revision, ev + 6, k); inf.revision[k] = 0;
        return;
    }
    if (op != OP_DESCARGA || !esperando_seq) return;
    esperando_seq = 0;
    /* BlueZ (load_file): estado 0 y que devuelva el mismo número de secuencia */
    if (ev[5] != 0 || n < 7 || ev[6] != (uint8_t)(seq - 1)) {
        log_error("stlc2500c: trozo %u de %s rechazado (estado %u, seq %u)", (unsigned)(seq - 1),
                  cual ? ".ssf" : ".ptc", ev[5], n >= 7 ? ev[6] : 0xFF);
        inf.fichero[cual] = -1; inf.error = 1;
        return;
    }
    inf.trozos[cual]++;
}

static btstack_chipset_result_t chipset_next_command(uint8_t *b)
{
    for (;;) switch (paso) {
    case P_REVISION:
        paso = P_FICHERO;
        cmd(b, OP_LEER_REVISION, 0);
        return BTSTACK_CHIPSET_VALID_COMMAND;
    case P_FICHERO: {
        int k;
        if (inf.error) { paso = P_HECHO; continue; }  /* sin el reset final: el chip sigue con su ROM */
        if (cual > 1) { paso = P_RESET; continue; }
        k = lector ? lector(cual, inf.hci_rev, inf.lmp_subver, off, b + 4, 254) : -1;
        if (k <= 0) {                                  /* -1: no hay; 0: acabado */
            if (k < 0) log_info("stlc2500c: no hay %s para HCI_Rev %04x LMP_Sub %04x", cual ? ".ssf" : ".ptc",
                                inf.hci_rev, inf.lmp_subver);
            else inf.fichero[cual] = 1;
            cual++; off = 0; seq = 0;
            continue;
        }
        cmd(b, OP_DESCARGA, (uint8_t)(k + 1));
        b[3] = seq++;
        off += (uint32_t)k; inf.bytes[cual] += (uint32_t)k;
        esperando_seq = 1;
        return BTSTACK_CHIPSET_VALID_COMMAND;
    }
    case P_RESET:
        paso = P_BDADDR;
        if (!inf.fichero[0] && !inf.fichero[1]) continue;   /* nada cargado: no hace falta */
        cmd(b, OP_RESET, 0);
        return BTSTACK_CHIPSET_VALID_COMMAND;
    case P_BDADDR:                                   /* pasos 11–12 del original: no depende del «fabricante» que
                                                        diga Read_Local_Version (BTstack solo hace el reset si es ST) */
        if (!bdaddr_puesta) { paso = P_HECHO; continue; }
        paso = P_RESET2;
        chipset_set_bd_addr_command(bdaddr, b);
        return BTSTACK_CHIPSET_VALID_COMMAND;
    case P_RESET2:
        paso = P_HECHO;
        cmd(b, OP_RESET, 0);
        return BTSTACK_CHIPSET_VALID_COMMAND;
    default:
        return BTSTACK_CHIPSET_DONE;
    }
}

static void chipset_set_baudrate_command(uint32_t baud, uint8_t *b)
{
    uint8_t p;                                       /* tabla de EBT_sil_FUN_446d0560 (docs/bluetooth.md §2.4) */
    switch (baud) {
    case 57600: p = 0x0E; break;  case 115200: p = 0x10; break; case 230400: p = 0x12; break;
    case 460800: p = 0x13; break; case 921600: p = 0x14; break; case 1843200: p = 0x16; break;
    default: log_error("stlc2500c: %u baudios no", (unsigned)baud); p = 0x10; break;
    }
    cmd(b, OP_BAUDIOS, 1); b[3] = p;
}

static void chipset_set_bd_addr_command(bd_addr_t addr, uint8_t *b)
{
    cmd(b, OP_NVDS, 8); b[3] = 0xFE; b[4] = 0x06;
    reverse_bd_addr(addr, &b[5]);                    /* en el cable, el byte menos significativo primero */
}

static const btstack_chipset_t chipset = {
    "STLC2500C", chipset_init, chipset_next_command, chipset_set_baudrate_command, chipset_set_bd_addr_command,
};

const btstack_chipset_t *btstack_chipset_stlc2500c_instance(void) { return &chipset; }
