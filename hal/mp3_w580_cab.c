/* EL LECTOR DE LA CABECERA DE UN MP3 (reproductor MP3 del W580i, hal/mp3_w580.c). PORTABLE: lee por una función, no toca registros; se prueba en el PC
 * (tmp/mp3/pc/). Qué hace: salta una etiqueta ID3v2, busca el primer fotograma MPEG capa III VÁLIDO (con otro fotograma coherente detrás), saca
 * versión, frecuencia, canales y tasa, lee Xing/Info/VBRI para los fotogramas y los bytes (duración y búsqueda por TOC) y, sin ellos, supone tasa constante.
 * Con eso el ARM calcula lo que el protocolo del DSP pide (código de frecuencia y canales del 0x0E00) y el tiempo (el DSP no manda la posición: tmp/dsp_emu/protocolo_mp3.md §6.1). */
#include "mp3_w580_i.h"

static const u16 br_v1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
static const u16 br_v2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };

typedef struct { u8 ver, prot, pad, canales; u16 kbps; u32 fs, largo, muestras; } Fot;

/* decodifica 4 bytes de cabecera; 0 = no es un fotograma capa III válido */
static int fot_de(const u8 *p, Fot *f)
{
    u32 h = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    unsigned ver, lay, bi, si;
    static const u16 fs1[3] = { 44100, 48000, 32000 };
    if ((h & 0xFFE00000u) != 0xFFE00000u) return 0;
    ver = (h >> 19) & 3; lay = (h >> 17) & 3; bi = (h >> 12) & 15; si = (h >> 10) & 3;
    if (ver == 1 || lay != 1 || bi == 0 || bi == 15 || si == 3) return 0;
    f->ver = (u8)(ver == 3 ? 1 : ver == 2 ? 2 : 25);
    f->prot = (u8)((h >> 16) & 1);                     /* 1 = SIN CRC */
    f->pad = (u8)((h >> 9) & 1);
    f->canales = (u8)(((h >> 6) & 3) == 3 ? 1 : 2);
    f->kbps = ver == 3 ? br_v1[bi] : br_v2[bi];
    f->fs = fs1[si] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    f->muestras = ver == 3 ? 1152u : 576u;
    f->largo = (ver == 3 ? 144000u : 72000u) * f->kbps / f->fs + f->pad;
    return 1;
}
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

#define VENT 4096u
/* busca el primer fotograma válido desde `desde`: sync + otro fotograma coherente a `largo` bytes (o fin de fichero). Devuelve el desplazamiento o 0xFFFFFFFF */
static u32 buscar_fotograma(Mp3Leer rd, void *ctx, u32 desde, u32 tam, u32 tope, Fot *f)
{
    static u8 b[VENT + 8];
    u32 pos = desde, fin = desde + tope;
    while (pos < tam && pos < fin) {
        int n = rd(ctx, pos, b, VENT + 4), i;
        if (n < 4) return 0xFFFFFFFFu;
        for (i = 0; i + 4 <= n && (u32)i < VENT; i++) {
            Fot g;
            u8 s[4];
            if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0 || !fot_de(b + i, &g)) continue;
            if (pos + (u32)i + g.largo + 4 > tam) { *f = g; return pos + (u32)i; }          /* el último del fichero: se acepta */
            if (rd(ctx, pos + (u32)i + g.largo, s, 4) == 4) {
                Fot h;
                if (fot_de(s, &h) && h.fs == g.fs && h.ver == g.ver) { *f = g; return pos + (u32)i; }
            }
        }
        if (n < (int)VENT + 4) break;
        pos += VENT;
    }
    return 0xFFFFFFFFu;
}

int mp3_cab_leer(Mp3Leer rd, void *ctx, u32 tam, Mp3Cab *c)
{
    u8 b[160];
    Fot f;
    u32 ini = 0, pos, i, fl;
    if (!rd || !c || tam < 8) return -1;
    for (i = 0; i < sizeof *c; i++) ((u8 *)c)[i] = 0;
    c->tam = tam;
    if (rd(ctx, 0, b, 10) != 10) return -2;
    if (b[0] == 'I' && b[1] == 'D' && b[2] == '3' && b[6] < 0x80 && b[7] < 0x80 && b[8] < 0x80 && b[9] < 0x80) {
        ini = 10u + (((u32)b[6] << 21) | ((u32)b[7] << 14) | ((u32)b[8] << 7) | b[9]);
        if (b[5] & 0x10) ini += 10;                                  /* pie */
        c->id3v2 = 1;
    }
    pos = buscar_fotograma(rd, ctx, ini, tam, 65536u, &f);
    if (pos == 0xFFFFFFFFu) return -3;
    c->inicio = pos; c->fs = f.fs; c->canales = f.canales; c->version = f.ver; c->kbps = f.kbps;
    /* Xing / Info / VBRI dentro del primer fotograma */
    if (rd(ctx, pos, b, sizeof b) >= 60) {
        u32 si = f.ver == 1 ? (f.canales == 1 ? 17u : 32u) : (f.canales == 1 ? 9u : 17u), x = 4 + (f.prot ? 0u : 2u) + si;
        if (x + 16 < sizeof b && ((b[x] == 'X' && b[x + 1] == 'i' && b[x + 2] == 'n' && b[x + 3] == 'g') || (b[x] == 'I' && b[x + 1] == 'n' && b[x + 2] == 'f' && b[x + 3] == 'o'))) {
            fl = be32(b + x + 4); x += 8;
            if (fl & 1) { c->fotogramas = be32(b + x); x += 4; }
            if (fl & 2) { c->bytes_audio = be32(b + x); x += 4; }
            if ((fl & 4) && x + 100 <= sizeof b) { for (i = 0; i < 100; i++) c->toc[i] = b[x + i]; c->hay_toc = 1; }
            c->vbr = (u8)(c->fotogramas != 0);
        } else if (b[36] == 'V' && b[37] == 'B' && b[38] == 'R' && b[39] == 'I') {
            c->bytes_audio = be32(b + 42); c->fotogramas = be32(b + 46); c->vbr = (u8)(c->fotogramas != 0);
        }
    }
    if (c->fotogramas) {
        /* fotogramas * muestras * 1000 / fs sin 64 bits: (fotogramas * (muestras/ 9)) ... se hace en segundos y resto */
        u32 por = f.muestras, seg = (c->fotogramas / c->fs) * por, resto = (c->fotogramas % c->fs) * por;     /* fotogramas*por/fs = seg + resto/fs */
        c->dur_ms = seg * 1000u + (resto / c->fs) * 1000u + ((resto % c->fs) * 1000u) / c->fs;
        if (c->bytes_audio && c->dur_ms) c->kbps = (u32)(((c->bytes_audio / 125u) * 1000u) / c->dur_ms);        /* tasa media */
    } else {
        u32 util = tam > pos ? tam - pos : 0;
        c->dur_ms = c->kbps ? (util / (c->kbps * 125u)) * 1000u + ((util % (c->kbps * 125u)) * 8u) / c->kbps : 0;   /* bytes*8/kbps = ms */
    }
    if (!c->bytes_audio) c->bytes_audio = tam > pos ? tam - pos : 0;
    return 0;
}

u32 mp3_cab_pos_bytes(const Mp3Cab *c, u32 ms)
{
    u32 p;
    if (!c->dur_ms || ms >= c->dur_ms) return c->tam;
    if (c->hay_toc && c->bytes_audio) {
        u32 a100 = (ms * 100u) / c->dur_ms, a = a100 > 99 ? 99 : a100;                      /* entrada del TOC */
        u32 fa = c->toc[a], fb = a < 99 ? c->toc[a + 1] : 256u;
        u32 fr = ((ms * 100u) % c->dur_ms) * 100u / c->dur_ms;                                /* 0..99 dentro de la entrada */
        u32 t = fa * 100u + (fb - fa) * fr;                                                   /* 0..25600 */
        p = c->inicio + (c->bytes_audio / 1024u) * t / 25u;
    } else {
        p = c->inicio + ms * (c->kbps / 8u) + ((ms * (c->kbps % 8u)) / 8u);                  /* ms * kbps / 8 bytes */
    }
    return p < c->tam ? p : c->tam;
}

u32 mp3_cab_alinear(Mp3Leer rd, void *ctx, const Mp3Cab *c, u32 pos)
{
    Fot f;
    u32 q;
    if (pos <= c->inicio) return c->inicio;
    if (pos >= c->tam) return c->tam;
    q = buscar_fotograma(rd, ctx, pos, c->tam, 8192u, &f);
    return q == 0xFFFFFFFFu ? pos : q;
}

u32 mp3_cab_codigo_fs(u32 fs)
{
    switch (fs) {
    case 8000: return 1;  case 11025: return 2; case 12000: return 3;  case 16000: return 4;  case 22050: return 5;
    case 24000: return 6; case 32000: return 7; case 44100: return 8;  case 48000: return 9;
    }
    return 0;
}
