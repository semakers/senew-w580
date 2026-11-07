/* shell.h — lo que el shell (standby, menú, barra de estado, ajustes) ofrece a las aplicaciones.
 * Una app incluye solo esto: trae la API de hardware (hw.h) y el framework (ui.h). */
#ifndef SHELL_H
#define SHELL_H
#include "hw.h"
#include "ui.h"
#include "diseno.h"
#include "podometro.h"

#define SISTEMA_VERSION HW_VERSION_SISTEMA    /* se define en api/hw.h: un solo sitio */

/* ---- ajustes del sistema: en RAM y, cada vez que cambian, al almacén persistente (sh_guardar_ajustes) ---- */
#define N_TEMAS 5                        /* (MD) diseño + paleta: shell.c */
#ifndef TEMA_FABRICA
#define TEMA_FABRICA 0                   /* (MD) material3 oscuro. -DTEMA_FABRICA=2: el clásico de siempre (para cotejar con las capturas de antes) */
#endif
typedef struct {
    u8 tema;                         /* 0..N_TEMAS-1 */
    u8 hora12;                       /* 1 = formato de 12 horas */
    u8 alarma_on, alarma_h, alarma_m;
    i16 zona_min;                    /* desfase de la hora local respecto al RTC, en minutos (el RTC va en UTC) */
    u8 brillo;                       /* retroiluminación de la pantalla, 0..100 */
    /* energía y bloqueo (pista S3) */
    u8 atenuar_s;                    /* segundos de inactividad hasta ATENUAR la luz (0 = nunca) */
    u8 apagar_s;                     /* segundos hasta APAGAR la pantalla y poder dormir a fondo (0 = nunca) */
    u8 bloqueo_auto;                 /* 1 = al apagarse la pantalla, se bloquea el teclado */
    u8 tapa_apaga;                   /* 1 = cerrar la tapa apaga la pantalla; abrirla la enciende */
    /* sonido (pista S5) */
    u8 volumen;                      /* 0 = SILENCIO, 1..5 (es el ancho del pulso del zumbador: shell/sonido.c) */
    u8 tono_teclas;                  /* 1 = un clic al pulsar una tecla */
    /* podómetro (pista S2) */
    u8 pasos;                        /* 1 = el podómetro cuenta en segundo plano (y el standby enseña el contador) */
    u8 pasos_ahorro;                 /* SH_PASOS_*: qué hace en reposo */
    /* el menú 3D (pista V; docs/menu3d.md) */
    u8 menu_estilo;                  /* M3_CLASICO (el menú de antes), M3_WALKMAN, M3_CRISTAL, M3_NEON (shell/m3d.h) */
    u8 menu_efecto;                  /* SH_EFECTO_*: inclinación por el acelerómetro / vaivén propio / quieto */
    u8 menu_intensidad;              /* 1..5: la ganancia de cuentas del sensor a grados */
    u8 menu_ejes;                    /* 0..7: bit 0 invierte X, bit 1 invierte Y, bit 2 cruza los ejes */
    /* las luces (pista S5; shell/luces.c) */
    u8 luces;                        /* 1 = las luces laterales se encienden con la pantalla; 0 = apagadas siempre */
    u8 luces_color;                  /* 0 = automático (a juego con el estilo del menú); 1..SH_LUZ_COLORES-1 = uno fijo de la paleta */
    u8 luces_latido;                 /* 1 = latido al mover el foco o inclinar, respiración con la música, destello cada 10 pasos, efecto con los avisos */
    /* (LANZ) el lanzador: qué apps van en el menú principal (Ajustes -> «Menú principal»; shell/apps.c) */
    u8 menu_propio;                  /* 0 = la disposición de fábrica y el orden automático (docs/sistema.md §28); 1 = la que eligió el
                                      * usuario, guardada aparte en NV_MENU (por la ruta de cada .sen) */
    u8 giro;                         /* (estética, v8) 1 = la pantalla gira sola con el acelerómetro (de fábrica, lo pidió el mantenedor); 0 = siempre de pie */
    u8 medios_color;                 /* (28-oct, v9) SH_MEDIOS_*: el color del menú de Medios y de su fondo animado (0 = el rojo del XMB) */
    char fondo[100];                 /* ruta de la imagen de fondo del standby ("" = liso) */
} Ajustes;
extern Ajustes aj;
int  sh_ajustes_version(void);       /* (Fase 3b) la versión del registro de ajustes (AJ_VERSION) */
int  sh_guardar_ajustes(void);       /* al almacén (hw_guardar); si nada cambió no gasta flash. (R2) Devuelve HW_OK o por qué no (y lo dice UNA vez en pantalla) */
/* Claves del almacén persistente (hw_cargar / hw_guardar). Una app nueva toma un rango 0xNN00 libre.
 * EL REGISTRO DE CLAVES ES ESTE ENUM: quien tome una, de casa o EXTERNA, la apunta aquí para que nadie la
 * reutilice (el almacén no sabe de dueños: dos apps con la misma clave se pisan en silencio).
 *   0x0100..0x04FF  apps de casa
 *   0x0500..0x05FF  (pista BT) servicios del sistema: 0x0500 = NV_BT, el Bluetooth (claves de enlace, BD_ADDR propia, último robot: plat/bt_w580.c);
 *                   0x0501 = NV_RED, la dirección del puente de red: [1][addr 6], y [banderas] si lo puso SENUS (shell/red_sis.c);
 *                   0x0502 = NV_APARATO, el id de este aparato que dio SENUS con SEN_VINCULAR (shell/gestion_sis.c)
 *   0x6000..0x6FFF  apps EXTERNAS (docs/sistema.md §11.6). Tomadas:
 *     0x6400  reservada: la partida de un juego externo */
enum { NV_AJUSTES = 0x0100, NV_MENU = 0x0101 /* (LANZ) las apps del menú principal elegidas por el usuario: shell/apps.c */, NV_NOTAS_ORDEN = 0x0200, NV_NOTA = 0x0201 /* +hueco */, NV_SNAKE = 0x0300, NV_CALC = 0x0400,
       NV_BT = 0x0500 /* (pista BT) docs/sistema.md §30 */, NV_RED = 0x0501 /* (1.6) el puente de red elegido: shell/red_sis.c */, NV_APARATO = 0x0502 /* (Fase 3b) el id vinculado: shell/gestion_sis.c */, NV_EXT_BASE = 0x6000, NV_EXT_JUEGO = 0x6400 };
extern const char *const sh_tema_nombre[N_TEMAS];
const TemaDatos *sh_tema(void);                  /* la paleta en uso (cada Pantalla la hereda con Tema) */
void sh_poner_tema(int i);
void sh_poner_fondo(const char *ruta);           /* carga ese GIF como fondo del standby ("" lo quita; SH_FONDO_MEDIOS: el de Medios, animado) */
const Imagen *sh_fondo(void);
/* (28-oct) EL FONDO DE MEDIOS ANIMADO (pedido del mantenedor: «que las partículas se puedan poner como fondo de pantalla»; se pone solo al elegir el
 * menú de Medios) y SU COLOR (Ajustes -> «  color de Medios»: no siempre rojo). shell/menu_medios.c, shell/m3d_dibujo.c */
#define SH_FONDO_MEDIOS "@medios"
int  sh_fondo_medios(void);                      /* 1 = el fondo del standby es el de Medios, animado */
enum { SH_MEDIOS_ROJO, SH_MEDIOS_NARANJA, SH_MEDIOS_ORO, SH_MEDIOS_VERDE, SH_MEDIOS_TURQUESA, SH_MEDIOS_AZUL, SH_MEDIOS_MORADO, SH_MEDIOS_ROSA,
       SH_MEDIOS_PLATA, SH_MEDIOS_NEGRO, SH_MEDIOS_MES, SH_MEDIOS_COLORES };
extern const char *const sh_medios_color_nombre[SH_MEDIOS_COLORES];
int  sh_medios_tono(void);                       /* el color de AHORA (con «según el mes», el del mes como en la PS3): SH_MEDIOS_ROJO..NEGRO */
Widget *sh_medios_fondo(Widget *encima, u32 t);  /* el fondo de Medios con «encima» delante, en el instante t (ms); quien lo pone lleva el
                                                       temporizador y lo vuelve a construir en cada cuadro (shell/shell.c: el standby) */
extern u32 sh_temporizador_fin;                        /* hw_ms() en que vence la cuenta atrás (0 = parada); la vigila el standby */
/* El vigilante de la alarma y de la cuenta atrás. Lo llama el standby una vez por segundo (en_app = 0: aviso en
 * pantalla + melodía hasta que alguien pulse) y, mientras una app externa tiene la pantalla, app_latido()
 * (en_app = 1: SOLO suena, unas pocas vueltas, y el aviso queda PENDIENTE para cuando la app devuelva la pantalla). */
enum { SH_AV_ALARMA = 1, SH_AV_TEMPORIZADOR = 2, SH_AV_BATERIA = 4, SH_AV_CARGADOR = 8 };
void sh_vigilar_avisos(int en_app);
void sh_aviso_pendiente(int cual, int dato);           /* SH_AV_*; dato = los mV de la batería. Lo usa shell/energia.c */
int  sh_avisos_pendientes(void);                       /* bits SH_AV_* de lo que ha saltado dentro de la app y nadie ha visto */
void sh_avisos_mostrar(void);                          /* los enseña en UN diálogo y los da por vistos (app_pantalla_soltar) */
void sh_bateria_vigilar(int en_app);                   /* shell/energia.c: la batería en mV cada 10 s: aviso a < 3500 y APAGADO a < 3400 (S7) */
void sh_usb_paso(int en_app);                          /* (USBD) shell/usb.c: VBUS -> USB/MTP en marcha, «Conectado por USB», cable fuera -> parado; una vez por vuelta */
int  sh_usb_activo(void);                              /* (USBD) el USB está en marcha: nada de reposo profundo */
void sh_cargador_vigilar(int en_app);                  /* shell/energia.c: el aviso «cargador conectado: este sistema NO carga» (S7) */
void sh_encendido_largo(void);                         /* shell/energia.c: la tecla de encendido mantenida 8 s apaga con orden (S7) */
int  sh_bateria_sin_calibrar(void);                    /* (P600) la placa no da recta de batería: sin reglas de tensión, y se dice */
int  sh_bateria_dudosa(void);                          /* (R2) la medida en marcha no casa con la del arranque: avisado, apagado aplazado 10 min */
int  sh_pmu_mudo(void);                                /* (R2) el PMU no contesta (3 sondeos): sin vigilancia de batería */
int  sh_carga_sin_confirmar(void);                     /* (R2) 0x61/0x63 no se releen como 10/00 */
int  sh_apagado_en_falso(void);                        /* 1 = se pidió apagar y el teléfono sigue vivo por el cargador (pantalla fuera, esperando) */

/* ---- andamio de pantalla ------------------------------------------------------------------------------
 * Pantalla(título, cuerpo, tecla izq., tecla der.): barra de estado (título + hora), el cuerpo y la
 * barra de teclas de selección, todo bajo el Tema en uso. Es una función: hace de StatelessWidget. */
Widget *Pantalla(const char *titulo, Widget *cuerpo, const char *izq, const char *der);
int  sh_teclas(Ctx *c, int k, void *dato);       /* manejador por defecto: sel. izq. = OK, sel. der. = atrás */
Widget *sh_fila(const char *texto, const char *detalle, int sel);          /* fila normal de una lista */
Widget *sh_dato(const char *nombre, const char *valor);                    /* «nombre ........ valor» */
Widget *sh_boton(Ctx *c, const Foco *f, void *texto);                      /* constructor de Enfocable: botón con rótulo */

/* ---- utilidades ------------------------------------------------------------------------------------------ */
extern const char *const sh_dia[7];              /* "lun" … "dom" */
extern const char *const sh_mes[12];             /* "ene" … "dic" */
int   sh_ahora(HwFecha *f);                       /* la hora LOCAL: el RTC (que va en UTC) más la zona horaria de Ajustes. HW_OK o HW_ERROR (RTC sin poner) */
int   sh_poner_hora_local(const HwFecha *f);      /* pone el RTC a partir de una hora local */
const char *sh_zona(void);                              /* «UTC-4:00»; arena */
char *sh_hora(const HwFecha *f, int con_segundos);   /* según el formato elegido; cadena en la arena */
char *sh_tam(u32 bytes);                         /* «812 B», «57 KB», «1,2 MB»; arena */

/* ---- aplicaciones ---------------------------------------------------------------------------------------
 * Cada app vive en apps/<nombre>/ y da UNA función que devuelve su pantalla raíz (su «build»). El menú
 * la abre con ui_empujar(app.abrir()). Para dar de alta una: una línea en la tabla de shell/apps.c. */
/* Una entrada del menú: o es una app de casa (`abrir` da su pantalla de widgets) o es una app EXTERNA
 * a pantalla completa (`entrar` se queda con el teléfono y no vuelve hasta que el usuario sale).
 * docs/sistema.md §11. */
typedef struct { const char *nombre; int icono; Widget *(*abrir)(void); void (*entrar)(void);
                 const Icono *arte;   /* (M2) externa con icono 2D propio (AppExterna.arte); NULL = sh_iconos[icono] */
                 int malla;           /* (M2) externa con malla propia: su índice en el menú 3D MÁS 1; 0 = la de `icono` */
                 const char *ruta;    /* (APX) app de fábrica que vive FUERA del firmware: el .sen del W6FS (/apps/x.sen) */
                 const char *id;      /* (LANZ) QUÉ es la celda, para guardarla y reconocerla: la ruta de su .sen, «@archivos»,
                                       * «@sistema», «@ajustes», «@ext:Nombre» (externa enlazada) o «@apps» (la lista de todo) */
} App;
#define SH_MAX_APPS 16
extern App sh_apps[SH_MAX_APPS];
extern u32 sh_apps_gen;                          /* (28-oct) sube con cada sh_apps_init: quien guarde algo de sh_apps sabe cuándo rehacerlo */
Widget *sh_menu_medios(void);                    /* (28-oct) el menú de MEDIOS (shell/menu_medios.c), un estilo del menú (M3_MEDIOS) */
Widget *sh_menu_escritorio(void);                /* (6-nov) el ESCRITORIO con cursor (shell/menu_escritorio.c), otro estilo (M3_ESCRITORIO) */
void sh_escritorio_preparar(void);
int  sh_escritorio_visible(void);                /* 1 = el escritorio está arriba: la pantalla no gira (shell/giro.c) */

void sh_medios_preparar(void);
extern int sh_n_apps;
void sh_apps_pendiente(void);                    /* (SER) hay una app nueva en la Memoria: rehacer el menú al volver al standby */
void sh_apps_init(void);                         /* arma la tabla: las de casa y las externas que haya */
/* (LANZ) EL LANZADOR: Ajustes -> «Menú principal» (apps/ajustes/menu_principal.c) decide qué apps van en el menú y en qué orden.
 * El menú son SH_CELDAS celdas; «Apps» (la lista de todo) va siempre la última y Ajustes siempre está: quedan SH_LZ_MAX elegibles.
 * La pantalla trabaja sobre una lista de TODAS las apps (las de fábrica, las externas enlazadas y las .sen descubiertas): las del
 * menú primero, en su orden. Cada cambio se guarda en el acto; el menú se rehace al cerrar la pantalla (sh_lz_cerrar). */
#define SH_CELDAS 15
#define SH_LZ_MAX (SH_CELDAS - 1)
typedef struct { const char *id, *nombre; const Icono *icono; u8 juego, fijo, en_menu, _r; } ShLzApp;
int  sh_lz_abrir(void);                          /* arma la lista; da cuántas hay (0 en modo seguro: ahí no se cambia el menú) */
const ShLzApp *sh_lz_app(int i);                 /* NULL fuera de la lista */
int  sh_lz_marcar(int i);                        /* pone o quita del menú: 0 hecho; -1 es fija (Ajustes); -2 no cabe */
int  sh_lz_mover(int de, int a);                 /* cambia dos filas de sitio (el orden del menú); 0 hecho */
int  sh_lz_en_menu(void);                        /* cuántas van en el menú (sin contar «Apps») */
int  sh_lz_celda(int i);                         /* la celda (1..) que ocupará la fila i, o 0 si no va en el menú */
void sh_lz_fabrica(void);                        /* olvida la elección: vuelve la disposición de fábrica (y rehace la lista) */
void sh_lz_cerrar(void);                         /* al salir: si algo cambió, rehace el menú */
Widget *app_menu_principal(void);                /* la pantalla (apps/ajustes/menu_principal.c) */
void sh_abrir_app(int i);                        /* abre la entrada i del menú (de casa: una ruta; externa: se queda con el teléfono) */
typedef struct AppExterna AppExterna;            /* api/app_externa.h; aquí basta el nombre */
int  sh_apps_externas(const AppExterna **v, int max);   /* las que dejó el enlazador en apps_ext */
extern Icono sh_iconos[];
enum { IC_ARCHIVOS, IC_IMAGENES, IC_RELOJ, IC_NOTAS, IC_CALC, IC_JUEGO, IC_CONSOLA, IC_TECLAS, IC_SISTEMA, IC_AJUSTES,
       IC_TELEFONO, IC_MUSICA, IC_CARPETA, IC_FICHERO, IC_ALARMA, IC_ENCHUFE, IC_CANDADO, IC_RAYO, IC_NOTA, IC_MOVIMIENTO, IC_PASOS,
       IC_APPS /* (EST3) la celda «Apps»; solo del sistema: no está entre los W5_IC_* de las .sen */, IC_N };
void sh_iconos_init(void);
const Icono *sh_icono_externo(const char *const *arte);   /* (M2) empaqueta el `arte` de una app externa; NULL si no vale o no caben más */
#define SH_APP_MUSICA "/apps/musica.sen"       /* (28-oct) la app Música: la de la red y lo descargado (hasta el 28-oct ytmusic.sen; la de antes, el Walkman, retirada) */
void sh_abrir_sen(const char *ruta, const char *argumento);   /* (1.2, APX) abre una app de fábrica (.sen) con un argumento y la empuja; shell/cg12_d.c */
Widget *cg_abrir_ruta(const char *ruta);         /* (C) carga y abre una app CARGABLE (.sen): shell/cargador.c. NULL = era un juego y ya ha vuelto */
Widget *sistema_raiz(void);                      /* la primera ruta: el standby */

/* ---- energía, bloqueo y tapa (shell/energia.c, pista S3) ------------------------------------------------
 * Una máquina de estados con el reloj de hw_ms(): VIVA -> ATENUADA (aj.atenuar_s) -> APAGADA (aj.apagar_s;
 * luz fuera, panel dormido, la UI sigue al día pero no pinta). Cualquier tecla, el giro de la tapa, la
 * alarma o el cargador la devuelven a VIVA. Con la pantalla APAGADA el bucle del teléfono puede dormir a
 * fondo (hw_dormir) hasta el siguiente temporizador de la UI.
 * Bloqueo del teclado: «*» y luego la tecla de selección izquierda (desde el standby), la tecla Bloqueo,
 * o automático al apagarse la pantalla si aj.bloqueo_auto. Mientras está bloqueado, NINGUNA tecla llega a
 * las apps: hay una ruta propia con el candado y la combinación para salir. */
enum { EN_VIVA, EN_ATENUADA, EN_APAGADA };
void sh_energia_init(void);          /* instala el filtro de teclas; lo llama sistema_raiz() */
void sh_energia_paso(void);          /* la máquina de estados: una vez por vuelta del bucle de la plataforma */
u32  sh_dormir_ms(void);             /* ms que se puede dormir a fondo AHORA (0 = no dormir) */
u32  sh_inactivo_ms(void);           /* (I1) ms desde la última actividad (tecla, tapa…) */
void sh_actividad(void);             /* «ha pasado algo»: reinicia la cuenta de inactividad y enciende la pantalla */
void sh_insistir(u32 ms);            /* una alerta sin atender: la pantalla NO se apaga en ese rato (tope 120 s) */
int  sh_pantalla_estado(void);       /* EN_VIVA / EN_ATENUADA / EN_APAGADA */
int  sh_bloqueado(void);
void sh_bloquear(int si);
int  sh_tapa_cerrada(void);
Widget *sh_bateria(void);            /* el icono de la batería con su nivel (barra de estado) */
void sh_apagar(void);                /* pantalla de despedida y apagado de verdad (hw_apagar) */
int  sh_reiniciar(void);             /* (AUD B7) como sh_apagar: lo pendiente (copia, música) primero, y hw_reiniciar. Solo vuelve si no hay */
void sh_energia_suspender(int si);   /* 1 = ni atenuar, ni apagar, ni bloquear, ni la tapa (una app EXTERNA a pantalla completa) */
void sh_energia_retener(int si);     /* (R2) lo mismo para una pantalla DE CASA (Movimiento), con TOPE: a los 10 min sin teclas la energía vuelve a mandar */
/* (R2) lo que solo llama el sistema (plat/): los dos dueños del gancho del tic y las claves del almacén de una app externa */
int  hwo_tic_sistema(void (*f)(void));
int  hwo_tic_app_fuera(void);
void hwo_claves_app(int si);
void sh_app_externa_recoger(void);   /* (R2) tras volver de `entrar`: si la app no soltó la pantalla, la memoria, el gancho, el tono o el vibrador, se recogen */
u32  app_mem_pico(void);             /* bytes que ha llegado a pedir prestados una app externa */
int  app_pantalla_tomada(void);
int  sh_pantalla_apagada(void);      /* (R3) 1 = el panel está dormido por inactividad */
void sh_flash_progreso_init(void);   /* (R3; shell/misarchivos.c) pone el gancho de hw_progreso: la franja «Guardando…» */
void sh_pantalla_arranque(const char *linea);   /* splash: se pinta ANTES de montar la UI (el FS tarda) */
#ifdef MIGAS                                       /* (DIAG 2026-10-20) migas en pantalla: dónde se cuelga el arranque en el aparato */
void sh_miga(const char *paso);
#define MIGA(p) sh_miga(p)
#else
#define MIGA(p) ((void)0)
#endif

/* ---- MIS ARCHIVOS (shell/misarchivos.c, pista S6): guardar ficheros del usuario desde las apps ------------------
 * La zona de ficheros de usuario (hw_usr_*) es la carpeta HW_FS_USUARIO de hw_fs_*: se lee con los visores de
 * siempre. Esto es lo que comparten Archivos y Notas para ESCRIBIR en ella. docs/sistema.md §13. */
#define SH_USR_CANCELADA 2
const char *sh_usr_motivo(void);     /* hw_usr_motivo() dicho para una persona (lleno, nombre repetido, 64 archivos…) */
void sh_pedir_nombre(const char *titulo, const char *inicial, void (*hecho)(const char *nombre, void *dato), void *dato);
                                     /* una pantalla que pide un nombre (1..47 bytes, sin «/»); `hecho` solo llega con uno válido */
void sh_usr_progreso(const char *titulo, const char *que, void (*fin)(int r, void *dato), void *dato);
/* (I1) LA MEMORIA (W6F): lo mismo para hw_w6f_* (shell/misarchivos.c) */
const char *sh_w6f_motivo(void);     /* hw_w6f_motivo() dicho para una persona */
const char *sh_w6f_situacion(const HwW6fEstado *w);   /* «se escribe, 14 MB», «OTRA VERSIÓN: no se toca»… (Sistema) */
const char *sh_w6f_sl(int motivo);   /* por qué está de solo lectura (W6fEstado.solo_lectura) */
void sh_w6f_progreso(const char *titulo, const char *que, void (*fin)(int r, void *dato), void *dato);   /* la barra de hw_w6f_copiar */
                                     /* la barra de la copia o el renombrado YA EMPEZADO; fin: HW_OK, HW_ERROR o SH_USR_CANCELADA */
void sh_tj_progreso(const char *titulo, const char *que, void (*fin)(int r, void *dato), void *dato);   /* (FATW) la barra de hw_tj_copiar */
const char *sh_tj_motivo(void);      /* (FATW) hw_tj_motivo() dicho para una persona */
void sh_captura(void);               /* la pantalla, a «captura NN.png» en Mis archivos (con «Actividad» mantenida, «#») */
int  sh_musica_tocar_ruta(const char *ruta);     /* toca ese MIDI si está en la lista (la rehace antes); HW_OK o HW_NO_DISPONIBLE */
void sh_musica_refrescar(void);      /* han cambiado los ficheros de Mis archivos: rehace la lista de pistas si el motor está en marcha */

/* ---- PASOS (shell/pasos.c, pista S2): el podómetro como servicio ------------------------------------------
 * Cuenta siempre que aj.pasos esté puesto, al ritmo del original (encender, ~20 ms, convertir, apagar; una muestra
 * cada 40 ms) y con su modo «vigilar». El detector es shell/podometro.c. Todo lo demás, en la cabecera de pasos.c. */
enum { SH_PASOS_SIN_AHORRO, SH_PASOS_FIEL, SH_PASOS_CORREGIDO, SH_PASOS_AHORROS };   /* por omisión, sin ahorro: DECISIÓN ABIERTA (docs/sistema.md §6.8) */
typedef struct {
    u8  activo, hay_sensor, dormido, gesto;      /* muestreando · el PMU contesta · sensor apagado por «vigilar» · pod_gesto() */
    u8  x, y;                                    /* la última muestra cruda */
    u32 muestras, sin_muestra, tarde, sacudidas, fallos;   /* iteraciones con y sin sensor · fases retrasadas · lecturas fallidas */
    u32 version;                                 /* sube con cada muestra y con cada cambio (para repintar) */
} PasosInfo;
void sh_pasos_init(void);
void sh_pasos_paso(void);            /* una vez por vuelta del bucle de la plataforma (y desde app_latido) */
u32  sh_pasos_proximo_ms(void);      /* ms hasta que necesite la CPU (0xFFFFFFFF = parado): para el reposo profundo */
const Podometro *sh_pasos(void);     /* el detector: pasos, dist, kmh, eje, andando… (solo leer) */
void sh_pasos_info(PasosInfo *p);
void sh_pasos_a_cero(void);
void sh_pasos_ajuste(int si, int ahorro);   /* cambia aj.pasos y aj.pasos_ahorro y los guarda */
void sh_pasos_retener(int si);       /* 1 = una pantalla quiere el sensor en vivo: sin modo vigilar, y aunque aj.pasos sea 0 */
void sh_pasos_retener_menu(int si);
void sh_pasos_retener_giro(int si);      /* (estética) el giro automático de la pantalla (shell/giro.c) */
void sh_giro_paso(void);                 /* (estética) mira el sensor y gira la pantalla si toca (lo llama sh_pasos_paso) */
int  sh_giro_de(int x, int y, int ahora);   /* la orientación que pide una muestra cruda, o -1 = quedarse (shell/giro.c) */  /* lo mismo, para el menú 3D (otro dueño: no se pisan) */

/* ---- LUCES (shell/luces.c, pista S5): las dos laterales RGB, el latido y los efectos «.ble» -----------------
 * El motor está en api/luces.h; esto es lo que decide el sistema con él. Todo se apaga en Ajustes -> «Luces». */
#define SH_LUZ_COLORES 10
#define SH_LUZ_CASA 4                /* efectos de casa (en memoria): Respirar, Arcoíris, Baliza, Aviso */
enum { SH_LUZ_EV_ALARMA, SH_LUZ_EV_TEMPORIZADOR, SH_LUZ_EV_BATERIA, SH_LUZ_EVENTOS };
extern const char *const sh_luz_color_nombre[SH_LUZ_COLORES];
void sh_luces_init(void);            /* le dice el color a la plataforma ANTES de que la pantalla encienda nada */
void sh_luces_paso(void);            /* una vez por vuelta del bucle de la plataforma */
void sh_luces_latido(void);          /* el foco del menú se ha movido */
void sh_luces_inclina(int x, int y); /* la inclinación que pinta el menú 3D */
void sh_luces_color(u8 rgb[3]);      /* el color que toca ahora, en % (0, 0, 0 = apagadas) */
void sh_luces_ajuste(int luces, int color, int latido);   /* cambia los tres ajustes y los guarda */
void sh_luces_evento(int cual);      /* SH_LUZ_EV_*: su efecto de fábrica si está el FS original; si no, el de casa */
const char *sh_luces_dir(void);      /* la carpeta de los efectos de fábrica en el FS, o NULL si no está */
int  sh_luces_fabrica(const char *relativo);   /* «contacteffects/Disco.ble»: HW_OK, HW_NO_DISPONIBLE o HW_ERROR */
int  sh_luces_casa(int i);
const char *sh_luces_casa_nombre(int i);
void sh_luces_parar(void);
void sh_luces_asentar(void);         /* para quien se queda con el bucle (una app externa): nada a medias, las laterales en su color */
const char *sh_luces_sonando(void);  /* el nombre del efecto en marcha, o "" */

/* ---- INCLINACIÓN (shell/inclina.c, pista V): del acelerómetro al vector con que se inclina el menú 3D --------
 * Unidades de ángulo: 1/4096 de vuelta. x > 0: se ve el lado izquierdo de las cosas; y > 0: se ve su cara de arriba.
 * Qué eje y qué sentido del sensor es cada uno NO SE SABE hasta tener el aparato: es el ajuste aj.menu_ejes. */
enum { SH_EFECTO_INCLINA, SH_EFECTO_VAIVEN, SH_EFECTO_QUIETO, SH_EFECTOS };
#define SH_INCLINA_MAX 205           /* el tope: 18° */
void sh_inclina_abrir(void);         /* el menú se ve: pide el sensor (si el efecto es por inclinación) y «plano» pasa a ser la postura de ahora */
void sh_inclina_cerrar(void);        /* el menú ya no se ve: suelta el sensor */
void sh_inclina_centrar(void);       /* «plano» = la postura de ahora */
int  sh_inclina_paso(int dt_ms, i16 *x, i16 *y);   /* una vez por cuadro del menú; 1 si lo que hay que pintar ha cambiado */
typedef struct { i16 crudo[2], plano[2], filtrado[2], objetivo[2], pintado[2]; u32 muestras; u8 sensor, retenido; } InclinaInfo;
void sh_inclina_prueba(int si, int x, int y);   /* solo para las medidas: el objetivo a mano, sin sensor */
void sh_inclina_info(InclinaInfo *i); /* para las pruebas y para la app Sistema: en cuentas x16 y en unidades de ángulo */
Widget *sh_menu(void);               /* el menú principal (shell/menu3d.c) */
void sh_menu_preparar(void);         /* en el arranque: deja hecha la imagen del fondo del estilo elegido */
Widget *sh_reloj_barra(const TemaDatos *t);   /* batería + avisos + hora, con los colores de t (NULL = los del tema del sistema) */
Widget *sh_bateria_de(Color c);      /* el icono de la batería en ese color */

/* ---- sonido (shell/sonido.c, pista S5) -------------------------------------------------------------------
 * Un secuenciador de melodías del ZUMBADOR que no bloquea: la melodía avanza en sh_sonido_paso(), una vez
 * por vuelta del bucle. Cada sonido trae su prioridad (el clic de una tecla NO corta una alarma) y los
 * avisos que piden atención se repiten hasta que alguien pulse algo (sh_callar). El «volumen» de Ajustes
 * es el ANCHO DEL PULSO, que es lo único que cambia la energía que sale del pin y se puede MEDIR en el
 * WAV: docs/sistema.md §14. */
enum { SN_NADA = 0, SN_TECLA, SN_BLOQUEO, SN_DESBLOQUEO, SN_ERROR, SN_ARRANQUE, SN_APAGADO,
       SN_PRUEBA, SN_BATERIA, SN_TEMPORIZADOR, SN_ALARMA, SN_N };
void sh_sonar(int cual);             /* SN_*; no hace nada con el volumen a 0 ni si suena algo más importante */
void sh_sonar_n(int cual, int vueltas);   /* lo mismo, pero la melodía se da `vueltas` veces y calla sola (los avisos DENTRO de una app externa) */
#define SN_VOL_MAX 5                         /* aj.volumen: 0 = silencio, 1..5 */
void sh_callar(void);
void sh_sonido_paso(void);           /* una vez por vuelta del bucle de la plataforma */
int  sh_sonando(void);               /* el SN_* que suena ahora, o 0 */
u32  sh_sonido_proximo_ms(void);     /* ms hasta el siguiente cambio de nota (0 = nada suena) */
void sh_sonido_volumen(int v);       /* 0..5; lo guarda */
int  sh_sonido_ancho(void);          /* el ancho del pulso (ciclo %) del volumen de ahora */

/* ---- MÚSICA (shell/musica.c, pista S7) --------------------------------------------------------------------
 * El motor del Walkman. Vive en el shell para que la música siga al salir de su pantalla (apps/musica/).
 * Toca ficheros MIDI por DOS salidas: PCM por el DSP (polifonía) si hw_pcm_init contesta, y si no la línea
 * melódica por el zumbador. Guarda volumen, modo, repetición, última pista y posición en el fichero
 * «musica.cfg» de la zona de usuario (hw_usr_*). Todo lo demás, en la cabecera de shell/musica.c y en
 * docs/sistema.md §15. Ninguna de estas funciones bloquea, salvo la primera prueba del DSP (hasta 0,5 s). */
enum { MU_PARADA, MU_SONANDO, MU_PAUSA };
enum { MU_SAL_NADA, MU_SAL_PCM, MU_SAL_ZUMBADOR, MU_SAL_MP3 };            /* por dónde suena la pista en curso */
enum { MU_MODO_AUTO, MU_MODO_ZUMBADOR, MU_MODOS };            /* lo que pide el usuario: PCM si hay / solo zumbador */
enum { MU_REP_NO, MU_REP_UNA, MU_REP_TODAS, MU_REPS };
enum { MU_T_MIDI = 1, MU_T_DSP, MU_T_MP3 };                             /* se puede tocar / lo decodifica el DSP (AAC, AMR…) y no sabemos / MP3 por el DSP (solo W580i: hal/mp3_w580.c) */
#define MU_MAX_PISTAS 64
#define MU_RUTA 112
typedef struct { char ruta[MU_RUTA]; u32 tam; u8 tipo; } MuPista;   /* ruta del FS, o «casa:Nombre.mid» (la que va dentro del main) */
typedef struct {
    u8  estado, salida, volumen, modo, repetir;               /* volumen 0..5 */
    u8  pcm_probado, pcm_hay, cedida;                         /* cedida: en pausa porque suena un aviso del sistema */
    u8  guardado;                                             /* 0 nada que decir, 1 guardado, 2 no hay zona de usuario, 3 falló */
    i16 pista, n;                                             /* la pista cargada (-1 = ninguna) y cuántas hay */
    u32 pos_ms, dur_ms, notas, percusion, perdidas, pistas_smf, tam, pcm_marcos;
    int pcm_motivo;                                           /* por qué no hay PCM (hw_pcm_motivo), o 0 */
    u16 hz;                                                   /* la nota del zumbador ahora mismo */
    u8  red;                                                  /* (1.8) lo que suena viene de la cola de la red (shell/musica_red.c; pista = -1) */
} MuInfo;
void sh_musica_abrir(void);          /* arranca el servicio la primera vez: busca las pistas y recupera lo guardado */
void sh_musica_buscar(void);         /* rehace la lista (/musica, /tpa/user/audio, y la de casa) */
int  sh_musica_n(void);
const MuPista *sh_musica_pista(int i);
char *sh_musica_nombre(int i);       /* el nombre sin carpeta ni extensión; cadena en la arena */
void sh_musica_info(MuInfo *m);
int  sh_musica_tocar(int i);         /* HW_OK, o HW_ERROR si esa pista no se puede tocar */
int  sh_musica_tocar_red(int i, u32 desde_ms);   /* (1.8) la pista i de la cola de la red (shell/musica_red.h): HW_OK, HW_ERROR, HW_NO_DISPONIBLE */
void sh_musica_alternar(void);       /* reproducir / pausa (parada: empieza la pista cargada, o la primera) */
void sh_musica_pausa(void);
int  sh_musica_app(void);            /* (28-oct) una app externa toma la pantalla: el MP3 sigue (1), lo demás se pausa (0) */
void sh_musica_parar(void);
void sh_musica_saltar(int d);        /* +1 siguiente, -1 anterior (a más de 3 s de pista, vuelve a empezarla) */
void sh_musica_volumen(int v);       /* 0..5 */
void sh_musica_modo(int m);          /* MU_MODO_*; cambia de salida sin perder el punto */
void sh_musica_repetir(int r);       /* MU_REP_* */
void sh_musica_paso(void);           /* una vez por vuelta del bucle de la plataforma */
int  sh_musica_estado(void);         /* MU_PARADA / MU_SONANDO / MU_PAUSA */
int  sh_musica_dsp_activo(void);     /* 1 = un MP3 tiene el DSP o la línea de audio encendidos (sonando, en pausa o en su cierre diferido): sin reposo profundo */
void sh_musica_gancho(void);         /* (M2) vuelve a poner su paso en el tic, si lo tenía: se lo prestó a la melodía de una app externa */
int  sh_musica_en_zumbador(void);    /* 1 = está SONANDO por el zumbador (sonido.c no mete clics de tecla encima) */
void sh_musica_aviso(int empieza);   /* sonido.c: empieza (1) o calla (0) un aviso que pide atención */
int  sh_musica_tecla(int k);         /* el filtro de teclas: Walkman, Play y volumen; 1 = se la queda */
void sh_musica_pantalla(int abierta);/* apps/musica dice si su pantalla está montada */
int  sh_musica_ceder_dsp(void);      /* (1.10) el micrófono necesita el DSP: la música por el DSP se para del todo (apuntando dónde iba) y el DSP se apaga YA; 1 = había que reanudar */
void sh_musica_recuperar_dsp(void);  /* (1.10) el micrófono acabó: la música vuelve a sonar desde donde iba (si sonaba) */
void sh_mic_paso(void);              /* (1.10) shell/micro_sis.c: el arranque del micrófono, por pasos, desde el bucle y el latido de las apps */
void sh_dictado_init(void);          /* (E5) shell/dictado.c: la pulsación larga de «*» en un campo de texto dicta */
int  sh_dictado_activo(void);
void sh_musica_sin_teclas(int si);   /* 1 = el shell NO atiende Walkman, Play ni el volumen (el probador de teclas las quiere todas) */
void sh_musica_guardar_ya(void);     /* lo pendiente, a la zona de usuario AHORA (al apagar) */
#include "bt_sis.h"                  /* (pista BT) el Bluetooth del sistema: dormido por omisión (docs/sistema.md §30) */
#endif
