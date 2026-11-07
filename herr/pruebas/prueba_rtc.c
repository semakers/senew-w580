/* prueba_rtc.c — (I1) hal/rtc.c DE VERDAD (compilado con -DRTC_PRUEBA) contra un MODELO del RTC en el PC. make probar-rtc.
 *
 * EL MODELO: el tiempo corre (ns) con cada acceso al bloque (T_ACC) y con hal_retardo (n x 500 ns: retardo(500) = 0,25 ms,
 * lo de docs/sistema.md §17). La fracción +02 (1/128 s) y +00 (los 8 bits bajos de 32 kHz) cambian AL INSTANTE; los campos
 * del calendario PROPAGAN EL ACARREO uno tras otro: el segundo se ve P ns después del vuelco, el minuto 2P, la hora 3P, el
 * día 4P, el mes 5P y el año 6P (P = 20 µs: el acarreo entero tarda 120 µs, menos que el retardo de 250 µs del original
 * — es la hipótesis que justifica ese retardo [inferido, R3]). Es lo peor que admite la hipótesis: entre dos lecturas
 * SEGUIDAS a mitad del acarreo salen dos copias iguales y rotas.
 *
 * LO QUE SE COMPRUEBA:
 *  vuelcos   cada fin de minuto, hora, día, mes (28-feb normal y bisiesto) y año, empezando a leer en todos los instantes de
 *            -3 ms a +20 ms del vuelco cada 0,73 µs: rtc_get devuelve SIEMPRE una hora que el reloj marcó de verdad (la
 *            de antes del vuelco o la de después), nunca una rota; y rtc_128 igual (minuto, segundo y fracción de un mismo
 *            instante). Cuenta cuántas veces tomó el camino largo.
 *  lejos     10 000 lecturas en instantes al azar fuera de los 2/128 s tras un vuelco: NINGUNA toma el camino largo, ninguna
 *            llama a hal_retardo (la cifra de batería), y todas valen.
 *  parado    el oscilador parado con la fracción clavada en 0 (o en 0x7F con el reloj fino a F): termina (con tope).
 *  negativa  (compilado con -DFRAC_LEJOS=0u: el camino corto aceptado también recién volcado) TIENE que salir una hora rota;
 *            si no sale, el modelo no ve el problema y la prueba no vale.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hal.h"

static long long ahora_ns;               /* el tiempo del modelo */
static int parado;                        /* 1: el oscilador no corre */
static long long fijo_ns;
static long accesos, retardos;
static long pausa_en = -1; static long long pausa_ns;      /* (AUD7 B1) una «interrupción larga»: en ese acceso, el tiempo salta pausa_ns */
#define T_ACC 150LL                       /* ns por acceso al bloque (≈ 2 ciclos de bus a 13 MHz, holgado) */
#define P     20000LL                     /* ns de propagación por campo */
static const long long BASE = 0;          /* el instante 0 del modelo es el segundo «base_seg» */
static time_t base_seg;
static u16 reg_escrito;

static long long t_modelo(void) { return parado ? fijo_ns : ahora_ns; }
static long long piso_div(long long a, long long b) { long long q = a / b; if ((a % b) && ((a < 0) != (b < 0))) q--; return q; }
static struct tm cal(long long t)          /* el calendario del instante t (ns) */
{
    time_t s = base_seg + (time_t)piso_div(t, 1000000000LL); struct tm r; gmtime_r(&s, &r); return r;
}
static u16 campo(int off)
{
    long long t = t_modelo(), frac = t - piso_div(t, 1000000000LL) * 1000000000LL;
    struct tm c;
    switch (off) {
    case 0x00: return (u16)((frac * 32768 / 1000000000LL) & 0xFF);
    case 0x02: return (u16)(frac * 128 / 1000000000LL);
    case 0x14: return 4;
    case 0x1E: return 0xC0;
    case 0x04: c = cal(t - 1 * P); return (u16)c.tm_sec;
    case 0x06: c = cal(t - 2 * P); return (u16)c.tm_min;
    case 0x08: c = cal(t - 3 * P); return (u16)c.tm_hour;
    case 0x0C: c = cal(t - 4 * P); return (u16)c.tm_mday;
    case 0x0E: c = cal(t - 5 * P); return (u16)(c.tm_mon + 1);
    case 0x10: c = cal(t - 6 * P); return (u16)((c.tm_year + 1900) % 100);
    case 0x12: c = cal(t - 6 * P); return (u16)((c.tm_year + 1900) / 100);
    default:   return 0;
    }
}
volatile u16 *rtc_prueba_reg(u32 off);
volatile u16 *rtc_prueba_reg(u32 off)
{
    static volatile u16 v;
    ahora_ns += T_ACC; accesos++;
    if (accesos == pausa_en) ahora_ns += pausa_ns;
    v = campo((int)off); reg_escrito = 0;
    return &v;
}
void hal_retardo(u32 n) { ahora_ns += (long long)n * 500; retardos++; }
u32 irq_cerrar(void) { return 0; }
void irq_abrir(void) { }
void intc_habilita(u32 n, void (*f)(void)) { (void)n; (void)f; }

static int igual(const rtc_t *r, const struct tm *c)
{
    return r->seg == c->tm_sec && r->min == c->tm_min && r->hora == c->tm_hour && r->dia == c->tm_mday &&
           r->mes == c->tm_mon + 1 && r->anio == c->tm_year + 1900;
}
static int valida_get(long long t0, long long t1, const rtc_t *r)   /* ¿r es la hora de algún segundo entre t0 y t1? */
{
    for (long long s = piso_div(t0, 1000000000LL) - 1; s <= piso_div(t1, 1000000000LL); s++) {
        struct tm c = cal(s * 1000000000LL); if (igual(r, &c)) return 1;
    }
    return 0;
}
static int valida_128(long long t0, long long t1, u32 v)           /* ¿v es (min*60+seg)*128+frac de algún instante t0..t1? */
{
    long long t = t0 - 2 * T_ACC;                                   /* el valor solo cambia en las fronteras de 1/128 s */
    while (t <= t1) {
        struct tm c = cal(t); long long frac = t - piso_div(t, 1000000000LL) * 1000000000LL;
        if ((u32)((c.tm_min * 60 + c.tm_sec) * 128 + (int)(frac * 128 / 1000000000LL)) == v) return 1;
        t = (piso_div(t, 7812500LL) + 1) * 7812500LL;
    }
    return 0;
}
static void pinta(const rtc_t *r, char *b) { sprintf(b, "%04u-%02u-%02u %02u:%02u:%02u", r->anio, r->mes, r->dia, r->hora, r->min, r->seg); }

static int vuelcos(int negativa)
{
    static const char *fechas[] = { "2026-10-14 10:10:59", "2026-10-14 10:59:59", "2026-10-14 23:59:59", "2026-10-31 23:59:59",
                                    "2026-02-28 23:59:59", "2028-02-28 23:59:59", "2028-02-29 23:59:59", "2026-12-31 23:59:59",
                                    "2099-12-31 23:59:59" };
    long rotas = 0, rotas128 = 0, total = 0, largas = 0;
    for (unsigned i = 0; i < sizeof fechas / sizeof *fechas; i++) {
        struct tm tm; memset(&tm, 0, sizeof tm); strptime(fechas[i], "%Y-%m-%d %H:%M:%S", &tm);
        base_seg = timegm(&tm);                                   /* el vuelco es en t = 1 s */
        for (long long d = -3000000; d <= 20000000; d += 733) {
            rtc_t r; char b[40]; long long t0; u32 l0 = rtc_lecturas_largas, v;
            ahora_ns = 1000000000LL + d; t0 = ahora_ns;
            rtc_get(&r); total++;
            if (!valida_get(t0, ahora_ns, &r)) { if (!rotas++) { pinta(&r, b); printf("  ROTA: %s leyendo a %+lld ns del vuelco de %s\n", b, d, fechas[i]); } }
            largas += rtc_lecturas_largas != l0;
            ahora_ns = 1000000000LL + d; t0 = ahora_ns;
            v = rtc_128();
            if (!valida_128(t0, ahora_ns, v)) { if (!rotas128++) printf("  ROTA rtc_128: %u leyendo a %+lld ns del vuelco de %s\n", v, d, fechas[i]); }
        }
    }
    printf("vuelcos: %ld lecturas en %zu vuelcos (de -3 ms a +20 ms), rotas %ld, rotas rtc_128 %ld, camino largo %ld (%.1f %%)\n",
           total, sizeof fechas / sizeof *fechas, rotas, rotas128, largas, 100.0 * largas / total);
    if (negativa) return rotas + rotas128 > 0 ? 0 : 1;
    return rotas + rotas128 == 0 ? 0 : 1;
}

static int lejos(void)
{
    long malas = 0; u32 l0 = rtc_lecturas_largas; long r0 = retardos;
    srand(580);
    base_seg = 1791900000;
    for (int i = 0; i < 10000; i++) {
        rtc_t r; long long t0;
        long long s = rand() % 100000, f = 2 * 7812500LL + 1000 + (long long)rand() % (1000000000LL - 2 * 7812500LL - 5000);
        ahora_ns = s * 1000000000LL + f; t0 = ahora_ns;
        rtc_get(&r); if (!valida_get(t0, ahora_ns, &r)) malas++;
        ahora_ns = s * 1000000000LL + f; t0 = ahora_ns;
        if (!valida_128(t0, ahora_ns, rtc_128())) malas++;
    }
    printf("lejos: 10000 + 10000 lecturas lejos del vuelco: malas %ld, camino largo %u, llamadas a hal_retardo %ld\n",
           malas, rtc_lecturas_largas - l0, retardos - r0);
    return malas == 0 && rtc_lecturas_largas == l0 && retardos == r0 ? 0 : 1;
}

static int parado_caso(void)
{
    rtc_t r; long a0;
    parado = 1; base_seg = 1791900000;
    fijo_ns = 0;                                      /* fracción clavada en 0: camino largo, pero termina */
    a0 = accesos; rtc_get(&r); (void)rtc_128();
    printf("parado (fracción 0): termina tras %ld accesos\n", accesos - a0);
    fijo_ns = 999999000LL;                            /* fracción 0x7F y el fino arriba: la espera del vuelco tiene tope */
    a0 = accesos; rtc_get(&r); (void)rtc_128();
    printf("parado (fracción 0x7F): termina tras %ld accesos\n", accesos - a0);
    parado = 0;
    return 0;
}

/* (AUD7 B1) LAS PAUSAS: una parada de ~1 s (0,98..2 s) en CUALQUIER acceso de la lectura (entre la primera fracción y los
 * campos, a mitad de leer los campos, entre las dos lecturas, antes de la última fracción), cerca de un vuelco de minuto, de hora y
 * de año. La hora tiene que salir coherente (una que el reloj marcó en algún instante de la llamada). */
static int pausas(int negativa)
{
    static const long long PAUSAS[] = { 980000000LL, 990000000LL, 999000000LL, 1000000000LL, 1001000000LL, 1005000000LL, 1100000000LL, 2000000000LL };
    static const char *fechas[] = { "2026-10-14 10:10:59", "2026-10-14 23:59:59", "2026-12-31 23:59:59" };
    long rotas = 0, total = 0;
    for (unsigned fi = 0; fi < 3; fi++) {
        struct tm tm; memset(&tm, 0, sizeof tm); strptime(fechas[fi], "%Y-%m-%d %H:%M:%S", &tm); base_seg = timegm(&tm);
        for (unsigned pi = 0; pi < sizeof PAUSAS / sizeof *PAUSAS; pi++)
            for (long k = 1; k <= 18; k++)
                for (long long d = -1100000000LL; d <= 30000000LL; d += 1700000LL) {
                    rtc_t r; long long t0; char b[40];
                    ahora_ns = 1000000000LL + d; t0 = ahora_ns; pausa_en = accesos + k; pausa_ns = PAUSAS[pi];
                    rtc_get(&r); total++; pausa_en = -1;
                    if (!valida_get(t0, ahora_ns, &r) && !rotas++) { pinta(&r, b); printf("  ROTA con pausa: %s (pausa %lld ms en el acceso %ld, a %+lld ns del vuelco de %s)\n", b, PAUSAS[pi] / 1000000, k, d, fechas[fi]); }
                }
    }
    printf("pausas: %ld lecturas con una parada de 0,98..2 s en cada uno de los 18 primeros accesos, cerca de 3 vuelcos: rotas %ld\n", total, rotas);
    return negativa ? (rotas > 0 ? 0 : 1) : (rotas == 0 ? 0 : 1);
}
int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "todo";
    int r = 0;
    (void)reg_escrito; (void)BASE;
    if (!strcmp(c, "negativa")) {
        r = vuelcos(1);
        puts(r ? "NEGATIVA MAL: con el camino corto recién volcado NO sale ninguna hora rota (el modelo no ve el problema)"
               : "NEGATIVA BIEN: con el camino corto recién volcado sale una hora rota, como debe");
        return r;
    }
    if (!strcmp(c, "negativa_pausas")) {
        r = pausas(1);
        puts(r ? "NEGATIVA DE LAS PAUSAS MAL: sin las dos lecturas ni el tope del salto NO sale ninguna hora rota (el modelo no ve el problema)"
               : "NEGATIVA DE LAS PAUSAS BIEN: con el camino corto de I1 (una lectura, sin tope) una parada de ~1 s da una hora rota, como dijo AUD7");
        return r;
    }
    r |= vuelcos(0); r |= lejos(); r |= parado_caso(); r |= pausas(0);
    puts(r ? "RTC: MAL" : "RTC (hal/rtc.c contra el modelo con acarreo): TODO BIEN");
    return r;
}
