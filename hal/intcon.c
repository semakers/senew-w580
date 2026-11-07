/* Controlador de interrupciones (0xFB020100/200/300, «ti_intcon»: 3 bancos, 92 líneas). docs/hal.md §7.
 * Mismo esquema que usaba el W600; cambian la puerta (15) y el bloque de reset (13, con pulso). */
#include "hal.h"

static irq_fn irq_tabla[92];

void intc_init(void)
{
    u32 b, i;
    R16(SYSCON + 0x1A) &= (u16)~0x0002u;                             /* puerta 15 */
    R16(SYSCON + 0x14) &= (u16)~0x0002u; R16(SYSCON + 0x14) |= 0x0002; /* bloque 13: pulso de reset, como FUN_4423a33c */
    for (b = 0; b < 3; b++) {
        u32 base = 0xFB020100 + 0x100 * b;
        R32(base + 0x04) = 0xFFFFFFFF;                               /* todo tapado */
        for (i = 0; i < 32; i++) R32(base + 0x1C + 4 * i) = (b == 0 && i >= 28) ? 2 : 0;
        R32(base + 0x18) = 1; R32(base + 0x18) = 2;                  /* como el original: 1 y luego 2 (traza e14 l. 192-197; hallazgo B3) */
    }
    R32(0xFB020104) = 0x0FFFFFFF;                                    /* la raíz deja pasar las cascadas */
}

void intc_habilita(u32 linea, irq_fn f)
{
    u32 b = linea < 28 ? 0 : linea < 60 ? 1 : 2;
    u32 bit = linea - (b == 0 ? 0 : b == 1 ? 28 : 60);
    u32 base = 0xFB020100 + 0x100 * b;
    irq_tabla[linea] = f;
    R32(base + 0x1C + 4 * bit) = 0;
    R32(base + 0x00) = ~(1u << bit);                                 /* reconoce lo que hubiera */
    R32(base + 0x04) &= ~(1u << bit);
}
/* (pista T) Lo mismo para una fuente por NIVEL y con prioridad, como las programa el original (ILR: bit 1 = nivel,
 * bits 6:2 = prioridad; la IRQ 76 de la Memory Stick es ILR = 0x7E = nivel, prioridad 31). La línea queda abierta. */
void intc_habilita_nivel(u32 linea, irq_fn f, u32 prioridad)
{
    u32 b = linea < 28 ? 0 : linea < 60 ? 1 : 2;
    u32 bit = linea - (b == 0 ? 0 : b == 1 ? 28 : 60);
    u32 base = 0xFB020100 + 0x100 * b;
    irq_tabla[linea] = f;
    R32(base + 0x1C + 4 * bit) = ((prioridad & 31) << 2) | 2;
    R32(base + 0x04) &= ~(1u << bit);
}

/* Robustez: una línea que interrumpe sin manejador, o un manejador que pide taparla (tormenta), se tapa en
 * su banco: sin esto la CPU no saldría nunca de la IRQ. irq_eventos cuenta todo lo que no es el tic (53):
 * lo usa el reposo profundo para saber si ha pasado algo desde el último barrido del teclado. */
volatile u32 irq_eventos, irq_huerfanas;
void intc_tapar(u32 linea)
{
    u32 b = linea < 28 ? 0 : linea < 60 ? 1 : 2;
    u32 bit = linea - (b == 0 ? 0 : b == 1 ? 28 : 60);
    R32(0xFB020104 + 0x100 * b) |= 1u << bit;
}
void intc_destapar(u32 linea)
{
    u32 b = linea < 28 ? 0 : linea < 60 ? 1 : 2;
    u32 bit = linea - (b == 0 ? 0 : b == 1 ? 28 : 60);
    R32(0xFB020104 + 0x100 * b) &= ~(1u << bit);
}
void irq_despacho(void)
{
    u32 v, l;
    v = R32(0xFB020110) & 0xFFFF; l = v;
    if (v == 28)      { l = 28 + (R32(0xFB020210) & 0xFFFF); R32(0xFB020218) = 1; }
    else if (v == 29) { l = 28 + (R32(0xFB020214) & 0xFFFF); R32(0xFB020218) = 2; }
    else if (v == 30) { l = 60 + (R32(0xFB020310) & 0xFFFF); R32(0xFB020318) = 1; }
    else if (v == 31) { l = 60 + (R32(0xFB020314) & 0xFFFF); R32(0xFB020318) = 2; }
    R32(0xFB020118) = 1;
    if (l != 53) irq_eventos++;
    if (l < 92 && irq_tabla[l]) irq_tabla[l]();
    else { irq_huerfanas++; if (l < 92 && l != 28 && l != 29 && l != 30 && l != 31) intc_tapar(l); }
}

