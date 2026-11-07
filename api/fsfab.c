/* Lector (SOLO LECTURA) del FS de fábrica del W580i, «SEMC FS». Escrito a partir de docs/fs.md
 * (pista C) y con el del W600 (w600/sistema/hal/fs.c) como plantilla. Capas:
 *
 *   región NOR 0x45E00000.. (bloques físicos de 1024, uno detrás de otro; sin NAND)
 *    -> bloques de cabecera (magia DF DA 19 00 D7 98 D3 0A): tabla físico -> lógico, 250 físicos por cabecera
 *    -> dispositivo lógico de bloques de 1024  ->  lógico 2 = tabla de montaje (entradas de 48 bytes)
 *    -> volúmenes: MBR + FAT12/16, 512 B/sector, 2 sectores/cluster = 1 bloque lógico, nombres largos VFAT.
 *
 * Qué está [observado] y qué no (fs.md §1.3): el formato entero, contra UNA imagen de fábrica
 * (W580_R8BE001_FS_RUSSIA; herr/pruebas/prueba_fsfab.c coteja los 483 ficheros con su sha256). De una flash
 * que el firmware original haya USADO no hay ninguna muestra: los estados de bloque que el original sabe
 * manejar y que no salen en la imagen de fábrica (UPDATED, UPDATING, DELETED, «commit», DIRTY, BAD) tienen
 * una codificación [sin saber]. Este lector solo cree las entradas `0x40000000 | lógico`; las demás que no
 * sean «limpio» o «cabecera» las CUENTA (estado.desconocidos) y no las usa. Si un lógico sale en dos cabeceras,
 * gana la de número de secuencia mayor [inferido, como en el W600]: en la imagen de fábrica no pasa.
 * Las cabeceras no tienen sitio fijo (fs.md §1.3): se barre el segmento entero buscando la magia.
 *
 * Todo lo que se lee de la flash se trata como datos sin confianza: cada índice se acota y cada cadena de
 * clusters tiene tope, para que una flash dañada dé «no existe», no un cuelgue ni una lectura fuera de sitio. */
#include "fsfab.h"

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);

#define SIN      0xFFFFu
#define MAX_CAB  192u                   /* 132 vivas como mucho en 32 MiB; el resto, copias viejas */
#define MAX_LOG  32768u                 /* el dispositivo lógico no puede ser mayor que el segmento */

static const u8 *reg;                   /* la región cruda */
static u16 mapa[MAX_LOG];               /* lógico -> físico (SIN = no existe) */
static FfEstado est;
static const u8 cero[FF_BLK];

static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static const u8 *fisico(u32 i) { return reg + i * FF_BLK; }
static int es_cabecera(const u8 *p) { return le32(p) == 0x0019DADFu && le32(p + 4) == 0x0AD398D7u; }

static const u8 *logico(u32 n)          /* un lógico sin bloque se lee como ceros */
{
    est.lecturas++;
    if (n >= MAX_LOG || mapa[n] == SIN) return cero;
    return fisico(mapa[n]);
}

const FfEstado *ff_estado(void) { return &est; }
int ff_listo(void) { return est.montado; }

int ff_montaje(unsigned i, char nombre[28], u8 *tipo, u32 *inicio, u32 *bloques)
{
    const u8 *e;
    if (!est.validos || i >= FF_BLK / 48) return 0;
    e = logico(2) + 48 * i;
    if (e[0] == 0 || e[0] == 0xFF) return 0;
    memcpy(nombre, e, 28); nombre[27] = 0;
    *tipo = e[29]; *inicio = le32(e + 36); *bloques = le32(e + 40);
    return 1;
}

/* ---- FAT12/16: el lector genérico (api/fat.c) sobre el dispositivo lógico ---------------------------------------
 * (pista T) Antes había aquí un lector FAT12/16 propio, atado a punteros a la flash. Se generalizó en api/fat.c
 * (que además sabe FAT32 y lee de cualquier dispositivo de sectores: la tarjeta) y esto es lo que queda: el
 * dispositivo. Un volumen = «bloques» lógicos de 1024 desde «inicio»; un sector = medio bloque; lo que cae fuera
 * o en un lógico sin bloque se lee como ceros. */
#include "fat.h"
typedef struct { u32 inicio, bloques; } Disp;
static Disp disp;
static FatVol vol;                      /* el volumen abierto (uno a la vez: su caché sirve mientras no se cambie) */
static u8 vol_hay;

static int leer_logicos(void *ctx, u32 lba, u32 n, u8 *buf)
{
    const Disp *d = ctx;
    for (; n; n--, lba++, buf += 512) {
        if (lba / 2 >= d->bloques) memset(buf, 0, 512);
        else memcpy(buf, logico(d->inicio + lba / 2) + (lba & 1) * 512, 512);
    }
    return 0;
}
static FatVol *vol_abrir(u32 inicio, u32 bloques)
{
    if (!bloques || inicio >= MAX_LOG || bloques > MAX_LOG - inicio) return 0;
    if (vol_hay && disp.inicio == inicio && disp.bloques == bloques) return &vol;
    vol_hay = 0; disp.inicio = inicio; disp.bloques = bloques;
    if (fat_montar(&vol, leer_logicos, &disp, bloques * 2)) return 0;
    vol_hay = 1;
    return &vol;
}
static u32 largo_de(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

/* Abre una ruta absoluta: elige el volumen FAT cuyo punto de montaje es el prefijo más largo y devuelve en *resto la
 * ruta DENTRO de ese volumen. Los puntos de montaje («/ifs», «/system») existen como directorios vacíos en el volumen «/». */
static FatVol *abrir(const char *ruta, const char **resto)
{
    char nom[28]; u8 tipo; u32 ini, blq, mejor = 0, m_ini = 0, m_blq = 0; unsigned i; int hay = 0;
    FatVol *v;
    if (!est.montado || ruta[0] != '/') return 0;
    for (i = 0; ff_montaje(i, nom, &tipo, &ini, &blq); i++) {
        u32 n = largo_de(nom), k;
        if (tipo != 1 && tipo != 2) continue;               /* no es FAT (p. ej. /tpa/pdh, tipo 8) */
        if (n == 1) n = 0;                                  /* «/» es prefijo de todo */
        for (k = 0; k < n && ruta[k] == nom[k]; k++) ;
        if (k < n || (ruta[n] != '/' && ruta[n] != 0)) continue;
        if (!hay || n > mejor) { hay = 1; mejor = n; m_ini = ini; m_blq = blq; }
    }
    if (!hay || !(v = vol_abrir(m_ini, m_blq))) return 0;
    *resto = ruta[mejor] ? ruta + mejor : "/";
    return v;
}

/* ---- la API ------------------------------------------------------------------------------------------------ */

int ff_listar(const char *dir, u32 desde, HwEntrada *e, u32 max)
{
    const char *r; FatVol *v = abrir(dir, &r);
    return v ? fat_listar(v, r, desde, e, max) : HW_ERROR;
}
u32 ff_tam(const char *ruta)
{
    const char *r; FatVol *v = abrir(ruta, &r);
    return v ? fat_tam(v, r) : 0;
}
/* Los bytes [desde, desde+max) del fichero (pista S6: para copiarlo por trozos sin tenerlo entero en RAM). */
u32 ff_leer_desde(const char *ruta, u32 desde, u8 *buf, u32 max)
{
    const char *r; FatVol *v = abrir(ruta, &r);
    return v ? fat_leer_desde(v, r, desde, buf, max) : 0;
}
u32 ff_leer(const char *ruta, u8 *buf, u32 max) { return ff_leer_desde(ruta, 0, buf, max); }

/* ---- montaje: barrer el segmento buscando cabeceras y aplicar sus tablas por orden de secuencia -------------- */

int ff_montar(const u8 *region, u32 bytes)
{
    static struct { u16 fis; u32 seq; } cab[MAX_CAB];
    u32 n = 0, i, j, nfis = bytes / FF_BLK;
    char nom[28]; u8 tipo; u32 ini, blq;

    memset(&est, 0, sizeof est);
    vol_hay = 0;
    memset(mapa, 0xFF, sizeof mapa);
    reg = region;
    if (!region) return 0;
    if (nfis > FF_MAX_FIS) nfis = FF_MAX_FIS;
    est.fisicos = nfis;
    for (i = 0; i < nfis; i++) {
        const u8 *p = fisico(i);
        if (es_cabecera(p)) { if (n < MAX_CAB) { cab[n].fis = (u16)i; cab[n++].seq = le32(p + 16); } }
        else if (le32(p) == 0x0018DADFu && le32(p + 4) == 0x0AD398D7u) est.marca_fin = 1;
    }
    if (!est.marca_fin && bytes >= nfis * FF_BLK + 8 && nfis < FF_MAX_FIS &&
        le32(region + nfis * FF_BLK) == 0x0018DADFu) est.marca_fin = 1;      /* la marca justo tras el último bloque entero */
    for (i = 1; i < n; i++)                                 /* inserción por secuencia */
        for (j = i; j > 0 && cab[j - 1].seq > cab[j].seq; j--) {
            u16 f = cab[j].fis; u32 s = cab[j].seq;
            cab[j] = cab[j - 1]; cab[j - 1].fis = f; cab[j - 1].seq = s;
        }
    for (i = 0; i < n; i++) {
        const u8 *p = fisico(cab[i].fis);
        u32 suma = FF_BLK - 12, indice = le32(p + 12);
        for (j = 12; j < FF_BLK; j += 4) suma += le32(p + j);
        if (suma != le32(p + 8)) { est.cabeceras_malas++; continue; }
        est.cabeceras++;
        for (j = 0; j < FF_GRUPO; j++) {
            u32 x = le32(p + 24 + 4 * j), log = x & 0xFFFFFF, f;
            if (x == 0x01FFFFFFu || x == 0x03FFFFFFu) continue;              /* limpio / aquí hay una cabecera */
            if (x >> 24 != 0x40) { est.desconocidos++; continue; }
            if (indice >= FF_MAX_FIS / FF_GRUPO + 1 || (f = indice * FF_GRUPO + j) >= nfis || log >= MAX_LOG) { est.fuera++; continue; }
            if (mapa[log] == SIN) est.validos++; else est.duplicados++;
            mapa[log] = (u16)f;
        }
    }
    if (!est.validos) return 0;
    for (i = 0; ff_montaje(i, nom, &tipo, &ini, &blq); i++) {
        est.montajes++;
        if ((tipo == 1 || tipo == 2) && vol_abrir(ini, blq)) est.volumenes++;
    }
    est.montado = est.volumenes > 0;
    return est.montado ? (int)est.validos : 0;
}
