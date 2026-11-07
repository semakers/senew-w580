/* musica_red.h — (1.8, 26-oct) LA MÚSICA POR LA RED, en el SISTEMA: una cola de URLs de MP3 que se bajan por la red del puente Bluetooth
 * (shell/red_sis.c, como dueño SH_BT_MUSICA) a la fuente FLUJO del motor (hw_mp3_flujo_*, con el colchón del sistema de 192 KB). Por ser del
 * sistema y no de la app, SIGUE SONANDO CON LA APP CERRADA (docs de la música por la red). La usa shell/musica.c (el servicio de música: PLAY,
 * pausa, siguiente, volumen) y la piden las apps por la API 1.8 (api/cargable_18.h). No sabe de dónde sale la música: toca URLs.
 *
 * Un corte del Bluetooth: el colchón tapa ~12 s (el motor hace una pausa limpia si se vacía y sigue solo); al volver la red se pide el resto
 * con «Range: bytes=N-» desde el byte que falta (el servidor tiene que saberlo: bt/musica_yt lo hace). */
#ifndef MUSICA_RED_H
#define MUSICA_RED_H
#include "hw.h"

#define MRED_MAX     32                  /* pistas en la cola */
#define MRED_URL     192
#define MRED_TITULO  100                 /* el servicio (bt/musica_yt) manda hasta 96 bytes */
#define MRED_ARTISTA 68                  /* y hasta 64 */
#define MRED_PORQUE  48
enum { MRED_QUIETA, MRED_CONECTANDO, MRED_PIDIENDO, MRED_RESPUESTA, MRED_BAJANDO, MRED_ENTERA, MRED_FALLO };   /* la fase de la descarga */
enum { MRED_EV_NADA, MRED_EV_TOCAR, MRED_EV_FALLO };   /* lo que sh_mured_paso le dice al servicio de música */

typedef struct {
    i16 actual, n;                       /* la pista de la cola que suena o se baja (-1 = ninguna) y cuántas hay */
    u8  fase, red, reintentos, tocando;  /* MRED_*; SH_RED_* de la red; reintentos seguidos de esta pista; 1 = ya se le dio PLAY */
    int http;                            /* el último estado HTTP (200, 206, 404…; 0 = aún no) */
    u32 recibidos, total;                /* bytes de ESTA pista ya en el colchón (desde el principio del fichero) y su tamaño (0 = no se sabe) */
    u32 colchon, dur_ms, desde_ms;       /* bytes en el colchón sin mandar al DSP; la duración (X-Duracion-Ms o de la cola); desde dónde se pidió */
    u32 esperas, ms_esperando;           /* pausas limpias por falta de datos (del motor) */
    char porque[MRED_PORQUE];            /* el último fallo, en palabras */
} MuRedInfo;

int  sh_mured_hay(void);                 /* 1 si este sistema puede: radio (la red) y el motor MP3 */
void sh_mured_vaciar(void);              /* la cola vacía (si sonaba una de la red, sigue hasta que el servicio la pare) */
int  sh_mured_poner(const char *url, const char *titulo, const char *artista, u32 dur_ms);   /* al final: el índice, o -1 (llena, url mala) */
int  sh_mured_n(void);
int  sh_mured_pista(int i, const char **url, const char **titulo, const char **artista, u32 *dur_ms);   /* 0 = existe */
/* lo que usa shell/musica.c */
int  sh_mured_empezar(int i, u32 desde_ms);   /* la pista i: abre el flujo y empieza a bajarla (lo de antes, fuera). HW_OK / HW_ERROR */
int  sh_mured_paso(void);                /* del bucle (sh_musica_paso): mueve la red al colchón; MRED_EV_* */
void sh_mured_play_falta(void);          /* el PLAY dio «faltan datos»: volver a avisar con más colchón */
void sh_mured_suena(void);               /* el PLAY fue bien (ya no hace falta avisar) */
int  sh_mured_tocando(void);             /* 1 si la actual ya suena (0 = aún cargando: el motor no es suyo todavía) */
void sh_mured_parar(void);               /* corta la descarga y suelta la red (y la radio, si era suya). El motor lo para musica.c */
int  sh_mured_actual(void);
void sh_mured_info(MuRedInfo *i);
void sh_mured_parar_reteniendo(void);    /* (1.10) para la descarga y conserva la red de la música (el micrófono la tiene un momento) */
#endif
