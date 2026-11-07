/* EL DSP DEL W580i (DB2020): arranque, transporte de mensajes y diagnóstico. Pista de audio, 2026-10-21 (tmp/audio_ag/informe.md e informe2.md).
 *
 * TODO sale del ORIGINAL R8BE001 y lleva su dirección: el cargador DSP_RM_LoadAllMemories (0x4469F7A4) con sus dos fases (la ventana
 * FA040024/28/2C para las secciones <= 0x13FFF, 0x4469FE04 + 0x441FE4DC; y la XRAM por peticiones 0x1800/0x1801/0x1802 del DSP, canal 0x24,
 * 0x4469FB60 + 0x4469F8E4), el control de reloj y reset del DSP (0x44227CCC), el buzón (0x441A7F30/7F80/7F98, 0x4422C668), los mensajes largos y
 * los flujos por el DMA PL080 (0x4421DCC4, 0x4421DB1C, 0x44227BC4, DMA_Config/DMA_Start 0x441A7304/7444) y la petición de versión (0x446A00A8).
 * Marcas: [observado] = leído en el código del original; [ejecutado] = visto hacerlo al original en el emulador (tmp/audio_ag/o1.qemu.log,
 * o1.volcado.txt); [inferido]; [sin saber].
 *
 * NADA de esto se ha ejecutado con un DSP de verdad: el emulador no ejecuta el DSP (el original allí acaba en «DSP did not respond» y
 * «Version responder TIMED OUT»). Por eso TODO tiene tope de tiempo y de vueltas: si el DSP no contesta se vuelve con un código negativo, el
 * núcleo del DSP queda en reset y nada se cuelga. El perro de software late (hw_perro_latido) en todas las esperas.
 *
 * NO se arranca solo: lo pide quien llame a dsp_arrancar() (hoy, apps/sistema/dsp_diag.c, «Diagnóstico DSP»).
 *
 * Las imágenes que se cargan (la COMÚN 0x4546F080 y la del arranque 0x45478604 = el módulo MIDI_PCM_ASR) van INFLADAS dentro del binario
 * (hal/dsp_w580_img.c en el árbol de desarrollo, generado del firmware original): en el aparato, la NOR original de 0x4546F080 queda debajo
 * de la Memoria W6F (0x44500000..0x45AFFFFF) y no se puede contar con ella. EN ESTE REPOSITORIO PÚBLICO esas imágenes NO vienen (son del
 * fabricante): hal/fabricante_ausente.c las define vacías y dsp_cargar() devuelve DSP_E_IMAGEN. */
#ifndef DSP_W580_H
#define DSP_W580_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;

/* ---- las imágenes ------------------------------------------------------------------------------------------------------------ */
typedef struct {
    const char *nombre;            /* «comun», «arranque», «mp3»… */
    u32 dir;                       /* dónde estaba en la NOR del original (solo para decirlo) */
    u32 n_sec;                     /* entradas de la tabla (la última con n = 0) */
    const u32 (*tab)[2];           /* {n palabras, dirección del DSP} */
    const u16 *dat;                /* las secciones seguidas, en el orden de la tabla */
    u32 n_pal;                     /* palabras en total */
} DspImagen;
extern const DspImagen dsp_img_comun, dsp_img_arranque;     /* hal/dsp_w580_img.c (generado) */
/* Otros módulos del DSP (MP3, AAC…): un .c con `dsp_img_<nombre>` en hal/ (el Makefile compila todo hal/ y el enlazador tira lo que
 * nadie usa). Se cargan con dsp_cargar(&dsp_img_mp3) con el DSP ya arrancado (la común va siempre delante). */
extern const DspImagen dsp_img_mp3;                          /* solo existe si se copia ese fichero a hal/ */

/* ---- pasos del arranque (DspEstado.paso = el que se está haciendo o el último que se alcanzó) ------------------------------------- */
enum { DSP_P_NADA = 0,
       DSP_P_PREPARA,          /* guarda SYSCON; PL080 iniciado (como pd_dma al arrancar el teléfono) */
       DSP_P_DSPIF,            /* FUN_441A7ED0: puerta 5, pulso del bloque 3, +08 = 1, puerta 5 cerrada */
       DSP_P_PCM,              /* FUN_441A7FE8: puerta 26, bloque 20 y los 11 registros de FA020000 (como el original al arrancar) */
       DSP_P_MASCARAS,         /* +10 leído, +0C &= ~0xDF y los tres clientes: 0x03, 0x1B, 0xDF */
       DSP_P_PREBOOT,          /* R_Req_AC_DSP_PreBoot: ClockRequest(1,26,4) + block_reset(20,0) */
       DSP_P_VENT_COMUN,       /* imagen común por la ventana FA040024/28/2C (fase 1) */
       DSP_P_VENT_MODULO,      /* la del arranque (o el módulo) por la ventana */
       DSP_P_SUELTA,           /* FUN_44227CCC(1): bloques 2 y 3 sueltos = EL DSP ARRANCA; 14 ms */
       DSP_P_ESPERA_1800,      /* el DSP pide la XRAM con 0x1800 (canal 0x24) */
       DSP_P_XRAM_COMUN,       /* fase 2 de la común: trozos por DMA + 0x1801 */
       DSP_P_XRAM_MODULO,      /* fase 2 del módulo; al acabar, 0x1802 */
       DSP_P_FIN,              /* 6 ms y la espera sobrante de un 0x1800 (su resultado se ignora) */
       DSP_P_VERSION,          /* 0x0201 por el canal 2 (mensaje largo de 112 palabras) y su respuesta */
       DSP_P_VIVO,             /* contestó la versión */
       DSP_P_APAGADO,          /* dsp_apagar() */
       DSP_P_N };

/* ---- errores (DspEstado.error y lo que devuelven las funciones) -------------------------------------------------------------------- */
enum { DSP_E_OK = 0,
       DSP_E_IMAGEN = -1,      /* imagen vacía, tabla mal o sin secciones */
       DSP_E_VENTANA = -2,     /* la ventana no se vació (+24 no volvió a 0) */
       DSP_E_NO_1800 = -3,     /* el DSP no pidió la XRAM (0x1800) en 1 s: NO ARRANCÓ su código */
       DSP_E_1800_MAL = -4,    /* el 0x1800 trae un tamaño o una dirección imposibles */
       DSP_E_XRAM_RELLENO = -5,/* la tabla de la imagen no cuadra al partirla en trozos */
       DSP_E_XRAM_DMA = -6,    /* no se pudo lanzar el DMA del trozo */
       DSP_E_XRAM_FIN = -7,    /* acabó el DMA pero el evento 3 de +10 (fin del flujo ARM->DSP) no llegó */
       DSP_E_NO_1800_MAS = -8, /* tras un 0x1801 el DSP no pidió el trozo siguiente */
       DSP_E_BUZON = -9,       /* el buzón ARM->DSP no se vació */
       DSP_E_SIN_VERSION = -10,/* la versión no contestó */
       DSP_E_ENVIO_LARGO = -11,/* el mensaje largo no pudo salir */
       DSP_E_PARAM = -12,      /* argumentos imposibles */
       DSP_E_OCUPADO = -13,    /* ya hay una operación en curso */
       DSP_E_NO_ARRANCADO = -14,/* el DSPIF no tiene reloj: primero dsp_arrancar() */
       DSP_E_RX_LARGO = -15,   /* una recepción larga del DSP falló */
       DSP_E_SIN_MENSAJE = -16 /* dsp_esperar: nada en el plazo */ };

#define DSP_RING_PASOS 32      /* el anillo de pasos */
#define DSP_RING_MSGS  12      /* el anillo de mensajes para la pantalla */
#define DSP_MSG_MAX    256     /* palabras de carga que guarda cada mensaje de la cola de recepción (la versión son 112; un mensaje largo cabe hasta 1023) */

typedef struct { u8 paso; signed char res; u16 sec; u32 ms; u32 extra; } DspPasoReg;       /* ms desde el inicio de dsp_arrancar(); sec = n.º de orden */
typedef struct { u8 rx; u8 clase; u16 canal; u16 n; u16 p[4]; } DspMsgReg;                  /* rx 0 = mandado por nosotros; n = palabras de carga; p = las 4 primeras */
/* un mensaje recibido (carga SIN el canal). clase 0x0A = mensaje de un canal (corto o largo); cualquier OTRA clase del buzón (la 0x14 de control, las
 * desconocidas) se entrega igual, con canal = 0xC000 | clase y el cuerpo entero en d[]: así se puede recibir CUALQUIER respuesta. */
typedef struct { u16 canal; u16 n; u16 d[DSP_MSG_MAX]; u8 clase, largo, recortado, _r; } DspMsg;

typedef struct {
    u8  paso, intentos;
    signed char error;             /* DSP_E_* del último intento (el primero que se produjo) */
    u8  vivo;                      /* 1 = contestó la versión */
    u8  gate5;                     /* 1 = el DSPIF tiene reloj (se puede leer) */
    u8  pcm_abierto;               /* 1 = la puerta 26 y el bloque 20 los abrió el arranque */
    u8  nucleo;                    /* 1 = el núcleo del DSP (bloque 2) está suelto */
    u8  ocupado;
    u32 ms_paso[DSP_P_N];          /* cuánto duró cada paso (el último intento) */
    u32 ms_total;                  /* el arranque entero */
    u32 ev[8];                     /* veces vista cada bit de FA040010 */
    u32 ev_otros;                  /* bits 8..15 */
    u32 ev_visto;                  /* OR de todo lo leído en FA040010 desde el arranque */
    u32 ventana_pal, ventana_esc;  /* palabras mandadas por la ventana y escrituras de +2C */
    u32 xram_pet, xram_trozos, xram_pal; /* peticiones 0x1800 recibidas, trozos mandados y palabras de datos reales dentro de ellos */
    u32 xram_dir, xram_cap;        /* dirección (en el DSP) y capacidad en palabras que dijo el primer 0x1800 */
    u32 tx_cortos, tx_largos, rx_cortos, rx_largos, rx_ctrl, rx_perdidos, fin_tx_fallos;
    u32 dma_lanzados, dma_tc, dma_err;     /* DMA lanzados; veces que el PL080 dio fin de transferencia / error en el canal 4 */
    u32 dma_ctrl, dma_cfg, dma_src, dma_dst;   /* lo último que se programó en el canal 4 */
    char version[64];              /* «## DSP VersionIdentity» (la cadena, un carácter por palabra desde la 1) */
    int  sondeo_ms;                /* (v11) sondeo corto: ms hasta que el DSP leyó un 0x1101 de 6 palabras; -1 = no lo leyó en 500 ms */
    u8   sondeo_hecho, version_intentos;
    u16  sc_ini[10];               /* (v14) SYSCON +02 +04 +06 +0A +0C +1C +38 +54 +80 +36 al EMPEZAR (antes de tocar nada): los de reloj que el original maneja con su motor de ClockRequest */
    u32  campanillas;              /* (2026-10-24) veces que el DSP pidió dormir (evento 7 de FA040010) y se atendió: ver dspi_campanilla() */
    u16  camp_marca, camp_cmd;     /* lo último que leímos en la palabra 0x13F14/15 del DSP: 0xFEED y el comando (0x2000 = dormir) */
    u32  camp_mal;                 /* veces que no se pudo atender (ventana ocupada) */
    u32  camp_dormido;             /* veces que se concedió el reposo sin trabajo pendiente (el DSP queda dormido: ver dspi_campanilla) */
    u16  s2c_ini, s2c_puesto;      /* (v13) SYSCON +2C al empezar y lo que se puso (el original lo deja en 0x0CCC en reposo y 0x0CCF con el DSP; senew arranca con bits 8 y 9 a 1) */
} DspEstado;
/* (v11) EXPERIMENTOS del arranque, elegibles desde «Diagnóstico DSP» (teclas 4, 6 y 7): el DSP real arranca y carga la XRAM pero no lee el buzón después (tx largo de la
 * versión sin confirmar; v10, 2026-10-21). Viven fuera de DspEstado porque dsp_arrancar() lo pone a cero. */
typedef struct { u16 pausa_ms; u8 sondeo, reintentos, re1802, r2c, sin_camp; } DspExp;   /* sin_camp (+6): dsp_arrancar NO activa el sondeo de la campanilla */
extern DspExp dsp_exp;

typedef struct {
    u8  valido;                    /* 1 = el DSPIF tiene reloj y se han leído sus registros */
    u32 r00, r04, r10, r14, r18, r1c;   /* FA040000/04/10/14/18/1C; r10 = OR de los eventos vistos (leer +10 los borra) */
    u32 s12, s18, s2c;             /* SYSCON +12 (bloques), +18 (puertas), +2C */
    u16 sc[10];                    /* (v14) los mismos 10 registros de reloj, AHORA */
    u32 pl_ctrl, pl_cfg, pl_tc, pl_err; /* PL080 canal 4: control, configuración, F2000014 (TC en bruto), F2000018 (error en bruto) */
} DspRegs;

/* ---- API -------------------------------------------------------------------------------------------------------------------- */
int  dsp_arrancar(void);           /* DSP_E_OK o un DSP_E_*: todo con tope; si falla, el núcleo queda parado (bloque 2 a reset) y el DSPIF con reloj para poder leerlo */
int  dsp_cargar(const DspImagen *modulo);   /* LoadAllMemories: la común + ese módulo (ventana + XRAM). Requiere el DSPIF con reloj (tras dsp_arrancar) */
int  dsp_pedir_version(void);      /* 0x0201 por el canal 2 y su respuesta; la cadena queda en dsp_version() */
void dsp_apagar(void);             /* núcleo y DSPIF a reset, puertas 2..5 cerradas, reloj del PCM cerrado si lo abrimos, SYSCON +2C como estaba */
const DspEstado *dsp_estado(void);
const char *dsp_paso_nombre(int paso);
const char *dsp_error_nombre(int err);
const char *dsp_version(void);     /* cadena vacía si no hay */
int  dsp_ultimos_mensajes(DspMsgReg *dst, int max);   /* los más recientes primero; devuelve cuántos */
int  dsp_pasos(DspPasoReg *dst, int max);             /* idem, del anillo de pasos */
void dsp_regs(DspRegs *r);         /* relee los registros (solo si el DSPIF tiene reloj) */
void dsp_progreso(void (*f)(void));   /* gancho que se llama al empezar cada paso y de vez en cuando en los largos (la pantalla se repinta) */
int  dsp_vivo(void);
void dsp_servir(void);             /* ATENDER AL DSP: vacía eventos y buzón y contesta a su campanilla de reposo (hay que llamarlo a menudo mientras viva; ver dspi_campanilla) */
/* (pista MP3) Gancho: se llama con CADA mensaje que pasa por el transporte (rx 0 = mandado, 1 = recibido; clase del buzón; canal; n palabras de carga SIN el canal; p = la
 * carga COMPLETA). Lo usa hal/mp3_w580.c para el diagnóstico (el anillo dsp_ultimos_mensajes solo guarda 4 palabras). NULL = nada. */
extern void (*dsp_gancho_msg)(int rx, int clase, unsigned canal, unsigned n, const u16 *p);

/* Transporte (con el DSPIF con reloj). `carga` = las palabras SIN el canal: el canal va en la última palabra del cuerpo, como hace el original
 * (FUN_4421E1BC). Corto si cuerpo (carga + canal) < 31 palabras; largo, por DMA, si no. */
int  dsp_enviar(u16 canal, const u16 *carga, unsigned n);   /* hasta ~1000 palabras de carga: corto por el buzón o largo por DMA, lo decide el tamaño */
/* Un mensaje del buzón TAL CUAL (clase y cuerpo, hasta 31 palabras, sin añadir el canal): para probar clases que no sean la 0x0A. */
int  dsp_enviar_raw(u16 clase, const u16 *cuerpo, unsigned n);
/* Saca de la cola (o lee del buzón) un mensaje; 0 = lo hay, DSP_E_SIN_MENSAJE = nada. canal 0xFFFF = cualquiera. */
int  dsp_recibir(DspMsg *m, u16 canal);
/* Espera un mensaje de ese canal cuya palabra 0 sea `orden` (0 = cualquiera). 0 = lo hay; DSP_E_SIN_MENSAJE si se acaba el plazo. */
int  dsp_esperar(DspMsg *m, u16 canal, u16 orden, unsigned ms);
/* (2026-10-26) EL CERROJO ENTRE EL BUCLE Y EL TIC (docs/mp3_dsp.md §7). Todas las funciones de arriba lo toman solas; dsp_bucle_tomar/soltar
 * reservan el transporte para una secuencia entera (una RPC, una rampa). Anidable. Mientras dsp_bucle != 0 el alimentador del tic no toca el DSP. */
extern volatile u8 dsp_bucle;
void dsp_bucle_tomar(void);
void dsp_bucle_soltar(void);
extern void (*dsp_vuelo_fin)(int r);        /* el bucle terminó un envío largo que el tic dejó en vuelo: r = 1 hecho, < 0 fallo */
extern int (*dsp_filtro_rx)(unsigned canal, const u16 *d, unsigned n);   /* lo recibido (corto, clase 0x0A): 1 = me lo quedo (no va a la cola) */
extern volatile u8 dsp_en_tic;              /* lo pone el alimentador mientras trabaja en la interrupción (la campanilla mira dsp_camp_tic) */
/* SOLO desde el tic (sin el bucle dentro: dsp_bucle == 0): nada espera */
int  dspi_tic_recibir(void);                /* eventos, campanilla, mensajes cortos a la cola (los créditos al filtro) y el acuse pendiente; 1 = un mensaje de control espera al bucle */
int  dspi_tx_corto_ya(u16 canal, const u16 *carga, unsigned n);   /* 0, o DSP_E_BUZON / DSP_E_OCUPADO si ahora no se puede (sin tocar nada) */
int  dspi_tx_largo_iniciar(u16 canal, const u16 *carga, unsigned n);   /* 0 = en vuelo; DSP_E_BUZON = luego */
int  dspi_tx_largo_mirar(void);             /* 1 = hecho (o nada en vuelo), 0 = aún, < 0 = fallo */
int  dspi_tx_en_vuelo(void);
#endif
