/* bl_syscon.c — SYSCON mínimo del bootloader: espera_ms (timer 2) y los relojes del USB. SIN PLL, SIN SDRAM.
 *
 * El USB funciona a 13 MHz: el original solo pide no bajar de 13 MHz con el USB encendido, y aquí no se baja. No encendemos el
 * PLL ni la SDRAM: no hace falta nada de eso para hablar USB, y las secuencias que colgaron el aparato están todas documentadas
 * en memorias (syscon-2c-bits-8-9-cuelgan, f5000800-bit-14-no-persiste): se evitan.
 *
 * Los relojes del USB vienen de tmp/usb_re/estatico.md (medido en el aparato en 2026-10-20): +02 bit 12 a 0, puertas 37/38
 * (+1A bits 10 y 11) a 0, +0A bits 0..2 a 0, P4.2 a 0. Idéntico a lo que hace senew en hal/usb_dc.c:encender_bloque().
 */
#include <stddef.h>
#include "bl.h"
#define SYSCON 0xF9090000u
#define R16(a) (*(volatile u16 *)(size_t)(a))
#define R32(a) (*(volatile u32 *)(size_t)(a))

u32 bl_fallo_pc __attribute__((section(".noinit")));   /* _bl_excepcion lo escribe antes de reiniciar */

/* ESPERA POR BUCLE NOP, no por timer: en el BL no sabemos si el timer 2 está corriendo bien al venir de senew por
 * reinicio_caliente (en v19.2 se colgaba aquí). A 13 MHz una vuelta subs/bne son 2 ciclos ≈ 154 ns, así que 1 ms = 6500 vueltas.
 * Usamos un factor de seguridad (hasta 20 veces más lento es tolerable, el bootloader no corre nada caliente). Puede durar más
 * de lo pedido si la CPU va a 208 MHz, no menos: suficientemente seguro para el pull-up USB. */
void bl_espera_ms(unsigned ms)
{
    volatile u32 i;
    u32 total = ms * 6500u;                                /* ~1 ms a 13 MHz, 2 ciclos por vuelta */
    for (i = 0; i < total; i++) { }
}

/* bl_syscon_usb: enciende los relojes del USB como hace senew. Devuelve 0 si bien, -1 si SYSCON no toma los bits. */
int bl_syscon_usb(void)
{
    R16(SYSCON + 0x02) = R16(SYSCON + 0x02) & (u16)~0x1000u;    /* FUN_44235a34(1,1) */
    R16(SYSCON + 0x1A) = R16(SYSCON + 0x1A) & (u16)~0x0400u;    /* puerta 37 */
    R16(SYSCON + 0x1A) = R16(SYSCON + 0x1A) & (u16)~0x0800u;    /* puerta 38 */
    R16(SYSCON + 0x0A) = R16(SYSCON + 0x0A) & (u16)~0x0007u;
    R16(0xF9000028)    = R16(0xF9000028) & (u16)~0x0004u;       /* P4.2 = 0 */
    bl_espera_ms(2);
    if ((R16(SYSCON + 0x1A) & 0x0C00u) || (R16(SYSCON + 0x02) & 0x1000u)) return -1;
    return 0;
}

/* bl_hw_ms: milisegundos aproximados. Sin timer de confianza, lo contamos por llamadas a esta función: el bucle principal la
 * llama suficientemente a menudo. No es preciso, pero el BL solo lo usa para `BlRes.ms` (indicador) y timeouts largos. */
static u32 bl_ms_acumulado;
static u32 bl_ms_contador;
void bl_hw_ms_arranque(void) { bl_ms_acumulado = 0; bl_ms_contador = 0; }
u32 bl_hw_ms(void)
{
    bl_ms_contador++;
    if (bl_ms_contador >= 1000) {              /* cada ~1000 vueltas del bucle principal = ~1 ms, aproximado */
        bl_ms_acumulado++;
        bl_ms_contador = 0;
    }
    return bl_ms_acumulado;
}

/* Testigos de fase en iRAM, para diagnosticar un cuelgue del BL leyendo desde senew después de un reset.
 * 0xF3002F10..0xF3002F17: 8 bytes. [0] = fase alcanzada, [1..3] = reservado, [4..7] = valor relacionado. */
void bl_fase(unsigned fase, u32 valor)
{
    *(volatile u8  *)(size_t)0xF3002F10 = (u8)fase;
    *(volatile u32 *)(size_t)0xF3002F14 = valor;
}
