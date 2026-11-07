/* placa.h — LA DESCRIPCIÓN DE PLACA: lo que distingue un modelo de otro, como DATOS (una estructura constante por
 * modelo, en placas/<modelo>/placa.c) y no como #ifdef. El núcleo (ui, shell, servicios, apps) la LEE con placa();
 * no incluye nunca una cabecera de soc/, chips/ ni placas/.
 * Regla: aquí solo entra lo que el núcleo necesita PREGUNTAR. Lo que solo necesita el port (pines, registros,
 * números de IRQ, puertas de reloj) vive en placas/<modelo>/placa_int.h y no sale de ahí. */
#ifndef PLACA_H
#define PLACA_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;

/* CAPACIDADES: lo que el núcleo pregunta antes de ofrecer algo (un ajuste, una app, un servicio). */
enum { CAP_ZUMBADOR     = 1u << 0,    /* hw_tono* suena */
       CAP_PCM          = 1u << 1,    /* hw_pcm_* puede abrirse (DSP) */
       CAP_VIBRADOR     = 1u << 2,
       CAP_ACELEROMETRO = 1u << 3,    /* hw_acel_*: podómetro, menú inclinable, app Movimiento */
       CAP_TAPA         = 1u << 4,    /* hay un sensor de «cerrado»: deslizador, tapa de concha o giro (qué es: tapa_tipo) */
       CAP_LUZ_TECLADO  = 1u << 5,
       CAP_LUZ_RGB      = 1u << 6,    /* ternas de color (luces laterales): motor de efectos completo */
       CAP_RTC_ALARMA   = 1u << 7,
       CAP_SERIE        = 1u << 8,
       CAP_FS_FABRICA   = 1u << 9,    /* hay lector del FS de fábrica de este modelo (/original) */
       CAP_APAGAR       = 1u << 10,   /* hw_apagar apaga de verdad */
       CAP_BATERIA_MV   = 1u << 11,   /* hw_bateria_mv da milivoltios creíbles */
       CAP_PERRO        = 1u << 12,   /* perro de software por el tic */
       CAP_PANEL_DETECTADO = 1u << 13 }; /* (P600) el panel se detecta en el arranque (W580: R28 del PDI): Sistema enseña cuál */

enum { TAPA_NO, TAPA_DESLIZADOR, TAPA_CONCHA, TAPA_GIRO };
enum { PIX_RGB565_BE, PIX_RGB565_LE };            /* orden de bytes del píxel que quiere el camino al panel */

/* Una celda del teclado: su nombre en la carcasa y a qué tecla lógica (K_*) cae (0 = ninguna). */
typedef struct { const char *nombre; u8 tecla; } PlacaCelda;

/* Una zona de la flash que es NUESTRA. El sistema no escribe NUNCA fuera de las que estén aquí con escribible = 1. */
typedef struct { const char *nombre; u32 base, tam; u32 bloque_logico; u8 escribible; } PlacaZona;
enum { ZONA_MAIN, ZONA_FS_PROPIO, ZONA_AJUSTES, ZONA_USUARIO, ZONA_FS_FABRICA, ZONA_MEMORIA, ZONA_N };   /* (P600) + la Memoria (W6F) */

/* La política de batería: SOLO números; las reglas (api/bat.h, api/enc.h) son código común y probado en el PC. */
typedef struct { int base_mv, pend_umv; } PlacaRecta;      /* mV = base_mv + (cuenta * pend_umv + 500) / 1000 */
typedef struct {
    u16 mv_no_creible, mv_arranque, mv_aviso, mv_corte;    /* 0 = desconocido: la regla no actúa y Sistema lo DICE */
    PlacaRecta frio, marcha;                               /* pend_umv = 0: no hay recta, hw_bateria_mv = HW_NO_DISPONIBLE */
    u8 carga_permitida;                                    /* SIEMPRE 0 en un port nuevo (docs/portabilidad.md §5.5); 1 solo el W580i: su carga USB está medida (CARGA) */
    u8 medido_en_aparato;                                  /* 0 = todo sale de leer el firmware original; 1 = cotejado en un teléfono */
} PlacaBateria;

typedef struct Placa {
    const char *modelo, *soc;          /* «W580i», «DB2020» */
    u16 api_placa;                     /* versión de ESTA estructura (PLACA_API): el núcleo se niega a arrancar con otra */
    u32 capacidades;                   /* CAP_* */
    /* pantalla */
    u16 ancho, alto; u8 pixel;         /* PIX_* */
    /* teclado */
    u8 filas, columnas, a_masa;        /* la matriz cruda que devuelve hw_teclas_mapa: filas x columnas + a_masa teclas directas */
    const PlacaCelda *celdas;          /* filas*columnas + a_masa entradas */
    u8 tecla_modo_seguro;              /* K_* que, pulsada al encender, da el modo seguro */
    /* cuerpo */
    u8 tapa_tipo;                      /* TAPA_* */
    u8 luces_sencillas, luces_rgb;     /* cuántas salidas de un canal y cuántas ternas ofrece hw_luz_* */
    const char *const *luz_nombre;     /* nombre de cada salida sencilla, para Ajustes/Luces */
    /* memoria */
    u32 ram_base, ram_tam;             /* la RAM del sistema (lo prestado a una app sale de aquí: plat_mem_app) */
    const PlacaZona *zonas;            /* ZONA_N entradas; tam = 0 = este modelo no la tiene */
    /* energía */
    PlacaBateria bat;
    u16 tic_hz, cpu_mhz;
    /* contenido de fábrica: dónde suele estar lo del usuario en el FS original de este modelo (NULL-terminado) */
    const char *const *rutas_musica, *const *rutas_imagen;
    /* (P600, paso 4 de §3.7) textos que el núcleo enseña: el nombre de la carcasa (standby, pantalla de arranque) y qué chip de
     * flash espera la guarda de la placa (aviso «Memoria protegida") */
    const char *rotulo, *flash_esperada;
    /* (P600, paso 3) para la app Teclas: el nombre CORTO de cada celda (rejilla) y cómo se numera la celda (1 = columna*5 + fila,
     * como el W580i; 0 = fila*5 + columna, como el W600i) */
    const char *const *celda_corto; u8 celda_columna_mayor;
} Placa;
#define PLACA_API 1
const Placa *placa(void);              /* la da la placa enlazada; en el PC, la del MODELO simulado */
static inline int placa_tiene(u32 cap) { return (placa()->capacidades & cap) == cap; }
#endif
