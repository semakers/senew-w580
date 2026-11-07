/* EL REPRODUCTOR DE MP3 DEL W580i POR EL DSP (pista MP3, 2026-10-21). La API que usa el resto del sistema es hw_mp3_* (api/hw.h); esto es lo que además enseña la
 * pantalla de diagnóstico (apps/sistema/mp3_diag.c) y los códigos de fase y de error.
 *
 * QUÉ HACE (todo sale del ORIGINAL R8BE001; el protocolo, en tmp/dsp_emu/protocolo_mp3.md): el MP3 lo DECODIFICA EL DSP (módulo MP3, índice 4); el ARM le manda el
 * fichero en bruto por un anillo circular (flujo 0x14), el DSP saca PCM por la interfaz PCM hacia el códec Vincenne del PMU y de ahí al altavoz (P3.7 = elevador, P1.5 = AMP).
 * Los parámetros de audio son los del APF del FS de fábrica (/ifs/settings/acoustic/NOMBRE.zapf; en el árbol público no hay copia embebida). Secuencia de una primera reproducción (87 mensajes,
 * el mismo orden y contenido que el original; tmp/mp3/comparar_mp3.py lo comprueba):
 *   DSP (arranque con el módulo de arranque) -> APF -> códec (53 campos) -> mezclador F1 (R_n) + subida del escalón + GPIO + rampa de volumen -> PreBoot ->
 *   cambio de módulo (dsp_cargar MP3) -> 0x1101 + versión -> PostBoot (R_n otra vez) -> versión -> proceso decodificador (0x1208/0x120A) -> INIT 0x0E06 -> R_2 -> CONFIG 0x0E00 ->
 *   umbral 0x0404 -> datos 0x0804 (anillo lleno) -> PLAY 0x0E02{0}.
 * El DSP NO se arranca nunca hasta que se pide sonar; con cable USB puesto se NIEGA (shell/usb.c; tmp/audio_ag/informe2.md §3). Todo con tope de tiempo y de vueltas; ante cualquier
 * fallo se apaga ordenadamente (proceso, códec, amplificador, DSP) y queda el motivo. */
#ifndef MP3_W580_H
#define MP3_W580_H
#include "dsp_w580.h"

/* ---- fases: dónde está (o dónde se quedó) la máquina de estados ------------------------------------------------------------------------ */
enum { MP3_F_NADA = 0,
       MP3_F_ABRIR,        /*  1 fichero y cabecera del MP3 (ID3v2, primer fotograma, Xing) */
       MP3_F_DSP,          /*  2 arranque del DSP con el módulo de arranque (hal/dsp_w580_arr.c) + 0x1101 */
       MP3_F_APF,          /*  3 carga del APF (FS de fábrica o embebido) */
       MP3_F_CODEC,        /*  4 los 53 campos del códec por I2C + escalón apagado */
       MP3_F_LINEA,        /*  5 mezclador F1 (R_n con el módulo vigente), escalón arriba, GPIO, rampa de volumen */
       MP3_F_PREBOOT,      /*  6 PreBoot: reinicio del mezclador y reloj del PCM */
       MP3_F_MODULO,       /*  7 cambio de módulo: ventana + XRAM del módulo MP3 */
       MP3_F_VERSION,      /*  8 0x1101 y 0x0201 -> 0x0202 */
       MP3_F_POSTBOOT,     /*  9 PostBoot: el mezclador otra vez (R_0, R_10, R_4, 0x0800, R_6, R_9…) */
       MP3_F_VERSION2,     /* 10 la segunda 0x0201 */
       MP3_F_PROCESO,      /* 11 gestor de recursos: 0x1208 -> 0x1209 + 0x0304 (anillo) */
       MP3_F_ARRANCA,      /* 12 0x120A -> 0x120B */
       MP3_F_INIT,         /* 13 0x0E06 -> 0x0E07 */
       MP3_F_R2,           /* 14 R_2 */
       MP3_F_CONFIG,       /* 15 0x0E00 -> 0x0E01 (frecuencia y canales de ESTE mp3) */
       MP3_F_UMBRAL,       /* 16 0x0404 */
       MP3_F_DATOS,        /* 17 el anillo se llena con 0x0804 */
       MP3_F_PLAY,         /* 18 0x0E02{0} -> 0x0E03 */
       MP3_F_SONANDO,      /* 19 suena: créditos y datos */
       MP3_F_PAUSA,        /* 20 rampa de bajada + 0x0E02{2} */
       MP3_F_BUSCA,        /* 21 búsqueda (0x0E02{1}, vaciar, CONFIG, umbral, datos, PLAY) */
       MP3_F_FIN,          /* 22 fin de pista (0x0E04) o parada: vaciar, liberar, matar el proceso */
       MP3_F_CIERRE,       /* 23 mezclador a cero, amplificador, escalón abajo */
       MP3_F_APAGADO,      /* 24 códec apagado, reloj del PCM, DSP en reset */
       MP3_F_N };

/* ---- errores (Mp3Estado.error; el PRIMERO que se produjo) ------------------------------------------------------------------------------ */
enum { MP3_E_OK = 0,
       MP3_E_PARAM = -1, MP3_E_FICHERO = -2,  MP3_E_NO_MP3 = -3,    MP3_E_USB = -4,        MP3_E_APF = -5,
       MP3_E_DSP = -6,                         /* dsp_arrancar/dsp_cargar fallaron: el código de A en `sub` */
       MP3_E_MEZCLADOR = -7,                   /* un R_n sin respuesta (dos seguidos: se aborta) */
       MP3_E_SALIDA = -8,                      /* 0x0800 sin 0x0801 */
       MP3_E_VERSION = -9, MP3_E_PROCESO = -10, MP3_E_ARRANCA = -11, MP3_E_INIT = -12, MP3_E_CONFIG = -13, MP3_E_ACCION = -14,
       MP3_E_ANILLO = -15,                     /* no llegó el 0x0304 del anillo / no entra ni un trozo */
       MP3_E_ATASCO = -16,                     /* sonando y sin créditos durante 6 s */
       MP3_E_DSP_PERDIDO = -17,                /* el DSP se reinició o se apagó por debajo */
       MP3_E_LECTURA = -18, MP3_E_NO_ABIERTO = -19, MP3_E_OCUPADO = -20, MP3_E_FORMATO = -21, MP3_E_CABLE = -22,
       MP3_E_ENVIO = -23,                      /* dsp_enviar falló (el código de A en `sub`) */
       MP3_E_FALTAN = -24,                     /* (flujo) aún no hay datos bastantes en el colchón para leer la cabecera: reintentar luego */
       MP3_E_RITMO = -25,                      /* sonando, el DSP pide datos a más de 1,1 × (1,6 × si es VBR) el ritmo del MP3 (tras un vaciado: suena acelerado) */
       MP3_E_N = 26 };

#define MP3_LOG_N     28          /* anillo de mensajes COMPLETOS (los últimos) */
#define MP3_LOG_PAL   26          /* palabras guardadas por mensaje */
#define MP3_RN_N      20          /* una respuesta de cada R_n: 0..17 y 91 (índice 18) */
typedef struct { u8 rx; u8 clase; u16 canal; u16 n; u16 w[MP3_LOG_PAL]; u32 ms; } Mp3LogMsg;     /* rx 0 = ARM->DSP, 1 = DSP->ARM; n = palabras reales */
typedef struct { u8 hay; u8 n_resp; u8 rn; u8 _r; u16 n; u16 w[MP3_LOG_PAL]; } Mp3Rn;           /* la PRIMERA respuesta del mezclador a cada R_n */

typedef struct {
    u8  estado;                    /* HW_MP3_* */
    u8  fase;                      /* MP3_F_*: la que se está haciendo o la última */
    signed char error, sub;        /* MP3_E_* y el código de dsp_* que lo causó (0 = ninguno) */
    u8  volumen, ruta;             /* nivel 0..15; ruta de salida (MP3_RUTA_*) */
    u8  apf_origen;                /* 0 ninguno, 1 FS de fábrica, 2 embebido */
    u8  cable;                     /* 1 = había cable USB */
    u8  canales, vbr;
    u8  rpc_seguidos_mal;          /* R_n sin respuesta seguidos */
    u8  rpc_mal_n;                 /* el último R_n sin respuesta */
    u8  linea;                     /* 0 chip apagado, 1 chip alimentado (línea baja), 2 línea arriba */
    u8  dsp_modulo;                /* 0 ninguno, 1 arranque, 2 MP3 */
    char apf_nombre[28];
    char ruta_fichero[96];
    u32 fs, kbps, dur_ms, pos_ms, tam;                    /* del MP3 */
    u32 ms_fase[MP3_F_N];          /* lo que tardó cada fase (la última vez) */
    u32 ms_total;                  /* del play */
    u32 rpc_ok, rpc_mal, rpc_res_no0;                     /* R_n contestados / sin respuesta / con resultado distinto de 0 */
    u16 proceso_canal, flujo, cap;                        /* canal del decodificador, id del flujo y capacidad del anillo (palabras) anunciada por 0x0304 */
    int espacio;                   /* palabras libres del anillo según nuestra cuenta */
    u32 pal_enviadas, trozos;      /* palabras mandadas al flujo (cabecera incluida) y trozos de 250 palabras */
    u32 cred_pal, cred_msgs, cred_pedidos;                /* créditos del DSP (palabras y mensajes) y veces que se quiso mandar y no cabía */
    u32 bytes_enviados;            /* del fichero (sin la cabecera del registro) */
    u32 t_ult_credito;             /* hw_ms del último crédito */
    u32 eos_enviado, eos_recibido; /* registro de fin mandado / 0x0E04 recibido */
    u32 n_msgs_tx, n_msgs_rx;
    u32 nodata;                    /* 0x0E04 {1}: el DSP se quedó sin datos (hueco en el sonido) */
    u16 c1_5[16];                  /* C1..D0 releídos del PMU (diagnóstico) */
    u8  c1_5_ok;
    u8  p15, p37;                  /* niveles de P1.5 y P3.7 releídos */
    u16 dsp_regs_s12, dsp_regs_s18;                       /* SYSCON +12 y +18 */
    u16 version_ok;                /* el DSP contestó la versión */
    char version[40];
} Mp3Diag;

/* Relee todo y rellena `d`. Cualquier campo se puede mirar sin parar nada. */
void mp3_diag(Mp3Diag *d);
int  mp3_diag_log(Mp3LogMsg *dst, int max);                /* los más recientes primero */
int  mp3_diag_rn(Mp3Rn *dst, int max);                     /* las R_n vistas (orden de n) */
const char *mp3_fase_nombre(int fase);
const char *mp3_error_nombre(int err);
void mp3_progreso(void (*f)(void));                        /* el gancho de repintado de la pantalla (cada fase y cada ≤100 ms en las largas) */
/* rutas de salida (qué APF): 0 = automática (altavoz; corredera cerrada/abierta) */
enum { MP3_RUTA_AUTO = 0, MP3_RUTA_ALTAVOZ_CERRADO, MP3_RUTA_ALTAVOZ_ABIERTO, MP3_RUTA_MANOS_LIBRES, MP3_RUTA_LINEOUT, MP3_RUTA_BT_ESTEREO, MP3_RUTAS };
void mp3_ruta(int ruta);
const char *mp3_ruta_apf(int ruta);                        /* nombre del APF (sin extensión) de esa ruta para MÚSICA */
int  mp3_ruta_actual(void);
#endif
