/* cg12_b.c — (1.2, pista APX) las envolturas del grupo B de la tabla (api/cargable12_b.h): MÚSICA y LUCES. Mismas reglas
 * que cg_tabla.c: todo puntero de la app se comprueba (cg_en_app / cg_legible) o se copia; nada del sistema sale por
 * puntero. Los servicios (shell/musica.c, shell/luces.c, api/luces.c) no cambian: aquí solo se traduce.
 * Lo que una app puede dejar a medias, y suelta cg12_b_soltar(): la retención de la pantalla del Walkman y un efecto
 * de luces que ella lanzó. La MÚSICA que suena NO se para al descargar: es justo lo que hace el servicio. */
#include "cargador.h"
#include "luces.h"

typedef char w5_mu_iguales[(int)W5_MU_SONANDO == (int)MU_SONANDO && (int)W5_MU_PAUSA == (int)MU_PAUSA && (int)W5_MU_SAL_ZUMBADOR == (int)MU_SAL_ZUMBADOR &&
                           (int)W5_MU_MODOS == (int)MU_MODOS && (int)W5_MU_REPS == (int)MU_REPS && (int)W5_MU_T_DSP == (int)MU_T_DSP &&
                           W5_MU_RUTA == MU_RUTA ? 1 : -1];
typedef char w5_led_iguales[(int)W5_LEDS == (int)HW_LEDS && (int)W5_LED_TECLADO_1 == (int)HW_LED_TECLADO_1 ? 1 : -1];
typedef char w5_ret_iguales[W5_OK == HW_OK && W5_ERROR == HW_ERROR && W5_NO_DISPONIBLE == HW_NO_DISPONIBLE ? 1 : -1];

static u8 mu_retenida;                     /* la app dijo mu_pantalla(1) y no la ha soltado */
static u8 lu_lanzado;                      /* la app lanzó un efecto de luces */

/* copia una cadena del sistema al búfer de la app, con tope y sin partir un carácter UTF-8; da la longitud o W5_ERROR */
static int copiar(char *buf, unsigned max, const char *s)
{
    unsigned n = 0;
    if (!max || !cg_en_app(buf, max)) return W5_ERROR;
    if (!s) s = "";
    while (s[n] && n + 1 < max) n++;
    if (s[n]) while (n && ((u8)s[n] & 0xC0) == 0x80) n--;      /* cortada: que no quede medio carácter */
    memcpy(buf, s, n);
    buf[n] = 0;
    return (int)n;
}

/* ---- MÚSICA ---------------------------------------------------------------------------------------------------------- */
int t_mu_abrir(void) { sh_musica_abrir(); return sh_musica_n(); }
int t_mu_buscar(void) { CG_PIDE(W5_CAP_MUSICA, W5_ERROR); sh_musica_buscar(); return sh_musica_n(); }
int t_mu_info(W5MuInfo *m)
{
    MuInfo i;
    if (!cg_en_app(m, sizeof *m)) return W5_ERROR;
    sh_musica_info(&i);
    memset(m, 0, sizeof *m);
    m->estado = i.estado; m->salida = i.salida; m->volumen = i.volumen; m->modo = i.modo;
    m->repetir = i.repetir; m->pcm_probado = i.pcm_probado; m->pcm_hay = i.pcm_hay; m->cedida = i.cedida;
    m->guardado = i.guardado; m->pista = i.pista; m->n = i.n;
    m->pos_ms = i.pos_ms; m->dur_ms = i.dur_ms; m->notas = i.notas; m->percusion = i.percusion; m->perdidas = i.perdidas;
    m->pistas_smf = i.pistas_smf; m->tam = i.tam; m->pcm_marcos = i.pcm_marcos; m->pcm_motivo = i.pcm_motivo; m->hz = i.hz;
    return W5_OK;
}
int t_mu_pista(int i, char *ruta, unsigned max)
{
    const MuPista *p = sh_musica_pista(i);
    if (!p) return W5_ERROR;
    if (ruta && copiar(ruta, max, p->ruta) < 0) return W5_ERROR;
    return p->tipo;
}
int t_mu_nombre(int i, char *buf, unsigned max)
{
    if (!sh_musica_pista(i) || ui_arena_libre() < 3072u + 512u) return W5_ERROR;   /* (INT2) con el margen del cuadro: agotarla es PÁNICO */   /* sh_musica_nombre lo arma en la arena del cuadro */
    return copiar(buf, max, sh_musica_nombre(i));
}
int t_mu_tocar(int i) { CG_PIDE(W5_CAP_MUSICA, W5_ERROR); return sh_musica_pista(i) && sh_musica_tocar(i) == HW_OK ? W5_OK : W5_ERROR; }
void t_mu_alternar(void) { CG_PIDE(W5_CAP_MUSICA); sh_musica_alternar(); }
void t_mu_parar(void) { CG_PIDE(W5_CAP_MUSICA); sh_musica_parar(); }
void t_mu_saltar(int d) { CG_PIDE(W5_CAP_MUSICA); if (d) sh_musica_saltar(d > 0 ? 1 : -1); }
int t_mu_volumen(int v) { CG_PIDE(W5_CAP_MUSICA, W5_ERROR); if (v < 0 || v > W5_MU_VOL_MAX) return W5_ERROR; sh_musica_volumen(v); return W5_OK; }
int t_mu_modo(int m) { CG_PIDE(W5_CAP_MUSICA, W5_ERROR); if (m < 0 || m >= MU_MODOS) return W5_ERROR; sh_musica_modo(m); return W5_OK; }
int t_mu_repetir(int r) { CG_PIDE(W5_CAP_MUSICA, W5_ERROR); if (r < 0 || r >= MU_REPS) return W5_ERROR; sh_musica_repetir(r); return W5_OK; }
void t_mu_pantalla(int abierta) { CG_PIDE(W5_CAP_MUSICA); mu_retenida = (u8)!!abierta; sh_musica_pantalla(mu_retenida); }

/* ---- LUCES ----------------------------------------------------------------------------------------------------------- */
int t_lu_leer(W5Luces *l)
{
    HwLuces h;
    int k;
    if (!cg_en_app(l, sizeof *l)) return W5_ERROR;
    hw_luz_leer(&h);
    memset(l, 0, sizeof *l);
    l->presente = h.presente; l->efecto = (u8)!!lu_efecto_activo();
    l->luces = aj.luces; l->color = (u8)(aj.luces_color % SH_LUZ_COLORES); l->latido = aj.luces_latido;
    l->n_colores = SH_LUZ_COLORES; l->n_casa = SH_LUZ_CASA;
    for (k = 0; k < W5_LEDS; k++) l->nivel[k] = h.nivel[k];
    for (k = 0; k < 6; k++) l->rgb[k / 3][k % 3] = h.rgb[k / 3][k % 3];
    return W5_OK;
}
int t_lu_ajuste(int luces, int color, int latido)          /* la lista blanca: estas tres claves y nada más del `aj` */
{ CG_PIDE(W5_CAP_LUCES_EF, W5_ERROR);
    if ((luces != 0 && luces != 1) || (latido != 0 && latido != 1) || color < 0 || color >= SH_LUZ_COLORES) return W5_ERROR;
    sh_luces_ajuste(luces, color, latido);
    return W5_OK;
}
int t_lu_color_nombre(int i, char *buf, unsigned max) { return i >= 0 && i < SH_LUZ_COLORES ? copiar(buf, max, sh_luz_color_nombre[i]) : W5_ERROR; }
int t_lu_casa(int i)
{ CG_PIDE(W5_CAP_LUCES_EF, W5_ERROR);
    int r;
    if (i < 0 || i >= SH_LUZ_CASA) return W5_ERROR;
    if ((r = sh_luces_casa(i)) == HW_OK) lu_lanzado = 1;
    return r;
}
int t_lu_casa_nombre(int i, char *buf, unsigned max) { return i >= 0 && i < SH_LUZ_CASA ? copiar(buf, max, sh_luces_casa_nombre(i)) : W5_ERROR; }

static const char *const lu_sub[2] = { "contacteffects", "systemeffects" };   /* las dos carpetas de efectos del FS original */
static int termina_ble(const char *s, u32 l) { return l >= 5 && !memcmp(s + l - 4, ".ble", 4); }
int t_lu_fabricas(char *buf, unsigned max)
{
    const char *d = sh_luces_dir();
    unsigned usado = 0;
    int cuantos = 0, k;
    if (!max || !cg_en_app(buf, max)) return W5_ERROR;
    buf[0] = 0;
    if (!d) return W5_NO_DISPONIBLE;
    for (k = 0; k < 2; k++) {
        HwEntrada e[8];
        char dir[128];
        u32 desde = 0, ld = (u32)strlen(d), ls = (u32)strlen(lu_sub[k]);
        int n, i;
        if (ld + ls + 2 > sizeof dir) continue;
        memcpy(dir, d, ld); dir[ld] = '/'; memcpy(dir + ld + 1, lu_sub[k], ls + 1);
        while ((n = hw_fs_listar(dir, desde, e, 8)) > 0) {
            for (i = 0; i < n; i++) {
                u32 l = (u32)strlen(e[i].nombre);
                if (e[i].es_dir || !termina_ble(e[i].nombre, l)) continue;
                if (usado + ls + 1 + l + 1 > max) return cuantos;    /* lo que no cabe, fuera (la cuenta es de lo copiado) */
                memcpy(buf + usado, lu_sub[k], ls); usado += ls;
                buf[usado++] = '/';
                memcpy(buf + usado, e[i].nombre, l + 1); usado += l + 1;
                cuantos++;
            }
            desde += (u32)n;
        }
    }
    return cuantos;
}
int t_lu_fabrica(const char *relativo)
{ CG_PIDE(W5_CAP_LUCES_EF, W5_ERROR);
    char r[96];
    u32 n, k;
    int ok = 0, res;
    if (!cg_copiar_cad(r, sizeof r, relativo)) return W5_ERROR;   /* (INT2) copiada byte a byte legible; demasiado larga = no vale */
    n = (u32)strlen(r);
    for (k = 0; k < 2; k++) {                                  /* «<carpeta>/<nombre>.ble», sin más barras ni «..» */
        u32 ls = (u32)strlen(lu_sub[k]), j;
        if (n > ls + 5 && !memcmp(r, lu_sub[k], ls) && r[ls] == '/') {
            ok = 1;
            for (j = ls + 1; j < n; j++) if (r[j] == '/' || r[j] == '\\') ok = 0;
        }
    }
    if (!ok || !termina_ble(r, n)) return W5_ERROR;
    for (k = 0; k + 1 < n; k++) if (r[k] == '.' && r[k + 1] == '.') return W5_ERROR;
    if ((res = sh_luces_fabrica(r)) == HW_OK) lu_lanzado = 1;
    return res;
}
void t_lu_parar(void) { CG_PIDE(W5_CAP_LUCES_EF); sh_luces_parar(); lu_lanzado = 0; }
int t_lu_sonando(char *buf, unsigned max) { return copiar(buf, max, sh_luces_sonando()); }
int t_lu_mantener(void)
{
    if (!lu_efecto_activo()) return 0;
    if (cg_primer_plano()) sh_actividad();                    /* solo con la app a la vista; el efecto se para solo (los de contacto, a los 20 s) */
    return 1;
}

void cg12_b_soltar(void)                   /* al descargar la app: suelta lo que tuviera retenido de este grupo */
{
    if (mu_retenida) { mu_retenida = 0; sh_musica_pantalla(0); }      /* la música, si suena, SIGUE: es del servicio */
    if (lu_lanzado) { lu_lanzado = 0; if (lu_efecto_activo()) sh_luces_parar(); }
}
