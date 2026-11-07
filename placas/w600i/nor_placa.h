/* nor_placa.h (W600i) — lo que chips/nor_intel/clasico.c necesita de la placa y del SoC DB2010. Pista F600.
 * Valores de w600:sistema/hal/hal.h:118-131 y w600:sistema/hal/nor.c:44-45,130 [observado]. Requiere el hal.h del DB2010
 * (R16, irq_cerrar, irq_abrir) SIN su bloque de la NOR (hal.h:115-150 pasa a ser `#include "clasico.h"`). */
#ifndef NOR_PLACA_H
#define NOR_PLACA_H
#include "hal.h"
#define NOR_BASE     0x44000000u                 /* selección de chip 0, 32 MiB */
#ifndef NOR_ESCRITURA
#define NOR_ESCRITURA 1
#endif
#define NOR_ZONA_INI 0x45680000u                 /* almacén: 4 bloques de 128 KiB */
#define NOR_ZONA_FIN 0x45700000u
#define NOR_USR_INI  0x45700000u                 /* W6U: 8 bloques */
#define NOR_USR_FIN  0x45800000u
#define NOR_ESCR_INI NOR_ZONA_INI
#define NOR_ESCR_FIN NOR_USR_FIN
/* La MEMORIA (W6F): DECIDIDA por el mantenedor (2026-10-14, P600 tercera vuelta): 16 MiB, 0x44600000..0x45600000, «mitad sistema, mitad
 * contenido»; 0x440A0000..0x44600000 (5,375 MiB) para el main y el contenido del paquete (W6FS con las .sen). */
#ifndef NOR_W6F_INI
#define NOR_W6F_INI  0x44600000u
#endif
#define NOR_W6F_FIN  0x45600000u
#define NOR_W6F_PAG  1024u
/* (P600; w600/docs/bateria_w600.md §6, A6) la guarda: SOLO el chip esperado (lo que contesta el emulador; el del aparato
 * [sin saber]). Si contesta otra cosa: sin escritura. */
#ifndef NOR_DISP_ESPERADO
#define NOR_DISP_ESPERADO 0x880Du
#define NOR_TAM_ESPERADO  0x19u
#endif
_Static_assert((NOR_W6F_INI & 0x3FFFFu) == 0 && NOR_W6F_INI >= 0x44200000u && NOR_W6F_FIN <= 0x45600000u &&
               NOR_W6F_FIN - NOR_W6F_INI >= 5u * 0x40000u, "NOR_W6F_*: alineada a 256 KiB, fuera del principio del main y de la partida de un juego");
#define RAMFN __attribute__((section(".ramfn"), noinline, target("arm")))
#define NOR_ESC(a, v) (R16(a) = (u16)(v))
#define NOR_LEE(a)    ((u32)R16(a))
#define NOR_PTR(a)    ((const u8 *)(a))
#define NOR_ORIGEN_EN_FLASH(o) ((u32)(o) >= NOR_BASE && (u32)(o) < NOR_BASE + 0x02000000u)
static inline __attribute__((always_inline)) u32 irq_pendiente(void) { return R32(0xFB020100) & ~R32(0xFB020104); }   /* hal.md §6 */
#endif
