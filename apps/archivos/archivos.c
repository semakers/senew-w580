/* Archivos: explorador de los ficheros. Tres sitios: el FS propio y, en /original, el de fábrica del teléfono (los dos
 * de SOLO LECTURA), y —pista S6— «Mis archivos»: la zona de ficheros de usuario (hw_usr_*), que es la carpeta
 * /usuario de la raíz y lo único que se puede escribir. Mis archivos es una carpeta PLANA (el formato W6U no tiene
 * directorios): ahí se puede abrir, renombrar, borrar y ver el espacio; y desde cualquier otro sitio, «Copiar a Mis
 * archivos» (por trozos, con barra de progreso y comprobando antes que cabe).
 * Una sola pantalla que cambia de directorio; la lista es perezosa (pide al FS solo la ventana que
 * se ve). Abre textos en un visor propio, las imágenes en el visor de Imágenes y los MIDI en Música.
 * (I1) Y LA MEMORIA (W6F, hw_w6f_*): la carpeta /memoria de la raíz (la última), escribible y CON CARPETAS: «Nueva carpeta»,
 * renombrar, borrar (un archivo o una carpeta vacía), «Cortar» y «Pegar aquí» (mover, atómico) y, desde CUALQUIER sitio,
 * «Copiar» + «Pegar aquí» en una carpeta de la Memoria (por trozos, con la barra). Mis archivos sigue igual (fase 1). */
#include "shell.h"
#include "tarjeta.h"                               /* (I1) refrescar al meter o sacar la tarjeta */

#define VENTANA 24
typedef struct { Widget base; } Archivos;
typedef struct {
    char ruta[160]; i32 sel, n; u16 pila_sel[12]; u8 prof;
    HwEntrada cache[VENTANA]; i32 cache_desde, cache_n;      /* la ventana del directorio que hay en memoria */
    char op_ruta[224], op_nombre[64], op_viejo[64]; u32 op_tam; u8 op_que;   /* (S6) la copia / el renombrado / el borrado pedido */
    u8 n_ops, ops_usr; const char *ops[16];                   /* (FATW) 16: en la tarjeta con algo en el portapapeles salen 13 */                   /* el menú de opciones que está abierto (depende de dónde se esté) */
    char pp_ruta[224], pp_nombre[64]; u32 pp_tam; u8 pp_modo, pp_dir;
    u32 tj_visto;                                             /* (I1) hw_tarjeta_cambios() de la última vez que se miró */   /* (I1) el portapapeles: 1 = copiar (de cualquier sitio), 2 = mover (dentro de la Memoria) */
} ArchivosE;
extern const Clase ClsArchivos;
Widget *app_recibir(void);                                   /* (SER) apps/recibir */
Widget *visor_texto(const char *ruta);
Widget *visor_hex(const char *ruta);
enum { OP_COPIAR = 1, OP_RENOMBRAR, OP_PEGAR, OP_MOVER };

static void recargar(ArchivosE *s) { s->n = hw_fs_contar(s->ruta); if (s->n < 0) s->n = 0; s->cache_n = 0; s->cache_desde = -1; if (s->sel >= s->n) s->sel = s->n ? s->n - 1 : 0; }
static const HwEntrada *entrada(ArchivosE *s, i32 i)
{
    if (i < 0 || i >= s->n) return NULL;
    if (s->cache_desde < 0 || i < s->cache_desde || i >= s->cache_desde + s->cache_n) {
        i32 d = i - VENTANA / 3; if (d < 0) d = 0;
        s->cache_n = hw_fs_listar(s->ruta, (u32)d, s->cache, VENTANA);
        s->cache_desde = d;
        if (s->cache_n < 0) s->cache_n = 0;
        if (i >= d + s->cache_n) return NULL;
    }
    return &s->cache[i - s->cache_desde];
}
static char *completa(const ArchivosE *s, const char *nombre) { return ui_fmt(s->ruta[1] ? "%s/%s" : "%s%s", s->ruta, nombre); }
static int termina(const char *s, const char *ext)
{
    size_t n = strlen(s), m = strlen(ext), i;
    if (n < m) return 0;
    for (i = 0; i < m; i++) { char a = s[n - m + i]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32); if (a != ext[i]) return 0; }
    return 1;
}
static int en_usr(const ArchivosE *s) { return !strcmp(s->ruta, HW_FS_USUARIO); }                       /* se está DENTRO de Mis archivos */
static int es_usr(const ArchivosE *s, const HwEntrada *e) { return !s->ruta[1] && e->es_dir && !strcmp(e->nombre, HW_FS_USUARIO + 1); }   /* la fila de la raíz */
static int usr_escribible(void) { HwUsrEstado u; hw_usr_estado(&u); return u.escribible; }
static int empieza(const char *a, const char *b, size_t n) { size_t i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }   /* (sin strncmp en el teléfono) */
/* (I1) la Memoria: se está DENTRO (en ella o en una carpeta suya); la fila de la raíz; la ruta DENTRO de la Memoria */
static int en_mem(const ArchivosE *s) { return empieza(s->ruta, HW_FS_MEMORIA, sizeof HW_FS_MEMORIA - 1) && (!s->ruta[sizeof HW_FS_MEMORIA - 1] || s->ruta[sizeof HW_FS_MEMORIA - 1] == '/'); }
static int es_mem(const ArchivosE *s, const HwEntrada *e) { return !s->ruta[1] && e->es_dir && !strcmp(e->nombre, HW_FS_MEMORIA + 1); }
static const char *mem_rel(const char *ruta) { return ruta + sizeof HW_FS_MEMORIA - 1; }
static int mem_escribible(void) { HwW6fEstado w; hw_w6f_estado(&w); return w.escribible; }
static int mem_hay(void) { HwW6fEstado w; hw_w6f_estado(&w); return w.situacion == HW_W6F_MONTADO; }
/* (FATW) LA TARJETA se escribe como la Memoria: dentro de /tarjeta (si es escribible) hay Nueva carpeta, Renombrar, Borrar,
 * Cortar, Copiar y Pegar aquí. Las operaciones «de zona» (z_*) van a la Memoria o a la tarjeta según la RUTA (destino); mover
 * entre la Memoria y la tarjeta es copiar y, ya comprometida la copia, borrar el origen (docs/fat_escritura.md §7). */
static int es_tj_ruta(const char *r) { return empieza(r, HW_FS_TARJETA, sizeof HW_FS_TARJETA - 1) && (!r[sizeof HW_FS_TARJETA - 1] || r[sizeof HW_FS_TARJETA - 1] == '/'); }
static int en_tj(const ArchivosE *s) { return es_tj_ruta(s->ruta); }
static int en_zona(const ArchivosE *s) { return en_mem(s) || en_tj(s); }
static const char *zrel(const char *r) { return es_tj_ruta(r) ? r + sizeof HW_FS_TARJETA - 1 : mem_rel(r); }
static int z_escribible(const ArchivosE *s) { return en_tj(s) ? hw_tj_escribible() : mem_escribible(); }
static int z_mkdir(const char *r) { return es_tj_ruta(r) ? hw_tj_mkdir(zrel(r)) : hw_w6f_mkdir(zrel(r)); }
static int z_borrar(const char *r) { return es_tj_ruta(r) ? hw_tj_borrar(zrel(r)) : hw_w6f_borrar(zrel(r)); }
static int z_renombrar(const char *v, const char *n) { return es_tj_ruta(n) ? hw_tj_renombrar_r(zrel(v), zrel(n), 0) : hw_w6f_renombrar(zrel(v), zrel(n)); }
static int z_info(const char *r, HwW6fInfo *w) { return es_tj_ruta(r) ? hw_tj_info(zrel(r), w) : hw_w6f_info(zrel(r), w); }
static int z_copiar(const char *dest, const char *orig, int reemplazar) { return es_tj_ruta(dest) ? hw_tj_copiar(zrel(dest), orig, reemplazar) : hw_w6f_copiar(zrel(dest), orig, reemplazar); }
static u32 z_libre(const char *r) { if (es_tj_ruta(r)) return hw_tj_libre(); { HwW6fEstado w; hw_w6f_estado(&w); return w.libre; } }
static const char *z_motivo(const char *r) { return es_tj_ruta(r) ? sh_tj_motivo() : sh_w6f_motivo(); }
static const char *z_donde(const char *r) { return es_tj_ruta(r) ? "la tarjeta" : "la Memoria"; }
static char *tj_espacio_txt(void)
{
    HwTarjeta t; hw_tarjeta_estado(&t);
    if (t.estado != HW_TJ_LISTA) return ui_fmt("%s.", t.texto);
    return ui_fmt("FAT%d. Libre: %s de %s.%s", t.fat, sh_tam(hw_tj_libre()), sh_tam(t.capacidad_kib > 4194303u ? 0xFFFFFFFFu : t.capacidad_kib * 1024u),
                  t.escribible ? "" : t.protegida ? " Protegida contra escritura: solo se puede leer." : " Solo lectura.");
}
static void copia_n(char *d, const char *o, size_t cap) { size_t n = strlen(o); if (n > cap - 1) n = cap - 1; memcpy(d, o, n); d[n] = 0; }
static void ir_a(Ctx *c, const char *ruta, int prof)
{
    ArchivosE *s = ui_estado(c);
    copia_n(s->ruta, ruta, sizeof s->ruta); s->prof = (u8)prof; s->pila_sel[0] = 0; s->sel = 0; recargar(s); ui_sucio(c);
}
static void elegir_nombre(ArchivosE *s, const char *nombre)     /* deja el cursor en ese fichero de la carpeta (tras copiar o renombrar) */
{
    HwEntrada e; i32 i;
    for (i = 0; i < s->n; i++) if (hw_fs_listar(s->ruta, (u32)i, &e, 1) == 1 && !strcmp(e.nombre, nombre)) { s->sel = i; return; }
}

static void subir(Ctx *c)
{
    ArchivosE *s = ui_estado(c);
    size_t n = strlen(s->ruta);
    if (n <= 1) { ui_sacar(); return; }
    while (n > 1 && s->ruta[n - 1] != '/') n--;
    s->ruta[n > 1 ? n - 1 : 1] = 0;
    s->sel = 0;
    recargar(s);
    if (s->prof) s->prof--;
    s->sel = s->prof < 12 ? s->pila_sel[s->prof] : 0;
    if (s->sel >= s->n) s->sel = s->n ? s->n - 1 : 0;
    ui_sucio(c);
}
static char *fecha_de(u32 seg)                                /* «13 oct 2026, 09:41» en hora local, de los segundos desde 2000 (UTC) */
{
    HwFecha f = { 2000, 1, 1, 0, 0, 0 };
    u32 dias = seg / 86400u, resto = seg % 86400u;
    for (;;) { u32 da = 365u + (u32)(hw_dias_mes(f.anio, 2) == 29); if (dias < da) break; dias -= da; f.anio++; }
    hw_fecha_sumar(&f, (int)(dias * 1440u + resto / 60u) + aj.zona_min);
    return ui_fmt("%d %s %d, %s", f.dia, sh_mes[f.mes - 1], f.anio, sh_hora(&f, 0));
}
static char *espacio_txt(void)
{
    HwUsrEstado u;
    hw_usr_estado(&u);
    return ui_fmt("%u de 64 archivos. Ocupado: %s. Cabe todavía un archivo de hasta %s.%s", u.ficheros, sh_tam(u.ocupado), sh_tam(u.libre),
                  u.escribible ? "" : " La memoria está protegida: solo se puede leer.");
}
static char *mem_espacio_txt(void)
{
    HwW6fEstado w;
    hw_w6f_estado(&w);
    if (w.situacion != HW_W6F_MONTADO) return ui_fmt("Situación: %s.", sh_w6f_situacion(&w));
    return ui_fmt("%u archivos y carpetas de %u. Libre: %s de %s (exacto: cabe un archivo de ese tamaño).%s%s", w.objetos, w.max_obj,
                  sh_tam(w.libre), sh_tam(w.capacidad), w.solo_lectura ? ui_fmt(" SOLO LECTURA: %s.", sh_w6f_sl(w.solo_lectura)) : "",
                  w.deficit ? " Falta un bloque de reserva: hasta reponerlo no se guarda (borrar sí)." : "");
}
static void propiedades(ArchivosE *s, const HwEntrada *e)
{
    if (es_usr(s, e)) { ui_aviso("Mis archivos", ui_fmt("Tus archivos: lo que guardas desde Notas, lo que copias aquí y las capturas. %s", espacio_txt())); return; }
    if (en_usr(s)) {
        HwUsrEntrada u; u32 i;
        memset(&u, 0, sizeof u);
        for (i = 0; hw_usr_listar(i, &u, 1) == 1 && strcmp(u.nombre, e->nombre); i++) ;
        ui_aviso(e->nombre, ui_fmt("%u bytes (%s). En Mis archivos.%s", e->tam, sh_tam(e->tam),
                 !strcmp(u.nombre, e->nombre) && u.fecha ? ui_fmt(" Guardado el %s.", fecha_de(u.fecha)) : " Sin fecha."));
        return;
    }
    if (es_mem(s, e)) { ui_aviso("Memoria", ui_fmt("La memoria nueva, con carpetas. %s", mem_espacio_txt())); return; }
    if (en_mem(s)) {
        HwW6fInfo w;
        if (hw_w6f_info(mem_rel(completa(s, e->nombre)), &w) != HW_OK) { ui_aviso(e->nombre, "Ya no existe."); return; }
        ui_aviso(e->nombre, w.es_dir ? ui_fmt("Carpeta de la Memoria.%s", w.fecha ? ui_fmt(" Creada el %s.", fecha_de(w.fecha)) : "")
                 : ui_fmt("%u bytes (%s). En la Memoria.%s%s", w.tam, sh_tam(w.tam), w.fecha ? ui_fmt(" Guardado el %s.", fecha_de(w.fecha)) : " Sin fecha.",
                          w.danado ? " DAÑADO: una parte no pasa su comprobación." : ""));
        return;
    }
    ui_aviso(e->nombre, e->es_dir ? ui_fmt("Carpeta de %s", s->ruta) : ui_fmt("%u bytes (%s). En %s. Solo lectura.", e->tam, sh_tam(e->tam), s->ruta));
}
static void abrir(Ctx *lista, int i, void *dato)
{
    Ctx *c = ui_ancestro(lista, &ClsArchivos);
    ArchivosE *s = ui_estado(c);
    const HwEntrada *e = entrada(s, i);
    (void)dato;
    if (!e) return;
    if (e->es_dir) {
        char *r = completa(s, e->nombre);
        if (strlen(r) >= sizeof s->ruta) { ui_aviso("Archivos", "La ruta es demasiado larga."); return; }
        if (s->prof < 12) s->pila_sel[s->prof] = (u16)s->sel;
        s->prof++;
        memcpy(s->ruta, r, strlen(r) + 1);
        s->sel = 0; recargar(s);
        ui_sucio(c);
    } else if (termina(e->nombre, ".gif") || termina(e->nombre, ".png") || termina(e->nombre, ".jpg") || termina(e->nombre, ".wbmp"))
        sh_abrir_sen("/apps/imagenes.sen", ui_fmt("%s/%s", s->ruta, e->nombre));   /* (APX) Imágenes es un .sen: va directa al visor */
    else if (termina(e->nombre, ".mid") && sh_musica_tocar_ruta(completa(s, e->nombre)) == HW_OK)
        sh_abrir_sen(SH_APP_MUSICA, NULL);                  /* (S6; APX: la interfaz es un .sen) un MIDI que la Música conoce: suena y se abre su pantalla */
    else if (termina(e->nombre, ".sen")) { Widget *w = cg_abrir_ruta(completa(s, e->nombre)); if (w) ui_empujar(w); }   /* (C) una app cargable */
    else if (termina(e->nombre, ".txt") || termina(e->nombre, ".xml") || termina(e->nombre, ".html") || termina(e->nombre, ".jad") ||
             termina(e->nombre, ".ml") || termina(e->nombre, ".ini") || termina(e->nombre, ".thm") || termina(e->nombre, ".dtd") || !e->tam)
        ui_empujar(visor_texto(completa(s, e->nombre)));
    else ui_empujar(visor_hex(completa(s, e->nombre)));     /* lo demás, en hexadecimal */
}

/* ---- (S6) escribir en Mis archivos: copiar, renombrar, borrar -------------------------------------------------- */
static void op_fin(int r, void *dato)                       /* acabó (o se canceló, o falló) la copia o el renombrado */
{
    Ctx *c = dato; ArchivosE *s = ui_estado(c);
    int copia = s->op_que == OP_COPIAR;
    s->op_que = 0;
    recargar(s);
    if (termina(s->op_nombre, ".mid") || termina(s->op_viejo, ".mid")) sh_musica_refrescar();
    if (r == HW_OK) {
        if (en_usr(s)) elegir_nombre(s, s->op_nombre);
        ui_aviso(copia ? "Copiado" : "Renombrado", copia ? ui_fmt("«%s» (%s) ya está en Mis archivos.", s->op_nombre, sh_tam(s->op_tam))
                                                         : ui_fmt("«%s» ahora se llama «%s».", s->op_viejo, s->op_nombre));
    } else if (r == SH_USR_CANCELADA)
        ui_aviso(copia ? "Copia cancelada" : "Cancelado", copia ? "No se ha copiado nada: Mis archivos queda como estaba." : "El archivo conserva su nombre.");
    else ui_aviso(copia ? "No se pudo copiar" : "No se pudo renombrar",
                  !copia && hw_usr_motivo() != HW_USR_M_FUENTE && hw_usr_tam(s->op_nombre) >= 0 && hw_usr_tam(s->op_viejo) >= 0
                      ? "La copia con el nombre nuevo está hecha, pero no se pudo borrar el archivo viejo: ahora están los dos, enteros. Borra el que sobre."
                      : sh_usr_motivo());
    ui_sucio(c);
}
static void copiar_ya(Ctx *c, int reemplazar)
{
    ArchivosE *s = ui_estado(c);
    if (hw_usr_copiar(s->op_nombre, s->op_ruta, reemplazar) != HW_OK) {
        ui_aviso("No se puede copiar", hw_usr_motivo() == HW_USR_M_LLENO && reemplazar
                 ? "No cabe: para reemplazar un archivo hace falta sitio para el viejo y el nuevo a la vez (así, si se va la corriente, no se pierde ninguno). Borra antes el viejo."
                 : sh_usr_motivo());
        return;
    }
    s->op_que = OP_COPIAR;
    sh_usr_progreso("Copiando", ui_fmt("«%s» a Mis archivos", s->op_nombre), op_fin, c);
}
static void copiar_reemplazando(void *dato) { copiar_ya(dato, 1); }
static void copiar_con_nombre(const char *nombre, void *dato)   /* ya hay nombre válido: ¿cabe?, ¿existe? */
{
    Ctx *c = dato; ArchivosE *s = ui_estado(c);
    HwUsrEstado u;
    int viejo;
    copia_n(s->op_nombre, nombre, sizeof s->op_nombre);
    hw_usr_estado(&u);
    viejo = hw_usr_tam(s->op_nombre);
    if (s->op_tam > u.libre) {
        ui_aviso("No cabe", s->op_tam > u.libre + u.ocupado
                 ? ui_fmt("«%s» mide %s y Mis archivos entero, aun vacío, no llega a tanto (caben unos %s).", s->op_nombre, sh_tam(s->op_tam), sh_tam(u.libre + u.ocupado))
                 : ui_fmt("«%s» mide %s y en Mis archivos solo queda sitio para %s. Borra algo y vuelve a probar.", s->op_nombre, sh_tam(s->op_tam), sh_tam(u.libre)));
        return;
    }
    if (hw_fs_mirar(s->op_ruta, &s->op_tam) != HW_OK) { ui_aviso("No se puede copiar", "El archivo de origen ya no está (o no se puede leer): no se toca nada."); return; }   /* (AUD8 A1) */
    if (viejo >= 0) ui_confirmar_no("Ya existe", ui_fmt("En Mis archivos ya hay un «%s» (%s). ¿Reemplazarlo por este (%s)?", s->op_nombre, sh_tam((u32)viejo), sh_tam(s->op_tam)), copiar_reemplazando, c);
    else copiar_ya(c, 0);
}
static void copiar(Ctx *c, const HwEntrada *e)
{
    ArchivosE *s = ui_estado(c);
    HwUsrEstado u;
    char *r = completa(s, e->nombre);
    size_t n = strlen(e->nombre);
    hw_usr_estado(&u);
    if (!u.montado || !u.escribible) { ui_aviso("Mis archivos", !u.montado ? "Este teléfono no tiene zona de archivos de usuario." : "La memoria está protegida contra escritura: no se puede copiar."); return; }
    if (strlen(r) >= sizeof s->op_ruta) { ui_aviso("Archivos", "La ruta es demasiado larga."); return; }
    memcpy(s->op_ruta, r, strlen(r) + 1); s->op_tam = e->tam;
    if (n > HW_USR_MAX_NOMBRE) {                            /* el nombre no cabe en Mis archivos: la pantalla del nombre lo dice y deja acortarlo */
        sh_pedir_nombre("Copiar como…", e->nombre, copiar_con_nombre, c);
        return;
    }
    copiar_con_nombre(e->nombre, c);
}
static void renombrar_con(const char *nombre, void *dato)
{
    Ctx *c = dato; ArchivosE *s = ui_estado(c);
    HwUsrEstado u;
    if (!strcmp(nombre, s->op_viejo)) return;               /* el mismo: nada que hacer */
    copia_n(s->op_nombre, nombre, sizeof s->op_nombre);
    if (hw_usr_renombrar(s->op_viejo, s->op_nombre) != HW_OK) {
        hw_usr_estado(&u);
        ui_aviso("No se puede renombrar",
                 hw_usr_motivo() == HW_USR_M_EXISTE ? ui_fmt("Ya hay un archivo que se llama «%s». Elige otro nombre.", s->op_nombre)
                 : hw_usr_motivo() == HW_USR_M_LLENO ? ui_fmt("Renombrar es copiar con el nombre nuevo y borrar el viejo, y mientras dura hace falta sitio para la copia: el archivo mide %s y quedan %s. Borra algo antes.", sh_tam(s->op_tam), sh_tam(u.libre))
                 : sh_usr_motivo());
        return;
    }
    s->op_que = OP_RENOMBRAR;
    sh_usr_progreso("Renombrando", ui_fmt("«%s»", s->op_nombre), op_fin, c);
}
static void borrar_si(void *dato)
{
    Ctx *c = dato; ArchivosE *s = ui_estado(c);
    if (hw_usr_borrar(s->op_viejo) != HW_OK) ui_aviso("No se pudo borrar", sh_usr_motivo());
    if (termina(s->op_viejo, ".mid")) sh_musica_refrescar();
    recargar(s);
    ui_sucio(c);
}
/* ---- (I1) la Memoria: carpeta nueva, renombrar, borrar, cortar/copiar y pegar ---------------------------------------- */
static void mem_fin(int r, void *dato)                      /* acabó la copia (pegar) a la Memoria o a la tarjeta */
{
    Ctx *c = dato; ArchivosE *s = ui_estado(c);
    int mover = s->op_que == OP_MOVER;
    const char *donde = z_donde(s->ruta);
    s->op_que = 0;
    if (r == HW_OK && mover) {                              /* (FATW) mover entre zonas: la copia YA está comprometida; ahora el origen */
        s->pp_modo = 0;
        if (z_borrar(s->pp_ruta) != HW_OK) { recargar(s); elegir_nombre(s, s->op_nombre); ui_aviso("Movido a medias", ui_fmt("«%s» ya está en %s, pero no se pudo borrar el original: ahora están los dos, enteros. %s", s->op_nombre, donde, z_motivo(s->pp_ruta))); ui_sucio(c); return; }
    }
    recargar(s);
    if (r == HW_OK) { elegir_nombre(s, s->op_nombre); ui_aviso(mover ? "Movido" : "Copiado", ui_fmt("«%s» (%s) ya está en %s.", s->op_nombre, sh_tam(s->op_tam), donde)); }
    else if (r == SH_USR_CANCELADA) ui_aviso("Copia cancelada", ui_fmt("No se ha copiado nada: %s queda como estaba%s.", donde, mover ? " y el original sigue en su sitio" : ""));
    else ui_aviso("No se pudo copiar", z_motivo(s->ruta));
    ui_sucio(c);
}
static void mem_tras(Ctx *c, int r, const char *titulo, const char *elegir)   /* tras una operación corta (atómica) */
{
    ArchivosE *s = ui_estado(c);
    if (r != HW_OK) { ui_aviso(titulo, z_motivo(s->ruta)); return; }   /* no ha cambiado nada: ni se relee */
    recargar(s);
    if (elegir) elegir_nombre(s, elegir);
    ui_sucio(c);
}
static void mem_carpeta(const char *nombre, void *dato)
{
    Ctx *c = dato; ArchivosE *s = ui_estado(c);
    copia_n(s->op_nombre, nombre, sizeof s->op_nombre);
    mem_tras(c, z_mkdir(completa(s, nombre)), "No se pudo crear la carpeta", s->op_nombre);
}
static void mem_renombrar(const char *nombre, void *dato)
{
    Ctx *c = dato; ArchivosE *s = ui_estado(c);
    if (!strcmp(nombre, s->op_viejo)) return;
    copia_n(s->op_nombre, nombre, sizeof s->op_nombre);
    { char *v = completa(s, s->op_viejo); char viejo[224]; copia_n(viejo, v, sizeof viejo);
      mem_tras(c, z_renombrar(viejo, completa(s, nombre)), "No se pudo renombrar", s->op_nombre); }
}
static void mem_borrar_si(void *dato)
{
    Ctx *c = dato; ArchivosE *s = ui_estado(c);
    mem_tras(c, z_borrar(completa(s, s->op_viejo)), "No se pudo borrar", NULL);
}
static void mem_pegar_ya(Ctx *c, int reemplazar)
{
    ArchivosE *s = ui_estado(c);
    char dest[224];
    copia_n(dest, completa(s, s->pp_nombre), sizeof dest);
    copia_n(s->op_nombre, s->pp_nombre, sizeof s->op_nombre); s->op_tam = s->pp_tam;
    if (z_copiar(dest, s->pp_ruta, reemplazar) != HW_OK) { ui_aviso("No se puede copiar", z_motivo(dest)); return; }
    s->op_que = s->pp_modo == 2 ? OP_MOVER : OP_PEGAR;
    (en_tj(s) ? sh_tj_progreso : sh_w6f_progreso)(s->pp_modo == 2 ? "Moviendo" : "Copiando", ui_fmt("«%s» a %s", s->pp_nombre, z_donde(s->ruta)), mem_fin, c);
}
static void mem_pegar_reemplazando(void *dato) { mem_pegar_ya(dato, 1); }
static void mem_pegar(Ctx *c)
{
    ArchivosE *s = ui_estado(c);
    char dest[224]; HwW6fInfo w; u32 libre;
    size_t n;
    if (!s->pp_modo) { ui_aviso("Pegar", "No hay nada copiado ni cortado: elige antes un archivo y «Copiar» (o «Cortar», dentro de la Memoria o de la tarjeta)."); return; }
    copia_n(dest, completa(s, s->pp_nombre), sizeof dest);
    if (s->pp_modo == 2 && es_tj_ruta(s->pp_ruta) == en_tj(s)) {   /* mover dentro de la MISMA zona: renombrar, sin copiar nada */
        n = strlen(s->pp_ruta);
        if (s->pp_dir && strlen(s->ruta) >= n && empieza(s->ruta, s->pp_ruta, n) && (!s->ruta[n] || s->ruta[n] == '/')) { ui_aviso("Mover", "Una carpeta no se puede mover dentro de sí misma."); return; }
        s->pp_modo = 0;
        mem_tras(c, z_renombrar(s->pp_ruta, dest), "No se pudo mover", s->pp_nombre);
        return;
    }
    if (s->pp_modo == 2 && s->pp_dir) { ui_aviso("Mover", "Las carpetas se mueven dentro de la Memoria o dentro de la tarjeta; entre una y otra, de momento, solo archivos."); return; }
    if (hw_fs_mirar(s->pp_ruta, &s->pp_tam) != HW_OK) {    /* (AUD8 A1) el origen se mira OTRA VEZ: pudo borrarse, moverse o irse con la tarjeta */
        ui_aviso("No se puede pegar", ui_fmt("«%s» ya no está donde se copió (o no se puede leer): no se toca nada. Vuelve a elegirlo y «Copiar».", s->pp_nombre));
        s->pp_modo = 0; return;
    }
    libre = z_libre(s->ruta);
    if (s->pp_tam > libre) { ui_aviso("No cabe", ui_fmt("«%s» mide %s y en %s queda sitio para %s.", s->pp_nombre, sh_tam(s->pp_tam), z_donde(s->ruta), sh_tam(libre))); return; }
    if (z_info(dest, &w) == HW_OK) {
        if (w.es_dir) { ui_aviso("Pegar", ui_fmt("Aquí hay una carpeta que se llama «%s».", s->pp_nombre)); return; }
        ui_confirmar_no("Ya existe", ui_fmt("Aquí ya hay un «%s» (%s). ¿Reemplazarlo por este (%s)? Hasta que acabe la copia vale el viejo.", s->pp_nombre, sh_tam(w.tam), sh_tam(s->pp_tam)), mem_pegar_reemplazando, c);
        return;
    }
    mem_pegar_ya(c, 0);
}
static void portapapeles(ArchivosE *s, const HwEntrada *e, int modo)
{
    char *r = completa(s, e->nombre);
    if (strlen(r) >= sizeof s->pp_ruta || strlen(e->nombre) > HW_W6F_MAX_NOMBRE) { ui_aviso("Archivos", "La ruta o el nombre son demasiado largos para la Memoria."); return; }
    copia_n(s->pp_ruta, r, sizeof s->pp_ruta); copia_n(s->pp_nombre, e->nombre, sizeof s->pp_nombre);
    s->pp_tam = e->tam; s->pp_modo = (u8)modo; s->pp_dir = e->es_dir;
    ui_aviso(modo == 2 ? "Cortado" : "Copiado", ui_fmt("«%s»: ahora ve a una carpeta de la Memoria o de la tarjeta y elige «Pegar aquí».", e->nombre));
}
static int opcion_mem(Ctx *c, ArchivosE *s, const char *op, const HwEntrada *e)   /* 1 = era de la Memoria */
{
    if (!strcmp(op, "Ir a la Memoria")) ir_a(c, HW_FS_MEMORIA, 1);
    else if (!strcmp(op, "Espacio en la Memoria")) ui_aviso("Memoria", mem_espacio_txt());
    else if (!strcmp(op, "Espacio en la tarjeta")) ui_aviso("Tarjeta", tj_espacio_txt());
    else if (!strcmp(op, "Nueva carpeta")) sh_pedir_nombre("Nueva carpeta", "", mem_carpeta, c);
    else if (!strcmp(op, "Pegar aquí")) mem_pegar(c);
    else if (!strcmp(op, "Copiar")) { if (!e || e->es_dir) ui_aviso("Copiar", "Se copian archivos: elige uno."); else portapapeles(s, e, 1); }
    else if (!strcmp(op, "Cortar")) { if (!e) ui_aviso("Cortar", "Elige un archivo o una carpeta."); else portapapeles(s, e, 2); }
    else if (!strcmp(op, "Renombrar ") && e) { copia_n(s->op_viejo, e->nombre, sizeof s->op_viejo); sh_pedir_nombre("Renombrar", e->nombre, mem_renombrar, c); }
    else if (!strcmp(op, "Borrar ") && e) {
        copia_n(s->op_viejo, e->nombre, sizeof s->op_viejo);
        ui_confirmar_no("Borrar", e->es_dir ? ui_fmt("¿Borrar la carpeta «%s» de %s? Tiene que estar vacía.", e->nombre, z_donde(s->ruta))
                                         : ui_fmt("¿Borrar «%s» (%s) de %s? No se puede deshacer.", e->nombre, sh_tam(e->tam), z_donde(s->ruta)), mem_borrar_si, c);
    } else return 0;
    return 1;
}
static void opcion(int i, void *dato)
{
    Ctx *c = dato; ArchivosE *s = ui_estado(c);
    const HwEntrada *e = entrada(s, s->sel);
    const char *op = i >= 0 && i < s->n_ops ? s->ops[i] : "";
    if (i == 0 && e) propiedades(s, e);
    else if (i == 1 && e && !e->es_dir) ui_empujar(visor_texto(completa(s, e->nombre)));
    else if (i == 2 && e && !e->es_dir) ui_empujar(visor_hex(completa(s, e->nombre)));
    else if (i == 3) ir_a(c, "/", 0);
    else if (!strcmp(op, "Recibir por serie")) ui_empujar(app_recibir());   /* (SER) */
    else if (opcion_mem(c, s, op, e)) { }
    else if (!strcmp(op, "Ir a Mis archivos")) ir_a(c, HW_FS_USUARIO, 1);
    else if (!strcmp(op, "Espacio libre")) ui_aviso("Mis archivos", espacio_txt());
    else if (!e || e->es_dir) { if (i > 3) ui_aviso("Archivos", "Eso se hace sobre un archivo: elige uno."); }
    else if (!strcmp(op, "Copiar a Mis archivos")) copiar(c, e);
    else if (!strcmp(op, "Renombrar")) {
        copia_n(s->op_viejo, e->nombre, sizeof s->op_viejo); s->op_tam = e->tam;
        sh_pedir_nombre("Renombrar", e->nombre, renombrar_con, c);
    } else if (!strcmp(op, "Borrar")) {
        copia_n(s->op_viejo, e->nombre, sizeof s->op_viejo);
        ui_confirmar_no("Borrar", ui_fmt("¿Borrar «%s» (%s) de Mis archivos? No se puede deshacer.", e->nombre, sh_tam(e->tam)), borrar_si, c);
    } else if (!strcmp(op, "Editar el texto")) sh_abrir_sen("/apps/notas.sen", e->nombre);   /* (APX) Notas es un .sen: abre ese fichero */
}
static int tecla(Ctx *t, int k, void *dato)
{
    /* Las cuatro primeras son las de siempre y van SIEMPRE en su sitio; detrás, lo que se puede hacer donde se está. */
    static const char *const fijas[4] = { "Propiedades", "Ver como texto", "Ver en hexadecimal", "Ir a la raíz" };
    Ctx *c = ui_ancestro(t, &ClsArchivos);
    (void)dato;
    if (k == K_SEL_IZQ) {
        ArchivosE *s = ui_estado(c);
        HwUsrEstado u;
        int n = 0, i;
        hw_usr_estado(&u);
        for (i = 0; i < 4; i++) s->ops[n++] = fijas[i];
        if (en_usr(s)) { s->ops[n++] = "Renombrar"; s->ops[n++] = "Borrar"; s->ops[n++] = "Editar el texto"; s->ops[n++] = "Espacio libre"; }
        else if (en_zona(s)) {                          /* (I1) la Memoria y (FATW) la tarjeta (los nombres con espacio: no son los de Mis archivos) */
            if (z_escribible(s)) { s->ops[n++] = "Nueva carpeta"; s->ops[n++] = "Renombrar "; s->ops[n++] = "Borrar "; s->ops[n++] = "Cortar"; }
            s->ops[n++] = "Copiar";
            if (s->pp_modo && z_escribible(s)) s->ops[n++] = "Pegar aquí";
            s->ops[n++] = en_tj(s) ? "Espacio en la tarjeta" : "Espacio en la Memoria";
        }
        else if (u.montado) { s->ops[n++] = "Copiar a Mis archivos"; s->ops[n++] = "Ir a Mis archivos"; }
        if (!en_zona(s) && mem_hay()) { s->ops[n++] = "Copiar"; s->ops[n++] = "Ir a la Memoria"; }
        else if (en_tj(s) && mem_hay()) s->ops[n++] = "Ir a la Memoria";   /* (I1) detrás de lo de antes: no se mueve ninguna opción */
        if (mem_escribible() || hw_tj_escribible()) s->ops[n++] = "Recibir por serie";   /* (SER) la última de todas: apps/recibir (FATW: o a la tarjeta) */
        s->n_ops = (u8)n;
        ui_opciones(en_usr(s) ? "Mis archivos" : en_mem(s) ? "Memoria" : en_tj(s) ? "Tarjeta" : "Archivos", s->ops, n, opcion, c);
        return 1;
    }
    if (k == K_SEL_DER || k == K_ATRAS || k == K_C || k == K_IZQ) { subir(c); return 1; }
    if (k == K_DER) { ui_tecla(K_OK); return 1; }
    return 0;
}
static Widget *fila(Ctx *l, int i, int sel, void *dato)
{
    ArchivosE *s = ui_estado(ui_ancestro(l, &ClsArchivos));
    const HwEntrada *e = entrada(s, i);
    const TemaDatos *t = sh_tema();
    int dir, mis; u32 tam;
    (void)dato;
    if (!e) return sh_fila("?", NULL, sel);
    dir = e->es_dir; tam = e->tam; mis = es_usr(s, e) ? 1 : es_mem(s, e) ? 2 : 0;
    {   /* (MD) una fila del diseño: icono, nombre y, a la derecha, el tamaño o si se escribe */
        DisFila f = { .titulo = mis == 1 ? "Mis archivos" : mis == 2 ? "Memoria" : ui_fmt("%s", e->nombre),   /* copia: la ventana puede recargarse antes de montar */
                      .icono = &sh_iconos[dir ? IC_CARPETA : IC_FICHERO], .accesorio = DIS_ACC_VALOR,
                      .valor = mis ? ((mis == 1 ? usr_escribible() : mem_escribible()) ? "se escribe" : "protegido") : dir ? NULL : sh_tam(tam) };
        (void)t;
        return dis_actual()->fila(&f, sel);
    }
}
static void movido(Ctx *l, int i, void *d) { (void)i; (void)d; ui_sucio(ui_ancestro(l, &ClsArchivos)); }   /* el pie «n de N» */
/* (I1) LA TARJETA ENTRA O SALE con la lista a la vista: se relee (la raíz gana o pierde su fila; dentro de /tarjeta, si ya no está,
 * se vuelve a la raíz y se dice). Antes la lista no se enteraba hasta navegar (pista T, §3.2 de tarjeta/INTEGRACION.md). */
static int en_tarjeta(const ArchivosE *s) { return empieza(s->ruta, HW_FS_TARJETA, sizeof HW_FS_TARJETA - 1) && (!s->ruta[sizeof HW_FS_TARJETA - 1] || s->ruta[sizeof HW_FS_TARJETA - 1] == '/' || s->ruta[sizeof HW_FS_TARJETA - 1] == ' '); }
static void mirar_tarjeta(Ctx *c)
{
    ArchivosE *s = ui_estado(c);
    HwTarjeta t;
    if (hw_tarjeta_cambios() == s->tj_visto) return;
    s->tj_visto = hw_tarjeta_cambios();
    hw_tarjeta_estado(&t);
    if (en_tarjeta(s) && t.estado != HW_TJ_LISTA) { ir_a(c, "/", 0); if (!ui_cima_emergente()) ui_aviso("Tarjeta", "La tarjeta ya no está (o ha dejado de leerse): vuelta a la raíz."); return; }
    if (s->op_que) return;                                   /* una copia en marcha: la lista se relee al acabar */
    recargar(s); ui_sucio(c);
}
static void iniciar(Ctx *c) { ArchivosE *s = ui_estado(c); s->ruta[0] = '/'; s->ruta[1] = 0; recargar(s); s->tj_visto = hw_tarjeta_cambios(); ui_cada(c, 500, mirar_tarjeta); }
static Widget *construir(Ctx *c)
{
    ArchivosE *s = ui_estado(c);
    size_t n = strlen(s->ruta);
    int usr = en_usr(s), mem = en_mem(s);
    const char *titulo = usr ? "Mis archivos" : mem && n == sizeof HW_FS_MEMORIA - 1 ? "Memoria" : n > 22 ? ui_fmt("…%s", s->ruta + n - 21) : s->ruta;
    const char *pie;
    Widget *cuerpo;
    if (usr) recargar(s);                                    /* Mis archivos CAMBIA (Notas, una captura, esta misma pantalla): se relee en cada construcción, que es barato */
    if (!s->n && !hw_fs_listo() && !usr && !mem) cuerpo = Centro(Relleno(8, Parrafo("No hay sistema de ficheros: esta flash solo trae el sistema. Carga un perfil con el FS de fábrica.", .centrado = 1)));
    else if (!s->n && usr) cuerpo = Centro(Relleno(10, Parrafo("Aún no hay nada. Aquí se guarda lo tuyo: desde Notas («Guardar como archivo»), con «Copiar a Mis archivos» sobre cualquier fichero, o una captura de pantalla (Actividad + #).", .centrado = 1, .color = sh_tema()->tenue)));
    else if (!s->n && mem) cuerpo = Centro(Relleno(10, Parrafo("Carpeta vacía. Aquí puedes crear carpetas («Nueva carpeta») y pegar archivos: «Copiar» sobre cualquier archivo y «Pegar aquí».", .centrado = 1, .color = sh_tema()->tenue)));
    else if (!s->n) cuerpo = Centro(Texto("(carpeta vacía)", .color = sh_tema()->tenue));
    else cuerpo = ListaPerezosa(.n = s->n, .fila = fila, .al_activar = abrir, .al_mover = movido, .sel = &s->sel, .alto_fila = dis_tokens()->m.alto_fila);
    if (usr) { HwUsrEstado u; hw_usr_estado(&u); pie = ui_fmt("%d de %d · libre %s%s", s->n ? s->sel + 1 : 0, s->n, sh_tam(u.libre), u.escribible ? "" : " · protegido"); }
    else if (mem) { HwW6fEstado w; hw_w6f_estado(&w); pie = ui_fmt("%d de %d · libre %s%s", s->n ? s->sel + 1 : 0, s->n, sh_tam(w.libre), w.escribible ? "" : " · protegido"); }
    else if (en_tj(s)) pie = hw_tj_escribible() ? ui_fmt("%d de %d · tarjeta, libre %s", s->n ? s->sel + 1 : 0, s->n, sh_tam(hw_tj_libre())) : ui_fmt("%d de %d · tarjeta, solo lectura", s->n ? s->sel + 1 : 0, s->n);
    else pie = ui_fmt("%d de %d · solo lectura", s->n ? s->sel + 1 : 0, s->n);
    return Teclas(tecla, Pantalla(titulo, Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        Expandido(cuerpo),
        Caja(.an = INF, .color = sh_tema()->barra, .hijo = RellenoXY(4, 2, Texto(pie, .color = sh_tema()->tenue, .marcas = F_ESTILO(DIS_T_PEQUENA)))))),
        "Opciones", n > 1 ? "Subir" : "Atrás"));
}
COMPONENTE_CON_ESTADO(Archivos, ArchivosE, construir, iniciar);
Widget *app_archivos(void) { return UI_W(Archivos); }

/* ---- visor de texto: los primeros 12 KB del fichero, partidos en líneas al ancho de la pantalla ------ */
#define VT_MAX (12 * 1024)
#define VT_LINEAS 1400
typedef struct { Widget base; const char *ruta; } VisorTexto;
typedef struct { char *buf; u16 *linea; char nombre[40]; u32 tam; i32 n, nl, primera, caben; u8 cortado; } VisorTextoE;
extern const Clase ClsVisorTexto;
#define VT_ANCHO (UI_ANCHO - 12)

static void vt_iniciar(Ctx *c)
{
    const VisorTexto *w = ui_widget(c); VisorTextoE *s = ui_estado(c);
    const char *b = w->ruta + strlen(w->ruta);
    u32 i, j = 0; i32 p = 0;
    while (b > w->ruta && b[-1] != '/') b--;
    for (i = 0; b[i] && i < sizeof s->nombre - 1; i++) s->nombre[i] = b[i];
    s->tam = hw_fs_tam(w->ruta);
    s->buf = mo_pedir(VT_MAX + 1); s->linea = mo_pedir(VT_LINEAS * sizeof(u16));
    s->n = (i32)hw_fs_leer(w->ruta, (u8 *)s->buf, VT_MAX);
    s->cortado = s->tam > VT_MAX;
    for (i = 0; i < (u32)s->n; i++) {                        /* limpia: fuera \r, tabulador = espacio, lo no imprimible = '.' */
        u8 ch = (u8)s->buf[i];
        if (ch == '\r') continue;
        s->buf[j++] = (char)(ch == '\t' ? ' ' : (ch < 32 && ch != '\n') || ch == 127 ? '.' : ch);
    }
    s->buf[j] = 0; s->n = (i32)j;
    while (p < s->n && s->nl < VT_LINEAS) { s->linea[s->nl++] = (u16)p; p += li_partir(s->buf + p, VT_ANCHO, 1, 0); }
    s->caben = 19;
}
static void vt_desechar(Ctx *c) { VisorTextoE *s = ui_estado(c); mo_soltar(s->buf); mo_soltar(s->linea); }
static void vt_pintar(Lienzo *l, Rect r, void *dato)
{
    VisorTextoE *s = dato;
    i32 i;
    const int pl = dis_paso_linea(0);                        /* (estética) el paso de la letra en uso (antes LI_ALTO_LINEA = 10) */
    s->caben = r.h / pl;
    for (i = 0; i < s->caben && s->primera + i < s->nl; i++) {
        i32 a = s->linea[s->primera + i], b = s->primera + i + 1 < s->nl ? s->linea[s->primera + i + 1] : s->n;
        while (b > a && (s->buf[b - 1] == '\n' || s->buf[b - 1] == ' ')) b--;
        li_texto_n(l, r.x + 4, r.y + 1 + (int)i * pl, s->buf + a, (int)(b - a), sh_tema()->texto, 1, 0);
    }
}
static int vt_tecla(Ctx *t, int k, void *dato)
{
    Ctx *c = ui_ancestro(t, &ClsVisorTexto); VisorTextoE *s = ui_estado(c);
    i32 p = s->primera, max = s->nl - s->caben;
    (void)dato;
    if (max < 0) max = 0;
    if (k == K_ABAJO) p++;
    else if (k == K_ARRIBA) p--;
    else if (k == K_DER || k == K_VOL_MENOS || k == K_OK || k == K_SEL_IZQ) p += s->caben - 1;
    else if (k == K_IZQ || k == K_VOL_MAS) p -= s->caben - 1;
    else return sh_teclas(t, k, dato);
    if (p > max) p = max;
    if (p < 0) p = 0;
    if (p != s->primera) { s->primera = p; ui_sucio(c); }
    return 1;
}
static Widget *vt_construir(Ctx *c)
{
    VisorTextoE *s = ui_estado(c);
    return Teclas(vt_tecla, Pantalla(s->nombre, Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        Expandido(Fila(.cruzado = P_ESTIRAR, .hijos = HIJOS(
            Expandido(s->n ? Pintura(vt_pintar, .dato = s, .version = (u32)s->primera + 1) : Centro(Texto(s->tam ? "(no se pudo leer: dañado)" : "(fichero vacío)", .color = sh_tema()->tenue))),   /* (I1) tam > 0 y nada leído = error, no vacío */
            BarraDesp(.total = s->nl, .visibles = s->caben, .pos = s->primera)))),
        Caja(.an = INF, .color = sh_tema()->barra, .hijo = RellenoXY(4, 2, Texto(
            ui_fmt("línea %d de %d · %s%s", s->nl ? s->primera + 1 : 0, s->nl, sh_tam(s->tam), s->cortado ? " (12 KB)" : ""), .color = sh_tema()->tenue, .marcas = F_ESTILO(DIS_T_PEQUENA)))))),
        "Pág. sig.", "Atrás"));
}
COMPONENTE_CON_ESTADO(VisorTexto, VisorTextoE, vt_construir, vt_iniciar, .desechar = vt_desechar);
/* la ruta se copia al montículo del visor: quien llama suele pasarla de la arena */
Widget *visor_texto(const char *ruta)
{
    static char copia[200];                                  /* un visor a la vez: la ruta vive aquí */
    size_t n = strlen(ruta); if (n > sizeof copia - 1) n = sizeof copia - 1;
    memcpy(copia, ruta, n); copia[n] = 0;
    return UI_W(VisorTexto, .ruta = copia);
}

/* ---- visor hexadecimal: hasta 64 KB del fichero, VH_BPF bytes por fila (4: hasta 16 384 filas) en una lista
 * PEREZOSA (solo existen las ~17 que se ven). Volumen +/- pasan página. ------------------------------ */
#define VH_MAX (64 * 1024)
typedef struct { Widget base; const char *ruta; } VisorHex;
typedef struct { u8 *buf; char nombre[40]; u32 n, tam; i32 sel; } VisorHexE;
extern const Clase ClsVisorHex;
static void vh_iniciar(Ctx *c)
{
    const VisorHex *w = ui_widget(c); VisorHexE *s = ui_estado(c);
    const char *b = w->ruta + strlen(w->ruta); u32 i;
    while (b > w->ruta && b[-1] != '/') b--;
    for (i = 0; b[i] && i < sizeof s->nombre - 1; i++) s->nombre[i] = b[i];
    s->tam = hw_fs_tam(w->ruta);
    s->n = s->tam < VH_MAX ? s->tam : VH_MAX;
    if (s->n && (s->buf = mo_probar(s->n)) != NULL) s->n = hw_fs_leer(w->ruta, s->buf, s->n); else s->n = 0;
}
/* (estética) bytes por fila: 4 con la letra nueva (el 5x7 monoespaciado va al doble: 12 px por carácter; con 6 la fila medía 304 px) */
#define VH_BPF 4
static void vh_desechar(Ctx *c) { mo_soltar(((VisorHexE *)ui_estado(c))->buf); }
static Widget *vh_fila(Ctx *l, int i, int sel, void *dato)
{
    VisorHexE *s = ui_estado(ui_ancestro(l, &ClsVisorHex));
    const TemaDatos *t = sh_tema();
    char h[32], a[8]; u32 p = (u32)i * VH_BPF, k; int n = 0;
    static const char hx[] = "0123456789ABCDEF";
    (void)dato;
    for (k = 0; k < VH_BPF; k++) {
        if (k == VH_BPF / 2) h[n++] = ' ';
        if (p + k < s->n) { u8 b = s->buf[p + k]; h[n++] = hx[b >> 4]; h[n++] = hx[b & 15]; a[k] = (char)(b >= 32 && b < 127 ? b : '.'); }
        else { h[n++] = ' '; h[n++] = ' '; a[k] = ' '; }
    }
    h[n] = 0; a[VH_BPF] = 0;
    return Caja(.an = INF, .al = INF, .color = sel ? t->barra : TRANSPARENTE, .hijo = RellenoXY(3, 0, Fila(.espacio = 7, .hijos = HIJOS(
        Texto(ui_fmt("%04x", p), .marcas = F_MONO, .color = t->acento), Texto(ui_fmt("%s", h), .marcas = F_MONO), Texto(ui_fmt("%s", a), .marcas = F_MONO, .color = t->tenue)))));
}
static void vh_movido(Ctx *l, int i, void *d) { (void)i; (void)d; ui_sucio(ui_ancestro(l, &ClsVisorHex)); }
static Widget *vh_construir(Ctx *c)
{
    VisorHexE *s = ui_estado(c);
    i32 filas = (i32)((s->n + VH_BPF - 1) / VH_BPF);
    return Teclas(sh_teclas, Pantalla(s->nombre, Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        Expandido(filas ? ListaPerezosa(.n = filas, .fila = vh_fila, .al_mover = vh_movido, .sel = &s->sel, .alto_fila = (i16)(dis_alto_texto(1, F_MONO, "g") + 2))
                        : Centro(Texto(s->tam ? "(no cabe en memoria)" : "(fichero vacío)", .color = sh_tema()->tenue))),
        Caja(.an = INF, .color = sh_tema()->barra, .hijo = RellenoXY(4, 2, Texto(
            ui_fmt("fila %d de %d · %s%s", filas ? s->sel + 1 : 0, filas, sh_tam(s->tam), s->tam > VH_MAX ? " (64 KB)" : ""), .color = sh_tema()->tenue, .marcas = F_ESTILO(DIS_T_PEQUENA)))))),
        "", "Atrás"));
}
COMPONENTE_CON_ESTADO(VisorHex, VisorHexE, vh_construir, vh_iniciar, .desechar = vh_desechar);
Widget *visor_hex(const char *ruta)
{
    static char copia[200];
    size_t n = strlen(ruta); if (n > sizeof copia - 1) n = sizeof copia - 1;
    memcpy(copia, ruta, n); copia[n] = 0;
    return UI_W(VisorHex, .ruta = copia);
}
