/* bl_pantalla.c — pantalla mínima del modo installer del BL (plat/bl/bl.h). Secuencias del panel copiadas de hal/lcd.c
 * (bloque XGAM/PDI/PDICON del DB2020, 240x320 RGB565) y recortadas al estricto mínimo del modo installer:
 *   - sin dependencia de hal.h (el BL no incluye hal.h; se declaran R16/R32/SYSCON aquí);
 *   - cache_limpiar() se vuelve un no-op: en el BL la caché de datos está apagada (bl_arranque.S §cp15);
 *   - espera_ms() → bl_espera_ms (bucle NOP a 13 MHz, 6500 vueltas/ms);
 *   - sin lcd_envios ni hw_respirar: la cámara no corre en el BL;
 *   - panel_detectar() hace swreset + lee 0x0028 como hal/lcd.c; sin reloj PLL el panel sigue respondiendo a 13 MHz.
 *
 * BÚFER DE CUADRO: 240*320*2 = 153 600 B. Vive en la SDRAM externa, en 0x20F00000 (banco del GAM 0x20C00000..0x21000000,
 * según «R32(GAM + 0x404) = 0x08320881»). La SDRAM la deja arriba la EROM (lee el bloque EMIF de +0x40, hallazgo A7 de la
 * pista R) antes de saltar a _bl_reset; si por algún motivo no estuviera arriba, la escritura al búfer se perdería y el
 * cuadro saldría negro — el instalador USB sigue respondiendo igual (el anfitrión imprime el progreso).
 */
#include <stddef.h>
#include "bl.h"
#include "bl_pantalla.h"

#ifdef BL_INSTALLER
#if __has_include("../../hal/lcd_paneles.h")
#include "../../hal/lcd_paneles.h"       /* generado: «make paneles» */
#else
#include "../../hal/lcd_paneles_stub.h"  /* sin datos del fabricante: solo emulador */
#endif
#include "bl_pantalla_fuente.h"

#define R8(a)  (*(volatile u8  *)(size_t)(a))
#define R16(a) (*(volatile u16 *)(size_t)(a))
#define R32(a) (*(volatile u32 *)(size_t)(a))
#define SYSCON 0xF9090000u
#define GAM    0xF7000000u
#define PDI    0xF7001000u
#define PCN    0xF7001400u
#define TOPE   2000000u

#define LCD_ANCHO 240
#define LCD_ALTO  320
#define CUADRO    ((u16 *)(size_t)0x20F00000u)

/* paleta: fondo azul oscuro Senew, texto blanco, acento ámbar para la barra */
#define FONDO   0x0010u   /* RGB565: #000080-ish */
#define TEXTO   0xFFFFu
#define BARRA_X 20
#define BARRA_W (LCD_ANCHO - 2 * BARRA_X)
#define BARRA_H 20

extern void bl_espera_ms(unsigned ms);
void hw_respirar(void) __attribute__((weak));            /* no existe en el BL, queda a 0 */

static u8 lcd_hay;
static const lcd_panel *panel = &lcd_paneles[3];
static int panel_r28 = -1;

/* ---- PDI / PDICON: copiado 1:1 de hal/lcd.c (sin lcd_envios ni cache_limpiar) ----------------------------- */

static int pdi_libre(void)
{
    u32 t = TOPE;
    while ((R32(PCN + 0x08) & 1) || (R32(PDI + 0x1F4) & 1)) if (!--t) { lcd_hay = 0; return -1; }
    return 0;
}
static void pdi_byte(u32 off, u8 v)
{
    volatile u32 *p = (volatile u32 *)((PDI + off) & ~3u); u32 s = (off & 3) * 8;
    *p = (*p & ~(0xFFu << s)) | ((u32)v << s);
}
static void pdi_cargar(u32 off, const u8 *p, u32 n) { while (n--) pdi_byte(off++, *p++); }
static void pdi_ptr(u32 byte, u8 off)
{
    R32(PDI + 0x1FC) = (R32(PDI + 0x1FC) & ~(0xFFu << (8 * byte))) | ((u32)off << (8 * byte));
}
static void pdi_area(const u8 *b, u32 n, u32 rs)
{
    u32 w, k;
    for (w = 0; w * 4 < n; w++) {
        u32 v = 0;
        for (k = 0; k < 4; k++) {
            int i = (int)n - 4 - 4 * (int)w + (int)k;
            if (i >= 0) v |= (u32)b[i] << (8 * k);
        }
        R32(PDI + 0xFC - 4 * w) = v;
    }
    R32(PDI + 0x11C) = rs;
}
static void pdi_velocidad(u8 v) { R32(PDI + 0x1F8) = (R32(PDI + 0x1F8) & 0xFFFF00FF) | (u32)v << 8; }
static void lcd_cursor(u32 x, u32 y)
{
    u8 d[10] = { 0x00,0x20, (u8)(x >> 8),(u8)x, 0x00,0x21, (u8)(y >> 8),(u8)y, 0x00,0x22 };
    pdi_area(d, 10, 0x33000000u);
}
static void pdi_orden(const u8 *b, u32 n, u32 rs)
{
    if (pdi_libre()) return;
    R32(PDI + 0x1F8) &= ~0x00900000u;
    pdi_velocidad(PDI_VEL_WRITE);
    pdi_byte(0x0E, (u8)n); pdi_byte(0x0F, 0);
    pdi_area(b, n, rs);
    R32(PDI + 0x1DC) = 1;
    pdi_libre();
}
static void pdi_fin_ordenes(void)
{
    u32 v;
    if (pdi_libre()) return;
    R32(PDI + 0x120) &= ~0xFFu;
    pdi_ptr(0, PGM_GC_EN);
    pdi_velocidad(PDI_VEL_GC);
    v = R32(PDI + 0x1F8); R32(PDI + 0x1F8) = v | 0x00100000u;
    v = R32(PDI + 0x1F8); R32(PDI + 0x1F8) = v & ~0x00800000u;
}
#define PANEL_ESPERA(ms) ((unsigned)(ms) + (unsigned)(ms) / 16u + 4u)
static void lcd_esperar(unsigned ms) { while (ms) { unsigned t = ms > 8u ? 8u : ms; bl_espera_ms(t); ms -= t; } }

static void guion(const u8 *g)
{
    while (*g != 0xFF) {
        if (*g == 0xFE) { lcd_esperar(PANEL_ESPERA(g[1])); g += 2; }
        else {
            u32 n = g[0], rs = g[1] | (u32)g[2] << 8 | (u32)g[3] << 16 | (u32)g[4] << 24;
            pdi_orden(g + 5, n, rs);
            g += 5 + n;
        }
    }
    pdi_fin_ordenes();
}

static int panel_leer_byte(u16 reg, int alto)
{
    int v;
    if (pdi_libre()) return -1;
    lcd_cursor(0, 0);
    if (pdi_libre()) return -1;
    R32(PDI + 0x1F8) &= ~0x00900000u;
    pdi_ptr(2, alto ? PGM_READHIGH_EN : PGM_READ_EN);
    pdi_velocidad(PDI_VEL_READ);
    R32(PDI + 0xFC) = (u32)(reg & 0xFF00) << 8 | (u32)reg << 24;
    R32(PDI + 0x11C) = 0;
    R32(PDI + 0x1DC) = 3;
    if (pdi_libre()) return -1;
    v = (int)(R32(PDI + 0x1F4) >> 8 & 0xFF);
    pdi_fin_ordenes();
    return v;
}
static int lcd_leer_reg(u16 reg)
{
    int a, b;
    if (!lcd_hay) return -1;
    a = panel_leer_byte(reg, 1);
    b = panel_leer_byte(reg, 0);
    return a < 0 || b < 0 ? -1 : a << 8 | b;
}

static void panel_detectar(void)
{
    int v, i;
    guion(g_swreset);
    (void)lcd_leer_reg(PANEL_REG_SONDA);
    v = lcd_leer_reg(PANEL_REG_ID);
    panel_r28 = v;
    panel = &lcd_paneles[3];
    if (v < 0) return;
    for (i = 0; i < 4; i++)
        if (lcd_paneles[i].id < 0 || (v & PANEL_REG_MASC) == lcd_paneles[i].id) { panel = &lcd_paneles[i]; return; }
}

/* lcd_enviar: igual que hal/lcd.c pero sin cache_limpiar (caché de datos apagada en el BL) ni contador lcd_envios */
static void lcd_enviar(const u16 *cuadro, u32 y0, u32 filas)
{
    static const u8 pre[4] = { 0, 0, 0, 0 };
    if (!lcd_hay || !filas || y0 + filas > LCD_ALTO) return;
    pdi_orden(pre, 4, 0);
    if (pdi_libre()) return;
    R32(PCN + 0x10) &= ~8u;
    R32(PCN + 0x60) = 0; R32(PCN + 0x5C) = y0 << 16;
    R32(PCN + 0x50) = (u32)(size_t)cuadro - 0x20000000u;
    R32(PCN + 0x54) = LCD_ANCHO;
    R32(PCN + 0x58) = 16;
    R32(PCN + 0x10) &= ~0x10u;
    lcd_cursor(0, y0);
    pdi_ptr(0, PGM_GC_EN);
    pdi_velocidad(PDI_VEL_GC);
    R32(PDI + 0x1F8) |= 0x00100000u; R32(PDI + 0x1F8) &= ~0x00800000u;
    R32(PCN + 0x48) = y0 << 16;
    R32(PCN + 0x4C) = LCD_ANCHO | filas << 16;
    R32(PCN + 0x04) = 2;
}

/* ---- Fuente, texto y barra -------------------------------------------------------------------------------- */

static void relleno(u16 c)
{
    u32 n = (u32)LCD_ANCHO * LCD_ALTO, i;
    u16 *p = CUADRO;
    for (i = 0; i < n; i++) p[i] = c;
}

/* Dibuja un glifo escalado SCx (sc=1 → 8x8, sc=2 → 16x16, sc=3 → 24x24). FG color del texto. */
static void pinta_glifo(int x, int y, unsigned char ch, u16 fg, int sc)
{
    const unsigned char *g;
    int fy, fx, dy, dx;
    if (ch < BL_FUENTE_PRIM || ch > BL_FUENTE_ULTM) ch = '?';
    g = bl_fuente8x8[ch - BL_FUENTE_PRIM];
    for (fy = 0; fy < 8; fy++) {
        unsigned char fila = g[fy];
        for (fx = 0; fx < 8; fx++) {
            if (!(fila & (0x80 >> fx))) continue;
            for (dy = 0; dy < sc; dy++) {
                int yy = y + fy * sc + dy;
                if (yy < 0 || yy >= LCD_ALTO) continue;
                for (dx = 0; dx < sc; dx++) {
                    int xx = x + fx * sc + dx;
                    if (xx < 0 || xx >= LCD_ANCHO) continue;
                    CUADRO[yy * LCD_ANCHO + xx] = fg;
                }
            }
        }
    }
}

static int largo(const char *s) { int n = 0; while (*s++) n++; return n; }

/* Pinta `s` centrado horizontalmente a y, escala sc. */
static void pinta_centrado(int y, const char *s, u16 fg, int sc)
{
    int w = largo(s) * 8 * sc;
    int x = (LCD_ANCHO - w) / 2;
    if (x < 0) x = 0;
    while (*s) {
        pinta_glifo(x, y, (unsigned char)*s++, fg, sc);
        x += 8 * sc;
    }
}

static void pinta_rect(int x, int y, int w, int h, u16 c)
{
    int yy, xx;
    for (yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= LCD_ALTO) continue;
        for (xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= LCD_ANCHO) continue;
            CUADRO[yy * LCD_ANCHO + xx] = c;
        }
    }
}

/* ---- interfaz pública ------------------------------------------------------------------------------------- */

int bl_pantalla_hay(void) { return lcd_hay; }

static void volcar(void) { if (lcd_hay) lcd_enviar(CUADRO, 0, LCD_ALTO); }

/* Pinta banner inicial sobre el cuadro (sin volcar). Lo llaman _ini y bl_mostrar. */
static void banner(const char *l1, const char *l2)
{
    relleno(FONDO);
    pinta_centrado(30,  l1, TEXTO, 2);
    pinta_centrado(80,  l2, TEXTO, 1);
}

int bl_pantalla_ini(void)
{
    u32 i, t = TOPE;

    R16(SYSCON + 0x18) &= (u16)~0x0200u;            /* XGAM: puerta 9 */
    R16(SYSCON + 0x12) |= 0x0200;                   /* bloque 7 */
    while ((R32(GAM) & 0xFF00) != 0x4100)
        if (!--t) { lcd_hay = 0; return -1; }
    lcd_hay = 1;
    R32(GAM + 0x08) = 0; R32(GAM + 0x08) = 0xFFFFFFFF; R32(GAM + 0x08) = 0xFFFFFF06;
    R32(GAM + 0x04) = 0xF9; R32(GAM + 0x04) = 0; R32(GAM + 0x04) = 0xF9;
    R32(GAM + 0x04) &= ~0x20000u; R32(GAM + 0x04) |= 0x20000u;
    R32(GAM + 0x10) = (R32(GAM + 0x10) & ~0xFFu) | 0x0A; R32(GAM + 0x10) |= 0x100;
    for (i = 0x40C; i <= 0x41C; i += 4) R32(GAM + i) = 0xF;
    R32(PDI + 0x1F8) = (R32(PDI + 0x1F8) & 0xFFFF00F7) | 0x800;
    R32(PCN + 0xFC) = 0x00030180; R32(PCN + 0x100) = 0x10040100;
    R32(PCN + 0x104) = 0x000EA1C5; R32(PCN + 0x108) = 0x166D2800;
    R32(PCN + 0x10C) = 0x00030180; R32(PCN + 0x110) = 0x10040100;
    R32(PCN + 0x114) = 0x000EA1C5; R32(PCN + 0x118) = 0x166D2800;
    R32(PCN + 0x10) = 0x180E;
    R32(GAM + 0x404) = 0x08320881;
    R32(GAM + 0x14) |= 7;
    R32(PCN + 0x1C) &= ~1u; R32(PCN + 0x24) |= 1;

    if (pdi_libre()) return -2;
    R32(PDI + 0x1FC) = 0x00FFFFFF;
    pdi_cargar(PGM_WRITE_EN, pgm_write, sizeof pgm_write);
    pdi_cargar(PGM_GC_EN, pgm_gc, sizeof pgm_gc);
    pdi_cargar(PGM_READHIGH_EN, pgm_readhigh, sizeof pgm_readhigh);
    pdi_cargar(PGM_READ_EN, pgm_read, sizeof pgm_read);
    pdi_ptr(0, PGM_GC_EN); pdi_ptr(2, PGM_READ_EN);
    R32(PCN + 0x40) = LCD_ANCHO | LCD_ALTO << 16; R32(PCN + 0x44) = 16;
    R32(PCN + 0x10) &= ~0x40u;
    R32(PCN + 0xE4) = 0x130F0E0D; R32(PCN + 0xE8) = 0x17161514;
    R32(PCN + 0xEC) = 0x06050403; R32(PCN + 0xF0) = 0x0C0B0A07;
    R32(PCN + 0xF4) = 0; R32(PCN + 0xF8) = 0;

    panel_detectar();
    guion(panel->init);
    banner("SENEW INSTALLER", "CONECTA EL CABLE");
    lcd_enviar(CUADRO, 0, LCD_ALTO);
    guion(g_init2);
    R32(PDI + 0x1E0) |= 0xFFFF;
    return lcd_hay ? 0 : -3;
}

void bl_mostrar(const char *l1, const char *l2)
{
    if (!lcd_hay) return;
    banner(l1 ? l1 : "", l2 ? l2 : "");
    volcar();
}

/* bl_barra: pinta «INSTALANDO...» arriba, un porcentaje grande en medio y la barra ámbar debajo. hecho/total con total == 0:
 * barra vacía y sin porcentaje. hecho >= total: 100 %. */
void bl_barra(u32 hecho, u32 total)
{
    unsigned pct = 0;
    int lleno;
    if (!lcd_hay) return;
    if (total && hecho > total) hecho = total;
    /* porcentaje en 32 bits: evitamos u64 (no hay libgcc 64-bit en el BL). hecho < 32 MiB → hecho*100 < 3.2 GiB cabe en u32.
     * El main de senew es ~1.5 MiB, muy lejos del tope. */
    if (total) pct = (unsigned)((hecho * 100u) / total);
    if (pct > 100) pct = 100;

    relleno(FONDO);
    pinta_centrado(40, "INSTALANDO", TEXTO, 2);

    /* porcentaje como «NNN%» */
    {
        unsigned p = pct, i = 0;
        char buf[6];
        if (p >= 100) { buf[i++] = '1'; buf[i++] = '0'; buf[i++] = '0'; }
        else if (p >= 10) { buf[i++] = '0' + p / 10; buf[i++] = '0' + p % 10; }
        else { buf[i++] = '0' + p; }
        buf[i++] = '%'; buf[i] = 0;
        pinta_centrado(100, buf, TEXTO, 3);
    }

    /* marco de la barra */
    pinta_rect(BARRA_X - 2, 170 - 2, BARRA_W + 4, 2, TEXTO);
    pinta_rect(BARRA_X - 2, 170 + BARRA_H, BARRA_W + 4, 2, TEXTO);
    pinta_rect(BARRA_X - 2, 170 - 2, 2, BARRA_H + 4, TEXTO);
    pinta_rect(BARRA_X + BARRA_W, 170 - 2, 2, BARRA_H + 4, TEXTO);
    /* relleno */
    lleno = (int)(((unsigned)BARRA_W * pct) / 100u);
    pinta_rect(BARRA_X, 170, lleno, BARRA_H, 0xFD40u);   /* ámbar RGB565 ~ #FFAA00 */
    volcar();
}

void bl_listo(void)
{
    if (!lcd_hay) return;
    relleno(FONDO);
    pinta_centrado(80,  "LISTO.", TEXTO, 3);
    pinta_centrado(160, "REINICIA.", TEXTO, 2);
    volcar();
}

#endif /* BL_INSTALLER */
