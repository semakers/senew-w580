/* Lector FAT12 / FAT16 / FAT32 con nombres largos (VFAT), de SOLO LECTURA, sobre un dispositivo de sectores de 512.
 * Pista T. Qué es y qué no hace: api/fat.h y docs/tarjeta.md §3. Sale de generalizar el lector FAT12/16 que traía
 * api/fsfab.c (pista S, cotejado con los 483 ficheros del FS de fábrica): ahora los dos usan este.
 *
 * (FATW) La ESCRITURA va al final del fichero (docs/fat_escritura.md) y solo existe con fat_montar_rw y una FatEscribir:
 * montado con fat_montar, el único camino al dispositivo sigue siendo FatLeer. Todo número leído del disco se acota antes
 * de usarse; los topes están marcados con «tope:». */
#include "fat.h"

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);
int   memcmp(const void *a, const void *b, __SIZE_TYPE__ n);

#define FAT_RACHA 2048u                 /* sectores como mucho en una petición al dispositivo (1 MiB) */

typedef struct { char nombre[FAT_NOMBRE]; char corto[13]; char corto_visto[13]; u8 attr, cortado; u32 cluster, tam;
                 FatDir ini; u32 nslots, lba, idx; } Ent;   /* (FATW) dónde está: el primer hueco del grupo (nombre largo incluido), cuántos, y la entrada corta */
typedef struct { u8 attr, raiz; u32 cluster, tam; } Res;      /* una ruta resuelta */

static u32 le16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* ---- la caché de sectores: FAT_CACHE sectores, el menos usado sale. El puntero que devuelve solo vale hasta la
 * siguiente llamada (otra lectura puede echar ese sector): quien lo usa copia lo que necesita antes. ------------ */
static int pend_vaciar(FatVol *v);
static const u8 *sec(FatVol *v, u32 lba)
{
    u32 i, cual = 0;
    if (v->sectores && lba >= v->sectores) { v->topes++; return 0; }      /* tope: nada fuera del dispositivo */
    for (i = 0; i < FAT_CACHE; i++)
        if (v->c_hay[i] && v->c_lba[i] == lba) { v->c_uso[i] = ++v->c_reloj; v->aciertos++; return v->c_dat[i]; }
    for (i = 0; i < FAT_CACHE; i++) {
        if (!v->c_hay[i]) { cual = i; break; }
        if (v->c_uso[i] < v->c_uso[cual]) cual = i;
    }
    if (v->c_hay[cual] && v->c_pend[cual] && pend_vaciar(v)) return 0;   /* (FATW) un sector de la FAT a medio escribir no se tira */
    v->c_hay[cual] = 0; v->lecturas++;
    if (v->leer(v->ctx, lba, 1, v->c_dat[cual])) { v->errores_es++; v->fallo_es = 1; return 0; }
    v->c_hay[cual] = 1; v->c_lba[cual] = lba; v->c_uso[cual] = ++v->c_reloj;
    return v->c_dat[cual];
}

/* ---- montaje ----------------------------------------------------------------------------------------------- */

/* ¿Parece un sector de arranque FAT? Lo que mira el original (bytes por sector potencia de 2 entre 0x200 y 0x1000,
 * sectores por clúster potencia de 2 <= 0x80, reservados != 0: docs/memory_stick.md §3.2) y, NUESTRO, tres cosas más
 * para no tomar el código de un MBR por un sector de arranque: la firma 55 AA, de 1 a 4 FAT y el byte de medio >= F0. */
static int es_arranque(const u8 *b)
{
    u32 bps = le16(b + 11), spc = b[13];
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    if (bps < 0x200 || bps > 0x1000 || (bps & (bps - 1))) return 0;
    if (!spc || spc > 0x80 || (spc & (spc - 1))) return 0;
    if (!le16(b + 14) || !b[16] || b[16] > 4 || b[21] < 0xF0) return 0;
    return 1;
}
const char *fat_motivo(int e)
{
    return e == 0 ? "bien" : e == FAT_E_ES ? "no se pudo leer" : e == FAT_E_NO_FAT ? "no tiene formato FAT" :
           e == FAT_E_MBR ? "tabla de particiones vacia o mal" : e == FAT_E_SECTOR ? "sectores que no son de 512" :
           e == FAT_E_GEOMETRIA ? "sector de arranque FAT imposible" : "error";
}
int fat_montar(FatVol *v, FatLeer leer, void *ctx, u32 sectores)
{
    const u8 *b; u32 res, nfat, raiz_s, antes, caben, raiz_cl, i; u8 et16[11], et32[11], hay16, hay32;
    memset(v, 0, sizeof *v);
    v->leer = leer; v->ctx = ctx; v->sectores = sectores;
    if (!(b = sec(v, 0))) return FAT_E_ES;
    if (!es_arranque(b)) {                                    /* MBR: SOLO la primera entrada, sin mirar el tipo */
        u32 ini, tam;
        if (b[510] != 0x55 || b[511] != 0xAA) return FAT_E_NO_FAT;
        ini = le32(b + 0x1C6); tam = le32(b + 0x1CA);
        if (!ini || !tam || (sectores && ini >= sectores)) return FAT_E_MBR;
        v->part = ini; v->con_mbr = 1;
        if (!(b = sec(v, ini))) return FAT_E_ES;
        if (!es_arranque(b)) return FAT_E_NO_FAT;
    }
    if (le16(b + 11) != 512) return FAT_E_SECTOR;
    v->spc = b[13]; res = le16(b + 14); nfat = b[16]; v->nraiz = le16(b + 17);
    v->total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    v->sfat = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    raiz_cl = le32(b + 44);
    hay16 = b[38] == 0x29; hay32 = b[66] == 0x29;
    memcpy(et16, b + 43, 11); memcpy(et32, b + 71, 11);      /* «b» deja de valer en la siguiente lectura */
    if (v->sectores && v->total > v->sectores - v->part) v->total = v->sectores - v->part;   /* tope: lo que hay */
    raiz_s = (v->nraiz * 32 + 511) / 512;
    if (!v->total || !v->sfat || res >= v->total || v->sfat > (v->total - res) / nfat) return FAT_E_GEOMETRIA;
    antes = res + nfat * v->sfat;
    if (raiz_s >= v->total - antes || v->part > 0xFFFFFFFFu - v->total) return FAT_E_GEOMETRIA;
    v->s_fat = v->part + res; v->s_raiz = v->part + antes;
    antes += raiz_s;
    v->s_datos = v->part + antes;
    v->nclusters = (v->total - antes) / v->spc;
    if (!v->nclusters) return FAT_E_GEOMETRIA;
    v->tipo = v->nclusters < 4085 ? 12 : v->nclusters < 65525 ? 16 : 32;    /* la regla del estándar: por el n.º de clústeres */
    caben = v->tipo == 32 ? (v->sfat > 0x7FFFFFu ? 0xFFFFFFFFu : v->sfat * 128u) :
            v->tipo == 16 ? (v->sfat > 0xFFFFu ? 0xFFFFFFu : v->sfat * 256u) : (v->sfat > 0xFFFFu ? 0xFFFFu : v->sfat) * 1024u / 3u;
    if (caben < 3) return FAT_E_GEOMETRIA;
    if (v->nclusters > caben - 2) { v->nclusters = caben - 2; v->topes++; }   /* tope: solo los clústeres que la FAT describe */
    if (v->tipo == 32) {
        if (v->nraiz || raiz_cl < 2 || raiz_cl >= v->nclusters + 2) return FAT_E_GEOMETRIA;
        v->raiz_cl = raiz_cl;
    } else if (!v->nraiz) return FAT_E_GEOMETRIA;
    if (v->tipo == 32 ? hay32 : hay16) {
        memcpy(v->etiqueta, v->tipo == 32 ? et32 : et16, 11);
        for (i = 11; i && (v->etiqueta[i - 1] == ' ' || v->etiqueta[i - 1] == 0); i--) v->etiqueta[i - 1] = 0;
        for (i = 0; i < 11; i++) if (v->etiqueta[i] && ((u8)v->etiqueta[i] < 0x20 || (u8)v->etiqueta[i] > 0x7E)) v->etiqueta[i] = '?';
    }
    if (!v->sectores) v->sectores = v->part + v->total;
    v->montado = 1;
    return 0;
}

/* ---- la FAT -------------------------------------------------------------------------------------------------- */

static u32 fat_sig(FatVol *v, u32 c)                        /* 0 = fin de cadena, clúster imposible o error de lectura */
{
    const u8 *p; u32 x, o;
    if (c < 2 || c >= v->nclusters + 2) return 0;
    if (v->tipo == 32) {
        o = c * 4; if (!(p = sec(v, v->s_fat + o / 512))) return 0;
        x = le32(p + o % 512) & 0x0FFFFFFFu;
    } else if (v->tipo == 16) {
        o = c * 2; if (!(p = sec(v, v->s_fat + o / 512))) return 0;
        x = le16(p + o % 512);
    } else {
        o = c + c / 2; if (!(p = sec(v, v->s_fat + o / 512))) return 0;
        x = p[o % 512];
        if (!(p = sec(v, v->s_fat + (o + 1) / 512))) return 0;     /* la entrada puede caer entre dos sectores */
        x |= (u32)p[(o + 1) % 512] << 8;
        x = (c & 1) ? x >> 4 : x & 0xFFF;
    }
    return x >= 2 && x < v->nclusters + 2 ? x : 0;
}
static u32 sector_de(const FatVol *v, u32 c) { return v->s_datos + (c - 2) * v->spc; }

/* ---- directorios --------------------------------------------------------------------------------------------- */

static void dir_abrir(FatDir *d, u32 cluster) { d->cluster = cluster; d->i = 0; d->vistos = 0; d->marca = cluster; d->paso = 0; d->hasta = 1; }
static const u8 *dir_entrada(FatVol *v, FatDir *d)          /* la entrada cruda de 32 bytes, o 0 al acabar */
{
    const u8 *p; u32 por_cluster = v->spc * 16, s;
    if (d->vistos++ > 65536u) { v->topes++; return 0; }     /* tope: una cadena en bucle no cuelga (2 MiB de directorio) */
    if (d->cluster == 0) {
        if (v->tipo == 32 || d->i >= v->nraiz) return 0;
        s = v->s_raiz + d->i / 16;
    } else {
        if (d->cluster < 2 || d->cluster >= v->nclusters + 2) return 0;
        if (d->i == por_cluster) {
            d->cluster = fat_sig(v, d->cluster); d->i = 0;
            if (d->cluster < 2) return 0;
            /* tope: una cadena EN BUCLE se corta en cuanto da la vuelta (detector de Brent: una marca que se va dejando cada
             * 1, 2, 4, 8… clústeres; volver a pisarla es un bucle), no al cabo de 65 536 entradas repetidas */
            if (d->cluster == d->marca) { v->topes++; return 0; }
            if (++d->paso == d->hasta) { d->marca = d->cluster; d->paso = 0; d->hasta *= 2; }
        }
        s = sector_de(v, d->cluster) + d->i / 16;
    }
    if (!(p = sec(v, s))) return 0;
    d->s_lba = s;
    return p + (d->i++ % 16) * 32;
}
static char *utf8(char *p, char *fin, u32 c, u8 *cortado)   /* «fin» = el último sitio (para el 0) */
{
    u32 n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (p + n > fin) { *cortado = 1; return p; }
    if (n == 1) *p++ = (char)c;
    else if (n == 2) { *p++ = (char)(0xC0 | c >> 6); *p++ = (char)(0x80 | (c & 63)); }
    else if (n == 3) { *p++ = (char)(0xE0 | c >> 12); *p++ = (char)(0x80 | (c >> 6 & 63)); *p++ = (char)(0x80 | (c & 63)); }
    else { *p++ = (char)(0xF0 | c >> 18); *p++ = (char)(0x80 | (c >> 12 & 63)); *p++ = (char)(0x80 | (c >> 6 & 63)); *p++ = (char)(0x80 | (c & 63)); }
    return p;
}
/* Siguiente entrada real (salta borradas, etiquetas, «.» y «..»). Nombre largo si lo hay, sus trozos vienen en orden
 * (n, n-1, … 1) y su suma cuadra con el corto; si no, el corto con los bits de minúsculas de NT. 0 al acabar. */
static int dir_sig(FatVol *v, FatDir *d, Ent *e)
{
    static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    u32 partes = 0, espera = 0, suma_lfn = 0;
    const u8 *p; FatDir antes, ini = *d;
    for (;;) {
        antes = *d;
        if (!(p = dir_entrada(v, d))) break;
        char *q, *fin, *c1 = e->corto, *c2 = e->corto_visto; u8 nada = 0;
        u32 i, suma = 0;
        if (p[0] == 0) return 0;
        if (p[0] == 0xE5) { partes = 0; continue; }
        if ((p[11] & 0x3F) == 0x0F) {                       /* trozo de nombre largo */
            u32 n = p[0] & 0x3F;
            if ((p[0] & 0x40) && n >= 1 && n <= 20) { partes = n; espera = n; suma_lfn = p[13]; v->largo[n * 13] = 0; ini = antes; }
            if (!partes || n != espera || p[13] != suma_lfn) { partes = 0; continue; }
            for (i = 0; i < 13; i++) v->largo[(n - 1) * 13 + i] = (u16)le16(p + pos[i]);
            espera = n - 1;
            continue;
        }
        if ((p[11] & 0x08) || p[0] == '.') { partes = 0; continue; }
        for (i = 0; i < 11; i++) suma = ((suma >> 1 | (suma & 1) << 7) + p[i]) & 0xFF;
        for (i = 0; i < 11; i++) {                          /* el corto 8.3: tal cual (para comparar) y con las minúsculas de NT (para enseñar) */
            u32 c = i == 0 && p[0] == 0x05 ? 0xE5u : p[i];
            if (c == ' ') continue;
            if (c < 0x20 || c > 0x7E) c = '_';              /* fuera de ASCII (página de códigos OEM: no la sabemos) */
            if (i == 8) { *c1++ = '.'; *c2++ = '.'; }
            *c1++ = (char)c;
            *c2++ = (char)((p[12] & (i < 8 ? 0x08 : 0x10)) && c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        *c1 = 0; *c2 = 0;
        q = e->nombre; fin = e->nombre + sizeof e->nombre - 1; e->cortado = 0;
        if (partes && espera == 0 && suma == suma_lfn) {
            for (i = 0; i < partes * 13 && v->largo[i] && v->largo[i] != 0xFFFF; i++) {
                u32 c = v->largo[i];
                if (c >= 0xD800 && c < 0xDC00 && i + 1 < partes * 13 && v->largo[i + 1] >= 0xDC00 && v->largo[i + 1] < 0xE000)
                    c = 0x10000u + ((c - 0xD800) << 10) + (v->largo[++i] - 0xDC00u);
                else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;                  /* medio par suelto */
                q = utf8(q, fin, c, &e->cortado);
                if (e->cortado) break;
            }
        } else for (i = 0; e->corto_visto[i]; i++) q = utf8(q, fin, (u8)e->corto_visto[i], &nada);
        *q = 0;
        e->attr = p[11]; e->tam = le32(p + 28);
        e->cluster = le16(p + 26) | (v->tipo == 32 ? le16(p + 20) << 16 : 0);
        e->lba = d->s_lba; e->idx = (d->i - 1) % 16;
        if (partes && espera == 0 && suma == suma_lfn) { e->ini = ini; e->nslots = partes + 1; }
        else { e->ini = antes; e->nslots = 1; }
        return 1;
    }
    return 0;
}
static int igual_sin_caja(const char *a, const char *b, u32 n)   /* a entero contra b[0..n) */
{
    u32 i;
    for (i = 0; i < n; i++) {
        u32 x = (u8)a[i], y = (u8)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (!x || x != y) return 0;
    }
    return a[n] == 0;
}

/* Resuelve una ruta del volumen. Una entrada casa por su nombre largo o por su corto 8.3 (así se abre lo que el
 * listado tuvo que enseñar por el corto). Recuerda la última ruta resuelta: las apps piden tamaño y luego leen. */
static int resolver(FatVol *v, const char *ruta, Res *r)
{
    const char *p = ruta; u32 n, largo = 0;
    static Ent e;                                           /* (no es reentrante: como todo el sistema, un solo hilo) */
    if (!v->montado || ruta[0] != '/') return 0;
    while (ruta[largo] && largo < FAT_NOMBRE) largo++;
    if (v->m_hay && largo < FAT_NOMBRE && !memcmp(v->m_ruta, ruta, largo + 1)) {
        r->attr = v->m_hay & 0x10; r->raiz = v->m_hay & 2; r->cluster = v->m_cluster; r->tam = v->m_tam;
        return 1;
    }
    r->attr = 0x10; r->raiz = 1; r->cluster = v->tipo == 32 ? v->raiz_cl : 0; r->tam = 0;
    for (;;) {
        FatDir d; int ok = 0;
        while (*p == '/') p++;
        if (!*p) break;
        for (n = 0; p[n] && p[n] != '/'; n++) ;
        if (!(r->attr & 0x10)) return 0;
        if (!r->raiz && r->cluster == 0) return 0;          /* subdirectorio sin clúster: vacío */
        dir_abrir(&d, r->cluster);
        while (dir_sig(v, &d, &e)) if (igual_sin_caja(e.nombre, p, n) || igual_sin_caja(e.corto, p, n)) { ok = 1; break; }
        if (!ok) return 0;
        r->attr = e.attr & 0x10; r->raiz = 0; r->cluster = e.cluster; r->tam = e.tam;
        p += n;
    }
    if (largo < FAT_NOMBRE) {
        memcpy(v->m_ruta, ruta, largo + 1); v->m_cluster = r->cluster; v->m_tam = r->tam;
        v->m_hay = (u8)(1 | r->attr | (r->raiz ? 2 : 0));
    }
    return 1;
}

/* ---- la API ------------------------------------------------------------------------------------------------ */

int fat_listar(FatVol *v, const char *dir, u32 desde, HwEntrada *e, u32 max)
{
    static Ent en;
    Res r; FatDir d; u32 i = 0, n = 0;
    if (!resolver(v, dir, &r) || !(r.attr & 0x10)) return HW_ERROR;
    if (!r.raiz && r.cluster == 0) return 0;                /* un subdirectorio sin clúster está vacío (no es la raíz) */
    if (v->l_hay && v->l_cluster == r.cluster && v->l_hay == (r.raiz ? 2 : 1) && desde >= v->l_idx) { d = v->l_dir; i = v->l_idx; }
    else dir_abrir(&d, r.cluster);                          /* (si no: las apps piden la entrada n.º k de una en una) */
    while ((n < max || !e) && dir_sig(v, &d, &en)) {
        if (i++ < desde) continue;
        if (e) {
            /* un nombre que no cabe en HwEntrada (63 bytes) se enseña por su corto 8.3, que SÍ se puede abrir;
             * cortado a medias sería un nombre que no existe */
            const char *nom = en.cortado ? en.corto_visto : en.nombre; u32 k = 0;
            while (nom[k]) k++;
            if (k > sizeof e[n].nombre - 1) { nom = en.corto_visto; for (k = 0; nom[k]; k++) ; }
            memcpy(e[n].nombre, nom, k + 1);
            e[n].es_dir = (en.attr & 0x10) != 0; e[n].tam = e[n].es_dir ? 0 : en.tam;
        }
        n++;
    }
    v->l_hay = r.raiz ? 2 : 1; v->l_cluster = r.cluster; v->l_dir = d; v->l_idx = i;
    return (int)n;
}
int fat_info(FatVol *v, const char *ruta, u32 *tam, int *es_dir)
{
    Res r;
    if (!resolver(v, ruta, &r)) return 0;
    if (tam) *tam = (r.attr & 0x10) ? 0 : r.tam;
    if (es_dir) *es_dir = (r.attr & 0x10) != 0;
    return 1;
}
u32 fat_tam(FatVol *v, const char *ruta)
{
    Res r;
    return resolver(v, ruta, &r) && !(r.attr & 0x10) ? r.tam : 0;
}
/* «n» bytes desde el desplazamiento «des» de una racha de sectores que empieza en «lba». Los sectores enteros van
 * DERECHOS al búfer de quien lee, en una sola petición (rachas largas: lo que pide docs/memory_stick.md §7); los
 * trozos de sector, por la caché. 0 = bien. */
static int leer_tramo(FatVol *v, u32 lba, u32 des, u32 n, u8 *dst)
{
    while (n) {
        u32 s = lba + des / 512, o = des % 512, m;
        if (o == 0 && n >= 512) {
            u32 ns = n / 512;
            if (v->sectores && (s >= v->sectores || ns > v->sectores - s)) { v->topes++; return -1; }
            v->lecturas += ns;
            if (v->leer(v->ctx, s, ns, dst)) { v->errores_es++; v->fallo_es = 1; return -1; }
            m = ns * 512;
        } else {
            const u8 *p = sec(v, s);
            if (!p) return -1;
            m = 512 - o < n ? 512 - o : n;
            memcpy(dst, p + o, m);
        }
        dst += m; des += m; n -= m;
    }
    return 0;
}
/* Los bytes [desde, desde+max) de un fichero. Sigue la cadena de la FAT por RACHAS de clústeres seguidos y recuerda
 * por dónde iba: leer un fichero a trozos, uno detrás de otro, no vuelve a recorrer la cadena desde el principio. */
u32 fat_leer_desde(FatVol *v, const char *ruta, u32 desde, u8 *buf, u32 max)
{
    Res r; u32 bpc, fin, k, i, c, pos, hecho = 0, saltos = 0;
    if (!max || !resolver(v, ruta, &r) || (r.attr & 0x10) || desde >= r.tam) return 0;
    fin = r.tam - desde < max ? r.tam : desde + max;
    bpc = v->spc * 512;
    k = desde / bpc;
    if (v->f_hay && v->f_ini == r.cluster && v->f_k <= k) { c = v->f_c; i = v->f_k; }
    else { c = r.cluster; i = 0; }
    for (; i < k; i++) {
        c = fat_sig(v, c);
        if (!c || ++saltos > v->nclusters) { if (c) v->topes++; return 0; }   /* tope: una cadena no tiene más clústeres que el volumen */
    }
    if (c < 2 || c >= v->nclusters + 2) return 0;
    pos = k * bpc;
    while (pos < fin) {
        u32 c0 = c, n = 1, sig = 0, a, b, resto = fin - pos;
        while (n * bpc < resto) {                           /* hace falta también el clúster siguiente: ¿va seguido? */
            sig = fat_sig(v, c0 + n - 1);
            if (sig != c0 + n || (n + 1) * v->spc > FAT_RACHA) break;
            n++; sig = 0;
        }
        a = pos > desde ? pos : desde;
        b = n * bpc < resto ? pos + n * bpc : fin;
        if (leer_tramo(v, sector_de(v, c0), a - pos, b - a, buf + (a - desde))) break;
        hecho += b - a;
        v->f_hay = 1; v->f_ini = r.cluster; v->f_k = pos / bpc + n - 1; v->f_c = c0 + n - 1;
        if (n * bpc >= resto) break;
        pos += n * bpc;
        if (!sig) break;                                    /* la cadena se acaba antes que el tamaño: lectura corta */
        c = sig;
        if (++saltos > v->nclusters) { v->topes++; break; }
    }
    return hecho;
}

/* ================================================================================================================
 * (FATW) ESCRITURA. Diseño, orden de escritura y qué deja un corte en cada punto: docs/fat_escritura.md.
 * Reglas que todo lo de abajo cumple:
 *  R1. La FAT se modifica en la caché (c_pend) y se VUELCA ENTERA (todas las 1.ª copia, luego la 2.ª) antes de escribir
 *      cualquier dato o entrada de directorio que dependa de ella. La 1.ª copia manda: fat_revisar copia la 1.ª en la 2.ª.
 *  R2. Los clústeres de datos se reservan (y se vuelcan) ANTES de escribir los datos; la entrada de directorio que los hace
 *      de alguien va AL FINAL. Lo que se corta antes queda «sin dueño» (lo devuelve fat_revisar), nunca a medias en un fichero.
 *  R3. Reemplazar un fichero es una sola escritura de sector (la entrada pasa de la cadena vieja a la nueva); la cadena vieja
 *      se libera DESPUÉS. Añadir: los clústeres nuevos se enlazan al cerrar y el tamaño se pone después del enlace.
 *  R4. Un grupo de entradas (nombre largo + corta) se pone en UN sector siempre que quepa. Si ocupa dos o tres: al CREAR se
 *      escriben del primero al último (hasta el último, el de la corta, solo hay trozos de nombre largo sin corta: invisibles);
 *      al BORRAR, del último al primero (la corta desaparece la primera: el fichero se va de una vez; quedan trozos huérfanos).
 *  R5. Antes de la primera escritura de una tanda se marca el volumen SUCIO (bit 0 del byte 37/65 del sector de arranque,
 *      como Linux; y el bit de «apagado limpio» de FAT[1] en FAT16/32, como Windows), con el FSInfo «sin saber»; fat_limpio
 *      lo deshace al quedar quieto. Al montar sucio, fat_revisar antes de dejar escribir.
 *  R6. Un fallo del dispositivo al escribir deja el volumen ROTO en RAM (se olvida la caché y no se escribe nada más). */
#define FAT_TROZO_W 64u                                     /* sectores por llamada al dispositivo al escribir */
static u8 sec_tmp[512];                                     /* (no reentrante, como el resto) */
static u16 u_tmp[256];
static const u8 cero[512];
static void w16(u8 *p, u32 x) { p[0] = (u8)x; p[1] = (u8)(x >> 8); }
static void w32(u8 *p, u32 x) { p[0] = (u8)x; p[1] = (u8)(x >> 8); p[2] = (u8)(x >> 16); p[3] = (u8)(x >> 24); }
static void olvidar(FatVol *v) { v->m_hay = 0; v->l_hay = 0; v->f_hay = 0; }
u32 fat_bytes_cluster(const FatVol *v) { return v->spc * 512u; }

static int roto(FatVol *v) { v->roto = 1; memset(v->c_hay, 0, sizeof v->c_hay); memset(v->c_pend, 0, sizeof v->c_pend); olvidar(v); return FAT_E_ROTO; }
static int dev_esc(FatVol *v, u32 lba, u32 n, const u8 *buf)
{
    u32 i;
    if (v->roto) return -1;
    if (!n) return 0;
    if (v->sectores && (lba >= v->sectores || n > v->sectores - lba)) { v->topes++; roto(v); return -1; }
    if (lba < v->part) { v->topes++; roto(v); return -1; }  /* tope: nunca delante del volumen (el MBR) */
    v->escrituras += n;
    for (i = 0; i < n; i += FAT_TROZO_W)                    /* a trozos: la interfaz respira (docs/tarjeta_escritura.md §1) */
        if (v->escribir(v->ctx, lba + i, n - i < FAT_TROZO_W ? n - i : FAT_TROZO_W, buf + (unsigned long)i * 512)) { v->errores_w++; v->fallo_es = 1; roto(v); return -1; }
    for (i = 0; i < FAT_CACHE; i++)                         /* la caché sigue diciendo lo que hay en el disco */
        if (v->c_hay[i] && v->c_lba[i] >= lba && v->c_lba[i] - lba < n) {
            const u8 *src = buf + (v->c_lba[i] - lba) * 512;
            if (src != v->c_dat[i]) memcpy(v->c_dat[i], src, 512);
        }
    return 0;
}
static int barrera(FatVol *v)                               /* antes de lo que compromete: lo de antes, en la tarjeta */
{
    if (v->roto) return -1;
    if (v->vaciar && v->vaciar(v->ctx)) { v->errores_w++; v->fallo_es = 1; roto(v); return -1; }
    return 0;
}
static u8 *sec_rw(FatVol *v, u32 lba, u32 *idx)            /* el sector en la caché, para modificarlo */
{
    const u8 *p = sec(v, lba); u32 i;
    if (!p) return 0;
    i = (u32)(p - v->c_dat[0]) / 512;
    if (idx) *idx = i;
    return v->c_dat[i];
}
static int sec_esc(FatVol *v, u32 lba)                     /* escribe lo que hay en la caché de ese sector */
{
    u32 i;
    for (i = 0; i < FAT_CACHE; i++) if (v->c_hay[i] && v->c_lba[i] == lba) return dev_esc(v, lba, 1, v->c_dat[i]);
    return -1;
}
static int pend_vaciar(FatVol *v)                           /* R1 */
{
    u32 i, k, hay = 0;
    for (i = 0; i < FAT_CACHE; i++) if (v->c_hay[i] && v->c_pend[i]) { hay = 1; if (dev_esc(v, v->c_lba[i], 1, v->c_dat[i])) return -1; }
    if (!hay) return 0;
    for (k = 1; k < v->nfat; k++)
        for (i = 0; i < FAT_CACHE; i++) if (v->c_hay[i] && v->c_pend[i] && dev_esc(v, v->c_lba[i] + k * v->sfat, 1, v->c_dat[i])) return -1;
    memset(v->c_pend, 0, sizeof v->c_pend);
    return 0;
}

/* ---- la FAT, en crudo ---- */
static u32 fin_cadena(const FatVol *v) { return v->tipo == 32 ? 0x0FFFFFFFu : v->tipo == 16 ? 0xFFFFu : 0xFFFu; }
static int es_fin(const FatVol *v, u32 x) { return x >= (v->tipo == 32 ? 0x0FFFFFF8u : v->tipo == 16 ? 0xFFF8u : 0xFF8u); }
static int es_mala(const FatVol *v, u32 x) { return x == (v->tipo == 32 ? 0x0FFFFFF7u : v->tipo == 16 ? 0xFFF7u : 0xFF7u); }
static int fat_val(FatVol *v, u32 c, u32 *x)                /* 0 = bien */
{
    const u8 *p; u32 o;
    if (v->tipo == 32) { o = c * 4; if (!(p = sec(v, v->s_fat + o / 512))) return -1; *x = le32(p + o % 512) & 0x0FFFFFFFu; }
    else if (v->tipo == 16) { o = c * 2; if (!(p = sec(v, v->s_fat + o / 512))) return -1; *x = le16(p + o % 512); }
    else {
        u32 y;
        o = c + c / 2; if (!(p = sec(v, v->s_fat + o / 512))) return -1;
        y = p[o % 512];
        if (!(p = sec(v, v->s_fat + (o + 1) / 512))) return -1;
        y |= (u32)p[(o + 1) % 512] << 8;
        *x = (c & 1) ? y >> 4 : y & 0xFFF;
    }
    return 0;
}
static int fat_poner(FatVol *v, u32 c, u32 x)
{
    u8 *p; u32 o, i;
    if (c >= v->nclusters + 2) { v->topes++; return -1; }  /* tope */
    if (v->tipo == 32) {
        o = c * 4; if (!(p = sec_rw(v, v->s_fat + o / 512, &i))) return -1;
        w32(p + o % 512, (le32(p + o % 512) & 0xF0000000u) | (x & 0x0FFFFFFFu)); v->c_pend[i] = 1;
    } else if (v->tipo == 16) {
        o = c * 2; if (!(p = sec_rw(v, v->s_fat + o / 512, &i))) return -1;
        w16(p + o % 512, x); v->c_pend[i] = 1;
    } else {
        o = c + c / 2;
        if (!(p = sec_rw(v, v->s_fat + o / 512, &i))) return -1;
        p[o % 512] = (c & 1) ? (u8)((p[o % 512] & 0x0F) | (x << 4 & 0xF0)) : (u8)x;
        v->c_pend[i] = 1;
        if (!(p = sec_rw(v, v->s_fat + (o + 1) / 512, &i))) return -1;
        p[(o + 1) % 512] = (c & 1) ? (u8)(x >> 4) : (u8)((p[(o + 1) % 512] & 0xF0) | (x >> 8 & 0x0F));
        v->c_pend[i] = 1;
    }
    return 0;
}
static u32 buscar_libre(FatVol *v)                          /* 0 = no hay (o no se pudo leer: v->fallo_es) */
{
    u32 n = v->nclusters, c = v->pista, k, x;
    if (c < 2 || c >= n + 2) c = 2;
    for (k = 0; k < n; k++) {
        if (fat_val(v, c, &x)) return 0;
        if (x == 0) { v->pista = c + 1 < n + 2 ? c + 1 : 2; return c; }
        if (++c >= n + 2) c = 2;
    }
    return 0;
}
static int liberar_cadena(FatVol *v, u32 c)                 /* desde «c» (incluido) hasta el fin; acotado */
{
    u32 k, x;
    for (k = 0; c >= 2 && c < v->nclusters + 2 && k <= v->nclusters; k++) {
        if (fat_val(v, c, &x)) return -1;
        if (x == 0) break;                                  /* ya libre: no es nuestra */
        if (fat_poner(v, c, 0)) return -1;
        if (v->libres_ok) v->libres++;
        if (es_fin(v, x) || es_mala(v, x)) break;
        c = x;
    }
    return pend_vaciar(v);
}
u32 fat_libres(FatVol *v)
{
    u32 c, x, n = 0;
    if (!v->montado) return 0;
    if (v->libres_ok) return v->libres;
    for (c = 2; c < v->nclusters + 2; c++) { if (fat_val(v, c, &x)) return 0; if (!x) n++; if (v->latido && !(c & 1023)) v->latido(); }
    v->libres = n; v->libres_ok = 1;
    return n;
}

/* ---- sucio / limpio (R5) ---- */
static u32 bit_limpio(const FatVol *v) { return v->tipo == 32 ? 0x08000000u : v->tipo == 16 ? 0x8000u : 0; }
static int poner_sucio(FatVol *v, int sucio)
{
    u8 *p; u32 x, off = v->tipo == 32 ? 65 : 37;
    if (!sucio && v->s_info) {                              /* FSInfo al día ANTES de quitar la marca */
        if (!(p = sec_rw(v, v->s_info, 0))) return -1;
        if (le32(p) == 0x41615252u && le32(p + 484) == 0x61417272u) {
            w32(p + 488, v->libres_ok ? v->libres : 0xFFFFFFFFu); w32(p + 492, v->pista >= 2 ? v->pista : 0xFFFFFFFFu);
            if (sec_esc(v, v->s_info)) return -1;
        }
    }
    if (sucio) {
        if (!(p = sec_rw(v, v->s_arranque, 0))) return -1;
        if (!(p[off] & 1)) { p[off] |= 1; if (sec_esc(v, v->s_arranque)) return -1; }
    }
    if (bit_limpio(v)) {
        if (fat_val(v, 1, &x)) return -1;
        if (fat_poner(v, 1, sucio ? x & ~bit_limpio(v) : x | bit_limpio(v)) || pend_vaciar(v)) return -1;
    }
    if (sucio && v->s_info) {
        if (!(p = sec_rw(v, v->s_info, 0))) return -1;
        if (le32(p) == 0x41615252u && le32(p + 484) == 0x61417272u && le32(p + 488) != 0xFFFFFFFFu) {
            w32(p + 488, 0xFFFFFFFFu); if (sec_esc(v, v->s_info)) return -1;
        }
    }
    if (!sucio) {
        if (!(p = sec_rw(v, v->s_arranque, 0))) return -1;
        if (p[off] & 1) { p[off] &= (u8)~1; if (sec_esc(v, v->s_arranque)) return -1; }
    }
    v->sucio = (u8)sucio;
    return 0;
}
static int preparar(FatVol *v)                              /* antes de escribir nada */
{
    if (!v->montado) return FAT_E_NO_HAY;
    if (!v->rw || !v->escribir) return FAT_E_SOLO_LECTURA;
    if (v->roto || v->sucio_al_montar) return FAT_E_ROTO;
    olvidar(v);
    if (!v->sucio && poner_sucio(v, 1)) return roto(v);
    return 0;
}
/* (28-oct) LA ZONA DE CIERRE de la tarjeta (docs/tarjeta_escritura.md §10; hal/ms.c, «el cierre»): «n» clústeres marcados MALOS (0xFFF7:
 * ninguna FAT los da a un fichero —ni la nuestra, ni la del original, ni la de un PC— y fat_revisar no los libera), cada uno en un BLOQUE DE
 * BORRADO distinto («sect_blq» sectores), con FAT_CIERRE_FIRMA al principio de su primer sector (así no se toma por nuestro un clúster malo de
 * verdad). Busca los que ya hay, desde el final del volumen; si faltan y se puede escribir, marca LIBRES del final: primero la firma, luego la
 * FAT (R2: un corte entre medias deja un clúster libre con basura, nada más). Devuelve cuántos hay (y el primer sector de cada uno en lba[]). */
static u32 mala(const FatVol *v) { return v->tipo == 32 ? 0x0FFFFFF7u : v->tipo == 16 ? 0xFFF7u : 0xFF7u; }
static int otro_bloque(const u32 *lba, u32 k, u32 s, u32 sect_blq) { u32 i; for (i = 0; i < k; i++) if (lba[i] / sect_blq == s / sect_blq) return 0; return 1; }
int fat_zona_cierre(FatVol *v, u32 *lba, u32 n, u32 sect_blq)
{
    u32 c, x, k = 0; const u8 *p; int e;
    if (!v->montado) return FAT_E_NO_HAY;
    if (!sect_blq) sect_blq = 1;
    for (c = v->nclusters + 1; c >= 2 && k < n; c--) {     /* 1. los que ya hay */
        if (v->latido && !(c & 1023)) v->latido();
        if (fat_val(v, c, &x)) return FAT_E_ES;
        if (!es_mala(v, x) || !otro_bloque(lba, k, sector_de(v, c), sect_blq)) continue;
        if (!(p = sec(v, sector_de(v, c)))) return FAT_E_ES;
        if (!memcmp(p, FAT_CIERRE_FIRMA, sizeof FAT_CIERRE_FIRMA - 1)) lba[k++] = sector_de(v, c);
    }
    if (k >= n || !v->rw) return (int)k;
    if ((e = preparar(v)) != 0) return k ? (int)k : e;      /* 2. faltan: libres del final, cada uno en otro bloque */
    memset(sec_tmp, 0, 512); memcpy(sec_tmp, FAT_CIERRE_FIRMA, sizeof FAT_CIERRE_FIRMA - 1);
    for (c = v->nclusters + 1; c >= 2 && k < n; c--) {
        if (fat_val(v, c, &x)) return roto(v);
        if (x || !otro_bloque(lba, k, sector_de(v, c), sect_blq)) continue;
        if (dev_esc(v, sector_de(v, c), 1, sec_tmp)) return roto(v);
        if (fat_poner(v, c, mala(v))) return roto(v);
        if (v->libres_ok) v->libres--;
        lba[k++] = sector_de(v, c);
    }
    if (pend_vaciar(v)) return roto(v);
    return (int)k;
}
int fat_limpio(FatVol *v)
{
    if (!v->montado || !v->rw || !v->sucio) return 0;
    if (v->roto) return FAT_E_ROTO;
    if (v->abiertos) return FAT_E_OCUPADA;
    if (pend_vaciar(v) || barrera(v) || poner_sucio(v, 0) || barrera(v)) return roto(v);
    return 0;
}

int fat_montar_rw(FatVol *v, FatLeer leer, FatEscribir escribir, void *ctx, u32 sectores, FatFecha fecha)
{
    const u8 *b; u32 x, info;
    int e = fat_montar(v, leer, ctx, sectores);
    if (e) return e;
    v->escribir = escribir; v->fecha = fecha;
    v->s_arranque = v->part;
    if (!(b = sec(v, v->s_arranque))) return FAT_E_ES;
    v->nfat = b[16]; info = le16(b + 48);
    v->sucio_al_montar = (b[v->tipo == 32 ? 65 : 37] & 1) != 0;
    if (v->tipo == 32 && info && info < v->s_fat - v->part) v->s_info = v->part + info;
    if (bit_limpio(v)) { if (fat_val(v, 1, &x)) return FAT_E_ES; if (!(x & bit_limpio(v))) v->sucio_al_montar = 1; }
    if (v->s_info && (b = sec(v, v->s_info)) && le32(b) == 0x41615252u && le32(b + 484) == 0x61417272u) {
        u32 l = le32(b + 488), p = le32(b + 492);
        if (!v->sucio_al_montar && l <= v->nclusters) { v->libres = l; v->libres_ok = 1; }
        if (p >= 2 && p < v->nclusters + 2) v->pista = p;
    }
    v->rw = escribir != 0;
    return 0;
}

/* ---- nombres ---- */
static u32 dos_fecha(FatVol *v) { return v->fecha ? v->fecha() : (u32)((20u << 9 | 1u << 5 | 1u) << 16); }
static int prohibido(u32 c) { return c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|' || c == 0x7F; }
/* UTF-8 (n bytes) a UTF-16; FAT_E_NOMBRE si no vale */
static int a_u16(const char *s, u32 n, u16 *u, u32 *nu)
{
    u32 i = 0, k = 0;
    if (!n || n >= FAT_NOMBRE) return FAT_E_NOMBRE;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return FAT_E_NOMBRE;
    if (s[n - 1] == '.' || s[n - 1] == ' ') return FAT_E_NOMBRE;   /* Windows los quita: aquí no se aceptan */
    while (i < n) {
        u32 c = (u8)s[i], m;
        if (c < 0x80) m = 0; else if ((c & 0xE0) == 0xC0) { c &= 0x1F; m = 1; } else if ((c & 0xF0) == 0xE0) { c &= 0x0F; m = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; m = 3; } else return FAT_E_NOMBRE;
        if (m && i + m >= n) return FAT_E_NOMBRE;
        for (i++; m; m--, i++) { if (((u8)s[i] & 0xC0) != 0x80) return FAT_E_NOMBRE; c = c << 6 | ((u8)s[i] & 0x3F); }
        if (prohibido(c) || (c >= 0xD800 && c < 0xE000) || c > 0x10FFFF) return FAT_E_NOMBRE;
        if (c >= 0x10000) { if (k + 2 > 255) return FAT_E_NOMBRE; c -= 0x10000; u[k++] = (u16)(0xD800 + (c >> 10)); u[k++] = (u16)(0xDC00 + (c & 0x3FF)); }
        else { if (k + 1 > 255) return FAT_E_NOMBRE; u[k++] = (u16)c; }
    }
    *nu = k;
    return 0;
}
static int corto_ok(u32 c)
{
    static const char otros[] = "$%'-_@~`!(){}^#&";
    u32 i;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    for (i = 0; otros[i]; i++) if ((u32)(u8)otros[i] == c) return 1;
    return 0;
}
/* ¿Cabe tal cual en 8.3 (con las minúsculas de NT)? 1 y el nombre de 11 y el byte 12; 0 si hace falta nombre largo */
static int corto_directo(const u16 *u, u32 n, u8 out[11], u8 *nt)
{
    u32 i, p = n, b, e, mb = 0, Mb = 0, me = 0, Me = 0;
    for (i = 0; i < n; i++) if (u[i] == '.') { if (p != n) return 0; p = i; }
    b = p; e = p == n ? 0 : n - p - 1;
    if (!b || b > 8 || e > 3 || (p != n && !e)) return 0;
    memset(out, ' ', 11);
    for (i = 0; i < n; i++) {
        u32 c = u[i], lo, up;
        if (i == p) continue;
        lo = c >= 'a' && c <= 'z'; up = c >= 'A' && c <= 'Z';
        if (lo) c -= 32;
        if (!corto_ok(c)) return 0;
        if (i < p) { mb |= lo; Mb |= up; out[i] = (u8)c; } else { me |= lo; Me |= up; out[8 + i - p - 1] = (u8)c; }
    }
    if ((mb && Mb) || (me && Me)) return 0;
    if (out[0] == 0xE5) return 0;
    *nt = (u8)((mb ? 0x08 : 0) | (me ? 0x10 : 0));
    return 1;
}
static void corto_base(const u16 *u, u32 n, u8 base[8], u32 *nb, u8 ext[3], u32 *ne)
{
    u32 i, p = n, k;
    for (i = n; i; i--) if (u[i - 1] == '.') { p = i - 1; break; }
    if (p == 0) p = n;                                      /* «.bashrc»: sin extensión */
    for (i = 0, k = 0; i < p && k < 8; i++) {
        u32 c = u[i];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        base[k++] = (u8)(corto_ok(c) ? c : '_');
    }
    if (!k) base[k++] = '_';
    *nb = k;
    for (i = p + 1, k = 0; p < n && i < n && k < 3; i++) {
        u32 c = u[i];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        ext[k++] = (u8)(corto_ok(c) ? c : '_');
    }
    *ne = k;
}
static u8 suma_corto(const u8 *n11) { u32 i, s = 0; for (i = 0; i < 11; i++) s = ((s >> 1 | (s & 1) << 7) + n11[i]) & 0xFF; return (u8)s; }

/* ---- directorios: buscar, huecos, escribir grupos ---- */
static u32 dir_ini(const FatVol *v, u32 cl, int raiz) { return raiz ? (v->tipo == 32 ? v->raiz_cl : 0) : cl; }
static int buscar(FatVol *v, u32 dir_cl, int raiz, const char *nom, u32 n, Ent *e)
{
    FatDir d;
    dir_abrir(&d, dir_ini(v, dir_cl, raiz));
    while (dir_sig(v, &d, e)) if (igual_sin_caja(e->nombre, nom, n) || igual_sin_caja(e->corto, nom, n)) return 1;
    return 0;
}
/* ¿Hay ya una entrada corta con esos 11 bytes? (sin contar la de «ign_lba/ign_idx») */
static int corto_usado(FatVol *v, u32 dir_cl, int raiz, const u8 *n11, u32 ign_lba, u32 ign_idx)
{
    FatDir d; const u8 *p;
    dir_abrir(&d, dir_ini(v, dir_cl, raiz));
    while ((p = dir_entrada(v, &d)) != 0) {
        if (p[0] == 0) return 0;
        if (p[0] == 0xE5 || (p[11] & 0x3F) == 0x0F || (p[11] & 0x08)) continue;
        if (!memcmp(p, n11, 11) && !(d.s_lba == ign_lba && (d.i - 1) % 16 == ign_idx)) return 1;
    }
    return v->fallo_es ? 1 : 0;
}
/* Busca «n» huecos seguidos (borrados o desde el 0x00 del final, nunca DETRÁS de él). Primero dentro de un sector (R4). */
static int dir_huecos(FatVol *v, u32 dir_cl, int raiz, u32 n, FatDir *res)
{
    u32 pasada;
    for (pasada = n <= 16 ? 0 : 1; pasada < 2; pasada++) {
        FatDir d, antes, ini; const u8 *p; u32 racha = 0; int fin = 0;
        dir_abrir(&d, dir_ini(v, dir_cl, raiz));
        for (;;) {
            antes = d;
            if (!(p = dir_entrada(v, &d))) break;
            if (pasada == 0 && antes.i % 16 == 0 && !fin) racha = 0;
            if (fin || p[0] == 0 || p[0] == 0xE5) {
                if (p[0] == 0 && !fin) { fin = 1; if (pasada == 0 && racha && (antes.i % 16) + n > 16) racha = 0; }
                if (!racha) ini = antes;
                if (++racha == n) {
                    if (pasada == 0 && ini.i / 16 != antes.i / 16) { racha = 0; if (fin) break; continue; }
                    *res = ini; return 0;
                }
            } else racha = 0;
        }
        if (v->fallo_es) return FAT_E_ES;
    }
    return FAT_E_LLENO;
}
static int dir_crecer(FatVol *v, u32 dir_cl, int raiz)     /* un clúster más (a ceros) al final de la carpeta */
{
    u32 c = dir_ini(v, dir_cl, raiz), k, x, nuevo, s;
    if (c == 0) return FAT_E_LLENO;                         /* la raíz fija de FAT12/16 no crece */
    for (k = 0; k <= v->nclusters; k++) {
        if (fat_val(v, c, &x)) return FAT_E_ES;
        if (es_fin(v, x)) break;
        if (x < 2 || x >= v->nclusters + 2) return FAT_E_ROTO;
        c = x;
    }
    if (k > v->nclusters || (k + 1) * v->spc * 16 > 65536) return FAT_E_LLENO;   /* el estándar: 65 536 entradas como mucho */
    if (!(nuevo = buscar_libre(v))) return v->fallo_es ? FAT_E_ES : FAT_E_LLENO;
    if (fat_poner(v, nuevo, fin_cadena(v)) || pend_vaciar(v)) return roto(v);   /* R2: reservado (sin dueño) */
    if (v->libres_ok) v->libres--;
    for (s = 0; s < v->spc; s++) if (dev_esc(v, sector_de(v, nuevo) + s, 1, cero)) return roto(v);
    if (fat_poner(v, c, nuevo) || pend_vaciar(v)) return roto(v);   /* enlazado: la carpeta tiene un clúster vacío más */
    return 0;
}
/* Escribe «ns» entradas de 32 (o 0xE5 si «ents» es NULL) desde «ini», por sectores: al crear, en orden; al borrar, del
 * último sector (el de la corta) al primero (R4). */
static int dir_grupo(FatVol *v, const FatDir *ini, u32 ns, const u8 (*ents)[32], int al_reves)
{
    u32 lba[21], idx[21], k, j, nsec = 0, sl[3];
    FatDir d = *ini;
    if (ns > 21) return FAT_E_NOMBRE;
    for (k = 0; k < ns; k++) {
        if (!dir_entrada(v, &d)) return v->fallo_es ? FAT_E_ES : FAT_E_ROTO;
        lba[k] = d.s_lba; idx[k] = (d.i - 1) % 16;
        if (!nsec || sl[nsec - 1] != lba[k]) { if (nsec == 3) return FAT_E_ROTO; sl[nsec++] = lba[k]; }
    }
    for (j = 0; j < nsec; j++) {
        u32 s = sl[al_reves ? nsec - 1 - j : j];
        u8 *p = sec_rw(v, s, 0);
        if (!p) return FAT_E_ES;
        for (k = 0; k < ns; k++) if (lba[k] == s) { if (ents) memcpy(p + idx[k] * 32, ents[k], 32); else p[idx[k] * 32] = 0xE5; }
        if (sec_esc(v, s)) return roto(v);
    }
    return 0;
}
/* Monta las entradas de un nombre: largo (si hace falta) + corto. Devuelve cuántas, o FAT_E_*. «plantilla»: los 32 bytes
 * de la corta (atributos, fechas, clúster, tamaño): se le pone el nombre. */
static int hacer_grupo(FatVol *v, u32 dir_cl, int raiz, const char *nom, u32 n, const u8 plantilla[32], u8 (*ents)[32],
                       u32 ign_lba, u32 ign_idx)
{
    static u16 u[256];
    static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    u32 nu, partes, k, i; u8 n11[11], nt = 0, base[8], ext[3]; u32 nb, ne;
    int e = a_u16(nom, n, u, &nu);
    if (e) return e;
    if (corto_directo(u, nu, n11, &nt) && !corto_usado(v, dir_cl, raiz, n11, ign_lba, ign_idx)) partes = 0;
    else {
        u32 t, h = 0;
        corto_base(u, nu, base, &nb, ext, &ne);
        for (i = 0; i < nu; i++) h = h * 31 + u[i];
        for (t = 1; t < 200; t++) {
            char cola[8]; u32 nc = 0, x = t <= 4 ? t : 1, m, j;
            u8 b2[8]; u32 nb2 = nb;
            memcpy(b2, base, 8);
            if (t > 4) {                                    /* como Windows: dos letras y cuatro cifras de un resumen */
                u32 hh = (h + t * 2654435761u) & 0xFFFF;
                static const char hx[] = "0123456789ABCDEF";
                nb2 = nb < 2 ? nb : 2;
                for (j = 0; j < 4; j++) b2[nb2++] = (u8)hx[hh >> (12 - 4 * j) & 15];
            }
            do { cola[nc++] = (char)('0' + x % 10); x /= 10; } while (x);
            m = 8 - 1 - nc; if (nb2 < m) m = nb2;
            memset(n11, ' ', 11);
            memcpy(n11, b2, m); n11[m] = '~';
            for (j = 0; j < nc; j++) n11[m + 1 + j] = (u8)cola[nc - 1 - j];
            memcpy(n11 + 8, ext, ne);
            if (!corto_usado(v, dir_cl, raiz, n11, ign_lba, ign_idx)) break;
            if (v->fallo_es) return FAT_E_ES;
        }
        if (t == 200) return FAT_E_EXISTE;
        nt = 0;
        partes = (nu + 12) / 13;
    }
    for (k = 0; k < partes; k++) {                          /* la entrada 0 es el trozo de MÁS número */
        u32 ord = partes - k; u8 *p = ents[k]; u8 s = suma_corto(n11);
        memset(p, 0, 32);
        p[0] = (u8)(ord | (k == 0 ? 0x40 : 0)); p[11] = 0x0F; p[13] = s;
        for (i = 0; i < 13; i++) {
            u32 j = (ord - 1) * 13 + i, c = j < nu ? u[j] : j == nu ? 0 : 0xFFFF;
            w16(p + pos[i], c);
        }
    }
    memcpy(ents[partes], plantilla, 32);
    memcpy(ents[partes], n11, 11);
    ents[partes][12] = nt;
    return (int)(partes + 1);
}
static void plantilla(FatVol *v, u8 p[32], u8 attr, u32 cl, u32 tam)
{
    u32 f = dos_fecha(v);
    memset(p, 0, 32);
    p[11] = attr;
    w16(p + 14, f & 0xFFFF); w16(p + 16, f >> 16); w16(p + 18, f >> 16);
    w16(p + 20, v->tipo == 32 ? cl >> 16 : 0); w16(p + 22, f & 0xFFFF); w16(p + 24, f >> 16); w16(p + 26, cl & 0xFFFF);
    w32(p + 28, tam);
}
static int crear_entrada(FatVol *v, u32 dir_cl, int raiz, const char *nom, u32 n, const u8 pl[32], u32 ign_lba, u32 ign_idx)
{
    static u8 ents[21][32];
    FatDir d; int ns, e;
    if ((ns = hacer_grupo(v, dir_cl, raiz, nom, n, pl, ents, ign_lba, ign_idx)) < 0) return ns;
    e = dir_huecos(v, dir_cl, raiz, (u32)ns, &d);
    if (e == FAT_E_LLENO) { if ((e = dir_crecer(v, dir_cl, raiz)) != 0) return e; e = dir_huecos(v, dir_cl, raiz, (u32)ns, &d); }
    if (e) return e;
    if (barrera(v)) return FAT_E_ROTO;
    return dir_grupo(v, &d, (u32)ns, (const u8 (*)[32])ents, 0);
}
/* «/a/b/c» -> la carpeta de «/a/b» y el nombre «c» */
static int partir(FatVol *v, const char *ruta, u32 *dir_cl, int *raiz, const char **nom, u32 *n)
{
    static char padre[FAT_NOMBRE];
    u32 l = 0, ult = 0; Res r;
    if (!ruta || ruta[0] != '/') return FAT_E_NO_HAY;
    while (ruta[l]) { if (ruta[l] == '/') ult = l; l++; }
    if (l >= FAT_NOMBRE) return FAT_E_NOMBRE;
    *nom = ruta + ult + 1; *n = l - ult - 1;
    if (!*n) return FAT_E_NOMBRE;
    memcpy(padre, ruta, ult ? ult : 1); padre[ult ? ult : 1] = 0;
    if (!resolver(v, padre, &r)) return v->fallo_es ? FAT_E_ES : FAT_E_NO_HAY;
    if (!(r.attr & 0x10)) return FAT_E_NO_DIR;
    if (!r.raiz && r.cluster < 2) return FAT_E_ROTO;        /* una carpeta sin clúster: no se escribe dentro */
    *dir_cl = r.cluster; *raiz = r.raiz;
    olvidar(v);
    return 0;
}
static int poner_entrada_corta(FatVol *v, const Ent *e, u32 cl, u32 tam, int con_fecha)
{
    u8 *p;
    if (barrera(v)) return FAT_E_ROTO;
    p = sec_rw(v, e->lba, 0);
    if (!p) return FAT_E_ES;
    p += e->idx * 32;
    w16(p + 20, v->tipo == 32 ? cl >> 16 : 0); w16(p + 26, cl & 0xFFFF); w32(p + 28, tam);
    if (con_fecha) { u32 f = dos_fecha(v); w16(p + 22, f & 0xFFFF); w16(p + 24, f >> 16); w16(p + 18, f >> 16); p[11] |= 0x20; }
    return sec_esc(v, e->lba) ? roto(v) : 0;
}

/* ---- escritura a trozos ---- */
static Ent ent_w;
int fat_esc_abrir(FatVol *v, FatEsc *f, const char *ruta, int modo)
{
    const char *nom; u32 n; int e, hay;
    memset(f, 0, sizeof *f);
    if ((e = preparar(v)) != 0) return e;
    if ((e = partir(v, ruta, &f->dir_cl, &hay, &nom, &n)) != 0) return e;
    f->raiz = (u8)hay;
    { u32 nu; if ((e = a_u16(nom, n, u_tmp, &nu)) != 0) return e; }
    memcpy(f->nombre, nom, n); f->nombre[n] = 0;
    hay = buscar(v, f->dir_cl, f->raiz, nom, n, &ent_w);
    if (v->fallo_es) return FAT_E_ES;
    if (hay && (ent_w.attr & 0x10)) return FAT_E_ES_DIR;
    if (hay && modo == FAT_ESC_NUEVO) return FAT_E_EXISTE;
    if (hay && modo == FAT_ESC_ANADIR) {                    /* el último clúster de lo que hay */
        u32 bpc = v->spc * 512, need = (ent_w.tam + bpc - 1) / bpc, c = ent_w.cluster, k, x;
        if (ent_w.tam && (c < 2 || c >= v->nclusters + 2)) return FAT_E_ROTO;
        for (k = 1; k < need; k++) {
            if (fat_val(v, c, &x)) return FAT_E_ES;
            if (x < 2 || x >= v->nclusters + 2) return FAT_E_ROTO;   /* la cadena es más corta que el tamaño: fat_revisar */
            c = x;
        }
        f->primero = ent_w.tam ? ent_w.cluster : 0; f->viejo_ult = ent_w.tam ? c : 0; f->ultimo = f->viejo_ult; f->tam = ent_w.tam;
        if (!ent_w.tam && ent_w.cluster) f->modo = FAT_ESC_REEMPLAZAR;   /* vacío con clúster (raro): se reemplaza */
    }
    if (!f->modo) f->modo = (u8)modo;
    v->abiertos++;
    return 0;
}
static int esc_tramo(FatVol *v, u32 lba, u32 des, const u8 *d, u32 n)   /* en una racha de sectores seguidos */
{
    while (n) {
        u32 s = lba + des / 512, o = des % 512, m;
        if (o == 0 && n >= 512) { m = n / 512; if (dev_esc(v, s, m, d)) return -1; m *= 512; }
        else {
            u8 *p = sec_rw(v, s, 0);
            if (!p) return -1;
            m = 512 - o < n ? 512 - o : n;
            memcpy(p + o, d, m);
            if (sec_esc(v, s)) return -1;
        }
        d += m; des += m; n -= m;
    }
    return 0;
}
int fat_esc_mas(FatVol *v, FatEsc *f, const u8 *d, u32 n)
{
    u32 bpc = v->spc * 512;
    if (!f->modo) return FAT_E_NO_HAY;
    if (v->roto) return FAT_E_ROTO;
    if (n > 0xFFFFFFFFu - f->tam) return FAT_E_LLENO;      /* FAT: 4 GiB - 1 */
    olvidar(v);
    while (n) {
        u32 off = f->tam % bpc, m;
        if (off == 0 || !f->ultimo) {                       /* clústeres nuevos: reservar, volcar la FAT, y luego los datos (R2) */
            u32 cl[16], k, want = (n + bpc - 1) / bpc, i;
            if (want > 16) want = 16;
            for (k = 0; k < want; k++) {
                u32 c = buscar_libre(v);
                if (!c) { if (pend_vaciar(v)) return roto(v); return v->fallo_es ? FAT_E_ES : FAT_E_LLENO; }
                if (fat_poner(v, c, fin_cadena(v))) return roto(v);
                if (f->modo == FAT_ESC_ANADIR) {
                    if (!f->nuevo_pri) f->nuevo_pri = c; else if (fat_poner(v, f->ultimo, c)) return roto(v);
                    if (!f->viejo_ult && !f->primero) f->primero = c;
                } else { if (!f->primero) f->primero = c; else if (fat_poner(v, f->ultimo, c)) return roto(v); }
                f->ultimo = c; cl[k] = c;
                if (v->libres_ok) v->libres--;
            }
            if (pend_vaciar(v)) return roto(v);
            for (i = 0; i < k && n; ) {                     /* los datos, por rachas de clústeres seguidos */
                u32 j = i + 1, b;
                while (j < k && cl[j] == cl[j - 1] + 1) j++;
                b = (j - i) * bpc < n ? (j - i) * bpc : n;
                if (esc_tramo(v, sector_de(v, cl[i]), 0, d, b)) return roto(v);
                d += b; n -= b; f->tam += b; i = j;
            }
            continue;
        }
        m = bpc - off < n ? bpc - off : n;                  /* el resto del último clúster */
        if (esc_tramo(v, sector_de(v, f->ultimo), off, d, m)) return roto(v);
        d += m; n -= m; f->tam += m;
    }
    return 0;
}
void fat_esc_abortar(FatVol *v, FatEsc *f)
{
    if (!f->modo) return;
    if (!v->roto) {
        if (f->modo == FAT_ESC_ANADIR) liberar_cadena(v, f->nuevo_pri);
        else liberar_cadena(v, f->primero);
    }
    f->modo = 0;
    if (v->abiertos) v->abiertos--;
}
int fat_esc_cerrar(FatVol *v, FatEsc *f)
{
    u32 n = 0; int e = 0, hay;
    if (!f->modo) return FAT_E_NO_HAY;
    if (v->roto) { f->modo = 0; if (v->abiertos) v->abiertos--; return FAT_E_ROTO; }
    olvidar(v);
    while (f->nombre[n]) n++;
    hay = buscar(v, f->dir_cl, f->raiz, f->nombre, n, &ent_w);
    if (v->fallo_es) { e = FAT_E_ES; goto mal; }
    if (hay && (ent_w.attr & 0x10)) { e = FAT_E_ES_DIR; goto mal; }
    if (f->modo == FAT_ESC_ANADIR && hay) {
        if (f->nuevo_pri) {                                 /* R3: enlazar, volcar, y luego el tamaño */
            if (f->viejo_ult) { if (fat_poner(v, f->viejo_ult, f->nuevo_pri) || pend_vaciar(v)) { e = roto(v); goto fuera; } }
            else f->primero = f->nuevo_pri;
        }
        e = poner_entrada_corta(v, &ent_w, f->primero, f->tam, 1);
    } else if (hay && f->modo == FAT_ESC_NUEVO) { e = FAT_E_EXISTE; goto mal; }
    else if (hay) {                                         /* REEMPLAZAR (o ANADIR a uno que ya no está: aquí no llega) */
        u32 viejo = ent_w.cluster;
        e = poner_entrada_corta(v, &ent_w, f->primero, f->tam, 1);   /* R3: una escritura y ya es el nuevo */
        if (!e && viejo != f->primero && liberar_cadena(v, viejo)) e = roto(v);
    } else {
        u8 pl[32];
        if (f->modo == FAT_ESC_ANADIR && f->nuevo_pri) f->primero = f->nuevo_pri;   /* se borró mientras: un fichero nuevo con lo añadido */
        plantilla(v, pl, 0x20, f->primero, f->tam);
        e = crear_entrada(v, f->dir_cl, f->raiz, f->nombre, n, pl, 0xFFFFFFFFu, 0);
        if (e) goto mal;
    }
fuera:
    f->modo = 0; if (v->abiertos) v->abiertos--;
    return e;
mal:
    fat_esc_abortar(v, f);
    return e;
}
int fat_esc_destino(FatVol *v, FatEsc *f, const char *ruta, int modo)
{
    const char *nom; u32 n, dc; int e, raiz;
    if (!f->modo || f->modo == FAT_ESC_ANADIR || (modo != FAT_ESC_NUEVO && modo != FAT_ESC_REEMPLAZAR)) return FAT_E_NO_HAY;
    if ((e = partir(v, ruta, &dc, &raiz, &nom, &n)) != 0) return e;
    { u32 nu; if ((e = a_u16(nom, n, u_tmp, &nu)) != 0) return e; }
    f->dir_cl = dc; f->raiz = (u8)raiz; f->modo = (u8)modo;
    memcpy(f->nombre, nom, n); f->nombre[n] = 0;
    return 0;
}
int fat_guardar(FatVol *v, const char *ruta, const u8 *d, u32 n)
{
    FatEsc f; int e;
    if ((e = fat_esc_abrir(v, &f, ruta, FAT_ESC_REEMPLAZAR)) != 0) return e;
    if ((e = fat_esc_mas(v, &f, d, n)) != 0) { fat_esc_abortar(v, &f); return e; }
    return fat_esc_cerrar(v, &f);
}

/* ---- truncar, borrar, carpetas, renombrar ---- */
static int buscar_ruta(FatVol *v, const char *ruta, u32 *dir_cl, int *raiz, Ent *e)
{
    const char *nom; u32 n; int r;
    if ((r = partir(v, ruta, dir_cl, raiz, &nom, &n)) != 0) return r;
    if (!buscar(v, *dir_cl, *raiz, nom, n, e)) return v->fallo_es ? FAT_E_ES : FAT_E_NO_HAY;
    return 0;
}
int fat_truncar(FatVol *v, const char *ruta, u32 tam)
{
    u32 dc, bpc = v->spc * 512, need, c, k, x; int raiz, e;
    if ((e = preparar(v)) != 0) return e;
    if (v->abiertos) return FAT_E_OCUPADA;
    if ((e = buscar_ruta(v, ruta, &dc, &raiz, &ent_w)) != 0) return e;
    if (ent_w.attr & 0x10) return FAT_E_ES_DIR;
    if (tam >= ent_w.tam) return tam == ent_w.tam ? 0 : FAT_E_LLENO;   /* alargar no se hace aquí */
    need = (tam + bpc - 1) / bpc; c = ent_w.cluster;
    if ((e = poner_entrada_corta(v, &ent_w, need ? c : 0, tam, 1)) != 0) return e;   /* primero el tamaño (la cadena queda larga) */
    if (!need) return liberar_cadena(v, c) ? roto(v) : 0;
    for (k = 1; k < need; k++) { if (fat_val(v, c, &x)) return FAT_E_ES; if (x < 2 || x >= v->nclusters + 2) return 0; c = x; }
    if (fat_val(v, c, &x)) return FAT_E_ES;
    if (es_fin(v, x)) return 0;
    if (fat_poner(v, c, fin_cadena(v)) || pend_vaciar(v)) return roto(v);   /* luego el fin de cadena: el resto queda sin dueño */
    return x >= 2 && x < v->nclusters + 2 && liberar_cadena(v, x) ? roto(v) : 0;
}
static int dir_vacia(FatVol *v, u32 cl)
{
    FatDir d; static Ent e;
    if (cl < 2) return 1;
    dir_abrir(&d, cl);
    if (dir_sig(v, &d, &e)) return 0;
    return v->fallo_es ? -1 : 1;
}
int fat_borrar(FatVol *v, const char *ruta)
{
    u32 dc, cl; int raiz, e;
    if ((e = preparar(v)) != 0) return e;
    if (v->abiertos) return FAT_E_OCUPADA;
    if ((e = buscar_ruta(v, ruta, &dc, &raiz, &ent_w)) != 0) return e;
    cl = ent_w.cluster;
    if (ent_w.attr & 0x10) {
        int vac = dir_vacia(v, cl);
        if (vac < 0) return FAT_E_ES;
        if (!vac) return FAT_E_NO_VACIA;
        if (cl == dir_ini(v, 0, 1)) return FAT_E_ROTO;
    }
    if ((e = dir_grupo(v, &ent_w.ini, ent_w.nslots, 0, 1)) != 0) return e;   /* la entrada fuera… */
    return cl && liberar_cadena(v, cl) ? roto(v) : 0;                         /* …y luego sus clústeres (sin dueño si se corta) */
}
int fat_mkdir(FatVol *v, const char *ruta)
{
    const char *nom; u32 n, dc, c, s; int raiz, e; u8 pl[32];
    if ((e = preparar(v)) != 0) return e;
    if ((e = partir(v, ruta, &dc, &raiz, &nom, &n)) != 0) return e;
    if (buscar(v, dc, raiz, nom, n, &ent_w)) return FAT_E_EXISTE;
    if (v->fallo_es) return FAT_E_ES;
    { u32 nu; if ((e = a_u16(nom, n, u_tmp, &nu)) != 0) return e; }
    if (!(c = buscar_libre(v))) return v->fallo_es ? FAT_E_ES : FAT_E_LLENO;
    if (fat_poner(v, c, fin_cadena(v)) || pend_vaciar(v)) return roto(v);      /* R2 */
    if (v->libres_ok) v->libres--;
    for (s = 1; s < v->spc; s++) if (dev_esc(v, sector_de(v, c) + s, 1, cero)) return roto(v);
    memset(sec_tmp, 0, 512);
    plantilla(v, sec_tmp, 0x10, c, 0); memset(sec_tmp, ' ', 11); sec_tmp[0] = '.';
    plantilla(v, sec_tmp + 32, 0x10, raiz ? 0 : dc, 0); memset(sec_tmp + 32, ' ', 11); sec_tmp[32] = '.'; sec_tmp[33] = '.';
    if (dev_esc(v, sector_de(v, c), 1, sec_tmp)) return roto(v);
    plantilla(v, pl, 0x10, c, 0);
    e = crear_entrada(v, dc, raiz, nom, n, pl, 0xFFFFFFFFu, 0);
    if (e && !v->roto) liberar_cadena(v, c);
    return e;
}
int fat_renombrar(FatVol *v, const char *vieja, const char *nueva)
{
    static Ent dst; static u8 corta[32];
    const char *nom; u32 n, dc_v, dc_n; int raiz_v, raiz_n, e, hay;
    if ((e = preparar(v)) != 0) return e;
    if (v->abiertos) return FAT_E_OCUPADA;
    if ((e = buscar_ruta(v, vieja, &dc_v, &raiz_v, &ent_w)) != 0) return e;
    if ((e = partir(v, nueva, &dc_n, &raiz_n, &nom, &n)) != 0) return e;
    hay = buscar(v, dc_n, raiz_n, nom, n, &dst);
    if (v->fallo_es) return FAT_E_ES;
    if (hay && !(dst.lba == ent_w.lba && dst.idx == ent_w.idx)) return FAT_E_EXISTE;   /* (el mismo con otras mayúsculas: sí) */
    if (ent_w.attr & 0x10) {                                /* ¿una carpeta dentro de sí misma? se sube por los «..» */
        u32 c = raiz_n ? 0 : dc_n, k;
        for (k = 0; c >= 2 && k < 64; k++) {
            const u8 *p;
            if (c == ent_w.cluster) return FAT_E_DENTRO;
            if (!(p = sec(v, sector_de(v, c)))) return FAT_E_ES;
            if (p[32] != '.' || p[33] != '.') break;
            c = le16(p + 32 + 26) | (v->tipo == 32 ? le16(p + 32 + 20) << 16 : 0);
        }
        if (k == 64) return FAT_E_DENTRO;
    }
    { const u8 *p = sec(v, ent_w.lba); if (!p) return FAT_E_ES; memcpy(corta, p + ent_w.idx * 32, 32); }
    corta[13] = 0;
    e = crear_entrada(v, dc_n, raiz_n, nom, n, corta, ent_w.lba, ent_w.idx);   /* 1.º el nombre nuevo… */
    if (e) return e;
    if ((ent_w.attr & 0x10) && ent_w.cluster >= 2 && (dc_n != dc_v || raiz_n != raiz_v)) {   /* …el «..» de la carpeta movida… */
        u8 *p = sec_rw(v, sector_de(v, ent_w.cluster), 0); u32 pc = raiz_n ? 0 : dc_n;
        if (!p) return FAT_E_ES;
        if (p[32] == '.' && p[33] == '.') { w16(p + 32 + 20, v->tipo == 32 ? pc >> 16 : 0); w16(p + 32 + 26, pc & 0xFFFF); if (sec_esc(v, sector_de(v, ent_w.cluster))) return roto(v); }
    }
    olvidar(v);
    /* …y 2.º fuera el viejo (sus huecos no se han movido: crear solo usa huecos libres). Si se corta entre los dos, el
     * fichero está con los DOS nombres y la misma cadena: fat_revisar quita el segundo que encuentra */
    return dir_grupo(v, &ent_w.ini, ent_w.nslots, 0, 1);
}

/* ---- fat_revisar: el chkdsk mínimo (docs/fat_escritura.md §5) ---- */
#define REV_HONDO 24u
#define REV_VISTAS 512u                                     /* carpetas como mucho (más: no se revisa, y no se escribe) */
static u32 rev_vis[REV_VISTAS], rev_nvis, rev_pcl[REV_HONDO];
static struct { u32 lba, idx; } rev_lfn[21]; static u32 rev_nlfn, rev_espera; static u8 rev_suma;
static int rev_huerfanos(FatVol *v, FatRevision *r)        /* los trozos de nombre largo apuntados no tienen corta: fuera */
{
    u32 k;
    for (k = 0; k < rev_nlfn; k++) {
        u8 *p = sec_rw(v, rev_lfn[k].lba, 0);
        if (!p) return -1;
        p[rev_lfn[k].idx * 32] = 0xE5;
        if (sec_esc(v, rev_lfn[k].lba)) return -1;
        r->lfn_huerfanos++;
    }
    rev_nlfn = 0;
    return 0;
}
static int marcar(u8 *mapa, u32 base, u32 bits, u32 c)     /* 1 = ya estaba; 0 = marcado (o fuera de la ventana) */
{
    u32 b;
    if (c < base || c - base >= bits) return 0;
    b = c - base;
    if (mapa[b >> 3] & (1u << (b & 7))) return 1;
    mapa[b >> 3] |= (u8)(1u << (b & 7));
    return 0;
}
static int rev_ent_escribir(FatVol *v, u32 lba, u32 idx, const u8 *ent)
{
    u8 *p = sec_rw(v, lba, 0);
    if (!p) return -1;
    memcpy(p + idx * 32, ent, 32);
    return sec_esc(v, lba);
}
static void marcar_dir(FatVol *v, u8 *mapa, u32 base, u32 bits, u32 c)
{
    u32 k, x;
    for (k = 0; c >= 2 && c < v->nclusters + 2 && k < 65536u; k++) {
        if (marcar(mapa, base, bits, c)) return;
        if (fat_val(v, c, &x) || x < 2 || x >= v->nclusters + 2) return;
        c = x;
    }
}
/* Una vuelta al árbol. mapa == NULL: arregla tamaños/cadenas y nombres largos huérfanos. Con mapa: marca los clústeres con
 * dueño que caen en la ventana [base, base+bits) y resuelve los cruces. -1 = no se pudo recorrer entero */
static int rev_vuelta(FatVol *v, FatRevision *r, u8 *mapa, u32 base, u32 bits)
{
    static FatDir pila[REV_HONDO]; static u8 ent[32];
    u32 hondo = 1, bpc = v->spc * 512;
    dir_abrir(&pila[0], v->tipo == 32 ? v->raiz_cl : 0); rev_pcl[0] = v->tipo == 32 ? v->raiz_cl : 0;
    if (mapa && v->tipo == 32) marcar_dir(v, mapa, base, bits, v->raiz_cl);
    if (!mapa) { rev_nvis = 0; if (v->tipo == 32) rev_vis[rev_nvis++] = v->raiz_cl; }
    rev_nlfn = 0;
    while (hondo) {
        FatDir *d = &pila[hondo - 1]; const u8 *p; u32 lba, idx, c0, tam;
        if (!(p = dir_entrada(v, d))) { if (v->fallo_es) return -1; if (!mapa && rev_huerfanos(v, r)) return -1; rev_nlfn = 0; hondo--; continue; }
        memcpy(ent, p, 32); lba = d->s_lba; idx = (d->i - 1) % 16;
        if (v->latido && !(d->i & 63)) v->latido();
        if (ent[0] == 0) { if (!mapa && rev_huerfanos(v, r)) return -1; rev_nlfn = 0; hondo--; continue; }
        if (ent[0] == 0xE5) { if (!mapa && rev_huerfanos(v, r)) return -1; rev_nlfn = 0; continue; }
        if ((ent[11] & 0x3F) == 0x0F) {
            u32 o = ent[0] & 0x3F;
            if ((ent[0] & 0x40) && o >= 1 && o <= 20) {
                if (!mapa && rev_huerfanos(v, r)) return -1;
                rev_nlfn = 0; rev_espera = o - 1; rev_suma = ent[13];
            } else if (!rev_nlfn || o != rev_espera || ent[13] != rev_suma || !o) {
                if (rev_nlfn < 21) { rev_lfn[rev_nlfn].lba = lba; rev_lfn[rev_nlfn].idx = idx; rev_nlfn++; }
                if (!mapa && rev_huerfanos(v, r)) return -1;
                rev_nlfn = 0; continue;
            } else rev_espera = o - 1;
            if (rev_nlfn < 21) { rev_lfn[rev_nlfn].lba = lba; rev_lfn[rev_nlfn].idx = idx; rev_nlfn++; }
            continue;
        }
        if (rev_nlfn && (rev_espera != 0 || suma_corto(ent) != rev_suma)) { if (!mapa && rev_huerfanos(v, r)) return -1; rev_nlfn = 0; }
        if ((ent[11] & 0x08) || ent[0] == '.') { rev_nlfn = 0; continue; }
        c0 = le16(ent + 26) | (v->tipo == 32 ? le16(ent + 20) << 16 : 0); tam = le32(ent + 28);
        if (ent[11] & 0x10) {
            if (c0 >= 2 && c0 < v->nclusters + 2) {
                if (!mapa) {
                    u32 k; u8 *q;
                    for (k = 0; k < rev_nvis && rev_vis[k] != c0; k++) ;
                    if (k < rev_nvis) {                     /* la misma carpeta con dos entradas (un mover a medias) o un ciclo */
                        if (rev_nlfn < 21) { rev_lfn[rev_nlfn].lba = lba; rev_lfn[rev_nlfn].idx = idx; rev_nlfn++; }
                        if (rev_huerfanos(v, r)) return -1;
                        r->lfn_huerfanos--; r->repetidas++;
                        continue;
                    }
                    if (rev_nvis == REV_VISTAS) return -1;
                    rev_vis[rev_nvis++] = c0;
                    if (!(q = sec_rw(v, sector_de(v, c0), 0))) return -1;   /* su «..» apunta a donde está */
                    if (q[32] == '.' && q[33] == '.') {
                        u32 pc = hondo == 1 ? 0 : rev_pcl[hondo - 1], tiene = le16(q + 32 + 26) | (v->tipo == 32 ? le16(q + 32 + 20) << 16 : 0);
                        if (tiene != pc) { w16(q + 32 + 20, v->tipo == 32 ? pc >> 16 : 0); w16(q + 32 + 26, pc & 0xFFFF); if (sec_esc(v, sector_de(v, c0))) return -1; r->puntos++; }
                    }
                } else marcar_dir(v, mapa, base, bits, c0);
                if (hondo == REV_HONDO) return -1;
                dir_abrir(&pila[hondo], c0); rev_pcl[hondo] = c0; hondo++;
            }
            rev_nlfn = 0;
            continue;
        }
        if (!mapa) {                                        /* tamaño contra cadena */
            u32 need = (u32)(((unsigned long long)tam + bpc - 1) / bpc), c = c0, k, x;
            if (c0 < 2 || c0 >= v->nclusters + 2) {
                if (tam || c0) { w16(ent + 20, 0); w16(ent + 26, 0); w32(ent + 28, 0); if (rev_ent_escribir(v, lba, idx, ent)) return -1; r->tamanos++; }
            } else if (need) {
                for (k = 1; k < need; k++) {
                    if (fat_val(v, c, &x)) return -1;
                    if (x < 2 || x >= v->nclusters + 2) {   /* la cadena se acaba antes: el tamaño baja a lo que hay */
                        if (!es_fin(v, x) && fat_poner(v, c, fin_cadena(v))) return -1;
                        w32(ent + 28, k * bpc); if (rev_ent_escribir(v, lba, idx, ent)) return -1; r->tamanos++;
                        break;
                    }
                    c = x;
                }
                if (k == need) {
                    if (fat_val(v, c, &x)) return -1;
                    if (!es_fin(v, x)) { if (fat_poner(v, c, fin_cadena(v))) return -1; r->cadenas_largas++; }
                }
            }
        } else if (c0 >= 2 && c0 < v->nclusters + 2) {     /* marcar; un clúster ya marcado es un cruce */
            u32 need = (u32)(((unsigned long long)tam + bpc - 1) / bpc), c = c0, prev = 0, k, x;
            for (k = 0; k < need && c >= 2 && c < v->nclusters + 2; k++) {
                if (marcar(mapa, base, bits, c)) {
                    if (k == 0) {                           /* la misma cadena con dos nombres: un renombrado a medias */
                        if (rev_nlfn < 21) { rev_lfn[rev_nlfn].lba = lba; rev_lfn[rev_nlfn].idx = idx; rev_nlfn++; }
                        if (rev_huerfanos(v, r)) return -1;
                        r->lfn_huerfanos -= 1; r->repetidas++;
                    } else {
                        if (fat_poner(v, prev, fin_cadena(v))) return -1;
                        w32(ent + 28, k * bpc); if (rev_ent_escribir(v, lba, idx, ent)) return -1; r->cruzados++;
                    }
                    break;
                }
                if (k + 1 == need) break;
                if (fat_val(v, c, &x)) return -1;
                prev = c; c = x;
            }
        }
        rev_nlfn = 0;
    }
    return 0;
}
int fat_revisar(FatVol *v, u8 *mapa, u32 bytes, FatRevision *r)
{
    FatRevision rr; u32 s, k, base, bits, libres = 0, c, x, vuelta;
    if (!r) r = &rr;
    memset(r, 0, sizeof *r);
    if (!v->montado) return FAT_E_NO_HAY;
    if (!v->rw) return FAT_E_SOLO_LECTURA;
    if (v->roto) return FAT_E_ROTO;
    if (v->abiertos) return FAT_E_OCUPADA;
    if (!mapa || !bytes) return FAT_E_LLENO;
    v->sucio_al_montar = 0;
    olvidar(v);
    if (!v->sucio && poner_sucio(v, 1)) return roto(v);
    for (s = 0; s < v->sfat; s++)                           /* las copias de la FAT, como la 1.ª (R1) */
        for (k = 1; k < v->nfat; k++) {
            if (v->latido && !(s & 31)) v->latido();
            const u8 *p = sec(v, v->s_fat + s);
            if (!p) return FAT_E_ES;
            memcpy(sec_tmp, p, 512);
            if (!(p = sec(v, v->s_fat + k * v->sfat + s))) return FAT_E_ES;
            if (memcmp(p, sec_tmp, 512)) { if (dev_esc(v, v->s_fat + k * v->sfat + s, 1, sec_tmp)) return roto(v); r->fat_copias++; }
        }
    if (rev_vuelta(v, r, 0, 0, 0) || pend_vaciar(v)) { v->sucio_al_montar = 1; return v->roto ? FAT_E_ROTO : FAT_E_ES; }
    bits = bytes * 8;
    for (vuelta = 0; vuelta < 2; vuelta++) {                /* (un cruce arreglado puede dejar más: otra vuelta) */
        u32 antes = r->cruzados + r->repetidas;
        libres = 0;
        for (base = 2; base < v->nclusters + 2; base += bits) {
            u32 hasta = v->nclusters + 2 - base < bits ? v->nclusters + 2 : base + bits;
            memset(mapa, 0, bytes);
            if (rev_vuelta(v, r, mapa, base, bits)) { pend_vaciar(v); v->sucio_al_montar = 1; return v->roto ? FAT_E_ROTO : FAT_E_ES; }   /* sin el árbol entero NO se libera nada */
            for (c = base; c < hasta; c++) {
                u32 b = c - base;
                if (v->latido && !(c & 1023)) v->latido();
                if (fat_val(v, c, &x)) return FAT_E_ES;
                if (x && !es_mala(v, x) && !(mapa[b >> 3] & (1u << (b & 7)))) {
                    if (fat_poner(v, c, 0)) return roto(v);
                    r->perdidos++; x = 0;
                }
                if (!x) libres++;
            }
            if (pend_vaciar(v)) return roto(v);
        }
        if (r->cruzados + r->repetidas == antes) break;
    }
    v->libres = libres; v->libres_ok = 1;
    if (poner_sucio(v, 0)) return roto(v);
    return (int)(r->fat_copias + r->cadenas_largas + r->tamanos + r->repetidas + r->lfn_huerfanos + r->perdidos + r->cruzados + r->puntos);
}
