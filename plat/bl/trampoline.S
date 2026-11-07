/* trampoline.S — Trampoline position-independent para el executor.b del anyCID.
 *
 * El exploit Java carga executor.b en SDRAM y salta al inicio (ARM mode). Este trampoline:
 *   1. Apaga el perro de hardware.
 *   2. Configura ITCM (64 KiB en 0) y DTCM (16 KiB en 0x10000).
 *   3. Copia la sección .itcm del BL desde SDRAM a ITCM (0x00000000).
 *   4. Copia la sección .data del BL desde SDRAM a DTCM (0x00010000).
 *   5. Limpia BSS.
 *   6. Pone las pilas (exc y svc).
 *   7. Salta a bl_main(2) en ITCM, que entra en modo installer (sin main).
 *
 * El BL completo (bl.bin) va pegado justo después de este trampoline en executor.b.
 * Los offsets y tamaños se llenan por empaquetar_installer.py como constantes al final.
 *
 * Se compila: arm-none-eabi-gcc -c -mcpu=arm926ej-s -marm -o trampoline.o trampoline.S
 * y se extrae el .text crudo.
 */
        .syntax unified
        .arm

        .section .text
        .global _trampoline_inicio
_trampoline_inicio:
        /* SVC, IRQ y FIQ cerradas */
        msr     cpsr_c, #0xD3

        /* Pila provisional en iRAM (igual que bl_arranque.S) */
        ldr     sp, =0xF3000200

        /* RETENCIÓN DE ALIMENTACIÓN: RTC+58 |= 1 */
        ldr     r0, =0xF9050058
        ldrh    r1, [r0]
        orr     r1, r1, #1
        strh    r1, [r0]

        /* LLAVE DEL PERRO: desarmar */
        ldr     r0, =0xF9010000
        mov     r1, #0xAA
        str     r1, [r0, #0x10]
        mov     r1, #0x5A
        str     r1, [r0, #0x24]
        mov     r1, #0xA5
        str     r1, [r0, #0x2C]

        /* cp15: fuera MMU y cachés */
        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #0x0005                 /* fuera MMU (bit 0) y caché D (bit 2) */
        bic     r0, r0, #0x1000                 /* fuera caché I (bit 12) */
        mcr     p15, 0, r0, c1, c0, 0
        mov     r0, #0
        mcr     p15, 0, r0, c8, c7, 0           /* invalida TLB */
        mcr     p15, 0, r0, c7, c7, 0           /* invalida cachés */

        /* ITCM 64 KiB en 0 */
        mov     r0, #0x1D
        mcr     p15, 0, r0, c9, c1, 1
        /* DTCM 16 KiB en 0x10000 */
        ldr     r0, =0x00010015
        mcr     p15, 0, r0, c9, c1, 0

        /* Averiguar dónde estamos (PC-relative) para encontrar el BL adjunto */
        adr     r8, _trampoline_inicio          /* r8 = dirección real de este código en SDRAM */

        /* Cargar las constantes del pie (offsets relativos al inicio del trampoline) */
        ldr     r9, _const_bl_offset            /* offset del bl.bin dentro de executor.b */

        /* r10 = dirección real del bl.bin en SDRAM */
        add     r10, r8, r9

        /* --- Copiar .itcm: desde bl.bin+ITCM_OFF a ITCM 0x00000000 --- */
        ldr     r0, _const_itcm_off             /* offset de .itcm dentro de bl.bin */
        add     r0, r10, r0                     /* r0 = fuente en SDRAM */
        mov     r1, #0                          /* r1 = destino ITCM */
        ldr     r2, _const_itcm_size            /* r2 = bytes a copiar */
1:      cmp     r2, #0
        ldrgt   r3, [r0], #4
        strgt   r3, [r1], #4
        subgt   r2, r2, #4
        bgt     1b

        /* --- Copiar .data: desde bl.bin+DATA_OFF a DTCM 0x00010000 --- */
        ldr     r0, _const_data_off             /* offset de .data dentro de bl.bin */
        add     r0, r10, r0                     /* r0 = fuente en SDRAM */
        ldr     r1, =0x00010000                 /* r1 = destino DTCM */
        ldr     r2, _const_data_size            /* r2 = bytes a copiar */
2:      cmp     r2, #0
        ldrgt   r3, [r0], #4
        strgt   r3, [r1], #4
        subgt   r2, r2, #4
        bgt     2b

        /* --- Limpiar BSS --- */
        ldr     r0, _const_bss_ini
        ldr     r1, _const_bss_fin
        mov     r2, #0
3:      cmp     r0, r1
        strlo   r2, [r0], #4
        blo     3b

        /* --- Pilas --- */
        /* Modo ABT: pila de excepciones */
        msr     cpsr_c, #0xD7                   /* ABT, IRQ=FIQ=1 */
        ldr     sp, _const_pila_exc
        /* Modo UND */
        msr     cpsr_c, #0xDB                   /* UND, IRQ=FIQ=1 */
        ldr     sp, _const_pila_exc
        /* Volver a SVC */
        msr     cpsr_c, #0xD3
        ldr     sp, _const_pila_svc

        /* --- Saltar a bl_main(2) en ITCM --- */
        ldr     r4, _const_bl_main              /* r4 = dirección de bl_main */
        mov     r0, #2                          /* r0 = entrada = BL_ENTRADA_SIN_MAIN */
        /* Invalidar cachés una vez más antes de saltar */
        mov     r2, #0
        mcr     p15, 0, r2, c7, c5, 0           /* invalidar I-cache */
        mcr     p15, 0, r2, c7, c10, 4          /* drain write buffer */
        mov     pc, r4                          /* saltar a bl_main en ITCM (ARM) */

/* Constantes: las llena empaquetar_installer.py después de compilar el trampoline.
 * Están como literals al final de .text para que adr+ldr las alcance. */
        .align 2
_const_bl_offset:   .word 0xDEAD0001            /* offset del bl.bin dentro de executor.b */
_const_itcm_off:    .word 0xDEAD0002            /* offset de .itcm dentro de bl.bin (= tamaño de .entrada) */
_const_itcm_size:   .word 0xDEAD0003            /* tamaño de .itcm */
_const_data_off:    .word 0xDEAD0004            /* offset de .data dentro de bl.bin */
_const_data_size:   .word 0xDEAD0005            /* tamaño de .data */
_const_bss_ini:     .word 0xDEAD0006            /* dirección BSS inicio (DTCM) */
_const_bss_fin:     .word 0xDEAD0007            /* dirección BSS fin (DTCM) */
_const_pila_exc:    .word 0xDEAD0008            /* _pila_exc */
_const_pila_svc:    .word 0xDEAD0009            /* _pila_svc */
_const_bl_main:     .word 0xDEAD000A            /* dirección de bl_main en ITCM */
