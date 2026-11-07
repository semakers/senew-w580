/* mezcla.h — las 16 voces, la mezcla y el lector de MIDI. Pista S5 (código de S4, docs/audio.md §5.1).
 *
 * Esto es SOFTWARE PURO: no toca un solo registro. Venía dentro de `audio/hal/hal_audio.c` (capas 4 y 5),
 * y al integrarlo en el sistema se ha movido aquí, a `api/`, por dos razones:
 *   - se compila también para el PC, así que la mezcla y el lector de SMF se prueban con guiones,
 *     con ASan y con UBSan (el lector de MIDI es un parser de un fichero de la flash: justo lo que hay
 *     que pasar por un sanitizador);
 *   - la HAL (`hal/hal_audio.c`) se queda solo con lo que habla con el DSPIF y con el zumbador.
 * El código es el mismo que midió S4 (la cuenta de note-on del `Sony Ericsson.mid` tiene que seguir
 * dando 128: lo comprueba `herr/pruebas/prueba_midi.c`).
 *
 * Quién lo usa: la app de Música y el servicio de sonido del shell. La salida se entrega por
 * `hw_pcm_escribir` (api/hw.h), que en el teléfono es el canal de flujo del DSP y en el PC una cuenta.
 */
#ifndef MEZCLA_H
#define MEZCLA_H
#include "hw.h"

typedef enum { VOZ_NADA = 0, VOZ_CUADRADA, VOZ_SIERRA, VOZ_TRIANGULO, VOZ_SENO, VOZ_MUESTRA } voz_tipo;

#define MZ_VOCES 32                    /* eran 16 (S4). El «MusicDJ Demo.mid» de fábrica pide 17 a la vez sin contar la percusión */
#define MZ_MAX_MARCOS 256              /* tope de un bloque de mezcla (mz_servir) */

typedef struct {
    voz_tipo tipo;
    u32 f_mhz;            /* frecuencia en milihercios (440000 = 440 Hz) */
    u16 vol;              /* 0..256 */
    u32 fase;             /* 16.16, interno */
    u32 paso;             /* interno: lo que avanza la fase por muestra. Se calcula UNA vez al poner la voz (S7):
                           * antes era una división de 64 bits por voz y por muestra, que un ARM926 no tiene */
    u16 env;              /* interno: envolvente 0..4096 (S7). Sube en ~5 ms al empezar y baja en ~30 ms al soltar:
                           * sin ella cada nota empieza y acaba con un chasquido (un seno cortado en seco) */
    u8  soltando;         /* interno: la nota ya se ha soltado y solo le queda la cola */
    const short *muestra; /* VOZ_MUESTRA: PCM de 16 bits a la misma frecuencia que la salida */
    u32 n, pos;
    u8  bucle;
    signed char pan;      /* -128 izquierda, 0 CENTRO, +127 derecha (solo en estéreo).
                           * Con signo a propósito: una voz puesta a ceros queda centrada y se puede
                           * pedir el extremo izquierdo, que con 0..255 no se podía (lo cazó el hito A3). */
} MzVoz;

void mz_formato(u32 rate, int canales);           /* el de la salida; hay que llamarlo antes de mezclar */
u32  mz_rate(void);
int  mz_canales(void);
int  mz_voz_poner(unsigned i, const MzVoz *v);    /* copia; arranca la fase a 0 */
void mz_voz_nota(unsigned i, voz_tipo t, u32 f_mhz, u16 vol);
void mz_voz_quitar(unsigned i);                   /* en seco */
void mz_voz_soltar(unsigned i);                   /* con su cola (~30 ms); después queda libre sola */
int  mz_voz_libre(unsigned i);                    /* 1 = no suena nada en ella */
void mz_voz_en_fase(unsigned i, unsigned con);    /* la voz i toma la fase de la voz `con` (unísonos: que sumen, no que se resten) */
void mz_todas_fuera(void);
unsigned mz_voces_activas(void);
void mz_volumen(int pct);                         /* atenuación de la MEZCLA, 0..100 (el volumen de usuario) */
unsigned mz_mezclar(short *dst, unsigned marcos); /* mezcla las voces activas; devuelve los marcos */
int  mz_servir(short *buf, unsigned marcos);      /* mezcla y entrega por hw_pcm_escribir */

/* ---- un fichero MIDI (SMF formato 0 y 1) ----------------------------------------------------------
 * Reparte las notas entre las voces. NO es un sintetizador General MIDI: cada nota suena como una onda
 * del catálogo de arriba (el W600 de fábrica toca los .mid DENTRO del DSP, con su banco de
 * instrumentos, que no tenemos: docs/audio.md §5.2). */
#define MZ_PISTAS 24        /* eran 8: el «MusicDJ Demo.mid» de fábrica trae 9 y se quedaba sin la última */
typedef struct {
    const u8 *d; u32 n;
    u32 division;          /* pulsos por negra */
    u32 us_por_negra;      /* del meta 0x51; por defecto 500000 */
    unsigned pistas;
    struct { u32 pos, fin; u32 t; u8 viva, ult; } p[MZ_PISTAS];
    u32 t;                 /* tiempo en pulsos ya consumido */
    unsigned long long cred;   /* tiempo dado y todavía sin gastar, en µs × división (lo que no llega a un pulso NO se pierde) */
    u32 notas, errores;
    u32 percusion;         /* notas del canal 10, que NO se tocan (no hay banco de percusión: un seno a la «altura»
                            * del número de nota sería un pitido falso). Se cuentan para poder decirlo */
    u32 perdidas;          /* notas que no cupieron en las MZ_VOCES voces */
    u8  mudo;              /* 1 = lleva la cuenta de qué notas suenan (voz_de/can_de) pero NO toca las voces de la
                            * mezcla: es lo que usa el camino del zumbador, y medir la duración */
    u8  voz_de[MZ_VOCES];  /* nota MIDI que ocupa cada voz, 0xFF = libre */
    u8  can_de[MZ_VOCES];
    u8  vel_de[MZ_VOCES];  /* su velocidad (para retomarla tras un salto) */
    u32 pulso_de[MZ_VOCES];   /* el pulso en que se atacó (para la línea del zumbador) */
    u32 off_de[128];          /* por altura: el pulso en que se soltó por última vez (ídem) */
} MzMidi;

int  mz_midi_abrir(MzMidi *m, const u8 *datos, u32 n);
/* Avanza `us` microsegundos de música: aplica, cada uno en SU pulso, los eventos que caigan dentro (un
 * cambio de tempo vale desde su pulso). El tiempo que no llega a un pulso se guarda para la vez siguiente:
 * llamarla cada 4 ms o cada 4 s da la misma música. 0 = se acabó (no quedan pistas vivas). */
int  mz_midi_avanzar_us(MzMidi *m, u32 us);
/* La de S4, por marcos de audio; ahora es mz_midi_avanzar_us(marcos × 1e6 / rate) y ya no pierde tempo
 * (antes truncaba los pulsos de cada bloque: con bloques de un tic iba un 22 % lenta). */
int  mz_midi_avanzar(MzMidi *m, unsigned marcos, unsigned rate);
/* Recorre el fichero entero SIN sonar: duración en ms (hasta el último evento), notas tocables y de
 * percusión. Devuelve lo que mz_midi_abrir (pistas, o negativo). */
int  mz_midi_medir(const u8 *datos, u32 n, u32 *dur_ms, u32 *notas, u32 *percusion);
/* La nota más aguda que suena ahora, o -1 si no suena ninguna. */
int  mz_midi_aguda(const MzMidi *m);
/* LA LÍNEA MELÓDICA: lo que da el zumbador, que solo tiene una voz. La regla (la misma, escrita aparte, está en
 * herr/midi_ref.py, que es contra lo que se coteja lo que suena):
 *   1. suena la nota MÁS AGUDA de las que están sonando (sin la percusión, que nunca entra);
 *   2. pero cuando la nota de la línea se acaba, la más aguda que queda solo entra enseguida si se ha atacado en
 *      ese momento o después; si YA ESTABA sonando por debajo (un bajo, un acorde tenido, un arpegio), espera
 *      MZ_LINEA_ESPERA_MS de silencio. Sin esto, en cada respiro entre dos notas de la melodía asoma lo de debajo
 *      unas decenas de ms (un gorgorito), o 4 ms justos entre dos notas pegadas (un chasquido).
 * La decisión va por PULSOS del fichero, no por el reloj: da lo mismo con el tic de 4 ms que de 1 en 1 ms.
 * `dt_ms` = lo que ha pasado desde la llamada anterior. Devuelve la nota MIDI de la línea ahora, o -1 (silencio). */
#define MZ_LINEA_ESPERA_MS 80
typedef struct { int nota, ult; u32 espera; u8 hubo; } MzLinea;            /* a ceros para empezar */
int  mz_midi_linea(const MzMidi *m, MzLinea *l, u32 dt_ms);
/* Tras un salto en modo mudo: enciende en la mezcla las notas que estaban sonando en ese punto. */
void mz_midi_retomar(MzMidi *m);
u32  mz_nota_mhz(int nota);                        /* nota MIDI 0..127 -> milihercios (la 69 = 440 000) */
#endif
