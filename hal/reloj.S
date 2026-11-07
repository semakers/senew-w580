/* Reloj a 208 MHz, MMU y cachés del DB2020. Escrito a partir de docs/hal.md §2.2 (FUN_44236530, traza e14).
 * NADA de esto lo decide el emulador (no modela la velocidad ni las cachés). */
        .syntax unified
        .arm

/* ---- pll_codigo: se COPIA a la ITCM y se ejecuta allí (cambia los tiempos de la NOR y de la SDRAM, así que
 * no puede correr desde ellas). Independiente de la posición. Devuelve 1 = enganchado (CPU a 208 MHz, bus a 52);
 * 0 = el PLL no enganchó: se deshace todo y se queda en 13 MHz (la rama de 13 del original). */
        .section .text.pll, "ax"
        .global pll_codigo, pll_codigo_fin
pll_codigo:
        ldr     r1, =0x14000000         /* 0. EMC para 52 MHz de bus, ANTES de subir el reloj */
        ldr     r2, =0x08415C14
        str     r2, [r1, #0x08]         /*    CS0: la NOR */
        mov     r2, #0x100
        str     r2, [r1, #0x18]
        ldr     r2, =0x12310303
        str     r2, [r1, #0x30]         /*    SDRAM: configuración */
        mov     r2, #0x37
        str     r2, [r1, #0x34]
        ldr     r2, =0x83060C1A
        str     r2, [r1, #0x3C]         /*    SDRAM: tiempos */
        ldr     r1, =0xF9090000         /* SYSCON */
        mov     r2, #3                  /* 1. +54 <- 3 (bus = CPU / 4) */
        strh    r2, [r1, #0x54]
        ldrh    r2, [r1, #0x1C]         /* 2. +1C &= FFEF (sin dividir por 2) */
        bic     r2, r2, #0x10
        strh    r2, [r1, #0x1C]
        ldrh    r2, [r1, #0x1C]         /* 3. +1C &= FFF7 */
        bic     r2, r2, #0x08
        strh    r2, [r1, #0x1C]
        ldrh    r2, [r1, #0x80]         /* 4. PLL: quita el bit 4... */
        bic     r2, r2, #0x10
        strh    r2, [r1, #0x80]
        ldr     r2, =0x2810             /* 5. ...y lo arranca */
        strh    r2, [r1, #0x80]
        ldr     r12, =2000000           /* 6. espera al enganche (el original no pone tope; nosotros sí) */
1:      ldrh    r2, [r1, #0x80]
        tst     r2, #1
        bne     2f
        subs    r12, r12, #1
        bne     1b
        ldrh    r2, [r1, #0x1C]         /* no enganchó: la rama de 13 MHz del original */
        and     r2, r2, #0x400
        strh    r2, [r1, #0x1C]
        ldrh    r2, [r1, #0x80]
        bic     r2, r2, #0x10
        strh    r2, [r1, #0x80]
        mov     r2, #0
        strh    r2, [r1, #0x54]
        ldr     r1, =0x14000000         /* y la memoria otra vez a 13 */
        ldr     r2, =0x08111210
        str     r2, [r1, #0x08]
        ldr     r2, =0x123D00C0
        str     r2, [r1, #0x30]
        mov     r2, #0x27
        str     r2, [r1, #0x34]
        ldr     r2, =0x20408108
        str     r2, [r1, #0x3C]
        mov     r0, #0
        bx      lr
2:      ldrh    r2, [r1, #0x1C]         /* 7. +1C |= 1: usar el PLL */
        orr     r2, r2, #1
        strh    r2, [r1, #0x1C]
        mov     r2, #0                  /* 8. +38 <- 0 */
        strh    r2, [r1, #0x38]
        mov     r0, #1
        bx      lr
        .ltorg
pll_codigo_fin:

/* ---- pll_ciclo (EXPERIMENTO 2026-10-24): como el original a t = 0,17 s, BAJA a 13 MHz por su rama de 13 (+1C &= 0x400: quita el PLL y los bits 15 y 11,
 * que a 208 no se dejan borrar; PLL parado; bus sin dividir; memoria con los tiempos de 13) y vuelve a SUBIR a 208 por el mismo camino que pll_codigo.
 * Se copia a la ITCM y se ejecuta allí con las IRQ cortadas. Devuelve 1 = enganchado otra vez a 208; 0 = se queda en 13. */
        .section .text.pll, "ax"
        .global pll_ciclo, pll_ciclo_fin
pll_ciclo:
        ldr     r1, =0xF9090000
        ldrh    r2, [r1, #0x1C]         /* rama de 13 del original (0x44236610): +1C &= 0x400 */
        and     r2, r2, #0x400
        strh    r2, [r1, #0x1C]
        ldrh    r2, [r1, #0x80]         /* +80 &= FFEF: PLL parado */
        bic     r2, r2, #0x10
        strh    r2, [r1, #0x80]
        mov     r2, #0                  /* +54 <- 0 */
        strh    r2, [r1, #0x54]
        ldr     r1, =0x14000000         /* memoria a 13 (emc_set_bus_clock(0xD)) */
        ldr     r2, =0x08111210
        str     r2, [r1, #0x08]
        ldr     r2, =0x123D00C0
        str     r2, [r1, #0x30]
        mov     r2, #0x27
        str     r2, [r1, #0x34]
        ldr     r2, =0x20408108
        str     r2, [r1, #0x3C]
        ldr     r12, =2000              /* un respiro a 13 MHz */
3:      subs    r12, r12, #1
        bne     3b
        ldr     r1, =0x14000000         /* y otra vez arriba, exactamente como pll_codigo */
        ldr     r2, =0x08415C14
        str     r2, [r1, #0x08]
        mov     r2, #0x100
        str     r2, [r1, #0x18]
        ldr     r2, =0x12310303
        str     r2, [r1, #0x30]
        mov     r2, #0x37
        str     r2, [r1, #0x34]
        ldr     r2, =0x83060C1A
        str     r2, [r1, #0x3C]
        ldr     r1, =0xF9090000
        mov     r2, #3
        strh    r2, [r1, #0x54]
        ldrh    r2, [r1, #0x1C]
        bic     r2, r2, #0x10
        strh    r2, [r1, #0x1C]
        ldrh    r2, [r1, #0x1C]
        bic     r2, r2, #0x08
        strh    r2, [r1, #0x1C]
        ldrh    r2, [r1, #0x80]
        bic     r2, r2, #0x10
        strh    r2, [r1, #0x80]
        ldr     r2, =0x2810
        strh    r2, [r1, #0x80]
        ldr     r12, =2000000
1:      ldrh    r2, [r1, #0x80]
        tst     r2, #1
        bne     2f
        subs    r12, r12, #1
        bne     1b
        mov     r0, #0                  /* no enganchó: se queda a 13 (memoria ya con tiempos de 52: más lenta, pero válida) */
        bx      lr
2:      ldrh    r2, [r1, #0x1C]
        orr     r2, r2, #1
        strh    r2, [r1, #0x1C]
        mov     r2, #0
        strh    r2, [r1, #0x38]
        mov     r0, #1
        bx      lr
        .ltorg
pll_ciclo_fin:

/* ---- mmu_activar(tabla): TTBR0, dominio 0 = cliente, invalida TLB y cachés, enciende MMU +
 * caché de datos + caché de instrucciones. La tabla es 1:1, así que el PC sigue valiendo. */
        .section .text.mmu, "ax"
        .global mmu_activar, cache_limpiar
        .type mmu_activar, %function
        .type cache_limpiar, %function
mmu_activar:
        mcr     p15, 0, r0, c2, c0, 0   /* TTBR0 */
        mov     r1, #1
        mcr     p15, 0, r1, c3, c0, 0   /* DACR: dominio 0, cliente */
        mov     r1, #0
        mcr     p15, 0, r1, c8, c7, 0   /* invalida la TLB */
        mcr     p15, 0, r1, c7, c7, 0   /* invalida las dos cachés */
        mrc     p15, 0, r1, c1, c0, 0
        orr     r1, r1, #0x1000         /* I */
        orr     r1, r1, #0x0005         /* C y M */
        mcr     p15, 0, r1, c1, c0, 0
        nop
        nop
        bx      lr

/* cache_limpiar(inicio, fin): escribe a la RAM las líneas sucias de ese tramo (c7,c10,1 línea a línea, como
 * FUN_44a2c0f4 del original) y vacía el búfer de escritura. Hace falta antes de que el PDICON lea el cuadro de la
 * SDRAM: no ve la caché. Con la caché de datos APAGADA (SCTLR bit 2 a 0: los mains de los hitos, que no encienden
 * la MMU) vuelve sin tocar nada. La llama lcd_enviar() sobre lo que manda (pista S4): ahí vive el invariante. */
cache_limpiar:
        mrc     p15, 0, r2, c1, c0, 0   /* SCTLR */
        tst     r2, #4                  /* C: ¿hay caché de datos? */
        bxeq    lr
        bic     r0, r0, #31
1:      cmp     r0, r1
        mcrlo   p15, 0, r0, c7, c10, 1  /* limpia la línea de caché de datos de esa dirección */
        addlo   r0, r0, #32
        blo     1b
        mov     r0, #0
        mcr     p15, 0, r0, c7, c10, 4  /* vacía el búfer de escritura (el original no lo hace; es inofensivo) */
        bx      lr
