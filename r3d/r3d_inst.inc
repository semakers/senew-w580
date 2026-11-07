/* Instancia las 6 variantes (z x paso de píxel) de un modo. Definir I_MODO e I_N(z, dx) antes. */
#define T_NOMBRE I_N(no, 1)
#define T_MODO I_MODO
#define T_Z R3D_Z_NO
#define T_DX 1
#include "r3d_tramo.inc"
#define T_NOMBRE I_N(si, 1)
#define T_MODO I_MODO
#define T_Z R3D_Z_SI
#define T_DX 1
#include "r3d_tramo.inc"
#define T_NOMBRE I_N(pr, 1)
#define T_MODO I_MODO
#define T_Z R3D_Z_SOLO_PROBAR
#define T_DX 1
#include "r3d_tramo.inc"
#define T_NOMBRE I_N(no, g)
#define T_MODO I_MODO
#define T_Z R3D_Z_NO
#define T_DX R3D_LCD_W
#include "r3d_tramo.inc"
#define T_NOMBRE I_N(si, g)
#define T_MODO I_MODO
#define T_Z R3D_Z_SI
#define T_DX R3D_LCD_W
#include "r3d_tramo.inc"
#define T_NOMBRE I_N(pr, g)
#define T_MODO I_MODO
#define T_Z R3D_Z_SOLO_PROBAR
#define T_DX R3D_LCD_W
#include "r3d_tramo.inc"
#undef I_MODO
#undef I_N
