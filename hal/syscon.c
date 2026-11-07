/* Relojes (SYSCON 0xF9090000), reset y esperas del DB2020. docs/hal.md §2, §3 y §6. */
#include "hal.h"

/* §2.1: el inicio de FUN_44236220 con los valores de la traza e14: todas las puertas cerradas y todos los
 * bloques vivos; cada controlador abre luego la suya (puerta = quitar el bit; reset = poner el bit). */
void syscon_init(void)
{
    /* Hallazgo A3 de la pista R [observado: desensamblado 0x4414098E..96 y traza e14 l. 135-136]: justo antes de
     * FUN_44236220, y ya con el PLL arriba, el crt0 del original deja a 0 los dos divisores de memoria; el +52 lo
     * había puesto a 0x30 para encender la SDRAM (arranque.S paso 5) y aquí se quedaba así. Son los únicos
     * accesos a esos dos registros en toda la imagen. plat llama a esto después de reloj_208(). */
    R16(SYSCON + 0x50) = 0; R16(SYSCON + 0x52) = 0;
    /* Hallazgo M3 [observado: 0x44236222..3A, traza e14 l. 137-142]: FUN_44236220 empieza con las tres palabras de
     * puertas a 0 y las tres de reset a 0 (todos los bloques en reset) y solo después pone los valores: así los
     * periféricos no quedan como los dejara el cargador (o nuestra vida anterior en un reinicio en caliente). */
    R16(SYSCON + 0x16) = 0; R16(SYSCON + 0x18) = 0; R16(SYSCON + 0x1A) = 0;
    R16(SYSCON + 0x10) = 0; R16(SYSCON + 0x12) = 0; R16(SYSCON + 0x14) = 0;
    R16(SYSCON + 0x16) = 0x3FFF; R16(SYSCON + 0x18) = 0x03FF; R16(SYSCON + 0x1A) = 0x7FBF;
    R16(SYSCON + 0x3A) |= 0x0100;
    R16(SYSCON + 0x10) = 0x1EBD; R16(SYSCON + 0x12) = 0x03DF; R16(SYSCON + 0x14) = 0x1ABF;
    R16(SYSCON + 0x0C) = (u16)((R16(SYSCON + 0x0C) & 0xFD77) | 0x0140);
    R16(SYSCON + 0x0A) &= 0xF9FF;
    R16(SYSCON + 0x1E) |= 0x0010;
    R16(SYSCON + 0x02) |= 0x1002;
    R16(SYSCON + 0x3E) |= 0x5000;
    R16(SYSCON + 0x44) &= 0xF7FF;
    R16(SYSCON + 0x04) = (u16)((R16(SYSCON + 0x04) & 0xF000) | 0x0708);
    R16(SYSCON + 0x0E) = 0x0084; R16(SYSCON + 0x2E) = 0x0300;
    R16(SYSCON + 0x32) = 0;      R16(SYSCON + 0x36) = 0x8000;
    R16(SYSCON + 0x1C) |= 0x0C00;
    R16(SYSCON + 0x06) = 0x0709;
    R16(SYSCON + 0x16) &= (u16)~0x0002u;         /* puerta 30: los temporizadores (espera_ms los usa desde ya) */
}

/* §3: «reset forzado» del original (sys_forced_reset, 0x441A7164..7A [observado en el DESENSAMBLADO; el pseudo-C
 * se come la escritura del bucle — hallazgo A1 de la pista R]):
 *     IRQ y FIQ cortadas;  F9010030 <- 0;  F901002C <- 0;  y para siempre:  F9010014 <- 1
 * o sea, no «espera» a que muerda el perro: lo dispara escribiendo sin parar en +14. Se hace igual, y sin tocar
 * nada más del bloque de temporizadores (la versión anterior paraba los dos temporizadores y esperaba con
 * espera_ms(), que reprograma F9010000/08: nada de eso lo hace el original).
 * NUESTRO: el bucle no es infinito. Si pasados ~3 s (medidos por el RTC, que solo se LEE; con tope de vueltas por si
 * el RTC no corre) el perro no ha mordido, reinicio EN CALIENTE. Son 3 s y no 1 para no adelantarnos a un perro de
 * verdad: [sin saber] su periodo; el modo estricto de la pista E4 lo supone de 2 s ([inferido]) y con él el reinicio
 * es ya el del perro. Sin perro (el emulador sin estricto ni reset-real):
 * arranque.S baja el reloj a 13 MHz y vuelve a entrar por el vector de reset.
 * [sin saber] qué es F9010014 y el periodo del perro. */
void reinicio_caliente(void);
void sys_reset(void)
{
    u32 t0, n;
    irq_cerrar();
    R32(0xF9010030) = 0;
    R32(0xF901002C) = 0;
    t0 = rtc_128();
    for (n = 0; n < 40000000u; n++) {
        R32(0xF9010014) = 1;
        if ((n & 0x3FF) == 0x3FF) {
            u32 t = rtc_128();
            if (((t >= t0 ? t : t + 3600u * 128u) - t0) >= 3u * 128u) break;
        }
    }
    R32(0xFB020104) = 0xFFFFFFFF; R32(0xFB020204) = 0xFFFFFFFF; R32(0xFB020304) = 0xFFFFFFFF;
    reinicio_caliente();
    for (;;) ;
}

/* §6: espera por el TEMPORIZADOR 2 (cuentas de 32 768 Hz en +20), así no depende de la velocidad de la CPU
 * ni de que el tic esté en marcha. No se da por hecho que el contador arranque en 0 [sin saber en el aparato]:
 * se mide la diferencia. Por tramos de hasta 0x4000 cuentas (500 ms), porque el contador es de 16 bits.
 * EL TOPE DE VUELTAS (por si el contador no corriera: puerta cerrada, bloque muerto) — pista S7. Antes eran 4 000 000 de
 * vueltas fijas por tramo, y con la CPU a 208 MHz CORTABAN la espera: la pista E6 midió 112 ms donde se pedían 120 (la vuelta
 * son 7 instrucciones; a 4 ns cada una, 4 M de vueltas son 112 ms), y un tramo de 500 ms se habría quedado en eso mismo SIN
 * AVISAR. Ahora el tope es PROPORCIONAL al tramo y holgado: 4096 vueltas por cuenta de 30,5 µs. Para que una cuenta durase
 * menos que 4096 vueltas la vuelta (que lleva una lectura de E/S) tendría que costar menos de 7,5 ns: a 208 MHz son 1,5 ciclos.
 * En el emulador con --tiempos (28 ns por vuelta) son ~1090 vueltas por cuenta: margen de 3,7; en el aparato, [inferido] más.
 * Si el contador no corre, un tramo de 500 ms se rinde a los ~2–4 s (67 M de vueltas) en vez de colgar; cada vez que el tope
 * corta se cuenta en espera_cortadas (se ve en Sistema): en un aparato sano tiene que ser 0. */
u32 espera_cortadas;
/* (2026-10-26) MEDIDO EN EL APARATO: el contador del temporizador 2 SE PONE A 0 al rearrancarlo, pero CON RETRASO (va en el reloj de 32 kHz):
 * a veces la primera lectura todavía da lo que quedó de la espera anterior (c0 = 461, 3410…) y unas decenas de vueltas después pasa a 0;
 * con «(cuenta - c0) & 0xFFFF» eso eran 62 000 cuentas de golpe y la espera ACABABA AL INSTANTE (10 de cada ~40 s de uso: los 104 ms entre
 * preguntas al CED de la tarjeta, y también los 250 ms de apagado de la tarjeta y los 14 ms tras darle tensión: identificaciones fallidas).
 * Ahora el tiempo se SUMA por avances entre dos lecturas seguidas; un paso hacia ATRÁS (>= 0x8000: el contador no avanza tanto entre dos
 * lecturas, ni siquiera con una IRQ larga por medio) es esa puesta a 0 retrasada, y no cuenta (espera_reinicios). Instrumentación (solo
 * RAM): las esperas que aun así acaban con menos de 8 vueltas por cuenta pedida (espera_cortas, las 8 últimas en espera_corta: {t, primera
 * lectura, segunda, última, vueltas}) y las llamadas desde una interrupción (espera_en_irq: el temporizador 2 no se puede compartir). */
volatile u32 espera_cortas, espera_en_irq, espera_corta_n, espera_reinicios, espera_saltos, espera_salto_ult[2];
volatile u32 espera_corta[8][5];
/* (2026-10-26, lo último) UN AVANCE LEGÍTIMO ENTRE DOS LECTURAS SEGUIDAS ES DE POCAS CUENTAS. MEDIDO: el temporizador 2 al rearrancarlo (+00 &= ~2; +00 |= 2)
 * (herr/puente/ms/reinicio_t2.c, 300 pruebas por caso): parado ~30 µs -> en el 80 % la primera lectura es la cuenta VIEJA y luego baja a 0; sin hueco
 * -> en el 96 % NI SE PONE A 0 (sigue desde la cuenta anterior, que tras unas esperas seguidas puede ser alta) y a veces se pone a 0 más tarde; parado
 * >= 4 ms -> siempre a 0 a la primera. Con «paso >= 0x8000 = hacia atrás» el reinicio retrasado desde una cuenta ALTA (c0 = 0xB706, 0xFA5C, …) daba
 * 0x10000 - c0 < 0x8000: ¡18 000 cuentas de avance! y la espera de 34 cuentas acababa en 22 vueltas (6 de 108 000 esperas de 1 ms en el estrés;
 * 1 cada ~25 min en el uso normal de alim5). Una IRQ larga (la del alimentador, máx 397 µs) son 13 cuentas: lo que pase de ESPERA_PASO_MAX es una
 * DISCONTINUIDAD (reinicio, envoltura espuria) y no cuenta como tiempo (espera_saltos). En el peor caso eso ALARGA una espera, nunca la acorta. */
#define ESPERA_PASO_MAX 128u
/* (2026-10-26, lo que quedaba) LA LECTURA DEL CONTADOR ES ASÍNCRONA: +20 va en el reloj de 32 kHz y la CPU lo lee en el suyo; en el instante en que
 * cuenta puede dar un valor A MEDIAS (las cifras bajas ya cambiadas y el acarreo todavía no: 0x7BB7 -> 0x7BB0, 0x9B9F -> 0x9B80, 0x12FF -> 0x1200).
 * MEDIDO en el aparato leyendo en un bucle apretado: 11-23 de esas lecturas cada 3 s (una por cada ~4000-8000 cuentas pasadas; de -1 hasta
 * -31 vistas, y en los cruces de 8 bits serían -255), todas hacia ATRÁS; con DOS lecturas seguidas iguales: 0 en 3 x 3 s (herr/puente/ms/lecturas_t2.py). Con la suma por avances el
 * paso atrás se ignora, pero «prev» quedaba en el valor bajo y la lectura siguiente, ya buena, se sumaba como tiempo: una espera de 34 cuentas
 * (1 ms) acabó con 176 vueltas tras 22 min de uso (lecturas 0x12FE, 0x12FE… 0x1300: el cruce 0x12FF -> 0x1300 daba +256 de golpe). La ventana
 * del valor a medias dura ~20 ns y una lectura cuesta ~300 ns: dos seguidas no coinciden en ella. */
static inline u32 t2_leer(void)
{
    u32 x, y, k = 0;
    do { x = R32(0xF9010020) & 0xFFFF; y = R32(0xF9010020) & 0xFFFF; } while (x != y && ++k < 16);
    return y;
}
void espera_ms(unsigned ms)
{
    u32 n = (ms * 32768u + 999u) / 1000u + 1;    /* cuentas de 32 768 Hz, redondeando hacia arriba, y una más (la primera llega a medias) */
#ifdef __arm__
    { u32 cpsr; __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr)); if ((cpsr & 0x1Fu) == 0x12u) espera_en_irq++; }
#endif
    while (n) {
        u32 t = n > 0x4000 ? 0x4000 : n, c0, c1, prev, cur, d, hecho = 0, tope = (t + 2) * 4096u, v = 0;
        R32(0xF9010000) &= ~2u;
        R32(0xF9010008) = 0xFFFF;
        R32(0xF9010000) |= 2;
        c0 = prev = t2_leer();
        c1 = cur = c0;
        while (hecho < t && --tope) {
            cur = t2_leer();
            if (v == 0) c1 = cur;
            d = (cur - prev) & 0xFFFF;
            if (d > ESPERA_PASO_MAX) {                         /* no es un avance: hacia atrás (>= 0x8000) o un salto (reinicio retrasado) */
                if (d >= 0x8000u) espera_reinicios++; else { espera_saltos++; espera_salto_ult[0] = prev; espera_salto_ult[1] = cur; }
                d = 0;
            }
            hecho += d; prev = cur; v++;
        }
        if (!tope) espera_cortadas++;
        if (v < t * 8u) { volatile u32 *e = espera_corta[espera_corta_n++ % 8u]; espera_cortas++; e[0] = t; e[1] = c0; e[2] = c1; e[3] = cur; e[4] = v; }
        R32(0xF9010000) &= ~2u;
        n -= t;
    }
}

u32 exc_ctx[21];                                 /* contexto de la última excepción (arranque.S) */

__attribute__((weak)) void irq_despacho(void) { }
