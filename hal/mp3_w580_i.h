/* Lo que comparten los ficheros del reproductor MP3 del W580i (hal/mp3_w580*.c): el lector de la cabecera del MP3, el APF y el códec. No es API: la API es
 * api/hw.h (hw_mp3_*) y hal/mp3_w580.h (lo que enseña la pantalla de diagnóstico). Todo lo de aquí es PORTABLE (sin registros): se prueba en el PC. */
#ifndef MP3_W580_I_H
#define MP3_W580_I_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;

/* ---- cabecera del MP3 (mp3_w580_cab.c) ---------------------------------------------------------------------------------------------- */
typedef struct {
    u32 inicio;                    /* desplazamiento del primer fotograma MPEG (tras un ID3v2) */
    u32 tam;                       /* bytes del fichero */
    u32 fs;                        /* Hz del primer fotograma */
    u32 kbps;                      /* tasa del primer fotograma (la media si hay Xing con bytes y fotogramas) */
    u32 dur_ms;                    /* duración: Xing/VBRI si hay; si no, tam/tasa */
    u32 fotogramas, bytes_audio;   /* de Xing/VBRI (0 si no) */
    u8  canales;                   /* 1 mono, 2 resto */
    u8  version;                   /* 1 = MPEG-1, 2 = MPEG-2, 25 = MPEG-2.5 */
    u8  vbr;                       /* 1 = Xing/VBRI con fotogramas */
    u8  hay_toc;                   /* 1 = Xing con tabla de contenidos (100 entradas) */
    u8  toc[100];
    u8  id3v2;                     /* 1 = había etiqueta ID3v2 al principio */
    u8  _r;
} Mp3Cab;
typedef int (*Mp3Leer)(void *ctx, u32 desde, u8 *buf, u32 n);   /* bytes leídos; < n = fin de fichero; < 0 = error */
/* 0 = bien; < 0: -1 argumentos, -2 no se pudo leer, -3 no hay un fotograma MPEG capa III válido */
int  mp3_cab_leer(Mp3Leer rd, void *ctx, u32 tam_fichero, Mp3Cab *c);
/* el desplazamiento de fichero para empezar a sonar en `ms` (alineado al siguiente fotograma si rd puede leer) */
u32  mp3_cab_pos_bytes(const Mp3Cab *c, u32 ms);
u32  mp3_cab_alinear(Mp3Leer rd, void *ctx, const Mp3Cab *c, u32 pos);
u32  mp3_cab_codigo_fs(u32 fs);                              /* 8000=1 … 48000=9; 0 si no es de la lista */

/* ---- inflate (zlib) y APF (mp3_w580_apf.c) -------------------------------------------------------------------------------------------- */
int  mp3_inflar(const u8 *z, u32 nz, u8 *out, u32 max);      /* zlib (78 xx) -> bytes; < 0 = error */
#define MP3_APF_TAM   13347u                                 /* un .apf entero */
#define MP3_APF_CUERPO 13339u
typedef struct { const char *nombre; const unsigned char *datos; unsigned tam; } Mp3ApfEmb;
extern const Mp3ApfEmb mp3_apf_emb[];                        /* mp3_w580_apfdat.c (generado) */
extern const unsigned mp3_apf_emb_n;
enum { MP3_APF_NADA = 0, MP3_APF_FS = 1, MP3_APF_EMBEBIDO = 2 };
/* Carga el APF `nombre` (sin extensión): primero del FS de fábrica (/ifs/settings/acoustic/NOMBRE.zapf), si no, el embebido. 0 = bien (origen en *origen). */
int  mp3_apf_cargar(const char *nombre, u32 (*leer_fs)(const char *ruta, u8 *buf, u32 max), int *origen);
const u8 *mp3_apf_cuerpo(void);                              /* los 13339 bytes del cuerpo, o NULL */
int  mp3_apf_poner(const u8 *apf, u32 n);                    /* para pruebas: un .apf ya inflado */
u32  mp3_apf_u8(unsigned off);                               /* un byte del cuerpo */
/* los campos del códec (ids 0x32..0x66) en el orden del original */
typedef struct { u8 reg, mascara, valor; u8 modo; } Mp3CodEsc;    /* modo 'W' inmediato, 'S' sombra del INSEL, 'F' bandera */
int  mp3_apf_codec(Mp3CodEsc *dst, int max);                 /* rellena las 53 escrituras (valor ya en su posición); devuelve cuántas */
/* los bloques que van al mezclador (R_6 / R_9) construidos como el original (comprobados contra farfield_closed_play) */
u32  mp3_apf_flag(unsigned id);                              /* bandera de clase 0 (id 0..17) */
int  mp3_apf_filtro(u16 *dst, u32 max);                      /* cuerpo del R_6 del filtro RX 44,1 kHz; devuelve las palabras */
int  mp3_apf_drc6(u16 *dst, u32 max);                        /* cuerpo del R_6 del DRC_RX */
int  mp3_apf_drc9(u16 *dst, u32 max);                        /* cuerpo del R_9 del DRC_RX */

/* ---- códec Vincenne del PMU: sombra C0..D0 y escalones (mp3_w580_apf.c) --------------------------------------------------------------- */
typedef struct { void (*pmu)(u8 reg, u8 valor); } Mp3CodHw;       /* escribe el registro físico del PMU (I2C 0x4A) */
void mp3_cod_iniciar(const Mp3CodHw *hw);                    /* sombra a 0 + la lista de inicio del original (C7|=40 CA=10 CC=50 C5[5:3]=3) */
void mp3_cod_rmw(u8 reg, u8 mascara, u8 valor);              /* sombra[reg] = (sombra & ~mascara) | (valor & mascara) y se escribe */
u8   mp3_cod_sombra(u8 reg);
void mp3_cod_aplicar_apf(void);                              /* los 53 campos del APF cargado, en orden (los INSEL quedan en sombra) */
void mp3_cod_escalon(int encendido, int rx);                 /* FUN_445777f8: C8, C0[6], C7, C0[2..5], y CD/CC/CB (los INSEL) */
void mp3_cod_subir(void);                                    /* vincenne(0x66,1): C0|=1, C0|=2, escalon(1,1) */
void mp3_cod_bajar_rx(void);                                 /* la parada: escalon(0,1) */
void mp3_cod_apagar(void);                                   /* el apagado del noveno segundo */
int  mp3_cod_final(u8 *regs16);                              /* C1..D0 (16) como quedan con el APF y el escalón arriba */
#endif
