/* (R2) hal/i2c.c DE VERDAD (compilado con -DI2C_PRUEBA) contra un MODELO del controlador I²C del DB2020 y del PMU, con fallos
 * inyectados; y las reglas de api/enc.h en tabla. Es la prueba de los hallazgos A1, A3 y A4 de docs/auditoria_seguridad_s7.md
 * que el PC no podía ver (plat/hw_pc.c simula por encima de la HAL). make probar-i2c.
 * El modelo: START baja el bit 5 de +2; cada byte (o lectura) sube el bit 3; STOP baja el bit 4; estado +6 = 0x18 / 0x40 tras
 * la dirección (lo que da el emulador de la pista E). Fallos: topes (el bit no cambia nunca: la espera agota su tope),
 * un registro que da tope al escribirlo, y registros que no guardan lo que se les escribe. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "hal.h"
#include "enc.h"

volatile u32 tics;
void intc_habilita(unsigned n, void (*f)(void)) { (void)n; (void)f; }
void intc_tapar(unsigned n) { (void)n; }
void intc_destapar(unsigned n) { (void)n; }
void gpio_dir(unsigned p, u8 m, int s) { (void)p; (void)m; (void)s; }
static int p11;                                                  /* (acel_ag) P1.1 = alimentación del acelerómetro */
void gpio_set(unsigned p, u8 m, int v) { if (p == 1 && (m & 2)) p11 = !!v; }
int gpio_get(unsigned p, u8 m) { return p == 1 && (m & 2) ? p11 : 0; }
void espera_ms(unsigned ms) { (void)ms; }

/* (acel_ag) MODELOS DEL ADC DEL PMU: cada «mundo» es una forma en que podría convertir el chip de verdad (el emulador solo modela
 * EMU). La prueba es que el autodetector de hal/i2c.c elija en cada mundo el modo que vive, y que con un ADC muerto no elija ninguno. */
enum { MU_EMU, MU_PULSO_B14, MU_PULSO_RELOJ, MU_INMEDIATO, MU_72, MU_B14_CERO, MU_MUERTO, MU_SIN_SENSOR, MU_N };
static const char *const mu_nombre[MU_N] = { "EMU (convierte al escribir A0|c o 6A)", "pulso con b14", "pulso con el reloj corriendo después",
    "solo inmediato 6n", "pulso tras el pulso de 0x72", "pulso con b14 A CERO", "ADC MUERTO", "ADC vivo, sensor sin alimentar (0)" };
static u8 pmu[256];                                              /* (definición tentativa: la de verdad va más abajo) */
static int mundo, puerta_abierta = 0, linea_pendiente, armado72, f5ctl;
static u8 sel70;
static int adc_valor(unsigned c)
{
    if (c == 10) return 110;
    if (c == 4) return mundo == MU_SIN_SENSOR || !p11 ? 0 : 130;
    if (c == 12) return mundo == MU_SIN_SENSOR || !p11 ? 0 : 120;
    return c == 1 ? 60 : c == 8 ? 3 : 200 + (int)c;
}
void hal_retardo(u32 n)
{
    (void)n;
    if (mundo == MU_PULSO_RELOJ && linea_pendiente && puerta_abierta) { linea_pendiente = 0; pmu[0x71] = (u8)adc_valor(sel70 & 15); }
}
static void adc_escribe70(u8 v)
{
    sel70 = v;
    if (mundo == MU_MUERTO) return;
    if (v == 0x6A) { pmu[0x71] = (u8)adc_valor(10); return; }  /* la lectura en frío funciona en todos (es lo único medido en el aparato) */
    if ((v & 0xF0) == 0x60 && (mundo == MU_INMEDIATO)) { pmu[0x71] = (u8)adc_valor(v & 15); return; }
    if ((v & 0xF0) == 0xA0 && (mundo == MU_EMU || mundo == MU_SIN_SENSOR)) pmu[0x71] = (u8)adc_valor(v & 15);
}
static void adc_linea(u16 v)                                      /* F5000874 */
{
    if (v != 0x12 || (sel70 & 0xF0) != 0xA0) return;
    if (mundo == MU_PULSO_B14 && puerta_abierta && (f5ctl & 0x4000)) pmu[0x71] = (u8)adc_valor(sel70 & 15);
    if (mundo == MU_B14_CERO && puerta_abierta && !(f5ctl & 0x4000)) pmu[0x71] = (u8)adc_valor(sel70 & 15);
    if (mundo == MU_72 && puerta_abierta && armado72 >= 2) pmu[0x71] = (u8)adc_valor(sel70 & 15);
    if (mundo == MU_PULSO_RELOJ && puerta_abierta) linea_pendiente = 1;
}

enum { F_NADA, F_DIR, F_REG, F_DATO, F_RX };
static struct { u16 ctl, dat, sta; int fase, lee; u8 reg; } m;
static u8 pmu[256], no_pega[256];
static int topes_pendientes, tope_siempre, tope_en_reg = -1, en_tope;
static unsigned long accesos;
static struct { u8 reg, v; } hist[512]; static int n_hist;

u16 i2c_pr_leer(u32 dir)
{
    accesos++;
    if (dir == 0xF5000800u) return (u16)f5ctl;                    /* (acel_ag) */
    if (dir == 0xF9090016u) return puerta_abierta ? 0 : 0x0800;
    if ((dir & 0xFFFFFFF0u) != 0xF9040000u) return 0;           /* SYSCON, retención, banda base: pasivos (y el bus 1: no hay nadie) */
    switch (dir & 0xF) { case 0: return m.dat; case 2: return m.ctl; case 6: return m.sta; }
    return 0;
}
static void byte_tx(u8 b)
{
    if (en_tope) return;                                          /* el bit 3 no sube: tope */
    if (m.fase == F_DIR) {
        if ((b >> 1) != 0x4A) { m.sta = 0x20; m.ctl |= 0x08; return; }   /* nadie en esa dirección */
        m.lee = b & 1; m.sta = m.lee ? 0x40 : 0x18; m.fase = m.lee ? F_RX : F_REG;
    } else if (m.fase == F_REG) {
        m.reg = b; m.fase = F_DATO;
        if (tope_en_reg == b) { en_tope = 1; return; }           /* este registro «cuelga» el bus al escribirlo */
    } else if (m.fase == F_DATO) {
        if (n_hist < 512) { hist[n_hist].reg = m.reg; hist[n_hist].v = b; n_hist++; }
        if (m.reg == 0x72) armado72 = b == 0xF0 ? 1 : (b == 0xF3 && armado72 == 1) ? 2 : armado72;   /* (acel_ag) F0 -> F3 */
        if (m.reg == 0x70) adc_escribe70(b);
        if (m.reg != 0x71 && !no_pega[m.reg]) pmu[m.reg] = b;     /* (0x71: solo lectura) */
        m.reg++;
    }
    m.ctl |= 0x08;
}
void i2c_pr_escribir(u32 dir, u16 v)
{
    accesos++;
    if (dir == 0xF9090010u) { if (!(v & 0x0010)) { m.fase = F_NADA; m.ctl = 0; } return; }   /* reset del bloque 11 */
    if (dir == 0xF9090016u) { puerta_abierta = !(v & 0x0800); if (!puerta_abierta) linea_pendiente = 0; return; }   /* (acel_ag) puerta 40 */
    if (dir == 0xF5000800u) { f5ctl = v; return; }
    if (dir == 0xF5000874u) { adc_linea(v); return; }
    if ((dir & 0xFFFFFFF0u) != 0xF9040000u) return;
    switch (dir & 0xF) {
    case 0: m.dat = v; break;
    case 2:
        m.ctl &= (u16)~0x08u;
        if (v == 0x24) {                                          /* START (o START repetido) */
            if (m.fase == F_NADA) { en_tope = 0; if (tope_siempre) en_tope = 1; else if (topes_pendientes) { topes_pendientes--; en_tope = 1; } }
            if (en_tope) { m.ctl = 0x24; return; }
            m.ctl = 0; m.fase = F_DIR; return;
        }
        if (v == 0x10) { m.ctl = en_tope ? 0x10 : 0; m.fase = F_NADA; return; }   /* STOP */
        if (m.fase == F_NADA) { m.ctl = v; return; }             /* la preparación de empezar() */
        if (m.fase == F_RX) { if (!en_tope) { m.dat = pmu[m.reg++]; m.ctl |= 0x08; } return; }
        byte_tx((u8)m.dat);
        break;
    case 6: break;
    }
}

static int mal;
#define MIRA(c, ...) do { if (c) printf("  bien  "); else { printf("  MAL   "); mal = 1; } printf(__VA_ARGS__); printf("\n"); } while (0)
static void limpio(void)
{
    memset(&m, 0, sizeof m); memset(pmu, 0, sizeof pmu); memset(no_pega, 0, sizeof no_pega); pmu[0x61] = 0x13;   /* 0x61 = 13: lo peor que puede haber */
    topes_pendientes = tope_siempre = en_tope = 0; tope_en_reg = -1; n_hist = 0; accesos = 0;
    memset(&i2c_cta, 0, sizeof i2c_cta); i2c_revivir(0); tics += 100000;
}
int main(void)
{
    u8 v = 0; int r, i; unsigned long a0;
    EncEstado e;

    printf("== 1. PMU sano: la carga a 0 es LO PRIMERO que se escribe, se relee, y la lista sale entera\n");
    limpio(); r = pmu_encendido_sin_soltar();
    MIRA(r == 0, "pmu_encendido_sin_soltar = %d", r);
    MIRA(n_hist > 2 && hist[0].reg == 0x61 && hist[0].v == 0x10 && hist[1].reg == 0x63 && hist[1].v == 0x00, "primeras escrituras: %02X<-%02X, %02X<-%02X (antes: 60<-88)", hist[0].reg, hist[0].v, hist[1].reg, hist[1].v);
    MIRA(pmu_carga.confirmada && pmu_carga.r61 == 0x10 && pmu_carga.r63 == 0, "carga confirmada (61 = %02X, 63 = %02X)", pmu_carga.r61, pmu_carga.r63);
    MIRA(pmu[0x41] == 0x26 && pmu[0x60] == 0x88 && pmu[0x61] == 0x10, "41 = %02X, 60 = %02X, 61 = %02X", pmu[0x41], pmu[0x60], pmu[0x61]);

    printf("== 2. (A1) UN tope en la primera transferencia: antes el bus quedaba muerto para siempre; ahora se recupera\n");
    limpio(); topes_pendientes = 1; r = pmu_encendido_sin_soltar();
    MIRA(i2c_cta.topes == 1, "topes = %u", i2c_cta.topes);
    MIRA(r == 0 && pmu_carga.confirmada && pmu[0x41] == 0x26, "arranque completo pese al tope: r = %d, carga confirmada %d, 41 = %02X", r, pmu_carga.confirmada, pmu[0x41]);
    MIRA(i2c_cta.recuperados >= 1, "recuperados = %u", i2c_cta.recuperados);

    printf("== 3. (A1) veto: tras un tope, el bus se rechaza sin tocar el hardware durante 2 s o 16 llamadas, y después se reintenta\n");
    limpio(); tope_siempre = 1; r = pmu_leer(0x23, &v);
    MIRA(r && i2c_vetado(0), "primer tope: r = %d, vetado = %d", r, i2c_vetado(0));
    a0 = accesos; r = pmu_leer(0x23, &v);
    MIRA(r && accesos == a0, "la siguiente llamada se rechaza sin un solo acceso (%lu)", accesos - a0);
    tope_siempre = 0; pmu[0x23] = 0x5A;
    for (i = 0; i < 20 && pmu_leer(0x23, &v); i++) ;
    MIRA(v == 0x5A && i <= 16, "con el tic PARADO vuelve a contestar tras %d rechazos (v = %02X)", i, v);
    limpio(); tope_siempre = 1; pmu_leer(0x23, &v); tope_siempre = 0; pmu[0x23] = 0xA5;
    r = pmu_leer(0x23, &v); MIRA(r, "a los 0 ms: rechazado");
    tics += 499; r = pmu_leer(0x23, &v); MIRA(r, "a los 1996 ms: rechazado");
    tics += 2; r = pmu_leer(0x23, &v); MIRA(!r && v == 0xA5, "a los 2004 ms: se reintenta y contesta (v = %02X); reintentos = %u, recuperados = %u", v, i2c_cta.reintentos, i2c_cta.recuperados);

    printf("== 4. (A4) la escritura de 0x60 «cuelga» el bus: antes la lista abortaba ahí y 61<-10 NO se escribía\n");
    limpio(); tope_en_reg = 0x60; r = pmu_encendido_sin_soltar();
    MIRA(pmu[0x61] == 0x10 && pmu[0x63] == 0x00 && pmu_carga.confirmada, "61 = %02X, 63 = %02X, confirmada = %d", pmu[0x61], pmu[0x63], pmu_carga.confirmada);
    MIRA(r == -1 && pmu_carga.lista_mal == 1, "y se sabe que la lista NO salió entera: r = %d, lista_mal = %u (solo la de 0x60)", r, pmu_carga.lista_mal);
    MIRA(pmu[0x41] == 0x26, "el resto de la lista sí se escribió (41 = %02X)", pmu[0x41]);

    printf("== 5. (A4) 0x61 NO guarda lo que se le escribe (se queda en 13): se detecta y se dice\n");
    limpio(); no_pega[0x61] = 1; r = pmu_carga_fuera();
    MIRA(r == -1 && !pmu_carga.confirmada && pmu_carga.r61 == 0x13, "pmu_carga_fuera = %d, confirmada = %d, releído 61 = %02X", r, pmu_carga.confirmada, pmu_carga.r61);
    for (i = 0, r = 0; i < n_hist; i++) if (hist[i].reg == 0x61) r++;
    MIRA(r == 3, "lo intentó %d veces", r);
    no_pega[0x61] = 0; r = pmu_carga_fuera(); MIRA(r == 0 && pmu_carga.confirmada, "NEGATIVA: en cuanto el registro guarda, confirmada = %d", pmu_carga.confirmada);

    printf("== 6. (A1) PMU mudo del todo: nada se cuelga, no se confirma nada, y al volver a contestar todo vuelve\n");
    limpio(); tope_siempre = 1; r = pmu_encendido_sin_soltar();
    MIRA(r == -1 && !pmu_carga.confirmada && pmu_carga.r61 == -1, "r = %d, confirmada = %d, r61 = %d (sin leer); topes = %u", r, pmu_carga.confirmada, pmu_carga.r61, i2c_cta.topes);
    tope_siempre = 0; r = pmu_carga_fuera(); MIRA(r == 0 && pmu[0x61] == 0x10, "vuelve: pmu_carga_fuera = %d, 61 = %02X", r, pmu[0x61]);
    r = pmu_apagar(); MIRA(r == 0 && pmu[0x41] == 0, "pmu_apagar devuelve que la escritura salió (41 = %02X)", pmu[0x41]);

    printf("== 7. (A3) api/enc.h: en qué señal de la tecla de encendido se cree\n");
    memset(&e, 0, sizeof e);
    for (i = 0; i < 200; i++) enc_vigia(&e, 1, -1);
    MIRA(e.vigias == 0 && !e.kbd && !enc_forzar(&e, 100000, 0) && !enc_mantenida(&e, -1), "foto «pulsada» desde el arranque, 51 s: NO cuenta (antes: apagado a los 8 y a los 11 s)");
    enc_vigia(&e, 0, -1); for (i = 0; i < ENC_LARGO_VIGIAS; i++) enc_vigia(&e, 1, -1);
    MIRA(e.vigias == ENC_LARGO_VIGIAS && enc_mantenida(&e, -1), "NEGATIVA: vista suelta una vez y luego 7,9 s pulsada: cuenta");
    for (i = 0; i < 20; i++) enc_vigia(&e, 1, -1);
    MIRA(!enc_forzar(&e, 749, 0), "13 s pulsada con el bucle latiendo: NO hay apagado forzado (antes sí)");
    MIRA(enc_forzar(&e, 750, 0), "NEGATIVA: y con el bucle 3 s sin latir, sí");
    MIRA(!enc_forzar(&e, 100000, 1), "en modo seguro, nunca");
    memset(&e, 0, sizeof e); for (i = 0; i < 100; i++) enc_vigia(&e, 1, 0);
    MIRA(e.vigias == 0, "foto clavada pero el NIVEL dice suelta: no cuenta");
    for (i = 0; i < 40; i++) enc_vigia(&e, 1, 1);
    MIRA(e.vigias == 40, "NEGATIVA: el nivel (armado) dice pulsada: cuenta");
    memset(&e, 0, sizeof e); enc_vigia(&e, 0, -1); enc_pmu(&e, 1);
    MIRA(!enc_pmu_probado(&e) && !enc_mantenida(&e, 1), "PMU «pulsada» sin haberlo visto suelto nunca: no cuenta");
    enc_pmu(&e, 0); enc_pmu(&e, 1);
    MIRA(enc_pmu_probado(&e) && enc_mantenida(&e, 1), "PMU visto suelto y pulsado: vía probada; pulsada ahora = mantenida");
    for (i = 0; i < 40; i++) enc_vigia(&e, 1, -1);
    MIRA(e.kbd && !enc_mantenida(&e, 0), "la foto se queda «pulsada» pero el PMU (vía probada) dice SUELTA: no se cree al teclado");
    MIRA(enc_mantenida(&e, -1), "si el PMU no se puede leer, manda el teclado");
    printf("== 8. (acel_ag) el AUTODETECTOR del modo del ADC elige, en cada mundo posible del chip, el modo que convierte\n");
    {
        static const int espera[MU_N] = { ADC_A, ADC_A, ADC_P, ADC_I, ADC_R, ADC_B, -1, ADC_A };
        int x, y;
        for (i = 0; i < MU_N; i++) {
            limpio(); pmu_encendido_sin_soltar();                 /* deja F5000800 bit 14 puesto, como en el aparato */
            mundo = i; armado72 = 0; linea_pendiente = 0; p11 = 0; pmu[0x71] = 0x55;
            adc_forzar(-1);
            acel_alimentar(1);                                    /* como el podómetro: alimenta y, 20 ms después, lee */
            r = acel_leer(&x, &y);
            MIRA(adc_dg.auto_modo == espera[i] && adc_dg.modo == (espera[i] < 0 ? ADC_A : espera[i]),
                 "%-40s -> elige %c (modo en uso %c) [esperado %c]; vivo A%d P%d R%d B%d I%d J%d S%d", mu_nombre[i],
                 adc_dg.auto_modo < 0 ? '-' : ADC_NOMBRES[adc_dg.auto_modo], ADC_NOMBRES[adc_dg.modo], espera[i] < 0 ? '-' : ADC_NOMBRES[espera[i]],
                 adc_dg.vivo[0], adc_dg.vivo[1], adc_dg.vivo[2], adc_dg.vivo[3], adc_dg.vivo[4], adc_dg.vivo[5], adc_dg.vivo[6]);
            if (espera[i] >= 0 && i != MU_SIN_SENSOR) MIRA(!r && x == 120 && y == 130, "  y la lectura del podómetro es la buena: X = %d, Y = %d", x, y);
            if (i == MU_SIN_SENSOR) MIRA(!r && x == 0 && y == 0, "  (el ADC vive pero el sensor da 0: se ve 0 / 0, no se esconde) X = %d, Y = %d", x, y);
            if (i == MU_MUERTO) {
                MIRA(!adc_dg.hallado && adc_dg.intentos == 1, "  ningún modo vivo: hallado = %d, intentos = %u (se queda en A)", adc_dg.hallado, adc_dg.intentos);
                acel_leer(&x, &y); MIRA(adc_dg.intentos == 1, "  la lectura siguiente NO repite la prueba (intentos = %u)", adc_dg.intentos);
                tics += 2600; acel_leer(&x, &y); MIRA(adc_dg.intentos == 2, "  a los 10 s sí (intentos = %u)", adc_dg.intentos);
                for (r = 0; r < 10; r++) { tics += 2600; acel_leer(&x, &y); }
                MIRA(adc_dg.intentos == 5, "  y deja de reintentar a las 5 (intentos = %u)", adc_dg.intentos);
                mundo = MU_INMEDIATO; adc_autodetectar(); acel_leer(&x, &y);
                MIRA(adc_dg.modo == ADC_I && x == 120 && y == 130, "  si revive, la prueba a mano (tecla 9) lo encuentra: modo %c, X = %d, Y = %d", ADC_NOMBRES[adc_dg.modo], x, y);
            }
            MIRA(p11 == 1, "  P1.1 sigue como lo dejó quien llama (encendido): %d", p11);
            MIRA((f5ctl & 0x4000) && !puerta_abierta, "  F5000800 bit 14 repuesto (%04X) y la puerta 40 cerrada (%d)", f5ctl, !puerta_abierta);
        }
        limpio(); pmu_encendido_sin_soltar(); mundo = MU_PULSO_RELOJ; p11 = 0; adc_forzar(-1); adc_autodetectar();
        MIRA(p11 == 0, "con el sensor APAGADO, la prueba lo alimenta y lo vuelve a apagar (P1.1 = %d) y elige %c", p11, ADC_NOMBRES[adc_dg.modo]);
        adc_forzar(ADC_A); acel_alimentar(1); r = acel_leer(&x, &y);
        MIRA(adc_dg.modo == ADC_A && adc_dg.forzado == ADC_A && y != 130, "FORZADO a A en un mundo donde A no vive: se usa A (Y = %d, el valor viejo, no 130)", y);
        adc_forzar(ADC_MODOS); acel_leer(&x, &y);
        MIRA(adc_dg.forzado == -1 && adc_dg.modo == ADC_P && x == 120, "tras el último modo, vuelta a automático: se vuelve a probar y elige %c (X = %d)", ADC_NOMBRES[adc_dg.modo], x);
        mundo = MU_EMU; tope_siempre = 1; adc_forzar(-1); r = acel_leer(&x, &y); tope_siempre = 0;
        MIRA(r == -1 && !adc_dg.hallado, "PMU mudo durante la prueba: la lectura falla (%d) y no se da ningún modo por vivo", r);
    }
    printf(mal ? "I2C Y TECLA DE ENCENDIDO: HAY FALLOS\n" : "I2C Y TECLA DE ENCENDIDO: TODO BIEN\n");
    return mal;
}
