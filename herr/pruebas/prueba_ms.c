/* Prueba en el PC del PROTOCOLO Memory Stick PRO (hal/ms.c), lectura y (pista MSW) escritura de sectores, contra un anfitrión y una tarjeta
 * SIMULADOS, con FALLOS INYECTABLES. Pista T. make probar-ms
 *
 * Es lo que el emulador no puede dar (su modelo de tarjeta no falla nunca: work/bitacora_E9.md, pendiente 2): tiempo
 * agotado y CRC en cualquier transacción, INT que no llega, tarjeta que se queda muda a media orden, ERR, tarjeta
 * que no es PRO, CED que tarda o no llega, atributos malos, PMU que no contesta, tarjeta que sale en CUALQUIER
 * transacción, rebote del pin, otra tarjeta al despertar. El reloj es simulado: los topes de tiempo se comprueban.
 *
 * OJO con lo que prueba: la tarjeta simulada es NUESTRA lectura del protocolo (la misma de docs/memory_stick.md): esto
 * prueba que hal/ms.c no se cuelga, no entrega datos malos y se recupera ante cada fallo; NO prueba que una M2 de
 * verdad se comporte así.
 *
 * El modelo además VIGILA al controlador (lo mismo que las reglas [ms] del emulador estricto): cualquier orden que no
 * sea 0x20, 0x21 o 0x24, una 0x21 a una tarjeta protegida o que se sale de la tarjeta, un TPC sin tensión o a menos de
 * 10 ms de darla, un READ/WRITE_LONG_DATA sin BREQ o de la orden que no toca, una orden nueva sin el CED de la
 * anterior… es una INFRACCIÓN y la prueba falla.
 *
 * (MSW) La tarjeta simulada GUARDA lo escrito (una tabla de sectores escritos; lo no escrito es dato()), y solo cuando
 * la tarjeta ACEPTA el sector (WRITE_LONG_DATA bien). Así se comprueba que lo que el controlador da por escrito lo está,
 * y que tras un fallo no ha escrito nada fuera del tramo pedido. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms.h"
/* (ms810) Con -DMS_SOLO_SERIE (make probar-ms-serie) la MISMA prueba contra el camino solo serie de hal/ms.c (el del W810):
 * la tarjeta simulada se queda en serie, el anfitrión no pasa nunca a paralelo, el registro INT se lee con GET_INT y cada
 * sector cuesta DOS TPC (GET_INT + el de datos) más un GET_INT de cierre por orden. */
#ifdef MS_SOLO_SERIE
#define TPC_LEER(n, ord)     (2u * (ord) + 2u * (n))           /* TPC de una lectura de n sectores en «ord» órdenes */
#else
#define TPC_LEER(n, ord)     ((ord) + (n))
#endif

/* ---- el reloj y el pin -------------------------------------------------------------------------------------- */
static unsigned long long us;                                 /* microsegundos simulados */
static u32 latidos, infracciones, tpcs, ordenes_mandadas;
static char ultima_infraccion[128];
static void infraccion(const char *q) { infracciones++; snprintf(ultima_infraccion, sizeof ultima_infraccion, "%s", q); }

/* ---- la tarjeta y el anfitrión ------------------------------------------------------------------------------ */
typedef struct {
    int muda;                       /* no contesta a nada */
    int tiempo_en_tpc;              /* el TPC n.º N (desde que se armó el fallo) da tiempo agotado; 0 = no */
    int crc_en_dato, crc_siempre;   /* el READ_LONG_DATA n.º N da CRC / todos los de la orden 0x20 lo dan */
    int pierde_int;                 /* la INT n.º N no llega al anfitrión (el estado sí es válido) */
    int cuelga_en_sector;           /* tras entregar N sectores la tarjeta se queda muda (ni INT ni estado) hasta el apagado */
    int cuelga_siempre;
    int err_en_sector;              /* al ir a dar el sector N dice ERR */
    int sale_en_tpc;                /* la tarjeta SALE justo antes del TPC n.º N */
    int sale_en_espera;             /* … o en la espera de INT n.º N */
    int ced_ms, ced_nunca;          /* lo que tarda el CED tras la tensión */
    int tipo, categoria, clase;     /* registros 0x04, 0x06, 0x07 (por omisión 1, 0, 0) */
    int firma_mal, info_lejos, sin_info, capacidad_cero;
    int pmu_mudo, rst_atascado;
    int max_por_orden;              /* si no es 0: una orden de más sectores se rechaza (CMDNK) */
    int int_tardia;                 /* H3 del revés y peor: la INT del paso a paralelo llega DESPUÉS de la primera orden */
    int wp;                         /* (MSW) STATUS0 bit 0: protegida contra escritura */
    int w_crc_en;                   /* (MSW) el WRITE_LONG_DATA n.º N llega con CRC malo: la tarjeta NO lo toma */
    int w_err_en;                   /* (MSW) al tomar el sector n.º N de una escritura, la tarjeta dice ERR (y no lo guarda) */
    int w_cuelga_en;                /* (MSW) tras tomar N sectores de una escritura, la tarjeta se queda muda */
    int no_paralelo;                /* (M2) 1 = el WRITE_REG del registro 0x10 no contesta nunca; 2 = pasa, pero en paralelo el anfitrión no enseña
                                       el CED (lo visto en el W580 real: «la tarjeta no pasa a 4 bits», sin saber en cuál de los dos) */
    int sin_msint_serie;            /* (M2) en serie la tarjeta NO levanta la INT en el anfitrión (que el DB2020 la dé en serie es [sin saber]) */
    int lenta_us;                   /* (M2) lo que tarda la tarjeta en tener lista cada respuesta (BREQ, CED…): hasta entonces su registro INT dice 0 */
    int estado_retenido;            /* (M2 ag) en paralelo los bits 3..0 de +8 NO están vivos: el anfitrión los carga con cada INT de la tarjeta (como el MSHC
                                       de Linux, tifm_ms.c); hasta la primera INT en paralelo dicen 0. La causa [inferida] del fallo de la v3 */
    int sin_int_par;                /* (M2 ag) en paralelo la INT de la tarjeta NO llega al anfitrión (el estado de +8, sí) */
    int bus4;                       /* (M2 ag) el bus de 4 bits no anda: 1 = todo TPC en paralelo da CRC (la tarjeta no toma nada); 2 = los datos que
                                       llegan en paralelo traen DAT1..3 pegados a 1 (sin error de CRC) */
} Fallos;
static Fallos F;
static u32 n_tpc_f, n_dato_f, n_int_f, n_espera_f;            /* cuentas desde que se armó F */

static int puesta, tension, h_par, c_par, colgada;
static unsigned long long t_tension;
static u8 reg[0x20], adr[4], int_reg; static int msint;
static u8 orden_c; static u32 quedan, lba_c, entregados, sectores_tarjeta = 131072;
static int orden_viva;                                        /* hay una orden sin llegar a su CED */
static unsigned long long t_listo;                            /* (M2) con F.lenta_us: desde cuándo vale int_reg */
static u8 est_par;                                            /* (M2 ag) con F.estado_retenido: lo que enseña +8 en paralelo (lo de la última INT) */

static u8 dato0(u32 lba, u32 i) { return (u8)(lba * 31u + i * 7u + (lba >> 8) + (i >> 3)); }
/* (MSW) lo escrito: tabla abierta de sectores (lba -> 512 bytes). Se vacía al cambiar de tarjeta (caso()). */
#define W_MAX 8192u
static struct { u32 lba; u8 usado, virgen; u8 b[512]; } wt[W_MAX];   /* (28-oct) virgen: lo que se escribió se PERDIÓ (modelo de la M2) */
static u32 w_n, n_wdato_f, w_tomados;
static int w_busca(u32 lba, int crear)
{
    u32 h = (lba * 2654435761u) % W_MAX, k;
    for (k = 0; k < W_MAX; k++, h = (h + 1) % W_MAX) {
        if (!wt[h].usado) { if (!crear) return -1; if (w_n >= W_MAX - 1) { printf("tabla de escritos llena\n"); exit(2); } wt[h].usado = 1; wt[h].lba = lba; w_n++; return (int)h; }
        if (wt[h].lba == lba) return (int)h;
    }
    return -1;
}
static u8 dato(u32 lba, u32 i) { int h = w_busca(lba, 0); return h < 0 || wt[h].virgen ? dato0(lba, i) : wt[h].b[i]; }
/* (28-oct) EL MODELO DE LA M2 DEL W580 [observado 28-oct, herr/puente/tj_lote.py; docs/tarjeta_escritura.md §10]: los cambios de los 4 ÚLTIMOS
 * bloques de borrado distintos (512 sectores) que se tocaron viven SOLO en su memoria: al quitarle la tensión vuelven a como estaban; tocar
 * un bloque nuevo consolida el más antiguo (aquí, el usado hace más tiempo). Solo con modelo_ftl = 1. */
#define FTL_BLQ 512u
#define FTL_PEND 4
static int modelo_ftl;
static struct { u32 blq; u8 usado; u8 guardado[FTL_BLQ / 8]; u8 habia[FTL_BLQ]; u8 b[FTL_BLQ][512]; } ftl_s[FTL_PEND];
static int ftl_orden[FTL_PEND], ftl_n;                         /* las ranuras pendientes, de la menos a la más reciente */
static u32 ftl_perdidos;                                       /* sectores que volvieron atrás en un corte */
static void ftl_tocar(u32 lba)                                 /* antes de que la tarjeta guarde el sector lba */
{
    u32 b = lba / FTL_BLQ, o = lba % FTL_BLQ; int k, j, s; int h;
    if (!modelo_ftl) return;
    for (k = 0; k < ftl_n; k++) if (ftl_s[ftl_orden[k]].blq == b) break;
    if (k < ftl_n) { s = ftl_orden[k]; for (j = k; j + 1 < ftl_n; j++) ftl_orden[j] = ftl_orden[j + 1]; ftl_orden[ftl_n - 1] = s; }
    else {
        if (ftl_n == FTL_PEND) { s = ftl_orden[0]; for (j = 0; j + 1 < ftl_n; j++) ftl_orden[j] = ftl_orden[j + 1]; ftl_n--; }   /* consolidado */
        else for (s = 0; s < FTL_PEND; s++) { for (j = 0; j < ftl_n; j++) if (ftl_orden[j] == s) break; if (j == ftl_n) break; }
        ftl_s[s].blq = b; ftl_s[s].usado = 1; memset(ftl_s[s].guardado, 0, sizeof ftl_s[s].guardado);
        ftl_orden[ftl_n++] = s;
    }
    s = ftl_orden[ftl_n - 1];
    if (ftl_s[s].guardado[o >> 3] & (1u << (o & 7))) return;  /* ya se apuntó cómo estaba */
    ftl_s[s].guardado[o >> 3] |= (u8)(1u << (o & 7));
    h = w_busca(lba, 0);
    ftl_s[s].habia[o] = (u8)(h >= 0 && !wt[h].virgen);
    if (ftl_s[s].habia[o]) memcpy(ftl_s[s].b[o], wt[h].b, 512);
}
static void ftl_corte(void)                                    /* sin tensión: lo pendiente vuelve a como estaba */
{
    int k; u32 o; int h;
    for (k = 0; k < ftl_n; k++) {
        int s = ftl_orden[k];
        for (o = 0; o < FTL_BLQ; o++) if (ftl_s[s].guardado[o >> 3] & (1u << (o & 7))) {
            h = w_busca(ftl_s[s].blq * FTL_BLQ + o, 1);
            if (ftl_s[s].habia[o]) { memcpy(wt[h].b, ftl_s[s].b[o], 512); wt[h].virgen = 0; } else wt[h].virgen = 1;
            ftl_perdidos++;
        }
    }
    ftl_n = 0;
}
static void w_vacia(void) { memset(wt, 0, sizeof wt); w_n = 0; ftl_n = 0; modelo_ftl = 0; }
static void levanta_int(u8 v)
{
    int_reg = v; t_listo = us + (unsigned long long)F.lenta_us;
    n_int_f++;
    if (F.pierde_int && n_int_f == (u32)F.pierde_int) return;
    if (F.sin_msint_serie && !h_par) return;
    if (h_par) est_par = v;
    if (F.sin_int_par && h_par) return;
    msint = 1;
}
static u8 int_vivo(void)
{
    if (!tension || !puesta || colgada) return 0;
    if (!orden_c && !orden_viva) {                            /* reposo tras encender: CED cuando toque */
        if (F.ced_nunca) return 0;
        return us - t_tension >= (unsigned long long)(F.ced_ms ? F.ced_ms : 2) * 1000 ? MS_INT_CED : 0;
    }
    return us < t_listo ? 0 : int_reg;
}
static void atributos(u8 *b)
{
    u32 dir = F.info_lejos ? 0x400 : 0x1A0, bloques = sectores_tarjeta / 128;
    memset(b, 0, 512);
    b[0] = F.firma_mal ? 0x00 : 0xA5; b[1] = 0xC3; b[4] = 1;
    b[0x10] = 0; b[0x11] = 0; b[0x12] = (u8)(dir >> 8); b[0x13] = (u8)dir; b[0x17] = 0x60; b[0x18] = F.sin_info ? 0x15 : 0x10;
    if (dir + 0x60 <= 512) {
        u8 *s = b + dir;
        s[0] = 2; s[2] = 0; s[3] = 0x80; s[4] = (u8)(bloques >> 8); s[5] = (u8)bloques;
        s[6] = F.capacidad_cero ? 0 : (u8)(bloques >> 8); s[7] = F.capacidad_cero ? 0 : (u8)bloques; s[0x2C] = 2; s[0x2D] = 0;
    }
}
static unsigned long long pin_hasta;
static int a_hay(void) { return puesta || us < pin_hasta; }
static int a_encender(void)
{
    if (F.rst_atascado) return MS_E_ANFITRION;
    if (F.pmu_mudo) return MS_E_PMU;
    tension = 1; t_tension = us; h_par = c_par = 0; colgada = 0; msint = 0; int_reg = 0; orden_c = 0; orden_viva = 0; quedan = 0; est_par = 0; t_listo = 0;   /* (M2 ag) t_listo viejo de otro caso estiraba la INT del encendido */
    memset(reg, 0, sizeof reg); memset(adr, 0, sizeof adr);
    reg[4] = (u8)(F.tipo ? F.tipo : 1); reg[6] = (u8)F.categoria; reg[7] = (u8)F.clase; reg[0x10] = 0x80; reg[2] = F.wp ? 1 : 0;
    if (!F.ced_nunca && !F.int_tardia && !F.sin_msint_serie) msint = 1;   /* H3: el CED del encendido da INT también en serie */
    us += 300;
    return 0;
}
static u32 cortes_w, cortes_w_muda;                           /* (28-oct) tensión quitada con una orden 0x21 SIN cerrar y la tarjeta metida: lo que rompe la tabla
                                                                 interna de una flash (aparte, con la tarjeta muda: ahí no queda otra) */
static void a_apagar(void) { if (tension && puesta && orden_viva && orden_c == 0x21) { if (colgada || F.muda) cortes_w_muda++; else cortes_w++; } if (tension) ftl_corte(); tension = 0; h_par = c_par = 0; orden_viva = 0; orden_c = 0; msint = 0; us += 200; }
static void a_paralelo(void)
{
#ifdef MS_SOLO_SERIE
    infraccion("SOLO SERIE: el anfitrion pasa a paralelo");
#endif
    if (!c_par && tension) infraccion("anfitrion a paralelo sin haber escrito 0 en el registro 0x10 de la tarjeta");
    h_par = 1; est_par = 0;
}
static int a_tpc(int t, u8 *b, u32 n, int a_la_tarjeta)
{
    tpcs++; n_tpc_f++;
    us += 100;
    if (F.sale_en_tpc && n_tpc_f == (u32)F.sale_en_tpc) puesta = 0;
    if (t != MS_TPC_READ_LONG_DATA && t != MS_TPC_READ_REG && t != MS_TPC_GET_INT && t != MS_TPC_SET_RW_REG_ADRS && t != MS_TPC_EX_SET_CMD && t != MS_TPC_WRITE_REG && t != MS_TPC_WRITE_LONG_DATA) { infraccion("TPC que no es de la lista"); return MS_T_NEGADO; }
    if (!tension) { infraccion("TPC sin tension"); return MS_T_ANFITRION; }
    if (us - t_tension < 10000) infraccion("TPC a menos de 10 ms de dar tension");
    if (!puesta) return MS_T_FUERA;
    if (F.muda || colgada || h_par != c_par) { us += 500000; return MS_T_TIEMPO; }
    if (F.tiempo_en_tpc && n_tpc_f == (u32)F.tiempo_en_tpc) { us += 500000; return MS_T_TIEMPO; }
    if (F.bus4 == 1 && h_par) { us += 300; return MS_T_CRC; }        /* (M2 ag) nada pasa por el bus de 4 bits */
    if (F.bus4 == 2 && h_par && !a_la_tarjeta && t != MS_TPC_GET_INT) {   /* (M2 ag) llega, pero con DAT1..3 pegados a 1 */
        int r = 0; u32 i;
        F.bus4 = 0; r = a_tpc(t, b, n, a_la_tarjeta); F.bus4 = 2; tpcs--; n_tpc_f--;
        for (i = 0; i < n; i++) b[i] |= 0xEE;
        return r;
    }
    switch (t) {
    case MS_TPC_SET_RW_REG_ADRS: if (n != 4 || !a_la_tarjeta) infraccion("SET_RW_REG_ADRS mal"); memcpy(adr, b, 4); break;
    case MS_TPC_READ_REG:
        if (n != adr[1] || adr[0] + n > sizeof reg) { infraccion("READ_REG con tamano distinto del de SET_RW_REG_ADRS"); return MS_T_TIEMPO; }
        memcpy(b, reg + adr[0], n); break;
    case MS_TPC_WRITE_REG:
#ifdef MS_SOLO_SERIE
        infraccion("SOLO SERIE: WRITE_REG (el W810 no escribe registros de la tarjeta)");
#endif
        if (n != adr[3] || adr[2] + n > sizeof reg) { infraccion("WRITE_REG con tamano distinto del de SET_RW_REG_ADRS"); return MS_T_TIEMPO; }
        if (adr[2] != 0x10 || n != 1 || b[0] != 0) infraccion("WRITE_REG que no es «registro 0x10 <- 0»");
        if (ms_info()->serie) infraccion("(M2) WRITE_REG 0x10 con el controlador ya caido a 1 bit");
        if (F.no_paralelo == 1) { us += 500000; return MS_T_TIEMPO; }
        memcpy(reg + adr[2], b, n); c_par = reg[0x10] == 0; break;
    case MS_TPC_GET_INT:
        if (h_par) infraccion("GET_INT por TPC en paralelo");
        b[0] = int_vivo(); break;
    case MS_TPC_EX_SET_CMD: {
        u32 cuenta = (u32)b[1] << 8 | b[2], a = (u32)b[3] << 24 | (u32)b[4] << 16 | (u32)b[5] << 8 | b[6];
        ordenes_mandadas++;
        if (n != 7) infraccion("EX_SET_CMD que no es de 7 bytes");
        if (b[0] != 0x20 && b[0] != 0x24 && b[0] != 0x21) { infraccion(b[0] == 0x10 ? "ORDEN 0x10: FORMATEAR" : "orden que no es 0x20, 0x21 ni 0x24"); levanta_int(MS_INT_CMDNK | MS_INT_CED); break; }
        if (b[0] == 0x21 && (F.wp || F.clase)) { infraccion("ORDEN 0x21 A UNA TARJETA PROTEGIDA"); levanta_int(MS_INT_CMDNK | MS_INT_CED); break; }
        if (b[0] == 0x21 && (!cuenta || a >= sectores_tarjeta || cuenta > sectores_tarjeta - a)) infraccion("ORDEN 0x21 QUE SE SALE DE LA TARJETA");
        if (orden_viva) infraccion("orden nueva sin haber llegado al CED de la anterior");
#ifndef MS_SOLO_SERIE
        if (!h_par && !ms_info()->serie) infraccion("orden en serie (sin haber caido a 1 bit)");
        if (h_par && ms_info()->serie) infraccion("(M2) orden en paralelo con el controlador caido a 1 bit");
#else
        if (h_par || c_par) infraccion("SOLO SERIE: orden en paralelo");
#endif
        if (b[0] == 0x24 && (a != 0 || cuenta != 1)) infraccion("atributos con direccion != 0 o cuenta != 1");
        orden_c = b[0]; quedan = cuenta; lba_c = a; entregados = 0; orden_viva = 1;
        if (F.int_tardia == 1) { F.int_tardia = 2; msint = 1; int_reg = 0; break; }      /* llega ahora la INT «vieja»: sin nada que decir aún */
        if (!cuenta || ((b[0] == 0x20 || b[0] == 0x21) && (a >= sectores_tarjeta || cuenta > sectores_tarjeta - a))) { levanta_int(MS_INT_ERR | MS_INT_CED); orden_viva = 0; break; }
        if (F.max_por_orden && b[0] == 0x20 && cuenta > (u32)F.max_por_orden) { levanta_int(MS_INT_CMDNK | MS_INT_CED); orden_viva = 0; break; }
        if (F.err_en_sector == 1 && b[0] == 0x20) { levanta_int(MS_INT_ERR | MS_INT_CED); orden_viva = 0; break; }
        if ((F.cuelga_en_sector == 1 || F.cuelga_siempre) && b[0] == 0x20) { colgada = 1; if (!F.cuelga_siempre) F.cuelga_en_sector = 0; break; }
        levanta_int(MS_INT_BREQ);
        break; }
    case MS_TPC_READ_LONG_DATA: {
        u32 i;
        n_dato_f++;
        if (n != 512) infraccion("READ_LONG_DATA que no es de 512");
        if (!(int_reg & MS_INT_BREQ) || !orden_viva) { infraccion("READ_LONG_DATA sin BREQ"); return MS_T_TIEMPO; }
        if (orden_c == 0x21) { infraccion("READ_LONG_DATA en una orden de escritura"); return MS_T_TIEMPO; }
        if (orden_c == 0x24) atributos(b); else for (i = 0; i < 512; i++) b[i] = dato(lba_c + entregados, i);
        entregados++; quedan--;
        if ((F.crc_siempre && orden_c == 0x20) || (F.crc_en_dato && n_dato_f == (u32)F.crc_en_dato)) { for (i = 0; i < 512; i += 37) b[i] ^= 0x5A; us += 400; if (quedan) levanta_int(MS_INT_BREQ); else { levanta_int(MS_INT_CED); orden_viva = 0; } return MS_T_CRC; }
        if (!quedan) { levanta_int(MS_INT_CED); orden_viva = 0; }
        else if (orden_c == 0x20 && F.err_en_sector && entregados + 1 == (u32)F.err_en_sector) { levanta_int(MS_INT_ERR | MS_INT_CED); orden_viva = 0; }
        else if (orden_c == 0x20 && ((F.cuelga_en_sector && entregados + 1 == (u32)F.cuelga_en_sector) || 0)) { colgada = 1; F.cuelga_en_sector = 0; int_reg = 0; }
        else levanta_int(MS_INT_BREQ);
        us += 300;
        break; }
    case MS_TPC_WRITE_LONG_DATA: {                            /* (MSW) */
        int h;
        n_wdato_f++;
        if (n != 512 || !a_la_tarjeta) infraccion("WRITE_LONG_DATA que no es de 512 a la tarjeta");
        if (!(int_reg & MS_INT_BREQ) || !orden_viva) { infraccion("WRITE_LONG_DATA sin BREQ (repetido, o sin orden)"); return MS_T_TIEMPO; }
        if (orden_c != 0x21) { infraccion("WRITE_LONG_DATA en una orden que no es de escritura"); return MS_T_TIEMPO; }
        if (F.w_crc_en && n_wdato_f == (u32)F.w_crc_en) { us += 400; return MS_T_CRC; }     /* no lo toma; sigue esperando el dato (BREQ) */
        if (F.w_err_en && n_wdato_f == (u32)F.w_err_en) { levanta_int(MS_INT_ERR | MS_INT_CED); orden_viva = 0; us += 300; return MS_T_BIEN; }
        ftl_tocar(lba_c + entregados);
        h = w_busca(lba_c + entregados, 1); memcpy(wt[h].b, b, 512); wt[h].virgen = 0; w_tomados++;
        entregados++; quedan--;
        if (F.w_cuelga_en && n_wdato_f == (u32)F.w_cuelga_en) { colgada = 1; int_reg = 0; us += 300; return MS_T_BIEN; }
        if (!quedan) { levanta_int(MS_INT_CED); orden_viva = 0; } else levanta_int(MS_INT_BREQ);
        us += 600;                                            /* escribir tarda más que leer [inventado] */
        break; }
    }
    return MS_T_BIEN;
}
static int a_espera_int(u32 ms)
{
    n_espera_f++;
    if (F.sale_en_espera && n_espera_f == (u32)F.sale_en_espera) puesta = 0;
    if (!puesta) return MS_T_FUERA;
    if (msint && tension) { msint = 0; if (us < t_listo) us = t_listo; us += 150; return MS_T_BIEN; }   /* la INT llega cuando la respuesta está lista */
    us += (unsigned long long)ms * 1000;
    return MS_T_TIEMPO;
}
static void a_olvida_int(void) { msint = 0; }
static int a_int_paralelo(void) { return h_par && c_par && F.no_paralelo != 2 ? (F.estado_retenido ? est_par : int_vivo()) : 0; }
static void a_espera_ms(u32 ms) { us += (unsigned long long)ms * 1000; }
static u32 a_ms(void) { return (u32)(us / 1000); }
static void a_latido(void) { latidos++; }
static const MsAnf anf = { a_hay, a_encender, a_apagar, a_paralelo, a_tpc, a_espera_int, a_olvida_int, a_int_paralelo, a_espera_ms, a_ms, a_latido };

/* ---- utilería de la prueba ----------------------------------------------------------------------------------- */
static int fallos_prueba, casos;
static const char *caso_actual;
#define OK(c, ...) do { if (!(c)) { fallos_prueba++; printf("  MAL [%s] ", caso_actual); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void caso(const char *n)
{
    if (caso_actual && infracciones) { fallos_prueba++; printf("  MAL [%s] %u infracciones del controlador; la ultima: %s\n", caso_actual, infracciones, ultima_infraccion); }
    caso_actual = n; casos++;
    memset(&F, 0, sizeof F); n_tpc_f = n_dato_f = n_int_f = n_espera_f = 0;
    us = 1000000; latidos = infracciones = tpcs = ordenes_mandadas = 0;
    puesta = tension = h_par = c_par = colgada = msint = 0; sectores_tarjeta = 131072; pin_hasta = 0;
    w_vacia(); n_wdato_f = 0; w_tomados = 0;
    ms_init(&anf);
}
static void armar(void) { n_tpc_f = n_dato_f = n_int_f = n_espera_f = n_wdato_f = 0; }
/* (I1) meter = lo que hace el bucle: un paso al ver la tarjeta y, mientras se prepara (el CED, por pasos), otro cada 4 ms. Se
 * apunta la vuelta más larga (lo que la interfaz se queda quieta) y cuántas vueltas fueron. */
static u32 peor_vuelta_ms, vueltas;
static void paso_medido(void) { unsigned long long a = us; ms_paso(); if ((u32)((us - a) / 1000) > peor_vuelta_ms) peor_vuelta_ms = (u32)((us - a) / 1000); vueltas++; }
static void meter(void)
{
    puesta = 1; peor_vuelta_ms = vueltas = 0; paso_medido();
    while (ms_info()->estado == MS_PREPARANDO && vueltas < 100000) { us += 4000; paso_medido(); }
}
static void sacar(void) { puesta = 0; ms_paso(); }
static u8 buf[300 * 512];
static int datos_bien(u32 lba, u32 n) { u32 s, i; for (s = 0; s < n; s++) for (i = 0; i < 512; i++) if (buf[s * 512 + i] != dato(lba + s, i)) return 0; return 1; }
static int leer(u32 lba, u32 n) { memset(buf, 0xEE, n * 512 < sizeof buf ? n * 512 : sizeof buf); return ms_leer(lba, n, buf); }
static unsigned long long t0;
static void crono(void) { t0 = us; }
static u32 pasado_ms(void) { return (u32)((us - t0) / 1000); }
static u32 azar_s;
static u32 azar(void) { azar_s = azar_s * 1664525u + 1013904223u; return azar_s >> 8; }
/* (MSW) escribir: wbuf con un patrón de (lba, semilla) */
static u8 wbuf[300 * 512];
static u8 patron(u32 lba, u32 i, u32 sem) { return (u8)(lba * 13u + i * 5u + sem * 101u + (i >> 5) + 0x5A); }
static int escribir(u32 lba, u32 n, u32 sem) { u32 s, i; for (s = 0; s < n && s < 300; s++) for (i = 0; i < 512; i++) wbuf[s * 512 + i] = patron(lba + s, i, sem); return ms_escribir(lba, n, wbuf); }
/* lo que hay en la tarjeta simulada es el patrón (sem) en [lba, lba+n) */
static int tarjeta_tiene(u32 lba, u32 n, u32 sem) { u32 s, i; for (s = 0; s < n; s++) for (i = 0; i < 512; i++) if (dato(lba + s, i) != patron(lba + s, i, sem)) return 0; return 1; }
static int tarjeta_virgen(u32 lba, u32 n) { u32 s; int h; for (s = 0; s < n; s++) if ((h = w_busca(lba + s, 0)) >= 0 && !wt[h].virgen) return 0; return 1; }
/* cuántos sectores seguidos desde lba tienen el patrón sem */
static u32 prefijo(u32 lba, u32 n, u32 sem) { u32 s; for (s = 0; s < n; s++) if (!tarjeta_tiene(lba + s, 1, sem)) break; return s; }
static int leer_igual(u32 lba, u32 n, u32 sem) { u32 s, i; memset(buf, 0xEE, n * 512); if (ms_leer(lba, n, buf)) return 0; for (s = 0; s < n; s++) for (i = 0; i < 512; i++) if (buf[s * 512 + i] != patron(lba + s, i, sem)) return 0; return 1; }

int main(int argc, char **argv)
{
    const MsInfo *I = ms_info();
    u32 k, n_azar = argc > 1 ? (u32)atoi(argv[1]) : 20000;
    int r;

    caso("normal: entra, se identifica, lee");
    crono(); meter();
    OK(I->estado == MS_LISTA && I->sectores == 131072 && I->generacion == 1, "estado %d sectores %u motivo %s", I->estado, I->sectores, ms_motivo(I->motivo));
    OK(pasado_ms() < 100, "identificar tardo %u ms", pasado_ms());
    OK(leer(0, 1) == 0 && datos_bien(0, 1), "sector 0");
    OK(leer(2048, 64) == 0 && datos_bien(2048, 64), "64 sectores");
    k = I->ordenes; OK(leer(131072 - 200, 200) == 0 && datos_bien(131072 - 200, 200) && I->ordenes - k == 4, "200 sectores al final en 4 ordenes (%u)", I->ordenes - k);
    OK(leer(131072, 1) == MS_E_RANGO && leer(131071, 2) == MS_E_RANGO && leer(0xFFFFFFFFu, 2) == MS_E_RANGO, "fuera de rango");
    OK(leer(5, 0) == 0, "cero sectores");
    OK(I->recuperaciones == 0 && I->e_tiempo == 0 && I->e_crc == 0 && I->e_int == 0, "sin fallos: rec %u tiempo %u crc %u int %u", I->recuperaciones, I->e_tiempo, I->e_crc, I->e_int);

    caso("reposo a los 30 s y despertar");
    meter(); OK(leer(7, 3) == 0, "lee");
    us += 29000000ull; ms_paso(); OK(I->estado == MS_LISTA && tension, "a los 29 s sigue encendida");
    us += 1100000ull; ms_paso(); OK(I->estado == MS_DORMIDA && !tension && I->reposos == 1, "a los 30 s apagada (estado %d, tension %d)", I->estado, tension);
    crono(); OK(leer(100, 8) == 0 && datos_bien(100, 8) && I->estado == MS_LISTA && tension && I->encendidos == 2 && I->generacion == 1, "despierta y lee; misma generacion");
    OK(pasado_ms() < 400, "despertar tardo %u ms", pasado_ms());

    caso("sale dormida; entra otra");
    meter(); us += 31000000ull; ms_paso(); sacar();
    OK(I->estado == MS_FUERA && I->salidas == 1 && leer(0, 1) == MS_E_FUERA, "fuera");
    sectores_tarjeta = 262144; meter(); OK(I->estado == MS_LISTA && I->generacion == 2 && I->sectores == 262144, "otra tarjeta: generacion %u, %u sectores", I->generacion, I->sectores);

    caso("rebote del pin: menos de 20 ms no es una tarjeta");
    pin_hasta = us + 10000; ms_paso();                        /* el pin se ve «dentro» y a los 20 ms ya no */
    OK(I->estado == MS_FUERA && I->entradas == 0 && !tension, "un pulso de 10 ms no es una tarjeta (entradas %u)", I->entradas);
    meter(); pin_hasta = 0; puesta = 0; pin_hasta = us + 5000; ms_paso();     /* y al reves: dentro, un hueco que se cierra… */
    puesta = 1; ms_paso(); OK(I->estado == MS_LISTA && I->salidas == 0, "… y sigue dentro (salidas %u)", I->salidas);

    caso("otra tarjeta al despertar (otra capacidad)");
    meter(); us += 31000000ull; ms_paso(); sectores_tarjeta = 65536;
    r = leer(0, 1); OK(r == MS_E_OTRA && I->estado == MS_MALA && !tension, "da MS_E_OTRA y la apaga (r %d, estado %d, tension %d)", r, I->estado, tension);
    OK(leer(0, 1) == MS_E_OTRA, "y sigue negandose");
    sacar(); meter(); OK(I->estado == MS_LISTA && I->sectores == 65536, "al sacarla y meterla, la nueva vale");

    caso("la tarjeta sale en CUALQUIER transaccion de una lectura");
    for (k = 1; k <= TPC_LEER(130, 3) + 7; k++) {
        unsigned long long antes;
        ms_init(&anf); puesta = 0; tension = 0; us += 1000000; meter();
        if (I->estado != MS_LISTA) { OK(0, "no quedo lista antes de la vuelta %u", k); break; }
        memset(&F, 0, sizeof F); armar(); F.sale_en_tpc = (int)k; antes = us;
        r = leer(1000, 130);
        if (k <= TPC_LEER(130, 3)) OK(r == MS_E_FUERA && I->estado == MS_FUERA && !tension, "TPC %u: r %d estado %d tension %d", k, r, I->estado, tension);
        else OK(r == 0 && datos_bien(1000, 130), "TPC %u (ya habia acabado): r %d", k, r);
        OK(us - antes < 2000000ull, "TPC %u: tardo %llu ms en darse cuenta", k, (us - antes) / 1000);
        memset(&F, 0, sizeof F);
    }
    caso("la tarjeta sale en CUALQUIER espera de INT");
    for (k = 1; k <= 135; k++) {
        ms_init(&anf); puesta = 0; tension = 0; us += 1000000; meter();
        memset(&F, 0, sizeof F); armar(); F.sale_en_espera = (int)k;
        r = leer(1000, 130);
        OK((r == MS_E_FUERA && I->estado == MS_FUERA && !tension) || (r == 0 && datos_bien(1000, 130)), "espera %u: r %d estado %d", k, r, I->estado);
        memset(&F, 0, sizeof F);
    }
    caso("la tarjeta sale mientras se identifica");
    for (k = 1; k <= 12; k++) {
        ms_init(&anf); puesta = 0; tension = 0; us += 1000000;
        memset(&F, 0, sizeof F); armar(); F.sale_en_tpc = (int)k;
        meter();
        OK((I->estado == MS_FUERA && !tension) || I->estado == MS_LISTA, "TPC %u de la identificacion: estado %d tension %d", k, I->estado, tension);
        memset(&F, 0, sizeof F);
    }

    caso("CRC en un sector: apagar, encender y a la segunda sale");
    meter(); armar(); F.crc_en_dato = 5;
    crono(); r = leer(300, 40);
    OK(r == 0 && datos_bien(300, 40), "r %d", r);
    OK(I->recuperaciones == 1 && I->e_crc == 1 && I->por_orden == 4 && I->encendidos == 2, "una recuperacion, y a 4 sectores por orden (rec %u crc %u por_orden %u)", I->recuperaciones, I->e_crc, I->por_orden);
    OK(pasado_ms() < 700, "tardo %u ms", pasado_ms());

    caso("CRC siempre: 3 intentos, error claro, y no se cuelga");
    meter(); F.crc_siempre = 1;
    crono(); r = leer(300, 40);
    OK(r == MS_E_LECTURA && I->estado == MS_DORMIDA && !tension && I->recuperaciones == 3, "r %d estado %d rec %u", r, I->estado, I->recuperaciones);
    OK(pasado_ms() < 2000, "tardo %u ms", pasado_ms());
    F.crc_siempre = 0; OK(leer(300, 40) == 0 && datos_bien(300, 40) && I->estado == MS_LISTA, "arreglada la tarjeta, la siguiente lectura sale");

    caso("tiempo agotado en CUALQUIER transaccion");
    for (k = 1; k <= 80; k++) {
        ms_init(&anf); puesta = 0; tension = 0; us += 1000000; meter();
        memset(&F, 0, sizeof F); armar(); F.tiempo_en_tpc = (int)k;
        crono(); r = leer(40, 70);
        OK(r == 0 && datos_bien(40, 70), "TPC %u: r %d (%s)", k, r, ms_motivo(r));
        OK(pasado_ms() < 2500, "TPC %u: tardo %u ms", k, pasado_ms());
        memset(&F, 0, sizeof F);
    }
    caso("tiempo agotado en CUALQUIER transaccion de la identificacion");
    for (k = 1; k <= 12; k++) {
        ms_init(&anf); puesta = 0; tension = 0; us += 1000000;
        memset(&F, 0, sizeof F); armar(); F.tiempo_en_tpc = (int)k;
        meter();
        OK(I->estado == MS_LISTA, "TPC %u: estado %d motivo %s", k, I->estado, ms_motivo(I->motivo));
        memset(&F, 0, sizeof F);
    }

    caso("INT que no llega (el estado si): se acepta, despacio");
    meter(); armar(); F.pierde_int = 3;
    crono(); r = leer(10, 8);
#ifdef MS_SOLO_SERIE
    OK(r == 0 && datos_bien(10, 8) && I->recuperaciones == 0, "r %d rec %u", r, I->recuperaciones);   /* (M2) en serie se pregunta: la INT perdida ni se nota */
#else
    OK(r == 0 && datos_bien(10, 8) && I->e_int == 1 && I->recuperaciones == 0, "r %d e_int %u rec %u", r, I->e_int, I->recuperaciones);
#endif
#ifdef MS_SOLO_SERIE
    OK(pasado_ms() < 100, "tardo %u ms (en serie se pregunta con GET_INT, sin esperar los 1500 de la INT)", pasado_ms());
#else
    OK(pasado_ms() >= 1500 && pasado_ms() < 1700, "tardo %u ms (el tope de 1500 de una INT)", pasado_ms());
#endif

    caso("la tarjeta se queda muda a media orden: recuperacion");
    meter(); armar(); F.cuelga_en_sector = 6;
    crono(); r = leer(10, 20);
    OK(r == 0 && datos_bien(10, 20) && I->recuperaciones == 1, "r %d rec %u", r, I->recuperaciones);
    OK(pasado_ms() < 2500, "tardo %u ms", pasado_ms());
    caso("… y si se queda muda SIEMPRE: error a los 3 intentos");
    meter(); F.cuelga_siempre = 1;
    crono(); r = leer(10, 20);
    OK(r == MS_E_LECTURA && !tension, "r %d", r);
    OK(pasado_ms() < 7000, "tardo %u ms", pasado_ms());

    caso("la tarjeta dice ERR en un sector");
    meter(); armar(); F.err_en_sector = 4;
    r = leer(10, 20); OK(r == MS_E_LECTURA && I->e_estado >= 3, "ERR persistente: r %d e_estado %u", r, I->e_estado);
    F.err_en_sector = 0; OK(leer(10, 20) == 0 && datos_bien(10, 20), "sin el fallo, sale");

    caso("una tarjeta que no admite mas de 4 sectores por orden");
    meter(); F.max_por_orden = 4;
    r = leer(0, 100); OK(r == 0 && datos_bien(0, 100) && I->por_orden == 4 && I->recuperaciones == 1, "cae a 4 por orden y lee (r %d por_orden %u rec %u)", r, I->por_orden, I->recuperaciones);

    caso("la INT del paso a paralelo llega tarde (tras la primera orden)");
    F.int_tardia = 1; meter();
    OK(I->estado == MS_LISTA && I->sectores == 131072, "se identifica igual (estado %d, %s)", I->estado, ms_motivo(I->motivo));
    OK(leer(3, 5) == 0 && datos_bien(3, 5), "y lee");

#ifndef MS_SOLO_SERIE
    /* (M2) EL RESPALDO A 1 BIT: en el W580 real la M2 dio «la tarjeta no pasa a 4 bits». Que falle el paso, en cualquiera de sus dos
     * tramos, apaga, reintenta EN SERIE y la tarjeta queda lista; desde ahí todo va en serie (lee, escribe, vacía) y no se vuelve a pedir
     * 4 bits ni con otra tarjeta. */
    for (k = 1; k <= 2; k++) {
        caso(k == 1 ? "(M2) el WRITE_REG del paso a 4 bits no contesta: cae a 1 bit" : "(M2) en paralelo el anfitrion no enseña NUNCA el estado de la tarjeta: cae a 1 bit en los atributos");
        F.no_paralelo = (int)k; crono(); meter();
        OK(I->estado == MS_LISTA && I->sectores == 131072 && I->serie == 1 && I->paralelo_fallos == 1, "lista en serie (estado %d, %s, serie %u, fallos %u)", I->estado, ms_motivo(I->motivo), I->serie, I->paralelo_fallos);
        OK(I->par_fase == (k == 1 ? MS_PAR_WRITE_REG : MS_PAR_ATRIBUTOS) && I->paralelo_bien == 0, "(M2 ag) el diagnostico dice el tramo: %s (t %u)", ms_par_fase(I->par_fase), I->par_t);
        OK(I->encendidos == k + 1 && I->recuperaciones == k && !h_par && !c_par, "apagados y encendidos (enc %u rec %u), el anfitrion y la tarjeta en serie", I->encendidos, I->recuperaciones);
        OK(pasado_ms() < (k == 1 ? 2500u : 4500u), "identificar tardo %u ms", pasado_ms());   /* (M2 ag) k = 2: dos tropiezos en los atributos antes de caer */
        r = leer(2048, 64); OK(r == 0 && datos_bien(2048, 64), "lee 64 sectores en serie (r %d)", r);
        OK(leer(131072 - 200, 200) == 0 && datos_bien(131072 - 200, 200), "200 al final");
        r = escribir(3000, 40, 21); OK(r == 0 && leer_igual(3000, 40, 21) && ms_vaciar() == 0, "escribe, relee y vacia en serie (r %d)", r);
        us += 31000000ull; ms_paso(); OK(I->estado == MS_DORMIDA, "duerme");
        OK(leer(7, 3) == 0 && datos_bien(7, 3) && I->paralelo_fallos == 1 && I->recuperaciones == k, "despierta en serie, sin volver a pedir 4 bits (fallos %u)", I->paralelo_fallos);
        sacar(); F.no_paralelo = 0; meter();
        OK(I->estado == MS_LISTA && I->serie == 1 && I->paralelo_fallos == 1 && !h_par, "otra tarjeta: sigue en serie (es el anfitrion)");
        OK(leer(100, 8) == 0 && datos_bien(100, 8), "y lee");
        OK(I->int_serie == MS_INT_SERIE_SI, "aprende que en serie SÍ hay MSINT (int_serie %u)", I->int_serie);
    }
    /* (M2 ag, 2026-10-21) LA CAUSA [inferida] DE LA v3 Y SU ARREGLO: en el anfitrión real los bits 3..0 de +8 se cargan con la INT. El
     * controlador viejo exigía CED en +8 nada más pasar a paralelo -> 1500 ms -> «no pasa a 4 bits». Ahora: la INT como el original. */
    caso("(M2 ag) +8 en paralelo solo cambia con la INT (anfitrion real): SIGUE en 4 bits");
    F.estado_retenido = 1; crono(); meter();
    OK(I->estado == MS_LISTA && I->serie == 0 && I->paralelo_fallos == 0 && I->paralelo_bien == 1 && h_par, "lista en 4 bits (estado %d, %s, serie %u, fallos %u, fase %s)", I->estado, ms_motivo(I->motivo), I->serie, I->paralelo_fallos, ms_par_fase(I->par_fase));
    OK(I->par_fin == MS_PAR_FIN_INT && I->int_paso == MS_INT_SERIE_SI && pasado_ms() < 100, "la INT del encendido acaba la espera (fin %u, %u ms en total)", I->par_fin, pasado_ms());
    OK(I->par_reg[2] == 1 && I->par_reg[4] == 0 && I->par_reg[5] == 0 && I->par_reg[6] == 0, "registros leidos en 4 bits: tipo %u cat %u clase %u r10 %u", I->par_reg[2], I->par_reg[4], I->par_reg[5], I->par_reg[6]);
    r = leer(2048, 64); OK(r == 0 && datos_bien(2048, 64), "lee 64 sectores en 4 bits (r %d)", r);
    r = escribir(3000, 40, 31); OK(r == 0 && leer_igual(3000, 40, 31) && ms_vaciar() == 0, "escribe, relee y vacia en 4 bits (r %d)", r);
    us += 31000000ull; ms_paso(); OK(I->estado == MS_DORMIDA, "duerme");
    OK(leer(7, 3) == 0 && datos_bien(7, 3) && I->serie == 0 && I->paralelo_bien == 2, "despierta en 4 bits");

    caso("(M2 ag) ... y tampoco llega INT tras el paso: espera el tope del original UNA vez y aprende");
    F.estado_retenido = 1; F.sin_msint_serie = 1; crono(); meter();
    OK(I->estado == MS_LISTA && I->serie == 0 && I->paralelo_fallos == 0, "lista en 4 bits (estado %d, %s, serie %u)", I->estado, ms_motivo(I->motivo), I->serie);
    OK(I->par_fin == MS_PAR_FIN_TOPE && I->int_paso == MS_INT_SERIE_NO && pasado_ms() >= 1500 && pasado_ms() < 1900, "espero el tope (fin %u, int_paso %u, %u ms)", I->par_fin, I->int_paso, pasado_ms());
    r = leer(500, 16); OK(r == 0 && datos_bien(500, 16), "lee (r %d)", r);
    us += 31000000ull; ms_paso(); crono();
    OK(leer(7, 3) == 0 && datos_bien(7, 3) && pasado_ms() < 400 && I->par_ms <= MS_PASO_CORTO_MS + 8, "al despertar, espera corta (%u ms en total, espera %u ms)", pasado_ms(), I->par_ms);

    caso("(M2 ag) estado retenido y la INT del paso llega con la primera orden: un tropiezo y SIGUE en 4 bits");
    F.estado_retenido = 1; F.int_tardia = 1; crono(); meter();       /* sin INT pendiente al pasar: +8 a 0 hasta la primera orden, que tropieza */
    OK(I->estado == MS_LISTA && I->serie == 0 && I->recuperaciones == 1 && I->paralelo_bien == 1, "lista en 4 bits tras un tropiezo (estado %d, %s, serie %u, rec %u)", I->estado, ms_motivo(I->motivo), I->serie, I->recuperaciones);
    OK(leer(3, 5) == 0 && datos_bien(3, 5), "y lee");

    caso("(M2 ag) en paralelo no llega la INT de la tarjeta: los atributos salen por el tope y cae a 1 bit");
    F.sin_int_par = 1; crono(); meter();
    OK(I->estado == MS_LISTA && I->serie == 1 && I->par_fase == MS_PAR_SIN_INT && I->paralelo_fallos == 1, "lista en serie (estado %d, %s, fase %s)", I->estado, ms_motivo(I->motivo), ms_par_fase(I->par_fase));
    OK(pasado_ms() < 5000, "tardo %u ms", pasado_ms());
    crono(); r = leer(100, 64); OK(r == 0 && datos_bien(100, 64) && pasado_ms() < 500, "lee 64 sectores en serie, deprisa (r %d, %u ms)", r, pasado_ms());

    caso("(M2 ag) el bus de 4 bits da CRC en todo: cae a 1 bit en el READ_REG");
    F.bus4 = 1; crono(); meter();
    OK(I->estado == MS_LISTA && I->serie == 1 && I->par_fase == MS_PAR_LEER_REG && I->par_t == MS_T_CRC && I->paralelo_fallos == 1, "lista en serie (estado %d, %s, fase %s, t %u)", I->estado, ms_motivo(I->motivo), ms_par_fase(I->par_fase), I->par_t);
    OK(pasado_ms() < 1000, "tardo %u ms", pasado_ms());
    r = leer(100, 30); OK(r == 0 && datos_bien(100, 30), "lee en serie (r %d)", r);
    r = escribir(4000, 10, 32); OK(r == 0 && leer_igual(4000, 10, 32), "escribe en serie (r %d)", r);

    caso("(M2 ag) datos rotos en 4 bits SIN CRC (DAT1..3 pegados): cae a 1 bit por los registros");
    F.bus4 = 2; crono(); meter();
    OK(I->estado == MS_LISTA && I->serie == 1 && I->par_fase == MS_PAR_REG_DISTINTOS && I->par_reg[2] == 0xEF, "lista en serie (estado %d, %s, fase %s, tipo en 4b %02X)", I->estado, ms_motivo(I->motivo), ms_par_fase(I->par_fase), I->par_reg[2]);
    r = leer(100, 30); OK(r == 0 && datos_bien(100, 30), "lee en serie, datos buenos (r %d)", r);

    caso("(M2) cae a 1 bit Y en serie no hay MSINT: se pregunta con GET_INT por tramos");
    F.no_paralelo = 2; F.sin_msint_serie = 1; F.lenta_us = 40000; meter();
    OK(I->estado == MS_LISTA && I->serie == 1, "lista en serie (estado %d, %s)", I->estado, ms_motivo(I->motivo));
    k = I->e_int; crono(); r = leer(500, 64);
    OK(r == 0 && datos_bien(500, 64) && I->e_int > k && I->int_serie == MS_INT_SERIE_NO, "lee 64 sectores sin una sola INT (r %d, halladas sin INT %u, int_serie %u)", r, I->e_int - k, I->int_serie);
    OK(pasado_ms() < 64 * 60, "tardo %u ms (sin esperar los 1500 de cada INT)", pasado_ms());
    r = escribir(800, 20, 22); OK(r == 0 && leer_igual(800, 20, 22), "y escribe (r %d)", r);
#else
    caso("(M2) en serie sin MSINT: se pregunta con GET_INT por tramos");
    F.sin_msint_serie = 1; F.lenta_us = 40000; meter();
    OK(I->estado == MS_LISTA && I->serie == 1, "lista (estado %d, %s)", I->estado, ms_motivo(I->motivo));
    k = I->e_int; crono(); r = leer(500, 64);
    OK(r == 0 && datos_bien(500, 64) && I->e_int > k && I->int_serie == MS_INT_SERIE_NO, "lee 64 sectores sin una sola INT (r %d, halladas sin INT %u, int_serie %u)", r, I->e_int - k, I->int_serie);
    OK(pasado_ms() < 64 * 60, "tardo %u ms", pasado_ms());
    r = escribir(800, 20, 22); OK(r == 0 && leer_igual(800, 20, 22), "y escribe (r %d)", r);
#endif

    caso("(M2) en serie, tarjeta de 1 ms por respuesta y sin MSINT: aprende que no hay INT y pregunta cada ms");
#ifndef MS_SOLO_SERIE
    F.no_paralelo = 1;
#endif
    F.sin_msint_serie = 1; F.lenta_us = 1000; meter();
    OK(I->estado == MS_LISTA, "lista (estado %d)", I->estado);
    crono(); r = leer(900, 64);
    OK(r == 0 && datos_bien(900, 64) && I->int_serie == MS_INT_SERIE_NO, "lee 64 sectores y aprende que no hay INT en serie (r %d, int_serie %u)", r, I->int_serie);
    OK(pasado_ms() < 64 * 4, "tardo %u ms (sin tramos de %u ms por sector)", pasado_ms(), MS_TRAMO_SERIE_MS);
    r = escribir(1200, 16, 23); OK(r == 0 && leer_igual(1200, 16, 23) && ms_vaciar() == 0, "escribe y vacía (r %d)", r);

    caso("tarjeta muda");
    F.muda = 1; crono(); meter();
    OK(I->estado == MS_MALA && I->motivo == MS_E_MUDA && !tension && I->encendidos == 3, "MALA/MUDA tras 3 encendidos (estado %d motivo %d enc %u)", I->estado, I->motivo, I->encendidos);
    OK(pasado_ms() < 6000, "tardo %u ms", pasado_ms());
    OK(leer(0, 1) == MS_E_MUDA, "leer dice por que");
    crono(); ms_paso(); ms_paso(); OK(pasado_ms() == 0 && tpcs && I->encendidos == 3, "y no lo reintenta sola en cada vuelta");
    F.muda = 0; sacar(); meter(); OK(I->estado == MS_LISTA, "sacada y metida (ya contesta): lista");

    caso("no es PRO");
    F.tipo = 0xFF; meter(); OK(I->estado == MS_MALA && I->motivo == MS_E_NO_PRO && I->encendidos == 1 && !tension, "MALA/NO_PRO sin reintentos");
    caso("clase 5");
    F.clase = 5; meter(); OK(I->estado == MS_MALA && I->motivo == MS_E_NO_PRO, "MALA/NO_PRO");
    caso("clase 1: PRO protegida");
    F.clase = 1; meter(); OK(I->estado == MS_LISTA && I->protegida && leer(1, 1) == 0, "lista y marcada protegida");

    caso("el CED tarda 3 s");
    F.ced_ms = 3000; crono(); meter();
    OK(I->estado == MS_LISTA, "lista (estado %d)", I->estado);
    OK(pasado_ms() > 2900 && pasado_ms() < 3300, "tardo %u ms", pasado_ms());
    OK(vueltas > 500 && peor_vuelta_ms <= 40, "(I1) POR PASOS: %u vueltas del bucle, la más larga %u ms (antes, una sola de 3 s)", vueltas, peor_vuelta_ms);
    caso("el CED no llega nunca");
    F.ced_nunca = 1; crono(); meter();
    OK(I->estado == MS_MALA && I->motivo == MS_E_CED && !tension, "MALA/CED");
    OK(pasado_ms() > 10000 && pasado_ms() < 11500, "tardo %u ms (101 x 104 ms)", pasado_ms());
    OK(vueltas > 2000 && peor_vuelta_ms <= 40, "(I1) POR PASOS: %u vueltas, la más larga %u ms (antes, una sola de 10,5 s)", vueltas, peor_vuelta_ms);
    caso("(I1) sacarla mientras se espera su CED");
    F.ced_ms = 3000; puesta = 1; ms_paso();
    OK(I->estado == MS_PREPARANDO && tension, "preparandola (estado %d), con tension", I->estado);
    { int v; for (v = 0; v < 100; v++) { us += 4000; ms_paso(); } }
    sacar();
    OK(I->estado == MS_FUERA && !tension && I->salidas == 1, "fuera y sin tension (estado %d, tension %d)", I->estado, tension);
    OK(leer(0, 1) == MS_E_FUERA, "y leer dice FUERA");
    caso("(I1) leer mientras se espera su CED");
    F.ced_ms = 3000; puesta = 1; ms_paso();
    OK(leer(0, 1) == MS_E_CED && I->estado == MS_PREPARANDO, "leer da «no lista» sin mandar nada y sigue preparándola");

    caso("atributos sin la firma A5C3");
    F.firma_mal = 1; meter(); OK(I->estado == MS_MALA && I->motivo == MS_E_ATRIBUTOS && I->encendidos == 3 && !tension, "MALA/ATRIBUTOS tras 3 encendidos");
    caso("atributos sin informacion del sistema");
    F.sin_info = 1; meter(); OK(I->estado == MS_MALA && I->motivo == MS_E_SIN_INFO && I->encendidos == 1, "MALA/SIN_INFO");
    caso("informacion del sistema fuera del primer sector");
    F.info_lejos = 1; meter(); OK(I->estado == MS_MALA && I->motivo == MS_E_INFO_LEJOS, "MALA/INFO_LEJOS (motivo %d)", I->motivo);
    caso("capacidad cero");
    F.capacidad_cero = 1; meter(); OK(I->estado == MS_MALA && I->motivo == MS_E_CAPACIDAD, "MALA/CAPACIDAD");

    caso("el PMU no contesta: no se manda ni un TPC");
    F.pmu_mudo = 1; meter(); OK(I->estado == MS_MALA && I->motivo == MS_E_PMU && tpcs == 0 && !tension, "MALA/PMU, %u TPC", tpcs);
    caso("el anfitrion no sale del reset");
    F.rst_atascado = 1; meter(); OK(I->estado == MS_MALA && I->motivo == MS_E_ANFITRION && tpcs == 0, "MALA/ANFITRION");

    /* ==== (MSW) ESCRITURA ==== */
    caso("MSW escribir, releer y comparar");
    meter();
    k = I->ordenes_w; r = escribir(100, 1, 1); OK(r == 0 && tarjeta_tiene(100, 1, 1) && leer_igual(100, 1, 1) && I->ordenes_w - k == 1, "1 sector (r %d)", r);
    k = I->ordenes_w; r = escribir(2000, 100, 2); OK(r == 0 && tarjeta_tiene(2000, 100, 2) && leer_igual(2000, 100, 2), "100 sectores (r %d)", r);
    OK(I->ordenes_w - k == (100 + MS_ESCRIBE_POR_ORDEN - 1) / MS_ESCRIBE_POR_ORDEN, "en %u ordenes", I->ordenes_w - k);
    OK(tarjeta_virgen(1999, 1) && tarjeta_virgen(2100, 1), "ni un sector de mas a los lados");
    r = escribir(2000, 100, 3); OK(r == 0 && leer_igual(2000, 100, 3), "reescribir encima");
    OK(I->tpc_reintentos == 0 && I->recuperaciones == 0 && I->e_escritura == 0, "sin reintentos ni recuperaciones");
    OK(ms_vaciar() == 0, "vaciar");
    k = latidos; r = escribir(5000, 64, 30); OK(r == 0 && latidos - k >= 64, "un latido al perro por sector escrito (%u en 64)", latidos - k);
    caso("MSW el ultimo sector, y no pasar del final");
    meter();
    r = escribir(131071, 1, 4); OK(r == 0 && tarjeta_tiene(131071, 1, 4) && leer_igual(131071, 1, 4), "ultimo sector (r %d)", r);
    r = escribir(131072 - 40, 40, 5); OK(r == 0 && leer_igual(131072 - 40, 40, 5), "los 40 ultimos (r %d)", r);
    k = ordenes_mandadas;
    OK(escribir(131072, 1, 6) == MS_E_RANGO && escribir(131071, 2, 6) == MS_E_RANGO && escribir(0xFFFFFFFFu, 2, 6) == MS_E_RANGO && escribir(100, 0xFFFFFFFFu, 6) == MS_E_RANGO, "fuera de rango");
    OK(ordenes_mandadas == k && tarjeta_tiene(131071, 1, 5), "sin mandar nada a la tarjeta");
    OK(escribir(5, 0, 6) == 0 && ordenes_mandadas == k, "cero sectores");
    caso("MSW protegida (STATUS0 bit 0): no se manda ninguna orden");
    F.wp = 1; meter(); k = ordenes_mandadas;
    OK(I->estado == MS_LISTA && I->protegida, "lista y protegida");
    r = escribir(10, 4, 7); OK(r == MS_E_PROTEGIDA && ordenes_mandadas == k && tarjeta_virgen(10, 4), "r %d", r);
    OK(leer(10, 4) == 0 && datos_bien(10, 4), "y se lee");
    caso("MSW protegida (clase 1)");
    F.clase = 1; meter(); k = ordenes_mandadas; r = escribir(10, 4, 7); OK(r == MS_E_PROTEGIDA && ordenes_mandadas == k, "r %d", r);
    caso("MSW dormida: escribir la despierta");
    meter(); us += 31000000ull; ms_paso(); OK(I->estado == MS_DORMIDA && !tension, "dormida");
    r = escribir(500, 8, 8); OK(r == 0 && leer_igual(500, 8, 8) && I->estado == MS_LISTA && I->generacion == 1, "despierta y escribe (r %d)", r);
    caso("MSW la tarjeta sale en CUALQUIER transaccion de una escritura de 70 sectores");
    for (k = 1; k <= TPC_LEER(70, 3) + 7; k++) {
        unsigned long long antes; u32 p;
        ms_init(&anf); puesta = 0; tension = 0; us += 1000000; w_vacia(); meter();
        memset(&F, 0, sizeof F); armar(); F.sale_en_tpc = (int)k; antes = us;
        r = escribir(3000, 70, 9);
        p = prefijo(3000, 70, 9);
        if (k <= TPC_LEER(70, 3)) OK(r == MS_E_FUERA && I->estado == MS_FUERA && !tension, "TPC %u: r %d estado %d tension %d", k, r, I->estado, tension);
        else OK(r == 0 && p == 70, "TPC %u (ya habia acabado): r %d", k, r);
        OK(tarjeta_virgen(3000 + p, 70 - p) && tarjeta_virgen(2999, 1) && tarjeta_virgen(3070, 1), "TPC %u: escribio algo que no es un prefijo del tramo (%u bien)", k, p);
        OK(us - antes < 2000000ull, "TPC %u: tardo %llu ms en darse cuenta", k, (us - antes) / 1000);
        memset(&F, 0, sizeof F);
    }
    caso("MSW la tarjeta sale en CUALQUIER espera de INT de una escritura");
    for (k = 1; k <= 75; k++) {
        u32 p;
        ms_init(&anf); puesta = 0; tension = 0; us += 1000000; w_vacia(); meter();
        memset(&F, 0, sizeof F); armar(); F.sale_en_espera = (int)k;
        r = escribir(3000, 70, 9); p = prefijo(3000, 70, 9);
        OK((r == MS_E_FUERA && I->estado == MS_FUERA && !tension) || (r == 0 && p == 70), "espera %u: r %d estado %d", k, r, I->estado);
        OK(tarjeta_virgen(3000 + p, 70 - p), "espera %u: no es un prefijo", k);
        memset(&F, 0, sizeof F);
    }
    caso("MSW tiempo agotado en CUALQUIER transaccion: error limpio, SIN reintento, y la siguiente sale");
    for (k = 1; k <= 75; k++) {
        u32 p, rr;
        ms_init(&anf); puesta = 0; tension = 0; us += 1000000; w_vacia(); meter();
        memset(&F, 0, sizeof F); armar(); F.tiempo_en_tpc = (int)k; rr = I->tpc_reintentos;
        crono(); r = escribir(40, 70, 10); p = prefijo(40, 70, 10);
        OK(r == MS_E_ESCRITURA || (r == 0 && p == 70), "TPC %u: r %d (%s)", k, r, ms_motivo(r));
        /* (28-oct) tras el error la orden se CIERRA (CED) y la tarjeta sigue lista y encendida; antes se apagaba con la orden abierta */
        OK(r != MS_E_ESCRITURA || (I->estado == MS_LISTA && tension), "TPC %u: tras el error, la orden cerrada y la tarjeta lista (estado %d, tension %d)", k, I->estado, tension);
        OK(I->tpc_reintentos == rr, "TPC %u: %u TPC repetidos a escondidas", k, I->tpc_reintentos - rr);
        {   u32 fin = 40 + (p / MS_ESCRIBE_POR_ORDEN + 1) * MS_ESCRIBE_POR_ORDEN; if (fin > 110) fin = 110;   /* ni una orden después de la fallada */
            OK(tarjeta_virgen(fin, 110 - fin) && tarjeta_virgen(39, 1) && tarjeta_virgen(110, 1), "TPC %u: escribio despues de la orden fallada o fuera del tramo", k); }
        OK(pasado_ms() < 2500, "TPC %u: tardo %u ms", k, pasado_ms());
        memset(&F, 0, sizeof F);
        r = escribir(40, 70, 11); OK(r == 0 && leer_igual(40, 70, 11), "TPC %u: repetida ENTERA, sale (r %d)", k, r);
    }
    caso("MSW CRC en el dato de un sector: error, y la orden se CIERRA dandole lo que pide (28-oct)");
    meter(); armar(); F.w_crc_en = 5; k = I->tpc_reintentos;
    r = escribir(700, 20, 12);
    OK(r == MS_E_ESCRITURA && I->tpc_reintentos == k && I->e_escritura == 1 && I->cerradas_w >= 1 && tension && I->estado == MS_LISTA, "r %d, cerradas %u, tension %d", r, I->cerradas_w, tension);
    OK(tarjeta_virgen(699, 1) && tarjeta_virgen(720, 1), "nada fuera del tramo");
    F.w_crc_en = 0; r = escribir(700, 20, 12); OK(r == 0 && leer_igual(700, 20, 12), "repetida, sale");
    caso("MSW la tarjeta dice ERR al tomar un sector");
    meter(); armar(); F.w_err_en = 3;
    r = escribir(800, 10, 13); OK(r == MS_E_ESCRITURA && prefijo(800, 10, 13) == 2 && tarjeta_virgen(802, 8) && I->e_estado >= 1, "r %d", r);
    caso("MSW la tarjeta se queda muda a media escritura");
    meter(); armar(); F.w_cuelga_en = 6;
    crono(); r = escribir(900, 20, 14); OK(r == MS_E_ESCRITURA && !tension && prefijo(900, 20, 14) == 6 && tarjeta_virgen(906, 14), "r %d", r);
    OK(pasado_ms() < 7000, "tardo %u ms", pasado_ms());
    r = escribir(900, 20, 14); OK(r == 0 && leer_igual(900, 20, 14), "repetida, sale (r %d)", r);
    caso("(28-oct) un fallo a media escritura NO corta la tension con la orden abierta");
    {
        u32 c0 = cortes_w;
        meter(); armar(); F.w_crc_en = 5; r = escribir(1700, 20, 21);
        OK(r == MS_E_ESCRITURA && cortes_w == c0, "CRC en el dato: r %d, %u cortes con la orden abierta", r, cortes_w - c0);
        OK(tension && I->estado == MS_LISTA, "CRC en el dato: la tarjeta sigue encendida y lista (tension %d, estado %d)", tension, I->estado);
        memset(&F, 0, sizeof F); c0 = cortes_w;
        meter(); armar(); F.w_err_en = 3; r = escribir(1800, 10, 22);
        OK(r == MS_E_ESCRITURA && cortes_w == c0 && tension, "ERR al tomar un sector: r %d, %u cortes, tension %d", r, cortes_w - c0, tension);
        memset(&F, 0, sizeof F);
        for (k = 1; k <= 75; k++) {
            ms_init(&anf); puesta = 0; tension = 0; us += 1000000; w_vacia(); meter();
            memset(&F, 0, sizeof F); armar(); F.tiempo_en_tpc = (int)k; c0 = cortes_w;
            r = escribir(40, 70, 23);
            OK(cortes_w == c0, "tiempo en el TPC %u: r %d, %u cortes con la orden abierta", k, r, cortes_w - c0);
            OK(tarjeta_virgen(39, 1) && tarjeta_virgen(110, 1), "tiempo en el TPC %u: escribio FUERA del tramo", k);
            memset(&F, 0, sizeof F);
        }
        meter(); armar(); F.w_cuelga_en = 6; c0 = cortes_w;
        r = escribir(1900, 20, 24); OK(r == MS_E_ESCRITURA && !tension, "muda a media escritura: no hay otra que apagar (r %d, tension %d)", r, tension);
        printf("  (28-oct) cortes con la orden de escritura abierta hasta aqui: %u con la tarjeta contestando, %u con ella muda (inevitables)\n", cortes_w, cortes_w_muda);
        memset(&F, 0, sizeof F);
    }
    caso("(28-oct) MODELO de la M2: SIN zona de cierre, lo ultimo escrito SE PIERDE al dormirse (lo medido en el aparato)");
    {
        u32 c0, z[6], perd;
        modelo_ftl = 1; meter(); c0 = I->cortes_sin_cierre;
        for (k = 0; k < 4; k++) OK(escribir((10 + k) * 512 + 3, 8, 40 + k) == 0, "escribe el bloque %u", 10 + k);
        OK(ms_cierre_pendiente(), "queda pendiente de cerrar");
        us += 31000000ull; ms_paso(); OK(I->estado == MS_DORMIDA, "duerme");
        for (k = 0, perd = 0; k < 4; k++) if (!tarjeta_tiene((10 + k) * 512 + 3, 8, 40 + k)) perd++;
        OK(perd == 4 && I->cortes_sin_cierre == c0 + 1, "el modelo pierde los 4 bloques (%u) y se cuenta el corte sin cierre (%u)", perd, I->cortes_sin_cierre - c0);
        caso("(28-oct) con zona de cierre: tras 1 s sin escribir se cierra POR PASOS y al dormirse NO se pierde nada");
        modelo_ftl = 1; meter();
        for (k = 0; k < 6; k++) z[k] = (100 + k) * 512 + 7;
        ms_cierre_zona(z, 6, NULL);
        for (k = 0; k < 4; k++) OK(escribir((10 + k) * 512 + 3, 8, 50 + k) == 0, "escribe el bloque %u", 10 + k);
        c0 = I->cierres;
        us += 1100000ull;
        for (k = 0; k < 20 && ms_cierre_pendiente(); k++) { paso_medido(); us += 4000; }
        OK(!ms_cierre_pendiente() && I->cierres == c0 + 1 && I->cierre_sectores == 6 && k == 6, "cerrado en %u vueltas (cierres %u, sectores %u)", k, I->cierres - c0, I->cierre_sectores);
        us += 31000000ull; ms_paso(); OK(I->estado == MS_DORMIDA, "duerme");
        for (k = 0, perd = 0; k < 4; k++) if (!tarjeta_tiene((10 + k) * 512 + 3, 8, 50 + k)) perd++;
        OK(perd == 0, "%u bloques perdidos", perd);
        OK(leer(10 * 512 + 3, 8) == 0 && datos_bien(10 * 512 + 3, 8) && tarjeta_tiene(10 * 512 + 3, 8, 50), "despierta y lee lo escrito");
        caso("(28-oct) con zona: apagar YA justo tras escribir (sin esperar el segundo): cierra entero antes");
        modelo_ftl = 1; meter(); ms_cierre_zona(z, 6, NULL); c0 = I->cierres;
        for (k = 0; k < 4; k++) escribir((20 + k) * 512, 4, 60 + k);
        ms_apagar_ya();
        for (k = 0, perd = 0; k < 4; k++) if (!tarjeta_tiene((20 + k) * 512, 4, 60 + k)) perd++;
        OK(perd == 0 && I->cierres == c0 + 1 && !tension, "%u perdidos, cierres %u", perd, I->cierres - c0);
        caso("(28-oct) con zona: un fallo de LECTURA tras escribir (apagar para recuperarse) cierra antes de quitar la tension");
        modelo_ftl = 1; meter(); ms_cierre_zona(z, 6, NULL); c0 = I->cierres;
        for (k = 0; k < 4; k++) escribir((30 + k) * 512, 4, 70 + k);
        armar(); F.crc_siempre = 1; r = leer(500, 4); F.crc_siempre = 0;
        for (k = 0, perd = 0; k < 4; k++) if (!tarjeta_tiene((30 + k) * 512, 4, 70 + k)) perd++;
        OK(r != 0 && perd == 0 && I->cierres == c0 + 1, "lectura r %d; %u perdidos, cierres %u", r, perd, I->cierres - c0);
        caso("(28-oct) la zona es de ESA tarjeta: al sacarla y meter otra se olvida");
        modelo_ftl = 1; meter(); ms_cierre_zona(z, 6, NULL); sacar(); meter(); c0 = I->cortes_sin_cierre; k = I->cierre_sectores;
        escribir(40 * 512, 4, 80); us += 31000000ull; ms_paso();
        OK(I->cierre_sectores == k && I->cortes_sin_cierre == c0 + 1, "sin zona: no escribe en la de la otra (sectores %u) y cuenta el corte", I->cierre_sectores - k);
        printf("  (28-oct) modelo de la M2: %u sectores volvieron atras en los cortes\n", ftl_perdidos);
    }
    caso("MSW sacarla y meterla entre escrituras");
    meter(); OK(escribir(50, 3, 15) == 0, "escribe"); sacar(); OK(escribir(50, 3, 16) == MS_E_FUERA && tarjeta_tiene(50, 3, 15), "fuera: no escribe");
    meter(); OK(escribir(50, 3, 16) == 0 && leer_igual(50, 3, 16), "metida otra vez, escribe");

    caso("AZAR: fallos, sacadas y metidas al tuntun");
    {
        u32 bien = 0, mal = 0, fuera = 0, datos_malos = 0, largas = 0, wbien = 0, wmal = 0; unsigned long long peor = 0;
        azar_s = 580;
        for (k = 0; k < n_azar; k++) {
            u32 q = azar() % 100, lba, n; unsigned long long antes;
            memset(&F, 0, sizeof F); armar();
            if (q < 6) { sacar(); continue; }
            if (q < 14) { if (!puesta) { sectores_tarjeta = 4096u << (azar() % 6); meter(); } continue; }
            if (q < 18) { us += 31000000ull; ms_paso(); continue; }
            if (q < 30) F.tiempo_en_tpc = 1 + (int)(azar() % 40);
            else if (q < 40) F.crc_en_dato = 1 + (int)(azar() % 30);
            else if (q < 46) F.pierde_int = 1 + (int)(azar() % 30);
            else if (q < 52) F.cuelga_en_sector = 1 + (int)(azar() % 30);
            else if (q < 58) F.sale_en_tpc = 1 + (int)(azar() % 60);
            else if (q < 62) F.sale_en_espera = 1 + (int)(azar() % 60);
            else if (q < 65) F.err_en_sector = 1 + (int)(azar() % 20);
            else if (q < 67) F.crc_siempre = 1;
            else if (q < 69) F.max_por_orden = 4;
            lba = I->sectores ? azar() % I->sectores : 0; n = 1 + azar() % 200;
            if (I->sectores && n > I->sectores - lba) n = I->sectores - lba;
            if (q >= 69 && q < 72) F.w_crc_en = 1 + (int)(azar() % 30);
            else if (q >= 72 && q < 74) F.w_err_en = 1 + (int)(azar() % 30);
            else if (q >= 74 && q < 76) F.w_cuelga_en = 1 + (int)(azar() % 30);
            antes = us;
            if (azar() & 1) {                                 /* (MSW) la mitad, escrituras: lo dado por escrito ESTÁ; lo demás, prefijo */
                u32 sem = azar(), p;
                if (lba >= 4096) lba %= 4096;                 /* (la tabla de escritos es de 8192) */
                if (n > 64) n = 1 + n % 64;
                if (I->sectores && n > I->sectores - lba) n = I->sectores - lba;
                r = escribir(lba, n, sem); p = prefijo(lba, n, sem);
                if (r == 0) { wbien++; if (p != n) datos_malos++; } else if (r == MS_E_FUERA) fuera++; else wmal++;
            } else {
                r = leer(lba, n);
                if (r == 0) { bien++; if (!datos_bien(lba, n)) datos_malos++; } else if (r == MS_E_FUERA) fuera++; else mal++;
            }
            if (us - antes > peor) peor = us - antes;
            if (us - antes > 20000000ull) largas++;
            ms_paso();
            if (I->estado == MS_MALA) { memset(&F, 0, sizeof F); sacar(); meter(); }
        }
        printf("  azar: %u vueltas; lecturas bien %u, con error %u; escrituras bien %u, con error %u; «no hay tarjeta» %u; peor llamada %llu ms; TPC %u, encendidos %u, recuperaciones %u\n",
               n_azar, bien, mal, wbien, wmal, fuera, peor / 1000, tpcs, I->encendidos, I->recuperaciones);
        OK(datos_malos == 0, "%u lecturas o escrituras dadas por BUENAS con datos MALOS", datos_malos);
        printf("  (28-oct) azar: cortes con una orden de escritura abierta: %u con la tarjeta contestando, %u con ella muda\n", cortes_w, cortes_w_muda);
        OK(cortes_w == 0, "(28-oct) %u cortes de tension con una orden de escritura abierta y la tarjeta contestando", cortes_w);
        OK(wbien > n_azar / 20, "pocas escrituras buenas (%u)", wbien);
        OK(largas == 0, "%u llamadas de mas de 20 s simulados", largas);
        OK(bien > n_azar / 10, "pocas lecturas buenas (%u)", bien);
    }
    caso("fin");
#ifdef MS_SOLO_SERIE
    printf("(SOLO SERIE) GET_INT repetidos tras un fallo: %u\n", ms_int_reintentos());
#endif
    printf("%s: %d casos, %d comprobaciones falladas\n", fallos_prueba ? "PROBAR-MS: MAL" : "PROBAR-MS: BIEN", casos - 1, fallos_prueba);
    return fallos_prueba ? 1 : 0;
}
