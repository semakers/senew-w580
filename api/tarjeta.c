/* La memoria externa como «/tarjeta». Pista T. Ver api/tarjeta.h. Portable.
 * (FATW) Y ESCRIBE: se monta con fat_montar_rw si la tarjeta no está protegida; si venía SUCIA (un corte, una tarjeta
 * sacada a media escritura) se pasa fat_revisar antes de dejar escribir; y al quedar quieta (FATW_QUIETA_MS sin escribir)
 * fat_limpio quita la marca de sucia. docs/fat_escritura.md. */
#include "tarjeta.h"
#include "fat.h"

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);

static FatVol vol;                      /* (~5 KiB: la caché de 8 sectores y los apuntes del lector) */
static u32 gen;                         /* la generación de la tarjeta que se intentó montar (0 = ninguna) */
static signed char fat_e;               /* resultado de aquel montaje */
static u32 errores;
static u8 ultimo = 0xFF;                /* el último estado avisado por el registro */
static u8 ocupada;                      /* hay una operación a medias: el controlador late (hw_perro_latido) DENTRO de una lectura larga y ese
                                         * latido vuelve a entrar en hw_tarjeta_paso(); mientras, ni se monta ni se mira nada */
/* (FATW) la escritura */
#define FATW_QUIETA_MS 800u             /* sin escribir este rato y sin nada abierto: fat_limpio (FSInfo y marca de limpio) */
static u8 protegida_m, arreglos_m, revision_mal;
static u32 ultima_w, cambios_w;
static u8 mapa_rev[2048];               /* el mapa de fat_revisar: 16 384 clústeres por vuelta */
static void tj_soltar(void);            /* una tarjeta que se va (o que cambia) se lleva lo abierto */
static int esc_tj(void *c, u32 lba, u32 n, const u8 *b) { ultima_w = hw_ms(); return hwo_tarjeta_escribir(c, lba, n, b) ? -1 : 0; }
static int vaciar_tj(void *c) { return hwo_tarjeta_vaciar(c) ? -1 : 0; }
static u32 fecha_dos(void)
{
    HwFecha f;
    hw_rtc_leer(&f);
    if (f.anio < 1980 || f.anio > 2107 || !f.mes || f.mes > 12 || !f.dia) return (u32)((20u << 9 | 1u << 5 | 1u) << 16);
    return (u32)((f.anio - 1980u) << 9 | (u32)f.mes << 5 | f.dia) << 16 | ((u32)f.hora << 11 | (u32)f.min << 5 | f.seg / 2u);
}

static void pon(char *d, u32 max, const char *s) { u32 i = 0; while (d[i]) i++; while (*s && i + 1 < max) d[i++] = *s++; d[i] = 0; }
static void pon_n(char *d, u32 max, u32 v) { char t[12]; int n = 11; t[11] = 0; do { t[--n] = (char)('0' + v % 10); v /= 10; } while (v); pon(d, max, t + n); }

/* (28-oct) LA ZONA DE CIERRE (docs/tarjeta_escritura.md §10): al montar con escritura, la busca (o la crea: 6 clústeres marcados malos al final
 * del volumen, uno por bloque de borrado) y se la da al controlador, que escribe 1 sector en cada uno antes de quitarle la tensión: así la M2
 * consolida lo escrito. Con menos de 4 no sirve (la tarjeta guarda 4 bloques en el aire) y se dice. */
#define ZONA_CIERRE_N 6u
static void zona_cierre(void)
{
    static u8 plantilla[512];
    u32 l[ZONA_CIERRE_N]; int k = fat_zona_cierre(&vol, l, ZONA_CIERRE_N, hwo_tarjeta_bloque_sectores());
    if (k >= 4) {
        memset(plantilla, 0, sizeof plantilla); memcpy(plantilla, FAT_CIERRE_FIRMA, sizeof FAT_CIERRE_FIRMA - 1);
        hwo_tarjeta_cierre_zona(l, (u32)k, plantilla);
        char t[96] = "tarjeta: zona de cierre en "; pon_n(t, sizeof t, (u32)k); pon(t, sizeof t, " bloques (lo escrito se consolida antes de quitarle la tension)\n");
        hw_log(t);
    } else {
        hwo_tarjeta_cierre_zona(l, 0, 0);
        char t[96] = "tarjeta: SIN zona de cierre ("; pon_n(t, sizeof t, (u32)(k < 0 ? -k : k)); pon(t, sizeof t, "): lo ultimo escrito puede perderse al apagarla\n");
        hw_log(t);
    }
}

/* ¿Hay una tarjeta con FAT? La monta la primera vez que se pregunta tras entrar (lee el sector 0 y el de arranque). */
static int lista(void)
{
    u32 g = hwo_tarjeta_generacion();
    if (!g) { if (gen) { tj_soltar(); cambios_w++; } gen = 0; vol.montado = 0; return 0; }
    if (g != gen) {
        gen = g;
        tj_soltar(); cambios_w++;
        ocupada++;
        protegida_m = (u8)(hwo_tarjeta_protegida() != 0); arreglos_m = 0; revision_mal = 0;
        fat_e = (signed char)fat_montar_rw(&vol, hwo_tarjeta_leer, protegida_m ? 0 : esc_tj, 0, hwo_tarjeta_sectores(), fecha_dos);
        vol.vaciar = vaciar_tj; vol.latido = hw_perro_latido;   /* (MSW) en lo largo, un latido al perro por trozo */
        if (!fat_e && vol.rw && vol.sucio_al_montar) {      /* (FATW) un corte: revisar ANTES de dejar escribir */
            FatRevision r; int x;
            hw_log("tarjeta: venia SUCIA (un corte o sacada escribiendo): revisandola\n");
            x = fat_revisar(&vol, mapa_rev, sizeof mapa_rev, &r);
            if (x < 0) { revision_mal = 1; hw_log("tarjeta: la revision NO pudo acabar: queda de SOLO LECTURA\n"); }
            else {
                arreglos_m = (u8)(x > 255 ? 255 : x);
                hw_log(x ? "tarjeta: revisada, con arreglos (clusteres sin dueno devueltos, tamanos, nombres a medias)\n" : "tarjeta: revisada, estaba bien\n");
            }
        }
        if (!fat_e && !hw_modo_seguro() && vol.rw && !vol.roto && !revision_mal && !vol.sucio_al_montar) zona_cierre();   /* (28-oct) */
        ocupada--;
        if (fat_e == FAT_E_ES && !hwo_tarjeta_generacion()) { gen = 0; return 0; }   /* se fue mientras se montaba */
    }
    return vol.montado;
}
/* Tras cada operación: si el dispositivo falló, se cuenta y se dice (una línea, no una por sector). */
static void repasar(void)
{
    if (!vol.fallo_es) return;
    vol.fallo_es = 0; errores++;
    hw_log(hwo_tarjeta_generacion() ? "tarjeta: ERROR DE LECTURA (la operacion queda a medias)\n" : "tarjeta: sacada a media lectura\n");
}

void hw_tarjeta_estado(HwTarjeta *t)
{
    const char *m = 0;
    memset(t, 0, sizeof *t);
    if (hw_modo_seguro()) { pon(t->texto, sizeof t->texto, "modo seguro: no se mira"); return; }   /* (AUD8 M1) FUERA, sin montar nada */
    t->lecturas = vol.lecturas; t->aciertos = vol.aciertos; t->errores = errores;
    t->escritos = vol.escrituras; t->errores_w = vol.errores_w; t->arreglos = arreglos_m;
    if (lista()) {
        t->escribible = (u8)hw_tj_escribible(); t->protegida = protegida_m; t->sucia = vol.sucio;
        t->capacidad_kib = vol.nclusters * (vol.spc / 2u); t->libre_kib = vol.libres_ok ? vol.libres * (vol.spc / 2u) : 0;
        t->estado = HW_TJ_LISTA; t->fat = vol.tipo; t->con_mbr = vol.con_mbr;
        t->sectores = hwo_tarjeta_sectores(); t->mib = t->sectores >> 11; t->dormida = (u8)hwo_tarjeta_dormida();
        memcpy(t->etiqueta, vol.etiqueta, sizeof t->etiqueta);
        pon(t->texto, sizeof t->texto, "FAT"); pon_n(t->texto, sizeof t->texto, vol.tipo);
        pon(t->texto, sizeof t->texto, ", "); pon_n(t->texto, sizeof t->texto, t->mib); pon(t->texto, sizeof t->texto, " MiB");
        if (vol.etiqueta[0]) { pon(t->texto, sizeof t->texto, ", "); pon(t->texto, sizeof t->texto, vol.etiqueta); }
        if (!t->escribible) pon(t->texto, sizeof t->texto, protegida_m ? ", protegida" : ", solo lectura");
    } else if (gen) {
        t->estado = HW_TJ_SIN_FAT; t->sectores = hwo_tarjeta_sectores(); t->mib = t->sectores >> 11;
        pon(t->texto, sizeof t->texto, fat_motivo(fat_e));
    } else if (hwo_tarjeta_preparando()) {              /* (I1) la espera del CED, por pasos: la entrada lo dice mientras dura */
        t->estado = HW_TJ_PREPARANDO;
        pon(t->texto, sizeof t->texto, "preparándola…");
    } else if (hwo_tarjeta_mala(&m)) {
        t->estado = HW_TJ_MALA;
        pon(t->texto, sizeof t->texto, m ? m : "no se pudo preparar");
    } else pon(t->texto, sizeof t->texto, "no hay tarjeta");
}
static u32 cambios;
u32 hw_tarjeta_cambios(void) { return cambios; }
int hw_tarjeta_paso(void)
{
    static u32 vista;                   /* la situación ya avisada: la generación, o «mala», o nada */
    HwTarjeta t; u32 clave;
    if (ocupada) return ultimo;
    if (hw_modo_seguro()) return HW_TJ_FUERA;               /* (AUD8 M1) en modo seguro la tarjeta no existe: ni se detecta ni se monta */
    if (vol.montado && vol.sucio && !vol.abiertos && hw_ms() - ultima_w > FATW_QUIETA_MS && hwo_tarjeta_generacion() == gen) {
        ocupada++; if (fat_limpio(&vol)) hw_log("tarjeta: no se pudo dejar LIMPIA (queda marcada sucia: se revisara al volver a montarla)\n"); ocupada--;
    }
    hwo_tarjeta_paso();
    clave = hwo_tarjeta_generacion();
    if (!clave && hwo_tarjeta_mala(0)) clave = 0xFFFFFFFFu;
    if (!clave && hwo_tarjeta_preparando()) clave = 0xFFFFFFFEu;   /* (I1) */
    if (clave == vista && ultimo != 0xFF) return ultimo;    /* lo de casi siempre: nada nuevo */
    vista = clave; cambios++;
    hw_tarjeta_estado(&t);                                  /* (monta la FAT de una tarjeta recién entrada) */
    if (t.estado != HW_TJ_FUERA || ultimo != 0xFF) {
        hw_log(t.estado == HW_TJ_FUERA ? "tarjeta: fuera\n" : t.estado == HW_TJ_LISTA ? (t.escribible ? "tarjeta: montada en /tarjeta, lectura y escritura (" : "tarjeta: montada en /tarjeta, solo lectura (") :
               t.estado == HW_TJ_SIN_FAT ? "tarjeta: SIN FAT VALIDA (" : t.estado == HW_TJ_PREPARANDO ? "tarjeta: metida, preparandola (" : "tarjeta: NO SIRVE (");
        if (t.estado != HW_TJ_FUERA) { hw_log(t.texto); hw_log(")\n"); }
    }
    ultimo = t.estado;
    return t.estado;
}

/* ---- para hw_fs.c -------------------------------------------------------------------------------------------- */
const char *tj_ruta(const char *r)
{
    static const char pre[] = HW_FS_TARJETA;
    unsigned i;
    for (i = 0; pre[i]; i++) if (r[i] != pre[i]) return 0;
    if (hw_modo_seguro()) return 0;                         /* (AUD8 M1) «/tarjeta…» no existe en modo seguro */
    if (r[i] == 0) return "/";
    if (r[i] == '/') return r + i;
    return r[i] == ' ' && r[i + 1] == '(' ? "" : 0;          /* «/tarjeta (motivo)»: la carpeta que solo lleva el mensaje */
}
int tj_entrada(char nombre[64])
{
    HwTarjeta t;
    if (hw_modo_seguro()) return 0;                         /* (AUD8 M1) ni una fila en la raíz */
    hw_tarjeta_estado(&t);
    if (t.estado == HW_TJ_FUERA) return 0;
    nombre[0] = 0; pon(nombre, 64, HW_FS_TARJETA + 1);
    if (t.estado != HW_TJ_LISTA) { pon(nombre, 64, " ("); pon(nombre, 61, t.texto); pon(nombre, 64, ")"); }
    return 1;
}
int tj_listar(const char *r, u32 desde, HwEntrada *e, u32 max)
{
    int n;
    if (!*r) return 0;                                      /* la carpeta del motivo: vacía */
    if (!lista()) return HW_ERROR;
    ocupada++; n = fat_listar(&vol, r, desde, e, max); ocupada--;
    repasar();
    return n;
}
u32 tj_tam(const char *r)
{
    u32 n;
    if (!*r || !lista()) return 0;
    ocupada++; n = fat_tam(&vol, r); ocupada--;
    repasar();
    return n;
}
u32 tj_leer_desde(const char *r, u32 desde, u8 *buf, u32 max)
{
    u32 n;
    if (!*r || !lista()) return 0;
    ocupada++; n = fat_leer_desde(&vol, r, desde, buf, max); ocupada--;
    repasar();
    return n;
}

/* ==== (FATW) escribir ======================================================================================== */
static u8 motivo_tj;
static struct { u8 activa; FatEsc f; char ruta[224]; u32 hecho, total; } cp;
static struct { u8 activa, dueno; FatEsc f; u32 n; u8 b[8192]; } rx;   /* b: lo recibido se junta en trozos de 8 KiB (MTP y la serie
                                                    llegan a pedazos de unos cientos de bytes: sin esto, una escritura por sector) */
static void tj_soltar(void) { cp.activa = 0; rx.activa = 0; }   /* el FatEsc era del volumen que se va: no se toca (la revisión lo devolverá) */
int hw_tj_motivo(void) { return motivo_tj; }
u32 hw_tj_cambios(void) { return cambios_w; }
int hw_tj_escribible(void) { return !hw_modo_seguro() && lista() && vol.rw && !vol.roto && !revision_mal && !vol.sucio_al_montar; }
static int traducir(int e)
{
    motivo_tj = e == 0 ? HW_W6F_M_NADA : e == FAT_E_SOLO_LECTURA ? HW_W6F_M_SOLO_LECTURA : e == FAT_E_NO_HAY ? HW_W6F_M_NO_HAY :
                e == FAT_E_EXISTE ? HW_W6F_M_EXISTE : e == FAT_E_LLENO ? HW_W6F_M_LLENO : e == FAT_E_NOMBRE ? HW_W6F_M_NOMBRE :
                e == FAT_E_NO_VACIA ? HW_W6F_M_NO_VACIA : e == FAT_E_ES_DIR ? HW_W6F_M_ES_DIR : e == FAT_E_NO_DIR ? HW_W6F_M_NO_DIR :
                e == FAT_E_DENTRO ? HW_W6F_M_DENTRO : e == FAT_E_OCUPADA ? HW_W6F_M_OCUPADA : HW_W6F_M_FLASH;
    return e == 0 ? HW_OK : HW_ERROR;
}
static int puede(void)
{
    if (!hw_tj_escribible()) { motivo_tj = lista() ? HW_W6F_M_SOLO_LECTURA : HW_W6F_M_SIN_ZONA; return 0; }
    return 1;
}
static int nombre_ok(const char *r)                         /* el último trozo cabe en HwEntrada, como en la Memoria */
{
    const char *b = r, *p; u32 n = 0;
    for (p = r; *p; p++) if (*p == '/') b = p + 1;
    while (b[n]) n++;
    if (!n || n > HW_W6F_MAX_NOMBRE) { motivo_tj = HW_W6F_M_NOMBRE; return 0; }
    return 1;
}
static int tras(int e) { cambios_w++; repasar(); if (vol.roto) hw_log("tarjeta: una ESCRITURA fallo: no se escribe mas hasta volver a meterla\n"); return traducir(e); }
#define OP(x) do { int e_; ocupada++; e_ = (x); ocupada--; return tras(e_); } while (0)
int hw_tj_info(const char *ruta, HwW6fInfo *i)
{
    u32 t; int d;
    memset(i, 0, sizeof *i);
    if (!lista()) return HW_NO_DISPONIBLE;
    if (!ruta[0] || (ruta[0] == '/' && !ruta[1])) { i->es_dir = 1; return HW_OK; }
    ocupada++; d = fat_info(&vol, ruta, &t, &d) ? d : -1; ocupada--;
    if (d < 0) return HW_NO_DISPONIBLE;
    i->es_dir = (u8)d; i->tam = t;
    return HW_OK;
}
int hw_tj_guardar(const char *ruta, const u8 *d, u32 n) { if (!puede()) return HW_NO_DISPONIBLE; if (!nombre_ok(ruta)) return HW_ERROR; OP(fat_guardar(&vol, ruta, d, n)); }
int hw_tj_mkdir(const char *ruta) { if (!puede()) return HW_NO_DISPONIBLE; if (!nombre_ok(ruta)) return HW_ERROR; OP(fat_mkdir(&vol, ruta)); }
int hw_tj_borrar(const char *ruta) { if (!puede()) return HW_NO_DISPONIBLE; OP(fat_borrar(&vol, ruta)); }
int hw_tj_renombrar_r(const char *vieja, const char *nueva, int reemplazar)
{
    int e;
    if (!puede()) return HW_NO_DISPONIBLE;
    if (!nombre_ok(nueva)) return HW_ERROR;
    ocupada++;
    e = fat_renombrar(&vol, vieja, nueva);
    if (e == FAT_E_EXISTE && reemplazar) {                  /* encima de un FICHERO: se quita y se renombra (no es atómico: §4) */
        u32 t; int d = 0;
        if (fat_info(&vol, nueva, &t, &d) && !d && fat_info(&vol, vieja, &t, &d) && !d && (e = fat_borrar(&vol, nueva)) == 0) e = fat_renombrar(&vol, vieja, nueva);
        else if (e == FAT_E_EXISTE && d) e = FAT_E_ES_DIR;
    }
    ocupada--;
    return tras(e);
}
u32 hw_tj_capacidad_kib(void) { return lista() ? vol.nclusters * (vol.spc / 2u) : 0; }
u32 hw_tj_libre(void)
{
    u32 l, b;
    if (!hw_tj_escribible()) return 0;
    ocupada++; l = fat_libres(&vol); ocupada--;
    b = fat_bytes_cluster(&vol);
    return l > 0xFFFFFFFFu / b ? 0xFFFFFFFFu : l * b;
}
static int sitio_para(u32 n) { u32 b = fat_bytes_cluster(&vol), l; ocupada++; l = fat_libres(&vol); ocupada--; return (n + b - 1) / b <= l; }
/* copiar a la tarjeta POR PASOS: el destino vale lo de antes hasta el final (FAT_ESC_REEMPLAZAR / NUEVO) */
int hw_tj_copiar(const char *destino, const char *ruta_fs, int reemplazar)
{
    u32 n, t; int e;
    if (cp.activa || rx.activa) { motivo_tj = HW_W6F_M_OCUPADA; return HW_ERROR; }
    if (!puede()) return HW_NO_DISPONIBLE;
    if (!nombre_ok(destino)) return HW_ERROR;
    for (n = 0; ruta_fs[n]; n++) ;
    if (n >= sizeof cp.ruta) { motivo_tj = HW_W6F_M_NOMBRE; return HW_ERROR; }
    if (hw_fs_mirar(ruta_fs, &t) != HW_OK || (cp.total = hw_fs_tam(ruta_fs)) != t) { motivo_tj = HW_W6F_M_FUENTE; return HW_ERROR; }
    if (!sitio_para(cp.total)) { motivo_tj = HW_W6F_M_LLENO; return HW_ERROR; }   /* (reemplazar: el viejo vale hasta el final) */
    ocupada++; e = fat_esc_abrir(&vol, &cp.f, destino, reemplazar ? FAT_ESC_REEMPLAZAR : FAT_ESC_NUEVO); ocupada--;
    if (e) return tras(e);
    memcpy(cp.ruta, ruta_fs, n + 1); cp.hecho = 0; cp.activa = 1; motivo_tj = HW_W6F_M_NADA;
    return HW_OK;
}
int hw_tj_paso(u32 max, u32 *hecho, u32 *total)
{
    static u8 trozo[4096];
    u32 hasta; int e = 0;
    if (!cp.activa) { motivo_tj = HW_W6F_M_CANCELADA; return HW_ERROR; }
    if (!lista() || !vol.rw) { cp.activa = 0; motivo_tj = HW_W6F_M_SIN_ZONA; return HW_ERROR; }
    hasta = cp.hecho + (max < sizeof trozo ? (u32)sizeof trozo : max);
    while (cp.hecho < cp.total && cp.hecho < hasta && !e) {
        u32 n = cp.total - cp.hecho < sizeof trozo ? cp.total - cp.hecho : (u32)sizeof trozo, l = hw_fs_leer_desde(cp.ruta, cp.hecho, trozo, n);
        if (l != n) { ocupada++; fat_esc_abortar(&vol, &cp.f); ocupada--; cp.activa = 0; motivo_tj = HW_W6F_M_FUENTE; cambios_w++; return HW_ERROR; }
        ocupada++; e = fat_esc_mas(&vol, &cp.f, trozo, n); ocupada--;
        cp.hecho += n;
    }
    if (hecho) *hecho = cp.hecho;
    if (total) *total = cp.total;
    if (e) { ocupada++; fat_esc_abortar(&vol, &cp.f); ocupada--; cp.activa = 0; return tras(e); }
    if (cp.hecho < cp.total) return 1;
    cp.activa = 0;
    OP(fat_esc_cerrar(&vol, &cp.f));
}
void hw_tj_cancelar(void) { if (cp.activa) { if (lista()) { ocupada++; fat_esc_abortar(&vol, &cp.f); ocupada--; } cp.activa = 0; cambios_w++; } }
int hw_tj_ocupada(void) { return cp.activa; }
/* recibir a trozos, con dueño (SER / MTP): el fichero NO existe hasta cerrar; abortar no deja nada */
int hw_tj_rx_abrir_d(const char *ruta, int dueno)
{
    int e;
    if (rx.activa || cp.activa) { motivo_tj = HW_W6F_M_OCUPADA; return HW_ERROR; }
    if (!puede()) return HW_NO_DISPONIBLE;
    if (!nombre_ok(ruta)) return HW_ERROR;
    ocupada++; e = fat_esc_abrir(&vol, &rx.f, ruta, FAT_ESC_REEMPLAZAR); ocupada--;
    if (e) return tras(e);
    rx.n = 0; rx.activa = 1; rx.dueno = (u8)dueno; motivo_tj = HW_W6F_M_NADA;
    return HW_OK;
}
static int rx_mia(int dueno) { if (rx.activa && rx.dueno == dueno && lista()) return 1; motivo_tj = rx.activa ? HW_W6F_M_OCUPADA : HW_W6F_M_CANCELADA; return 0; }
static int rx_soltar(void)                                  /* lo juntado, a la FAT */
{
    int e;
    if (!rx.n) return 0;
    ocupada++; e = fat_esc_mas(&vol, &rx.f, rx.b, rx.n); ocupada--;
    rx.n = 0;
    return e;
}
int hw_tj_rx_mas_d(int dueno, const void *d, u32 n)
{
    int e = 0; const u8 *p = d;
    if (!rx_mia(dueno)) return HW_ERROR;
    while (n && !e) {
        u32 k = sizeof rx.b - rx.n < n ? (u32)sizeof rx.b - rx.n : n;
        memcpy(rx.b + rx.n, p, k); rx.n += k; p += k; n -= k;
        if (rx.n == sizeof rx.b) e = rx_soltar();
    }
    if (e) { ocupada++; fat_esc_abortar(&vol, &rx.f); ocupada--; rx.activa = 0; return tras(e); }
    return HW_OK;
}
int hw_tj_rx_destino(int dueno, const char *ruta, int reemplazar)   /* antes de cerrar: el nombre bueno (el «renombrado» del temporal) */
{
    int e;
    if (!rx_mia(dueno)) return HW_ERROR;
    if (!nombre_ok(ruta)) return HW_ERROR;
    ocupada++; e = fat_esc_destino(&vol, &rx.f, ruta, reemplazar ? FAT_ESC_REEMPLAZAR : FAT_ESC_NUEVO); ocupada--;
    return traducir(e);
}
int hw_tj_rx_cerrar_d(int dueno)
{
    int e;
    if (!rx_mia(dueno)) return HW_ERROR;
    rx.activa = 0;
    if ((e = rx_soltar()) != 0) { ocupada++; fat_esc_abortar(&vol, &rx.f); ocupada--; return tras(e); }
    OP(fat_esc_cerrar(&vol, &rx.f));
}
void hw_tj_rx_abortar_d(int dueno) { if (rx.activa && rx.dueno == dueno) { if (lista()) { ocupada++; fat_esc_abortar(&vol, &rx.f); ocupada--; } rx.activa = 0; cambios_w++; } }
int hw_tj_rx_dueno(void) { return rx.activa ? rx.dueno : 0; }
/* para hw_fs.c: escribir y borrar en «/tarjeta/…» */
int tj_escribir(const char *r, const u8 *d, u32 n) { if (!*r) return HW_NO_DISPONIBLE; return hw_tj_guardar(r, d, n); }
int tj_borrar(const char *r) { if (!*r || (r[0] == '/' && !r[1])) return HW_NO_DISPONIBLE; return hw_tj_borrar(r); }
