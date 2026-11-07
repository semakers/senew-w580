/* hal_uart_bloques.c — hal_uart_dma.h, hal_time_ms.h y hal_cpu.h de BTstack sobre bt_hw.h, POR SONDEO.
 *
 * BTstack (btstack_uart_block_embedded.c) pide «manda este bloque» y «avísame cuando hayan llegado N bytes»;
 * espera que una DMA lo haga y llame a las funciones de aviso desde la interrupción. Aquí no hay DMA: el
 * trabajo lo hace hal_uart_bloques_sondear(), que el servicio llama en cada vuelta antes del run loop.
 * Las funciones de aviso de btstack_uart_block_embedded solo levantan una bandera y despiertan el run loop,
 * así que llamarlas desde aquí (fuera de interrupción) es exactamente lo que esperan.
 *
 * Cambio de velocidad: BTstack llama hal_uart_dma_set_baud() cuando quiere. Si aún queda algo por salir,
 * se espera a que la transmisión se vacíe (como uarts_set_format del original, docs/bluetooth.md §2.4). */
#include "bt_hw.h"
#include "hal_uart_dma.h"
#include "hal_time_ms.h"
#include "hal_cpu.h"
#include <stddef.h>

static void (*aviso_recibido)(void);
static void (*aviso_enviado)(void);

static const uint8_t *tx_p;  static uint16_t tx_n;      /* bloque en curso de transmisión */
static uint8_t       *rx_p;  static uint16_t rx_n;      /* bloque pedido de recepción */

/* contadores para el LEEME / la pantalla de diagnóstico */
uint32_t bt_hal_tx_bytes, bt_hal_rx_bytes;

/* EL BYTE DE TIPO (2026-10-25, en el aparato): H4 lee el tipo de paquete SIEMPRE en &hci_packet[0], y es la primera petición tras
 * abrir el transporte (btstack_uart_embedded_open -> hal_uart_dma_init; luego reset_statemachine -> 1 byte en hci_packet[0]).
 * BTstack 1.8.3 acepta ahí `01` (orden), pasa a H4_W4_CMD_HEADER y ESE ESTADO NO TIENE CASE en hci_transport_h4_block_read: cae en
 * btstack_assert(false) (vacío con NDEBUG), read_pos crece sin parar y la recepción DESBORDA hci_packet y pisa la RAM de al lado.
 * En el aparato: un `01` basura al soltar el nRESET, BTstack mudo repitiendo HCI_Reset y a los segundos el perro de software con
 * el PC en la RAM de bt_servicio (0x203DCECA). Un anfitrión solo recibe eventos (04) y ACL (02): lo demás en esa posición se tira. */
static uint8_t *tipo_p;
uint32_t bt_hal_rx_tirados;
void hal_uart_dma_init(void)                         { tx_p = NULL; tx_n = 0; rx_p = NULL; rx_n = 0; tipo_p = NULL; }
void hal_uart_dma_set_block_received(void (*f)(void)) { aviso_recibido = f; }
void hal_uart_dma_set_block_sent(void (*f)(void))     { aviso_enviado = f; }
void hal_uart_dma_set_csr_irq_handler(void (*f)(void)) { (void)f; }   /* sin despertar por CTS (eHCILL/ahorro: no) */
void hal_uart_dma_set_sleep(uint8_t s)                { (void)s; }    /* nunca se deja dormir al chip (§3 «Ahorro») */

int hal_uart_dma_set_baud(uint32_t baud)
{
    uint32_t t0 = bt_hw_ms();
    while (!bt_hw_tx_vacio() && bt_hw_ms() - t0 < 50) { }
    return bt_hw_baudios(baud);
}

void hal_uart_dma_send_block(const uint8_t *buf, uint16_t n)
{
    tx_p = buf; tx_n = n;                            /* sale en la próxima hal_uart_bloques_sondear() */
}

void hal_uart_dma_receive_block(uint8_t *buf, uint16_t n)
{
    if (!tipo_p) tipo_p = buf;                       /* la primera tras abrir: el tipo */
    rx_p = buf; rx_n = n;
}

/* Una vuelta: mete lo que quepa en la FIFO de TX y saca lo que haya en RX hacia el bloque pedido. Devuelve 1 si
 * ha pasado algo (para que quien llama sepa que no puede dormir). */
int hal_uart_bloques_sondear(void)
{
    int algo = 0;
    if (tx_n) {
        int h = bt_hw_tx_hueco();
        while (h-- > 0 && tx_n) { bt_hw_tx(*tx_p++); tx_n--; bt_hal_tx_bytes++; algo = 1; }
        bt_hw_tx_fin();
        if (!tx_n) { tx_p = NULL; if (aviso_enviado) aviso_enviado(); }
    }
    while (rx_n) {                                   /* H4 pide 1, 2/4 y luego N: un bloque a la vez */
        int k = bt_hw_rx(rx_p, rx_n);
        if (k <= 0) break;
        if (rx_p == tipo_p && rx_n == 1 && *rx_p != 0x04 && *rx_p != 0x02) {   /* ni evento ni ACL: fuera (arriba) */
            bt_hal_rx_tirados++; bt_hal_rx_bytes++; algo = 1;
            continue;
        }
        rx_p += k; rx_n = (uint16_t)(rx_n - k); bt_hal_rx_bytes += (uint32_t)k; algo = 1;
        if (!rx_n) { rx_p = NULL; if (aviso_recibido) aviso_recibido(); }
    }
    return algo;
}

uint32_t hal_time_ms(void) { return bt_hw_ms(); }

/* El run loop embebido apaga las interrupciones para mirar si hay algo que hacer y, si no, «duerme». En senew el
 * servicio NO duerme dentro de BTstack: vuelve al bucle del sistema, que es el que decide cuándo dormir. */
void hal_cpu_disable_irqs(void) { }
void hal_cpu_enable_irqs(void) { }
void hal_cpu_enable_irqs_and_sleep(void) { }

int hal_uart_bloques_tx_ocupado(void) { return tx_n != 0; }
