/* Las 16 voces, la mezcla y el lector de MIDI. Interfaz y procedencia: mezcla.h.
 *
 * Es el código que escribió y midió la pista S4 (`audio/hal/hal_audio.c`, capas 4 y 5), movido aquí
 * SIN cambiar la aritmética: la tabla del seno, la fase 16.16, el panorama con signo, el reparto de
 * notas entre voces y el recorrido del SMF son los mismos. Lo que S5 ha añadido:
 *   - el formato (rate / canales) vive aquí en vez de venir del `audio_fmt` de la HAL;
 *   - `mz_volumen(pct)`: la atenuación de la mezcla, que es el volumen DE USUARIO (la ganancia del DSP
 *     es otra cosa y en el emulador no se puede medir: docs/audio.md §4.3);
 *   - `mz_servir` entrega por `hw_pcm_escribir` (api/hw.h) en vez de por `audio_escribir`, para que la
 *     mezcla no sepa si debajo hay un DSP o un contador del PC.
 *
 * Lo que ha cambiado la pista S7 al USARLO de verdad (la app de Música; hasta entonces nadie lo llamaba):
 *   - el paso de fase se calcula UNA vez por nota (era una división de 64 bits por voz y por muestra);
 *   - una ENVOLVENTE mínima (sube en 5 ms, cae en 30 ms): un seno cortado en seco chasca en cada nota;
 *   - el lector de MIDI lleva el tiempo EXACTO (`mz_midi_avanzar_us`): cada evento en su pulso y el resto
 *     que no llega a un pulso se guarda. La versión de S4 truncaba los pulsos de cada bloque;
 *   - 24 pistas en vez de 8 y 32 voces en vez de 16; el canal 10 (percusión) NO se toca y se cuenta; las
 *     notas que no caben en las voces se cuentan; un modo «mudo» para el camino del zumbador y para medir la duración.
 * La tabla del seno, la fase 16.16, el panorama con signo y el reparto de voces siguen siendo los de S4.
 */
#include "mezcla.h"

void *memset(void *d, int c, __SIZE_TYPE__ n);
int   memcmp(const void *a, const void *b, __SIZE_TYPE__ n);

static MzVoz voces[MZ_VOCES];
static u32 vz_rate = 16000;
static int vz_canales = 1;
static int vz_vol = 100;                 /* volumen de usuario, 0..100 */
static u16 env_sube = 51, env_baja = 9;  /* por muestra, sobre 4096: 5 ms de subida y 30 ms de caída a 16 kHz */

#define ENV_MAX 4096

void mz_formato(u32 rate, int canales)
{
    u32 a, b;
    vz_rate = rate ? rate : 16000;
    vz_canales = canales == 2 ? 2 : 1;
    a = ENV_MAX * 1000u / (vz_rate * 5u);  b = ENV_MAX * 1000u / (vz_rate * 30u);
    env_sube = (u16)(a ? a : 1);  env_baja = (u16)(b ? b + 1 : 1);
}
u32 mz_rate(void) { return vz_rate; }
int mz_canales(void) { return vz_canales; }
void mz_volumen(int pct) { vz_vol = pct < 0 ? 0 : pct > 100 ? 100 : pct; }

/* seno de un cuarto de periodo, 65 puntos, 0..16384 (Q14). Generada con la identidad de
 * Chebyshev en el PC y pegada aquí: no hay coma flotante en el main. */
static const short seno_q[65] = {
        0,   402,   804,  1205,  1606,  2006,  2404,  2801,  3196,  3590,
     3981,  4370,  4756,  5139,  5520,  5897,  6270,  6639,  7005,  7366,
     7723,  8076,  8423,  8765,  9102,  9434,  9760, 10080, 10394, 10702,
    11003, 11297, 11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395,
    13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978, 15137, 15286,
    15426, 15557, 15679, 15791, 15893, 15986, 16069, 16143, 16207, 16261,
    16305, 16340, 16364, 16379, 16384
};

static int seno16(u32 fase)        /* fase 16.16 de un periodo entero; devuelve -16384..16384 */
{
    u32 f = (fase >> 8) & 0xFF;    /* 0..255 = un periodo */
    if (f < 64)  return  seno_q[f];
    if (f < 128) return  seno_q[128 - f];
    if (f < 192) return -seno_q[f - 128];
    return -seno_q[256 - f];
}

int mz_voz_poner(unsigned i, const MzVoz *v)
{
    if (i >= MZ_VOCES || !v) return -1;
    voces[i] = *v;
    voces[i].fase = 0;
    voces[i].pos = 0;
    /* fase 16.16 por muestra = f/rate, con f en milihercios. UNA división por nota, no por muestra. */
    voces[i].paso = (u32)(((unsigned long long)v->f_mhz << 16) / (1000ull * vz_rate));
    voces[i].env = 0;
    voces[i].soltando = 0;
    return 0;
}

void mz_voz_nota(unsigned i, voz_tipo t, u32 f_mhz, u16 vol)
{
    MzVoz v;
    memset(&v, 0, sizeof v);
    v.tipo = t; v.f_mhz = f_mhz; v.vol = vol;      /* pan = 0 = centro */
    mz_voz_poner(i, &v);
}

void mz_voz_en_fase(unsigned i, unsigned con) { if (i < MZ_VOCES && con < MZ_VOCES) voces[i].fase = voces[con].fase; }
void mz_voz_quitar(unsigned i) { if (i < MZ_VOCES) voces[i].tipo = VOZ_NADA; }
void mz_voz_soltar(unsigned i) { if (i < MZ_VOCES && voces[i].tipo != VOZ_NADA) voces[i].soltando = 1; }
int  mz_voz_libre(unsigned i) { return i < MZ_VOCES && voces[i].tipo == VOZ_NADA; }
void mz_todas_fuera(void) { unsigned i; for (i = 0; i < MZ_VOCES; i++) voces[i].tipo = VOZ_NADA; }

unsigned mz_voces_activas(void)
{
    unsigned i, n = 0;
    for (i = 0; i < MZ_VOCES; i++) if (voces[i].tipo != VOZ_NADA) n++;
    return n;
}

/* Una muestra de una voz, -16384..16384 antes del volumen. */
static int voz_muestra(MzVoz *v)
{
    u32 f = v->fase;
    switch (v->tipo) {
    case VOZ_CUADRADA:  return (f & 0x8000u) ? -16384 : 16384;
    case VOZ_SIERRA:    return (int)((f & 0xFFFFu) >> 1) - 16384;
    case VOZ_TRIANGULO: {
        int x = (int)(f & 0xFFFFu);
        return x < 32768 ? (x >> 1) - 16384 : 16384 - ((x - 32768) >> 1);
    }
    case VOZ_SENO:      return seno16(f);
    case VOZ_MUESTRA:   return v->pos < v->n ? v->muestra[v->pos] / 2 : 0;
    default:            return 0;
    }
}

unsigned mz_mezclar(short *dst, unsigned marcos)
{
    unsigned k, i, c = (unsigned)vz_canales;
    for (k = 0; k < marcos; k++) {
        int izq = 0, der = 0;
        for (i = 0; i < MZ_VOCES; i++) {
            MzVoz *v = &voces[i];
            int s;
            if (v->tipo == VOZ_NADA) continue;
            s = voz_muestra(v) * (int)v->vol / 256;
            s = s * (int)v->env / ENV_MAX;
            if (v->soltando) {                         /* la cola: baja hasta 0 y la voz queda libre */
                if (v->env <= env_baja) { v->env = 0; v->tipo = VOZ_NADA; }
                else v->env = (u16)(v->env - env_baja);
            } else if (v->env < ENV_MAX) {
                u32 e = (u32)v->env + env_sube;
                v->env = (u16)(e > ENV_MAX ? ENV_MAX : e);
            }
            if (c == 2) {
                int p = (int)v->pan + 128;             /* 0..255 */
                izq += s * (255 - p) / 255;
                der += s * p / 255;
            }
            else izq += s;
            v->fase += v->paso;
            if (v->tipo == VOZ_MUESTRA) {
                v->pos++;
                if (v->pos >= v->n) { if (v->bucle) v->pos = 0; else v->tipo = VOZ_NADA; }
            }
        }
        izq = izq * vz_vol / 100;                      /* el volumen de usuario, al final de la mezcla */
        der = der * vz_vol / 100;
        if (izq > 32767) izq = 32767;
        if (izq < -32768) izq = -32768;
        if (der > 32767) der = 32767;
        if (der < -32768) der = -32768;
        dst[k * c] = (short)izq;
        if (c == 2) dst[k * c + 1] = (short)der;
    }
    return marcos;
}

int mz_servir(short *buf, unsigned marcos)
{
    if (marcos > MZ_MAX_MARCOS) marcos = MZ_MAX_MARCOS;
    mz_mezclar(buf, marcos);
    return hw_pcm_escribir(buf, marcos);
}

/* ===================== el fichero MIDI ==================================================== */

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 be16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }

static u32 vlq(const u8 *d, u32 *pos, u32 fin)
{
    u32 v = 0, k = 0;
    while (*pos < fin && k < 4) {
        u8 b = d[(*pos)++];
        v = (v << 7) | (b & 0x7F);
        k++;
        if (!(b & 0x80)) break;
    }
    return v;
}

int mz_midi_abrir(MzMidi *m, const u8 *d, u32 n)
{
    u32 pos = 0;
    unsigned i;
    memset(m, 0, sizeof *m);
    for (i = 0; i < MZ_VOCES; i++) { m->voz_de[i] = 0xFF; m->can_de[i] = 0xFF; }
    if (!d || n < 14 || memcmp(d, "MThd", 4) || be32(d + 4) < 6) return -1;
    m->d = d; m->n = n;
    m->division = be16(d + 12);
    if (!m->division || (m->division & 0x8000)) return -2;   /* SMPTE no */
    m->us_por_negra = 500000;
    if (be32(d + 4) > n - 8) return -1;
    pos = 8 + be32(d + 4);
    while (pos + 8 <= n && m->pistas < MZ_PISTAS) {
        u32 len = be32(d + pos + 4);
        if (len > n - pos - 8) break;                        /* un trozo que se sale del fichero: hasta aquí */
        if (!memcmp(d + pos, "MTrk", 4)) {
            m->p[m->pistas].pos = pos + 8;
            m->p[m->pistas].fin = pos + 8 + len;
            m->p[m->pistas].t = 0;
            m->p[m->pistas].viva = 1;
            m->p[m->pistas].ult = 0;
            /* el primer retardo */
            { u32 q = m->p[m->pistas].pos;
              m->p[m->pistas].t = vlq(d, &q, m->p[m->pistas].fin);
              m->p[m->pistas].pos = q; }
            m->pistas++;
        }
        pos += 8 + len;
    }
    return m->pistas ? (int)m->pistas : -3;
}

/* do..si de la octava -1 en milihercios */
static u32 nota_mhz(u8 nota)
{
    static const u32 base[12] = { 8176, 8662, 9177, 9723, 10301, 10913, 11562, 12250, 12978, 13750, 14568, 15434 };
    u32 f = base[nota % 12];
    unsigned oct = nota / 12;
    while (oct--) f *= 2;
    return f;
}
u32 mz_nota_mhz(int nota) { return nota_mhz((u8)(nota < 0 ? 0 : nota > 127 ? 127 : nota)); }

/* El volumen de una nota en la mezcla. Deja sitio: una voz a toda velocidad son ±2 048 de ±32 767, así que
 * dieciséis a la vez y en fase no recortan (el «Greeting.mid» de fábrica llega a 14 notas a la vez). */
static u16 vol_de(u8 vel) { return (u16)(8 + (unsigned)vel * 24u / 127u); }

static void nota_on(MzMidi *m, u8 can, u8 nota, u8 vel)
{
    unsigned i, libre = MZ_VOCES, cola = MZ_VOCES;
    if (can == 9) { m->percusion++; return; }      /* la percusión no se toca: no hay con qué (mezcla.h) */
    for (i = 0; i < MZ_VOCES; i++) {
        if (m->voz_de[i] == nota && m->can_de[i] == can) { libre = i; break; }
        if (m->voz_de[i] != 0xFF) continue;
        if (m->mudo || mz_voz_libre(i)) { if (libre == MZ_VOCES) libre = i; }
        else if (cola == MZ_VOCES) cola = i;       /* libre en la tabla, pero todavía suena su cola */
    }
    if (libre == MZ_VOCES) libre = cola;           /* antes que perder la nota, se le corta la cola a otra */
    if (libre == MZ_VOCES) { m->perdidas++; return; }
    m->voz_de[libre] = nota;
    m->can_de[libre] = can;
    m->vel_de[libre] = vel;
    m->pulso_de[libre] = m->t;
    if (!m->mudo) {
        mz_voz_nota(libre, VOZ_SENO, nota_mhz(nota), vol_de(vel));
        /* Un UNÍSONO (la misma altura ya sonando en otro canal): la voz nueva entra EN FASE con la que ya está.
         * Si no, dos senos iguales atacados con unos ms de diferencia se restan: en el «MusicDJ Demo» de fábrica
         * un mi bemol doblado a 4 ms se quedaba en el 13 % de su amplitud [medido en la mezcla, pista S7]. */
        for (i = 0; i < MZ_VOCES; i++)
            if (i != libre && m->voz_de[i] == nota && !mz_voz_libre(i)) { mz_voz_en_fase(libre, i); break; }
    }
    m->notas++;
}

static void voz_fuera(MzMidi *m, unsigned i)
{
    if (!m->mudo) mz_voz_soltar(i);
    m->off_de[m->voz_de[i] & 0x7F] = m->t;
    m->voz_de[i] = 0xFF; m->can_de[i] = 0xFF;
}

static void nota_off(MzMidi *m, u8 can, u8 nota)
{
    unsigned i;
    for (i = 0; i < MZ_VOCES; i++)
        if (m->voz_de[i] == nota && m->can_de[i] == can) voz_fuera(m, i);
}

/* Un evento de una pista. Devuelve 0 si la pista se ha acabado. */
static int pista_evento(MzMidi *m, unsigned k)
{
    const u8 *d = m->d;
    u32 pos = m->p[k].pos, fin = m->p[k].fin;
    u8 st;
    if (pos >= fin) { m->p[k].viva = 0; return 0; }
    st = d[pos];
    if (st & 0x80) pos++; else st = m->p[k].ult;     /* estado corrido */
    if (st >= 0x80 && st < 0xF0) m->p[k].ult = st;
    if (st == 0xFF) {                                /* meta */
        u8 tipo = pos < fin ? d[pos++] : 0;
        u32 len = vlq(d, &pos, fin);
        if (tipo == 0x51 && len == 3 && pos + 3 <= fin) {
            u32 us = ((u32)d[pos] << 16) | ((u32)d[pos + 1] << 8) | d[pos + 2];
            if (us) m->us_por_negra = us;
        }
        if (tipo == 0x2F) { m->p[k].viva = 0; m->p[k].pos = fin; return 0; }
        if (len > fin - pos) len = fin - pos;
        pos += len;
    } else if (st == 0xF0 || st == 0xF7) {
        u32 len = vlq(d, &pos, fin);
        if (len > fin - pos) len = fin - pos;
        pos += len;
    } else {
        u8 alto = st & 0xF0, can = st & 0x0F;
        u8 a = pos < fin ? d[pos++] : 0, b = 0;
        if (alto != 0xC0 && alto != 0xD0) b = pos < fin ? d[pos++] : 0;
        if (alto == 0x90 && b) nota_on(m, can, a & 0x7F, b & 0x7F);
        else if (alto == 0x80 || (alto == 0x90 && !b)) nota_off(m, can, a & 0x7F);
        else if (alto == 0xB0 && (a == 120 || a == 123)) {
            unsigned i;
            for (i = 0; i < MZ_VOCES; i++)
                if (m->can_de[i] == can && m->voz_de[i] != 0xFF) voz_fuera(m, i);
        }
    }
    if (pos >= fin) { m->p[k].viva = 0; m->p[k].pos = fin; return 0; }
    m->p[k].t += vlq(d, &pos, fin);
    m->p[k].pos = pos;
    return 1;
}

/* El pulso del evento más próximo entre las pistas vivas. 0 = no queda ninguna. */
static int proximo(const MzMidi *m, u32 *pulso)
{
    unsigned k, hay = 0;
    u32 p = 0;
    for (k = 0; k < m->pistas; k++)
        if (m->p[k].viva && (!hay || m->p[k].t < p)) { p = m->p[k].t; hay = 1; }
    *pulso = p;
    return (int)hay;
}

static void eventos_en(MzMidi *m)               /* todos los eventos cuyo pulso ya ha llegado (≤ m->t) */
{
    unsigned k;
    for (k = 0; k < m->pistas; k++)
        while (m->p[k].viva && m->p[k].t <= m->t) pista_evento(m, k);
}

int mz_midi_avanzar_us(MzMidi *m, u32 us)
{
    u32 sig;
    if (!m->pistas || !m->division) return 0;
    m->cred += (unsigned long long)us * m->division;         /* µs × división: se gasta a razón de us_por_negra por pulso */
    for (;;) {
        unsigned long long hace_falta;
        if (!proximo(m, &sig)) return 0;                     /* se acabó */
        hace_falta = sig > m->t ? (unsigned long long)(sig - m->t) * m->us_por_negra : 0;
        if (hace_falta > m->cred) {                          /* no se llega al evento: se avanza lo que dé y se guarda el resto */
            u32 av = (u32)(m->cred / m->us_por_negra);
            m->t += av;
            m->cred -= (unsigned long long)av * m->us_por_negra;
            return 1;
        }
        m->cred -= hace_falta;
        if (sig > m->t) m->t = sig;
        eventos_en(m);                                       /* un cambio de tempo aquí vale ya para el tramo siguiente */
    }
}

int mz_midi_avanzar(MzMidi *m, unsigned marcos, unsigned rate)
{
    u32 us = (u32)((unsigned long long)marcos * 1000000ull / (rate ? rate : 8000));
    int vivo = mz_midi_avanzar_us(m, us);
    return vivo ? 1 : (!m->mudo && mz_voces_activas() ? 1 : 0);
}

int mz_midi_medir(const u8 *datos, u32 n, u32 *dur_ms, u32 *notas, u32 *percusion)
{
    MzMidi m;
    unsigned long long us = 0, pulsos = 0;
    u32 sig;
    int r = mz_midi_abrir(&m, datos, n);
    if (dur_ms) *dur_ms = 0;
    if (notas) *notas = 0;
    if (percusion) *percusion = 0;
    if (r < 0) return r;
    m.mudo = 1;
    while (proximo(&m, &sig)) {
        u32 tempo = m.us_por_negra;
        if (sig > m.t) { pulsos += sig - m.t; m.t = sig; }
        eventos_en(&m);
        if (m.us_por_negra != tempo) { us += pulsos * tempo / m.division; pulsos = 0; }   /* el tramo que acaba iba al tempo de antes */
    }
    us += pulsos * m.us_por_negra / m.division;
    if (dur_ms) *dur_ms = (u32)(us / 1000);
    if (notas) *notas = m.notas;
    if (percusion) *percusion = m.percusion;
    return r;
}

int mz_midi_aguda(const MzMidi *m)
{
    unsigned i;
    int top = -1;
    for (i = 0; i < MZ_VOCES; i++) if (m->voz_de[i] != 0xFF && (int)m->voz_de[i] > top) top = m->voz_de[i];
    return top;
}

int mz_midi_linea(const MzMidi *m, MzLinea *l, u32 dt_ms)
{
    unsigned i;
    int mejor = -1;
    for (i = 0; i < MZ_VOCES; i++) {
        if (m->voz_de[i] == 0xFF) continue;
        if (mejor < 0 || m->voz_de[i] > m->voz_de[mejor] ||
            (m->voz_de[i] == m->voz_de[mejor] && m->pulso_de[i] > m->pulso_de[mejor])) mejor = (int)i;
    }
    if (mejor < 0) { l->nota = -1; l->espera = 0; return -1; }
    if (l->hubo && l->nota == (int)m->voz_de[mejor]) { l->espera = 0; return l->nota; }
    if (l->hubo && l->nota < 0 && m->pulso_de[mejor] < m->off_de[l->ult & 0x7F]) {   /* ya sonaba cuando la línea se quedó sin nota: que espere */
        l->espera += dt_ms;
        if (l->espera < MZ_LINEA_ESPERA_MS) return -1;
    } else if (l->hubo && l->nota >= 0) {                  /* ¿sigue sonando la nota de la línea? */
        unsigned sigue = 0;
        for (i = 0; i < MZ_VOCES; i++) if ((int)m->voz_de[i] == l->nota) sigue = 1;
        if (!sigue && m->pulso_de[mejor] < m->off_de[l->nota & 0x7F]) {   /* se acabó, y la que queda ya sonaba: empieza la espera */
            l->ult = l->nota; l->nota = -1; l->espera = 0;   /* la espera se cuenta desde que se VE (hasta un tic después de acabarse) */
            return -1;
        }
    }
    l->nota = l->ult = m->voz_de[mejor]; l->hubo = 1; l->espera = 0;
    return l->nota;
}

void mz_midi_retomar(MzMidi *m)
{
    unsigned i;
    for (i = 0; i < MZ_VOCES; i++)
        if (m->voz_de[i] != 0xFF) mz_voz_nota(i, VOZ_SENO, nota_mhz(m->voz_de[i]), vol_de(m->vel_de[i]));
}
