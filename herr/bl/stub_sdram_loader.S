/* stub_sdram_loader.S — Stub mínimo para el emulador: copia executor.b de NOR a SDRAM y salta allá.
 *
 * Va en BL_INI (0x44140000) de la NOR del emulador. El executor.b está en 0x44200000 (dentro de la NOR).
 * Este stub simula lo que hace el exploit Java del anyCID: cargar executor.b en SDRAM y ejecutarlo.
 *
 * Solo para pruebas en el emulador. No es código de producción.
 */
        .syntax unified
        .arm

        .section .text
        .global _start
_start:
        ldr     r0, _src        /* fuente: executor.b en NOR */
        ldr     r1, _dst        /* destino: SDRAM */
        ldr     r2, _tam        /* 256 KiB */
        mov     r4, r1          /* guardar dirección de salto */
1:
        ldr     r3, [r0], #4
        str     r3, [r1], #4
        subs    r2, r2, #4
        bne     1b
        mov     pc, r4          /* saltar al trampoline en SDRAM */

        .align 2
_src:   .word   0x44200000
_dst:   .word   0x20100000
_tam:   .word   0x00040000
