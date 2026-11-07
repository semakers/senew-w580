/* r3d interno: recíprocos sin dividir (la división de la libgcc cuesta ~100 instrucciones; esto ~15).
 * Copia recortada de w600/r3d/r3d_int.h (la proyección y el cargador de vértices no se traen). */
#ifndef R3D_INT_H
#define R3D_INT_H
#include "r3d.h"
extern const u16 r3d_rec_tab[256];
extern u32 r3d_rec_dy[4096];                 /* 2^32 / dy para dy = 1..4095 (se rellena al iniciar) */
/* 2^31 / w, con ~17 bits buenos. w >= 1. Tabla de 256 + un paso de Newton. */
static inline u32 r3d_rec31(u32 w)
{
    u32 n = (u32)__builtin_clz(w), m = w << n, y0 = r3d_rec_tab[(m >> 23) & 0xFF];
    u32 e = 0u - (m >> 16) * y0;                        /* (2 - x*y0) en Q31 */
    u32 y1 = (u32)(((u64)y0 * e) >> 16);                /* 1/x en Q30 */
    return n ? y1 >> (31 - n) : 1u;
}
/* 2^30 / w */
static inline u32 r3d_rec30(u32 w) { return r3d_rec31(w) >> 1; }
#endif
