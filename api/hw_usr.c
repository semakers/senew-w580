/* hw_usr_* — los FICHEROS DE USUARIO de la API de hardware: api/usuario.c (el formato W6U) puesto sobre el
 * puerto de flash que dé la plataforma. Pista S6 del W600; en la S6 del W580 se le añade copiar por trozos y
 * renombrar (api/usrop.c) y el motivo de cada fallo. Portable: el teléfono da la zona 0x45C00000..0x45DFFFFF (hal/hal.h)
 * de la NOR (hal/nor.c: nor_usr_*) y el PC un modelo en RAM (o en el fichero que diga W600_USR).
 *
 * Lo que promete (y demuestra herr/pruebas/prueba_usuario.c cortando la corriente en cada operación de
 * flash): hw_usr_escribir y hw_usr_borrar son ATÓMICAS por fichero. Tras un corte en cualquier punto, ese
 * fichero vale entero lo de antes o entero lo nuevo, y los demás no cambian. */
#include "hw.h"
#include "usuario.h"

const AlmPuerto *hwo_usr_puerto(int *puede_escribir);   /* la plataforma: el puerto de la zona, o NULL si no la hay */
void hwo_usr_tras_escribir(void);                       /* la plataforma: el PC vuelca su fichero; el teléfono, nada */

static void (*prog_f)(u32, u32);
void hw_progreso_gancho(void (*f)(u32 hecho, u32 total)) { prog_f = f; }
void hw_progreso(u32 hecho, u32 total) { hwo_progreso(); if (prog_f) prog_f(hecho, total); }

static signed char hay;                                 /* 0 = sin mirar · 1 = montada · -1 = no hay */
static u8 escribible;

int hw_usr_iniciar(void)
{
    int puede = 0;
    const AlmPuerto *p = hwo_usr_puerto(&puede);
    hay = -1; escribible = 0;
    if (!p) { hw_log("usr: no hay zona de usuario\n"); return HW_NO_DISPONIBLE; }
    usr_al_paso(hw_progreso);                           /* (R3) cada registro copiado o grabado y cada borrado: progreso */
    if (usr_montar(p, puede) != USR_OK) { hw_log(puede ? "usr: la zona de usuario no se pudo montar\n" : "usr: sin escritura y sin volumen: no hay zona de usuario\n"); return HW_NO_DISPONIBLE; }
    hay = 1; escribible = (u8)(puede != 0);
    if (puede) hwo_usr_tras_escribir();                 /* por si el montaje le dio formato */
    hw_log("usr: zona de usuario montada\n");
    return HW_OK;
}
static int lista(void) { if (!hay) hw_usr_iniciar(); return hay > 0; }

int hw_usr_tam(const char *nombre)
{
    UsrEntrada e;
    if (!lista() || usr_info(nombre, &e) != USR_OK) return HW_NO_DISPONIBLE;
    return (int)e.tam;
}
int hw_usr_leer(const char *nombre, u32 desde, void *buf, u32 max)
{
    int r;
    if (!lista()) return HW_NO_DISPONIBLE;
    r = usr_leer(nombre, desde, buf, max);
    return r >= 0 ? r : r == USR_E_NO_HAY ? HW_NO_DISPONIBLE : HW_ERROR;
}
/* ---- escribir, borrar, copiar y renombrar. Lo que va mal queda en hw_usr_motivo() para que la pantalla lo
 * diga con sus palabras (lleno, nombre repetido, demasiado largo, 64 ficheros…). ------------------------------- */
#include "usrop.h"
static UsroOp op;                                       /* la copia o el renombrado EN MARCHA (por pasos), si lo hay */
static char op_ruta[200];                               /* de dónde copia: una ruta de hw_fs */
static u8 motivo;

int hw_usr_motivo(void) { return motivo; }
static u8 empujando;
int hw_usr_ocupada(void) { return op.activa || empujando; }
static int traducir(int e)                              /* de USR_E_* / USRO_E_* a HW_* + motivo */
{
    motivo = e == USR_OK ? HW_USR_M_NADA : e == USR_E_LLENO ? HW_USR_M_LLENO : e == USR_E_ARG ? HW_USR_M_NOMBRE : e == USR_E_MUCHOS ? HW_USR_M_MUCHOS :
             e == USRO_E_EXISTE ? HW_USR_M_EXISTE : e == USR_E_NO_HAY ? HW_USR_M_NO_HAY : e == USRO_E_OCUPADA ? HW_USR_M_OCUPADA :
             e == USRO_E_FUENTE ? HW_USR_M_FUENTE : e == USRO_E_MISMO ? HW_USR_M_MISMO : e == USR_E_SIN_MONTAR ? HW_USR_M_SIN_ZONA : HW_USR_M_FLASH;
    return e == USR_OK ? HW_OK : e == USR_E_NO_HAY ? HW_NO_DISPONIBLE : HW_ERROR;
}
static int puede(void)                                  /* ¿se puede empezar una escritura ahora? Si no, deja dicho por qué */
{
    if (!lista()) { motivo = HW_USR_M_SIN_ZONA; return 0; }
    if (!escribible) { motivo = HW_USR_M_SOLO_LECTURA; return 0; }
    if (op.activa || empujando) { motivo = HW_USR_M_OCUPADA; return 0; }   /* una copia a medias tiene abierta la única escritura a trozos */
    return 1;
}
static u32 ahora(void) { HwFecha f; return hw_rtc_leer(&f) == HW_OK ? hw_fecha_seg(&f) : 0; }

int hw_usr_escribir(const char *nombre, const void *d, u32 n)
{
    int r;
    if (!puede()) return motivo == HW_USR_M_OCUPADA ? HW_ERROR : HW_NO_DISPONIBLE;
    r = usr_escribir(nombre, d, n, ahora());
    hwo_usr_tras_escribir();
    return r == USR_OK ? traducir(r) : (traducir(r), HW_ERROR);   /* HW_ERROR: no cabe, nombre malo, ya hay 64, o la flash: hw_usr_motivo() */
}
int hw_usr_borrar(const char *nombre)
{
    int r;
    if (!puede()) return motivo == HW_USR_M_OCUPADA ? HW_ERROR : HW_NO_DISPONIBLE;
    r = usr_borrar(nombre);
    hwo_usr_tras_escribir();
    return traducir(r);
}
static int de_fs(void *dato, u32 desde, u8 *buf, u32 n) { (void)dato; return (int)hw_fs_leer_desde(op_ruta, desde, buf, n); }
int hw_usr_copiar(const char *destino, const char *ruta_fs, int reemplazar)
{
    u32 i;
    if (!puede()) return motivo == HW_USR_M_OCUPADA ? HW_ERROR : HW_NO_DISPONIBLE;
    for (i = 0; ruta_fs && ruta_fs[i] && i < sizeof op_ruta - 1; i++) op_ruta[i] = ruta_fs[i];
    op_ruta[i] = 0;
    if (!i || (ruta_fs[i] != 0)) { motivo = HW_USR_M_FUENTE; return HW_ERROR; }
    {   /* (AUD8 A1) el origen tiene que EXISTIR y leerse con su tamaño: un 0 de hw_fs_tam (tarjeta sacada, fichero borrado) reemplazaba por vacío */
        u32 t;
        if (hw_fs_mirar(op_ruta, &t) != HW_OK || hw_fs_tam(op_ruta) != t) { motivo = HW_USR_M_FUENTE; hw_log("usr: copiar: el origen ya no esta o no se lee: NO se toca el destino\n"); return HW_ERROR; }
    }
    return traducir(usro_copiar(&op, destino, hw_fs_tam(op_ruta), ahora(), reemplazar, de_fs, 0)) == HW_OK ? HW_OK : HW_ERROR;
}
int hw_usr_renombrar(const char *viejo, const char *nuevo)
{
    if (!puede()) return motivo == HW_USR_M_OCUPADA ? HW_ERROR : HW_NO_DISPONIBLE;
    return traducir(usro_renombrar(&op, viejo, nuevo)) == HW_OK ? HW_OK : HW_ERROR;
}
int hw_usr_paso(u32 max, u32 *hecho, u32 *total)
{
    int r;
    if (!op.activa) {                                   /* no hay nada en marcha: alguien la abandonó (hw_usr_cancelar: p. ej. al apagar). NO se ha hecho */
        motivo = HW_USR_M_CANCELADA; if (hecho) *hecho = op.hecho; if (total) *total = op.tam; return HW_ERROR;
    }
    r = usro_paso(&op, max);
    if (hecho) *hecho = op.hecho;
    if (total) *total = op.tam;
    if (r == USRO_SIGUE) return 1;
    hwo_usr_tras_escribir();
    /* (R3; AUD3-B4) con un error de la FLASH no se puede prometer «queda como estaba»: si lo que falló fue la relectura del
     * sello, el fichero nuevo ya está comprometido. Lo que sí es verdad siempre: queda ENTERO, lo de antes o lo nuevo. */
    hw_log(r == USR_OK ? "usr: copia o renombrado HECHO\n" : r == USR_E_FLASH ? "usr: copia o renombrado con ERROR DE LA FLASH (el destino vale entero lo de antes o entero lo nuevo)\n"
                                                                              : "usr: copia o renombrado FALLIDO (el destino queda como estaba)\n");
    return traducir(r) == HW_OK ? HW_OK : HW_ERROR;
}
/* Escritura «a empujones» de un fichero cuyo contenido se va GENERANDO (la captura de pantalla): empezar con el
 * tamaño exacto, mas las veces que haga falta, acabar. Es la escritura a trozos de usuario.c tal cual. Quien la usa
 * la hace ENTERA sin soltar el bucle (no convive con una copia por pasos ni con otra escritura). */
int hw_usr_empezar(const char *nombre, u32 tam)
{
    if (!puede()) return motivo == HW_USR_M_OCUPADA ? HW_ERROR : HW_NO_DISPONIBLE;
    if (traducir(usr_empezar(nombre, tam, ahora())) != HW_OK) return HW_ERROR;
    empujando = 1;
    return HW_OK;
}
int hw_usr_mas(const void *d, u32 n)
{
    if (!empujando) { motivo = HW_USR_M_NADA; return HW_ERROR; }
    if (traducir(usr_mas(d, n)) == HW_OK) return HW_OK;
    empujando = 0; hwo_usr_tras_escribir();
    return HW_ERROR;
}
int hw_usr_acabar(void)
{
    int r;
    if (!empujando) { motivo = HW_USR_M_NADA; return HW_ERROR; }
    empujando = 0;
    r = usr_acabar();
    hwo_usr_tras_escribir();
    return traducir(r) == HW_OK ? HW_OK : HW_ERROR;
}
void hw_usr_cancelar(void)
{
    if (op.activa) { usro_cancelar(&op); hwo_usr_tras_escribir(); hw_log("usr: copia o renombrado CANCELADO\n"); }
    if (empujando) { empujando = 0; usr_cancelar(); hwo_usr_tras_escribir(); }
}
int hw_usr_listar(u32 desde, HwUsrEntrada *e, u32 max)
{
    UsrEntrada u;
    u32 n = 0, i;
    if (!lista() || !e) return 0;
    for (; n < max && usr_listar(desde + n, &u, 1) == 1; n++) {
        for (i = 0; i < sizeof e[n].nombre - 1 && u.nombre[i]; i++) e[n].nombre[i] = u.nombre[i];
        e[n].nombre[i] = 0; e[n].tam = u.tam; e[n].fecha = u.fecha;
    }
    return (int)n;
}
void hw_usr_estado(HwUsrEstado *e)
{
    UsrEstado s;
    if (!e) return;
    lista();
    usr_estado(&s);
    *e = (HwUsrEstado){ .montado = (u8)(hay > 0), .escribible = (u8)(hay > 0 && escribible), .bloques = s.bloques, .tam_bloque = s.tam_bloque,
                        .ficheros = s.ficheros, .ocupado = s.ocupado, .libre = s.libre, .limpiezas = s.limpiezas, .errores = s.errores };
}
