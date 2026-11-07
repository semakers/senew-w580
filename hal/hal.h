/* HAL del W580i (plataforma DB2020, ARM926EJ-S). Pista S.
 * Cada función cita el apartado de docs/hal.md (pista H2) del que sale. Es la adaptación al
 * DB2020 de la HAL del W600 (DB2010): los mismos nombres donde hacen lo mismo, para que plat/hw_w580.c
 * se parezca al del W600. NADA de esto se ha ejecutado en un teléfono: solo en el emulador de la pista E,
 * que no modela ni el EMC/SDRAM, ni la velocidad, ni el PDI del panel (docs/sistema.md §5). */
#ifndef HAL_H
#define HAL_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
#define R8(a)  (*(volatile u8  *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R32(a) (*(volatile u32 *)(a))

#define SYSCON 0xF9090000u
#define UART0  0xFB060000u                       /* la de las trazas (UART_ACB, IRQ 46) */
#define UART1  0xFB070000u
#define UART3  0xFB080000u                       /* Bluetooth */

#if !defined(I2C_PRUEBA) && !defined(NOR_PRUEBA) && !defined(RTC_PRUEBA)  /* (I1) y prueba_rtc.c, hal/rtc.c; (R2) herr/pruebas/prueba_i2c.c compila hal/i2c.c en el PC, con la libc del PC; (R3) y prueba_nor.c, hal/nor.c */
void *memcpy(void *d, const void *s, unsigned n);   /* plat/mem.c */
void *memset(void *d, int c, unsigned n);
int   memcmp(const void *a, const void *b, unsigned n);
unsigned strlen(const char *s);
#endif

/* ---- §2 relojes, §3 perro y reset ------------------------------------------------------------------ */
/* (R3, P3) EL REGISTRO DE CAUSA DEL ÚLTIMO REINICIO (hal/reinicio.c): sobrevive al reinicio en caliente (iRAM). */
enum { RST_DESCONOCIDA = 0,      /* no había registro válido: arranque en frío, o la memoria no sobrevivió */
       RST_SIN_APUNTAR,          /* había registro pero nadie apuntó por qué se reinició (¿reset de hardware?) */
       RST_PERRO, RST_EXCEPCION, RST_PANICO, RST_PILA, RST_FORZADO,   /* anormales: se le dicen al usuario */
       RST_PEDIDO, RST_APAGADO };                                      /* normales: reinicio pedido, apagado ordenado */
typedef struct { u8 valido, causa, sin_bucle; u32 pc, lr, arranques; } RstInfo;   /* sin_bucle: arranques seguidos ANTERIORES que no llegaron al bucle */
void rst_arranque(RstInfo *o);                   /* una vez, lo primero: lee lo del arranque anterior y apunta este */
void rst_apuntar(u8 causa, u32 pc, u32 lr);      /* antes de reiniciar o apagar */
void rst_bucle_vivo(void);                       /* el bucle principal lleva vivo un rato: la cuenta a 0 */
void rst_nor_borrando(u8 bloque);                /* (AUD6 B1) hal/nor.c: 1 + bloque absoluto de las zonas que se va a borrar; 0 = acabado */
u32  rst_nor_pendiente(void);                    /* lo que el arranque ANTERIOR dejó apuntado ahí (0 = nada): un suspendido se atribuye a ESE bloque */
void syscon_init(void);                          /* §2.1: puertas y resets como FUN_44236220 */
void sys_reset(void);                            /* §3: reset forzado por el perro; si no muerde, salto al vector de reset */
int  reloj_208(void);                            /* §2.2: 1 = PLL enganchado (CPU 208, bus 52) */
void mmu_cache_init(void);                       /* §1.2: tabla de secciones 1:1 nuestra, MMU + cachés */
void cache_limpiar(u32 inicio, u32 fin);         /* baja a la SDRAM las líneas sucias de ese tramo (c7,c10,1); no hace nada con la caché de datos apagada.
                                                    La llama lcd_enviar() sobre lo que manda: quien use lcd_enviar NO tiene que limpiar (docs/sistema.md §10.2) */
extern u32 reloj_mhz;
/* (R3; AUD3-M4 y M6) EL RETARDO CORTO DEL ORIGINAL (su rutina de la ITCM, 0x44233818 -> flash 0x442AB8A0 [observado con
 * tools/e_dis.py]: «n» vueltas de un bucle interior de 4 instrucciones que da tantas vueltas como diga el factor de 0x10E48,
 * que vale 2 a 13 MHz [observado por AUD3] y 0x11 a 208 MHz [solo en el pseudo-C]). Aquí: n x factor vueltas de un bucle que
 * NO es más corto que el suyo (un contador volátil: 6 instrucciones o más por vuelta). Lo usan el asentamiento del barrido
 * del teclado (n = 0x1D) y la lectura coherente del RTC (n = 500). */
void hal_retardo(u32 n);
void espera_ms(unsigned ms);                     /* §6: por el temporizador 2 (32 768 Hz); no depende de la CPU */
extern u32 espera_cortadas;                      /* (S7) veces que el tope de vueltas cortó una espera (el temporizador 2 no corre): debe ser 0 */

/* ---- §8 UART ------------------------------------------------------------------------------------------- */
int  uart_init(u32 base);                        /* 115 200 8N1 por sondeo; 1 = hay cable (P0.0 a 1) */
int  uart_cable(void);
void uart_putc(u32 base, char c);
void uart_puts(u32 base, const char *s);
void uart_hex(u32 base, u32 v);
void uart_dec(u32 base, u32 v);
int  uart_getc(u32 base);                        /* -1 si no hay nada */
void uart_rx_irq_init(void);                     /* recepción de la UART 0 por IRQ 46 a un anillo */
int  uart_rx(u8 *d, int max);
extern volatile u32 uart_rx_irqs, uart_rx_perdidos;

/* ---- §4 pantalla: XGAM (PDI + PDICON) -> panel R61505 de 240x320 ---------------------------------------- */
#define LCD_ANCHO 240
#define LCD_ALTO  320
int  lcd_init(const u16 *cuadro);                /* 0 = bien; < 0 = el bloque gráfico no contesta (sin pantalla, no cuelga). DETECTA el panel
                                                    (registro 0x0028) y manda `cuadro` (o nada si es NULL) a su memoria antes de encenderlo */
void lcd_reg(u8 indice, u16 valor);              /* un registro del R61505 por el PDI */
int  lcd_leer_reg(u16 reg);                      /* lee un registro del panel por el PDI (16 bits); -1 = no se pudo */
const char *lcd_panel_nombre(void);              /* el descriptor de dispdriver0.dat que eligió la detección */
int  lcd_panel_r28(void);                        /* lo leído del registro 0x0028 (-1 = no se pudo leer) */
void lcd_enviar(const u16 *cuadro, u32 y0, u32 filas);   /* esas filas del cuadro (banda de ancho completo): RGB565 nativo, en la SDRAM */
void lcd_enviar_girado(const u16 *cuadro, u32 paso, u32 codigo, u32 esquina);   /* (estética, giro) el cuadro ENTERO girado por el PDICON:
                                    * paso = píxeles por fila del cuadro lógico; codigo = PDICON+60 (5 = arriba a la derecha, 6 = a la izquierda,
                                    * 3 = boca abajo); esquina = PDICON+5C (x | y << 16) donde empieza a leer. docs/estetica.md §7 */
void lcd_dormir(void);                           /* guion standby_from_normal del panel */
void lcd_despertar(const u16 *cuadro);           /* normal_from_standby, init, ese cuadro, init2 */
void lcd_apagar(void);                           /* guion shutdown (al apagar el teléfono) */
extern u8 lcd_hay;

/* ---- §5 teclado: matriz de 6 columnas x 5 filas + 5 teclas a masa ---------------------------------------- */
void kbd_init(void);
int  kbd_mapa(u8 m[7]);                          /* m[columna] bit fila; m[6] = a masa. Devuelve cuántas pulsadas */
extern volatile u32 kbd_rafagas;                 /* (R3) veces que la guarda de ráfaga tapó la IRQ 60 (hal/kbd.c) */
void kbd_destapar(void);                         /* (AUD6 B5) destapa la IRQ 60 si la guarda de ráfaga la dejó tapada (antes de dormir) */
void kbd_masa(int *nivel, int *foto);            /* (R2) las teclas a masa por nivel (-1 = el bucle está barriendo) y por la foto: para la IRQ del tic */
int  kbd_scan(void);                             /* primera celda (columna*5 + fila) pulsada o -1 */
void kbd_irq_init(void);                         /* IRQ 60: cuenta en kbd_irqs y rearma */
extern volatile u32 kbd_irqs;

/* ---- §6 tic, §7 controlador de interrupciones ------------------------------------------------------------ */
typedef void (*irq_fn)(void);
void intc_init(void);
void intc_habilita(u32 linea, irq_fn f);         /* linea = numeración global (53 = tic, 60 = teclado) */
void intc_habilita_nivel(u32 linea, irq_fn f, u32 prioridad);   /* (pista T) fuente por nivel, con prioridad (ILR = prioridad << 2 | 2) */
void intc_tapar(u32 linea);
void intc_destapar(u32 linea);
void tic_init(void);
extern volatile u32 tics;                        /* sube 250 veces por segundo */
extern volatile u32 tic_tono_hasta;              /* 0, o el tic en que la interrupción apaga el zumbador */
extern volatile u32 irq_eventos, irq_huerfanas;
void tic_largo(u32 cuentas);                     /* reposo profundo: un solo despertar dentro de «cuentas» de 32 768 Hz */
void tic_normal(void);
extern volatile u8 tic_vencio;
extern u8 tic_en_marcha;
u32  irq_cerrar(void);                           /* arranque.S: devuelve el bit I anterior */
void irq_abrir(void);
void en_pila_de_fallo(void (*f)(void *), void *arg);
extern u32 exc_ctx[21];

/* ---- §9 GPIO y tonos ------------------------------------------------------------------------------------- */
void gpio_init(void);
void gpio_dir(unsigned puerto, u8 mascara, int salida);
void gpio_set(unsigned puerto, u8 mascara, int v);
int  gpio_get(unsigned puerto, u8 mascara);
/* POLARIDAD DEL DESLIZADOR: el nivel de P2.6 que significa «CERRADO». [inferido]: el firmware original, corrido en
 * el emulador por la pista E2, bloquea el teclado con P2.6 = 1. [sin saber] en un aparato: si resulta al revés, se
 * cambia AQUÍ (o -DDESLIZADOR_CERRADO_ES=0) y no hay que tocar nada más. Detalle en hal/gpio.c. */
#ifndef DESLIZADOR_CERRADO_ES
#define DESLIZADOR_CERRADO_ES 1
#endif
int  gpio_deslizador(void);                      /* P2.6 crudo (0/1); si la IRQ está en uso, la vuelve a armar */
void gpio_deslizador_irq_init(void);             /* IRQ 67 «en los dos sentidos», como el original: cuenta en desliz_irqs */
extern volatile u32 desliz_irqs;
void pwm_luz(int cual, u32 pct);                 /* §4.6: luz 1 = PWM B (P3.2), luz 2 = PWM A (P3.1) */
void pwm_luz_dd(u32 pct);                        /* la «luz 0» de DD_drv_Backlight: los dos PWM a la vez (pista L: [inferido, fuerte] el teclado del deslizador, ledid 1) */
void pwm_luz_dd_nivel(u32 nivel);                /* (S5) lo mismo por NIVEL 0..255 (A = n, B = n << 2); la primera vez, la secuencia entera de FUN_4529c744; 0 = apagar */
void tono_init(void);                            /* generador 0xF9020000: puerta 32, bloque 22 */
u32  tono(u32 f_hz, u32 ciclo_pct);
void tono_fin(void);

/* ---- §10 RTC ---------------------------------------------------------------------------------------------- */
typedef struct { u16 anio; u8 mes, dia, hora, min, seg; } rtc_t;
void rtc_init(void);
int  rtc_get(rtc_t *t);                          /* 0 = hora válida */
void rtc_set(const rtc_t *t);
void rtc_alarma(int hora, int min);              /* alarma 1 diaria por IRQ 36; hora < 0 la quita */
extern volatile u32 rtc_alarmas;
u32  rtc_128(void);                              /* (minuto*60 + segundo)*128 + fracción de 1/128 s */
extern u32 rtc_lecturas_largas;                  /* (I1) veces que la lectura tomó el camino largo del original (cerca del vuelco) */

/* ---- §11 I²C (dos buses) y §12 PMU «Vincenne» (bus 0, 0x4A) ------------------------------------------------ */
typedef struct { u32 escrituras, lecturas, topes, nack, reintentos, recuperados; } i2c_cuentas;   /* (R2) reintentos tras un veto; recuperados = volvió a contestar */
extern i2c_cuentas i2c_cta;
int  i2c_vetado(unsigned bus);                  /* (R2) 1 = dio un tope hace menos de 2 s y aún no se reintenta */
void i2c_revivir(unsigned bus);                 /* (S7) el bus deja de darse por muerto (perro y apagado forzado, desde la IRQ) */
int  i2c_escribir(unsigned bus, u8 dir7, const u8 *d, u32 n);             /* 0 = bien; -1 = el bus no contesta */
int  i2c_escribir_leer(unsigned bus, u8 dir7, const u8 *e, u32 ne, u8 *l, u32 nl);
int  pmu_leer(u8 reg_fisico, u8 *v);
int  pmu_escribir(u8 reg_fisico, u8 v);
int  pmu_luz_pantalla(u32 pct);                  /* §4.6 luz 0: 0x45 = nivel y 0x43 bit 7 */
int  pmu_luz_leer(void);
int  pmu_encendido(void);                        /* la lista de inicio del PMU del original y, si 0x41 se relee 0x26, fuera la retención */
int  pmu_encendido_sin_soltar(void);             /* lo mismo, sin tocar la retención (mains de hito) */
int  pmu_apagar(void);                           /* (R2) devuelve si la escritura de 0x41 salió (0 = sí) */
int  pmu_cargador(void);                         /* bit 0 cargador, bit 1 USB; -1 sin bus */
int  pmu_adc(u32 canal);                         /* crudo de 8 bits, canales 0..13; -1 sin bus */
int  pmu_bateria(void);                       /* la lectura «en frío» del arranque temprano (0x70 <- 0x6A, 0x71 sin pulso); -1 sin bus */
int  pmu_vbat_auto(void);                     /* (S7) VBAT en marcha como el original: registro 0x77, sin pulso; -1 sin bus */
/* (R2) lo mismo, RELEÍDO y con 3 intentos; 0 = confirmado. El último resultado, en pmu_carga. */
typedef struct { u8 confirmada, intentos; short r61, r63; u8 lista_mal; } PmuCarga;   /* r61/r63: lo releído (-1 = no se pudo leer) */
extern PmuCarga pmu_carga;
int  pmu_carga_fuera(void);                   /* (S7/R2/R3) 0x61 <- 0x10, 0x63 <- 0x00: corriente de carga 0, RELEÍDO y con 3 intentos; 0 = confirmado (queda en pmu_carga) */
int  pmu_carga_usb(int vcod, int icod);       /* (CARGA) 0x63 como el original (tensión y después corriente, leyendo el chip); 0x63 releído o -1 */
u8   pmu_ev21_tomar(u8 mascara);             /* (CARGA) sucesos de 0x21 acumulados por pmu_sucesos (bit 7 = «USB voltage reached») */
int  pmu_bdata_abrir(void);                  /* (CARGA) 0x50 bits 0..2 <- 3; >= 400 ms después, pmu_bdata_cerrar: ADC 11 (cuenta o -1) y 0x50 como estaba */
int  pmu_bdata_cerrar(void);
int  pmu_fg_encender(void);                  /* (CARGA) medidor de combustible: 91 <- 80, 90 <- F0 */
int  pmu_fg_leer(int n, long *v);            /* (CARGA) contador n (3 = sigma-delta con signo: la corriente de la batería al momento) */
int  pmu_vibrador(int si);                       /* el motor CONTINUO (0xA0 = 0x20, 0xA1 = el nivel) o parado. [medido] (hal/i2c.c) */
int  pmu_vibrador_patron(int hz, int pct);       /* 0xA0: pulsos a 1, 2, 4 u 8 Hz con el 25/50/75/100 % encendido (lo más cercano, como el original); 0 = parado */
int  pmu_vibrador_nivel(int n);                 /* 0xA1: tensión del motor 5..12 (12 = la del original; con 5 no arranca); vale para lo siguiente */
int  pmu_led(int n, int si);                     /* PMU 0x36 bit 1 (n = 0, RedLED_On(0)) o bit 0 (n = 1). [sin saber] qué LED es */
extern i2c_cuentas ledc_cta;
extern u32 ledc_rampas;                          /* (S5) veces que se ha arrancado una rampa del PWM del chip de LED (0x16 con sentido) */
/* Controlador de LED por I²C 1, 0x40 (DD_drv_Backlight). Niveles DE REGISTRO; los máximos son los del original en lo
 * ejecutado (pista E5) y ledc_escribir() recorta a ellos. [sin saber] qué LED es cada salida y qué corriente cada nivel. */
#define LEDC_MAX_PANTALLA 0x84u                  /* registro 0x2F (luz 2): 0x84 con el logo, 0x2F en el standby, 0x06 atenuado */
#define LEDC_MAX_TECLADO  0x05u                  /* registros 0x09 (luz 4) y 0x0A (luz 1) EN EL STANDBY del original (su 36 %): rampa 2, 4, 5 */
#define LEDC_TOPE_TECLADO 0x10u                  /* (S5) el 100 % del código del original en 0x09 y 0x0A (docs/luces.md §4: la traza solo cuadra con rango 0x10);
                                                    lo alcanzan los efectos .ble a intensidad 100. Es a lo que recorta ledc_escribir() */
int  ledc_init(void);                            /* 0 = contestó a todo el inicio */
int  ledc_escribir(u8 reg, u8 v);                /* recorta 0x09 y 0x0A a LEDC_TOPE_TECLADO y 0x2F a LEDC_MAX_PANTALLA */
int  ledc_leer(u8 reg, u8 *v);
int  ledc_sombra_de(u8 reg);                     /* lo último escrito en ese registro; -1 = chip sin iniciar */
int  ledc_luz(int n, u32 nivel);                 /* luz 1, 2 o 4; nivel del registro, 0 = apagada. -1 = no contesta */
int  ledc_grupo(int g, u8 a, u8 b, u8 c);        /* grupo de tres 0 o 1 (registros 0x0B..0x0D / 0x13..0x15); 0, 0, 0 = apagado. = ledc_grupo_fundir(g, a, b, c, 2, 0) */
/* (S5) Las dos TERNAS RGB ([inferido, fuerte] las luces laterales; canales en orden R, G, B: docs/luces.md §3), como el original:
 *  - fundir (FUN_4529ca74): los niveles van al chip y quien sube o baja es SU PWM (0x16/0x17), a la velocidad «vel» (0..7:
 *    6 ≈ 50 ms, 7 ≈ 95, 4 ≈ 100, 5 ≈ 190, 2 ≈ 500, 3 ≈ 950, 0 ≈ 1000, 1 ≈ el doble; [inferido]). El PWM es UNO para todo el chip:
 *    fundir una terna reinicia la rampa de la otra. Para bajar se mandan los MISMOS niveles de color (no ceros);
 *  - fijo (FUN_4529c8b4, «rgbon»): PWM a FF y niveles de golpe; modo 1 = corriente fija, 2 = por el PWM, 3 = por el patrón;
 *  - apagar (FUN_4529c9ec): campos y niveles a 0 y, sin ternas, la bomba de carga fuera.
 * Niveles DE REGISTRO 0..255 (el original: % × 255 / 100). -1 = no contesta o argumentos que el original rechaza (todo a 0). */
int  ledc_grupo_fundir(int g, u8 r, u8 v, u8 a, u8 vel, int bajar);
int  ledc_grupo_fijo(int g, u8 r, u8 v, u8 a, u8 modo);
int  ledc_grupo_apagar(int g);
void pmu_irq_init(void);                         /* IRQ 37: sucesos del PMU (la tecla de encendido); el manejador solo avisa y tapa la línea */
int  pmu_sucesos(u8 *estado);                    /* si había aviso: lee 0x21/0x22 (se borran), deja 0x23 en *estado, destapa y devuelve 1 */
extern volatile u8 pmu_pendiente;                          /* la lectura del arranque (0x70 <- 0x6A); -1 sin bus */

/* ---- §13 acelerómetro: analógico, por el ADC del PMU (X = canal 12, Y = canal 4), alimentado por P1.1 ------ */
void acel_alimentar(int si);
int  acel_leer(int *x, int *y);                  /* crudos 0..255 (el cero anda por 125); -1 sin bus. Hay que alimentarlo ~15 ms antes */
/* (2026-10-20, tmp/acel_ag) EL MODO DE CONVERSIÓN DEL ADC DEL PMU y su autodetector (hal/i2c.c, donde se explica cada modo). */
enum { ADC_A = 0, ADC_P, ADC_R, ADC_B, ADC_I, ADC_J, ADC_S, ADC_MODOS };
#define ADC_NOMBRES "APRBIJS"
typedef struct {
    u8 modo;                                     /* el que usa pmu_adc() ahora */
    signed char forzado, auto_modo;              /* forzado: -1 = automático; auto_modo: lo que eligió la última prueba (-1 = ninguno vivo) */
    u8 hallado, probado, probando, intentos, rango_hay;
    short v[ADC_MODOS][4];                       /* última prueba, por modo: canales 1, 8, 4 (Y), 12 (X); -1 = lectura fallida */
    short veneno[ADC_MODOS];                     /* la lectura 0x6A puesta en 0x71 antes de convertir (lo que da un modo que no convierte) */
    u8 viejos[ADC_MODOS], distintos[ADC_MODOS], vivo[ADC_MODOS];   /* vivo: bit 0 = ronda 1, bit 1 = ronda 2 (3 = vivo) */
    short x, y, xmin, xmax, ymin, ymax;          /* lo último que dio acel_leer() y el rango visto desde que se borró */
    u32 lecturas, fallos;
} AdcDiag;
extern AdcDiag adc_dg;
int  adc_autodetectar(void);                     /* prueba todos los modos ya (~0,1 s); devuelve el elegido o -1 */
void adc_forzar(int modo);                       /* 0..ADC_MODOS-1 = siempre ese; -1 = automático (se vuelve a probar en la próxima lectura) */
int  pmu_adc_modo(int modo, u32 canal);          /* una conversión en un modo concreto (diagnóstico) */

/* ---- §16 NOR (Intel M18, 64 MiB, bloques de 256 KiB, programa con búfer de 1 KiB) -------------------------- */
u32  nor_id(void);                               /* fabricante << 16 | dispositivo; se ejecuta desde RAM */
#ifndef NOR_ESCRITURA
#define NOR_ESCRITURA 1                          /* 0 = el binario no contiene NINGUNA rutina que escriba en la NOR */
#endif
/* Las dos zonas escribibles, CONTIGUAS, dentro de lo que en fábrica ocupa el main original (0x44140000..0x45DFFFFF),
 * que es lo que nuestro main sustituye: por debajo del FS de fábrica (0x45E00000) y del GDFS (0x47E00000).
 * Almacén clave -> valor: 4 bloques de 256 KiB. Ficheros de usuario (W6U): 8 bloques de 256 KiB. */
#define NOR_ZONA_INI 0x45B00000u
#define NOR_ZONA_FIN 0x45C00000u
#define NOR_USR_INI  0x45C00000u
#define NOR_USR_FIN  0x45E00000u
/* (I1; W6F, fs2/INTEGRACION.md §1) LA ZONA DE W6F, el sistema de ficheros escribible propio (api/w6f.c), en MODO OBJETO y
 * SIN espejo: 88 bloques de 256 KiB pegados por debajo del almacén (0x44500000..0x45AFFFFF; hasta 2026-10-14, 64 desde 0x44B00000), dentro de lo que en fábrica es
 * el main original (que nuestro main sustituye). El valor llega del Makefile (W6F_INI: UN sitio; lo ven el C, el enlace y
 * los guiones); este es el de omisión. El FS de fábrica, el GDFS, el almacén y W6U NO se mueven. */
#ifndef NOR_W6F_INI
#define NOR_W6F_INI  0x44500000u   /* (2026-10-14, el mantenedor) 88 bloques, 22 MiB; antes 0x44B00000 (64). Crece hacia abajo sin migrar: docs/fs_propio.md §7.6 */
#endif
#define NOR_W6F_FIN  NOR_ZONA_INI
/* Todo lo escribible sigue siendo UN tramo (lo usan EN_ZONA de hal/nor.c y el mapa sin caché de hal/reloj.c), ahora desde W6F. */
#define NOR_ESCR_INI NOR_W6F_INI
#define NOR_ESCR_FIN NOR_USR_FIN
/* El tramo CON espejo en RAM (almacén + W6U) deja de coincidir con el escribible: la zona de W6F no tiene espejo. */
#define NOR_ESPEJO_INI NOR_ZONA_INI
#define NOR_ESPEJO_FIN NOR_USR_FIN
_Static_assert((NOR_W6F_INI & 0xFFFFFu) == 0 && NOR_W6F_INI >= 0x44200000u && NOR_W6F_INI + 5u * 0x40000u <= NOR_ZONA_INI,
               "NOR_W6F_INI: multiplo de 1 MiB, por encima del main y con sitio para W6F");
enum { NOR_E_RANGO = 1, NOR_E_CHIP, NOR_E_TIEMPO, NOR_E_CHIP_SR, NOR_E_VERIFICA, NOR_E_BITS, NOR_E_DESHABILITADA };
enum { NOR_R_SUSPENDIDO = 0x4000, NOR_R_TIEMPO = 0x8000, NOR_R_RANGO = 0x10000 };
typedef struct { u32 borrados, programas, suspensiones, errores, ultimo_sr, cesiones; } nor_cuentas;   /* (2026-10-26) cesiones: programas que abrieron las IRQ entre palabras */
extern nor_cuentas nor_cta;
/* (R3, AUD3-M2) Tras empezar o REANUDAR un borrado, sondeos durante los que no se mira si hay una IRQ: cada vuelta de
 * suspender/reanudar deja al chip borrar al menos este rato, así que el borrado ACABA aunque una fuente se quede pendiente
 * para siempre (la IRQ de la UART sin cable, una ráfaga del teclado). [sin medir] cuánto tiempo son en el aparato (unas
 * decenas de instrucciones por sondeo desde RAM); [sin saber] si el M18 pide un mínimo entre reanudar y suspender. */
#ifndef NOR_SONDEOS_SIN_MIRAR
#define NOR_SONDEOS_SIN_MIRAR 2000u
#endif
/* (R3, P1) LO QUE EL CHIP DICE DE SÍ MISMO, leído UNA vez (hal/nor.c), y las dos respuestas: `geometria` (su tabla CFI vale para
 * LEER nuestras zonas) y `que` (PERMISO de escritura: es exactamente el de hal/nor_placa.h, o qué no cuadra). */
enum { NOR_CHIP_BIEN = 0, NOR_CHIP_ID = 1, NOR_CHIP_CFI = 2 };
typedef struct {
    u8  leido, geometria, que;      /* que = NOR_CHIP_* */
    u8  cfi_mal;                    /* la primera palabra del CFI que no cuadra con la placa (0x10 QRY, 0x13 órdenes, 0x27 tamaño, 0x2A búfer, 0x2C/0x2D/0x2F bloques) */
    u8  cfi_tam, cfi_bufer;         /* log2 del tamaño (0x1A = 64 MiB) y del búfer (0x0A = 1 KiB) */
    u8  suspendidos, saneo_lento;   /* (P2) borrados SUSPENDIDOS que se encontraron y terminaron al arrancar; 1 = alguno pasó del tope */
    u8  ocupado, suspendido, suspendido_ajeno;   /* (AUD6) al arrancar: ocupado en el tope; había un suspendido; suspendido que NO se ha podido atribuir a nuestras zonas (no se toca) */
    u16 cfi_ordenes;
    u32 id, cfi_bloques, cfi_bloque;
} NorChip;
const NorChip *nor_chip(void);                   /* la primera vez lee el chip (IRQ cortadas, desde RAM): ANTES de encender la caché de datos */
int  nor_escr_init(void);                        /* lee la tabla CFI y fija la geometría; 0 = se puede escribir */
int  nor_escr_motivo(void);
u32  nor_zona_bloque(void);                      /* bytes del bloque de borrado (0 = sin escritura) */
u32  nor_zona_bloques(void);
const u8 *nor_zona(u32 bloque);
int  nor_borrar(u32 bloque);                     /* 0 = bien; -NOR_E_* */
int  nor_programar(u32 bloque, u32 desplaz, const void *datos, u32 n);
/* (I1; W6F) la zona de W6F, en modo OBJETO y SIN espejo: se lee derecho de la flash mapeada (sin caché: hal/reloj.c) */
u32  nor_w6f_bloques(void);                      /* 0 = sin geometría */
const u8 *nor_w6f(u32 bloque);                   /* el bloque de 256 KiB, tal cual está en el chip */
int  nor_w6f_borrar(u32 bloque);                 /* 0 = bien; -NOR_E_* */
int  nor_w6f_programar(u32 bloque, u32 region, const u8 *datos);   /* UNA región de 1 KiB entera, que tiene que estar en blanco; «datos» en RAM */
u32  nor_usr_bloques(void);
const u8 *nor_usr(u32 bloque);
int  nor_usr_borrar(u32 bloque);
int  nor_usr_programar(u32 bloque, u32 desplaz, const void *datos, u32 n);
#endif
