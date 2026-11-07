/* usuario.c — «W6U», la zona de FICHEROS DE USUARIO. Interfaz y garantías: usuario.h. Pista S6.
 * Nada de aquí toca registros: todo va por el puerto (leer / programar / borrar), como el almacén.
 *
 * ---- EL FORMATO (todo little-endian, alineado a 4) ---------------------------------------------------------
 *
 * BLOQUE:  +0 u32 'W6U1'   +4 u32 secuencia   +8 u32 CRC32(esos 8 bytes)   +12 u32 «retirado»
 *   Los bloques con cabecera buena y «retirado» == 0xFFFFFFFF forman EL REGISTRO, ordenados por secuencia:
 *   el de menor secuencia es la COLA y el de mayor, la CABEZA (donde se escribe). Los demás están libres.
 *   «retirado» se programa (a 0) cuando todo lo vivo del bloque ya está copiado más adelante: desde ese
 *   momento el bloque no cuenta, quede como quede el borrado que venga después.
 *
 * REGISTRO: +0 u16 marca   +2 u16 n (bytes de carga)   +4 u32 tx   +8 u32 a   +12 u32 CRC32(carga)
 *           +16 u32 CRC32(los 16 bytes anteriores)   +20 carga, rellena a 4 con 0xFF
 *           y, lo ÚLTIMO que se escribe y en una operación aparte, u32 SELLO = ~CRC de la cabecera con el
 *           bit 31 a 0 (nunca puede valer 0xFFFFFFFF = «sin escribir»). Un registro sin su sello no existe.
 *   'TZ' TROZO       tx = la escritura a la que pertenece; a = desplazamiento dentro del fichero; carga = datos
 *   'CM' COMPROMISO  tx = esa escritura; a = tamaño del fichero; carga = u32 fecha, u32 CRC32 del fichero
 *                    entero, u8 longitud del nombre, nombre.  ES EL QUE HACE QUE EL FICHERO EXISTA.
 *   'BR' BORRADO     tx = uno nuevo; carga = u8 longitud del nombre, nombre
 *
 * QUÉ VALE: para cada nombre, el ÚLTIMO compromiso o borrado del registro (en orden de bloque y posición).
 * Los trozos se buscan por (tx, a): si hay dos iguales (una limpieza cortada) vale el último; son idénticos.
 *
 * ESCRIBIR un fichero = sus trozos (que nadie ve: no tienen compromiso) + su compromiso. Hasta que el sello
 * del compromiso está escrito, el fichero anterior sigue siendo el vigente, entero; después lo es el nuevo,
 * entero. No hay punto intermedio. BORRAR = un registro de borrado: con sello, el fichero ya no existe.
 *
 * LIMPIEZA (lo que reparte el desgaste): cuando no quedan más de RES_BLQ bloques libres, se copia lo VIVO
 * de la cola a la cabeza (trozos y compromisos de ficheros vigentes y de la escritura en curso), se
 * comprueba, se RETIRA la cola y queda libre. Los borrados no se copian nunca: cuando un borrado llega a la
 * cola, todo lo anterior a él ya no existe (la limpieza va siempre por orden), así que ya no hace falta.
 * Un registro que no cabe en lo que queda de la cabeza NO se parte: se abre el bloque siguiente (lo que se
 * desperdicia al final de un bloque es menos que un registro máximo, y está descontado de la capacidad).
 *
 * CAPACIDAD: caben `limite` = (bloques − RES_BLQ) × (bloque − cabecera − registro máximo) bytes de
 * registros vivos. usr_empezar lo comprueba ANTES de escribir nada (con el fichero viejo todavía contando:
 * reemplazar un fichero necesita sitio para los dos a la vez, que es lo que cuesta la atomicidad) y deja
 * siempre sitio para un borrado, para que de un volumen lleno se pueda salir borrando.
 * Por qué DOS bloques de reserva y no uno: uno es el destino de la limpieza; el otro absorbe lo que
 * desperdician los cortes de corriente EN MITAD de una limpieza (cada corte deja a medias, como mucho, un
 * registro en la cabeza). Con uno solo, un corte ahí podía dejar la cola sin sitio donde acabar de copiarse.
 *
 * (R3; AUD3-A1, docs/auditoria_datos_e_hitos.md) LA RESERVA NO SE GASTA SIN PROGRESO, Y BORRAR CABE SIEMPRE.
 * Lo que había: la limpieza iba SIEMPRE por la cola. Con el volumen lleno de datos fríos, lo que desperdicia un corte
 * (un registro a medias en la cabeza) no se recuperaba hasta una vuelta entera del anillo después; unas decenas de
 * cortes dentro de limpiezas se comían los dos bloques de reserva y el volumen quedaba sin salida: ni escribir ni
 * BORRAR (el registro de borrado tampoco tenía sitio). Tres reglas nuevas:
 *  1. VÍCTIMA. Con la reserva entera (libres >= RES_BLQ) la limpieza sigue yendo por la cola: mismo comportamiento y
 *     mismo reparto del desgaste que antes. Con la reserva TOCADA (libres < RES_BLQ: solo pasa tras un corte o un fallo
 *     de la flash) se limpia el bloque cerrado que MENOS cuesta: primero los que no piden abrir ningún bloque (lo vivo
 *     que les queda cabe en la cabeza, o no les queda nada), después el de menos bytes vivos. Lo desperdiciado se
 *     recupera en la limpieza siguiente, no una vuelta después.
 *  2. NO SE EMPIEZA LO QUE NO CABE. Antes de copiar nada se cuenta (plan) cuántos bloques nuevos pide la víctima; si
 *     son más de los libres, USR_E_LLENO sin haber tocado la flash. Una limpieza ya no puede dejar el anillo sin
 *     bloques a medio copiar.
 *  3. SITIO PARA BORRAR. Los últimos `res_br` bytes de cada bloque no los usan ni los trozos, ni los compromisos, ni
 *     la limpieza: solo un registro de BORRADO cuando no hay otro sitio. Con el borrado escrito, los trozos de ese
 *     fichero dejan de estar vivos y sus bloques se retiran sin copiar (regla 1).
 * Una víctima que no es la cola obliga a un cuidado más: sus BORRADOS se copian si en un bloque ANTERIOR queda un
 * compromiso de ese nombre y más adelante no hay otro compromiso ni borrado suyo (si no, el fichero resucitaría).
 * LO QUE NO SE PUEDE [inferido; el modelo estricto no deja completar una palabra a medias]: cada corte gasta al menos
 * un registro. Una racha de decenas de cortes seguidos DENTRO de limpiezas, sin que ninguna acabe, sigue pudiendo
 * llenar: entonces se sale borrando (regla 3) y, en último término, con «Vaciar Mis archivos» (plat/).
 */
#include "usuario.h"

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);
int   memcmp(const void *a, const void *b, __SIZE_TYPE__ n);

#define MAGIA      0x31553657u         /* 'W6U1' */
#define M_TROZO    0x5A54u             /* 'TZ' */
#define M_COMPR    0x4D43u             /* 'CM' */
#define M_BORRA    0x5242u             /* 'BR' */
#define CAB_BLQ    16u
#define CAB_REG    20u
#define PAD4(n)    (((n) + 3u) & ~3u)
#define TOT(n)     (CAB_REG + PAD4(n) + 4u)          /* lo que ocupa un registro con n bytes de carga */
#define SELLO(cc)  (~(cc) & 0x7FFFFFFFu)
#define MAX_BLQ    64u
#define RES_BLQ    2u                  /* bloques que una escritura normal nunca gasta (ver arriba) */
#define TROZO_TOPE 4096u
#define CM_FIJO    9u                  /* fecha (4) + CRC del fichero (4) + longitud del nombre (1) */
#define TOT_CM_MAX TOT(CM_FIJO + USR_MAX_NOMBRE)
#define TOT_BR_MAX TOT(1u + USR_MAX_NOMBRE)

typedef struct { u32 blq, pos, tot, marca, n, tx, a, crc; const u8 *p; int valido; } Reg;
typedef struct {
    char nombre[USR_MAX_NOMBRE + 1];
    u32 tam, fecha, tx, crc;
    u32 coste;                         /* bytes de registros vivos que ocupa (sus trozos + su compromiso) */
    u32 pos0; u8 blq0;                 /* dónde está su trozo de desplazamiento 0 (pos0 = 0: no se sabe) */
} Fich;

static const AlmPuerto *pt;
static UsrEstado es;
static u32 nb, tb, trozo_max, limite;
static u32 res_br;                     /* (R3) al final de cada bloque: solo para registros de BORRADO (ver la cabecera) */
static UsrPaso al_paso;                /* (R3; AUD3-M1) «sigo viva»: tras cada registro copiado o grabado y alrededor de cada borrado */
#define TOPE_REG (tb - res_br)         /* hasta dónde llegan los registros que NO son un borrado de último recurso */
static void paso(u32 hecho, u32 total) { if (al_paso) al_paso(hecho, total); }
void usr_al_paso(UsrPaso f) { al_paso = f; }
static u8  ord[MAX_BLQ];               /* los bloques del registro, de la cola (ord[0]) a la cabeza */
static u32 n_ord, seq_cab;             /* cuántos son y la secuencia de la cabeza */
static u32 fin;                        /* en la cabeza: dónde va el siguiente registro */
static u32 ult_tx;                     /* el mayor tx visto: el siguiente es ult_tx + 1 */
static u32 vivos;                      /* bytes de registros de ficheros vigentes */
static Fich fi[USR_MAX_FICHEROS];      /* ordenados por nombre */
static u32 n_fi;
static struct {                        /* la escritura en curso (usr_empezar … usr_acabar) */
    u8 activo, blq0;
    char nombre[USR_MAX_NOMBRE + 1];
    u32 tam, fecha, tx, escrito, crc, coste, en_buf, pos0;
} ab;
static u8 buf[TROZO_TOPE];             /* donde usr_mas junta un trozo entero */

static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u32 le16(const u8 *p) { return p[0] | (p[1] << 8); }
static void p32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void p16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

#define CABEZA   (ord[n_ord - 1])
#define LIM(b)   ((b) == CABEZA ? fin : tb)          /* hasta dónde hay registros en ese bloque */

/* (AUD2 B4) ¿Es un volumen NUESTRO aunque no monte? La marca en la cabecera de un bloque basta (sin mirar el CRC). Solo lee. */
int usr_firma(const AlmPuerto *p)
{
    u32 b;
    if (!p || !p->leer) return 0;
    for (b = 0; b < p->bloques; b++) if (le32(p->leer(b)) == MAGIA) return 1;
    return 0;
}
/* ---- bloques ------------------------------------------------------------------------------------------- */
/* La secuencia del bloque si forma parte del registro (cabecera buena y sin retirar); 0 si no cuenta. */
static u32 seq_de(u32 b)
{
    const u8 *p = pt->leer(b);
    u32 s = le32(p + 4);
    if (le32(p) != MAGIA || le32(p + 8) != alm_crc32(0, p, 8) || le32(p + 12) != 0xFFFFFFFFu) return 0;
    return s == 0xFFFFFFFFu ? 0 : s;
}
static u32 libres(void) { return nb - n_ord; }
static int en_registro(u32 b) { u32 i; for (i = 0; i < n_ord; i++) if (ord[i] == b) return 1; return 0; }

/* ---- registros ----------------------------------------------------------------------------------------- */
/* El siguiente registro del bloque b desde *pos, sin pasar de lim. 0 = no hay más. La regla es UNA y la usan
 * el montaje, la lectura y la limpieza: en p hay un registro si su cabecera cuadra (marca, n en su rango, CRC)
 * y cabe en el bloque; entonces ocupa su longitud, tenga sello (válido) o no (se quedó a medias); si no, se
 * mira 4 bytes más allá. */
static int reg_sig(u32 b, u32 *pos, u32 lim, Reg *r)
{
    const u8 *m = pt->leer(b);
    u32 p = *pos;
    while (p + CAB_REG + 4 <= lim) {
        u32 marca = le16(m + p), n = le16(m + p + 2), tot = TOT(n), cc;
        if ((marca == M_TROZO ? n >= 1 && n <= trozo_max :
             marca == M_COMPR ? n > CM_FIJO && n <= CM_FIJO + USR_MAX_NOMBRE :
             marca == M_BORRA ? n > 1 && n <= 1 + USR_MAX_NOMBRE : 0)
            && p + tot <= tb && (cc = le32(m + p + 16)) == alm_crc32(0, m + p, 16)) {
            r->blq = b; r->pos = p; r->tot = tot; r->marca = marca; r->n = n;
            r->tx = le32(m + p + 4); r->a = le32(m + p + 8); r->crc = le32(m + p + 12); r->p = m + p;
            r->valido = le32(m + p + tot - 4) == SELLO(cc);
            *pos = p + tot;
            return 1;
        }
        p += 4;
    }
    *pos = p;
    return 0;
}

/* ---- la tabla de ficheros (en RAM, ordenada por nombre) -------------------------------------------------- */
static int cmp_nombre(const char *a, const char *b)
{
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    while (*x && *x == *y) { x++; y++; }
    return (int)*x - (int)*y;
}
static u32 nombre_len(const char *s)   /* 1..USR_MAX_NOMBRE si es un nombre válido; 0 si no */
{
    u32 n = 0;
    if (!s) return 0;
    while (s[n]) { if (s[n] == '/' || n >= USR_MAX_NOMBRE) return 0; n++; }
    return n;
}
static int buscar(const char *nombre)
{
    u32 i;
    for (i = 0; i < n_fi; i++) if (!cmp_nombre(fi[i].nombre, nombre)) return (int)i;
    return -1;
}
static Fich *por_tx(u32 tx) { u32 i; for (i = 0; i < n_fi; i++) if (fi[i].tx == tx) return &fi[i]; return 0; }
static void quitar(const char *nombre)
{
    int i = buscar(nombre);
    if (i < 0) return;
    for (n_fi--; (u32)i < n_fi; i++) fi[i] = fi[i + 1];
}
static Fich *poner(const char *nombre)  /* la entrada de ese nombre (nueva, en su sitio, si no estaba); 0 si no cabe */
{
    int i = buscar(nombre);
    u32 k;
    if (i >= 0) return &fi[i];
    if (n_fi >= USR_MAX_FICHEROS) return 0;
    for (k = n_fi; k > 0 && cmp_nombre(fi[k - 1].nombre, nombre) > 0; k--) fi[k] = fi[k - 1];
    memset(&fi[k], 0, sizeof fi[k]);
    for (i = 0; nombre[i]; i++) fi[k].nombre[i] = nombre[i];
    n_fi++;
    return &fi[k];
}
/* El nombre que trae la carga de un compromiso (desde = CM_FIJO - 1) o de un borrado (desde = 0), copiado y
 * comprobado. 0 si no es un nombre válido (basura con CRC bueno: no debería existir, pero no se le cree). */
static int nombre_de(const Reg *r, u32 desde, char *dst)
{
    const u8 *c = r->p + CAB_REG + desde;
    u32 len = c[0], i;
    if (!len || len > USR_MAX_NOMBRE || desde + 1 + len != r->n) return 0;
    for (i = 0; i < len; i++) { if (!c[1 + i] || c[1 + i] == '/') return 0; dst[i] = (char)c[1 + i]; }
    dst[len] = 0;
    return 1;
}

/* ¿Hay MÁS ADELANTE en el registro (desde el bloque ord[k], posición p) otro registro válido igual a r
 * (misma marca y tx, y para un trozo el mismo desplazamiento)? Si lo hay, r es una copia vieja. */
static int hay_despues(const Reg *r, u32 k, u32 p)
{
    Reg q;
    for (; k < n_ord; k++, p = CAB_BLQ)
        while (reg_sig(ord[k], &p, LIM(ord[k]), &q))
            if (q.valido && q.marca == r->marca && q.tx == r->tx && (q.marca != M_TROZO || q.a == r->a)) return 1;
    return 0;
}

/* Comprueba que los trozos de un fichero están TODOS y calcula lo que ocupa. 0 = le falta algo. */
static int cubrir(Fich *f)
{
    Reg r;
    u32 k, p, suma = 0, coste = 0, off;
    f->pos0 = 0;
    for (k = 0; k < n_ord; k++)
        for (p = CAB_BLQ; reg_sig(ord[k], &p, LIM(ord[k]), &r); )
            if (r.valido && r.marca == M_TROZO && r.tx == f->tx) {
                if (r.a > f->tam || r.n > f->tam - r.a) return 0;
                suma += r.n; coste += r.tot;
                if (r.a == 0) { f->blq0 = (u8)r.blq; f->pos0 = r.pos; }
            }
    if (suma != f->tam) {              /* hay trozos repetidos (una limpieza cortada) o falta alguno: a la manera lenta */
        for (off = 0, coste = 0; off < f->tam; ) {
            u32 n = 0, tot = 0;
            for (k = 0; k < n_ord; k++)
                for (p = CAB_BLQ; reg_sig(ord[k], &p, LIM(ord[k]), &r); )
                    if (r.valido && r.marca == M_TROZO && r.tx == f->tx && r.a == off) { n = r.n; tot = r.tot; }
            if (!n) return 0;
            off += n; coste += tot;
        }
    }
    f->coste = coste + TOT(CM_FIJO + nombre_len(f->nombre));
    return 1;
}

/* Reconstruye TODO el estado en RAM a partir de lo que hay en la flash. Lo usan el montaje y cualquier
 * camino de error (así la RAM nunca se queda contando algo que la flash no tiene). */
static int indexar(void)
{
    u32 seqs[MAX_BLQ], b, i, k, p, ult, hasta;
    const u8 *m;
    Reg r;
    char nombre[USR_MAX_NOMBRE + 1];
    n_ord = 0; n_fi = 0; vivos = 0; ult_tx = 0; fin = tb; ab.activo = 0;
    for (b = 0; b < nb; b++) {
        u32 s = seq_de(b);
        if (!s) continue;
        for (i = n_ord; i > 0 && seqs[i - 1] > s; i--) { seqs[i] = seqs[i - 1]; ord[i] = ord[i - 1]; }
        seqs[i] = s; ord[i] = (u8)b; n_ord++;
    }
    if (!n_ord) return USR_E_NO_HAY;
    seq_cab = seqs[n_ord - 1];
    /* dónde acaba lo escrito en la cabeza: detrás del último registro (entero o a medias) y de lo último que
     * no esté borrado, lo que quede más lejos */
    m = pt->leer(CABEZA);
    for (ult = tb; ult > CAB_BLQ && le32(m + ult - 4) == 0xFFFFFFFFu; ult -= 4) ;
    for (p = CAB_BLQ, hasta = CAB_BLQ; reg_sig(CABEZA, &p, tb, &r); ) hasta = r.pos + r.tot;
    fin = hasta > ult ? hasta : ult;
    /* primera pasada, en orden: quién vale para cada nombre, y el mayor tx */
    for (k = 0; k < n_ord; k++)
        for (p = CAB_BLQ; reg_sig(ord[k], &p, LIM(ord[k]), &r); ) {
            if (r.tx != 0xFFFFFFFFu && r.tx > ult_tx) ult_tx = r.tx;
            if (!r.valido) continue;
            if (r.marca == M_COMPR && nombre_de(&r, CM_FIJO - 1, nombre)) {
                Fich *f = poner(nombre);
                if (!f) { es.errores++; continue; }
                f->tam = r.a; f->tx = r.tx; f->fecha = le32(r.p + CAB_REG); f->crc = le32(r.p + CAB_REG + 4);
            } else if (r.marca == M_BORRA && nombre_de(&r, 0, nombre)) quitar(nombre);
        }
    /* segunda: a cada fichero vigente, sus trozos. Al que le falte alguno no existe (solo puede pasar con la
     * flash dañada: un compromiso no se escribe hasta que sus trozos están enteros y comprobados) */
    for (i = 0; i < n_fi; ) {
        if (cubrir(&fi[i])) { vivos += fi[i].coste; i++; }
        else { es.errores++; for (n_fi--, k = i; k < n_fi; k++) fi[k] = fi[k + 1]; }
    }
    return USR_OK;
}

/* ---- lectura -------------------------------------------------------------------------------------------- */
int usr_contar(void) { return es.montado ? (int)n_fi : 0; }
int usr_listar(u32 desde, UsrEntrada *e, u32 max)
{
    u32 i, n = 0;
    if (!es.montado || !e) return 0;
    for (i = desde; i < n_fi && n < max; i++, n++) {
        memcpy(e[n].nombre, fi[i].nombre, sizeof e[n].nombre);
        e[n].tam = fi[i].tam; e[n].fecha = fi[i].fecha;
    }
    return (int)n;
}
int usr_info(const char *nombre, UsrEntrada *e)
{
    int i;
    if (!es.montado) return USR_E_SIN_MONTAR;
    if (!nombre_len(nombre)) return USR_E_ARG;
    if ((i = buscar(nombre)) < 0) return USR_E_NO_HAY;
    if (e) { memcpy(e->nombre, fi[i].nombre, sizeof e->nombre); e->tam = fi[i].tam; e->fecha = fi[i].fecha; }
    return USR_OK;
}
int usr_leer(const char *nombre, u32 desde, void *bufr, u32 max)
{
    Fich *f; Reg r; int i; u32 n, k, p; u8 *d = bufr;
    if (!es.montado) return USR_E_SIN_MONTAR;
    if (!nombre_len(nombre) || (max && !bufr)) return USR_E_ARG;
    if ((i = buscar(nombre)) < 0) return USR_E_NO_HAY;
    f = &fi[i];
    if (desde >= f->tam || !max) return 0;
    n = f->tam - desde < max ? f->tam - desde : max;
    p = f->pos0;
    if (p && reg_sig(f->blq0, &p, tb, &r) && r.pos == f->pos0 && r.valido && r.marca == M_TROZO && r.tx == f->tx && r.a == 0
        && desde + n <= r.n) memcpy(d, r.p + CAB_REG + desde, n);          /* el caso corriente: cabe en su primer trozo */
    else {
        for (k = 0; k < n_ord; k++)                                        /* en orden: si un trozo está dos veces, queda el último */
            for (p = CAB_BLQ; reg_sig(ord[k], &p, LIM(ord[k]), &r); )
                if (r.valido && r.marca == M_TROZO && r.tx == f->tx && r.a < desde + n && r.a + r.n > desde) {
                    u32 a = r.a > desde ? r.a : desde, b = r.a + r.n < desde + n ? r.a + r.n : desde + n;
                    memcpy(d + (a - desde), r.p + CAB_REG + (a - r.a), b - a);
                }
    }
    if (desde == 0 && n == f->tam && alm_crc32(0, d, n) != f->crc) { es.errores++; return USR_E_FLASH; }   /* entero: se comprueba */
    return (int)n;
}
void usr_estado(UsrEstado *e)
{
    u32 disp = limite > vivos + TOT_BR_MAX + TOT_CM_MAX ? limite - vivos - TOT_BR_MAX - TOT_CM_MAX : 0, resto;
    es.bloques = nb; es.tam_bloque = tb; es.ficheros = n_fi; es.ocupado = vivos;
    es.libre = trozo_max ? disp / TOT(trozo_max) * trozo_max : 0;
    resto = trozo_max ? disp % TOT(trozo_max) : 0;
    if (resto > TOT(4)) es.libre += (resto - TOT(0)) & ~3u;
    if (!es.montado) es.ficheros = es.ocupado = es.libre = 0;
    if (e) *e = es;
}

/* ---- escritura: abrir la cabeza, grabar un registro, hacer sitio ------------------------------------------ */
/* Abre como cabeza el siguiente bloque libre del anillo. Siempre lo borra antes (un bloque que «parece»
 * borrado puede venir de un borrado cortado) y relee la cabecera. Si un bloque falla, prueba el siguiente. */
static int abrir_cabeza(void)
{
    u8 cab[12];
    u32 intento, b = n_ord ? CABEZA : nb - 1;
    p32(cab, MAGIA); p32(cab + 4, seq_cab + 1); p32(cab + 8, alm_crc32(0, cab, 8));
    for (intento = 0; intento < nb; intento++) {
        b = (b + 1) % nb;
        if (en_registro(b)) continue;
        paso(0, 0);                                      /* el borrado de un bloque es lo más largo que hay */
        if (!pt->borrar(b) && !pt->programar(b, 0, cab, 12) && seq_de(b) == seq_cab + 1) {
            ord[n_ord++] = (u8)b; seq_cab++; fin = CAB_BLQ;
            return USR_OK;
        }
        es.errores++;
    }
    return USR_E_FLASH;
}

/* Graba un registro en la cabeza, que ya tiene sitio (sitio() o la limpieza lo han mirado). Tres pasos:
 * la cabecera, la carga y —lo último, aparte— el sello; y se RELEE todo antes de darlo por bueno. */
static int grabar(u32 marca, u32 tx, u32 a, const u8 *d, u32 n, u32 crc, u32 *donde)
{
    u8 cab[CAB_REG], cola[4];
    u32 cc, p = fin, tot = TOT(n), hb = CABEZA;
    const u8 *m;
    p16(cab, marca); p16(cab + 2, n); p32(cab + 4, tx); p32(cab + 8, a); p32(cab + 12, crc);
    cc = alm_crc32(0, cab, 16); p32(cab + 16, cc); p32(cola, SELLO(cc));
    fin = p + tot;                                       /* pase lo que pase, ese sitio ya no se reutiliza */
    if (pt->programar(hb, p, cab, CAB_REG)) return USR_E_FLASH;
    if (pt->programar(hb, p + CAB_REG, d, n)) return USR_E_FLASH;
    if (pt->programar(hb, p + tot - 4, cola, 4)) return USR_E_FLASH;
    m = pt->leer(hb) + p;
    if (memcmp(m, cab, CAB_REG) || memcmp(m + CAB_REG, d, n) || le32(m + tot - 4) != SELLO(cc)) return USR_E_FLASH;
    if (donde) *donde = p;
    return USR_OK;
}

/* ¿Sigue haciendo falta este registro? Un trozo o un compromiso de un fichero vigente (o un trozo de la
 * escritura en curso), y que no tenga ya una copia más adelante. Los borrados, nunca (ver la cabecera). */
static int vivo(const Reg *r, u32 k)
{
    if (!r->valido || r->marca == M_BORRA) return 0;
    if (!por_tx(r->tx) && !(r->marca == M_TROZO && ab.activo && ab.tx == r->tx)) return 0;
    return !hay_despues(r, k, r->pos + r->tot);
}

/* (R3) ¿Hace falta copiar este BORRADO al limpiar el bloque ord[k]? Solo si en un bloque ANTERIOR queda un compromiso
 * válido de ese nombre (sin el borrado, al montar volvería a valer) y más adelante no hay ya otro compromiso o borrado
 * del mismo nombre (que es quien manda). En la cola (k = 0) no hay bloques anteriores: nunca, como siempre. */
static int borrado_hace_falta(const Reg *r, u32 k)
{
    char nom[USR_MAX_NOMBRE + 1], otro[USR_MAX_NOMBRE + 1];
    Reg q;
    u32 i, p;
    int antes = 0;
    if (!r->valido || r->marca != M_BORRA || !k || !nombre_de(r, 0, nom)) return 0;
    for (i = 0; i < k && !antes; i++)
        for (p = CAB_BLQ; reg_sig(ord[i], &p, tb, &q); )
            if (q.valido && q.marca == M_COMPR && nombre_de(&q, CM_FIJO - 1, otro) && !cmp_nombre(nom, otro)) { antes = 1; break; }
    if (!antes) return 0;
    for (i = k, p = r->pos + r->tot; i < n_ord; i++, p = CAB_BLQ)
        while (reg_sig(ord[i], &p, LIM(ord[i]), &q))
            if (q.valido && ((q.marca == M_COMPR && nombre_de(&q, CM_FIJO - 1, otro)) || (q.marca == M_BORRA && nombre_de(&q, 0, otro)))
                && !cmp_nombre(nom, otro)) return 0;
    return 1;
}
/* ¿Se copia este registro al limpiar el bloque ord[k]? */
static int se_copia(const Reg *r, u32 k) { return vivo(r, k) || borrado_hace_falta(r, k); }

/* (R3) EL PLAN de limpiar el bloque ord[k], sin tocar la flash: cuántos bytes hay que copiar (*bytes) y cuántos bloques
 * NUEVOS hay que abrir para que quepan, copiando en el mismo orden que limpiar_bloque (un registro no se parte). */
static u32 plan(u32 k, u32 *bytes)
{
    u32 t = ord[k], p = CAB_BLQ, pos = fin, abre = 0, suma = 0;
    Reg r;
    while (reg_sig(t, &p, tb, &r)) {
        if (!se_copia(&r, k)) continue;
        if (pos + r.tot > TOPE_REG) { abre++; pos = CAB_BLQ; }
        pos += r.tot; suma += r.tot;
    }
    if (bytes) *bytes = suma;
    return abre;
}
/* (R3) QUÉ BLOQUE SE LIMPIA (índice en ord; nunca la cabeza). Con la reserva entera, la cola. Con la reserva tocada, el
 * que menos cuesta: menos bloques que abrir y, a igualdad, menos bytes que copiar (el más viejo si empatan). */
static u32 victima(u32 *abre, u32 *bytes)
{
    u32 k, mejor = 0, a, b;
    *abre = plan(0, bytes);
    if (libres() >= RES_BLQ) return 0;
    for (k = 1; k + 1 < n_ord; k++) {
        a = plan(k, &b);
        if (a < *abre || (a == *abre && b < *bytes)) { mejor = k; *abre = a; *bytes = b; }
    }
    return mejor;
}

/* LIMPIA UN BLOQUE (la cola, salvo con la reserva tocada: ver victima): copia a la cabeza lo vivo y lo retira. El
 * orden es la garantía: el bloque no se toca hasta que TODO lo suyo que hace falta está más adelante, entero y
 * releído; un corte en cualquier punto lo deja o intacto (con alguna copia de más, idéntica) o retirado. */
static int limpiar_cola(void)
{
    static const u8 cero[4] = { 0, 0, 0, 0 };
    u32 k, t, p = CAB_BLQ, donde, i, abre, total, hecho = 0;
    Reg r;
    int e;
    if (n_ord < 2) return USR_E_LLENO;                   /* la cabeza no se limpia a sí misma */
    k = victima(&abre, &total);
    if (abre > libres()) return USR_E_LLENO;             /* (R3) no cabe: NO se empieza (la reserva no se gasta sin poder acabar) */
    t = ord[k];
    while (reg_sig(t, &p, tb, &r)) {
        if (!se_copia(&r, k)) continue;
        for (i = 0, e = USR_E_FLASH; i < 2 && e; i++) {  /* si un sitio falla, uno más allá */
            if (fin + r.tot > TOPE_REG) {                /* no cabe en lo que queda de la cabeza: otra (para esto es la reserva) */
                if (!libres()) return USR_E_LLENO;
                if ((e = abrir_cabeza())) return e;
            }
            e = grabar(r.marca, r.tx, r.a, r.p + CAB_REG, r.n, r.crc, &donde);
        }
        if (e) return e;
        hecho += r.tot; paso(hecho, total);
        if (r.marca == M_TROZO && r.a == 0) {            /* el primer trozo de alguien ha cambiado de sitio */
            Fich *f = por_tx(r.tx);
            if (f) { f->blq0 = CABEZA; f->pos0 = donde; }
            else if (ab.activo && ab.tx == r.tx) { ab.blq0 = CABEZA; ab.pos0 = donde; }
        }
    }
    if (pt->programar(t, 12, cero, 4) && seq_de(t)) return USR_E_FLASH;   /* no se pudo retirar: sigue en el registro, coherente */
    for (n_ord--, i = k; i < n_ord; i++) ord[i] = ord[i + 1];
    es.limpiezas++;
    return USR_OK;
}

/* Deja la cabeza con sitio para un registro de `tot` bytes. Una escritura normal solo abre un bloque si
 * después siguen quedando RES_BLQ libres; si no, limpia la cola (que es quien puede gastar la reserva).
 * La admisión de usr_empezar garantiza que esto acaba antes de dar una vuelta entera al anillo. */
static int sitio(u32 tot)
{
    u32 v;
    int e;
    for (v = 0; v < 3 * nb + 3; v++) {
        if (n_ord && fin + tot <= TOPE_REG) return USR_OK;
        if (libres() > RES_BLQ || (n_ord < 2 && libres())) e = abrir_cabeza();
        else if (n_ord >= 2) e = limpiar_cola();
        else break;
        if (e) return e;
    }
    return USR_E_LLENO;
}
/* sitio + grabar, con un segundo intento más allá si el primero falla (en un corte fallan los dos, y deprisa) */
static int anadir(u32 marca, u32 tx, u32 a, const u8 *d, u32 n, u32 *donde)
{
    u32 crc = alm_crc32(0, d, n), i;
    int e = USR_E_FLASH;
    for (i = 0; i < 2 && e; i++) {
        e = sitio(TOT(n));
        /* (R3) BORRAR CABE SIEMPRE: si no hay sitio corriente ni forma de hacerlo, el borrado usa la reserva del final
         * de la cabeza, que nadie más puede gastar */
        if (e == USR_E_LLENO && marca == M_BORRA && n_ord && fin + TOT(n) <= tb) e = USR_OK;
        if (e) break;
        e = grabar(marca, tx, a, d, n, crc, donde);
        if (!e) paso(0, 0);
    }
    return e;
}
/* Tras una escritura: repone la reserva de bloques libres si la limpieza la gastó. */
static void equilibrar(void)
{
    u32 v, antes;
    int e;
    for (v = 0; v < nb && n_ord >= 2 && (antes = libres()) < RES_BLQ; v++) {
        if ((e = limpiar_cola()) == USR_E_LLENO) break;  /* (R3) no cabe ninguna limpieza: no es un error, se queda como está */
        if (e) { es.errores++; indexar(); break; }
        if (libres() <= antes) break;                    /* (R3) esa limpieza no ha ganado un bloque: insistir sería copiar sin progreso */
    }
}
/* Algo falló a media operación: la RAM se rehace desde la flash, que es la que manda. */
static int fallo(int e)
{
    ab.activo = 0;
    if (e == USR_E_FLASH) es.errores++;
    if (indexar() != USR_OK) es.montado = 0;
    return e;
}

/* ---- la API de escritura --------------------------------------------------------------------------------- */
static u32 coste_fichero(u32 tam, u32 len)
{
    return tam / trozo_max * TOT(trozo_max) + (tam % trozo_max ? TOT(tam % trozo_max) : 0) + TOT(CM_FIJO + len);
}
int usr_empezar(const char *nombre, u32 tam, u32 fecha)
{
    u32 len = nombre_len(nombre), i;
    if (!es.montado) return USR_E_SIN_MONTAR;
    if (!len || ab.activo) return USR_E_ARG;
    if (es.solo_lectura) return USR_E_FLASH;
    if (buscar(nombre) < 0 && n_fi >= USR_MAX_FICHEROS) return USR_E_MUCHOS;
    /* LLENO se dice aquí, antes de tocar nada. El fichero viejo sigue contando: hasta el compromiso existen los dos. */
    if (tam > limite || coste_fichero(tam, len) + TOT_BR_MAX > limite - (vivos < limite ? vivos : limite)) return USR_E_LLENO;
    memset(&ab, 0, sizeof ab);
    for (i = 0; i < len; i++) ab.nombre[i] = nombre[i];
    ab.tam = tam; ab.fecha = fecha; ab.tx = ++ult_tx; ab.activo = 1;
    return USR_OK;
}
static int trozo(const u8 *d, u32 n)
{
    u32 donde = 0;
    int e = anadir(M_TROZO, ab.tx, ab.escrito, d, n, &donde);
    if (e) return e;
    if (!ab.escrito) { ab.blq0 = CABEZA; ab.pos0 = donde; }
    ab.escrito += n; ab.coste += TOT(n);
    return USR_OK;
}
int usr_mas(const void *datos, u32 n)
{
    const u8 *d = datos;
    int e = USR_OK;
    if (!es.montado) return USR_E_SIN_MONTAR;
    if (!ab.activo || (n && !d)) return USR_E_ARG;
    if (n > ab.tam - ab.escrito - ab.en_buf) { usr_cancelar(); return USR_E_ARG; }   /* más de lo anunciado */
    ab.crc = alm_crc32(ab.crc, d, n);
    while (n && !e) {
        u32 k;
        if (!ab.en_buf && n >= trozo_max) { e = trozo(d, trozo_max); d += trozo_max; n -= trozo_max; continue; }   /* entero: sin copiar */
        k = trozo_max - ab.en_buf < n ? trozo_max - ab.en_buf : n;
        memcpy(buf + ab.en_buf, d, k); ab.en_buf += k; d += k; n -= k;
        if (ab.en_buf == trozo_max) { e = trozo(buf, trozo_max); ab.en_buf = 0; }
    }
    return e ? fallo(e) : USR_OK;
}
int usr_acabar(void)
{
    u8 carga[CM_FIJO + USR_MAX_NOMBRE];
    u32 len, i;
    Fich *f;
    int e = USR_OK;
    if (!es.montado) return USR_E_SIN_MONTAR;
    if (!ab.activo) return USR_E_ARG;
    if (ab.en_buf) { e = trozo(buf, ab.en_buf); ab.en_buf = 0; if (e) return fallo(e); }
    if (ab.escrito != ab.tam) { usr_cancelar(); return USR_E_ARG; }                   /* menos de lo anunciado */
    len = nombre_len(ab.nombre);
    p32(carga, ab.fecha); p32(carga + 4, ab.crc); carga[8] = (u8)len;
    for (i = 0; i < len; i++) carga[CM_FIJO + i] = (u8)ab.nombre[i];
    /* EL COMPROMISO. Hasta que su sello está escrito vale el fichero de antes; desde entonces, este. */
    if ((e = anadir(M_COMPR, ab.tx, ab.tam, carga, CM_FIJO + len, 0))) return fallo(e);
    if (!(f = poner(ab.nombre))) return fallo(USR_E_MUCHOS);                          /* no puede pasar: se miró en usr_empezar */
    vivos -= f->coste;                                                               /* 0 si es nuevo */
    f->tam = ab.tam; f->fecha = ab.fecha; f->tx = ab.tx; f->crc = ab.crc;
    f->coste = ab.coste + TOT(CM_FIJO + len); f->blq0 = ab.blq0; f->pos0 = ab.pos0;
    vivos += f->coste;
    ab.activo = 0;
    equilibrar();
    return USR_OK;
}
void usr_cancelar(void) { ab.activo = 0; }               /* sus trozos se quedan sin compromiso: no existen */

int usr_escribir(const char *nombre, const void *d, u32 n, u32 fecha)
{
    int e, i;
    if (!es.montado) return USR_E_SIN_MONTAR;
    if (!nombre_len(nombre) || (n && !d)) return USR_E_ARG;
    /* si ya vale exactamente eso (y cabe en un trozo, que es lo que se puede mirar sin leerlo entero), no se gasta flash */
    if ((i = buscar(nombre)) >= 0 && fi[i].tam == n && n <= trozo_max && !ab.activo && !es.solo_lectura) {
        Reg r; u32 p = fi[i].pos0;
        if (!n || (p && reg_sig(fi[i].blq0, &p, tb, &r) && r.valido && r.marca == M_TROZO && r.tx == fi[i].tx && r.a == 0
                   && r.n == n && !memcmp(r.p + CAB_REG, d, n))) return USR_OK;
    }
    if ((e = usr_empezar(nombre, n, fecha))) return e;
    if ((e = usr_mas(d, n))) return e;
    return usr_acabar();
}
int usr_borrar(const char *nombre)
{
    u8 carga[1 + USR_MAX_NOMBRE];
    u32 len = nombre_len(nombre), i;
    int k, e;
    if (!es.montado) return USR_E_SIN_MONTAR;
    if (!len) return USR_E_ARG;
    if ((k = buscar(nombre)) < 0) return USR_E_NO_HAY;
    if (es.solo_lectura) return USR_E_FLASH;
    carga[0] = (u8)len;
    for (i = 0; i < len; i++) carga[1 + i] = (u8)nombre[i];
    if ((e = anadir(M_BORRA, ++ult_tx, 0, carga, 1 + len, 0))) return fallo(e);
    if ((k = buscar(nombre)) >= 0) { vivos -= fi[k].coste; quitar(nombre); }          /* (la limpieza no cambia los índices, pero por si acaso) */
    equilibrar();
    return USR_OK;
}

/* ---- montaje -------------------------------------------------------------------------------------------- */
int usr_montar(const AlmPuerto *p, int puede_escribir)
{
    u32 maxreg;
    int e;
    pt = p; n_ord = 0; n_fi = 0; vivos = 0; fin = 0; seq_cab = 0; ult_tx = 0; ab.activo = 0;
    es = (UsrEstado){ .bloques = p ? p->bloques : 0, .tam_bloque = p ? p->tam_bloque : 0, .solo_lectura = (u8)!puede_escribir };
    if (!p || p->bloques < RES_BLQ + 1 || p->bloques > MAX_BLQ || p->tam_bloque < 1024 || (p->tam_bloque & 3)) return USR_E_ARG;
    nb = p->bloques; tb = p->tam_bloque;
    trozo_max = ((tb - CAB_BLQ) / 8) & ~3u;
    if (trozo_max > TROZO_TOPE) trozo_max = TROZO_TOPE;
    maxreg = TOT(trozo_max) > TOT_CM_MAX ? TOT(trozo_max) : TOT_CM_MAX;
    res_br = (tb - CAB_BLQ) / 8u / TOT_BR_MAX;          /* (R3) sitio para 8 borrados por bloque; en las geometrías chicas de las pruebas, */
    res_br = TOT_BR_MAX * (res_br > 8u ? 8u : res_br ? res_br : 1u);   /* los que quepan en 1/8 del bloque (al menos uno) */
    limite = (nb - RES_BLQ) * (tb - CAB_BLQ - maxreg - res_br);
    e = indexar();
    if (e == USR_E_NO_HAY) {                             /* no hay volumen: darle formato (un bloque abierto con la secuencia 1) */
        if (!puede_escribir) return USR_E_NO_HAY;
        n_ord = 0; seq_cab = 0;
        if ((e = abrir_cabeza())) { es.errores++; return e; }
        es.formateos++;
        e = indexar();
    }
    if (e) return e;
    es.montado = 1;
    return USR_OK;
}
