/* r3d, capa de trama: triángulo proyectado -> tramos -> píxeles. Punto fijo; sin coma flotante.
 * COPIA de w600/r3d/r3d_ras.c (pista M1 del W600) para el menú 3D del W580 (pista V). Cambios:
 * R3D_SOLO_COLOR quita los modos con textura y el paletizador; el marco mide lo que sea y el z-buffer se da con
 * su tamaño; rampa propia por material; r3d_borrar_z_rect. El triángulo (r3d_triangulo) está sin tocar. */
#include "r3d_int.h"

typedef struct {
    u32 z, dz;            /* Q15 */
    u32 l, dl;            /* Q16, 0..255 */
    u32 uv, duv;          /* u en los 16 bits altos, v en los bajos; 6 bits de texel + 10 de fracción */
    const u8 *tex;
    const u16 *lut;
    const u16 *rampa;
    u32 color;
} R3dTramo;
typedef void (*fn_tramo)(u16 *p, u16 *zb, int n, const R3dTramo *t);

#define I_MODO R3D_PLANO
#define I_N(z, d) tr_plano_##z##_##d
#include "r3d_inst.inc"
#define I_MODO R3D_RAMPA
#define I_N(z, d) tr_rampa_##z##_##d
#include "r3d_inst.inc"
#ifndef R3D_SOLO_COLOR
#define I_MODO R3D_DECAL
#define I_N(z, d) tr_decal_##z##_##d
#include "r3d_inst.inc"
#define I_MODO R3D_TEXLUZ
#define I_N(z, d) tr_texluz_##z##_##d
#include "r3d_inst.inc"
#define I_MODO R3D_DECAL_ALFA
#define I_N(z, d) tr_decala_##z##_##d
#include "r3d_inst.inc"
#define I_MODO R3D_TEXLUZ_ALFA
#define I_N(z, d) tr_texluza_##z##_##d
#include "r3d_inst.inc"
#endif
#define I_MODO R3D_MEZCLA
#define I_N(z, d) tr_mezcla_##z##_##d
#include "r3d_inst.inc"
#ifndef R3D_SOLO_COLOR
#define I_MODO R3D_OSCURECER
#define I_N(z, d) tr_oscurecer_##z##_##d
#include "r3d_inst.inc"
#endif

#define FILA(n) { { tr_##n##_no_1, tr_##n##_si_1, tr_##n##_pr_1 }, { tr_##n##_no_g, tr_##n##_si_g, tr_##n##_pr_g } }
#if defined(__arm__) && !defined(R3D_SIN_ASM)
void r3d_asm_rampa_si_1(u16 *p, u16 *zb, int n, const R3dTramo *t);
#define tr_rampa_si_1 r3d_asm_rampa_si_1
#endif
static const fn_tramo tabla[R3D_NMODOS][2][3] = {
    [R3D_PLANO] = FILA(plano), [R3D_RAMPA] = FILA(rampa), [R3D_MEZCLA] = FILA(mezcla) };   /* los demás modos: NULL -> PLANO */

static struct {
    u16 *base;            /* píxel lógico (0,0) */
    s32 paso_x, paso_y;   /* en píxeles del cuadro */
    s32 w, h, girado;
    u16 *cuadro;
    fn_tramo tramo;
    R3dMaterial mat;
    R3dTramo t;
    int usa_uv, usa_l, usa_z, sin_relleno, es_min;
    fn_tramo tramo_plano;   /* TEXLUZ sin z con la misma luz en los tres vértices: el tramo sin luz, más barato */
} e;
/* El z-buffer NO vive en .bss: una app externa no puede declarar arreglos grandes (docs/sistema.md
 * §11.3), que se quedarían en el .bss del sistema aunque nadie juegue. Lo da r3d_memoria_z(). */
static u16 *zbuf, *zmem;                   /* sin memoria de z se dibuja SIN z (nunca se indexa a ciegas) */
static u32 zcap;                           /* u16 que caben en zmem */

static void z_elegir(void) { zbuf = (zmem && (u32)(e.w * e.h) <= zcap) ? zmem : 0; }
int r3d_memoria_z(void *mem, u32 tam)
{
    zmem = (u16 *)mem; zcap = mem ? tam / 2 : 0;
    z_elegir();
    return zbuf ? 0 : -1;
}
R3dCuenta r3d_cuenta;
static u32 area_minima = R3D_AREA_MINIMA;
void r3d_area_minima(u32 doble_area_q8) { area_minima = doble_area_q8; }   /* (pista V) en marcha: depende de la resolución del marco */

int r3d_ancho(void) { return e.w; }
int r3d_alto(void) { return e.h; }
void r3d_sin_relleno(int si) { e.sin_relleno = si; }

/* Marco que nos presta el sistema (docs/sistema.md §11.2). En APP_BANDAS `pix` apunta DENTRO del marco
 * del sistema con paso 176: se rasteriza en el sitio definitivo, sin copia y sin memoria extra. En
 * APP_GIRADA el sistema gira al volcar, así que aquí la imagen es LINEAL (paso_x = 1) y los tramos
 * pueden usar las rutinas rápidas en ensamblador, que son las de paso 1. */
void r3d_iniciar_trama_marco(u16 *pix, int paso, int an, int al)
{
    e.cuadro = pix; e.girado = 0;
    e.w = an; e.h = al;
    e.base = pix; e.paso_x = 1; e.paso_y = paso;
    z_elegir();
}

#if defined(__arm__) && !defined(R3D_SIN_ASM)
void r3d_asm_llena32(u32 *p, u32 v, u32 bloques);     /* bloques de 8 palabras (32 bytes) */
#endif
static void llena16(u16 *p, u32 v, u32 n)
{
    u32 vv = v | (v << 16);
    while (n && ((uintptr_t)p & 3)) { *p++ = (u16)v; n--; }
#if defined(__arm__) && !defined(R3D_SIN_ASM)
    if (n >= 16) { u32 bl = n >> 4; r3d_asm_llena32((u32 *)(void *)p, vv, bl); p += bl << 4; n -= bl << 4; }
#endif
    { u32 *q = (u32 *)p; for (; n >= 8; n -= 8) { q[0] = vv; q[1] = vv; q[2] = vv; q[3] = vv; q += 4; } p = (u16 *)q; }
    while (n--) *p++ = (u16)v;
}

void r3d_borrar_z(void) { if (zbuf) llena16(zbuf, 0xFFFF, (u32)(e.w * e.h)); }
void r3d_borrar_z_rect(int x0, int y0, int x1, int y1)
{
    int y;
    if (!zbuf) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > e.w) x1 = e.w;
    if (y1 > e.h) y1 = e.h;
    if (x1 <= x0) return;
    for (y = y0; y < y1; y++) llena16(zbuf + y * e.w + x0, 0xFFFF, (u32)(x1 - x0));
}

/* rampas de 32 niveles por color, en formato de panel; caché pequeña de correspondencia directa */
#define NRAMPAS 64
static u16 rampas[NRAMPAS][32]; static u16 rampa_color[NRAMPAS]; static u8 rampa_ok[NRAMPAS];
const u16 *r3d_rampa(u16 c)
{
    u32 i = (c ^ (c >> 6) ^ (c >> 11)) & (NRAMPAS - 1), s;
    if (!rampa_ok[i] || rampa_color[i] != c) {
        for (s = 1; s <= 32; s++) {
            u32 v = ((((c & 0xF81Fu) * s) >> 5) & 0xF81F) | ((((c & 0x07E0u) * s) >> 5) & 0x07E0);
            rampas[i][s - 1] = R3D_SWAP(v);
        }
        rampa_color[i] = c; rampa_ok[i] = 1; R3D_CUENTA(r3d_cuenta.rampas++);
    }
    return rampas[i];
}

void r3d_material(const R3dMaterial *m)
{
    const int z = zbuf ? m->z : R3D_Z_NO;      /* sin memoria de z: se dibuja sin z, no se indexa a ciegas */
    e.mat = *m;
    e.tramo = tabla[m->modo][e.girado ? 1 : 0][z];
    if (!e.tramo) { e.mat.modo = R3D_PLANO; e.tramo = tabla[R3D_PLANO][0][z]; }      /* un modo que esta copia no trae */
    e.tramo_plano = 0;
    e.usa_uv = (m->modo >= R3D_DECAL && m->modo <= R3D_TEXLUZ_ALFA) || m->modo == R3D_OSCURECER;
    e.usa_l = m->modo == R3D_RAMPA || m->modo == R3D_TEXLUZ || m->modo == R3D_TEXLUZ_ALFA;
    e.usa_z = z != R3D_Z_NO;
    e.t.tex = 0; e.t.lut = 0;
    if (m->modo == R3D_RAMPA) e.t.rampa = m->rampa ? m->rampa : r3d_rampa(m->color);
    else if (m->modo == R3D_PLANO) e.t.color = R3D_SWAP(m->color);
    else if (m->modo == R3D_MEZCLA) e.t.color = (m->color & 0xF7DEu) >> 1;
}
const R3dMaterial *r3d_material_actual(void) { return &e.mat; }

#ifdef R3D_MEDIR
/* Diagnóstico (solo en el build de medidas): reparto del coste DENTRO de r3d_triangulo. */
unsigned (*r3d_reloj)(void);
unsigned r3d_t_pre, r3d_t_grad, r3d_t_tramos;
#endif
#if defined(R3D_MEDIR) && !defined(MEDIR_UNA_PASADA)
#define MED_T0() unsigned mt_ = r3d_reloj ? r3d_reloj() : 0
#define MED_ACU(c) do { if (r3d_reloj) { unsigned n_ = r3d_reloj(); (c) += n_ - mt_; mt_ = n_; } } while (0)
#define SIN_RELLENO e.sin_relleno
#else
#define MED_T0() do { } while (0)
#define MED_ACU(c) do { } while (0)
#define SIN_RELLENO 0                /* la pasada «sin relleno» solo existe en el arnés de dos pasadas */
#endif

static inline s32 sat32(s64 v) { return v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFFLL ? -0x7FFFFFFF : (s32)v; }
static inline int bits32(u32 v) { return v ? 32 - __builtin_clz(v) : 0; }

/* dx/dy en Q16, con dx y dy en Q4 (dy > 0) */
static inline s32 pendiente(s32 dx, s32 dy)
{
    if (dy < 4096) return (s32)(((s64)dx * (s64)r3d_rec_dy[dy]) >> 16);
    return (s32)(((s64)dx * 65536) / dy);
}
typedef struct { s32 dx, dy; u32 fila; } Grad;      /* por píxel; fila = valor en (x = 0, y actual) */

/* 2^20 / m para m = 1..16: dividir por el largo de un tramo corto sin llamar a la libgcc. */
static const s32 rec_m[17] = { 0, 1048576, 524288, 349525, 262144, 209715, 174763, 149797, 131072,
                               116508, 104858, 95325, 87381, 80660, 74898, 69905, 65536 };

void r3d_triangulo(const R3dPV *a, const R3dPV *b, const R3dPV *c)
{
    const R3dPV *v0 = a, *v1 = b, *v2 = c, *tmp;
    s32 ya, ym, yb, area, y, inv, s, sh, kb, x1r, x2r, y1r, y2r;
    s64 k[4]; s32 kx1, kx2, ky1, ky2; u32 au, mk;
    s32 dxl, dxc, xl_e, xc_e; int mitad, medio_dcha;
#ifdef R3D_SOLO_COLOR       /* sin texturas: el compilador quita entero el camino de u, v y perspectiva */
    Grad gz, gl, gu, gv, gw; int persp = 0;
    fn_tramo tramo = e.tramo; int usa_l = e.usa_l, pequeno = 0; const int usa_z = e.usa_z, usa_uv = 0;
#else
    Grad gz, gl, gu, gv, gw; int persp = e.mat.persp && e.usa_uv;
    fn_tramo tramo = e.tramo; int usa_l = e.usa_l, pequeno = 0; const int usa_z = e.usa_z, usa_uv = e.usa_uv;
#endif
    s32 a0u, a1u, a2u, a0v, a1v, a2v, a0w = 0, a1w = 0, a2w = 0;
    MED_T0();

    if (v0->y > v1->y) { tmp = v0; v0 = v1; v1 = tmp; }
    if (v1->y > v2->y) { tmp = v1; v1 = v2; v2 = tmp; }
    if (v0->y > v1->y) { tmp = v0; v0 = v1; v1 = tmp; }
    ya = (v0->y + 15) >> 4; ym = (v1->y + 15) >> 4; yb = (v2->y + 15) >> 4;
    if (ya < 0) ya = 0;
    if (yb > e.h) yb = e.h;
    if (ya >= yb) return;
    {   /* ¿cruza el centro de alguna COLUMNA de píxeles? Los triángulos de menos de un píxel muchas veces no,
         * y pagaban la preparación entera para no pintar nada (lo mismo que arriba se hace con las filas). */
        s32 xmin = v0->x, xmax = v0->x;
        if (v1->x < xmin) xmin = v1->x; else if (v1->x > xmax) xmax = v1->x;
        if (v2->x < xmin) xmin = v2->x; else if (v2->x > xmax) xmax = v2->x;
        xmin = (xmin + 15) >> 4; xmax = (xmax + 15) >> 4;
        if (xmin >= xmax || xmax <= 0 || xmin >= e.w) return;
    }
    x1r = v1->x - v0->x; x2r = v2->x - v0->x; y1r = v1->y - v0->y; y2r = v2->y - v0->y;
    area = x1r * y2r - x2r * y1r;
    if (area == 0) return;
    medio_dcha = area > 0;
    R3D_CUENTA(r3d_cuenta.tris_trama++);
    au = (u32)(area < 0 ? -area : area);

    /* ---- triángulos MINÚSCULOS: un solo píxel ---------------------------------------------------------
     * Medido en Bob-omb Battlefield [observado]: de los ~954 triángulos que se rasterizan por cuadro,
     * **453 (47 %) tienen menos de 4 píxeles de área y entre todos aportan 326 píxeles (0,55 % de la
     * imagen)**. Pagar por ellos el recíproco, cinco gradientes y el recorrido de filas es tirar el
     * tiempo. Se pintan como UN píxel en su centro: así no se abren agujeros en la silueta (que es lo
     * que pasaría si se descartaran) y cuesta ~40 instrucciones en vez de ~500. */
#if R3D_AREA_PUNTO > 0
    if (au < R3D_AREA_PUNTO && au < R3D_AREA_MINIMA) {
        s32 cx = ((v0->x + v1->x + v2->x) * 21845) >> 16, cy = ((v0->y + v1->y + v2->y) * 21845) >> 16;
        s32 px = cx >> 4, py = cy >> 4;
        R3D_CUENTA(r3d_cuenta.tris_min++);
        if (px < 0 || px >= e.w || py < 0 || py >= e.h) return;
        R3D_CUENTA(r3d_cuenta.tramos++); R3D_CUENTA(r3d_cuenta.pixeles++); R3D_CUENTA(r3d_cuenta.pixeles_min++);
        if (SIN_RELLENO) return;
        if (e.usa_z) { e.t.z = v0->z - ((u32)e.mat.zsesgo << 15); e.t.dz = 0; }
        if (e.usa_l) { e.t.l = (u32)v0->l; e.t.dl = 0; }
        if (e.usa_uv) { e.t.uv = ((((u32)v0->u >> 6) & 0xFFFF) << 16) | (((u32)v0->v >> 6) & 0xFFFF); e.t.duv = 0; }
        e.tramo(e.base + py * e.paso_y + px * e.paso_x, zbuf + py * e.w + px, 1, &e.t);
        return;
    }
#endif
    /* luz PLANA sobre textura sin z (el cielo, los fondos): el mismo resultado con el tramo sin luz,
     * apuntando a la fila de la tabla que corresponde a esa luz. Medido: el cielo eran 308 k por cuadro. */
    /* (el atajo de luz plana sobre textura no existe en esta copia: no hay texturas) */

    /* ---- triángulos PEQUEÑOS: cobertura de verdad, atributos constantes ----------------------------------
     * Se recorren sus filas como las de cualquiera (así no hay huecos ni solapes con los vecinos), pero la
     * profundidad, la luz y el texel son los de una media barata de los tres vértices: sin recíproco del
     * área y sin gradientes. */
    if (au < area_minima) {
        pequeno = 1; persp = 0;
        gz.dx = gz.dy = 0; gz.fila = 0; gl = gz; gu = gz; gv = gz; gw = gz;      /* no se usan; callan al compilador */
        if (usa_z) { e.t.z = (v0->z >> 1) + (v1->z >> 2) + (v2->z >> 2) - ((u32)e.mat.zsesgo << 15); e.t.dz = 0; }
        if (usa_l) { e.t.l = (u32)((v0->l >> 1) + (v1->l >> 2) + (v2->l >> 2)); e.t.dl = 0; }
        if (usa_uv) { const u32 u = (u32)((v0->u >> 1) + (v1->u >> 2) + (v2->u >> 2)), v = (u32)((v0->v >> 1) + (v1->v >> 2) + (v2->v >> 2));
                      e.t.uv = (((u >> 6) & 0xFFFF) << 16) | ((v >> 6) & 0xFFFF); e.t.duv = 0; }
        goto filas;
    }

    gz.dx = gz.dy = 0; gz.fila = 0; gl = gz; gu = gz; gv = gz; gw = gz;      /* (solo los triángulos que llevan gradientes) */
    /* k = d * 2^32 / área (Q28 por píxel), con un recíproco de 15+ bits; luego se normalizan a 31 bits */
    s = bits32(au) - 15; if (s < 0) s = 0;
    inv = (s32)r3d_rec30(au >> s);
    if (area < 0) inv = -inv;
    k[0] = (s64)y2r * inv; k[1] = (s64)y1r * inv; k[2] = (s64)x1r * inv; k[3] = (s64)x2r * inv;
    mk = 0;
    { int i; for (i = 0; i < 4; i++) { if (s >= 2) k[i] >>= (s - 2); else k[i] *= (1 << (2 - s));
        { u64 m = (u64)(k[i] < 0 ? -k[i] : k[i]); u32 hb = (u32)(m >> 32) ? 32 + (u32)bits32((u32)(m >> 32)) : (u32)bits32((u32)m); if (hb > mk) mk = hb; } } }
    sh = (s32)mk - 30; if (sh < 0) sh = 0;
    kx1 = (s32)(k[0] >> sh); kx2 = (s32)(k[1] >> sh); ky1 = (s32)(k[2] >> sh); ky2 = (s32)(k[3] >> sh); kb = 28 - sh;
    MED_ACU(r3d_t_pre);

/* diferencias de 32 bits x k de 31 bits: cada producto es un SMULL */
#define GRAD(g, A0, A1, A2) { const s32 d1 = (s32)((u32)(A1) - (u32)(A0)), d2 = (s32)((u32)(A2) - (u32)(A0)); \
        (g).dx = sat32(((s64)d1 * kx1 - (s64)d2 * kx2) >> kb); (g).dy = sat32(((s64)d2 * ky1 - (s64)d1 * ky2) >> kb); \
        (g).fila = (u32)(A0) + (u32)(s32)(((s64)(-v0->x) * (g).dx + (s64)(ya * 16 - v0->y) * (g).dy) >> 4); }
    if (usa_z) { GRAD(gz, v0->z, v1->z, v2->z); gz.fila -= ((u32)e.mat.zsesgo << 15); }
    if (usa_l) GRAD(gl, v0->l, v1->l, v2->l);
    if (persp && e.mat.persp == 2) {            /* automática: solo si la profundidad varía > 1,5x y el triángulo es grande */
        u32 wa = v0->w, wb = v0->w;
        if (v1->w > wa) wa = v1->w;
        if (v2->w > wa) wa = v2->w;
        if (v1->w < wb) wb = v1->w;
        if (v2->w < wb) wb = v2->w;
        if (au < 100 * 256 || wa <= wb + (wb >> 1)) persp = 0;
    }
    if (usa_uv) {
        if (!persp) { GRAD(gu, v0->u, v1->u, v2->u); GRAD(gv, v0->v, v1->v, v2->v); }
        else {
            /* u/w, v/w y 1/w lineales en pantalla; w normalizado a 15 bits dentro del triángulo */
            u32 wm = v0->w > v1->w ? v0->w : v1->w; int ws;
            if (v2->w > wm) wm = v2->w;
            ws = bits32(wm) - 15; if (ws < 0) ws = 0;
            a0w = (s32)(v0->w >> ws); a1w = (s32)(v1->w >> ws); a2w = (s32)(v2->w >> ws);
            a0u = (s32)(((s64)(v0->u >> 4) * a0w) >> 11); a1u = (s32)(((s64)(v1->u >> 4) * a1w) >> 11); a2u = (s32)(((s64)(v2->u >> 4) * a2w) >> 11);
            a0v = (s32)(((s64)(v0->v >> 4) * a0w) >> 11); a1v = (s32)(((s64)(v1->v >> 4) * a1w) >> 11); a2v = (s32)(((s64)(v2->v >> 4) * a2w) >> 11);
            GRAD(gu, a0u, a1u, a2u); GRAD(gv, a0v, a1v, a2v); GRAD(gw, a0w * 4096, a1w * 4096, a2w * 4096);
        }
    }
#undef GRAD

    MED_ACU(r3d_t_grad);
filas:
    dxl = pendiente(x2r, y2r);                        /* arista larga v0-v2 */
    xl_e = v0->x * 4096 + (s32)(((s64)(ya * 16 - v0->y) * dxl) >> 4);
    y = ya;
    {
        /* Todo lo que es constante en el triángulo se fija UNA vez (antes se reescribía en cada fila), y
         * el estado de las filas va en locales: `e` es global y entre fila y fila hay una llamada. */
        R3dTramo *const t = &e.t; const s32 an = e.w, paso_fila = e.paso_y, paso_px = e.paso_x;
        u16 *fp = e.base + y * paso_fila, *fz = zbuf + y * an;
        if (!pequeno) {
            if (usa_z) t->dz = (u32)gz.dx;
            if (usa_l) t->dl = (u32)gl.dx;
            if (usa_uv && !persp) t->duv = ((((u32)gu.dx >> 6) & 0xFFFF) << 16) + (((u32)(gv.dx >> 6)) & 0xFFFF);
        }
        for (mitad = 0; mitad < 2; mitad++) {
            const R3dPV *p0 = mitad ? v1 : v0, *p1 = mitad ? v2 : v1;
            s32 y_fin = mitad ? yb : (ym < yb ? ym : yb), dyc = p1->y - p0->y, xa, xb, dxa, dxb;
            if (y_fin < 0) y_fin = 0;
            if (y >= y_fin || dyc <= 0) continue;
            dxc = pendiente(p1->x - p0->x, dyc);
            xc_e = p0->x * 4096 + (s32)(((s64)(y * 16 - p0->y) * dxc) >> 4);
            if (medio_dcha) { xa = xl_e; dxa = dxl; xb = xc_e; dxb = dxc; } else { xa = xc_e; dxa = dxc; xb = xl_e; dxb = dxl; }
/* Una fila afín. Z, L y UV son constantes de compilación en cada copia: el compilador quita lo que sobra. */
#define FILAS(Z, L, UV) \
            for (; y < y_fin; y++) { \
                s32 xi = (xa + 0xFFFF) >> 16, xd = (xb + 0xFFFF) >> 16, n; \
                R3D_CUENTA(r3d_cuenta.filas++); \
                if (xi < 0) xi = 0; \
                if (xd > an) xd = an; \
                n = xd - xi; \
                if (n > 0) { \
                    R3D_CUENTA(r3d_cuenta.tramos++); R3D_CUENTA(r3d_cuenta.pixeles += (u32)n); \
                    if (Z) t->z = gz.fila + (u32)xi * (u32)gz.dx; \
                    if (L) t->l = gl.fila + (u32)xi * (u32)gl.dx; \
                    if (UV) { const u32 u = gu.fila + (u32)xi * (u32)gu.dx, v = gv.fila + (u32)xi * (u32)gv.dx; \
                              t->uv = (((u >> 6) & 0xFFFF) << 16) | ((v >> 6) & 0xFFFF); } \
                    if (!SIN_RELLENO) tramo(fp + xi * paso_px, fz + xi, n, t); \
                } \
                xa += dxa; xb += dxb; fp += paso_fila; fz += an; \
                if (Z) gz.fila += (u32)gz.dy; \
                if (L) gl.fila += (u32)gl.dy; \
                if (UV) { gu.fila += (u32)gu.dy; gv.fila += (u32)gv.dy; } \
            }
            if (pequeno) { FILAS(0, 0, 0) }
            else if (!persp) {
                if (usa_z && usa_l && usa_uv) { FILAS(1, 1, 1) }
                else if (usa_z && usa_uv) { FILAS(1, 0, 1) }
                else if (usa_z && usa_l) { FILAS(1, 1, 0) }
                else if (usa_uv && !usa_l) { FILAS(0, 0, 1) }
                else { FILAS(usa_z, usa_l, usa_uv) }
            } else for (; y < y_fin; y++) {
                s32 xi = (xa + 0xFFFF) >> 16, xd = (xb + 0xFFFF) >> 16, n;
                R3D_CUENTA(r3d_cuenta.filas++);
                if (xi < 0) xi = 0;
                if (xd > an) xd = an;
                n = xd - xi;
                if (n > 0) {
                    /* perspectiva correcta cada 16 píxeles: un recíproco por trozo */
                    u16 *p = fp + xi * paso_px, *zb = fz + xi;
                    s32 uw = (s32)(gu.fila + (u32)xi * (u32)gu.dx), vw = (s32)(gv.fila + (u32)xi * (u32)gv.dx);
                    s32 ww = (s32)(gw.fila + (u32)xi * (u32)gw.dx), r, u0, v0t;
                    R3D_CUENTA(r3d_cuenta.tramos++); R3D_CUENTA(r3d_cuenta.pixeles += (u32)n);
                    if (usa_z) t->z = gz.fila + (u32)xi * (u32)gz.dx;
                    if (usa_l) t->l = gl.fila + (u32)xi * (u32)gl.dx;
                    r = (s32)r3d_rec30((u32)(ww < 4096 ? 1 : ww >> 12));
                    u0 = (s32)(((s64)uw * r) >> 15); v0t = (s32)(((s64)vw * r) >> 15);
                    while (n > 0) {
                        s32 m = n > 16 ? 16 : n, u1, v1t, du, dv;
                        uw += gu.dx * m; vw += gv.dx * m; ww += gw.dx * m;
                        r = (s32)r3d_rec30((u32)(ww < 4096 ? 1 : ww >> 12));
                        u1 = (s32)(((s64)uw * r) >> 15); v1t = (s32)(((s64)vw * r) >> 15);
                        /* dividir por m (1..16) con la libgcc costaba ~100 instrucciones POR TRAMO: tabla de recíprocos */
                        if (m == 16) { du = (u1 - u0) >> 4; dv = (v1t - v0t) >> 4; }
                        else { const s32 r16 = rec_m[m]; du = (s32)(((s64)(u1 - u0) * r16) >> 20); dv = (s32)(((s64)(v1t - v0t) * r16) >> 20); }
                        t->uv = ((((u32)u0 >> 6) & 0xFFFF) << 16) | (((u32)v0t >> 6) & 0xFFFF);
                        t->duv = ((((u32)du >> 6) & 0xFFFF) << 16) + (((u32)(dv >> 6)) & 0xFFFF);
                        if (!SIN_RELLENO) tramo(p, zb, m, t);
                        if (usa_z) t->z += t->dz * (u32)m;
                        if (usa_l) t->l += t->dl * (u32)m;
                        p += m * paso_px; zb += m; n -= m; u0 = u1; v0t = v1t;
                    }
                }
                xa += dxa; xb += dxb; fp += paso_fila; fz += an;
                if (usa_z) gz.fila += (u32)gz.dy;
                if (usa_l) gl.fila += (u32)gl.dy;
                gu.fila += (u32)gu.dy; gv.fila += (u32)gv.dy; gw.fila += (u32)gw.dy;
            }
#undef FILAS
            if (medio_dcha) xl_e = xa; else xl_e = xb;      /* la arista larga sigue en la otra mitad */
        }
    }
    MED_ACU(r3d_t_tramos);
}
