/* bl_usb.c — UDC MÍNIMO del bootloader (DB2020, 0xFC030000): solo EP0, por sondeo, sin IRQ.
 *
 * Es un trozo chico de hal/usb_dc.c reducido a lo que el BL necesita:
 *   - usbdc_iniciar() (relojes del USB ya los encendió bl_syscon_usb()): reset del bloque, EP0 de 64 B, SELF_PWR, pull-up;
 *   - bl_usb_paso(): mira DEVSTAT/IRQ_SRC y despacha SETUP, EP0_RX, EP0_TX al capítulo 9.
 *   - sin EPn, sin bulk, sin cola de eventos.
 *
 * El capítulo 9 implementa:
 *   - GET_DESCRIPTOR (dispositivo, configuración, cadenas): descriptores propios del BL (VID:PID 0x1209:0x0002, texto «SNBL»);
 *   - SET_ADDRESS (hardware se la aplica solo, igual que en senew);
 *   - SET_CONFIGURATION (1);
 *   - peticiones de vendedor 0xD0..0xDB → bl_ep0_*.
 * No hace falta MTP, Still Image, MS OS, nada.
 */
#include <stddef.h>
#include "bl.h"
#define R8(a)  (*(volatile u8  *)(size_t)(a))
#define R16(a) (*(volatile u16 *)(size_t)(a))
#define R32(a) (*(volatile u32 *)(size_t)(a))

#define UDC          0xFC030000u
#define EP_NUM       (UDC + 0x04)
#define DATA         (UDC + 0x08)
#define CTRL         (UDC + 0x0C)
#define STAT_FLG     (UDC + 0x10)
#define RXFSTAT      (UDC + 0x14)
#define SYSCON1      (UDC + 0x18)
#define SYSCON2      (UDC + 0x1C)
#define DEVSTAT      (UDC + 0x20)
#define IRQ_EN       (UDC + 0x28)
#define IRQ_SRC      (UDC + 0x30)
#define EPN_STAT     (UDC + 0x34)
#define EP0_CFG      (UDC + 0x80)
#define R108         (UDC + 0x108)

#define N_SEL 0x20u
#define N_DIR 0x10u
#define N_SETUP 0x40u
#define C_FIFO_EN 0x04u
#define C_SET_HALT 0x40u
#define C_CLR_HALT 0x80u
#define S_FIFO_EN 0x04u
#define S_ACK 0x08u
#define S1_PULLUP 0x0001u
#define S1_SELF_PWR 0x0004u
#define S1_CFG_LOCK 0x0100u
#define S2_CLR_CFG 0x04u
#define S2_DEV_CFG 0x08u
#define D_ATT 0x01u
#define D_RESET 0x20u
#define I_EP0_TX 0x01u
#define I_EP0_RX 0x02u
#define I_SETUP 0x04u
#define I_DS_CHG 0x08u

extern void bl_espera_ms(unsigned ms);

/* ---- Descriptores ------------------------------------------------------------------------------------------- */
#define LE16(x) (u8)((x) & 0xFF), (u8)((x) >> 8)
#define LE32(x) LE16(x), LE16((x) >> 16)
static const u8 desc_dispositivo[18] = {
    18, 1, LE16(0x0200),
    0xFF, 0, 0,                         /* clase VENDOR: ningún driver OS se la lleva (Windows: WinUSB por los MS OS, abajo) */
    64, LE16(BL_VID), LE16(BL_PID), LE16(0x0030),   /* bcdDevice: 0x0019 -> 0x0030 con los MS OS; Windows guarda lo leído por
                                         * VID/PID/bcdDevice y no vuelve a preguntar: súbelo si cambian */
    1, 2, 3,                            /* fab, prod, serie */
    1
};
#ifdef BL_INSTALLER
/* (installer) descriptor paralelo con BL_PID_INSTALLER. El resto es idéntico; se elige por bl_entrada_actual al responder
 * GET_DESCRIPTOR (DISPOSITIVO). Así el anfitrión pu_bl_installer.py se engancha a 1209:0003 y no estorba al clásico. */
static const u8 desc_dispositivo_installer[18] = {
    18, 1, LE16(0x0200),
    0xFF, 0, 0,
    64, LE16(BL_VID), LE16(BL_PID_INSTALLER), LE16(0x0030),
    1, 2, 3,
    1
};
#endif
#define CONF_LARGO 18
static const u8 desc_configuracion[CONF_LARGO] = {
    9, 2, LE16(CONF_LARGO), 1, 1, 0,
    0xC0, 1,                            /* autoalim, 2 mA */
    9, 4, 0, 0, 0, 0xFF, 0, 0, 0        /* interfaz 0, 0 endpoints, clase VENDOR */
};
static const u8 str_lang[4] = { 4, 3, LE16(0x0409) };
static const u8 str_fab[16] = { 16, 3, 'S',0,'e',0,'n',0,'e',0,'w',0,'B',0,'L',0 };
static const u8 str_prod[20] = { 20, 3, 'W',0,'5',0,'8',0,'0',0,' ',0,'S',0,'N',0,'B',0,'L',0 };
static const u8 str_serie[26] = { 26, 3, 'S',0,'N',0,'B',0,'L',0,'0',0,'0',0,'0',0,'0',0,'0',0,'0',0,'0',0,'1',0 };
/* (6-nov-2026) MS OS 1.0, los MISMOS tres que senew (api/usb.c, que no se comparte con el BL: mantenerlos iguales): la cadena 0xEE
 * «MSFT100» con el código de vendedor 0x54 (no choca con las órdenes 0xD0..0xDB), la compat ID «WINUSB» de la interfaz 0 y el
 * DeviceInterfaceGUID de senew. Windows 8 y posteriores le ponen WinUSB solos al BL (1209:0002 y 1209:0003): ni la primera
 * instalación ni una actualización desde Windows piden driver. Lo único que sigue pidiéndolo es el modo flash de Sony (0fce:adde). */
#define BL_VENDOR_MS 0x54
static const u8 str_msos[18] = { 18, 3, 'M',0,'S',0,'F',0,'T',0,'1',0,'0',0,'0',0, BL_VENDOR_MS, 0 };
static const u8 desc_msos_compat[40] = {
    LE16(40), 0, 0, LE16(0x0100), LE16(4), 1, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 'W','I','N','U','S','B', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
#define MSOS_PROPS_LARGO (10 + 14 + 40 + 78)
static const u8 desc_msos_props[MSOS_PROPS_LARGO] = {
    LE32(MSOS_PROPS_LARGO), LE16(0x0100), LE16(5), LE16(1),
    LE32(14 + 40 + 78), LE32(1), LE16(40),
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,'I',0,'n',0,'t',0,'e',0,'r',0,'f',0,'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0, 0,0,
    LE32(78),
    '{',0,'7',0,'5',0,'E',0,'2',0,'6',0,'8',0,'4',0,'D',0,'-',0,'1',0,'4',0,'0',0,'4',0,'-',0,
    '4',0,'9',0,'3',0,'5',0,'-',0,'9',0,'4',0,'9',0,'8',0,'-',0,
    'B',0,'7',0,'9',0,'1',0,'4',0,'3',0,'A',0,'F',0,'B',0,'A',0,'8',0,'5',0,'}',0, 0,0
};

/* ---- Estado y buffers ------------------------------------------------------------------------------------- */
u8 bl_buf[BL_BUF] __attribute__((aligned(32)));
static u32 bl_arg[BL_N_ARG];
static u32 bl_cursor;
static u8 setup_pend[8];
static u8 setup_hay;
static const u8 *in_p; static u16 in_n, in_pos; static u8 in_zlp;
static u8 out_dst_tipo;                            /* 0 = ninguno, 1 = bl_buf, 2 = cmd (sin uso de momento) */
static u16 out_dst_off, out_dst_n, out_n;
static u8 u_dir_pend;
static u8 u_config;
enum { F_LIBRE, F_DATOS_IN, F_DATOS_OUT, F_ESTADO_IN, F_ESTADO_OUT };
static u8 u_fase;
static u8 ep0_ocupado;

static BlRes bl_res;
static u8 cmd_op; static u8 cmd_seq;

extern u32 bl_hw_ms(void);
void bl_cmd_poner(u8 op, u8 seq)
{
    bl_res.estado = BL_ST_PENDIENTE;
    bl_res.op = op; bl_res.seq = seq;
    bl_res.codigo = 0; bl_res.r0 = 0; bl_res.r1 = 0;
    cmd_op = op; cmd_seq = seq;
}
BlRes *bl_res_actual(void) { return &bl_res; }

/* ---- UDC primitivas ------------------------------------------------------------------------------------- */
static void vaciar_fuentes(void)
{
    int i;
    R16(EP_NUM) = N_SETUP; R16(EP_NUM) = 0;
    for (i = 0; i < 16 && R16(IRQ_SRC); i++) R16(IRQ_SRC) = 0xFFFF;
}

int bl_udc_iniciar(void)
{
    unsigned n;
    R16(IRQ_EN) = 0;
    R16(R108) = 0;
    R16(SYSCON1) = R16(SYSCON1) & ~S1_PULLUP;
    /* (BL, 2026-10-24) Al entrar al BL desde senew (reinicio_caliente), senew dejó el pull-up ENCENDIDO con el host enumerado
     * como 1209:0001. Linux solo re-enumera si ve la desconexión USB: tenemos que apagar el pull-up EL TIEMPO SUFICIENTE
     * (>100 ms, varias tramas USB) para que el host registre la desconexión. En frío (bootrom salta al BL) el pull-up ya está
     * apagado y esta espera es ociosa, también vale. */
    bl_espera_ms(300);
    vaciar_fuentes();
    /* Reset de ESTADO del UDC: por si quedó algo del uso anterior (senew) */
    { int k; for (k = 1; k < 16; k++) R16(UDC + 0x80 + 4*k) = 0x0009; }
    R16(SYSCON1) = R16(SYSCON1) & ~S1_CFG_LOCK;
    R16(EP0_CFG) = 0x3001;                         /* 64 B, puntero 1 */
    for (n = 1; n < 16; n++) { R16(UDC + 0x80 + 4 * n) = 0x0009; R16(UDC + 0xC0 + 4 * n) = 0x0009; }
    R16(SYSCON1) = R16(SYSCON1) | S1_CFG_LOCK;
    R16(SYSCON1) = R16(SYSCON1) | S1_SELF_PWR;
    vaciar_fuentes();
    /* IRQ_EN con los bits del EP0 y cambio de estado: aunque no abrimos IRQ real (sondeamos IRQ_SRC), en el DB2020 algunos
     * bits de IRQ_SRC quizá solo se setean si IRQ_EN los tiene habilitados (comportamiento de OMAP). Igual que senew tras RESET. */
    R16(IRQ_EN) = I_DS_CHG | I_SETUP | I_EP0_TX | I_EP0_RX;
    if (!(R16(SYSCON1) & S1_SELF_PWR)) return -1;
    /* pull-up */
    R16(SYSCON1) = R16(SYSCON1) | S1_PULLUP;
    return 0;
}


/* usbdc_ep_escribir del EP0 IN: devuelve n o -1 si FIFO ocupada. */
static int ep0_escribir(const u8 *p, u32 n)
{
    unsigned i;
    u16 st;
    R16(EP_NUM) = N_SEL | N_DIR;
    st = R16(STAT_FLG);
    if (st & S_FIFO_EN) { R16(EP_NUM) = N_DIR; return -1; }
    for (i = 0; i + 1 < n; i += 2) R16(DATA) = p[i] | (p[i + 1] << 8);
    if (i < n) R8(DATA) = p[i];
    R16(CTRL) = C_FIFO_EN;
    R16(EP_NUM) = N_DIR;    /* senew: ESC(EP_NUM, N_DIR | num) para EP0 (num=0): deja seleccionado como IN sin N_SEL */
    return (int)n;
}

/* Leer el EP0 OUT: devuelve n o -1. */
static int ep0_leer(u8 *p, u32 max)
{
    u16 st, n; unsigned i;
    R16(EP_NUM) = N_SEL;
    st = R16(STAT_FLG);
    if ((st & S_FIFO_EN) || !(st & S_ACK)) { R16(EP_NUM) = 0; return -1; }
    n = R16(RXFSTAT);
    if (n > max) n = max;
    for (i = 0; i + 1 < n; i += 2) { u16 w = R16(DATA); p[i] = w; p[i + 1] = w >> 8; }
    if (i < n) p[i] = R8(DATA);
    R16(CTRL) = C_FIFO_EN;                          /* armar el siguiente OUT */
    R16(EP_NUM) = 0;
    return (int)n;
}

static int leer_setup_regs(u8 d[8])
{
    u16 w[4]; unsigned i, k;
    /* como senew hal/usb_dc.c:leer_setup: lee hasta 4 veces por si llega otro SETUP mientras procesamos. */
    R16(IRQ_SRC) = I_EP0_TX | I_EP0_RX;        /* lo viejo del EP0 ya no vale */
    for (k = 0; k < 4; k++) {
        R16(EP_NUM) = N_SETUP;
        for (i = 0; i < 4; i++) w[i] = R16(DATA);
        R16(EP_NUM) = 0;
        R16(IRQ_SRC) = I_SETUP;
        if (!(R16(IRQ_SRC) & I_SETUP)) break;
    }
    for (i = 0; i < 4; i++) { d[2 * i] = w[i]; d[2 * i + 1] = w[i] >> 8; }
    return 0;
}

/* ---- EP0: capítulo 9 ------------------------------------------------------------------------------------ */
static void ep0_stall(void)
{
    R16(EP_NUM) = N_SEL | N_DIR; R16(CTRL) = C_SET_HALT; R16(EP_NUM) = 0;
    R16(EP_NUM) = N_SEL;         R16(CTRL) = C_SET_HALT; R16(EP_NUM) = 0;
    u_fase = F_LIBRE;
}

static void ep0_bombear(void)
{
    u32 n;
    if (u_fase != F_DATOS_IN || ep0_ocupado) return;
    n = in_n - in_pos;
    if (n == 0 && !in_zlp) { u_fase = F_ESTADO_OUT; return; }
    if (n > 64) n = 64;
    if (ep0_escribir(in_p + in_pos, n) < 0) return;
    ep0_ocupado = 1;
    in_pos += n;
    if (n == 0) in_zlp = 0;
}
static void responder(const u8 *p, u32 n)
{
    u32 pedido = setup_pend[6] | (setup_pend[7] << 8);
    if (n > pedido) n = pedido;
    in_p = p; in_n = n; in_pos = 0;
    in_zlp = (n < pedido && (n % 64) == 0 && n > 0);
    if (n == 0) in_zlp = 1;
    u_fase = F_DATOS_IN;
    ep0_bombear();
}
static void estado_in(void)
{
    u_fase = F_ESTADO_IN;
    if (!ep0_ocupado && ep0_escribir(0, 0) >= 0) ep0_ocupado = 1;
}

static void peticion_estandar(u8 tipo, u8 pet, u16 valor, u16 indice)
{
    (void)indice;
    switch (pet) {
    case 0x06: /* GET_DESCRIPTOR */
        if (!(tipo & 0x80)) break;
        switch (valor >> 8) {
        case 1:
#ifdef BL_INSTALLER
            /* bl_entrada_actual lo pone bl_main antes del primer paso de USB (se declara en bl_main.c; aquí extern) */
            extern int bl_entrada_actual;
            if (bl_entrada_actual == BL_ENTRADA_SIN_MAIN) { responder(desc_dispositivo_installer, sizeof desc_dispositivo_installer); return; }
#endif
            responder(desc_dispositivo, sizeof desc_dispositivo); return;
        case 2: responder(desc_configuracion, sizeof desc_configuracion); return;
        case 3: {
            u8 i = valor & 0xFF;
            if (i == 0) { responder(str_lang, 4); return; }
            if (i == 1) { responder(str_fab, sizeof str_fab); return; }
            if (i == 2) { responder(str_prod, sizeof str_prod); return; }
            if (i == 3) { responder(str_serie, sizeof str_serie); return; }
            if (i == 0xEE) { responder(str_msos, sizeof str_msos); return; }
            break;
        }
        default: break;
        }
        break;
    case 0x05: /* SET_ADDRESS */
        if (tipo != 0x00 || valor > 127) break;
        u_dir_pend = (u8)valor | 0x80;
        estado_in();
        return;
    case 0x09: /* SET_CONFIGURATION */
        if (tipo != 0x00 || (valor & 0xFF) > 1) break;
        u_config = valor & 0xFF;
        estado_in();
        return;
    case 0x08: /* GET_CONFIGURATION */
        if (tipo != 0x80) break;
        bl_buf[BL_BUF - 2] = u_config;
        responder(bl_buf + BL_BUF - 2, 1); return;
    case 0x00: /* GET_STATUS */
        if (!(tipo & 0x80)) break;
        bl_buf[BL_BUF - 2] = 1; bl_buf[BL_BUF - 1] = 0;    /* autoalim */
        responder(bl_buf + BL_BUF - 2, 2); return;
    default: break;
    }
    ep0_stall();
}

/* Peticiones de vendedor del BL (bl.h). */
static BlId id_copia;
extern u32 bl_fallo_pc;
extern int bl_entrada_actual;                      /* 1 = bandera, 2 = sin main */
static void rellenar_id(void)
{
    unsigned i;
    static const char build_full[32] = __DATE__ " " __TIME__;    /* "Oct 23 2026 20:48:12" */
    const char *build = build_full;
    for (i = 0; i < 4; i++) id_copia.magia[i] = "SNBL"[i];
    id_copia.version = BL_VERSION;
    id_copia.entrada = bl_entrada_actual;
    id_copia.bl_ini = 0x44140000u;
    id_copia.main_ini = 0x44180000u;
    id_copia.main_fin = 0x443C0000u;
    id_copia.bloque = BL_BLOQUE;
    id_copia.region = BL_REGION;
    id_copia.buf = (u32)(size_t)bl_buf;
    id_copia.buf_tam = BL_BUF;
    id_copia.mhz = 13;                                 /* el BL no sube el PLL */
    id_copia.fallos = (bl_fallo_pc != 0);
    id_copia.fallo_pc = bl_fallo_pc;
    for (i = 0; i < sizeof id_copia.build; i++) id_copia.build[i] = (i < sizeof build - 1) ? build[i] : 0;
}

static int ep0_tiene_sitio(void) { return bl_res.estado != BL_ST_PENDIENTE && bl_res.estado != BL_ST_EJECUTANDO; }

/* Devuelve: 0 nada mío, 1 IN (en *p, *n), 2 OUT (*n esperado en EP0), 3 LISTO sin datos, 4 STALL. */
static int bl_ep0(const u8 s[8], const u8 **p, u32 *n)
{
    u8 tipo = s[0], pet = s[1];
    u32 valor = s[2] | (s[3] << 8), indice = s[4] | (s[5] << 8), largo = s[6] | (s[7] << 8);
    if ((tipo & 0x60) != 0x40) return 0;             /* no es vendor */
    if ((tipo & 0x1F) != 0) return 4;                /* solo al dispositivo */
    if (pet >= BL_R_ARG0 && pet < BL_R_ARG0 + BL_N_ARG) {   /* ARG */
        if (tipo != 0x40 || largo != 0) return 4;
        bl_arg[pet - BL_R_ARG0] = valor | (indice << 16);
        return 3;
    }
    switch (pet) {
    case BL_R_ID:
        if (tipo != 0xC0) return 4;
        rellenar_id();
        bl_res.ms = bl_hw_ms();
        *p = (const u8 *)&id_copia; *n = sizeof id_copia;
        return 1;
    case BL_R_RES:
        if (tipo != 0xC0) return 4;
        bl_res.ms = bl_hw_ms();
        *p = (const u8 *)&bl_res; *n = sizeof bl_res;
        return 1;
    case BL_R_PUT:
        if (tipo != 0x40 || (u32)valor + largo > BL_BUF || !ep0_tiene_sitio()) return 4;
        if (!largo) return 3;
        out_dst_tipo = 1; out_dst_off = valor; out_dst_n = largo; out_n = 0;
        *p = bl_buf + valor; *n = largo;
        return 2;
    case BL_R_GET:
        if (tipo != 0xC0) return 4;
        {
            u32 dir = valor | (indice << 16);
            /* permitido: NOR 0x44000000..0x47FFFFFF (lectura mapeada), ITCM 0..0xFFFF, DTCM 0x10000..0x13FFF, iRAM 0xF3000000..0xF3003FFF */
            /* evitar u64 (sin libgcc 64-bit): comprobar que largo <= tope - dir en u32 */
            int ok = (largo <= 64) && (
                   (dir >= 0x44000000u && dir < 0x48000000u && largo <= 0x48000000u - dir)
                || (dir < 0x10000u && largo <= 0x10000u - dir)
                || (dir >= 0x10000u && dir < 0x14000u && largo <= 0x14000u - dir)
                || (dir >= 0xF3000000u && dir < 0xF3004000u && largo <= 0xF3004000u - dir));
            if (!ok) return 4;                       /* nivel 0: respuestas de 64 B */
            {
                static u8 getbuf[64]; unsigned i;
                for (i = 0; i < largo; i++) getbuf[i] = *(volatile u8 *)(size_t)(dir + i);
                *p = getbuf; *n = largo;
                return 1;
            }
        }
    case BL_R_CUR:
        if (tipo != 0x40 || largo != 0 || valor + 4 > BL_BUF) return 4;
        bl_cursor = valor;
        return 3;
    case BL_R_POKE:
        if (tipo != 0x40 || largo != 0 || bl_cursor + 4 > BL_BUF) return 4;
        {
            u32 v = valor | (indice << 16);
            bl_buf[bl_cursor + 0] = (u8)v;
            bl_buf[bl_cursor + 1] = (u8)(v >> 8);
            bl_buf[bl_cursor + 2] = (u8)(v >> 16);
            bl_buf[bl_cursor + 3] = (u8)(v >> 24);
            bl_cursor += 4;
            return 3;
        }
    case BL_R_GO:
        if (tipo != 0x40 || largo != 0 || !ep0_tiene_sitio()) return 4;
        {
            u8 op = (u8)valor, seq = (u8)(valor >> 8);
            int ok = op < BL_OP_N;
#ifdef BL_INSTALLER
            /* las órdenes installer (0xD4..0xD7) solo valen con el BL en modo installer: la bandera BLOA en el modo clásico no
             * usa pantalla ni WRITE_PROPIO. El dispatch de bl_main responde BL_E_MODO si el anfitrión lo intenta. */
            if (!ok && (op == BL_OP_WRITE_PROPIO || op == BL_OP_INSTALADOR_INI || op == BL_OP_LISTO || op == BL_OP_WIPE_PROPIO)) ok = 1;
#endif
            if (!ok) return 4;
            bl_cmd_poner(op, seq);
            return 3;
        }
    }
    return 4;
}

static void fin_datos_out(void)
{
    /* En el BL, el único OUT con fase de datos es PUT: ya se copió en el EP0 OUT recibido. */
    out_dst_tipo = 0;
    estado_in();
}

static void peticion_vendedor(u8 tipo, u8 pet, u16 valor, u16 indice, u16 largo)
{
    const u8 *p = 0; u32 n = 0;
    int r;
    (void)valor; (void)largo;
    /* MS OS 1.0, antes que bl_ep0 (que solo admite peticiones al dispositivo): Windows pide la compat ID como dispositivo (0xC0)
     * o como interfaz (0xC1), y las propiedades como interfaz */
    if (pet == BL_VENDOR_MS && (tipo == 0xC0 || tipo == 0xC1) && indice == 4) { responder(desc_msos_compat, sizeof desc_msos_compat); return; }
    if (pet == BL_VENDOR_MS && (tipo == 0xC0 || tipo == 0xC1) && indice == 5) { responder(desc_msos_props, sizeof desc_msos_props); return; }
    r = bl_ep0(setup_pend, &p, &n);
    if (r == 1) { responder(p, n); return; }
    if (r == 2) { u_fase = F_DATOS_OUT; out_n = 0; return; }
    if (r == 3) { estado_in(); return; }
    ep0_stall();
}

/* Quita STALL del EP0 (ambas direcciones). hal/usb_dc.h comenta que el hardware lo hace solo al llegar SETUP, pero en silicio
 * parece que no pasa (en v19.5 el 1er intento leía device descriptor OK y el 2º intento STALLaba en config → STALL residual). */
static void ep0_quitar_halt(void)
{
    R16(EP_NUM) = N_SEL | N_DIR;
    R16(CTRL) = C_CLR_HALT;
    R16(EP_NUM) = N_DIR;
    R16(EP_NUM) = N_SEL;
    R16(CTRL) = C_CLR_HALT;
    R16(EP_NUM) = 0;
}

static void setup_llega(void)
{
    u8 tipo, pet; u16 valor, indice, largo;
    leer_setup_regs(setup_pend);
    setup_hay = 1;
    ep0_quitar_halt();                                /* por si quedó STALL de un intento anterior */
    u_fase = F_LIBRE; ep0_ocupado = 0; in_zlp = 0;
    tipo = setup_pend[0]; pet = setup_pend[1];
    valor = setup_pend[2] | (setup_pend[3] << 8);
    indice = setup_pend[4] | (setup_pend[5] << 8);
    largo = setup_pend[6] | (setup_pend[7] << 8);
    /* El EP0 OUT: armarlo si hay fase de datos IN (para que llegue el ZLP de la fase de estado OUT) o si hay datos OUT.
     * Igual que hal/usb_dc.c (leer_setup, ep0_out_esperado). */
    if ((tipo & 0x80) || largo > 0) { R16(EP_NUM) = 0; R16(CTRL) = C_FIFO_EN; }
    switch (tipo & 0x60) {
    case 0x00: peticion_estandar(tipo, pet, valor, indice); break;
    case 0x40: peticion_vendedor(tipo, pet, valor, indice, largo); break;
    default: ep0_stall(); break;
    }
}
static void ep0_rx_tramo(void)
{
    u8 b[64]; int n = ep0_leer(b, sizeof b);
    unsigned i;
    if (n < 0) return;
    if (u_fase == F_DATOS_OUT) {
        if (out_dst_tipo == 1) {
            if (out_n + n > out_dst_n) { ep0_stall(); return; }
            for (i = 0; i < (unsigned)n; i++) bl_buf[out_dst_off + out_n + i] = b[i];
            out_n += n;
            if (out_n >= out_dst_n || n < 64) fin_datos_out();
        } else {
            /* no esperábamos datos */
            ep0_stall();
        }
        return;
    }
    if (u_fase == F_ESTADO_OUT || u_fase == F_DATOS_IN) { u_fase = F_LIBRE; return; }
}
static void ep0_tx_hecho(void)
{
    ep0_ocupado = 0;
    if (u_fase == F_DATOS_IN) { ep0_bombear(); return; }
    if (u_fase == F_ESTADO_IN) {
        u_fase = F_LIBRE;
        if (u_dir_pend) {
            u8 d = u_dir_pend & 0x7F; u_dir_pend = 0;
            /* el UDC se la aplica solo: no hace falta usbdc_direccion */
            (void)d;
        }
    }
}

/* Sondeo del UDC: una vuelta. */
static u16 devstat_antes_bl;
void bl_usb_paso(void)
{
    u16 src = R16(IRQ_SRC);
    if (src & I_DS_CHG) {
        /* Procesar el cambio de estado como hace senew: cuando el bus RESET se termina, re-inicializar el estado del EP0
         * (igual que senew hace en cambio_estado). Sin esto, el host hace el bus reset de la enumeración y el BL ignora el
         * suceso: los SETUPs posteriores no se procesan bien. */
        u16 d = R16(DEVSTAT), x = d ^ devstat_antes_bl;
        devstat_antes_bl = d;
        R16(IRQ_SRC) = I_DS_CHG;
        if ((x & D_RESET) && !(d & D_RESET)) {
            /* bus reset terminó */
            R16(IRQ_EN) = I_DS_CHG | I_SETUP | I_EP0_TX | I_EP0_RX;
            setup_hay = 0;
            u_fase = F_LIBRE;
            ep0_ocupado = 0;
            u_dir_pend = 0;
            u_config = 0;
            out_dst_tipo = 0;
        }
    }
    if (src & I_SETUP) { R16(IRQ_SRC) = I_SETUP; setup_llega(); }
    else {
        if (src & I_EP0_TX) { R16(IRQ_SRC) = I_EP0_TX; ep0_tx_hecho(); }
        if (src & I_EP0_RX) { R16(IRQ_SRC) = I_EP0_RX; ep0_rx_tramo(); }
    }
}

/* Argumentos de órdenes (los pone bl_main al despachar). */
u32 *bl_arg_actual(void) { return bl_arg; }
u8 *bl_buf_actual(void) { return bl_buf; }
