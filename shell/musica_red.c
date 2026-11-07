/* musica_red.c — (1.8, 26-oct) LA MÚSICA POR LA RED, en el sistema (musica_red.h; docs de la música por la red).
 *
 * Una pista de la cola: (1) la red del puente, como dueño SH_BT_MUSICA (si ya la usa la app, se SUMA: shell/red_sis.c); (2) GET de su URL;
 * (3) con la respuesta, hw_mp3_flujo_abrir(NULL) = el colchón del SISTEMA (192 KB), con el total si viene Content-Length; (4) en cada vuelta
 * del bucle, lo que haya en el canal pasa al colchón (cabe lo que quepa: el control de flujo de W5RED hace esperar al servidor); (5) con
 * ~64 KB (o la pista entera) le dice al servicio de música que ya puede dar PLAY (MRED_EV_TOCAR); (6) acabada la respuesta,
 * hw_mp3_flujo_fin. Si la red se corta o el canal se cierra antes de tiempo: se vuelve a conectar (esperando 1, 2, 3… s) y se pide el RESTO
 * con «Range: bytes=N-», empalmando byte a byte (el servidor dice en X-Desde-Byte dónde empieza lo que sirve). Mientras tanto el motor
 * suena del colchón y, si se vacía, hace una pausa limpia y sigue solo (hal/mp3_w580.c). Nada de esto toca la NOR: el colchón es RAM.
 * El motor y la red son DÉBILES: en el PC (sin radio ni DSP) sh_mured_hay() da 0 y nada se llama. */
#include "shell.h"
#include "bt_sis.h"
#include "red_sis.h"
#include "musica_red.h"
#include "../bt/red/red.h"

int  hw_mp3_flujo_abrir(void *colchon, u32 tam, u32 total) __attribute__((weak));
u32  hw_mp3_flujo_escribir(const void *d, u32 n) __attribute__((weak));
u32  hw_mp3_flujo_libre(void) __attribute__((weak));
void hw_mp3_flujo_fin(void) __attribute__((weak));
void hw_mp3_flujo_info(HwMp3Flujo *i) __attribute__((weak));

#define MRED_PLAY      (64u * 1024u)    /* con esto en el colchón (~4 s a 128 kbps) se da PLAY: el motor pide ~1 s y sigue rellenando */
#define MRED_REINTENTOS 8               /* cortes seguidos sin que llegue nada nuevo antes de rendirse con esta pista */
#define MRED_PLAZO_S   30               /* para que el servidor conteste (prepara la pista: ~2-3 s; una que no está, ~0,5 s) */

typedef struct { char url[MRED_URL]; char titulo[MRED_TITULO]; char artista[MRED_ARTISTA]; u32 dur_ms; } Pista;
static Pista cola[MRED_MAX];
static int n_cola;
static u8 tmp[4096];                    /* del canal al colchón (el sistema es de un solo hilo) */
static struct {
    i16 actual;
    u8  fase, reintentos, tocando, avisado, flujo;
    int canal, http;
    u32 recibidos, total, dur_ms, desde_ms, desde_byte, play_desde, t_reintento, ult_bueno;
    char porque[MRED_PORQUE];
} mr = { .actual = -1 };   /* (no «D»: las herramientas del banco buscan el D del motor, hal/mp3_w580.c) */

int sh_mured_hay(void) { return sh_red_hay() && hw_mp3_flujo_abrir && hw_mp3_flujo_escribir && hw_mp3_flujo_libre && hw_mp3_flujo_fin && hw_mp3_flujo_info; }

static void copiar(char *d, const char *s, u32 max) { u32 i = 0; if (s) while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }
static int empieza(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int tiene(const char *s, char c) { while (*s) if (*s++ == c) return 1; return 0; }
static void porque(const char *a, const char *b) { copiar(mr.porque, a, sizeof mr.porque); if (b) { u32 n = (u32)strlen(mr.porque); copiar(mr.porque + n, b, sizeof mr.porque - n); } }
static char *num(char *d, u32 v) { char t[12]; int n = 11; t[11] = 0; do { t[--n] = (char)('0' + v % 10); v /= 10; } while (v); copiar(d, t + n, 12); return d + strlen(d); }
static void registro(const char *que) { hw_log("mured: "); hw_log(que); if (mr.actual >= 0) { hw_log(" ["); hw_log(cola[mr.actual].titulo); hw_log("]"); } hw_log("\n"); }

void sh_mured_vaciar(void) { n_cola = 0; }
int sh_mured_n(void) { return n_cola; }
int sh_mured_actual(void) { return mr.actual; }
int sh_mured_poner(const char *url, const char *titulo, const char *artista, u32 dur_ms)
{
    Pista *p;
    if (n_cola >= MRED_MAX || !url || strlen(url) >= MRED_URL || !(empieza(url, "http://") || empieza(url, "https://"))) return -1;
    p = &cola[n_cola];
    copiar(p->url, url, MRED_URL); copiar(p->titulo, titulo ? titulo : "", MRED_TITULO); copiar(p->artista, artista ? artista : "", MRED_ARTISTA);
    p->dur_ms = dur_ms;
    return n_cola++;
}
int sh_mured_pista(int i, const char **url, const char **titulo, const char **artista, u32 *dur_ms)
{
    if (i < 0 || i >= n_cola) return -1;
    if (url) *url = cola[i].url;
    if (titulo) *titulo = cola[i].titulo;
    if (artista) *artista = cola[i].artista;
    if (dur_ms) *dur_ms = cola[i].dur_ms;
    return 0;
}

static void cerrar_canal(void)
{
    if (mr.canal > 0) { red_cerrar(mr.canal); sh_red_canal_poner(mr.canal, SH_BT_NADIE); }
    mr.canal = 0;
}
/* rendirse con esta pista: si ya sonaba, que toque lo que queda en el colchón y acabe (fin de pista normal: el servicio pasa a la siguiente);
 * si no, MRED_EV_FALLO y el servicio la salta */
static int rendirse(void)
{
    mr.fase = MRED_FALLO;
    registro("se rinde");
    if (mr.tocando && mr.flujo) { hw_mp3_flujo_fin(); return MRED_EV_NADA; }
    return MRED_EV_FALLO;
}
/* un corte (la red, el canal cerrado antes de tiempo): reintentar con espera creciente; a la MRED_REINTENTOS sin progreso, rendirse */
static int cortado(const char *por, const char *detalle)
{
    cerrar_canal();
    porque(por, detalle);
    if (++mr.reintentos > MRED_REINTENTOS) return rendirse();
    mr.fase = MRED_CONECTANDO;
    mr.t_reintento = hw_ms() + 1000u * (mr.reintentos < 5 ? mr.reintentos : 5);
    registro("cortada: se reintenta");
    return MRED_EV_NADA;
}

int sh_mured_empezar(int i, u32 desde_ms)
{
    if (!sh_mured_hay() || i < 0 || i >= n_cola) return HW_ERROR;
    cerrar_canal();
    mr.actual = (i16)i; mr.fase = MRED_CONECTANDO; mr.reintentos = 0; mr.tocando = 0; mr.avisado = 0; mr.flujo = 0; mr.http = 0;
    mr.recibidos = 0; mr.total = 0; mr.desde_ms = desde_ms; mr.desde_byte = 0; mr.play_desde = MRED_PLAY; mr.t_reintento = 0;
    mr.dur_ms = cola[i].dur_ms; mr.porque[0] = 0; mr.ult_bueno = 0;
    registro(desde_ms ? "empieza (desde un punto)" : "empieza");
    return HW_OK;
}
void sh_mured_play_falta(void) { mr.avisado = 0; mr.play_desde = mr.recibidos + 32u * 1024u; }
void sh_mured_suena(void) { mr.tocando = 1; }
int sh_mured_tocando(void) { return mr.actual >= 0 && mr.tocando; }
void sh_mured_parar(void)
{
    sh_mured_parar_reteniendo();
    sh_red_soltar(SH_BT_MUSICA);                       /* la red sigue si la usa la app; si no, se corta */
    sh_bt_soltar(SH_BT_MUSICA);                        /* y la radio, si era de la música y nadie más la quiere */
}
/* (1.10) parar la descarga SIN soltar la red: la música cede el DSP al micrófono y volverá enseguida (shell/musica.c); medido 27-oct: soltándola,
 * la radio se apagaba y volver a sonar tardaba 17,6 s (encender la radio y reconectar) */
void sh_mured_parar_reteniendo(void)
{
    cerrar_canal();
    if (mr.fase != MRED_QUIETA) registro("parada");
    mr.fase = MRED_QUIETA; mr.actual = -1; mr.flujo = 0; mr.tocando = 0;
}

/* el valor numérico de la cabecera «Nombre: valor» (0 si no está) */
static u32 cabecera(const char *c, const char *nombre)
{
    u32 n = (u32)strlen(nombre), v = 0;
    const char *p = c;
    while (*p) {
        u32 i;
        for (i = 0; i < n && p[i] && (p[i] | 32) == (nombre[i] | 32); i++) ;
        if (i == n && p[i] == ':') {
            p += i + 1;
            while (*p == ' ') p++;
            while (*p >= '0' && *p <= '9') v = v * 10u + (u32)(*p++ - '0');
            return v;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return 0;
}

static void pedir(void)
{
    char url[MRED_URL + 24], cab[48], *q;
    u32 n;
    int c;
    copiar(url, cola[mr.actual].url, sizeof url);
    cab[0] = 0;
    if (mr.flujo) {                                      /* reanudar: el resto, desde el byte exacto (empalma con lo que ya hay en el colchón) */
        copiar(cab, "Range: bytes=", sizeof cab); q = num(cab + strlen(cab), mr.desde_byte + mr.recibidos); copiar(q, "-\r\n", 4);
    } else if (mr.desde_ms) {                            /* empezar en un punto (volver a tocar desde donde iba): el servidor alinea a una trama */
        n = (u32)strlen(url);
        copiar(url + n, tiene(url, '?') ? "&desde_ms=" : "?desde_ms=", sizeof url - n);
        num(url + strlen(url), mr.desde_ms);
    }
    c = red_http("GET", url, cab[0] ? cab : 0, 0, 0, 0, MRED_PLAZO_S);
    if (c > 0) { sh_red_canal_poner(c, SH_BT_MUSICA); mr.canal = c; mr.fase = MRED_RESPUESTA; mr.http = 0; }
    else if (c == RED_E_SIN_SESION) mr.fase = MRED_CONECTANDO;
    /* OCUPADO / SIN_CANAL: en la vuelta siguiente */
}

static int respuesta(void)
{
    static char cab[RED_CAB];
    int h = red_http_estado(mr.canal);
    if (!h) {
        if (red_canal(mr.canal) == RED_C_CERRADO) return cortado("sin respuesta: ", red_texto(mr.canal));
        return MRED_EV_NADA;
    }
    mr.http = h;
    red_cabeceras(mr.canal, cab, sizeof cab);
    if (h != 200 && h != 206) {
        char t[12];
        cerrar_canal(); num(t, (u32)h); porque("el servidor dice HTTP ", t);
        return rendirse();
    }
    if (mr.flujo) {                                      /* una reanudación: tiene que ser el trozo pedido */
        if (h != 206) { cerrar_canal(); porque("el servidor no sabe reanudar (Range)", 0); return rendirse(); }
    } else {
        u32 cl = cabecera(cab, "Content-Length"), dur = cabecera(cab, "X-Duracion-Ms");
        mr.desde_byte = cabecera(cab, "X-Desde-Byte");
        if (dur) mr.dur_ms = dur;
        mr.total = cl;
        if (hw_mp3_flujo_abrir(0, 0, cl) != HW_OK) { cerrar_canal(); porque("el motor no abrió el flujo", 0); return rendirse(); }
        mr.flujo = 1;
    }
    mr.fase = MRED_BAJANDO;
    return MRED_EV_NADA;
}

static int bajar(void)
{
    for (;;) {
        u32 libre = hw_mp3_flujo_libre(), disp = (u32)red_disponibles(mr.canal), n, w;
        int k;
        if (!libre || !disp) break;
        n = libre < disp ? libre : disp;
        if (n > sizeof tmp) n = sizeof tmp;
        k = red_recibir(mr.canal, tmp, n);
        if (k <= 0) break;
        w = hw_mp3_flujo_escribir(tmp, (u32)k);
        mr.recibidos += w;
        if (w < (u32)k) { porque("el colchón no tomó todo", 0); break; }          /* no pasa: n <= libre */
    }
    if (mr.recibidos - mr.ult_bueno >= 32u * 1024u) { mr.ult_bueno = mr.recibidos; mr.reintentos = 0; }   /* llega: los cortes vuelven a contar desde 0 */
    if (red_canal(mr.canal) == RED_C_CERRADO && !red_disponibles(mr.canal)) {
        int cod = red_codigo(mr.canal);
        if (cod == RED_BIEN && (!mr.total || mr.recibidos >= mr.total)) {   /* total = Content-Length de la PRIMERA respuesta (desde desde_byte) */
            cerrar_canal();
            hw_mp3_flujo_fin();
            mr.fase = MRED_ENTERA;
            registro("entera");
        } else return cortado(cod == RED_BIEN ? "se cortó antes del final: " : "se cortó: ", red_texto(mr.canal));
    }
    return MRED_EV_NADA;
}

int sh_mured_paso(void)
{
    int ev = MRED_EV_NADA, e;
    if (mr.actual < 0 || mr.fase == MRED_QUIETA || mr.fase == MRED_FALLO) return MRED_EV_NADA;
    switch (mr.fase) {
    case MRED_CONECTANDO:
        e = sh_red_estado();
        if (!sh_red_es_dueno(SH_BT_MUSICA) || e == SH_RED_FALLO || e == SH_RED_APAGADA) {
            if ((i32)(hw_ms() - mr.t_reintento) < 0) break;
            if (sh_red_conectar(0, SH_BT_MUSICA)) {     /* no arrancó (sin puente, ocupada por un robot…): otra vez luego */
                ev = cortado("la red: ", sh_red_fallo_nombre(sh_red_fallo()));
                break;
            }
            e = sh_red_estado();
        }
        if (e == SH_RED_LISTA) { mr.fase = MRED_PIDIENDO; pedir(); }
        else if (e == SH_RED_FALLO) ev = cortado("la red: ", sh_red_fallo_nombre(sh_red_fallo()));
        break;
    case MRED_PIDIENDO:
        if (sh_red_estado() != SH_RED_LISTA) { mr.fase = MRED_CONECTANDO; break; }
        pedir();
        break;
    case MRED_RESPUESTA:
        ev = respuesta();
        if (mr.fase == MRED_BAJANDO) ev = bajar();
        break;
    case MRED_BAJANDO:
        if (sh_red_estado() != SH_RED_LISTA && red_canal(mr.canal) != RED_C_CERRADO) { ev = cortado("la red se cayó", 0); break; }
        ev = bajar();
        break;
    default: break;
    }
    if (ev == MRED_EV_NADA && mr.flujo && !mr.tocando && !mr.avisado && (mr.fase == MRED_ENTERA || mr.recibidos >= mr.play_desde)) {
        mr.avisado = 1;
        return MRED_EV_TOCAR;
    }
    return ev;
}

void sh_mured_info(MuRedInfo *i)
{
    HwMp3Flujo f;
    memset(i, 0, sizeof *i);
    i->actual = mr.actual; i->n = (i16)n_cola; i->fase = mr.fase; i->red = (u8)sh_red_estado(); i->reintentos = mr.reintentos;
    i->tocando = mr.tocando; i->http = mr.http; i->recibidos = mr.recibidos; i->total = mr.total; i->dur_ms = mr.dur_ms; i->desde_ms = mr.desde_ms;
    copiar(i->porque, mr.porque, sizeof i->porque);
    if (mr.flujo && hw_mp3_flujo_info) { hw_mp3_flujo_info(&f); i->colchon = f.nivel; i->esperas = f.esperas; i->ms_esperando = f.ms_esperando; }
}
