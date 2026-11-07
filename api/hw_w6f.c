/* hw_w6f_* — (I1) «LA MEMORIA»: el sistema de ficheros escribible propio, W6F (api/w6f.c, de la pista F2, TAL CUAL; formato y
 * garantías en docs/fs_propio.md), puesto sobre el puerto de flash que dé la plataforma (hwo_w6f_flash: el teléfono, la zona
 * 0x44B00000..0x45AFFFFF de hal/nor.c en modo objeto; el PC, un modelo en RAM). Portable. docs/sistema.md §18.
 *
 * LAS CINCO CONDICIONES DE LA AUDITORÍA (docs/auditoria_fs_propio.md §S5; fs2/INTEGRACION.md §2), aquí:
 *  (1) los cambios de HAL son los de fs2/INTEGRACION.md §1, rehechos a mano (hal/nor.c, hal.h, enlace.ld, Makefile);
 *  (2) un fallo pasajero no declara «sin reserva»: está en w6f.c (x_borrar, hay_salida); make probar-w6f pasa sus reproductores;
 *  (3) w6f_gc_paso EN LOS RATOS MUERTOS: hw_w6f_ocioso() (lo llama el bucle principal cuando ui_paso() no pinta) mientras quede
 *      trabajo; y TRAS MONTAR, antes de la primera escritura, hasta que diga 0 o pase medio segundo (gc_inicial);
 *  (4) los códigos: W6F_E_GEOMETRIA / W6F_E_VERSION = NO dar formato (situación OTRA_CONF / OTRA_VERSION: el shell lo dice),
 *      W6F_E_AJENO = preguntar (hw_formato_pendiente: HW_ZONA_MEMORIA), W6F_E_VIRGEN / W6F_E_FORMATO = dar formato (no destruye
 *      nada: el shell lo hace con la pantalla de «preparando»), W6F_E_OCUPADO = «luego» (HW_W6F_M_DENTRO), y LEER compara lo
 *      leído con el tamaño: una lectura corta (página rota) es un ERROR, nunca «fin de fichero»;
 *  (5) el tope de secuencia: en w6f.c (SEQ_TOPE).
 * Y: el FORMATO avisa de cada bloque con hw_progreso (perro y franja «Guardando…»); la recolección dentro de una escritura,
 * también (respiro). Ninguna función de aquí desde una interrupción. */
#include "hw.h"
#include "w6f.h"
#include "w6f_sistema.h"
#include "serie_rx.h"                       /* (INT2) SRX_TEMPORAL: se borra al arrancar */

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);

static W6f fs;
static uint32_t mem[W6F_SIS_MEM_U32];
static W6fConf conf;
static const W6fFlash *fl;
static u8 situ, puede, gc_inicial, gc_pend, motivo;
static u32 pasos_fondo, peor_paso_ms, montaje_ms;
static u32 cambios;                         /* (MTP; AUD9 A1) ver tras() */

static u32 ahora(void) { HwFecha f; return hw_rtc_leer(&f) == HW_OK ? hw_fecha_seg(&f) : 0; }
static void respiro(void *c) { (void)c; hw_progreso(0, 0); }                         /* dentro de una escritura que recolecta */
static void paso_formato(void *c, uint32_t h, uint32_t t) { (void)c; hw_progreso(h, t); }   /* 64 borrados: 13–18 s en el emulador */
static void dice(const char *a, u32 v, const char *b)
{
    char t[12]; int n = 11; t[11] = 0; do { t[--n] = (char)('0' + v % 10); v /= 10; } while (v);
    hw_log(a); hw_log(t + n); hw_log(b);
}

static int montar(void)
{
    u32 t0 = hw_ms();
    int r = w6f_montar(&fs, fl, &conf, mem, sizeof mem);
    montaje_ms = hw_ms() - t0;
    if (r == W6F_OK && hwo_w6f_fallo_de_prueba()) { fs.montado = 0; r = W6F_E_FLASH; }   /* (AUD7 M1) solo el PC: un volumen NUESTRO que «no monta» */
    switch (r) {
    case W6F_OK:          cambios++; situ = HW_W6F_MONTADO; gc_inicial = (u8)!conf.solo_lectura; gc_pend = gc_inicial;
                          dice("w6f: Memoria montada (", fs.n_obj, conf.solo_lectura ? " objetos, SOLO LECTURA" : " objetos)"); dice(" en ", montaje_ms, " ms\n"); break;
    case W6F_E_VIRGEN:    situ = HW_W6F_VIRGEN; hw_log("w6f: la zona de la Memoria esta en blanco: falta darle formato\n"); break;
    case W6F_E_FORMATO:   situ = HW_W6F_VIRGEN; hw_log("w6f: un formato de la Memoria se quedo a medias: hay que repetirlo\n"); break;
    case W6F_E_AJENO:     situ = HW_W6F_AJENO;  hw_log("w6f: la zona de la Memoria tiene datos que NO son de este sistema: NO se formatea sin permiso\n"); break;
    case W6F_E_GEOMETRIA: situ = HW_W6F_OTRA_CONF; hw_log("w6f: *** la Memoria se hizo con OTRA configuracion (zona u objetos): NO se toca ***\n"); break;
    case W6F_E_VERSION:   situ = HW_W6F_OTRA_VERSION; hw_log("w6f: *** la Memoria es de OTRA version del formato: NO se toca ***\n"); break;
    default:              situ = HW_W6F_FALLO; dice("w6f: la Memoria no se pudo montar (error -", (u32)-r, ")\n"); break;
    }
    return r;
}
int hw_w6f_iniciar(void)
{
    int p = 0;
    situ = HW_W6F_NO_HAY; puede = 0; gc_inicial = gc_pend = 0;
    fl = hwo_w6f_flash(&p);
    if (!fl) { hw_log("w6f: no hay zona de la Memoria\n"); return HW_NO_DISPONIBLE; }
    puede = (u8)(p != 0);
    conf = w6f_sis_conf(!puede);
    conf.respiro = respiro;
    if (w6f_mem_necesaria(fl, &conf) == 0 || w6f_mem_necesaria(fl, &conf) > sizeof mem) {
        situ = HW_W6F_FALLO; dice("w6f: *** la geometria pide ", w6f_mem_necesaria(fl, &conf), " bytes de RAM: no hay Memoria ***\n");
        return HW_NO_DISPONIBLE;
    }
    montar();
    /* sin permiso de escritura, un volumen que no se puede montar tal cual no se toca ni se ofrece: no hay Memoria */
    if (!puede && situ != HW_W6F_MONTADO && situ != HW_W6F_OTRA_CONF && situ != HW_W6F_OTRA_VERSION) situ = HW_W6F_NO_HAY;
    /* (INT2; AUD9 BAJO) el temporal de SER (api/serie_rx.h: SRX_TEMPORAL) que dejara un corte entre «cerrar» y «renombrar» queda
     * comprometido, oculto y ocupando sitio hasta la próxima cabecera de SER: se borra al arrancar, como hace MTP con el suyo */
    if (situ == HW_W6F_MONTADO && puede && hw_w6f_id(SRX_TEMPORAL) > 0) {
        hw_log("w6f: habia un temporal de SER de una recepcion cortada: se borra\n"); hw_w6f_borrar(SRX_TEMPORAL);
    }
    return situ == HW_W6F_MONTADO ? HW_OK : HW_NO_DISPONIBLE;
}
/* hw_formatear(HW_ZONA_MEMORIA) de la plataforma llama a esto con ajena = 1 (el usuario ha dicho que sí) */
int hw_w6f_formatear_zona(int ajena);
int hw_w6f_formatear_zona(int ajena)
{
    int r;
    if (!fl || !puede) return HW_NO_DISPONIBLE;
    if (situ == HW_W6F_OTRA_CONF || situ == HW_W6F_OTRA_VERSION) return HW_NO_DISPONIBLE;   /* NUNCA: condición 4 de la auditoría */
    if (situ == HW_W6F_AJENO && !ajena) return HW_NO_DISPONIBLE;                              /* lo ajeno, solo con el sí del usuario */
    /* (AUD7 M1) un volumen que NO SE PUDO MONTAR (FALLO) no se formatea nunca por aquí: puede ser nuestro y con datos. Solo lo que
     * está en blanco (o con un formato nuestro a medias), y lo ajeno tras el sí del usuario. */
    if (situ != HW_W6F_VIRGEN && situ != HW_W6F_AJENO) return HW_NO_DISPONIBLE;
    hw_log(situ == HW_W6F_AJENO ? "w6f: formato de la Memoria, CONFIRMADO por el usuario\n" : "w6f: formato de la Memoria (zona en blanco)\n");
    conf.respiro = 0;
    r = w6f_formatear(fl, &conf, mem, sizeof mem, paso_formato, 0);
    conf.respiro = respiro;
    hwo_w6f_tras_escribir();
    if (r) { dice("w6f: el formato fallo (error -", (u32)-r, ")\n"); montar(); return HW_ERROR; }
    return montar() == W6F_OK ? HW_OK : HW_ERROR;
}
int hw_w6f_formatear(void) { return hw_w6f_formatear_zona(0); }
int hw_w6f_ajena(void);
int hw_w6f_ajena(void) { return situ == HW_W6F_AJENO && puede; }

void hw_w6f_estado(HwW6fEstado *e)
{
    W6fEstado w;
    memset(e, 0, sizeof *e);
    e->situacion = situ;
    e->pasos_fondo = pasos_fondo; e->peor_paso_ms = peor_paso_ms; e->montaje_ms = montaje_ms;
    if (situ != HW_W6F_MONTADO) return;
    w6f_estado(&fs, &w);
    e->solo_lectura = (u8)w.solo_lectura; e->deficit = (u8)w.deficit;
    e->escribible = (u8)(puede && !w.solo_lectura);
    e->capacidad = w.paginas_cap * (fs.carga ? fs.carga : 1); e->libre = w.bytes_libres;
    e->objetos = w.objetos; e->max_obj = w.max_obj; e->bloques = w.bloques; e->bloques_malos = w.bloques_malos;
    e->borrados_min = w.borrados_min; e->borrados_max = w.borrados_max;
    e->programas = fs.cta.programas; e->borrados = fs.cta.borrados;
}
int hw_w6f_hay(void);
int hw_w6f_hay(void) { return situ == HW_W6F_MONTADO; }

/* ---- condición 3: la recolección en los ratos muertos ------------------------------------------------------------------ */
static int gc_un_paso(void)
{
    u32 t0 = hw_ms(), dt; int r = w6f_gc_paso(&fs, W6F_SIS_TOPE);
    dt = hw_ms() - t0; if (dt > peor_paso_ms) peor_paso_ms = dt;
    pasos_fondo++;
    if (r <= 0) gc_pend = gc_inicial = 0;
    if (r < 0 && r != W6F_E_OCUPADO) dice("w6f: el paso de recoleccion dio -", (u32)-r, "\n");
    return r > 0;
}
/* TOPE por episodio (lo que va de una escritura o de montar a que la recolección dice «nada más»): si w6f_gc_paso dijera «queda
 * trabajo» sin parar, el bucle no volvería a dormir nunca (batería). Con el tope, se deja y se dice; la escritura siguiente
 * abre otro episodio. 4096 pasos son de sobra: un volumen entero (64 bloques x 256 páginas) a 32 programas por paso son 512. */
#define W6F_PASOS_EPISODIO 4096u
static u32 pasos_episodio;
int hw_w6f_ocioso(void)
{
    if (situ != HW_W6F_MONTADO || !gc_pend || conf.solo_lectura) return 0;
    gc_un_paso();
    if (gc_pend && ++pasos_episodio >= W6F_PASOS_EPISODIO) { gc_pend = 0; hw_log("w6f: *** la recoleccion no acaba: se deja (tope de pasos) ***\n"); }
    if (!gc_pend) pasos_episodio = 0;
    hwo_w6f_tras_escribir();
    return 1;
}
static void gc_antes_de_escribir(void)          /* tras montar, antes de la primera escritura: hasta 0 o medio segundo */
{
    u32 t0;
    if (!gc_inicial) return;
    u32 n = 0;
    t0 = hw_ms();
    while (gc_inicial && hw_ms() - t0 < 500u && n < 64u) { gc_un_paso(); n++; }   /* (y como mucho 64 pasos: si el reloj no avanzara, no se queda) */
    gc_inicial = 0;
    dice("w6f: antes de la primera escritura, ", n, " pasos de recoleccion"); dice(" en ", hw_ms() - t0, " ms\n");
}

/* ---- leer ------------------------------------------------------------------------------------------------------------ */
static const char *rel(const char *r) { return *r ? r : "/"; }
int hw_w6f_info(const char *ruta, HwW6fInfo *i)
{
    W6fInfo inf; int id;
    memset(i, 0, sizeof *i);
    if (situ != HW_W6F_MONTADO || (id = w6f_buscar(&fs, rel(ruta))) < 0) return HW_NO_DISPONIBLE;
    if (w6f_info(&fs, (uint16_t)id, &inf)) { i->danado = 1; return HW_OK; }
    i->tam = inf.tam; i->fecha = inf.fecha; i->es_dir = inf.es_dir; i->danado = inf.danado;
    return HW_OK;
}
/* (AUD7 B6) Un nombre de W6F que no cabe en HwEntrada (63) no se enseña como si fuera el nombre: se corta en una frontera UTF-8 y se
 * marca con «…» (U+2026). El recorte ocupa siempre 60..63 bytes. Solo puede venir de fuera (desde el teléfono no se crean > 63). */
#define RECORTE_MAX ((u32)sizeof ((HwEntrada *)0)->nombre)
static void recortar(char *dst, const char *largo)
{
    u32 j = RECORTE_MAX - 4;
    memcpy(dst, largo, j);
    while (j && ((u8)largo[j] & 0xC0) == 0x80) j--;
    dst[j] = (char)0xE2; dst[j + 1] = (char)0x80; dst[j + 2] = (char)0xA6; dst[j + 3] = 0;
}
/* (AUD7 B6, el resto) ¿El último componente de «ruta» es el RECORTE de un nombre largo de su misma carpeta? Entonces borrar o
 * renombrar por él obraría sobre otro objeto (el que se llame exactamente así, si existe) o daría un «no existe» engañoso: se niega
 * con HW_W6F_M_NOMBRE. Un nombre de 60..63 bytes acabado en «…» que NO es el recorte de nadie se opera como cualquier otro. */
static int es_recorte(const char *ruta)
{
    static W6fInfo inf;
    static char padre[224], r[RECORTE_MAX];
    const char *b = ruta, *p; u32 n, lp; uint32_t cur = 0; int id, x;
    for (p = ruta; *p; p++) if (*p == '/') b = p + 1;
    for (n = 0; b[n]; n++) ;
    if (n < RECORTE_MAX - 4 || n >= RECORTE_MAX || (u8)b[n - 3] != 0xE2 || (u8)b[n - 2] != 0x80 || (u8)b[n - 1] != 0xA6) return 0;
    lp = (u32)(b - ruta);
    if (lp >= sizeof padre) return 1;                          /* no se puede mirar: por si acaso, no */
    memcpy(padre, ruta, lp); padre[lp] = 0;
    if (lp > 1) padre[lp - 1] = 0;                             /* sin la «/» final, salvo la raíz */
    if ((id = w6f_buscar(&fs, rel(padre))) < 0) return 0;
    while ((x = w6f_listar(&fs, (uint16_t)id, &cur, &inf)) != 0) {
        if (x < 0) { if (x == W6F_E_DANADO) continue; break; }
        for (n = 0; inf.nombre[n] && n < RECORTE_MAX; n++) ;
        if (n < RECORTE_MAX) continue;                         /* cabe: no es largo */
        recortar(r, inf.nombre);
        for (n = 0; r[n] && r[n] == b[n]; n++) ;
        if (r[n] == b[n]) return 1;
    }
    return 0;
}
/* hw_fs.c: la carpeta, a ventanas. El orden es el de la tabla de objetos (más o menos, el de creación). */
int hwi_w6f_listar(const char *ruta, u32 desde, HwEntrada *e, u32 max);
int hwi_w6f_listar(const char *ruta, u32 desde, HwEntrada *e, u32 max)
{
    static W6fInfo inf;
    uint32_t cur = 0; u32 k = 0, i = 0, j; int id, r;
    if (situ != HW_W6F_MONTADO || (id = w6f_buscar(&fs, rel(ruta))) < 0) return HW_ERROR;
    if (id && (w6f_info(&fs, (uint16_t)id, &inf) || !inf.es_dir)) return HW_ERROR;
    while ((r = w6f_listar(&fs, (uint16_t)id, &cur, &inf)) != 0) {
        if (r < 0 && r != W6F_E_DANADO) break;
        if (i++ < desde) continue;
        if (!e) { k++; continue; }
        if (k >= max) break;
        if (r < 0) { memcpy(e[k].nombre, "(dañado)", 10); e[k].tam = 0; e[k].es_dir = 0; k++; continue; }
        for (j = 0; inf.nombre[j] && j < sizeof e[k].nombre - 1; j++) e[k].nombre[j] = inf.nombre[j];
        e[k].nombre[j] = 0; e[k].tam = inf.es_dir ? 0 : inf.tam; e[k].es_dir = inf.es_dir;
        if (inf.nombre[j]) recortar(e[k].nombre, inf.nombre);
        k++;
    }
    return (int)k;
}
int hwi_w6f_tam(const char *ruta);
int hwi_w6f_tam(const char *ruta) { HwW6fInfo i; return hw_w6f_info(ruta, &i) == HW_OK && !i.es_dir ? (int)i.tam : -1; }
/* Condición 4 (N4): o los bytes pedidos (los que haya hasta el final), o NADA. Una página rota a mitad no se toma por el final. */
int hwi_w6f_leer(const char *ruta, u32 desde, u8 *buf, u32 max);
int hwi_w6f_leer(const char *ruta, u32 desde, u8 *buf, u32 max)
{
    W6fInfo inf; int id, r; u32 quiero;
    if (situ != HW_W6F_MONTADO || (id = w6f_buscar(&fs, rel(ruta))) <= 0 || w6f_info(&fs, (uint16_t)id, &inf) || inf.es_dir) return -1;
    if (desde >= inf.tam) return 0;
    quiero = inf.tam - desde < max ? inf.tam - desde : max;
    r = w6f_leer(&fs, (uint16_t)id, desde, buf, quiero);
    if (r != (int)quiero) { dice("w6f: lectura corta o mala (", (u32)(r < 0 ? 0 : r), " bytes): el fichero esta danado ahi\n"); return -2; }
    return r;
}

/* ---- escribir ---------------------------------------------------------------------------------------------------------- */
static int traducir(int e)
{
    motivo = e == W6F_OK ? HW_W6F_M_NADA : e == W6F_E_LLENO ? HW_W6F_M_LLENO : e == W6F_E_NOMBRE || e == W6F_E_ARG ? HW_W6F_M_NOMBRE :
             e == W6F_E_MUCHOS ? HW_W6F_M_MUCHOS : e == W6F_E_EXISTE ? HW_W6F_M_EXISTE : e == W6F_E_NO_HAY ? HW_W6F_M_NO_HAY :
             e == W6F_E_OCUPADO ? HW_W6F_M_DENTRO : e == W6F_E_NO_VACIO ? HW_W6F_M_NO_VACIA : e == W6F_E_NO_DIR ? HW_W6F_M_NO_DIR :
             e == W6F_E_ES_DIR ? HW_W6F_M_ES_DIR : e == W6F_E_CRC || e == W6F_E_DANADO ? HW_W6F_M_DANADO : e == W6F_E_PROFUNDO ? HW_W6F_M_PROFUNDO :
             e == W6F_E_SOLO_LECTURA ? HW_W6F_M_SOLO_LECTURA : e == W6F_E_SIN_MONTAR ? HW_W6F_M_SIN_ZONA : HW_W6F_M_FLASH;
    if (e == W6F_E_LLENO) { W6fEstado w; w6f_estado(&fs, &w); if (w.deficit) motivo = HW_W6F_M_DEFICIT; }   /* no está lleno: falta un bloque de reserva */
    if (e != W6F_OK && e != W6F_E_OCUPADO) dice("w6f: la operacion dio -", (u32)-e, "\n");
    return e == W6F_OK ? HW_OK : e == W6F_E_NO_HAY ? HW_NO_DISPONIBLE : HW_ERROR;
}
static struct { u8 activa; int h; u32 hecho, total; char ruta[224]; } cp;
int hw_w6f_motivo(void) { return motivo; }
int hw_w6f_ocupada(void) { return cp.activa; }
static int puede_escribir(void)
{
    if (situ != HW_W6F_MONTADO) { motivo = HW_W6F_M_SIN_ZONA; return 0; }
    if (!puede || conf.solo_lectura) { motivo = HW_W6F_M_SOLO_LECTURA; return 0; }
    gc_antes_de_escribir();
    return 1;
}
static int nombre_ok(const char *ruta)          /* el último componente cabe en HwEntrada (W6F admitiría 255) */
{
    const char *b = ruta, *p; u32 n = 0;
    for (p = ruta; *p; p++) if (*p == '/') b = p + 1;
    while (b[n]) n++;
    if (n > HW_W6F_MAX_NOMBRE) { motivo = HW_W6F_M_NOMBRE; return 0; }
    return 1;
}
/* (MTP; AUD9 A1/M3) CADA operación que puede cambiar lo que hay en la Memoria pasa por aquí (con éxito o no: por si acaso) y sube el
 * contador de cambios. El respondedor MTP lo mira al empezar cada paso: si subió fuera de él, sus handles y listas ya no valen. */
u32 hw_w6f_cambios(void) { return cambios; }
static int tras(int r) { cambios++; gc_pend = 1; hwo_w6f_tras_escribir(); return traducir(r); }
int hwi_w6f_guardar(const char *ruta, const u8 *d, u32 n);
int hwi_w6f_guardar(const char *ruta, const u8 *d, u32 n)
{
    if (!puede_escribir()) return HW_NO_DISPONIBLE;
    if (!nombre_ok(ruta)) return HW_ERROR;
    return tras(w6f_guardar(&fs, rel(ruta), d, n, ahora()));
}
int hw_w6f_mkdir(const char *ruta)
{
    if (!puede_escribir()) return HW_NO_DISPONIBLE;
    if (!nombre_ok(ruta)) return HW_ERROR;
    return tras(w6f_mkdir(&fs, rel(ruta), ahora()));
}
int hw_w6f_borrar(const char *ruta)
{
    if (!puede_escribir()) return HW_NO_DISPONIBLE;
    if (es_recorte(rel(ruta))) { motivo = HW_W6F_M_NOMBRE; hw_log("w6f: borrar por un nombre RECORTADO: no\n"); return HW_ERROR; }
    return tras(w6f_borrar(&fs, rel(ruta)));
}
int hw_w6f_renombrar(const char *vieja, const char *nueva)
{
    if (!puede_escribir()) return HW_NO_DISPONIBLE;
    if (!nombre_ok(nueva)) return HW_ERROR;
    if (es_recorte(rel(vieja))) { motivo = HW_W6F_M_NOMBRE; hw_log("w6f: renombrar un nombre RECORTADO: no\n"); return HW_ERROR; }
    return tras(w6f_renombrar(&fs, rel(vieja), rel(nueva), 0));
}
/* ---- copiar a la Memoria POR PASOS (como hw_usr_copiar): el destino vale lo de antes hasta w6f_cerrar ------------------ */
int hw_w6f_copiar(const char *destino, const char *ruta_fs, int reemplazar)
{
    W6fInfo inf; W6fEstado w; int id; u32 n;
    if (cp.activa) { motivo = HW_W6F_M_OCUPADA; return HW_ERROR; }
    if (!puede_escribir()) return HW_NO_DISPONIBLE;
    if (!nombre_ok(destino)) return HW_ERROR;
    for (n = 0; ruta_fs[n]; n++) ;
    if (n >= sizeof cp.ruta) { motivo = HW_W6F_M_NOMBRE; return HW_ERROR; }
    id = w6f_buscar(&fs, rel(destino));
    if (id >= 0 && !w6f_info(&fs, (uint16_t)id, &inf)) {
        if (inf.es_dir) { motivo = HW_W6F_M_ES_DIR; return HW_ERROR; }
        if (!reemplazar) { motivo = HW_W6F_M_EXISTE; return HW_ERROR; }
    }
    {   /* (AUD8 A1) el origen tiene que EXISTIR y leerse con el tamaño que dice su carpeta: un 0 de hw_fs_tam no basta */
        u32 t;
        if (hw_fs_mirar(ruta_fs, &t) != HW_OK || (cp.total = hw_fs_tam(ruta_fs)) != t) { motivo = HW_W6F_M_FUENTE; hw_log("w6f: copiar: el origen ya no esta o no se lee: NO se toca el destino\n"); return HW_ERROR; }
    }
    w6f_estado(&fs, &w);
    if (cp.total > w.bytes_libres) { motivo = w.deficit ? HW_W6F_M_DEFICIT : HW_W6F_M_LLENO; return HW_ERROR; }   /* reemplazar: el viejo vale hasta el final */
    cp.h = w6f_abrir(&fs, rel(destino), W6F_CREAR, ahora());
    if (cp.h < 0) return traducir(cp.h);
    memcpy(cp.ruta, ruta_fs, n + 1);
    cp.hecho = 0; cp.activa = 1; motivo = HW_W6F_M_NADA;
    return HW_OK;
}
int hw_w6f_paso(u32 max, u32 *hecho, u32 *total)
{
    static u8 trozo[4096];
    u32 hasta;
    int r = W6F_OK;
    if (!cp.activa) { motivo = HW_W6F_M_CANCELADA; return HW_ERROR; }
    hasta = cp.hecho + (max < sizeof trozo ? (u32)sizeof trozo : max);
    while (cp.hecho < cp.total && cp.hecho < hasta && r == W6F_OK) {
        u32 n = cp.total - cp.hecho < sizeof trozo ? cp.total - cp.hecho : (u32)sizeof trozo, l = hw_fs_leer_desde(cp.ruta, cp.hecho, trozo, n);
        if (l != n) { w6f_abortar(&fs, cp.h); cp.activa = 0; motivo = HW_W6F_M_FUENTE; gc_pend = 1; return HW_ERROR; }
        r = w6f_escribir(&fs, cp.h, trozo, n);
        cp.hecho += n;
    }
    if (hecho) *hecho = cp.hecho;
    if (total) *total = cp.total;
    if (r != W6F_OK) { w6f_abortar(&fs, cp.h); cp.activa = 0; return tras(r); }
    if (cp.hecho < cp.total) return 1;
    cp.activa = 0;
    return tras(w6f_cerrar(&fs, cp.h));
}
void hw_w6f_cancelar(void) { if (cp.activa) { w6f_abortar(&fs, cp.h); cp.activa = 0; gc_pend = 1; } }
/* ---- (SER) recibir a la Memoria A TROZOS, sin fichero de origen (apps/recibir: lo que llega por la UART 0) -------------------
 * Como la copia de arriba, pero los datos los da quien llama: hw_w6f_rx_abrir (un fichero que NO existe hasta hw_w6f_rx_cerrar),
 * hw_w6f_rx_mas, hw_w6f_rx_cerrar (el compromiso) o hw_w6f_rx_abortar (no queda nada). Y renombrar CON reemplazo, atómico: el
 * paso final de «temporal -> nombre bueno». El protocolo y sus reglas están en api/serie_rx.c. */
/* (MTP; AUD9 M2) LA RANURA TIENE DUEÑO: quien la abre (SER o MTP) es el único que puede escribir, cerrar o abortar en ella; el otro
 * recibe HW_ERROR con motivo HW_W6F_M_OCUPADA y NO toca nada. Las cuatro de SER de siempre son las de dueño HW_RX_SER. */
static struct { u8 activa, dueno; int h; } rx;
int hw_w6f_rx_abrir_d(const char *ruta, int dueno)
{
    if (rx.activa || cp.activa) { motivo = HW_W6F_M_OCUPADA; return HW_ERROR; }
    if (!puede_escribir()) return HW_NO_DISPONIBLE;
    if (!nombre_ok(ruta)) return HW_ERROR;
    rx.h = w6f_abrir(&fs, rel(ruta), W6F_CREAR, ahora());
    if (rx.h < 0) return traducir(rx.h);
    rx.activa = 1; rx.dueno = (u8)dueno; motivo = HW_W6F_M_NADA;
    return HW_OK;
}
static int rx_mia(int dueno) { if (rx.activa && rx.dueno == dueno) return 1; motivo = rx.activa ? HW_W6F_M_OCUPADA : HW_W6F_M_CANCELADA; return 0; }
int hw_w6f_rx_mas_d(int dueno, const void *d, u32 n)
{
    int r;
    if (!rx_mia(dueno)) return HW_ERROR;
    r = w6f_escribir(&fs, rx.h, d, n);
    if (r != W6F_OK) { w6f_abortar(&fs, rx.h); rx.activa = 0; return tras(r); }
    return HW_OK;
}
int hw_w6f_rx_cerrar_d(int dueno)
{
    if (!rx_mia(dueno)) return HW_ERROR;
    rx.activa = 0;
    return tras(w6f_cerrar(&fs, rx.h));
}
void hw_w6f_rx_abortar_d(int dueno) { if (rx.activa && rx.dueno == dueno) { w6f_abortar(&fs, rx.h); rx.activa = 0; gc_pend = 1; } }
int hw_w6f_rx_dueno(void) { return rx.activa ? rx.dueno : 0; }
int hw_w6f_rx_abrir(const char *ruta) { return hw_w6f_rx_abrir_d(ruta, HW_RX_SER); }
int hw_w6f_rx_mas(const void *d, u32 n) { return hw_w6f_rx_mas_d(HW_RX_SER, d, n); }
int hw_w6f_rx_cerrar(void) { return hw_w6f_rx_cerrar_d(HW_RX_SER); }
void hw_w6f_rx_abortar(void) { hw_w6f_rx_abortar_d(HW_RX_SER); }
int hw_w6f_renombrar_r(const char *vieja, const char *nueva, int reemplazar)
{
    if (!puede_escribir()) return HW_NO_DISPONIBLE;
    if (!nombre_ok(nueva)) return HW_ERROR;
    return tras(w6f_renombrar(&fs, rel(vieja), rel(nueva), reemplazar != 0));
}
u32 hw_w6f_libre(void)
{
    W6fEstado w;
    if (situ != HW_W6F_MONTADO) return 0;
    w6f_estado(&fs, &w);
    return w.deficit || w.solo_lectura ? 0 : w.bytes_libres;
}

/* ---- (MTP) lo que necesita el respondedor MTP (api/mtp.c; docs/mtp.md) y SER no tiene ------------------------------------
 * Los objetos POR ID (el id de W6F es estable mientras el objeto vive: un renombrado lo conserva; el que «pisa» uno con
 * reemplazar se queda con el SUYO y el pisado desaparece), la lectura por id y el id que reserva la recepción a trozos de SER
 * (hw_w6f_rx_abrir): es el handle que MTP devuelve en SendObjectInfo, antes de que el fichero exista. */
static void a_obj(const W6fInfo *inf, HwW6fObj *o)
{
    u32 j;
    o->id = inf->id; o->padre = inf->padre; o->es_dir = inf->es_dir; o->danado = inf->danado;
    o->tam = inf->es_dir ? 0 : inf->tam; o->fecha = inf->fecha;
    for (j = 0; inf->nombre[j] && j < sizeof o->nombre - 1; j++) o->nombre[j] = inf->nombre[j];
    o->nombre[j] = 0;
}
int hw_w6f_obj(u32 id, HwW6fObj *o)
{
    static W6fInfo inf;
    memset(o, 0, sizeof *o);
    if (situ != HW_W6F_MONTADO || id >= conf.max_obj) return HW_NO_DISPONIBLE;
    if (id == 0) { o->es_dir = 1; return HW_OK; }
    if (w6f_info(&fs, (uint16_t)id, &inf)) return HW_NO_DISPONIBLE;
    a_obj(&inf, o);
    return HW_OK;
}
int hw_w6f_hijo(u32 dir, u32 *cursor, HwW6fObj *o)
{
    static W6fInfo inf;
    int r;
    if (situ != HW_W6F_MONTADO || dir >= conf.max_obj) return HW_ERROR;
    for (;;) {
        uint32_t c = *cursor;                           /* (coordinador) u32 y uint32_t son tipos distintos en arm-none-eabi */
        r = w6f_listar(&fs, (uint16_t)dir, &c, &inf);
        *cursor = c;
        if (r == W6F_E_DANADO) continue;                /* el que no se puede leer no se enseña (Archivos sí lo dice) */
        if (r <= 0) return r < 0 ? HW_ERROR : 0;
        a_obj(&inf, o);
        return 1;
    }
}
int hw_w6f_id(const char *ruta) { return situ == HW_W6F_MONTADO ? w6f_buscar(&fs, rel(ruta)) : -1; }
u32 hw_w6f_max_obj(void) { return situ == HW_W6F_MONTADO ? conf.max_obj : 0; }
int hw_w6f_rx_id_d(int dueno) { return rx.activa && rx.dueno == dueno ? (int)fs.escr[rx.h].id : -1; }
/* Por id, con la condición 4 (N4): o los bytes pedidos (los que haya hasta el final), o un error; nunca una lectura corta. */
int hw_w6f_leer_id(u32 id, u32 desde, void *buf, u32 max)
{
    W6fInfo inf; int r; u32 quiero;
    if (situ != HW_W6F_MONTADO || id == 0 || id >= conf.max_obj || w6f_info(&fs, (uint16_t)id, &inf) || inf.es_dir) return -1;
    if (desde >= inf.tam) return 0;
    quiero = inf.tam - desde < max ? inf.tam - desde : max;
    r = w6f_leer(&fs, (uint16_t)id, desde, buf, quiero);
    return r == (int)quiero ? r : -2;
}
