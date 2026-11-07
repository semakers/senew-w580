/* bl_arranque.S — ENTRADA DEL BOOTLOADER PROPIO (plat/bl/bl.h, v19). Vive en el primer bloque de la NOR (BL_INI, 0x44140000),
 * donde el cargador de fábrica entra siempre. Hace lo mínimo imprescindible desde la NOR (los primeros 256 B) y copia el resto
 * del BL a las TCM para no leer ni ejecutar desde la NOR mientras se la borra/escribe.
 *
 * Reglas:
 *  - sin SDRAM: todo en iRAM/DTCM, con pila provisional en 0xF3000200 (el mismo sitio que hal/arranque.S del main);
 *  - sin IRQ nunca: las excepciones (abort, undef) van a un bucle de registro en DTCM;
 *  - lo que corre desde la NOR es SOLO esto, 256 B pelados; en cuanto se copia la ITCM/DTCM, el PC salta a bl_main en ITCM.
 */
        .syntax unified
        .arm

/* +0x00..+0x3F: 8 saltos de vector + 8 direcciones. En la NOR, el BL va al cargador de fábrica, no al main. */
        .section .bl_vectores, "ax"
        .global _bl_inicio
_bl_inicio:
        ldr     pc, [pc, #0x18]         /* 0x00 reset -> _bl_reset */
        ldr     pc, [pc, #0x18]         /* 0x04 indef */
        ldr     pc, [pc, #0x18]         /* 0x08 swi */
        ldr     pc, [pc, #0x18]         /* 0x0C pabt */
        ldr     pc, [pc, #0x18]         /* 0x10 dabt */
        ldr     pc, [pc, #0x18]         /* 0x14 reservado */
        ldr     pc, [pc, #0x18]         /* 0x18 irq */
        ldr     pc, [pc, #0x18]         /* 0x1C fiq */
        .word   _bl_reset, _bl_cuelgue, _bl_cuelgue, _bl_cuelgue, _bl_cuelgue, _bl_cuelgue, _bl_cuelgue, _bl_cuelgue

/* +0x40..+0xAF: el bloque EMIF (hallazgo A7 de la pista R), IDÉNTICO al del main. Lo lee alguien de fuera (el cargador, la EROM
 * o la herramienta de grabación: [sin saber] quién). Se copia TAL CUAL del main (hal/arranque.S), byte a byte. */
        .section .bl_emif, "ax"
        .global _bl_emif
_bl_emif:
        .byte   0x45, 0x4D, 0x49, 0x46, 0x01, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x27, 0xB0, 0x49, 0x2A
        .byte   0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x4C
        .byte   0x00, 0x00, 0x00, 0x50, 0x03, 0x01, 0x01, 0x01, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00
        .byte   0x29, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00
        .byte   0x62, 0x00, 0x55, 0x00, 0x55, 0x00, 0x55, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        .byte   0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x04, 0x00, 0x10, 0x08, 0x40
        .byte   0x5A, 0x3C, 0x1E, 0x1E, 0x88, 0x13, 0x88, 0x13, 0x88, 0x13, 0x88, 0x13, 0x01, 0x00, 0x00, 0x00

/* +0xB0: la DECISIÓN. Corre desde la NOR, poca cosa:
 *   1. CPSR a SVC con IRQ y FIQ cerradas; pila provisional en la iRAM.
 *   2. cp15: fuera MMU y cachés (la tabla es 1:1 si venimos del main); ITCM en 0, DTCM en 0x10000.
 *   3. ¿La bandera 0xF3002F00 vale BL_MAGIA y 0xF3002F04 vale ~BL_MAGIA? → modo flasheo (entrada = 1).
 *      ¿La primera palabra del main es ldr pc,[pc,#0x18] y el reset cae en MAIN_INI..MAIN_FIN? → saltar al main (y borrar bandera).
 *      Si no, modo flasheo también (entrada = 2: el main está roto; es seguro quedarse en el BL para rescatar).
 *   4. Modo flasheo:
 *      a. Copia .itcm (código) desde la NOR al 0x00000000, y .data desde la NOR al 0x00010000.
 *      b. Pila SVC definitiva en la DTCM, al final.
 *      c. Salta a bl_main(entrada).
 */
        .section .bl_entrada, "ax"
        .global _bl_reset, _bl_cuelgue
_bl_reset:
#ifdef BL_RAM
        /* (27-oct) EN RAM, saltando desde el ORIGINAL con su MMU puesta: la imagen está en una sección 1:1 (0x20100000), así que se
         * apaga la MMU y la caché de datos ANTES de tocar nada (la iRAM y parte de la E/S no son 1:1 en la tabla del original). El
         * fragmento que salta ya limpió la caché de datos. */
        msr     cpsr_c, #0xD3
        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #0x0005
        mcr     p15, 0, r0, c1, c0, 0
        mov     r0, #0
        mcr     p15, 0, r0, c8, c7, 0
#endif
        msr     cpsr_c, #0xD3                   /* SVC, I=F=1 */
        ldr     sp, =0xF3000200                 /* pila provisional en iRAM */

        /* RETENCIÓN DE ALIMENTACIÓN: RTC+58 |= 1 (el crt0 del original lo pone en §3; sin esto el aparato se puede apagar a los
         * ~2 s si el PMU no está sujeto). hal/arranque.S lo pone antes de la llave del perro; aquí igual. */
        ldr     r0, =0xF9050058
        ldrh    r1, [r0]
        orr     r1, r1, #1
        strh    r1, [r0]

        /* LLAVE DEL PERRO DE HARDWARE: F9010010 <- AA, F9010024 <- 5A, F901002C <- A5 (FUN_441ad0cc). El modelo del emulador
         * resetea a los 2 s sin esto; el aparato puede o no estar armado al salir del bootrom, pero igual que senew: desarmamos. */
        ldr     r0, =0xF9010000
        mov     r1, #0xAA
        str     r1, [r0, #0x10]
        mov     r1, #0x5A
        str     r1, [r0, #0x24]
        mov     r1, #0xA5
        str     r1, [r0, #0x2C]
        /* cp15: fuera MMU y caché de datos, dejamos la de instrucciones ENCENDIDA (es segura; el bootrom entra con todo apagado,
         * pero si venimos de hw_reiniciar las cachés estaban puestas: hay que apagar la D antes de leer flags) */
        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #0x0005                 /* fuera MMU (M, bit 0) y caché de datos (C, bit 2) */
        mcr     p15, 0, r0, c1, c0, 0
        mov     r0, #0
        mcr     p15, 0, r0, c8, c7, 0           /* invalida TLB */
        mcr     p15, 0, r0, c7, c7, 0           /* invalida cachés */
        mov     r0, #0x1D                       /* ITCM 64 KiB en 0 */
        mcr     p15, 0, r0, c9, c1, 1
        ldr     r0, =0x00010015                 /* DTCM 16 KiB en 0x00010000 */
        mcr     p15, 0, r0, c9, c1, 0

#ifdef BL_FORZAR_FLASHEO
        /* Prueba de desarrollo: salta directo a modo flasheo (sin mirar bandera ni main). */
        ldr     r0, =0xF3002F00                 /* igual lo tocamos para dejarlo coherente */
        b       2f
#else
        /* ¿Bandera? r0 = 0xF3002F00. r1 = BL_MAGIA (0x424C4F41). */
        ldr     r0, =0xF3002F00
        ldr     r2, [r0]
        ldr     r3, [r0, #4]
        ldr     r1, =0x424C4F41
        cmp     r2, r1
        bne     1f
        mvn     r4, r1
        cmp     r3, r4
        bne     1f
        /* bandera puesta: borrarla (si el BL falla, un reset vuelve al main si lo hay) y entrada = BL_ENTRADA_BANDERA */
        mov     r2, #0
        str     r2, [r0]
        str     r2, [r0, #4]
        mov     r0, #1                          /* r0 = entrada = BL_ENTRADA_BANDERA */
        b       2f
1:      /* sin bandera: ¿tiene vectores el main? r2 = *MAIN_INI, r3 = *(MAIN_INI+0x20) (vector de reset) */
        ldr     r0, =MAIN_INI
        ldr     r2, [r0]
        ldr     r3, [r0, #0x20]
        ldr     r1, =0xE59FF018                 /* ldr pc,[pc,#0x18] */
        cmp     r2, r1
        bne     3f
        ldr     r1, =MAIN_INI
        cmp     r3, r1
        blo     3f
        ldr     r1, =MAIN_FIN
        cmp     r3, r1
        bhs     3f
        /* main válido: salto a MAIN_INI (sin volver). Dejamos pila y cp15 como están: el main hace su cp15 y pila otra vez. */
        bx      r0
3:      /* main roto: entrada = 2 (BL_ENTRADA_SIN_MAIN) */
        mov     r0, #2
#endif
2:      /* MODO FLASHEO: entrada = r0. Guardamos entrada en r11 (callee-saved) para después. */
#ifdef BL_FORZAR_FLASHEO
        mov     r0, #3                          /* entrada = 3 = «forzado» (desarrollo) */
#endif
        mov     r11, r0

        /* Copiar .itcm (código): desde _itcm_lma (NOR) a 0x00000000, hasta _itcm_fin - _itcm_ini bytes. */
        ldr     r0, =_itcm_lma
        mov     r1, #0
        ldr     r2, =_itcm_fin
4:      cmp     r1, r2
        ldrlo   r3, [r0], #4
        strlo   r3, [r1], #4
        blo     4b

        /* Copiar .data: desde _data_lma (NOR) a _data (DTCM) hasta _edata. */
        ldr     r0, =_data_lma
        ldr     r1, =_data
        ldr     r2, =_edata
5:      cmp     r1, r2
        ldrlo   r3, [r0], #4
        strlo   r3, [r1], #4
        blo     5b

        /* .bss a cero */
        ldr     r1, =_bss
        ldr     r2, =_ebss
        mov     r3, #0
6:      cmp     r1, r2
        strlo   r3, [r1], #4
        blo     6b

        /* Pila definitiva en DTCM. */
        ldr     sp, =_pila_svc

        /* Instala vectores propios en 0 (los que están ahí al copiar código son del propio BL en ITCM: .bl_vectores_itcm al
         * principio de .itcm los puso). */

        /* Saltar a bl_main(entrada) en ITCM (ARM). r0 = entrada. */
        mov     r0, r11
        ldr     r1, =bl_main
        bx      r1

/* El bucle de cuelgue por si el cargador salta a un vector no reset del BL en la NOR (nunca debería): bucle infinito. En modo
 * flasheo las excepciones las atrapa _bl_excepcion (en ITCM). */
_bl_cuelgue:
        b       _bl_cuelgue
        .ltorg

/* ---- Lo que corre desde la ITCM (modo flasheo): va a .bl_vectores_itcm (al principio de .itcm, por el enlace). ------------- */
        .section .bl_vectores_itcm, "ax"
        .global _bl_vectores_itcm
_bl_vectores_itcm:
        /* en 0: cuando el BL está en modo flasheo, el PC inicial de las excepciones es aquí (ITCM mapeada en 0). */
        ldr     pc, [pc, #0x18]
        ldr     pc, [pc, #0x18]
        ldr     pc, [pc, #0x18]
        ldr     pc, [pc, #0x18]
        ldr     pc, [pc, #0x18]
        ldr     pc, [pc, #0x18]
        ldr     pc, [pc, #0x18]
        ldr     pc, [pc, #0x18]
        .word   _bl_excepcion, _bl_excepcion, _bl_excepcion, _bl_excepcion, _bl_excepcion, _bl_excepcion, _bl_excepcion, _bl_excepcion

/* ---- Las rutinas de abajo son del BL en modo flasheo, en ITCM. --------------------------------------------------------- */
        .section .text, "ax"

/* _bl_excepcion: una excepción en modo flasheo. Guarda PC y causa en una variable NOINIT que sobrevive al reset, pone la bandera
 * para volver al BL, y hace reset por el perro de hardware (no hay perro armado, así que esto hace sys_reset simplificado:
 * ldr pc, =_bl_reset). */
        .global _bl_excepcion
        .type _bl_excepcion, %function
_bl_excepcion:
        /* r0 = lr (pc de la instrucción fallida, aproximado) */
        mov     r0, lr
        ldr     r1, =bl_fallo_pc
        str     r0, [r1]
        /* Rearmar la bandera y volver al BL. */
        ldr     r0, =0xF3002F00
        ldr     r1, =0x424C4F41
        str     r1, [r0]
        mvn     r1, r1
        str     r1, [r0, #4]
        /* Salto directo al reset (código en la NOR, ya debería ser accesible). */
        ldr     pc, =_bl_reset

/* _bl_irq_cerrar / _bl_irq_abrir: en el BL, las IRQ siempre están cerradas (I=F=1 en SVC). Devuelve el bit I actual. */
        .global _bl_irq_cerrar, _bl_irq_abrir
_bl_irq_cerrar:
        mrs     r0, cpsr
        and     r0, r0, #0x80
        orr     r1, r0, #0xC0
        msr     cpsr_c, r1
        mov     r0, #0                          /* en el BL nunca estaban abiertas: 0 */
        bx      lr
_bl_irq_abrir:
        /* nada: en el BL nunca se abren */
        bx      lr
        .ltorg
