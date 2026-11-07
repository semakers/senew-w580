/* cg_red.c — (1.6) las envolturas del grupo de RED de la tabla `w5` (api/cargable_red.h; el contrato documentado está en
 * cargables/sdk/w5_red.h). Mismas reglas que cg_bt.c: todo lo que entra de la app se VALIDA (cadenas byte a byte con cg_copiar_cad,
 * búferes con cg_legible_n / cg_en_app) y se COPIA a búferes del sistema; nada acaba en pánico. Sin CG_PIDE: la red es OPCIONAL
 * para la app; sin radio cada función contesta «no disponible».
 * La app es DUEÑA de la red que conecta (o a la que se suma: 1.8, la música del sistema puede estar usándola); sus canales solo los ve
 * ella (sh_red_canal_poner), y al descargarla cg_descargar() llama a sh_red_soltar_app() (antes que a sh_bt_soltar_app): se cierran SUS
 * canales y la red se corta solo si nadie más la usa. Debajo: shell/red_sis.c y bt/red/red.c. */
#include "cargador.h"
#include "cargable_red.h"
#include "bt_sis.h"
#include "red_sis.h"
#include "../bt/red/red.h"

#define TROZO 4096u                                    /* lo más que se copia por llamada en enviar/recibir (= la cola de envío) */
static u8 cop[TROZO];                                  /* el sistema es de un solo hilo: un búfer estático, no 4 KB de pila */
static int viva_(void) { return cg_estado() == CG_VIVA && sh_red_hay(); }
static int mia_(void) { return sh_red_es_dueno(SH_BT_APP); }
static int mio_(int c) { return mia_() && sh_red_canal_dueno(c) == SH_BT_APP; }   /* (1.8) el canal es de la app (no de la música) */
static int lista_(void) { return viva_() && mia_() && sh_red_estado() == SH_RED_LISTA; }

int t_red_conectar(const unsigned char *addr)
{
    u8 a[6];
    if (!viva_()) return W5_NO_DISPONIBLE;
    if (addr) { if (!cg_legible_n(addr, 6)) return W5_ERROR; memcpy(a, addr, 6); }
    return sh_red_conectar(addr ? a : 0, SH_BT_APP) ? W5_ERROR : W5_OK;
}
void t_red_desconectar(void) { if (viva_() && mia_()) sh_red_soltar(SH_BT_APP); }   /* (1.8) suelta lo suyo; si la usa la música, sigue */
/* (1.8) la app ve la red solo si es dueña (la conectó o se sumó): si la usa otro (la música), la ve APAGADA y red_conectar la suma */
int  t_red_estado(void) { return viva_() && mia_() ? sh_red_estado() : W5_RED_APAGADA; }
int  t_red_fallo(void) { return viva_() ? (mia_() ? sh_red_fallo() : W5_REDF_OK) : W5_REDF_SIN_RADIO; }

int t_red_http(const char *metodo, const char *url, const char *cab, const void *cuerpo, unsigned n, unsigned banderas)
{
    static char m[16], u[1024], c[1536];
    if (!viva_()) return W5_RED_E_NO_DISPONIBLE;
    if (!lista_()) return W5_RED_E_SIN_SESION;
    m[0] = c[0] = 0;
    if (metodo && !cg_copiar_cad(m, sizeof m, metodo)) return W5_RED_E_ARG;
    if (!url || !cg_copiar_cad(u, sizeof u, url) || !u[0]) return W5_RED_E_ARG;
    if (cab && !cg_copiar_cad(c, sizeof c, cab)) return W5_RED_E_ARG;
    if (n == W5_RED_CUERPO_A_TROZOS) cuerpo = 0;
    else if (n) {
        if (n > W5_RED_CUERPO_MAX || !cuerpo || !cg_legible_n(cuerpo, n)) return W5_RED_E_ARG;
        memcpy(cop, cuerpo, n); cuerpo = cop;
    }
    {   int k = red_http(m, u, c, cuerpo, n, banderas & 0xFFu, (banderas >> 8) & 0xFFu);
        if (k > 0) sh_red_canal_poner(k, SH_BT_APP);
        return k; }
}
int t_red_tcp(const char *host, unsigned puerto, unsigned banderas)
{
    static char h[256];
    if (!viva_()) return W5_RED_E_NO_DISPONIBLE;
    if (!lista_()) return W5_RED_E_SIN_SESION;
    if (!host || !cg_copiar_cad(h, sizeof h, host) || !h[0]) return W5_RED_E_ARG;
    {   int k = red_tcp(h, puerto, banderas & 0xFFu, (banderas >> 8) & 0xFFu);
        if (k > 0) sh_red_canal_poner(k, SH_BT_APP);
        return k; }
}

int  t_red_canal(int c) { return viva_() && mio_(c) ? red_canal(c) : W5_RC_LIBRE; }
int  t_red_codigo(int c) { return viva_() && mio_(c) ? red_codigo(c) : W5_RCOD_SIN_SESION; }
int  t_red_http_estado(int c) { return viva_() && mio_(c) ? red_http_estado(c) : 0; }
int  t_red_fin_recibido(int c) { return viva_() && mio_(c) ? red_fin_recibido(c) : 1; }
int  t_red_disponibles(int c) { return viva_() && mio_(c) ? red_disponibles(c) : 0; }
int  t_red_pendiente(int c) { return viva_() && mio_(c) ? red_pendiente(c) : 0; }
int  t_red_terminar(int c) { return viva_() && mio_(c) && !red_terminar(c) ? W5_OK : W5_ERROR; }
void t_red_cerrar(int c) { if (viva_() && mio_(c)) { red_cerrar(c); sh_red_canal_poner(c, SH_BT_NADIE); } }

static int copiar_txt(char *d, unsigned max, const char *t, unsigned n)
{
    if (!d || !max || !cg_en_app(d, max)) return 0;
    if (n > max - 1) n = max - 1;
    memcpy(d, t, n); d[n] = 0;
    return (int)n;
}
int t_red_texto(int c, char *d, unsigned max)
{
    const char *t; unsigned n = 0;
    if (!viva_() || !mio_(c)) return 0;
    t = red_texto(c);
    while (t[n] && n < 63) n++;
    return copiar_txt(d, max, t, n);
}
int t_red_cabeceras(int c, char *d, unsigned max)
{
    static char t[RED_CAB];
    if (!viva_() || !mio_(c)) return 0;
    return copiar_txt(d, max, t, (unsigned)red_cabeceras(c, t, sizeof t));
}
int t_red_recibir(int c, void *d, unsigned max)
{
    int k;
    if (!viva_() || !mio_(c) || !d || !max) return 0;
    if (max > TROZO) max = TROZO;
    if (!cg_en_app(d, max)) return 0;
    k = red_recibir(c, cop, max);
    if (k > 0) memcpy(d, cop, (unsigned)k);
    return k;
}
int t_red_enviar(int c, const void *d, unsigned n)
{
    if (!viva_() || !mio_(c) || !d || !n) return 0;
    if (n > TROZO) n = TROZO;
    if (!cg_legible_n(d, n)) return 0;
    memcpy(cop, d, n);                                 /* la copia: lo que se mira es lo que se manda */
    return red_enviar(c, cop, n);
}
/* la capacidad que se suma a CG_CAPS (shell/cargador.h): solo con radio (el W580) */
unsigned cg_red_caps(void) { return sh_red_hay() ? (unsigned)W5_CAP_RED : 0u; }
