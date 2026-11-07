/* Modelo estricto de NOR con cortes de alimentación (ver pc_nor.h). Solo PC. */
#include <stdlib.h>
#include <string.h>
#include "pc_nor.h"
#include "../hal/m18.h"

PcNor pc_nor = { .corte_en = -1, .modo_corte = -1, .semilla = 1, .fallo_en = -1 };
static AlmPuerto puerto;

u32 pc_nor_azar(void)
{
    u32 x = pc_nor.semilla;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return pc_nor.semilla = x;
}
void pc_nor_encender(void) { pc_nor.cortado = 0; pc_nor.corte_en = -1; pc_nor.fallo_en = -1; }

static const u8 *leer(u32 b) { return pc_nor.mem + (size_t)b * pc_nor.tam; }

static int op_cortada(void)            /* 1 = esta operación es la del corte; 2 = falla ella sola (la corriente sigue) */
{
    long o = pc_nor.ops++;
    if (o == pc_nor.corte_en) return 1;
    return pc_nor.fallo_en >= 0 && o >= pc_nor.fallo_en && o < pc_nor.fallo_en + pc_nor.fallo_n ? 2 : 0;
}
static int borrar(u32 b)
{
    u8 *p; u32 i, t = pc_nor.tam; int oc;
    if (pc_nor.cortado) return -1;
    if (b >= pc_nor.bloques) { pc_nor.violaciones++; return -1; }
    p = pc_nor.mem + (size_t)b * t;
    if ((oc = op_cortada())) {
        int modo = pc_nor.modo_corte >= 0 ? pc_nor.modo_corte : (int)(pc_nor_azar() % 7);
        u32 corte = pc_nor_azar() % (t + 1);
        pc_nor.cortado = oc == 1;
        switch (modo) {
        case 0: break;                                                         /* no llegó a empezar */
        case 1: for (i = 0; i < t; i++) p[i] |= (u8)pc_nor_azar(); break;      /* bits a medio subir */
        case 2: memset(p, 0xFF, corte); break;                                 /* un trozo borrado y el resto no */
        case 3: for (i = 0; i < t; i++) p[i] &= (u8)pc_nor_azar(); break;      /* fase de preprograma: bits a medio bajar */
        case 4: memset(p, 0x00, corte); break;                                 /* preprograma a medias */
        case 5: for (i = 0; i < t; i++) p[i] = (u8)pc_nor_azar(); break;       /* basura */
        default: memset(p, 0xFF, t); memset(pc_nor.prog + (size_t)b * t / 2, 0, t / 2); break;   /* acabó justo */
        }
        if (modo != 0 && modo != 6) memset(pc_nor.prog + (size_t)b * t / 2, 1, t / 2);   /* lo que quede NO es terreno borrado */
        return -1;
    }
    memset(p, 0xFF, t);
    memset(pc_nor.prog + (size_t)b * t / 2, 0, t / 2);
    pc_nor.n_borr++;
    if (b < 64) pc_nor.borrados_por_bloque[b]++;
    return 0;
}
static int programar(u32 b, u32 desplaz, const void *datos, u32 n)
{
    const u8 *d = datos; u32 hecho = 0, i, t = pc_nor.tam; int oc;
    if (pc_nor.cortado) return -1;
    if (b >= pc_nor.bloques || (desplaz & 1) || desplaz >= t || n > t - desplaz) { pc_nor.violaciones++; return -1; }
    while (hecho < n) {
        u32 dir = desplaz + hecho, trozo = 64 - (dir & 63);
        u8 *p = pc_nor.mem + (size_t)b * t + dir, *pr = pc_nor.prog + ((size_t)b * t + dir) / 2;
        if (trozo > n - hecho) trozo = n - hecho;
        for (i = 0; i < trozo; i++) if ((p[i] & d[hecho + i]) != d[hecho + i]) { pc_nor.violaciones++; return -1; }   /* subir un bit */
        for (i = 0; i < trozo; i++) if (p[i] != d[hecho + i] && pr[i / 2]) { pc_nor.violaciones++; return -1; }       /* palabra ya programada */
        if ((oc = op_cortada())) {
            int modo = pc_nor.modo_corte >= 0 ? pc_nor.modo_corte : (int)(pc_nor_azar() % 5);
            u32 corte = pc_nor_azar() % (trozo + 1);
            pc_nor.cortado = oc == 1;
            switch (modo) {
            case 0: break;                                                                     /* nada */
            case 1: memcpy(p, d + hecho, corte & ~1u); break;                                  /* unas palabras sí, otras no */
            case 2: for (i = 0; i < trozo; i++) p[i] &= (u8)(d[hecho + i] | pc_nor_azar()); break;   /* bits a medias en toda la ventana */
            case 3:                                                                            /* palabras enteras + una a medias */
                memcpy(p, d + hecho, corte & ~1u);
                for (i = corte & ~1u; i < trozo && i < (corte & ~1u) + 2; i++) p[i] &= (u8)(d[hecho + i] | pc_nor_azar());
                break;
            default: memcpy(p, d + hecho, trozo); break;                                       /* acabó justo */
            }
            for (i = 0; i < trozo; i++) if (p[i] != 0xFF) pr[i / 2] = 1;   /* una palabra que sigue leyéndose borrada se puede volver a usar */
            return -1;
        }
        for (i = 0; i < trozo; i++) { if (d[hecho + i] != 0xFF || p[i] != 0xFF) pr[i / 2] = 1; p[i] = d[hecho + i]; }
        pc_nor.n_prog++;
        hecho += trozo;
    }
    return 0;
}
/* ---- MODO M18 (W580i): NOR_M18=1 -------------------------------------------------------------------------
 * El mismo modelo estricto, pero «programar» pasa por la capa de modo control del teléfono (hal/m18.h, el MISMO
 * código que hal/nor.c): cada operación de flash es el programa de una REGIÓN entera de 1 KiB físico (512 bytes
 * lógicos) con un búfer de 512 palabras. Además de lo de siempre (no subir bits, no programar dos veces una
 * palabra) se comprueba el modo control: la segunda mitad de cada segmento de 32 bytes tiene que ir a 0xFFFF.
 * Un corte deja la región a medias de las mismas maneras. pc_nor.mem sigue siendo la vista LÓGICA (bloques de
 * 128 KiB), que es la que fotografían las pruebas; la región física se reconstruye de ella en cada operación. */
static unsigned char m18_reg[M18_REGION];
static u32 m18_blq;
static void m18_a_fisico(u32 off)                                    /* la región física, de la vista lógica */
{
    const u8 *l = pc_nor.mem + (size_t)m18_blq * pc_nor.tam + off / 2; u32 i;
    memset(m18_reg, 0xFF, sizeof m18_reg);
    for (i = 0; i < M18_LOG_REG; i++) m18_reg[m18_fis(i)] = l[i];
}
static const unsigned char *m18_region(void *ctx, unsigned off) { (void)ctx; m18_a_fisico(off); return m18_reg; }
static int m18_prog_region(void *ctx, unsigned off, const unsigned short *buf)
{
    const u8 *f = (const u8 *)buf;
    u8 *p = pc_nor.mem + (size_t)m18_blq * pc_nor.tam + off / 2, *pr = pc_nor.prog + ((size_t)m18_blq * pc_nor.tam + off / 2) / 2;
    u8 d[M18_LOG_REG]; u32 i; int oc;
    (void)ctx;
    if (pc_nor.cortado) return -1;
    if ((off & (M18_REGION - 1)) || off >= M18_BLOQUE) { pc_nor.violaciones++; return -1; }
    for (i = 0; i < M18_REGION; i++) if ((i & 16) && f[i] != 0xFF) { pc_nor.violaciones++; return -1; }      /* modo control: la 2.ª mitad del segmento, intacta */
    for (i = 0; i < M18_LOG_REG; i++) d[i] = f[m18_fis(i)];
    for (i = 0; i < M18_LOG_REG; i++) if (d[i] != 0xFF && (p[i] & d[i]) != d[i]) { pc_nor.violaciones++; return -1; }   /* subir un bit */
    for (i = 0; i < M18_LOG_REG; i++) if (d[i] != 0xFF && p[i] != d[i] && pr[i / 2]) { pc_nor.violaciones++; return -1; }   /* palabra ya programada */
    if ((oc = op_cortada())) {
        int modo = pc_nor.modo_corte >= 0 ? pc_nor.modo_corte : (int)(pc_nor_azar() % 5);
        u32 corte = pc_nor_azar() % (M18_LOG_REG + 1);
        pc_nor.cortado = oc == 1;
        switch (modo) {
        case 0: break;
        case 1: for (i = 0; i < (corte & ~1u); i++) p[i] &= d[i]; break;
        case 2: for (i = 0; i < M18_LOG_REG; i++) p[i] &= (u8)(d[i] | pc_nor_azar()); break;
        case 3:
            for (i = 0; i < (corte & ~1u); i++) p[i] &= d[i];
            for (i = corte & ~1u; i < M18_LOG_REG && i < (corte & ~1u) + 2; i++) p[i] &= (u8)(d[i] | pc_nor_azar());
            break;
        default: for (i = 0; i < M18_LOG_REG; i++) p[i] &= d[i]; break;
        }
        for (i = 0; i < M18_LOG_REG; i++) if (p[i] != 0xFF) pr[i / 2] = 1;
        return -1;
    }
    for (i = 0; i < M18_LOG_REG; i++) if (d[i] != 0xFF) { pr[i / 2] = 1; p[i] &= d[i]; }
    pc_nor.n_prog++;
    return 0;
}
static int programar_m18(u32 b, u32 desplaz, const void *datos, u32 n)
{
    static unsigned short buf[512];
    static const M18Puerto pto = { 0, m18_prog_region, m18_region };
    int r;
    if (pc_nor.cortado) return -1;
    if (b >= pc_nor.bloques || pc_nor.tam != M18_LOGICO) { pc_nor.violaciones++; return -1; }
    m18_blq = b;
    r = m18_programar(&pto, pc_nor.mem + (size_t)b * pc_nor.tam, desplaz, datos, n, buf, 0);
    if (r == -M18_E_RANGO || r == -M18_E_BITS) pc_nor.violaciones++;
    return r ? -1 : 0;
}
int pc_nor_m18;
void pc_nor_crear(u32 bloques, u32 tam)
{
    free(pc_nor.mem); free(pc_nor.prog);
    pc_nor.mem = malloc((size_t)bloques * tam); pc_nor.prog = calloc((size_t)bloques * tam / 2, 1);
    memset(pc_nor.mem, 0xFF, (size_t)bloques * tam);
    pc_nor.bloques = bloques; pc_nor.tam = tam; pc_nor.ops = 0; pc_nor.corte_en = -1; pc_nor.cortado = 0; pc_nor.fallo_en = -1;
    pc_nor_m18 = getenv("NOR_M18") && atoi(getenv("NOR_M18"));
    if (pc_nor_m18) {                                                /* la fórmula de la capa contra la del original (FUN_44f964dc) */
        u32 l;
        for (l = 0; l < M18_LOGICO; l++) if (m18_fis(l) != (((l + 0x47E00000u + 0xB8200000u) * 2) & 0xFFFFFFE0u) + (l & 0xF)) abort();
    }
    puerto = (AlmPuerto){ bloques, tam, leer, pc_nor_m18 ? programar_m18 : programar, borrar };
}
const AlmPuerto *pc_nor_puerto(void) { return &puerto; }
