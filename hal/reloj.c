/* Reloj a 208 MHz y MMU + cachés. docs/hal.md §1.2 y §2.2.
 * NADA de esto lo decide el emulador (no modela la velocidad ni las cachés; el PLL «engancha» solo). */
#include "hal.h"

extern const u32 pll_codigo[], pll_codigo_fin[];
void mmu_activar(u32 *tabla);

u32 reloj_mhz = 13;
void hal_retardo(u32 n)                                  /* el retardo corto del original (hal.h) */
{
    volatile u32 i;
    u32 v = n * (reloj_mhz >= 208 ? 0x11u : 2u);
    for (i = 0; i < v; i++) { }
}

/* Copia la rutina del PLL a la ITCM (0x100: detrás de los vectores) y la ejecuta allí, con las IRQ cortadas:
 * primero el EMC a 52 MHz de bus y después el PLL, como el original (§2.2, pasos 0..8 de la traza e14). */
int reloj_208(void)
{
    volatile u32 *d = (volatile u32 *)0x00000100;
    const u32 *o;
    u32 cerradas = irq_cerrar();
    int ok;
    for (o = pll_codigo; o < pll_codigo_fin; o++) *d++ = *o;
    ok = ((int (*)(void))0x00000100)();
    if (ok) reloj_mhz = 208;
    if (!cerradas) irq_abrir();
    return ok;
}

/* EXPERIMENTO (2026-10-24): bajar a 13 MHz y volver a 208, como hace el original a t = 0,17 s; deja +1C sin los bits 15 y 11 (0x0401, el valor del original
 * con el DSP en marcha). Devuelve (+1C antes << 16) | +1C después, o 0xFFFFFFFF si no volvió a enganchar. Solo con el DSP parado. */
extern const u32 pll_ciclo[], pll_ciclo_fin[];
u32 reloj_ciclo_13_208(void)
{
    volatile u32 *d = (volatile u32 *)0x00000100;
    const u32 *o;
    u32 cerradas = irq_cerrar(), antes = R16(0xF909001Cu), r;
    int ok;
    for (o = pll_ciclo; o < pll_ciclo_fin; o++) *d++ = *o;
    ok = ((int (*)(void))0x00000100)();
    if (!ok) reloj_mhz = 13;
    r = ok ? ((antes << 16) | R16(0xF909001Cu)) : 0xFFFFFFFFu;
    if (!cerradas) irq_abrir();
    return r;
}

/* Tabla de primer nivel, 4096 secciones de 1 MiB, 1:1, acceso total. Con caché y búfer de escritura: la SDRAM.
 * Con caché: la NOR de lectura (0x44000000..0x47FFFFFF) MENOS las secciones de las dos zonas escribibles
 * (mientras la NOR está en modo de órdenes devuelve estado, no datos). Todo lo demás (periféricos, ITCM/DTCM,
 * iRAM, el alias 0x04000000 de la NOR) sin caché ni búfer. El original usa una tabla de páginas en la SDRAM
 * (TTBR0 = 0x20E64000); esta es nuestra. */
static u32 tabla[4096] __attribute__((aligned(16384), section(".bss.ttb")));   /* (R3, P6) la primera del .bss: lejos de las pilas (hal/enlace.ld) */

void mmu_cache_init(void)
{
    u32 i;
    for (i = 0; i < 4096; i++) {
        u32 e = (i << 20) | 0xC12;                       /* AP = 11, bit 4, sección */
        if (i >= 0x200 && i < 0x210) e |= 0xC;           /* SDRAM: C y B */
        else if (i >= 0x440 && i < 0x480 && !(i >= (NOR_ESCR_INI >> 20) && i < (NOR_ESCR_FIN >> 20))) e |= 0x8;   /* NOR: C */
        tabla[i] = e;
    }
    mmu_activar(tabla);
}
