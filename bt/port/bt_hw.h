/* bt_hw.h — lo ÚNICO que el servicio Bluetooth necesita del aparato. Dos implementaciones:
 *   bt_hw_w580.c  el W580i: UART 3 (0xFB080000), nRESET en SYSCON+04 bit 14, P2.0 = RX del chip (docs/bluetooth.md §2–§3)
 *   bt_hw_pty.c   el PC: un pty con el controlador falso al otro lado (bt/prueba/stlc2500_falso.py)
 * Encima va hal_uart_bloques.c (hal_uart_dma.h de BTstack) y el run loop embebido. Todo por SONDEO: el servicio
 * llama a bt_paso() desde el bucle del sistema; nada corre en interrupción salvo, en el teléfono, el vaciado de la FIFO. */
#ifndef BT_HW_H
#define BT_HW_H
#include <stdint.h>

int      bt_hw_abrir(void);                 /* reloj y pines de la UART, 115 200 8N1, RTS/CTS; 0 = bien */
void     bt_hw_cerrar(void);                /* suelta la UART y deja el chip en reset */
void     bt_hw_reset(int activo);           /* 1 = nRESET a 0 (chip parado), 0 = suelto */
int      bt_hw_vivo(void);                  /* 1 si la línea RX (P2.0) está a 1: el chip ha salido de reset */
int      bt_hw_escucha(void);               /* 1 si el chip ya acepta datos (su RTS = nuestra CTS afirmada); el pty: siempre */
int      bt_hw_baudios(uint32_t b);         /* 0 = bien; solo las de la tabla de la escala rápida */
int      bt_hw_tx_hueco(void);              /* cuántos bytes caben ya en la FIFO de transmisión (0 = llena) */
void     bt_hw_tx(uint8_t c);
void     bt_hw_tx_fin(void);                /* tras una ráfaga de bt_hw_tx: empujarla (W580 por interrupción); el pty: nada */
int      bt_hw_tx_vacio(void);              /* 1 cuando el último bit ha salido (para cambiar de velocidad) */
int      bt_hw_rx(uint8_t *d, int max);     /* no bloquea: bytes copiados */
uint32_t bt_hw_ms(void);                    /* reloj en ms (el tic del sistema) */
void     bt_hw_log(const char *s);          /* trazas (UART 0 en el teléfono, stderr en el PC) */

#endif
