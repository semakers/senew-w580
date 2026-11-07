/* Plantilla de un tramo horizontal. Se incluye con T_NOMBRE, T_MODO, T_Z y T_DX definidos.
 * Camino caliente: sin coma flotante, sin divisiones, sin llamadas. */
static void __attribute__((unused)) T_NOMBRE(u16 *p, u16 *zb, int n, const R3dTramo *t)   /* (unused: algunos tienen versión en ensamblador) */
{
#if T_Z != R3D_Z_NO
    u32 z = t->z; const u32 dz = t->dz;
#endif
#if T_MODO == R3D_RAMPA || T_MODO == R3D_TEXLUZ || T_MODO == R3D_TEXLUZ_ALFA
    u32 l = t->l; const u32 dl = t->dl;
#endif
#if T_MODO == R3D_DECAL || T_MODO == R3D_TEXLUZ || T_MODO == R3D_DECAL_ALFA || T_MODO == R3D_TEXLUZ_ALFA || T_MODO == R3D_OSCURECER
    u32 uv = t->uv; const u32 duv = t->duv;
    const u8 *const tex = t->tex;
#if T_MODO != R3D_OSCURECER
    const u16 *const lut = t->lut;
#endif
#endif
#if T_MODO == R3D_RAMPA
    const u16 *const rampa = t->rampa;
#endif
#if T_MODO == R3D_PLANO || T_MODO == R3D_MEZCLA
    const u32 color = t->color;
#endif
    (void)zb;
    do {
#if T_Z != R3D_Z_NO
        const u32 zv = z >> 15;
        if (zv < *zb) {
#endif
#if T_MODO == R3D_PLANO
            *p = (u16)color;
#elif T_MODO == R3D_MEZCLA
            { u32 d = *p; d = ((d >> 8) | (d << 8)) & 0xF7DE; *p = R3D_SWAP((d >> 1) + color); }
#elif T_MODO == R3D_RAMPA
            *p = rampa[l >> 19];
#elif T_MODO == R3D_OSCURECER
            if (tex[(uv >> 26) | ((uv & 0xFC00) >> 4)]) { u32 d = *p; d = (((d >> 8) | (d << 8)) & 0xF7DE) >> 1; *p = R3D_SWAP(d); }
#else
            const u32 c = tex[(uv >> 26) | ((uv & 0xFC00) >> 4)];
#if T_MODO == R3D_DECAL_ALFA || T_MODO == R3D_TEXLUZ_ALFA
            if (c) {
#endif
#if T_MODO == R3D_TEXLUZ || T_MODO == R3D_TEXLUZ_ALFA
                *p = lut[((l >> 19) << 8) | c];
#else
                *p = lut[c];
#endif
#if T_Z == R3D_Z_SI
                *zb = (u16)zv;
#endif
#if T_MODO == R3D_DECAL_ALFA || T_MODO == R3D_TEXLUZ_ALFA
            }
#endif
#endif
#if T_Z == R3D_Z_SI && (T_MODO == R3D_PLANO || T_MODO == R3D_RAMPA || T_MODO == R3D_MEZCLA || T_MODO == R3D_OSCURECER)
            *zb = (u16)zv;
#endif
#if T_Z != R3D_Z_NO
        }
        z += dz; zb++;
#endif
#if T_MODO == R3D_RAMPA || T_MODO == R3D_TEXLUZ || T_MODO == R3D_TEXLUZ_ALFA
        l += dl;
#endif
#if T_MODO == R3D_DECAL || T_MODO == R3D_TEXLUZ || T_MODO == R3D_DECAL_ALFA || T_MODO == R3D_TEXLUZ_ALFA || T_MODO == R3D_OSCURECER
        uv += duv;
#endif
        p += T_DX;
    } while (--n);
}
#undef T_NOMBRE
#undef T_MODO
#undef T_Z
#undef T_DX
