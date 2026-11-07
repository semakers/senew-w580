/* cg12_c.c — (1.2, pista APX) las envolturas del grupo C de la tabla (api/cargable12_c.h): carpetas, Mis archivos (texto),
 * imágenes que decodifica el sistema, el fondo del standby, el diálogo de pedir un nombre y las notas de la Notas de casa.
 * Mismas reglas que cg_tabla.c: todo puntero de la app se comprueba (cg_en_app / cg_legible) o se copia; nada del sistema
 * sale por puntero (las imágenes salen como MANEJADOR; los textos, copiados a la app o a la arena del cuadro). */
#include "cargador.h"
#include "img.h"
#include <stddef.h>

#define P(x) ((W5Pal)(size_t)(x))
#define MUERE(m, r) do { cg_matar(m); return r; } while (0)

/* una cadena de la app copiada a un búfer del sistema (con tope); 0 si no es legible, está vacía o no cabe */
static int copiar_cad(char *d, size_t cap, const char *s) { return cg_copiar_cad(d, cap, s); }   /* (INT2) byte a byte legible */
EntradaTexto *cg_et_de(W5Et e);                    /* (INT2) shell/cg_tabla.c: con PM, solo del depósito del sistema */

/* ---- carpetas: cualquiera, solo lectura -------------------------------------------------------------------------------- */
static void a_ent(W5Ent *d, const HwEntrada *s)
{
    size_t n;
    memset(d, 0, sizeof *d);
    for (n = 0; n < sizeof d->nombre - 1 && n < sizeof s->nombre && s->nombre[n]; n++) d->nombre[n] = s->nombre[n];
    d->tam = s->es_dir ? 0 : s->tam;
    d->es_dir = s->es_dir ? 1 : 0;
}
int t_fs_contar(const char *dir)
{
    char r[160];
    int n;
    if (!copiar_cad(r, sizeof r, dir)) return W5_ERROR;
    n = hw_fs_contar(r);
    return n < 0 ? W5_NO_DISPONIBLE : n;
}
int t_fs_listar(const char *dir, unsigned desde, W5Ent *e, unsigned max)
{
    char r[160];
    HwEntrada t[8];
    unsigned hechas = 0;
    if (!copiar_cad(r, sizeof r, dir) || !max || max > 256 || !cg_en_app(e, (size_t)max * sizeof *e)) return W5_ERROR;
    while (hechas < max) {
        unsigned pide = max - hechas > 8 ? 8 : max - hechas, i;
        int n = hw_fs_listar(r, desde + hechas, t, pide);
        if (n <= 0) break;
        for (i = 0; i < (unsigned)n; i++) a_ent(&e[hechas + i], &t[i]);
        hechas += (unsigned)n;
        if ((unsigned)n < pide) break;
    }
    return (int)hechas;
}

/* ---- Mis archivos ---------------------------------------------------------------------------------------------------- */
static int ma_mot;                                   /* el motivo de la última ma_escribir (W5_MA_M_*) */
typedef char w5_ma_motivos[(int)W5_MA_M_CANCELADA == (int)HW_USR_M_CANCELADA && (int)W5_MA_M_SIN_ZONA == (int)HW_USR_M_SIN_ZONA
                           && (int)W5_MA_M_LLENO == (int)HW_USR_M_LLENO && W5_MA_MAX_NOMBRE == HW_USR_MAX_NOMBRE ? 1 : -1];
static int nombre_ma(char *d, const char *s)        /* un nombre de Mis archivos de la app, copiado; 0 si no vale */
{
    size_t i;
    if (!copiar_cad(d, HW_USR_MAX_NOMBRE + 1, s) || !d[0]) return 0;
    for (i = 0; d[i]; i++) if (d[i] == '/' || (u8)d[i] < 0x20) return 0;
    return 1;
}
static int es_txt(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && n[l - 4] == '.' && (n[l - 3] | 32) == 't' && (n[l - 2] | 32) == 'x' && (n[l - 1] | 32) == 't';
}
/* (grupo D) con qué fichero abrió el sistema la app; si cg12_d.c no lo exporta todavía, solo se escriben .txt */
const char *cg12_d_argumento(void) __attribute__((weak));
static int es_el_argumento(const char *n)
{
    const char *a = cg12_d_argumento ? cg12_d_argumento() : NULL;
    return a && a[0] && !strcmp(a, n);
}
int t_ma_estado(W5MaEstado *e)
{
    HwUsrEstado u;
    if (!cg_en_app(e, sizeof *e)) return W5_ERROR;
    hw_usr_estado(&u);
    e->montado = u.montado ? 1 : 0; e->escribible = u.escribible ? 1 : 0; e->libre = u.libre; e->ficheros = u.ficheros;
    return W5_OK;
}
int t_ma_listar(unsigned desde, W5Ent *e, unsigned max)
{
    HwUsrEntrada u;
    unsigned i;
    CG_PIDE(W5_CAP_MIS_TEXTOS, W5_ERROR);            /* (INT2) los ficheros del usuario, solo para quien declaró Mis archivos (como ap_pasos, AUD10) */
    if (!max || max > 64 || !cg_en_app(e, (size_t)max * sizeof *e)) return W5_ERROR;
    for (i = 0; i < max && hw_usr_listar(desde + i, &u, 1) == 1; i++) {
        size_t n;
        memset(&e[i], 0, sizeof e[i]);
        for (n = 0; n < sizeof u.nombre && u.nombre[n] && n < sizeof e[i].nombre - 1; n++) e[i].nombre[n] = u.nombre[n];
        e[i].tam = u.tam;
    }
    return (int)i;
}
int t_ma_tam(const char *nombre)
{
    char n[HW_USR_MAX_NOMBRE + 1];
    int t;
    CG_PIDE(W5_CAP_MIS_TEXTOS, W5_ERROR);            /* (INT2) */
    if (!nombre_ma(n, nombre)) return W5_ERROR;
    t = hw_usr_tam(n);
    return t < 0 ? W5_NO_DISPONIBLE : t;
}
int t_ma_leer(const char *nombre, unsigned desde, void *buf, unsigned max)
{
    char n[HW_USR_MAX_NOMBRE + 1];
    int r;
    CG_PIDE(W5_CAP_MIS_TEXTOS, W5_ERROR);            /* (INT2) */
    if (!nombre_ma(n, nombre) || !cg_en_app(buf, max)) return W5_ERROR;
    r = hw_usr_leer(n, desde, buf, max);
    return r == HW_NO_DISPONIBLE ? W5_NO_DISPONIBLE : r < 0 ? W5_ERROR : r;
}
int t_ma_escribir(const char *nombre, const void *d, unsigned tam)
{ CG_PIDE(W5_CAP_MIS_TEXTOS, W5_ERROR);
    char n[HW_USR_MAX_NOMBRE + 1];
    if (!nombre_ma(n, nombre)) { ma_mot = W5_MA_M_NOMBRE; return W5_ERROR; }
    if ((tam && !cg_en_app(d, tam))) return W5_ERROR;
    if (tam > W5_MA_MAX || !(es_txt(n) || es_el_argumento(n))) { ma_mot = W5_MA_M_PROHIBIDO; return W5_ERROR; }
    if (!cg_escritura_permitida()) { ma_mot = W5_MA_M_TOPE; return W5_ERROR; }
    if (hw_usr_escribir(n, d, tam) == HW_OK) { ma_mot = W5_MA_M_NADA; return W5_OK; }
    ma_mot = hw_usr_motivo();
    if (ma_mot == W5_MA_M_NADA) ma_mot = W5_MA_M_SIN_ZONA;   /* HW_NO_DISPONIBLE sin motivo: no hay zona */
    return W5_ERROR;
}
int t_ma_motivo(void) { return ma_mot; }
const char *t_ma_motivo_texto(void)
{
    const char *s;
    if (ui_arena_libre() < 3072u + 512u) return NULL;   /* (INT2) ni un literal del sistema (con PM la app no lo puede leer) ni dejar la arena
                                                       * sin el margen que necesita el cuadro (si se agota, PÁNICO del sistema) */
    if (ma_mot == W5_MA_M_TOPE) s = "Demasiadas escrituras seguidas: espera un minuto y vuelve a probar. El texto sigue aquí.";
    else if (ma_mot == W5_MA_M_PROHIBIDO) s = "Una app solo puede guardar en Mis archivos archivos de texto (.txt) o el archivo con que se abrió, y de hasta 16 KB.";
    else if (ma_mot == W5_MA_M_NADA) s = "Hecho.";
    else {
        HwUsrEstado u;
        hw_usr_estado(&u);
        s = !u.montado ? "Este teléfono no tiene Mis archivos." : sh_usr_motivo();   /* sh_usr_motivo mira hw_usr_motivo(): el de la última escritura */
    }
    return ui_fmt("%s", s);
}

/* ---- el diálogo que pide un nombre (sh_pedir_nombre) ------------------------------------------------------------------- */
static void th_nombre(const char *nombre, void *dato)
{
    CgRet *t = cg_ret_de(dato);
    char *c;
    if (!t || cg_estado() != CG_VIVA || ui_arena_libre() < 3072u + 512u) return;   /* (INT2) sin agotar la arena (pánico) */
    c = ui_fmt("%s", nombre ? nombre : "");          /* el nombre, en la arena (la app la puede leer): nunca el búfer del diálogo */
    cg_llamar(t->fn[0], P(c), P(t->dato), 0, 0);
}
void t_ui_pedir_nombre(const char *titulo, const char *inicial, W5Nombre hecho, void *dato)
{
    char ti[40], in[HW_USR_MAX_NOMBRE + 17];
    if (cg_estado() != CG_VIVA || cg_perfil() != W5_PERFIL_WIDGETS) return;
    if (!hecho || !copiar_cad(ti, sizeof ti, titulo ? titulo : "")) MUERE("ui_pedir_nombre: argumentos que no valen.", );
    if (!inicial) in[0] = 0;
    else if (!cg_legible(inicial)) MUERE("ui_pedir_nombre: argumentos que no valen.", );
    else if (!copiar_cad(in, sizeof in, inicial)) in[0] = 0;   /* uno larguísimo: se empieza vacío */
    sh_pedir_nombre(ti, in, th_nombre, cg_ret((void *)hecho, 0, 0, dato));
}

/* ---- el modo de una entrada de texto ----------------------------------------------------------------------------------- */
typedef char w5_et_modos[(int)W5_ET_ABC == (int)ET_ABC && (int)W5_ET_ABC_INICIAL == (int)ET_Abc && (int)W5_ET_MAYUS == (int)ET_MAYUS
                         && (int)W5_ET_123 == (int)ET_123 ? 1 : -1];
int t_et_poner_modo(W5Et e, int modo)
{
    EntradaTexto *t = cg_et_de(e);                   /* (INT2) el MISMO control que et_*: con PM la entrada vive en el depósito del sistema
                                                      * (la comprobación de antes, «en la memoria de la app», cerraba Notas con PM) */
    if (!t) return W5_ERROR;                         /* cg_et_de ya ha cerrado la app diciéndolo */
    if (modo < W5_ET_ABC || modo > W5_ET_123) return W5_ERROR;
    t->modo = (u8)modo; t->version++;
    return W5_OK;
}

/* ---- las notas, en el almacén: SOLO las claves de la Notas de casa (NV_NOTAS_ORDEN, NV_NOTA + hueco) -------------------- */
typedef char w5_nt_claves[W5_NT_N == 12 && NV_NOTA == NV_NOTAS_ORDEN + 1 ? 1 : -1];
static int nt_clave(int hueco, u16 *k, u32 *max)
{
    if (hueco == W5_NT_ORDEN) { *k = NV_NOTAS_ORDEN; *max = 1 + W5_NT_N; return 1; }
    if (hueco >= 0 && hueco < W5_NT_N) { *k = (u16)(NV_NOTA + hueco); *max = W5_NT_TAM; return 1; }
    return 0;
}
int t_nt_leer(int hueco, void *buf, unsigned max)
{
    u16 k; u32 m; u8 t[W5_NT_TAM];
    int n;
    CG_PIDE(W5_CAP_NOTAS, W5_ERROR);                 /* (INT2) las notas del usuario, solo para quien declaró W5_CAP_NOTAS */
    if (!nt_clave(hueco, &k, &m) || (max && !cg_en_app(buf, max))) return W5_ERROR;
    n = hw_cargar(k, t, m);
    if (n < 0) return W5_NO_DISPONIBLE;
    {
        u32 c = (u32)n < m ? (u32)n : m;             /* lo que trajo hw_cargar (copia hasta m) */
        if (c > max) c = max;
        if (c) memcpy(buf, t, c);
    }
    return n;
}
int t_nt_guardar(int hueco, const void *d, unsigned n)
{ CG_PIDE(W5_CAP_NOTAS, W5_ERROR);
    u16 k; u32 m; u8 t[W5_NT_TAM];
    int r;
    if (!nt_clave(hueco, &k, &m) || n > m || (n && !cg_en_app(d, n))) return W5_ERROR;
    if (!cg_escritura_permitida()) return W5_ERROR;
    if (n) memcpy(t, d, n);                           /* copiado: la flash se escribe desde el sistema */
    r = hw_guardar(k, t, n);
    return r == HW_OK ? W5_OK : r == HW_NO_DISPONIBLE ? W5_NO_DISPONIBLE : W5_ERROR;
}
int t_nt_olvidar(int hueco)
{ CG_PIDE(W5_CAP_NOTAS, W5_ERROR);
    u16 k; u32 m;
    int r;
    if (!nt_clave(hueco, &k, &m) || !cg_escritura_permitida()) return W5_ERROR;
    r = hw_olvidar(k);
    return r == HW_OK ? W5_OK : r == HW_NO_DISPONIBLE ? W5_NO_DISPONIBLE : W5_ERROR;
}
int t_nt_escribible(void) { HwNvEstado e; hw_nv_estado(&e); return e.escribible ? 1 : 0; }

/* ---- imágenes: las decodifica el sistema; la app tiene un número ------------------------------------------------------- */
#define IM_N 2
static struct { UiGif *g; int id; u8 animado; } im[IM_N];
static int im_seq;
typedef char w5_im_formatos[(int)W5_IM_GIF == (int)UI_F_GIF && (int)W5_IM_PNG == (int)UI_F_PNG && (int)W5_IM_JPEG == (int)UI_F_JPEG ? 1 : -1];
static int im_hueco(int h) { int k = (h & 3) - 1; return h > 0 && k >= 0 && k < IM_N && im[k].g && im[k].id == h ? k : -1; }
int t_im_abrir(const char *ruta)
{ CG_PIDE(W5_CAP_IMAGENES, W5_ERROR);
    char r[160];
    int k;
    UiGif *g;
    if (!copiar_cad(r, sizeof r, ruta)) return W5_ERROR;
    for (k = 0; k < IM_N && im[k].g; k++) ;
    if (k == IM_N) return W5_IM_E_MUCHAS;
    if (!(g = ui_gif_abrir(r)))
        return ui_img_error == IMG_E_NO_SOPORTADO ? W5_IM_E_VARIANTE : ui_img_error == IMG_E_MEMORIA || ui_img_error == IMG_E_GRANDE ? W5_IM_E_MEMORIA
             : ui_img_error == IMG_E_TIEMPO ? W5_IM_E_TIEMPO : W5_IM_E_ROTO;
    im_seq = (im_seq + 1) & 0x3FFFFF;
    im[k].g = g; im[k].id = (im_seq << 2) | (k + 1); im[k].animado = 0;
    return im[k].id;
}
int t_im_info(int h, W5ImInfo *i)
{
    int k = im_hueco(h);
    UiGif *g;
    if (k < 0 || !cg_en_app(i, sizeof *i)) return W5_ERROR;
    g = im[k].g;
    i->w = (unsigned short)g->im.w; i->h = (unsigned short)g->im.h; i->w_orig = g->w_orig; i->h_orig = g->h_orig;
    i->formato = g->formato; i->reduccion = g->reduccion ? g->reduccion : 1; i->parcial = g->parcial ? 1 : 0;
    i->animado = im[k].animado;
    return W5_OK;
}
int t_im_siguiente(int h)
{
    int k = im_hueco(h), ms;
    if (k < 0) return 0;
    ms = ui_gif_siguiente(im[k].g);
    if (ms) im[k].animado = 1;
    return ms;
}
void t_im_pintar(W5Lienzo l, int h, int x, int y) { int k = im_hueco(h); if (k >= 0 && l && cg_li_vale(l)) li_imagen((Lienzo *)l, x, y, &im[k].g->im); }   /* (INT2) lienzo validado como los li_* (AUD9 C1) */
void t_im_cerrar(int h) { int k = im_hueco(h); if (k >= 0) { ui_gif_cerrar(im[k].g); im[k].g = NULL; im[k].id = 0; } }

/* ---- el fondo del standby ------------------------------------------------------------------------------------------------ */
int t_ap_poner_fondo(const char *ruta)
{ CG_PIDE(W5_CAP_IMAGENES, W5_ERROR);
    char r[160];
    if (cg_estado() != CG_VIVA || cg_perfil() != W5_PERFIL_WIDGETS) return W5_ERROR;
    if (!copiar_cad(r, sizeof r, ruta)) return W5_ERROR;
    if (strlen(r) > sizeof aj.fondo - 1) { sh_poner_fondo(r); return W5_NO_DISPONIBLE; }   /* lo dice sh_poner_fondo y no toca nada */
    if (!r[0]) { sh_poner_fondo(""); return W5_OK; }
    if (!hw_fs_tam(r)) return W5_ERROR;               /* solo una imagen que existe */
    sh_poner_fondo(r);
    return sh_fondo() ? W5_OK : W5_ERROR;
}

void cg12_c_soltar(void)                             /* al descargar la app: suelta lo que tuviera retenido de este grupo */
{
    int k;
    for (k = 0; k < IM_N; k++) if (im[k].g) { ui_gif_cerrar(im[k].g); im[k].g = NULL; im[k].id = 0; }
    ma_mot = W5_MA_M_NADA;
}
