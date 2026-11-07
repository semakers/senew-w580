/* Maestros I²C del DB2020 POR SONDEO (dos buses: 0xF9040000 y 0xF9070000, mismo controlador que el DB2010) y lo
 * del PMU «Vincenne» (bus 0, esclavo 0x4A) que usa el sistema: retroiluminación, cargador, ADC. docs/hal.md §11, §12.
 *
 * La secuencia es la del controlador por sondeo del original (0x449CDCD0 / 0x449CDD3C): en CADA transferencia,
 * puerta de reloj (13 / 14) y pulso de reset del bloque (11 / 12), +2 = 4, divisor 0x7E, +0 = 0, +2 = 0;
 * START = +2 <- 0x24 y esperar a que baje el bit 5; cada byte = +0 <- byte, +2 <- 0 y esperar el bit 3;
 * STOP = +2 <- 0x10 y esperar a que baje el bit 4. NUESTRO: todas las esperas tienen tope (un bus colgado no
 * para el sistema: tras el primer tope ese bus se da por muerto), y se mira el estado (+6) tras la dirección
 * para contar un NACK [inferido: los códigos 0x18/0x40 son los del emulador].
 * Esclavos (§11): bus 0 = solo el PMU; bus 1 = radio FM (0x10, 0x11), cámara, controlador de LED (0x40). */
#include "hal.h"

/* (R2) Todos los accesos a registros de este fichero van por RG_L / RG_E: en el teléfono son la lectura y la escritura de
 * siempre (el código que sale es el mismo); con -DI2C_PRUEBA (herr/pruebas/prueba_i2c.c, en el PC) van a un MODELO del
 * controlador y del PMU al que se le pueden inyectar topes, NACK y escrituras que no llegan. */
#ifdef I2C_PRUEBA
u16 i2c_pr_leer(u32 dir); void i2c_pr_escribir(u32 dir, u16 v);
#define RG_L(a)    i2c_pr_leer(a)
#define RG_E(a, v) i2c_pr_escribir((a), (u16)(v))
#else
#define RG_L(a)    R16(a)
#define RG_E(a, v) (R16(a) = (u16)(v))
#endif
#define I2C(n)   (0xF9040000u + 0x30000u * (n))
#define I2C_DAT  (I2C(bus) + 0)
#define I2C_CTL  (I2C(bus) + 2)
#define I2C_DIV  (I2C(bus) + 4)
#define I2C_STA  (I2C(bus) + 6)
#define TOPE 300000u

/* (R2; auditoría de S7, A1) UN TOPE YA NO MATA EL BUS PARA SIEMPRE. Antes, la primera espera agotada dejaba `muerto[bus] = 1`
 * hasta el reinicio: sin vigilancia de batería, sin aviso del cargador y sin poder apagar, en silencio. Ahora «muerto» CADUCA:
 * tras un tope las transferencias de ese bus se rechazan al instante (para que un bus colgado no cueste un tope —unos ms— en
 * cada llamada) SOLO durante I2C_VETO_TICS (2 s) o I2C_VETO_LLAMADAS rechazos, lo que llegue antes (lo segundo, porque el tic
 * no late en el arranque temprano ni en la pantalla de fallo); después se reintenta, y cada transferencia empieza con el
 * pulso de reset del bloque (empezar). Se cuenta todo: topes, reintentos tras un veto y recuperaciones (una transferencia
 * buena en un bus que estuvo vetado). Lo que NO se hace: soltar a mano la línea de datos con nueve relojes (hallazgo M5 de
 * esa auditoría): [sin saber] por qué pines salen SCL/SDA del bus 0 ni cómo se ponen como GPIO. */
#define I2C_VETO_TICS 500u
#define I2C_VETO_LLAMADAS 16u
i2c_cuentas i2c_cta;
static u8 muerto[2], rechazos[2], estuvo[2];
static u32 muerto_tic[2];

static int espera(unsigned bus, u16 mascara, int hasta_uno)
{
    u32 t = TOPE;
    while (((RG_L(I2C_CTL) & mascara) != 0) != (hasta_uno != 0))
        if (!--t) { muerto[bus] = 1; estuvo[bus] = 1; rechazos[bus] = 0; muerto_tic[bus] = tics; i2c_cta.topes++; return -1; }
    return 0;
}
static int vetado(unsigned bus)                                   /* 1 = ese bus dio un tope hace poco: no se intenta todavía */
{
    if (!muerto[bus]) return 0;
    if ((u32)(tics - muerto_tic[bus]) < I2C_VETO_TICS && ++rechazos[bus] < I2C_VETO_LLAMADAS) return 1;
    muerto[bus] = 0; i2c_cta.reintentos++;
    return 0;
}
static void bien(unsigned bus) { if (estuvo[bus]) { estuvo[bus] = 0; i2c_cta.recuperados++; } }
int i2c_vetado(unsigned bus) { return bus < 2 && muerto[bus]; }
static void empezar(unsigned bus)
{
    u16 m = bus ? 0x0020 : 0x0010, v;
    RG_E(SYSCON + 0x16, RG_L(SYSCON + 0x16) & (u16)~(bus ? 0x0040u : 0x0020u));   /* puerta 13 / 14 */
    v = RG_L(SYSCON + 0x10);
    RG_E(SYSCON + 0x10, v & ~m); RG_E(SYSCON + 0x10, v | m);      /* pulso de reset del bloque 11 / 12 */
    RG_E(I2C_CTL, 0x04); RG_E(I2C_DIV, 0x7E); RG_E(I2C_DAT, 0); RG_E(I2C_CTL, 0);
}
static int start(unsigned bus) { RG_E(I2C_CTL, 0x24); return espera(bus, 0x20, 0); }
static int stop(unsigned bus)  { RG_E(I2C_CTL, 0x10); return espera(bus, 0x10, 0); }
static int tx(unsigned bus, u8 b) { RG_E(I2C_DAT, b); RG_E(I2C_CTL, 0); return espera(bus, 0x08, 1); }
static int direccion(unsigned bus, u8 b)
{
    u16 st;
    if (tx(bus, b)) return -1;
    st = RG_L(I2C_STA) & 0xFF;
    if (st != ((b & 1) ? 0x40 : 0x18)) { i2c_cta.nack++; return -2; }   /* nadie contesta en esa dirección */
    return 0;
}

/* (S7) Un bus dado por muerto vuelve a intentarse. Lo usan el perro de software y el apagado forzado, que toman el I²C DESDE LA
 * INTERRUPCIÓN DEL TIC cuando ya han decidido no volver al bucle principal: la transacción que el bucle tuviera a medias se
 * abandona (cada transferencia empieza con un pulso de reset del bloque: empezar()), y si el esclavo se hubiera quedado a medio
 * byte y la primera no saliera, hay que poder repetirla. [sin saber] en el aparato cómo queda el PMU tras una transacción cortada. */
void i2c_revivir(unsigned bus) { if (bus < 2) muerto[bus] = 0; }
int i2c_escribir(unsigned bus, u8 dir7, const u8 *d, u32 n)       /* START, dirección+E, n bytes, STOP. 0 = bien */
{
    int r;
    if (bus > 1 || vetado(bus)) return -1;
    empezar(bus);
    r = start(bus);
    if (!r) r = direccion(bus, (u8)(dir7 << 1));
    while (!r && n--) r = tx(bus, *d++);
    if (r != -1) { if (stop(bus)) r = -1; }                       /* (R2) un STOP que no sale también es un fallo de la transferencia */
    if (!r) { i2c_cta.escrituras++; bien(bus); }
    return r ? -1 : 0;
}
/* Escribe «ne» bytes (el registro) y lee «nl» con START repetido. El original solo lee 1 byte y sin ACK;
 * para más de uno se contesta ACK a todos menos al último [inferido del DB2010: +2 bit 2]. */
int i2c_escribir_leer(unsigned bus, u8 dir7, const u8 *e, u32 ne, u8 *l, u32 nl)
{
    int r;
    if (bus > 1 || vetado(bus)) return -1;
    empezar(bus);
    r = start(bus);
    if (!r) r = direccion(bus, (u8)(dir7 << 1));
    while (!r && ne--) r = tx(bus, *e++);
    if (!r) r = start(bus);
    if (!r) r = direccion(bus, (u8)((dir7 << 1) | 1));
    while (!r && nl--) {
        RG_E(I2C_CTL, nl ? 0x04 : 0x00);
        r = espera(bus, 0x08, 1);
        *l++ = (u8)RG_L(I2C_DAT);
    }
    if (r != -1) { if (stop(bus)) r = -1; }
    if (!r) { i2c_cta.lecturas++; bien(bus); }
    return r ? -1 : 0;
}

/* ---- PMU (Vincenne, bus 0, 0x4A), por registro FÍSICO (§12) ------------------------------------------------ */
#define PMU 0x4A
int pmu_leer(u8 reg, u8 *v) { return i2c_escribir_leer(0, PMU, &reg, 1, v, 1); }
#ifdef MP3_TONO
/* desarrollo (2026-10-24): registro de TODAS las escrituras al PMU (hora en ms, registro, valor, quién llama), para ver si alguien toca el códec mientras suena */
volatile u32 pmu_log_n; volatile u32 pmu_log[128][3];
static void pmu_anotar(u8 reg, u8 v, void *quien) { u32 k = pmu_log_n++ % 128u; pmu_log[k][0] = tics * 4u; pmu_log[k][1] = ((u32)reg << 8) | v; pmu_log[k][2] = (u32)quien; }
int pmu_escribir(u8 reg, u8 v) { u8 b[2] = { reg, v }; pmu_anotar(reg, v, __builtin_return_address(0)); return i2c_escribir(0, PMU, b, 2); }
#else
int pmu_escribir(u8 reg, u8 v) { u8 b[2] = { reg, v }; return i2c_escribir(0, PMU, b, 2); }
#endif

/* Retroiluminación de la pantalla = «luz 0» del controlador de luces del original (§4.6): registro 0x45 = nivel
 * (7 bits; el de fábrica es 0xA000 >> 9 = 0x50) y 0x43 bit 7 = encendida (los demás bits de 0x43 se conservan:
 * el inicio del original deja 0x04). Primero el nivel y luego el bit; al apagar, al revés.
 * [inferido] que la luz 0 sea la de la pantalla (hal.md §4.6); [emulador: banco de registros] no se ve nada. */
#define LUZ_TOPE 0x50u
int pmu_luz_pantalla(u32 pct)                                     /* 0 = apagada; 1..100 = nivel */
{
    u8 v; u32 niv;
    if (pct > 100) pct = 100;
    if (pmu_leer(0x43, &v)) return -1;
    if (!pct) return (pmu_escribir(0x43, (u8)(v & 0x7F)) || pmu_escribir(0x45, 0x00)) ? -1 : 0;
    niv = pct * LUZ_TOPE / 100; if (!niv) niv = 1;
    return (pmu_escribir(0x45, (u8)niv) || pmu_escribir(0x43, (u8)(v | 0x80))) ? -1 : 0;
}
int pmu_luz_leer(void)                                            /* 0..100, o -1 sin bus */
{
    u8 v, niv;
    if (pmu_leer(0x43, &v) || pmu_leer(0x45, &niv)) return -1;
    if (!(v & 0x80)) return 0;
    niv &= 0x7F;
    return niv >= LUZ_TOPE ? 100 : (niv * 100 + LUZ_TOPE - 1) / LUZ_TOPE;
}
/* Alimentación (§3, §12): el original escribe 0x41 = 0x26 al arrancar (lista de valores iniciales) y su proceso
 * del PMU quita después la retención provisional (F9050058 bit 0) que pone el crt0; apaga con 0x41 = 0. */
/* Hallazgo M4 de la pista R: antes se escribía SOLO 0x41 = 0x26 y se quitaba la retención. El original escribe su
 * LISTA DE INICIO ENTERA (4423.c l. 4534..4563; traza e14 l. 858..888 [observado, en este orden]) y es DESPUÉS, en la
 * primera pasada de su proceso del PMU (FUN_442342b4), cuando quita F9050058 bit 0. Se hace igual: la lista tal
 * cual y, solo si 0x41 se relee 0x26, fuera la retención. Ojo con lo que lleva la lista:
 *  - 0x43 <- 0x04 APAGA el bit 7 (la «luz 0» del PMU que encendió el arranque temprano): la luz se pone después;
 *  - EL CARGADOR (pista S7; docs/bateria.md §4.1, §8.A1). El original escribe 0x60 <- 0x88 (techo 3900 mV, bit 7),
 *    0x61 <- 0x13 (código de corriente 3 = 300 mA) y 0x63 <- 0, y a los 17 s su gestor (BATTMGR) lo sustituye por
 *    0x61 <- 0x10 (corriente 0) si no hay cargador [ejecutado, pista B]. Aquí no hay gestor: la consigna de 300 mA
 *    se quedaba PARA SIEMPRE (hallazgo M8). Ahora la lista lleva DIRECTAMENTE 0x61 <- 0x10: es el único «no cargar»
 *    del que hay evidencia en el original (lo usa hasta para probar el transistor de carga). 0x60 se deja como en
 *    la lista (0x88) y NO se reescribe nunca (§8.A2: si su bit 7 es un perro de la carga, que caduque); 0x62 no se
 *    toca. ESTE SISTEMA NO CARGA LA BATERÍA. [sin saber] (solo el aparato) si el código 0 corta de verdad;
 *  - 0x40, 0x42, 0x44: reguladores [inferido], uno a uno [sin saber]; 0x26/0x27/0x24/0x25: máscaras de sucesos.
 * pmu_encendido_sin_soltar() hace lo mismo SIN quitar la retención (para los mains de hito: primera prueba). */
static const u8 pmu_lista[][2] = {
    {0x60,0x88},{0x61,0x10},{0x40,0x28},{0x43,0x04},{0x41,0x26},{0x42,0x42},{0x44,0x0C},{0x26,0x08},{0x27,0x72},{0x38,0x1B},
    {0xA1,0x0C},{0x50,0x80},{0xB6,0xB2},{0x72,0xF3},{0x73,0x00},{0x36,0x00},{0x63,0x00},{0xC0,0x00},{0xA0,0x00},{0xB0,0x04},
    {0xB1,0x00},{0xB2,0x00},{0xB3,0x00},{0xB4,0x00},{0xB5,0x00},{0xB0,0xF4},{0x91,0x80},{0x24,0x00},{0x25,0x00} };
/* (R2; auditoría de S7, A4) «NO CARGAR» SE ESCRIBE LO PRIMERO Y SE COMPRUEBA. Antes la lista abortaba en la primera escritura
 * fallida —y la primera era 0x60 <- 0x88—, así que 0x61 <- 0x10 podía no llegar a salir y solo lo decía una línea de la UART.
 * (R3) UN SOLO NOMBRE, el de la política («la carga FUERA», docs/bateria.md §8.A; lo que escribe el original siempre que no quiere
 * cargar: Charger_SetCurrentLimit(0)). R2 llamó pmu_carga_cero a la versión releída y dejó pmu_carga_fuera como la simple (dos
 * escrituras sin releer), que ya solo usaban los hitos: ahora hay una, la releída, con el nombre público de siempre (el que
 * exige fisico/comprobar-aparato en el ELF), y los hitos ganan la relectura.
 * pmu_carga_fuera(): 0x61 <- 0x10 y 0x63 <- 0x00, RELEÍDOS; hasta 3 intentos (cada uno levanta el veto del bus). Devuelve 0 si
 * los dos se releen como se escribieron. El resultado queda en pmu_carga (lo enseñan Sistema y un aviso fijo del shell).
 * [sin saber] si en el aparato 0x61 y 0x63 se releen tal cual: si no, saldrá «sin confirmar» con lo leído, que es la verdad.
 * La lista ya NO aborta: se intentan todas sus escrituras y se cuentan las fallidas (pmu_carga.lista_mal). */
PmuCarga pmu_carga = { 0, 0, -1, -1, 0 };
int pmu_carga_fuera(void)
{
    int i; u8 a = 0, b = 0;
    pmu_carga.confirmada = 0; pmu_carga.r61 = pmu_carga.r63 = -1;
    for (i = 0; i < 3; i++) {
        int e, la, lb;
        i2c_revivir(0);
        pmu_carga.intentos++;
        e = pmu_escribir(0x61, 0x10); e |= pmu_escribir(0x63, 0x00);
        la = pmu_leer(0x61, &a); lb = pmu_leer(0x63, &b);
        pmu_carga.r61 = la ? -1 : a; pmu_carga.r63 = lb ? -1 : b;
        if (!e && !la && !lb && a == 0x10 && b == 0x00) { pmu_carga.confirmada = 1; return 0; }
    }
    return -1;
}
int pmu_encendido_sin_soltar(void)                                /* 0 = la lista entera escrita y 0x41 releído 0x26 (la carga: pmu_carga) */
{
    u32 i; u8 v = 0; int mal = 0;
    pmu_carga_fuera();                                             /* LO PRIMERO, y con independencia del resto de la lista */
    /* (2026-10-20, tmp/acel_re/informe.md) EL DISPARO DEL ADC DEL PMU: el original, en su lista de inicio (FUN_442341b8, strh en
     * 0x442341D4, 195 de 195 arranques trazados), pone F5000800 bit 14 con la puerta 40 abierta. Sin él [inferido] el disparo de la
     * conversión lo lleva el temporizador GSM (que aquí no corre) y nuestros pulsos (0xF5000874) no convierten: el acelerómetro
     * (canales 4 y 12) y todo lo que se lee con pulso se queda en el valor viejo. El emulador no lo modela. */
    RG_E(SYSCON + 0x16, RG_L(SYSCON + 0x16) & (u16)~0x0800u);     /* puerta 40 */
    RG_E(0xF5000800, RG_L(0xF5000800) | 0x4000u);
    RG_E(SYSCON + 0x16, RG_L(SYSCON + 0x16) | 0x0800u);
    int pasada;
    /* Cada escritura de la lista se INTENTA de verdad (se levanta el veto del bus antes de cada una: aquí importa más escribirlo
     * todo que ahorrar unos ms si el PMU no contesta), y si alguna falla se da una segunda pasada entera. */
    for (pasada = 0; pasada < 2; pasada++) {
        mal = 0;
        for (i = 0; i < sizeof pmu_lista / sizeof pmu_lista[0]; i++) { i2c_revivir(0); if (pmu_escribir(pmu_lista[i][0], pmu_lista[i][1])) mal++; }
        if (!pasada) pmu_carga.lista_mal = (u8)mal;
        if (!mal) break;
    }
    i2c_revivir(0);
    pmu_carga_fuera();                                             /* y al final se RELEE: la lista no ha podido dejar otra cosa */
    return (mal || pmu_leer(0x41, &v) || v != 0x26) ? -1 : 0;     /* (como antes: de la carga NO depende soltar la retención; lo suyo va en pmu_carga) */
}
int pmu_encendido(void)                                           /* 0 = hecho: la lista y, SOLO si 0x41 se relee bien, fuera la retención */
{
    if (pmu_encendido_sin_soltar()) return -1;                    /* si el PMU no lo confirma, la retención se queda puesta */
    RG_E(0xF9050058, RG_L(0xF9050058) & (u16)~1u);
    return 0;
}
/* (S7) VBAT en marcha, como el original (HAL_SYSPWR cada 10 s): la «medida automática 9» es el registro 0x77, que
 * se LEE sin pulso ni selección (docs/bateria.md §2, §5.4). -1 = sin bus. 0x75 y 0x76 son las otras dos. */
int pmu_vbat_auto(void)
{
    u8 v;
    return pmu_leer(0x77, &v) ? -1 : v;
}
int pmu_apagar(void)                                              /* (R2) 0 = la escritura de 0x41 salió; la retención se quita siempre */
{
    int r = pmu_escribir(0x41, 0x00);
    RG_E(SYSCON + 0x16, RG_L(SYSCON + 0x16) & (u16)~0x0080u);     /* (S7) puerta 27: el RTC con su reloj. El apagado TEMPRANO (batería baja) llega aquí antes
                                                                   * de rtc_init, con la puerta cerrada por syscon_init: lo cazó el estricto de la pista E ([reloj]) */
    RG_E(0xF9050058, RG_L(0xF9050058) & (u16)~1u);
    return r;
}
int pmu_cargador(void)                                            /* bit 0 = cargador (0x23 bit 3), bit 1 = USB (bit 7); -1 = sin bus */
{
    u8 v;
    if (pmu_leer(0x23, &v)) return -1;
    return ((v >> 3) & 1) | ((v >> 6) & 2);
}
/* (CARGA, 25-oct-2026) LA CARGA POR USB, como el original (work/b/usb_carga_RE.md §5.2) y MEDIDA en el aparato (work/bitacora_CARGA.md):
 * 0x63 bits 0..1 = tensión (1 = 3,9 V, 2 = 4,0 V, 3 = 4,1 V en bornes, cargando), bits 4..5 = corriente (1 ≈ 75 mA, 3 ≈ 275 mA). El
 * original LEE EL CHIP antes de cada campo (PD_MIXSIG, 4423.c:5112-5136: el chip cambia 0x63 solo) y escribe primero la tensión y
 * después la corriente. Al llegar los bornes al techo el CHIP pone 0x63 = 00 y levanta el suceso 0x21 bit 7 [medido, E4]. Devuelve
 * 0x63 releído (puede ser 00 si el chip corta al instante: eso no es un fallo), o -1 si el I²C falló. Quien decide: api/carga.h. */
int pmu_carga_usb(int vcod, int icod)
{
    u8 r;
    if (vcod < 0 || vcod > 3 || icod < 0 || icod > 3) return -1;
    if (pmu_leer(0x63, &r) || pmu_escribir(0x63, (u8)((r & 0xFC) | vcod))) return -1;
    if (pmu_leer(0x63, &r) || pmu_escribir(0x63, (u8)((r & 0xCF) | (icod << 4)))) return -1;
    if (pmu_leer(0x63, &r)) return -1;
    pmu_carga.r63 = r;
    return r;
}
/* Los sucesos de 0x21 que han ido llegando (pmu_sucesos los lee y los borra en el chip): quien los necesita los TOMA (y los borra aquí). */
volatile u8 pmu_ev21;
u8 pmu_ev21_tomar(u8 mascara) { u8 v = pmu_ev21 & mascara; pmu_ev21 &= (u8)~mascara; return v; }
/* BDATA (la línea de identificación y temperatura de la batería), como HAL_Battery del original (0x44A13208..7E): 0x50 bits 0..2 <- 3
 * (la fuente de corriente de la línea), 400 ms, ADC 11, y 0x50 como estaba. En DOS mitades para no parar el bucle 400 ms: abrir y,
 * al menos 400 ms después, cerrar (devuelve la cuenta del ADC 11, o -1). Con 0x50 en modo 0 el canal da 0 [medido]. */
static u8 bdata_modo;
int pmu_bdata_abrir(void)
{
    u8 r;
    if (pmu_leer(0x50, &r)) return -1;
    bdata_modo = r & 7;
    return pmu_escribir(0x50, (u8)((r & 0xF8) | 3));
}
int pmu_bdata_cerrar(void)
{
    u8 r; int c = pmu_adc(11);
    if (!pmu_leer(0x50, &r)) pmu_escribir(0x50, (u8)((r & 0xF8) | bdata_modo));
    return c;
}
/* El MEDIDOR DE COMBUSTIBLE del PMU (work/b/usb_carga_RE.md §7.2): encender = 91 <- 80, 90 <- F0 (FUN_4423458c, lo que hace el
 * original al aparecer cargador o USB); el contador n se elige con 0x90 bits 2..3 y se lee en ráfaga de 3 bytes desde 0x92 (LSB
 * primero). n = 3 (sigma-delta, con signo de 17 bits) da la corriente de la batería al momento: −288 en reposo con USB, +416 cargando
 * con el código 1 y +2560 con el 3 [medido]; ≈ 9,4 cuentas por mA [inferido de que el código 1 son 75 mA]. Solo para ENSEÑARLA. */
int pmu_fg_encender(void) { return pmu_escribir(0x91, 0x80) || pmu_escribir(0x90, 0xF0) ? -1 : 0; }
int pmu_fg_leer(int n, long *v)
{
    u8 r90, reg = 0x92, b[3]; long x;
    if (n < 0 || n > 3 || pmu_leer(0x90, &r90) || pmu_escribir(0x90, (u8)((r90 & 0xF3) | (n << 2))) || i2c_escribir_leer(0, PMU, &reg, 1, b, 3)) return -1;
    x = (long)b[0] | (long)b[1] << 8 | (long)b[2] << 16;
    if (n == 3 && (x & 0x10000)) x -= 0x20000;                  /* con signo de 17 bits (44A1.c:3785-3787) */
    *v = x;
    return 0;
}
/* ADC (§12, vincenne_adc_request 0x44232F70): 0x70 <- 0xA0 | canal; PULSO de conversión (FUN_44233734: puerta 40
 * de SYSCON, F5000874 <- 0x11, retardo, F5000874 <- 0x12, puerta cerrada); leer 0x71. 14 canales.
 * [sin saber] qué es F5000874 (página de la banda base) y si sin el pulso convierte: se copia. */
#define F5_CTL   0xF5000800u                                     /* bloque de temporización de la capa GSM («HW_TG»): bit 14 = ¿disparo del ADC por programa? */
#define F5_LINEA 0xF5000874u                                     /* sus «líneas»: nibble alto = línea (1 = inicio del ADC), bajo = 1 poner / 2 quitar */
static void puerta40(int abierta)                                 /* SYSCON+16 bit 11 (= puerta 11 de la capa GSM: el reloj de ese bloque) */
{
    RG_E(SYSCON + 0x16, abierta ? (u16)(RG_L(SYSCON + 0x16) & (u16)~0x0800u) : (u16)(RG_L(SYSCON + 0x16) | 0x0800u));
}
static void adc_pulso(void)                                       /* modo A: lo de v3/v4 (puerta abierta SOLO durante el pulso) */
{
    volatile unsigned i;
    puerta40(1);
    RG_E(F5_LINEA, 0x11);
    for (i = 0; i < 200; i++) ;
    RG_E(F5_LINEA, 0x12);
    puerta40(0);
}

/* ---- (2026-10-20, tmp/acel_ag/informe.md) EL MODO DE CONVERSIÓN DEL ADC, CON AUTODETECTOR ----------------------------------
 * En el aparato (v3 y v4, la segunda ya con F5000800 bit 14) el acelerómetro no se mueve, y el emulador NO distingue ninguna de las
 * formas de convertir (convierte al escribir 0x70). En vez de apilar otro parche a ciegas, se prueban TODAS las formas candidatas
 * en el propio aparato y se usa la primera que dé lecturas VIVAS. Los modos (ADC_*, hal.h):
 *   A  0x70 <- A0|canal; pulso F5000874 11/12 con la puerta 40 abierta solo durante el pulso (200 vueltas); leer 0x71. = v3/v4.
 *   P  lo mismo, pero con la puerta 40 ABIERTA durante toda la conversión (desde antes de escribir 0x70 hasta después de leer 0x71),
 *      pulso de ~4 veces el del original y una espera de ~8 veces su retardo antes de leer. Razón [inferido]: F5000874 es una orden
 *      al bloque de temporización GSM, que solo avanza con su reloj (puerta 11/40); en el teléfono con el original la capa GSM tiene
 *      esa puerta abierta casi siempre (Do_SYSCON_ClockRequest NO la cierra si la 11 está pedida), en senew se cerraba justo detrás
 *      de escribir 0x12: el «quitar línea» podía no ejecutarse nunca y no habría más flancos.
 *   R  P precedido del pulso de 0x72 (F3 -> F0 -> F3) que hace el original justo antes de su PRIMERA conversión con pulso (gestor de
 *      batería, orden 4 de 0xA46; traza e14 l. 5326-5331) y en cada «AD_TX» (FUN_4423327c / FUN_442332b8: 72 &= F0 ... 72 |= 3).
 *   B  P con F5000800 bit 14 A 0 durante la conversión (por si la polaridad es la contraria a la supuesta; el original lo baja en su
 *      orden 7 de 0xA43). Se repone al acabar.
 *   I  0x70 <- 60|canal y leer 0x71, SIN pulso: la forma del 0x6A (= 60|10), que SÍ convierte en el aparato.
 *   J  I con la espera larga antes de leer.
 *   S  0x70 <- A0|canal, espera larga, leer, SIN pulso.
 * «VIVO» (adc_probar_modo): antes de cada conversión de prueba se ENVENENA 0x71 con la lectura en frío (0x6A: 0x71 = cuenta de la
 * batería); un modo que no convierte devuelve ese veneno. Se convierten los canales 1 (temperatura de placa), 8 (tensión del
 * cargador), 4 (Y) y 12 (X); vivo = ninguna lectura fallida, como mucho UNA igual al veneno y al menos TRES valores distintos, en
 * las DOS rondas, y el canal 1 estable entre rondas (±8). Se elige el primer modo vivo en el orden A P R B I J S (del más parecido al
 * original al menos); si ninguno vive se sigue con A y se reintenta cada 10 s (5 veces). El modo vale para TODO pmu_adc() (también
 * temperatura y cargador). Se puede FORZAR un modo (Sistema -> Opciones -> «Banco acel/audio», tecla 3) y repetir la prueba (9).
 * Seguridad: solo se escribe en 0x70 (selector del ADC), 0x72 (los valores del original, F3/F0) y F5000800 bit 14 (que el original
 * pone y quita); el sensor se alimenta (P1.1) lo que dura la prueba si no lo estaba. [sin saber] qué bit de 0x70 es qué: solo el
 * aparato dirá cuál vive. */
#ifndef ADC_RETARDO_PULSO
#define ADC_RETARDO_PULSO (0x19u * 4u)                            /* hal_retardo: 0x19 = el pulso del original (~12-25 µs) */
#endif
#ifndef ADC_RETARDO_ESPERA
#define ADC_RETARDO_ESPERA (0x19u * 8u)
#endif
AdcDiag adc_dg = { .modo = ADC_A, .forzado = -1, .x = -1, .y = -1 };
static u32 adc_tic_prueba;
static int adc_conv(int modo, u32 canal)                          /* una conversión en el modo dado; -1 = el bus no contesta */
{
    u8 v; int r; u16 c = 0;
    if (canal > 13) return -1;
    switch (modo) {
    case ADC_A:
        if (pmu_escribir(0x70, (u8)(0xA0 | canal))) return -1;
        adc_pulso();
        break;
    case ADC_P: case ADC_R: case ADC_B:
        if (modo == ADC_R && (pmu_escribir(0x72, 0xF0) || pmu_escribir(0x72, 0xF3))) return -1;
        puerta40(1);
        if (modo == ADC_B) { c = RG_L(F5_CTL); RG_E(F5_CTL, (u16)(c & (u16)~0x4000u)); }
        r = pmu_escribir(0x70, (u8)(0xA0 | canal));
        if (!r) {
            RG_E(F5_LINEA, 0x11); hal_retardo(ADC_RETARDO_PULSO); RG_E(F5_LINEA, 0x12);
            hal_retardo(ADC_RETARDO_ESPERA);
            r = pmu_leer(0x71, &v);
        }
        if (modo == ADC_B) RG_E(F5_CTL, c);
        puerta40(0);
        return r ? -1 : v;
    case ADC_I: case ADC_J:
        if (pmu_escribir(0x70, (u8)(0x60 | canal))) return -1;
        if (modo == ADC_J) hal_retardo(ADC_RETARDO_ESPERA);
        break;
    case ADC_S:
        if (pmu_escribir(0x70, (u8)(0xA0 | canal))) return -1;
        hal_retardo(ADC_RETARDO_ESPERA);
        break;
    default: return -1;
    }
    return pmu_leer(0x71, &v) ? -1 : v;
}
static const u8 adc_canal_prueba[4] = { 1, 8, 4, 12 };
static int adc_probar_modo(int m, int ronda)                      /* rellena adc_dg para el modo m; 1 = vivo en esta ronda */
{
    int k, j, viejos = 0, dist = 0, malas = 0;
    for (k = 0; k < 4; k++) {
        int p, v;
        i2c_revivir(0);
        p = pmu_bateria();                                         /* el VENENO: 0x71 queda con la cuenta de la batería */
        v = adc_conv(m, adc_canal_prueba[k]);
        if (!k) adc_dg.veneno[m] = (short)p;
        if (v < 0 || p < 0) { malas++; v = -1; } else if (v == p) viejos++;
        if (ronda && (adc_dg.vivo[m] & 1) && !k && v >= 0 && adc_dg.v[m][0] >= 0 && (v - adc_dg.v[m][0] > 8 || adc_dg.v[m][0] - v > 8)) malas++;   /* canal 1 inestable */
        adc_dg.v[m][k] = (short)v;
    }
    for (k = 0; k < 4; k++) {
        if (adc_dg.v[m][k] < 0) continue;
        for (j = 0; j < k && adc_dg.v[m][j] != adc_dg.v[m][k]; j++) ;
        if (j == k) dist++;
    }
    adc_dg.viejos[m] = (u8)viejos; adc_dg.distintos[m] = (u8)dist;
    return !malas && viejos <= 1 && dist >= 3;
}
int adc_autodetectar(void)                                        /* prueba todos los modos; devuelve el elegido (o -1 si ninguno vive) */
{
    static const u8 orden_prueba[ADC_MODOS] = { ADC_A, ADC_P, ADC_B, ADC_I, ADC_J, ADC_S, ADC_R };   /* R lo último: cambia 0x72 */
    static const u8 preferencia[ADC_MODOS] = { ADC_A, ADC_P, ADC_R, ADC_B, ADC_I, ADC_J, ADC_S };
    int ronda, k, encendido = gpio_get(1, 0x02), elegido = -1;
    if (!encendido) { acel_alimentar(1); espera_ms(20); }          /* el original nunca convierte los canales 4/12 sin alimentar */
    for (k = 0; k < ADC_MODOS; k++) adc_dg.vivo[k] = 0;
    for (ronda = 0; ronda < 2; ronda++)
        for (k = 0; k < ADC_MODOS; k++) if (adc_probar_modo(orden_prueba[k], ronda)) adc_dg.vivo[orden_prueba[k]] |= (u8)(1u << ronda);
    if (!encendido) acel_alimentar(0);
    for (k = 0; k < ADC_MODOS; k++) {
        int m = preferencia[k];
        if (adc_dg.vivo[m] == 3 && adc_dg.v[m][2] > 10 && adc_dg.v[m][2] < 245 && adc_dg.v[m][3] > 10 && adc_dg.v[m][3] < 245) { elegido = m; break; }
    }
    if (elegido < 0)                                                /* ningún modo con el sensor en rango: al menos uno con el ADC vivo */
        for (k = 0; k < ADC_MODOS; k++) if (adc_dg.vivo[preferencia[k]] == 3) { elegido = preferencia[k]; break; }
    adc_dg.hallado = (u8)(elegido >= 0);
    adc_dg.auto_modo = (signed char)elegido;
    adc_dg.intentos++; adc_dg.probado = 1; adc_tic_prueba = tics;
    if (adc_dg.forzado < 0) adc_dg.modo = (u8)(elegido >= 0 ? elegido : ADC_A);
    return elegido;
}
void adc_forzar(int modo)                                         /* 0..ADC_MODOS-1 = ese modo siempre; -1 = automático (se vuelve a probar) */
{
    if (modo >= ADC_MODOS) modo = -1;
    adc_dg.forzado = (signed char)modo;
    if (modo >= 0) adc_dg.modo = (u8)modo;
    else { adc_dg.probado = 0; adc_dg.intentos = 0; }
}
static void adc_quizas_probar(void)                               /* la prueba, perezosa: la primera vez y, sin modo vivo, cada 10 s (5 veces) */
{
    if (adc_dg.forzado >= 0 || adc_dg.probando) return;
    if (adc_dg.probado && (adc_dg.hallado || adc_dg.intentos >= 5 || (u32)(tics - adc_tic_prueba) < 2500u)) return;
    adc_dg.probando = 1;
    adc_autodetectar();
    adc_dg.probando = 0;
}
int pmu_adc(u32 canal)                                            /* lectura cruda de 8 bits del canal 0..13 en el modo en uso; -1 = sin bus */
{
    if (canal > 13) return -1;
    adc_quizas_probar();
    return adc_conv(adc_dg.modo, canal);
}
int pmu_adc_modo(int modo, u32 canal) { return modo < 0 || modo >= ADC_MODOS ? -1 : adc_conv(modo, canal); }
/* LA BATERÍA, como la mide el original: UNA vez, en su arranque temprano (FUN_449cdb04; traza e14 l. 548-549
 * [observado]): 0x70 <- 0x6A y leer 0x71, SIN el pulso de conversión (el pulso es de las conversiones 0xA0 | canal).
 * Corregido tras la lista de la pista E4: antes se daba el pulso detrás del 0x6A, una mezcla que el original no hace.
 * [sin saber] si 0x6A vale con el PMU ya iniciado (el original lo usa antes de su lista de inicio) y qué canal usa
 * el original para la batería EN MARCHA (convierte los 1, 4, 8, 11, 12 y 13; nunca el 10 con 0xA0). */
int pmu_bateria(void)
{
    u8 v;
    if (pmu_escribir(0x70, 0x6A)) return -1;
    if (pmu_leer(0x71, &v)) return -1;
    return v;
}

/* Sucesos del PMU por la IRQ 37 («VINCENNE»): 0x21 y 0x22 son los sucesos pendientes (se borran al leer; 0x21
 * bit 0 = la tecla de encendido ha cambiado) y 0x23 el estado (bit 0 = tecla pulsada, bit 3 cargador, bit 7 USB).
 * Las máscaras se ponen como el inicio del original (4423.c l. 4537..4563): 0x26 <- 0x08, 0x27 <- 0x72, 0x24 <- 0,
 * 0x25 <- 0. El I²C va por sondeo y NO se toca desde la interrupción: el manejador solo levanta un aviso y TAPA
 * la línea (si en el aparato es de nivel, no puede haber tormenta); el bucle lee los sucesos —eso baja la línea—
 * y la destapa. [emulador: el modelo de E da el suceso de la tecla]; en un teléfono, sin probar. */
volatile u8 pmu_pendiente;
static void pmu_irq(void) { pmu_pendiente = 1; intc_tapar(37); }
void pmu_irq_init(void)
{
    if (pmu_escribir(0x26, 0x08) || pmu_escribir(0x27, 0x72) || pmu_escribir(0x24, 0x00) || pmu_escribir(0x25, 0x00)) return;
    pmu_pendiente = 1;                                            /* la primera pasada lee lo que hubiera (la causa del encendido) */
    intc_habilita(37, pmu_irq);
    intc_tapar(37);
}
int pmu_sucesos(u8 *estado)
{
    u8 a, b;
    if (!pmu_pendiente) return 0;
    pmu_pendiente = 0;
    if (!pmu_leer(0x21, &a)) pmu_ev21 |= a;                         /* (CARGA) bit 7 = «USB voltage reached»: lo toma la carga */
    pmu_leer(0x22, &b);
    if (pmu_leer(0x23, estado)) { *estado = 0; intc_destapar(37); return -1; }   /* (R2) -1 = hubo suceso pero 0x23 NO se pudo leer: *estado no vale */
    intc_destapar(37);
    return 1;
}

/* ---- acelerómetro (§13): ANALÓGICO de dos ejes, por el ADC del PMU. Alimentación: P1.1 (accel_power_on
 * 0x453FEF64). Y = canal 4, X = canal 12 (accel_read_xy 0x453FEFD4). El cero anda por 125 cuentas.
 * [sin saber] sensibilidad (cuentas por g) y orientación de los ejes. [emulador] propiedades pmu-adc4/12. */
void acel_alimentar(int si)
{
    gpio_dir(1, 0x02, 1);
    gpio_set(1, 0x02, si);
}
int acel_leer(int *x, int *y)
{
    int a = pmu_adc(4), b = pmu_adc(12);
    adc_dg.lecturas++;
    if (a < 0 || b < 0) { adc_dg.fallos++; return -1; }
    *y = a; *x = b;
    adc_dg.x = (short)b; adc_dg.y = (short)a;                     /* (acel_ag) lo último leído y el rango visto, para el banco */
    if (!adc_dg.rango_hay) { adc_dg.xmin = adc_dg.xmax = (short)b; adc_dg.ymin = adc_dg.ymax = (short)a; adc_dg.rango_hay = 1; }
    if (b < adc_dg.xmin) adc_dg.xmin = (short)b;
    if (b > adc_dg.xmax) adc_dg.xmax = (short)b;
    if (a < adc_dg.ymin) adc_dg.ymin = (short)a;
    if (a > adc_dg.ymax) adc_dg.ymax = (short)a;
    return 0;
}

/* ---- VIBRADOR y LED del PMU (§12) ------------------------------------------------------------------------------
 * [medido en el aparato, 25-oct-2026: el ACELERÓMETRO del propio teléfono muestreado a ~717 Hz mientras se escribe
 * 0xA0 por el puente; herr/puente/vibrador/, docs/reconocimiento_original.md §2] 0xA0 (lógico 0x2C) MUEVE EL MOTOR
 * (arranca en ~50-100 ms y frena en ~50-100 ms) y es un PATRÓN, no una fuerza:
 *   0 = parado; bit 5 = en marcha;
 *   bits 3..4 = FRECUENCIA del patrón: 0x20 1 Hz, 0x28 2 Hz, 0x30 4 Hz, 0x38 8 Hz (medidos 1,0 s, 525, 262 y 125 ms);
 *   bits 0..1 = CICLO de trabajo: 0 = 100 % (continuo), 1 = 75 %, 2 = 50 %, 3 = 25 % (medidos 98, 81, 52 y 27 %).
 *   Con el 100 % la frecuencia no cuenta (0x20 = 0x28 = 0x30 = 0x38). El patrón lo lleva un temporizador del PMU que
 *   corre suelto: la primera ráfaga sale en cualquier punto del periodo, así que un TOQUE CORTO tiene que ir continuo
 *   (con 0x23 un toque de 150 ms puede caer entero en la parte apagada). A 8 Hz y 25 % (31 ms) el motor casi no arranca.
 * 0xA1 (lógico 0x2D) nibble bajo = NIVEL 5..12 (lo que admite la orden 2 de la señal 0xA47), [medido] la TENSIÓN del
 *   motor: con 5 no arranca; con 8, 10 y 12 sí, antes cuanto más alto, y gira a otra velocidad (el espectro se mueve).
 *   El original lo pone a 12 (0x0C) en su lista de inicio y no lo cambia nunca (nadie manda la orden 2).
 * El original [observado en el pseudo-C y el desensamblado]: el arranque temprano (FUN_449cdc4c), al encender con la
 * tecla: 0xA1 <- 0x0C, 0xA0 <- 0x20, 75 ms, 0xA0 <- 0. El proceso «User I/O» (señal 0xAC00680 {periodo_ms, %},
 * CNH160637_user_io_ld): a = la más cercana de {1, 2, 4, 8} a 1000 / periodo_ms (tablas 0x4427FB84 y 0x4427FB7C) y
 * b = el más cercano de {25, 50, 75, 100} % (0x4427FB8C y 0x4427FB80); el reproductor de SMAF pide (1000, 100) = 0x20.
 * AudioControl_Vibrate(p, 1500, 0, 1500) escribió 0x23 en el aparato (1 Hz al 25 %).
 * NO hace falta SYSCON [medido]: el motor arranca con +18 = 0x01C1 y +2C = 0x0FFF tal cual. */
static u8 vib_nivel = 12;
static int vib_indice(const u16 *t, int v)                         /* el más cercano en una tabla creciente de 4, como FUN_4422b7a8 */
{
    int i;
    for (i = 0; i < 4; i++) if (v <= t[i]) return !i || (int)t[i] - v < v - (int)t[i - 1] ? i : i - 1;
    return 3;
}
int pmu_vibrador_nivel(int n)
{
    u8 v;
    if (n < 5 || n > 12) return -1;
    vib_nivel = (u8)n;
    if (pmu_leer(0xA1, &v)) return -1;
    return pmu_escribir(0xA1, (u8)((v & 0xF0) | vib_nivel)) ? -1 : 0;
}
int pmu_vibrador_patron(int hz, int pct)
{
    static const u16 frec[4] = { 1, 2, 4, 8 }, ciclo[4] = { 25, 50, 75, 100 };
    u8 v;
    if (hz <= 0 || pct <= 0) return pmu_escribir(0xA0, 0x00) ? -1 : 0;
    if (pmu_leer(0xA1, &v)) return -1;
    if (pmu_escribir(0xA1, (u8)((v & 0xF0) | vib_nivel))) return -1;
    return pmu_escribir(0xA0, (u8)(0x20 | (vib_indice(frec, hz) << 3) | (3 - vib_indice(ciclo, pct)))) ? -1 : 0;
}
int pmu_vibrador(int si) { return pmu_vibrador_patron(si ? 1 : 0, 100); }   /* continuo: 0xA1 = nivel (12 de inicio), 0xA0 = 0x20 */
/* Los dos «LED» del registro físico 0x36 (lógico 8): RedLED_On / RedLED_Off (0x44235358 / 0x442353A4, nombres de
 * la biblioteca de elfs) son las órdenes 3 y 4 de la señal 0xA47 [observado]: encender n = 0 pone el bit 1, n = 1 el
 * bit 0; apagar los quita; los bits 3..5 son un nivel (orden 5). [mirado en el aparato, 25-oct-2026] con el nivel a 0 (como el
 * original) ninguno de los dos bits enciende nada visible en el W580i: lo más probable, sin LED montado. */
int pmu_led(int n, int si)
{
    u8 v, m = n ? 0x01 : 0x02;
    if (pmu_leer(0x36, &v)) return -1;
    return pmu_escribir(0x36, si ? (u8)(v | m) : (u8)(v & ~m)) ? -1 : 0;
}

/* ---- CONTROLADOR DE LED por I²C 1, 0x40 («DCDC LED»; DD_drv_Backlight.c, 0x4529C744..0x4529E188) ----------------
 * (Pista S4.) El mecanismo, [observado en el pseudo-C]; lo que el original le escribe de verdad, [observado en el
 * emulador con el chip contestando: propiedad de máquina i2c=0.0x4a:1.0x40, pista E5, trazas e5_c2led_*]. El chip del
 * emulador es un banco de registros: NADA de esto se ha visto encender un LED. [sin saber] qué chip es, qué LED cuelga
 * de cada salida y qué corriente es cada nivel.
 *  - registros: leddcdc_reg_write(reg, v) = I²C 1, 0x40, {reg, v}; el original lleva una sombra de cada uno y solo
 *    escribe lo que cambia;
 *  - inicio (FUN_4529deb8): escribe TODA la sombra, que nace a 0 salvo 0x16 = 1, 0x1F = 0x0C, 0x22 = 4, 0x29 = 1;
 *    registros 0x00..0x05, 0x08..0x1D, 0x1F..0x26, 0x29, 0x2C..0x35; y lee 0x37 y 0x38 (visto ejecutar así);
 *  - LUCES SUELTAS 1, 4 y 2 (FUN_4529c744 / FUN_4529c810): el elevador (FUN_4529d930(1): 0x22 |= 0x84, 0x21 = 0x9A,
 *    0x00 |= 0x08), el campo de la luz a 1 (FUN_4529da78: luz 1 -> registro 1 bits 2..3; luz 4 -> registro 1
 *    bits 0..1; luz 2 -> registro 2 bits 6..7) y el nivel (FUN_4529da10: luz 1 -> 0x0A, luz 4 -> 0x09, luz 2 -> 0x2F).
 *    Apagarla: campo a 0, nivel 0 y, si era la última, FUN_4529d930(0): 0x00 &= ~0x08;
 *  - GRUPOS DE TRES («RGB» por cómo los usa: FF,FF,FF en el standby y FF,7F,00 al pulsar la tecla Walkman, visto en
 *    la traza; [sin saber] qué LED son), grupo 0 y 1 (FUN_4529ca74(grupo, a, b, c, 2, 0) / FUN_4529c9ec): la primera
 *    vez FUN_4529d990(1) (0x24 = 0x7F, 0x25 = 0x7C, 0x23 = 9, 0x00 |= 0x04); FUN_4529dca4(2) (0x16 = 0x11 sobre el 1
 *    inicial), FUN_4529dcf0(0) (0x16 &= ~6, 0x17 = 0 SIEMPRE), los tres campos del grupo a 2 (FUN_4529dbb4: grupo 0
 *    -> registro 2 bits 0..5; grupo 1 -> registro 4 bits 0..5), los tres niveles (FUN_4529daf4: 0x0B..0x0D /
 *    0x13..0x15) y FUN_4529db70(0) (0x16 |= 2). Apagarlo: campos a 0, niveles a 0 y, sin grupos, 0x00 &= ~0x04.
 *    [sin saber] qué son 0x16, 0x17 y 0x23..0x25; se escribe lo que escribe el original, en su orden.
 * LOS MÁXIMOS. El original, en todo lo ejecutado, no pasa de 0x05 en 0x09 y 0x0A ni de 0x84 en 0x2F (0x84 con el
 * logo, 0x2F en el standby, 0x06 atenuado); en los grupos escribe 0xFF. (S5) Pero 0x05 es su 36 %: el 100 % de su CÓDIGO
 * en 0x09/0x0A es 0x10 (la traza solo cuadra con ese rango: docs/luces.md §4) y lo escribe cualquier efecto a intensidad
 * 100. ledc_escribir() RECORTA a 0x10 y 0x84: ningún camino de este árbol puede pedirle al chip más que el original
 * (regla [luz] del emulador estricto). El standby y el hito 6 siguen sin pasar de 0x05 (LEDC_MAX_TECLADO).
 * (La «luz 0» de ese mismo módulo no pasa por el chip: son los dos PWM del GPIO, pwm_luz_dd() en gpio.c; el original
 * no la enciende nunca en lo ejecutado.) */
#define LEDC 0x40
static u8 ledc_sombra[0x36];
static u8 ledc_listo, ledc_en_uso, ledc_grupos;
static void gr_olvidar(void);
u32 ledc_rampas;                                                  /* (S5) rampas del PWM arrancadas (plat lo mira para saber cuándo empezó la última) */
i2c_cuentas ledc_cta;                                             /* escrituras bien / topes (= no contesta) del chip */
int ledc_escribir(u8 reg, u8 v)
{
    u8 b[2];
    if ((reg == 0x09 || reg == 0x0A) && v > LEDC_TOPE_TECLADO) v = LEDC_TOPE_TECLADO;   /* nunca por encima del original */
    if (reg == 0x2F && v > LEDC_MAX_PANTALLA) v = LEDC_MAX_PANTALLA;
    b[0] = reg; b[1] = v;
    if (i2c_escribir(1, LEDC, b, 2)) { ledc_cta.nack++; return -1; }
    if (reg < sizeof ledc_sombra) ledc_sombra[reg] = v;
    if (reg == 0x16 && (v & 6)) ledc_rampas++;                    /* (S5) con un bit de sentido arranca una rampa del PWM */
    ledc_cta.escrituras++;
    return 0;
}
static int ledc_si_cambia(u8 reg, u8 v) { return ledc_sombra[reg] == v ? 0 : ledc_escribir(reg, v); }   /* como el original: solo lo que cambia */
int ledc_leer(u8 reg, u8 *v) { return i2c_escribir_leer(1, LEDC, &reg, 1, v, 1); }
int ledc_sombra_de(u8 reg) { return ledc_listo && reg < sizeof ledc_sombra ? ledc_sombra[reg] : -1; }   /* lo último escrito; -1 = chip sin iniciar */
int ledc_init(void)                                               /* 0 = el chip contestó a todo el inicio */
{
    u32 r; u8 v;
    for (r = 0; r < sizeof ledc_sombra; r++) ledc_sombra[r] = 0;
    ledc_sombra[0x16] = 1; ledc_sombra[0x1F] = 0x0C; ledc_sombra[0x22] = 4; ledc_sombra[0x29] = 1;
    ledc_listo = 0; ledc_en_uso = 0; ledc_grupos = 0; gr_olvidar();
    for (r = 0; r <= 0x35; r++) {
        if (r == 6 || r == 7 || r == 0x1E || r == 0x27 || r == 0x28 || r == 0x2A || r == 0x2B) continue;
        if (ledc_escribir((u8)r, ledc_sombra[r])) return -1;      /* como el original: al primer fallo, lo deja */
    }
    if (ledc_leer(0x37, &v) || ledc_leer(0x38, &v)) return -1;
    ledc_listo = 1;
    return 0;
}
/* Luz n = 1, 2 o 4 al nivel «nivel» DEL REGISTRO (0 = apagada; se recorta al máximo del original). -1 = no contesta.
 * El orden de las escrituras es el de FUN_4529c744 (elevador, campo, nivel) y FUN_4529c810 (campo, nivel, elevador). */
int ledc_luz(int n, u32 nivel)
{
    u8 reg_campo = n == 2 ? 2 : 1, despl = n == 1 ? 2 : n == 2 ? 6 : 0, reg_nivel = n == 1 ? 0x0A : n == 2 ? 0x2F : 0x09;
    u8 bit = n == 1 ? 1 : n == 2 ? 2 : 4, campo, tope = n == 2 ? LEDC_MAX_PANTALLA : LEDC_TOPE_TECLADO;
    if (n != 1 && n != 2 && n != 4) return -1;
    if (!ledc_listo && ledc_init()) return -1;
    if (nivel > tope) nivel = tope;
    campo = (u8)((ledc_sombra[reg_campo] & ~(3u << despl)) | ((nivel ? 1u : 0u) << despl));
    if (nivel) {
        if (!ledc_en_uso) {                                       /* el elevador, la primera vez */
            if (ledc_si_cambia(0x22, (u8)(ledc_sombra[0x22] | 0x84)) || ledc_si_cambia(0x21, 0x9A) ||
                ledc_si_cambia(0x00, (u8)(ledc_sombra[0x00] | 0x08))) return -1;
        }
        ledc_en_uso |= bit;
        if (ledc_si_cambia(reg_campo, campo) || ledc_si_cambia(reg_nivel, (u8)nivel)) return -1;
    } else {
        if (ledc_si_cambia(reg_campo, campo) || ledc_si_cambia(reg_nivel, 0)) return -1;
        ledc_en_uso &= (u8)~bit;
        if (!ledc_en_uso && ledc_si_cambia(0x00, (u8)(ledc_sombra[0x00] & ~0x08u))) return -1;
    }
    return 0;
}
/* ---- LAS DOS TERNAS RGB (pista S5; docs/luces.md §2.3) -------------------------------------------------------------
 * El estado es el del original: de cada terna, el OBJETIVO pedido, lo ESCRITO y el último sentido (FUN_4529cfa0: +0, +1 y
 * +0x16 del primer canal). Con él decide, como FUN_4529ca74, cuándo NO hay que escribir nada. */
static u8 gr_obj[2][3], gr_act[2][3], gr_sent[2];
static void gr_olvidar(void) { u32 i; for (i = 0; i < 6; i++) { gr_obj[i / 3][i % 3] = 0; gr_act[i / 3][i % 3] = 0; } gr_sent[0] = gr_sent[1] = 0; }
static int gr_bomba(int si)                                       /* FUN_4529d990: la bomba de carga de las ternas */
{
    if (si) return ledc_si_cambia(0x24, 0x7F) || ledc_si_cambia(0x25, 0x7C) || ledc_si_cambia(0x23, 0x09) ||
                   ledc_si_cambia(0x00, (u8)(ledc_sombra[0x00] | 0x04));
    return ledc_si_cambia(0x00, (u8)(ledc_sombra[0x00] & ~0x04u)) || ledc_si_cambia(0x23, 0x09);
}
static int gr_campos(int g, u8 modo)                              /* FUN_4529dbb4: los tres campos de 2 bits a «modo», si NINGUNO lo vale ya */
{
    u8 reg = g ? 4 : 2, s = ledc_sombra[reg];
    if ((s & 3) == modo || ((s >> 2) & 3) == modo || ((s >> 4) & 3) == modo) return 0;
    return ledc_escribir(reg, (u8)((s & 0xC0) | (modo * 0x15u)));
}
static int gr_niveles(int g, const u8 *n)                         /* FUN_4529daf4 */
{
    u8 reg = g ? 0x13 : 0x0B;
    return ledc_si_cambia(reg, n[0]) || ledc_si_cambia((u8)(reg + 1), n[1]) || ledc_si_cambia((u8)(reg + 2), n[2]);
}
static int gr_pwm_valor(u8 v)                                     /* FUN_4529dcf0: quita el sentido y pone el valor del PWM (0x17 se escribe SIEMPRE) */
{
    return ledc_si_cambia(0x16, (u8)(ledc_sombra[0x16] & 0xF9)) || ledc_escribir(0x17, v);
}
static int gr_entra(int g, u8 r, u8 v, u8 a)                      /* lo común de ca74 y c8b4: la bomba la primera vez y la cuenta de ternas */
{
    if (!ledc_listo && ledc_init()) return -1;
    if (!ledc_grupos && gr_bomba(1)) return -1;
    if (!gr_act[g][0] && !gr_act[g][1] && !gr_act[g][2]) ledc_grupos |= (u8)(g ? 2 : 1);
    gr_obj[g][0] = r; gr_obj[g][1] = v; gr_obj[g][2] = a;
    return 0;
}
int ledc_grupo_fundir(int g, u8 r, u8 v, u8 a, u8 vel, int bajar)
{
    int cero, igual;
    if ((g != 0 && g != 1) || (!r && !v && !a)) return -1;        /* el original lo rechaza (error 2) */
    bajar = bajar != 0;
    cero = !gr_act[g][0] && !gr_act[g][1] && !gr_act[g][2];
    if (gr_entra(g, r, v, a)) return -1;
    igual = gr_act[g][0] == r && gr_act[g][1] == v && gr_act[g][2] == a;
    if ((igual && gr_sent[g] == bajar) || (cero && bajar)) { gr_sent[g] = (u8)bajar; return 0; }   /* ya está así, o bajar lo que está apagado */
    gr_act[g][0] = r; gr_act[g][1] = v; gr_act[g][2] = a;
    gr_sent[g] = (u8)bajar;
    if (ledc_sombra[0x18] & 8) { if (ledc_escribir(0x18, (u8)(ledc_sombra[0x18] & ~8u))) return -1; }   /* FUN_4529dca4(vel) */
    if (ledc_si_cambia(0x16, (u8)(((vel & 7u) << 3) | (ledc_sombra[0x16] & 0xC7) | 1))) return -1;
    if (gr_pwm_valor(bajar ? 0xFF : 0x00)) return -1;             /* sube desde 0 o baja desde FF */
    if (gr_campos(g, 2) || gr_niveles(g, gr_act[g])) return -1;
    return ledc_si_cambia(0x16, bajar ? (u8)(4 | (ledc_sombra[0x16] & 0xFD)) : (u8)(2 | (ledc_sombra[0x16] & 0xFB))) ? -1 : 0;   /* FUN_4529db70 */
}
int ledc_grupo_fijo(int g, u8 r, u8 v, u8 a, u8 modo)
{
    if ((g != 0 && g != 1) || (!r && !v && !a) || modo < 1 || modo > 3) return -1;
    if (gr_entra(g, r, v, a)) return -1;
    gr_act[g][0] = r; gr_act[g][1] = v; gr_act[g][2] = a;
    if (gr_pwm_valor(0xFF)) return -1;
    return gr_niveles(g, gr_act[g]) || gr_campos(g, modo) ? -1 : 0;   /* aquí los niveles van ANTES que los campos */
}
int ledc_grupo_apagar(int g)
{
    if (g != 0 && g != 1) return -1;
    if (!ledc_listo) return ledc_init() ? -1 : 0;                 /* recién iniciado ya está todo a 0 */
    if (!gr_obj[g][0] && !gr_obj[g][1] && !gr_obj[g][2]) return 0;
    gr_obj[g][0] = gr_obj[g][1] = gr_obj[g][2] = 0;
    gr_act[g][0] = gr_act[g][1] = gr_act[g][2] = 0;
    if (gr_campos(g, 0) || gr_niveles(g, gr_act[g])) return -1;
    ledc_grupos &= (u8)~(g ? 2u : 1u);
    return !ledc_grupos && gr_bomba(0) ? -1 : 0;
}
/* Grupo de tres g = 0 o 1 con los niveles a, b, c (0..255; los tres a 0 = apagado): lo de S4, que es fundir hacia arriba
 * a la velocidad 2 (la del original en su standby) o apagar. -1 = no contesta. */
int ledc_grupo(int g, u8 a, u8 b, u8 c)
{
    return a || b || c ? ledc_grupo_fundir(g, a, b, c, 2, 0) : ledc_grupo_apagar(g);
}
