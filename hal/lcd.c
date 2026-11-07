/* Pantalla del W580i: bloque gráfico «XGAM» (0xF7000000) del DB2020 -> panel de 240x320 (controlador tipo
 * Renesas R61505, «Lindsay»), RGB565. docs/hal.md §4, de la que esto es el §4.7 con topes en las esperas.
 *
 * Cambia ENTERO respecto al DB2010 (allí el PDI recibía los píxeles por DMA). Aquí hay dos piezas:
 *   PDI    0xF7001000: un secuenciador con memoria de programa (microcódigo) que saca las ÓRDENES por el bus
 *          de 8 bits del panel. El microcódigo y las secuencias del panel son datos de configuración del
 *          propio teléfono (/ifs/settings/display/dispdriver0.dat): hal/lcd_paneles.h se GENERA de ese
 *          fichero (herr/hacer_paneles.py) con los cuatro descriptores de panel, y lcd_init() ELIGE el
 *          suyo como el original: reset por software, lectura del registro 0x0028 del panel por el PDI.
 *   PDICON 0xF7001400: manda el CUADRO; lee los píxeles ÉL SOLO de la SDRAM (no pasa por la CPU ni por DMA).
 *
 * LO QUE EL EMULADOR NO PRUEBA: la máquina de la pista E solo modela el PDICON (+04 <- 2 copia el rectángulo
 * a la consola); las órdenes del PDI se registran y no se interpretan. Es decir: que se vea imagen en el
 * emulador NO dice que lcd_init() encienda el panel de verdad (hal.md §4.8).
 *
 * El cuadro se manda por BANDAS DE ANCHO COMPLETO (x = 0, 240 px, filas y0..y0+h-1), que es lo que recomienda
 * hal.md §4.3: PDICON+48 es la esquina del rectángulo DENTRO del búfer de origen (el origen, +50, es siempre el
 * principio del cuadro) y la posición en el panel la pone el PDI con el cursor (R20 = 0, R21 = y0).
 * [observado por la pista E, 00:05] su máquina lo modela así desde entonces: si esta lectura fuera otra, las
 * capturas del emulador saldrían con las bandas descolocadas. No cuesta CPU: los píxeles los mueve el PDICON. */
#include "hal.h"
#if __has_include("lcd_paneles.h")
#include "lcd_paneles.h"          /* generado desde el dispdriver0.dat de TU teléfono: «make paneles» */
#else
#include "lcd_paneles_stub.h"     /* sin datos del fabricante: vale para el emulador, NO para un teléfono (ver el fichero) */
#endif
/* (S7) LAS ESPERAS `%wNN` DE LOS GUIONES DEL PANEL, CON MARGEN. El descriptor pide un MÍNIMO (1, 10, 34, 120 o 150 ms) y aquí se
 * esperaba justo eso por el temporizador 2 (1,04 / 10,1 / 34,27 / 120,9 ms medidos por la pista E6): si el oscilador de 32 768 Hz
 * del aparato contara un 1 % deprisa, o el contador no arrancara de 0, quedarían cortas. El original espera con el delay() de su
 * sistema y deja 5–8, 16, 40 y 124 ms [medido por E6]. Se deja un margen del mismo orden: +1/16 y +4 ms (1 -> 5, 10 -> 14,
 * 34 -> 40, 120 -> 131, 150 -> 163). Cuesta ~45 ms más por encendido del panel. */
#define PANEL_ESPERA(ms) ((unsigned)(ms) + (unsigned)(ms) / 16u + 4u)

#define GAM  0xF7000000u
#define PDI  0xF7001000u
#define PCN  0xF7001400u
#define TOPE 2000000u

u8 lcd_hay;                                        /* 1 = el bloque gráfico contestó en lcd_init */
static const lcd_panel *panel = &lcd_paneles[3];   /* hasta detectar: el descriptor sin `reg` */
static int panel_r28 = -1;                         /* lo leído del registro 0x0028 (-1 = no se pudo leer) */

/* espera a que no estén ocupados ni el PDICON (+08 bit 0) ni el PDI (+1F4 bit 0); con tope: 0 = libre */
static int pdi_libre(void)
{
    u32 t = TOPE;
    while ((R32(PCN + 0x08) & 1) || (R32(PDI + 0x1F4) & 1)) if (!--t) { lcd_hay = 0; return -1; }
    return 0;
}
static void pdi_byte(u32 off, u8 v)                 /* FUN_441ad8b4: la memoria de programa se escribe byte a byte */
{
    volatile u32 *p = (volatile u32 *)((PDI + off) & ~3u); u32 s = (off & 3) * 8;
    *p = (*p & ~(0xFFu << s)) | ((u32)v << s);
}
static void pdi_cargar(u32 off, const u8 *p, u32 n) { while (n--) pdi_byte(off++, *p++); }
static void pdi_ptr(u32 byte, u8 off)               /* FUN_441ad7bc: punteros de entrada a los programas */
{
    R32(PDI + 0x1FC) = (R32(PDI + 0x1FC) & ~(0xFFu << (8 * byte))) | ((u32)off << (8 * byte));
}
/* n <= 24 bytes alineados al FINAL del área de datos (el último en +0FF), escritos por palabras hacia abajo
 * desde +0FC; solo las palabras que hacen falta, y la primera, si queda a medias, con ceros delante. Es lo que
 * hace el original [observado, traza e23: `00 A4 00 01` de 12 bytes -> +FC = 0x0100A400, +F8 = 0, +F4 = 0].
 * rs = el registro +11C tal cual: un bit por byte, el último byte en el bit 31; 1 = dato, 0 = orden (pdirs = 1). */
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
static void lcd_cursor(u32 x, u32 y)                /* «pdidataarea»: R20 = x, R21 = y, índice R22 = escribir GRAM */
{
    u8 d[10] = { 0x00,0x20, (u8)(x >> 8),(u8)x, 0x00,0x21, (u8)(y >> 8),(u8)y, 0x00,0x22 };
    pdi_area(d, 10, 0x33000000u);                   /* bytes 2,3 y 6,7 son dato */
}
static void pdi_orden(const u8 *b, u32 n, u32 rs)   /* secuencia C de §4.5: una transferencia de n <= 24 bytes */
{
    if (pdi_libre()) return;
    R32(PDI + 0x1F8) &= ~0x00900000u;
    pdi_velocidad(PDI_VEL_WRITE);
    pdi_byte(0x0E, (u8)n); pdi_byte(0x0F, 0);       /* nL nH del programa de escritura */
    pdi_area(b, n, rs);
    R32(PDI + 0x1DC) = 1;
    pdi_libre();
}
/* C.7: deja el PDI listo para el cuadro (se lo cede al PDICON). El área de datos NO se toca aquí: en la traza del
 * original el cursor («pdidataarea») solo se repone al mandar un cuadro (E.5) y al empezar una lectura (D), y entre
 * el final de «init» y el «pre_update» del primer cuadro el área conserva los restos del guion. */
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
/* Un guion de hal/lcd_paneles.h: transferencias y esperas, y al acabar el paso C.7. */
/* (2026-10-25, música en segundo plano) Despertar el panel (normal_from_standby + init + init2) para el bucle ~300 ms, casi todo en estas esperas,
 * y eso basta a veces para vaciar el DSP (su colchón es de ~0,2-0,6 s según cómo esté): la pantalla se apaga en el bolsillo, una tecla la
 * despierta y la música se cortaba [medido: herr/puente/dsp/exp_despertar.py]. Las esperas van a trozos de 8 ms y entre trozo y trozo se
 * deja «respirar» al motor de música (hw_respirar, débil: plat/hw_w580.c). Una espera más larga que la pedida no le hace nada al panel. */
void hw_respirar(void) __attribute__((weak));
static void lcd_esperar(unsigned ms)
{
    while (ms) { unsigned t = ms > 8u ? 8u : ms; espera_ms(t); ms -= t; if (hw_respirar) hw_respirar(); }
}
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
void lcd_reg(u8 reg, u16 v)                         /* un registro suelto: índice de 16 bits + dato de 16 bits, byte alto primero */
{
    u8 b[4] = { 0x00, reg, (u8)(v >> 8), (u8)v };
    pdi_orden(b, 4, 0xC0000000u);
    pdi_fin_ordenes();
}

/* Secuencia D de §4.5 (marika_pdi_read_byte, 0x449D44F4): UN byte de un registro del panel. alto = 1 usa el
 * programa «readhigh», 0 el «read». Como el original: rellena antes el área de datos con pdidataarea, quita los
 * bits 20 y 23 de +1F8, apunta el programa en +1FC byte 2, velocidad de lectura (0x15), índice en +0FE/+0FF,
 * RS a 0, +1DC <- 3, y el byte sale en +1F4 bits 8..15. [observado en la traza e23: +0FC <- 0xA0000000 para 0x00A0].
 * [sin saber] qué hace cada instrucción del microcódigo: se copia tal cual. -1 = el PDI no se libera. */
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
int lcd_leer_reg(u16 reg)                           /* 16 bits: dos lecturas, byte alto y byte bajo. -1 = no se pudo */
{
    int a, b;
    if (!lcd_hay) return -1;
    a = panel_leer_byte(reg, 1);                    /* [inferido] el orden alto, bajo (así lo describe la pista E) */
    b = panel_leer_byte(reg, 0);
    return a < 0 || b < 0 ? -1 : a << 8 | b;
}

/* DETECCIÓN DEL PANEL, como el original [observado, traza e23 de la pista E, líneas 1363..1382]:
 *   1. el guion «swreset» del primer descriptor (8 bytes de orden a 0 + R A4 = 0x0001 en UNA transferencia de 12
 *      bytes; espera 1 ms): «copia la NVMEM del controlador a sus registros» (comentario del fichero);
 *   2. lee el registro 0x00A0 (la «dummy read» del primer descriptor, que no casa con nada);
 *   3. lee el registro 0x0028 y se queda con el primer descriptor cuyo valor sea (R28 & 0x000F):
 *      0x0 «Reg28-00 Original Hitachi», 0xA «Reg28-0A Hitachi LPG», 0xE «Reg28-0E Sharp»;
 *      [inferido] si ninguno casa, el último descriptor, que no trae `reg` («QVGA by Hitachi driver: R61504»).
 * Si la lectura falla (el PDI no se libera) se queda ese mismo último descriptor y panel_r28 = -1.
 * [emulador] el modelo de E devuelve en una lectura lo último escrito en ese registro (0 tras el reset): sale
 * «Reg28-00», igual que con el firmware original. Las otras ramas solo se ejercitan con -DPRUEBA_PANEL_R28=0x…
 * (hace de registro leído; sirve para ver por el registro de QEMU los guiones de cada panel).
 * [sin saber] qué devuelve un panel de verdad; el W580i del mantenedor será uno de los tres. */
static void panel_detectar(void)
{
    int v, i;
    guion(g_swreset);
    (void)lcd_leer_reg(PANEL_REG_SONDA);
    v = lcd_leer_reg(PANEL_REG_ID);
#ifdef PRUEBA_PANEL_R28
    v = PRUEBA_PANEL_R28;
#endif
    panel_r28 = v;
    panel = &lcd_paneles[3];
    if (v < 0) return;
    for (i = 0; i < 4; i++)
        if (lcd_paneles[i].id < 0 || (v & PANEL_REG_MASC) == lcd_paneles[i].id) { panel = &lcd_paneles[i]; return; }
}
const char *lcd_panel_nombre(void) { return panel->nombre; }
int lcd_panel_r28(void) { return panel_r28; }

/* cuadro: lo que se pinta en la memoria del panel ANTES de encenderlo (init_seq = init, cuadro, init2: el
 * original manda un cuadro entre los dos guiones para que al encender no se vea basura). NULL = no mandar nada. */
int lcd_init(const u16 *cuadro)
{
    u32 i, t = TOPE;

    R16(SYSCON + 0x18) &= (u16)~0x0200u;            /* A: XGAM. Puerta 9... */
    R16(SYSCON + 0x12) |= 0x0200;                   /* ...y bloque 7 */
    while ((R32(GAM) & 0xFF00) != 0x4100)           /* versión de GAMCON (el original espera 30 s y sigue) */
        if (!--t) { lcd_hay = 0; return -1; }
    lcd_hay = 1;
    R32(GAM + 0x08) = 0; R32(GAM + 0x08) = 0xFFFFFFFF; R32(GAM + 0x08) = 0xFFFFFF06;   /* relojes de los sub-bloques */
#ifdef SENEW_CAMARA                                 /* (27-oct, depuración) CIRES_N (bit 16) es también el reset del SENSOR de la cámara: se conserva como lo dejó el original para heredar su configuración */
    { u32 c16 = R32(GAM + 0x04) & 0x10000u;
      R32(GAM + 0x04) = 0xF9 | c16; R32(GAM + 0x04) = c16; R32(GAM + 0x04) = 0xF9 | c16; }
#else
    R32(GAM + 0x04) = 0xF9; R32(GAM + 0x04) = 0; R32(GAM + 0x04) = 0xF9;               /* y sus resets */
#endif
    R32(GAM + 0x04) &= ~0x20000u; R32(GAM + 0x04) |= 0x20000u;
    R32(GAM + 0x10) = (R32(GAM + 0x10) & ~0xFFu) | 0x0A; R32(GAM + 0x10) |= 0x100;
    for (i = 0x40C; i <= 0x41C; i += 4) R32(GAM + i) = 0xF;
    R32(PDI + 0x1F8) = (R32(PDI + 0x1F8) & 0xFFFF00F7) | 0x800;
    R32(PCN + 0xFC) = 0x00030180; R32(PCN + 0x100) = 0x10040100;
    R32(PCN + 0x104) = 0x000EA1C5; R32(PCN + 0x108) = 0x166D2800;
    R32(PCN + 0x10C) = 0x00030180; R32(PCN + 0x110) = 0x10040100;
    R32(PCN + 0x114) = 0x000EA1C5; R32(PCN + 0x118) = 0x166D2800;
    R32(PCN + 0x10) = 0x180E;
    R32(GAM + 0x404) = 0x08320881;                  /* bancos de memoria gráfica: 0x20400000, 0x20800000, 0x20C00000 */
    R32(GAM + 0x14) |= 7;
    R32(PCN + 0x1C) &= ~1u; R32(PCN + 0x24) |= 1;

    if (pdi_libre()) return -2;                     /* B: instalar el panel (el microcódigo es el mismo en los cinco descriptores) */
    R32(PDI + 0x1FC) = 0x00FFFFFF;
    pdi_cargar(PGM_WRITE_EN, pgm_write, sizeof pgm_write);
    pdi_cargar(PGM_GC_EN, pgm_gc, sizeof pgm_gc);
    pdi_cargar(PGM_READHIGH_EN, pgm_readhigh, sizeof pgm_readhigh);
    pdi_cargar(PGM_READ_EN, pgm_read, sizeof pgm_read);
    pdi_ptr(0, PGM_GC_EN); pdi_ptr(2, PGM_READ_EN);
    R32(PCN + 0x40) = LCD_ANCHO | LCD_ALTO << 16; R32(PCN + 0x44) = 16;
    R32(PCN + 0x10) &= ~0x40u;                      /* pdinibbleswap = 0 */
    R32(PCN + 0xE4) = 0x130F0E0D; R32(PCN + 0xE8) = 0x17161514;                         /* encaminamiento de bits, 16 bpp */
    R32(PCN + 0xEC) = 0x06050403; R32(PCN + 0xF0) = 0x0C0B0A07;
    R32(PCN + 0xF4) = 0; R32(PCN + 0xF8) = 0;

    panel_detectar();                               /* swreset, lee R00A0 y R0028, elige descriptor */
    guion(panel->init);                             /* init_seq = 00 (init), D0 (un cuadro), 01 (init2) */
    if (cuadro) lcd_enviar(cuadro, 0, LCD_ALTO);
    guion(g_init2);                                 /* encender: R07 = 0x0173 */
    R32(PDI + 0x1E0) |= 0xFFFF;                     /* +1E0 bytes 0 y 1 <- 0xFF: en la traza del original, DESPUÉS de encender. [sin saber] qué es */
    return lcd_hay ? 0 : -3;
}

/* Secuencia E de §4.5. cuadro: RGB565 en el orden natural del ARM (little-endian), 240 px por fila, de arriba
 * abajo, en la SDRAM (el origen que se da al PDICON es dirección - 0x20000000). Manda las filas y0..y0+filas-1.
 * CONTRATO DE LA CACHÉ (pista S4; docs/sistema.md §10.2): el PDICON lee la SDRAM física, no la caché de datos de la
 * CPU, y la memoria gráfica va con caché de escritura diferida. Por eso ESTA función baja a la SDRAM las líneas de
 * las filas que manda (cache_limpiar: c7,c10,1 línea a línea, como FUN_44a2c0f4 del original antes de cada
 * `PDICON+04 <- 2`) justo antes de programar la transferencia. Nadie que llame a lcd_enviar tiene que limpiar, y
 * nadie debe escribir `PDICON+04` por otro camino. Con la caché apagada (los hitos) cache_limpiar no hace nada.
 * Lo vigila el emulador estricto (regla [cache]: 0 transferencias con líneas sucias; herr/probar_w580.py lo apunta
 * en cada pasada). Una fila son 480 bytes = 15 líneas justas si el cuadro está alineado a 32 (los dos lo están). */
volatile u32 lcd_envios;                            /* (1.9) sube con cada envío al PDICON: la cámara (hal/camara_w580.c) no reanuda su DMA hasta que salga la pantalla */
void lcd_enviar(const u16 *cuadro, u32 y0, u32 filas)
{
    static const u8 pre[4] = { 0, 0, 0, 0 };
    if (!lcd_hay || !filas || y0 + filas > LCD_ALTO) return;
    cache_limpiar((u32)(cuadro + y0 * LCD_ANCHO), (u32)(cuadro + (y0 + filas) * LCD_ANCHO));
    pdi_orden(pre, 4, 0);                           /* pre_update: $00 00 00 00$ (g_pre_update) */
    if (pdi_libre()) return;
    R32(PCN + 0x10) &= ~8u;
    /* (S7) +5C LLEVA LA MISMA ESQUINA QUE +48, como el original (pdicon_set_offset, 0x441ADD40: 0x00FA0000 con el rectángulo
     * en y = 250; pista E6). Antes iba a 0. [sin saber] cuál de los dos usa el PDICON para situarse dentro del búfer: si es +5C,
     * con 0 las bandas parciales enseñarían las filas de ARRIBA del búfer. Así queda bien en los dos casos. +60 sigue a 0 (el suyo). */
    R32(PCN + 0x60) = 0; R32(PCN + 0x5C) = y0 << 16;
    R32(PCN + 0x50) = (u32)cuadro - 0x20000000u; R32(PCN + 0x54) = LCD_ANCHO; R32(PCN + 0x58) = 16;
    R32(PCN + 0x10) &= ~0x10u;
    lcd_cursor(0, y0);
    pdi_ptr(0, PGM_GC_EN);
    pdi_velocidad(PDI_VEL_GC);
    R32(PDI + 0x1F8) |= 0x00100000u; R32(PDI + 0x1F8) &= ~0x00800000u;                   /* el PDI, otra vez para el PDICON */
    R32(PCN + 0x48) = y0 << 16;
    R32(PCN + 0x4C) = LCD_ANCHO | filas << 16;
    R32(PCN + 0x04) = 2;
    lcd_envios++;                                   /* (1.9; revisión 4) DESPUÉS de arrancarlo: la cámara ve el PDICON ocupado o el contador nuevo, nunca ninguno */
}

/* (estética, 26-oct) EL CUADRO GIRADO POR EL PDICON, como el visor del original en «Landscape» (pdicon_set_rotation = +60,
 * pdicon_set_offset = +5C; work/export/c/449D.c): el cuadro lógico (paso píxeles por fila, 16 bpp) se manda ENTERO al panel y el
 * PDICON lo lee girado empezando en la esquina dada. Medido en el aparato (docs/estetica.md §7): código 5 + (0, 239) y código 6 +
 * (319, 0) con un cuadro de 320x240; 0 CPU. El cursor del panel va a (0, 0) y el rectángulo es el panel entero. */
void lcd_enviar_girado(const u16 *cuadro, u32 paso, u32 codigo, u32 esquina)
{
    static const u8 pre[4] = { 0, 0, 0, 0 };
    if (!lcd_hay) return;
    cache_limpiar((u32)cuadro, (u32)(cuadro + LCD_ANCHO * LCD_ALTO));
    pdi_orden(pre, 4, 0);
    if (pdi_libre()) return;
    R32(PCN + 0x10) &= ~8u;
    R32(PCN + 0x60) = codigo; R32(PCN + 0x5C) = esquina;
    R32(PCN + 0x50) = (u32)cuadro - 0x20000000u; R32(PCN + 0x54) = paso; R32(PCN + 0x58) = 16;
    R32(PCN + 0x10) &= ~0x10u;
    lcd_cursor(0, 0);
    pdi_ptr(0, PGM_GC_EN);
    pdi_velocidad(PDI_VEL_GC);
    R32(PDI + 0x1F8) |= 0x00100000u; R32(PDI + 0x1F8) &= ~0x00800000u;
    R32(PCN + 0x48) = 0;
    R32(PCN + 0x4C) = LCD_ANCHO | (u32)LCD_ALTO << 16;
    R32(PCN + 0x04) = 2;
    lcd_envios++;                                   /* (1.9; revisión 4) DESPUÉS de arrancarlo: la cámara ve el PDICON ocupado o el contador nuevo, nunca ninguno */
}

/* Dormir, despertar y apagar el PANEL (no la luz) con los guiones del descriptor (hal.md §4.1):
 *   standby_from_normal_seq = 0E                 -> el guion standby_from_normal. OJO: su final (`%w1E $00 10$ 00 00`)
 *       NO se manda: en la traza e23 el original se para en la espera de 30 ms (no hay `%d` detrás del `%w`), así
 *       que el panel dormido queda con R10 = 0x0008, no 0x0000. La versión anterior de este fichero sí lo mandaba;
 *   normal_from_standby_seq = 0B 00 D0 01        -> normal_from_standby (todo DATOS), init, un cuadro, init2;
 *   shutdown_seq = 09                            -> shutdown (R10 = 0x0004), al apagar el teléfono.
 * [emulador: las órdenes llegan al registro y el modelo guarda R07/R10]; en el aparato, sin probar. */
void lcd_dormir(void)
{
    if (!lcd_hay) return;
    guion(g_standby_from_normal);
}
void lcd_despertar(const u16 *cuadro)
{
    if (!lcd_hay) return;
    guion(g_normal_from_standby);
    guion(panel->init);
    if (cuadro) lcd_enviar(cuadro, 0, LCD_ALTO);
    guion(g_init2);
}
void lcd_apagar(void)
{
    if (!lcd_hay) return;
    guion(g_shutdown);
}
