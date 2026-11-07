/* Tramos calientes en ensamblador ARMv5TE. NUESTRO: copiado de w600/r3d/r3d_arm.S (pista M1 del W600)
 * para el menú 3D del W580 (pista V). De allí se traen SOLO el tramo de rampa con z y el relleno de 32 bits; el
 * resto (tramos con textura, cargador de vértices de otro formato) no hace falta aquí y no se ha copiado.
 * void f(u16 *p, u16 *zb, int n, const R3dTramo *t);  R3dTramo: z 0, dz 4, l 8, dl 12, uv 16, duv 20, tex 24, lut 28, rampa 32
 * p - zb es constante dentro del tramo: se direcciona el píxel desde zb. */
        .arch armv5te
        .arm
        .text
/* color con luz (rampa de 32), con z. visible 13, tapado 8 */
        .global r3d_asm_rampa_si_1
        .type r3d_asm_rampa_si_1, %function
r3d_asm_rampa_si_1:
        push    {r4-r8, lr}
        ldmia   r3, {r4-r7}                     /* z dz l dl */
        ldr     r8, [r3, #32]                   /* rampa */
        sub     r0, r0, r1
        sub     r0, r0, #2
        mov     lr, #0x3E
1:      ldrh    r3, [r1], #2
        mov     r12, r4, lsr #15
        cmp     r12, r3
        bhs     2f
        strh    r12, [r1, #-2]
        and     r12, lr, r6, lsr #18            /* (l >> 19) * 2 */
        ldrh    r3, [r8, r12]
        strh    r3, [r1, r0]
2:      add     r4, r4, r5
        add     r6, r6, r7
        subs    r2, r2, #1
        bne     1b
        pop     {r4-r8, pc}
        .size r3d_asm_rampa_si_1, . - r3d_asm_rampa_si_1

/* void r3d_asm_llena32(u32 *p, u32 v, u32 bloques): `bloques` bloques de 8 palabras. Borrar el z-buffer
 * (11 616 palabras) con el bucle en C eran ~23 000 instrucciones por cuadro; así, ~4 400. */
        .global r3d_asm_llena32
        .type r3d_asm_llena32, %function
r3d_asm_llena32:
        push    {r4-r8}
        mov     r3, r1
        mov     r4, r1
        mov     r5, r1
        mov     r6, r1
        mov     r7, r1
        mov     r8, r1
        mov     r12, r1
1:      stmia   r0!, {r1, r3, r4, r5, r6, r7, r8, r12}
        subs    r2, r2, #1
        bne     1b
        pop     {r4-r8}
        bx      lr
        .size r3d_asm_llena32, . - r3d_asm_llena32
