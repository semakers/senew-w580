/* w6f.c — W6F: sistema de ficheros de registro sobre páginas de escritura única. Ver w6f.h y docs/fs_propio.md.
 *
 * RESUMEN DEL FORMATO (todo en little-endian; «carga» = pag - 28):
 *   Cada página empieza con una cabecera de 28 bytes:
 *      0 u16 magia 0x3657   2 u8 tipo   3 u8 versión   4 u16 id   6 u16 marca 0x4F6B
 *      8 u32 gen   12 u32 idx   16 u32 seq   20 u32 dcrc (CRC32 de la carga útil)   24 u16 lote   26 u16 hcrc (CRC16 de 0..25)
 *   lote: 0 = página normal; 1 = copia de la recolección; 3 = copia que además COMPROMETE el lote (ver 'G').
 *   Tipos:
 *     'B' página 0 de cada bloque: gen = borrados del bloque; carga = pag, ppb, n_bloques, clase (u32 cada uno).
 *     'D' datos: id del fichero, gen del contenido, idx = n.º de página dentro del fichero, carga entera.
 *     'N' nodo (el COMPROMISO de un fichero o directorio): id, gen; carga = padre u16, clase u8 (1 fichero, 2 dir),
 *         nombre_len u8, tam u32, fecha u32, mata u16, cola_len u16, cola_aparte u8, 3×0, nombre, cola en línea.
 *     'C' punto de control del rango de ids «id»: gen = entradas; carga = por cada id del rango, la seq de su nodo
 *         vivo (0 = no existe). Un nodo de ese rango con seq menor que la del punto de control está MUERTO si su
 *         id figura con 0 o con una seq mayor que la suya: así se borra sin lápidas.
 *     'F' un formato empezó y no acabó.
 *     'G' compromiso de un lote de recolección (cuando la última copia no pudo llevarlo).
 *   Las copias de la recolección de un bloque van SEGUIDAS a un bloque recién borrado; si al final de ese bloque
 *   quedan copias sin compromiso detrás, no valen (la recolección se cortó) y el bloque se vuelve a borrar.
 *   El estado de un objeto es su nodo de mayor seq que pase la regla del punto de control; su contenido, las
 *   páginas 'D' de su (id, gen) —para cada idx, la de mayor seq con CRC bueno— más la cola del nodo.
 */
#include <string.h>
#include "w6f.h"

#ifndef W6F_MUTANTE
#define W6F_MUTANTE 0                  /* fs2/pruebas/mutantes.sh: cada número estropea una cosa y las pruebas tienen que cazarlo */
#endif
#define MUT(n) (W6F_MUTANTE == (n))
#ifdef W6F_DEPURA
#include <stdio.h>
int w6f_traza;
#define TRAZA(...) do { if (w6f_traza) fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define TRAZA(...) do { } while (0)
#endif

#define P_NIL    ((w6f_pag)W6F_P_MAX)          /* fin de cadena (la página es de datos, viva, la última) */
#define P_MUERTA ((w6f_pag)(W6F_P_MAX - 1))    /* no viva: blanca, basura, superada o cabecera de bloque */
#define P_META   ((w6f_pag)(W6F_P_MAX - 2))    /* nodo o punto de control vivo */
#define P_TD     ((w6f_pag)(W6F_P_MAX - 3))    /* (montaje) candidata de datos */
#define P_TN     ((w6f_pag)(W6F_P_MAX - 4))    /* (montaje) candidata de nodo */
#define P_TDG    ((w6f_pag)(W6F_P_MAX - 5))    /* (montaje) copias de un lote todavía sin compromiso: datos, nodo, punto de control */
#define P_TNG    ((w6f_pag)(W6F_P_MAX - 6))
#define P_TCG    ((w6f_pag)(W6F_P_MAX - 7))
#define P_TOPE   (W6F_P_MAX - 8u)

enum { B_LIBRE = 0, B_USADO, B_CABEZA, B_SUCIO, B_MALO, B_DESTINO };
enum { M_DIR = 1, M_DANADO = 2 /* le faltan páginas de datos: se lee lo que hay */, M_RESERVA = 4, M_ESCR = 8, M_CONDENADO = 16, M_CP_PEND = 32,
       M_NODO_MALO = 64 /* su nodo no pasa el CRC (o llegó copiado como dañado): no se lee ni se le cree nada */ };
#define LOTE_DANADA 0x8000u            /* (en el campo lote) la recolección copió esta página sabiendo que su original ya no pasaba el CRC */
#define SEQ_TOPE   0xFF000000u         /* a partir de aquí no se escribe: solo lectura. Quedan 16 M de secuencias hasta la nula:
                                          ninguna operación empezada por debajo puede llegar (se mira además en cada página) */
#define SEQ_NULA   0xFFFFFFF0u         /* una página normal con una secuencia así no es válida */
#define NINGUNO   0xFFFFFFFFu
#define SIN_MIRAR 0x40                 /* (en blq.fallos) bloque heredado del montaje cuyo blanco aún no se ha comprobado página a página */
#define SIN_CUENTA 0xFFFFFFFFu
#define MAGIA     0x3657u
#define MARCA     0x4F6Bu
#define NODO_FIJO 20u

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;

static u32 g16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 g32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void p16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void p32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static u32 crc16(const u8 *d, u32 n)                   /* CRC-16/CCITT-FALSE (binascii.crc_hqx(d, 0xFFFF)), por medios bytes */
{
    static const uint16_t t[16] = { 0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
                                    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF };
    u32 c = 0xFFFF, i;
    for (i = 0; i < n; i++) {
        c = ((c << 4) ^ t[((c >> 12) ^ (d[i] >> 4)) & 15]) & 0xFFFF;
        c = ((c << 4) ^ t[((c >> 12) ^ d[i]) & 15]) & 0xFFFF;
    }
    return c;
}
static u32 crc32_mas(u32 c, const u8 *d, u32 n)        /* el de zlib, por medios bytes; empezar con 0 */
{
    static const u32 t[16] = { 0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
                               0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C };
    u32 i;
    c = ~c;
    for (i = 0; i < n; i++) { c ^= d[i]; c = t[c & 15] ^ (c >> 4); c = t[c & 15] ^ (c >> 4); }
    return ~c;
}

/* ---- cabecera de página ------------------------------------------------------------------------------ */
typedef struct { u8 tipo, danada; u16 id, lote; u32 gen, idx, seq, dcrc; } Cab;
static int cab_leer(const u8 *b, Cab *c)               /* 1 = válida; 2 = en blanco; 0 = basura; 3 = de OTRA versión del formato */
{
    u32 i;
    for (i = 0; i < W6F_CAB && b[i] == 0xFF; i++) { }
    if (i == W6F_CAB) return 2;
    if (g16(b) != MAGIA || g16(b + 6) != MARCA) return 0;
    if (!MUT(9) && g16(b + 26) != crc16(b, 26)) return 0;
    if (b[3] != W6F_VERSION) return 3;                 /* (N6) otra versión del formato, anterior o posterior: ni basura ni nuestro */
    c->lote = (u16)(g16(b + 24) & ~LOTE_DANADA); c->danada = (g16(b + 24) & LOTE_DANADA) != 0;
    if (c->lote != 0 && c->lote != 1 && c->lote != 3) return 0;
    c->tipo = b[2]; c->id = (u16)g16(b + 4); c->gen = g32(b + 8); c->idx = g32(b + 12); c->seq = g32(b + 16); c->dcrc = g32(b + 20);
    if (c->tipo == 'B' || c->tipo == 'F') return 1;
    if (c->seq >= SEQ_NULA) return 0;                  /* (M3) ninguna página nuestra llega ahí: antes se deja de escribir */
    return c->tipo == 'D' || c->tipo == 'N' || c->tipo == 'C' || c->tipo == 'G';
}
static void cab_poner_l(u8 *b, int tipo, u32 id, u32 gen, u32 idx, u32 seq, u32 dcrc, u32 lote)
{
    p16(b, MAGIA); b[2] = (u8)tipo; b[3] = W6F_VERSION; p16(b + 4, id); p16(b + 6, MARCA);
    p32(b + 8, gen); p32(b + 12, idx); p32(b + 16, seq); p32(b + 20, dcrc); p16(b + 24, lote); p16(b + 26, crc16(b, 26));
}
static void cab_poner(u8 *b, int tipo, u32 id, u32 gen, u32 idx, u32 seq, u32 dcrc) { cab_poner_l(b, tipo, id, gen, idx, seq, dcrc, 0); }

/* ---- la flash ---------------------------------------------------------------------------------------- */
static int fl_leer(W6f *fs, u32 P, u32 off, void *buf, u32 n)
{
    fs->cta.montajes_lecturas++;
    if (P >= fs->n_pag || off > fs->fl->pag || n > fs->fl->pag - off) { fs->cta.incoherencias++; return -1; }   /* (M5) jamás se le pide al puerto algo de fuera */
    return fs->fl->leer(fs->fl->ctx, P / fs->fl->ppb, P % fs->fl->ppb, off, buf, n);
}
static int blanca(W6f *fs, u32 P)                      /* ¿toda la página a 0xFF? */
{
    u8 t[64]; u32 o, i, n;
    for (o = 0; o < fs->fl->pag; o += n) {
        n = fs->fl->pag - o; if (n > sizeof t) n = sizeof t;
        if (fl_leer(fs, P, o, t, n)) return 0;
        for (i = 0; i < n; i++) if (t[i] != 0xFF) return 0;
    }
    return 1;
}
static int coincide1(W6f *fs, u32 P, const u8 *con)
{
    u8 t[64]; u32 o, n;
    for (o = 0; o < fs->fl->pag; o += n) {
        n = fs->fl->pag - o; if (n > sizeof t) n = sizeof t;
        if (fl_leer(fs, P, o, t, n) || memcmp(t, con + o, n)) return 0;
    }
    return 1;
}
/* ¿la página quedó igual que fs->pg? Se lee DOS veces: (M7) una celda a medio programar puede leerse bien una vez. */
static int coincide_con(W6f *fs, u32 P, const u8 *con) { return coincide1(fs, P, con) && coincide1(fs, P, con); }
static int coincide(W6f *fs, u32 P) { return coincide_con(fs, P, fs->pg); }
static int crc_carga(W6f *fs, u32 P, u32 n, u32 *crc)  /* CRC32 de los primeros «n» bytes de la carga de P */
{
    u8 t[128]; u32 o, k, c = 0;
    for (o = 0; o < n; o += k) {
        k = n - o; if (k > sizeof t) k = sizeof t;
        if (fl_leer(fs, P, W6F_CAB + o, t, k)) return -1;
        c = crc32_mas(c, t, k);
    }
    *crc = c;
    return 0;
}
static int dato_bueno1(W6f *fs, u32 P)
{
    u8 h[W6F_CAB]; Cab c; u32 crc;
    if (fl_leer(fs, P, 0, h, W6F_CAB) || cab_leer(h, &c) != 1 || c.tipo != 'D' || c.danada) return 0;
    if (crc_carga(fs, P, fs->carga, &crc)) return 0;
    return crc == c.dcrc;
}
/* ¿La página de datos P pasa su CRC (y no llegó copiada como dañada)? (N7) Si sale mal se relee conf.relecturas veces:
 * una lectura mala puede ser pasajera. */
static int dato_bueno(W6f *fs, u32 P)
{
    u32 i, veces = MUT(24) ? 0 : fs->conf.relecturas ? fs->conf.relecturas : 2;
    for (i = 0; i <= veces; i++) if (dato_bueno1(fs, P)) return 1;
    return 0;
}

/* Páginas blancas que se pueden gastar. Para el usuario, libres_min bloques libres no cuentan nunca: son el destino de
 * la recolección y su repuesto. Lo URGENTE (el punto de control de un borrado) sí puede tirar de ellos. */
static u32 disponibles(const W6f *fs)
{
    u32 ppb = fs->fl->ppb, quedan = fs->urgente ? 0 : fs->libres_min, n = fs->libres > quedan ? fs->libres - quedan : 0;
    return (fs->cabeza != NINGUNO ? ppb - fs->llenado : 0) + (fs->parcial != NINGUNO ? ppb - fs->parcial_llenado : 0) +
           n * (ppb - 1);
}
static void matar(W6f *fs, u32 P)
{
    if (fs->sig[P] != P_MUERTA) { fs->sig[P] = P_MUERTA; fs->blq[P / fs->fl->ppb].vivas--; }
}
static void vivir(W6f *fs, u32 P, w6f_pag v)
{
    if (fs->sig[P] == P_MUERTA) fs->blq[P / fs->fl->ppb].vivas++;
    fs->sig[P] = v;
}
static void calcular_cap(W6f *fs)
{
    u32 total = fs->utiles * (fs->fl->ppb - 1), quita = fs->conf.reserva_bloques * (fs->fl->ppb - 1) + fs->n_rangos + 2;
    fs->cap = total > quita ? total - quita : 0;
}
static void malo(W6f *fs, u32 b)
{
    if (fs->blq[b].estado == B_MALO) return;
    if (fs->blq[b].estado == B_LIBRE) fs->libres--;
    fs->blq[b].estado = B_MALO; fs->utiles--;
    calcular_cap(fs);                                  /* la capacidad anunciada BAJA en un bloque */
    if (fs->fl->marcar_malo) fs->fl->marcar_malo(fs->fl->ctx, b);
}

static u32 un_libre(const W6f *fs, int el_mas_gastado)
{
    u32 b, mejor = NINGUNO;
    for (b = 0; b < fs->fl->n_bloques; b++) {
        if (fs->blq[b].estado != B_LIBRE) continue;
        if (mejor == NINGUNO || (el_mas_gastado ? fs->blq[b].borrados > fs->blq[mejor].borrados : fs->blq[b].borrados < fs->blq[mejor].borrados)) mejor = b;
    }
    return mejor;
}
/* La última página que NO está en blanco de «b» a partir de «desde» (0 = todas en blanco). Al montar solo se miran las
 * cabeceras; antes de escribir en un bloque heredado se comprueba entero, para no programar nunca detrás de restos. */
static u32 mirar(W6f *fs, u32 b, u32 desde)
{
    u32 k, ultima = 0;
    fs->blq[b].fallos &= (u8)~SIN_MIRAR;
    if (MUT(4)) return 0;
    for (k = desde; k < fs->fl->ppb; k++) if (!blanca(fs, b * fs->fl->ppb + k)) ultima = k;
    return ultima;
}
static int borrar_bloque(W6f *fs, u32 b);
/* Un bloque libre, listo para escribir desde su página 1 y ya descontado de los libres.
 * (M7) Un bloque libre HEREDADO del montaje pudo ser el que se estaba abriendo cuando se fue la corriente: su primera
 * página puede haberse quedado a medio programar aunque se lea en blanco. Por omisión se BORRA otra vez antes de usarlo;
 * con conf.reusar_blanco solo se comprueba que se lea en blanco entero. */
static u32 tomar_libre(W6f *fs, int el_mas_gastado, u32 *ini)
{
    u32 b;
    while ((b = un_libre(fs, el_mas_gastado)) != NINGUNO) {
        int heredado = (fs->blq[b].fallos & SIN_MIRAR) != 0;
        fs->libres--; *ini = 1;
        if (!heredado) return b;
        if (!fs->conf.reusar_blanco && !MUT(18)) {
            fs->blq[b].fallos &= (u8)~SIN_MIRAR; fs->blq[b].estado = B_SUCIO;
            if (borrar_bloque(fs, b)) continue;        /* (queda sucio o malo) */
            fs->libres--;
            return b;
        }
        if (!mirar(fs, b, 1)) return b;
        fs->blq[b].estado = B_SUCIO;                   /* decía estar libre y tiene restos: a borrar */
    }
    return NINGUNO;
}
static int nueva_cabeza(W6f *fs)
{
    u32 b, ini;
    if (fs->cabeza != NINGUNO) { fs->blq[fs->cabeza].estado = B_USADO; fs->cabeza = NINGUNO; }
    fs->fallos_seguidos = 0;
    if (fs->parcial != NINGUNO) { fs->cabeza = fs->parcial; fs->llenado = fs->parcial_llenado; fs->parcial = NINGUNO; return 0; }
    /* los últimos libres son de la recolección; solo lo urgente (borrar) puede abrir uno de ellos */
    if (fs->libres <= ((fs->urgente || MUT(110)) ? 0 : fs->libres_min) || (b = tomar_libre(fs, 0, &ini)) == NINGUNO) return W6F_E_LLENO;
    fs->blq[b].estado = B_CABEZA; fs->cabeza = b; fs->llenado = ini;
    return 0;
}

/* Programa fs->pg en la siguiente página blanca del registro. Antes COMPRUEBA que la página está en blanco entera
 * (jamás se programa sobre algo: restos, una página a medias de un corte…) y después la relee: vale si quedó
 * igual, diga lo que diga la flash. Una página que falla se da por muerta y se prueba en la siguiente. */
static int prog(W6f *fs, u32 *Pout)
{
    u32 intentos = 0;
    while (intentos < 8) {
        u32 P;
        /* si no queda sitio DESPUÉS de que haya fallado un programa, lo que ha fallado es la flash, no que esté lleno: se
         * dice así (W6F_E_FLASH: reintentar; la reserva no se toca) */
        if (fs->cabeza == NINGUNO || fs->llenado >= fs->fl->ppb) { int r = nueva_cabeza(fs); if (r) return intentos ? W6F_E_FLASH : r; }
        if (fs->blq[fs->cabeza].fallos & SIN_MIRAR) { u32 u = mirar(fs, fs->cabeza, fs->llenado); if (u) { fs->llenado = u + 1; continue; } }
        P = fs->cabeza * fs->fl->ppb + fs->llenado++;
        if (!MUT(4) && !blanca(fs, P)) continue;
        fs->fl->programar(fs->fl->ctx, P / fs->fl->ppb, P % fs->fl->ppb, fs->pg);
        fs->cta.programas++;
        if (MUT(109) || coincide(fs, P)) { fs->fallos_seguidos = 0; *Pout = P; return 0; }
        fs->cta.fallos_programa++; intentos++;
        if (++fs->fallos_seguidos >= 4) {              /* este bloque no quiere: se cierra y se sigue en otro */
            fs->blq[fs->cabeza].estado = B_USADO; fs->blq[fs->cabeza].fallos++; fs->cabeza = NINGUNO; fs->fallos_seguidos = 0;
        }
    }
    return W6F_E_FLASH;
}

/* ---- nodos ------------------------------------------------------------------------------------------- */
typedef struct { Cab c; u16 padre, mata, cola_len; u8 clase, nl, cola_aparte; u32 tam, fecha; } Nodo;

static int nodo_carga(const W6f *fs, const u8 *p, Nodo *n)     /* p = la parte fija de la carga (20 bytes); 0 = coherente */
{
    u32 r;
    n->padre = (u16)g16(p); n->clase = p[2]; n->nl = p[3]; n->tam = g32(p + 4); n->fecha = g32(p + 8);
    n->mata = (u16)g16(p + 12); n->cola_len = (u16)g16(p + 14); n->cola_aparte = p[16];
    if (n->nl == 0 || n->cola_aparte > 1 || n->mata == 0) return -1;
    if (NODO_FIJO + n->nl + n->cola_len > fs->carga) return -1;
    if (n->clase == 2) return (n->tam || n->cola_len || n->cola_aparte) ? -1 : 0;
    if (n->clase != 1) return -1;
    r = n->tam % fs->carga;
    if (n->cola_aparte) return (r == 0 || n->cola_len) ? -1 : 0;
    return (n->cola_len != r || r > fs->cola_max) ? -1 : 0;
}
/* h = cabecera + parte fija (48 bytes). 0 = bien; -1 = no es un nodo coherente; -2 = lo es, pero nombra un id que no
 * cabe en la tabla de ESTE montaje (max_obj): (A3) eso no es basura, es un volumen hecho con más objetos. */
static int nodo_campos(const W6f *fs, const u8 *h, Nodo *n)
{
    if (cab_leer(h, &n->c) != 1 || n->c.tipo != 'N') return -1;
    if (nodo_carga(fs, h + W6F_CAB, n) || n->c.id == 0 || n->padre == n->c.id) return -1;
    if (n->c.id >= fs->conf.max_obj || n->padre >= fs->conf.max_obj || (n->mata != 0xFFFF && n->mata >= fs->conf.max_obj)) return -2;
    return 0;
}
static int nodo_leer(W6f *fs, u32 P, Nodo *n)
{
    u8 h[W6F_CAB + NODO_FIJO]; u32 i, veces = MUT(24) || !fs->montado ? 0 : fs->conf.relecturas ? fs->conf.relecturas : 2; int r = -1;
    for (i = 0; i <= veces && r == -1; i++)            /* (N7) en marcha, una lectura que sale mal se relee (al montar no: allí decide el CRC) */
        r = fl_leer(fs, P, 0, h, sizeof h) ? -1 : nodo_campos(fs, h, n);
    return r;
}
/* (M1) ¿el nodo P pasa su CRC ENTERO ahora mismo? Lo que se saque de un nodo para escribirlo en otro (renombrar,
 * truncar, añadir, recolectar) o para dárselo al usuario (la cola en línea) pasa antes por aquí. */
static int nodo_bueno(W6f *fs, u32 P)
{
    Nodo n; u32 crc, i, veces = MUT(24) ? 0 : fs->conf.relecturas ? fs->conf.relecturas : 2;
    if (P == fs->nodo_ok) return 1;
    for (i = 0; i <= veces; i++) {                     /* (N7) una lectura mala puede ser pasajera: se relee antes de creérsela */
        if (nodo_leer(fs, P, &n) || n.c.danada) continue;
        if (crc_carga(fs, P, NODO_FIJO + n.nl + n.cola_len, &crc) || crc != n.c.dcrc) continue;
        fs->nodo_ok = (w6f_pag)P;
        return 1;
    }
    return 0;
}
static u32 n_datos(const W6f *fs, const Nodo *n) { return n->tam / fs->carga + n->cola_aparte; }

static int nombre_valido(const u8 *s, u32 n)
{
    u32 i = 0;
    if (n == 0 || n > W6F_NOMBRE_MAX) return 0;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
    while (i < n) {
        u32 c = s[i], k, cp, j;
        if (c < 0x20 || c == 0x7F || c == '/') return 0;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { k = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; }
        else return 0;
        if (i + k >= n + 0u && i + k > n - 1) return 0;
        for (j = 1; j <= k; j++) { if ((s[i + j] & 0xC0) != 0x80) return 0; cp = (cp << 6) | (s[i + j] & 0x3F); }
        if ((k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += k + 1;
    }
    return 1;
}
static u8 hash8(const u8 *s, u32 n)
{
    u32 h = 2166136261u, i;
    for (i = 0; i < n; i++) h = (h ^ s[i]) * 16777619u;
    return (u8)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
}
static int vivo(const W6f *fs, u32 id) { return id && id < fs->conf.max_obj && fs->obj[id].nodo != P_NIL; }
static int es_dir(const W6f *fs, u32 id) { return id == 0 || (vivo(fs, id) && (fs->obj[id].marcas & M_DIR)); }

/* Termina el nodo que hay armado en fs->pg (el nombre y la cola ya están en su sitio). Devuelve su seq. */
static u32 nodo_armar(W6f *fs, u32 id, u32 gen, u32 padre, int clase, u32 nl, u32 tam, u32 fecha, u32 mata,
                      u32 cola_len, int cola_aparte, u32 lote)
{
    u8 *p = fs->pg + W6F_CAB; u32 usado = NODO_FIJO + nl + cola_len, seq = fs->seq++;
    p16(p, padre); p[2] = (u8)clase; p[3] = (u8)nl; p32(p + 4, tam); p32(p + 8, fecha); p16(p + 12, mata);
    p16(p + 14, cola_len); p[16] = (u8)cola_aparte; p[17] = p[18] = p[19] = 0;
    memset(p + usado, 0xFF, fs->carga - usado);
    cab_poner_l(fs->pg, 'N', id, gen, 0, seq, crc32_mas(0, p, usado), lote);
    return seq;
}
static int nodo_emitir(W6f *fs, u32 id, u32 gen, u32 padre, int clase, u32 nl, u32 tam, u32 fecha, u32 mata,
                       u32 cola_len, int cola_aparte, u32 *Pout, u32 *seq_out)
{
    *seq_out = nodo_armar(fs, id, gen, padre, clase, nl, tam, fecha, mata, cola_len, cola_aparte, 0);
    return prog(fs, Pout);
}

/* Punto de control del rango «r», tal como está la tabla AHORA (quien borra quita antes el objeto de la tabla). */
static void cp_armar(W6f *fs, u32 r, u32 lote)
{
    u8 *p = fs->pg + W6F_CAB; u32 i, base = r * fs->por_rango;
    memset(p, 0, fs->por_rango * 4); memset(p + fs->por_rango * 4, 0xFF, fs->carga - fs->por_rango * 4);
    for (i = 0; i < fs->por_rango && base + i < fs->conf.max_obj; i++)
        if (vivo(fs, base + i)) p32(p + 4 * i, fs->obj[base + i].seq);
    cab_poner_l(fs->pg, 'C', r, fs->por_rango, 0, fs->seq++, crc32_mas(0, p, fs->por_rango * 4), lote);
}
static int cp_emitir(W6f *fs, u32 r)
{
    u32 P; int e;
    cp_armar(fs, r, 0);
    if ((e = prog(fs, &P))) return e;
    if (fs->cp[r] != P_NIL) matar(fs, fs->cp[r]);
    fs->cp[r] = (w6f_pag)P; vivir(fs, P, P_META);
    return 0;
}

/* ---- recolección ------------------------------------------------------------------------------------- *
 * Vaciar un bloque (la víctima) = copiar sus páginas vivas, SEGUIDAS y marcadas como lote, a un destino; la última
 * copia lleva el compromiso (o lo lleva una página 'G'). Hasta el compromiso la RAM no cambia nada.
 *  - Lo normal: el destino es un bloque recién borrado y exclusivo. Si algo falla (o se va la corriente) el destino no
 *    tiene nada vivo y se borra sin necesitar sitio, y la víctima sigue entera: repetir no consume nada.
 *  - Cuando FALTA un bloque libre de la reserva (murió uno al borrarlo): el destino es el sitio que le quede a la
 *    cabeza, y la víctima se puede vaciar en VARIOS lotes; así dos bloques a medias acaban siendo uno lleno y uno libre.
 *    Mientras quede un bloque libre, lo que un corte queme ahí se recupera por el camino normal.
 * (C1) Lo que se copia se COMPRUEBA: el CRC de cada copia se calcula de lo que de verdad se escribe; si no casa con el
 * del original, la copia va marcada como DAÑADA (leerla da error; el lote se compromete igual; nadie más lo paga).
 * Y antes de dar el lote por bueno se RELEE entero: la víctima no se toca hasta entonces. */
static void quitar(W6f *fs, u32 id);
static W6fEscr *escritor_de(W6f *fs, u32 id, u32 gen)
{
    u32 i;
    for (i = 0; i < W6F_MAX_ESCR; i++) if (fs->escr[i].activo && fs->escr[i].id == id && fs->escr[i].gen == gen) return &fs->escr[i];
    return 0;
}
static u32 idx_de(W6f *fs, u32 P) { u8 t[4]; return fl_leer(fs, P, 12, t, 4) ? NINGUNO : g32(t); }
/* ¿la página P, cuya cabecera es «c», pasa el CRC de su carga? */
static int entera(W6f *fs, u32 P, const Cab *c)
{
    u32 crc, n = 0;
    if (c->tipo == 'G') return 1;
    if (c->tipo == 'D') n = fs->carga;
    else if (c->tipo == 'C') n = fs->por_rango * 4;
    else if (c->tipo == 'N') { u8 f[NODO_FIJO]; Nodo nn; if (fl_leer(fs, P, W6F_CAB, f, NODO_FIJO) || nodo_carga(fs, f, &nn)) return 0; n = NODO_FIJO + nn.nl + nn.cola_len; }
    if (!n || crc_carga(fs, P, n, &crc)) return 0;
    return crc == c->dcrc;
}
static void cp_pendiente(W6f *fs, u32 r) { fs->pend_cp |= 1u << (r < 31 ? r : 31); }
static void cp_pendientes(W6f *fs)                     /* reescribe los puntos de control que se deben; lo que no salga, queda debido */
{
    u32 r, debe = fs->pend_cp; u8 urg = fs->urgente;
    if (fs->solo_lectura) return;
    fs->pend_cp = 0; fs->urgente = 1;
    for (r = 0; r < fs->n_rangos; r++) {
        if (!(debe & (1u << (r < 31 ? r : 31)))) continue;
        if (cp_emitir(fs, r)) cp_pendiente(fs, r);
    }
    fs->urgente = urg;
}
static int prog_destino(W6f *fs, u32 *Pout)
{
    while (fs->dest_llenado < fs->fl->ppb) {
        u32 P = fs->destino * fs->fl->ppb + fs->dest_llenado++;
        if (!MUT(4) && !blanca(fs, P)) continue;
        fs->fl->programar(fs->fl->ctx, P / fs->fl->ppb, P % fs->fl->ppb, fs->pg);
        fs->cta.programas++;
        if (coincide(fs, P)) { *Pout = P; return 0; }
        fs->cta.fallos_programa++;
    }
    return W6F_E_FLASH;
}
/* ¿De quién es la página viva P? Cuando su cabecera ya no lo dice (se pudrió después de montar), lo dice la RAM.
 * Devuelve 'C', 'N', 'D' (con su id; y para 'D' la generación y el índice) o 0. */
static int duenio(W6f *fs, u32 P, u32 *id, u32 *gen, u32 *idx)
{
    u32 i, k, pasos = 0; Nodo n; w6f_pag p;
    for (i = 0; i < fs->n_rangos; i++) if (fs->cp[i] == P) { *id = i; return 'C'; }
    for (i = 1; i < fs->conf.max_obj; i++) if (fs->obj[i].nodo == P) { *id = i; return 'N'; }
    for (i = 1; i < fs->conf.max_obj; i++) {
        if (fs->obj[i].nodo == P_NIL || (fs->obj[i].marcas & (M_DIR | M_DANADO))) continue;
        for (p = fs->obj[i].primera, k = 0; p != P_NIL && p < fs->n_pag && pasos++ <= fs->n_pag; p = fs->sig[p], k++)
            if (p == P) { *id = i; *idx = k; *gen = nodo_leer(fs, fs->obj[i].nodo, &n) ? 0 : n.c.gen; return 'D'; }
    }
    for (i = 0; i < W6F_MAX_ESCR; i++) {
        const W6fEscr *e = &fs->escr[i];
        if (!e->activo || e->modo != W6F_CREAR) continue;
        for (p = e->primera, k = 0; p != P_NIL && p < fs->n_pag && pasos++ <= fs->n_pag; p = fs->sig[p], k++)
            if (p == P) { *id = e->id; *idx = k; *gen = e->gen; return 'D'; }
    }
    return 0;
}
#define E_LECTURA (-100)                               /* (interno) el puerto no pudo leer: se deja el lote, sin culpar al destino */
/* Copia la página viva P de la víctima al destino. 1 = copiada; 0 = no era de nadie (se dio por muerta); < 0 = error. */
static int copia(W6f *fs, u32 P, int ultima)
{
    u8 h[W6F_CAB]; Cab c; Nodo n; u32 Pn, lote = (ultima && !MUT(11)) ? 3 : 1, id = 0, gen = 0, idx = 0, crc; int r, v, tipo, danada = 0;
    u32 veces = fs->conf.relecturas ? fs->conf.relecturas : 2, intento;
    if (MUT(24)) veces = 0;
    /* (N7) una lectura que sale mal puede ser pasajera (NAND): se relee antes de creérsela */
    for (intento = 0; ; intento++) {
        if (!fl_leer(fs, P, 0, h, W6F_CAB) && (v = cab_leer(h, &c)) == 1) break;
        if (intento >= veces) { if (fl_leer(fs, P, 0, h, W6F_CAB)) return E_LECTURA; v = cab_leer(h, &c); break; }   /* (M2) un error de lectura no mata nada */
    }
    if (v == 1 && c.tipo == 'D') { tipo = 'D'; id = c.id; gen = c.gen; idx = c.idx; }
    else if (v == 1 && c.tipo == 'N' && vivo(fs, c.id) && fs->obj[c.id].nodo == P) { tipo = 'N'; id = c.id; gen = c.gen; }
    else if (v == 1 && c.tipo == 'C' && c.id < fs->n_rangos && fs->cp[c.id] == P) { tipo = 'C'; id = c.id; }
    else {                                             /* la cabecera ya no vale (o miente): se rehace con lo que sabe la RAM */
        tipo = duenio(fs, P, &id, &gen, &idx); v = 0;
        if (!tipo) { fs->cta.incoherencias++; matar(fs, P); return 0; }
        if (tipo == 'N') gen = g32(h + 8);
    }
    if (tipo == 'D') {
        for (intento = 0; ; intento++) {               /* (N7) antes de marcarla dañada para siempre, releerla */
            if (fl_leer(fs, P, W6F_CAB, fs->pg + W6F_CAB, fs->carga)) { if (intento >= veces) return E_LECTURA; continue; }
            crc = crc32_mas(0, fs->pg + W6F_CAB, fs->carga);
            if (crc == g32(h + 20) || intento >= veces) break;
        }
        danada = (v == 1 && c.danada) || crc != g32(h + 20);       /* (con la cabecera rota pero la carga buena, la copia sale SANA) */
        if (MUT(19)) { crc = g32(h + 20); danada = 0; }             /* el fallo C1: heredar el CRC sin mirar */
        cab_poner_l(fs->pg, 'D', id, gen, idx, fs->seq++, crc, lote | (danada ? LOTE_DANADA : 0));   /* secuencia NUEVA: la copia gana a su original */
    } else if (tipo == 'N') {
        u8 *p = fs->pg + W6F_CAB; int bueno = 0;
        bueno = v == 1 && nodo_bueno(fs, P);
        if (fl_leer(fs, P, W6F_CAB, p, fs->carga)) return E_LECTURA;
        if (nodo_carga(fs, p, &n) || !nombre_valido(p + NODO_FIJO, n.nl) || n.padre >= fs->conf.max_obj || n.padre == id) {
            if (fs->obj[id].marcas & M_ESCR) return E_LECTURA;      /* (hay una escritura abierta sobre él: más tarde) */
            fs->cta.perdidos++; quitar(fs, id); cp_pendiente(fs, id / fs->por_rango);   /* ni se sabe cómo se llama: se pierde, y se dice */
            return 0;
        }
        if (n.mata != 0xFFFF && fs->pend_cp) return E_LECTURA;       /* (B2) su punto de control aún se debe: no se copia sin «mata» hasta que esté */
        if (!bueno && !MUT(20)) danada = 1;                         /* (M1) no se le pone un CRC bueno a la podredumbre sin decirlo */
        /* la copia sale SIN «mata»: el punto de control que lo remata se escribe siempre pegado al nodo */
        nodo_armar(fs, id, gen, n.padre, n.clase, n.nl, n.tam, n.fecha, MUT(103) ? n.mata : 0xFFFF, n.cola_len, n.cola_aparte, lote | (danada ? LOTE_DANADA : 0));
    } else cp_armar(fs, id, lote);
    if ((r = prog_destino(fs, &Pn))) return r;
    fs->sig[Pn] = P_TD;                                /* copiada, aún sin valor */
    fs->gc_ncop++; fs->cta.copias_gc++;
    if (danada) fs->cta.copias_danadas++;
    if (lote == 3) { fs->gc_comprometido = 1; fs->gc_compromiso = Pn; }
    return 1;
}
/* El lote está escrito: ¿se lee ENTERO y con su compromiso? (lo mismo que exigirá el montaje) */
static int lote_releido(W6f *fs)
{
    u32 ppb = fs->fl->ppb, k, n = 0, P; u8 h[W6F_CAB]; Cab c;
    for (k = 1; k < ppb; k++) {
        P = fs->destino * ppb + k;
        if (fs->sig[P] != P_TD) continue;
        if (fl_leer(fs, P, 0, h, W6F_CAB) || cab_leer(h, &c) != 1 || !c.lote || !entera(fs, P, &c)) return 0;
        n++;
    }
    P = fs->gc_compromiso;
    if (P >= fs->n_pag || fl_leer(fs, P, 0, h, W6F_CAB) || cab_leer(h, &c) != 1 || c.lote != 3 || !entera(fs, P, &c)) return 0;
    return n == fs->gc_ncop;
}
/* El lote está comprometido en la flash: cada copia sustituye a su original, si es que sigue vivo. */
static void reenlazar(W6f *fs)
{
    u32 V = fs->victima, ppb = fs->fl->ppb, k, uid = NINGUNO, ugen = 0, uidx = 0, i;
    w6f_pag *uh = 0;
    for (k = 1; k < ppb; k++) {
        u32 Pn = fs->destino * ppb + k; u8 h[W6F_CAB]; Cab c;
        if (fs->sig[Pn] != P_TD) continue;
        fs->sig[Pn] = P_MUERTA;
        if (fl_leer(fs, Pn, 0, h, W6F_CAB) || cab_leer(h, &c) != 1) continue;
        if (c.tipo == 'N') {
            W6fObj *o = &fs->obj[c.id];
            if (vivo(fs, c.id) && o->nodo / ppb == V) {
                matar(fs, o->nodo); vivir(fs, Pn, P_META); o->nodo = (w6f_pag)Pn; o->seq = c.seq;
                if (c.danada) o->marcas |= M_NODO_MALO;
            }
        } else if (c.tipo == 'C') {
            if (c.id < fs->n_rangos && fs->cp[c.id] != P_NIL && fs->cp[c.id] / ppb == V) { matar(fs, fs->cp[c.id]); vivir(fs, Pn, P_META); fs->cp[c.id] = (w6f_pag)Pn; }
        } else if (c.tipo == 'D') {
            w6f_pag *hq; u32 pos, pasos = 0; int por_idx = 0;
            if (uh && uid == c.id && ugen == c.gen && uidx <= c.idx) { hq = uh; pos = uidx; }
            else {
                Nodo n; W6fEscr *w;
                hq = 0; pos = 0;
                if (vivo(fs, c.id) && !(fs->obj[c.id].marcas & M_DIR) && !nodo_leer(fs, fs->obj[c.id].nodo, &n) && n.c.gen == c.gen)
                    hq = &fs->obj[c.id].primera;
                else if ((w = escritor_de(fs, c.id, c.gen)) != 0) hq = &w->primera;
            }
            if (hq && vivo(fs, c.id) && (fs->obj[c.id].marcas & M_DANADO) && !escritor_de(fs, c.id, c.gen)) {
                /* a este fichero le faltan páginas: en su cadena la posición no es el índice; se busca por el índice */
                por_idx = 1; hq = &fs->obj[c.id].primera;
                while (*hq != P_NIL && pasos++ <= fs->n_pag && !(*hq / ppb == V && idx_de(fs, *hq) == c.idx)) hq = &fs->sig[*hq];
                pos = c.idx;
            } else while (hq && *hq != P_NIL && pos < c.idx && pasos++ <= fs->n_pag) { hq = &fs->sig[*hq]; pos++; }
            if (hq && *hq != P_NIL && pos == c.idx && (MUT(111) || *hq / ppb == V)) {
                u32 vieja = *hq;
                vivir(fs, Pn, fs->sig[vieja]); *hq = (w6f_pag)Pn; matar(fs, vieja);
                for (i = 0; i < W6F_MAX_ESCR; i++) {
                    W6fEscr *e = &fs->escr[i];
                    if (MUT(101) || !e->activo) continue;
                    if (e->primera == vieja) e->primera = (w6f_pag)Pn;
                    if (e->ultima == vieja) e->ultima = (w6f_pag)Pn;
                    if (e->cola0 == vieja) e->cola0 = (w6f_pag)Pn;
                    if (e->ant == vieja) e->ant = (w6f_pag)Pn;
                }
            }
            if (por_idx) uh = 0; else { uh = hq; uid = c.id; ugen = c.gen; uidx = pos; }
        }
    }
    fs->cambios++; fs->crc_ok = P_NIL; fs->nodo_ok = P_NIL;
}

static u32 media_borrados(const W6f *fs)
{
    u32 b, n = 0, s = 0;
    for (b = 0; b < fs->fl->n_bloques; b++)
        if (fs->blq[b].estado != B_MALO && fs->blq[b].borrados != SIN_CUENTA) n++;
    if (!n) return 0;
    for (b = 0; b < fs->fl->n_bloques; b++)            /* (las cuentas valen < 2^24 y los bloques son pocos: no desborda) */
        if (fs->blq[b].estado != B_MALO && fs->blq[b].borrados != SIN_CUENTA) s += fs->blq[b].borrados / n;
    return s;
}

/* La víctima (con algo vivo) que toca. *frio = 1 si sale por nivelado de desgaste. */
static u32 elegir(W6f *fs, int forzado, int *es_frio)
{
    u32 b, ppb = fs->fl->ppb, mejor = NINGUNO, mm = 0, mx = 0, frio = NINGUNO, minimo, disp = disponibles(fs);
    *es_frio = 0;
    for (b = 0; b < fs->fl->n_bloques; b++) {
        const W6fBlq *q = &fs->blq[b];
        if (q->estado == B_SUCIO) return b;            /* sin cabecera pero con páginas válidas dentro: fuera cuanto antes */
        if (q->estado == B_MALO || q->borrados == SIN_CUENTA) continue;
        if (q->borrados > mx) mx = q->borrados;
        if (q->estado == B_USADO && (frio == NINGUNO || q->borrados < fs->blq[frio].borrados)) frio = b;
    }
    /* nivelado estático (solo en los pasos de fondo): los datos fríos del bloque menos borrado se mudan al libre más
     * gastado, y el bloque fresco entra en el reparto */
    if (!MUT(108) && !forzado && frio != NINGUNO && mx - fs->blq[frio].borrados > fs->conf.umbral_desgaste) { *es_frio = 1; return frio; }
    minimo = forzado ? 1 : (ppb - 1) / 4 ? (ppb - 1) / 4 : 1;
    if (!forzado) {
        /* (M4) de fondo: mientras no haya un colchón de dos bloques de páginas en blanco se recolectan también los bloques
         * que rinden poco (un octavo de páginas muertas), para que la próxima escritura no tenga que esperar; con colchón,
         * solo los de un cuarto; y si sobra sitio, nada. NUNCA de fondo un bloque que rinda menos de un octavo: copiarlo
         * entero para ganar una página cuesta un borrado por operación (medido: docs/fs_propio.md §12, M4). */
        u32 pierde = 0;
        if (disp < 2 * (ppb - 1)) minimo = (ppb - 1) / 8 ? (ppb - 1) / 8 : 1;
        else if (disp >= (fs->conf.reserva_bloques + 1) * (ppb - 1)) return NINGUNO;
        /* si ya hay dos bloques a medias, el ciclo cerrará uno y su resto en blanco se pierde: de fondo solo compensa si
         * la víctima da MÁS que eso (si no, la recolección de fondo no acabaría nunca) */
        if (fs->cabeza != NINGUNO && fs->parcial != NINGUNO) pierde = ppb - fs->llenado < ppb - fs->parcial_llenado ? ppb - fs->llenado : ppb - fs->parcial_llenado;
        minimo += pierde;
    }
    for (b = 0; b < fs->fl->n_bloques; b++) {
        const W6fBlq *q = &fs->blq[b]; u32 m;
        if (q->estado != B_USADO) continue;
        m = ppb - 1 - q->vivas;
        if (m < minimo) continue;
        if (mejor == NINGUNO || m > mm || (m == mm && q->borrados < fs->blq[mejor].borrados)) { mejor = b; mm = m; }
    }
    return mejor;
}

static int borrar_bloque(W6f *fs, u32 b)
{
    W6fBlq *q = &fs->blq[b]; u32 ppb = fs->fl->ppb, P = b * ppb, cuenta, k;
    u8 *p = fs->pgb + W6F_CAB;                         /* (en pg puede haber una página armada esperando sitio) */
    fs->cta.borrados++; fs->crc_ok = P_NIL; fs->nodo_ok = P_NIL;
    TRAZA("borrar bloque %u (estado %u, vivas %u)\n", b, q->estado, q->vivas);
    if (fs->fl->borrar(fs->fl->ctx, b)) {
        fs->cta.fallos_borrado++;
        q->estado = B_SUCIO;
        if ((++q->fallos & 0x3F) >= 2 || fs->fl->clase == W6F_FL_NAND) malo(fs, b);
        return W6F_E_FLASH;
    }
    cuenta = MUT(114) ? 1 : (q->borrados == SIN_CUENTA ? media_borrados(fs) : q->borrados) + 1;
    q->estado = B_SUCIO;
    for (k = 0; k < ppb; k++) if (!blanca(fs, P + k)) break;         /* antes de darlo por libre: en blanco ENTERO */
    memset(fs->pgb, 0xFF, fs->fl->pag);
    p32(p, fs->fl->pag); p32(p + 4, ppb); p32(p + 8, fs->fl->n_bloques); p32(p + 12, fs->fl->clase); p32(p + 16, fs->conf.max_obj);
    cab_poner(fs->pgb, 'B', 0, cuenta, 0, 0, crc32_mas(0, p, 20));
    if (k < ppb || (fs->fl->programar(fs->fl->ctx, b, 0, fs->pgb), fs->cta.programas++, !coincide_con(fs, P, fs->pgb))) {
        fs->cta.fallos_programa++;
        if ((++q->fallos & 0x3F) >= 3) malo(fs, b);
        return W6F_E_FLASH;
    }
    q->borrados = cuenta; q->estado = B_LIBRE; q->fallos = 0; fs->libres++;
    if (fs->libres >= fs->libres_min) fs->repo_intentos = 0;
    return 0;
}

static void abortar_gc(W6f *fs, int culpa_del_destino)
{
    u32 k, D = fs->destino, ppb = fs->fl->ppb;
    for (k = 0; k < ppb; k++) if (fs->sig[D * ppb + k] == P_TD) fs->sig[D * ppb + k] = P_MUERTA;   /* las copias no llegaron a contar */
    fs->blq[D].estado = B_USADO;                       /* (sin nada vivo si era un bloque fresco: lo primero que hará el siguiente paso es borrarlo) */
    if (culpa_del_destino && !fs->blq[D].vivas && (++fs->blq[D].fallos & 0x3F) >= 4) malo(fs, D);   /* (nunca se aparta un bloque que tenga algo vivo) */
    fs->destino = fs->victima = NINGUNO; fs->cta.lotes_abortados++;
}

/* El destino, ya con su lote comprometido, pasa a ser sitio donde seguir escribiendo. */
static void destino_a_cabeza(W6f *fs)
{
    u32 D = fs->destino, ppb = fs->fl->ppb, ll = fs->dest_llenado;
    fs->destino = NINGUNO;
    if (ll >= ppb) { fs->blq[D].estado = B_USADO; return; }
    fs->blq[D].estado = B_CABEZA;
    if (fs->cabeza == NINGUNO) { fs->cabeza = D; fs->llenado = ll; fs->fallos_seguidos = 0; return; }
    if (fs->parcial != NINGUNO) {                      /* tres a medias: se cierra la que menos sitio tenga */
        if (fs->parcial_llenado >= ll && fs->parcial_llenado >= fs->llenado) { fs->blq[fs->parcial].estado = B_USADO; }
        else if (fs->llenado >= ll) { fs->blq[fs->cabeza].estado = B_USADO; fs->cabeza = fs->parcial; fs->llenado = fs->parcial_llenado; fs->fallos_seguidos = 0; }
        else { fs->blq[D].estado = B_USADO; return; }
    }
    fs->parcial = D; fs->parcial_llenado = ll;
}
static void cerrar_llenas(W6f *fs)                     /* una cabeza llena es un bloque usado más (sin abrir otra) */
{
    u32 ppb = fs->fl->ppb;
    if (fs->parcial != NINGUNO && fs->parcial_llenado >= ppb) { fs->blq[fs->parcial].estado = B_USADO; fs->parcial = NINGUNO; }
    if (fs->cabeza != NINGUNO && fs->llenado >= ppb) {
        fs->blq[fs->cabeza].estado = B_USADO; fs->cabeza = fs->parcial; fs->llenado = fs->parcial_llenado; fs->parcial = NINGUNO; fs->fallos_seguidos = 0;
    }
    if (fs->cabeza == NINGUNO && fs->parcial != NINGUNO) { fs->cabeza = fs->parcial; fs->llenado = fs->parcial_llenado; fs->parcial = NINGUNO; fs->fallos_seguidos = 0; }
}
/* Todo lo que una compactación completa podría dejar en blanco, en páginas. */
static u32 recuperable(const W6f *fs)
{
    u32 b, ppb = fs->fl->ppb, n = fs->libres * (ppb - 1);
    for (b = 0; b < fs->fl->n_bloques; b++) {
        const W6fBlq *q = &fs->blq[b];
        if (q->estado == B_USADO || q->estado == B_SUCIO) n += ppb - 1 - (q->vivas < ppb - 1 ? q->vivas : ppb - 1);
    }
    if (fs->cabeza != NINGUNO) n += ppb - fs->llenado;
    if (fs->parcial != NINGUNO) n += ppb - fs->parcial_llenado;
    return n;
}

/* Un trozo de recolección. 1 = hizo algo; 0 = no hay nada que hacer (o nada posible); < 0 = error. */
static int gc_trabajo(W6f *fs, int forzado, u32 tope)
{
    u32 ppb = fs->fl->ppb, n = 0, b, k, ini = 1; int r, frio = 0, quedan_vivas = 0;
    if (fs->seq >= SEQ_TOPE - 2 * ppb) { if (fs->victima != NINGUNO) abortar_gc(fs, 0); return 0; }     /* (N2) con la secuencia acabándose no se recolecta */
    cerrar_llenas(fs);
    if (fs->victima == NINGUNO) {
        if (fs->pend_cp) cp_pendientes(fs);
        for (b = 0; b < fs->fl->n_bloques; b++)        /* lo que no tiene nada vivo se borra sin más: no necesita sitio */
            if ((fs->blq[b].estado == B_SUCIO || fs->blq[b].estado == B_USADO) && fs->blq[b].vivas == 0) { borrar_bloque(fs, b); return 1; }
        fs->gc_emergencia = 0;
        if (fs->libres < fs->libres_min) {
            /* (A1) FALTA un bloque libre de la reserva: murió uno al borrarlo. Se repone juntando bloques a medias: la
             * víctima con menos vivas se vacía en el sitio de la cabeza (en los lotes que haga falta); si la cabeza no
             * tiene sitio, un ciclo normal concentra el blanco en un bloque nuevo. Solo si las cuentas dicen que se puede. */
            u32 mejor = NINGUNO;
            /* hace falta, FUERA de los bloques ya libres, al menos lo que falta por reponer y un poco más (cada lote gasta
             * una página en su compromiso); y si tras muchos lotes no se ha logrado, se deja hasta que algo cambie */
            if (recuperable(fs) - fs->libres * (ppb - 1) < (fs->libres_min - fs->libres) * (ppb - 1) + 4) return 0;
            if (fs->repo_intentos > 4 * fs->fl->n_bloques + 16) return 0;
            fs->repo_intentos++;
            if (fs->cabeza != NINGUNO && (fs->blq[fs->cabeza].fallos & SIN_MIRAR)) { u32 u = mirar(fs, fs->cabeza, fs->llenado); if (u) { fs->llenado = u + 1; return 1; } }
            if (fs->cabeza != NINGUNO && ppb - fs->llenado >= 2)
                for (b = 0; b < fs->fl->n_bloques; b++)
                    if (fs->blq[b].estado == B_USADO && fs->blq[b].vivas && (mejor == NINGUNO || fs->blq[b].vivas < fs->blq[mejor].vivas)) mejor = b;
            if (mejor != NINGUNO) {
                fs->victima = mejor; fs->destino = fs->cabeza; fs->dest_llenado = fs->llenado; fs->cabeza = NINGUNO;
                fs->blq[fs->destino].estado = B_DESTINO; fs->gc_emergencia = 1; fs->cta.emergencias++;
            } else if (!fs->libres) return 0;          /* ni bloque libre ni sitio en la cabeza: no hay con qué */
            else forzado = 1;
        }
        if (fs->victima == NINGUNO) {
            if ((b = elegir(fs, forzado, &frio)) == NINGUNO) return 0;
            fs->victima = b;
            if ((fs->destino = tomar_libre(fs, frio, &ini)) == NINGUNO) { fs->victima = NINGUNO; return 1; }   /* los «libres» tenían restos: se borrarán */
            fs->blq[fs->destino].estado = B_DESTINO; fs->dest_llenado = ini;
        }
        b = fs->victima;
        fs->gc_pos = 0; fs->gc_ncop = 0; fs->gc_comprometido = 0; fs->gc_compromiso = NINGUNO; fs->cta.ciclos_gc++;
        TRAZA("gc: victima %u (vivas %u) -> destino %u desde %u (%s)\n", b, fs->blq[b].vivas, fs->destino, fs->dest_llenado, fs->gc_emergencia == 1 ? "en la cabeza" : "bloque fresco");
    }
    b = fs->victima;
    if (MUT(5)) for (k = 0; k < ppb; k++) matar(fs, b * ppb + k);
    while (fs->gc_pos < ppb) {
        u32 P = b * ppb + fs->gc_pos, huecos = ppb - fs->dest_llenado; int ultima = 1;
        if (fs->sig[P] == P_MUERTA) { fs->gc_pos++; continue; }
        if (n >= tope) return 1;
        for (k = fs->gc_pos + 1; k < ppb; k++) if (fs->sig[b * ppb + k] != P_MUERTA) { ultima = 0; break; }
        if (huecos == 0 || (huecos == 1 && !ultima)) { quedan_vivas = 1; break; }     /* no cabe más: el lote se cierra aquí (parcial) */
        if ((r = copia(fs, P, ultima)) < 0) { abortar_gc(fs, r != E_LECTURA); return W6F_E_FLASH; }
        n += (u32)r; fs->gc_pos++;
        if (n > fs->cta.peor_paso_programas) fs->cta.peor_paso_programas = n;
    }
    if (fs->gc_ncop && !fs->gc_comprometido && !MUT(11)) {             /* la última copiada no era la última viva: compromiso aparte */
        u32 P;
        memset(fs->pg, 0xFF, fs->fl->pag);
        cab_poner_l(fs->pg, 'G', 0, 0, 0, fs->seq++, 0, 3);
        if ((r = prog_destino(fs, &P))) { abortar_gc(fs, 1); return r; }
        fs->gc_compromiso = P;
    }
    if (fs->gc_ncop) {
        /* la víctima no se toca hasta que el lote está comprometido Y releído */
        if (!MUT(11) && !MUT(21) && !lote_releido(fs)) { abortar_gc(fs, 1); return W6F_E_FLASH; }
        reenlazar(fs); destino_a_cabeza(fs);
        if (quedan_vivas) fs->cta.lotes_parciales++;
    } else if (fs->gc_emergencia == 1) destino_a_cabeza(fs);
    else if (fs->dest_llenado == 1) {                  /* no hizo falta copiar nada: el destino (recién borrado, sin tocar) vuelve a estar libre */
        fs->blq[fs->destino].estado = B_LIBRE; fs->libres++; fs->destino = NINGUNO;
    } else { fs->blq[fs->destino].estado = B_USADO; fs->destino = NINGUNO; }
    fs->victima = NINGUNO;                             /* la víctima, si quedó sin vivas, se borra en el paso siguiente */
    return 1;
}

/* Que haya «n» páginas blancas para lo que se va a escribir, recolectando si hace falta (por pasos acotados, con
 * «respiro» entre ellos).
 *  - Escritura normal: nunca se toca la reserva (libres_min bloques libres), y si a la reserva le FALTA un bloque no se
 *    admite escribir hasta reponerlo: W6F_E_LLENO. Con el volumen dentro de su capacidad siempre se puede: o hay un
 *    bloque sin vivas que borrar, o hay un bloque libre de destino y una víctima con al menos una página muerta.
 *  - Lo URGENTE (fs->urgente: el punto de control de un borrado) puede gastar hasta el último bloque libre: borrar cabe
 *    mientras quede UNA página en blanco o un bloque libre. */
static int sitio(W6f *fs, u32 n)
{
    u32 paso = fs->conf.tope_paso ? fs->conf.tope_paso : 32, errores = 0;
    u32 tope = (n + 8) * (fs->fl->ppb / paso + 6) * (fs->fl->n_bloques + 4) + 64;
    for (;;) {
        int r, falta = !fs->urgente && fs->libres < fs->libres_min;
        if (fs->seq >= SEQ_TOPE) { fs->solo_lectura = 1; fs->motivo_sl = W6F_SL_SECUENCIA; return W6F_E_SOLO_LECTURA; }   /* (N2) también a mitad de una operación larga */
        if (!falta && disponibles(fs) >= n) return 0;
        r = MUT(12) ? 0 : gc_trabajo(fs, 1, paso);
        if (r < 0 && ++errores >= 3) return r;
        if (r == 0 || !--tope) return W6F_E_LLENO;
        if (fs->conf.respiro) fs->conf.respiro(fs->conf.respiro_ctx);
    }
}

/* ---- cadenas y lectura ------------------------------------------------------------------------------- */
static u32 pagina_de(W6f *fs, u32 id, u32 k)           /* la página de datos n.º k del fichero, o NINGUNO */
{
    u32 i = 0, pasos = 0; w6f_pag p = fs->obj[id].primera;
    if (fs->obj[id].marcas & M_DANADO) {               /* (M2) le faltan páginas: en su cadena la posición no es el índice */
        for (; p != P_NIL && p < fs->n_pag && pasos++ <= fs->n_pag; p = fs->sig[p]) if (idx_de(fs, p) == k) return p;
        return NINGUNO;
    }
    if (fs->cur_cambios == fs->cambios && fs->cur_id == id && fs->cur_pag != P_NIL && fs->cur_idx <= k) { i = fs->cur_idx; p = fs->cur_pag; }
    while (i < k && p != P_NIL && pasos++ <= fs->n_pag) { p = fs->sig[p]; i++; }
    if (p == P_NIL || p >= fs->n_pag) return NINGUNO;
    fs->cur_cambios = fs->cambios; fs->cur_id = (u16)id; fs->cur_idx = k; fs->cur_pag = p;
    return p;
}
static u32 largo_cadena(const W6f *fs, w6f_pag p)
{
    u32 n = 0;
    while (p != P_NIL && p < fs->n_pag && n <= fs->n_pag) { n++; p = fs->sig[p]; }
    return n;
}
static void matar_cadena(W6f *fs, w6f_pag p)
{
    u32 pasos = 0;
    while (p != P_NIL && p < fs->n_pag && pasos++ <= fs->n_pag) { w6f_pag s = fs->sig[p]; matar(fs, p); p = s; }
}
static int dato_verificado(W6f *fs, u32 P)
{
    if (fs->crc_ok == P || MUT(17)) return 1;
    if (!dato_bueno(fs, P)) return 0;
    fs->crc_ok = (w6f_pag)P;
    return 1;
}

/* Lee lo que se pueda. (M1) La cola en línea pasa por el CRC del nodo. (M2) Si una página está rota o falta, se devuelve
 * lo leído hasta ella; la llamada que EMPIEZA en ella da el error (W6F_E_CRC / W6F_E_DANADO). Las demás páginas se leen. */
int w6f_leer(W6f *fs, uint16_t id, uint32_t desde, void *buf, uint32_t max)
{
    Nodo n; u32 hecho = 0, llenas;
    if (!fs->montado) return W6F_E_SIN_MONTAR;
    if (!vivo(fs, id)) return W6F_E_NO_HAY;
    if (fs->obj[id].marcas & M_DIR) return W6F_E_ES_DIR;
    if (fs->obj[id].marcas & M_NODO_MALO) return W6F_E_CRC;
    if (nodo_leer(fs, fs->obj[id].nodo, &n)) { fs->obj[id].marcas |= M_NODO_MALO; return W6F_E_CRC; }
    if (desde >= n.tam) return 0;
    if (max > n.tam - desde) max = n.tam - desde;
    if (max > 0x7FFFFFFFu) max = 0x7FFFFFFFu;
    llenas = n.tam / fs->carga;
    while (hecho < max) {
        u32 pos = desde + hecho, k = pos / fs->carga, o = pos % fs->carga, t = fs->carga - o;
        if (t > max - hecho) t = max - hecho;
        if (k < llenas || n.cola_aparte) {
            u32 P = pagina_de(fs, id, k);
            if (P == NINGUNO) return hecho ? (int)hecho : W6F_E_DANADO;
            if (!dato_verificado(fs, P)) return hecho ? (int)hecho : W6F_E_CRC;
            if (fl_leer(fs, P, W6F_CAB + o, (u8 *)buf + hecho, t)) return hecho ? (int)hecho : W6F_E_FLASH;
        } else {
            if (!MUT(22) && !nodo_bueno(fs, fs->obj[id].nodo)) { fs->obj[id].marcas |= M_NODO_MALO; return hecho ? (int)hecho : W6F_E_CRC; }
            if (fl_leer(fs, fs->obj[id].nodo, W6F_CAB + NODO_FIJO + n.nl + o, (u8 *)buf + hecho, t)) return hecho ? (int)hecho : W6F_E_FLASH;
        }
        hecho += t;
    }
    return (int)hecho;
}

int w6f_leer_todo(W6f *fs, uint16_t id, void *buf, uint32_t max)
{
    Nodo n; u32 hecho = 0; int g;
    if (!fs->montado) return W6F_E_SIN_MONTAR;
    if (!vivo(fs, id)) return W6F_E_NO_HAY;
    if (fs->obj[id].marcas & M_DIR) return W6F_E_ES_DIR;
    if ((fs->obj[id].marcas & M_NODO_MALO) || nodo_leer(fs, fs->obj[id].nodo, &n)) return W6F_E_CRC;
    if (n.tam > max || n.tam > 0x7FFFFFFFu) return W6F_E_ARG;
    while (hecho < n.tam) {
        g = w6f_leer(fs, id, hecho, (u8 *)buf + hecho, n.tam - hecho);
        if (g < 0) return g;
        if (g == 0) return W6F_E_DANADO;
        hecho += (u32)g;
    }
    return (int)hecho;
}

int w6f_verificar(W6f *fs, uint16_t id)
{
    Nodo n; u32 k, nd;
    if (!fs->montado) return W6F_E_SIN_MONTAR;
    if (!vivo(fs, id)) return W6F_E_NO_HAY;
    if ((fs->obj[id].marcas & M_NODO_MALO) || !nodo_bueno(fs, fs->obj[id].nodo) || nodo_leer(fs, fs->obj[id].nodo, &n)) return W6F_E_CRC;
    if (fs->obj[id].marcas & M_DIR) return 0;
    nd = n_datos(fs, &n);
    for (k = 0; k < nd; k++) {
        u32 P = pagina_de(fs, id, k);
        if (P == NINGUNO) return W6F_E_DANADO;
        if (!dato_bueno(fs, P)) return W6F_E_CRC;
    }
    return (fs->obj[id].marcas & M_DANADO) ? W6F_E_DANADO : 0;
}

/* ---- rutas ------------------------------------------------------------------------------------------- */
static int buscar_en(W6f *fs, u32 padre, const char *nom, u32 nl)
{
    u32 id; u8 h = hash8((const u8 *)nom, nl), t[W6F_NOMBRE_MAX];
    for (id = 1; id < fs->conf.max_obj; id++) {
        const W6fObj *o = &fs->obj[id]; Nodo n;
        if (o->nodo == P_NIL || o->padre != padre || o->hash != h) continue;
        if (nodo_leer(fs, o->nodo, &n) || n.nl != nl) continue;
        if (fl_leer(fs, o->nodo, W6F_CAB + NODO_FIJO, t, nl) || memcmp(t, nom, nl)) continue;
        return (int)id;
    }
    return W6F_E_NO_HAY;
}
/* Recorre la ruta hasta el penúltimo componente. Deja el directorio que contiene al último, y el último (nl = 0 si
 * la ruta es la raíz). «prof» = niveles que tendría el último componente. */
static int resolver(W6f *fs, const char *ruta, u32 *padre, const char **nom, u32 *nl, u32 *prof)
{
    u32 dir = 0, niveles = 0, largo = 0;
    const char *p = ruta;
    if (!ruta) return W6F_E_ARG;
    while (ruta[largo]) if (++largo > W6F_RUTA_MAX) return W6F_E_NOMBRE;
    while (*p == '/') p++;
    *nom = p; *nl = 0;
    while (*p) {
        const char *ini = p; u32 n; int id;
        while (*p && *p != '/') p++;
        n = (u32)(p - ini);
        while (*p == '/') p++;
        if (!nombre_valido((const u8 *)ini, n)) return W6F_E_NOMBRE;
        if (++niveles > W6F_PROF_MAX) return W6F_E_PROFUNDO;
        if (!*p) { *nom = ini; *nl = n; break; }
        id = buscar_en(fs, dir, ini, n);
        if (id < 0) return W6F_E_NO_HAY;
        if (!(fs->obj[id].marcas & M_DIR)) return W6F_E_NO_DIR;
        dir = (u32)id;
    }
    *padre = dir; if (prof) *prof = niveles;
    return 0;
}
int w6f_buscar(W6f *fs, const char *ruta)
{
    u32 padre, nl; const char *nom; int r;
    if (!fs->montado) return W6F_E_SIN_MONTAR;
    if ((r = resolver(fs, ruta, &padre, &nom, &nl, 0))) return r;
    if (nl == 0) return 0;
    return buscar_en(fs, padre, nom, nl);
}
int w6f_info(W6f *fs, uint16_t id, W6fInfo *inf)
{
    Nodo n;
    if (!fs->montado) return W6F_E_SIN_MONTAR;
    memset(inf, 0, sizeof *inf);
    if (id == 0) { inf->es_dir = 1; return 0; }
    if (!vivo(fs, id)) return W6F_E_NO_HAY;
    if (nodo_leer(fs, fs->obj[id].nodo, &n)) return W6F_E_DANADO;
    inf->id = id; inf->padre = fs->obj[id].padre; inf->es_dir = (fs->obj[id].marcas & M_DIR) != 0;
    inf->danado = (fs->obj[id].marcas & (M_DANADO | M_NODO_MALO)) != 0; inf->tam = n.tam; inf->fecha = n.fecha; inf->nombre_len = n.nl;
    if (fl_leer(fs, fs->obj[id].nodo, W6F_CAB + NODO_FIJO, inf->nombre, n.nl)) return W6F_E_FLASH;
    inf->nombre[n.nl] = 0;
    return 0;
}
int w6f_listar(W6f *fs, uint16_t dir, uint32_t *cursor, W6fInfo *inf)
{
    u32 id;
    if (!fs->montado) return W6F_E_SIN_MONTAR;
    if (!es_dir(fs, dir)) return W6F_E_NO_DIR;
    for (id = *cursor + 1; id < fs->conf.max_obj; id++)
        if (fs->obj[id].nodo != P_NIL && fs->obj[id].padre == dir) { *cursor = id; return w6f_info(fs, (u16)id, inf) ? W6F_E_DANADO : 1; }
    *cursor = id;
    return 0;
}
static u32 altura(const W6f *fs, u32 dir)              /* niveles por debajo de «dir» */
{
    u32 id, h = 0;
    for (id = 1; id < fs->conf.max_obj; id++) {
        u32 x = id, n = 0;
        if (fs->obj[id].nodo == P_NIL) continue;
        while (x && x != dir && n <= W6F_PROF_MAX + 1) { x = fs->obj[x].padre; n++; }
        if (x == dir && n > h) h = n;
    }
    return h;
}
static int nombre_en_uso(const W6f *fs, u32 padre, const char *nom, u32 nl)    /* ¿lo tiene pedido una escritura abierta? */
{
    u32 i;
    for (i = 0; i < W6F_MAX_ESCR; i++) {
        const W6fEscr *w = &fs->escr[i];
        if (w->activo && w->padre == padre && w->nl == nl && !memcmp(w->nombre, nom, nl)) return 1;
    }
    return 0;
}
/* (N1) Un bloque apartado solo EN RAM (el puerto no tiene dónde apuntarlo: NOR) pudo serlo por una racha pasajera de
 * fallos. Antes de dar el volumen por sin salida se les da otra oportunidad: vuelven a «sucio» y se intentará borrarlos.
 * Los que el puerto dice malos (NAND, marca persistente) no se tocan. Devuelve cuántos. */
static u32 rehabilitar(W6f *fs)
{
    u32 b, n = 0;
    for (b = 0; b < fs->fl->n_bloques; b++) {
        if (fs->blq[b].estado != B_MALO || (fs->fl->es_malo && fs->fl->es_malo(fs->fl->ctx, b))) continue;
        fs->blq[b].estado = B_SUCIO; fs->blq[b].fallos = 0; fs->utiles++; n++;
    }
    if (n) calcular_cap(fs);
    return n;
}
/* ¿Queda por dónde seguir escribiendo? Un bloque libre, sitio en una cabeza, o algo sin vivas que se pueda borrar. */
static int hay_salida(const W6f *fs)
{
    u32 b;
    if (fs->libres || fs->victima != NINGUNO) return 1;
    if (fs->cabeza != NINGUNO && fs->llenado < fs->fl->ppb) return 1;
    if (fs->parcial != NINGUNO && fs->parcial_llenado < fs->fl->ppb) return 1;
    for (b = 0; b < fs->fl->n_bloques; b++)
        if ((fs->blq[b].estado == B_USADO || fs->blq[b].estado == B_SUCIO) && !fs->blq[b].vivas) return 1;
    return 0;
}
/* Los bloques libres que habrá en cuanto la recolección acabe lo que tiene a medias (sin escribir nada más). */
static u32 libres_previstos(const W6f *fs)
{
    u32 b, n = fs->libres + (fs->victima != NINGUNO && fs->gc_emergencia != 1 ? 1 : 0);
    for (b = 0; b < fs->fl->n_bloques; b++)
        if ((fs->blq[b].estado == B_USADO || fs->blq[b].estado == B_SUCIO) && !fs->blq[b].vivas && b != fs->victima) n++;
    return n;
}
/* Páginas que el usuario puede gastar todavía. (A1) Si a la reserva le falta un bloque (murió uno y aún no se ha
 * repuesto), CERO: no se admite escribir hasta que la recolección lo reponga; borrar sí se puede. */
static u32 libres_usuario(const W6f *fs)
{
    u32 u = fs->usadas + fs->pend + fs->reservadas;
    if (fs->solo_lectura || libres_previstos(fs) < fs->libres_min) return 0;
    return fs->cap > u ? fs->cap - u : 0;
}
static int id_libre(const W6f *fs)
{
    u32 id;
    if (fs->n_obj + fs->ids_reservados + 1 >= fs->conf.max_obj) return W6F_E_MUCHOS;
    for (id = 1; id < fs->conf.max_obj; id++) if (fs->obj[id].nodo == P_NIL && !(fs->obj[id].marcas & M_RESERVA)) return (int)id;
    return W6F_E_MUCHOS;
}
static int puede_escribir(const W6f *fs)
{
    if (!fs->montado) return W6F_E_SIN_MONTAR;
    return fs->solo_lectura ? W6F_E_SOLO_LECTURA : 0;
}
/* (M3) la secuencia se acaba: antes de que dé la vuelta, el volumen pasa a solo lectura (y lo dice) */
static int secuencia_bien(W6f *fs)
{
    if (fs->seq < SEQ_TOPE) return 1;
    fs->solo_lectura = 1; fs->motivo_sl = W6F_SL_SECUENCIA;
    return 0;
}

uint32_t w6f_paginas_para(const W6f *fs, uint32_t tam)
{
    return 1 + tam / fs->carga + (tam % fs->carga > fs->cola_max);
}

/* ---- escritura a trozos ------------------------------------------------------------------------------ */
static void soltar(W6f *fs, W6fEscr *w)
{
    if (!vivo(fs, w->id)) { fs->obj[w->id].marcas = 0; fs->ids_reservados--; }
    else fs->obj[w->id].marcas &= (u8)~M_ESCR;
    fs->reservadas -= 2; fs->pend -= w->pend;
    w->activo = 0; fs->cambios++; fs->crc_ok = P_NIL;
}

static int x_abrir(W6f *fs, const char *ruta, int modo, uint32_t fecha)
{
    u32 padre, nl, prof, h; const char *nom; int r, id, nuevo = 0;
    W6fEscr *w; Nodo n;
    if ((r = puede_escribir(fs))) return r;
    if (modo != W6F_CREAR && modo != W6F_ANADIR) return W6F_E_ARG;
    sitio(fs, 0);                                      /* (A1) si a la reserva le falta un bloque, se repone antes de mirar si cabe */
    if ((r = resolver(fs, ruta, &padre, &nom, &nl, &prof))) return r;
    if (nl == 0) return W6F_E_ES_DIR;
    for (h = 0; h < W6F_MAX_ESCR && fs->escr[h].activo; h++) { }
    if (h == W6F_MAX_ESCR) return W6F_E_OCUPADO;
    id = buscar_en(fs, padre, nom, nl);
    if (id >= 0) {
        if (fs->obj[id].marcas & M_DIR) return W6F_E_ES_DIR;
        if (fs->obj[id].marcas & M_ESCR) return W6F_E_OCUPADO;
        if (modo == W6F_ANADIR || MUT(3)) {            /* añadir saca del nodo viejo el tamaño y la cola: tiene que estar sano */
            if (fs->obj[id].marcas & M_DANADO) return W6F_E_DANADO;
            if ((fs->obj[id].marcas & M_NODO_MALO) || !nodo_bueno(fs, fs->obj[id].nodo) || nodo_leer(fs, fs->obj[id].nodo, &n)) { fs->obj[id].marcas |= M_NODO_MALO; return W6F_E_CRC; }
        }
    } else {
        if (nombre_en_uso(fs, padre, nom, nl)) return W6F_E_OCUPADO;
        if ((id = id_libre(fs)) < 0) return id;
        nuevo = 1; modo = W6F_CREAR;
    }
    if (libres_usuario(fs) < 2) return W6F_E_LLENO;
    w = &fs->escr[h];
    w->modo = (u8)modo; w->id = (u16)id; w->padre = (u16)padre; w->nl = (u8)nl; memcpy(w->nombre, nom, nl); w->nombre[nl] = 0;
    w->fecha = fecha; w->tam = 0; w->n_buf = 0; w->npag = 0; w->pend = 0; w->base = 0; w->escrito0 = 0; w->tenia_cola0 = 0;
    w->primera = w->ultima = w->cola0 = w->ant = P_NIL;
    if (modo == W6F_ANADIR) {
        u32 resto = n.tam % fs->carga, k; w6f_pag p = fs->obj[id].primera;
        w->gen = n.c.gen; w->tam = n.tam; w->base = n.tam / fs->carga; w->npag = w->base; w->fecha = fecha ? fecha : n.fecha;
        for (k = 0; k < w->base; k++) { if (p == P_NIL || p >= fs->n_pag) return W6F_E_DANADO; w->ant = p; p = fs->sig[p]; }
        w->ultima = w->ant;
        if (n.cola_aparte) {
            if (p == P_NIL || p >= fs->n_pag) return W6F_E_DANADO;
            if (!dato_bueno(fs, p)) return W6F_E_CRC;
            if (fl_leer(fs, p, W6F_CAB, w->buf, resto)) return W6F_E_FLASH;
            w->cola0 = p; w->tenia_cola0 = 1;
        } else if (resto && fl_leer(fs, fs->obj[id].nodo, W6F_CAB + NODO_FIJO + n.nl, w->buf, resto)) return W6F_E_FLASH;
        w->n_buf = resto;
    } else {
        w->gen = (MUT(3) && !nuevo) ? n.c.gen : fs->seq++;
    }
    if (nuevo) { fs->obj[id].marcas = M_RESERVA; fs->ids_reservados++; }
    else fs->obj[id].marcas |= M_ESCR;
    fs->reservadas += 2;
    w->activo = 1;
    return (int)h;
}

/* Programa el búfer de la escritura como su siguiente página de datos. */
static int volcar(W6f *fs, W6fEscr *w, int de_reserva)
{
    u32 P; int r;
    if (!de_reserva && libres_usuario(fs) < 1) return W6F_E_LLENO;
    if ((r = sitio(fs, 1))) return r;
    memcpy(fs->pg + W6F_CAB, w->buf, w->n_buf);
    memset(fs->pg + W6F_CAB + w->n_buf, 0xFF, fs->carga - w->n_buf);
    cab_poner(fs->pg, 'D', w->id, w->gen, w->npag, fs->seq++, crc32_mas(0, fs->pg + W6F_CAB, fs->carga));
    if ((r = prog(fs, &P))) return r;
    vivir(fs, P, P_NIL);
    if (w->modo == W6F_ANADIR && !w->escrito0) {
        if (w->cola0 != P_NIL) { matar(fs, w->cola0); w->cola0 = P_NIL; fs->usadas--; }
        if (w->ant == P_NIL) fs->obj[w->id].primera = (w6f_pag)P; else fs->sig[w->ant] = (w6f_pag)P;
        w->primera = (w6f_pag)P;
    } else if (w->ultima == P_NIL) w->primera = (w6f_pag)P;
    else fs->sig[w->ultima] = (w6f_pag)P;
    w->escrito0 = 1; w->ultima = (w6f_pag)P; w->npag++; w->pend++; fs->pend++; w->n_buf = 0;
    fs->cambios++; fs->crc_ok = P_NIL;
    return 0;
}

static int x_escribir(W6f *fs, int h, const void *d, uint32_t n)
{
    W6fEscr *w; u32 hecho = 0; int r;
    if ((r = puede_escribir(fs))) return r;
    if (h < 0 || h >= W6F_MAX_ESCR || !fs->escr[h].activo) return W6F_E_ARG;
    w = &fs->escr[h];
    if (n > 0xFFFFFFFFu - w->tam) return W6F_E_LLENO;
    while (hecho < n) {
        u32 t = fs->carga - w->n_buf;
        if (t > n - hecho) t = n - hecho;
        memcpy(w->buf + w->n_buf, (const u8 *)d + hecho, t);
        w->n_buf += t; hecho += t; w->tam += t;
        if (w->n_buf == fs->carga && (r = volcar(fs, w, 0))) { w->n_buf -= t; w->tam -= t; return r; }   /* no entró: este trozo no cuenta */
    }
    return 0;
}

static void x_abortar(W6f *fs, int h)
{
    W6fEscr *w;
    if (!fs->montado || h < 0 || h >= W6F_MAX_ESCR || !fs->escr[h].activo) return;
    w = &fs->escr[h];
    if (w->modo == W6F_CREAR) matar_cadena(fs, w->primera);
    else if (w->escrito0) {
        if (w->tenia_cola0 && !MUT(102)) {             /* la primera página escrita se queda de cola: empieza igual que la vieja */
            matar_cadena(fs, fs->sig[w->primera]); fs->sig[w->primera] = P_NIL; fs->usadas++; fs->pend--; w->pend--;
        } else {
            matar_cadena(fs, w->primera);
            if (w->ant == P_NIL) fs->obj[w->id].primera = P_NIL; else fs->sig[w->ant] = P_NIL;
        }
    }
    soltar(fs, w);
}

static int x_cerrar(W6f *fs, int h)
{
    W6fEscr *w; u32 P, seq, resto, viejas = 0; int r, aparte, existia;
    if ((r = puede_escribir(fs))) return r;
    if (h < 0 || h >= W6F_MAX_ESCR || !fs->escr[h].activo) return W6F_E_ARG;
    w = &fs->escr[h];
    existia = vivo(fs, w->id);
    if (existia) viejas = w->modo == W6F_CREAR ? largo_cadena(fs, fs->obj[w->id].primera) : 0;   /* (de la RAM: el nodo viejo puede estar podrido) */
    else if (!es_dir(fs, w->padre) || buscar_en(fs, w->padre, w->nombre, w->nl) >= 0) { x_abortar(fs, h); return W6F_E_EXISTE; }
    resto = w->n_buf; aparte = resto > fs->cola_max;
    if (aparte && !MUT(2) && (r = volcar(fs, w, 1))) { x_abortar(fs, h); return r; }
    if ((r = sitio(fs, 1))) { x_abortar(fs, h); return r; }
    memcpy(fs->pg + W6F_CAB + NODO_FIJO, w->nombre, w->nl);
    if (!aparte) memcpy(fs->pg + W6F_CAB + NODO_FIJO + w->nl, w->buf, resto);
    r = nodo_emitir(fs, w->id, w->gen, w->padre, 1, w->nl, w->tam, w->fecha, 0xFFFF, aparte ? 0 : resto, aparte, &P, &seq);
    if (r) { x_abortar(fs, h); return r; }
    if (aparte && MUT(2)) { w->n_buf = resto; if ((r = volcar(fs, w, 1))) { x_abortar(fs, h); return r; } }
    vivir(fs, P, P_META);
    if (existia) {
        matar(fs, fs->obj[w->id].nodo);
        if (w->modo == W6F_CREAR) {
            if (!(MUT(3))) matar_cadena(fs, fs->obj[w->id].primera);
            fs->usadas -= viejas; fs->usadas += w->pend;
            fs->obj[w->id].primera = w->primera;
        } else fs->usadas += w->pend;
        fs->obj[w->id].marcas &= (u8)~(M_ESCR | M_DANADO | M_NODO_MALO);
    } else {
        fs->obj[w->id].marcas = 0; fs->obj[w->id].primera = w->primera; fs->obj[w->id].padre = w->padre;
        fs->obj[w->id].hash = hash8((const u8 *)w->nombre, w->nl);
        fs->ids_reservados--; fs->n_obj++; fs->usadas += 1 + w->pend;
    }
    fs->obj[w->id].nodo = (w6f_pag)P; fs->obj[w->id].seq = seq;
    fs->reservadas -= 2; fs->pend -= w->pend;
    w->activo = 0; fs->cambios++; fs->crc_ok = P_NIL;
    return 0;
}

static int x_guardar(W6f *fs, const char *ruta, const void *d, uint32_t n, uint32_t fecha)
{
    int h = x_abrir(fs, ruta, W6F_CREAR, fecha), r;
    if (h < 0) return h;
    if ((r = x_escribir(fs, h, d, n))) { x_abortar(fs, h); return r; }
    return x_cerrar(fs, h);
}

/* ---- operaciones de un solo nodo --------------------------------------------------------------------- */
static int x_truncar(W6f *fs, const char *ruta, uint32_t tam)
{
    Nodo n; int r, id; u32 nd, nd2, k2, r2, P, seq, cola = 0, aparte = 0;
    if ((r = puede_escribir(fs))) return r;
    if ((id = w6f_buscar(fs, ruta)) < 0) return id;
    if (id == 0 || (fs->obj[id].marcas & M_DIR)) return W6F_E_ES_DIR;
    if (fs->obj[id].marcas & M_ESCR) return W6F_E_OCUPADO;
    if (fs->obj[id].marcas & M_DANADO) return W6F_E_DANADO;
    if ((fs->obj[id].marcas & M_NODO_MALO) || !nodo_bueno(fs, fs->obj[id].nodo) || nodo_leer(fs, fs->obj[id].nodo, &n)) { fs->obj[id].marcas |= M_NODO_MALO; return W6F_E_CRC; }
    if (tam > n.tam) return W6F_E_ARG;
    if (tam == n.tam) return 0;
    if ((r = sitio(fs, 1))) return r;
    nd = n_datos(fs, &n); k2 = tam / fs->carga; r2 = tam % fs->carga;
    if (fl_leer(fs, fs->obj[id].nodo, W6F_CAB + NODO_FIJO, fs->pg + W6F_CAB + NODO_FIJO, n.nl + n.cola_len)) return W6F_E_FLASH;
    if (r2 > fs->cola_max) { if (k2 >= nd) return W6F_E_DANADO; aparte = 1; }
    else if (r2) {
        cola = r2;
        if (k2 < nd) {                                 /* la cola nueva sale de una página de datos */
            u32 Pd = pagina_de(fs, (u32)id, k2);
            if (Pd == NINGUNO) return W6F_E_DANADO;
            if (!dato_bueno(fs, Pd)) return W6F_E_CRC;
            if (fl_leer(fs, Pd, W6F_CAB, fs->pg + W6F_CAB + NODO_FIJO + n.nl, r2)) return W6F_E_FLASH;
        }
    }
    nd2 = k2 + aparte;
    if ((r = nodo_emitir(fs, (u32)id, n.c.gen, n.padre, 1, n.nl, tam, n.fecha, 0xFFFF, cola, (int)aparte, &P, &seq))) return r;
    vivir(fs, P, P_META); matar(fs, fs->obj[id].nodo);
    fs->obj[id].nodo = (w6f_pag)P; fs->obj[id].seq = seq;
    if (nd2 == 0) { if (!MUT(105)) matar_cadena(fs, fs->obj[id].primera); fs->obj[id].primera = P_NIL; }
    else {
        u32 Pu = pagina_de(fs, (u32)id, nd2 - 1);
        if (Pu != NINGUNO) { if (!MUT(105)) matar_cadena(fs, fs->sig[Pu]); fs->sig[Pu] = P_NIL; }
    }
    fs->usadas -= nd - nd2; fs->cambios++; fs->crc_ok = P_NIL;
    return 0;
}

static int x_mkdir(W6f *fs, const char *ruta, uint32_t fecha)
{
    u32 padre, nl, prof, P, seq; const char *nom; int r, id;
    if ((r = puede_escribir(fs))) return r;
    if ((r = resolver(fs, ruta, &padre, &nom, &nl, &prof))) return r;
    if (nl == 0 || buscar_en(fs, padre, nom, nl) >= 0) return W6F_E_EXISTE;
    if (nombre_en_uso(fs, padre, nom, nl)) return W6F_E_OCUPADO;
    sitio(fs, 0);
    if ((id = id_libre(fs)) < 0) return id;
    if (libres_usuario(fs) < 1) return W6F_E_LLENO;
    if ((r = sitio(fs, 1))) return r;
    memcpy(fs->pg + W6F_CAB + NODO_FIJO, nom, nl);
    if ((r = nodo_emitir(fs, (u32)id, 0, padre, 2, nl, 0, fecha, 0xFFFF, 0, 0, &P, &seq))) return r;
    vivir(fs, P, P_META);
    fs->obj[id].nodo = (w6f_pag)P; fs->obj[id].seq = seq; fs->obj[id].primera = P_NIL; fs->obj[id].padre = (u16)padre;
    fs->obj[id].hash = hash8((const u8 *)nom, nl); fs->obj[id].marcas = M_DIR;
    fs->n_obj++; fs->usadas++;
    return 0;
}

/* Quita un objeto de la tabla y da por muertas sus páginas (lo que lo hace definitivo es el punto de control). */
static void quitar(W6f *fs, u32 id)
{
    u32 pag = 1, pasos = 0; w6f_pag p = fs->obj[id].primera;
    while (p != P_NIL && p < fs->n_pag && pasos++ <= fs->n_pag) { pag++; p = fs->sig[p]; }
    matar_cadena(fs, fs->obj[id].primera);
    matar(fs, fs->obj[id].nodo);
    fs->obj[id].nodo = P_NIL; fs->obj[id].primera = P_NIL; fs->obj[id].marcas = 0;
    fs->usadas -= pag; fs->n_obj--; fs->cambios++; fs->crc_ok = P_NIL; fs->repo_intentos = 0;
}

static int x_borrar(W6f *fs, const char *ruta)
{
    int r, id; u32 x; W6fObj copia;
    if ((r = puede_escribir(fs))) return r;
    if ((id = w6f_buscar(fs, ruta)) < 0) return id;
    if (id == 0) return W6F_E_ARG;
    if (fs->obj[id].marcas & M_ESCR) return W6F_E_OCUPADO;
    if (!MUT(106) && (fs->obj[id].marcas & M_DIR)) {
        for (x = 1; x < fs->conf.max_obj; x++) if (fs->obj[x].nodo != P_NIL && fs->obj[x].padre == id) return W6F_E_NO_VACIO;
        for (x = 0; x < W6F_MAX_ESCR; x++) if (fs->escr[x].activo && fs->escr[x].padre == id) return W6F_E_OCUPADO;
    }
    /* borrar es URGENTE: su punto de control puede gastar hasta la última página en blanco y el último bloque libre.
     * (N1) Si no sale a la primera se REINTENTA: `sitio` borra lo que no tiene nada vivo y repone; un fallo pasajero de la
     * flash (o un bloque libre que no se deja abrir) no es «no hay salida». */
    fs->urgente = 1;
    for (x = 0, r = W6F_E_FLASH; x < (MUT(25) ? 1u : 6u) && r; x++) {
        if (x >= 3 && !rehabilitar(fs) && !hay_salida(fs)) break;      /* tres intentos; y otros tres dando otra oportunidad a los bloques apartados solo en RAM */
        r = sitio(fs, 1);
        if (!r && !MUT(7)) {
            copia = fs->obj[id];
            fs->obj[id].nodo = P_NIL;                  /* el punto de control se escribe SIN él… */
            r = cp_emitir(fs, (u32)id / fs->por_rango);
            fs->obj[id] = copia;                       /* …y solo si quedó escrito se quita de verdad */
        }
        if (r == W6F_E_SOLO_LECTURA) break;
    }
    fs->urgente = 0;
    if (r) {
        /* solo lectura «sin reserva» ÚNICAMENTE si de verdad no queda nada: ni bloque libre, ni sitio, ni nada que borrar.
         * Si no, ha fallado ESTA operación (W6F_E_FLASH): nada ha cambiado y se puede reintentar. */
        if (r != W6F_E_SOLO_LECTURA && (MUT(25) || (!hay_salida(fs) && !rehabilitar(fs)))) { fs->solo_lectura = 1; fs->motivo_sl = W6F_SL_SIN_RESERVA; return W6F_E_SOLO_LECTURA; }
        return r == W6F_E_LLENO ? W6F_E_FLASH : r;
    }
    quitar(fs, (u32)id);
    sitio(fs, 0);                                      /* si el punto de control gastó un bloque de la reserva, se repone ya */
    return 0;
}

static int x_renombrar(W6f *fs, const char *vieja, const char *nueva, int reemplazar)
{
    u32 padre, nl, prof, P, seq, x; const char *nom; int r, id, dst; Nodo n;
    if ((r = puede_escribir(fs))) return r;
    if ((id = w6f_buscar(fs, vieja)) < 0) return id;
    if (id == 0) return W6F_E_ARG;
    if (fs->obj[id].marcas & M_ESCR) return W6F_E_OCUPADO;
    if ((r = resolver(fs, nueva, &padre, &nom, &nl, &prof))) return r;
    if (nl == 0) return W6F_E_EXISTE;
    dst = buscar_en(fs, padre, nom, nl);
    if (dst == id) return 0;
    if (dst >= 0) {
        if (!reemplazar) return W6F_E_EXISTE;
        if ((fs->obj[id].marcas & M_DIR) || (fs->obj[dst].marcas & M_DIR)) return W6F_E_ES_DIR;
        if (fs->obj[dst].marcas & M_ESCR) return W6F_E_OCUPADO;
    } else if (nombre_en_uso(fs, padre, nom, nl)) return W6F_E_OCUPADO;
    if (fs->obj[id].marcas & M_DIR) {
        for (x = padre; x; x = fs->obj[x].padre) if (x == (u32)id) return W6F_E_ARG;      /* dentro de sí mismo */
        if (prof + altura(fs, (u32)id) > W6F_PROF_MAX) return W6F_E_PROFUNDO;
        for (x = 0; x < W6F_MAX_ESCR; x++) if (fs->escr[x].activo && fs->escr[x].padre == id) return W6F_E_OCUPADO;
    }
    /* (M1) el nodo nuevo se hace con lo que dice el viejo: tiene que pasar su CRC ahora */
    if ((fs->obj[id].marcas & M_NODO_MALO) || !nodo_bueno(fs, fs->obj[id].nodo) || nodo_leer(fs, fs->obj[id].nodo, &n)) { fs->obj[id].marcas |= M_NODO_MALO; return W6F_E_CRC; }
    if (MUT(8) && dst >= 0) { if ((r = x_borrar(fs, nueva))) return r; dst = -1; }
    if ((r = sitio(fs, dst >= 0 ? 2 : 1))) return r;
    if (fl_leer(fs, fs->obj[id].nodo, W6F_CAB + NODO_FIJO + n.nl, fs->pg + W6F_CAB + NODO_FIJO + nl, n.cola_len)) return W6F_E_FLASH;
    memcpy(fs->pg + W6F_CAB + NODO_FIJO, nom, nl);
    if ((r = nodo_emitir(fs, (u32)id, n.c.gen, padre, n.clase, nl, n.tam, n.fecha, dst >= 0 ? (u32)dst : 0xFFFF,
                         n.cola_len, n.cola_aparte, &P, &seq))) return r;
    vivir(fs, P, P_META); matar(fs, fs->obj[id].nodo);
    fs->obj[id].nodo = (w6f_pag)P; fs->obj[id].seq = seq; fs->obj[id].padre = (u16)padre; fs->obj[id].hash = hash8((const u8 *)nom, nl);
    if (dst >= 0) {                                    /* el nodo ya «mata» al destino; el punto de control lo remata */
        quitar(fs, (u32)dst);
        fs->urgente = 1;
        for (x = 0, r = W6F_E_FLASH; x < 4 && r; x++) if (!(r = sitio(fs, 1))) r = cp_emitir(fs, (u32)dst / fs->por_rango);
        fs->urgente = 0;
        /* el renombrado YA vale (el nodo «mata» al pisado); lo que se debe es su punto de control: solo lectura hasta que
         * se pueda escribir (se reintenta solo: `revaluar`) */
        if (r) { cp_pendiente(fs, (u32)dst / fs->por_rango); fs->solo_lectura = 1; fs->motivo_sl = W6F_SL_FLASH; return W6F_E_FLASH; }
    }
    return 0;
}

void w6f_estado(W6f *fs, W6fEstado *e)
{
    u32 b, L;
    memset(e, 0, sizeof *e);
    if (!fs->montado) return;
    L = libres_usuario(fs);
    e->paginas_cap = fs->cap; e->paginas_usadas = fs->usadas + fs->pend + fs->reservadas; e->paginas_libres = L;
    e->bytes_libres = L >= 2 ? (L - 2) * fs->carga + fs->carga - 1 : 0;
    e->objetos = fs->n_obj; e->max_obj = fs->conf.max_obj - 1; e->bloques = fs->fl->n_bloques; e->bloques_libres = fs->libres;
    e->borrados_min = SIN_CUENTA; e->paginas_blancas = disponibles(fs);
    e->solo_lectura = fs->solo_lectura ? (fs->motivo_sl ? fs->motivo_sl : W6F_SL_PEDIDO) : 0; e->deficit = libres_previstos(fs) < fs->libres_min;
    for (b = 0; b < fs->fl->n_bloques; b++) {
        const W6fBlq *q = &fs->blq[b];
        if (q->estado == B_MALO) { e->bloques_malos++; continue; }
        if (q->estado == B_USADO || q->estado == B_SUCIO) e->paginas_muertas += fs->fl->ppb - 1 - q->vivas;
        if (q->borrados == SIN_CUENTA) continue;
        if (q->borrados < e->borrados_min) e->borrados_min = q->borrados;
        if (q->borrados > e->borrados_max) e->borrados_max = q->borrados;
        e->borrados_suma += q->borrados;
    }
    if (e->borrados_min == SIN_CUENTA) e->borrados_min = 0;
}

/* ---- memoria y geometría ----------------------------------------------------------------------------- */
static int geometria(const W6fFlash *fl, const W6fConf *c)
{
    if (!fl || !c || !fl->leer || !fl->programar || !fl->borrar) return 0;
    if (fl->pag < 512 || fl->pag > 4096 || (fl->pag & 3) || fl->ppb < 8 || fl->ppb > 4096) return 0;
    /* (A1) reserva mínima 3: con menos no hay bloque de repuesto y el primer bloque que muera al borrarlo deja el volumen sin destino */
    if (c->max_obj < 2 || c->max_obj > 65535 || c->reserva_bloques < 3 || c->reserva_bloques > 4096 || fl->n_bloques > 65536 ||
        fl->n_bloques < c->reserva_bloques + 2) return 0;
    if ((uint64_t)fl->n_bloques * fl->ppb > P_TOPE) return 0;
    return 1;
}
#define ALIN(x) (((x) + 7u) & ~7u)
uint32_t w6f_mem_necesaria(const W6fFlash *fl, const W6fConf *c)
{
    u32 np, carga;
    if (!geometria(fl, c)) return 0;
    np = fl->n_bloques * fl->ppb; carga = fl->pag - W6F_CAB;
    return ALIN(np * (u32)sizeof(w6f_pag)) + ALIN(c->max_obj * (u32)sizeof(W6fObj)) + ALIN(fl->n_bloques * (u32)sizeof(W6fBlq)) +
           ALIN(((c->max_obj + carga / 4 - 1) / (carga / 4)) * (u32)sizeof(w6f_pag)) + 2 * ALIN(fl->pag) + W6F_MAX_ESCR * ALIN(carga);
}
static int repartir(W6f *fs, const W6fFlash *fl, const W6fConf *conf, void *mem, u32 mem_bytes)
{
    u8 *m = mem; u32 i; W6fConf cf;
    if (!geometria(fl, conf)) return W6F_E_GEOMETRIA;
    if (!mem || ((uintptr_t)mem & 3) || mem_bytes < w6f_mem_necesaria(fl, conf)) return W6F_E_MEMORIA;
    cf = *conf; conf = &cf;                            /* (puede venir de dentro del propio fs) */
    memset(fs, 0, sizeof *fs);
    fs->fl = fl; fs->conf = *conf;
    if (!fs->conf.umbral_desgaste) fs->conf.umbral_desgaste = 8;
    fs->n_pag = fl->n_bloques * fl->ppb; fs->carga = fl->pag - W6F_CAB; fs->cola_max = fs->carga - NODO_FIJO - W6F_NOMBRE_MAX - 1;
    fs->por_rango = fs->carga / 4; fs->n_rangos = (conf->max_obj + fs->por_rango - 1) / fs->por_rango;
    fs->sig = (w6f_pag *)(void *)m; m += ALIN(fs->n_pag * (u32)sizeof(w6f_pag));
    fs->obj = (W6fObj *)(void *)m;  m += ALIN(conf->max_obj * (u32)sizeof(W6fObj));
    fs->blq = (W6fBlq *)(void *)m;  m += ALIN(fl->n_bloques * (u32)sizeof(W6fBlq));
    fs->cp = (w6f_pag *)(void *)m;  m += ALIN(fs->n_rangos * (u32)sizeof(w6f_pag));
    fs->pg = m;                     m += ALIN(fl->pag);
    fs->pgb = m;                    m += ALIN(fl->pag);
    for (i = 0; i < W6F_MAX_ESCR; i++) { fs->escr[i].buf = m; m += ALIN(fs->carga); }
    fs->solo_lectura = conf->solo_lectura != 0;
    fs->cabeza = NINGUNO; fs->victima = NINGUNO; fs->parcial = NINGUNO; fs->destino = NINGUNO;
    fs->libres_min = 2; fs->cur_pag = P_NIL; fs->crc_ok = P_NIL; fs->nodo_ok = P_NIL; fs->gc_compromiso = NINGUNO;
    if (fs->solo_lectura) fs->motivo_sl = W6F_SL_PEDIDO;
    return 0;
}

/* ---- montaje ----------------------------------------------------------------------------------------- */
static uint64_t clave(W6f *fs, u32 P)                  /* orden de la cadena: idx creciente y, a igual idx, seq decreciente */
{
    u8 t[8];
    if (fl_leer(fs, P, 12, t, 8)) return ~(uint64_t)0;
    return ((uint64_t)g32(t) << 32) | (0xFFFFFFFFu - g32(t + 4));
}
static w6f_pag ordenar(W6f *fs, w6f_pag lista)         /* mezcla de abajo arriba sobre la lista enlazada: O(n log n), sin memoria */
{
    u32 tramo = 1;
    w6f_pag p = lista; uint64_t ant = 0; int ordenada = 1;
    while (p != P_NIL) { uint64_t k = clave(fs, p); if (k < ant) { ordenada = 0; break; } ant = k; p = fs->sig[p]; }
    if (ordenada) return lista;
    for (;;) {
        w6f_pag cola = P_NIL, q; u32 mezclas = 0;
        p = lista; lista = P_NIL;
        while (p != P_NIL) {
            u32 np = 0, nq = tramo, i; uint64_t kp = 0, kq = 0; int hp = 0, hq = 0;
            mezclas++;
            q = p;
            for (i = 0; i < tramo && q != P_NIL; i++) { np++; q = fs->sig[q]; }
            while (np || (nq && q != P_NIL)) {
                w6f_pag e;
                if (!np) { e = q; q = fs->sig[q]; nq--; hq = 0; }
                else if (!nq || q == P_NIL) { e = p; p = fs->sig[p]; np--; hp = 0; }
                else {
                    if (!hp) { kp = clave(fs, p); hp = 1; }
                    if (!hq) { kq = clave(fs, q); hq = 1; }
                    if (kp <= kq) { e = p; p = fs->sig[p]; np--; hp = 0; }
                    else { e = q; q = fs->sig[q]; nq--; hq = 0; }
                }
                if (cola != P_NIL) fs->sig[cola] = e; else lista = e;
                cola = e;
            }
            p = q;
        }
        if (cola != P_NIL) fs->sig[cola] = P_NIL;
        if (mezclas <= 1) return lista;
        tramo *= 2;
    }
}

/* Deja la cadena de un fichero con una página por idx (la de mayor seq con CRC bueno), dice cuántas hay y si está entera.
 * (M2) Si le faltan páginas la cadena se queda con las que hay: se pueden leer, y no se dan por muertas. */
static int depurar(W6f *fs, u32 id, u32 nd, u32 *presentes)
{
    w6f_pag p = fs->obj[id].primera, *hueco = &fs->obj[id].primera; u32 espera = 0, n = 0; int seguida = 1;
    while (p != P_NIL) {
        u32 idx = (u32)(clave(fs, p) >> 32); w6f_pag q = fs->sig[p], elegida = p; int varias = 0;
        while (q != P_NIL && (u32)(clave(fs, q) >> 32) == idx) { varias = 1; q = fs->sig[q]; }
        if (varias) {                                  /* hay duplicadas: decide el CRC, de la más nueva a la más vieja */
            w6f_pag c = p; elegida = P_NIL;
            if (MUT(6)) { while (fs->sig[c] != q) c = fs->sig[c]; elegida = c; }
            else for (c = p; c != q; c = fs->sig[c]) if (dato_bueno(fs, c)) { elegida = c; break; }
            if (elegida == P_NIL) elegida = p;
            for (c = p; c != q; ) { w6f_pag s2 = fs->sig[c]; if (c != elegida) fs->sig[c] = P_MUERTA; c = s2; }
        }
        if (idx != espera) seguida = 0;
        *hueco = elegida; fs->sig[elegida] = q; hueco = &fs->sig[elegida];
        espera = idx + 1; n++; p = q;
    }
    *presentes = n;
    return MUT(113) || (seguida && espera == nd);
}

int w6f_montar(W6f *fs, const W6fFlash *fl, const W6fConf *conf, void *mem, uint32_t mem_bytes)
{
    u32 b, P, id, ppb, cabeceras = 0, no_blancas = 0, formato = 0, maxseq = 0, r_ant = NINGUNO, S_ant = 0, futuras = 0, encogida = 0, fuera = 0;
    int r;
    u8 h[W6F_CAB + NODO_FIJO]; Cab c;
    if ((r = repartir(fs, fl, conf, mem, mem_bytes))) return r;
    conf = &fs->conf;
    ppb = fl->ppb;
    for (P = 0; P < fs->n_pag; P++) fs->sig[P] = P_MUERTA;
    memset(fs->obj, 0, conf->max_obj * sizeof *fs->obj); memset(fs->blq, 0, fl->n_bloques * sizeof *fs->blq);
    for (id = 0; id < conf->max_obj; id++) { fs->obj[id].nodo = P_NIL; fs->obj[id].primera = P_NIL; }
    for (id = 0; id < fs->n_rangos; id++) fs->cp[id] = P_NIL;

    /* 1. todas las cabeceras: bloques, puntos de control, candidatas; y los lotes de la recolección */
    for (b = 0; b < fl->n_bloques; b++) {
        W6fBlq *q = &fs->blq[b]; u32 ultima = 0, k, pend = NINGUNO, j;
        q->borrados = SIN_CUENTA; q->estado = B_SUCIO; q->fallos = 0; q->vivas = 0;
        if (fl->es_malo && fl->es_malo(fl->ctx, b)) { q->estado = B_MALO; continue; }
        fs->utiles++;
        for (k = 0; k < ppb; k++) {
            int v;
            P = b * ppb + k;
            if (fl_leer(fs, P, 0, h, W6F_CAB)) { no_blancas++; ultima = k; continue; }
            v = cab_leer(h, &c);
            if (v == 2) continue;
            no_blancas++; ultima = k;
            if (v == 3) { futuras++; continue; }
            if (v != 1) continue;
            if (c.tipo == 'B') {
                u8 g[20];
                if (k == 0 && !fl_leer(fs, P, W6F_CAB, g, 20) && crc32_mas(0, g, 20) == c.dcrc && g32(g) == fl->pag && g32(g + 4) == ppb) {
                    q->borrados = c.gen < 0x01000000u ? c.gen : SIN_CUENTA; q->estado = B_USADO; cabeceras++;   /* una cuenta absurda no vale */
                    if (g32(g + 8) > fl->n_bloques) encogida = 1;      /* (A3) se le dio formato con MÁS bloques que los de ahora */
                }
                continue;
            }
            if (c.tipo == 'F') { formato = 1; continue; }
            if (c.seq > maxseq && !(MUT(10) && c.tipo == 'D')) maxseq = c.seq;
            if ((c.tipo == 'D' || c.tipo == 'N') && c.gen < SEQ_NULA && c.gen > maxseq && !MUT(10)) maxseq = c.gen;
            /* la página que compromete un lote tiene que estar ENTERA (un corte puede dejarle la cabecera buena y la carga a medias) */
            if (c.lote == 3 && !MUT(14) && !entera(fs, P, &c)) continue;
            if (c.tipo == 'D') fs->sig[P] = c.lote ? P_TDG : P_TD;
            else if (c.tipo == 'N') fs->sig[P] = c.lote ? P_TNG : P_TN;
            else if (c.tipo == 'C') fs->sig[P] = P_TCG;
            if (c.lote && pend == NINGUNO && !MUT(13)) pend = k;
            if (c.lote == 3 || c.lote == 0 || MUT(13)) {       /* compromiso (o página normal): lo pendiente hasta aquí, si es de lote y hay compromiso, vale */
                u32 desde = (c.lote == 3 || MUT(13)) ? (pend == NINGUNO ? k : pend) : k;
                for (j = desde; j <= k; j++) {
                    u32 Pj = b * ppb + j;
                    if (fs->sig[Pj] == P_TDG) fs->sig[Pj] = P_TD;
                    else if (fs->sig[Pj] == P_TNG) fs->sig[Pj] = P_TN;
                    else if (fs->sig[Pj] == P_TCG) {
                        Cab cc; u32 crc, S = 0; u8 t[4];
                        fs->sig[Pj] = P_MUERTA;
                        if (fl_leer(fs, Pj, 0, h, W6F_CAB) || cab_leer(h, &cc) != 1 || cc.id >= fs->n_rangos || cc.gen != fs->por_rango) continue;
                        if (crc_carga(fs, Pj, fs->por_rango * 4, &crc) || crc != cc.dcrc) continue;
                        if (fs->cp[cc.id] != P_NIL) S = fl_leer(fs, fs->cp[cc.id], 16, t, 4) ? 0 : g32(t);
                        if (fs->cp[cc.id] == P_NIL || cc.seq > S) fs->cp[cc.id] = (w6f_pag)Pj;
                    }
                }
                if (c.lote == 3 || MUT(13)) pend = NINGUNO;
            }
        }
        if (pend != NINGUNO) {                         /* un lote sin compromiso: la recolección se cortó; esas copias no valen */
            for (j = pend; j < ppb; j++) { P = b * ppb + j; if (fs->sig[P] == P_TDG || fs->sig[P] == P_TNG || fs->sig[P] == P_TCG) fs->sig[P] = P_MUERTA; }
            q->fallos = 0x80;                          /* y en este bloque no se sigue escribiendo: se borrará */
        }
        q->vivas = (u16)ultima;                        /* provisional: la última página no blanca */
    }
    if (formato) return W6F_E_FORMATO;
    if (!cabeceras) {
        if (futuras) return W6F_E_VERSION;
        if (no_blancas) return W6F_E_AJENO;
        for (P = 0; P < fs->n_pag; P++) if (fs->blq[P / ppb].estado != B_MALO && !blanca(fs, P)) return W6F_E_AJENO;
        return W6F_E_VIRGEN;
    }
    if (futuras) return W6F_E_VERSION;                 /* (B4) hay páginas de un formato posterior: no es «ajeno» ni se monta a medias */
    if (encogida) return W6F_E_GEOMETRIA;
    fs->seq = maxseq + (MUT(104) ? 0 : 1);
    if (maxseq >= SEQ_TOPE) { fs->solo_lectura = 1; fs->motivo_sl = W6F_SL_SECUENCIA; }   /* (M3) */

    /* 2. nodos: por cada id gana el de mayor seq que respete su punto de control */
    for (P = 0; P < fs->n_pag; P++) {
        Nodo n; u32 crc, rg; W6fObj *o; u8 nom[W6F_NOMBRE_MAX]; int rn;
        if (fs->sig[P] != P_TN) continue;
        fs->sig[P] = P_MUERTA;
        if ((rn = nodo_leer(fs, P, &n)) == -1) continue;
        if (!MUT(1) && (crc_carga(fs, P, NODO_FIJO + n.nl + n.cola_len, &crc) || crc != n.c.dcrc)) continue;
        if (rn == -2) { fuera++; continue; }           /* (A3) un nodo BUENO de un objeto que no cabe en esta tabla */
        if (fl_leer(fs, P, W6F_CAB + NODO_FIJO, nom, n.nl) || !nombre_valido(nom, n.nl)) continue;
        rg = n.c.id / fs->por_rango;
        if (fs->cp[rg] != P_NIL) {
            u8 t[4]; u32 S;
            if (rg != r_ant) { r_ant = rg; S_ant = fl_leer(fs, fs->cp[rg], 16, t, 4) ? 0 : g32(t); }
            S = S_ant;
            /* anterior al punto de control: vale solo si el punto de control da por vivo al objeto y el nodo no es más
             * viejo que el que apuntó (puede ser más nuevo: una copia de la recolección hecha entre medias) */
            if (n.c.seq < S && (fl_leer(fs, fs->cp[rg], W6F_CAB + 4 * (n.c.id % fs->por_rango), t, 4) || g32(t) == 0 || (MUT(15) ? n.c.seq != g32(t) : n.c.seq < g32(t)))) continue;
        }
        o = &fs->obj[n.c.id];
        if (o->nodo != P_NIL && o->seq >= n.c.seq) continue;
        if (o->nodo != P_NIL) fs->sig[o->nodo] = P_MUERTA;
        o->nodo = (w6f_pag)P; o->seq = n.c.seq; o->padre = n.padre; o->hash = hash8(nom, n.nl);
        o->marcas = (u8)((n.clase == 2 ? M_DIR : 0) | (n.c.danada ? M_NODO_MALO : 0));
        fs->sig[P] = P_META;
    }
    /* (A3) montar con menos objetos de los que el volumen tiene no es «perder unos cuantos»: es un error */
    if (fuera) return W6F_E_GEOMETRIA;
    for (id = 0; id < fs->n_rangos; id++) if (fs->cp[id] != P_NIL) fs->sig[fs->cp[id]] = P_META;
    /* 2b. «mata»: un renombrado encima de otro al que el corte no dejó escribir su punto de control */
    for (id = 1; id < conf->max_obj; id++) {
        Nodo n;
        if (fs->obj[id].nodo == P_NIL || nodo_leer(fs, fs->obj[id].nodo, &n) || n.mata == 0xFFFF || n.mata == id) continue;
        if (fs->obj[n.mata].nodo != P_NIL && fs->obj[n.mata].seq < fs->obj[id].seq) fs->obj[n.mata].marcas |= M_CONDENADO;
    }
    for (id = 1; id < conf->max_obj; id++)
        if (fs->obj[id].nodo != P_NIL && (fs->obj[id].marcas & M_CONDENADO)) {
            fs->sig[fs->obj[id].nodo] = P_MUERTA; fs->obj[id].nodo = P_NIL; fs->obj[id].marcas = 0;
            cp_pendiente(fs, id / fs->por_rango);
        }
    /* 2c. huérfanos y ciclos: lo que no llega a la raíz por directorios vivos se cuelga de la raíz */
    for (id = 1; id < conf->max_obj; id++) {
        u32 x = id, n = 0;
        if (fs->obj[id].nodo == P_NIL) continue;
        while (x && n <= W6F_PROF_MAX) {
            u32 pa = fs->obj[x].padre;
            if (pa && (fs->obj[pa].nodo == P_NIL || !(fs->obj[pa].marcas & M_DIR))) break;
            x = pa; n++;
        }
        if (x && !MUT(107)) { fs->obj[id].padre = 0; fs->cta.danados++; }
    }

    /* 3. datos: de atrás adelante, para que las listas salgan casi ordenadas */
    {
        u32 uid = 0, ugen = 0, und = 0;
        for (P = fs->n_pag; P-- > 0; ) {
            W6fObj *o;
            if (fs->sig[P] != P_TD) continue;
            fs->sig[P] = P_MUERTA;
            if (fl_leer(fs, P, 0, h, W6F_CAB) || cab_leer(h, &c) != 1) continue;
            if (c.id == 0 || c.id >= conf->max_obj) continue;
            o = &fs->obj[c.id];
            if (o->nodo == P_NIL || (o->marcas & M_DIR)) continue;
            if (c.id != uid) { Nodo n; if (nodo_leer(fs, o->nodo, &n)) continue; uid = c.id; ugen = n.c.gen; und = n_datos(fs, &n); }
            if (c.gen != ugen || c.idx >= und) continue;
            fs->sig[P] = o->primera; o->primera = (w6f_pag)P;
        }
    }
    for (id = 1; id < conf->max_obj; id++) {
        W6fObj *o = &fs->obj[id]; Nodo n; u32 presentes = 0;
        if (o->nodo == P_NIL) continue;
        fs->n_obj++; fs->usadas++;
        if (o->marcas & M_NODO_MALO) fs->cta.danados++;
        if (o->marcas & M_DIR) continue;
        if (nodo_leer(fs, o->nodo, &n)) { o->marcas |= M_NODO_MALO; fs->cta.danados++; continue; }
        o->primera = ordenar(fs, o->primera);
        if (!depurar(fs, id, n_datos(fs, &n), &presentes)) { o->marcas |= M_DANADO; fs->cta.danados++; }   /* (M2) se queda con las que hay */
        fs->usadas += presentes;
    }

    /* 4. bloques: vivas y libres. (M7) Por omisión NO se sigue escribiendo en ningún bloque a medias heredado: su primera
     * página en blanco puede ser la que se estaba programando cuando se fue la corriente (se cierran: su resto es
     * basura que la recolección recupera borrando). Con conf.reusar_blanco, se sigue en las dos con más sitio. */
    for (b = 0; b < fl->n_bloques; b++) {
        W6fBlq *q = &fs->blq[b]; u32 ultima = q->vivas, k, v = 0, abortado = q->fallos & 0x80, ini;
        q->fallos = 0;
        if (q->estado == B_MALO) { q->vivas = 0; continue; }
        for (k = 0; k < ppb; k++) if (fs->sig[b * ppb + k] != P_MUERTA) v++;
        q->vivas = (u16)v;
        if (q->estado != B_USADO) continue;            /* sin cabecera: sucio, se borrará antes de usarlo */
        q->fallos = SIN_MIRAR;
        if (ultima == 0) { q->estado = B_LIBRE; fs->libres++; continue; }
        ini = ultima + 1;
        if (abortado || (!conf->reusar_blanco && !MUT(23)) || ini >= ppb || (v == 0 && !MUT(16))) continue;     /* sin nada vivo: mejor borrarlo que seguir en él (y es la reserva) */
        if (fs->cabeza == NINGUNO || ini < fs->llenado) {
            fs->parcial = fs->cabeza; fs->parcial_llenado = fs->llenado; fs->cabeza = b; fs->llenado = ini;
        } else if (fs->parcial == NINGUNO || ini < fs->parcial_llenado) { fs->parcial = b; fs->parcial_llenado = ini; }
    }
    if (fs->cabeza != NINGUNO) fs->blq[fs->cabeza].estado = B_CABEZA;
    if (fs->parcial != NINGUNO) fs->blq[fs->parcial].estado = B_CABEZA;
    calcular_cap(fs);
    fs->montado = 1;
    /* (A1) si no queda ni un bloque libre, ni sitio en una cabeza, ni nada que borrar: solo lectura, y dicho */
    if (!fs->solo_lectura && !hay_salida(fs)) { fs->solo_lectura = 1; fs->motivo_sl = W6F_SL_SIN_RESERVA; }

    /* 5. rematar los «mata» pendientes: su punto de control, antes de que nada pueda superar al nodo que mata */
    if (fs->pend_cp && !MUT(112)) cp_pendientes(fs); else if (MUT(112)) fs->pend_cp = 0;
    return 0;
}

/* ---- formato ----------------------------------------------------------------------------------------- */
static void cab_bloque(W6f *fs, const W6fFlash *fl, const W6fConf *conf, u32 cuenta)
{
    u8 *p = fs->pg + W6F_CAB;
    memset(fs->pg, 0xFF, fl->pag);
    p32(p, fl->pag); p32(p + 4, fl->ppb); p32(p + 8, fl->n_bloques); p32(p + 12, fl->clase); p32(p + 16, conf->max_obj);
    cab_poner(fs->pg, 'B', 0, cuenta, 0, 0, crc32_mas(0, p, 20));
}
int w6f_formatear(const W6fFlash *fl, const W6fConf *conf, void *mem, uint32_t mem_bytes,
                  void (*paso)(void *ctx, uint32_t hechos, uint32_t total), void *ctx)
{
    W6f f, *fs = &f; u32 b, k, ppb, marca = NINGUNO, buenos = 0, vuelta;
    u8 h[W6F_CAB + 20]; Cab c; int r;
    if (conf && conf->solo_lectura) return W6F_E_SOLO_LECTURA;
    if ((r = repartir(fs, fl, conf, mem, mem_bytes))) return r;
    ppb = fl->ppb;
    /* ¿hay ya una marca de formato (venimos de un corte)? Si no, se pone una donde quepa: así un formato cortado no
     * deja montado medio volumen viejo. El bloque de la marca se borra el último. */
    for (b = 0; b < fl->n_bloques && marca == NINGUNO; b++) {
        if (fl->es_malo && fl->es_malo(fl->ctx, b)) continue;
        for (k = 1; k < ppb; k++)
            if (!fl_leer(fs, b * ppb + k, 0, h, W6F_CAB) && cab_leer(h, &c) == 1 && c.tipo == 'F') { marca = b; break; }
    }
    for (b = 0; b < fl->n_bloques && marca == NINGUNO; b++) {
        u32 ultima = 0;
        if (fl->es_malo && fl->es_malo(fl->ctx, b)) continue;
        if (fl_leer(fs, b * ppb, 0, h, W6F_CAB) || cab_leer(h, &c) != 1 || c.tipo != 'B') continue;
        for (k = 1; k < ppb; k++) if (!blanca(fs, b * ppb + k)) ultima = k;       /* enteras: nunca se programa detrás de restos */
        if (!conf->reusar_blanco) break;                                        /* (M7) no se programa blanco heredado: la marca irá en un bloque recién borrado */
        if (ultima == 0 || ultima + 1 >= ppb) continue;                         /* vacío no vale: no hay nada que tapar */
        memset(fs->pg, 0xFF, fl->pag);
        cab_poner(fs->pg, 'F', 0, 0, 0, 0, 0);
        fl->programar(fl->ctx, b, ultima + 1, fs->pg);
        if (coincide(fs, b * ppb + ultima + 1)) marca = b;
    }
    for (b = 0; b < fl->n_bloques && marca == NINGUNO; b++) {     /* no cabía en ningún sitio: se le hace sitio en el primer bloque nuestro */
        if (fl->es_malo && fl->es_malo(fl->ctx, b)) continue;
        if (fl_leer(fs, b * ppb, 0, h, W6F_CAB) || cab_leer(h, &c) != 1 || c.tipo != 'B') continue;
        for (k = 1; k < ppb; k++) if (!blanca(fs, b * ppb + k)) break;
        if (k == ppb) continue;                        /* bloque nuestro y vacío: nada que tapar */
        if (fl->borrar(fl->ctx, b) || !blanca(fs, b * ppb) || !blanca(fs, b * ppb + 1)) continue;
        cab_bloque(fs, fl, conf, (c.gen < 0x01000000u ? c.gen : 0) + 1);
        fl->programar(fl->ctx, b, 0, fs->pg);
        if (!coincide(fs, b * ppb)) continue;
        memset(fs->pg, 0xFF, fl->pag);
        cab_poner(fs->pg, 'F', 0, 0, 0, 0, 0);
        fl->programar(fl->ctx, b, 1, fs->pg);
        if (!coincide(fs, b * ppb + 1)) continue;
        marca = b;
    }
    /* (M7) cada bloque se BORRA siempre, aunque se lea en blanco (pudo quedarse a medio programar en un corte); solo se
     * respeta el que ya tiene cabecera nuestra de ESTA geometría y todo lo demás en blanco (un formato que se repite). */
    for (vuelta = 0; vuelta < 2; vuelta++)
        for (b = 0; b < fl->n_bloques; b++) {
            u32 cuenta = 0, hecho = 0, todo_blanco = 1;
            if ((b == marca) != (vuelta == 1)) continue;
            if (fl->es_malo && fl->es_malo(fl->ctx, b)) continue;
            if (!fl_leer(fs, b * ppb, 0, h, W6F_CAB + 20) && cab_leer(h, &c) == 1 && c.tipo == 'B' && crc32_mas(0, h + W6F_CAB, 20) == c.dcrc) {
                cuenta = c.gen < 0x01000000u ? c.gen : 0;
                hecho = g32(h + W6F_CAB) == fl->pag && g32(h + W6F_CAB + 4) == ppb && g32(h + W6F_CAB + 8) == fl->n_bloques && g32(h + W6F_CAB + 16) == conf->max_obj;
            }
            for (k = hecho ? 1 : 0; k < ppb && todo_blanco; k++) if (!blanca(fs, b * ppb + k)) todo_blanco = 0;
            if (!(hecho && todo_blanco)) {
                if (fl->borrar(fl->ctx, b)) { if (fl->marcar_malo && fl->clase == W6F_FL_NAND) fl->marcar_malo(fl->ctx, b); continue; }
                cuenta++;
                for (k = 0; k < ppb; k++) if (!blanca(fs, b * ppb + k)) break;
                if (k < ppb) continue;
                cab_bloque(fs, fl, conf, cuenta);
                fl->programar(fl->ctx, b, 0, fs->pg);
                if (!coincide(fs, b * ppb)) continue;
            }
            buenos++;
            if (paso) paso(ctx, buenos, fl->n_bloques);
        }
    return buenos >= conf->reserva_bloques + 2 ? 0 : W6F_E_FLASH;
}

/* ---- las funciones públicas que cambian algo ---------------------------------------------------------
 * (M5) Ninguna es reentrante: mientras una está en marcha (y puede estar llamando a «respiro» entre dos pasos de
 * recolección), cualquier otra que cambie el volumen devuelve W6F_E_OCUPADO sin tocar nada. Leer sí se puede. */
/* (N1) El solo lectura por falta de reserva o por un punto de control que no se pudo escribir NO es para siempre: si
 * ahora hay por dónde (la flash vuelve a dejarse, hay algo que borrar), se levanta y se escriben los puntos de control
 * que se deban. */
static void revaluar(W6f *fs)
{
    if (!fs->montado || !fs->solo_lectura || (fs->motivo_sl != W6F_SL_SIN_RESERVA && fs->motivo_sl != W6F_SL_FLASH)) return;
    if (fs->conf.solo_lectura || (!hay_salida(fs) && !rehabilitar(fs))) return;
    fs->solo_lectura = 0; fs->motivo_sl = W6F_SL_NO;
    if (fs->pend_cp) { cp_pendientes(fs); if (fs->pend_cp) { fs->solo_lectura = 1; fs->motivo_sl = W6F_SL_FLASH; } }
}
static int entrar(W6f *fs)
{
    if (fs->dentro) return W6F_E_OCUPADO;
    revaluar(fs);
    if (fs->montado && !fs->solo_lectura) secuencia_bien(fs);
    fs->dentro = 1;
    return 0;
}
#define ENVUELVE(llamada) do { int r_ = entrar(fs); if (r_) return r_; r_ = (llamada); fs->dentro = 0; return r_; } while (0)
int  w6f_abrir(W6f *fs, const char *ruta, int modo, uint32_t fecha) { ENVUELVE(x_abrir(fs, ruta, modo, fecha)); }
int  w6f_escribir(W6f *fs, int h, const void *d, uint32_t n) { ENVUELVE(x_escribir(fs, h, d, n)); }
int  w6f_cerrar(W6f *fs, int h) { ENVUELVE(x_cerrar(fs, h)); }
void w6f_abortar(W6f *fs, int h) { if (fs->dentro) return; fs->dentro = 1; x_abortar(fs, h); fs->dentro = 0; }
int  w6f_guardar(W6f *fs, const char *ruta, const void *d, uint32_t n, uint32_t fecha) { ENVUELVE(x_guardar(fs, ruta, d, n, fecha)); }
int  w6f_truncar(W6f *fs, const char *ruta, uint32_t tam) { ENVUELVE(x_truncar(fs, ruta, tam)); }
int  w6f_mkdir(W6f *fs, const char *ruta, uint32_t fecha) { ENVUELVE(x_mkdir(fs, ruta, fecha)); }
int  w6f_borrar(W6f *fs, const char *ruta) { ENVUELVE(x_borrar(fs, ruta)); }
int  w6f_renombrar(W6f *fs, const char *vieja, const char *nueva, int reemplazar) { ENVUELVE(x_renombrar(fs, vieja, nueva, reemplazar)); }
int  w6f_gc_paso(W6f *fs, uint32_t tope)
{
    int r;
    if (!fs->montado || fs->dentro) return 0;
    revaluar(fs);
    if (fs->solo_lectura) return 0;
    fs->dentro = 1;
    r = gc_trabajo(fs, 0, tope ? tope : 1);
    /* sin nada más que hacer: dejar ABIERTO un bloque donde escribir (por omisión eso es borrar un bloque libre heredado),
     * para que la primera escritura de la sesión no tenga que esperar a ese borrado */
    if (r == 0 && fs->cabeza == NINGUNO && fs->parcial == NINGUNO && fs->victima == NINGUNO && fs->libres > fs->libres_min && !nueva_cabeza(fs)) r = 1;
    fs->dentro = 0;
    return r;
}
/* Vaciar funciona SIEMPRE que la flash se deje borrar: también con el volumen lleno, en déficit o en solo lectura por
 * falta de reserva (lo único que lo impide es conf.solo_lectura). */
int w6f_vaciar(W6f *fs, void *mem, uint32_t mem_bytes)
{
    const W6fFlash *fl = fs->fl; W6fConf cf = fs->conf; int r;
    if (!fs->montado) return W6F_E_SIN_MONTAR;
    if (cf.solo_lectura) return W6F_E_SOLO_LECTURA;
    if (fs->dentro) return W6F_E_OCUPADO;
    if (!fs->solo_lectura) {                           /* la marca de «formato a medias», por el registro (en una página que es de esta sesión) */
        u32 P; u8 urg = fs->urgente;
        fs->dentro = 1; fs->urgente = 1;
        if (!sitio(fs, 1)) { memset(fs->pg, 0xFF, fl->pag); cab_poner(fs->pg, 'F', 0, 0, 0, 0, 0); (void)prog(fs, &P); }
        fs->urgente = urg; fs->dentro = 0;
    }
    fs->montado = 0;
    if ((r = w6f_formatear(fl, &cf, mem, mem_bytes, 0, 0))) return r;
    return w6f_montar(fs, fl, &cf, mem, mem_bytes);
}

#ifdef W6F_DEPURA
#include <stdio.h>
void w6f_volcar(W6f *fs)
{
    u32 b;
    fprintf(stderr, "cabeza %d/%u parcial %d/%u destino %d victima %d libres %u utiles %u disp %u cap %u usadas %u pend %u res %u\n", (int)fs->cabeza, fs->llenado,
            (int)fs->parcial, fs->parcial_llenado, (int)fs->destino, (int)fs->victima, fs->libres, fs->utiles, disponibles(fs), fs->cap, fs->usadas, fs->pend, fs->reservadas);
    for (b = 0; b < fs->fl->n_bloques; b++) fprintf(stderr, "  blq %u: estado %u vivas %u borrados %u fallos %u\n", b, fs->blq[b].estado, fs->blq[b].vivas, fs->blq[b].borrados, fs->blq[b].fallos);
}
#endif
#ifdef W6F_DEPURA
int w6f_invariante(W6f *fs)             /* 1 = hay un bloque libre para destino, o algo que borrar sin copiar */
{
    u32 b;
    if (fs->libres || fs->destino != NINGUNO) return 1;
    for (b = 0; b < fs->fl->n_bloques; b++) if ((fs->blq[b].estado == B_USADO || fs->blq[b].estado == B_SUCIO) && !fs->blq[b].vivas) return 1;
    return 0;
}
#endif
