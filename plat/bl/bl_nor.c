/* bl_nor.c — Las rutinas de ESCRITURA de la NOR del bootloader, Intel M18. Vive en la ITCM (todo el código del BL en modo
 * flasheo vive ahí): no se lee código de la NOR mientras está en modo de órdenes.
 *
 * Difiere de hal/nor.c:
 *  - rango acotado a [MAIN_INI, MAIN_FIN) y nada más: no toca su propio bloque, ni W6FS, ni W6F, ni GDFS, ni FS fábrica;
 *  - sin suspensión (en el BL no hay IRQ, un borrado de 1 s no deja a nadie sin tic);
 *  - sin espejo (modo objeto puro: la región tiene que estar en blanco; para reescribir, borrar el bloque primero);
 *  - sin IRQ: todo es síncrono con sondeo del SR.
 *
 * Secuencia del M18 (hal.md §16, traza e14): las mismas de siempre, por 0x44000000.
 *   borrar    = 70 · 50 · 60 D0 (desbloquear) · 20 D0 · sondear SR hasta bit 7 · 60 01 (bloquear) · 50 si error · FF
 *   programar = 70 · 50 · 60 D0 · E9 · 0x1FF · 512 palabras · D0 · 70 · sondear SR · 60 01 · 50 si error · FF
 */
#include <stddef.h>
#include "bl.h"
#define R8(a)  (*(volatile u8  *)(size_t)(a))
#define R16(a) (*(volatile u16 *)(size_t)(a))

#define SR_LISTO     0x80u
#define SR_ERROR     0x3Au
#define TOPE_BORRAR   40000000u
#define TOPE_PROGRAMA  4000000u

static int bl_en_zona(u32 dir, u32 n)
{
    u32 fin = dir + n;
    return fin >= dir && dir >= 0x44180000u && fin <= 0x443C0000u;    /* MAIN_INI..MAIN_FIN */
}

#ifdef BL_INSTALLER
/* Zona AMPLIADA del modo installer: añade [BL_INI, MAIN_INI) encima de la de siempre, es decir [BL_INI, MAIN_FIN). Solo la usa
 * WRITE_PROPIO. Es seguro ESCRIBIR el propio bloque del BL porque TODO el código del modo flasheo corre desde la ITCM (y los
 * datos desde la DTCM): nada se lee de la NOR mientras el programa Intel está en marcha. El propio bl_enlace.ld lo garantiza
 * (NOR tiene solo .bl_vectores + .bl_emif + .bl_entrada, y _bl_reset en +0xB0 ya está en ITCM al llegar aquí). */
static int bl_en_zona_propia(u32 dir, u32 n)
{
    u32 fin = dir + n;
    return fin >= dir && dir >= 0x44140000u && fin <= 0x443C0000u;    /* BL_INI..MAIN_FIN */
}
#endif

/* Borra un bloque de 256 KiB. Devuelve 0 o BL_E_* y deja en *sr_out el último SR si falla.
 * (installer) `propio` = 1 permite BL_INI..MAIN_INI (el propio bloque del BL); 0 = solo el main como siempre. */
static int bl_nor_borrar_zona(u32 blq, u32 *sr_out, int propio)
{
    u32 sr = 0, tope = TOPE_BORRAR;
#ifdef BL_INSTALLER
    int ok = propio ? bl_en_zona_propia(blq, BL_BLOQUE) : bl_en_zona(blq, BL_BLOQUE);
#else
    int ok = bl_en_zona(blq, BL_BLOQUE); (void)propio;
#endif
    if (!ok || (blq & (BL_BLOQUE - 1))) return BL_E_RANGO;
    R16(blq) = 0x70;
    R16(blq) = 0x50;
    R16(blq) = 0x60; R16(blq) = 0xD0;              /* desbloquear */
    R16(blq) = 0x20; R16(blq) = 0xD0;              /* borrar */
    for (;;) {
        sr = R16(blq);
        if (sr & SR_LISTO) break;
        if (!--tope) {
            if (sr_out) *sr_out = sr;
            R16(blq) = 0xFF;
            return BL_E_TIEMPO;
        }
    }
    R16(blq) = 0x60; R16(blq) = 0x01;              /* bloquear */
    if (sr & SR_ERROR) { R16(blq) = 0x50; R16(blq) = 0xFF; if (sr_out) *sr_out = sr; return BL_E_NOR; }
    R16(blq) = 0xFF;
    return BL_OK;
}

int bl_nor_borrar(u32 blq, u32 *sr_out)        { return bl_nor_borrar_zona(blq, sr_out, 0); }
#ifdef BL_INSTALLER
int bl_nor_borrar_propio(u32 blq, u32 *sr_out) { return bl_nor_borrar_zona(blq, sr_out, 1); }
#endif

/* Comprueba que una región de 1 KiB está en blanco (todo 0xFFFF). */
int bl_nor_en_blanco(u32 dir)
{
    unsigned i;
    if (!bl_en_zona(dir, BL_REGION) || (dir & (BL_REGION - 1))) return BL_E_RANGO;
    for (i = 0; i < BL_REGION / 2; i++)
        if (R16(dir + 2 * i) != 0xFFFF) return BL_E_NO_BLANCO;
    return BL_OK;
}

/* Programa una región de 1 KiB (modo objeto, orden 0xE9: 512 palabras de una). La región TIENE que estar en blanco y la fuente
 * no puede estar en la flash (está en bl_buf, en DTCM). Si una región entera es 0xFFFF, no se programa (es inofensivo pero no
 * hace falta).
 * (installer) `propio` = 1 permite el rango [BL_INI, MAIN_INI) (bl_en_zona_propia); 0 = solo el main. */
static int bl_nor_programar_zona(u32 dir, const u16 *org, u32 *sr_out, int propio)
{
    u32 blq = dir & ~(BL_BLOQUE - 1), sr = 0, tope;
    unsigned i;
    int toda_ff = 1;
#ifdef BL_INSTALLER
    int ok = propio ? bl_en_zona_propia(dir, BL_REGION) : bl_en_zona(dir, BL_REGION);
#else
    int ok = bl_en_zona(dir, BL_REGION); (void)propio;
#endif
    if (!ok || (dir & (BL_REGION - 1))) return BL_E_RANGO;
    if ((u32)(size_t)org >= 0x44000000u && (u32)(size_t)org < 0x48000000u) return BL_E_RANGO;
    for (i = 0; i < BL_REGION / 2; i++) {
        if (R16(dir + 2 * i) != 0xFFFF) return BL_E_NO_BLANCO;
        if (org[i] != 0xFFFF) toda_ff = 0;
    }
    if (toda_ff) return BL_OK;                     /* nada que programar */

    R16(blq) = 0x70; R16(dir) = 0x50;
    R16(blq) = 0x60; R16(blq) = 0xD0;              /* desbloquear */
    R16(dir) = 0x70;
    tope = TOPE_PROGRAMA;
    for (;;) { sr = R16(dir); if (sr & SR_LISTO) break; if (!--tope) { if (sr_out) *sr_out = sr; R16(dir) = 0xFF; return BL_E_TIEMPO; } }

    R16(dir) = 0xFF;
    R16(dir) = 0xE9; R16(dir) = 0x1FF;
    for (i = 0; i < BL_REGION / 2; i++) R16(dir + 2 * i) = org[i];
    R16(dir) = 0xD0;
    R16(dir) = 0x70;
    tope = TOPE_PROGRAMA;
    for (;;) { sr = R16(dir); if (sr & SR_LISTO) break; if (!--tope) { if (sr_out) *sr_out = sr; R16(dir) = 0xFF; return BL_E_TIEMPO; } }

    R16(blq) = 0x60; R16(blq) = 0x01;              /* bloquear */
    if (sr & SR_ERROR) { R16(blq) = 0x50; R16(blq) = 0xFF; if (sr_out) *sr_out = sr; return BL_E_NOR; }
    R16(blq) = 0xFF;

    /* Verificar */
    for (i = 0; i < BL_REGION / 2; i++) {
        if (R16(dir + 2 * i) != org[i]) { if (sr_out) *sr_out = dir + 2 * i; return BL_E_VERIFICA; }
    }
    return BL_OK;
}

int bl_nor_programar(u32 dir, const u16 *org, u32 *sr_out)        { return bl_nor_programar_zona(dir, org, sr_out, 0); }
#ifdef BL_INSTALLER
int bl_nor_programar_propio(u32 dir, const u16 *org, u32 *sr_out) { return bl_nor_programar_zona(dir, org, sr_out, 1); }
#endif

/* CRC-32 (poly 0xEDB88320, inicial 0xFFFFFFFF, salida xor 0xFFFFFFFF): lee de la NOR directamente. */
u32 bl_crc32(u32 dir, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    u32 i, k;
    for (i = 0; i < n; i++) {
        c ^= R8(dir + i);
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
