/* El servicio de SONIDO del sistema. Pista S5.
 *
 * Qué es: un secuenciador de melodías del ZUMBADOR (el generador de 0xF9020000 por el pin P0.7) que **no
 * bloquea**. La HAL de la pista S4 trae `tono_melodia()`, pero esa se queda girando en un bucle sobre el
 * tic: si la alarma suena 2 segundos, el teléfono está 2 segundos sin pintar, sin leer teclas y sin
 * atender nada. Aquí la melodía avanza una nota por vuelta del bucle (`sh_sonido_paso()`), como la
 * máquina de estados de la energía.
 *
 * Es PORTABLE (solo usa hw_tono / hw_tono_fin / hw_ms), así que se prueba con guiones en el PC —que
 * además escribe el WAV con el mismo modelo del emulador— y se MIDE en el emulador.
 *
 * El volumen. El generador no tiene un registro de volumen que sepamos usar: los bits 5:4 de su registro
 * de control son [inferido] una atenuación y nadie los ha medido (docs/audio.md §6.1, pregunta 2). Lo que
 * sí cambia la energía que sale del pin, y SE MIDE EN EL WAV, es el ANCHO DEL PULSO: una cuadrada al 50 %
 * entrega el doble de energía que un pulso del 25 %. Así que «volumen» aquí = ancho del pulso, y la tabla
 * de abajo es la que hay que comparar con el «ciclo %» que mide audio/herr/onda.py.
 * [sin saber] cómo de bien se corresponde eso con el volumen PERCIBIDO en un teléfono (un pulso estrecho
 * también cambia el timbre: mete armónicos). Lo dice la pantalla de Ajustes.
 *
 * Prioridades: una alarma no la puede cortar el clic de una tecla. Cada sonido trae la suya y uno nuevo
 * solo entra si la iguala o la supera.
 */
#include "shell.h"

typedef struct { u16 hz, ms; } SnNota;             /* hz = 0 es un silencio */

/* Las melodías. Todas cortas a propósito: esto es un zumbador de un teléfono de 2005, no un sintetizador.
 * Las notas son las temperadas de la tabla de S4 (la4 = 440): do5 523, mi5 659, sol5 784, la5 880, do6 1047. */
static const SnNota m_tecla[]      = { { 2000, 12 } };
static const SnNota m_alarma[]     = { { 880, 150 }, { 0, 60 }, { 1175, 150 }, { 0, 60 }, { 880, 150 }, { 0, 60 }, { 1175, 300 } };
static const SnNota m_temporiz[]   = { { 1047, 120 }, { 0, 80 }, { 1047, 120 }, { 0, 80 }, { 1047, 320 } };
static const SnNota m_bateria[]    = { { 784, 180 }, { 0, 70 }, { 523, 400 } };
static const SnNota m_bloqueo[]    = { { 1568, 40 }, { 0, 40 }, { 1047, 70 } };
static const SnNota m_desbloqueo[] = { { 1047, 40 }, { 0, 40 }, { 1568, 70 } };
static const SnNota m_error[]      = { { 440, 90 }, { 0, 50 }, { 415, 180 } };
static const SnNota m_arranque[]   = { { 523, 90 }, { 659, 90 }, { 784, 90 }, { 1047, 220 } };
static const SnNota m_apagado[]    = { { 1047, 90 }, { 784, 90 }, { 523, 220 } };
static const SnNota m_prueba[]     = { { 440, 1000 } };    /* el la4 de 440,0 Hz que midió S4: la referencia */

#define MEL(v) v, (u16)(sizeof v / sizeof v[0])
static const struct { const SnNota *n; u16 cuantas; u8 prio, repite; u16 pausa; } cat[SN_N] = {
    [SN_TECLA]       = { MEL(m_tecla),      0, 0,    0 },
    [SN_BLOQUEO]     = { MEL(m_bloqueo),    1, 0,    0 },
    [SN_DESBLOQUEO]  = { MEL(m_desbloqueo), 1, 0,    0 },
    [SN_ERROR]       = { MEL(m_error),      1, 0,    0 },
    [SN_ARRANQUE]    = { MEL(m_arranque),   1, 0,    0 },
    [SN_APAGADO]     = { MEL(m_apagado),    1, 0,    0 },
    [SN_PRUEBA]      = { MEL(m_prueba),     2, 0,    0 },
    [SN_BATERIA]     = { MEL(m_bateria),    2, 1, 1000 },
    [SN_TEMPORIZADOR]= { MEL(m_temporiz),   3, 9, 1200 },
    [SN_ALARMA]      = { MEL(m_alarma),     3, 29, 1500 },   /* ~30 repeticiones = ~60 s, o hasta que alguien pulse algo */
};

/* El ancho del pulso por nivel de volumen (0 = silencio). 100 = onda cuadrada (50 % arriba), que es el
 * máximo de energía; los de abajo son pulsos cada vez más estrechos. Lo que mide onda.py es la mitad:
 * ciclo 100 -> 50,0 %, ciclo 50 -> 25 %, ciclo 26 -> 13 %, ciclo 14 -> 7 %, ciclo 6 -> 3 %. */

static const u8 ancho_de_vol[SN_VOL_MAX + 1] = { 0, 6, 14, 26, 50, 100 };

static struct {
    const SnNota *n;
    u16 cuantas, i;
    u8  prio, cual, repite;
    u16 pausa;
    u32 hasta;                                     /* hw_ms() en que acaba la nota en curso */
} s;

int sh_sonando(void) { return s.n ? s.cual : 0; }

void sh_callar(void)
{
    int era;
    if (!s.n) return;
    era = s.cual;
    hw_tono_fin();                                         /* PRIMERO se calla y DESPUÉS se suelta (S7): la música mira `s.n` desde la
                                                            * interrupción del tic para saber si el zumbador es suyo; al revés, podía
                                                            * poner su nota entre las dos líneas y este hw_tono_fin se la apagaba */
    s.n = NULL; s.cual = 0; s.prio = 0; s.repite = 0;
    if (era >= SN_BATERIA) sh_musica_aviso(0);             /* el aviso ha callado: la música que se apartó, sigue */
}

/* Arranca la nota `i` de la melodía en marcha. Devuelve 0 si ya no hay nota. */
static int nota_actual(void)
{
    u32 hz;
    if (s.i >= s.cuantas) return 0;
    hz = s.n[s.i].hz;
    /* Cada nota lleva SU plazo (hw_tono_ms): la apaga el tic de la plataforma, no este secuenciador, así que
     * dura lo pedido aunque el bucle tarde en volver (pintar una pantalla entera son decenas de ms). Aquí solo
     * se decide cuándo empieza la siguiente. [medido en el WAV del emulador: evidencia/w600_sonido/] */
    if (!hz) hw_tono_fin();
    else if (hw_tono_ms(hz, ancho_de_vol[aj.volumen > SN_VOL_MAX ? SN_VOL_MAX : aj.volumen], s.n[s.i].ms) != HW_OK) hw_tono_fin();
    s.hasta = hw_ms() + s.n[s.i].ms;
    return 1;
}

void sh_sonar(int cual) { sh_sonar_n(cual, 0); }

/* `vueltas` > 0: la melodía se da ese número de veces y calla sola, diga lo que diga el catálogo (dentro de
 * una app externa nadie va a «pulsar una tecla para callarla»: las teclas son del juego). 0 = lo del catálogo. */
void sh_sonar_n(int cual, int vueltas)
{
    if (cual <= 0 || cual >= SN_N || !cat[cual].n) return;
    if (!aj.volumen) return;                               /* silencio: ni se empieza */
    if (cual == SN_TECLA && !aj.tono_teclas) return;
    if (cual == SN_TECLA && sh_musica_en_zumbador()) return;   /* la música va por el mismo pin: un clic la trocearía (S7) */
    if (s.n && cat[cual].prio < s.prio) return;             /* lo que suena manda (una alarma no la corta un clic) */
    s.n = cat[cual].n; s.cuantas = cat[cual].cuantas; s.i = 0;
    s.prio = cat[cual].prio; s.cual = (u8)cual;
    s.repite = vueltas > 0 ? (u8)(vueltas - 1) : cat[cual].repite; s.pausa = cat[cual].pausa;
    if (!s.pausa) s.pausa = 1000;
    if (cual >= SN_BATERIA) sh_musica_aviso(1);             /* un aviso que pide atención: la música por el zumbador se aparta */
    nota_actual();
}

/* Una vez por vuelta del bucle de la plataforma, como sh_energia_paso(). */
void sh_sonido_paso(void)
{
    if (!s.n) return;
    if ((i32)(hw_ms() - s.hasta) < 0) return;              /* la nota todavía suena */
    s.i++;
    if (nota_actual()) return;
    if (s.repite) {                                        /* otra vuelta, tras una pausa de silencio */
        s.repite--;
        hw_tono_fin();
        s.hasta = hw_ms() + s.pausa;
        s.i = (u16)-1;                                     /* el s.i++ de la vuelta siguiente la pone en 0 */
        return;
    }
    sh_callar();
}

/* Cuánto falta para que haya que volver a mirar (para que el reposo profundo no se duerma encima de una
 * melodía). 0 = no hay nada sonando. */
u32 sh_sonido_proximo_ms(void)
{
    i32 d;
    if (!s.n) return 0;
    d = (i32)(s.hasta - hw_ms());
    return d <= 0 ? 1 : (u32)d;
}

void sh_sonido_volumen(int v)
{
    aj.volumen = (u8)(v < 0 ? 0 : v > SN_VOL_MAX ? SN_VOL_MAX : v);
    if (!aj.volumen) sh_callar();
    sh_guardar_ajustes();
}
int sh_sonido_ancho(void) { return ancho_de_vol[aj.volumen > SN_VOL_MAX ? SN_VOL_MAX : aj.volumen]; }
