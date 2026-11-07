/* EL APF Y EL CÓDEC del reproductor MP3 del W580i (hal/mp3_w580.c). PORTABLE (sin registros: el PMU entra por un puntero a función); se prueba en el PC contra
 * tmp/dsp_emu/apf.py y contra la traza del ORIGINAL (tmp/mp3/ref_hw.py).
 *
 *  1. mp3_inflar: un inflate (RFC 1950/1951) pequeño, a partir del algoritmo de puff.c (Mark Adler, dominio público): los .zapf del FS de fábrica son zlib.
 *  2. El APF («Audio Parameter File», tmp/dsp_emu/protocolo_mp3.md §4): 'APF\0', versión 1.8, longitud BE 13339, y un cuerpo con una tabla fija de 108 parámetros.
 *     Aquí solo lo que el reproductor usa: los 53 campos del códec (cuerpo +10587.., ids 0x32..0x66), las banderas de clase 0 (cuerpo +id) y los bloques
 *     RX 44,1 kHz (+1574, 516 B) y DRC_RX (+10350, 48 B) que van al mezclador del DSP.
 *  3. El códec Vincenne del PMU (C0..D0): una SOMBRA y el leer-modificar-escribir del original (FUN_446f6100), y los escalones de potencia (FUN_445777f8,
 *     FUN_44577990…) con el mismo orden de escrituras que el original ([ejecutado] en el emulador: tmp/mp3/ref_hw.py). */
#include "mp3_w580_i.h"

/* ============================================================== 1. inflate ======================================================================== */
#define MAXBITS 15
#define FIXLCODES 288
typedef struct { const u8 *src; u32 n, pos; u32 bitbuf; int bitcnt; u8 *out; u32 max, o; } Z;
typedef struct { short count[MAXBITS + 1]; short symbol[FIXLCODES]; } Huff;
static int zbits(Z *z, int need)
{
    u32 v = z->bitbuf;
    while (z->bitcnt < need) {
        if (z->pos >= z->n) return -1;
        v |= (u32)z->src[z->pos++] << z->bitcnt; z->bitcnt += 8;
    }
    z->bitbuf = v >> need; z->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}
static int zdecode(Z *z, const Huff *h)
{
    int len, code = 0, first = 0, count, index = 0;
    for (len = 1; len <= MAXBITS; len++) {
        int b = zbits(z, 1);
        if (b < 0) return -1;
        code |= b;
        count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -2;
}
static int zconstruir(Huff *h, const short *length, int n)
{
    int sym, len, left; short offs[MAXBITS + 1];
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++) if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}
static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static int zcodigos(Z *z, const Huff *lc, const Huff *dc)
{
    int sym, len, dist;
    for (;;) {
        sym = zdecode(z, lc);
        if (sym < 0) return -1;
        if (sym < 256) { if (z->o >= z->max) return -2; z->out[z->o++] = (u8)sym; }
        else if (sym == 256) return 0;
        else {
            int e;
            sym -= 257;
            if (sym >= 29) return -3;
            e = zbits(z, lext[sym]); if (e < 0) return -1;
            len = lbase[sym] + e;
            sym = zdecode(z, dc); if (sym < 0 || sym >= 30) return -3;
            e = zbits(z, dext[sym]); if (e < 0) return -1;
            dist = dbase[sym] + e;
            if ((u32)dist > z->o || z->o + (u32)len > z->max) return -4;
            while (len--) { z->out[z->o] = z->out[z->o - (u32)dist]; z->o++; }
        }
    }
}
static int zfijo(Z *z)
{
    static Huff lc, dc; static u8 hecho;
    if (!hecho) {
        short l[FIXLCODES]; int s;
        for (s = 0; s < 144; s++) l[s] = 8;
        for (; s < 256; s++) l[s] = 9;
        for (; s < 280; s++) l[s] = 7;
        for (; s < FIXLCODES; s++) l[s] = 8;
        zconstruir(&lc, l, FIXLCODES);
        for (s = 0; s < 30; s++) l[s] = 5;
        zconstruir(&dc, l, 30);
        hecho = 1;
    }
    return zcodigos(z, &lc, &dc);
}
static int zdinamico(Z *z)
{
    static const short orden[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[320]; Huff lc, dc, cc;
    int nlen, ndist, ncode, index, err, t;
    t = zbits(z, 5); if (t < 0) return -1; nlen = t + 257;
    t = zbits(z, 5); if (t < 0) return -1; ndist = t + 1;
    t = zbits(z, 4); if (t < 0) return -1; ncode = t + 4;
    if (nlen > 286 || ndist > 30) return -3;
    for (index = 0; index < ncode; index++) { t = zbits(z, 3); if (t < 0) return -1; lengths[orden[index]] = (short)t; }
    for (; index < 19; index++) lengths[orden[index]] = 0;
    err = zconstruir(&cc, lengths, 19);
    if (err != 0) return -5;
    index = 0;
    while (index < nlen + ndist) {
        int sym = zdecode(z, &cc), len, rep;
        if (sym < 0) return -1;
        if (sym < 16) lengths[index++] = (short)sym;
        else {
            len = 0;
            if (sym == 16) { if (index == 0) return -6; len = lengths[index - 1]; t = zbits(z, 2); rep = 3 + t; }
            else if (sym == 17) { t = zbits(z, 3); rep = 3 + t; }
            else { t = zbits(z, 7); rep = 11 + t; }
            if (t < 0) return -1;
            if (index + rep > nlen + ndist) return -6;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -7;
    err = zconstruir(&lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return -8;
    err = zconstruir(&dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return -9;
    return zcodigos(z, &lc, &dc);
}
int mp3_inflar(const u8 *s, u32 ns, u8 *out, u32 max)
{
    Z z; int last, tipo, r;
    if (ns < 6 || (s[0] & 0x0F) != 8 || ((s[0] << 8) | s[1]) % 31) return -10;      /* cabecera zlib */
    z.src = s + 2; z.n = ns - 2; z.pos = 0; z.bitbuf = 0; z.bitcnt = 0; z.out = out; z.max = max; z.o = 0;
    do {
        last = zbits(&z, 1); tipo = zbits(&z, 2);
        if (last < 0 || tipo < 0) return -1;
        if (tipo == 0) {
            u32 len, nlen;
            z.bitbuf = 0; z.bitcnt = 0;
            if (z.pos + 4 > z.n) return -1;
            len = z.src[z.pos] | ((u32)z.src[z.pos + 1] << 8); nlen = z.src[z.pos + 2] | ((u32)z.src[z.pos + 3] << 8); z.pos += 4;
            if (len != (~nlen & 0xFFFFu) || z.pos + len > z.n || z.o + len > z.max) return -2;
            while (len--) z.out[z.o++] = z.src[z.pos++];
            r = 0;
        } else if (tipo == 1) r = zfijo(&z);
        else if (tipo == 2) r = zdinamico(&z);
        else return -3;
        if (r) return r;
    } while (!last);
    return (int)z.o;
}

/* ============================================================== 2. el APF ========================================================================= */
static u8 apf_buf[MP3_APF_TAM + 8];
static u8 apf_ok;
static u8 zbuf[6144];
int mp3_apf_poner(const u8 *a, u32 n)
{
    u32 i;
    apf_ok = 0;
    if (n < MP3_APF_TAM || a[0] != 'A' || a[1] != 'P' || a[2] != 'F' || a[3] != 0 || a[4] != 1) return -1;
    if ((((u32)a[6] << 8) | a[7]) != MP3_APF_CUERPO) return -2;
    for (i = 0; i < MP3_APF_TAM; i++) apf_buf[i] = a[i];
    apf_ok = 1;
    return 0;
}
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void copia(char *d, const char *s, u32 max) { u32 i = 0; while (s[i] && i + 1 < max) { d[i] = s[i]; i++; } d[i] = 0; }
int mp3_apf_cargar(const char *nombre, u32 (*leer_fs)(const char *, u8 *, u32), int *origen)
{
    static const char *const base[2] = { "/original/ifs/settings/acoustic/", "/ifs/settings/acoustic/" };
    char ruta[96]; u32 i, n;
    static u8 tmp[MP3_APF_TAM + 8];
    int r;
    apf_ok = 0; if (origen) *origen = MP3_APF_NADA;
    if (leer_fs) {
        for (i = 0; i < 2; i++) {
            u32 k = 0; const char *b = base[i], *s = nombre;
            while (*b && k < 70) ruta[k++] = *b++;
            while (*s && k < 86) ruta[k++] = *s++;
            copia(ruta + k, ".zapf", 7);
            n = leer_fs(ruta, zbuf, sizeof zbuf);
            if (n >= 16 && n < sizeof zbuf) {
                r = mp3_inflar(zbuf, n, tmp, sizeof tmp);
                if (r >= (int)MP3_APF_TAM && !mp3_apf_poner(tmp, (u32)r)) { if (origen) *origen = MP3_APF_FS; return 0; }
            }
        }
    }
    for (i = 0; i < mp3_apf_emb_n; i++) {
        if (!streq(mp3_apf_emb[i].nombre, nombre)) continue;
        r = mp3_inflar(mp3_apf_emb[i].datos, mp3_apf_emb[i].tam, tmp, sizeof tmp);
        if (r >= (int)MP3_APF_TAM && !mp3_apf_poner(tmp, (u32)r)) { if (origen) *origen = MP3_APF_EMBEBIDO; return 0; }
        return -3;
    }
    return -4;
}
const u8 *mp3_apf_cuerpo(void) { return apf_ok ? apf_buf + 8 : (const u8 *)0; }
u32 mp3_apf_u8(unsigned off) { return apf_ok && off < MP3_APF_CUERPO ? apf_buf[8 + off] : 0; }
u32 mp3_apf_flag(unsigned id) { return mp3_apf_u8(id); }

/* los 53 campos del códec: id 0x32..0x66 -> {registro, máscara, modo}. 'W' = leer-modificar-escribir inmediato; 'S' = solo sombra (el INSEL se aplica al
 * subir el escalón); 'F' = bandera; '-' = el original no lo escribe (HIFI_ALOOP). La tabla sale de ac_vincenne2 (4457.c) y se comprobó 53/53 contra el original. */
typedef struct { u8 reg, mascara; char modo; } Camp;
static const Camp camp[53] = {
    {0xC1,0x80,'W'},{0xC1,0x40,'W'},{0xC1,0x20,'W'},{0xC1,0x08,'W'},{0xC1,0x04,'W'},{0xC1,0x02,'-'},                         /* 0x32..0x37 */
    {0xC2,0x70,'W'},{0xC2,0x07,'W'},{0xC3,0xF0,'W'},{0xC3,0x0F,'W'},{0xC4,0xF0,'W'},{0xC4,0x0F,'W'},                         /* 0x38..0x3D */
    {0xC5,0xC0,'W'},{0xC5,0x38,'W'},{0xC5,0x07,'W'},{0xC6,0x80,'W'},{0xC6,0x40,'W'},{0xC6,0x38,'W'},{0xC6,0x07,'W'},       /* 0x3E..0x44 */
    {0xC8,0x08,'W'},{0xC8,0x06,'W'},{0xC7,0x01,'F'},                                                                        /* 0x45..0x47 (ST_ON) */
    {0xC9,0x20,'W'},{0xC9,0x10,'W'},{0xC9,0x04,'W'},{0xC9,0x02,'W'},{0xC9,0x01,'W'},{0xCA,0xFF,'W'},                         /* 0x48..0x4D */
    {0xCB,0xC0,'W'},{0xCB,0x30,'W'},{0xCB,0x0C,'S'},{0xCB,0x03,'S'},                                                         /* 0x4E..0x51 */
    {0xCC,0xC0,'W'},{0xCC,0x30,'W'},{0xCC,0x0C,'S'},{0xCC,0x03,'S'},                                                         /* 0x52..0x55 */
    {0xCD,0xC0,'W'},{0xCD,0x30,'W'},{0xCD,0x0C,'S'},{0xCD,0x03,'S'},                                                         /* 0x56..0x59 */
    {0xCE,0x20,'W'},{0xCE,0x10,'W'},{0xCE,0x06,'W'},{0xCF,0x20,'W'},{0xCF,0x10,'W'},{0xCF,0x06,'W'},                         /* 0x5A..0x5F */
    {0xD0,0x80,'W'},{0xD0,0x40,'W'},{0xD0,0x20,'W'},{0xD0,0x10,'W'},{0xD0,0x0C,'W'},{0xD0,0x03,'W'},                         /* 0x60..0x65 */
    {0xC0,0x00,'F'}                                                                                                          /* 0x66 POWER_ON */
};
#define CAMPOS_OFF 10587u
static unsigned corrim(u8 m) { unsigned s = 0; while (m && !(m & 1)) { m >>= 1; s++; } return s; }
static u8 campo_pos(unsigned k) { return (u8)((mp3_apf_u8(CAMPOS_OFF + k) << corrim(camp[k].mascara)) & 0xFF); }   /* el valor en su posición */
int mp3_apf_codec(Mp3CodEsc *dst, int max)
{
    int k, n = 0;
    if (!apf_ok) return -1;
    for (k = 0; k < 53 && n < max; k++) {
        if (camp[k].modo == '-') continue;
        dst[n].reg = camp[k].reg; dst[n].mascara = camp[k].mascara; dst[n].valor = campo_pos((unsigned)k); dst[n].modo = (u8)camp[k].modo; n++;
    }
    return n;
}

/* ---- los mensajes del mezclador con trozos del APF (comprobados palabra por palabra contra el original con farfield_closed_play: tmp/mp3/gen_datos.py) ---- */
#include "mp3_w580_dat.h"
static u16 apf_w(unsigned off_bytes) { return (u16)((mp3_apf_u8(off_bytes) << 8) | mp3_apf_u8(off_bytes + 1)); }
int mp3_apf_filtro(u16 *dst, u32 max)
{
    u32 i, n = MP3_FILTRO_N;
    if (!apf_ok || max < n) return -1;
    for (i = 0; i < n; i++) dst[i] = 0;
    for (i = 0; i < MP3_FILTRO_PRE_N; i++) dst[i] = mp3_filtro_pre[i];
    dst[11] = 0x0100;
    dst[12] = apf_w(1574); dst[13] = apf_w(1576);                      /* las dos primeras palabras del bloque (0x0100, 0): su cabecera */
    dst[14] = 0x0052; dst[15] = 0x0050;                                /* dos palabras fijas en todas las muestras del original; su origen: [sin saber] */
    for (i = 2; i < 257; i++) dst[14 + i] = apf_w(1574 + 2 * i);       /* el resto del bloque: 255 tomas */
    return (int)n;
}
int mp3_apf_drc6(u16 *dst, u32 max)
{
    u32 i, n = MP3_DRC6_N;
    if (!apf_ok || max < n) return -1;
    for (i = 0; i < n; i++) dst[i] = 0;
    for (i = 0; i < MP3_DRC6_PRE_N; i++) dst[i] = mp3_drc6_pre[i];
    for (i = 0; i < 23; i++) dst[MP3_DRC6_PRE_N + i] = apf_w(10350 + 2 * i);   /* DRC_RX_Data (24 palabras): en este mensaje la última no va */
    return (int)n;
}
int mp3_apf_drc9(u16 *dst, u32 max)
{
    u32 i, n = MP3_DRC9_N;
    if (!apf_ok || max < n) return -1;
    for (i = 0; i < n; i++) dst[i] = 0;
    for (i = 0; i < MP3_DRC9_PRE_N; i++) dst[i] = mp3_drc9_pre[i];
    for (i = 0; i < 24 && MP3_DRC9_PRE_N + i < n; i++) dst[MP3_DRC9_PRE_N + i] = apf_w(10350 + 2 * i);
    return (int)n;
}

/* ============================================================== 3. el códec Vincenne (PMU C0..D0) ================================================== */
static u8 sh[0x11];                                                  /* sombra C0..D0 */
static Mp3CodHw cod_hw;
void mp3_cod_iniciar(const Mp3CodHw *hw)
{
    unsigned i;
    cod_hw = *hw;
    for (i = 0; i < sizeof sh; i++) sh[i] = 0;
    /* la lista de inicio del códec del original (FUN_445773ac, tabla 0x20041A6E; [ejecutado] al arrancar el teléfono): se hace a la sombra SIN escribir */
    sh[0xC7 - 0xC0] |= 0x40;
    sh[0xCA - 0xC0] = 0x10;
    sh[0xCC - 0xC0] = (u8)((sh[0xCC - 0xC0] & ~0xC0) | 0x40);
    sh[0xCC - 0xC0] = (u8)((sh[0xCC - 0xC0] & ~0x30) | 0x10);
    sh[0xC5 - 0xC0] = (u8)((sh[0xC5 - 0xC0] & ~0x38) | 0x18);
}
u8 mp3_cod_sombra(u8 reg) { return reg >= 0xC0 && reg <= 0xD0 ? sh[reg - 0xC0] : 0; }
void mp3_cod_rmw(u8 reg, u8 mascara, u8 valor)
{
    if (reg < 0xC0 || reg > 0xD0) return;
    sh[reg - 0xC0] = (u8)((sh[reg - 0xC0] & ~mascara) | (valor & mascara));
    if (cod_hw.pmu) cod_hw.pmu(reg, sh[reg - 0xC0]);
}
void mp3_cod_aplicar_apf(void)
{
    int k;
    for (k = 0; k < 53; k++) {
        if (camp[k].modo != 'W') continue;
        mp3_cod_rmw(camp[k].reg, camp[k].mascara, campo_pos((unsigned)k));
    }
}
/* campo k (índice 0..52 = id - 0x32) */
static u32 cv(unsigned id) { return mp3_apf_u8(CAMPOS_OFF + id - 0x32u); }
void mp3_cod_escalon(int encendido, int rx)
{
    /* FUN_445777f8(ruta, rx, encendido): encendido = param_3 || POWER_ON; rx = param_2 || POWER_ON */
    u8 mute = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
    int bvar4 = 0;
    int tx1 = cv(0x3F) != 0 || cv(0x40) != 0, tx2 = cv(0x43) != 0 || cv(0x44) != 0;
    int rxlo = ((cv(0x4E) | cv(0x4F) | cv(0x52) | cv(0x53) | cv(0x56) | cv(0x57)) & 1) != 0;
    if (encendido) {
        if (tx1) b2 = 4;
        if (tx2) b3 = 8;
        mp3_cod_rmw(0xC8, 0x01, (u8)(cv(0x47) & 1));
        mute = 0x40;
    }
    mp3_cod_rmw(0xC0, 0x40, mute);
    if (rx && rxlo) { b4 = 0x10; bvar4 = 1; }
    if (!bvar4) { mp3_cod_rmw(0xCD, 0x0F, 0); mp3_cod_rmw(0xCC, 0x0F, 0); mp3_cod_rmw(0xCB, 0x0F, 0); }
    mp3_cod_rmw(0xC7, 0x40, 0x40);
    mp3_cod_rmw(0xC0, 0x04, b2); mp3_cod_rmw(0xC0, 0x08, b3); mp3_cod_rmw(0xC0, 0x10, b4); mp3_cod_rmw(0xC0, 0x20, b5);
    if (bvar4) {                                                     /* los INSEL, que el APF dejó en sombra: CB/CC/CD [3:0] = campo(0x0C) << 2 | campo(0x03) */
        mp3_cod_rmw(0xCD, 0x0F, (u8)((cv(0x58) << 2) | cv(0x59)));
        mp3_cod_rmw(0xCC, 0x0F, (u8)((cv(0x54) << 2) | cv(0x55)));
        mp3_cod_rmw(0xCB, 0x0F, (u8)((cv(0x50) << 2) | cv(0x51)));
    }
}
void mp3_cod_subir(void) { mp3_cod_rmw(0xC0, 0x01, 0x01); mp3_cod_rmw(0xC0, 0x02, 0x02); mp3_cod_escalon(1, 1); }   /* vincenne(0x66, 1) */
void mp3_cod_bajar_rx(void) { mp3_cod_escalon(0, 1); }
void mp3_cod_apagar(void)
{
    mp3_cod_escalon(0, 0);
    mp3_cod_rmw(0xC0, 0x02, 0); mp3_cod_rmw(0xC0, 0x40, 0); mp3_cod_rmw(0xC0, 0x01, 0);
}
int mp3_cod_final(u8 *r16)
{
    int i;
    for (i = 0; i < 16; i++) r16[i] = sh[1 + i];
    return 16;
}
