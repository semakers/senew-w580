/* nor_placa.h (W810i) — lo que chips/nor_intel/clasico.c necesita de la placa. Pista port810 (docs/port_w810.md §3).
 * MISMO guarda NOR_PLACA_H que placas/w600i/nor_placa.h: placas/w810i/memoria.c incluye ÉSTE y luego el memoria.c del W600.
 *
 * La NOR del W810 [observado en el emulador con el main ORIGINAL R4DB005]: 64 MiB, 0x44000000..0x47FFFFFF, «Flash extends over
 * two chip select» (CS0 0x44000000, CS1 0x46000000: EMC +28 = 0x04018060 en el original), familia Intel «Tyax» (L18/L30), su FS
 * en 0x45900000..0x47DFFFFF y el GDFS en 0x47E00000 [inferido de los literales]. El chip del APARATO: [sin saber].
 *
 * DISPOSICIÓN APROBADA por el mantenedor el 2026-10-15 (docs/port_w810.md §4): TODO lo que escribimos, en CS0 y por debajo del FS original:
 *   0x44000000..0x44140000  EROM y zona de arranque de fábrica        NUNCA se toca
 *   0x44140000..0x44800000  main + W6FS del paquete (6,75 MiB)        lo escribe solo quien instale
 *   0x44800000..0x45700000  la Memoria (W6F), 15 MiB, 120 bloques     APROBADA por el mantenedor (2026-10-15), con escritura
 *   0x45700000..0x45780000  reserva (4 bloques, sin uso)
 *   0x45780000..0x45800000  almacén de ajustes (4 bloques)
 *   0x45800000..0x45900000  Mis archivos (W6U, 8 bloques)
 *   0x45900000..0x47E00000  FS ORIGINAL (37 MiB, datos del usuario)    NUNCA se toca (solo lectura; hoy sin lector)
 *   0x47E00000..0x48000000  GDFS (calibración, IMEI…)                   NUNCA se toca */
#ifndef NOR_PLACA_H
#define NOR_PLACA_H
#include "hal.h"
#define NOR_BASE     0x44000000u                 /* selección de chip 0 (el segundo chip, CS1, empieza en 0x46000000) */
#ifndef NOR_ESCRITURA
#define NOR_ESCRITURA 1
#endif
#define NOR_ZONA_INI 0x45780000u                 /* almacén: 4 bloques de 128 KiB, en la sección de 1 MiB 0x457 (sin caché) */
#define NOR_ZONA_FIN 0x45800000u
#define NOR_USR_INI  0x45800000u                 /* W6U: 8 bloques, la sección 0x458 entera (sin caché) */
#define NOR_USR_FIN  0x45900000u
#define NOR_ESCR_INI NOR_ZONA_INI
#define NOR_ESCR_FIN NOR_USR_FIN
#ifndef NOR_W6F_INI
#define NOR_W6F_INI  0x44800000u                 /* APROBADA (2026-10-15): 15 MiB; ver arriba */
#endif
#define NOR_W6F_FIN  0x45700000u
#define NOR_W6F_PAG  1024u
/* La guarda: SOLO el chip que presenta el emulador del W810 (build-w810: 0089 880D con CFI de 64 MiB, 0x27 = 0x1A). El del aparato
 * es [sin saber] (y si son dos de 32 MiB, la CFI de 0x44000000 dirá 0x19): con otra respuesta, SIN escritura. */
#ifndef NOR_DISP_ESPERADO
#define NOR_DISP_ESPERADO 0x880Du
#define NOR_TAM_ESPERADO  0x1Au
#endif
_Static_assert((NOR_W6F_INI & 0x3FFFFu) == 0 && NOR_W6F_INI >= 0x44400000u && NOR_W6F_FIN <= NOR_ZONA_INI &&
               NOR_W6F_FIN - NOR_W6F_INI >= 5u * 0x40000u, "NOR_W6F_*: alineada, detrás del main y antes del almacén");
_Static_assert(NOR_USR_FIN <= 0x45900000u, "nada nuestro dentro del FS original del W810 (0x45900000..0x47DFFFFF)");
#define RAMFN __attribute__((section(".ramfn"), noinline, target("arm")))
#define NOR_ESC(a, v) (R16(a) = (u16)(v))
#define NOR_LEE(a)    ((u32)R16(a))
#define NOR_PTR(a)    ((const u8 *)(a))
#define NOR_ORIGEN_EN_FLASH(o) ((u32)(o) >= NOR_BASE && (u32)(o) < NOR_BASE + 0x04000000u)
static inline __attribute__((always_inline)) u32 irq_pendiente(void) { return R32(0xFB020100) & ~R32(0xFB020104); }   /* hal.md §6 */
#endif
