/* Arranque del main propio del W580i (DB2020, ARM926EJ-S). Pista S.
 * Escrito a partir de docs/hal.md §1 (lo que hace el firmware R8BE001 en 0x44140C64..):
 * entra en ARM, modo supervisor, en 0x44140000, sin RAM externa (la SDRAM la enciende el propio main).
 * [emulador] la máquina de la pista E tiene la SDRAM siempre y el EMC es un cerrojo: aquí solo se prueba
 * el ORDEN y que nada cuelga; los tiempos de la SDRAM solo los contesta el aparato. */
        .syntax unified
        .arm
        .section .vectores, "ax"
        .global _inicio
_inicio:
        ldr     pc, [pc, #0x18]         /* 0x00 reset */
        ldr     pc, [pc, #0x18]         /* 0x04 instrucción indefinida */
        ldr     pc, [pc, #0x18]         /* 0x08 SWI */
        ldr     pc, [pc, #0x18]         /* 0x0C aborto de prebúsqueda */
        ldr     pc, [pc, #0x18]         /* 0x10 aborto de datos */
        ldr     pc, [pc, #0x18]         /* 0x14 reservado */
        ldr     pc, [pc, #0x18]         /* 0x18 IRQ */
        ldr     pc, [pc, #0x18]         /* 0x1C FIQ */
        .word   reset, exc_und, exc_swi, exc_pabt, exc_dabt, exc_res, exc_irq, exc_fiq

/* +0x40: el bloque «EMIF» (hallazgo A7 de la pista R). El main original lleva en 0x44140040..0x441400AF un bloque de
 * 0x70 bytes: «EMIF», versión 1, tamaño 0x70, una suma (0x2A49B027) y los parámetros de memoria del aparato (bases de
 * CS1..CS3 0x48000000 / 0x4C000000 / 0x50000000, tiempos de la SDRAM 5A 3C 1E 1E…). El propio main NO lo lee, así
 * que lo lee algo de fuera: el cargador, la EROM o la herramienta de grabación [sin saber quién ni para qué].
 * Antes aquí teníamos código. Se copia TAL CUAL (son datos de configuración del hardware del propio teléfono; la
 * suma no se recalcula porque no se cambia ni un byte) y «reset» empieza detrás, en 0x441400B0. [emulador: nadie lo
 * lee]. «make w580-aparato» (herr/hacer_aparato.py) comprueba que está en su sitio y es igual al del original. */
        .global _emif
_emif:
        .byte   0x45, 0x4D, 0x49, 0x46, 0x01, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x27, 0xB0, 0x49, 0x2A
        .byte   0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x4C
        .byte   0x00, 0x00, 0x00, 0x50, 0x03, 0x01, 0x01, 0x01, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00
        .byte   0x29, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00
        .byte   0x62, 0x00, 0x55, 0x00, 0x55, 0x00, 0x55, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        .byte   0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x04, 0x00, 0x10, 0x08, 0x40
        .byte   0x5A, 0x3C, 0x1E, 0x1E, 0x88, 0x13, 0x88, 0x13, 0x88, 0x13, 0x88, 0x13, 0x01, 0x00, 0x00, 0x00
_emif_fin:

        .section .text.arranque, "ax"
reset:
#ifdef SENEW_RAM    /* (27-oct) el rastro de etapas en la iRAM (hal/traza_iram.c): lo primero, antes de tocar nada */
        ldr     r0, =0xF3003000
        ldr     r1, =0x4D525A54                 /* 'TZRM' */
        str     r1, [r0]
        mov     r1, #0x800
        str     r1, [r0, #4]
        mov     r1, #0
        str     r1, [r0, #8]
        mov     r1, #1
        str     r1, [r0, #12]
#endif
        /* 1. pilas provisionales en la iRAM 0xF3000000 (§1.1): la RAM externa no existe todavía */
        msr     cpsr_c, #0xD2           /* IRQ, I=F=1 */
        ldr     sp, =0xF3000200
        msr     cpsr_c, #0xD7           /* ABT */
        ldr     sp, =0xF3000208
        msr     cpsr_c, #0xDB           /* UND */
        ldr     sp, =0xF3000210
        msr     cpsr_c, #0xD3           /* SVC */
        ldr     sp, =0xF3000100

        /* 2. cp15 (§1.2): caché de instrucciones sí; MMU y caché de datos no; vectores bajos; TCM.
         *    (Si venimos de un reinicio en caliente con la MMU puesta, aquí se apaga: la tabla es 1:1.) */
        mrc     p15, 0, r0, c1, c0, 0
        ldr     r1, =0xFFFF5FFA
        and     r0, r0, r1
        orr     r0, r0, #0x1100
        mcr     p15, 0, r0, c1, c0, 0
        mov     r0, #0x1D               /* ITCM 64 KiB en 0 */
        mcr     p15, 0, r0, c9, c1, 1
        ldr     r0, =0x00010015         /* DTCM 16 KiB en 0x00010000 (en el DB2010 era 0x4200001D) */
        mcr     p15, 0, r0, c9, c1, 0

#ifndef SENEW_RAM   /* (27-oct) EN RAM: la imagen ESTÁ en la SDRAM; ni se apaga ni se reprograma (pasos 2b, 3 y 5-7) */
        /* 2b. «¿La SDRAM ya estaba encendida?» (arranque en caliente, o un cargador que la dejó puesta): lo PRIMERO
         *     que hace el crt0 del original (crt0_memoria, 0x44140838 -> emc_sdram_warm_reset, 0x44236BE0
         *     [observado en el desensamblado]): si EMC+00 bit 5, EMC+30 &= ~0x30000, espera 0x61 vueltas,
         *     EMC+00 &= ~0x20, |= 7, &= ~0x40, espera 0x27AC4 vueltas. Después la enciende desde cero (paso 6).
         *     Hallazgo A2 de la pista R: sin esto, el reinicio en caliente reprogramaba una SDRAM en marcha. */
        ldr     r0, =0x14000000
        ldr     r1, [r0]
        tst     r1, #0x20
        beq     20f
        ldr     r1, [r0, #0x30]
        bic     r1, r1, #0x30000
        str     r1, [r0, #0x30]
        mov     r1, #0x61
21:     subs    r1, r1, #1
        bne     21b
        ldr     r1, [r0]
        bic     r1, r1, #0x20
        str     r1, [r0]
        ldr     r1, [r0]
        orr     r1, r1, #7
        str     r1, [r0]
        ldr     r1, [r0]
        bic     r1, r1, #0x40
        str     r1, [r0]
        ldr     r1, =0x27AC4
22:     subs    r1, r1, #1
        bne     22b
20:
        /* 3. EMC+04 <- 0x8888 (§1.4 paso 3) */
        ldr     r0, =0x14000000
        ldr     r1, =0x8888
        str     r1, [r0, #0x04]

#endif
        /* 4. los vectores, a la ITCM (dirección 0): 8 saltos + 8 direcciones */
        ldr     r0, =_inicio
        mov     r1, #0
        mov     r2, #16
1:      ldr     r3, [r0], #4
        str     r3, [r1], #4
        subs    r2, r2, #1
        bne     1b

#ifndef SENEW_RAM
        /* 5-7. divisor del reloj de memoria, SDRAM y selecciones de chip: la rutina se COPIA a la ITCM
         *      (0x100, detrás de los vectores) y se ejecuta allí, porque retemporiza la memoria de la que
         *      estamos leyendo el código (§1.2: «todo lo que retemporiza la memoria, desde la ITCM») */
        ldr     r0, =mem_codigo
        ldr     r1, =mem_codigo_fin
        mov     r2, #0x100
2:      cmp     r0, r1
        ldrlo   r3, [r0], #4
        strlo   r3, [r2], #4
        blo     2b
        mov     r0, #0x100
        mov     lr, pc
        bx      r0

#endif
        /* 8. RTC+58 |= 1: retención de la alimentación (§3). Lo quita pmu_encendido() cuando el PMU ya sujeta */
        ldr     r0, =0xF9050058
        ldrh    r1, [r0]
        orr     r1, r1, #1
        strh    r1, [r0]

#if !defined(HITO_SIN_LLAVE) && !defined(SENEW_RAM)  /* 9. perro guardián: la llave, una vez (§3, FUN_441ad0cc). (R3) HITO_SIN_LLAVE: SOLO el main de medida del perro (hito 8a); el Makefile se niega con cualquier otro; sin cambiar el número de líneas: el objeto de los demás es idéntico byte a byte */
        ldr     r0, =0xF9010000
        mov     r1, #0xAA
        str     r1, [r0, #0x10]
        mov     r1, #0x5A
        str     r1, [r0, #0x24]
        mov     r1, #0xA5
        str     r1, [r0, #0x2C]
#endif
#ifdef SENEW_RAM    /* (27-oct) EN RAM la llave NO se pone aquí: el BL armó el perro (4 s) y se desarma al llegar al bucle (hw_perro_latido) */
        ldr     r0, =0xF3003000
        mov     r1, #4
        str     r1, [r0, #12]                   /* etapa 4: pasó el crt0 hasta las pilas */
#endif
        /* 10. pilas definitivas en la SDRAM; ABT y UND apuntan al contexto de fallo (ver exc_*) */
        msr     cpsr_c, #0xD2
        ldr     sp, =_pila_irq
        msr     cpsr_c, #0xD7
        ldr     sp, =exc_ctx
        msr     cpsr_c, #0xDB
        ldr     sp, =exc_ctx
        msr     cpsr_c, #0xD3
        ldr     sp, =_pila_svc

        /* 11. .data de flash a RAM, .bss a cero */
        ldr     r0, =_data_flash
        ldr     r1, =_data
        ldr     r2, =_edata
3:      cmp     r1, r2
        ldrlo   r3, [r0], #4
        strlo   r3, [r1], #4
        blo     3b
        ldr     r1, =_bss
        ldr     r2, =_ebss
        mov     r3, #0
4:      cmp     r1, r2
        strlo   r3, [r1], #4
        blo     4b

#ifdef SENEW_RAM
        ldr     r0, =0xF3003000
        mov     r1, #6
        str     r1, [r0, #12]                   /* etapa 6: .data y .bss listos, entra a main() */
#endif
        /* 12. a C (Thumb o ARM: BX decide) */
        ldr     r0, =main
        mov     lr, pc
        bx      r0
5:      b       5b
        .ltorg

/* ---- mem_codigo: se COPIA a la ITCM y se ejecuta allí. Independiente de la posición, sin pila.
 * Es §1.3 «encendido de la SDRAM» (la rutina de la ITCM 0xFBF8 del original, a 13 MHz) con los valores
 * que la traza e14 de la pista E vio escribir, más los pasos 5 y 7 de §1.4. */
/* Las cuatro esperas: el original da 0x146 vueltas de CUATRO instrucciones Thumb (adds/subs/cmp/bne, 0x442BBED0..);
 * aquí las vueltas son de dos (subs/bne), así que se dan 0x1F0 para que duren lo mismo o algo más (hallazgo M1 de la
 * pista R: con 0x145 duraban ~2/3, por debajo de la pausa de encendido que el original le da a la SDRAM). */
mem_codigo:
        ldr     r1, =0xF9090000         /* 5. SYSCON+52: divisor del reloj de memoria (FUN_441407e8(3)) */
        ldrh    r2, [r1, #0x52]
        orr     r2, r2, #0x30
        strh    r2, [r1, #0x52]
        ldr     r1, =0x14000000         /* 6. SDRAM */
        ldr     r2, [r1, #0x04]
        bic     r2, r2, #0xF0
        orr     r2, r2, #0x80
        str     r2, [r1, #0x04]
        ldr     r2, [r1, #0x04]
        bic     r2, r2, #0x0F
        orr     r2, r2, #0x08
        str     r2, [r1, #0x04]
        ldr     r2, [r1]
        and     r2, r2, #0x40
        str     r2, [r1]
        ldr     r3, =0x1F0
10:     subs    r3, r3, #1
        bne     10b
        ldr     r2, [r1]
        orr     r2, r2, #0x20
        str     r2, [r1]
        ldr     r2, =0x123D00C0         /* configuración a 13 MHz */
        str     r2, [r1, #0x30]
        ldr     r3, =0x1F0
11:     subs    r3, r3, #1
        bne     11b
        mov     r2, #1
        str     r2, [r1, #0x40]
        mov     r2, #0
        str     r2, [r1, #0x40]
        mov     r3, #8                  /* descriptor[6] = 8 órdenes «2» */
        mov     r2, #2
12:     str     r2, [r1, #0x40]
        subs    r3, r3, #1
        bne     12b
        mov     r2, #0
        str     r2, [r1, #0x38]
        str     r2, [r1, #0x40]
        mov     r2, #0x27
        str     r2, [r1, #0x34]
        mov     r2, #0
        str     r2, [r1, #0x40]
        ldr     r2, =0x20408108         /* tiempos a 13 MHz */
        str     r2, [r1, #0x3C]
        ldr     r3, =0x1F0
13:     subs    r3, r3, #1
        bne     13b
        ldr     r2, [r1]
        orr     r2, r2, #0x40
        str     r2, [r1]
        ldr     r2, [r1]
        orr     r2, r2, #0x1F
        str     r2, [r1]
        ldr     r3, =0x1F0
14:     subs    r3, r3, #1
        bne     14b
        ldr     r2, =0x00211B00         /* 7. selecciones de chip estáticas */
        str     r2, [r1, #0x08]
        ldr     r2, =0x00211200
        str     r2, [r1, #0x0C]
        str     r2, [r1, #0x10]
        str     r2, [r1, #0x14]
        mov     r2, #0
        str     r2, [r1, #0x18]
        str     r2, [r1, #0x1C]
        str     r2, [r1, #0x20]
        str     r2, [r1, #0x24]
        ldr     r2, =0x04018080         /* CS1 0x48000000, CS2 0x4C000000, CS3 0x50000000 */
        str     r2, [r1, #0x28]
        ldr     r2, =0x08111210         /* CS0 (la NOR) a 13 MHz */
        str     r2, [r1, #0x08]
        mov     r2, #0x100
        str     r2, [r1, #0x18]
        bx      lr
        .ltorg
mem_codigo_fin:

/* IRQ: guarda lo que el convenio de llamada no guarda y llama a irq_despacho() */
exc_irq:
        sub     lr, lr, #4
        stmfd   sp!, {r0-r3, r12, lr}
        ldr     r0, =irq_despacho
        mov     lr, pc
        bx      r0
        ldmfd   sp!, {r0-r3, r12, pc}^

/* El resto de excepciones: guardan el contexto del que falló en exc_ctx y llaman a excepcion_c(ctx) con una
 * pila propia (la de fallo). No vuelven. El sp de los modos ABT y UND apunta SIEMPRE a exc_ctx (paso 8): así
 * «stmia sp, {r0-r12}» guarda los registros sin gastar ninguno. Disposición de exc_ctx (palabras):
 * 0..12 = r0..r12, 13 = sp, 14 = lr (los del modo que falló), 15 = pc del fallo, 16 = cpsr del que falló,
 * 17 = tipo (1 indefinida, 2 SWI, 3 aborto de prebúsqueda, 4 aborto de datos, 5 reservada, 7 FIQ),
 * 18 = DFSR, 19 = FAR, 20 = IFSR. */
exc_und:  stmia sp, {r0-r12}
          mov   r0, #1
          b     6f
exc_pabt: sub   lr, lr, #4
          stmia sp, {r0-r12}
          mov   r0, #3
          b     6f
exc_dabt: sub   lr, lr, #8
          stmia sp, {r0-r12}
          mov   r0, #4
          b     6f
exc_swi:  ldr   sp, =exc_ctx            /* llega en modo SVC: su pila se pierde (no se usa SWI: es un salto perdido) */
          stmia sp, {r0-r12}
          mov   r0, #2
          b     6f
exc_res:  ldr   sp, =exc_ctx
          stmia sp, {r0-r12}
          mov   r0, #5
          b     6f
exc_fiq:  ldr   sp, =exc_ctx
          stmia sp, {r0-r12}
          mov   r0, #7
6:        mov   r4, sp
          str   lr, [r4, #60]
          mrs   r1, spsr
          str   r1, [r4, #64]
          str   r0, [r4, #68]
          mov   r6, #0
          mov   r7, #0
          and   r2, r1, #0x1F           /* modo del que falló: solo se visita si es IRQ, SVC o SYS */
          cmp   r2, #0x12
          cmpne r2, #0x13
          cmpne r2, #0x1F
          bne   7f
          mrs   r3, cpsr
          and   r5, r3, #0x1F
          cmp   r5, r2                  /* ¿el mismo modo en que estamos? (SWI): su sp ya no existe */
          beq   7f
          bic   r5, r3, #0x1F
          orr   r5, r5, r2
          orr   r5, r5, #0xC0
          msr   cpsr_c, r5
          mov   r6, sp
          mov   r7, lr
          msr   cpsr_c, r3
7:        str   r6, [r4, #52]
          str   r7, [r4, #56]
          mrc   p15, 0, r6, c5, c0, 0
          str   r6, [r4, #72]
          mrc   p15, 0, r6, c6, c0, 0
          str   r6, [r4, #76]
          mrc   p15, 0, r6, c5, c0, 1
          str   r6, [r4, #80]
          msr   cpsr_c, #0xD3
          ldr   sp, =_pila_fallo
          mov   r0, r4
          ldr   r2, =excepcion_c
          mov   lr, pc
          bx    r2
8:        b     8b

/* en_pila_de_fallo(f, arg): corta las IRQ, pasa a SVC con la pila de fallo y llama a f(arg). No vuelve.
 * La usa el pánico de software (montículo agotado, pila desbordada): la pila normal puede estar rota. */
        .global en_pila_de_fallo
        .type en_pila_de_fallo, %function
en_pila_de_fallo:
          msr   cpsr_c, #0xD3
          ldr   sp, =_pila_fallo
          mov   r2, r0
          mov   r0, r1
          mov   lr, pc
          bx    r2
9:        b     9b

        .ltorg

/* Abrir y cerrar las IRQ desde C */
        .global irq_abrir, irq_cerrar
        .type irq_abrir, %function
        .type irq_cerrar, %function
irq_abrir:
        mrs     r0, cpsr
        bic     r0, r0, #0x80
        msr     cpsr_c, r0
        bx      lr
irq_cerrar:
        mrs     r0, cpsr
        orr     r1, r0, #0x80
        msr     cpsr_c, r1
        and     r0, r0, #0x80           /* devuelve el estado anterior del bit I */
        bx      lr

/* reinicio_caliente(): para cuando el perro no muerde (sys_reset). NUESTRO: el original no tiene este camino (su
 * reinicio es siempre el perro, que deja el chip como recién encendido). Por eso, antes de volver a entrar por el
 * vector de reset, hay que dejar a mano lo que un reset de verdad dejaría hecho (hallazgo A2 de la pista R): el
 * código de «reset» programa el EMC con los tiempos de 13 MHz, y si el PLL siguiera puesto (208 MHz, bus a 52) la
 * NOR se leería con tiempos cuatro veces cortos. Así que:
 *   1. IRQ y FIQ cortadas, modo SVC, pila en la iRAM; fuera MMU y caché de datos (la tabla es 1:1);
 *   2. desde la ITCM (cambia el reloj con el que se lee la NOR): la rama de 13 MHz del original
 *      (0x44236608..20 [observado en el desensamblado]): SYSCON+1C &= 0x400, SYSCON+80 &= ~0x10, SYSCON+54 <- 0.
 *      El EMC sigue con los tiempos de 52 MHz, que a 13 son más largos de lo necesario [inferido: valen];
 *   3. salto al vector de reset, que (paso 2b) apaga la SDRAM encendida como hace el original y lo reprograma todo.
 *      (BL, 2026-10-23) El de BL_INI, NO el nuestro: es por donde entra el cargador de fábrica, y ahí está el bootloader, que
 *      mira la bandera de la iRAM (quedarse a grabar) o salta al main. Sin BL (MAIN_INI = BL_INI) es el mismo sitio de antes.
 * [emulador: no modela ni el reloj ni el EMC; solo prueba que el camino no cuelga]. */
        .global reinicio_caliente
        .type reinicio_caliente, %function
reinicio_caliente:
        msr     cpsr_c, #0xD3
        ldr     sp, =0xF3000100
        mrc     p15, 0, r0, c1, c0, 0
        ldr     r1, =0xFFFF5FFA
        and     r0, r0, r1
        orr     r0, r0, #0x1100
        mcr     p15, 0, r0, c1, c0, 0
        ldr     r0, =bajar_reloj_codigo
        ldr     r1, =bajar_reloj_fin
        mov     r2, #0x100              /* a la ITCM, donde «reset» pondrá luego mem_codigo */
30:     cmp     r0, r1
        ldrlo   r3, [r0], #4
        strlo   r3, [r2], #4
        blo     30b
        mov     r0, #0x100
        mov     lr, pc
        bx      r0
        ldr     pc, =_bl_ini            /* (BL) por donde entra el cargador de fábrica: el bootloader mira su bandera y salta al main */
        .ltorg
bajar_reloj_codigo:                     /* independiente de la posición, sin pila */
        ldr     r1, =0xF9090000
        ldrh    r2, [r1, #0x1C]
        and     r2, r2, #0x400
        strh    r2, [r1, #0x1C]         /* la CPU deja de ir por el PLL */
        ldrh    r2, [r1, #0x80]
        bic     r2, r2, #0x10
        strh    r2, [r1, #0x80]
        mov     r2, #0
        strh    r2, [r1, #0x54]         /* bus = CPU (13 MHz) */
        bx      lr
        .ltorg
bajar_reloj_fin:
