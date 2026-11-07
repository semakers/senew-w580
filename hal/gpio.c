/* GPIO (0xF9000000): 5 puertos de 8 pines, 10 bytes por puerto; los PWM de las luces y el generador de tonos.
 * docs/hal.md §9 y §4.6. Los registros de datos son los del DB2010. Puerta 10, bloque 8. */
#include "hal.h"

#define GPIO_DAT(p)  R16(0xF9000000 + 10u * (p))
#define GPIO_DIR(p)  R16(0xF9000002 + 10u * (p))

/* El inicio del GPIO del ORIGINAL (FUN_441a9060; traza e14 l. 407..470 [observado, registro a registro] —
 * hallazgo A4 de la pista R: antes aquí solo se abría la puerta y los pines quedaban como los dejara el cargador):
 * puerta 10; pulso de reset del bloque 8 (SYSCON+10 bit 0: 1 -> 0 -> 1); rutas de función alternativa a 0; DATOS
 * y DIRECCIONES de los cinco puertos; máscaras de IRQ a 0; rutas definitivas (+3C..+4C) y el segundo juego
 * (+50..+74). Con esto P0.0/P0.1 quedan en la función de la UART 0 (+3C = 0x05A5), P1.1 (acelerómetro) como salida
 * a 0, P2.6 (deslizador) como entrada, P3.1/P3.2 (habilitación de los PWM de luces) como salidas a 0.
 * [sin saber] qué significa cada campo de las rutas y qué cuelga de P0.7, P1.2, P1.3, P1.6, P2.0, P2.1, P3.3 y
 * P3.4, que el original deja a 1: se copia la tabla tal cual. */
void gpio_init(void)
{
    static const u8  dat[5]  = { 0x80, 0x4C, 0x43, 0x18, 0x00 }, dir[5] = { 0xE2, 0xBF, 0xBA, 0xEF, 0xFF };
    static const u16 ruta[5] = { 0x05A5, 0x0000, 0x0055, 0x0114, 0x0000 };
    u32 p; u16 v;
    R16(SYSCON + 0x16) &= (u16)~0x0001u;                  /* puerta 10 */
    v = R16(SYSCON + 0x10);
    R16(SYSCON + 0x10) = (u16)(v & ~1u); R16(SYSCON + 0x10) = (u16)(v | 1u);   /* bloque 8: pulso de reset */
    for (p = 0; p < 5; p++) R16(0xF900003C + 4 * p) = 0;
    for (p = 0; p < 5; p++) GPIO_DAT(p) = dat[p];
    for (p = 0; p < 5; p++) GPIO_DIR(p) = dir[p];
    for (p = 0; p < 5; p++) R16(0xF9000004 + 10 * p) = 0;
    for (p = 0; p < 5; p++) R16(0xF900003C + 4 * p) = ruta[p];
    R16(0xF9000054) = 0; R16(0xF9000050) = 0; R16(0xF900005C) = 0; R16(0xF9000058) = 0x2200;
    R16(0xF9000064) = 0x000A; R16(0xF9000060) = 0; R16(0xF900006C) = 0; R16(0xF9000068) = 0x0002;
    R16(0xF9000074) = 0; R16(0xF9000070) = 0;
}
void gpio_dir(unsigned p, u8 mascara, int salida)
{
    u16 d = GPIO_DIR(p) & 0xFF;
    GPIO_DIR(p) = salida ? (u16)(d | mascara) : (u16)(d & (u8)~mascara);
}
void gpio_set(unsigned p, u8 mascara, int v)
{
    u16 d = GPIO_DAT(p) & 0xFF;
    GPIO_DAT(p) = v ? (u16)(d | mascara) : (u16)(d & (u8)~mascara);
}
int gpio_get(unsigned p, u8 mascara) { return (GPIO_DAT(p) & mascara) != 0; }

/* EL DESLIZADOR: P2.6, entrada (ed_JackFlipDeviceDriver.c: jackflip_open 0x451C3D88, jackflip_ioctl 0x451C3F50
 * [observado en el pseudo-C]: el valor es (P2 & 0x40) != 0 y se suscribe a la IRQ del pin «en los dos sentidos»
 * con 200 ms de antirrebote).
 *
 *   >>> POLARIDAD: DESLIZADOR_CERRADO_ES (hal.h) dice qué nivel de P2.6 significa «cerrado». Vale 1. <<<
 *   Es [inferido], no medido en un aparato: la pista E2 vio en el emulador que el firmware ORIGINAL bloquea el
 *   teclado al poner P2.6 = 1 (docs/emulador.md §6: «1 = cerrado»), o sea, así lo interpreta el original; que el
 *   sensor del aparato dé de verdad un 1 con el teléfono cerrado, [sin saber]. Si un aparato dice lo contrario,
 *   se cambia esa constante (o se compila con -DDESLIZADOR_CERRADO_ES=0) y nada más.
 *
 * La IRQ (línea 65 + puerto = 67): el GPIO interrumpe por NIVEL — con +4 (máscara) a 1, mientras el pin valga lo
 * que diga su bit de +6. «Los dos sentidos» lo hace el original a mano (FUN_441a8400, modo 2 [observado]): pone
 * en +6 el nivel CONTRARIO al que tiene el pin ahora; su rutina de IRQ (pd_gpio 0x441A9AA4) quita el bit de +4 y
 * avisa al proceso, que tras el antirrebote vuelve a armar. Aquí igual: la rutina cuenta y desarma (+4), y
 * gpio_deslizador() —que el sistema llama cada 200 ms— vuelve a armar esperando el nivel contrario al leído.
 * [emulador: propiedad gpio-in=2.6=V de la pista E2, con su IRQ 67]. En un aparato, sin probar. */
volatile u32 desliz_irqs;
static u8 desliz_armado;
static void desliz_armar(void)
{
    u16 nivel = R16(0xF9000006 + 20) & 0xFF, pin = GPIO_DAT(2) & 0x40;
    u32 e = irq_cerrar();
    R16(0xF9000006 + 20) = pin ? (u16)(nivel & ~0x40u) : (u16)(nivel | 0x40u);   /* espera el nivel contrario */
    R16(0xF9000004 + 20) = (u16)((R16(0xF9000004 + 20) | 0x40) & 0xFF);
    if (!e) irq_abrir();
}
static void desliz_irq(void)
{
    R16(0xF9000004 + 20) = (u16)(R16(0xF9000004 + 20) & ~0x40u & 0xFF);          /* desarma: la línea baja */
    desliz_irqs++;
}
void gpio_deslizador_irq_init(void)
{
    gpio_dir(2, 0x40, 0);
    intc_habilita(67, desliz_irq);
    desliz_armado = 1;
    desliz_armar();
}
int gpio_deslizador(void)                                 /* P2.6 crudo (0/1); si la IRQ está en uso, la rearma */
{
    int v = (GPIO_DAT(2) & 0x40) != 0;
    if (desliz_armado && !(R16(0xF9000004 + 20) & 0x40)) desliz_armar();
    return v;
}

/* LUCES POR PWM (hal.md §4.6 y §9). Dos PWM del bloque GPIO: A (8 bits, +36; habilitación P3.1; marcha +32 bit 2)
 * y B (10 bits, +7C; periodo +78 = 0x400; habilitación P3.2; marcha +32 bit 3). Dos dueños en el original:
 *  - el controlador de luces del arranque (0x449CDE50; descriptores 0x2004724C): «luz 1» = PWM B, «luz 2» = PWM A,
 *    nivel de fábrica 0xA000 de 0xFFFF -> pwm_luz();
 *  - DD_drv_Backlight.c (0x4529C744, la «luz 0» del controlador de la pantalla, [observado en el pseudo-C]):
 *    preescala 0, P3.1 salida a 1, periodo de B = 0x400, P3.2 salida a 1, los dos PWM en marcha, y nivel n (8 bits)
 *    -> A = n, B = n << 2  -> pwm_luz_dd().
 * [inferido] que esa «luz 0» de DD_drv_Backlight sea la retroiluminación de la pantalla EN MARCHA (el módulo es
 * del controlador de pantalla; la del PMU —0x43 bit 7— el original la enciende solo en el arranque temprano y su
 * lista de inicio del PMU la apaga antes del primer cuadro: hallazgo A6 de la pista R, traza e14 l. 863).
 * [sin saber] cuál de las dos ilumina de verdad el cristal: plat enciende las dos. [emulador: cerrojo]. */
void pwm_luz(int cual, u32 pct)
{
    u32 niv = pct > 100 ? 0xA000u : 0xA000u * pct / 100u;
    if (cual == 1) {
        if (pct) { R16(0xF9000078) = 0x400; R16(0xF900007C) = (u16)(niv >> 6); gpio_dir(3, 0x04, 1); gpio_set(3, 0x04, 1); R16(0xF9000032) |= 8; }
        else { R16(0xF9000032) &= (u16)~8u; gpio_set(3, 0x04, 0); R16(0xF900007C) = 0; }
    } else {
        if (pct) { R16(0xF9000036) = (u16)(niv >> 8); gpio_dir(3, 0x02, 1); gpio_set(3, 0x02, 1); R16(0xF9000032) |= 4; }
        else { R16(0xF9000032) &= (u16)~4u; gpio_set(3, 0x02, 0); R16(0xF9000036) = 0; }
    }
}
void pwm_luz_dd(u32 pct)                                  /* la «luz 0» de DD_drv_Backlight: los dos PWM a la vez */
{
    u32 n;
    if (pct > 100) pct = 100;
    n = pct * 255u / 100u;                                /* NUESTRO: lineal a fondo de escala. El original interpola entre un mínimo y
                                                             un máximo por luz que saca de su configuración ([sin saber] los valores) */
    if (pct) {
        if (!n) n = 1;
        R16(0xF9000032) &= (u16)~3u;                      /* gpio_pwm_prescale(0) */
        gpio_dir(3, 0x02, 1); gpio_set(3, 0x02, 1);
        R16(0xF9000078) = 0x400;
        gpio_dir(3, 0x04, 1); gpio_set(3, 0x04, 1);
        R16(0xF9000032) |= 4; R16(0xF9000032) |= 8;       /* PWM A y PWM B en marcha */
        R16(0xF9000036) = (u16)n; R16(0xF900007C) = (u16)(n << 2);
    } else {                                              /* FUN_4529c810: nivel 0, P3.1 a 0, PWM A parado, P3.2 a 0, PWM B parado */
        R16(0xF9000036) = 0; R16(0xF900007C) = 0;
        gpio_set(3, 0x02, 0); R16(0xF9000032) &= (u16)~4u;
        gpio_set(3, 0x04, 0); R16(0xF9000032) &= (u16)~8u;
    }
}

/* (S5) Lo mismo por NIVEL de registro (FUN_4529da10(0, n): A = n, B = n << 2), para los fundidos por pasos del teclado del
 * deslizador (ledid 1). La primera vez que se enciende, la secuencia entera de FUN_4529c744; después solo los dos niveles.
 * OJO [observado en el pseudo-C]: el camino del FUNDIDO del original (FUN_4529ce78) NO pone P3.1/P3.2 de salida ni a 1,
 * solo habilita los dos PWM; aquí se ponen siempre ([sin saber] si en el aparato hacen falta: nunca se ha visto ejecutar). */
static u8 pwm_dd_encendido;
void pwm_luz_dd_nivel(u32 n)
{
    if (n > 255) n = 255;
    if (n) {
        if (!pwm_dd_encendido) {
            R16(0xF9000032) &= (u16)~3u;                  /* gpio_pwm_prescale(0) */
            gpio_dir(3, 0x02, 1); gpio_set(3, 0x02, 1);
            R16(0xF9000078) = 0x400;
            gpio_dir(3, 0x04, 1); gpio_set(3, 0x04, 1);
            R16(0xF9000032) |= 4; R16(0xF9000032) |= 8;
            pwm_dd_encendido = 1;
        }
        R16(0xF9000036) = (u16)n; R16(0xF900007C) = (u16)(n << 2);
    } else if (pwm_dd_encendido) {                        /* FUN_4529c810 */
        R16(0xF9000036) = 0; R16(0xF900007C) = 0;
        gpio_set(3, 0x02, 0); R16(0xF9000032) &= (u16)~4u;
        gpio_set(3, 0x04, 0); R16(0xF9000032) &= (u16)~8u;
        pwm_dd_encendido = 0;
    }
}

/* Generador de tonos 0xF9020000: igual que el DB2010 (FUN_441ad048: +0 <- 0x40 | vol, +4 <- a, +6 <- b;
 * FUN_441ad0ac: +0 <- 0). Puerta 32, bloque 22 (FUN_441ad020). Las tres escrituras de tono() son las del W600,
 * donde se midió la4 = 440,0 Hz con +4 = 13 MHz / f y +6 = ancho del pulso [inferido que aquí signifiquen lo
 * mismo: mismos desplazamientos y misma función]. [sin saber] por qué pin sale en el W580i y si hay un
 * transductor que suene (hal.md §17): NO se toca ninguna ruta de pin. [emulador: cerrojo]. */
void tono_init(void)
{
    R16(SYSCON + 0x16) &= (u16)~0x0008u;                  /* puerta 32 */
    R16(SYSCON + 0x10) |= 0x0004;                         /* bloque 22 */
}
u32 tono(u32 f, u32 ciclo)
{
    u32 periodo, e;
    if (f < 397 || f > 812000) return 0;
    if (ciclo > 100) ciclo = 100;
    periodo = 13000000u / f;
    e = irq_cerrar();
    R16(0xF9020000) = 0x40 | 1;
    R16(0xF9020004) = (u16)periodo;
    R16(0xF9020006) = (u16)(periodo * ciclo / 200);
    if (!e) irq_abrir();
    return periodo;
}
void tono_fin(void) { R16(0xF9020000) = 0; }
