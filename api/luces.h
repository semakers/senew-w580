/* luces.h — (pista S5) EL MOTOR DE LUCES, portable: el mismo código en el teléfono y en el PC.
 *
 * Tres piezas, de abajo arriba (todo sale de leer el firmware original: docs/luces.md):
 *  1. el INTÉRPRETE de los efectos «.ble» del W580i (bytecode de palabras de 32 bits; FUN_45391f48): ble_*;
 *  2. el MOTOR que ejecuta sus órdenes sobre las seis luces: poner, fundir por pasos de 50 ms (FUN_4529ce78) y las tres
 *     órdenes de las ternas RGB, que funde el propio chip con su PWM: lu_*;
 *  3. el REPRODUCTOR: un efecto a la vez, desde un fichero (los 30 de fábrica están en el FS original) o desde
 *     memoria, avanzado por pasos desde el bucle del sistema (nunca bloquea): lu_efecto_*.
 * Debajo solo hay las primitivas de la plataforma (hw_luz_salida, hw_luz_rgb_*; api/hw.h).
 *
 * Los «ledid» son los del original: 0 = pantalla, 1 = teclado del deslizador, 2 = teclado zona 2, 3 = teclado zona 1;
 * las ternas RGB (las laterales) son «rgb 1» y «rgb 2» en los ficheros y terna 0 y 1 aquí. Todo en % (0..100), como
 * en los ficheros; a niveles de registro los pasa el motor con la cuenta del original (% × máximo / 100).
 * [sin saber] hasta tener un aparato: qué LED físico es cada salida, sus colores y cuánto dura cada rampa del chip. */
#ifndef LUCES_H
#define LUCES_H
#include "hw.h"

/* ---- 1. el intérprete ------------------------------------------------------------------------------------ */
enum { BLE_NOP, BLE_FIN, BLE_PONER, BLE_ESPERAR, BLE_FUNDIR, BLE_SALTAR, BLE_RGB_FUNDIR, BLE_RGB_PONER, BLE_RGB_APAGAR,
       BLE_SI_ABIERTO = 0x0D, BLE_FIN_SI, BLE_LED_PARPADEAR, BLE_LED_APAGAR };
/* cómo acabó (Ble.fin): CORRE = sigue; ACABADO = FIN, se salió por el final o una espera <= 0 (como el original);
 * los E_* son programas malos: salto fuera, orden desconocida, orden cortada por el final del fichero, y
 * E_SIN_ESPERA = BLE_MAX_SEGUIDAS órdenes sin una sola espera (el original se quedaría colgado; aquí se para). */
enum { BLE_CORRE, BLE_ACABADO, BLE_E_SALTO, BLE_E_ORDEN, BLE_E_CORTADA, BLE_E_SIN_ESPERA };
#define BLE_VERSION_MAX  5          /* la palabra 0 del fichero: el original exige < 6; los 30 de fábrica llevan 5 */
#define BLE_MAX_PALABRAS 512        /* el mayor de fábrica (alarm.ble) tiene 274 */
#define BLE_MAX_SEGUIDAS 256        /* órdenes seguidas sin esperar (la fábrica no pasa de 13) */
#define BLE_ESPERA_MIN   20         /* el reproductor no espera menos que esto entre tandas (la fábrica no baja de 25 ms) */

/* A dónde van las órdenes: el motor de verdad (lu_ops) o, en las pruebas, un registrador. Los % llegan YA con la
 * intensidad aplicada y recortados como hace el original; «traza», si no es NULL, recibe cada orden que se EJECUTA
 * tal como está en el fichero (palabra donde empieza, código, argumentos crudos). */
typedef struct {
    void (*poner)(void *ctx, u32 led, u32 pct);                              /* pct = 0: apagar */
    void (*fundir)(void *ctx, u32 led, u32 pct, u32 ritmo);                  /* ritmo = 100000 / ms */
    void (*rgb_fundir)(void *ctx, u32 terna, u32 r, u32 g, u32 b, u32 vel, u32 bajar);
    void (*rgb_poner)(void *ctx, u32 terna, u32 r, u32 g, u32 b, u32 modo);
    void (*rgb_apagar)(void *ctx, u32 terna);
    void (*led_pmu)(void *ctx, u32 led, u32 a, u32 b, int apagar);           /* órdenes 0F / 10: los LED del PMU (ningún efecto de fábrica las usa) */
    int  (*cerrado)(void *ctx);                                              /* 1 = deslizador cerrado */
    void (*traza)(void *ctx, u32 palabra, u32 op, const u32 *args, u32 n);
} LuOps;
typedef struct {
    const u32 *p; u32 n;            /* el programa, SIN la palabra de versión (los saltos cuentan desde aquí) */
    u32 pc;
    u8  fin;                        /* BLE_* */
    u8  en_bucle;                   /* el único contador de SALTAR del original */
    int cuenta;
    u8  intensidad;                 /* 0..100: escala PONER y FUNDIR */
    u8  con_rgb;                    /* 0 = las órdenes RGB no se ejecutan (la «preferencia» del original) */
    u8  con_base, base[3];          /* 1 = las órdenes RGB usan este color en vez del suyo (el «RGB base») */
    const LuOps *ops; void *ctx;
} Ble;
void ble_preparar(Ble *b, const u32 *palabras, u32 n, u32 intensidad, const LuOps *ops, void *ctx);
void ble_color_base(Ble *b, u32 r, u32 g, u32 bl);        /* 0, 0, 0 = el color del propio efecto */
int  ble_correr(Ble *b);                                   /* ejecuta hasta la primera espera: devuelve sus ms (> 0), o 0 = se acabó (b->fin dice cómo) */
u32  ble_velocidad(int ms);                                /* ms de un RGB_FUNDIR -> código de velocidad del PWM del chip (la tabla del original) */
u32  ble_rampa_ms(u32 vel);                                /* [inferido] lo que dura la rampa de ese código */
const char *ble_nombre(u32 op, u32 *n_args);               /* nombre y nº de argumentos de una orden (NULL = desconocida) */

/* ---- 2. el motor ----------------------------------------------------------------------------------------- */
extern const LuOps lu_ops;                                 /* el intérprete sobre el motor de verdad */
int  lu_poner(u32 led, u32 pct);                           /* FUN_4529c744 / c810: fija, de golpe; 0 = apaga */
int  lu_fundir(u32 led, u32 pct, u32 ritmo);               /* FUN_4529ce78: hasta ese %, un paso cada 50 ms (el primero, ya) */
int  lu_rgb_fundir(u32 terna, u32 r, u32 g, u32 b, u32 vel, u32 bajar);
int  lu_rgb_poner(u32 terna, u32 r, u32 g, u32 b, u32 modo);
int  lu_rgb_apagar(u32 terna);
void lu_sincronizar(void);                                 /* el motor se entera de lo que hay escrito ahora (lo pudo cambiar la plataforma) */
u32  lu_paso(void);                                        /* una vez por vuelta del bucle: los pasos de 50 ms y la orden siguiente del efecto.
                                                              Devuelve los ms hasta que vuelva a hacer falta (0 = nada en marcha) */
/* El PWM del chip, modelado ([inferido]: docs/luces.md §2.1): valor 0..255 a los «ms» de arrancar una rampa. Lo usan
 * las dos plataformas para decir cómo «se ve» una terna en hw_luz_leer(). */
u32  lu_pwm_modelo(u32 vel, int sentido, u32 ms);          /* sentido: 0 = fijo en FF, 1 = subiendo desde 0, 2 = bajando desde FF */

/* ---- 3. el reproductor de efectos -------------------------------------------------------------------------- */
enum { LU_EF_PROPIO = 0, LU_EF_BASE = 1 };                 /* de quién es el color de las órdenes RGB: del efecto, o el color base (efectos «de sistema») */
int  lu_efecto_memoria(const u32 *palabras, u32 n, u32 intensidad, int color);   /* «palabras» tiene que seguir viva mientras dure */
int  lu_efecto_fichero(const char *ruta, u32 intensidad, int color);             /* HW_OK; HW_NO_DISPONIBLE (no está); HW_ERROR (versión o tamaño) */
void lu_efecto_parar(void);                                /* lo para, apaga las dos ternas (como el original) y devuelve las luces a la plataforma */
int  lu_efecto_activo(void);
int  lu_efecto_fin(void);                                  /* cómo acabó el último (BLE_*) */
void lu_color_base(u32 r, u32 g, u32 b);                   /* en %; el de los efectos LU_EF_BASE */
void lu_rgb_permitir(int si);                              /* 0 = los efectos no tocan las ternas (Ajustes: luces laterales apagadas) */
typedef struct { u32 efectos, ordenes, esperas, pasos_fundido; } LuCuentas;
extern LuCuentas lu_cta;
#endif
