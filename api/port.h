/* port.h — LO QUE IMPLEMENTA UN PORT. Lista CERRADA: si un modelo nuevo necesita algo que no está aquí, se discute
 * y se sube PORT_API; no se añade un #ifdef al núcleo. Encima de esto, plat/tel/ (común a todos los teléfonos)
 * implementa la API pública hw_* (api/hw.h): repetición de teclas, fases de luz, perro, pantalla de fallo, protección
 * de la flash, formato pendiente, modo seguro… Un port NO reescribe nada de eso.
 *
 * Cada función: CONTRATO (qué garantiza) y PRUEBA (cómo se demuestra; C = prueba de conformidad en el PC contra un
 * modelo; E = en el emulador estricto del modelo; A = en el aparato, con el elf de diagnóstico, SOLO LECTURA).
 * Convenio: 0 = bien, < 0 = no se pudo; NINGUNA bloquea sin tope (toda espera lleva tope de vueltas y lo cuenta). */
#ifndef PORT_H
#define PORT_H
#include "placa.h"
#define PORT_API 1

/* ---- NIVEL 0: «se ve y se toca» (hitos 2..5). Con esto arranca el sistema entero SIN persistencia --------------- */
int  port_iniciar_temprano(void);      /* relojes, RAM, UART de trazas. NO enciende nada que gaste (ni luz ni panel). C: orden de llamadas. E: traza = original */
int  port_bat_arranque(int *lectura, int *estado);   /* lectura en frío y estado del cargador ANTES de encender nada; -1 = sin lectura. E: e7 */
int  port_carga_cero(void);            /* corriente de carga a 0, RELEÍDA. Se llama lo primero y antes de ceder el bucle. E: registros releídos */
int  port_iniciar(void);               /* interrupciones, tic, teclado, panel (detectado, no supuesto), RTC. E: hitos */
u32  port_ms(void);                    /* monótono; resolución <= 1000 / tic_hz. C: no retrocede */
void port_reposo(void);                /* duerme hasta la siguiente interrupción (como tarde un tic) */
void port_pantalla(const u16 *cuadro, int y0, int filas);   /* filas completas, en placa()->pixel. E: suma de la captura */
int  port_teclas(u8 *mapa);            /* matriz cruda, placa()->filas (+1 si a_masa) bytes; LECTURA PURA, vale desde la IRQ del tic. E: cada celda */
int  port_luz(int pct);                /* retroiluminación 0..100, recortada al máximo del original. E: registro */
void port_traza(const char *s);        /* UART; sin cable no cuelga */
void port_reiniciar(void);             /* no vuelve */
int  port_apagar(void);                /* no vuelve; si vuelve (< 0), TODO queda apagado y la carga a 0 */
int  port_tecla_encendido(void);       /* 1/0 AHORA por una vía que no dependa del bucle; -1 = no hay otra vía */
void port_mem_app(u8 **base, u32 *tam);/* la RAM que el sistema no usa (para apps): sale del guion de enlace, no del núcleo */
void port_tic_gancho(void (*f)(void)); /* f en cada tic, desde la interrupción (perro, tono con plazo, música) */

/* ---- NIVEL 1: persistencia. Sin este nivel el sistema arranca «sin escritura» (ya existe: HW_PROT_COMPILADO) ---- */
typedef struct { u32 fabricante, dispositivo, tam, bloque_borrado, bufer_programa; u16 juego_ordenes; u8 cfi_valida; } PortFlash;
int  port_flash_identificar(PortFlash *f);            /* lee ID y CFI; no escribe. A: diag (solo lectura) */
int  port_flash_borrar(u32 dir);                      /* un bloque; se niega fuera de una PlacaZona escribible. C: prueba_m18 / modelo; E: estricto */
int  port_flash_programar(u32 dir, const void *d, u32 n);   /* ídem; con verificación. C, E */
const u8 *port_flash_leer(u32 dir);                   /* puntero de lectura (NOR mapeada) o NULL si hay que copiar (NAND) */
int  port_flash_copiar(u32 dir, void *buf, u32 n);    /* lectura por copia: vale para NOR y NAND */

/* ---- NIVEL 2: reloj y energía en marcha ----------------------------------------------------------------------- */
typedef struct { u16 anio; u8 mes, dia, hora, min, seg; } PortFecha;
int  port_rtc_leer(PortFecha *f);
int  port_rtc_poner(const PortFecha *f);
int  port_bat_marcha(void);            /* cuenta cruda en marcha (se pasa por placa()->bat.marcha); -1 = sin lectura */
int  port_cargador(void);              /* bit 0 cargador, bit 1 USB; -1 */
u32  port_dormir(u32 max_ms);          /* reposo profundo; devuelve lo dormido. Un port puede devolver 0 (no duerme) */

/* ---- OPCIONALES: cada grupo va con su CAP_* en placa(). Sin la capacidad, plat/tel/ ni las llama ---------------- */
int  port_tono(u32 hz, int ciclo_pct);                /* CAP_ZUMBADOR; hz = 0 calla */
int  port_vibrador(int si);                           /* CAP_VIBRADOR */
int  port_tapa(void);                                 /* CAP_TAPA: 1 = cerrado */
int  port_acel(int alimentar, int *x, int *y);        /* CAP_ACELEROMETRO */
int  port_luz_sencilla(int n, int nivel);             /* CAP_LUZ_TECLADO: n < placa()->luces_sencillas */
int  port_luz_rgb(int terna, int r, int g, int b, int vel, int modo);   /* CAP_LUZ_RGB */
int  port_rtc_alarma(int hora, int min);              /* CAP_RTC_ALARMA */
int  port_serie(int escribir, u8 *d, int n);          /* CAP_SERIE */
int  port_pcm(const short *pcm, u32 marcos);          /* CAP_PCM */
int  port_diag(int cual, short *valores, int max);    /* diagnóstico de SOLO LECTURA propio de la placa (pantalla «Energía»): pares nombre/valor en placa_int */
#endif
