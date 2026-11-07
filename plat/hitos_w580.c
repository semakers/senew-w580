/* Los mains MÍNIMOS de los hitos del W580i (make w580 DEFS=-DHITO=n B=build/hito_n), a la manera de los de
 * w600/propio: cada uno prueba UNA cosa sobre la HAL, sin framework. docs/sistema.md §3.
 *   HITO=2  arranque + UART 0: escribe y cuenta
 *   HITO=3  + pantalla: barras de color por el PDICON
 *   HITO=4  + tic: el reloj avanza por la IRQ 53 del temporizador 1 (se ve por la UART y en pantalla)
 *   HITO=5  + teclado: enseña la celda pulsada
 *   HITO=7  (R3; AUD3-M3) EL PASO INTERMEDIO antes del sistema: lo que el hito 5 NO estrena y el sistema estrenaba de golpe —
 *           MMU + cachés (mmu_cache_init, con la limpieza de caché de lcd_enviar ya de verdad), el tic ANTES de la lista del
 *           PMU (como plat/hw_w580.c) y pmu_encendido(), que SUELTA LA RETENCIÓN de la alimentación—, más el teclado. Si
 *           este va y el sistema sin escritura no, lo que falla es de plat/ para arriba, no de esto. No toca la NOR.
 *   HITO=8  (R3; propuesta P4) SOLO MEDIDA, FUERA DEL ORDEN: el perro de HARDWARE. Ver «hito 8» más abajo. PUEDE REINICIARSE SOLO.
 *   HITO=6  LUCES: enciende de una en una cada luz candidata (PMU, PWM A, PWM B, los dos PWM, chip de LED: luces 1, 2 y
 *           4 y los dos grupos de tres; los LED del PMU) y el vibrador, diciendo por la UART cuál; para VER cuál es cuál
 *
 * SON LO PRIMERO QUE SE PROBARÍA EN UN APARATO (hallazgo M11 de la pista R), así que:
 *  - NINGUNO escribe en la NOR, y ninguno salvo el 7 quita la retención de la alimentación (F9050058 bit 0 se queda como lo
 *    deja arranque.S; la lista del PMU se escribe con pmu_encendido_sin_soltar()); el 7 la suelta A PROPÓSITO, como el sistema;
 *  - (R3) TODOS SE PUEDEN APAGAR: la tecla de encendido mantenida 3 s (leída como NIVEL de ahora en el PMU, 0x23 bit 0, y solo
 *    si antes se la ha visto SUELTA: la pulsación que encendió el teléfono no cuenta) apaga con 0x41 <- 0, como el sistema.
 *    Antes la única salida de un hito era quitar la batería;
 *  - el hito 2 va a 13 MHz, sin PLL: lo mínimo. Del 3 en adelante suben a 208 MHz como hace el original antes de
 *    encender XGAM ([sin saber] si el bloque gráfico vive a 13); -DHITO_13MHZ los deja a 13;
 *  - TODOS dan DOS SEÑALES DE VIDA que no dependen ni de la pantalla ni del cable, las dos del arranque temprano del
 *    original (FUN_449cdc4c): (1) el pulso de la LUZ del PMU (0x45 <- 0x50, 0x43 <- 0x80; el original la tiene
 *    encendida de 0,12 a 0,58 s y su lista de inicio la apaga: aquí igual, 460 ms) y (2) el pulso del vibrador (traza
 *    e14 l. 555, 556 y 595 [observado]: 0xA1 <- 0x0C, 0xA0 <- 0x20, ~75 ms, 0xA0 <- 0x00). El hito 2 repite el del
 *    vibrador cada 2 s. [inferido] que eso mueva el vibrador (hal/i2c.c) y [sin saber] si la luz del PMU ilumina
 *    algo; si no se nota nada, no dice que el main no corra;
 *  - (S4) del 3 en adelante encienden LA LUZ QUE ENCIENDE EL ORIGINAL con su logo: la luz 2 del chip de LED (I²C 1,
 *    0x40; registro 0x2F <- 0x84), y nada más. Con -DLUCES_TODAS=1, además, las demás candidatas a la vez (PMU, los
 *    dos PWM, las luces 1 y 4 del chip y sus dos grupos): es DIAGNÓSTICO, por si con la del original el panel se
 *    queda a oscuras; el original no enciende nunca PMU ni PWM en marcha. Los niveles del chip nunca pasan de los
 *    máximos del original (hal/i2c.c los recorta). */
#ifdef HITO
#include "hal.h"
#include "bat.h"
#if HITO == 7
#include "hw.h"                                    /* solo HW_VERSION_SISTEMA, para el rótulo */
#endif

static u16 cuadro[LCD_ANCHO * LCD_ALTO] __attribute__((aligned(32), section(".gram")));   /* 0x20E80000, como el original */
#define C565(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))   /* nativo: lo que lee el PDICON */

/* (R3) noinline: metida dentro de main, gcc la convertía en un bucle de `strd` (64 bits) sobre el cuadro de la GRAM y el
 * emulador abortaba (aborto de datos en 0x20E9AF40, hitos 5 y 7) [observado]; [inferido] el modelo de la memoria gráfica de E
 * no admite accesos de 64 bits. En un aparato [sin saber]. Así escribe de 16 en 16, como antes de R3. */
__attribute__((noinline)) static void rect(int x, int y, int w, int h, u16 c)
{
    int i, j;
    for (j = y; j < y + h && j < LCD_ALTO; j++) for (i = x; i < x + w && i < LCD_ANCHO; i++) cuadro[j * LCD_ANCHO + i] = c;
}
/* dígitos de 7 segmentos, grandes: no hace falta fuente */
static void digito(int x, int y, int d, u16 c, u16 fondo)
{
    static const u8 seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    u8 s = seg[d % 10];
    rect(x, y, 28, 48, fondo);
    if (s & 1)  rect(x + 4, y, 20, 4, c);
    if (s & 2)  rect(x + 24, y + 4, 4, 18, c);
    if (s & 4)  rect(x + 24, y + 26, 4, 18, c);
    if (s & 8)  rect(x + 4, y + 44, 20, 4, c);
    if (s & 16) rect(x, y + 26, 4, 18, c);
    if (s & 32) rect(x, y + 4, 4, 18, c);
    if (s & 64) rect(x + 4, y + 22, 20, 4, c);
}
static void numero(int x, int y, u32 v, int cifras, u16 c, u16 fondo)
{
    int i;
    for (i = cifras - 1; i >= 0; i--) { digito(x + i * 32, y, (int)(v % 10), c, fondo); v /= 10; }
}
static void barras(void)
{
    static const u16 col[8] = { C565(255,255,255), C565(255,255,0), C565(0,255,255), C565(0,255,0),
                                C565(255,0,255), C565(255,0,0), C565(0,0,255), C565(0,0,0) };
    int i, x, y;
    for (i = 0; i < 8; i++) rect(i * 30, 0, 30, 160, col[i]);
    for (y = 160; y < 200; y++) for (x = 0; x < LCD_ANCHO; x++) cuadro[y * LCD_ANCHO + x] = C565(x * 255 / 239, 0, 0);
    for (y = 200; y < 240; y++) for (x = 0; x < LCD_ANCHO; x++) cuadro[y * LCD_ANCHO + x] = C565(0, x * 255 / 239, 0);
    for (y = 240; y < 280; y++) for (x = 0; x < LCD_ANCHO; x++) cuadro[y * LCD_ANCHO + x] = C565(0, 0, x * 255 / 239);
    rect(0, 280, LCD_ANCHO, 40, C565(40, 40, 40));
    rect(0, 0, LCD_ANCHO, 1, C565(255, 0, 0)); rect(0, LCD_ALTO - 1, LCD_ANCHO, 1, C565(255, 0, 0));   /* marco de 1 px: si falta un borde, se ve */
    rect(0, 0, 1, LCD_ALTO, C565(255, 0, 0)); rect(LCD_ANCHO - 1, 0, 1, LCD_ALTO, C565(255, 0, 0));
}

static void pulso_vibrador(void)                    /* la señal de vida: lo que hace el arranque temprano del original */
{
    int r = pmu_vibrador(1);
    espera_ms(75);
    pmu_vibrador(0);
    uart_puts(UART0, r ? "vibrador: el PMU no contesta\n" : "vibrador: pulso de 75 ms (PMU A1 <- 0C, A0 <- 20, A0 <- 00)\n");
}
static void luz_del_logo(void)                      /* (S4) lo que enciende el original con su logo: la luz 2 del chip de LED, a 0x84 */
{
    int c = ledc_init();
    if (!c) c = ledc_luz(2, LEDC_MAX_PANTALLA);
    uart_puts(UART0, c ? "luz: el chip de LED (I2C 1, 0x40) NO contesta: nada encendido\n" : "luz: chip de LED, luz 2 (reg 2F <- 84), como el original con su logo\n");
#ifdef LUCES_TODAS                                  /* DIAGNÓSTICO: además, todas las demás candidatas (hallazgo A6 de la pista R) */
    {
        int a = pmu_luz_pantalla(100);
        pwm_luz_dd(100);
        if (!c) c = ledc_luz(4, LEDC_MAX_TECLADO) || ledc_luz(1, LEDC_MAX_TECLADO) || ledc_grupo(0, 0xFF, 0xFF, 0xFF) || ledc_grupo(1, 0xFF, 0xFF, 0xFF);
        uart_puts(UART0, a ? "luces (TODAS): PMU no contesta" : "luces (TODAS): PMU 43.7/45 encendida"); uart_puts(UART0, ", PWM A y B en marcha, chip de LED ");
        uart_puts(UART0, c ? "NO contesta\n" : "luces 1, 2 y 4 y los dos grupos\n");
    }
#endif
}
/* (R3) APAGAR CON LA TECLA DE ENCENDIDO. Se llama cada ~100 ms. Regla (la de api/enc.h del sistema, en pequeño): se cree al
 * NIVEL de ahora (PMU 0x23 bit 0, releído cada vez: nada de sucesos ni fotos) y solo después de haberla visto SUELTA una vez
 * (quien enciende manteniendo la tecla no apaga sin querer). 30 lecturas seguidas pulsada = 3 s: carga a 0 y 0x41 <- 0. Si el
 * PMU no contesta, no se hace nada. Si tras apagar sigue habiendo corriente (cargador), insiste cada segundo y lo dice. */
static u8 enc_suelta, enc_cuenta;
static void mirar_encendido(void)
{
    u8 v;
    if (pmu_leer(0x23, &v)) return;
    if (v & 1) { enc_suelta = 1; enc_cuenta = 0; return; }                /* (2026-10-20, hito 7 en bueno) bit 0 = 1 cuando SUELTA (modelo E lo tenía invertido; el W580 real dio la verdad) */
    if (!enc_suelta || ++enc_cuenta < 30) return;
    uart_puts(UART0, "tecla de encendido mantenida 3 s (y vista suelta antes): APAGAR (carga a 0, 41 <- 0)\n");
    pmu_carga_fuera();
    for (;;) { pmu_apagar(); espera_ms(1000); uart_puts(UART0, "apagar: sigo con corriente; insisto (41 <- 0)\n"); }
}
static void espera_mirando(unsigned ms)            /* espera_ms, mirando la tecla de encendido cada 100 ms */
{
    while (ms >= 100) { espera_ms(100); mirar_encendido(); ms -= 100; }
    if (ms) espera_ms(ms);
}
/* (S7) La misma comprobación temprana de la batería que el sistema (plat/hw_w580.c; api/bat.h; docs/bateria.md §4.4, §8.A3):
 * 0x23 y la lectura en frío (0x6A). Sin cargador ni USB y 2850 <= mV < 3400: TRES toques de vibrador (el arranque normal da
 * uno), corriente de carga a 0 y apagar (0x41 <- 0, fuera la retención), insistiendo cada segundo. Los mains de hito tampoco
 * cargan: la lista del PMU lleva 0x61 <- 0x10. */
static void bateria_al_arrancar(void)
{
    u8 v = 0; int e, l, que, i;
    e = pmu_leer(0x23, &v) ? -1 : v;
    l = pmu_bateria();
    que = bat_arranque_decidir(e, l);
    uart_puts(UART0, "bateria al arrancar: lectura "); uart_dec(UART0, l < 0 ? 0u : (u32)l);
    uart_puts(UART0, l < 0 ? " (el PMU no contesta)" : " = "); if (l >= 0) { uart_dec(UART0, (u32)bat_mv_frio(l)); uart_puts(UART0, " mV"); }
    uart_puts(UART0, ", estado 23 = "); uart_hex(UART0, (u32)e & 0xFFu);
    uart_puts(UART0, que == BAT_ARR_SEGUIR ? ": bien\n" : que == BAT_ARR_CARGADOR ? ": hay CARGADOR o USB. Este main NO carga la bateria (61 <- 10)\n" :
                     que == BAT_ARR_NO_CREIBLE ? ": lectura NO CREIBLE (< 2850 mV o sin PMU); se sigue sin fiarse\n" :
                     ": BATERIA BAJA (< 3400 mV) sin cargador: NO se sigue. Tres toques de vibrador y apagar\n");
    if (que != BAT_ARR_APAGAR) return;
    pmu_carga_fuera();
    for (i = 0; i < 3; i++) { pmu_vibrador(1); espera_ms(75); pmu_vibrador(0); espera_ms(175); }
    for (;;) { pmu_apagar(); espera_ms(1000); uart_puts(UART0, "bateria baja: sigo con corriente; insisto en apagar (41 <- 0)\n"); }
}
#if HITO == 8
/* ---- HITO 8: EL PERRO DE HARDWARE, SOLO MEDIDA (R3; propuesta P4 de la pista S7) ------------------------------------------
 * El sistema NO arma el perro de hardware (F9010000): de su periodo, de cómo se alimenta y de si F901002C <- 0 lo rearma no
 * se sabe nada (el original pone la llave una vez y no lo toca más; docs/hal.md §3). Este main MIDE, en el aparato y sin
 * nada que perder (ni NOR ni retención), las tres cosas que hacen falta para decidir:
 *   MEDIDA 1 (binario aparte: -DHITO_SIN_LLAVE, arranque.S NO pone la llave): ¿muerde solo?, ¿a los cuántos segundos?
 *   MEDIDA 2: con la llave puesta (arranque normal), F901002C <- 0: ¿se rearma?, ¿a los cuántos segundos muerde?
 *   MEDIDA 3: tras F901002C <- 0, reescribir la llave (10 <- AA, 24 <- 5A, 2C <- A5) cada segundo durante 60 s: ¿lo mantiene
 *             vivo? Y al dejar de reescribirla: ¿cuánto tarda en morder?
 * CÓMO SE LEE: cada medida cuenta décimas de segundo en pantalla y por la UART, y las va apuntando en la iRAM (0xF3003E00;
 * misma idea que hal/reinicio.c). Si el perro muerde, el arranque siguiente ENSEÑA cuánto duró la medida anterior y pasa a
 * la siguiente. Si la iRAM no sobrevive al reset [sin saber], la cuenta de la UART y el reloj de quien mira valen igual, y
 * la pantalla lo dice («sin registro»). Una medida que llega a 120 s sin morder se da por «NO MUERDE» y se pasa a la otra.
 * ES UN MAIN QUE SE REINICIA SOLO A PROPÓSITO. No va en el orden de la primera prueba. [emulador, estricto] el modelo
 * reinicia a los 2 s sin llave y 2 s después de F901002C <- 0: esos 2 s son INVENTADOS (docs/hal.md §3); aquí solo se ensaya
 * que el main hace lo que dice. */
typedef struct { u32 firma, fase, decimas, res[3], suma; } PerroReg;   /* res[k] = décimas que duró la medida k+1 (0xFFFFFFFF: no mordió) */
#define PERRO_REG ((volatile PerroReg *)0xF3003E00u)
#define PERRO_FIRMA 0x50445238u
static u32 perro_suma(const volatile PerroReg *r) { return r->firma ^ r->fase ^ (r->decimas * 3u) ^ r->res[0] ^ (r->res[1] << 1) ^ (r->res[2] << 2) ^ 0x5AA55AA5u; }
static void perro_llave(void) { R32(0xF9010010) = 0xAA; R32(0xF9010024) = 0x5A; R32(0xF901002C) = 0xA5; }
static void perro_pinta(u32 fase, u32 decimas, u16 fondo)
{
    numero(8, 8, fase, 1, C565(255, 255, 0), fondo);
    numero(56, 100, decimas / 10, 4, C565(255, 255, 255), fondo);
    lcd_enviar(cuadro, 0, 160);
}
static void medir_perro(void)
{
    volatile PerroReg *r = PERRO_REG;
    int hay = r->firma == PERRO_FIRMA && r->suma == perro_suma(r), k;
    u32 fase, d;
    u16 fondo;
#ifdef HITO_SIN_LLAVE
    const u32 primera = 1, ultima = 1;
#else
    const u32 primera = 2, ultima = 3;
#endif
    uart_puts(UART0, "HITO 8, SOLO MEDIDA del perro de hardware. ESTE MAIN PUEDE REINICIARSE SOLO. No toca la NOR; la retencion NO se quita\n");
    if (hay && r->fase >= primera && r->fase <= ultima) {     /* venimos de un reinicio a media medida: el perro mordió */
        uart_puts(UART0, "registro de la iRAM valido: la medida "); uart_dec(UART0, r->fase); uart_puts(UART0, " acabo en REINICIO a las ");
        uart_dec(UART0, r->decimas); uart_puts(UART0, " decimas de segundo\n");
        r->res[r->fase - 1] = r->decimas; r->fase++; r->decimas = 0; r->suma = perro_suma(r);
    } else if (!hay || r->fase < primera || r->fase > ultima + 1) {
        uart_puts(UART0, hay ? "registro de otra tanda: se empieza\n" : "sin registro en la iRAM (primer arranque, o la iRAM no sobrevive al reset): se empieza\n");
        r->firma = PERRO_FIRMA; r->fase = primera; r->decimas = 0; r->res[0] = r->res[1] = r->res[2] = 0; r->suma = perro_suma(r);
    }
    lcd_init(cuadro);
    luz_del_logo();
    for (;;) {
        fase = r->fase;
        if (fase > ultima) break;
        fondo = fase == 1 ? C565(96, 0, 0) : fase == 2 ? C565(0, 64, 0) : C565(0, 0, 112);
        rect(0, 0, LCD_ANCHO, LCD_ALTO, fondo);
        uart_puts(UART0, fase == 1 ? "MEDIDA 1: SIN la llave del perro (arranque.S no la ha puesto). Cuento hasta que muerda (tope 120 s)\n" :
                         fase == 2 ? "MEDIDA 2: con la llave puesta; a los 3 s, F901002C <- 0. Cuento desde ahi hasta que muerda (tope 120 s)\n" :
                                     "MEDIDA 3: F901002C <- 0 y la llave reescrita cada segundo durante 60 s; despues se deja y se cuenta (tope 120 s)\n");
        if (fase >= 2) { for (d = 0; d < 30; d++) { perro_pinta(fase, 0, fondo); espera_mirando(100); } R32(0xF901002C) = 0; uart_puts(UART0, "F901002C <- 0\n"); }
        for (d = 0; ; d++) {
            if (fase == 3 && d < 600 && d % 10 == 0) perro_llave();
            if (fase == 3 && d == 600) { uart_puts(UART0, "MEDIDA 3: 60 s vivo reescribiendo la llave: LA LLAVE LO MANTIENE. Dejo de reescribirla\n"); r->res[2] = 600; }
            r->decimas = fase == 3 ? (d >= 600 ? d - 600 : d | 0x40000000u) : d; r->suma = perro_suma(r);   /* bit 30: mordió MIENTRAS se reescribía la llave */
            if (d % 10 == 0) { uart_puts(UART0, "medida "); uart_dec(UART0, fase); uart_puts(UART0, ": "); uart_dec(UART0, d / 10); uart_puts(UART0, " s\n"); }
            perro_pinta(fase, d, fondo);
            espera_mirando(100);
            if ((fase == 3 ? d >= 600 + 1200 : d >= 1200)) break;
        }
        uart_puts(UART0, "medida "); uart_dec(UART0, fase); uart_puts(UART0, ": NO MUERDE en 120 s\n");
        if (fase == 3) perro_llave();                  /* se deja como lo deja el arranque: con la llave */
        if (fase != 3 || r->res[2] != 600) r->res[fase - 1] = 0xFFFFFFFFu;
        r->fase = fase + 1; r->decimas = 0; r->suma = perro_suma(r);
    }
    uart_puts(UART0, "RESUMEN del perro de hardware (decimas de segundo hasta el reinicio; NO = no mordio en 120 s):\n");
    for (k = (int)primera - 1; k < (int)ultima; k++) {
        uart_puts(UART0, "  medida "); uart_dec(UART0, (u32)k + 1); uart_puts(UART0, ": ");
        if (r->res[k] == 0xFFFFFFFFu) uart_puts(UART0, "NO"); else uart_dec(UART0, r->res[k] & 0x3FFFFFFFu);
        if (r->res[k] != 0xFFFFFFFFu && (r->res[k] & 0x40000000u)) uart_puts(UART0, " (mordio MIENTRAS se reescribia la llave: la llave NO lo mantiene)");
        uart_puts(UART0, k == 2 && r->res[k] == 600 ? " (la llave lo mantuvo 60 s y despues NO mordio... o no llego a apuntarse)\n" : "\n");
    }
    uart_puts(UART0, "FIN de la medida. Para repetirla: apagar (tecla de encendido 3 s) y encender\n");
    rect(0, 0, LCD_ANCHO, LCD_ALTO, C565(0, 0, 0));
    for (k = (int)primera - 1; k < (int)ultima; k++) numero(40, 20 + 60 * (k - (int)primera + 1), r->res[k] == 0xFFFFFFFFu ? 99999 : (r->res[k] & 0x3FFFFFFFu), 5, C565(255, 255, 255), C565(0, 0, 0));
    lcd_enviar(cuadro, 0, LCD_ALTO);
    r->fase = 0; r->suma = perro_suma(r);              /* la próxima vez, desde el principio */
    for (;;) espera_mirando(100);
}
#endif
#if defined(HITO_DIAG) && HITO == 4
/* (2026-10-19) HITO 4 DIAGNÓSTICO: en el W580 bueno el hito 4 pintó las barras pero el «0000» no avanzó (la IRQ 53 no entra en
 * el aparato; en el emulador sí). Esto NO duerme con WFI (si la IRQ no llega, el WFI tapa todo lo demás): espera con el
 * temporizador 2 (espera_mirando, la misma del hito 3, que funcionó) y cada ~100 ms PINTA EN CRUDO cada eslabón de la cadena
 * temporizador 1 -> intcon banco 1 -> cascada 28 -> banco 0 -> CPU -> vector de la ITCM -> irq_despacho -> tic(). Además, una
 * vez por segundo FUERZA la línea 53 por software (ISR del banco 1, FB02029C <- 0x02000000, como hace el original): así se
 * separa «el temporizador no interrumpe» de «ninguna interrupción llega a la CPU». NO lee los SIR (leerlos borra la fuente).
 * Seis filas de 8 cifras hexadecimales (en 7 segmentos; el hueco es una cifra vacía):
 *   0 blanco   tics, en DECIMAL. ~250/s = TODO BIEN; ~1/s = solo entran las forzadas (el temporizador no interrumpe); 0 = no
 *              entra ninguna (mirar filas 2-4)
 *   1 amarillo cuenta F901001C (4) _ comparador F9010004 (2, esperado 82) control F9010000 (1, esperado 1).
 *              Si la cuenta no cambia entre cuadros: el temporizador 1 NO CUENTA (reloj de 32 kHz o puerta)
 *   2 cian     ITR banco 1 >> 24 (2) _ MIR banco 1 >> 24 (2; el bit 25 = 2 debe estar a 0) _ ILR 53 FB020280 (2, esperado 70).
 *              Con el bit 25 del ITR a 1 (02) mantenido: llega al intcon pero nadie lo atiende
 *   3 verde    ITR banco 0 >> 28 (1) _ MIR banco 0 >> 28 (1, esperado 0) _ irq_huerfanas (4)
 *   4 magenta  ITCM[0x18] baja (4, esperado F018) _ ITCM[0x38] == flash[0x38] (1 = sí) bit I del CPSR (1, esperado 0)
 *              modo (1, esperado 3 = SVC)
 *   5 rojo     forzadas emitidas (4, decimal) _ segundos por el temporizador 2 (3, decimal) */
static void hexa(int x, int y, u32 v, int cifras, u16 c, u16 fondo)
{
    static const u8 seg[16] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71 };
    int i;
    for (i = cifras - 1; i >= 0; i--) {
        u8 s = seg[v & 15]; int xx = x + i * 30;
        rect(xx, y, 28, 48, fondo);
        if (s & 1)  rect(xx + 4, y, 20, 4, c);
        if (s & 2)  rect(xx + 24, y + 4, 4, 18, c);
        if (s & 4)  rect(xx + 24, y + 26, 4, 18, c);
        if (s & 8)  rect(xx + 4, y + 44, 20, 4, c);
        if (s & 16) rect(xx, y + 26, 4, 18, c);
        if (s & 32) rect(xx, y + 4, 4, 18, c);
        if (s & 64) rect(xx + 4, y + 22, 20, 4, c);
        v >>= 4;
    }
}
static void deci(int x, int y, u32 v, int cifras, u16 c, u16 fondo)
{
    u32 h = 0; int i;
    for (i = 0; i < cifras; i++) { h |= (v % 10) << (4 * i); v /= 10; }
    hexa(x, y, h, cifras, c, fondo);
}
static void diag_tic(void)
{
    u32 vuelta = 0, forzadas = 0, cpsr;
    const u16 F = C565(0, 0, 0);
    rect(0, 0, LCD_ANCHO, LCD_ALTO, F);
    for (;;) {
        u32 cuenta = R32(0xF901001C) & 0xFFFF, cmp = R32(0xF9010004) & 0xFF, ctl = R32(0xF9010000) & 0xF;
        u32 itr1 = R32(0xFB020200), mir1 = R32(0xFB020204), ilr53 = R32(0xFB020280) & 0xFF;
        u32 itr0 = R32(0xFB020100), mir0 = R32(0xFB020104);
        u32 itcm = 0, v18, v38, f38 = R32(MAIN_INI + 0x38);
        __asm__ volatile("" : "+r"(itcm));             /* la ITCM está en 0: que gcc no lo tome por un puntero nulo */
        v18 = R32(itcm + 0x18); v38 = R32(itcm + 0x38);
        __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
        deci(0, 4, tics, 8, C565(255, 255, 255), F);
        hexa(0, 56, cuenta, 4, C565(255, 255, 0), F);  hexa(150, 56, cmp, 2, C565(255, 255, 0), F);  hexa(210, 56, ctl, 1, C565(255, 255, 0), F);
        hexa(0, 108, itr1 >> 24, 2, C565(0, 255, 255), F); hexa(90, 108, mir1 >> 24, 2, C565(0, 255, 255), F); hexa(180, 108, ilr53, 2, C565(0, 255, 255), F);
        hexa(0, 160, itr0 >> 28, 1, C565(0, 255, 0), F); hexa(60, 160, mir0 >> 28, 1, C565(0, 255, 0), F); hexa(120, 160, irq_huerfanas & 0xFFFF, 4, C565(0, 255, 0), F);
        hexa(0, 212, v18 & 0xFFFF, 4, C565(255, 0, 255), F); hexa(150, 212, v38 == f38, 1, C565(255, 0, 255), F);
        hexa(180, 212, (cpsr >> 7) & 1, 1, C565(255, 0, 255), F); hexa(210, 212, cpsr & 0xF, 1, C565(255, 0, 255), F);
        deci(0, 264, forzadas, 4, C565(255, 0, 0), F); deci(150, 264, vuelta / 10, 3, C565(255, 0, 0), F);
        lcd_enviar(cuadro, 0, 312);
        if (vuelta % 10 == 0) {
            uart_puts(UART0, "diag tics="); uart_dec(UART0, tics); uart_puts(UART0, " cuenta="); uart_hex(UART0, cuenta);
            uart_puts(UART0, " cmp="); uart_hex(UART0, cmp); uart_puts(UART0, " ctl="); uart_hex(UART0, ctl);
            uart_puts(UART0, " itr1="); uart_hex(UART0, itr1); uart_puts(UART0, " mir1="); uart_hex(UART0, mir1);
            uart_puts(UART0, " ilr53="); uart_hex(UART0, ilr53); uart_puts(UART0, " itr0="); uart_hex(UART0, itr0);
            uart_puts(UART0, " mir0="); uart_hex(UART0, mir0); uart_puts(UART0, " v18="); uart_hex(UART0, v18);
            uart_puts(UART0, " v38="); uart_hex(UART0, v38); uart_puts(UART0, " cpsr="); uart_hex(UART0, cpsr);
            uart_puts(UART0, " forzadas="); uart_dec(UART0, forzadas); uart_puts(UART0, "\n");
            R32(0xFB02029C) = 0;                       /* [sin saber] si el ISR se borra solo: el original (0x44232c2c) solo escribe 1; */
            R32(0xFB02029C) = 0x02000000;              /* aquí se baja antes para que cada forzada sea un flanco nuevo. Línea 53 = banco 1, bit 25 */
            forzadas++;
        }
        espera_mirando(100);
        vuelta++;
    }
}
#endif
int main(void)
{
    u32 n = 0, seg = 0xFFFFFFFF;
    int pll = 0;
    (void)n; (void)seg; (void)numero; (void)pll; (void)luz_del_logo; (void)barras; (void)espera_mirando;
#if HITO >= 3 && !defined(HITO_13MHZ)
    pll = reloj_208();                             /* como el original: antes de inicializar SYSCON */
#endif
    syscon_init();
#if HITO == 7
    mmu_cache_init();                              /* MMU 1:1 + cachés, en el mismo punto que el sistema (antes de GPIO y UART) */
#endif
    gpio_init();
    uart_init(UART0);
#if HITO == 7
    uart_puts(UART0, "W580 paso 7 (antes del sistema " HW_VERSION_SISTEMA "): MMU + caches + interrupciones + soltar la retencion\n");
    uart_puts(UART0, "mmu y caches: encendidas\n");
#else
    uart_puts(UART0, "W580 hito " );
    uart_dec(UART0, HITO);
    uart_puts(UART0, ": main propio en 0x");
    uart_hex(UART0, MAIN_INI);                     /* (BL) 0x44180000 desde v19, detrás del bootloader */
    uart_puts(UART0, ", pila en la SDRAM\n");
#endif
    uart_puts(UART0, pll ? "reloj: PLL enganchado, 208 MHz\n" : "reloj: 13 MHz\n");
    bateria_al_arrancar();                         /* (S7) antes de encender nada, como el sistema: con la batería baja y nada enchufado, no se sigue */
    uart_puts(UART0, pmu_luz_pantalla(100) ? "luz del PMU: no contesta\n" : "luz del PMU: pulso de 460 ms (45 <- 50, 43 <- 80), como el arranque del original\n");
#if HITO == 7
    tic_init();                                    /* como el sistema: el tic late ANTES de la lista del PMU (el I2C va con interrupciones) */
    uart_puts(UART0, "tic en marcha (IRQ 53), antes de la lista del PMU\n");
    espera_ms(460);
    rtc_init();                                    /* como el sistema: la puerta 27 abierta ANTES de soltar la retención (F9050058 está en el bloque del RTC; el estricto lo cazó) */
    uart_puts(UART0, pmu_encendido() ? "pmu: no contesta (la retencion se queda puesta)\n" : "pmu: lista de inicio escrita y RETENCION SOLTADA (F9050058 bit 0 a 0), como el sistema\n");
#else
    espera_ms(460);
    uart_puts(UART0, pmu_encendido_sin_soltar() ? "pmu: no contesta\n" : "pmu: lista de inicio escrita (apaga la luz del PMU: 43 <- 04); la retencion NO se quita\n");
#endif
    pulso_vibrador();
#if HITO == 8
    medir_perro();
#elif HITO == 2
    for (;;) {
        uart_puts(UART0, "hola desde el W580, vuelta "); uart_dec(UART0, n++); uart_puts(UART0, "\n");
        espera_mirando(500);
        if ((n & 3) == 0) pulso_vibrador();        /* cada 2 s */
    }
#elif HITO == 6
    {
        static const char *const que[] = { "PMU 43.7/45 (la del arranque temprano)", "PWM A (P3.1)", "PWM B (P3.2)", "PWM A y B a la vez (luz 0 de DD_drv_Backlight)",
            "chip de LED, luz 1 (reg 0A <- 05)", "chip de LED, luz 2 (reg 2F <- 84): la del logo del original", "chip de LED, luz 4 (reg 09 <- 05)",
            "chip de LED, grupo 0 (reg 0B, 0C, 0D <- FF)", "chip de LED, grupo 1 (reg 13, 14, 15 <- FF)", "PMU 36 bit 1 (RedLED_On(0))", "PMU 36 bit 0 (RedLED_On(1))" };
        int chip = ledc_init();
        uart_puts(UART0, chip ? "chip de LED (I2C 1, 0x40): NO contesta\n" : "chip de LED (I2C 1, 0x40): iniciado\n");
        for (;;) {
            u32 k;
            for (k = 0; k < 11; k++) {
                int r = 0, si;
                for (si = 1; si >= 0; si--) {
                    switch (k) {
                    case 0: r = pmu_luz_pantalla(si ? 100 : 0); break;
                    case 1: pwm_luz(2, si ? 100 : 0); break;
                    case 2: pwm_luz(1, si ? 100 : 0); break;
                    case 3: pwm_luz_dd(si ? 100 : 0); break;
                    case 4: r = ledc_luz(1, si ? LEDC_MAX_TECLADO : 0); break;      /* los niveles del chip: los máximos del original, no más */
                    case 5: r = ledc_luz(2, si ? LEDC_MAX_PANTALLA : 0); break;
                    case 6: r = ledc_luz(4, si ? LEDC_MAX_TECLADO : 0); break;
                    case 7: r = ledc_grupo(0, si ? 0xFF : 0, si ? 0xFF : 0, si ? 0xFF : 0); break;
                    case 8: r = ledc_grupo(1, si ? 0xFF : 0, si ? 0xFF : 0, si ? 0xFF : 0); break;
                    case 9: r = pmu_led(0, si); break;
                    default: r = pmu_led(1, si); break;
                    }
                    if (si) {
                        uart_puts(UART0, "luz "); uart_dec(UART0, k + 1); uart_puts(UART0, " de 11: "); uart_puts(UART0, que[k]);
                        uart_puts(UART0, r ? " -> NO CONTESTA\n" : " -> encendida 2 s\n");
                        espera_mirando(2000);
                    } else espera_mirando(500);
                }
            }
            uart_puts(UART0, "vuelta "); uart_dec(UART0, ++n); uart_puts(UART0, " hecha\n");
            pulso_vibrador();
        }
    }
#else
    luz_del_logo();
    barras();                                      /* el cuadro va a la memoria del panel ANTES de encenderlo, como el original */
    { int r = lcd_init(cuadro); uart_puts(UART0, r ? "lcd_init: FALLO\n" : "lcd_init: bien\n"); }
    uart_puts(UART0, "panel: "); uart_puts(UART0, lcd_panel_nombre()); uart_puts(UART0, ", R28 = "); uart_hex(UART0, (u32)lcd_panel_r28()); uart_puts(UART0, "\n");
    lcd_enviar(cuadro, 0, LCD_ALTO);
    uart_puts(UART0, "cuadro de barras enviado por el PDICON\n");
#if HITO == 3
    for (;;) espera_mirando(100);                  /* (R3) antes un bucle vacío: ahora se puede apagar con la tecla de encendido */
#else
#if HITO != 7
    tic_init();
    uart_puts(UART0, "tic en marcha (IRQ 53)\n");
#endif
#if HITO >= 5
    kbd_init();
#endif
#if defined(HITO_DIAG) && HITO == 4
    diag_tic();                                    /* no vuelve */
#endif
    for (;;) {
        u32 t = tics, s = t / 250;
        static u32 enc_t;
        if (t - enc_t >= 25) { enc_t = t; mirar_encendido(); }   /* (R3) cada 100 ms */
#if HITO >= 5
        static int antes = -2; int k = kbd_scan();
        if (k != antes) {
            antes = k;
            uart_puts(UART0, "celda "); if (k < 0) uart_puts(UART0, "-"); else uart_dec(UART0, (u32)k); uart_puts(UART0, "\n");
            rect(0, 230, LCD_ANCHO, 50, C565(0, 0, 0));
            if (k >= 0) numero(88, 231, (u32)k, 2, C565(255, 255, 0), C565(0, 0, 0));
            lcd_enviar(cuadro, 230, 50);                /* solo la banda de la tecla */
        }
#endif
        if (s != seg) {
            seg = s;
            uart_puts(UART0, "tics="); uart_dec(UART0, t); uart_puts(UART0, " segundos="); uart_dec(UART0, s);
            uart_puts(UART0, " irq_huerfanas="); uart_dec(UART0, irq_huerfanas); uart_puts(UART0, "\n");
            numero(56, 100, s, 4, C565(255, 255, 255), C565(0, 0, 128));
            lcd_enviar(cuadro, 100, 48);                /* solo la banda del contador */
        }
        __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c0, 4" ::: "r0");      /* WFI: despierta el tic */
    }
#endif
#endif
}
void tic_vigia(void) { }
void excepcion_c(const u32 *ctx)
{
    uart_puts(UART0, "\n*** EXCEPCION tipo "); uart_dec(UART0, ctx[17]); uart_puts(UART0, " pc="); uart_hex(UART0, ctx[15]);
    uart_puts(UART0, " lr="); uart_hex(UART0, ctx[14]); uart_puts(UART0, " dir="); uart_hex(UART0, ctx[19]); uart_puts(UART0, " sp="); uart_hex(UART0, ctx[13]); uart_puts(UART0, "\n");   /* (R3) y la dirección del fallo */
    for (;;) ;
}
#endif
