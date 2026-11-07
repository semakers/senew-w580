/* app_externa.h — el contrato de una APP EXTERNA del sistema del W600 (pista S3).
 *
 * Una app externa es una aplicación cuyo código y cuyos datos viven FUERA de `sistema/` (p. ej. un juego
 * grande portado), que aparece en el menú como una más y que, mientras corre, se queda con la
 * pantalla, con el teclado y con el bucle. El sistema compila exactamente igual sin ninguna.
 *
 * Esto es lo ÚNICO que una app externa puede incluir del sistema, además de `hw.h` (la API de
 * hardware) y `ui.h` (solo por los códigos de tecla K_*). Nada de `shell.h`: una app a pantalla
 * completa no usa widgets.
 *
 * El contrato entero, con ejemplos y con el presupuesto de memoria y de flash, está en
 * `docs/sistema.md` §11. Resumen:
 *   1. se da de alta con APP_EXTERNA(...) y una línea `APPS_EXT=<dir>` al compilar;
 *   2. toma la pantalla (app_pantalla_tomar), dibuja, vuelca, y la SUELTA siempre al salir;
 *   3. la memoria grande se PIDE PRESTADA (app_mem_*) y se devuelve al salir: una app externa NO
 *      puede declarar arreglos estáticos grandes, porque se quedarían en el .bss del sistema;
 *   4. las teclas, en crudo (hw_teclas_mapa / hw_tecla_abajo);
 *   5. el bucle es suyo, a unos 30 Hz sobre el tic de 4 ms, saltando DIBUJOS (nunca lógica) si va tarde;
 *   6. la partida se guarda con hw_usr_* en la zona de usuario.
 */
#ifndef APP_EXTERNA_H
#define APP_EXTERNA_H
#include "hw.h"

/* ---- 1. el descriptor ---------------------------------------------------------------------------
 * Va a la sección .apps_ext; el enlazador las junta y el shell las recorre al arrancar. */

/* La malla del icono para el menú 3D (pista M2; docs/menu3d.md §1.2 — es el mismo formato que las de casa,
 * shell/mallas3d.h). La app la trae en SUS datos: el sistema solo guarda el puntero.
 *   pos   s8 x, y, z por posición (1.0 = 100; x a la derecha, y arriba, z hacia quien mira); como mucho 255
 *   nor   s8 x, y, z por normal (x127); como mucho 255. Posiciones y normales van aparte y sin repetir
 *   tri   6 bytes por triángulo: tres índices de posición y tres de normal, en sentido antihorario visto de
 *         frente; AGRUPADOS por papel, en este orden: cuerpo, secundario, claro, oscuro, acento
 *   nt    cuántos triángulos de cada papel. El color de cada papel lo pone el ESTILO del menú
 *   natural  RGB propios para cuerpo, secundario y acento (el estilo «Cristal» da a cada icono su color)
 * El menú no gira un icono más de unos 50°: no hacen falta caras traseras. Las de casa llevan 30–242 triángulos. */
typedef struct AppMalla {
    const signed char *pos, *nor;
    const u8 *tri;
    u8  npos, nnor;
    u16 nt[5];
    u8  natural[3][3];
} AppMalla;

typedef struct AppExterna {
    const char *nombre;                /* el que sale en el menú */
    int   icono;                       /* uno de los APP_IC_* de abajo */
    void (*entrar)(void);              /* no vuelve hasta que el usuario sale de la app */
    const char *const *arte;           /* opcional: 12 cadenas de la MISMA longitud (1..16 caracteres; «#» = tinta)
                                          con su propio icono de 1 bit para el menú «Clásico»; NULL = el de `icono` */
    const AppMalla *malla;             /* opcional (pista M2): su icono para el menú 3D; NULL = la malla de `icono`.
                                          Caben APP_MALLAS_MAX apps con malla propia; a las demás, la de `icono` */
} AppExterna;
#define APP_MALLAS_MAX 2

/* Los iconos del sistema que una app externa puede pedir (shell/iconos.c). */
enum { APP_IC_JUEGO = 5, APP_IC_IMAGENES = 1, APP_IC_MUSICA = 11, APP_IC_CONSOLA = 6, APP_IC_SISTEMA = 8 };

#define APP_EXTERNA(id, nom, ic, fn, ...) \
    __attribute__((used, section("apps_ext"))) const AppExterna app_ext_##id = \
        { .nombre = (nom), .icono = (ic), .entrar = (fn), __VA_ARGS__ }

/* ---- 2. la pantalla completa --------------------------------------------------------------------
 * Píxeles RGB565 con el byte alto primero (la macro RGB() de ui.h los compone así).
 * m->pix[y * m->paso + x] es el píxel (x, y) del área de la app. */
typedef struct { u16 *pix; int an, al, paso; int modo; } AppMarco;
enum { APP_BANDAS,                     /* W580: 240x180 centrado, bandas negras arriba y abajo: SIN copia */
       APP_GIRADA };                   /* W580: 320x240, el teléfono de lado: el sistema gira al volcar */

int  app_pantalla_tomar(AppMarco *m, int modo);   /* HW_OK; HW_ERROR si no hay sitio para el girado. CALLA el zumbador
                                                     (el clic de la tecla que abrió la app no se queda sonando) */
void app_pantalla_volcar_entera(void);            /* (1.1, M5X) el panel ENTERO (W5_JG_PANEL de una app cargable) */
void app_pantalla_volcar(void);                   /* el marco al panel, por DMA; de paso llama a app_latido() */
void app_pantalla_soltar(void);                   /* devuelve pantalla, teclas, luz y temporizadores al shell, y si
                                                     saltó algún aviso mientras tanto, lo enseña */

/* ---- 2b. lo que el sistema sigue haciendo mientras manda la app (pista S6) ----------------------
 * La app es la dueña del bucle, así que el reloj del standby no late: sin esto una alarma que saltara
 * jugando no hacía nada hasta salir. app_latido() es el latido que el sistema necesita:
 *   - mira (una vez por segundo) la ALARMA, la CUENTA ATRÁS y la BATERÍA baja;
 *   - si salta algo, SUENA por el zumbador por encima de la app (alarma 3 vueltas ≈ 7 s, cuenta atrás 2,
 *     batería 2) y lo deja PENDIENTE: ni diálogo, ni se toca el marco, ni se roba una tecla;
 *   - hace avanzar esa melodía.
 * app_pantalla_volcar() ya lo llama: una app que vuelca cada cuadro no tiene que hacer nada. La que pase
 * ratos sin volcar (una pausa, una carga larga) debe llamarlo ella, al menos unas 10 veces por segundo.
 * app_avisos() dice qué hay pendiente, por si la app quiere pintar su propia señal; el diálogo del
 * sistema («Mientras jugabas…») sale solo al llamar a app_pantalla_soltar(). */
enum { APP_AV_ALARMA = 1, APP_AV_TEMPORIZADOR = 2, APP_AV_BATERIA = 4 };
void app_latido(void);
int  app_avisos(void);                            /* bits APP_AV_* */

/* ---- 2c. una MELODÍA larga de la app por el generador de tonos (pista M2) -----------------------
 * El zumbador es UNA voz y es del sistema (por ahí van la alarma, la cuenta atrás y la batería). Una app
 * externa no lo toca por su cuenta: le da al sistema su lista de notas y el sistema la toca
 *   - desde el TIC de 4 ms (hw_tic_gancho), no desde el bucle de la app: el ritmo no baila con lo que tarde
 *     un cuadro. Cada nota empieza donde acababa la anterior (por hw_ms(), sin deriva); el grano es el tic.
 *     Donde no hay tic (el PC) avanza en app_latido();
 *   - con el VOLUMEN del sistema (Ajustes → Sonido; en silencio, calla);
 *   - CEDIENDO el zumbador a cualquier aviso del sistema mientras suena (la melodía sigue contando el tiempo
 *     por debajo y reaparece en el punto que le toque, no donde se quedó);
 *   - y la CALLA sola en app_pantalla_soltar().
 * notas[i].hz = 0 es un silencio; por debajo de 397 Hz o por encima de 20 kHz el generador no llega: silencio.
 * `repetir_desde` >= 0: al acabar vuelve a esa nota (0 = toda la lista en bucle; así cabe una entrada que
 * suena una vez); < 0: suena una vez y calla. La lista tiene que seguir viva mientras suene (no se copia).
 * app_melodia(NULL, 0, 0) la calla. Devuelve HW_OK, o HW_ERROR si la app no tiene la pantalla o la lista no vale. */
typedef struct { u16 hz, ms; } AppNota;
int  app_melodia(const AppNota *notas, u32 cuantas, int repetir_desde);
int  app_melodia_suena(void);                     /* 1 = hay una melodía de la app en marcha (aunque ahora mande un aviso) */

/* ---- 3. la memoria prestada ---------------------------------------------------------------------
 * Todo lo que hay entre el final de los estáticos del sistema y el final de la RAM (~15 MiB hoy).
 * Se reparte por avance y se devuelve ENTERA de golpe: por eso el sistema la recupera al salir. */
void *app_mem_tomar(u32 *bytes);       /* toma el bloque; *bytes = cuántos hay. NULL si ya está tomado */
void *app_mem_reservar(u32 n);         /* n bytes alineados a 8 y A CERO; NULL si no caben */
u32   app_mem_libre(void);
void  app_mem_soltar(void);            /* lo devuelve todo (lo hace también app_pantalla_soltar) */

/* ---- 3b. los ESTÁTICOS de la app, como en un arranque en frío (pista S6) ------------------------
 * Los estáticos de una app externa viven en el .data y el .bss del sistema y NO vuelven solos a su valor
 * inicial entre una entrada y la siguiente (el sistema no se reinicia al salir de una app). Un programa
 * que no sabe «cerrarse» (un juego antiguo portado: cientos de globales) necesita que se los repongan.
 * La forma oficial, sin tocar los símbolos del script de enlace:
 *   1. el PRIMER fuente de su APPEXT_SRC contiene solo   APP_ESTATICOS_INI(miapp);
 *      y el ÚLTIMO solo                                  APP_ESTATICOS_FIN(miapp);
 *      (el enlazador coloca los datos en el orden de los objetos: todo lo de la app queda entre los dos);
 *   2. lo PRIMERO que hace su `entrar`, antes de tocar un solo estático:   APP_ESTATICOS_REINICIAR(miapp);
 * Devuelve HW_OK, o HW_ERROR si los marcadores no caen donde deben (y entonces NO toca nada).
 * En el teléfono vuelve a copiar ese trozo de .data desde la flash y pone a cero ese trozo de .bss; en el
 * PC (no hay imagen en flash) guarda una foto la primera vez y la repone las siguientes.
 * OJO: lo que la app haya puesto en esos estáticos en la entrada anterior se pierde — es justo el encargo. */
#define APP_ESTATICOS_INI(id) char id##_est_data_ini __attribute__((aligned(8))) = 1; \
                              char id##_est_bss_ini __attribute__((aligned(8)))
#define APP_ESTATICOS_FIN(id) char id##_est_data_fin __attribute__((aligned(8))) = 1; \
                              char id##_est_bss_fin __attribute__((aligned(8)))
#define APP_ESTATICOS_REINICIAR(id) __extension__ ({ \
        extern char id##_est_data_ini, id##_est_data_fin, id##_est_bss_ini, id##_est_bss_fin; \
        app_estaticos_reiniciar(&id##_est_data_ini, &id##_est_data_fin, &id##_est_bss_ini, &id##_est_bss_fin); })
int app_estaticos_reiniciar(void *data_ini, void *data_fin, void *bss_ini, void *bss_fin);

/* ---- 6. guardar la partida: la zona de ficheros de usuario (api/usuario.h) ----------------------
 * Atómicas por fichero: tras un corte de corriente el fichero vale ENTERO lo de antes o ENTERO lo
 * nuevo. Nombres de 1..47 bytes UTF-8, sin «/» ni NUL; no hay directorios.
 * Un juego guarda su partida, p. ej., en «partida.sav». */
int hw_usr_tam(const char *nombre);                                 /* bytes, o HW_NO_DISPONIBLE */
int hw_usr_leer(const char *nombre, u32 desde, void *buf, u32 max); /* bytes leídos, o HW_NO_DISPONIBLE */
int hw_usr_escribir(const char *nombre, const void *d, u32 n);      /* reemplaza el fichero entero */
int hw_usr_borrar(const char *nombre);
#endif
