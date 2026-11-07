/* cg18.c — (1.8, 26-oct) las envolturas del grupo MÚSICA POR LA RED Y PÍXELES (api/cargable_18.h; el contrato para las apps está en
 * cargables/sdk/w5_musica_red.h). Mismas reglas que cg15.c: lo que entra de la app se VALIDA (cadenas con cg_copiar_cad, estructuras con
 * cg_en_app, los píxeles con cg_legible_n) y se COPIA; nada acaba en pánico. La cola y la descarga son del SISTEMA (shell/musica_red.c): la
 * música que la app pone SIGUE al descargarla (como mu_tocar_ruta, 1.5). Sin radio o sin el motor MP3 (el PC) contesta W5_NO_DISPONIBLE. */
#include "cargador.h"
#include "musica_red.h"
#include "red_sis.h"

int hw_mp3_info(HwMp3Info *i) __attribute__((weak));

typedef char w5_mured_tams[W5_MURED_MAX == MRED_MAX && W5_MURED_URL == MRED_URL && W5_MURED_TITULO == MRED_TITULO && W5_MURED_ARTISTA == MRED_ARTISTA &&
                           (int)W5_MURED_FALLO == (int)MRED_FALLO && (int)W5_MURED_ENTERA == (int)MRED_ENTERA ? 1 : -1];

int t_mu_red_vaciar(void)
{
    CG_PIDE(W5_CAP_MUSICA, W5_ERROR);
    if (!sh_mured_hay()) return W5_NO_DISPONIBLE;
    sh_mured_vaciar();
    return W5_OK;
}
int t_mu_red_poner(const char *url, const char *titulo, const char *artista, unsigned dur_ms)
{
    static char u[MRED_URL], t[MRED_TITULO], a[MRED_ARTISTA];
    int k;
    CG_PIDE(W5_CAP_MUSICA, W5_ERROR);
    if (!sh_mured_hay()) return W5_NO_DISPONIBLE;
    if (!url || !cg_copiar_cad(u, sizeof u, url) || !u[0]) return W5_ERROR;           /* 0 = no es de la app o no cabe */
    t[0] = a[0] = 0;
    if (titulo && !cg_copiar_cad(t, sizeof t, titulo)) return W5_ERROR;
    if (artista && !cg_copiar_cad(a, sizeof a, artista)) return W5_ERROR;
    k = sh_mured_poner(u, t, a, dur_ms);
    return k < 0 ? W5_ERROR : k;
}
int t_mu_red_tocar(int i)
{
    int r;
    CG_PIDE(W5_CAP_MUSICA, W5_ERROR);
    if (!sh_mured_hay()) return W5_NO_DISPONIBLE;
    r = sh_musica_tocar_red(i, 0);
    return r == HW_OK ? W5_OK : r == HW_NO_DISPONIBLE ? W5_NO_DISPONIBLE : W5_ERROR;
}
int t_mu_red_info(W5MuRed *i)
{
    MuRedInfo r;
    MuInfo m;
    HwMp3Info mi;
    u32 kbps = 128;
    CG_PIDE(W5_CAP_MUSICA, W5_ERROR);
    if (!i || !cg_en_app(i, sizeof *i)) return W5_ERROR;
    memset(i, 0, sizeof *i);
    i->actual = -1;
    if (!sh_mured_hay()) return W5_OK;
    sh_mured_info(&r);
    sh_musica_info(&m);
    if (hw_mp3_info && hw_mp3_info(&mi) == HW_OK && mi.kbps) kbps = mi.kbps;
    i->actual = r.actual; i->n = r.n; i->fase = r.fase; i->red = r.red; i->reintentos = r.reintentos; i->tocando = r.tocando;
    i->activa = m.red; i->http = r.http; i->recibidos = r.recibidos; i->total = r.total;
    i->colchon = r.colchon; i->colchon_ms = r.colchon * 8u / kbps;
    i->pos_ms = m.red ? m.pos_ms : 0; i->dur_ms = r.dur_ms; i->esperas = r.esperas; i->ms_esperando = r.ms_esperando;
    memcpy(i->porque, r.porque, sizeof i->porque < sizeof r.porque ? sizeof i->porque : sizeof r.porque);
    i->porque[sizeof i->porque - 1] = 0;
    return W5_OK;
}
int t_mu_red_pista(int i, W5MuRedPista *p)
{
    const char *u, *t, *a;
    u32 d;
    CG_PIDE(W5_CAP_MUSICA, W5_ERROR);
    if (!p || !cg_en_app(p, sizeof *p)) return W5_ERROR;
    if (sh_mured_pista(i, &u, &t, &a, &d)) return W5_ERROR;
    memset(p, 0, sizeof *p);
    memcpy(p->url, u, strlen(u) < sizeof p->url ? strlen(u) : sizeof p->url - 1);
    memcpy(p->titulo, t, strlen(t) < sizeof p->titulo ? strlen(t) : sizeof p->titulo - 1);
    memcpy(p->artista, a, strlen(a) < sizeof p->artista ? strlen(a) : sizeof p->artista - 1);
    p->dur_ms = d;
    return W5_OK;
}
void t_li_pixeles(W5Lienzo l, int x, int y, int w, int h, const void *rgb565)
{
    Imagen im;
    if (!l || !cg_li_vale(l) || !rgb565 || w <= 0 || h <= 0 || w > UI_LADO_MAX || h > UI_LADO_MAX) return;
    if (!cg_legible_n(rgb565, (size_t)w * (size_t)h * 2u)) return;
    im.w = (u16)w; im.h = (u16)h; im.pix = (const Color *)rgb565; im.transparente = 0; im.opaca = 1;   /* opaca: filas enteras con memcpy (sirve sin alinear) */
    li_imagen((Lienzo *)l, x, y, &im);
}
