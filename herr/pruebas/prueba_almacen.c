/* Prueba del almacén con cortes de alimentación en CADA operación de flash.
 *   prueba_almacen [operaciones] [bloques] [tam_bloque] [semilla]
 * Para cada operación del guion al azar (guardar / borrar / remontar):
 *   1. se ejecuta entera sobre una copia para saber cuántas operaciones de flash (K) necesita;
 *   2. para cada i en 0..K-1: se restaura la flash, se corta la alimentación en la operación i (dejándola
 *      a medias de 5–7 maneras), se «reinicia» (alm_montar) y se comprueba:
 *        - todas las DEMÁS claves valen exactamente lo de antes;
 *        - la clave tocada vale lo de antes o lo nuevo (nunca otra cosa, nunca medio valor);
 *        - el almacén sigue sirviendo: dos escrituras más y sus lecturas (y una de cada 3 veces, un SEGUNDO
 *          corte durante esa recuperación, y otra comprobación);
 *   3. se restaura y se ejecuta de verdad.
 * El modelo de flash es estricto (plat/pc_nor.c): subir un bit o programar dos veces una palabra es
 * una violación y la prueba falla. Al final: basura al azar en toda la zona y montaje. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "almacen.h"
#include "pc_nor.h"

#define NCLAVES 20
static struct { int hay; u32 n; u8 d[ALM_MAX_VALOR]; } ref[NCLAVES], antes;
static long casos, fallos, dobles;
static u32 az = 12345;
static u32 azar(void) { az ^= az << 13; az ^= az >> 17; az ^= az << 5; return az; }

static int igual(int k, int hay, u32 n, const u8 *d)
{
    static u8 b[ALM_MAX_VALOR];
    int r = alm_cargar((u16)(100 + k), b, sizeof b);
    if (!hay) return r == ALM_E_NO_HAY;
    return r == (int)n && !memcmp(b, d, n);
}
static void falla(const char *que, int k, long i)
{
    if (fallos++ < 20) fprintf(stderr, "FALLO: %s (clave %d, corte en la operación %ld, caso %ld)\n", que, k, i, casos);
}
static void comprobar(int tocada, int nueva_hay, u32 nueva_n, const u8 *nueva_d, long i)
{
    int k;
    for (k = 0; k < NCLAVES; k++) {
        if (k == tocada) {
            if (!igual(k, antes.hay, antes.n, antes.d) && !igual(k, nueva_hay, nueva_n, nueva_d)) falla("la clave tocada no vale ni lo de antes ni lo nuevo", k, i);
        } else if (!igual(k, ref[k].hay, ref[k].n, ref[k].d)) falla("OTRA clave cambió", k, i);
    }
}

int main(int argc, char **argv)
{
    long nops = argc > 1 ? atol(argv[1]) : 2000, op;
    u32 bloques = argc > 2 ? (u32)atol(argv[2]) : 4, tam = argc > 3 ? (u32)atol(argv[3]) : 4096, tope_valor;
    size_t total;
    u8 *foto, *foto_p, *nuevo = malloc(ALM_MAX_VALOR);
    AlmEstado e;
    if (argc > 4) az = (u32)atol(argv[4]) | 1;
    pc_nor.semilla = az * 7 + 1;
    pc_nor_crear(bloques, tam);
    total = (size_t)bloques * tam;
    foto = malloc(total); foto_p = malloc(total / 2);
    tope_valor = tam / 12 < ALM_MAX_VALOR ? tam / 12 : ALM_MAX_VALOR;

    /* formato inicial con corte en cada operación */
    {
        long k, i;
        alm_montar(pc_nor_puerto(), 1); k = pc_nor.ops;
        for (i = 0; i < k; i++) {
            memset(pc_nor.mem, 0xFF, total); memset(pc_nor.prog, 0, total / 2);
            pc_nor.ops = 0; pc_nor.corte_en = i; pc_nor.cortado = 0;
            alm_montar(pc_nor_puerto(), 1);
            pc_nor_encender();
            if (alm_montar(pc_nor_puerto(), 1) != ALM_OK) falla("no monta tras cortar el formato", -1, i);
            casos++;
        }
    }
    for (op = 0; op < nops; op++) {
        int k = (int)(azar() % NCLAVES), tipo = (int)(azar() % 100), nueva_hay; u32 n = 0, j; long K, i, ops0;
        if (tipo < 4) { if (alm_montar(pc_nor_puerto(), 1) != ALM_OK) falla("remontar", -1, -1); continue; }
        nueva_hay = tipo < 85;
        if (nueva_hay) {
            n = azar() % 100 < 8 ? azar() % (tope_valor + 1) : azar() % (tope_valor / 6 + 1);
            for (j = 0; j < n; j++) nuevo[j] = (u8)(azar() >> 9);
            if (azar() % 16 == 0) memset(nuevo, 0xFF, n);                  /* un valor todo a 0xFF también vale */
        }
        antes.hay = ref[k].hay; antes.n = ref[k].n; memcpy(antes.d, ref[k].d, ref[k].n);
        memcpy(foto, pc_nor.mem, total); memcpy(foto_p, pc_nor.prog, total / 2);
        /* 1. cuántas operaciones de flash hace */
        ops0 = pc_nor.ops;
        { int r = nueva_hay ? alm_guardar((u16)(100 + k), nuevo, n) : alm_borrar((u16)(100 + k));
          K = pc_nor.ops - ops0;
          if (r == ALM_E_LLENO) { memcpy(pc_nor.mem, foto, total); memcpy(pc_nor.prog, foto_p, total / 2); alm_montar(pc_nor_puerto(), 1); continue; }
          if (r != ALM_OK) falla("la operación sin corte falló", k, -1); }
        /* 2. un corte en cada una */
        for (i = 0; i < K; i++) {
            int r2;
            memcpy(pc_nor.mem, foto, total); memcpy(pc_nor.prog, foto_p, total / 2);
            pc_nor.cortado = 0; pc_nor.corte_en = -1;
            if (alm_montar(pc_nor_puerto(), 1) != ALM_OK) falla("no monta la foto", k, i);
            pc_nor.corte_en = pc_nor.ops + i;
            if (nueva_hay) alm_guardar((u16)(100 + k), nuevo, n); else alm_borrar((u16)(100 + k));
            pc_nor_encender();
            if (alm_montar(pc_nor_puerto(), 1) != ALM_OK) { falla("no monta tras el corte", k, i); continue; }
            comprobar(k, nueva_hay, n, nuevo, i);
            casos++;
            /* el almacén tiene que seguir sirviendo; a veces, con un segundo corte durante la recuperación */
            {
                u8 x[40]; int k2 = (int)(azar() % NCLAVES), doble = azar() % 3 == 0; u32 n2 = azar() % 40;
                int quedo_nuevo = igual(k, nueva_hay, n, nuevo);
                for (j = 0; j < n2; j++) x[j] = (u8)azar();
                if (k2 == k) k2 = (k + 1) % NCLAVES;
                if (doble) { pc_nor.corte_en = pc_nor.ops + (long)(azar() % 3); dobles++; }
                r2 = alm_guardar((u16)(100 + k2), x, n2);
                if (doble) {
                    pc_nor_encender();
                    if (alm_montar(pc_nor_puerto(), 1) != ALM_OK) falla("no monta tras el segundo corte", k2, i);
                    if (!igual(k2, ref[k2].hay, ref[k2].n, ref[k2].d) && !igual(k2, 1, n2, x)) falla("segundo corte: la clave no vale ni lo de antes ni lo nuevo", k2, i);
                    if (!(quedo_nuevo ? igual(k, nueva_hay, n, nuevo) : igual(k, antes.hay, antes.n, antes.d))) falla("segundo corte: cambió la clave del primero", k, i);
                } else if (r2 != ALM_OK && r2 != ALM_E_LLENO) falla("tras recuperarse no deja guardar", k2, i);
                else if (r2 == ALM_OK && !igual(k2, 1, n2, x)) falla("tras recuperarse no lee lo guardado", k2, i);
            }
        }
        /* 3. de verdad */
        memcpy(pc_nor.mem, foto, total); memcpy(pc_nor.prog, foto_p, total / 2);
        pc_nor.cortado = 0; pc_nor.corte_en = -1;
        alm_montar(pc_nor_puerto(), 1);
        if ((nueva_hay ? alm_guardar((u16)(100 + k), nuevo, n) : alm_borrar((u16)(100 + k))) != ALM_OK) falla("la operación de verdad falló", k, -1);
        ref[k].hay = nueva_hay; ref[k].n = n; memcpy(ref[k].d, nuevo, n);
        if (azar() % 7 == 0) alm_montar(pc_nor_puerto(), 1);
        { int q; for (q = 0; q < NCLAVES; q++) if (!igual(q, ref[q].hay, ref[q].n, ref[q].d)) falla("sin cortes: una clave no vale lo guardado", q, -1); }
    }
    alm_estado(&e);
    printf("almacén %u x %u: %ld operaciones, %ld cortes comprobados (%ld con segundo corte), %ld fallos, %ld violaciones de la flash\n",
           bloques, tam, nops, casos, dobles, fallos, pc_nor.violaciones);
    printf("  generación %u, compactaciones en la última montura %u, programas %ld, borrados %ld; por bloque:", e.generacion, e.compactaciones, pc_nor.n_prog, pc_nor.n_borr);
    { u32 b; for (b = 0; b < bloques && b < 64; b++) printf(" %u", pc_nor.borrados_por_bloque[b]); printf("\n"); }

    { long v0 = pc_nor.violaciones; printf("  violaciones del modelo estricto en la prueba de cortes: %ld\n", v0); if (v0) fallos++; }
    /* basura: 300 zonas al azar (y almacenes válidos con bytes cambiados): montar no puede romperse y debe quedar utilizable */
    {
        int v; long mal = 0;
        memcpy(foto, pc_nor.mem, total);
        for (v = 0; v < 300; v++) {
            size_t j; u8 x[16] = "hola"; u8 y[16];
            if (v % 2) for (j = 0; j < total; j++) pc_nor.mem[j] = (u8)azar();
            else { memcpy(pc_nor.mem, foto, total); for (j = 0; j < 1 + azar() % 200; j++) pc_nor.mem[azar() % total] = (u8)azar(); }
            for (j = 0; j < total; j++) pc_nor.prog[j / 2] = (u8)(pc_nor.mem[j & ~(size_t)1] != 0xFF || pc_nor.mem[j | 1] != 0xFF);
            pc_nor_encender();
            if (alm_montar(pc_nor_puerto(), 1) != ALM_OK) { mal++; continue; }
            for (j = 0; j < NCLAVES; j++) alm_cargar((u16)(100 + j), nuevo, ALM_MAX_VALOR);
            if (alm_guardar(7, x, 5) != ALM_OK || alm_cargar(7, y, 16) != 5 || memcmp(x, y, 5)) mal++;
        }
        printf("  basura: 300 montajes sobre basura o almacenes dañados, %ld sin poder usarse después\n", mal);
        fallos += mal;
    }
    printf(fallos || pc_nor.violaciones ? "HAY FALLOS\n" : "TODO BIEN\n");
    return fallos || pc_nor.violaciones ? 1 : 0;
}
