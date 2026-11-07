/* Los ficheros de la API (hw_fs_*): reparte entre NUESTRO sistema de ficheros (W6FS, w6fs.c), el original
 * del teléfono (lo pone cada plataforma: hwo_fs_*) y, desde la S6 del W580, la zona de ficheros de usuario
 * (hw_usr_*), que es la carpeta «/usuario», y desde la I1 la Memoria (W6F, hw_w6f_*), la carpeta «/memoria». Reglas en api/hw.h. Portable. */
#include "hw.h"
#include "w6fs.h"
#include "tarjeta.h"                             /* (pista T) la memoria externa: la carpeta «/tarjeta» */

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
/* lo que pone la plataforma */
const u8 *hwo_w6fs(int i, u32 *max);             /* i-ésimo sitio donde puede haber un W6FS (NULL = no hay más) */
int  hwo_fs_listo(void);
int  hwo_fs_listar(const char *dir, u32 desde, HwEntrada *e, u32 max);
u32  hwo_fs_tam(const char *ruta);
u32  hwo_fs_leer(const char *ruta, u8 *buf, u32 max);
u32  hwo_fs_leer_desde(const char *ruta, u32 desde, u8 *buf, u32 max);
void hwo_fs_estado(HwFsEstado *e);

static W6fs propio;
static signed char mirado;                       /* 0 = sin mirar, 1 = hay nuestro, -1 = no */
static int hay_propio(void)
{
    if (!mirado) {
        int i; u32 max; const u8 *b;
        mirado = -1;
        for (i = 0; (b = hwo_w6fs(i, &max)) != 0 || i == 0; i++) if (b && w6fs_montar(&propio, b, max)) { mirado = 1; hw_log("fs: W6FS propio montado\n"); break; }
    }
    return mirado > 0;
}
/* ruta dentro del FS original, o NULL si la ruta es del nuestro */
static const char *del_original(const char *r)
{
    static const char pre[] = "/original";
    unsigned i;
    if (!hay_propio()) return r;
    for (i = 0; pre[i]; i++) if (r[i] != pre[i]) return 0;
    if (r[i] == 0) return "/";
    return r[i] == '/' ? r + i : 0;
}
/* (S6) La zona de ficheros de usuario (hw_usr_*) como la carpeta HW_FS_USUARIO. Devuelve NULL si la ruta no es de
 * ahí; "" si es la carpeta misma; y si no, el nombre del fichero (lo que va detrás de «/usuario/»; si trae otra «/»
 * no es un nombre válido y hw_usr_* dirá que no existe: la zona es PLANA). */
static const char *del_usuario(const char *r)
{
    static const char pre[] = HW_FS_USUARIO;
    unsigned i;
    for (i = 0; pre[i]; i++) if (r[i] != pre[i]) return 0;
    if (r[i] == 0) return "";
    return r[i] == '/' ? r + i + 1 : 0;
}
static int usr_hay(void) { HwUsrEstado u; hw_usr_estado(&u); return u.montado; }
/* (I1) LA MEMORIA (W6F, api/hw_w6f.c) es la carpeta HW_FS_MEMORIA: devuelve NULL si la ruta no es de ahí, y si no, la ruta
 * DENTRO de la Memoria ("" = la carpeta misma; «/fotos/a.png»). Con carpetas, de verdad. */
static const char *del_memoria(const char *r)
{
    static const char pre[] = HW_FS_MEMORIA;
    unsigned i;
    for (i = 0; pre[i]; i++) if (r[i] != pre[i]) return 0;
    if (r[i] == 0) return "";
    return r[i] == '/' ? r + i : 0;
}
int hw_w6f_hay(void);
int hwi_w6f_listar(const char *ruta, u32 desde, HwEntrada *e, u32 max);
int hwi_w6f_tam(const char *ruta);
int hwi_w6f_leer(const char *ruta, u32 desde, u8 *buf, u32 max);
int hwi_w6f_guardar(const char *ruta, const u8 *d, u32 n);

int hw_fs_listo(void) { return hay_propio() || hwo_fs_listo(); }
static int listar_fs(const char *dir, u32 desde, HwEntrada *e, u32 max)   /* los dos FS de solo lectura */
{
    const char *o = del_original(dir);
    int i, n;
    if (o) return hwo_fs_listo() ? hwo_fs_listar(o, desde, e, max) : HW_ERROR;
    if ((i = w6fs_buscar(&propio, dir)) < 0) return HW_ERROR;
    n = w6fs_listar(&propio, i, desde, e, max);
    if (i == 0 && n >= 0 && hwo_fs_listo()) {    /* en la raíz, al final: la entrada «original» */
        u32 propios = w6fs_tam(&propio, 0);
        if (!e) return n + 1;
        if ((u32)n < max && desde + (u32)n == propios) { memcpy(e[n].nombre, "original", 9); e[n].tam = 0; e[n].es_dir = 1; n++; }
    }
    return n;
}
static int listar_sin_memoria(const char *dir, u32 desde, HwEntrada *e, u32 max);
static int listar_sin_tarjeta(const char *dir, u32 desde, HwEntrada *e, u32 max)
{
    const char *m = del_memoria(dir);
    int n, total;
    if (m) return hw_w6f_hay() ? hwi_w6f_listar(m, desde, e, max) : HW_ERROR;
    n = listar_sin_memoria(dir, desde, e, max);
    if (dir[0] != '/' || dir[1] || !hw_w6f_hay()) return n;
    /* (I1) en la raíz, lo ÚLTIMO de todo (detrás de «usuario»: así no se mueve ninguna fila de las de antes): «memoria» */
    total = listar_sin_memoria(dir, 0, 0, 0);
    if (total < 0) total = 0;
    if (!e) return total + 1;
    if (n < 0) n = 0;
    if ((u32)n < max && desde + (u32)n == (u32)total) { memcpy(e[n].nombre, HW_FS_MEMORIA + 1, sizeof HW_FS_MEMORIA - 1); e[n].tam = 0; e[n].es_dir = 1; n++; }
    return n;
}
static int listar_sin_memoria(const char *dir, u32 desde, HwEntrada *e, u32 max)
{
    const char *u = del_usuario(dir);
    int n;
    if (u) {                                     /* la carpeta de la zona de usuario: plana, por orden de nombre */
        HwUsrEstado es; HwUsrEntrada ue; u32 k, j;
        if (*u || !usr_hay()) return HW_ERROR;
        hw_usr_estado(&es);
        if (!e) return (int)es.ficheros;
        for (k = 0; k < max && hw_usr_listar(desde + k, &ue, 1) == 1; k++) {
            for (j = 0; ue.nombre[j] && j < sizeof e[k].nombre - 1; j++) e[k].nombre[j] = ue.nombre[j];
            e[k].nombre[j] = 0; e[k].tam = ue.tam; e[k].es_dir = 0;
        }
        return (int)k;
    }
    n = listar_fs(dir, desde, e, max);
    if (dir[0] == '/' && !dir[1] && usr_hay()) { /* en la raíz, lo último de todo: la carpeta «usuario» (aunque no haya ningún FS) */
        int total = listar_fs(dir, 0, 0, 0);
        if (total < 0) total = 0;
        if (!e) return total + 1;
        if (n < 0) n = 0;
        if ((u32)n < max && desde + (u32)n == (u32)total) { memcpy(e[n].nombre, HW_FS_USUARIO + 1, sizeof HW_FS_USUARIO - 1); e[n].tam = 0; e[n].es_dir = 1; n++; }
    }
    return n;
}
/* (pista T) Y la tarjeta: lo que cuelga de «/tarjeta» lo contesta api/tarjeta.c; en la raíz, detrás de todo lo demás (también de
 * la Memoria: así lo que entra y sale no mueve ninguna otra fila), su entrada («tarjeta», o «tarjeta (motivo)» si no sirve). */
int hw_fs_listar(const char *dir, u32 desde, HwEntrada *e, u32 max)
{
    const char *t = tj_ruta(dir);
    char nombre[64];
    int n;
    if (t) return tj_listar(t, desde, e, max);
    n = listar_sin_tarjeta(dir, desde, e, max);
    if (dir[0] == '/' && !dir[1] && tj_entrada(nombre)) {
        int total = listar_sin_tarjeta(dir, 0, 0, 0);
        if (total < 0) total = 0;
        if (!e) return total + 1;
        if (n < 0) n = 0;
        if ((u32)n < max && desde + (u32)n == (u32)total) { memcpy(e[n].nombre, nombre, sizeof nombre); e[n].tam = 0; e[n].es_dir = 1; n++; }
    }
    return n;
}
int hw_fs_contar(const char *dir) { return hw_fs_listar(dir, 0, 0, 0); }
/* (AUD8 A1) Una prueba POSITIVA de que el origen existe: se busca en el listado de su carpeta (el mismo camino que ve Archivos).
 * hw_fs_tam da 0 tanto a un fichero vacío como a uno que no existe o no se puede leer, y copiar «0 bytes» reemplazaba el destino
 * por un fichero vacío. Un nombre que no cabe en HwEntrada (recortado) no se encuentra: también se niega. */
int hw_fs_mirar(const char *ruta, u32 *tam)
{
    static char dir[224]; static HwEntrada e;
    const char *b = ruta, *p; u32 n, i, k;
    if (!ruta || ruta[0] != '/') return HW_NO_DISPONIBLE;
    for (p = ruta; *p; p++) if (*p == '/') b = p + 1;
    n = (u32)(b - ruta);
    if (!*b || n >= sizeof dir) return HW_NO_DISPONIBLE;
    memcpy(dir, ruta, n); dir[n > 1 ? n - 1 : 1] = 0;          /* la carpeta, sin la «/» final (salvo la raíz) */
    for (i = 0; hw_fs_listar(dir, i, &e, 1) == 1; i++) {
        for (k = 0; e.nombre[k] && e.nombre[k] == b[k]; k++) ;
        if (e.nombre[k] || b[k]) continue;
        if (e.es_dir) return HW_NO_DISPONIBLE;
        if (tam) *tam = e.tam;
        return HW_OK;
    }
    return HW_NO_DISPONIBLE;
}
u32 hw_fs_tam(const char *ruta)
{
    const char *u = del_usuario(ruta), *o, *m = del_memoria(ruta); int i;
    if ((o = tj_ruta(ruta)) != 0) return tj_tam(o);
    if (m) { i = hwi_w6f_tam(m); return i > 0 ? (u32)i : 0; }
    if (u) { i = *u ? hw_usr_tam(u) : 0; return i > 0 ? (u32)i : 0; }
    o = del_original(ruta);
    if (o) return hwo_fs_listo() ? hwo_fs_tam(o) : 0;
    i = w6fs_buscar(&propio, ruta);
    return i < 0 || w6fs_es_dir(&propio, i) ? 0 : w6fs_tam(&propio, i);
}
u32 hw_fs_leer_desde(const char *ruta, u32 desde, u8 *buf, u32 max)
{
    const char *u = del_usuario(ruta), *o, *m = del_memoria(ruta); int i; u32 n;
    if ((o = tj_ruta(ruta)) != 0) return tj_leer_desde(o, desde, buf, max);
    if (m) { i = hwi_w6f_leer(m, desde, buf, max); return i > 0 ? (u32)i : 0; }   /* (I1) una lectura corta de W6F es un error: 0, nunca medio fichero */
    if (u) { i = *u ? hw_usr_leer(u, desde, buf, max) : 0; return i > 0 ? (u32)i : 0; }
    o = del_original(ruta);
    if (o) return hwo_fs_listo() ? (desde ? hwo_fs_leer_desde(o, desde, buf, max) : hwo_fs_leer(o, buf, max)) : 0;
    i = w6fs_buscar(&propio, ruta);
    if (i < 0 || w6fs_es_dir(&propio, i)) return 0;
    n = w6fs_tam(&propio, i);
    if (desde >= n) return 0;
    n -= desde; if (n > max) n = max;
    memcpy(buf, w6fs_datos(&propio, i) + desde, n);
    return n;
}
u32 hw_fs_leer(const char *ruta, u8 *buf, u32 max) { return hw_fs_leer_desde(ruta, 0, buf, max); }
const u8 *hw_fs_mapa(const char *ruta, u32 *tam)
{
    int i;
    if (tj_ruta(ruta) || del_usuario(ruta) || del_memoria(ruta) || del_original(ruta) || (i = w6fs_buscar(&propio, ruta)) < 0 || w6fs_es_dir(&propio, i)) return 0;
    *tam = w6fs_tam(&propio, i);
    return w6fs_datos(&propio, i);
}
/* Escribir y borrar: solo en la carpeta de la zona de usuario (lo demás es de solo lectura). El motivo de un
 * HW_ERROR, en hw_usr_motivo(). */
int hw_fs_escribir(const char *ruta, const u8 *buf, u32 n)
{
    const char *u = del_usuario(ruta), *m = del_memoria(ruta), *t = tj_ruta(ruta);
    if (t) return tj_escribir(t, buf, n);                                  /* (FATW) en la tarjeta: crear o reemplazar entero */
    if (m) return *m ? hwi_w6f_guardar(m, buf, n) : HW_NO_DISPONIBLE;     /* (I1) en la Memoria: guardar entero y atómico */
    return u && *u ? hw_usr_escribir(u, buf, n) : HW_NO_DISPONIBLE;
}
int hw_fs_borrar(const char *ruta)
{
    const char *u = del_usuario(ruta), *m = del_memoria(ruta), *t = tj_ruta(ruta);
    if (t) return tj_borrar(t);                                            /* (FATW) en la tarjeta: un fichero o una carpeta vacía */
    if (m) return *m ? hw_w6f_borrar(m) : HW_NO_DISPONIBLE;               /* (I1) un fichero o una carpeta vacía */
    return u && *u ? hw_usr_borrar(u) : HW_NO_DISPONIBLE;
}
void hw_fs_estado(HwFsEstado *e)
{
    hwo_fs_estado(e);
    e->original = e->montado; e->propio = (u8)hay_propio();
    e->propio_entradas = e->propio ? propio.n : 0; e->propio_bytes = e->propio ? propio.tam : 0;
    e->montado = e->propio || e->original;
}
