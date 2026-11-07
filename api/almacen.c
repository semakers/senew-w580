/* El almacén persistente: pares clave → valor sobre unos bloques de NOR. Formato y garantías: almacen.h.
 * Nada de aquí toca registros: todo va por el puerto (leer / programar / borrar). */
#include "almacen.h"

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
int   memcmp(const void *a, const void *b, __SIZE_TYPE__ n);

#define MAGIA   0x564E3657u            /* 'W6NV' */
#define ACTIVO  0x56544341u            /* 'ACTV' */
#define M_VALOR 0x5652u
#define M_BORRA 0x5842u
#define CAB_BLQ 16u
#define PAD4(n) (((n) + 3u) & ~3u)

static const AlmPuerto *pt;
static AlmEstado es;
static u32 fin;                                          /* desplazamiento donde va el siguiente registro */
static struct { u16 clave; u32 pos; } ind[ALM_MAX_CLAVES];   /* pos = desplazamiento del registro vigente */
static u32 n_ind;

u32 alm_crc32(u32 crc, const void *d, u32 n)
{
    static const u32 t[16] = { 0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
                               0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C };
    const u8 *p = d;
    crc = ~crc;
    while (n--) { crc ^= *p++; crc = t[crc & 15] ^ (crc >> 4); crc = t[crc & 15] ^ (crc >> 4); }
    return ~crc;
}
static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u32 le16(const u8 *p) { return p[0] | (p[1] << 8); }
static void p32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void p16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

/* (AUD2 B4) ¿Es NUESTRO aunque no monte? Basta la marca en la cabecera de un bloque (no se mira el CRC: lo que interesa es
 * no llamar «restos del firmware original» a un almacén propio dañado o a medio formatear). Solo lee. */
int alm_firma(const AlmPuerto *p)
{
    u32 b;
    if (!p || !p->leer) return 0;
    for (b = 0; b < p->bloques; b++) if (le32(p->leer(b)) == MAGIA) return 1;
    return 0;
}
/* generación del bloque si su cabecera es válida Y está confirmado; 0 si no cuenta */
static u32 gen_de(u32 b)
{
    const u8 *p = pt->leer(b);
    if (le32(p) != MAGIA || le32(p + 8) != alm_crc32(0, p, 8) || le32(p + 12) != ACTIVO) return 0;
    return le32(p + 4);
}

static int buscar(u16 clave)
{
    u32 i;
    for (i = 0; i < n_ind; i++) if (ind[i].clave == clave) return (int)i;
    return -1;
}
static void quitar(u16 clave)
{
    int i = buscar(clave);
    if (i >= 0) ind[i] = ind[--n_ind];
}
static void poner(u16 clave, u32 pos)
{
    int i = buscar(clave);
    if (i >= 0) { ind[i].pos = pos; return; }
    if (n_ind < ALM_MAX_CLAVES) { ind[n_ind].clave = clave; ind[n_ind].pos = pos; n_ind++; }
    else es.errores++;
}

/* Recorre el bloque activo: arma el índice y fija «fin». Tolera cualquier cosa: lo que no es un registro
 * válido se salta (un registro a medias con la cabecera buena ocupa su longitud; lo demás, de 4 en 4). */
static void recorrer(void)
{
    const u8 *b = pt->leer(es.activo);
    u32 t = es.tam_bloque, p = CAB_BLQ, ult = t;
    n_ind = 0; es.vivos = 0;
    while (ult > CAB_BLQ && le32(b + ult - 4) == 0xFFFFFFFFu) ult -= 4;      /* tras «ult» todo está borrado */
    while (p + 12 <= t) {
        u32 marca = le16(b + p), clave = le16(b + p + 2), len = le16(b + p + 4);
        u32 total = 8 + PAD4(len) + 4;
        if ((marca == M_VALOR || marca == M_BORRA) && len <= ALM_MAX_VALOR && le16(b + p + 6) == (alm_crc32(0, b + p, 6) & 0xFFFF) && p + total <= t) {
            if (le32(b + p + total - 4) == alm_crc32(0, b + p, 8 + len)) {
                if (marca == M_VALOR) poner((u16)clave, p); else quitar((u16)clave);
            }
            p += total;
        } else if (p >= ult) break;
        else p += 4;
    }
    fin = p > ult ? p : ult;
    es.usado = fin;
    for (p = 0; p < n_ind; p++) es.vivos += 8 + PAD4(le16(b + ind[p].pos + 4)) + 4;
    es.claves = n_ind;
}

/* Añade un registro en (bloque, *pos). Dos operaciones: cabecera + datos, y después el CRC, que es lo que valida. */
static int anadir(u32 bloque, u32 *pos, u32 marca, u16 clave, const u8 *d, u32 n)
{
    u8 cab[8], cola[4]; u32 crc, p = *pos, total = 8 + PAD4(n) + 4;
    if (p + total > es.tam_bloque) return ALM_E_LLENO;
    p16(cab, marca); p16(cab + 2, clave); p16(cab + 4, n); p16(cab + 6, alm_crc32(0, cab, 6) & 0xFFFF);
    crc = alm_crc32(alm_crc32(0, cab, 8), d, n);
    p32(cola, crc);
    *pos = p + total;                                    /* pase lo que pase, ese sitio ya no se reutiliza */
    if (pt->programar(bloque, p, cab, 8)) return ALM_E_FLASH;
    if (n && pt->programar(bloque, p + 8, d, n)) return ALM_E_FLASH;
    if (pt->programar(bloque, p + total - 4, cola, 4)) return ALM_E_FLASH;
    return ALM_OK;
}

/* Prepara un bloque nuevo con esa generación (sin confirmar). */
static int abrir_bloque(u32 b, u32 gen)
{
    u8 cab[12];
    if (pt->borrar(b)) return ALM_E_FLASH;               /* siempre: un bloque que «parece» borrado puede venir de un borrado cortado */
    p32(cab, MAGIA); p32(cab + 4, gen); p32(cab + 8, alm_crc32(0, cab, 8));
    return pt->programar(b, 0, cab, 12) ? ALM_E_FLASH : ALM_OK;
}
static int confirmar(u32 b)
{
    u8 c[4];
    p32(c, ACTIVO);
    return pt->programar(b, 12, c, 4) ? ALM_E_FLASH : ALM_OK;
}

/* Compacta el bloque activo en el siguiente del anillo, sin la clave «fuera» y, si marca = M_VALOR, con
 * su valor nuevo. El bloque viejo NO se toca: sigue siendo el bueno hasta que el nuevo queda confirmado. */
static int compactar(u16 fuera, u32 marca, const u8 *d, u32 n)
{
    const u8 *viejo = pt->leer(es.activo);
    u32 intento, i, hace_falta = CAB_BLQ + (marca == M_VALOR ? 8 + PAD4(n) + 4 : 0);
    for (i = 0; i < n_ind; i++) if (ind[i].clave != fuera) hace_falta += 8 + PAD4(le16(viejo + ind[i].pos + 4)) + 4;
    if (hace_falta > es.tam_bloque) return ALM_E_LLENO;
    for (intento = 1; intento < es.bloques; intento++) {
        u32 nb = (es.activo + intento) % es.bloques, pos = CAB_BLQ;
        int mal = abrir_bloque(nb, es.generacion + 1);
        for (i = 0; i < n_ind && !mal; i++) if (ind[i].clave != fuera)
            mal = anadir(nb, &pos, M_VALOR, ind[i].clave, viejo + ind[i].pos + 8, le16(viejo + ind[i].pos + 4));
        if (!mal && marca == M_VALOR) mal = anadir(nb, &pos, M_VALOR, fuera, d, n);
        if (!mal) mal = confirmar(nb);
        if (!mal && gen_de(nb) == es.generacion + 1) {
            es.activo = nb; es.generacion++; es.compactaciones++;
            recorrer();
            return ALM_OK;
        }
        es.errores++;                                    /* ese bloque falló: queda sin confirmar y se prueba el siguiente */
    }
    return ALM_E_FLASH;
}

int alm_montar(const AlmPuerto *p, int puede_escribir)
{
    u32 b, mejor = 0, gen = 0, g;
    pt = p; n_ind = 0; fin = 0;
    es = (AlmEstado){ .bloques = p ? p->bloques : 0, .tam_bloque = p ? p->tam_bloque : 0, .solo_lectura = !puede_escribir };
    if (!p || p->bloques < 2 || p->tam_bloque < 256 || (p->tam_bloque & 3)) return ALM_E_ARG;
    for (b = 0; b < p->bloques; b++) if ((g = gen_de(b)) > gen) { gen = g; mejor = b; }
    if (!gen) {                                          /* no hay almacén: darle formato */
        if (!puede_escribir) return ALM_E_NO_HAY;
        for (b = 0; b < p->bloques; b++)
            if (abrir_bloque(b, 1) == ALM_OK && confirmar(b) == ALM_OK && gen_de(b) == 1) break;
        if (b == p->bloques) { es.errores++; return ALM_E_FLASH; }
        mejor = b; gen = 1; es.formateos = 1;
    }
    es.activo = mejor; es.generacion = gen; es.montado = 1;
    recorrer();
    return ALM_OK;
}

int alm_tam(u16 clave)
{
    int i;
    if (!es.montado) return ALM_E_SIN_MONTAR;
    i = buscar(clave);
    return i < 0 ? ALM_E_NO_HAY : (int)le16(pt->leer(es.activo) + ind[i].pos + 4);
}
int alm_cargar(u16 clave, void *buf, u32 max)
{
    int i; u32 n; const u8 *r;
    if (!es.montado) return ALM_E_SIN_MONTAR;
    if ((i = buscar(clave)) < 0) return ALM_E_NO_HAY;
    r = pt->leer(es.activo) + ind[i].pos;
    n = le16(r + 4);
    memcpy(buf, r + 8, n < max ? n : max);
    return (int)n;
}

static int escribir(u32 marca, u16 clave, const u8 *d, u32 n)
{
    int r, i;
    u32 pos;
    if (!es.montado) return ALM_E_SIN_MONTAR;
    if (es.solo_lectura) return ALM_E_FLASH;
    if (n > ALM_MAX_VALOR || (n && !d)) return ALM_E_ARG;
    i = buscar(clave);
    if (marca == M_BORRA && i < 0) return ALM_OK;                          /* no estaba */
    if (marca == M_VALOR && i >= 0) {
        const u8 *v = pt->leer(es.activo) + ind[i].pos;
        if (le16(v + 4) == n && !memcmp(v + 8, d, n)) return ALM_OK;      /* ya vale eso: no se gasta flash */
    }
    if (marca == M_VALOR && i < 0 && n_ind >= ALM_MAX_CLAVES) return ALM_E_LLENO;
    if (fin + 8 + PAD4(n) + 4 <= es.tam_bloque) {
        pos = fin;
        r = anadir(es.activo, &pos, marca, clave, d, n);
        if (r == ALM_OK) {
            const u8 *b = pt->leer(es.activo) + fin;                       /* se relee lo escrito antes de darlo por bueno */
            if (le32(b + 8 + PAD4(n)) == alm_crc32(0, b, 8 + n) && le16(b) == marca) {
                if (marca == M_VALOR) poner(clave, fin); else quitar(clave);
                recorrer();
                return ALM_OK;
            }
        }
        es.errores++;
        recorrer();                                      /* lo que quedara a medias se salta; el índice sigue con lo anterior */
    }
    return compactar(clave, marca, d, n);
}
int alm_guardar(u16 clave, const void *d, u32 n) { return escribir(M_VALOR, clave, d, n); }
int alm_borrar(u16 clave) { return escribir(M_BORRA, clave, 0, 0); }
void alm_estado(AlmEstado *e) { *e = es; }
