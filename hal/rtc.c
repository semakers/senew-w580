/* RTC (0xF9050000): un calendario en binario, un campo por registro. docs/hal.md §10: IGUAL que el del DB2010
 * campo a campo; cambian la puerta de reloj (27) y la línea de interrupción (36, era la 43).
 * Escrito a partir de esa sección (lectura doble coherente; puesta en hora con la fracción al final). */
#include "hal.h"

#define RTC 0xF9050000u
/* (I1) Todo acceso al bloque del RTC pasa por RTC_REG: en el PC (herr/pruebas/prueba_rtc.c, -DRTC_PRUEBA) es un modelo con
 * el tiempo corriendo y el acarreo propagándose campo a campo; en el teléfono, el registro. */
#ifdef RTC_PRUEBA
volatile u16 *rtc_prueba_reg(u32 off);
#define RTC_REG(o) (*rtc_prueba_reg(o))
#else
#define RTC_REG(o) R16(RTC + (o))
#endif

void rtc_init(void)
{
    R16(SYSCON + 0x16) &= (u16)~0x0080u;                  /* puerta 27 */
    if ((RTC_REG(0x58) & 0x8C) != 0x8C)                 /* oscilador parado: lo arranca (la hora no será fiable) */
        RTC_REG(0x58) = (u16)((RTC_REG(0x58) & 0x7F) | 0x8C);
}

static void leer(rtc_t *t)
{
    t->anio = (u16)((RTC_REG(0x12) & 0xFF) * 100 + (RTC_REG(0x10) & 0xFF));
    t->mes  = (u8)RTC_REG(0x0E); t->dia = (u8)RTC_REG(0x0C);
    t->hora = (u8)RTC_REG(0x08); t->min = (u8)RTC_REG(0x06);
    t->seg  = (u8)RTC_REG(0x04);
}

/* (R3; AUD3-M6) LA LECTURA COHERENTE, COMO EL ORIGINAL (441AC548 [observado con tools/e_dis.py]):
 *  1. esperar mientras se esté en el último instante del segundo —+14 bit 2 a 1, la fracción +02 en 0x7F y los bits 4..7 de
 *     +00 a 1 (441AC554..56C)—, para no leer a caballo del vuelco;
 *  2. leer los campos, ESPERAR retardo(500) (441AC5A4 `movs r0,#0xfa; lsls r0,#1; bl 0x44233818`) y releer; repetir si algo
 *     cambió. Antes aquí se leía dos veces SEGUIDAS: si el contador propaga el acarreo campo a campo [inferido: es la única
 *     explicación de ese retardo], las dos lecturas podían salir iguales y rotas (una hora equivocada en un cambio de minuto).
 * Con tope: si el RTC no tuviera reloj, la espera no se queda para siempre.
 *
 * (I1) EL CAMINO CORTO, por la batería. La espera de 500 (~0,25 ms de vueltas, ~51 000 instrucciones) en CADA lectura subió
 * «un segundo de standby» de ~45 k a ~147 k instrucciones (R3, §17.2). Pero el acarreo solo se propaga en el VUELCO del
 * segundo (la fracción +02 pasa de 0x7F a 0): si la fracción, leída ANTES y DESPUÉS de los campos, está en 2..0x7F y no ha
 * dado la vuelta (la segunda >= la primera), entre las dos no hubo vuelco y el anterior fue hace >= 2/128 s (15,6 ms, sesenta
 * veces el retardo del original): no puede quedar acarreo a medias [inferido de la misma razón que justifica el retardo; la
 * duración real de la propagación, sin saber]. Solo si la fracción está en 0 o 1 (recién volcado), o volcó entre las dos
 * lecturas, se toma el camino del original, entero. El emulador y el modelo del PC dan la fracción así (+02 = 1/128 s).
 * Prueba: herr/pruebas/prueba_rtc.c (make probar-rtc), lecturas sembradas en todos los vuelcos, con su negativa. */
#define FRAC() (RTC_REG(0x02) & 0x7F)
#ifndef FRAC_SALTO                                        /* (AUD7 B1) fracciones que pueden pasar entre la primera y la última lectura del camino corto */
#define FRAC_SALTO 2u                                     /* (la negativa de la prueba de las pausas lo compila a 128: sin tope) */
#endif
#ifndef FRAC_LEJOS                                        /* (la negativa de la prueba lo compila a 0) */
#define FRAC_LEJOS 2u                                     /* fracciones 0 y 1: recién volcado, camino largo */
#endif
u32 rtc_lecturas_largas;                                  /* veces que se tomó el camino del original (diagnóstico y pruebas) */
static void esperar_vuelco(void)
{
    u32 tope = 100000;
    while ((RTC_REG(0x14) & 4) && (RTC_REG(0x02) & 0xFF) == 0x7F && (RTC_REG(0x00) & 0xF0) == 0xF0 && --tope) { }
}
static int iguales(const rtc_t *a, const rtc_t *b)
{
    return a->seg == b->seg && a->min == b->min && a->hora == b->hora && a->dia == b->dia && a->mes == b->mes && a->anio == b->anio;
}
int rtc_get(rtc_t *t)                                    /* 0 = el chip dice que la hora es válida */
{
    rtc_t b;
    int tope = 8;
    u32 f1 = FRAC(), f2;
    leer(t);
#ifdef RTC_UNA_LECTURA                                    /* (solo la negativa de la prueba: el camino corto de I1 antes de AUD7) */
    b = *t;
#else
    leer(&b);
#endif
    f2 = FRAC();
    /* (AUD7 B1) y además: las dos lecturas de los campos, IGUALES, y entre las dos fracciones, como mucho FRAC_SALTO: si algo para
     * esta función ~1 s entre medias (una interrupción larga), la fracción puede dar la vuelta entera y volver a quedar >= f1 con
     * un vuelco dentro; o leer() puede quedar partida en dos segundos distintos (los primeros campos de uno, los últimos de otro).
     * Con las dos lecturas iguales y el salto acotado, eso exige DOS paradas de casi un segundo exacto: se toma el camino largo. */
    if (!(f1 >= FRAC_LEJOS && f2 >= f1 && f2 - f1 <= FRAC_SALTO && iguales(t, &b))) {   /* cerca del vuelco o a caballo: como el original */
        rtc_lecturas_largas++;
        do { esperar_vuelco(); leer(t); hal_retardo(500); leer(&b); }
        while (--tope && !iguales(t, &b));
    }
    return (RTC_REG(0x1E) & 0xC0) == 0xC0 ? 0 : -1;
}
void rtc_set(const rtc_t *t)
{
    u32 cerradas = irq_cerrar();
    rtc_init();
    esperar_vuelco();                                     /* no escribir sobre el vuelco (FUN_441ac628) */
    RTC_REG(0x10) = (u16)(t->anio % 100); RTC_REG(0x12) = (u16)(t->anio / 100);
    RTC_REG(0x0E) = t->mes;  RTC_REG(0x0C) = t->dia;
    RTC_REG(0x08) = t->hora; RTC_REG(0x06) = t->min;
    RTC_REG(0x04) = t->seg;
    RTC_REG(0x02) = 0;                                  /* la fracción, lo último: consolida */
    if (!cerradas) irq_abrir();
}

/* Alarma 1 del chip (diaria): +2A/+2C/+2E = segundo, minuto y hora; +16 bit 5 = activa; +18 bit 2 = permiso
 * de IRQ; +1C bit 2 = saltó (leerlo lo limpia [inferido, así el emulador]); IRQ 36. hal.md §10. */
volatile u32 rtc_alarmas;                              /* veces que ha saltado (lo sube la IRQ) */
static void rtc_irq(void) { if (RTC_REG(0x1C) & 0x04) rtc_alarmas++; }
void rtc_alarma(int hora, int min)                     /* hora < 0 = quitarla */
{
    u32 cerradas = irq_cerrar();
    RTC_REG(0x18) = (u16)(RTC_REG(0x18) & ~0x04u);
    RTC_REG(0x16) = (u16)(RTC_REG(0x16) & ~0x20u);
    if (hora >= 0) {
        RTC_REG(0x2A) = 0; RTC_REG(0x2C) = (u16)min; RTC_REG(0x2E) = (u16)hora;
        (void)RTC_REG(0x1C);                         /* limpia lo pendiente */
        RTC_REG(0x16) = (u16)(RTC_REG(0x16) | 0x20);
        RTC_REG(0x18) = (u16)(RTC_REG(0x18) | 0x04);
        intc_habilita(36, rtc_irq);
    }
    if (!cerradas) irq_abrir();
}

/* Fracción de hora en 1/128 s, leída con coherencia (+06 minutos, +04 segundos, +02 fracción de 1/128 s).
 * Sirve para medir cuánto ha durado un reposo profundo sin depender del tic (que está parado). */
u32 rtc_128(void)
{
    u32 m, s, f, s2, m2, f2, tope = 8;
    /* (I1) el mismo camino corto que rtc_get: fracción, campos dos veces, fracción; si no hubo vuelco ni se estaba recién volcado,
     * las dos lecturas son iguales y entre las fracciones pasó poco (AUD7 B1), vale */
    f = RTC_REG(0x02) & 0x7F; m = RTC_REG(0x06) & 0xFF; s = RTC_REG(0x04) & 0xFF; m2 = m; s2 = s;
#ifndef RTC_UNA_LECTURA
    m2 = RTC_REG(0x06) & 0xFF; s2 = RTC_REG(0x04) & 0xFF;
#endif
    f2 = RTC_REG(0x02) & 0x7F;
    if (f >= FRAC_LEJOS && f2 >= f && f2 - f <= FRAC_SALTO && m == m2 && s == s2) return (m * 60 + s) * 128 + f;
    rtc_lecturas_largas++;
    /* (R3; AUD3-M6) como rtc_get: fuera del vuelco, y minuto y segundo releídos tras el retardo (la fracción es la primera) */
    do { esperar_vuelco(); m = RTC_REG(0x06) & 0xFF; s = RTC_REG(0x04) & 0xFF; f = RTC_REG(0x02) & 0x7F; hal_retardo(500); m2 = RTC_REG(0x06) & 0xFF; s2 = RTC_REG(0x04) & 0xFF; } while ((s != s2 || m != m2) && --tope);
    return (m * 60 + s) * 128 + f;
}
