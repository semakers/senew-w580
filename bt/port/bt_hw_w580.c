/* bt_hw_w580.c — bt_hw.h en el W580i: el STLC2500 por la UART 3. Primero en el emulador, a nivel de registros, contra un
 * STLC2500 falso (bt/prueba_emu/LEEME.md); desde el 2026-10-25, EN EL APARATO (docs/bluetooth.md §10). Compila con las banderas del sistema (bt/Makefile,
 * objetivo «arm») y cada registro cita docs/bluetooth.md. Es la propuesta para
 * sistema/hal/bt.c cuando se integre (no se ha tocado sistema/).
 *
 * Por sondeo, como hal/uart.c: la línea de interrupción de la UART 3 es [sin saber] (la de la UART 0 es la 46).
 * Sin interrupción, el control de flujo RTS/CTS (+16 = 0xC0, [inferido] qué hace cada bit) es lo que evita perder bytes cuando el
 * bucle tarda: el teléfono baja RTS al llenarse la FIFO y el chip espera. Si eso no fuera así, primera cosa a
 * medir en el aparato (docs/pendiente_del_aparato.md, propuesta 37 de docs/bluetooth.md §7). */
#include "bt_hw.h"
#include "hal.h"

#define U3(o) R16(UART3 + (o))
/* (velocidad, 2026-10-25) LA UART 3 POR INTERRUPCIÓN: línea 49, por NIVEL [observado en el aparato con
 * sistema/herr/puente/bt/crudo_irq.c]. +12 = máscara (0x41 recepción: umbral +06 y «parado»; 0x02 hay sitio para transmitir),
 * +14 = estado de lo habilitado (el 0x40 se reconoce escribiéndolo; 0x18 son errores), como el manejador común del original
 * (0x44220B8C, que despacha a FUN_442208a4 / FUN_44220944 / FUN_442209ec). La rutina mueve FIFO <-> anillos; BTstack sigue
 * trabajando por sondeo en sus pasos, pero el chip ya no se para entre paso y paso (las FIFO son de 256 B: a 1 843 200, 1,4 ms).
 * Sin la interrupción (bt_hw_usar_irq = 0, o BT_HW_IRQ=0 al compilar) es el sondeo de siempre. */
#ifndef BT_HW_IRQ
#define BT_HW_IRQ 1
#endif
#define RXN 16384u                                   /* ~90 ms a 1 843 200: una app a 30 cuadros/s no lo llena */
#define TXN 8192u
int bt_hw_usar_irq = BT_HW_IRQ;                      /* se mira al abrir; el puente lo puede poner a 0 */
uint32_t bt_hw_irq_llamadas, bt_hw_rx_lleno;
static u8 rx_an[RXN], tx_an[TXN];
static volatile u32 rx_ent, rx_sal, tx_ent, tx_sal;  /* entradas y salidas, solo crecen: ocupado = ent - sal */
static volatile u16 mascara;                         /* copia de +12 */
static volatile u8 con_irq, abierta;
#ifndef BT_HW_FLUJO
#define BT_HW_FLUJO 0xC0                             /* +16; solo la prueba negativa de bt/prueba_emu lo cambia (0 = sin RTS/CTS) */
#endif

extern volatile u32 tics;                            /* hal/timer.c: 250 por segundo */

static void espera_bit4(void)                        /* +0E bit 4: el bloque está listo / transmisor vacío */
{
    u32 t0 = tics;
    while (!(U3(0x0E) & 0x10) && tics - t0 < 250) { }
}

uint32_t bt_hw_rx_errores;                           /* bytes tirados por llegar con marca de error (abajo, en bt_hw_rx) */
static void mascara_pon(u16 m) { mascara = m; U3(0x12) = m; }
static void drenar_rx(void)                          /* FIFO -> anillo: desde la interrupción o con las IRQ cerradas */
{
    u32 k = 0;
    while (k < 320 && (U3(0x2E) & 0x1FF)) {
        u16 v;
        if (rx_ent - rx_sal >= RXN) {                /* lleno: se deja en la FIFO (RTS frena al chip) y se tapa la recepción */
            bt_hw_rx_lleno++;
            if (mascara & 0x41) mascara_pon((u16)(mascara & ~0x41u));
            return;
        }
        v = U3(0x00); k++;
        if (v & 0xFF00u) { bt_hw_rx_errores++; continue; }
        rx_an[rx_ent & (RXN - 1u)] = (u8)v; rx_ent++;
    }
}
static void bombear_tx(void)                         /* anillo -> FIFO; sin nada que mandar, fuera la interrupción de «hay sitio» */
{
    u32 h = U3(0x2C) & 0x1FF;
    while (h && tx_sal != tx_ent) { U3(0x02) = tx_an[tx_sal & (TXN - 1u)]; tx_sal++; h--; }
    if (tx_sal == tx_ent && (mascara & 2)) mascara_pon((u16)(mascara & ~2u));
}
static void isr_u3(void)
{
    u32 i;
    bt_hw_irq_llamadas++;
    if (!abierta) { intc_tapar(49); return; }        /* nunca un registro de la UART con la puerta cerrada */
    for (i = 0; i < 16; i++) {
        u16 s = (u16)(U3(0x14) & mascara);
        if (!s) break;
        if (s & 0x41) { drenar_rx(); if (s & 0x40) U3(0x14) = 0x40; }
        if (s & 0x02) bombear_tx();
    }
}

void bt_hw_reset(int activo)                         /* §3: Do_SYSCON_SetExternalReset(2, x) */
{
    if (activo) R16(SYSCON + 0x04) &= (u16)~0x4000u; /* línea a 0: chip en reset */
    else        R16(SYSCON + 0x04) |= 0x4000u;
}

int bt_hw_vivo(void) { return gpio_get(2, 0x01); }   /* §3: P2.0 (RX del teléfono = TX del chip en reposo) a 1 */
/* P2.2 = CTS del teléfono = RTS del chip, activo a 0. [observado en el aparato, 2026-10-25, herr/puente/bt/crudo.c]: P2.0 sube al soltar
 * el reset (< 1 ms), pero P2.2 no baja hasta los ~56 ms: ahí empieza a escuchar. Y la UART SÍ retiene la transmisión hasta entonces
 * (+16 = 0xC0: el contador de TX +28 no se mueve hasta que baja P2.2). */
int bt_hw_escucha(void) { return !gpio_get(2, 0x04); }

int bt_hw_baudios(uint32_t b)                        /* §2.2: escala rápida, 1 843 200 / divisor en +20 */
{
    u16 div; int u;
    switch (b) {
    case 115200: div = 16; break; case 230400: div = 8; break; case 460800: div = 4; break;
    case 921600: div = 2;  break; case 1843200: div = 1; break; default: return -1;
    }
    espera_bit4();                                   /* como uarts_set_format: que se vacíe antes */
    U3(0x20) = div;
    /* (velocidad) los umbrales como FUN_4421ff98 del original (latencia 0): +08 = cuándo bajamos RTS (0xFF - b/12 500: más
     * margen cuanto más rápido, el chip no para en seco) y +06 = cuándo interrumpe la recepción (0xFF - (b·125/10⁶ + 0x21), ≥ 0x19).
     * 115 200: 246 / 208; 921 600: 182 / 107; 1 843 200: 108 / 25. */
    U3(0x08) = (u16)(0xFF - b / 12500u);
    u = 0xFF - (int)(b / 8000u + 0x21);
    U3(0x06) = (u16)(u < 0x19 ? 0x19 : u);
    return 0;
}

int bt_hw_abrir(void)
{
    /* §3: TX (P2.1) y RTS del teléfono (P2.3) a 1, como EILD_HCITL_FUN_4415cd58. P2.0/P2.2 entradas; la función
     * alternativa del puerto 2 (+44 = 0x0055) ya la deja gpio_init (docs/hal.md §9). */
    gpio_dir(2, 0x02 | 0x08, 1);
    gpio_set(2, 0x02 | 0x08, 1);
    /* §2.2: puerta 35 y bloque 25 (la UART 0 es puerta 33 = +1A bit 2 y bloque 23 = +14 bit 2) */
    R16(SYSCON + 0x1A) &= (u16)~0x0010u;
    R16(SYSCON + 0x14) |= 0x0010;
    espera_bit4();
    /* uarts_hw_defaults */
    U3(0x04) = 7;     U3(0x06) = 0x80;  U3(0x08) = 0x80;  U3(0x0A) = 0x100; U3(0x0C) = 0x1801;
    U3(0x12) = 0;     U3(0x16) = 0;     U3(0x18) = 1;     U3(0x1A) = 0x38;  U3(0x1C) = 0x11;
    U3(0x1E) = 0x13;  U3(0x20) = 4;     U3(0x22) = 9;     U3(0x24) = 10;    U3(0x26) = 13000;
    /* lo que hace el HCITL (FUN_4415d0d4) */
    U3(0x0A) &= (u16)~0x100u;                        /* escala rápida (1 843 200) */
    U3(0x0C) = (u16)((U3(0x0C) & ~0x1Fu) | 0x01);    /* 8N1 */
    bt_hw_baudios(115200);                           /* +20 = 16 */
    /* RTS/CTS por hardware: uarts_set_flow(h, 1, 1) del HCITL (FUN_4415d0d4, «Enable hardware flow»; los dos argumentos a 1
     * [observado en el desensamblado 0x4415D120..24]) = 0x80 (RTS del teléfono automático) | 0x40 (obedecer el CTS del chip);
     * el original escribe 0xC0 [observado en el emulador, tablas aprendidas]. Antes aquí 0x80: docs/bluetooth.md §2.2 decía
     * «uarts_set_flow(h, 1) -> 0x80» y se le escapaba el tercer argumento (el estricto de bt/prueba_emu lo cazó). */
    U3(0x16) = BT_HW_FLUJO;
    rx_ent = rx_sal = tx_ent = tx_sal = 0;
    abierta = 1;
    con_irq = (u8)(bt_hw_usar_irq != 0);
    if (con_irq) {
        U3(0x14) = 0x1FF;                            /* lo que hubiera pendiente, reconocido */
        intc_habilita_nivel(49, isr_u3, 12);
        mascara_pon(0x41);
    }
    return 0;
}

void bt_hw_cerrar(void)
{
    if (con_irq) { u32 e = irq_cerrar(); mascara_pon(0); intc_tapar(49); con_irq = 0; if (!e) irq_abrir(); }
    abierta = 0;                                     /* y ANTES de cerrar la puerta: un registro de la UART sin reloj cuelga el bus */
    gpio_set(2, 0x02 | 0x08, 0);                     /* §3 al apagar (EILD_HCITL_FUN_4415cde4) */
    R16(SYSCON + 0x1A) |= 0x0010;                    /* puerta 35 cerrada */
    bt_hw_reset(1);
}

/* ---- transmisión: con interrupción, al anillo; bt_hw_tx_fin() empuja lo que quepa en la FIFO y deja la interrupción de «hay sitio»
 * (0x02) puesta mientras quede algo. Sin ella, directo a la FIFO como siempre. */
int  bt_hw_tx_hueco(void) { return con_irq ? (int)(TXN - (tx_ent - tx_sal)) : (U3(0x2C) & 0x1FF); }   /* hal.md §8: +2C = sitio en la FIFO */
void bt_hw_tx(uint8_t c)
{
    if (!con_irq) { U3(0x02) = c; return; }
    tx_an[tx_ent & (TXN - 1u)] = c; tx_ent++;
}
void bt_hw_tx_fin(void)
{
    u32 e;
    if (!con_irq || tx_sal == tx_ent) return;
    e = irq_cerrar();
    bombear_tx();
    if (tx_sal != tx_ent && !(mascara & 2)) mascara_pon((u16)(mascara | 2));
    if (!e) irq_abrir();
}
int  bt_hw_tx_vacio(void) { return (!con_irq || tx_sal == tx_ent) && (U3(0x0E) & 0x10) != 0; }

/* (la rutina y sus ayudantes están arriba, junto a bt_hw_reset) +00 trae el dato en los bits 0..7 y las MARCAS DE ERROR en 8..15 (docs/hal.md §8; el original las junta en FUN_4421fec0 y se
 * las pasa al HCITL). [observado en el aparato, 2026-10-25, herr/puente/bt/crudo.c]: al soltar el nRESET con la UART ya abierta
 * (la línea venía de estar a 0 = «break») entra UN byte basura, `01` con +00 = 0x0201; los bytes buenos llegan con el byte alto a
 * 0. Un `01` suelto es el tipo «orden» de H4 y BTstack 1.8.3 no tiene salida para él (ver hal_uart_bloques.c): se tiran aquí. */
int bt_hw_rx(uint8_t *d, int max)
{
    int k = 0, vueltas = 0;
    if (con_irq) {
        u32 e = irq_cerrar();
        drenar_rx();                                 /* por si la interrupción no llegara: el sondeo de siempre como respaldo */
        if (!(mascara & 0x41) && rx_ent - rx_sal < RXN / 2) mascara_pon((u16)(mascara | 0x41));   /* hay sitio otra vez */
        if (!e) irq_abrir();
        while (k < max && rx_sal != rx_ent) { d[k++] = rx_an[rx_sal & (RXN - 1u)]; rx_sal++; }
        return k;
    }
    while (k < max && vueltas < 512 && (U3(0x2E) & 0x1FF)) {     /* +2E = bytes en la FIFO de RX [observado en el aparato] */
        u16 v = U3(0x00);
        vueltas++;
        if (v & 0xFF00u) { bt_hw_rx_errores++; continue; }
        d[k++] = (uint8_t)v;
    }
    return k;
}

uint32_t bt_hw_ms(void) { return tics * 4u; }       /* el tic es de 4 ms: a BTstack le basta */

void hw_log(const char *s);
/* (0.29.3) también al registro del sistema: lo que dice el servicio Bluetooth (SDP, PIN, el puente que llama) se lee con `pu.py log`,
 * no solo por la UART 0, que en el aparato no se ve. */
void bt_hw_log(const char *s) { uart_puts(UART0, s); uart_puts(UART0, "\n"); hw_log(s); hw_log("\n"); }
