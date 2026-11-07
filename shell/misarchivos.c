/* «MIS ARCHIVOS» (pista S6 del W580): lo que comparten las apps para guardar ficheros del usuario en la zona W6U
 * (hw_usr_*, que en hw_fs_* es la carpeta HW_FS_USUARIO). Aquí no hay formato ni flash: solo pantallas.
 *
 *   sh_usr_motivo()      el porqué del último fallo de hw_usr_*, dicho para una persona
 *   sh_pedir_nombre()    una pantalla que pide un nombre de fichero con el teclado numérico y lo valida
 *   sh_usr_progreso()    el diálogo con la barra de una copia o un renombrado en marcha (hw_usr_paso), cancelable
 *   sh_captura()         la pantalla, a un PNG en Mis archivos (con «Actividad» mantenida, pulsar «#»)
 */
#include "shell.h"
#include "tarjeta.h"                                 /* (FATW) la barra y los motivos de la tarjeta */

u32 alm_crc32(u32 crc, const void *d, u32 n);         /* api/almacen.c: el CRC-32 de siempre (el de PNG y zlib), encadenable */

/* ---- el motivo, con palabras ---------------------------------------------------------------------------- */
const char *sh_usr_motivo(void)
{
    switch (hw_usr_motivo()) {
    case HW_USR_M_NADA:    return "Hecho.";
    case HW_USR_M_LLENO:   return "No cabe: en Mis archivos no queda sitio para eso. Borra algo y vuelve a probar.";
    case HW_USR_M_NOMBRE:  return "Ese nombre no vale: tiene que tener entre 1 y 47 bytes y no llevar «/».";
    case HW_USR_M_MUCHOS:  return "Ya hay 64 archivos, que es el máximo. Borra alguno.";
    case HW_USR_M_EXISTE:  return "Ya hay un archivo con ese nombre.";
    case HW_USR_M_NO_HAY:  return "Ese archivo ya no existe.";
    case HW_USR_M_OCUPADA: return "Hay otra copia en marcha: espera a que acabe.";
    case HW_USR_M_FUENTE:  return "No se pudo leer el archivo de origen. No se ha guardado nada.";
    case HW_USR_M_MISMO:   return "Es el mismo nombre que ya tiene.";
    case HW_USR_M_SIN_ZONA: return "Este teléfono no tiene zona de archivos de usuario.";
    case HW_USR_M_SOLO_LECTURA: return "La memoria está protegida contra escritura: aquí no se puede guardar ni borrar.";
    case HW_USR_M_CANCELADA: return "Se abandonó antes de acabar: no se ha guardado nada y lo que había sigue como estaba.";
    default:               return "La memoria ha dado un error al escribir. El archivo está entero: como estaba o, si llegó a guardarse, con lo nuevo. Míralo.";   /* (R3; AUD3-B4) antes prometía «sigue como estaba» */
    }
}

/* ---- (I1) la Memoria (W6F), con palabras ------------------------------------------------------------------- */
const char *sh_w6f_motivo(void)
{
    switch (hw_w6f_motivo()) {
    case HW_W6F_M_NADA:     return "Hecho.";
    case HW_W6F_M_LLENO:    return "No cabe: en la Memoria no queda sitio para eso. Borra algo y vuelve a probar.";
    case HW_W6F_M_NOMBRE:   return "Ese nombre no vale: entre 1 y 63 bytes, sin «/», sin «.» ni «..» solos.";
    case HW_W6F_M_MUCHOS:   return "La Memoria ya tiene el máximo de archivos y carpetas (1098, con las carpetas). Borra alguno.";
    case HW_W6F_M_EXISTE:   return "Ya hay algo con ese nombre ahí.";
    case HW_W6F_M_NO_HAY:   return "Eso ya no existe.";
    case HW_W6F_M_OCUPADA:  return "Hay otra copia en marcha: espera a que acabe.";
    case HW_W6F_M_DENTRO:   return "La Memoria está ocupada con otra operación: vuelve a probar en un momento.";
    case HW_W6F_M_FUENTE:   return "No se pudo leer el archivo de origen. No se ha guardado nada.";
    case HW_W6F_M_SIN_ZONA: return "Este teléfono no tiene la Memoria montada.";
    case HW_W6F_M_SOLO_LECTURA: return "La Memoria está en solo lectura: aquí no se puede guardar ni borrar (mira el porqué en Sistema).";
    case HW_W6F_M_CANCELADA: return "Se abandonó antes de acabar: no se ha guardado nada y lo que había sigue como estaba.";
    case HW_W6F_M_NO_VACIA: return "La carpeta no está vacía: borra antes lo que tiene dentro.";
    case HW_W6F_M_NO_DIR:   return "Eso no es una carpeta.";
    case HW_W6F_M_ES_DIR:   return "Ahí hay una carpeta con ese nombre.";
    case HW_W6F_M_DANADO:   return "Ese archivo está dañado (una parte no pasa su comprobación): no se ha podido usar entero.";
    case HW_W6F_M_PROFUNDO: return "Demasiadas carpetas una dentro de otra (16 como mucho).";
    case HW_W6F_M_DEFICIT:  return "Ahora no se puede guardar: a la Memoria le falta un bloque de reserva (uno se ha estropeado). Borrar sí se puede; se repone sola.";
    default:                return "La memoria ha dado un error al escribir. Lo que había sigue entero: como estaba o, si llegó a guardarse, con lo nuevo.";
    }
}
const char *sh_w6f_sl(int m)
{
    return m == 1 ? "pedido (sin permiso de escritura)" : m == 2 ? "la secuencia se agota" : m == 3 ? "sin bloques de reserva" : m == 4 ? "la flash no dejó escribir un punto de control" : "no";
}
const char *sh_w6f_situacion(const HwW6fEstado *w)
{
    switch (w->situacion) {
    case HW_W6F_MONTADO:  return w->escribible ? ui_fmt("se escribe, %s", sh_tam(w->capacidad)) : "solo lectura";
    case HW_W6F_VIRGEN:   return "SIN FORMATO (se le da al arrancar)";
    case HW_W6F_AJENO:    return "AJENA (pregunta al arrancar)";
    case HW_W6F_OTRA_CONF: return "OTRA CONFIGURACIÓN: no se toca";
    case HW_W6F_OTRA_VERSION: return "OTRA VERSIÓN: no se toca";
    case HW_W6F_FALLO:    return "NO SE PUDO MONTAR";
    default:              return "no hay";
    }
}

/* ---- pedir un nombre ------------------------------------------------------------------------------------- */
typedef struct { Widget base; } PideNombre;
typedef struct { u8 nada; } PideNombreE;
extern const Clase ClsPideNombre;
#define PN_CAP 64                                       /* deja teclear de más: así el «demasiado largo» se DICE, no se calla */
static struct {
    char titulo[40], buf[PN_CAP], queja[120];
    EntradaTexto ent;
    void (*hecho)(const char *nombre, void *dato); void *dato;
} pn;

static void pn_cambio(Ctx *x, void *d) { (void)d; pn.queja[0] = 0; ui_sucio(ui_ancestro(x, &ClsPideNombre)); }
static void pn_quejarse(Ctx *c, const char *q) { size_t n = strlen(q); if (n > sizeof pn.queja - 1) n = sizeof pn.queja - 1; memcpy(pn.queja, q, n); pn.queja[n] = 0; ui_sucio(c); }
static void pn_aceptar(Ctx *c)
{
    unsigned i, n = pn.ent.len;
    while (n && pn.buf[n - 1] == ' ') n--;              /* sin espacios al final (no se ven y luego no se encuentra) */
    pn.buf[n] = 0; pn.ent.len = (u16)n; if (pn.ent.cur > n) pn.ent.cur = (u16)n;
    if (!n) { pn_quejarse(c, "Escribe un nombre."); return; }
    for (i = 0; i < n; i++) if (pn.buf[i] == '/' || pn.buf[i] == '\n') { pn_quejarse(c, "El nombre no puede llevar «/» ni saltos de línea."); return; }
    if (n > HW_USR_MAX_NOMBRE) { pn_quejarse(c, ui_fmt("Demasiado largo: mide %u bytes y el máximo son %d (las letras con tilde y la ñ cuentan 2).", n, HW_USR_MAX_NOMBRE)); return; }
    ui_sacar();
    if (pn.hecho) pn.hecho(pn.buf, pn.dato);
}
static int pn_tecla(Ctx *t, int k, void *dato)
{
    Ctx *c = ui_ancestro(t, &ClsPideNombre);
    (void)dato;
    if (k == K_SEL_IZQ || k == K_OK) { pn_aceptar(c); return 1; }
    if (k == K_SEL_DER || k == K_ATRAS) { ui_sacar(); return 1; }
    if (k == K_C) return 1;                             /* C con el campo vacío no cierra la pantalla */
    return 0;
}
static Widget *pn_construir(Ctx *c)
{
    const TemaDatos *t = sh_tema();
    (void)c;
    return Teclas(pn_tecla, Pantalla(pn.titulo, Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        RellenoXY(6, 6, Texto("Nombre del archivo:", .color = t->tenue)),
        RellenoXY(6, 0, Caja(.an = INF, .color = t->fondo, .borde = t->acento, .hijo = Relleno(4,
            CampoTexto(&pn.ent, .lineas = 2, .autofoco = 1, .al_cambiar = pn_cambio)))),
        RellenoXY(6, 6, Parrafo(pn.queja[0] ? pn.queja : "Hasta 47 bytes, sin «/». El 1 da el punto; * cambia entre abc, Abc, ABC y 123.",
                                .color = pn.queja[0] ? t->acento : t->tenue)),
        Expandido(Hueco()),
        Caja(.an = INF, .color = t->barra, .hijo = RellenoXY(4, 2, Texto(
            ui_fmt("%d de %d bytes · %s · C borra", pn.ent.len, HW_USR_MAX_NOMBRE, et_modo_nombre(&pn.ent)), .color = t->tenue))))),
        "Aceptar", "Cancelar"));
}
COMPONENTE_CON_ESTADO(PideNombre, PideNombreE, pn_construir, NULL);

void sh_pedir_nombre(const char *titulo, const char *inicial, void (*hecho)(const char *nombre, void *dato), void *dato)
{
    size_t n = strlen(titulo ? titulo : "");
    if (n > sizeof pn.titulo - 1) n = sizeof pn.titulo - 1;
    memcpy(pn.titulo, titulo ? titulo : "", n); pn.titulo[n] = 0;
    pn.queja[0] = 0; pn.hecho = hecho; pn.dato = dato;
    et_iniciar(&pn.ent, pn.buf, PN_CAP);
    pn.ent.modo = ET_ABC;
    if (inicial && strlen(inicial) < PN_CAP) et_poner(&pn.ent, inicial);
    if (pn.ent.len > HW_USR_MAX_NOMBRE) {               /* el nombre que se propone ya no cabe (un fichero de fábrica de nombre largo): se dice de entrada */
        char *q = ui_fmt("Este nombre mide %d bytes y el máximo son %d: acórtalo (C borra).", pn.ent.len, HW_USR_MAX_NOMBRE);
        memcpy(pn.queja, q, strlen(q) + 1);
    }
    ui_empujar(UI_W(PideNombre));
}

/* ---- la barra de una copia o un renombrado en marcha ----------------------------------------------------------
 * La operación ya está EMPEZADA (hw_usr_copiar / hw_usr_renombrar dijeron HW_OK). Una tarea de la UI da los pasos
 * (un trozo cada uno: la pantalla sigue viva y la barra se mueve) y, al acabar, quita el diálogo y avisa a quien lo
 * pidió con el resultado: HW_OK, HW_ERROR (mira hw_usr_motivo) o SH_USR_CANCELADA. Cancelar deja el destino como
 * estaba (lo escrito no tiene compromiso: no existe). Ninguna otra tecla hace nada mientras dura. */
typedef struct { Widget base; } UsrProgreso;
typedef struct { u32 hecho, total; } UsrProgresoE;
extern const Clase ClsUsrProgreso;
/* UN trozo (4 KiB) por vuelta del bucle: el teclado se mira una vez por vuelta, así que «Cancelar» tarda como mucho lo
 * que tarda un trozo en grabarse. En el emulador eso son ~0,3 s (la copia va a ~12 KB/s [observado]); en el teléfono,
 * [sin saber]. Una pulsación más corta que eso se puede perder: se MANTIENE la tecla hasta que el diálogo se va. */
#define PG_PASO 4096u
static struct { char titulo[32], que[64]; void (*fin)(int r, void *dato); void *dato;
                int (*paso)(u32 max, u32 *hecho, u32 *total); void (*cancelar)(void); } pg;   /* (I1) de Mis archivos o de la Memoria */
static u8 pg_w6f;

static void pg_cerrar(int r)
{
    void (*f)(int, void *) = pg.fin;
    pg.fin = NULL;
    ui_quitar_ruta(&ClsUsrProgreso);                    /* por su clase: puede tener encima el menú de la tecla de encendido */
    if (f) f(r, pg.dato);
}
static int pg_tarea(Ctx *c, void *dato)
{
    UsrProgresoE *s = ui_estado(c);
    int r = pg.paso(PG_PASO, &s->hecho, &s->total);
    (void)dato;
    ui_sucio(c);
    if (r == 1) return 1;
    pg_cerrar(r);
    return 0;
}
static void pg_iniciar(Ctx *c) { ui_tarea(c, pg_tarea, NULL); }
static int pg_tecla(Ctx *t, int k, void *dato)
{
    (void)t; (void)dato;
    if ((k == K_SEL_DER || k == K_ATRAS || k == K_C) && pg.fin) { pg.cancelar(); pg_cerrar(SH_USR_CANCELADA); }
    return 1;
}
static Widget *pg_construir(Ctx *c)
{
    UsrProgresoE *s = ui_estado(c);
    const TemaDatos *t = sh_tema();
    return Teclas(pg_tecla, Velo(Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        Expandido(Centro(RellenoXY(10, 4, Caja(.an = INF, .color = t->fondo, .borde = t->acento, .hijo = Relleno(1, Columna(.cruzado = P_ESTIRAR, .encoge = 1, .hijos = HIJOS(
            Caja(.an = INF, .color = t->barra, .hijo = RellenoXY(6, 4, Texto(pg.titulo, .color = t->acento))),
            Relleno(6, Parrafo(pg.que)),
            RellenoXY(6, 2, Progreso(.valor = (i32)(s->hecho / 16), .max = (i32)(s->total / 16 ? s->total / 16 : 1), .color = t->acento, .fondo = t->barra, .al = 8)),
            Relleno(6, Texto(ui_fmt("%s de %s", sh_tam(s->hecho), sh_tam(s->total)), .color = t->tenue))))))))),
        Caja(.an = INF, .al = dis_tokens()->m.alto_barra_teclas, .color = t->barra, .hijo = RellenoXY(4, 0, Fila(.cruzado = P_CENTRO, .hijos = HIJOS(   /* (estética) el alto de las teclas del diseño */
            Texto("mantén pulsado para", .color = t->tenue, .marcas = F_ESTILO(DIS_T_PEQUENA)), Hueco(), Texto("Cancelar")))))))));
}
COMPONENTE_CON_ESTADO(UsrProgreso, UsrProgresoE, pg_construir, pg_iniciar);

void sh_usr_progreso(const char *titulo, const char *que, void (*fin)(int r, void *dato), void *dato)
{
    size_t n = strlen(titulo); if (n > sizeof pg.titulo - 1) n = sizeof pg.titulo - 1;
    memcpy(pg.titulo, titulo, n); pg.titulo[n] = 0;
    n = strlen(que); if (n > sizeof pg.que - 1) n = sizeof pg.que - 1;
    while (n && ((u8)que[n] & 0xC0) == 0x80) n--;       /* sin partir un carácter UTF-8 */
    memcpy(pg.que, que, n); pg.que[n] = 0;
    pg.fin = fin; pg.dato = dato;
    pg.paso = pg_w6f == 2 ? hw_tj_paso : pg_w6f ? hw_w6f_paso : hw_usr_paso; pg.cancelar = pg_w6f == 2 ? hw_tj_cancelar : pg_w6f ? hw_w6f_cancelar : hw_usr_cancelar;
    pg_w6f = 0;
    ui_empujar_emergente(UI_W(UsrProgreso));
}
void sh_w6f_progreso(const char *titulo, const char *que, void (*fin)(int r, void *dato), void *dato)
{
    pg_w6f = 1;                                         /* (I1) la misma barra, sobre la copia a la Memoria */
    sh_usr_progreso(titulo, que, fin, dato);
}

/* (FATW) la misma barra sobre la copia a la TARJETA (hw_tj_copiar), y sus motivos con palabras */
void sh_tj_progreso(const char *titulo, const char *que, void (*fin)(int r, void *dato), void *dato)
{
    pg_w6f = 2;
    sh_usr_progreso(titulo, que, fin, dato);
}
const char *sh_tj_motivo(void)
{
    switch (hw_tj_motivo()) {
    case HW_W6F_M_NADA:     return "Hecho.";
    case HW_W6F_M_LLENO:    return "No cabe: en la tarjeta no queda sitio para eso (o la carpeta raíz ya está llena). Borra algo y vuelve a probar.";
    case HW_W6F_M_NOMBRE:   return "Ese nombre no vale en la tarjeta: entre 1 y 63 bytes, sin \\ / : * ? \" < > |, y sin punto ni espacio al final.";
    case HW_W6F_M_EXISTE:   return "Ya hay algo con ese nombre ahí.";
    case HW_W6F_M_NO_HAY:   return "Eso ya no existe.";
    case HW_W6F_M_OCUPADA:  return "La tarjeta está ocupada con otra copia o recepción: espera a que acabe.";
    case HW_W6F_M_DENTRO:   return "Una carpeta no se puede mover dentro de sí misma.";
    case HW_W6F_M_FUENTE:   return "No se pudo leer el archivo de origen. No se ha guardado nada.";
    case HW_W6F_M_SIN_ZONA: return "No hay tarjeta (o no tiene un formato FAT que se pueda usar).";
    case HW_W6F_M_SOLO_LECTURA: return "La tarjeta está protegida contra escritura (o quedó a medias y no se pudo revisar): solo se puede leer.";
    case HW_W6F_M_CANCELADA: return "Se abandonó antes de acabar: no se ha guardado nada y lo que había sigue como estaba.";
    case HW_W6F_M_NO_VACIA: return "La carpeta no está vacía: borra antes lo que tiene dentro.";
    case HW_W6F_M_NO_DIR:   return "Eso no es una carpeta.";
    case HW_W6F_M_ES_DIR:   return "Ahí hay una carpeta con ese nombre.";
    default:                return "La tarjeta dio un error al escribir (¿se ha sacado?). Lo que no se estaba tocando sigue igual; al volver a meterla se revisa sola.";
    }
}

/* ---- (R3; AUD3-M1) LA FRANJA «GUARDANDO»: progreso visible de una operación larga de flash ------------------------------
 * Una limpieza de Mis archivos o un formato corren DENTRO de una llamada (el bucle no da vueltas): sin esto el teléfono parece
 * colgado y el usuario le quita la batería a media limpieza (el camino que fabricaba cortes en serie: auditoría de datos, A1).
 * La plataforma avisa de cada paso (hw_progreso); si la operación lleva más de 0,7 s, aquí se pinta —directamente en el
 * cuadro, sin pasar por el árbol de widgets— una franja en la fila de las teclas de función, con barra si se sabe el total.
 * Al acabar se repinta todo (ui_forzar_todo). Lo corto no enseña nada (en el PC el reloj del guion no corre dentro de una
 * llamada: las capturas no cambian). Con una app externa en pantalla o el panel dormido no se pinta. */
static u32 fr_t0, fr_visto; static u8 fr_viva;
static void flash_progreso(u32 hecho, u32 total)
{
    const TemaDatos *t = sh_tema();
    u32 ahora = hw_ms();
    /* (estética) la franja tapa la fila de las teclas de función del diseño en uso (antes, 16 px fijos) y el texto va en la talla pequeña */
    const int FRANJA_ALTO = dis_tokens()->m.alto_barra_teclas, pq = F_ESTILO(DIS_T_PEQUENA), ht = dis_alto_texto(1, pq, NULL);
    int y = UI_ALTO - FRANJA_ALTO, x0 = 8 + li_ancho_texto_f("Guardando…", 1, pq) + 8, an = UI_ANCHO - x0 - 6, w, yb = y + FRANJA_ALTO / 2 - 3;
    Lienzo l = { ui_pantalla, { 0, (i16)y, UI_ANCHO, (i16)FRANJA_ALTO } };
    if (!fr_viva || ahora - fr_visto > 1500u) { fr_viva = 1; fr_t0 = ahora; }   /* otra operación: la cuenta empieza de nuevo */
    fr_visto = ahora;
    if (ahora - fr_t0 < 700u || app_pantalla_tomada() || sh_pantalla_apagada()) return;
    li_rect(&l, 0, y, UI_ANCHO, FRANJA_ALTO, t->barra);
    li_texto_f(&l, 8, y + (FRANJA_ALTO - ht) / 2, "Guardando…", t->texto, 1, pq);
    li_rect(&l, x0, yb, an, 6, t->fondo);
    if (total) w = (int)((u32)an * (hecho > total ? total : hecho) / total);
    else { w = an / 4; x0 += (int)((ahora / 120u) % 4u) * (an / 4); }             /* sin total: un trozo que se mueve */
    li_rect(&l, x0, yb, w, 6, t->acento);
    hw_pantalla_volcar(ui_pantalla, y, FRANJA_ALTO);
    ui_forzar_todo();                                                            /* el cuadro siguiente del shell lo repinta todo */
}
void sh_flash_progreso_init(void) { hw_progreso_gancho(flash_progreso); }

/* ---- la captura de pantalla ---------------------------------------------------------------------------------
 * Lo que hay en el cuadro (ui_pantalla, 240x320) a un PNG de verdad en Mis archivos: «captura NN.png».
 * PNG RGB de 8 bits, sin entrelazar, con UN bloque deflate de Huffman FIJO y solo repeticiones de distancia 1
 * (correr el byte anterior). Con el filtro de fila que deje más ceros (Sub o Up) las zonas lisas de una pantalla
 * de la UI se quedan en casi nada; un fondo con fotografía comprime mal y ocupa más (hasta ~1,1 x 231 KB).
 * Se hace DOS veces sobre el mismo cuadro: la primera solo cuenta (W6U necesita el tamaño antes de empezar y el
 * trozo IDAT lleva su longitud delante) y la segunda escribe, a trozos, por la escritura atómica de siempre: si se
 * va la corriente a medias no queda ninguna captura a medias. No bloquea más que lo que tarda la flash. */
static struct { u8 buf[2048]; u32 n, total, crc, bits, a, b; int nbits, escribe, mal; int ant, corre; } cz;

static void cz_byte(u32 v)
{
    cz.total++;
    if (!cz.escribe) return;
    cz.buf[cz.n++] = (u8)v;
    if (cz.n == sizeof cz.buf) { if (hw_usr_mas(cz.buf, cz.n) != HW_OK) cz.mal = 1; cz.n = 0; }
}
static void cz_dato(u32 v) { u8 x = (u8)v; cz.crc = alm_crc32(cz.crc, &x, 1); cz_byte(v); }   /* un byte de un trozo PNG: cuenta para su CRC */
static void cz_be32(u32 v, int con_crc) { int i; for (i = 24; i >= 0; i -= 8) { if (con_crc) cz_dato(v >> i); else cz_byte((v >> i) & 255); } }
static void cz_bits(u32 v, int n)                       /* deflate: los bits salen del menos significativo al más */
{
    cz.bits |= v << cz.nbits; cz.nbits += n;
    while (cz.nbits >= 8) { cz_dato(cz.bits & 255); cz.bits >>= 8; cz.nbits -= 8; }
}
static void cz_huff(u32 codigo, int n)                  /* un código de Huffman: del bit más significativo al menos */
{
    u32 r = 0; int i;
    for (i = 0; i < n; i++) r |= ((codigo >> i) & 1) << (n - 1 - i);
    cz_bits(r, n);
}
static void cz_literal(int v) { if (v < 144) cz_huff(0x30u + (u32)v, 8); else cz_huff(0x190u + (u32)v - 144, 9); }
static void cz_repite(int len)                          /* «copia len bytes de 1 más atrás», 3..258 */
{
    static const u16 base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const u8 extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    int c = 28;
    while (base[c] > len) c--;
    if (c + 257 < 280) cz_huff((u32)c + 1, 7); else cz_huff(0xC0u + (u32)c - 23, 8);
    if (extra[c]) cz_bits((u32)(len - base[c]), extra[c]);
    cz_huff(0, 5);                                      /* distancia 1 */
}
static void cz_soltar(void)                             /* lo que se venía repitiendo */
{
    while (cz.corre >= 3) { int l = cz.corre > 258 ? 258 : cz.corre; cz_repite(l); cz.corre -= l; }
    while (cz.corre > 0) { cz_literal(cz.ant); cz.corre--; }
}
static void cz_crudo(int v)                             /* un byte de la imagen ya filtrada */
{
    cz.a = (cz.a + (u32)v) % 65521u; cz.b = (cz.b + cz.a) % 65521u;          /* Adler-32 de lo sin comprimir */
    if (v == cz.ant) { cz.corre++; return; }
    cz_soltar();
    cz_literal(v); cz.ant = v;
}
static void cz_fila(int y, u8 *fila)                    /* la fila y del cuadro a RGB de 8 bits */
{
    int x;
    for (x = 0; x < UI_ANCHO; x++) {
        unsigned c = ui_pantalla[y * UI_ANCHO + x], v = ((c >> 8) | (c << 8)) & 0xFFFF, r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        fila[3 * x] = (u8)((r << 3) | (r >> 2)); fila[3 * x + 1] = (u8)((g << 2) | (g >> 4)); fila[3 * x + 2] = (u8)((b << 3) | (b >> 2));
    }
}
/* El PNG entero. Con escribe = 0 solo cuenta (devuelve los bytes del fichero y deja en *idat los del flujo zlib). */
static u32 cz_png(int escribe, u32 idat_n, u32 *idat)
{
    static const u8 firma[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    static u8 f0[UI_LADO_MAX * 3], f1[UI_LADO_MAX * 3];   /* (giro) la fila más ancha de las dos orientaciones */
    u8 *ant = f0, *act = f1, *tmp;
    u32 i, antes;
    int y;
    memset(&cz, 0, sizeof cz); cz.escribe = escribe;
    for (i = 0; i < 8; i++) cz_byte(firma[i]);
    cz_be32(13, 0); cz.crc = 0;
    cz_dato('I'); cz_dato('H'); cz_dato('D'); cz_dato('R');
    cz_be32(UI_ANCHO, 1); cz_be32(UI_ALTO, 1); cz_dato(8); cz_dato(2); cz_dato(0); cz_dato(0); cz_dato(0);
    cz_be32(cz.crc, 0);
    cz_be32(idat_n, 0); cz.crc = 0;
    cz_dato('I'); cz_dato('D'); cz_dato('A'); cz_dato('T');
    antes = cz.total;
    cz_dato(0x78); cz_dato(0x01);                       /* zlib: deflate, ventana de 32 K */
    cz_bits(1, 1); cz_bits(1, 2);                       /* un solo bloque, el último, de Huffman fijo */
    cz.a = 1; cz.b = 0; cz.ant = -1; cz.corre = 0;
    memset(ant, 0, UI_ANCHO * 3);
    for (y = 0; y < UI_ALTO; y++) {
        int x, ceros_sub = 0, ceros_up = 0, sub;
        cz_fila(y, act);
        for (x = 0; x < UI_ANCHO * 3; x++) {
            if ((u8)(act[x] - (x >= 3 ? act[x - 3] : 0)) == 0) ceros_sub++;
            if (act[x] == ant[x]) ceros_up++;
        }
        sub = ceros_sub > ceros_up;
        cz_crudo(sub ? 1 : 2);                          /* el filtro de la fila: 1 = Sub (el de la izquierda), 2 = Up (el de arriba) */
        for (x = 0; x < UI_ANCHO * 3; x++) cz_crudo((u8)(act[x] - (sub ? (x >= 3 ? act[x - 3] : 0) : ant[x])));
        tmp = ant; ant = act; act = tmp;
    }
    cz_soltar();
    cz_huff(0, 7);                                      /* fin del bloque */
    if (cz.nbits) cz_bits(0, 8 - cz.nbits);
    { u32 adler = (cz.b << 16) | cz.a; for (y = 24; y >= 0; y -= 8) cz_dato(adler >> y); }
    if (idat) *idat = cz.total - antes;
    cz_be32(cz.crc, 0);
    cz_be32(0, 0); cz.crc = 0;
    cz_dato('I'); cz_dato('E'); cz_dato('N'); cz_dato('D');
    cz_be32(cz.crc, 0);
    if (escribe && cz.n && hw_usr_mas(cz.buf, cz.n) != HW_OK) cz.mal = 1;
    return cz.total;
}
void sh_captura(void)
{
    HwUsrEstado es;
    char nombre[24];
    u32 idat = 0, tam;
    int i;
    hw_usr_estado(&es);
    if (!es.montado || !es.escribible) { ui_aviso("Captura", es.montado ? "La memoria está protegida contra escritura: no se puede guardar la captura." : "Este teléfono no tiene Mis archivos: no hay dónde guardar la captura."); return; }
    for (i = 1; i < 100; i++) {                         /* el primer «captura NN.png» que esté libre */
        char *n = ui_fmt("captura %02d.png", i);
        memcpy(nombre, n, strlen(n) + 1);
        if (hw_usr_tam(nombre) == HW_NO_DISPONIBLE) break;
    }
    if (i == 100) { ui_aviso("Captura", "Ya hay 99 capturas en Mis archivos: borra alguna."); return; }
    tam = cz_png(0, 0, &idat);                          /* 1.ª pasada: cuánto ocupa */
    if (hw_usr_empezar(nombre, tam) != HW_OK) {
        ui_aviso("Captura", hw_usr_motivo() == HW_USR_M_LLENO ? ui_fmt("No cabe la captura: ocupa %s y en Mis archivos quedan %s libres. Borra algo.", sh_tam(tam), sh_tam(es.libre)) : sh_usr_motivo());
        return;
    }
    cz_png(1, idat, NULL);                              /* 2.ª: la misma, escribiendo */
    if (cz.mal || cz.total != tam) { hw_usr_cancelar(); ui_aviso("Captura", "No se pudo guardar la captura. No ha quedado nada a medias."); return; }
    if (hw_usr_acabar() != HW_OK) { ui_aviso("Captura", sh_usr_motivo()); return; }
    hw_log("captura: guardada en la zona de usuario\n");
    ui_aviso("Captura", ui_fmt("Guardada en Mis archivos: «%s» (%s).", nombre, sh_tam(tam)));
}
