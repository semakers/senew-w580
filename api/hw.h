/* hw.h — LA API DE HARDWARE del sistema (nacida en el W600, aquí sobre el W580i). Es lo único del teléfono que ve una aplicación.
 *
 * Reglas:
 *  - Estable: las apps (apps/), el shell (shell/) y el framework (ui/, a través de api/ui_sobre_hw.c)
 *    solo usan esto; nadie más toca registros. Hay dos implementaciones: plat/hw_w580.c (el teléfono,
 *    sobre hal/) y plat/hw_pc.c (el PC: simulada, para probar con guiones).
 *  - Nada bloquea: todo devuelve enseguida (lo que tarda se sondea desde un temporizador de la UI).
 *  - Lo que no está implementado EXISTE y devuelve HW_NO_DISPONIBLE: una app puede preguntarlo y
 *    enseñar una pantalla honesta; cuando se implemente no habrá que tocar la app.
 *  - Cada función dice qué se ha visto funcionar: [emulador] = ejecutado en el QEMU del W600;
 *    [emulador: cerrojo] = el modelo acepta la escritura pero no hace nada (no prueba nada);
 *    [teléfono] = nada: ninguna función se ha ejecutado todavía en un teléfono.
 */
#ifndef HW_H
#define HW_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;

/* La versión del sistema, en UN solo sitio: la enseñan la app Sistema (por shell.h), hw_info() y la
 * pantalla de fallo (que vive en la plataforma y no puede incluir shell.h). */
/* v0.9 (pista S6): sonido medido, avisos dentro de las apps externas y zona de ficheros de usuario.
 * El prefijo sigue siendo «S3» A PROPÓSITO: los guiones de pruebas de las apps externas esperan
 * la línea «sistema S3» por la UART para saber que ha arrancado; cambiarlo hay que acordarlo con ellos. */
/* v0.10 (pista S7): la app de Música (docs/sistema.md §15). */
/* v0.13 (pista S5 del W580): las luces completas (laterales RGB, teclados, efectos .ble): docs/sistema.md §12. */
/* v0.14 (pista S6 del W580): «Mis archivos» (la zona de ficheros de usuario en Archivos y en Notas, copiar por trozos,
 * renombrar, captura de pantalla): docs/sistema.md §13. */
/* v0.15 (pista S7 del W580): seguridad física y de datos antes de la primera prueba: el sistema NO carga la batería (corriente
 * de carga a 0), no arranca ni sigue con la batería baja, avisa del cargador, detecta la flash antes de escribir, modo seguro,
 * perro de software y apagado por pulsación larga: docs/sistema.md §15. */
/* v0.18 (pista I1): la Memoria (W6F, con carpetas), la tarjeta de memoria (solo lectura) y la lectura del RTC sin espera: §18. */
/* v0.19 (I1 tras la auditoría AUD7): docs/sistema.md §18.6.
 * v0.20 (coordinador, 2026-10-14): pintado parcial del menú a 176x220 + PEND 01–07 (AUD7-B6, AUD2-B4, AUD-B7, AUD-B6,
 *       AUD8-A1/M1/B1/B2) + SER (recibir por serie): docs/sistema.md §19.
 * v0.21 (ronda 2, integración INT2, 2026-10-14): protección de memoria (PM, ronda 3) + juegos grandes (M5X, API 1.1) + código
 *       generado (JIT) y fs_listar + la Memoria de 22 MiB desde 0x44500000 + las utilidades de casa sacadas a .sen (APX, API 1.2,
 *       funciones 77–138, todas validadas como las de PM) + USB y MTP (USBD, AUD9) + SER borra su temporal al arrancar:
 *       docs/ronda2.md. */
/* v0.22 (coordinador, 2026-10-14): ronda 3 = la M2 «a plenitud»: escritura de sectores (MSW) + FAT con escritura robusta y
 *       MTP «Tarjeta» (FATW): docs/tarjeta_escritura.md, docs/fat_escritura.md. */
/* v0.23 (coordinador, 2026-10-14): ronda 4 = 0.22 + diseños intercambiables (Material 3 de serie y clásico; API 1.3) + iconos y
 *       mallas 3D en las .sen: docs/material.md, docs/menu3d.md §9. */
/* (Fase 3b) LA VERSIÓN DE SENEW, en este solo sitio (contrato del canal §7): SENEW_VERSION para las personas, SENEW_BUILD entero creciente para
 * comparar. De aquí salen DeviceVersion de MTP («0.29.0+290»), SEN_INFO y el rótulo del sistema. */
/* (6-nov) 0.30.0: el estilo ESCRITORIO (shell/menu_escritorio.c); 0.30.1: el escritorio ES la pantalla de inicio; 0.30.2: la hora
 * de la bandeja. Las dos líneas de abajo las leen herramientas (compilar_senus.sh): sin nada detrás */
#define SENEW_VERSION "0.30.3"
#define SENEW_BUILD   303
#define HW_VERSION_SISTEMA "S10 " SENEW_VERSION

#define HW_OK              0
#define HW_NO_DISPONIBLE (-1)      /* no implementado todavía o el hardware no está */
#define HW_ERROR         (-2)      /* argumento malo o fallo del dispositivo */

/* ---- arranque y sistema ------------------------------------------------------------------------ */
void hw_iniciar(void);             /* relojes (PLL 208 MHz), MMU+cachés, tic, pantalla, teclado, RTC, serie. Una vez. [emulador] */
void hw_reposo(void);              /* duerme la CPU hasta la siguiente interrupción (el tic, como tarde 4 ms). [emulador] */
u32  hw_dormir(u32 max_ms);        /* REPOSO PROFUNDO: para el tic, programa el temporizador para despertar como tarde en max_ms (tope 1,9 s),
                                      cierra las puertas de reloj de temporizadores e intcon como el original (SYSCON +16 |= 0x420, +1A |= 0x800),
                                      WFI, y al despertar (tecla, giro de la tapa, alarma del RTC, serie o el propio temporizador) las reabre y
                                      pone hw_ms() al día con lo que ha pasado según el RTC. Devuelve los ms dormidos. [emulador estricto]; PC: avanza el reloj */
void hw_panico(const char *titulo, const char *detalle);   /* PANTALLA DE FALLO: la pinta, la manda por la UART, espera una tecla y reinicia. No vuelve.
                                      (En el PC: imprime y aborta, para que la prueba falle.) Las excepciones de la CPU acaban aquí. */
typedef struct { u32 excepciones, pila_svc_libre, pila_irq_libre, dormidas, despertares_tecla, despertares_tapa, ms_dormido;
                 u32 esperas_cortadas;     /* (S7) veces que una espera por el temporizador 2 se rindió por su tope de vueltas (el contador no corre): debe ser 0 */
} HwSalud;
void hw_salud(HwSalud *s);         /* contadores de robustez y energía (pila libre mínima vista, en bytes; 0 en el PC) */
int  hw_reiniciar(void);           /* reset por software; no vuelve. [emulador] */
int  hw_apagar(void);              /* apaga como el original: luces y panel fuera, PMU 0x41 = 0 y fuera la retención (F9050058 bit 0). No vuelve.
                                      SI VUELVE (HW_ERROR) el teléfono sigue vivo —[sin saber] si 0x41 <- 0 apaga con el cargador puesto; el original
                                      ni lo intenta— y se queda CON LA PANTALLA Y LAS LUCES APAGADAS y sin reescribir nada en el PMU: el shell decide
                                      (shell/energia.c: «apagado en falso»). [emulador estricto: la máquina se para]; en el PC, HW_NO_DISPONIBLE salvo
                                      que el guion diga «apagable 1» (se apaga) o «apagable 2» (vuelve con HW_ERROR, como con el cargador puesto) */
/* ---- (S7) SEGURIDAD: modo seguro, perro de software, pulsación larga, protección de la flash (docs/sistema.md §15) --------------
 * MODO SEGURO: la tecla «C» pulsada al encender (se mira una vez, al iniciar el teclado). En modo seguro la plataforma NO escribe
 * en la NOR (ni puede: las rutinas de escritura se niegan), el almacén NO se lee (hw_cargar = HW_NO_DISPONIBLE: todo de fábrica),
 * Mis archivos queda de solo lectura, y el shell no arranca apps externas, podómetro, menú 3D ni efectos de luz, ni duerme el panel. */
int  hw_modo_seguro(void);
/* (R3, P3) POR QUÉ ARRANCÓ: lo que el arranque anterior dejó apuntado en una memoria que sobrevive al reinicio en caliente (la
 * plataforma dirá HW_RST_DESCONOCIDA si no la tiene o si no sobrevivió). Las causas ANORMALES (>= HW_RST_PERRO) se le dicen al
 * usuario al llegar al standby. seguro_auto = 1: el modo seguro lo ha puesto el sistema solo (3 arranques seguidos no
 * llegaron al bucle), nadie pulsó nada. */
enum { HW_RST_NORMAL = 0, HW_RST_DESCONOCIDA, HW_RST_SIN_APUNTAR, HW_RST_PERRO, HW_RST_EXCEPCION, HW_RST_PANICO, HW_RST_PILA, HW_RST_FORZADO };
typedef struct { unsigned char causa, seguro_auto, sin_bucle; unsigned pc, lr, arranques; } HwReinicio;
void hw_reinicio(HwReinicio *r);
/* PERRO DE SOFTWARE (no es el de hardware, F9010000, que la llave del arranque desarma): lo vigila la interrupción del tic. Si
 * hw_perro_latido() no se llama en «plazo» segundos (10 de fábrica) —el bucle principal lo hace en cada vuelta; app_latido(), por
 * las apps externas—, el sistema se da por colgado: corriente de carga a 0, vibrador y tono fuera, PANTALLA DE FALLO con el PC
 * interrumpido, y reinicio a los 15 s (o con una tecla). Las operaciones de flash dan prórroga (hasta 60 s sin latido).
 * NO puede sacar de un cuelgue con las interrupciones cortadas. En el PC no hace nada. */
void hw_perro_latido(void);
void hw_perro_plazo(int segundos);                  /* 5..120; el shell pone 30 mientras una app externa tiene la pantalla */
/* PULSACIÓN LARGA de la tecla de encendido, vista DESDE LA INTERRUPCIÓN DEL TIC (celda 30 del teclado, lectura pura): a los 8 s
 * hw_encendido_largo() pasa a 1 para que el shell apague con orden; si a los 11 s sigue pulsada (la interfaz no responde), la
 * plataforma apaga ELLA: 0x41 <- 0 y fuera la retención; con cargador (no se apaga), reinicia. */
int  hw_encendido_largo(void);
/* PROTECCIÓN DE LA FLASH: por qué no se escribe (bits; 0 = se escribe). Se decide ANTES de la primera escritura. */
enum { HW_PROT_COMPILADO = 1,       /* binario sin rutinas de escritura (-DNOR_ESCRITURA=0) */
       HW_PROT_CHIP = 2,            /* fabricante/dispositivo que no es el esperado (0089 887E) */
       HW_PROT_CFI = 4,             /* la tabla CFI no cuadra: tamaño, bloques de 256 KiB, región de 1 KiB, juego de órdenes 0x0200 */
       HW_PROT_SEGURO = 8,          /* modo seguro */
       HW_PROT_ESPEJO = 16 };       /* (R2) la copia en RAM de las zonas no coincide con el chip (memoria corrompida): nada se escribe hasta reiniciar */
#define HW_NV_EXT_INI 0x6000        /* (R2) claves del almacén de las apps EXTERNAS: lo único que pueden guardar u olvidar mientras tienen la pantalla */
#define HW_NV_EXT_FIN 0x6FFF
int  hw_nor_proteccion(void);
/* FORMATO PENDIENTE: una zona nuestra que no está en blanco ni lleva nuestra firma (en un aparato: restos del main original) NO se
 * formatea sola: queda sin montar hasta que el usuario lo confirme. Bits: 1 = el almacén de ajustes, 2 = Mis archivos. */
enum { HW_ZONA_AJUSTES = 1, HW_ZONA_USUARIO = 2, HW_ZONA_MEMORIA = 4 };   /* (I1) 4 = la Memoria (W6F), si es AJENA */
int  hw_formato_pendiente(void);
int  hw_formato_propio(void);    /* (AUD2 B4) de las zonas de hw_formato_pendiente(), las que llevan NUESTRA marca (dañadas, no restos ajenos) */
int  hw_formatear(int zonas);                       /* borra esas zonas y les da formato. HW_OK / HW_ERROR / HW_NO_DISPONIBLE (flash protegida) */
int  hw_borrar_ajustes(void);                       /* EL RESCATE del modo seguro: borra el almacén de ajustes entero (la ÚNICA escritura que el modo
                                                       seguro admite, y solo pedida y confirmada por el usuario). HW_OK / HW_ERROR / HW_NO_DISPONIBLE */
void hw_log(const char *s);        /* traza de depuración por la UART 0 si hay cable; si no, se descarta. [emulador] */
u32  hw_cuenta(void);              /* contador libre de 32 768 Hz para medir; SOLO existe en el emulador (0 en el PC) */
typedef struct {
    const char *version;           /* versión del sistema («S1 0.3») */
    const char *plataforma;        /* «W600 (DB2010)» o «PC (simulado)» */
    u32 cpu_mhz;                   /* 104 si el PLL enganchó, 13 si no. En el emulador no prueba la frecuencia real */
    u8  pll, mmu;                  /* 1 = PLL enganchado; 1 = MMU y cachés encendidas */
    u32 nor_id;                    /* fabricante<<16 | dispositivo (0x0089887E en el emulador; el del aparato, [sin saber]); 0 = sin leer */
    u32 nand_id;                   /* los 4 bytes de identificación (20 AA 80 15 → 0x1580AA20); 0 = no hay (el W580i no tiene NAND) */
    const char *panel;             /* W580i: el descriptor de panel que eligió la detección (NULL = sin pantalla o no se sabe) */
    int panel_r28;                 /* lo leído del registro 0x0028 del panel (-1 = no se pudo leer) */
    u32 ram_total;                 /* bytes de RAM (16 MiB) */
    u32 ram_estatica;              /* .data + .bss + pilas del sistema (0 en el PC) */
    u32 tam_codigo;                /* bytes del binario en la NOR (0 en el PC) */
    u32 irq_serie, irq_alarma;     /* interrupciones atendidas: recepción de la UART 0 (44) y alarma del RTC (43) */
} HwInfo;
void hw_info(HwInfo *i);

/* ---- pantalla: PLACA_ANCHO x PLACA_ALTO (placas/<modelo>/placa_conf.h), RGB565 con el byte alto primero ---------------- */
#include "placa_conf.h"
void hw_pantalla_volcar(const u16 *cuadro, int y0, int filas);   /* manda al panel esas filas del cuadro (DMA). [emulador] */
/* (estética, giro) Cómo va el cuadro LÓGICO al panel: 0 = derecho (el de siempre, PLACA_ANCHO x PLACA_ALTO); 1 = girado con el arriba del
 * cuadro en el borde DERECHO del panel (el teléfono de lado con el teclado a la derecha; cuadro PLACA_ALTO x PLACA_ANCHO); 2 = boca abajo;
 * 3 = con el arriba en el borde IZQUIERDO (teclado a la izquierda). El cuadro lógico ocupa los mismos píxeles en los cuatro: su paso de
 * fila es su ancho. HW_OK, o HW_NO_DISPONIBLE si esta plataforma no sabe girar (entonces sigue en 0). Lo llama ui_poner_giro. */
int hw_pantalla_girar(int giro);

/* ---- luces --------------------------------------------------------------------------------------
 * (Pista S4.) La luz se lleva POR FASES, como la lleva el firmware original del W580i (qué escribe en cada una, medido
 * en el emulador con el chip de LED contestando: plat/hw_w580.c y docs/sistema.md §10.1). Quien decide la fase es el
 * shell (shell/energia.c); la plataforma sabe qué se enciende en cada una, a qué nivel y con qué rampa:
 *   HW_LUZ_LOGO      arranque: solo la luz de la pantalla, al nivel del logo;
 *   HW_LUZ_VIVA      en uso: la pantalla al brillo de hw_luz(0, pct), más el teclado y lo demás;
 *   HW_LUZ_ATENUADA  reposo: baja un poco y, a los 3 s, al mínimo, con el teclado y lo demás apagados;
 *   HW_LUZ_DORMIDA   el panel duerme: la luz se queda en el mínimo (así lo hace el original);
 *   HW_LUZ_APAGADA   se apaga el teléfono: todo fuera.
 * Los cambios van EN RAMPA y no bloquean (los pasos los da la plataforma desde hw_tecla_evento, hw_teclas_mapa,
 * hw_pantalla_volcar y hw_dormir); HW_LUZ_APAGADA sí es inmediata.
 * 4 «luces»: 0 = el BRILLO de la pantalla, 1..100 (50 = el nivel del standby del original, el de fábrica; 100 = el de
 * su logo, su máximo; 0 = lo mismo que HW_LUZ_APAGADA); 1, 2, 3 = las luces 1, 2 y 4 del chip de LED del I²C 1 (0x40),
 * una a una y a mano (diagnóstico, Ajustes) durante 5 s; después manda otra vez la fase. [sin saber] qué LED es cada una.
 * En el emulador el chip es un banco de registros (y por omisión ni contesta): se programa y no se ve nada. */
#define HW_LUCES 4
enum { HW_LUZ_APAGADA, HW_LUZ_LOGO, HW_LUZ_VIVA, HW_LUZ_ATENUADA, HW_LUZ_DORMIDA };
void hw_luz_fase(int fase);
int hw_luz_fase_actual(void);
int hw_luz(int n, int pct);        /* luz 0: el brillo. Luces 1..3: 0 = apagada, 1..100 = encendida. HW_ERROR si el chip de LED no contesta */
int hw_luz_estado(int n);          /* luz 0: el brillo puesto; 1..3: 100 o 0 según lo último escrito en el chip. HW_ERROR si el chip no contesta */

/* (Pista S5.) LAS SEIS LUCES, UNA A UNA, Y LOS EFECTOS. Es la capa de abajo del motor de luces (api/luces.h), que es quien
 * se usa: una app no llama a esto. Los nombres y lo que es cada salida salen de leer el firmware original
 * (docs/luces.md): los «ledid» de sus efectos y las dos ternas RGB.
 *   HW_LED_PANTALLA      ledid 0: la retroiluminación (chip de LED, registro 0x2F; 0..0x84)
 *   HW_LED_TECLADO_DESL  ledid 1: el teclado del deslizador (los dos PWM del GPIO; 0..0xFF) [inferido, fuerte]
 *   HW_LED_TECLADO_2     ledid 2: teclado, zona 2 (chip, 0x0A; 0..0x10)
 *   HW_LED_TECLADO_1     ledid 3: teclado, zona 1 (chip, 0x09; 0..0x10)
 *   ternas 0 y 1         las dos luces LATERALES de colores (chip, 0x0B..0x0D y 0x13..0x15; R, G, B, 0..0xFF) [inferido, fuerte]
 * NIVELES DE REGISTRO, recortados al máximo del original. Las ternas las funde el propio chip con su PWM: «vel» es el
 * código de velocidad (0..7) y «bajar» el sentido; el PWM es uno para las dos (fundir una reinicia la otra).
 * QUIÉN MANDA: las luces son de la FASE (hw_luz_fase) salvo mientras dura un efecto: hw_luz_efecto(1) las cede al motor
 * (la plataforma deja de dar sus pasos) y hw_luz_efecto(0) las devuelve (la fase se vuelve a montar desde lo que haya
 * escrito). Fuera de un efecto, hw_luz_salida() no hace nada y las ternas solo se dejan tocar con la pantalla viva
 * (el latido, el color): dormido, CERO tráfico. Si la fase pasa a atenuada, dormida o apagada, el efecto pierde las luces.
 * [emulador: el chip es un banco de registros (propiedad i2c=0.0x4a:1.0x40; sin ella no contesta y todo esto devuelve
 * HW_ERROR)]. [sin saber] hasta tener un aparato: qué LED es cada salida, su color y cuánta corriente es cada nivel. */
enum { HW_LED_PANTALLA, HW_LED_TECLADO_DESL, HW_LED_TECLADO_2, HW_LED_TECLADO_1, HW_LEDS };
int  hw_luz_salida_max(int led);                     /* 0x84, 0xFF, 0x10, 0x10 */
int  hw_luz_salida(int led, int nivel);              /* 0 = apagada. HW_OK; HW_ERROR (el chip no contesta); HW_NO_DISPONIBLE (no hay efecto en marcha) */
int  hw_luz_salida_nivel(int led);                   /* lo último escrito (0 si está apagada) */
int  hw_luz_rgb_fundir(int terna, int r, int g, int b, int vel, int bajar);   /* como el original: no escribe nada si ya está así y en ese sentido */
int  hw_luz_rgb_fijo(int terna, int r, int g, int b, int modo);               /* de golpe; modo 1 = corriente fija, 2 = por el PWM (a tope), 3 = por el patrón */
int  hw_luz_rgb_apagar(int terna);
int  hw_luz_efecto(int si);                          /* 1 = pide las luces para un efecto, 0 = las devuelve, -1 = pregunta. Devuelve 1 si el efecto las tiene */
void hw_luz_color(int r, int g, int b);              /* el color de las laterales EN USO NORMAL (niveles 0..255; 0, 0, 0 = apagadas). De fábrica, blanco:
                                                        lo que hace el original. Si la pantalla está viva las funde ya a ese color (FUN_45391b30) */
typedef struct {
    u8 presente;                                     /* 1 = el chip de LED contesta */
    u8 fase, efecto;                                 /* HW_LUZ_* · hay un efecto con las luces */
    u8 nivel[HW_LEDS];                               /* lo escrito en cada luz sencilla (registro) */
    u8 rgb[2][3];                                    /* cada terna TAL COMO SE VERÍA ahora: nivel × PWM del chip (modelo [inferido] de su rampa), 0 si su campo está a 0 */
    u8 rgb_nivel[2][3];                              /* lo escrito en sus registros de nivel */
    u8 pwm;                                          /* el PWM del chip ahora, 0..255 (modelo) */
    u32 escrituras;                                  /* órdenes dadas al hardware de luces desde el arranque (W580i: escrituras I²C al chip + cambios del PWM del GPIO) */
} HwLuces;
void hw_luz_leer(HwLuces *e);

/* ---- teclas --------------------------------------------------------------------------------------
 * Los códigos lógicos son los K_* de ui/ui.h (K_ARRIBA … K_ENCENDIDO). Las pulsaciones llegan a las
 * apps como eventos de la UI (Teclas / Enfocable); esto es para lo demás: estado mantenido y matriz. */
int  hw_tecla_evento(void);        /* siguiente pulsación (K_*) o 0. Repite flechas y C si se mantienen (400 ms, luego 120). Lo consume la UI. [emulador] */
int  hw_tecla_abajo(int k);        /* 1 si esa tecla lógica está pulsada ahora (juegos, pulsación larga). [emulador] */
int  hw_teclas_mapa(u8 col[7]);    /* matriz cruda: col[fila] bit c = celda fila*5+c pulsada; fila 6 = teclas a masa. Devuelve cuántas. [emulador] */
const char *hw_celda_nombre(int celda);   /* nombre real de la celda 0..34 («3», «Vol+», «Walkman»…); "" si no hay tecla */
int  hw_celda_tecla(int celda);    /* celda → K_* (0 si no tiene) */

/* ---- tiempo ---------------------------------------------------------------------------------------- */
u32  hw_ms(void);                  /* milisegundos desde el arranque, a saltos de 4 (el tic de 250 Hz por IRQ). [emulador] */
typedef struct { u16 anio; u8 mes, dia, hora, min, seg; } HwFecha;
int  hw_rtc_leer(HwFecha *f);      /* calendario del RTC del chip (0xF9050000). HW_OK, o HW_ERROR si el chip dice que la hora no es válida (f se rellena igual). [emulador] */
int  hw_rtc_poner(const HwFecha *f);   /* pone el RTC en hora. HW_ERROR si la fecha no es válida. [emulador] */
int  hw_rtc_alarma(int hora, int min);  /* programa la alarma diaria DEL CHIP (IRQ 36) a esa hora del RTC; hora < 0 la quita. [emulador]; en el PC no hace nada */
int  hw_rtc_alarma_salto(void);        /* 1 (una sola vez) si la alarma del chip ha saltado desde la última consulta */
int  hw_dia_semana(int anio, int mes, int dia);   /* 0 = lunes … 6 = domingo (cálculo, no hardware) */
int  hw_dias_mes(int anio, int mes);
u32  hw_fecha_seg(const HwFecha *f);              /* segundos desde 2000-01-01 00:00:00 (cálculo; 0 si es anterior) */
void hw_fecha_sumar(HwFecha *f, int minutos);     /* suma (o resta) minutos a una fecha, pasando de día, mes y año (cálculo) */

/* ---- ficheros -------------------------------------------------------------------------------------------
 * Dos sistemas de ficheros de SOLO LECTURA y una carpeta que se escribe (api/hw_fs.c):
 *  - el NUESTRO (W6FS, api/w6fs.h; la imagen la hace herr/hacer_fs.py): si está en la flash (detrás del main,
 *    0x44400000, o en el segmento del FS, 0x45800000) es la raíz «/»;
 *  - el ORIGINAL del teléfono (SEMC FS sobre NOR + NAND, docs/fs.md): si está, aparece en «/original» cuando
 *    hay uno nuestro, o es la raíz «/» cuando no lo hay (como hasta la v0.6).
 *  - (S6) la ZONA DE FICHEROS DE USUARIO (más abajo: hw_usr_*), si está montada, es la carpeta «/usuario» de la raíz
 *    (la última entrada). Es PLANA (W6U no tiene directorios) y es lo único que se puede escribir.
 * Rutas absolutas con «/». Los ajustes pequeños no van aquí: van al almacén (hw_guardar). */
typedef struct { char nombre[64]; u32 tam; u8 es_dir; } HwEntrada;
typedef struct { u8 montado; u32 nand_id, bloques, cabeceras, lecturas_nor, lecturas_nand, errores;
                 u8 propio, original; u32 propio_entradas, propio_bytes; } HwFsEstado;   /* propio = hay W6FS nuestro; original = hay FS de fábrica */
int  hw_fs_listo(void);            /* 1 si hay FS (lo monta la primera vez: puede tardar ~0,2 s). [emulador] */
int  hw_fs_contar(const char *dir);                                  /* entradas del directorio; HW_ERROR si no existe. [emulador] */
int  hw_fs_listar(const char *dir, u32 desde, HwEntrada *e, u32 max); /* hasta «max» entradas a partir de la n.º «desde»; devuelve cuántas. [emulador] */
int  hw_fs_mirar(const char *ruta, u32 *tam);   /* (AUD8 A1) HW_OK si hay un FICHERO con ese nombre exacto en su carpeta (y su tamaño); si no, HW_NO_DISPONIBLE. Un 0 de hw_fs_tam no distingue «vacío» de «no está» */
u32  hw_fs_tam(const char *ruta);                                    /* tamaño del fichero; 0 si no existe o es un directorio. [emulador] */
u32  hw_fs_leer(const char *ruta, u8 *buf, u32 max);                 /* los primeros «max» bytes; devuelve los leídos. [emulador] */
u32  hw_fs_leer_desde(const char *ruta, u32 desde, u8 *buf, u32 max); /* (S6) los bytes [desde, desde+max): para leer por trozos lo que no cabe en RAM */
const u8 *hw_fs_mapa(const char *ruta, u32 *tam);                    /* fichero de NUESTRO FS sin copiarlo: puntero a la flash, o NULL (no existe o es del original) */
int  hw_fs_escribir(const char *ruta, const u8 *buf, u32 n);         /* (S6) en HW_FS_USUARIO/nombre = hw_usr_escribir; en lo demás, HW_NO_DISPONIBLE (solo lectura) */
int  hw_fs_borrar(const char *ruta);                                 /* (S6) en HW_FS_USUARIO/nombre = hw_usr_borrar; en lo demás, HW_NO_DISPONIBLE */
void hw_fs_estado(HwFsEstado *e);

/* ---- almacén persistente: pares clave -> valor que sobreviven al apagado ----------------------------------
 * En el teléfono vive en la NOR (0x45B00000..0x45BFFFFF, 4 bloques lógicos de 128 KiB; hal/hal.h), con
 * escritura atómica (tras un corte de alimentación cada clave vale lo de antes o lo nuevo) y desgaste
 * repartido; formato en api/almacen.h, controlador en hal/nor.c. En el PC, un modelo de NOR en RAM (o en
 * el fichero que diga W600_NV). Los valores miden hasta HW_NV_MAX bytes. Guardar lo que ya vale no escribe.
 * [emulador] probado con reinicio y con la flash guardada y vuelta a cargar (nv-guardar). */
#define HW_NV_MAX 2048
typedef struct { u8 montado, escribible; int motivo; u32 bloques, tam_bloque, usado, vivos, claves, generacion,
                 borrados, programas, suspensiones, errores; } HwNvEstado;
int  hw_cargar(u16 clave, void *buf, u32 max);       /* bytes del valor (copia hasta max); HW_NO_DISPONIBLE si la clave no existe o no hay almacén */
int  hw_guardar(u16 clave, const void *d, u32 n);    /* HW_OK; HW_ERROR (no cabe / fallo de flash); HW_NO_DISPONIBLE (sin almacén o escritura deshabilitada) */
int  hw_olvidar(u16 clave);                          /* borra la clave */
void hw_nv_estado(HwNvEstado *e);                    /* motivo: 0 o por qué no se puede escribir (NOR_E_* de hal.h) */

/* ---- FICHEROS DE USUARIO: la zona «W6U» (pista S6; formato en api/usuario.c, docs/sistema.md §13) ---------
 * Ficheros con nombre (1..47 bytes UTF-8, sin «/»; no hay directorios; 64 como mucho) que se crean, se
 * REEMPLAZAN ENTEROS y se borran. En el teléfono viven en la NOR (0x45C00000..0x45DFFFFF, 8 bloques lógicos
 * de 128 KiB; caben ~740 KB de datos); en el PC, en un modelo en RAM (o en el fichero que diga W600_USR).
 * hw_usr_escribir y hw_usr_borrar son ATÓMICAS: tras un corte de corriente en cualquier punto ese fichero
 * vale ENTERO lo de antes o ENTERO lo nuevo, y los demás no cambian [observado: herr/pruebas/prueba_usuario.c,
 * un corte en cada operación de flash; evidencia/usuario/cortes.txt]. Escribir lo que ya vale no gasta flash.
 * Reemplazar un fichero necesita sitio para el viejo y el nuevo a la vez (es lo que cuesta ser atómico).
 * Para lo pequeño (ajustes, una marca) sigue estando el almacén de arriba. */
typedef struct { u8 montado, escribible; u32 bloques, tam_bloque, ficheros, ocupado, libre, limpiezas, errores; } HwUsrEstado;
typedef struct { char nombre[48]; u32 tam, fecha; } HwUsrEntrada;   /* fecha: segundos desde 2000-01-01 UTC (0 = sin fecha) */
int  hw_usr_iniciar(void);                           /* lo llama hw_iniciar; HW_OK o HW_NO_DISPONIBLE (sin zona, o sin escritura y sin volumen) */
int  hw_usr_tam(const char *nombre);                 /* bytes del fichero, o HW_NO_DISPONIBLE si no existe (o no hay zona) */
int  hw_usr_leer(const char *nombre, u32 desde, void *buf, u32 max);   /* bytes leídos (0 al final); HW_NO_DISPONIBLE si no existe; HW_ERROR si está dañado */
int  hw_usr_escribir(const char *nombre, const void *d, u32 n);        /* HW_OK; HW_ERROR (no cabe, nombre malo, ya hay 64, o la flash); HW_NO_DISPONIBLE */
int  hw_usr_borrar(const char *nombre);              /* HW_OK; HW_NO_DISPONIBLE si no existe */
int  hw_usr_listar(u32 desde, HwUsrEntrada *e, u32 max);               /* por orden de nombre; devuelve cuántas */
void hw_usr_estado(HwUsrEstado *e);                  /* libre = bytes que caben todavía en UN fichero nuevo (por lo bajo) */
/* (S6 del W580) «MIS ARCHIVOS»: la misma zona vista como la carpeta HW_FS_USUARIO de hw_fs_* (se lista, se lee, y
 * hw_fs_escribir / hw_fs_borrar funcionan ahí), y dos operaciones largas que van POR PASOS para poder pintar una barra
 * de progreso y cancelar (api/usrop.h; todo con la API de usuario.c, sin tocar el formato):
 *   hw_usr_copiar(destino, ruta, reemplazar)  copia a la zona un fichero de CUALQUIER sitio de hw_fs (nuestro FS, /original,
 *                                             la propia zona), por trozos de 4 KiB: no hace falta que quepa en RAM;
 *   hw_usr_renombrar(viejo, nuevo)            copia con el nombre nuevo y, ya comprometida la copia, borra el viejo.
 * Las dos EMPIEZAN (y ahí dicen lo que no puede ser: no cabe, nombre repetido, nombre malo, 64 ficheros) y luego se llama
 * a hw_usr_paso hasta que deje de devolver 1. Mientras hay una en marcha, hw_usr_escribir / hw_usr_borrar dan HW_ERROR
 * con motivo HW_USR_M_OCUPADA (quien guarde en segundo plano tiene que reintentar después: shell/musica.c lo hace).
 * Tras un corte de corriente: copiar deja el destino como estaba o entero; renombrar deja el viejo, los dos o el nuevo
 * (nunca ninguno) [observado: herr/pruebas/prueba_usuario.c, operaciones «copiar» y «renombrar»]. */
#define HW_FS_USUARIO "/usuario"
#define HW_USR_MAX_NOMBRE 47
enum { HW_USR_M_NADA, HW_USR_M_LLENO, HW_USR_M_NOMBRE, HW_USR_M_MUCHOS, HW_USR_M_FLASH, HW_USR_M_EXISTE, HW_USR_M_NO_HAY,
       HW_USR_M_OCUPADA, HW_USR_M_FUENTE, HW_USR_M_MISMO, HW_USR_M_SIN_ZONA, HW_USR_M_SOLO_LECTURA, HW_USR_M_CANCELADA };
int  hw_usr_motivo(void);                            /* HW_USR_M_*: por qué falló la última hw_usr_* de escritura (o NADA si fue bien) */
int  hw_usr_copiar(const char *destino, const char *ruta_fs, int reemplazar);   /* empieza. HW_OK; HW_ERROR (motivo); HW_NO_DISPONIBLE */
int  hw_usr_renombrar(const char *viejo, const char *nuevo);                    /* empieza. Ídem; nunca encima de otro (HW_USR_M_EXISTE) */
int  hw_usr_paso(u32 max, u32 *hecho, u32 *total);   /* copia hasta `max` bytes más (como poco, un trozo de 4 KiB). 1 = sigue; HW_OK = acabada y comprometida; HW_ERROR (motivo; CANCELADA si ya no hay ninguna en marcha) */
void hw_usr_cancelar(void);                          /* abandona la que haya: el destino queda como estaba */
/* (R3; AUD3-M1) UNA OPERACIÓN LARGA DE FLASH AVANZA: lo llaman la limpieza y las escrituras de Mis archivos (api/usuario.c,
 * por registro) y los formatos de zona (por bloque borrado). La plataforma prorroga con ello al perro —acotado: plat/— y, si el
 * shell ha puesto su gancho, este pinta el progreso. total = 0: no se sabe cuánto falta. El gancho NO puede escribir en la flash. */
void hw_progreso(u32 hecho, u32 total);
void hw_progreso_gancho(void (*f)(u32 hecho, u32 total));
void hwo_progreso(void);           /* (R3) lo pone la PLATAFORMA: «la flash trabaja y avanza» (el teléfono prorroga al perro, acotado) */
void plat_bucle(void);             /* (R3) lo pone la plataforma; lo llama el bucle principal en cada vuelta (el teléfono: el arranque llegó al bucle) */
int  hw_usr_ocupada(void);                           /* 1 = hay una copia o un renombrado en marcha */
/* Un fichero cuyo contenido se GENERA (la captura de pantalla): empezar con el tamaño EXACTO, mas…, acabar; todo
 * seguido, sin soltar el bucle. Hasta hw_usr_acabar el fichero no existe (o vale el anterior); hw_usr_cancelar lo deja así. */
int  hw_usr_empezar(const char *nombre, u32 tam);
int  hw_usr_mas(const void *d, u32 n);
int  hw_usr_acabar(void);

/* ---- (I1) «MEMORIA»: W6F, el sistema de ficheros escribible propio (api/w6f.c; api/hw_w6f.c; docs/fs_propio.md;
 * docs/sistema.md §18) -----------------------------------------------------------------------------------------------
 * En el teléfono, 88 bloques de la NOR en modo objeto (0x44500000..0x45AFFFFF, hal/hal.h), ~20 MiB; en el PC, un modelo en
 * RAM (o en el fichero que diga W600_W6F). CON DIRECTORIOS, nombres de 1..HW_W6F_MAX_NOMBRE bytes, cada operación ATÓMICA
 * (guardar, borrar, crear carpeta, renombrar/mover: tras un corte vale entero lo de antes o lo de después; fs2/pruebas).
 * FASE 1 de la migración (docs/fs_propio.md §8): CONVIVE con Mis archivos (W6U), que sigue igual; nada se migra ni se borra.
 * En hw_fs_* es la carpeta HW_FS_MEMORIA de la raíz (la última, detrás de «usuario»): se lista, se lee, y hw_fs_escribir /
 * hw_fs_borrar funcionan ahí. Lo largo (copiar un fichero de cualquier sitio) va POR PASOS, como hw_usr_copiar.
 * Lo que tarda un rato de verdad: dar formato (BORRA los 64 bloques: ~13–18 s en el emulador; en un M18, lo que tarden 64
 * borrados) avisa con hw_progreso (el perro no muerde, la franja «Guardando…» se ve). La recolección va en los ratos muertos
 * del bucle (hw_w6f_ocioso) y, si hace falta, dentro de una escritura, a pasos (también con hw_progreso). */
#define HW_FS_MEMORIA "/memoria"
#define HW_W6F_MAX_NOMBRE 63            /* lo que cabe en HwEntrada.nombre (W6F admite 255: lo de fuera más largo se ve recortado) */
enum { HW_W6F_NO_HAY = 0,               /* sin zona: sin geometría, modo seguro, o sin escritura y sin volumen */
       HW_W6F_MONTADO,                  /* en uso (puede ser de solo lectura: ver escribible y solo_lectura) */
       HW_W6F_VIRGEN,                   /* la zona está en blanco (o un formato se quedó a medias): hay que dar formato, que no destruye nada */
       HW_W6F_AJENO,                    /* la zona tiene algo que NO es de este sistema (en un aparato: restos del main original): se PREGUNTA */
       HW_W6F_OTRA_CONF,                /* un volumen nuestro hecho con OTRA configuración (otro tamaño de zona, menos objetos): NO se toca */
       HW_W6F_OTRA_VERSION,             /* un volumen nuestro de OTRA versión del formato: NO se toca */
       HW_W6F_FALLO };                  /* no se pudo montar por otra causa (memoria, flash) */
typedef struct {
    u8  situacion, escribible;          /* HW_W6F_*; escribible = montado, con permiso y sin solo lectura */
    u8  solo_lectura, deficit;          /* W6fEstado: el motivo (0 no, 1 pedido, 2 secuencia, 3 sin reserva, 4 flash) y la falta de un bloque de reserva */
    u32 capacidad, libre, objetos, max_obj, bloques, bloques_malos, borrados_min, borrados_max;
    u32 programas, borrados, pasos_fondo, peor_paso_ms, montaje_ms;   /* cuentas de esta sesión */
} HwW6fEstado;
typedef struct { u32 tam, fecha; u8 es_dir, danado; } HwW6fInfo;   /* fecha: segundos desde 2000-01-01 UTC (0 = sin fecha) */
enum { HW_W6F_M_NADA, HW_W6F_M_LLENO, HW_W6F_M_NOMBRE, HW_W6F_M_MUCHOS, HW_W6F_M_FLASH, HW_W6F_M_EXISTE, HW_W6F_M_NO_HAY,
       HW_W6F_M_OCUPADA, HW_W6F_M_FUENTE, HW_W6F_M_SIN_ZONA, HW_W6F_M_SOLO_LECTURA, HW_W6F_M_CANCELADA, HW_W6F_M_NO_VACIA,
       HW_W6F_M_NO_DIR, HW_W6F_M_ES_DIR, HW_W6F_M_DANADO, HW_W6F_M_PROFUNDO, HW_W6F_M_DEFICIT, HW_W6F_M_DENTRO };
int  hw_w6f_iniciar(void);              /* lo llama hw_iniciar: monta (sin dar formato nunca). HW_OK si queda montado */
void hw_w6f_estado(HwW6fEstado *e);
int  hw_w6f_formatear(void);            /* da formato si la situación lo admite (VIRGEN; AJENO solo por hw_formatear(HW_ZONA_MEMORIA), que es tras
                                           el sí del usuario); NUNCA con OTRA_CONF / OTRA_VERSION. Tarda (hw_progreso). HW_OK / HW_ERROR / HW_NO_DISPONIBLE */
int  hw_w6f_ocioso(void);               /* el bucle principal, cuando no hay nada que pintar: un paso ACOTADO de recolección si queda trabajo. 1 = hizo algo */
int  hw_w6f_motivo(void);               /* HW_W6F_M_*: por qué falló la última operación de escritura */
int  hw_w6f_info(const char *ruta, HwW6fInfo *i);    /* ruta DENTRO de la Memoria («/fotos/a.png»; «» o «/» = la raíz). HW_OK / HW_NO_DISPONIBLE */
int  hw_w6f_mkdir(const char *ruta);
int  hw_w6f_borrar(const char *ruta);                /* fichero, o carpeta VACÍA */
int  hw_w6f_renombrar(const char *vieja, const char *nueva);   /* renombra y/o mueve, atómico; nunca encima de otro (HW_W6F_M_EXISTE) */
int  hw_w6f_copiar(const char *destino, const char *ruta_fs, int reemplazar);   /* empieza: destino DENTRO de la Memoria, origen cualquier ruta de hw_fs */
int  hw_w6f_paso(u32 max, u32 *hecho, u32 *total);   /* como hw_usr_paso: 1 = sigue; HW_OK = acabada y comprometida; HW_ERROR (motivo) */
void hw_w6f_cancelar(void);                          /* abandona la copia: el destino queda como estaba */
int  hw_w6f_ocupada(void);
/* (SER) recibir a trozos (apps/recibir, api/serie_rx.c): un fichero que no existe hasta hw_w6f_rx_cerrar; abortar no deja nada */
int  hw_w6f_rx_abrir(const char *ruta);
int  hw_w6f_rx_mas(const void *d, u32 n);
int  hw_w6f_rx_cerrar(void);
void hw_w6f_rx_abortar(void);
/* (MTP; AUD9 M2) la ranura de recepción tiene DUEÑO: las cuatro de arriba son las de SER (dueño HW_RX_SER); MTP usa las _d con
 * HW_RX_MTP. Con la ranura de otro: abrir, mas y cerrar dan HW_ERROR (motivo HW_W6F_M_OCUPADA) y abortar no hace nada. */
enum { HW_RX_SER = 1, HW_RX_MTP = 2, HW_RX_RED = 3 };   /* (1.11) RED: una canción que se descarga a la tarjeta (shell/descarga_red.c) */
int  hw_w6f_rx_abrir_d(const char *ruta, int dueno);
int  hw_w6f_rx_mas_d(int dueno, const void *d, u32 n);
int  hw_w6f_rx_cerrar_d(int dueno);
void hw_w6f_rx_abortar_d(int dueno);
int  hw_w6f_rx_dueno(void);                          /* 0 = libre; HW_RX_* */
u32  hw_w6f_cambios(void);                           /* (MTP; AUD9 A1) sube con CADA operación que puede cambiar la Memoria (y al montar) */
int  hw_w6f_renombrar_r(const char *vieja, const char *nueva, int reemplazar);   /* como hw_w6f_renombrar, y con reemplazar = 1 pisa al destino (atómico) */
u32  hw_w6f_libre(void);                             /* el fichero NUEVO más grande que cabe ahora (0 si no se puede escribir) */
/* (MTP) para el respondedor MTP (api/mtp.c; docs/mtp.md): los objetos POR ID (estable mientras el objeto vive; 0 = la raíz),
 * la lectura por id y el id que reserva hw_w6f_rx_abrir (el handle de un SendObjectInfo). La escritura a trozos y el renombrado
 * con reemplazo son los de SER, de arriba. */
typedef struct { u32 id, padre, tam, fecha; u8 es_dir, danado; char nombre[256]; } HwW6fObj;
int  hw_w6f_obj(u32 id, HwW6fObj *o);                /* HW_OK / HW_NO_DISPONIBLE (no existe o no se puede leer) */
int  hw_w6f_hijo(u32 dir, u32 *cursor, HwW6fObj *o); /* *cursor = 0 al empezar. 1 = hay uno; 0 = fin; HW_ERROR (no es una carpeta) */
int  hw_w6f_id(const char *ruta);                    /* el id de una ruta DENTRO de la Memoria (0 = la raíz), o < 0 */
u32  hw_w6f_max_obj(void);                           /* ids posibles: 1 .. max_obj-1 (0 sin Memoria) */
int  hw_w6f_rx_id_d(int dueno);                      /* el id que tendrá lo que recibe ese dueño (W6F lo reserva al abrir), o < 0 */
int  hw_w6f_leer_id(u32 id, u32 desde, void *buf, u32 max);   /* bytes (0 al final); < 0 = no existe, es carpeta, o la página está rota */

/* ---- energía ---------------------------------------------------------------------------------------- */
/* (CARGA, 25-oct-2026) LA BATERÍA SE CARGA POR USB, como el original (api/carga.h; docs/bateria.md §12): solo con el PC que ha
 * CONFIGURADO el teléfono (500 mA), la batería identificada y entre 5 y 45 °C; el chip corta solo a 4,1 V. Con el CARGADOR de pared
 * (0x23 bit 3) NO se carga (no está medido en el aparato): la corriente de carga del PMU por esa vía (0x61) sigue a 0 siempre.
 * Antes (pista S7, docs/bateria.md §8.A): no se cargaba nunca. */
#define HW_BAT_MV_NO_CREIBLE 2850   /* por debajo la lectura no es creíble (el original la descarta): ni se avisa ni se apaga por ella; se dice en Sistema */
#define HW_BAT_MV_AVISO 3500        /* por debajo: aviso «batería baja» (una vez) */
#define HW_BAT_MV_CORTE 3400        /* dos sondeos seguidos por debajo: guardar y apagar. Son BAT_MV_AVISO y BAT_MV_CORTE de api/bat.h */
int hw_bateria_mv(void);           /* milivoltios de la batería, medidos como el original EN MARCHA: la «medida automática» del PMU (registro 0x77, sin
                                      pulso) por su recta por defecto, mV = 2755 + 10,843 x cuenta (108 -> 3926). Si 0x77 da 00 o FF, la lectura «en frío»
                                      (0x70 <- 0x6A) por la suya (2796 + 10,784 x lectura); hw_bateria_fuente() dice cuál. Se relee como mucho cada 5 s.
                                      [sin saber] la calibración de ESE aparato (su GDFS): puede errar decenas de mV. HW_NO_DISPONIBLE si el PMU no contesta */
int hw_bateria_pct(void);          /* 0..100 por la tabla de capacidad de fábrica del original (4155 mV = 100 … 3510 = 2), con el 0 en 3400 mV, que es donde
                                      ESTE sistema se apaga (por debajo de 3400 el original se niega a ARRANCAR; en marcha él corta a 3250). HW_NO_DISPONIBLE sin PMU */
int hw_bateria_fuente(void);       /* 0 = el registro 0x77 (lo normal); 1 = la reserva 0x6A (0x77 dio 00 o FF); HW_NO_DISPONIBLE sin PMU */
int hw_bateria_presente(void);     /* 1 = hay batería; 0 = el PMU dice que NO está conectada (0x23 bit 6); HW_NO_DISPONIBLE sin PMU */
/* Cómo fue el arranque en lo que toca a la batería (la comprobación temprana de hw_iniciar, antes de encender nada; hal/bat.h):
 * con 2850 <= mV < 3400 y nada enchufado el teléfono NO llega aquí (tres toques de vibrador y se apaga). */
enum { HW_ARR_BIEN = 0, HW_ARR_BAT_NO_CREIBLE = 2, HW_ARR_CON_CARGADOR = 3 };
typedef struct { int como;         /* HW_ARR_*: bien; lectura por debajo de 2850 mV o sin lectura (se siguió, sin fiarse); había cargador o USB */
                 int lectura;      /* la lectura en frío (0x6A) de ese momento, o -1 */
                 int mv;           /* esa lectura por la recta del arranque, o -1 */
                 int estado23;     /* el registro 0x23 de ese momento, o -1 */ } HwArranque;
void hw_arranque(HwArranque *a);
/* La carga FUERA (PMU 0x61 <- 0x10, 0x63 <- 0x00), otra vez: lo llama quien va a dejar al sistema sin su bucle (ceder la pantalla a
 * una app externa). No hace falta en marcha normal: ya está así desde el arranque. HW_ERROR si el PMU no contesta */
int hw_carga_fuera(void);
/* (R2; auditoría de S7, A1/A3/A4) Lo que hay que poder VER del PMU y de la tecla de encendido, y vigilar la carga. */
typedef struct { unsigned char carga_confirmada;   /* 0x61 y 0x63 RELEÍDOS como 10 y 00 */
                 short r61, r63;                   /* lo último releído (-1 = no se pudo leer) */
                 unsigned char lista_mal;          /* escrituras de la lista de inicio del PMU que fallaron */
                 unsigned i2c_topes, i2c_nack, i2c_reintentos, i2c_recuperados;
                 unsigned char enc_foto_armada, enc_nivel_armado, enc_pmu_armado, enc_pmu_probado, enc_descartes; } HwPmuSalud;
void hw_pmu_salud(HwPmuSalud *p);
int hw_carga_vigilar(void);        /* relee 0x61/0x63: 0x61 tiene que ser 10 y 0x63 sin corriente (salvo que la carga USB esté cargando); si no, los
                                      reescribe. HW_OK = confirmado; HW_ERROR = no se ha podido */
/* (CARGA) Un paso de la carga por USB (api/carga.h): lee el PMU (0x23, 0x63, bornes, BDATA cada minuto), decide y escribe 0x63. Se
 * puede llamar en cada vuelta: actúa como mucho una vez por segundo. usb_ma = lo que el PC concede (usb_carga_ma()). Devuelve el
 * estado CARGA_*; HW_NO_DISPONIBLE donde la plataforma no carga. */
int hw_carga_paso(int usb_ma);
typedef struct { int estado, motivo;      /* CARGA_* y CARGA_M_* de api/carga.h (estado HW_NO_DISPONIBLE: la plataforma no carga) */
                 int mv;                  /* bornes del último paso (mV), -1 */
                 int temp_dc, tipo, bdata;   /* batería: décimas de °C (-32768 = sin medir), tipo 1/2/0 (255 = sin medir), cuenta del ADC 11 */
                 int icod, pedida, r63;   /* el código de corriente programado, la pedida (0,1 mA) y 0x63 releído */
                 unsigned ms_cargando;    /* en esta enchufada */
                 int techos, escalones, usb_ma;
                 int fg_ok; long fg;      /* el medidor (sigma-delta: ≈ 9,4 por mA, + = entra en la batería) */ } HwCarga;
void hw_carga_estado(HwCarga *c);
int hw_encendido_mantenida(void);  /* la tecla de encendido mantenida, según api/enc.h (para la pulsación larga; hw_tecla_encendido es la lectura cruda) */
int hw_borrar_usuario(void);       /* el segundo rescate del modo seguro: borra Mis archivos entero (solo en modo seguro) */
/* DIAGNÓSTICO de solo lectura para la pantalla «Energía (PMU)» de Sistema (las pruebas T1–T4 de docs/bateria.md §9): los registros
 * del cargador RELEÍDOS DEL CHIP y las medidas. Cada campo es 0..255, o -1 si esa lectura falló. Solo se llama con la pantalla abierta:
 * son 11 lecturas de registro y 3 conversiones con pulso (ADC 8, 9 y 1), que el original también hace (docs/bateria.md §1, §2, §3.5). */
typedef struct { short r23, r60, r61, r62, r63, r75, r76, r77, r7a, l6a, adc8, adc9, adc1; } HwPmuDiag;
int hw_pmu_diag(HwPmuDiag *d);
/* (2026-10-21) EL BANCO DEL ACELERÓMETRO Y DEL ALTAVOZ (Sistema -> Opciones -> «Banco acel/audio»): lo que hay que ver en el aparato
 * cuando el acelerómetro no se mueve y el altavoz no suena (tmp/acel_re/informe.md §5 E1, tmp/sonido_re/informe.md §6.1). Cada campo
 * 0..255 (o el registro de 16 bits), -1 = la lectura falló. Lo implementa la plataforma con un símbolo DÉBIL (solo el W580i). */
typedef struct {
    int f5000800, syscon16, p1, p1dir, p3, p3dir;      /* releídos */
    int l6a;                                           /* 0x70 <- 0x6A, 0x71 sin pulso (la batería «en frío», la que SÍ funciona) */
    int a_y, a_x, a_1, a_8;                            /* A: 0xA0|canal + pulso (lo de siempre): canales 4, 12, 1, 8 */
    int i_y, i_x, i_1, i_8;                            /* I: 0x60|canal SIN pulso (la forma del 0x6A) */
    int s_y, s_x;                                      /* S: 0xA0|canal SIN pulso, 2 ms de espera */
    int bit14, acel, codec, esc_mal;                   /* estado: F5000800 bit 14 puesto por nosotros, P1.1, códec abierto, escrituras I²C falladas al abrirlo */
    int c0, c7, c8, cb, cc, cd;                        /* el códec releído */
    u32 vueltas;
} HwBanco;
enum { HW_BANCO_LEER = 0, HW_BANCO_BIT14, HW_BANCO_ACEL, HW_BANCO_CODEC_ABRIR, HW_BANCO_CODEC_CERRAR, HW_BANCO_POPS, HW_BANCO_TONO, HW_BANCO_AURICULAR };     /* HW_OK, o HW_NO_DISPONIBLE si el PMU no contesta a nada */
int hw_tecla_encendido(void);      /* 1 si la tecla de encendido está pulsada AHORA (celda a masa 30; el PMU la da también en 0x23 bit 0) */
int hw_pantalla_dormir(int si);    /* 1 = apaga el panel con el guion power_off del original (R07/R10); 0 = lo vuelve a encender (guion power_on, ~0,4 s).
                                      [emulador: los registros llegan al modelo]; la luz se maneja aparte con hw_luz(0, …) */
int hw_cargador(void);             /* bit 0 = cargador, bit 1 = USB (PMU, registro 0x23 bits 3 y 7); HW_NO_DISPONIBLE si el PMU no contesta. OJO: que haya
                                      cargador NO quiere decir que se cargue: este sistema no carga. Se relee con cada suceso del PMU (IRQ 37) y, por si no
                                      llegara, una vez por segundo */
int hw_adc(int canal);             /* lectura CRUDA de 8 bits del ADC del PMU, canal 0..13: 1 = temperatura de la placa, 4 y 12 = acelerómetro, 8 = tensión del
                                      cargador (x 25 mV), 9 = corriente por el transistor de carga [inferido], 11 = línea BDATA de la batería, 13 = accesorio
                                      (docs/bateria.md §2). El 10 NO se convierte (el original no lo hace): devuelve la cuenta de la batería de hw_bateria_mv().
                                      HW_NO_DISPONIBLE sin PMU. [emulador] */
int hw_vibrar(int ms);             /* W580i: el motor (PMU 0xA0 = 0x20, continuo) durante ms (tope 2 s; 0 = parar). [medido] con el acelerómetro; HW_ERROR si el PMU no contesta. PC: HW_NO_DISPONIBLE */
int hw_vibrar_patron(int hz, int pct, int ms);   /* (API 1.5) continuo (pct 100) o a golpes: 1/2/4/8 Hz con el 25/50/75 % del periodo (lo más cercano;
                                                  hal/i2c.c pmu_vibrador_patron) durante ms (tope 10 s); ms, hz o pct <= 0 = parar. PC: HW_NO_DISPONIBLE */

/* ---- ACELERÓMETRO (W580i; docs/hal.md §13) -------------------------------------------------------
 * Analógico, de dos ejes, por el ADC del PMU (X = canal 12, Y = canal 4), alimentado por P1.1. No interrumpe: se
 * sondea. La lectura es CRUDA, 0..255; el cero (sin aceleración) anda por HW_ACEL_CERO. [sin saber] cuántas cuentas
 * son 1 g (por los umbrales del podómetro original, 15..25) y cómo van los ejes respecto a la carcasa.
 * El sensor tarda ~15 ms en asentarse tras alimentarlo: quien lo use lo alimenta, espera y lee (el original
 * alterna «encender» y «medir» cada 20 ms: 25 muestras por segundo). [emulador: propiedades pmu-adc12 / pmu-adc4];
 * en el PC, una señal simulada que mueve el guion (órdenes «acel» y «andar»). */
#define HW_ACEL_CERO 125
int hw_acel_alimentar(int si);     /* 1 = enciende el sensor, 0 = lo apaga. HW_OK */
int hw_acel_leer(int *x, int *y);  /* HW_OK, o HW_NO_DISPONIBLE si el PMU no contesta */
/* (2026-10-20, tmp/acel_ag) DIAGNÓSTICO DEL ADC DEL ACELERÓMETRO: el modo de conversión en uso, lo que dio cada modo en la última
 * prueba del autodetector (hal/i2c.c) y el rango de X/Y visto. Lo implementa el W580i con un símbolo DÉBIL (banco: apps/sistema/banco_hw.c).
 * Modos, por índice: "APRBIJS" (A = A0|canal + pulso corto = v3/v4; P = pulso con la puerta 40 abierta toda la conversión; R = P tras
 * el pulso de 0x72; B = P con F5000800 bit 14 a 0; I = 60|canal sin pulso; J = I con espera; S = A0|canal sin pulso). */
#define HW_ADC_MODOS 7
typedef struct {
    int modo, forzado, auto_modo, hallado, intentos;   /* modo en uso; forzado (-1 = automático); lo que eligió la prueba (-1 = ninguno vivo) */
    int v[HW_ADC_MODOS][4];                            /* por modo: canales 1, 8, 4 (Y), 12 (X) en la última prueba; -1 = lectura fallida */
    int veneno[HW_ADC_MODOS], viejos[HW_ADC_MODOS], distintos[HW_ADC_MODOS], vivo[HW_ADC_MODOS];   /* vivo: bit 0 ronda 1, bit 1 ronda 2 */
    int x, y, xmin, xmax, ymin, ymax;                  /* la lectura de ahora (modo en uso, sensor alimentado 20 ms) y el rango visto */
    int r70, r72, r73, p11;                            /* PMU 0x70 / 0x72 / 0x73 releídos; P1.1 (alimentación) */
    u32 lecturas, fallos;
} HwAcelDiag;
enum { HW_ACEL_DIAG_LEER = 0, HW_ACEL_DIAG_PROBAR, HW_ACEL_DIAG_SIGUIENTE, HW_ACEL_DIAG_RANGO };   /* PROBAR = autodetector ya; SIGUIENTE = forzar el modo siguiente (tras el último, automático); RANGO = borrar el rango */
int hw_acel_diag(int accion, HwAcelDiag *d);   /* HW_OK, o HW_NO_DISPONIBLE */

/* ---- GPIO: 5 puertos (0..4) de 8 pines ---------------------------------------------------------------
 * [emulador: cerrojo] los registros guardan lo escrito; no hay nada conectado detrás. */
int hw_gpio_modo(int puerto, int pin, int salida);   /* 1 = salida, 0 = entrada */
int hw_gpio_poner(int puerto, int pin, int valor);
int hw_gpio_leer(int puerto, int pin);               /* 0/1, o HW_ERROR */
int hw_tapa(void);                                   /* W580i: el DESLIZADOR (P2.6). 1 = ABIERTO, 0 = cerrado; la polaridad es DESLIZADOR_CERRADO_ES (hal/hal.h), [inferido] */
int hw_tapa_cerrada(void);                           /* 1 = tapa cerrada (línea P1.2 a 1), 0 = abierta (docs/emulador_radio.md §7). [emulador: propiedad «flip»] */
u32 hw_tapa_cambios(void);                           /* veces que la interrupción del pin ha visto cambiar la tapa (0 si la IRQ no llega: entonces solo hay sondeo) */

/* ---- I²C -------------------------------------------------------------------------------------------- */
int hw_i2c_escribir(int dir7, const u8 *d, int n);   /* START, dirección, n bytes, STOP. Devuelve n, o HW_ERROR si el bus no contesta (con tope: no cuelga). [emulador] */
int hw_i2c_leer(int dir7, int reg, u8 *d, int n);    /* escribe «reg» (si reg >= 0), START repetido y lee n bytes. Devuelve n o HW_ERROR. [emulador] */

/* ---- puerto serie (UART 0, 115200 8N1, la del conector; pensada para el ESP32-C3) --------------------- */
int hw_serie_conectado(void);                        /* 1 si el pin de detección ve algo al otro lado. [emulador] */
int hw_serie_escribir(const u8 *d, int n);           /* devuelve los bytes aceptados (0 si no hay nadie: no cuelga). [emulador] */
int hw_serie_leer(u8 *d, int max);                   /* no bloquea: lo recibido hasta ahora (la IRQ 44 lo va guardando en un anillo de 1 KiB). [emulador] */

/* ---- SONIDO (pista S5; la HAL es la de la pista S4, docs/audio.md) -------------------------------------
 * Dos caminos, independientes:
 *
 *  1. EL ZUMBADOR: el generador de frecuencia 0xF9020000 sacado por el pin P0.7 (TGBUZZ). Es un pin y
 *     tres registros: no necesita DSP, no necesita códec y no se puede mezclar. Es por donde van TODOS
 *     los avisos del sistema (teclas, alarma, batería baja). **Suena de verdad** [observado, medido en
 *     un WAV del emulador de S4: la4 = 440,0 Hz exacta, cuadrada al 50 %]. Volumen: no hay registro de
 *     volumen que sepamos usar, así que el «volumen» es el ANCHO DEL PULSO (menos tiempo arriba, menos
 *     energía); eso SÍ se mide en el WAV. [sin saber] si el pin del W600 llega a un altavoz: eso lo
 *     contesta el teléfono (docs/audio.md §6.1).
 *  2. EL PCM por el DSP (DSPIF 0xFA040000): música. Hace falta que el DSP arranque y conteste, y para
 *     eso hay que subirle su código, que NO tenemos. Así que en el emulador estos hw_pcm_* devuelven
 *     HW_NO_DISPONIBLE salvo con «dsp-sin-nucleo=on», que abre el transporte sin ese paso: entonces el
 *     PCM sale del ARM por el puerto correcto y se graba [observado]. Lo que el DSP haría con él:
 *     [sin saber] (docs/audio.md §4.3). Quien llame esto tiene que DEGRADAR CON GRACIA. */
int hw_tono(u32 hz, int ciclo_pct);                  /* 397 Hz … 20 kHz; HW_ERROR fuera de rango. ciclo 100 = onda cuadrada */
int hw_tono_fin(void);
int hw_tono_ms(u32 hz, int ciclo_pct, u32 ms);       /* lo mismo, pero se apaga SOLO a los `ms` (lo apaga el tic de 4 ms, no el bucle:
                                                      * la nota dura lo pedido aunque el sistema esté pintando). Para clics y melodías */
int hw_tono_mantener(u32 hz, int ciclo_pct);         /* (pista S7) deja el zumbador dando EXACTAMENTE esto (hz = 0: callado) y NO escribe ni un
                                                      * registro si ya lo estaba dando: se puede llamar en cada tic sin reiniciar la onda.
                                                      * Devuelve 1 si ha escrito, 0 si no hacía falta, HW_ERROR fuera de rango. Lleva una SOMBRA de
                                                      * lo último que se pidió (no relee el generador: [sin saber] si en un teléfono se deja leer).
                                                      * Es lo que usa la música para recuperar el zumbador después de un aviso del sistema */
int hw_tic_gancho(void (*f)(void));                  /* (pista S7) `f` se llamará en CADA tic de 4 ms, DESDE LA INTERRUPCIÓN: tiene que ser corta, no
                                                      * puede pintar ni tocar la UI ni el montículo, y solo puede usar hw_ms, hw_tono_mantener y memoria
                                                      * propia. NULL lo quita. HW_OK; HW_NO_DISPONIBLE donde no hay tic (el PC): entonces quien lo pide
                                                      * tiene que sondear desde el bucle. Con el reposo profundo (hw_dormir) el tic se para: no late */
#define HW_SND_ZUMBADOR 1
#define HW_SND_PCM      2
int hw_sonido_hay(void);                             /* qué caminos están disponibles (bits HW_SND_*) */

int  hw_pcm_init(u32 rate, int canales);             /* enciende el DSPIF, declara el formato y abre el flujo. HW_OK o HW_NO_DISPONIBLE.
                                                        PUEDE TARDAR ~0,5 s (espera la respuesta del DSP): solo se llama cuando el usuario
                                                        pide música, nunca en el arranque */
int  hw_pcm_motivo(void);                            /* 0, o en qué paso falló hw_pcm_init (el negativo de audio_init: -3 formato, -4..-8 mezclador, -9 flujo, -10 arranque) */
int  hw_pcm_escribir(const short *pcm, u32 marcos);  /* entrega un bloque ya mezclado (marcos × canales muestras intercaladas).
                                                        Devuelve los marcos entregados; ≤ 0 = no se pudo (NO bloquea: mejor un hueco que colgar) */
void hw_pcm_cerrar(void);
u32  hw_pcm_marcos(void);                            /* marcos entregados desde hw_pcm_init */

/* ---- MP3 POR EL DSP (pista MP3, 2026-10-21; hal/mp3_w580.c; protocolo: tmp/dsp_emu/protocolo_mp3.md; informe: tmp/mp3/informe.md) ---------------------
 * El MP3 lo DECODIFICA EL DSP del W580i (módulo MP3, igual que el original): el ARM le manda el fichero en bruto y el DSP saca el PCM por el puerto PCM al códec del PMU
 * y al altavoz, con los parámetros acústicos del FS de fábrica (APF). El DSP NO se arranca hasta hw_mp3_play(); con cable USB puesto se NIEGA (HW_MP3_E_CABLE).
 * Solo existe en el W580i (el resto de placas y el PC: todo HW_NO_DISPONIBLE). Todo tiene tope de tiempo: ante cualquier fallo se apaga ordenadamente y queda el motivo.
 * hw_mp3_play y hw_mp3_parar BLOQUEAN (hasta ~4 s la primera vez: arranque del DSP y carga del módulo); pausa, volumen y búsqueda, unos 0,1-0,3 s. */
enum { HW_MP3_PARADO = 0, HW_MP3_ABIERTO, HW_MP3_SONANDO, HW_MP3_PAUSA, HW_MP3_FIN, HW_MP3_ERROR };
typedef struct {
    u8  estado, canales, vbr, volumen;           /* HW_MP3_*; 1 mono / 2 estéreo; 1 = VBR; nivel 0..15 */
    u32 fs, kbps, dur_ms, pos_ms, tam;           /* Hz, kbit/s, ms, ms, bytes del fichero */
    int motivo;                                  /* 0 o el primer fallo (negativo; hw_mp3_motivo_txt lo dice) */
} HwMp3Info;
int  hw_mp3_abrir(const char *ruta);             /* abre un .mp3 (ID3v2, primer fotograma, Xing); si hay otro sonando lo cierra. HW_OK / HW_ERROR (motivo) / HW_NO_DISPONIBLE */
int  hw_mp3_play(void);                          /* de abierto/parado: la secuencia completa del original; de pausa: sigue. HW_OK = suena; HW_ERROR = no (hw_mp3_motivo) */
int  hw_mp3_pausa(void);                         /* rampa de bajada del volumen y 0x0E02{2} */
int  hw_mp3_parar(void);                         /* para, libera el decodificador y baja la línea; ~9 s después apaga el códec y el DSP */
int  hw_mp3_volumen(int nivel);                  /* 0..15: 16 niveles de 3 dB; 7 = el de fábrica del original (ganancia 0x0200) */
int  hw_mp3_buscar(u32 ms);                      /* salta a esa posición de la pista (sonando o en pausa) */
int  hw_mp3_estado(void);                        /* HW_MP3_*; HW_MP3_FIN = la pista acabó (el DSP avisó de su fin) */
int  hw_mp3_activo(void);                        /* 1 = el DSP o la línea de audio siguen encendidos (sonando, en pausa o en el cierre diferido de ~9 s): no dormir a fondo */
int  hw_mp3_cerrar_ya(void);                     /* (1.10) para y apaga YA el códec y el DSP (sin el cierre diferido): para cederle el DSP al micrófono */
u32  hw_mp3_tiempo(u32 *dur_ms);                 /* ms transcurridos (los cuenta el ARM: el DSP no manda la posición) y, si dur_ms != NULL, la duración */
int  hw_mp3_motivo(void);                        /* 0, o el último fallo (HW_MP3_E_*) */
const char *hw_mp3_motivo_txt(int motivo);       /* texto corto en español del motivo */
int  hw_mp3_info(HwMp3Info *i);
void hw_mp3_paso(void);                          /* una vez por vuelta del bucle: créditos del DSP, datos del fichero, fin de pista, cierre diferido. No bloquea */
/* (2026-10-25) LA FUENTE EN RAM («flujo»): en vez de un fichero, un COLCHÓN (anillo) que alguien rellena con el MP3 según le llega (la red: la base
 * del «Spotify», docs/mp3_dsp.md §6). hw_mp3_flujo_abrir deja la pista abierta sin leer nada; hw_mp3_play lee la cabecera del propio colchón (hace
 * falta tener ya unos KB: si no, HW_ERROR con motivo HW_MP3_E_FALTAN y se reintenta luego). Si el colchón baja de `bajo` bytes sonando y no se
 * ha dicho hw_mp3_flujo_fin, el motor hace una PAUSA LIMPIA (rampa y 0x0E02 {2}, como la tecla) y sigue solo al volver a `alto`; una pausa del
 * usuario manda (no sigue sola). Sin `total` la duración es 0 (no se sabe). Nada de esto bloquea. */
typedef struct {
    u8  activo, fin, esperando;                  /* hay un flujo abierto; no llegará más; en pausa por falta de datos */
    u32 tam, nivel, total, bajo, alto;           /* colchón (bytes), lo que hay sin mandar al DSP, total si se sabe, umbrales */
    u32 escritos, mandados;                      /* bytes que entraron y que ya se dieron al DSP */
    u32 esperas, ms_esperando;                   /* veces que se vació y ms en total esperando datos */
} HwMp3Flujo;
#define HW_MP3_E_FALTAN (-24)                    /* (flujo) aún no hay datos bastantes para leer la cabecera */
int  hw_mp3_flujo_abrir(void *colchon, u32 tam, u32 total);   /* HW_OK; el colchón es de quien llama y tiene que vivir hasta hw_mp3_parar; (1.8) colchon NULL = el del
                                                               * SISTEMA (192 KB, el de los ficheros: shell/musica_red.c) */
u32  hw_mp3_flujo_escribir(const void *d, u32 n);             /* COPIA lo que quepa; devuelve los bytes que entraron (0 = lleno o sin flujo) */
u32  hw_mp3_flujo_libre(void);                                /* bytes que caben ahora */
void hw_mp3_flujo_fin(void);                                  /* no llegará más: cuando se vacíe, fin de pista */
void hw_mp3_flujo_info(HwMp3Flujo *i);
#define HW_MP3_E_CABLE (-22)                     /* = MP3_E_CABLE de hal/mp3_w580.h */
#endif
