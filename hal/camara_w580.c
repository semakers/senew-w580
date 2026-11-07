/* hal/camara_w580.c — LA CÁMARA del W580i (sensor OmniVision OV2640 por I²C 1 + CDI/CDICON de XGAM). 27-oct-2026, sesión «drivers: micrófono y cámara».
 * Todo lo de aquí está MEDIDO en el aparato con el firmware ORIGINAL (docs/reconocimiento_original.md §5; cada paso lleva su marca):
 *  - ENCENDIDO (camdriver0.dat cam_power_on_0..7, y ejecutado por el original en el emulador con puntos de ruptura): reset activo → P3.5 = 1 (alimentación) → P0.7 entrada y
 *    salida a 0 (PWDN, 5 ms) → reloj del sensor SYSCON+06 = 0x0200 (13 MHz) → reset SUELTO. **GAMCON+04 bit 16 (CIRES_N) es TAMBIÉN el reset del sensor**: a 0, el sensor ni
 *    contesta por I²C ni conserva su configuración [observado].
 *  - XGAM: el módulo 6 (CDI + CDICON) hay que habilitarlo (GAMCON+08 &= ~6; GAMCON+04 |= 6, y un pulso): sin eso CDI/CDICON leen 0 [observado]; los identificadores son 0x43xx / 0x44xx.
 *  - El sensor se programa con la TABLA del guion del original (hal/camara_w580_tab.c, generada por herr/camara/gen_tabla_c.py): rama «Premier», REV2B, 176×144, YUV 4:2:2 (Y1 U Y2 V),
 *    7 fps. Incluye la ventana de entrada que escribe un gancho de la biblioteca del original (50..54 = 9B 70 2C 44 0C) y D3 = 04 después del reset del DVP: sin ellas CDI no recibe nada.
 *  - CDI/CDICON con los valores del visor del original; el cuadro llega por DMA a DOS búferes (doble búfer) de 352×144 B; CDI+0C es un contador de bytes y CDI+04 bit 14 = «cuadro completo».
 * API: cam_iniciar() enciende, configura y deja el flujo corriendo; cam_capturar() espera un cuadro completo y PARA el flujo para que los dos búferes queden quietos (cam_yuyv());
 * cam_a_rgb565() lo convierte; cam_apagar() lo apaga todo. NO se usa desde interrupciones. [sin saber] la corrección de color, el balance de blancos de otros módulos (Flex),
 * REV1A del sensor (solo se admite la 0x42 = REV2B, la del aparato). */
#ifdef TELEFONO                                    /* solo en el firmware del teléfono: los hitos del emulador no enlazan plat/tel/perro.c */
#include "hal.h"
#include "camara_w580.h"

#define SYSCON_ 0xF9090000u
#define GAM_    0xF7000000u
#define CDI_    0xF7000800u
#define CDICON_ 0xF7000C00u
#define GRAM0   0x20000000u
#define CAM_ANCHO 176
#define CAM_ALTO  144
#define CAM_BYTES (CAM_ANCHO * CAM_ALTO * 2u)           /* 50 688 */

extern const u8 cam_tabla[];
extern const u32 cam_tabla_n;
void hw_perro_latido(void);
u32 hw_ms(void);

/* los dos búferes de cuadro y el «cubo raw» del CDICON, en la GRAM (el CDICON direcciona 24 bits = offsets desde 0x20000000; la GRAM está por debajo de 0x21000000) */
static u8 cam_b0[CAM_BYTES + 64] __attribute__((section(".gram"), aligned(64)));
static u8 cam_b1[CAM_BYTES + 64] __attribute__((section(".gram"), aligned(64)));
static u8 cam_raw[10000] __attribute__((section(".gram"), aligned(64)));
static u8 cam_on;
static u32 cam_t_marcha;                                /* hw_ms() cuando arrancó el flujo */
#define CAM_ASENTAR_MS 1200u                              /* los primeros cuadros salen oscuros: la exposición automática del sensor tarda ~1 s en converger (a 7 fps) [observado 27-oct] */

/* ARM926: limpiar e invalidar la caché de datos de un rango (el DMA del CDICON escribe por detrás de la caché) */
__attribute__((target("arm"), noinline)) static void cache_inv(const void *p, u32 bytes)
{
    u32 a = (u32)p & ~31u, fin = (u32)p + bytes;
    for (; a < fin; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

static int sccb_w(u8 reg, u8 val) { u8 b[2]; b[0] = reg; b[1] = val; return i2c_escribir(1, 0x30, b, 2); }
static int sccb_r(u8 reg, u8 *v) { return i2c_escribir_leer(1, 0x30, &reg, 1, v, 1); }

static int aplicar_tabla(void)
{
    u32 i, fallos = 0;
    if (!cam_tabla_n) return 1;                            /* sin la tabla del sensor (senew público: hal/fabricante_ausente.c, «make camara-tabla») */
    for (i = 0; i < cam_tabla_n; i++) {
        u8 t = cam_tabla[3 * i], x = cam_tabla[3 * i + 1], y = cam_tabla[3 * i + 2];
        if (t == 0) { if (sccb_w(x, y)) fallos++; }
        else {
            u32 ms = (u32)x | ((u32)y << 8);
            while (ms) { u32 q = ms > 20u ? 20u : ms; espera_ms(q); ms -= q; hw_perro_latido(); }
        }
        if ((i & 15) == 15) hw_perro_latido();
    }
    return (int)fallos;
}

void cam_apagar(void)
{
    cam_vigilar(0);                                        /* (1.9) el tic deja de mirar ANTES de tocar CDI/CDICON */
    R32(CDI_ + 0x04) &= ~1u;                               /* CDI y CDICON parados */
    R32(CDICON_ + 0x04) &= ~0x10u;
    gpio_set(3, 0x20, 0); espera_ms(10);                   /* cam_power_off_0: P3.5 = 0 y 10 ms */
    R16(SYSCON_ + 0x06) = 0x0709;                          /* reloj del sensor fuera (valor de reposo medido) */
    R32(GAM_ + 0x04) &= ~0x10000u;                         /* reset activo */
    gpio_dir(0, 0x80, 1); gpio_set(0, 0x80, 1);            /* PWDN activo = cámara dormida, como deja el GPIO de arranque */
    cam_on = 0;
}

/* 0 = bien; -1 el sensor no contesta; -2 no es el OV2640 REV2B; -3 la tabla falló; -4 CDI/CDICON no responden */
int cam_iniciar(void)
{
    u8 v = 0, v2 = 0;
    int r;
    if (cam_on) cam_apagar();
    R32(GAM_ + 0x04) &= ~0x10000u;                         /* camera_reset(1): reset ACTIVO (y el del sensor) */
    gpio_dir(3, 0x20, 1); gpio_set(3, 0x20, 1);            /* P3.5 = 1: las tres alimentaciones */
    gpio_dir(0, 0x80, 1); gpio_set(0, 0x80, 0);            /* P0.7 = 0: PWDN activo bajo (el original lo pasa por entrada y lectura: solo elige la rama del guion, que es la Premier) */
    espera_ms(5);
    R16(SYSCON_ + 0x06) = 0x0200;                          /* reloj del sensor a 13 MHz */
    R32(GAM_ + 0x04) |= 0x10000u;                          /* camera_reset(0): reset SUELTO */
    espera_ms(10);
    if (sccb_w(0xFF, 0x01) || sccb_r(0x0A, &v) || v != 0x26) { cam_apagar(); return -1; }       /* ping del guion: banco 1, registro 0x0A = 0x26 */
    if (sccb_r(0x0B, &v2) || v2 != 0x42) { cam_apagar(); return -2; }                           /* REV2B */
    r = aplicar_tabla();
    if (r) { cam_apagar(); return -3; }
    /* XGAM: puertas 22 y 23 (+1A bits 8 y 7) y el bloque de CDI/CDICON fuera de reset (+14 bit 7) */
    R16(SYSCON_ + 0x1A) = (u16)(R16(SYSCON_ + 0x1A) & ~0x0180u);
    R16(SYSCON_ + 0x14) = (u16)(R16(SYSCON_ + 0x14) & ~0x0080u); R16(SYSCON_ + 0x14) = (u16)(R16(SYSCON_ + 0x14) | 0x0080u);
    /* módulo 6 de XGAM: FUN_441ae8dc(6,1) + FUN_441ae918(6,0) */
    R32(GAM_ + 0x08) &= ~6u; R32(GAM_ + 0x04) |= 6u; R32(GAM_ + 0x04) &= ~6u; R32(GAM_ + 0x04) |= 6u;
    if (((R32(CDI_) >> 8) & 0xFF) != 0x43 || ((R32(CDICON_) >> 8) & 0xFF) != 0x44) { cam_apagar(); return -4; }
    R32(CDI_ + 0x04) &= ~1u; R32(CDICON_ + 0x04) &= ~0x10u;
    R32(CDICON_ + 0x04) = 0x507;                           /* cdicon_set_yuv2 */
    R32(CDICON_ + 0x24) = (u32)cam_b0 - GRAM0; R32(CDICON_ + 0x30) = (u32)cam_b1 - GRAM0;
    R32(CDICON_ + 0x18) = (u32)cam_raw - GRAM0; R32(CDICON_ + 0x1C) = (u32)cam_raw - GRAM0 + sizeof cam_raw - 4u;
    R32(CDICON_ + 0x48) = CAM_ANCHO; R32(CDICON_ + 0x4C) = CAM_ALTO;
    { u32 i; for (i = 0x50; i <= 0x5C; i += 4) R32(CDICON_ + i) = 0; R32(CDICON_ + 0x64) = 0; R32(CDICON_ + 0x68) = 0; }
    R32(CDICON_ + 0x04) &= ~0x220u;
    { u32 q = R32(CDI_ + 0x04); q = (5u << 1) | (q & ~0x1Eu); q &= ~0x40u; q &= ~0x1F00u; R32(CDI_ + 0x04) = q; }   /* cdi_config(modo 5, 0xE4, …) */
    R32(CDI_ + 0x18) = 0xE4; R32(CDI_ + 0x10) = CAM_ANCHO << 1; R32(CDI_ + 0x14) = CAM_ALTO; R32(CDI_ + 0x08) = CAM_BYTES;
    R32(CDI_ + 0x28) = 0x7C; R32(CDI_ + 0x30) = 0x08;
    R32(CDICON_ + 0x04) |= 0x10u;                          /* CDICON en marcha */
    R32(CDI_ + 0x04) |= 1u;                                /* CDI en marcha: el flujo es continuo */
    cam_on = 1;
    cam_t_marcha = hw_ms();
    return 0;
}

/* Espera un cuadro completo (CDI+04 bit 14) hasta tope_ms y PARA el flujo: los dos búferes quedan quietos y completos (uno es el último cuadro, el otro el anterior).
 * 0 = bien; -1 no está encendida; -2 no llegó ningún cuadro. */
int cam_capturar(u32 tope_ms)
{
    u32 t0;
    if (!cam_on) return -1;
    while ((u32)(hw_ms() - cam_t_marcha) < CAM_ASENTAR_MS) hw_perro_latido();   /* dejar correr el flujo hasta que la exposición se asiente (y se llenen los dos búferes) */
    t0 = hw_ms();
    /* CDI+04 bit 14 vale 1 MIENTRAS ENTRA un cuadro y BAJA unos ms cuando se completó (CDI+0C = 50 688) [observado 27-oct, 60 lecturas cada ~2 ms tras reanudar el flujo]. Se espera ese
     * flanco de bajada: así el flujo se para con un cuadro NUEVO y entero (antes se esperaba el 1 y se paraba a mitad de cuadro: un búfer entero y el otro a medias). */
    while (!(R32(CDI_ + 0x04) & 0x4000u)) {                      /* 1.º: que EMPIECE un cuadro (tras reanudar el bit sigue bajo hasta que llega el siguiente: si no, se tomaría el cuadro viejo) */
        if ((u32)(hw_ms() - t0) > tope_ms) return -2;
        hw_perro_latido();
    }
    while (R32(CDI_ + 0x04) & 0x4000u) {                          /* 2.º: que se COMPLETE (flanco de bajada) */
        if ((u32)(hw_ms() - t0) > tope_ms) return -2;
        hw_perro_latido();
    }
    R32(CDICON_ + 0x04) &= ~0x10u; R32(CDI_ + 0x04) &= ~1u;     /* parado justo al completarse un cuadro */
    cache_inv(cam_b0, CAM_BYTES); cache_inv(cam_b1, CAM_BYTES);
    return 0;
}

/* vuelve a poner el flujo en marcha tras cam_capturar */
void cam_seguir(void) { if (cam_on) { R32(CDICON_ + 0x04) |= 0x10u; R32(CDI_ + 0x04) |= 1u; cam_t_marcha = hw_ms() - CAM_ASENTAR_MS; } }

/* (1.9, 27-oct) EL CUADRO SIN ESPERAR, para la API de las apps (shell/camara_sis.c). cam_capturar espera el flanco con un bucle (bloquea hasta ~0,3 s);
 * aquí lo vigila el TIC (hal/timer.c, tic_camara, cada 4 ms): CDI+04 bit 14 está a 1 mientras entra un cuadro y a 0 solo 8-18 ms al completarse
 * [observado 27-oct], así que desde el bucle (o desde la app, cada 40-100 ms) se perdería; el tic lo ve.
 * SERIALIZADO CON LA PANTALLA, como el original (su visor va a ~280 ms por cuadro [observado, cronología del visor]): el DMA de la cámara (CDICON) y el que
 * manda la pantalla (PDICON) son maestros de XGAM y, si coinciden, el CDICON PIERDE RÁFAGAS de 32 bytes: el cuadro sale en franjas desplazadas [observado
 * 27-oct: con el doble búfer alterno (7 c/s) 16 de 16 fotos rotas; con el visor sin pintar, 0 de 6; GAM+40C..41C no lo cambian]. Así que:
 *   FLUYE  -> en el flanco de bajada se PARA el flujo: el cuadro entero queda en el búfer 0 (LISTO), SALVO que la pantalla se mandara MIENTRAS entraba
 *             (lcd_envios cambió desde que se reanudó, o el PDICON está ocupado): ese cuadro puede estar roto y se DESCARTA (ESPERA otra vez). Hace falta
 *             porque la pantalla se manda también a mitad de cuadro (el destello de la foto, el reloj, un aviso) [observado 27-oct, build/cam5: 3 de 5 fotos
 *             rotas sin esto];
 *   LISTO  -> el bucle lo toma (cam_tomar) y lo suelta (cam_soltar) -> ESPERA;
 *   ESPERA -> se REANUDA cuando el PDICON ha mandado la pantalla que pintó ese cuadro y está libre (lcd_envios cambió y PDICON+08 bit 0 = 0 dos tics
 *             seguidos), o a los 60 ms si nadie mandó pantalla (una foto, el visor tapado). */
#define PDICON_ 0xF7001400u
extern volatile u32 lcd_envios;                                     /* hal/lcd.c: sube con cada envío de pantalla al PDICON */
enum { CF_NADA, CF_FLUYE, CF_LISTO, CF_ESPERA };
static volatile u8 cam_fase, cam_prev, cam_libres;
static volatile u32 cam_n_cuadros, cam_envios0, cam_t_soltar, cam_env_flujo;
/* (1.9) medidas para el puente: [0] cuadros enteros, [1] reanudaciones por plazo (sin pantalla), [2] Σ ms entre dos cuadros seguidos,
 * [3] cuántos de esos, [4] el máximo entre dos, [5] cuadros cuyo contador de bytes (CDI+0C) al completarse NO era 50 688; [6] cuadros DESCARTADOS porque la
 * pantalla se mandó mientras entraban */
volatile u32 cam_med[7];
static u32 cam_t_ult;
static void flujo(int si)
{
    if (si) { cam_env_flujo = lcd_envios; R32(CDICON_ + 0x04) |= 0x10u; R32(CDI_ + 0x04) |= 1u; }
    else { R32(CDICON_ + 0x04) &= ~0x10u; R32(CDI_ + 0x04) &= ~1u; }
}
void cam_tic(void)
{
    u32 t;
    if (!cam_on) return;
    if (cam_fase == CF_FLUYE) {
        if (R32(CDI_ + 0x04) & 0x4000u) { cam_prev = 1; return; } /* entrando */
        if (!cam_prev) return;                                      /* aún no empezó el primero tras reanudar */
        flujo(0);                                                   /* flanco de bajada: parado justo al completarse */
        cam_prev = 0; cam_n_cuadros++; cam_med[0]++;
        { u32 b = R32(CDI_ + 0x0C); if (b != CAM_BYTES) cam_med[5]++; }
        if (lcd_envios != cam_env_flujo || (R32(PDICON_ + 0x08) & 1u)) {   /* la pantalla salió mientras entraba: puede venir roto */
            cam_med[6]++;
            cam_envios0 = lcd_envios - 1u; cam_t_soltar = hw_ms(); cam_libres = 0; cam_fase = CF_ESPERA;   /* (envios0 distinto: reanuda en cuanto el PDICON esté libre) */
            return;
        }
        t = hw_ms();
        if (cam_t_ult) { u32 d = t - cam_t_ult; cam_med[2] += d; cam_med[3]++; if (d > cam_med[4]) cam_med[4] = d; }
        cam_t_ult = t;
        cam_fase = CF_LISTO;
    } else if (cam_fase == CF_ESPERA) {
        int pdi_libre = !(R32(PDICON_ + 0x08) & 1u);
        static u32 env_visto;
        if (lcd_envios != env_visto) { env_visto = lcd_envios; cam_libres = 0; }   /* (revisión 4) los tics libres cuentan desde el último envío */
        cam_libres = pdi_libre ? (u8)(cam_libres < 255 ? cam_libres + 1 : 255) : 0;
        if (cam_libres >= 2 && lcd_envios != cam_envios0) { cam_fase = CF_FLUYE; cam_prev = 0; flujo(1); }
        else if (pdi_libre && (u32)(hw_ms() - cam_t_soltar) >= 60u) { cam_fase = CF_FLUYE; cam_prev = 0; cam_med[1]++; flujo(1); }
    }
}
void cam_vigilar(int si)                                            /* 1 = el tic vigila (llamar con el flujo en marcha); 0 = deja de hacerlo (antes de cam_apagar) */
{
    u32 i = irq_cerrar();                                           /* el bit I de antes: si ya estaban cortadas, se quedan así */
    cam_fase = si ? CF_FLUYE : CF_NADA; cam_prev = 0;
    if (cam_on) { flujo(0); R32(CDICON_ + 0x24) = (u32)cam_b0 - GRAM0; R32(CDICON_ + 0x30) = (u32)cam_b1 - GRAM0; flujo(1); }   /* empieza en el búfer 0 */
    if (si) { u32 k; for (k = 0; k < 7; k++) cam_med[k] = 0; cam_t_ult = 0; }
    if (!i) irq_abrir();
}
int cam_listo(void) { return cam_fase == CF_LISTO; }
u32 cam_cuadros(void) { return cam_n_cuadros; }
int cam_asentada(void) { return cam_on && (u32)(hw_ms() - cam_t_marcha) >= CAM_ASENTAR_MS; }
const u8 *cam_tomar(void)                                           /* el cuadro parado (caché invalidada), o NULL si aún no hay. Hasta cam_soltar el flujo sigue parado */
{
    if (cam_fase != CF_LISTO) return 0;
    cache_inv(cam_b0, CAM_BYTES);
    return cam_b0;
}
void cam_soltar(void)                                               /* tras cam_tomar: el tic reanuda en cuanto la pantalla que lo enseña haya salido */
{
    u32 i = irq_cerrar();
    if (cam_fase == CF_LISTO) { cam_envios0 = lcd_envios; cam_t_soltar = hw_ms(); cam_libres = 0; cam_fase = CF_ESPERA; }
    if (!i) irq_abrir();
}

const u8 *cam_yuyv(int cual) { return cual ? cam_b1 : cam_b0; }
int cam_ancho(void) { return CAM_ANCHO; }
int cam_alto(void) { return CAM_ALTO; }

/* YUV 4:2:2 empaquetado (Y1 U Y2 V) → RGB565, entero (BT.601): out de CAM_ANCHO × CAM_ALTO u16 */
void cam_a_rgb565(const u8 *yuyv, u16 *out)
{
    u32 n = (u32)CAM_ANCHO * CAM_ALTO / 2u, i;
    for (i = 0; i < n; i++) {
        int y1 = yuyv[4 * i], u = yuyv[4 * i + 1] - 128, y2 = yuyv[4 * i + 2], v = yuyv[4 * i + 3] - 128;
        int rr = (359 * v) >> 8, gg = (88 * u + 183 * v) >> 8, bb = (454 * u) >> 8, k;
        for (k = 0; k < 2; k++) {
            int y = k ? y2 : y1, r = y + rr, g = y - gg, b = y + bb;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            out[2 * i + k] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

/* para el puente (pu.py llamar): enlazadas aunque nadie más las use (el enlazador quita lo no referenciado) */
__attribute__((used)) void *const cam_ref[] = { (void *)cam_iniciar, (void *)cam_capturar, (void *)cam_seguir, (void *)cam_a_rgb565, (void *)cam_apagar, (void *)cam_yuyv,
                                               (void *)cam_tic, (void *)cam_vigilar, (void *)cam_tomar, (void *)cam_soltar };
#endif /* TELEFONO */
