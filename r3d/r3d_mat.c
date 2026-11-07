/* r3d: senos, raíz entera y la tabla de recíprocos de las pendientes. Copiado de w600/r3d/r3d_geo.c
 * (de ese fichero solo se traen estas utilidades; la proyección, el recorte y la lista diferida no). */
#include "r3d_int.h"

u32 r3d_rec_dy[4096];
void r3d_iniciar_trama_marco(u16 *pix, int paso, int an, int al);
void r3d_iniciar_marco(u16 *pix, int paso, int an, int al)
{
    r3d_iniciar_trama_marco(pix, paso, an, al);
    /* (pista O) 2^32 / i con UNA división de 32 bits por cada i impar (antes, 4095 divisiones de 64 bits de la libgcc:
     * 1,05 M de instrucciones en el primer cuadro del menú): 2^32 / i = (2^32 - 1) / i, más 1 si el resto era i - 1;
     * y para i par, la mitad (por abajo) del de i / 2. */
    if (!r3d_rec_dy[1]) {
        u32 i;
        r3d_rec_dy[1] = 0xFFFFFFFFu; r3d_rec_dy[2] = 0x80000000u;
        for (i = 3; i < 4096; i++) {
            if (i & 1) { const u32 q = 0xFFFFFFFFu / i; r3d_rec_dy[i] = q + (0xFFFFFFFFu - q * i == i - 1); }
            else r3d_rec_dy[i] = r3d_rec_dy[i >> 1] >> 1;
        }
    }
}

s32 r3d_sen(u32 a)
{
    a &= 4095;
    if (a < 1024) return r3d_seno_q14[a];
    if (a < 2048) return r3d_seno_q14[2048 - a];
    if (a < 3072) return -r3d_seno_q14[a - 2048];
    return -r3d_seno_q14[4096 - a];
}
s32 r3d_cos(u32 a) { return r3d_sen(a + 1024); }
u32 r3d_raiz(u32 v)
{
    u32 r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}
