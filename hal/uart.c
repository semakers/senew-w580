/* UART por sondeo. docs/hal.md §8 (eipd_marita-uarts.c): mismos registros que el DB2010, otras bases y
 * otra numeración de relojes. La de las trazas es la UART 0 (0xFB060000): puerta 33, bloque 23, IRQ 46.
 *
 * Diferencia con el W600: allí un puerto sin cable se daba por muerto y no se escribía. Aquí se transmite
 * SIEMPRE (el emulador de la pista E no modela el pin de detección, P0.0, y lo transmitido sale en su
 * registro como «W580_UART0 tx»); lo que depende del cable es solo poner P0.1 como salida (TX), que es lo
 * que hace el original (FUN_4415dc88). La espera de hueco en la FIFO tiene tope: si vence, el puerto se da
 * por muerto y no vuelve a parar al sistema. [sin saber] la ruta de función alternativa de P0.0/P0.1 en el
 * aparato (hal.md §8): sin ella, por el conector no sale nada. */
#include "hal.h"

static u8 muerto;

int uart_cable(void) { return R16(0xF9000000) & 0x01; }      /* P0.0: la línea RX en reposo */

int uart_init(u32 b)                               /* como FUN_4422075c(0, …); devuelve 1 si hay cable */
{
    int cable;
    R16(SYSCON + 0x1A) &= (u16)~0x0004u;           /* puerta 33 */
    R16(SYSCON + 0x14) |= 0x0004;                  /* bloque 23 */
    R16(b + 0x04) = 7;     R16(b + 0x0A) = 0x100;  R16(b + 0x0C) = 0x1801;
    R16(b + 0x12) = 0;     R16(b + 0x16) = 0;
    R16(b + 0x20) = 4;     /* 115 200 = 460 800 / 4 */
    R16(b + 0x22) = 9;     R16(b + 0x26) = 13000;
    cable = uart_cable();
    if (cable) R16(0xF9000002) = (u16)((R16(0xF9000002) | 0x02) & 0xFF);   /* P0.1 salida (TX) */
    muerto = 0;
    return cable;
}

void uart_putc(u32 b, char c)
{
    u32 tope = 2000000;
    if (muerto) return;
    while ((R16(b + 0x2C) & 0x1FF) == 0)           /* hueco en la FIFO de transmisión */
        if (!--tope) { muerto = 1; return; }
    R16(b + 0x02) = (u8)c;
}

void uart_puts(u32 b, const char *s)
{
    while (*s) { if (*s == '\n') uart_putc(b, '\r'); uart_putc(b, *s++); }
}

void uart_hex(u32 b, u32 v)
{
    int i;
    for (i = 28; i >= 0; i -= 4) uart_putc(b, "0123456789ABCDEF"[(v >> i) & 15]);
}

void uart_dec(u32 b, u32 v)
{
    char t[11]; int n = 10;
    t[10] = 0;
    do { t[--n] = (char)('0' + v % 10); v /= 10; } while (v);
    uart_puts(b, t + n);
}

/* Recepción por sondeo: +2E = bytes en la FIFO de recepción, +00 = saca uno. [observado] en el
 * emulador; en el firmware la recepción va por interrupción (+14 bit 0) y solo se ve leer +00. */
int uart_getc(u32 b)
{
    if (muerto) return -1;
    return (R16(b + 0x2E) & 0x1FF) ? (int)(R16(b + 0x00) & 0xFF) : -1;
}

/* Recepción POR INTERRUPCIÓN (línea 46 la UART 0; hal.md §8): +12 = máscara (0x41 = recepción), +14 =
 * pendientes, que se reconocen escribiendo el bit (el original escribe 0x40). [emulador: nadie al otro lado] El manejador vacía la FIFO a un anillo de 1 KiB, así
 * 115 200 baudios no dependen de cada cuánto sondea la aplicación. Si la interrupción no llegara nunca
 * (en un teléfono está sin probar), uart_rx sigue sacando lo que haya en la FIFO por sondeo. */
#define ANILLO 1024u
static volatile u8 anillo[ANILLO];
static volatile u32 an_pon, an_saca;
volatile u32 uart_rx_irqs, uart_rx_perdidos;
static void vaciar_fifo(void)
{
    u32 tope = 256;
    while ((R16(UART0 + 0x2E) & 0x1FF) && tope--) {
        u8 c = (u8)R16(UART0 + 0x00);
        if (an_pon - an_saca < ANILLO) { anillo[an_pon % ANILLO] = c; an_pon++; } else uart_rx_perdidos++;
    }
}
static void uart0_irq(void)
{
    u16 p = R16(UART0 + 0x14);
    uart_rx_irqs++;
    vaciar_fifo();
    R16(UART0 + 0x14) = p;                             /* reconoce lo que había */
}
/* (R3; AUD3-B10) LA RECEPCIÓN SOLO CON CABLE. El original no usa el puerto si P0.0 (la línea RX en reposo) no está a 1
 * (4422007C). Con la línea al aire [sin saber] si la IRQ 46 queda pendiente sin parar: sería CPU perdida y un borrado de flash
 * que se suspende a cada vuelta. Sin cable no se abre; uart_rx() lo vuelve a mirar por si lo enchufan después. (La
 * transmisión sigue yendo siempre: es la traza. El emulador de E no modela P0.0 ni tiene RX: `-serial none`.) */
static u8 rx_abierta;
void uart_rx_irq_init(void)
{
    if (muerto || rx_abierta || !uart_cable()) return;
    rx_abierta = 1;
    intc_habilita(46, uart0_irq);
    R16(UART0 + 0x14) = R16(UART0 + 0x14);
    R16(UART0 + 0x12) = 0x0041;                        /* solo recepción */
}
int uart_rx(u8 *d, int max)                            /* lo recibido hasta ahora (anillo + lo que quede en la FIFO) */
{
    int n = 0; u32 cerradas;
    if (muerto) return 0;
    if (!rx_abierta) { uart_rx_irq_init(); if (!rx_abierta) return 0; }      /* (R3) sin cable no hay recepción; si lo han puesto, se abre ahora */
    cerradas = irq_cerrar(); vaciar_fifo(); if (!cerradas) irq_abrir();
    while (n < max && an_saca != an_pon) { d[n++] = anillo[an_saca % ANILLO]; an_saca++; }
    return n;
}
