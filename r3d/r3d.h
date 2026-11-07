/* r3d: rasterizador 3D por software en punto fijo (ARM926, sin FPU). NUESTRO.
 * Nació en la pista M1 del W600 (w600/r3d/); esto es una COPIA recortada para el menú 3D del W580
 * (pista V, docs/menu3d.md). De allí se trae solo la capa de TRAMA —triángulos ya proyectados (R3dPV)
 * a píxeles, con z-buffer— más los senos y los recíprocos. No se ha copiado la capa de geometría con recorte, ni
 * las texturas, ni el cargador de vértices, ni nada del juego: aquí las mallas las proyecta shell/menu3d.c.
 * Cambios respecto del original:
 *   - R3D_SOLO_COLOR (siempre definido aquí): solo los modos PLANO, RAMPA y MEZCLA; no hay R3dTex;
 *   - el marco puede medir lo que sea (r3d_iniciar_marco) y el z-buffer se da con su tamaño;
 *   - R3dMaterial.rampa: una tabla propia de 32 colores para el modo RAMPA (sombra -> color -> brillo); lo que
 *     haya en la tabla se escribe TAL CUAL en el marco (el orden de bytes lo decide quien la rellena). */
#ifndef R3D_H
#define R3D_H
#include <stdint.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;   /* como hal.h */
typedef signed char s8; typedef short s16; typedef int s32; typedef long long s64;

#define R3D_SOLO_COLOR 1
#define R3D_LCD_W 240                      /* (solo lo usan los tramos «girados», que aquí no se llaman) */
#define R3D_SWAP(c) ((u16)(((u32)(c) >> 8) | ((u32)(c) << 8)))
#define R3D_RGB(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))   /* nativo */

/* (son #define y no enum porque la plantilla de tramos los usa en #if) */
#define R3D_PLANO 0
#define R3D_RAMPA 1
#define R3D_DECAL 2
#define R3D_TEXLUZ 3
#define R3D_DECAL_ALFA 4
#define R3D_TEXLUZ_ALFA 5
#define R3D_MEZCLA 6
#define R3D_OSCURECER 7
#define R3D_NMODOS 8
/* Por debajo de R3D_AREA_MINIMA (doble del área, píxeles² Q8) un triángulo se rasteriza con su cobertura de
 * verdad pero con profundidad y luz constantes (sin recíproco ni gradientes). */
#ifndef R3D_AREA_MINIMA
#define R3D_AREA_MINIMA (16 * 256)
#endif
void r3d_area_minima(u32 doble_area_q8);   /* cambia ese umbral en marcha (el marco al doble de resolución admite el cuádruple) */
#ifndef R3D_AREA_PUNTO
#define R3D_AREA_PUNTO 0
#endif

#define R3D_Z_NO 0
#define R3D_Z_SI 1
#define R3D_Z_SOLO_PROBAR 2

/* Vértice proyectado. */
typedef struct {
    s32 x, y;        /* píxeles Q4 */
    u32 z;           /* profundidad Q15: (0 cerca .. 65535 lejos) << 15 */
    s32 u, v;        /* (sin uso aquí) */
    s32 l;           /* luz 0..255 en Q16 */
    u32 w;           /* (sin uso aquí) */
} R3dPV;

typedef struct R3dTex R3dTex;              /* no existe en esta copia */
typedef struct {
    u8 modo, z, cara, persp;
    u16 color;                 /* RGB565 nativo (PLANO, MEZCLA; RAMPA si .rampa es NULL) */
    s32 zsesgo;
    const R3dTex *tex;         /* siempre NULL */
    const u16 *rampa;          /* RAMPA: 32 colores, se escriben tal cual */
} R3dMaterial;

typedef struct {
    u32 tris_entrada, tris_descartados, tris_cara, tris_recortados, tris_trama, tramos, pixeles;
    u32 tris_min, pixeles_min;
    u32 filas, rampas;
} R3dCuenta;
extern R3dCuenta r3d_cuenta;
#ifndef R3D_CON_CUENTAS
#define R3D_SIN_CUENTAS 1
#endif
#ifdef R3D_SIN_CUENTAS
#define R3D_CUENTA(x) ((void)0)
#else
#define R3D_CUENTA(x) (x)
#endif

/* Marco: `pix` con `paso` u16 por fila, de ancho x alto. Se dibuja directamente sobre él. */
void r3d_iniciar_marco(u16 *pix, int paso, int ancho, int alto);
/* El z-buffer no vive en .bss del rasterizador: se le da memoria (tam en bytes) antes de dibujar. Si el marco
 * no cabe en ella, se dibuja SIN z (nunca se indexa a ciegas). */
int  r3d_memoria_z(void *mem, u32 tam);
int  r3d_ancho(void);
int  r3d_alto(void);
void r3d_borrar_z(void);
void r3d_borrar_z_rect(int x0, int y0, int x1, int y1);   /* solo ese rectángulo (x1, y1 excluidos) */
void r3d_material(const R3dMaterial *m);

/* trama */
void r3d_triangulo(const R3dPV *a, const R3dPV *b, const R3dPV *c);

/* utilidades enteras */
extern const s16 r3d_seno_q14[1025];                 /* cuarto de onda, 4096 pasos por vuelta */
s32  r3d_sen(u32 ang4096);
s32  r3d_cos(u32 ang4096);
u32  r3d_raiz(u32 v);
const u16 *r3d_rampa(u16 color_nativo);              /* 32 niveles negro -> color, en formato de panel (con caché) */
#endif
