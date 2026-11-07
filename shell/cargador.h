/* cargador.h — lo interno del cargador de apps (shell/cargador.c) que usa la tabla (shell/cg_tabla.c).
 * Lo PÚBLICO para el resto del sistema son las cuatro funciones del final. Documento: docs/apps_cargables.md. */
#ifndef CARGADOR_H
#define CARGADOR_H
#include "shell.h"
#include "cargable.h"
#include "cargable_bt.h"                 /* (1.4, pista BT) el grupo Bluetooth, DETRÁS de la tabla 1.3 (api/cargable_bt.h) */
#include "cargable_15.h"                 /* (1.5) sonido y vibrador, DETRÁS del Bluetooth (api/cargable_15.h) */
#include "cargable_red.h"                /* (1.6) la red por el puente Bluetooth, DETRÁS del sonido (api/cargable_red.h) */
#include "cargable_17.h"                 /* (1.7) la pantalla: la letra nueva y su tamaño, DETRÁS de la red (api/cargable_17.h) */
#include "cargable_18.h"                /* (1.8) la música por la red y los píxeles: cg18.c */
#include "cargable_19.h"                /* (1.9) la cámara: cg19.c */
#include "cargable_110.h"               /* (1.10) el micrófono: cg110.c */
#include "cargable_111.h"               /* (1.11) las descargas a la tarjeta: cg111.c */

/* Una retrollamada de la app, internada: el widget del sistema guarda un puntero a ESTO (que vive en el sistema y
 * no se mueve), nunca la función de la app. `gen` la invalida cuando la app se descarga. */
typedef struct { void *fn[3]; void *dato; u32 gen; } CgRet;
#define CG_RETS 128
#define CG_RETROS 8

/* LO QUE ES DE ESTE MODELO y la app pregunta con ap_consulta(): un porte a otro teléfono lo define en su api/hw.h. */
#ifndef HW_CAPS
#define HW_CAPS (W5_CAP_ACELEROMETRO | W5_CAP_VIBRADOR | W5_CAP_ZUMBADOR | W5_CAP_LUCES | W5_CAP_TAPA | W5_CAP_FICHEROS | W5_CAP_RELOJ | \
                 W5_CAP_COLOR | W5_CAP_TECLADO_NUM | W5_CAP_JOYSTICK)
#endif
/* (1.1, M5X) lo que añade el CARGADOR a las capacidades del modelo: la ventana de recursos, solo donde hay MMU (el teléfono) */
/* (1.4, pista BT) + W5_CAP_BT solo donde hay controlador (el W580): por eso CG_CAPS es una EXPRESIÓN, no una constante */
unsigned cg_bt_caps(void);                       /* shell/cg_bt.c */
unsigned cg_red_caps(void);                      /* (1.6) shell/cg_red.c */
unsigned cg_cam_caps(void);                      /* (1.9) shell/cg19.c */
unsigned cg_mic_caps(void);                      /* (1.10) shell/cg110.c */
#if defined(TELEFONO) && TELEFONO
#define CG_CAPS (HW_CAPS | W5_CAP_REC_VENTANA | CG12_CAPS | cg_bt_caps() | cg_red_caps() | W5_CAP_GIRA | cg_cam_caps() | cg_mic_caps())   /* (1.7) W5_CAP_GIRA; (1.9) W5_CAP_CAMARA; (1.10) W5_CAP_MICROFONO */
#else
#define CG_CAPS (HW_CAPS | CG12_CAPS | cg_bt_caps() | cg_red_caps() | W5_CAP_GIRA | cg_cam_caps() | cg_mic_caps())
#endif
#ifndef HW_TECLAS_A
#define HW_TECLAS_A 0xFFFFFFFEu          /* existen todas las W5K_* de 1 a 31 */
#define HW_TECLAS_B 0u
#endif
#ifndef HW_MODELO
#define HW_MODELO 580
#endif
#ifndef HW_ACEL_1G
#define HW_ACEL_1G 0                     /* [sin saber] hasta medirlo en el aparato */
#endif

enum { CG_LIBRE, CG_VIVA, CG_MUERTA };
int    cg_acel(int si);
int    cg_estado(void);
const char *cg_ruta_viva(void);                   /* (LANZ) la ruta de la .sen viva; NULL si no hay */
int    cg_perfil(void);
int    cg_api_menor(void);
void   cg12_a_soltar(void); void cg12_b_soltar(void); void cg12_c_soltar(void); void cg12_d_soltar(void);   /* (1.2) shell/cg12_*.c */
void   cg13_soltar(void);                           /* (MD 1.3) suelta el diseño que hubiera pedido la app */
void   cg12_d_al_abrir(void);
void   cg12_d_tras_descargar(void);   /* (APX) abre la app que esperaba a que se fuera la anterior */
void   cg_salir(void);
Widget *cg_abrir_con(const char *ruta, const char *argumento);   /* (1.2) shell/cg12_d.c */
u32    cg_caps_app(void);                    /* (APX) las W5_CAP_* que DECLARÓ la app cargada (0 si no hay) */
/* (APX) un servicio de la 1.2 solo para quien lo declaró en su cabecera: si no, es un error de la app (se la cierra diciéndolo) */
#define CG_PIDE(cap, ...) do { if (!(cg_caps_app() & (cap))) { cg_matar("La app usa un servicio que no declaró en su cabecera."); return __VA_ARGS__; } } while (0)
int    cg_de_fabrica(const char *ruta, char *nombre, u32 max, int *icono);   /* (APX) shell/apps.c */
int    sh_ruta_de_fabrica(const char *ruta);                                 /* (APX) y (EST3) su copia nueva en /memoria/apps */
int    sh_ruta_retirada(const char *ruta);                                   /* (EST3) ya no es del sistema: no sale en ninguna lista */
int    cg_copia_de_fabrica(const char *dir, const char *nombre);             /* (Fase 3b) esa .sen de `dir` es una copia vieja de una de fábrica: no sale */
const char *cg_ruta_nueva(const char *ruta);                                 /* (EST3) /apps/x.sen -> /memoria/apps/x.sen si esa está y vale */
#define CG12_CAPS (CG12_CAPS_0 | CG12_CAPS_A | CG12_CAPS_B | CG12_CAPS_C | CG12_CAPS_D)   /* (1.2) los servicios que ofrece el sistema (api/cargable12_*.h) */
int    cg_api_menor(void);               /* (1.1) la menor que declaró la app cargada */
int    cg_app_sabe_girar(void);
void   cg_avisar_giro(int giro);         /* (1.7) el suceso W5_GIRO a la app cargada (si declara W5_CAP_GIRA) */          /* (1.7) 1 = no hay app cargada, o la que hay declara W5_CAP_GIRA (la pantalla puede girar) */
void   cg_matar(const char *motivo);                 /* la app ha hecho algo que no puede: no se la vuelve a llamar */
W5Pal  cg_llamar(void *fn, W5Pal a, W5Pal b, W5Pal c, W5Pal d);   /* EL ÚNICO SITIO por donde se entra al código de la app */
CgRet *cg_ret(void *f0, void *f1, void *f2, void *dato);          /* nunca NULL (si no vale, mata la app y da una vacía) */
CgRet *cg_ret_de(void *dato);                        /* el registro si `dato` es uno vigente; si no, NULL */
int    cg_en_app(const void *p, size_t n);           /* [p, p+n) cae entero en la memoria de la app */
int    cg_legible(const void *p);                    /* memoria de la app, arena del cuadro o (sin PM) imagen del sistema */
int    cg_legible_n(const void *p, size_t n);        /* (AUD9) el rango [p,p+n) entero legible */
int    cg_copiar_cad(char *d, size_t cap, const char *s);   /* (INT2) una cadena de la app a un búfer del SISTEMA, byte a byte legible;
                                                              * 1 si cabe entera con su 0 (cap incluye el 0), 0 si no vale o no cabe */
int    cg_li_vale(W5Lienzo l);                       /* (INT2) shell/cg_tabla.c: el lienzo de la retrollamada de pintar en curso (con PM) */
int    cg_sis_leible(const void *p, size_t n);       /* (AUD9) [p,p+n) en la arena o en la RAM/flash del sistema (no de la app) */
int    cg_cadena(const char *s);                     /* una cadena que el sistema puede guardar (NULL vale) */
Retro  cg_retro(W5Retro f, int crear);               /* el trampolín del sistema para ese temporizador de la app */
void  *cg_monton_pedir(u32 n);
void   cg_monton_soltar(void *p);
u32    cg_monton_libre(void);
u32    cg_pila_libre(void);
int    cg_primer_plano(void);
void   cg_latido(void);
const char *cg_nombre_fich(const char *nombre);      /* con el prefijo de la app; NULL si el nombre no vale */
const char *cg_fich_tarjeta(const char *nombre);     /* (28-oct) la ruta en la tarjeta de un fichero propio de una app que vino de ella; si no, NULL */
int    cg_escritura_permitida(void);                 /* el tope de escrituras por minuto */
u32    cg_rec_tam(void);
int    cg_rec_leer(u32 desde, void *buf, u32 max);
const void *cg_rec_mapa(u32 desde, u32 n);
int    cg_rec_comprobar(void);
Widget *cg_widget(void *w);                          /* un widget que la app devuelve: el mismo si vale; si no, NULL */
Widget *cg_pantalla(void *construir, void *iniciar, void *desechar, void *dato);
int    cg_ctx(const void *c);                        /* un contexto que la app devuelve: 1 si parece uno */
Ctx   *cg_mi_pantalla(Ctx *c);
void   cg_salir(void);
/* (1.4, pista BT) LA TABLA QUE DA EL SISTEMA: la 1.3 (W5Api, sin tocar) y DETRÁS el grupo Bluetooth (W5_API14), en la casilla
 * W5_BT_INDICE = n de la 1.3, como espera bt/sdk/w5_bt.h. El sistema dice menor CG_API_MENOR = 4; una app compilada con el SDK de
 * siempre declara W5_API_MENOR = 3 (cargable.h no cambia) y carga igual. cg_api sigue siendo la W5Api de siempre (la cabecera). */
#define CG_API_MENOR W5_DESC_MENOR         /* (1.5) detrás del Bluetooth, el sonido (api/cargable_15.h); (1.6) detrás, la red (api/cargable_red.h): menor 6;
                                            * (1.7) detrás, la pantalla (api/cargable_17.h): la letra nueva, menor 7;
                                            * (1.8) detrás, la música por la red y los píxeles (api/cargable_18.h): menor 8;
                                            * (1.9) detrás, la cámara (api/cargable_19.h): menor 9;
                                            * (1.10) detrás, el micrófono (api/cargable_110.h): menor 10;
                                            * (1.11) detrás, las descargas a la tarjeta (api/cargable_111.h): menor 11 */
typedef struct CgApi {
    W5Api base;
#define W5_F14(r, nombre, args) r (*nombre) args;
    W5_API14(W5_F14)
    W5_API15(W5_F14)
    W5_API16(W5_F14)
    W5_API17(W5_F14)
    W5_API18(W5_F14)
    W5_API19(W5_F14)
    W5_API110(W5_F14)
    W5_API111(W5_F14)
#undef W5_F14
} CgApi;
extern const CgApi cg_api_todo;
#define cg_api (cg_api_todo.base)
/* (PM) aislamiento de memoria: shell/cg_proteccion.c. Solo en el teléfono con la MMU puesta; en el PC no hace nada. */
typedef struct { u32 montajes, llamadas, ventanas; } CgPmMedidas;
extern CgPmMedidas cg_pm_med;
int    cg_pm_posible(void);
int    cg_pm_activa(void);
const W5Api *cg_pm_api(void);                        /* la tabla que ve la app (la copia de la puerta, si hay protección) */
int    cg_pm_abrir(const void *p, u32 n, int escribir);   /* abre a la app esas páginas (ALINEADAS a 4 KiB); 0 si no vale */
void   cg_pm_cerrar(const void *p, u32 n);           /* las vuelve a cerrar (solo sistema) */
void   cg_pm_rec_fichero(const u8 *fis, u32 tam);    /* el fichero mapeado de la app en la NOR, para ap_rec_mapa */
const void *cg_pm_rec_mapa(u32 desde, u32 n);        /* abre esas páginas de la NOR en solo lectura; el puntero REAL (1:1) o NULL */
void   cg_pm_fallo(const char *motivo) __attribute__((noreturn));   /* la app hizo algo imposible en la puerta: fuera, YA */
void  *cg_arena_app(u32 n);
void   cg_arena_reset(void);                         /* (AUD9) vacía la arena del cuadro de la app (un fotograma del juego) */
void   cg_et_reset(void);                            /* las entradas de texto del depósito del sistema, libres (shell/cg_tabla.c) */                          /* memoria de un cuadro DENTRO de la app (ui_arena con protección) */
#if TELEFONO
u32    cg_pm_extra(u32 tam_rec);
const W5Api *cg_pm_montar(u8 *base, const u8 *app_ini, const u8 *app_fin, const u8 *pi, const u8 *pf, u32 tam_rec, volatile u32 *vec);
void   cg_pm_desmontar(volatile u32 *vec);
u32    cg_pm_usp(void);
void   cg_pm_reiniciar(void);
W5Pal  cg_usr(W5Pal *args, u32 usp);
#endif
extern const Clase ClsWCgPant;
typedef struct { Widget base; CgRet *r; } WCgPant;      /* la pantalla anfitriona: una clase del SISTEMA */
int    ui_en_arena(const void *p);
/* ui_arena_libre(): ui/ui.h */
u32    cg_crc(u32 crc, const void *d, u32 n);

/* Validación pura de la cabecera (la usa el cargador y la prueba de fuzz). 0 = vale; si no, `motivo`. */
int    cg_cab_vale(const W5xCab *c, u32 tam_real, char *motivo, u32 nm);
/* (ICO) la malla 3D de la sección de icono de una .sen (shell/cg_icono.c). AppMalla: api/app_externa.h */
struct AppMalla;
int    cg_ico_validar(const u8 *sec, u32 tam, struct AppMalla *m);                 /* 0 = vale */
int    cg_ico_leer(const char *ruta, const W5xCab *c, u8 *buf, struct AppMalla *m);   /* 1 trae y vale, 0 no trae, -1 no vale */
/* Carga desde un lector cualquiera (el fichero se lee SIEMPRE a trozos con desplazamiento). */
typedef u32 (*CgLeer)(void *quien, u32 desde, void *buf, u32 max);
int    cg_cargar_de(CgLeer leer, void *quien, u32 tam, const char *ruta, char *motivo, u32 nm);
void   cg_descargar(void);
typedef struct { u32 cargas, descargas, llamadas, muertes, t_carga_ms, tam_bloque, pila_usada, rets; } CgMedidas;
extern CgMedidas cg_med;

/* ---- lo público para el sistema ------------------------------------------------------------------------------ */
int     cg_al_menu(App *v, int n, int max);          /* shell/apps.c: añade las apps descubiertas; devuelve el nuevo n */
/* (LANZ) el lanzador (shell/apps.c, Ajustes -> «Menú principal») */
#define CG_MAX 32                                    /* (APX) las .sen que se descubren como mucho (eran 12) */
#define CG_CELDAS 13                                 /* (LANZ) celdas del menú que puede ocupar una .sen: SH_CELDAS menos Ajustes y «Apps» */
void    cg_menu_empezar(void);
int     cg_icono_fabrica(const char *ruta, Icono *arte, u16 *bits, int con_malla);   /* (28-oct) shell/apps.c: el icono propio de una de fábrica */
Widget *cg_abrir_juegos(void);                       /* (28-oct) la celda «Juegos»: los juegos de la tarjeta */
const Icono *cg_juegos_arte(void);                   /*          su arte (un mando) y su malla 3D (índice + 1; 0 = la de IC_JUEGO) */
int     cg_juegos_malla(void);
int     cg_juegos_descubrir(int *mallas, int max);   /* (28-oct, Medios) los de la tarjeta, como la pantalla «Juegos»; cg_app_info(i) da cada uno */
int     cg_menu_sen(App *v, int k, const char *ruta);   /* la .sen de esa ruta a la celda k (0..CG_CELDAS-1): 0 bien, -1 no está o no vale */
void    cg_menu_terminar(void);
App     cg_celda_apps(void);                         /* la celda «Apps» (la lista de todo) */
typedef struct { const char *ruta, *nombre; const Icono *icono; int juego; } CgInfo;
int     cg_descubrir(void);                          /* las .sen que no son de fábrica, en el orden de §28: cuántas */
int     cg_app_info(int i, CgInfo *o);               /* la i de las descubiertas: 0 bien (los punteros valen hasta el siguiente descubrir) */
/* cg_abrir_ruta(): declarada en shell/shell.h (la usa Archivos) */
void    cg_fallo_cpu(const u32 *ctx);                /* plat: excepción de la CPU. Si la culpa es de la app, NO VUELVE (sigue el sistema) */
int     cg_perro_irq(void);                          /* plat, desde la IRQ: 1 = el cuelgue es de la app y se la ha sacado */
#if !defined(TELEFONO) || !TELEFONO
void    cg_prueba(const char *orden);                /* plat/pc_main.c, orden «cargable …» de un guion */
#endif
#endif
