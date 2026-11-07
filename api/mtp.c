/* mtp.c — el RESPONDEDOR MTP sobre el tubo de api/usb.h y la Memoria (W6F). Pista MTP; docs/mtp.md; contrato §3.
 *
 * LO QUE SE SIGUE (y de dónde):
 *  - PTP = «PIMA 15740:2000» y su transporte USB («USB Still Image Capture Device Definition 1.0», §6: contenedores de orden,
 *    datos, respuesta y evento, cabecera de 12 bytes: longitud u32, tipo u16, código u16, id de transacción u32; little-endian);
 *  - MTP 1.1 (USB-IF, «Media Transfer Protocol v.1.1», 2011): las operaciones 0x98xx de propiedades de objeto (Apéndice D),
 *    los tipos de datos (§3.2: UINT16 0x0004, UINT32 0x0006, UINT64 0x0008, UINT128 0x000A, STR 0xFFFF), las cadenas (§3.2.3:
 *    un byte con el número de caracteres UCS-2 CONTANDO el nulo, o 0 si está vacía), el ObjectInfo (§5.3.1), el StorageInfo
 *    (§5.2.2), las fechas «AAAAMMDDThhmmss» (§3.2.5) y los códigos de respuesta (Apéndice F);
 *  - el comportamiento de los iniciadores reales, por lo que hacen libmtp (src/libmtp.c: GetObjectPropList con
 *    handle 0xFFFFFFFF y profundidad 0xFFFFFFFF si el dispositivo la anuncia; si no, GetObjectHandles(0xFFFFFFFF, 0, 0) =
 *    TODOS los objetos y GetObjectInfo de cada uno; SendObjectInfo + SendObject para subir) y el respondedor de Android
 *    (frameworks/av/media/mtp/MtpServer.cpp: el SendObjectInfo pendiente se guarda hasta el SendObject; el padre 0 y
 *    0xFFFFFFFF de SendObjectInfo son la raíz; las propiedades que da por defecto, que son las de aquí). [inferido: de
 *    memoria de ese código, no releído hoy: esta máquina no tiene libmtp instalado].
 *
 * CÓMO VA: un autómata de transacción (LIBRE → orden → [datos de la computadora | trabajo a pasos | datos al host] →
 * respuesta). La salida se arma en `sal` (2 KiB) y se vacía al tubo en los trozos que este acepte; lo largo (la lista de
 * objetos, un fichero) se GENERA a medida que el tubo se vacía, con un cursor: nunca se carga un fichero ni una lista entera.
 * Lo recibido (SendObject) va a trozos a W6F (hw_w6f_rx_abrir/mas/cerrar de la pista SER) en un TEMPORAL (MTP_TEMP, en la raíz) que solo se renombra al
 * nombre bueno cuando ha llegado ENTERO y del tamaño anunciado: un corte, un cable fuera, una Memoria llena o un tamaño que
 * no cuadra lo tiran y no queda nada (W6F no compromete nada hasta cerrar; y si se corta la corriente entre el cierre del
 * temporal y el renombrado atómico, el temporal se borra en el siguiente mtp_iniciar).
 *
 * LOS HANDLES: (generación << 16) | id de W6F. El id es estable mientras el objeto vive (W6F lo conserva al renombrar); la
 * generación sube cada vez que el respondedor ve desaparecer ese id (borrado, pisado), así un handle no se reutiliza dentro de
 * la sesión aunque W6F reutilice el id. [sin saber] si algo del teléfono cambia la Memoria con la sesión abierta: la pantalla
 * «Conectado por USB» lo impide (USBD/coordinador); si no, un handle viejo podría señalar un objeto nuevo con el mismo id.
 *
 * (FATW) LA TARJETA, SEGUNDA STORAGE (MTP_STORAGE_TJ, «Tarjeta»), solo mientras hay una tarjeta con FAT (StoreAdded /
 * StoreRemoved al meterla y sacarla). FAT no tiene ids: el respondedor numera los objetos de la tarjeta en una TABLA en RAM
 * (MTP_TJ_MAX entradas: padre + nombre como lo da el listado) que se llena la primera vez que se lista. Dentro, un objeto de la
 * tarjeta tiene un id «virtual» TJ_BASE + índice; su handle lleva el bit 31 (y la época de la tabla), y los de la Memoria van con
 * la generación en 15 bits. Lo recibido NO pasa por un temporal con nombre: va a clústeres sin dueño (hw_tj_rx_*) y la entrada
 * con el nombre bueno se escribe de una vez al final, reemplazando atómicamente al viejo si lo había (docs/fat_escritura.md §7).
 * Con la tarjeta protegida, la storage es de solo lectura. Si la tarjeta cambia FUERA de MTP, la tabla se tira y se avisa con
 * StoreRemoved + StoreAdded, como la Memoria. */
#include "mtp.h"
#include "usb.h"
#include "w6f_sistema.h"
#include "tarjeta.h"                                              /* (FATW) la segunda storage: la tarjeta */
#include "gestion.h"                                              /* (Fase 3b) el canal de gestión de SENUS: operaciones 0x9400.. (api/gestion.h) */

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);
void *memmove(void *d, const void *s, __SIZE_TYPE__ n);
int memcmp(const void *a, const void *b, __SIZE_TYPE__ n);

/* ---- códigos (MTP 1.1, Apéndices C, D, F, G) --------------------------------------------------------------------------- */
enum { C_ORDEN = 1, C_DATOS = 2, C_RESP = 3, C_EVENTO = 4 };
enum {
    OP_DEVICE_INFO = 0x1001, OP_OPEN_SESSION = 0x1002, OP_CLOSE_SESSION = 0x1003, OP_STORAGE_IDS = 0x1004, OP_STORAGE_INFO = 0x1005,
    OP_NUM_OBJECTS = 0x1006, OP_OBJECT_HANDLES = 0x1007, OP_OBJECT_INFO = 0x1008, OP_GET_OBJECT = 0x1009, OP_DELETE_OBJECT = 0x100B,
    OP_SEND_OBJECT_INFO = 0x100C, OP_SEND_OBJECT = 0x100D, OP_DEV_PROP_DESC = 0x1014, OP_DEV_PROP_VALUE = 0x1015,
    OP_MOVE_OBJECT = 0x1019, OP_PARTIAL_OBJECT = 0x101B, OP_PARTIAL_OBJECT_64 = 0x95C1,
    OP_PROPS_SUPPORTED = 0x9801, OP_PROP_DESC = 0x9802, OP_PROP_VALUE = 0x9803, OP_SET_PROP_VALUE = 0x9804, OP_PROP_LIST = 0x9805,
};
enum {
    R_OK = 0x2001, R_GENERAL = 0x2002, R_SIN_SESION = 0x2003, R_OP_NO = 0x2005, R_PARAM_NO = 0x2006, R_INCOMPLETA = 0x2007,
    R_STORAGE_MAL = 0x2008, R_HANDLE_MAL = 0x2009, R_DEVPROP_NO = 0x200A, R_FORMATO_MAL = 0x200B, R_LLENA = 0x200C,
    R_PROTEGIDO = 0x200D, R_SOLO_LECTURA = 0x200E, R_DENEGADO = 0x200F, R_BORRADO_PARCIAL = 0x2012, R_STORE_NO = 0x2013,
    R_SIN_OBJINFO = 0x2015, R_OCUPADO = 0x2019, R_PADRE_MAL = 0x201A, R_PARAM_MAL = 0x201D, R_SESION_YA = 0x201E,
    R_CANCELADA = 0x201F, R_PROP_MAL = 0xA801, R_PROP_VALOR_MAL = 0xA803, R_DATASET_MAL = 0xA806, R_GRUPO_NO = 0xA807,
    R_PROFUNDIDAD_NO = 0xA808, R_DEMASIADO = 0xA809, R_PROP_NO = 0xA80A,
};
enum { EV_OBJ_QUITADO = 0x4003, EV_STORE_PUESTA = 0x4004, EV_STORE_QUITADA = 0x4005 };
enum { F_INDEF = 0x3000, F_CARPETA = 0x3001, F_TEXTO = 0x3004, F_WAV = 0x3008, F_MP3 = 0x3009, F_JPEG = 0x3801, F_BMP = 0x3804,
       F_GIF = 0x3807, F_PNG = 0x380B };
enum { T_U8 = 0x0002, T_U16 = 0x0004, T_U32 = 0x0006, T_U64 = 0x0008, T_U128 = 0x000A, T_STR = 0xFFFF };
enum { P_STORAGE = 0xDC01, P_FORMATO = 0xDC02, P_PROTECCION = 0xDC03, P_TAM = 0xDC04, P_NOMBRE_FICH = 0xDC07, P_CREADO = 0xDC08,
       P_MODIFICADO = 0xDC09, P_PADRE = 0xDC0B, P_PUOID = 0xDC41, P_NOMBRE = 0xDC44 };
#define DP_NOMBRE_AMIGO 0xD402

static const u16 ops[] = { OP_DEVICE_INFO, OP_OPEN_SESSION, OP_CLOSE_SESSION, OP_STORAGE_IDS, OP_STORAGE_INFO, OP_NUM_OBJECTS,
    OP_OBJECT_HANDLES, OP_OBJECT_INFO, OP_GET_OBJECT, OP_DELETE_OBJECT, OP_SEND_OBJECT_INFO, OP_SEND_OBJECT, OP_DEV_PROP_DESC,
    OP_DEV_PROP_VALUE, OP_MOVE_OBJECT, OP_PARTIAL_OBJECT, OP_PARTIAL_OBJECT_64, OP_PROPS_SUPPORTED, OP_PROP_DESC, OP_PROP_VALUE,
    OP_SET_PROP_VALUE, OP_PROP_LIST,
    /* (Fase 3b) el canal de gestión. SENUS lo detecta aquí: si está 0x9400, hay canal. Desde 0.29.1 también 0x9407 (SEN_BT_EMPAREJAR); sin radio contesta R_OP_NO */
    OP_SEN_INFO, OP_SEN_AJ_LEER, OP_SEN_AJ_PONER, OP_SEN_RELOJ, OP_SEN_VINCULAR, OP_SEN_ENTRAR_BL, OP_SEN_BT_ESTADO, OP_SEN_BT_EMPAREJAR, OP_SEN_APPS };
static const u16 eventos[] = { EV_OBJ_QUITADO, EV_STORE_QUITADA, EV_STORE_PUESTA };
static const u16 formatos[] = { F_INDEF, F_CARPETA, F_TEXTO, F_WAV, F_MP3, F_JPEG, F_BMP, F_GIF, F_PNG };
static const u16 props[] = { P_STORAGE, P_FORMATO, P_PROTECCION, P_TAM, P_NOMBRE_FICH, P_CREADO, P_MODIFICADO, P_PADRE, P_PUOID, P_NOMBRE };
#define N(a) ((u32)(sizeof a / sizeof a[0]))
#define TXT_(x) #x
#define TXT(x) TXT_(x)
/* (FATW) la tarjeta */
#define TJ_BASE     0x8000u                                       /* ids virtuales de la tarjeta: TJ_BASE + índice (0 = su raíz) */
#ifndef MTP_TJ_MAX
#define MTP_TJ_MAX  512u                                          /* objetos de la tarjeta que se numeran (más: no se ven por MTP) */
#endif
static struct { u16 padre; u8 vivo, es_dir; char nombre[64]; } tj[MTP_TJ_MAX];
static u32 tj_n; static u8 tj_lleno, tj_hay, tj_poblada; static u16 tj_ep; static u32 tj_visto, tj_cambios_vistos, tj_gen;
static char tj_ruta_buf[2][300];
#define NOMBRE_DISPOSITIVO USB_TEXTO_MODELO " sistema propio"

/* ---- el estado (RAM fija) --------------------------------------------------------------------------------------------- */
enum { E_CAB, E_PARAMS, E_DATOS, E_TIRAR };                       /* lo que se está leyendo del tubo */
enum { T_LIBRE, T_DATOS_ENT, T_TRABAJO, T_SALIDA };                /* la transacción */
enum { G_NADA, G_BUF, G_LISTA, G_FICH };                          /* el generador de la fase de datos al host */
enum { L_HANDLES, L_PROPS };
enum { A_TODOS, A_HIJOS, A_DESC, A_UNO };                         /* qué objetos entran en una lista */

static struct {
    u8  vivo, tx, ent_fase, gen, resp_pend, resp_np;
    u32 sesion;
    /* lo que llega */
    u8  cab[12]; u32 cab_n, falta, c_len, c_tid; u16 c_tipo, c_cod;
    u32 t_ultimo;                                                  /* hw_ms del último byte de una fase de datos de la computadora */
    /* la transacción en curso */
    u16 op; u32 tid, par[5], npar;
    u8  ent[1024]; u32 ent_n; u8 ent_rebosa;                      /* datos pequeños de la computadora (ObjectInfo, una cadena) */
    /* la salida */
    u8  sal[2048]; u32 sal_i, sal_n;
    u8  dat[1024]; u32 dat_n, dat_i;                              /* G_BUF */
    u16 resp_cod; u32 resp_par[3];
    u8  ev[4][16], ev_n;                                           /* eventos por salir (una cola corta) */
    u8  permiso;                                                  /* (AUD9) 0 = sin preguntar, 1 = sí, 2 = no */
    u16 epoca; u32 visto;                                         /* (AUD9 A1) la época de los handles; hw_w6f_cambios() al acabar el último paso */
    /* G_LISTA */
    struct { u8 tipo, ambito, contado, cabecera, mal, st; u16 formato; u32 raiz, prop, cursor, n, bytes, max, dado; } li;   /* st: 0 las dos, 1 Memoria, 2 tarjeta */
    /* G_FICH */
    struct { u32 id, desde, total, hecho; u8 mal; char ruta[300]; } fi;
    /* SendObjectInfo pendiente */
    struct { u8 activo, carpeta, tj; u32 handle, tam, id_viejo; char ruta[W6F_RUTA_MAX + 1]; } po;
    /* SendObject en curso */
    struct { u8 activo, mal; u16 cod; u32 hecho; } so;
    /* DeleteObject a pasos */
    struct { u32 raiz, cur, n; u8 parcial, cortado; } bo;
    u16 gen_id[W6F_SIS_MAX_OBJ];
    u32 apps_cambios;
    MtpInfo inf;
} m;
static char ruta_a[W6F_RUTA_MAX + 1], ruta_b[W6F_RUTA_MAX + 1];
static HwW6fObj ob, ob2;

/* ---- utilidades ------------------------------------------------------------------------------------------------------- */
static void pon16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void pon32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 lee16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 lee32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 largo(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static int empieza(const char *s, const char *pre) { while (*pre) if (*s++ != *pre++) return 0; return 1; }

/* (AUD9 A1) la generación de un id es la suya (sube cuando MTP lo ve morir) MÁS la época (sube cuando la Memoria cambia FUERA de
 * MTP): las dos solo suben, así que un handle no vuelve a valer nunca, y un cambio ajeno invalida TODOS a la vez. */
static u32 gen_de(u32 id) { return (u32)(m.gen_id[id < W6F_SIS_MAX_OBJ ? id : 0] + m.epoca) & 0x7FFFu; }   /* (FATW) 15 bits: el 31 es de la tarjeta */
static int es_tj(u32 id) { return id >= TJ_BASE; }
static u32 handle_de(u32 id)
{
    if (es_tj(id)) return id == TJ_BASE ? 0 : 0x80000000u | (u32)(tj_ep & 0x7FFFu) << 16 | (id - TJ_BASE);   /* su raíz es el 0 de su storage */
    return id ? gen_de(id) << 16 | id : 0;
}
static u32 storage_de(u32 id) { return es_tj(id) ? MTP_STORAGE_TJ : MTP_STORAGE; }
static void id_muere(u32 id) { if (es_tj(id)) { if (id - TJ_BASE < tj_n) tj[id - TJ_BASE].vivo = 0; return; } if (id && id < W6F_SIS_MAX_OBJ) m.gen_id[id]++; }
/* ---- (FATW) la tabla de la tarjeta ---- */
static void tj_tirar(void) { tj_n = 0; tj_lleno = 0; tj_poblada = 0; tj_ep++; }
static int tjm_ruta(u32 idx, char *r, u32 max)                    /* «/a/b» DENTRO de la tarjeta (la raíz: «/») */
{
    u32 pila[32], n = 0, k = 0, l;
    while (idx && n < N(pila)) { if (idx >= tj_n || !tj[idx].vivo) return -1; pila[n++] = idx; idx = tj[idx].padre; }
    if (idx) return -1;
    r[k++] = '/';
    while (n--) { l = 0; while (tj[pila[n]].nombre[l]) l++; if (k + l + 2 > max) return -1; memcpy(r + k, tj[pila[n]].nombre, l); k += l; if (n) r[k++] = '/'; }
    r[k] = 0;
    return 0;
}
static u32 tj_buscar(u32 padre, const char *nom)                 /* el índice de ese hijo, o 0 */
{
    u32 i, j;
    for (i = 1; i < tj_n; i++) if (tj[i].vivo && tj[i].padre == padre) { for (j = 0; nom[j] && tj[i].nombre[j] == nom[j]; j++) ; if (!nom[j] && !tj[i].nombre[j]) return i; }
    return 0;
}
static u32 tj_poner(u32 padre, const char *nom, int es_dir)      /* el que hay o uno nuevo; 0 si la tabla está llena */
{
    u32 i = tj_buscar(padre, nom), j;
    if (i) { tj[i].es_dir = (u8)es_dir; return i; }
    for (i = 1; i < tj_n && tj[i].vivo; i++) ;
    if (i == tj_n) { if (tj_n >= MTP_TJ_MAX) { if (!tj_lleno) hw_log("mtp: la tarjeta tiene más objetos de los que se numeran: el resto no se ve\n"); tj_lleno = 1; return 0; } tj_n++; }
    for (j = 0; nom[j] && j < sizeof tj[i].nombre - 1; j++) tj[i].nombre[j] = nom[j];
    tj[i].nombre[j] = 0; tj[i].padre = (u16)padre; tj[i].es_dir = (u8)es_dir; tj[i].vivo = 1;
    return i;
}
static void tj_poblar(void)                                      /* el árbol entero, carpeta a carpeta, en orden de índice */
{
    static HwEntrada e[8];
    u32 d, desde; int k, n;
    if (tj_poblada || !tj_hay) return;
    tj_n = 1; tj[0].vivo = 1; tj[0].es_dir = 1; tj[0].padre = 0; tj[0].nombre[0] = 0;
    for (d = 0; d < tj_n; d++) {
        if (!tj[d].vivo || !tj[d].es_dir || tjm_ruta(d, tj_ruta_buf[0], sizeof tj_ruta_buf[0])) continue;
        for (desde = 0; (n = tj_listar(tj_ruta_buf[0], desde, e, N(e))) > 0; desde += (u32)n)
            for (k = 0; k < n; k++) tj_poner(d, e[k].nombre, e[k].es_dir);
        hw_perro_latido();
    }
    tj_poblada = 1;
}
static int tj_obj(u32 id, HwW6fObj *o)
{
    u32 i = id - TJ_BASE, t; HwW6fInfo inf;
    memset(o, 0, sizeof *o);
    if (i >= tj_n || !tj[i].vivo) return HW_NO_DISPONIBLE;
    o->id = id; o->es_dir = tj[i].es_dir;
    if (!i) return HW_OK;                                         /* su raíz */
    o->padre = TJ_BASE + tj[i].padre;
    if (tjm_ruta(i, tj_ruta_buf[1], sizeof tj_ruta_buf[1]) || hw_tj_info(tj_ruta_buf[1], &inf) != HW_OK) return HW_NO_DISPONIBLE;   /* (aún no, o ya no, existe) */
    o->es_dir = inf.es_dir; o->tam = inf.es_dir ? 0 : inf.tam;
    for (t = 0; tj[i].nombre[t]; t++) o->nombre[t] = tj[i].nombre[t];
    o->nombre[t] = 0;
    return HW_OK;
}
static int obj_de(u32 id, HwW6fObj *o) { return es_tj(id) ? tj_obj(id, o) : hw_w6f_obj(id, o); }
static int oculto(const HwW6fObj *o) { return empieza(o->nombre, ".mtp~") || empieza(o->nombre, ".recibiendo"); }   /* los temporales de MTP y de SER */
/* El id de un handle VÁLIDO (vivo, de esta generación, no oculto), o 0. Deja el objeto en *o. */
static u32 id_de(u32 h, HwW6fObj *o)
{
    u32 id = h & 0xFFFFu;
    if (h & 0x80000000u) {                                         /* (FATW) de la tarjeta */
        if (!tj_hay || (h >> 16 & 0x7FFFu) != (u32)(tj_ep & 0x7FFFu) || !id || id >= tj_n || tj_obj(TJ_BASE + id, o) != HW_OK) return 0;
        return TJ_BASE + id;
    }
    if (!id || id >= W6F_SIS_MAX_OBJ || h >> 16 != gen_de(id)) return 0;
    if (hw_w6f_obj(id, o) != HW_OK || oculto(o)) return 0;
    return id;
}

/* La ruta DENTRO de la Memoria de un id («/a/b»; la raíz es «/»), subiendo por los padres. 0 = bien. */
static int ruta_de(u32 id, char *r, u32 max)
{
    u32 pila[W6F_PROF_MAX + 2], n = 0, k = 0, l;
    HwW6fObj *o = &ob2;
    if (es_tj(id)) return tjm_ruta(id - TJ_BASE, r, max);        /* (FATW) DENTRO de la tarjeta */
    while (id && n < N(pila)) { pila[n++] = id; if (hw_w6f_obj(id, o) != HW_OK) return -1; id = o->padre; }
    if (id) return -1;
    r[k++] = '/';
    while (n--) {
        if (hw_w6f_obj(pila[n], o) != HW_OK) return -1;
        l = largo(o->nombre);
        if (k + l + 2 > max) return -1;
        memcpy(r + k, o->nombre, l); k += l;
        if (n) r[k++] = '/';
    }
    r[k] = 0;
    return 0;
}
static int en_apps(const char *r) { return empieza(r, "/" MTP_DIR_APPS) && (r[sizeof MTP_DIR_APPS] == '/' || !r[sizeof MTP_DIR_APPS]); }
static int es_sistema(const char *r) { return !memcmp(r, "/" MTP_DIR_APPS, sizeof MTP_DIR_APPS + 1); }   /* "/apps" exacta */

/* El formato por la extensión (sin mirar mayúsculas). */
static u16 formato_de(const HwW6fObj *o)
{
    static const struct { const char *ext; u16 f; } t[] = { { "txt", F_TEXTO }, { "wav", F_WAV }, { "mp3", F_MP3 }, { "jpg", F_JPEG },
        { "jpeg", F_JPEG }, { "bmp", F_BMP }, { "gif", F_GIF }, { "png", F_PNG } };
    const char *p = 0, *s; u32 i, j;
    if (o->es_dir) return F_CARPETA;
    for (s = o->nombre; *s; s++) if (*s == '.') p = s + 1;
    if (!p) return F_INDEF;
    for (i = 0; i < N(t); i++) {
        for (j = 0; t[i].ext[j] && p[j] && (p[j] | 0x20) == t[i].ext[j]; j++) ;
        if (!t[i].ext[j] && !p[j]) return t[i].f;
    }
    return F_INDEF;
}

/* ---- cadenas MTP (UCS-2 con nulo; el byte de delante cuenta los caracteres CON el nulo) ---------------------------------- */
static u32 utf8_sig(const char **s)                               /* el siguiente código; U+FFFD si el UTF-8 está mal */
{
    const u8 *p = (const u8 *)*s; u32 c = *p++, n = 0, k;
    if (c >= 0xF0 && c < 0xF5) { n = 3; c &= 7; } else if (c >= 0xE0) { n = 2; c &= 15; } else if (c >= 0xC2 && c < 0xE0) { n = 1; c &= 31; }
    else if (c >= 0x80) { *s = (const char *)p; return 0xFFFD; }
    for (k = 0; k < n; k++) { if ((*p & 0xC0) != 0x80) { *s = (const char *)p; return 0xFFFD; } c = c << 6 | (*p++ & 0x3F); }
    *s = (const char *)p;
    return c > 0x10FFFF || (c >= 0xD800 && c < 0xE000) ? 0xFFFD : c;
}
static u32 str_tam(const char *s)                                 /* bytes de la cadena MTP */
{
    u32 n = 0, c;
    while (*s && n < 252) { c = utf8_sig(&s); n += c > 0xFFFF ? 2 : 1; }
    return n ? 1 + 2 * (n + 1) : 1;
}
static u32 str_pon(u8 *p, const char *s)
{
    u32 n = 0, c;
    while (*s && n < 252) {
        c = utf8_sig(&s);
        if (c > 0xFFFF) { c -= 0x10000; pon16(p + 1 + 2 * n++, 0xD800 | c >> 10); c = 0xDC00 | (c & 0x3FF); }
        pon16(p + 1 + 2 * n++, c);
    }
    if (!n) { p[0] = 0; return 1; }
    pon16(p + 1 + 2 * n, 0); p[0] = (u8)(n + 1);
    return 1 + 2 * (n + 1);
}
/* Lee una cadena MTP de los datos recibidos y la deja en UTF-8. 0 = bien; -1 = mal formada o no cabe. */
static int str_lee(const u8 *p, u32 n, u32 *pos, char *d, u32 max)
{
    u32 nc, i, c, k = 0;
    if (*pos >= n) return -1;
    nc = p[(*pos)++];
    if (!nc) { d[0] = 0; return 0; }
    if (*pos + 2 * nc > n) return -1;
    for (i = 0; i + 1 < nc; i++) {
        c = lee16(p + *pos + 2 * i);
        if (c >= 0xD800 && c < 0xDC00) {                          /* par sustituto */
            u32 b = i + 2 < nc ? lee16(p + *pos + 2 * (i + 1)) : 0;
            if (b < 0xDC00 || b >= 0xE000) return -1;
            c = 0x10000 + ((c - 0xD800) << 10) + (b - 0xDC00); i++;
        } else if (c >= 0xDC00 && c < 0xE000) return -1;
        if (!c) break;
        if (k + 5 > max) return -1;
        if (c < 0x80) d[k++] = (char)c;
        else if (c < 0x800) { d[k++] = (char)(0xC0 | c >> 6); d[k++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { d[k++] = (char)(0xE0 | c >> 12); d[k++] = (char)(0x80 | (c >> 6 & 63)); d[k++] = (char)(0x80 | (c & 63)); }
        else { d[k++] = (char)(0xF0 | c >> 18); d[k++] = (char)(0x80 | (c >> 12 & 63)); d[k++] = (char)(0x80 | (c >> 6 & 63)); d[k++] = (char)(0x80 | (c & 63)); }
    }
    d[k] = 0; *pos += 2 * nc;
    return 0;
}
/* NOMBRES SANEADOS: uno solo (sin barras), ni «.» ni «..», sin controles, no reservado (.mtp~), que quepa (HW_W6F_MAX_NOMBRE
 * bytes UTF-8). Lo que no pasa se rechaza entero: no se «arregla» un nombre a escondidas. */
static int nombre_bueno(const char *s)
{
    u32 n = largo(s), i;
    if (!n || n > HW_W6F_MAX_NOMBRE) return 0;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
    if (empieza(s, ".mtp~") || empieza(s, ".recibiendo")) return 0;   /* reservados: los temporales de MTP y de SER (api/serie_rx.h) */
    for (i = 0; i < n; i++) { u8 c = (u8)s[i]; if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') return 0; }
    return 1;
}

/* Fecha MTP «AAAAMMDDThhmmss» de los segundos desde 2000-01-01 (W6F). Sin u64: todo en u32. */
static void fecha_txt(u32 seg, char *t)
{
    u32 dias = seg / 86400u, s = seg % 86400u, z, era, doe, yoe, doy, mp, d, mes, a;
    z = dias + 10957u + 719468u; era = z / 146097u; doe = z - era * 146097u;
    yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u; a = yoe + era * 400u;
    doy = doe - (365u * yoe + yoe / 4u - yoe / 100u); mp = (5u * doy + 2u) / 153u; d = doy - (153u * mp + 2u) / 5u + 1u;
    mes = mp < 10u ? mp + 3u : mp - 9u; a += mes <= 2u;
    {
        u32 v[6] = { a, mes, d, s / 3600u, s / 60u % 60u, s % 60u }, w[6] = { 4, 2, 2, 2, 2, 2 }, i, j, k = 0;
        for (i = 0; i < 6; i++) { if (i == 3) t[k++] = 'T'; for (j = w[i]; j--; ) { t[k + j] = (char)('0' + v[i] % 10u); v[i] /= 10u; } k += w[i]; }
        t[k] = 0;
    }
}

/* ---- la salida -------------------------------------------------------------------------------------------------------- */
static void responder(u16 cod, u32 np, u32 a, u32 b, u32 c)
{
    m.resp_cod = cod; m.resp_np = (u8)np; m.resp_par[0] = a; m.resp_par[1] = b; m.resp_par[2] = c; m.resp_pend = 1;
    m.tx = T_SALIDA;
    if (cod != R_OK) m.inf.errores++;
}
static void cab_datos(u8 *p, u32 total)                          /* cabecera de un contenedor de datos de `total` bytes de carga */
{
    pon32(p, total + 12u); pon16(p + 4, C_DATOS); pon16(p + 6, m.op); pon32(p + 8, m.tid);
}
/* Datos pequeños: se arman enteros en `dat` y luego la respuesta. */
static void datos_buf(u32 n, u16 cod)
{
    m.gen = G_BUF; m.dat_n = n; m.dat_i = 0;
    cab_datos(m.sal + m.sal_n, n); m.sal_n += 12;
    responder(cod, 0, 0, 0, 0);
}
static void evento(u16 cod, u32 par)
{
    u8 *e;
    if (m.ev_n >= 4) return;                                       /* la cola llena: se pierde (el host relee igual al primero) */
    e = m.ev[m.ev_n++];
    pon32(e, 16); pon16(e + 4, C_EVENTO); pon16(e + 6, cod); pon32(e + 8, m.tid); pon32(e + 12, par);
}

/* ---- listas: qué objetos entran ----------------------------------------------------------------------------------------- */
static int dentro_de(u32 id, u32 raiz)                            /* ¿id cuelga (a cualquier profundidad) de raiz? */
{
    u32 n = 0;
    HwW6fObj *o = &ob2;
    while (id && n++ <= 40) { if (obj_de(id, o) != HW_OK) return 0; id = o->padre; if (id == raiz) return 1; if (id == TJ_BASE) return 0; }
    return 0;
}
static int entra(u32 id, HwW6fObj *o)
{
    if (obj_de(id, o) != HW_OK || (!es_tj(id) && oculto(o))) return 0;
    if (m.li.formato && formato_de(o) != m.li.formato) return 0;
    switch (m.li.ambito) {
    case A_TODOS: return 1;
    case A_HIJOS: return o->padre == m.li.raiz || (m.li.raiz == 0 && m.li.st == 0 && o->padre == TJ_BASE);   /* (FATW) «la raíz» de las dos storages */
    case A_DESC:  return dentro_de(id, m.li.raiz);
    default:      return id == m.li.raiz;
    }
}
/* Una propiedad de un objeto: su tipo, y si p != 0 la escribe. Devuelve los bytes del VALOR (0 = no es nuestra). */
static u32 prop_valor(u16 prop, u32 id, const HwW6fObj *o, u8 *p, u16 *tipo)
{
    char f[16];
    switch (prop) {
    case P_STORAGE:    *tipo = T_U32; if (p) pon32(p, storage_de(id)); return 4;
    case P_FORMATO:    *tipo = T_U16; if (p) pon16(p, formato_de(o)); return 2;
    case P_PROTECCION: *tipo = T_U16; if (p) pon16(p, 0); return 2;
    case P_TAM:        *tipo = T_U64; if (p) { pon32(p, o->tam); pon32(p + 4, 0); } return 8;
    case P_PADRE:      *tipo = T_U32; if (p) pon32(p, handle_de(o->padre)); return 4;
    case P_PUOID:      *tipo = T_U128; if (p) { memset(p, 0, 16); pon32(p, handle_de(id)); memcpy(p + 8, USB_TEXTO_MODELO "MTP", 7); } return 16;
    case P_NOMBRE_FICH: case P_NOMBRE: *tipo = T_STR; return p ? str_pon(p, o->nombre) : str_tam(o->nombre);
    case P_CREADO: case P_MODIFICADO:
        *tipo = T_STR;
        if (!o->fecha) { if (p) p[0] = 0; return 1; }
        fecha_txt(o->fecha, f);
        return p ? str_pon(p, f) : str_tam(f);
    }
    return 0;
}
static int formato_conocido(u16 f) { u32 i; for (i = 0; i < N(formatos); i++) if (formatos[i] == f) return 1; return 0; }
static int prop_nuestra(u32 prop) { u32 i; for (i = 0; i < N(props); i++) if (props[i] == prop) return 1; return 0; }
/* Los elementos de la lista de propiedades de un objeto: cuántos y cuántos bytes (p = 0), o los escribe. */
static u32 elementos(u32 id, const HwW6fObj *o, u8 *p, u32 *n)
{
    u32 i, k = 0, v; u16 tipo;
    *n = 0;
    for (i = 0; i < N(props); i++) {
        if (m.li.prop != 0xFFFFFFFFu && m.li.prop != props[i]) continue;
        if (p) { pon32(p + k, handle_de(id)); pon16(p + k + 4, props[i]); }
        v = prop_valor(props[i], id, o, p ? p + k + 8 : 0, &tipo);
        if (p) pon16(p + k + 6, tipo);
        k += 8 + v; (*n)++;
    }
    return k;
}
#define LISTA_POR_PASO 64u                                        /* ids que se miran por paso al contar */
#define LISTA_MAX_OBJ  1200u                                      /* lo más que ocupa un objeto en la lista de propiedades */
static u8 lista_st;                                              /* (FATW) la storage que pidió la orden (0 = todas) */
/* (FATW) el cursor de la lista recorre los ids de la Memoria [1, max) y luego los de la tarjeta [TJ_BASE + 1, TJ_BASE + tj_n) */
static u32 cur_primero(void) { return m.li.st == 2 ? TJ_BASE + 1 : 1; }
static int cur_fin(u32 c) { return m.li.st == 1 ? c >= m.li.max : c >= TJ_BASE + (tj_hay ? tj_n : 0) || (c >= m.li.max && c < TJ_BASE && !tj_hay); }
static u32 cur_sig(u32 c) { c++; if (c >= m.li.max && c < TJ_BASE && m.li.st != 1) c = TJ_BASE + 1; return c; }
static void lista_empezar(u8 tipo, u8 ambito, u32 raiz, u16 formato, u32 prop)
{
    memset(&m.li, 0, sizeof m.li);
    m.li.tipo = tipo; m.li.ambito = ambito; m.li.raiz = raiz; m.li.formato = formato; m.li.prop = prop; m.li.max = hw_w6f_max_obj();
    m.li.st = (u8)(es_tj(raiz) ? 2 : (raiz && raiz != 0xFFFFFFFFu) ? 1 : lista_st);
    if (m.li.st != 1) tj_poblar();
    m.gen = G_LISTA;
    responder(R_OK, 0, 0, 0, 0);                                   /* sale detrás de la lista */
}
/* Un trozo de la lista. Primero se cuenta (a pasos), luego cabecera + número + elementos. 1 = hizo algo. */
static int lista_rellenar(void)
{
    u32 libre = sizeof m.sal - m.sal_n, n, k;
    if (!m.li.contado) {
        u32 vueltas = 0;
        for (m.li.cursor = m.li.cursor ? m.li.cursor : cur_primero(); !cur_fin(m.li.cursor) && vueltas < LISTA_POR_PASO; m.li.cursor = cur_sig(m.li.cursor), vueltas++) {
            if (!entra(m.li.cursor, &ob)) continue;
            if (m.li.tipo == L_HANDLES) { m.li.n++; m.li.bytes += 4; }
            else { k = elementos(m.li.cursor, &ob, 0, &n); m.li.n += n; m.li.bytes += k; }
        }
        if (cur_fin(m.li.cursor)) { m.li.contado = 1; m.li.cursor = cur_primero(); }
        return 1;
    }
    if (!m.li.cabecera) {
        if (libre < 16) return 0;
        cab_datos(m.sal + m.sal_n, 4 + m.li.bytes); pon32(m.sal + m.sal_n + 12, m.li.n); m.sal_n += 16;
        m.li.cabecera = 1; m.li.dado = 0;
        return 1;
    }
    while (!cur_fin(m.li.cursor) && !m.li.mal) {
        if (sizeof m.sal - m.sal_n < LISTA_MAX_OBJ) return 0;     /* que se vacíe antes */
        if (entra(m.li.cursor, &ob)) {
            k = m.li.tipo == L_HANDLES ? 4 : elementos(m.li.cursor, &ob, 0, &n);
            if (m.li.dado + k > m.li.bytes) { m.li.mal = 1; break; }   /* (AUD9 M3) no cabe en lo anunciado: la lista ya no vale */
            if (m.li.tipo == L_HANDLES) pon32(m.sal + m.sal_n, handle_de(m.li.cursor));
            else elementos(m.li.cursor, &ob, m.sal + m.sal_n, &n);
            m.sal_n += k; m.li.dado += k;
            m.li.cursor = cur_sig(m.li.cursor);
            return 1;                                              /* uno por vuelta: el presupuesto del paso manda */
        }
        m.li.cursor = cur_sig(m.li.cursor);
    }
    /* (AUD9 M3) la longitud del contenedor ya se dijo: si lo dado no llega (la Memoria cambió entre contar y dar), se RELLENA con
     * ceros hasta ella y la respuesta es General_Error. El encuadre de la sesión no se pierde nunca. */
    if (m.li.dado < m.li.bytes) {
        k = m.li.bytes - m.li.dado < libre ? m.li.bytes - m.li.dado : libre;
        if (!k) return 0;
        memset(m.sal + m.sal_n, 0, k); m.sal_n += k; m.li.dado += k; m.li.mal = 1;
        if (m.li.dado < m.li.bytes) return 1;
    }
    if (m.li.mal) m.resp_cod = R_GENERAL;
    m.gen = G_NADA;
    return 1;
}
/* ---- un fichero a trozos -------------------------------------------------------------------------------------------------- */
static int fich_rellenar(void)
{
    u32 libre = sizeof m.sal - m.sal_n, n;
    int r;
    if (m.fi.hecho >= m.fi.total) { m.gen = G_NADA; if (m.fi.mal) m.resp_cod = R_GENERAL; return 1; }
    if (libre < 512) return 0;
    n = m.fi.total - m.fi.hecho < libre ? m.fi.total - m.fi.hecho : libre;
    r = m.fi.mal ? -1 : es_tj(m.fi.id) ? (int)tj_leer_desde(m.fi.ruta, m.fi.desde + m.fi.hecho, m.sal + m.sal_n, n)   /* (FATW) */
                      : hw_w6f_leer_id(m.fi.id, m.fi.desde + m.fi.hecho, m.sal + m.sal_n, n);
    if (r != (int)n) {                                             /* página rota (o el fichero cambió): la longitud ya se dijo, se rellena con
                                                                      ceros y la RESPUESTA dice que falló (General_Error): libmtp y Windows lo dan por malo */
        if (!m.fi.mal) hw_log("mtp: lectura mala: se rellena y se responde General_Error\n");
        m.fi.mal = 1; memset(m.sal + m.sal_n, 0, n);
    }
    m.sal_n += n; m.fi.hecho += n; m.inf.bytes_tx += n;
    return 1;
}
/* Rellena `sal`: lo que quede del generador y, al acabar, la respuesta. 1 = puso algo o cambió de estado. */
static int rellenar(void)
{
    u32 libre = sizeof m.sal - m.sal_n, n, i;
    if (m.gen == G_BUF) {
        n = m.dat_n - m.dat_i < libre ? m.dat_n - m.dat_i : libre;
        memcpy(m.sal + m.sal_n, m.dat + m.dat_i, n); m.sal_n += n; m.dat_i += n;
        if (m.dat_i >= m.dat_n) m.gen = G_NADA;
        return n != 0;
    }
    if (m.gen == G_LISTA) return lista_rellenar();
    if (m.gen == G_FICH) return fich_rellenar();
    if (m.resp_pend && libre >= 24) {
        u8 *p = m.sal + m.sal_n;
        pon32(p, 12u + 4u * m.resp_np); pon16(p + 4, C_RESP); pon16(p + 6, m.resp_cod); pon32(p + 8, m.tid);
        for (i = 0; i < m.resp_np; i++) pon32(p + 12 + 4 * i, m.resp_par[i]);
        m.sal_n += 12u + 4u * m.resp_np; m.resp_pend = 0;
        return 1;
    }
    if (m.tx == T_SALIDA && !m.resp_pend && m.sal_i >= m.sal_n) { m.tx = T_LIBRE; m.inf.ocupado = 0; return 1; }
    return 0;
}

/* ---- lo pendiente: SendObjectInfo / SendObject ---------------------------------------------------------------------------- */
static void po_tirar(void)
{
    if (m.po.activo && !m.po.carpeta) { if (m.po.tj) hw_tj_rx_abortar_d(HW_RX_MTP); else hw_w6f_rx_abortar_d(HW_RX_MTP); }   /* (AUD9 M2) solo la ranura SUYA: la de SER no se toca */
    m.po.activo = 0; m.so.activo = 0;
    m.inf.rx_tam = m.inf.rx_hecho = 0;
}

/* ---- las operaciones ------------------------------------------------------------------------------------------------------- */
static u32 arr16(u8 *p, const u16 *a, u32 n) { u32 i; pon32(p, n); for (i = 0; i < n; i++) pon16(p + 4 + 2 * i, a[i]); return 4 + 2 * n; }
static void op_device_info(void)
{
    u8 *p = m.dat; u32 k = 0;
    pon16(p + k, 100); k += 2;                                     /* StandardVersion */
    pon32(p + k, 6); k += 4;                                       /* MTPVendorExtensionID: Microsoft (MTP 1.1 §5.1.1.2) */
    pon16(p + k, 100); k += 2;                                     /* MTPVersion */
    k += str_pon(p + k, "microsoft.com: 1.0;");
    pon16(p + k, 0); k += 2;                                       /* FunctionalMode */
    k += arr16(p + k, ops, N(ops));
    k += arr16(p + k, eventos, N(eventos));
    { static const u16 dp[] = { DP_NOMBRE_AMIGO }; k += arr16(p + k, dp, N(dp)); }
    pon32(p + k, 0); k += 4;                                       /* CaptureFormats: ninguno */
    k += arr16(p + k, formatos, N(formatos));                      /* PlaybackFormats */
    k += str_pon(p + k, "Sony Ericsson (sistema propio)");
    k += str_pon(p + k, USB_TEXTO_MODELO "i");
    k += str_pon(p + k, SENEW_VERSION "+" TXT(SENEW_BUILD));       /* DeviceVersion: la versión de verdad (contrato §7), de un solo sitio: api/hw.h */
    k += str_pon(p + k, gs_serie_mtp());                           /* la derivada del id vinculado; sin vincular, la fija. [sin saber] Windows la usa para reconocerlo */
    datos_buf(k, R_OK);
}
static void op_storage_info(void)
{
    HwW6fEstado e; u8 *p = m.dat; u32 k = 0;
    if (m.par[0] == MTP_STORAGE_TJ && tj_hay) {                    /* (FATW) la tarjeta: extraíble; RO si está protegida */
        u32 cap = hw_tj_capacidad_kib(), w = (u32)hw_tj_escribible();
        pon16(p, 0x0004); pon16(p + 2, 0x0002); pon16(p + 4, w ? 0x0000 : 0x0001); k = 6;   /* RemovableRAM, GenericHierarchical */
        pon32(p + k, cap << 10); pon32(p + k + 4, cap >> 22); k += 8;   /* MaxCapacity (u64) */
        pon32(p + k, w ? hw_tj_libre() : 0); pon32(p + k + 4, 0); k += 8;   /* FreeSpaceInBytes (hw_tj_libre: tope 4 GiB - 1) */
        pon32(p + k, 0xFFFFFFFFu); k += 4;                         /* FreeSpaceInObjects: no aplica */
        k += str_pon(p + k, "Tarjeta");
        k += str_pon(p + k, "");
        datos_buf(k, R_OK);
        return;
    }
    if (m.par[0] != MTP_STORAGE) { responder(R_STORAGE_MAL, 0, 0, 0, 0); return; }
    hw_w6f_estado(&e);
    pon16(p, 0x0003); pon16(p + 2, 0x0002); pon16(p + 4, e.escribible ? 0x0000 : 0x0001); k = 6;   /* FixedRAM, GenericHierarchical, RW/RO */
    pon32(p + k, e.capacidad); pon32(p + k + 4, 0); k += 8;       /* MaxCapacity (u64) */
    pon32(p + k, e.escribible ? hw_w6f_libre() : 0); pon32(p + k + 4, 0); k += 8;   /* FreeSpaceInBytes: el fichero NUEVO más grande que cabe (exacto) */
    pon32(p + k, e.max_obj > e.objetos + 1 ? e.max_obj - e.objetos - 1 : 0); k += 4;   /* FreeSpaceInObjects */
    k += str_pon(p + k, "Memoria");
    k += str_pon(p + k, "");
    datos_buf(k, R_OK);
}
/* El ámbito de GetObjectHandles / GetNumObjects (PTP §10.5.6): padre 0 = TODOS; 0xFFFFFFFF = la raíz; si no, esa carpeta. */
static int ambito_padre(u32 padre, u8 *amb, u32 *raiz)
{
    HwW6fObj *o = &ob;
    if (padre == 0) { *amb = A_TODOS; *raiz = 0; return 0; }
    if (padre == 0xFFFFFFFFu) { *amb = A_HIJOS; *raiz = lista_st == 2 ? TJ_BASE : 0; return 0; }
    if (!(*raiz = id_de(padre, o))) return R_HANDLE_MAL;
    if (!o->es_dir) return R_PADRE_MAL;
    *amb = A_HIJOS;
    return 0;
}
static void op_handles(int solo_contar)
{
    u8 amb; u32 raiz; int r;
    if (m.par[0] != 0xFFFFFFFFu && m.par[0] != MTP_STORAGE && !(m.par[0] == MTP_STORAGE_TJ && tj_hay)) { responder(R_STORAGE_MAL, 0, 0, 0, 0); return; }
    lista_st = (u8)(m.par[0] == MTP_STORAGE ? 1 : m.par[0] == MTP_STORAGE_TJ ? 2 : 0);
    if ((r = ambito_padre(m.par[2], &amb, &raiz))) { responder((u16)r, 0, 0, 0, 0); return; }
    lista_empezar(L_HANDLES, amb, raiz, (u16)m.par[1], 0);
    if (solo_contar) {                                             /* GetNumObjects: se cuenta aquí mismo, a pasos, sin fase de datos */
        m.li.tipo = L_HANDLES; m.gen = G_NADA; m.resp_pend = 0; m.tx = T_TRABAJO;
    }
}
static void op_object_info(void)
{
    HwW6fObj *o = &ob; u32 id = id_de(m.par[0], o), k; u8 *p = m.dat; char f[16];
    if (!id) { responder(R_HANDLE_MAL, 0, 0, 0, 0); return; }
    memset(p, 0, 52);
    pon32(p, storage_de(id)); pon16(p + 4, formato_de(o)); pon16(p + 6, 0); pon32(p + 8, o->tam);
    pon32(p + 38, handle_de(o->padre));
    if (o->es_dir) pon16(p + 42, 0x0001);                          /* AssociationType: GenericFolder */
    k = 52;
    k += str_pon(p + k, o->nombre);
    if (o->fecha) { fecha_txt(o->fecha, f); k += str_pon(p + k, f); k += str_pon(p + k, f); } else { p[k++] = 0; p[k++] = 0; }
    p[k++] = 0;                                                    /* Keywords */
    datos_buf(k, R_OK);
}
static void op_get_object(u32 desde, u32 max, int partial)
{
    HwW6fObj *o = &ob; u32 id = id_de(m.par[0], o), n;
    if (!id) { responder(R_HANDLE_MAL, 0, 0, 0, 0); return; }
    if (o->es_dir) { responder(R_HANDLE_MAL, 0, 0, 0, 0); return; }
    if (partial && desde > o->tam) { responder(R_PARAM_MAL, 0, 0, 0, 0); return; }
    n = o->tam - desde; if (partial && max < n) n = max;
    memset(&m.fi, 0, sizeof m.fi);
    m.fi.id = id; m.fi.desde = desde; m.fi.total = n;
    if (es_tj(id) && ruta_de(id, m.fi.ruta, sizeof m.fi.ruta)) m.fi.mal = 1;   /* (FATW) se lee por su ruta */
    cab_datos(m.sal + m.sal_n, n); m.sal_n += 12;
    m.gen = G_FICH;
    responder(R_OK, (u32)(partial != 0), n, 0, 0);
}
static void op_delete(void)
{
    HwW6fObj *o = &ob; u32 id;
    if (m.par[0] == 0xFFFFFFFFu) { responder(R_DENEGADO, 0, 0, 0, 0); return; }   /* «borrar todo»: no (la carpeta del sistema; docs/mtp.md) */
    if (!(id = id_de(m.par[0], o))) { responder(R_HANDLE_MAL, 0, 0, 0, 0); return; }
    if (ruta_de(id, ruta_a, sizeof ruta_a)) { responder(R_GENERAL, 0, 0, 0, 0); return; }
    if (!es_tj(id) && es_sistema(ruta_a)) { responder(R_PROTEGIDO, 0, 0, 0, 0); return; }   /* (contrato §6) lo protegido contesta R_PROTEGIDO: R_DENEGADO es «el usuario dijo No» */
    if (es_tj(id) && !hw_tj_escribible()) { responder(R_SOLO_LECTURA, 0, 0, 0, 0); return; }
    m.bo.raiz = m.bo.cur = id; m.bo.parcial = 0; m.bo.cortado = 0; m.bo.n = 0;
    m.tx = T_TRABAJO;
}
/* Un paso del borrado (recursivo, de abajo arriba; cada borrado es atómico en W6F). 1 = sigue; 0 = acabado. */
static int borrar_paso(void)
{
    HwW6fObj *o = &ob; u32 c = 0, padre;
    if (obj_de(m.bo.cur, o) != HW_OK) return 0;
    padre = o->padre;
    if (es_tj(m.bo.cur)) {                                         /* (FATW) en la tarjeta: los hijos, de la tabla */
        u32 i;
        if (o->es_dir && !m.bo.parcial) for (i = 1; i < tj_n; i++) if (tj[i].vivo && TJ_BASE + tj[i].padre == m.bo.cur && tj_obj(TJ_BASE + i, &ob2) == HW_OK) { m.bo.cur = TJ_BASE + i; return 1; }
    } else if (o->es_dir && hw_w6f_hijo(m.bo.cur, &c, o) == 1 && !m.bo.parcial) {
        m.bo.cur = o->id;                                          /* baja al primer hijo */
        return 1;
    }
    if (ruta_de(m.bo.cur, ruta_a, sizeof ruta_a)) { m.bo.parcial = 1; return 0; }
    if ((es_tj(m.bo.cur) ? hw_tj_borrar(ruta_a) : hw_w6f_borrar(ruta_a)) != HW_OK) { m.bo.parcial = 1; if (m.bo.cur == m.bo.raiz) return 0; m.bo.cur = padre; return 1; }
    id_muere(m.bo.cur); m.inf.borrados++; m.bo.n++;
    if (!es_tj(m.bo.cur) && en_apps(ruta_a)) m.apps_cambios++;
    if (m.bo.cur == m.bo.raiz) return 0;
    m.bo.cur = padre;
    return 1;
}
/* (FATW) SendObjectInfo hacia la TARJETA: carpeta = hw_tj_mkdir; fichero = hw_tj_rx_abrir_d con el nombre BUENO (nada
 * existe, ni temporal, hasta hw_tj_rx_cerrar_d; si ya había uno, se reemplaza de una escritura al cerrar). */
static void fin_send_object_info_tj(void)
{
    HwW6fObj *o = &ob; u32 pos = 52, padre = m.par[1], pid = TJ_BASE, tam, l, idx; u16 fmt; char nom[HW_W6F_MAX_NOMBRE * 4 + 8]; int r; HwW6fInfo inf;
    if (!tj_hay) { responder(R_STORE_NO, 0, 0, 0, 0); return; }
    if (m.par[0] && m.par[0] != MTP_STORAGE_TJ) { responder(R_STORAGE_MAL, 0, 0, 0, 0); return; }
    if (!hw_tj_escribible()) { responder(R_SOLO_LECTURA, 0, 0, 0, 0); return; }
    if (m.ent_rebosa || m.ent_n < 53 || str_lee(m.ent, m.ent_n, &pos, nom, sizeof nom)) { responder(R_DATASET_MAL, 0, 0, 0, 0); return; }
    fmt = (u16)lee16(m.ent + 4); tam = lee32(m.ent + 8);
    if (!nombre_bueno(nom)) { hw_log("mtp: nombre rechazado\n"); responder(R_DATASET_MAL, 0, 0, 0, 0); return; }
    tj_poblar();
    if (padre != 0 && padre != 0xFFFFFFFFu) {
        if (!(pid = id_de(padre, o)) || !es_tj(pid) || !o->es_dir) { responder(R_PADRE_MAL, 0, 0, 0, 0); return; }
    }
    if (ruta_de(pid, m.po.ruta, sizeof m.po.ruta)) { responder(R_GENERAL, 0, 0, 0, 0); return; }
    l = largo(m.po.ruta);
    if (l + largo(nom) + 2 > sizeof m.po.ruta) { responder(R_DATASET_MAL, 0, 0, 0, 0); return; }
    if (l > 1) m.po.ruta[l++] = '/';
    memcpy(m.po.ruta + l, nom, largo(nom) + 1);
    r = hw_tj_info(m.po.ruta, &inf);
    if (fmt == F_CARPETA) {
        if (r == HW_OK) { responder(R_GENERAL, 0, 0, 0, 0); return; }
        if (hw_tj_mkdir(m.po.ruta) != HW_OK) { responder(hw_tj_motivo() == HW_W6F_M_LLENO ? R_LLENA : hw_tj_motivo() == HW_W6F_M_NOMBRE ? R_DATASET_MAL : R_GENERAL, 0, 0, 0, 0); return; }
        if (!(idx = tj_poner(pid - TJ_BASE, nom, 1))) { responder(R_GENERAL, 0, 0, 0, 0); return; }
        responder(R_OK, 3, MTP_STORAGE_TJ, handle_de(pid), handle_de(TJ_BASE + idx));
        return;
    }
    if (r == HW_OK && inf.es_dir) { responder(R_DENEGADO, 0, 0, 0, 0); return; }
    if (tam == 0xFFFFFFFFu) { responder(R_DEMASIADO, 0, 0, 0, 0); return; }
    if (tam > hw_tj_libre()) { responder(R_LLENA, 0, 0, 0, 0); return; }   /* (el viejo, si se pisa, vale hasta el final) */
    if (!(idx = tj_poner(pid - TJ_BASE, nom, 0))) { responder(R_GENERAL, 0, 0, 0, 0); return; }
    if (hw_tj_rx_abrir_d(m.po.ruta, HW_RX_MTP) != HW_OK) {
        responder(hw_tj_motivo() == HW_W6F_M_OCUPADA ? R_OCUPADO : hw_tj_motivo() == HW_W6F_M_LLENO ? R_LLENA : hw_tj_motivo() == HW_W6F_M_NOMBRE ? R_DATASET_MAL : R_GENERAL, 0, 0, 0, 0);
        return;
    }
    m.po.activo = 1; m.po.carpeta = 0; m.po.tj = 1; m.po.tam = tam; m.po.id_viejo = 0; m.po.handle = handle_de(TJ_BASE + idx);
    responder(R_OK, 3, MTP_STORAGE_TJ, handle_de(pid), m.po.handle);
}
/* SendObjectInfo: los datos ya están en ent[]. */
static void fin_send_object_info(void)
{
    HwW6fObj *o = &ob; u32 pos = 52, padre = m.par[1], pid = 0, tam, l; u16 fmt; char nom[HW_W6F_MAX_NOMBRE * 4 + 8]; int r, id;
    HwW6fEstado e;
    po_tirar();                                                    /* uno nuevo deja el anterior (MTP 1.1 D.2.? SendObjectInfo) */
    if (m.par[0] == MTP_STORAGE_TJ || (padre & 0x80000000u && padre != 0xFFFFFFFFu)) { fin_send_object_info_tj(); return; }   /* (FATW) */
    if (m.par[0] && m.par[0] != MTP_STORAGE) { responder(R_STORAGE_MAL, 0, 0, 0, 0); return; }
    hw_w6f_estado(&e);
    if (!e.escribible) { responder(R_SOLO_LECTURA, 0, 0, 0, 0); return; }
    if (m.ent_rebosa || m.ent_n < 53 || str_lee(m.ent, m.ent_n, &pos, nom, sizeof nom)) { responder(R_DATASET_MAL, 0, 0, 0, 0); return; }
    fmt = (u16)lee16(m.ent + 4); tam = lee32(m.ent + 8);
    if (!nombre_bueno(nom)) { hw_log("mtp: nombre rechazado\n"); responder(R_DATASET_MAL, 0, 0, 0, 0); return; }
    if (padre != 0 && padre != 0xFFFFFFFFu) {
        if (!(pid = id_de(padre, o))) { responder(R_PADRE_MAL, 0, 0, 0, 0); return; }
        if (!o->es_dir) { responder(R_PADRE_MAL, 0, 0, 0, 0); return; }
    }
    if (ruta_de(pid, m.po.ruta, sizeof m.po.ruta)) { responder(R_GENERAL, 0, 0, 0, 0); return; }
    l = largo(m.po.ruta);
    if (l + largo(nom) + 2 > sizeof m.po.ruta) { responder(R_DATASET_MAL, 0, 0, 0, 0); return; }
    if (l > 1) m.po.ruta[l++] = '/';
    memcpy(m.po.ruta + l, nom, largo(nom) + 1);
    id = hw_w6f_id(m.po.ruta);
    if (fmt == F_CARPETA) {
        if (id >= 0) { responder(R_GENERAL, 0, 0, 0, 0); return; }   /* ya hay algo con ese nombre */
        r = hw_w6f_mkdir(m.po.ruta);
        if (r != HW_OK) { responder(hw_w6f_motivo() == HW_W6F_M_LLENO || hw_w6f_motivo() == HW_W6F_M_MUCHOS ? R_LLENA : R_GENERAL, 0, 0, 0, 0); return; }
        id = hw_w6f_id(m.po.ruta);
        if (id <= 0) { responder(R_GENERAL, 0, 0, 0, 0); return; }
        responder(R_OK, 3, MTP_STORAGE, handle_de(pid), handle_de((u32)id));
        return;
    }
    if (id >= 0) {                                                 /* ya existe: un fichero se PISA al final (atómico); una carpeta no */
        if (hw_w6f_obj((u32)id, o) != HW_OK || o->es_dir) { responder(R_DENEGADO, 0, 0, 0, 0); return; }
        m.po.id_viejo = (u32)id;
    } else m.po.id_viejo = 0;
    if (tam == 0xFFFFFFFFu || tam > e.capacidad) { responder(R_DEMASIADO, 0, 0, 0, 0); return; }
    if (tam > hw_w6f_libre()) { responder(R_LLENA, 0, 0, 0, 0); return; }   /* SE COMPRUEBA ANTES DE EMPEZAR (el viejo, si se pisa, vale hasta el final) */
    if (hw_w6f_id("/" MTP_TEMP) > 0) hw_w6f_borrar("/" MTP_TEMP);   /* un temporal comprometido que dejara un corte */
    r = hw_w6f_rx_abrir_d("/" MTP_TEMP, HW_RX_MTP);                 /* (SER) la escritura a trozos, con dueño (AUD9 M2): nada existe hasta cerrar */
    if (r != HW_OK && hw_w6f_motivo() == HW_W6F_M_OCUPADA) { responder(R_OCUPADO, 0, 0, 0, 0); return; }   /* la tiene SER: «ocupado», sin tocarla */
    if (r != HW_OK) { responder(hw_w6f_motivo() == HW_W6F_M_LLENO || hw_w6f_motivo() == HW_W6F_M_MUCHOS || hw_w6f_motivo() == HW_W6F_M_DEFICIT ? R_LLENA : R_GENERAL, 0, 0, 0, 0); return; }
    id = hw_w6f_rx_id_d(HW_RX_MTP);
    if (id <= 0) { hw_w6f_rx_abortar_d(HW_RX_MTP); responder(R_GENERAL, 0, 0, 0, 0); return; }
    m.po.activo = 1; m.po.carpeta = 0; m.po.tj = 0; m.po.tam = tam; m.po.handle = handle_de((u32)id);
    responder(R_OK, 3, MTP_STORAGE, handle_de(pid), m.po.handle);
}
/* SendObject: el contenedor de datos ya ha llegado entero (o se ha tirado). */
static void fin_send_object(void)
{
    int r; u32 viejo = m.po.id_viejo, id = m.po.handle & 0xFFFFu;
    if (!m.so.activo) { responder(R_SIN_OBJINFO, 0, 0, 0, 0); return; }
    m.so.activo = 0;
    if (m.so.mal) { po_tirar(); responder(m.so.cod, 0, 0, 0, 0); return; }
    m.po.activo = 0;
    if (m.po.tj) {                                                 /* (FATW) la tarjeta: entero y del tamaño anunciado, o nada */
        if (m.so.hecho != m.po.tam) { hw_tj_rx_abortar_d(HW_RX_MTP); responder(R_INCOMPLETA, 0, 0, 0, 0); return; }
        if (hw_tj_rx_cerrar_d(HW_RX_MTP) != HW_OK) { responder(hw_tj_motivo() == HW_W6F_M_LLENO ? R_LLENA : R_INCOMPLETA, 0, 0, 0, 0); return; }
        m.inf.recibidos++; m.inf.rx_tam = m.inf.rx_hecho = 0;
        responder(R_OK, 0, 0, 0, 0);
        return;
    }
    if (m.so.hecho != m.po.tam) { hw_w6f_rx_abortar_d(HW_RX_MTP); responder(R_INCOMPLETA, 0, 0, 0, 0); return; }   /* menos de lo anunciado */
    if (hw_w6f_rx_cerrar_d(HW_RX_MTP) != HW_OK) { hw_w6f_borrar("/" MTP_TEMP); responder(R_INCOMPLETA, 0, 0, 0, 0); return; }
    /* el temporal está entero y comprometido: AHORA se le pone el nombre bueno (atómico; pisa el viejo si lo había) */
    if (viejo && hw_w6f_id(m.po.ruta) != (int)viejo) viejo = 0;   /* (el viejo ya no es el mismo) */
    r = hw_w6f_renombrar_r("/" MTP_TEMP, m.po.ruta, 1);
    if (r != HW_OK) { hw_w6f_borrar("/" MTP_TEMP); responder(hw_w6f_motivo() == HW_W6F_M_LLENO ? R_LLENA : R_GENERAL, 0, 0, 0, 0); return; }
    if (viejo && viejo != id) { u32 hv = handle_de(viejo); id_muere(viejo); evento(EV_OBJ_QUITADO, hv); }
    m.inf.recibidos++;
    if (en_apps(m.po.ruta)) m.apps_cambios++;
    m.inf.rx_tam = m.inf.rx_hecho = 0;
    responder(R_OK, 0, 0, 0, 0);
}
static void op_send_object(void)
{
    m.so.activo = m.po.activo && !m.po.carpeta; m.so.mal = 0; m.so.cod = R_OK; m.so.hecho = 0;
    m.tx = T_DATOS_ENT;
}
/* Los datos de SendObject, a trozos según llegan. */
static void datos_send_object(const u8 *d, u32 n, int primero, u32 total)
{
    if (!m.so.activo || m.so.mal) return;
    if (primero && total != m.po.tam) {                           /* el tamaño no cuadra con lo anunciado: se tira YA y se escurre lo demás */
        hw_log("mtp: SendObject de otro tamaño que el anunciado: se tira\n");
        if (m.po.tj) hw_tj_rx_abortar_d(HW_RX_MTP); else hw_w6f_rx_abortar_d(HW_RX_MTP);
        m.so.mal = 1; m.so.cod = R_INCOMPLETA; return;
    }
    if (!n) return;
    if (n > m.po.tam - m.so.hecho) {                              /* más de lo anunciado (el chequeo del principio ya lo impide; por si acaso) */
        if (m.po.tj) hw_tj_rx_abortar_d(HW_RX_MTP); else hw_w6f_rx_abortar_d(HW_RX_MTP);
        m.so.mal = 1; m.so.cod = R_INCOMPLETA; return;
    }
    if ((m.po.tj ? hw_tj_rx_mas_d(HW_RX_MTP, d, n) : hw_w6f_rx_mas_d(HW_RX_MTP, d, n)) != HW_OK) {
        m.so.mal = 1;
        m.so.cod = (m.po.tj ? hw_tj_motivo() : hw_w6f_motivo()) == HW_W6F_M_LLENO || (!m.po.tj && hw_w6f_motivo() == HW_W6F_M_DEFICIT) ? R_LLENA : R_GENERAL;
        hw_log("mtp: no se pudo escribir lo recibido: se tira\n");
        return;
    }
    m.so.hecho += n; m.inf.rx_hecho = m.so.hecho; m.inf.bytes_rx += n;
}
static void op_props_supported(void)
{
    u8 *p = m.dat;
    if (m.par[0] && !formato_conocido((u16)m.par[0])) { responder(R_FORMATO_MAL, 0, 0, 0, 0); return; }
    datos_buf(arr16(p, props, N(props)), R_OK);
}
static void op_prop_desc(void)
{
    u8 *p = m.dat; u32 k = 0; u16 tipo; HwW6fObj vacio;
    if (!prop_nuestra(m.par[0])) { responder(R_PROP_NO, 0, 0, 0, 0); return; }
    memset(&vacio, 0, sizeof vacio);
    prop_valor((u16)m.par[0], 0, &vacio, 0, &tipo);
    pon16(p, m.par[0]); pon16(p + 2, tipo); p[4] = (u8)(m.par[0] == P_NOMBRE_FICH); k = 5;   /* solo el nombre se cambia */
    switch (tipo) {                                                /* DefaultValue */
    case T_U16: pon16(p + k, 0); k += 2; break;
    case T_U32: pon32(p + k, 0); k += 4; break;
    case T_U64: memset(p + k, 0, 8); k += 8; break;
    case T_U128: memset(p + k, 0, 16); k += 16; break;
    default: p[k++] = 0; break;
    }
    pon32(p + k, 0); k += 4;                                       /* GroupCode */
    p[k++] = 0;                                                    /* FormFlag: ninguno */
    datos_buf(k, R_OK);
}
static void op_prop_value(void)
{
    HwW6fObj *o = &ob; u32 id = id_de(m.par[0], o); u16 tipo;
    if (!id) { responder(R_HANDLE_MAL, 0, 0, 0, 0); return; }
    if (!prop_nuestra(m.par[1])) { responder(R_PROP_NO, 0, 0, 0, 0); return; }
    datos_buf(prop_valor((u16)m.par[1], id, o, m.dat, &tipo), R_OK);
}
/* GetObjectPropList (MTP 1.1 D.2.?): handle, formato, propiedad, grupo, profundidad. */
static void op_prop_list(void)
{
    HwW6fObj *o = &ob; u32 h = m.par[0], prop = m.par[2], prof = m.par[4], id = 0; u8 amb;
    if (prop == 0) { responder(R_GRUPO_NO, 0, 0, 0, 0); return; }
    if (prop != 0xFFFFFFFFu && !prop_nuestra(prop)) { responder(R_PROP_NO, 0, 0, 0, 0); return; }
    if (h != 0 && h != 0xFFFFFFFFu && !(id = id_de(h, o))) { responder(R_HANDLE_MAL, 0, 0, 0, 0); return; }
    if (prof == 0) {
        if (h == 0xFFFFFFFFu) amb = A_TODOS;                       /* todos los objetos */
        else if (h == 0) { lista_empezar(L_PROPS, A_UNO, 0xFFFFFFFFu, 0, prop); return; }   /* ninguno */
        else amb = A_UNO;
    } else if (prof == 1) {
        if (id && !o->es_dir) { lista_empezar(L_PROPS, A_UNO, 0xFFFFFFFFu, 0, prop); return; }   /* un fichero no tiene hijos */
        amb = A_HIJOS;
    } else if (prof == 0xFFFFFFFFu) amb = id ? A_DESC : A_TODOS;
    else { responder(R_PROFUNDIDAD_NO, 0, 0, 0, 0); return; }
    lista_empezar(L_PROPS, amb, id, (u16)m.par[1], prop);
}
static void fin_set_prop_value(void)
{
    HwW6fObj *o = &ob; u32 id = id_de(m.par[0], o), pos = 0, l; char nom[HW_W6F_MAX_NOMBRE * 4 + 8]; int r;
    if (!id) { responder(R_HANDLE_MAL, 0, 0, 0, 0); return; }
    if (m.par[1] != P_NOMBRE_FICH && m.par[1] != P_NOMBRE) { responder(prop_nuestra(m.par[1]) ? R_DENEGADO : R_PROP_NO, 0, 0, 0, 0); return; }
    if (m.ent_rebosa || str_lee(m.ent, m.ent_n, &pos, nom, sizeof nom) || !nombre_bueno(nom)) { responder(R_PROP_VALOR_MAL, 0, 0, 0, 0); return; }
    if (ruta_de(id, ruta_a, sizeof ruta_a) || ruta_de(o->padre, ruta_b, sizeof ruta_b)) { responder(R_GENERAL, 0, 0, 0, 0); return; }
    if (!es_tj(id) && es_sistema(ruta_a)) { responder(R_DENEGADO, 0, 0, 0, 0); return; }
    l = largo(ruta_b);
    if (l + largo(nom) + 2 > sizeof ruta_b) { responder(R_PROP_VALOR_MAL, 0, 0, 0, 0); return; }
    if (l > 1) ruta_b[l++] = '/';
    memcpy(ruta_b + l, nom, largo(nom) + 1);
    if (es_tj(id)) {                                               /* (FATW) */
        u32 j;
        if (hw_tj_renombrar_r(ruta_a, ruta_b, 0) != HW_OK) { responder(hw_tj_motivo() == HW_W6F_M_EXISTE || hw_tj_motivo() == HW_W6F_M_NOMBRE ? R_PROP_VALOR_MAL : R_GENERAL, 0, 0, 0, 0); return; }
        for (j = 0; nom[j] && j < sizeof tj[0].nombre - 1; j++) tj[id - TJ_BASE].nombre[j] = nom[j];
        tj[id - TJ_BASE].nombre[j] = 0;
        responder(R_OK, 0, 0, 0, 0); return;
    }
    r = hw_w6f_renombrar_r(ruta_a, ruta_b, 0);
    if (r != HW_OK) { responder(hw_w6f_motivo() == HW_W6F_M_EXISTE ? R_PROP_VALOR_MAL : R_GENERAL, 0, 0, 0, 0); return; }
    if (en_apps(ruta_a) || en_apps(ruta_b)) m.apps_cambios++;
    responder(R_OK, 0, 0, 0, 0);
}
static void op_move(void)
{
    HwW6fObj *o = &ob; u32 id = id_de(m.par[0], o), pid = 0, l; int r;
    char nom[sizeof o->nombre];
    if (!id) { responder(R_HANDLE_MAL, 0, 0, 0, 0); return; }
    if (m.par[1] != storage_de(id)) { responder(m.par[1] == MTP_STORAGE || m.par[1] == MTP_STORAGE_TJ ? R_OP_NO : R_STORAGE_MAL, 0, 0, 0, 0); return; }   /* (FATW) entre storages: no (el host copia y borra) */
    memcpy(nom, o->nombre, sizeof nom);
    if (es_tj(id)) pid = TJ_BASE;
    if (m.par[2] && m.par[2] != 0xFFFFFFFFu) {
        if (!(pid = id_de(m.par[2], o)) || !o->es_dir || es_tj(pid) != es_tj(id)) { responder(R_PADRE_MAL, 0, 0, 0, 0); return; }
    }
    if (ruta_de(id, ruta_a, sizeof ruta_a) || ruta_de(pid, ruta_b, sizeof ruta_b)) { responder(R_GENERAL, 0, 0, 0, 0); return; }
    if (!es_tj(id) && es_sistema(ruta_a)) { responder(R_DENEGADO, 0, 0, 0, 0); return; }
    l = largo(ruta_b);
    if (l + largo(nom) + 2 > sizeof ruta_b) { responder(R_PADRE_MAL, 0, 0, 0, 0); return; }
    if (l > 1) ruta_b[l++] = '/';
    memcpy(ruta_b + l, nom, largo(nom) + 1);
    if (es_tj(id)) {                                               /* (FATW) dentro de la tarjeta */
        if (hw_tj_renombrar_r(ruta_a, ruta_b, 0) != HW_OK) { responder(hw_tj_motivo() == HW_W6F_M_DENTRO || hw_tj_motivo() == HW_W6F_M_NOMBRE ? R_PADRE_MAL : R_GENERAL, 0, 0, 0, 0); return; }
        tj[id - TJ_BASE].padre = (u16)(pid - TJ_BASE);
        responder(R_OK, 0, 0, 0, 0); return;
    }
    r = hw_w6f_renombrar_r(ruta_a, ruta_b, 0);
    if (r != HW_OK) { responder(hw_w6f_motivo() == HW_W6F_M_NOMBRE || hw_w6f_motivo() == HW_W6F_M_PROFUNDO ? R_PADRE_MAL : R_GENERAL, 0, 0, 0, 0); return; }
    if (en_apps(ruta_a) || en_apps(ruta_b)) m.apps_cambios++;
    responder(R_OK, 0, 0, 0, 0);
}
static void op_dev_prop(int desc)
{
    u8 *p = m.dat; u32 k = 0;
    if (m.par[0] != DP_NOMBRE_AMIGO) { responder(R_DEVPROP_NO, 0, 0, 0, 0); return; }
    if (desc) { pon16(p, DP_NOMBRE_AMIGO); pon16(p + 2, T_STR); p[4] = 0; k = 5; k += str_pon(p + k, NOMBRE_DISPOSITIVO); }
    k += str_pon(p + k, NOMBRE_DISPOSITIVO);
    if (desc) p[k++] = 0;
    datos_buf(k, R_OK);
}

/* ---- (Fase 3b) EL CANAL DE GESTIÓN: aquí solo la transacción; el formato y la validación, api/gestion.c; lo del sistema, shell/gestion_sis.c.
 * Con fase de datos de salida (de la computadora) no se devuelven datos: el resultado va en los tres parámetros de la respuesta. ------- */
static void op_sen_leer(int cod, u32 n) { if (cod == GS_R_OK) datos_buf(n, R_OK); else responder((u16)cod, 0, 0, 0, 0); }
static void fin_sen_aj_poner(void)
{
    u32 par[3]; int r = gs_op_aj_poner(m.ent, m.ent_n, m.ent_rebosa, par);
    if (r == GS_R_OK) responder(R_OK, 3, par[0], par[1], par[2]); else responder((u16)r, 0, 0, 0, 0);
}
/* ---- llega una orden ------------------------------------------------------------------------------------------------------- */
static int lleva_datos(u16 op) { return op == OP_SEND_OBJECT_INFO || op == OP_SEND_OBJECT || op == OP_SET_PROP_VALUE || op == OP_SEN_AJ_PONER || op == OP_SEN_RELOJ || op == OP_SEN_VINCULAR || op == OP_SEN_BT_EMPAREJAR; }
static void orden(void)
{
    u16 op = m.op;
    m.inf.transacciones++; m.inf.op = op; m.inf.ocupado = 1; m.t_ultimo = hw_ms();
    lista_st = 0;
    m.gen = G_NADA; m.resp_pend = 0; m.ent_n = 0; m.ent_rebosa = 0;
    if (op == OP_DEVICE_INFO) { op_device_info(); return; }
    if (op == OP_OPEN_SESSION) {
        if (m.sesion) { responder(R_SESION_YA, 1, m.sesion, 0, 0); return; }
        if (!m.par[0]) { responder(R_PARAM_MAL, 0, 0, 0, 0); return; }
        /* (AUD9) hasta que el usuario dice «Sí» en el teléfono: Device_Busy (que el host reintente); con «No»: Access_Denied */
        if (m.permiso != 1) { m.inf.pide_permiso = 1; responder(m.permiso == 2 ? R_DENEGADO : R_OCUPADO, 0, 0, 0, 0); return; }
        m.sesion = m.par[0]; m.inf.sesion = 1; responder(R_OK, 0, 0, 0, 0); return;
    }
    if (!m.sesion) {                                               /* sin sesión: nada más (y si trae datos, se escurren) */
        if (lleva_datos(op)) { m.tx = T_DATOS_ENT; m.so.activo = 0; m.op = 0; return; }
        responder(R_SIN_SESION, 0, 0, 0, 0); return;
    }
    switch (op) {
    case OP_CLOSE_SESSION:   po_tirar(); m.sesion = 0; m.inf.sesion = 0; responder(R_OK, 0, 0, 0, 0); return;
    case OP_STORAGE_IDS:     pon32(m.dat, tj_hay ? 2 : 1); pon32(m.dat + 4, MTP_STORAGE); pon32(m.dat + 8, MTP_STORAGE_TJ); datos_buf(tj_hay ? 12 : 8, R_OK); return;   /* (FATW) */
    case OP_STORAGE_INFO:    op_storage_info(); return;
    case OP_NUM_OBJECTS:     op_handles(1); return;
    case OP_OBJECT_HANDLES:  op_handles(0); return;
    case OP_OBJECT_INFO:     op_object_info(); return;
    case OP_GET_OBJECT:      op_get_object(0, 0xFFFFFFFFu, 0); return;
    case OP_PARTIAL_OBJECT:  op_get_object(m.par[1], m.par[2], 1); return;
    case OP_PARTIAL_OBJECT_64: if (m.par[2]) { responder(R_PARAM_MAL, 0, 0, 0, 0); return; } op_get_object(m.par[1], m.par[3], 1); return;
    case OP_DELETE_OBJECT:   op_delete(); return;
    case OP_SEND_OBJECT_INFO: case OP_SET_PROP_VALUE:
    case OP_SEN_AJ_PONER: case OP_SEN_RELOJ: case OP_SEN_VINCULAR: case OP_SEN_BT_EMPAREJAR: m.tx = T_DATOS_ENT; return;
    case OP_SEND_OBJECT:     op_send_object(); return;
    case OP_DEV_PROP_DESC:   op_dev_prop(1); return;
    case OP_DEV_PROP_VALUE:  op_dev_prop(0); return;
    case OP_MOVE_OBJECT:     op_move(); return;
    case OP_PROPS_SUPPORTED: op_props_supported(); return;
    case OP_PROP_DESC:       op_prop_desc(); return;
    case OP_PROP_VALUE:      op_prop_value(); return;
    case OP_PROP_LIST:       op_prop_list(); return;
    case OP_SEN_INFO:        datos_buf(gs_op_info(m.dat), R_OK); return;
    case OP_SEN_AJ_LEER:     { u32 n = 0; int r = gs_op_aj_leer(m.dat, sizeof m.dat, &n); op_sen_leer(r, n); return; }
    case OP_SEN_ENTRAR_BL:   responder((u16)gs_op_entrar_bl(), 0, 0, 0, 0); return;
    case OP_SEN_BT_ESTADO:   datos_buf(gs_op_bt(m.dat), R_OK); return;
    case OP_SEN_APPS:        { u32 n = 0; int r = gs_op_apps(m.par[0], m.dat, &n); op_sen_leer(r, n); return; }
    }
    responder(R_OP_NO, 0, 0, 0, 0);
}
/* Se acabó la fase de datos de la computadora: a terminar la operación. */
static void fin_datos(void)
{
    if (!m.sesion || m.op == 0) { responder(R_SIN_SESION, 0, 0, 0, 0); return; }
    if (m.op == OP_SEND_OBJECT_INFO) fin_send_object_info();
    else if (m.op == OP_SEND_OBJECT) fin_send_object();
    else if (m.op == OP_SET_PROP_VALUE) fin_set_prop_value();
    else if (m.op == OP_SEN_AJ_PONER) fin_sen_aj_poner();
    else if (m.op == OP_SEN_RELOJ) responder((u16)gs_op_reloj(m.ent, m.ent_n, m.ent_rebosa), 0, 0, 0, 0);
    else if (m.op == OP_SEN_VINCULAR) responder((u16)gs_op_vincular(m.ent, m.ent_n, m.ent_rebosa), 0, 0, 0, 0);
    else if (m.op == OP_SEN_BT_EMPAREJAR) responder((u16)gs_op_bt_emparejar(m.ent, m.ent_n, m.ent_rebosa), 0, 0, 0, 0);
    else responder(R_GENERAL, 0, 0, 0, 0);
}
/* Un paso del trabajo de una operación que no cabe en una vuelta. */
static void trabajo(void)
{
    if (m.op == OP_DELETE_OBJECT) {
        if (m.bo.cortado) { responder(m.bo.n ? R_BORRADO_PARCIAL : R_GENERAL, 0, 0, 0, 0); return; }   /* (AUD9 A1) ids viejos: no se sigue */
        if (!borrar_paso()) responder(!m.bo.parcial ? R_OK : m.bo.n ? R_BORRADO_PARCIAL : R_GENERAL, 0, 0, 0, 0);
        return;
    }
    if (m.op == OP_NUM_OBJECTS) {                                  /* contar a pasos, como la lista */
        lista_rellenar();
        if (m.li.contado) { m.gen = G_NADA; responder(R_OK, 1, m.li.n, 0, 0); }
        return;
    }
    responder(R_GENERAL, 0, 0, 0, 0);
}

/* ---- lo que llega del tubo, byte a byte de cabecera y a trozos de datos ------------------------------------------------------- */
static void abortar_transaccion(void)
{
    if (m.so.activo) po_tirar();                                  /* un SendObject a medias: el temporal se tira, no queda nada */
    m.so.activo = 0;
    m.tx = T_LIBRE; m.gen = G_NADA; m.resp_pend = 0; m.sal_i = m.sal_n = 0; m.ent_fase = E_CAB; m.cab_n = 0; m.inf.ocupado = 0;
}
static void consumir(const u8 *d, u32 n)
{
    u32 k;
    while (n) {
        if (m.ent_fase == E_CAB) {
            k = 12 - m.cab_n < n ? 12 - m.cab_n : n;
            memcpy(m.cab + m.cab_n, d, k); m.cab_n += k; d += k; n -= k;
            if (m.cab_n < 12) return;
            m.cab_n = 0;
            m.c_len = lee32(m.cab); m.c_tipo = (u16)lee16(m.cab + 4); m.c_cod = (u16)lee16(m.cab + 6); m.c_tid = lee32(m.cab + 8);
            if (m.c_len < 12) m.c_len = 12;
            m.falta = m.c_len - 12;
            if (m.c_tipo == C_ORDEN && m.c_len <= 32) {
                if (m.tx != T_LIBRE) { hw_log("mtp: orden nueva a media transacción: se abandona la anterior\n"); abortar_transaccion(); }
                m.op = m.c_cod; m.tid = m.c_tid; m.npar = 0; memset(m.par, 0, sizeof m.par);
                m.ent_fase = E_PARAMS;
            } else if (m.c_tipo == C_DATOS && m.tx == T_DATOS_ENT && (m.c_cod == m.op || !m.op)) {
                m.ent_fase = E_DATOS; m.t_ultimo = hw_ms();
                if (m.op == OP_SEND_OBJECT) datos_send_object(d, 0, 1, m.falta);
            } else m.ent_fase = E_TIRAR;                            /* datos que nadie espera, o algo raro: se escurre */
            if (m.ent_fase != E_PARAMS && m.ent_fase != E_DATOS && !m.falta) m.ent_fase = E_CAB;
            if (m.ent_fase == E_PARAMS && !m.falta) { m.ent_fase = E_CAB; orden(); }
            else if (m.ent_fase == E_DATOS && !m.falta) { m.ent_fase = E_CAB; fin_datos(); }
            continue;
        }
        k = m.falta < n ? m.falta : n;
        if (m.ent_fase == E_PARAMS) {
            u32 i;
            for (i = 0; i < k; i++) { u32 j = (m.c_len - 12 - m.falta) + i; if (j < 20) m.par[j / 4] |= (u32)d[i] << (8 * (j % 4)); }
        } else if (m.ent_fase == E_DATOS) {
            m.t_ultimo = hw_ms();
            if (m.op == OP_SEND_OBJECT) datos_send_object(d, k, 0, 0);
            else { u32 c = k; if (m.ent_n + c > sizeof m.ent) { c = sizeof m.ent - m.ent_n; m.ent_rebosa = 1; } memcpy(m.ent + m.ent_n, d, c); m.ent_n += c; }
        }
        m.falta -= k; d += k; n -= k;
        if (!m.falta) {
            u8 f = m.ent_fase;
            m.ent_fase = E_CAB;
            if (f == E_PARAMS) { m.npar = (m.c_len - 12) / 4; orden(); }
            else if (f == E_DATOS) fin_datos();
        }
    }
}

/* (AUD9 A1/M3) LA MEMORIA CAMBIÓ FUERA DE MTP (el teléfono escribió entre dos pasos; mtp_paso y el resto del sistema corren en
 * el mismo bucle, así que «fuera» es exactamente «entre el final de un paso y el principio del siguiente»): TODOS los handles
 * dejan de valer (la época sube), lo que se estaba haciendo con ids viejos se corta sin tocar nada más, y si hay sesión se le dice
 * al host que la storage se ha ido y ha vuelto (StoreRemoved + StoreAdded) para que la vuelva a leer. [inferido: libmtp/gvfs y
 * Windows releen con esos eventos; no visto con ellos] */
static int tj_lista(void) { HwTarjeta t; hw_tarjeta_estado(&t); return t.estado == HW_TJ_LISTA; }
static void tj_ajena(void);
static void ajeno(void)
{
    hw_log("mtp: la Memoria ha cambiado fuera de MTP: los handles se invalidan\n");
    m.epoca++; m.inf.ajenos++;
    if (m.gen == G_LISTA) { if (m.li.cabecera) m.li.mal = 1; else { m.li.contado = 0; m.li.cursor = 0; m.li.n = 0; m.li.bytes = 0; } }
    if (m.gen == G_FICH) m.fi.mal = 1;                            /* ese id puede ser ya otro fichero: ceros y General_Error */
    if (m.tx == T_TRABAJO && m.op == OP_DELETE_OBJECT) { m.bo.parcial = 1; m.bo.cortado = 1; }   /* no se borra nada más por ids viejos */
    if (m.tx == T_TRABAJO && m.op == OP_NUM_OBJECTS) { m.li.contado = 0; m.li.cursor = 0; m.li.n = 0; m.li.bytes = 0; }
    if (m.po.activo) m.po.id_viejo = 0;                           /* el que iba a pisar ya no se sabe quién es: se mira por la ruta al final */
    if (m.sesion) { evento(EV_STORE_QUITADA, MTP_STORAGE); evento(EV_STORE_PUESTA, MTP_STORAGE); }
    m.visto = hw_w6f_cambios();
}
/* (FATW) LA TARJETA: entra (StoreAdded), sale (StoreRemoved) o cambia fuera de MTP (las dos, como la Memoria): la tabla se tira
 * y la época sube, así ningún handle viejo de la tarjeta vuelve a valer. Lo que se hacía con ella se corta. */
static void tj_ajena(void)
{
    u8 antes = tj_hay; u32 g = hwo_tarjeta_generacion();
    tj_visto = hw_tarjeta_cambios();
    if (hw_tj_cambios() == tj_cambios_vistos && g == tj_gen && (u8)tj_lista() == antes) return;   /* el paso de la tarjeta se movió, pero es la MISMA */
    tj_cambios_vistos = hw_tj_cambios(); tj_gen = g;
    tj_hay = (u8)tj_lista();
    tj_tirar();
    if (m.po.activo && m.po.tj) po_tirar();
    if (m.gen == G_LISTA && m.li.st != 1) { if (m.li.cabecera) m.li.mal = 1; else { m.li.contado = 0; m.li.cursor = 0; m.li.n = 0; m.li.bytes = 0; } }
    if (m.gen == G_FICH && es_tj(m.fi.id)) m.fi.mal = 1;
    if (m.tx == T_TRABAJO && m.op == OP_DELETE_OBJECT && es_tj(m.bo.cur)) { m.bo.parcial = 1; m.bo.cortado = 1; }
    hw_log(antes && !tj_hay ? "mtp: la tarjeta se ha ido\n" : !antes && tj_hay ? "mtp: hay tarjeta: segunda storage\n" : "mtp: la tarjeta ha cambiado fuera de MTP\n");
    if (!m.sesion) return;
    if (antes) evento(EV_STORE_QUITADA, MTP_STORAGE_TJ);
    if (tj_hay) evento(EV_STORE_PUESTA, MTP_STORAGE_TJ);
}
void mtp_permitir(int si)
{
    m.permiso = si ? 1 : 2; m.inf.pide_permiso = 0;
    hw_log(si ? "mtp: el usuario PERMITE el acceso a la Memoria\n" : "mtp: el usuario NIEGA el acceso a la Memoria\n");
    if (!si && m.sesion) { abortar_transaccion(); po_tirar(); m.sesion = 0; m.inf.sesion = 0; }
}
int mtp_permiso(void) { return m.permiso; }
int mtp_sesion_abierta(void) { return m.vivo && m.sesion != 0; }

/* ---- el contrato -------------------------------------------------------------------------------------------------------------- */
void mtp_iniciar(void)
{
    u32 ap = m.apps_cambios;
    memset(&m, 0, sizeof m);
    m.apps_cambios = ap;
    m.vivo = 1;
    if (hw_w6f_id("/" MTP_TEMP) > 0) { hw_log("mtp: había un temporal de una recepción cortada: se borra\n"); hw_w6f_borrar("/" MTP_TEMP); }
    if (hw_w6f_id("/" MTP_DIR_APPS) < 0) { HwW6fEstado e; hw_w6f_estado(&e); if (e.escribible) hw_w6f_mkdir("/" MTP_DIR_APPS); }
    m.visto = hw_w6f_cambios();
    tj_tirar(); tj_hay = (u8)tj_lista(); tj_visto = hw_tarjeta_cambios(); tj_cambios_vistos = hw_tj_cambios(); tj_gen = hwo_tarjeta_generacion();
    gs_iniciar();
    hw_log("mtp: en marcha (sin permiso hasta el «Sí» del teléfono)\n");
}
void mtp_parar(void)
{
    if (!m.vivo) return;
    abortar_transaccion();
    po_tirar();
    m.vivo = 0; m.sesion = 0; m.inf.sesion = 0;
    gs_parar();
    hw_log("mtp: parado\n");
}
void mtp_paso(void)
{
    static u8 trozo[1024];
    u32 t0, gasto = 0;
    int k, hizo, aviso;
    if (!m.vivo) return;
    if (hw_w6f_cambios() != m.visto) ajeno();                     /* (AUD9 A1/M3) la Memoria cambió FUERA de MTP desde el último paso */
    if (hw_tarjeta_cambios() != tj_visto || hw_tj_cambios() != tj_cambios_vistos) tj_ajena();   /* (FATW) la tarjeta entró, salió o cambió fuera */
    aviso = usb_tubo_aviso();
    if (aviso) {                                                   /* cancelar (clase 0x64), reset de MTP (0x66) o del bus: el tubo ya se vació */
        abortar_transaccion();
        if (aviso != USB_AVISO_CANCELAR) { po_tirar(); m.sesion = 0; m.inf.sesion = 0; }
    }
    if (m.ev_n && usb_tubo_evento(m.ev[0], 16) > 0) { m.ev_n--; memmove(m.ev[0], m.ev[1], sizeof m.ev - 16); }
    t0 = hw_ms();
    do {
        hizo = 0;
        if (m.sal_i < m.sal_n) {
            k = usb_tubo_escribir(m.sal + m.sal_i, m.sal_n - m.sal_i);
            if (k > 0) { m.sal_i += (u32)k; gasto += (u32)k; hizo = 1; }
            if (m.sal_i >= m.sal_n) m.sal_i = m.sal_n = 0;
            else if (m.sal_i > sizeof m.sal / 2) { u32 q = m.sal_n - m.sal_i; memcpy(m.sal, m.sal + m.sal_i, q); m.sal_i = 0; m.sal_n = q; }
        }
        if (m.tx == T_SALIDA) hizo |= rellenar();
        else if (m.tx == T_TRABAJO) { trabajo(); hizo = 1; gasto += 512; }
        else {
            k = usb_tubo_leer(trozo, sizeof trozo);
            if (k > 0) { consumir(trozo, (u32)k); gasto += (u32)k; hizo = 1; }
        }
    } while (hizo && gasto < MTP_PASO_BYTES && hw_ms() - t0 < MTP_PASO_MS);
    /* una fase de datos de la computadora que se ha parado: se da por perdida (el temporal se tira) */
    if (m.tx == T_DATOS_ENT && hw_ms() - m.t_ultimo > MTP_PLAZO_MS) {
        hw_log("mtp: la computadora dejó de mandar datos: se abandona la transacción\n");
        abortar_transaccion();
    }
    m.visto = hw_w6f_cambios();                                    /* lo cambiado dentro de este paso es de MTP */
    tj_cambios_vistos = hw_tj_cambios();
}
u32 mtp_apps_cambios(void) { return m.apps_cambios; }
void mtp_info(MtpInfo *i)
{
    HwTarjeta t;
    *i = m.inf;
    hw_tarjeta_estado(&t);
    i->almacenes = (u8)(tj_hay ? 2 : 1); i->tarjeta = t.estado;
    if (m.so.activo) { i->rx_tam = m.po.tam; i->rx_hecho = m.so.hecho; }
}
