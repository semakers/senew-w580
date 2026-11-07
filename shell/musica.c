/* El MOTOR DE MÚSICA del sistema (pista S7). El W600 es un Walkman.
 *
 * Vive en el SHELL y no en la app (apps/musica/ es solo la pantalla) por una razón: la música tiene que
 * seguir sonando cuando se sale al menú, y lo que es de una pantalla muere con ella.
 *
 * Qué toca: ficheros MIDI (SMF 0 y 1) con el lector de api/mezcla.c. Los MP3, AAC y AMR del teléfono los
 * decodifica el DSP con un código que no tenemos (docs/audio.md §6.3): se listan y se dice por qué no suenan.
 *
 * DOS SALIDAS, y la app enseña cuál está usando:
 *
 *   MU_SAL_PCM       polifonía: hasta 32 voces de seno mezcladas en el ARM y entregadas como PCM de 16 kHz
 *                    mono al puerto del DSP (hw_pcm_*). Solo si hw_pcm_init() contesta. En el emulador eso
 *                    pide «dsp-sin-nucleo=on»; en un teléfono NO va a contestar hasta que alguien cargue el
 *                    código del DSP. La mezcla se hace en el BUCLE, por tiempo: en cada vuelta se entregan
 *                    los marcos que tocan según hw_ms() (16 por milisegundo), en bloques de 64 = un tic.
 *   MU_SAL_ZUMBADOR  una voz: la LÍNEA MELÓDICA (mz_midi_linea: la nota más aguda, sin percusión, con la
 *                    regla de la espera) por el generador de tonos (hw_tono_*), subida de octava si baja de
 *                    397 Hz, que es lo mínimo que el generador acepta. Es el camino con menos incógnitas y
 *                    el que va a sonar en un teléfono si el pin llega a algo (docs/audio.md §6.1).
 *                    La secuencia cuelga del TIC de 4 ms (hw_tic_gancho), no del bucle: lo que cuelga del
 *                    bucle baila lo que tarde en pintarse una pantalla (S6 midió clics de 12 ms que duraban
 *                    de 9 a 61). Donde no hay gancho (el PC) se sondea desde el bucle, que allí va a 4 ms.
 *
 * Con «automático» se prueba el PCM UNA vez (hw_pcm_init puede tardar 0,5 s si el DSP no contesta) y, si no
 * hay, se queda en el zumbador sin volver a preguntar hasta que se pida. Si el PCM falla a media pista, se
 * sigue por el zumbador desde el mismo punto.
 *
 * CONVIVENCIA con los avisos del sistema (shell/sonido.c), que van SIEMPRE por el zumbador:
 *   - por PCM no se pisan: son dos caminos. La alarma suena encima y la música sigue;
 *   - por el zumbador solo hay un pin: los avisos que piden atención (alarma, cuenta atrás, batería) ponen
 *     la música EN PAUSA y al callarlos sigue donde estaba; los sonidos cortos (bloqueo, error) se llevan el
 *     zumbador lo que duran y la música lo recupera sola (hw_tono_mantener); y el clic de las teclas no
 *     suena mientras hay música por el zumbador (la trocearía).
 *
 * Qué se guarda, y dónde: un fichero «musica.cfg» en la ZONA DE USUARIO (hw_usr_*, docs/sistema.md §13) con
 * el volumen, el modo de sonido, la repetición, la última pista (por su RUTA) y la posición. Es la primera
 * app de casa que usa esa zona. Se escribe 2 s después del último cambio (pausa, parada, cambio de pista,
 * volumen…), al empezar cada pista (antes de que suene) y al apagar; NUNCA mientras suena: escribir en la NOR
 * corta las interrupciones y la música tropieza (medido), y además la posición cambia sin parar.
 *
 * El gancho corre DENTRO DE LA INTERRUPCIÓN del tic. La exclusión con el bucle es un cerrojo de un byte: el
 * bucle lo pone antes de tocar el estado y el gancho, si lo ve puesto, no entra (el tiempo no se pierde: va
 * por hw_ms() y el tic siguiente lo recupera).
 */
#include "shell.h"
#include "mezcla.h"
#include "musica_casa.h"
#include "musica_red.h"                 /* (1.8) la música por la red: una cola de URLs (sigue con la app cerrada) */
#include "bt_sis.h"                    /* (1.10) SH_BT_MUSICA: soltar la red retenida al ceder el DSP */
#include "red_sis.h"

void *memset(void *d, int c, __SIZE_TYPE__ n);
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
int   memcmp(const void *a, const void *b, __SIZE_TYPE__ n);

/* (MP3, 2026-10-21) El MP3 lo decodifica el DSP del W580i (hal/mp3_w580.c, api/hw.h «MP3 POR EL DSP»). En el resto de placas esas funciones no existen: son
 * débiles y mp3_hay() dice 0, así que el .mp3 sigue siendo «lo decodifica el DSP y no lo tenemos». */
int  hw_mp3_abrir(const char *ruta) __attribute__((weak));
int  hw_mp3_play(void) __attribute__((weak));
int  hw_mp3_pausa(void) __attribute__((weak));
int  hw_mp3_parar(void) __attribute__((weak));
int  hw_mp3_volumen(int nivel) __attribute__((weak));
int  hw_mp3_buscar(u32 ms) __attribute__((weak));
int  hw_mp3_estado(void) __attribute__((weak));
int  hw_mp3_activo(void) __attribute__((weak));
int  hw_mp3_cerrar_ya(void) __attribute__((weak));
int  hw_mp3_info(HwMp3Info *i) __attribute__((weak));
u32  hw_mp3_tiempo(u32 *dur_ms) __attribute__((weak));
int  hw_mp3_motivo(void) __attribute__((weak));
void hw_mp3_paso(void) __attribute__((weak));
static int mp3_hay(void) { return hw_mp3_abrir != 0 && hw_mp3_play != 0 && hw_mp3_paso != 0; }
/* (2026-10-25, música sólida) la VEDA DE LA FLASH (plat/hw_w580.c): mientras suena, nada se escribe en la NOR (lo que se pida guardar espera en RAM
 * y se escribe al callar). Débil: el PC no la tiene. */
void hwo_flash_veda(int si) __attribute__((weak));
/* el volumen del Walkman (0..5) en los 16 niveles de 3 dB del DSP (7 = el de fábrica, ganancia 0x0200 = −30 dB: la unidad del mezclador es 0x4000).
 * (2026-10-26, el mantenedor: «el 5 es muy bajito, que suene por lo menos 3 veces más fuerte») antes { 0, 2, 4, 6, 7, 9 }: el 5 se quedaba 18 dB por debajo de
 * lo que da el DSP. Medido con el micrófono del Mac junto al altavoz (herr/puente/dsp/exp_vol_alto.py, tono de 1 kHz a −3 dBFS; evidencia/musica/volumen):
 * 3 dB exactos por nivel hasta el 15; el 13 suena ×3,9 que el 9 con un 5,5 % de distorsión (solo el 2.º armónico); el 15, ×7,3 pero con el 3.º y el 5.º
 * (11 %: el altavoz empieza a romper). Ahora pasos iguales de 6 dB y el 5 = el 13. */
static const u8 vol_mp3[6] = { 0, 5, 7, 9, 11, 13 };

#define MU_FICHERO   "musica.cfg"
#define MU_CFG_VER   1
#define MU_TAM_MAX   (256u * 1024u)      /* un MIDI mayor que esto no se carga */
#define MU_RATE      16000u
#define MU_BLOQUE    64u                 /* marcos por bloque de mezcla = 4 ms = un tic */
#define MU_VOL_MAX   5

/* El volumen 0..5 en cada salida. Zumbador: el ancho del pulso (lo mismo que shell/sonido.c, y lo que se mide
 * en el WAV). PCM: el porcentaje de la mezcla (mz_volumen). */
static const u8 vol_ancho[MU_VOL_MAX + 1] = { 0, 6, 14, 26, 50, 100 };
static const u8 vol_pcm[MU_VOL_MAX + 1]   = { 0, 10, 20, 35, 60, 100 };

typedef struct {                         /* lo que va al fichero de la zona de usuario; little-endian, sin relleno */
    u8  version, volumen, modo, repetir;
    u32 pos_ms;
    char ruta[MU_RUTA];
} MuCfg;

static MuPista lista[MU_MAX_PISTAS];
static int n_lista;

static struct {
    volatile u8 cerrojo;                 /* el bucle está tocando el estado: el gancho no entra */
    volatile u8 estado;                  /* MU_PARADA / MU_SONANDO / MU_PAUSA */
    volatile u8 salida;                  /* MU_SAL_* de la pista en curso */
    volatile u8 acabada;                 /* la pista ha llegado al final: el bucle decide qué viene */
    u8  iniciado, gancho, cedida, pantalla;
    u8  volumen, modo, repetir;
    u8  pcm_probado, pcm_hay, pcm_abierto, fin_midi;
    i16 pista;                           /* índice en la lista; -1 = ninguna */
    const u8 *datos; u32 n; u8 *copia;   /* el fichero cargado (copia = del montículo, hay que soltarla) */
    MzMidi mid;
    MzLinea lin;
    u32 dur_ms, notas, percusion, pistas_smf;
    volatile u32 pos_ms;
    volatile u32 t_ult;                  /* hw_ms() hasta donde ya se ha avanzado (zumbador) o entregado (PCM) */
    volatile u16 hz;                     /* lo que el zumbador debe estar dando ahora (0 = silencio) */
    u32 t_ini, pos_ini;                  /* para el registro: cuándo y dónde empezó este tramo */
    u32 pcm_marcos, cola;
    int pcm_motivo;                      /* por qué no hay PCM… o, tras un MP3 que no suena, el motivo de hw_mp3_motivo() */
    u8  sucio, guardado;                 /* guardado: 0 sin intentar, 1 bien, 2 no hay zona, 3 falló */
    u32 sucio_desde;
    MuCfg ult;                           /* lo último que se escribió (para no escribir lo mismo) */
    u8  red, red_fallos;                 /* (1.8) lo que suena viene de la cola de la red (shell/musica_red.c; pista = -1); fallos seguidos */
    u32 red_desde;                       /* (1.8) el punto (ms) desde el que se pidió: el motor cuenta desde 0 */
} mu = { .pista = -1, .volumen = 4 };
static void veda_al_dia(void) { if (hwo_flash_veda) hwo_flash_veda(mu.estado == MU_SONANDO); }

/* ---- utilidades de cadenas (no hay libc) ----------------------------------------------------------------- */
static int minus(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int acaba_en(const char *s, const char *ext)
{
    u32 a = (u32)strlen(s), b = (u32)strlen(ext), i;
    if (a < b) return 0;
    for (i = 0; i < b; i++) if (minus(s[a - b + i]) != ext[i]) return 0;
    return 1;
}
static int tipo_de(const char *nombre)
{
    static const char *const dsp[] = { ".mp3", ".m4a", ".aac", ".amr", ".wav", ".3gp", ".mp4", ".mmf", ".imy" };
    unsigned i;
    if (acaba_en(nombre, ".mid") || acaba_en(nombre, ".midi")) return MU_T_MIDI;
    if (acaba_en(nombre, ".mp3")) return mp3_hay() ? MU_T_MP3 : MU_T_DSP;
    for (i = 0; i < sizeof dsp / sizeof dsp[0]; i++) if (acaba_en(nombre, dsp[i])) return MU_T_DSP;
    return 0;
}
static int tocable_t(int t) { return t == MU_T_MIDI || t == MU_T_MP3; }       /* los tipos que el motor sabe tocar */
static const char *base_de(const char *ruta)
{
    const char *b = ruta, *p;
    for (p = ruta; *p; p++) if (*p == '/' || *p == ':') b = p + 1;
    return b;
}
static void copiar(char *d, const char *s, u32 max)
{
    u32 i = 0;
    while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void num(u32 v) { char t[12]; int n = 11; t[11] = 0; do { t[--n] = (char)('0' + v % 10); v /= 10; } while (v); hw_log(t + n); }
static void registro(const char *que)                 /* una línea por la UART: es con lo que se MIDE el ritmo en el emulador */
{
    hw_log("musica: "); hw_log(que);
    if (mu.pista >= 0) { hw_log(" [" ); hw_log(base_de(lista[mu.pista].ruta)); hw_log("]"); }
    hw_log(mu.salida == MU_SAL_PCM ? " salida=pcm" : mu.salida == MU_SAL_ZUMBADOR ? " salida=zumbador" : " salida=-");
    hw_log(" pos_ms="); num(mu.pos_ms); hw_log(" dur_ms="); num(mu.dur_ms);
    hw_log(" reloj_ms="); num(hw_ms()); hw_log(" marcos="); num(mu.pcm_marcos); hw_log("\n");
}

/* ---- la lista de pistas ---------------------------------------------------------------------------------- */
static void anadir(const char *ruta, u32 tam, int tipo)
{
    int i;
    if (n_lista >= MU_MAX_PISTAS || strlen(ruta) >= MU_RUTA) return;
    for (i = 0; i < n_lista; i++)                         /* el mismo nombre ya está (p. ej. la de casa y la del FS) */
        if (!strcmp(base_de(lista[i].ruta), base_de(ruta))) return;
    copiar(lista[n_lista].ruta, ruta, MU_RUTA);
    lista[n_lista].tam = tam; lista[n_lista].tipo = (u8)tipo;
    n_lista++;
}
static void explorar(const char *dir, int nivel, int tipo)
{
    HwEntrada e[4];
    char ruta[MU_RUTA];
    u32 desde = 0, ld = (u32)strlen(dir);
    int n, i;
    if (hw_fs_contar(dir) <= 0) return;
    for (;;) {
        n = hw_fs_listar(dir, desde, e, 4);
        if (n <= 0) break;
        for (i = 0; i < n; i++) {
            if (ld + 1 + strlen(e[i].nombre) >= MU_RUTA) continue;
            copiar(ruta, dir, MU_RUTA); ruta[ld] = '/'; copiar(ruta + ld + 1, e[i].nombre, MU_RUTA - ld - 1);
            if (e[i].es_dir) { if (nivel < 2) explorar(ruta, nivel + 1, tipo); }
            else if (tipo_de(e[i].nombre) == tipo) anadir(ruta, e[i].tam, tipo);
        }
        desde += (u32)n;
        if (n < 4) break;
    }
}
/* (28-oct, el mantenedor: «si reproduzco una canción de las descargadas, cuando termine debe continuar reproduciendo las demás descargadas en
 * aleatorio») Las DESCARGADAS son los MP3 de /tarjeta/music (W5_DESC_CARPETA: ahí las deja la Música de la red). Al acabar una (o con
 * «siguiente»), la que viene es OTRA descargada al azar, sin repetir hasta que hayan sonado todas; entonces empieza otra vuelta (sin repetir
 * la que acaba de sonar). La vuelta se apunta por la RUTA (un hash), no por el índice: la lista se rehace (sh_musica_buscar) y los índices
 * cambian. Una pista pedida a mano (sh_musica_tocar_ruta) empieza una vuelta nueva. «Repetir una» sigue mandando. */
#define MU_DESC "/tarjeta/music/"
static struct { u32 h[MU_MAX_PISTAS]; u16 n; u32 azar; } vuelta;
static int es_descargada(int i) { return i >= 0 && i < n_lista && lista[i].tipo == MU_T_MP3 && !memcmp(lista[i].ruta, MU_DESC, sizeof MU_DESC - 1); }
static u32 hash_ruta(const char *s) { u32 h = 2166136261u; while (*s) { h ^= (u8)*s++; h *= 16777619u; } return h; }
static int ya_sono(int i) { u32 h = hash_ruta(lista[i].ruta); int k; for (k = 0; k < vuelta.n; k++) if (vuelta.h[k] == h) return 1; return 0; }
static void apuntar_vuelta(int i) { if (es_descargada(i) && !ya_sono(i) && vuelta.n < MU_MAX_PISTAS) vuelta.h[vuelta.n++] = hash_ruta(lista[i].ruta); }
static int descargada_al_azar(int actual)                 /* -1 = no hay otra descargada */
{
    int i, n = 0, k;
    apuntar_vuelta(actual);
    for (i = 0; i < n_lista; i++) if (i != actual && es_descargada(i) && !ya_sono(i)) n++;
    if (!n) {                                             /* han sonado todas: otra vuelta */
        vuelta.n = 0; apuntar_vuelta(actual);
        for (i = 0; i < n_lista; i++) if (i != actual && es_descargada(i)) n++;
        if (!n) return -1;                                /* era la única */
    }
    vuelta.azar = vuelta.azar * 1103515245u + 12345u + hw_ms();
    k = (int)((vuelta.azar >> 8) % (u32)n);
    for (i = 0; i < n_lista; i++) if (i != actual && es_descargada(i) && !ya_sono(i) && k-- == 0) break;
    if (i < n_lista) apuntar_vuelta(i);
    return i < n_lista ? i : -1;
}

void sh_musica_buscar(void)
{
    static const char *const raices[] = { "/musica", "/tpa/user/audio", "/original/tpa/user/audio",
                                          "/tpa/user/music", "/original/tpa/user/music",     /* W580: la música de fábrica está en /tpa/user/music */
                                          HW_FS_USUARIO,                                     /* (S6) y los MIDI que el usuario haya copiado a Mis archivos */
                                          "/tarjeta/music", "/tarjeta/musica",               /* (pista T) y la música de la memoria externa (api/tarjeta.h) */
                                          HW_FS_MEMORIA };                                   /* (v10) la Memoria interna (W6F): lo que el ordenador sube por MTP cae aquí («other», etc.) */
    char actual[MU_RUTA];
    unsigned r;
    int t, i;
    actual[0] = 0;
    if (mu.pista >= 0) copiar(actual, lista[mu.pista].ruta, MU_RUTA);
    n_lista = 0;
    static const u8 orden_t[3] = { MU_T_MIDI, MU_T_MP3, MU_T_DSP };
    int ti;
    for (ti = 0; ti < 3; ti++) {                          /* primero lo que se puede tocar (MIDI, MP3); después lo que no */
        t = orden_t[ti];
        if (hw_fs_listo())
            for (r = 0; r < sizeof raices / sizeof raices[0]; r++) explorar(raices[r], 0, t);
        if (t == MU_T_MIDI) anadir("casa:" MUSICA_CASA_NOMBRE ".mid", sizeof musica_casa, MU_T_MIDI);
    }
    if (actual[0]) {                                      /* la pista en curso sigue siendo la misma aunque cambie de sitio */
        for (i = 0; i < n_lista && strcmp(lista[i].ruta, actual); i++) ;
        if (i < n_lista) mu.pista = (i16)i;
        else if (mu.estado == MU_PARADA) mu.pista = -1;   /* (S6) la pista cargada ya no está (se borró de Mis archivos) y no suena: ninguna */
        else if (n_lista < MU_MAX_PISTAS) {               /* …y si suena (lo hace desde su copia en RAM), su entrada se queda hasta que acabe */
            copiar(lista[n_lista].ruta, actual, MU_RUTA); lista[n_lista].tam = mu.n; lista[n_lista].tipo = mu.salida == MU_SAL_MP3 ? MU_T_MP3 : MU_T_MIDI;
            mu.pista = (i16)n_lista++;
        } else { sh_musica_parar(); mu.pista = -1; }
    }
}
void sh_musica_refrescar(void) { if (mu.iniciado) sh_musica_buscar(); }
int sh_musica_tocar_ruta(const char *ruta)
{
    int i;
    sh_musica_abrir();
    sh_musica_buscar();
    for (i = 0; i < n_lista && strcmp(lista[i].ruta, ruta); i++) ;
    if (i == n_lista && n_lista < MU_MAX_PISTAS && strlen(ruta) < MU_RUTA && tocable_t(tipo_de(ruta)) && hw_fs_tam(ruta)) {
        /* no está: o es de una carpeta que la Música no mira, o hay OTRA pista con el mismo nombre (la lista no repite
         * nombres) y esta es, por ejemplo, su copia en Mis archivos. Se añade tal cual, al final: se pidió ESTE fichero. */
        copiar(lista[n_lista].ruta, ruta, MU_RUTA); lista[n_lista].tam = hw_fs_tam(ruta); lista[n_lista].tipo = (u8)tipo_de(ruta);
        n_lista++;
    }
    vuelta.n = 0;                                         /* (28-oct) pedida a mano: la vuelta de las descargadas empieza aquí */
    if (i < n_lista) apuntar_vuelta(i);
    return i < n_lista && tocable_t(lista[i].tipo) && sh_musica_tocar(i) == HW_OK ? HW_OK : HW_NO_DISPONIBLE;
}
int sh_musica_n(void) { return n_lista; }
const MuPista *sh_musica_pista(int i) { return i >= 0 && i < n_lista ? &lista[i] : NULL; }
char *sh_musica_nombre(int i)
{
    char t[64];
    const char *b;
    u32 k;
    if (i < 0 || i >= n_lista) return ui_fmt("—");
    b = base_de(lista[i].ruta);
    copiar(t, b, sizeof t);
    for (k = (u32)strlen(t); k > 1; k--) if (t[k - 1] == '.') { t[k - 1] = 0; break; }
    return ui_fmt("%s", t);
}

/* ---- lo guardado en la zona de usuario ------------------------------------------------------------------- */
static void ensuciar(void) { mu.sucio = 1; mu.sucio_desde = hw_ms(); }
static void guardar(void)
{
    MuCfg c;
    if (hw_usr_ocupada()) { mu.sucio = 1; mu.sucio_desde = hw_ms(); return; }   /* (S6) hay una copia de Mis archivos en marcha: se guarda después */
    mu.sucio = 0;
    memset(&c, 0, sizeof c);
    c.version = MU_CFG_VER; c.volumen = mu.volumen; c.modo = mu.modo; c.repetir = mu.repetir;
    if (mu.pista >= 0) { copiar(c.ruta, lista[mu.pista].ruta, MU_RUTA); c.pos_ms = mu.estado == MU_PARADA ? 0 : mu.pos_ms; }
    if (mu.guardado == 1 && !memcmp(&c, &mu.ult, sizeof c)) return;     /* lo mismo que ya hay: ni se intenta */
    switch (hw_usr_escribir(MU_FICHERO, &c, sizeof c)) {
    case HW_OK: mu.guardado = 1; mu.ult = c; hw_log("musica: guardado en la zona de usuario\n"); break;
    case HW_NO_DISPONIBLE: mu.guardado = 2; break;        /* teléfono sin zona de usuario: se sigue sin guardar, y la app lo dice */
    default: mu.guardado = 3; break;
    }
}
/* (2026-10-25) también lo llama la API de apps (ap_ajuste_poner del volumen de la música con guardar = 1) con la música SONANDO: entonces no se
 * escribe (veda de la flash); queda sucio y lo escribe sh_musica_paso en el siguiente silencio. Al apagar se llama tras la pausa. */
void sh_musica_guardar_ya(void) { if (mu.iniciado && mu.sucio && mu.estado != MU_SONANDO) guardar(); }

/* ---- el motor ---------------------------------------------------------------------------------------------- */
static u32 hz_de_nota(int nota)
{
    u32 f;
    if (nota < 0) return 0;
    f = mz_nota_mhz(nota);
    while (f < 397000u) f *= 2;                           /* el generador no baja de 397 Hz: la nota sube de octava */
    f = (f + 500) / 1000;
    return f > 20000 ? 0 : f;
}

/* El paso del ZUMBADOR. En el teléfono lo llama la interrupción del tic cada 4 ms; en el PC, el bucle. */
static void mu_tic(void)
{
    u32 ahora, dt;
    if (mu.cerrojo || mu.estado != MU_SONANDO || mu.salida != MU_SAL_ZUMBADOR || mu.acabada) return;
    ahora = hw_ms(); dt = ahora - mu.t_ult; mu.t_ult = ahora;
    if (dt > 100) dt = 100;                               /* el reloj dio un salto: la música no salta con él */
    if (dt) {
        if (!mz_midi_avanzar_us(&mu.mid, dt * 1000u)) mu.acabada = 1;
        else mu.pos_ms += dt;
    }
    mu.hz = (u16)(mu.acabada ? 0 : hz_de_nota(mz_midi_linea(&mu.mid, &mu.lin, dt)));
    if (!sh_sonando())                                    /* si suena un aviso del sistema, el zumbador es suyo */
        hw_tono_mantener(mu.volumen ? mu.hz : 0, vol_ancho[mu.volumen]);
}

static void a_zumbador(void);

/* El paso del PCM: en el bucle. Entrega los marcos que tocan por tiempo, en bloques de un tic. */
static void mu_pcm(void)
{
    static short bloque[MU_BLOQUE];
    u32 ahora = hw_ms(), debidos = (ahora - mu.t_ult) * (MU_RATE / 1000u);
    if (debidos > MU_RATE / 4) { mu.t_ult = ahora - 250; debidos = MU_RATE / 4; }   /* un parón largo no se «recupera» a la carrera */
    while (debidos >= MU_BLOQUE && !mu.acabada) {
        if (!mu.fin_midi) {
            if (!mz_midi_avanzar_us(&mu.mid, MU_BLOQUE * 1000000u / MU_RATE)) {
                unsigned v;
                mu.fin_midi = 1; mu.cola = 0;
                for (v = 0; v < MZ_VOCES; v++) mz_voz_soltar(v);    /* lo que quede sonando, con su cola */
            } else mu.pos_ms += MU_BLOQUE * 1000u / MU_RATE;
        }
        mz_mezclar(bloque, MU_BLOQUE);
        if (hw_pcm_escribir(bloque, MU_BLOQUE) <= 0) {    /* el DSP ha dejado de aceptar: se sigue por el zumbador */
            hw_log("musica: el PCM ha fallado a media pista\n");
            a_zumbador();
            return;
        }
        mu.pcm_marcos += MU_BLOQUE;
        mu.t_ult += MU_BLOQUE * 1000u / MU_RATE; debidos -= MU_BLOQUE;
        if (mu.fin_midi && (!mz_voces_activas() || ++mu.cola > 25)) mu.acabada = 1;
    }
}

static void callar_salida(void)                           /* con el cerrojo puesto */
{
    if (mu.salida == MU_SAL_ZUMBADOR) { mu.hz = 0; if (!sh_sonando()) hw_tono_mantener(0, 0); }
    if (mu.salida == MU_SAL_PCM) mz_todas_fuera();
}

/* Decide la salida de la pista que va a sonar. La prueba del PCM es cara (hasta 0,5 s si el DSP no contesta),
 * así que se hace una vez y se recuerda. */
static int elegir_salida(void)
{
    if (mu.modo == MU_MODO_AUTO) {
        if (!mu.pcm_probado) {
            mu.pcm_probado = 1;
            mu.pcm_hay = hw_pcm_init(MU_RATE, 1) == HW_OK;
            mu.pcm_abierto = mu.pcm_hay;
            mu.pcm_motivo = mu.pcm_hay ? 0 : hw_pcm_motivo();
            hw_log(mu.pcm_hay ? "musica: el DSP acepta PCM: polifonia\n" : "musica: el DSP no contesta: una voz por el zumbador\n");
        } else if (mu.pcm_hay && !mu.pcm_abierto) {
            mu.pcm_abierto = hw_pcm_init(MU_RATE, 1) == HW_OK;
            if (!mu.pcm_abierto) { mu.pcm_hay = 0; mu.pcm_motivo = hw_pcm_motivo(); }
        }
        if (mu.pcm_hay && mu.pcm_abierto) return MU_SAL_PCM;
    }
    return MU_SAL_ZUMBADOR;
}
static void cerrar_pcm(void) { if (mu.pcm_abierto) { hw_pcm_cerrar(); mu.pcm_abierto = 0; } }

/* (2026-10-25) qué fallos del motor MP3 sonando merecen un reintento automático (los del DSP; no los del fichero ni el cable) y cuántos: 3 por minuto */
volatile u32 mu_reintentos;
static int mp3_recuperable(int m) { return m == -16 || m == -17 || m == -23 || m == -7 || m == -14 || m == -25; }   /* ATASCO, DSP_PERDIDO, ENVIO, MEZCLADOR, ACCION, RITMO (hal/mp3_w580.h) */
static int reintento_permitido(void)
{
    static u32 t[3];
    static u8 k;
    u32 ahora = hw_ms();
    if (t[k] && ahora - t[k] < 60000u) return 0;          /* el más viejo de los 3 últimos fue hace menos de un minuto */
    t[k] = ahora ? ahora : 1; k = (u8)((k + 1) % 3);
    return 1;
}
int sh_mic_activo(void);                                  /* (1.10) shell/micro_sis.c */
static void arrancar_mp3(u32 desde_ms)                    /* hw_mp3_play BLOQUEA (hasta ~4 s la primera vez: arranque del DSP y carga del módulo) */
{
    if (sh_mic_activo()) {                                /* (revisión 27-oct, 3) el DSP es del micrófono (dictado, una app que graba): no se le quita */
        mu.estado = MU_PARADA; mu.pos_ms = desde_ms; registro("mp3: el microfono tiene el DSP; no suena");
        return;
    }
    mu.cerrojo = 1;
    callar_salida(); cerrar_pcm();
    mu.salida = MU_SAL_MP3; mu.acabada = 0; mu.cedida = 0;
    mu.cerrojo = 0;
    if (desde_ms >= mu.dur_ms) desde_ms = 0;
    if (desde_ms) hw_mp3_buscar(desde_ms);                /* sin sonar todavía: solo apunta desde dónde empezar */
    hw_mp3_volumen(vol_mp3[mu.volumen]);
    if (hw_mp3_play() != HW_OK) {
        mu.pcm_motivo = hw_mp3_motivo(); mu.estado = MU_PARADA; mu.pos_ms = 0;
        registro("mp3: no suena");
        return;
    }
    mu.pos_ms = desde_ms; mu.t_ini = hw_ms(); mu.pos_ini = desde_ms; mu.pcm_motivo = 0;
    mu.estado = MU_SONANDO;
    veda_al_dia();
    registro("suena (mp3 por el DSP)");
}

/* Pone el motor a sonar desde `desde_ms` de la pista cargada. */
static void arrancar(u32 desde_ms)
{
    u32 falta;
    if (mu.pista >= 0 && lista[mu.pista].tipo == MU_T_MP3) { arrancar_mp3(desde_ms); return; }
    mu.cerrojo = 1;
    callar_salida();
    mu.salida = (u8)elegir_salida();
    mz_midi_abrir(&mu.mid, mu.datos, mu.n);
    mu.mid.mudo = 1;
    memset(&mu.lin, 0, sizeof mu.lin);
    if (desde_ms >= mu.dur_ms) desde_ms = 0;
    for (falta = desde_ms; falta; ) {                     /* el salto: se recorre en mudo hasta el punto */
        u32 t = falta > 1000 ? 1000 : falta;
        if (!mz_midi_avanzar_us(&mu.mid, t * 1000u)) break;
        mz_midi_linea(&mu.mid, &mu.lin, t);
        falta -= t;
    }
    mu.pos_ms = desde_ms; mu.fin_midi = 0; mu.acabada = 0; mu.cola = 0; mu.hz = 0; mu.cedida = 0;
    if (mu.salida == MU_SAL_PCM) {
        mz_formato(MU_RATE, 1); mz_volumen(vol_pcm[mu.volumen]); mz_todas_fuera();
        mu.mid.mudo = 0;
        mz_midi_retomar(&mu.mid);                         /* las notas que estaban sonando en ese punto */
    } else {
        cerrar_pcm();
        hw_tono_mantener(0, 0);                           /* desde el bucle: abre los relojes y la ruta del zumbador si hacía falta */
    }
    mu.t_ult = mu.t_ini = hw_ms(); mu.pos_ini = desde_ms;
    mu.estado = MU_SONANDO;
    mu.cerrojo = 0;
    veda_al_dia();
    registro("suena");
}

static void a_zumbador(void)                              /* el PCM ha fallado: mismo punto, otra salida */
{
    mu.pcm_hay = 0; mu.pcm_motivo = -20;
    cerrar_pcm();
    if (mu.estado == MU_SONANDO) arrancar(mu.pos_ms);
}

/* Carga la pista i (lee el fichero y lo mide). 0 = no se puede tocar. */
static int cargar_mp3(int i)                              /* el MP3: se abre el fichero y su cabecera; el DSP no se toca hasta que suene */
{
    HwMp3Info mi;
    if (!mp3_hay()) return 0;
    mu.cerrojo = 1;
    callar_salida();                                      /* si lo anterior era un MIDI */
    mu.cerrojo = 0;
    if (hw_mp3_abrir(lista[i].ruta) != HW_OK) {
        mu.pcm_motivo = hw_mp3_motivo();
        hw_mp3_parar(); mu.estado = MU_PARADA; mu.salida = MU_SAL_NADA;
        return 0;
    }
    hw_mp3_info(&mi);
    mu.cerrojo = 1;
    mu.estado = MU_PARADA; mu.acabada = 0;
    if (mu.copia) { mo_soltar(mu.copia); mu.copia = NULL; }
    mu.datos = NULL; mu.n = lista[i].tam; mu.pista = (i16)i; mu.dur_ms = mi.dur_ms; mu.notas = 0; mu.percusion = 0; mu.pistas_smf = 0;
    mu.pos_ms = 0; mu.salida = MU_SAL_MP3; mu.pcm_motivo = 0;
    mu.cerrojo = 0;
    return 1;
}
static int cargar(int i)
{
    const u8 *d = NULL;
    u32 n = 0, dur, notas, perc;
    u8 *copia = NULL;
    int r;
    if (i < 0 || i >= n_lista || !tocable_t(lista[i].tipo)) return 0;
    if (lista[i].tipo == MU_T_MP3) return cargar_mp3(i);
    if (!memcmp(lista[i].ruta, "casa:", 5)) { d = musica_casa; n = sizeof musica_casa; }
    else if (!(d = hw_fs_mapa(lista[i].ruta, &n))) {      /* del FS propio sale sin copiar; del original hay que leerlo */
        n = hw_fs_tam(lista[i].ruta);
        if (!n || n > MU_TAM_MAX || !(copia = mo_probar(n))) return 0;
        if (hw_fs_leer(lista[i].ruta, copia, n) != n) { mo_soltar(copia); return 0; }
        d = copia;
    }
    r = mz_midi_medir(d, n, &dur, &notas, &perc);
    if (r < 0 || !dur) { if (copia) mo_soltar(copia); return 0; }
    if (mu.salida == MU_SAL_MP3) { if (mp3_hay()) hw_mp3_parar(); mu.salida = MU_SAL_NADA; }      /* venía de un MP3: la línea baja */
    mu.cerrojo = 1;
    callar_salida();
    mu.estado = MU_PARADA; mu.acabada = 0;
    if (mu.copia) mo_soltar(mu.copia);
    mu.copia = copia; mu.datos = d; mu.n = n;
    mu.pista = (i16)i; mu.dur_ms = dur; mu.notas = notas; mu.percusion = perc; mu.pistas_smf = (u32)r;
    mu.pos_ms = 0;
    mz_midi_abrir(&mu.mid, d, n); mu.mid.mudo = 1;
    mu.cerrojo = 0;
    return 1;
}

static int tocable(int desde, int d)                      /* la siguiente pista tocable en el sentido d (da la vuelta); -1 = ninguna */
{
    int k, i = desde;
    for (k = 0; k < n_lista; k++) {
        i += d;
        if (i >= n_lista) i = 0;
        if (i < 0) i = n_lista - 1;
        if (tocable_t(lista[i].tipo)) return i;
    }
    return -1;
}

void sh_musica_abrir(void)
{
    MuCfg c;
    int i;
    if (mu.iniciado) return;
    mu.iniciado = 1;
    sh_musica_buscar();
    mu.gancho = hwo_tic_sistema(mu_tic) == HW_OK;
    if (hw_usr_tam(MU_FICHERO) == (int)sizeof c && hw_usr_leer(MU_FICHERO, 0, &c, sizeof c) == (int)sizeof c && c.version == MU_CFG_VER) {
        c.ruta[MU_RUTA - 1] = 0;
        mu.volumen = c.volumen > MU_VOL_MAX ? MU_VOL_MAX : c.volumen;
        mu.modo = c.modo < MU_MODOS ? c.modo : MU_MODO_AUTO;
        mu.repetir = c.repetir < MU_REPS ? c.repetir : MU_REP_NO;
        mu.ult = c; mu.guardado = 1;
        for (i = 0; i < n_lista && strcmp(lista[i].ruta, c.ruta); i++) ;
        if (i < n_lista && cargar(i)) {                   /* la última pista, donde se dejó: en pausa, lista para seguir */
            if (c.pos_ms && c.pos_ms < mu.dur_ms) { mu.pos_ms = c.pos_ms; mu.estado = MU_PAUSA; }
        }
        hw_log("musica: recuperado de la zona de usuario\n");
        registro("recuperado");
    }
}

/* ---- (1.8) LA MÚSICA POR LA RED: la cola y la descarga son de shell/musica_red.c; aquí, el PLAY y qué viene después ------------------- */
static u8 red_retener;                                    /* (1.10) al ceder el DSP: parar la descarga pero NO soltar la red */
static void red_dejar(void) { if (mu.red) { if (red_retener) sh_mured_parar_reteniendo(); else sh_mured_parar(); mu.red = 0; } }
int sh_musica_tocar_red(int i, u32 desde_ms)
{
    if (sh_mic_activo()) { registro("red: el microfono tiene el DSP; no suena"); return HW_ERROR; }   /* (revisión 3) */
    const char *t;
    sh_musica_abrir();
    if (!mp3_hay() || !sh_mured_hay()) return HW_NO_DISPONIBLE;
    if (sh_mured_pista(i, 0, &t, 0, 0)) return HW_ERROR;
    if (mu.salida == MU_SAL_MP3 && hw_mp3_estado() != HW_MP3_PARADO) hw_mp3_parar();   /* lo que tuviera el motor (un fichero, la de la red
                                                          * anterior, o un ERROR: si no, el paso lo vería y reintentaría en bucle, 26-oct) */
    mu.cerrojo = 1;
    callar_salida();                                      /* un MIDI (zumbador o PCM) */
    mu.cerrojo = 0;
    cerrar_pcm();
    if (sh_mured_empezar(i, desde_ms) != HW_OK) return HW_ERROR;
    mu.cerrojo = 1;
    if (mu.copia) { mo_soltar(mu.copia); mu.copia = NULL; }
    mu.red = 1; mu.pista = -1; mu.salida = MU_SAL_MP3; mu.estado = MU_SONANDO;   /* SONANDO ya mientras carga: la veda de la flash puesta */
    mu.acabada = 0; mu.cedida = 0; mu.pos_ms = desde_ms; mu.red_desde = desde_ms; mu.datos = NULL; mu.n = 0;
    mu.notas = mu.percusion = mu.pistas_smf = 0; mu.pcm_motivo = 0;
    { u32 d = 0; sh_mured_pista(i, 0, 0, 0, &d); mu.dur_ms = d; }
    mu.cerrojo = 0;
    veda_al_dia();
    registro("red: carga");
    return HW_OK;
}
static void red_arrancar(void)                            /* el colchón ya tiene bastante: PLAY (el punto lo puso la petición) */
{
    if (mu.estado != MU_SONANDO) return;                  /* en pausa mientras cargaba: se dará al seguir */
    hw_mp3_volumen(vol_mp3[mu.volumen]);
    if (hw_mp3_play() == HW_OK) {
        sh_mured_suena(); mu.red_fallos = 0;
        mu.t_ini = hw_ms(); mu.pos_ini = mu.red_desde; mu.pcm_motivo = 0;
        veda_al_dia(); registro("suena (red)");
        return;
    }
    if (hw_mp3_motivo() == HW_MP3_E_FALTAN) { sh_mured_play_falta(); return; }   /* con más colchón, otra vez */
    mu.pcm_motivo = hw_mp3_motivo();
    registro("red: no suena");
    mu.estado = MU_PARADA; veda_al_dia(); red_dejar();
}
static void red_siguiente(int fallo)                      /* acabó (o no se pudo bajar) la de la cola: la siguiente, o parar */
{
    int a = sh_mured_actual(), n = sh_mured_n(), sig;
    if (fallo && ++mu.red_fallos >= 3) { registro("red: 3 seguidas sin poder bajarse"); sh_musica_parar(); return; }
    if (mu.repetir == MU_REP_UNA && !fallo) sig = a;
    else { sig = a + 1; if (sig >= n) sig = mu.repetir == MU_REP_TODAS ? 0 : -1; }
    if (sig < 0 || sh_musica_tocar_red(sig, 0) != HW_OK) sh_musica_parar();
}

int sh_musica_tocar(int i)
{
    int seguida;
    sh_musica_abrir();
    red_dejar();                                          /* (1.8) un fichero: la red de la música se suelta */
    seguida = mu.estado == MU_SONANDO;                    /* de una pista a otra sin silencio (fin de pista, «siguiente» sonando) */
    if (!cargar(i)) return HW_ERROR;
    if (seguida) ensuciar();                              /* (2026-10-25) la música sigue: se apunta en el próximo silencio (veda de la flash) */
    else guardar();                                       /* la pista nueva se apunta AHORA, en el silencio de antes de sonar (ver sh_musica_paso) */
    arrancar(0);
    return mu.estado == MU_SONANDO ? HW_OK : HW_ERROR;
}

void sh_musica_pausa(void)
{
    if (mu.estado != MU_SONANDO) return;
    if (mu.red && hw_mp3_estado() != HW_MP3_SONANDO) {     /* (1.8) aún cargando: solo se apunta; el colchón se sigue llenando */
        mu.estado = MU_PAUSA; veda_al_dia(); registro("pausa (cargando)");
        return;
    }
    if (mu.salida == MU_SAL_MP3) {                        /* rampa de bajada y 0x0E02{2}: ~0,7 s */
        mu.pos_ms = hw_mp3_tiempo(0);
        hw_mp3_pausa();
        mu.estado = hw_mp3_estado() == HW_MP3_PAUSA ? MU_PAUSA : MU_PARADA;
        if (mu.estado == MU_PARADA) mu.pcm_motivo = hw_mp3_motivo();
        veda_al_dia();
        registro("pausa"); ensuciar();
        return;
    }
    mu.cerrojo = 1;
    callar_salida();
    mu.estado = MU_PAUSA; mu.cedida = 0;
    mu.cerrojo = 0;
    veda_al_dia();
    registro("pausa");
    ensuciar();
}

static void seguir(void)
{
    if (mu.red && mu.estado == MU_PAUSA) {                /* (1.8) */
        if (hw_mp3_estado() == HW_MP3_PAUSA) { if (hw_mp3_play() == HW_OK) { mu.estado = MU_SONANDO; registro("sigue (red)"); } return; }
        if (hw_mp3_estado() == HW_MP3_ABIERTO) { mu.estado = MU_SONANDO; sh_mured_play_falta(); veda_al_dia(); return; }   /* estaba cargando */
        sh_musica_tocar_red(sh_mured_actual(), mu.pos_ms);   /* el motor ya no la tiene: se vuelve a pedir desde donde iba */
        return;
    }
    if (mu.estado != MU_PAUSA || mu.pista < 0) return;
    if (mu.salida == MU_SAL_MP3 && hw_mp3_estado() == HW_MP3_PAUSA) {      /* el decodificador sigue vivo: solo PLAY y la rampa de subida */
        if (hw_mp3_play() == HW_OK) { mu.estado = MU_SONANDO; registro("sigue (mp3)"); }
        else { mu.pcm_motivo = hw_mp3_motivo(); mu.estado = MU_PARADA; }
        return;
    }
    arrancar(mu.pos_ms);                                  /* vuelve a abrir y salta al punto: vale también tras un arranque en frío */
}

void sh_musica_alternar(void)
{
    sh_musica_abrir();
    if (mu.estado == MU_SONANDO) sh_musica_pausa();
    else if (mu.estado == MU_PAUSA) seguir();
    else {
        int i = mu.pista >= 0 ? mu.pista : tocable(-1, 1);
        if (i >= 0) sh_musica_tocar(i);
    }
}

void sh_musica_parar(void)
{
    if (!mu.iniciado) return;
    if (mu.salida == MU_SAL_MP3 && mp3_hay()) hw_mp3_parar();      /* libera el decodificador y baja la línea; ~9 s después apaga el códec y el DSP */
    red_dejar();                                          /* (1.8) la descarga y la red de la música */
    mu.cerrojo = 1;
    callar_salida();
    if (mu.estado != MU_PARADA) registro("parada");
    mu.estado = MU_PARADA; mu.pos_ms = 0; mu.acabada = 0; mu.cedida = 0;
    mu.cerrojo = 0;
    veda_al_dia();
    cerrar_pcm();
    ensuciar();
}

void sh_musica_saltar(int d)
{
    int i, sonaba;
    sh_musica_abrir();
    if (mu.red) {                                         /* (1.8) por la cola de la red */
        int a = sh_mured_actual(), n = sh_mured_n();
        if (!n) return;
        i = d < 0 && mu.pos_ms - mu.red_desde > 3000 ? a : a + (d > 0 ? 1 : -1);
        if (i >= n) i = 0;
        if (i < 0) i = n - 1;
        sh_musica_tocar_red(i, 0);
        return;
    }
    sonaba = mu.estado == MU_SONANDO;
    if (d < 0 && mu.pista >= 0 && mu.pos_ms > 3000) i = mu.pista;     /* «anterior» a media pista = volver a empezarla */
    else if (d > 0 && es_descargada(mu.pista) && (i = descargada_al_azar(mu.pista)) >= 0) { /* (28-oct) otra descargada, al azar */ }
    else i = tocable(mu.pista < 0 ? (d > 0 ? -1 : 0) : mu.pista, d > 0 ? 1 : -1);
    if (i < 0) return;
    if (sonaba) sh_musica_tocar(i);
    else if (cargar(i)) ensuciar();
}

void sh_musica_volumen(int v)
{
    sh_musica_abrir();
    v = v < 0 ? 0 : v > MU_VOL_MAX ? MU_VOL_MAX : v;
    if (v == mu.volumen) return;
    mu.volumen = (u8)v;
    mz_volumen(vol_pcm[v]);                               /* el zumbador lo coge solo en el tic siguiente */
    if (mu.salida == MU_SAL_MP3 && mp3_hay()) hw_mp3_volumen(vol_mp3[v]);
    ensuciar();
}

void sh_musica_modo(int m)
{
    sh_musica_abrir();
    if (m < 0 || m >= MU_MODOS) return;
    mu.modo = (u8)m;
    if (m == MU_MODO_AUTO) mu.pcm_probado = 0;            /* «automático» otra vez = volver a preguntarle al DSP */
    if (mu.estado == MU_SONANDO && mu.salida != MU_SAL_MP3) arrancar(mu.pos_ms);     /* cambia de salida sin perder el punto */
    else if (m != MU_MODO_AUTO) cerrar_pcm();
    ensuciar();
}

void sh_musica_repetir(int r) { sh_musica_abrir(); if (r >= 0 && r < MU_REPS) { mu.repetir = (u8)r; ensuciar(); } }

/* Una vez por vuelta del bucle de la plataforma. */
void sh_musica_paso(void)
{
    if (!mu.iniciado) return;
    if (mp3_hay()) {
        hw_mp3_paso();                                    /* créditos, datos del fichero, cable USB y el cierre diferido; no bloquea */
        if (mu.red) {                                     /* (1.8) la red al colchón; avisa cuando hay para PLAY o si no se puede bajar */
            int ev = sh_mured_paso();
            MuRedInfo ri;
            if (ev == MRED_EV_TOCAR) red_arrancar();
            else if (ev == MRED_EV_FALLO) { registro("red: no se pudo bajar"); red_siguiente(1); }   /* (si ya sonaba, musica_red deja que acabe) */
            if (mu.red) { sh_mured_info(&ri); if (ri.dur_ms) mu.dur_ms = ri.dur_ms; }
        }
        if (mu.red && !sh_mured_tocando()) mu.pos_ms = mu.red_desde;   /* (1.8) cargando: el motor aún no es de esta pista; no se le mira */
        else if (mu.salida == MU_SAL_MP3 && (mu.estado == MU_SONANDO || mu.estado == MU_PAUSA)) {
            int e = hw_mp3_estado();
            mu.pos_ms = hw_mp3_tiempo(0) + (mu.red ? mu.red_desde : 0);
            if (e == HW_MP3_FIN) mu.acabada = 1;
            else if (e == HW_MP3_ERROR) {
                mu.pcm_motivo = hw_mp3_motivo();
                if (mu.estado == MU_SONANDO && mp3_recuperable(mu.pcm_motivo) && reintento_permitido()) {
                    /* (2026-10-25) RED DE SEGURIDAD: el motor abortó sonando por algo del DSP (se vació porque el bucle estuvo parado, se
                     * perdió, no contestó): se vuelve a tocar DESDE DONDE IBA (arranque completo, ~2 s) en vez de dejar la canción muerta */
                    u32 donde = mu.pos_ms;
                    hw_log("musica: motivo -"); num((u32)-mu.pcm_motivo); hw_log("\n");   /* (26-oct) cuál, antes de que el reintento lo borre */
                    registro("mp3: error, se reintenta desde donde iba");
                    mu_reintentos++;
                    if (mu.red) sh_musica_tocar_red(sh_mured_actual(), donde);   /* (1.8) el flujo no se puede volver a tocar: se pide desde ahí */
                    else arrancar_mp3(donde);
                } else { mu.estado = MU_PARADA; hw_log("musica: motivo -"); num((u32)-mu.pcm_motivo); hw_log("\n"); registro("mp3: error"); }
            }
        }
    }
    if (mu.estado == MU_SONANDO) {
        if (mu.salida == MU_SAL_PCM) mu_pcm();
        else if (!mu.gancho) mu_tic();                    /* sin gancho del tic (el PC): se sondea aquí */
    }
    if (mu.acabada && mu.estado == MU_SONANDO) {          /* fin de la pista: qué viene */
        int sig;
        registro("fin");
        mu.pos_ms = mu.dur_ms;
        if (mu.red) { red_siguiente(0); goto guardado; }    /* (1.8) la siguiente de la cola de la red */
        if (mu.repetir == MU_REP_UNA) sig = mu.pista;
        else if (es_descargada(mu.pista) && (sig = descargada_al_azar(mu.pista)) >= 0) { /* (28-oct) otra descargada, al azar */ }
        else {
            sig = tocable(mu.pista, 1);
            if (sig >= 0 && sig <= mu.pista && mu.repetir != MU_REP_TODAS) sig = -1;   /* era la última y no se repite */
        }
        if (app_pantalla_tomada()) {                      /* (28-oct) con una app en pantalla solo suena el MP3 (sh_musica_app): las otras se saltan */
            int k = 0;
            while (sig >= 0 && lista[sig].tipo != MU_T_MP3 && k++ < n_lista) {
                int s2 = tocable(sig, 1);
                sig = s2 >= 0 && s2 <= sig && mu.repetir != MU_REP_TODAS ? -1 : s2;
            }
            if (sig >= 0 && lista[sig].tipo != MU_T_MP3) sig = -1;
        }
        if (sig >= 0 && sh_musica_tocar(sig) == HW_OK) { /* sigue */ }
        else sh_musica_parar();
    }
guardado:
    /* Guardar es escribir en la NOR, y eso corta las interrupciones mientras programa: con la música sonando se
     * come tics y la pista da un tropezón [medido en el emulador estricto: 36 ms, 2 s después de empezar cada
     * pista, que era cuando se guardaba]. Así que mientras SUENA no se guarda nada: lo pendiente (el volumen que
     * se ha tocado) espera a la pausa, a la parada, al cambio de pista o al apagado. */
    veda_al_dia();                                        /* (2026-10-25) la veda de la flash sigue al estado (también tras un error o un aviso) */
    if (mu.sucio && mu.estado != MU_SONANDO && hw_ms() - mu.sucio_desde >= 2000) guardar();
}

/* (28-oct, el mantenedor: «cuando una canción está sonando e inicio un juego la música se pausa, ¿no puede sonar con el juego corriendo?») Una app
 * externa toma la pantalla (shell/app_externa.c). El MP3 (de fichero o de la red) SIGUE: lo manda al DSP el alimentador del TIC desde el
 * colchón del sistema (192 KB, fuera del préstamo de apps) y app_latido llama a sh_musica_paso en cada cuadro de la app para rellenarlo
 * (8 KB por paso). Lo del zumbador o el PCM se pausa como antes: el zumbador es de la app y de los avisos, y el PCM lo sintetiza el bucle.
 * Devuelve 1 si sigue sonando. */
int sh_musica_app(void)
{
    if (mu.iniciado && mu.estado == MU_SONANDO && mu.salida == MU_SAL_MP3) { registro("sigue con la app en pantalla"); return 1; }
    sh_musica_pausa();
    return 0;
}

int sh_musica_estado(void) { return mu.estado; }
int sh_musica_dsp_activo(void) { return mp3_hay() && hw_mp3_activo && hw_mp3_activo(); }   /* el MP3 tiene el DSP o la línea encendidos: no se duerme a fondo */
/* (M2) Una app externa tuvo prestado el gancho del tic para su melodía (shell/app_externa.c) y lo devuelve. */
void sh_musica_gancho(void) { if (mu.gancho) hwo_tic_sistema(mu_tic); }
int sh_musica_en_zumbador(void) { return mu.estado == MU_SONANDO && mu.salida == MU_SAL_ZUMBADOR && mu.volumen; }

/* shell/sonido.c avisa de que empieza (1) o calla (0) un aviso de los que piden atención. Por el zumbador no
 * caben los dos: la música se aparta y vuelve. Por PCM no hace falta nada. */
void sh_musica_aviso(int empieza)
{
    if (!mu.iniciado) return;
    if (empieza) {
        if (mu.estado == MU_SONANDO && mu.salida == MU_SAL_ZUMBADOR) {
            mu.cerrojo = 1; mu.hz = 0; mu.estado = MU_PAUSA; mu.cedida = 1; mu.cerrojo = 0;
            registro("cede el zumbador a un aviso");
        }
    } else if (mu.cedida && mu.estado == MU_PAUSA) {
        mu.cedida = 0;
        seguir();
    }
}

/* (1.10, 27-oct) CEDER EL DSP AL MICRÓFONO (el dictado o una app que graba). Decisión del mantenedor: PAUSAR Y REANUDAR. El micrófono reinicia el DSP con
 * otro módulo (AAC_PLUS) y no se sabe si puede convivir con el MP3 [sin saber]: la música que va por el DSP (un MP3 de fichero o de la red) se PARA
 * DEL TODO apuntando la pista y el punto, el DSP se apaga YA (sin el cierre diferido de ~9 s) y, al devolverlo, vuelve a sonar desde ese punto si
 * sonaba (en pausa se queda parada). La de la red sigue por Range desde el byte de ese punto (sh_musica_tocar_red con desde_ms). El zumbador y el
 * PCM de los MIDI no usan este camino. Devuelve 1 si había algo que reanudar. */
static struct { u8 hay, red, red_retenida; i16 pista; u32 pos_ms; char ruta[MU_RUTA]; } ced;   /* (revisión 12) la RUTA: el índice cambia si la lista se rehace */
int sh_musica_ceder_dsp(void)
{
    if (ced.hay) return 1;
    if (mu.iniciado && mu.salida == MU_SAL_MP3 && mu.estado != MU_PARADA) {
        ced.red = mu.red; ced.pista = mu.red ? (i16)sh_mured_actual() : mu.pista; ced.pos_ms = mu.pos_ms;
        ced.ruta[0] = 0;
        if (!mu.red && mu.pista >= 0) memcpy(ced.ruta, lista[mu.pista].ruta, sizeof ced.ruta);
        ced.hay = mu.estado == MU_SONANDO && ced.pista >= 0;
        red_retener = ced.red_retenida = ced.red && ced.hay;          /* la de la red que volverá: su red se queda (sin apagar la radio) */
        sh_musica_parar();
        red_retener = 0;
        registro(ced.hay ? "cede el DSP al microfono (seguira)" : "cede el DSP al microfono");
    }
    if (mp3_hay() && hw_mp3_cerrar_ya) hw_mp3_cerrar_ya();
    return ced.hay;
}
void sh_musica_recuperar_dsp(void)
{
    if (ced.red_retenida && !ced.hay) { ced.red_retenida = 0; sh_red_soltar(SH_BT_MUSICA); sh_bt_soltar(SH_BT_MUSICA); }
    if (!ced.hay) return;
    ced.hay = 0;
    if (ced.red) {
        ced.red_retenida = 0;
        if (sh_musica_tocar_red(ced.pista, ced.pos_ms) != HW_OK) { sh_red_soltar(SH_BT_MUSICA); sh_bt_soltar(SH_BT_MUSICA); }   /* no volvió: la red retenida, fuera */
        return;
    }
    { int i; for (i = 0; i < n_lista; i++) if (!strcmp(lista[i].ruta, ced.ruta)) break;   /* la pista por su ruta */
      if (i == n_lista) { registro("la pista cedida ya no esta en la lista"); return; }
      ced.pista = (i16)i; }
    if (!cargar(ced.pista)) return;
    arrancar(ced.pos_ms);
    registro("vuelve del microfono");
}

void sh_musica_pantalla(int abierta) { mu.pantalla = (u8)!!abierta; }
static u8 sin_teclas;
void sh_musica_sin_teclas(int si) { sin_teclas = (u8)!!si; }

/* El filtro de teclas del shell (energia.c) le enseña aquí TODAS las teclas antes que al foco:
 *   Walkman  abre la app de Música; con ella ya abierta, reproducir / pausa
 *   Play     reproducir / pausa, desde cualquier sitio
 *   Vol +/-  el volumen de la música, mientras suena o con su pantalla abierta (si no, son de la app: pasar página) */
int sh_musica_tecla(int k)
{
    if (sin_teclas) return 0;
    if (k == K_WALKMAN) {
        if (mu.pantalla) sh_musica_alternar();
        else sh_abrir_sen(SH_APP_MUSICA, NULL);            /* (APX) la interfaz es un .sen; el servicio sigue aquí. Con OTRA app de fuera
                                                              * abierta (solo cabe una) esa se cierra (W5_SALIR: guarda) y se abre Música */
        return 1;
    }
    if (k == K_PLAY) { sh_musica_alternar(); return 1; }
    if ((k == K_VOL_MAS || k == K_VOL_MENOS) && (mu.pantalla || mu.estado == MU_SONANDO)) {
        sh_musica_volumen((int)mu.volumen + (k == K_VOL_MAS ? 1 : -1));
        return 1;
    }
    return 0;
}

void sh_musica_info(MuInfo *m)
{
    memset(m, 0, sizeof *m);
    m->estado = mu.estado; m->salida = mu.salida; m->volumen = mu.volumen; m->modo = mu.modo; m->repetir = mu.repetir;
    m->pcm_probado = mu.pcm_probado; m->pcm_hay = mu.pcm_hay; m->pcm_motivo = mu.pcm_motivo;
    m->pista = mu.pista; m->n = (i16)n_lista;
    m->pos_ms = mu.pos_ms > mu.dur_ms ? mu.dur_ms : mu.pos_ms; m->dur_ms = mu.dur_ms;
    m->notas = mu.notas; m->percusion = mu.percusion; m->pistas_smf = mu.pistas_smf; m->perdidas = mu.mid.perdidas;
    m->tam = mu.n; m->pcm_marcos = mu.pcm_marcos; m->hz = mu.hz; m->guardado = mu.guardado; m->cedida = mu.cedida;
    m->red = mu.red;
}
