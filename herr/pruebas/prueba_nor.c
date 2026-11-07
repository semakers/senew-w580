/* prueba_nor.c — (R3; P1, P2 y AUD3-M2) hal/nor.c DE VERDAD (compilado con -DNOR_PRUEBA) contra un MODELO del chip en el PC.
 * El emulador de la pista E no modela la suspensión a través de un reset, ni un chip lento, ni un chip que necesita un rato
 * tras reanudar para avanzar: aquí sí. make probar-nor. Cada caso es un proceso (hal/nor.c guarda estado estático).
 *
 * EL MODELO (juego de órdenes Intel, lo que usa nor.c): 70 estado · 50 limpiar · 60+D0 / 60+01 des/bloquear · 20+D0 borrar ·
 * B0 suspender · D0 reanudar · 41+dato programar palabra · 90 identificar · 98 CFI · FF lectura. El tiempo pasa con cada
 * LECTURA del estado. VIOLACIONES que cuenta (cualquiera hace fallar el caso):
 *   - una orden que no sea 70 / D0 / 50 / FF / B0 con un borrado SUSPENDIDO o en curso;
 *   - leer la flash «mapeada» (por puntero: es lo que hace el código que vive en la flash) con el chip ocupado, suspendido
 *     en ese bloque, o fuera del modo de lectura;
 *   - borrar o programar un bloque bloqueado; programar subiendo un bit; una orden destructiva fuera de las zonas.
 *
 * (R3, tras AUD6) Con un borrado SUSPENDIDO el modelo admite además 90 y 98 (identificar / CFI): lo permite el juego de órdenes
 * de Intel en suspensión de borrado [inferido de la familia; en un M18, sin saber]; con el chip BORRANDO, no.
 *   prueba_nor CASO      (normal | irq | avance | suspendido | sin_registro | susp_ajeno | susp_fuera | otro_susp | ocupado |
 *                         lento | error | otro_id | sin_qry | cfi_tam | programa | modelo) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal.h"
#include "m18.h"
#include "nor_placa.h"

#define NBLQ ((NOR_ESCR_FIN - NOR_ESCR_INI) / M18_BLOQUE)
/* (I1) Desde W6F lo escribible empieza en NOR_W6F_INI: los bloques del modelo 0..63 son de W6F y el almacén y W6U van detrás.
 * ZB(n) = el bloque «n» de los de siempre (0..3 almacén, 4..11 W6U) en el modelo. */
#define N_W6F_MOD ((NOR_W6F_FIN - NOR_W6F_INI) / M18_BLOQUE)
#define ZB(n) (N_W6F_MOD + (n))
enum { M_LECTURA, M_ESTADO, M_ID, M_CFI };
static struct {
    u8  mem[NBLQ * M18_BLOQUE];
    int modo, previa;                  /* previa: la orden de dos pasos a medias (0x60, 0x20, 0x41) */
    u32 previa_dir;
    u16 sr;
    int borrando, suspendido, susp_pedida;   /* bloque que se borra (-1 = ninguno) */
    long resta, desde_reanudar, lat_susp;
    u8  abierto[NBLQ];
    /* lo que se configura en cada caso */
    long dura_borrado, min_avance, latencia_susp; int irq_siempre, falla_borrado;
    u16 id[2], cfi[0x40];
    /* lo que se cuenta */
    long violaciones, sondeos, destructivas, suspensiones, reanudaciones, borrados_hechos, palabras;
    long objetos;                      /* (I1) programas de búfer (E9) de una región entera, en modo objeto */
    u32 buf_dir, buf_n; u16 buf[512];  /* (I1) el E9 a medias: dirección, palabras que faltan por llegar, lo llegado */
    char primera[160];
} c;
static u32 irq_cerradas, aperturas;
static u32 rst_pend, rst_apuntes;        /* (R3, AUD6) el registro de reinicio: lo que dejó apuntado el arranque anterior */
u32 rst_nor_pendiente(void) { return rst_pend; }
static u32 rst_ultimo;                   /* (I1) el último código distinto de 0 que se apuntó */
void rst_nor_borrando(u8 b) { rst_apuntes++; if (b) rst_ultimo = b; }
u32 irq_cerrar(void) { u32 a = irq_cerradas; irq_cerradas = 1; return a; }
static int mirar_al_abrir; static long abiertas_mal;    /* (2026-10-26) programa que cede: al abrir las IRQ el chip tiene que estar en LECTURA y sin orden a medias */
void irq_abrir(void);
nor_cuentas nor_cta_dummy;

static void viola(const char *que, u32 dir, u32 v)
{
    if (!c.violaciones++) snprintf(c.primera, sizeof c.primera, "%s (dir %08X, valor %04X)", que, dir, v);
}
static int blq_de(u32 dir) { return dir >= NOR_ESCR_INI && dir < NOR_ESCR_FIN ? (int)((dir - NOR_ESCR_INI) / M18_BLOQUE) : -1; }
static void tic(void)                  /* pasa «un sondeo» de tiempo */
{
    c.sondeos++;
    if (c.borrando < 0) return;
    if (c.suspendido) return;
    if (c.susp_pedida) { if (--c.lat_susp <= 0) { c.susp_pedida = 0; c.suspendido = 1; c.suspensiones++; c.sr = 0x80 | 0x40; } return; }
    if (++c.desde_reanudar > c.min_avance) c.resta--;
    if (c.resta <= 0) {
        if (c.falla_borrado) c.sr = 0x80 | 0x20;
        else { memset(c.mem + (size_t)c.borrando * M18_BLOQUE, 0xFF, M18_BLOQUE); c.sr = 0x80; c.borrados_hechos++; }
        c.borrando = -1;
    }
}
u16 nor_pr_leer(u32 dir)
{
    if (c.modo == M_ID) return dir - 0x44000000u < 4 ? c.id[(dir - 0x44000000u) / 2] : 0;
    if (c.modo == M_CFI) return (dir - 0x44000000u) / 2 < 0x40 ? c.cfi[(dir - 0x44000000u) / 2] : 0;
    if (c.modo == M_ESTADO || c.borrando >= 0) { tic(); return c.borrando >= 0 && !c.suspendido ? 0x00 : c.sr; }
    { int b = blq_de(dir); return b < 0 ? 0xFFFF : (u16)(c.mem[dir - NOR_ESCR_INI] | (c.mem[dir - NOR_ESCR_INI + 1] << 8)); }
}
static long ordenes;                   /* todas las escrituras al chip */
u32 nor_pr_sanear(u32 blq);
u32 nor_pr_objeto(u32 dir, const u16 *org);
u32 nor_pr_control(u32 dir, const u16 *org);
void nor_pr_escribir(u32 dir, u16 v)
{
    ordenes++;
    int b = blq_de(dir), ocupado = c.borrando >= 0;
    if (c.previa == 0x41) {                              /* el dato de un programa de palabra */
        c.previa = 0;
        if (b < 0) { viola("programa fuera de las zonas", dir, v); return; }
        c.destructivas++;
        if (!c.abierto[b]) { viola("programa en un bloque bloqueado", dir, v); c.sr = 0x80 | 0x12; return; }
        { u16 hay = (u16)(c.mem[dir - NOR_ESCR_INI] | (c.mem[dir - NOR_ESCR_INI + 1] << 8));
          if ((hay & v) != v) viola("programa que sube un bit", dir, v);
          hay &= v; c.mem[dir - NOR_ESCR_INI] = (u8)hay; c.mem[dir - NOR_ESCR_INI + 1] = (u8)(hay >> 8); }
        c.palabras++; c.sr = 0x80; c.modo = M_ESTADO;
        return;
    }
    if (c.previa == 0xE9) {                              /* (I1) la cuenta del búfer: 0x1FF = 512 palabras */
        c.previa = 0;
        if (v != 0x1FF || dir != c.previa_dir) { viola("E9 sin la cuenta 0x1FF en la misma dirección", dir, v); return; }
        c.previa = 0xE8; c.buf_dir = dir; c.buf_n = 0; return;
    }
    if (c.previa == 0xE8) {                              /* (I1) las 512 palabras, en orden, y la confirmación D0 */
        if (c.buf_n < 512) {
            if (dir != c.buf_dir + 2 * c.buf_n) { viola("palabra del búfer fuera de su sitio", dir, v); c.previa = 0; return; }
            c.buf[c.buf_n++] = v; return;
        }
        c.previa = 0;
        if (v != 0xD0 || dir != c.buf_dir) { viola("búfer sin la confirmación D0", dir, v); return; }
        if (b < 0) { viola("programa de búfer fuera de las zonas", dir, v); return; }
        c.destructivas++;
        if (!c.abierto[b]) { viola("programa de búfer en un bloque bloqueado", dir, v); c.sr = 0x80 | 0x12; c.modo = M_ESTADO; return; }
        if ((dir & 0x3FFu) || dir < NOR_W6F_INI || dir >= NOR_W6F_FIN) viola("MODO OBJETO fuera de la zona de W6F o desalineado", dir, v);
        { u32 k; for (k = 0; k < 1024; k++) if (c.mem[dir - NOR_ESCR_INI + k] != 0xFF) { viola("MODO OBJETO sobre una región que NO está en blanco", dir, v); break; }
          for (k = 0; k < 512; k++) { c.mem[dir - NOR_ESCR_INI + 2 * k] &= (u8)c.buf[k]; c.mem[dir - NOR_ESCR_INI + 2 * k + 1] &= (u8)(c.buf[k] >> 8); } }
        c.objetos++; c.sr = 0x80; c.modo = M_ESTADO;
        return;
    }
    if (c.previa == 0x60) { c.previa = 0; if (b >= 0 && v == 0xD0) { c.abierto[b] = 1; return; } if (b >= 0 && v == 0x01) { c.abierto[b] = 0; return; } }
    if (c.previa == 0x20) {
        c.previa = 0;
        if (v == 0xD0) {
            if (b < 0) { viola("borrado fuera de las zonas", dir, v); return; }
            c.destructivas++;
            if (!c.abierto[b]) { viola("borrado de un bloque bloqueado", dir, v); c.sr = 0x80 | 0x22; return; }
            c.borrando = b; c.resta = c.dura_borrado; c.desde_reanudar = 0; c.suspendido = c.susp_pedida = 0; c.sr = 0; c.modo = M_ESTADO;
            return;
        }
    }
    if (ocupado && v != 0x70 && v != 0xD0 && v != 0x50 && v != 0xFF && v != 0xB0 && !(c.suspendido && (v == 0x90 || v == 0x98)))
        viola(c.suspendido ? "orden con un borrado SUSPENDIDO" : "orden con un borrado en curso", dir, v);
    switch (v) {
    case 0x70: c.modo = M_ESTADO; break;
    case 0x50: c.sr &= (u16)~0x3A; break;
    case 0xFF: if (!ocupado || c.suspendido) c.modo = M_LECTURA; break;      /* con el chip borrando, FF no hace nada */
    case 0x90: c.modo = M_ID; break;
    case 0x98: c.modo = M_CFI; break;
    case 0xB0: if (ocupado && !c.suspendido && !c.susp_pedida) { c.susp_pedida = 1; c.lat_susp = c.latencia_susp; } break;
    case 0xD0: if (c.suspendido) { c.suspendido = 0; c.desde_reanudar = 0; c.reanudaciones++; c.sr = 0; c.modo = M_ESTADO; } break;
    case 0x60: case 0x20: case 0x41: case 0xE9: c.previa = v; c.previa_dir = dir; break;
    default: viola("orden desconocida", dir, v);
    }
}
const u8 *nor_pr_mem(u32 dir)          /* la flash «mapeada»: solo vale con el chip en lectura y sin nada a medias */
{
    if (c.borrando >= 0 && !c.suspendido) viola("lectura de la flash con el chip BORRANDO", dir, 0);
    else if (c.suspendido) viola("lectura de la flash con un borrado SUSPENDIDO (sin volver a lectura o en ese bloque)", dir, 0);
    else if (c.modo != M_LECTURA) viola("lectura de la flash fuera del modo de lectura", dir, 0);
    return c.mem + (dir - NOR_ESCR_INI);
}
u32 nor_pr_irq(void) { return (u32)c.irq_siempre; }
void irq_abrir(void) { irq_cerradas = 0; aperturas++; if (mirar_al_abrir && (c.modo != M_LECTURA || c.previa)) abiertas_mal++; }

static void chip_de_fabrica(void)
{
    memset(&c, 0, sizeof c);
    memset(c.mem, 0xFF, sizeof c.mem);
    c.borrando = -1; c.sr = 0x80; c.dura_borrado = 30000; c.latencia_susp = 20;
    c.id[0] = 0x0089; c.id[1] = 0x887E;
    c.cfi[0x10] = 'Q'; c.cfi[0x11] = 'R'; c.cfi[0x12] = 'Y'; c.cfi[0x13] = 0x00; c.cfi[0x14] = 0x02;
    c.cfi[0x27] = 0x1A; c.cfi[0x2A] = 0x0A; c.cfi[0x2C] = 1; c.cfi[0x2D] = 0xFF; c.cfi[0x2E] = 0x00; c.cfi[0x2F] = 0x00; c.cfi[0x30] = 0x04;
}
static int mal;
#define MIRA(cond, ...) do { printf("  %s  ", (cond) ? "bien" : "MAL "); printf(__VA_ARGS__); printf("\n"); if (!(cond)) mal = 1; } while (0)
static int espejo_igual(u32 blq_abs)   /* el espejo lógico de ese bloque == lo que el chip tiene (vista lógica) */
{
    const u8 *e = blq_abs < 4 ? nor_zona(blq_abs) : nor_usr(blq_abs - 4); u32 l;
    for (l = 0; l < M18_LOGICO; l++) if (e[l] != c.mem[(size_t)ZB(blq_abs) * M18_BLOQUE + m18_fis(l)]) return 0;
    return 1;
}
static void fin_comun(void)
{
    MIRA(c.borrando < 0 && !c.suspendido, "el chip no queda ni borrando ni SUSPENDIDO");
    MIRA(c.modo == M_LECTURA, "el chip queda en modo de lectura");
    MIRA(c.violaciones == 0, "violaciones del modelo: %ld%s%s", c.violaciones, c.violaciones ? " — la primera: " : "", c.primera);
}
int main(int argc, char **argv)
{
    const char *caso = argc > 1 ? argv[1] : "normal";
    int r; u32 i;
    chip_de_fabrica();
    for (i = 0; i < sizeof c.mem; i += 2) { c.mem[i] = (u8)(i * 7 + 3); c.mem[i + 1] = (i & 16) ? 0xFF : (u8)(i >> 5); if (i & 16) c.mem[i] = 0xFF; }   /* contenido: modo control */
    memset(c.mem, 0xFF, (size_t)N_W6F_MOD * M18_BLOQUE);     /* (I1) la zona de W6F, en blanco salvo lo que ponga el caso */
    printf("== hal/nor.c contra el modelo: caso «%s»\n", caso);
    if (!strcmp(caso, "modelo")) {     /* NEGATIVA del propio modelo: lo que tiene que cazar, lo caza */
        c.borrando = 5; c.suspendido = 1; c.sr = 0xC0; c.resta = 100;
        nor_pr_escribir(0x44000000u, 0x90);
        MIRA(c.violaciones == 0, "una orden 90 con un borrado suspendido SE ADMITE (identificar en suspensión)");
        nor_pr_escribir(NOR_ESCR_INI + 6 * M18_BLOQUE, 0x60);
        MIRA(c.violaciones == 1, "una orden 60 con un borrado suspendido se cuenta como violación (%s)", c.primera);
        c.modo = M_LECTURA; (void)nor_pr_mem(NOR_ESCR_INI + 5 * M18_BLOQUE);
        MIRA(c.violaciones == 2, "leer la flash con un borrado suspendido se cuenta");
        chip_de_fabrica(); c.modo = M_ESTADO; (void)nor_pr_mem(NOR_ESCR_INI);
        MIRA(c.violaciones == 1, "leer la flash fuera del modo de lectura se cuenta");
        chip_de_fabrica(); nor_pr_escribir(NOR_ESCR_INI, 0x20); nor_pr_escribir(NOR_ESCR_INI, 0xD0);
        MIRA(c.violaciones == 1, "borrar un bloque sin desbloquear se cuenta");
        printf(mal ? "MAL\n" : "BIEN\n"); return mal;
    }
    if (!strcmp(caso, "suspendido")) { /* P2: un borrado SUSPENDIDO sobrevivió a un reinicio en caliente (bloque 5 = el 2.º de Mis archivos) */
        memset(c.mem + ZB(5) * M18_BLOQUE, 0x5A, M18_BLOQUE / 3);    /* a medio borrar: basura */
        c.borrando = ZB(5); c.suspendido = 1; c.sr = 0xC0; c.resta = 12000; c.abierto[ZB(5)] = 1; c.modo = M_LECTURA;
        rst_pend = 6;                                               /* y el registro de reinicio dice que se estaba borrando ESE (1 + 5) */
        r = nor_escr_init();
        MIRA(r == 0, "nor_escr_init = %d", r);
        MIRA(nor_chip()->suspendidos == 1, "nor_chip()->suspendidos = %u (encontró y terminó el borrado suspendido)", nor_chip()->suspendidos);
        MIRA(c.reanudaciones == 1 && c.borrados_hechos == 1, "reanudaciones %ld, borrados acabados %ld", c.reanudaciones, c.borrados_hechos);
        for (i = 0; i < M18_BLOQUE && c.mem[ZB(5) * M18_BLOQUE + i] == 0xFF; i++) ;
        MIRA(i == M18_BLOQUE, "el bloque 5 quedó borrado entero");
        MIRA(!c.abierto[ZB(5)], "y bloqueado otra vez");
        MIRA(espejo_igual(5), "el espejo del bloque 5 es lo que hay en el chip (0xFF), no lo de antes");
        MIRA(c.destructivas == 0, "ninguna orden de borrar o programar NUEVA: %ld", c.destructivas);
        fin_comun(); printf(mal ? "MAL\n" : "BIEN\n"); return mal;
    }
    /* ---- (R3) los casos de la auditoría AUD6 (work/aud6/prueba_nor_aud6.c), con lo que tiene que pasar AHORA ---- */
    if (!strcmp(caso, "sin_registro") || !strcmp(caso, "susp_fuera") || !strcmp(caso, "otro_susp") || !strcmp(caso, "susp_ajeno")) {
        /* un suspendido que NO se puede atribuir (sin registro / registro con un bloque fuera de rango) o a un chip SIN permiso,
         * o uno AJENO (en la misma partición, fuera de nuestras zonas): NO se reanuda, no se le manda nada que cambie la flash,
         * no se lee la flash con él suspendido, y no hay escritura (se dice) */
        c.borrando = !strcmp(caso, "susp_ajeno") ? 999 : ZB(5); c.suspendido = 1; c.sr = 0xC0; c.resta = 3000; c.abierto[ZB(5)] = 1; c.modo = M_LECTURA;
        if (!strcmp(caso, "susp_fuera")) rst_pend = 200;
        if (!strcmp(caso, "otro_susp")) { c.id[1] = 0x1234; rst_pend = 6; }
        r = nor_escr_init();
        MIRA(r == -NOR_E_CHIP && c.reanudaciones == 0 && c.destructivas == 0, "nor_escr_init = %d; reanudaciones %ld; órdenes destructivas %ld: NO se toca", r, c.reanudaciones, c.destructivas);
        MIRA(!strcmp(caso, "otro_susp") ? nor_chip()->que == NOR_CHIP_ID : nor_chip()->suspendido_ajeno == 1, "%s", !strcmp(caso, "otro_susp") ? "el chip no es el de la placa (que = ID)" : "se dice: suspendido_ajeno = 1");
        MIRA(nor_zona_bloque() == 0 && nor_borrar(0) == -NOR_E_RANGO, "sin zonas ni escritura");
        MIRA(c.violaciones == 0, "violaciones del modelo: %ld%s%s", c.violaciones, c.violaciones ? " — la primera: " : "", c.primera);
        printf(mal ? "MAL\n" : "BIEN\n"); return mal;
    }
    if (!strcmp(caso, "rango_sanear")) {   /* (AUD6 M3) la rutina de RAM de sanear mira ELLA MISMA el rango: fuera de nuestras zonas, ni una orden */
        static const u32 malas[] = { NOR_ESCR_INI - M18_BLOQUE, NOR_ESCR_FIN, 0x45E00000u, 0x47E00000u, NOR_ESCR_INI + 0x400u, 0x44140000u };
        u32 k, todas = 1;
        c.borrando = 5; c.suspendido = 1; c.sr = 0xC0; c.resta = 3000;
        for (k = 0; k < sizeof malas / sizeof malas[0]; k++) { long o = ordenes; if (nor_pr_sanear(malas[k]) != 0x200 || ordenes != o) todas = 0; }
        MIRA(todas, "6 direcciones fuera de rango o desalineadas: devuelve 0x200 sin mandar NINGUNA orden");
        MIRA(c.reanudaciones == 0, "y no se reanuda nada");
        printf(mal ? "MAL\n" : "BIEN\n"); return mal;
    }
    if (!strcmp(caso, "ocupado")) {    /* el SoC se reinicia con el chip BORRANDO (no suspendido): se espera, con tope, sin mandar nada */
        memset(c.mem + ZB(5) * M18_BLOQUE, 0x5A, M18_BLOQUE / 3);
        c.borrando = ZB(5); c.suspendido = 0; c.sr = 0x00; c.resta = 5000; c.abierto[ZB(5)] = 1; c.modo = M_ESTADO;
        r = nor_escr_init();
        MIRA(r == 0 && !nor_chip()->ocupado, "nor_escr_init = %d: esperó a que acabara (dentro del tope)", r);
        MIRA(espejo_igual(5), "el espejo del bloque 5 es lo que el chip tiene");
        fin_comun(); printf(mal ? "MAL\n" : "BIEN\n"); return mal;
    }
    if (!strcmp(caso, "ocupado_siempre")) {   /* ... y si no acaba en el tope: ni identificar, ni zonas, ni escritura */
        c.borrando = ZB(5); c.suspendido = 0; c.sr = 0x00; c.resta = 2000000000L; c.abierto[ZB(5)] = 1; c.modo = M_ESTADO;
        r = nor_escr_init();
        MIRA(r == -NOR_E_CHIP && nor_chip()->ocupado && nor_zona_bloque() == 0, "nor_escr_init = %d, ocupado = %u: sin zonas", r, nor_chip()->ocupado);
        MIRA(c.violaciones == 0 && c.destructivas == 0, "ni una orden ni una lectura con él ocupado: violaciones %ld", c.violaciones);
        printf(mal ? "MAL\n" : "BIEN\n"); return mal;
    }
    if (!strcmp(caso, "otro_id") || !strcmp(caso, "sin_qry") || !strcmp(caso, "cfi_tam")) {     /* P1: geometría frente a permiso */
        const NorChip *ch;
        if (!strcmp(caso, "otro_id")) c.id[1] = 0x1234;
        if (!strcmp(caso, "sin_qry")) c.cfi[0x10] = 0;
        if (!strcmp(caso, "cfi_tam")) c.cfi[0x27] = 0x19;
        r = nor_escr_init(); ch = nor_chip();
        MIRA(r == -NOR_E_CHIP && nor_escr_motivo() == NOR_E_CHIP, "nor_escr_init = %d (sin permiso de escritura)", r);
        if (!strcmp(caso, "sin_qry")) {
            MIRA(!ch->geometria && nor_zona_bloque() == 0 && nor_usr_bloques() == 0, "sin tabla CFI no hay geometría: ni zonas");
            MIRA(ch->que == NOR_CHIP_CFI && ch->cfi_mal == 0x10, "que = CFI, palabra 0x%02X", ch->cfi_mal);
        } else {
            MIRA(ch->geometria && nor_zona_bloque() == M18_LOGICO && nor_zona_bloques() == 4 && nor_usr_bloques() == 8, "HAY geometría: las zonas se LEEN (4 + 8 bloques de %u)", nor_zona_bloque());
            MIRA(espejo_igual(0) && espejo_igual(11), "el espejo trae lo que hay en el chip");
            if (!strcmp(caso, "otro_id")) MIRA(ch->que == NOR_CHIP_ID && ch->id == 0x00891234u, "que = ID (leído %08X; se espera %08X)", ch->id, NOR_PLACA_ID);
            else MIRA(ch->que == NOR_CHIP_CFI && ch->cfi_mal == 0x27, "que = CFI, palabra 0x%02X (tamaño 2^%u)", ch->cfi_mal, ch->cfi_tam);
        }
        r = nor_borrar(0); MIRA(r == -NOR_E_RANGO, "nor_borrar(0) = %d: se niega", r);
        r = nor_usr_programar(0, 0, "hola", 4); MIRA(r == -NOR_E_RANGO, "nor_usr_programar = %d: se niega", r);
        MIRA(c.destructivas == 0, "órdenes de borrar o programar que llegaron al chip: %ld", c.destructivas);
        fin_comun(); printf(mal ? "MAL\n" : "BIEN\n"); return mal;
    }
    /* ---- (I1) W6F: un borrado de W6F SUSPENDIDO por un reinicio y apuntado en el registro (códigos 13..): se sanea ESE bloque ---- */
    if (!strcmp(caso, "w6f_susp")) {
        memset(c.mem + 9 * M18_BLOQUE, 0x5A, M18_BLOQUE / 3);
        c.borrando = 9; c.suspendido = 1; c.sr = 0xC0; c.resta = 12000; c.abierto[9] = 1; c.modo = M_LECTURA;
        rst_pend = 12 + 1 + 9;                                       /* el código del bloque 9 de W6F */
        r = nor_escr_init();
        MIRA(r == 0 && nor_chip()->suspendidos == 1 && c.reanudaciones == 1 && c.borrados_hechos == 1, "nor_escr_init = %d: el bloque 9 de W6F, reanudado y terminado", r);
        for (i = 0; i < M18_BLOQUE && c.mem[9 * M18_BLOQUE + i] == 0xFF; i++) ;
        MIRA(i == M18_BLOQUE && !c.abierto[9], "borrado entero y bloqueado otra vez");
        MIRA(nor_w6f_bloques() == N_W6F_MOD, "y la zona de W6F se ve: %u bloques", nor_w6f_bloques());
        fin_comun(); printf(mal ? "MAL\n" : "BIEN\n"); return mal;
    }
    /* los demás: chip esperado, se escribe */
    if (!strcmp(caso, "irq")) c.irq_siempre = 1;
    if (!strcmp(caso, "avance")) { c.irq_siempre = 1; c.min_avance = 1500; c.dura_borrado = 4000; }   /* tras reanudar, 1500 sondeos sin avanzar */
    if (!strcmp(caso, "lento")) c.dura_borrado = 45000000;                                            /* más que TOPE_BORRAR */
    if (!strcmp(caso, "error")) c.falla_borrado = 1;
    r = nor_escr_init();
    MIRA(r == 0 && nor_chip()->que == NOR_CHIP_BIEN && nor_chip()->suspendidos == 0, "nor_escr_init = %d, chip reconocido, sin borrados suspendidos", r);
    MIRA(c.destructivas == 0 && c.reanudaciones == 0, "al iniciar no se manda ninguna orden destructiva ni D0");
    MIRA(rst_apuntes == 0, "al iniciar no se apunta ningún borrado");
    if (!strcmp(caso, "w6f")) {        /* (I1) la zona de W6F: modo OBJETO, sin espejo, con sus guardas */
        static u8 d[1024], blanco[1024]; long o;
        for (i = 0; i < sizeof d; i++) d[i] = (u8)(i * 29 + 7);
        memset(blanco, 0xFF, sizeof blanco);
        MIRA(nor_w6f_bloques() == N_W6F_MOD && nor_w6f(3) == c.mem + 3 * M18_BLOQUE, "%u bloques de W6F; nor_w6f(3) apunta al chip (sin espejo)", nor_w6f_bloques());
        r = nor_w6f_programar(3, 7, d); MIRA(r == 0 && c.objetos == 1, "nor_w6f_programar(3, 7) = %d: UN programa de búfer (E9) de la región entera", r);
        MIRA(!memcmp(c.mem + 3 * M18_BLOQUE + 7 * 1024, d, 1024) && !memcmp(c.mem + 3 * M18_BLOQUE + 6 * 1024, blanco, 1024), "lo programado está en el chip, y la región de al lado sigue en blanco");
        MIRA(!c.abierto[3], "el bloque queda bloqueado otra vez");
        o = ordenes; r = nor_w6f_programar(3, 7, d);
        MIRA(r == -NOR_E_RANGO && ordenes == o, "otra vez la misma región (ya NO está en blanco): %d, y NINGUNA orden al chip", r);
        o = ordenes; r = nor_w6f_programar(3, 256, d); MIRA(r == -NOR_E_RANGO && ordenes == o, "región 256 (fuera del bloque): %d, sin órdenes", r);
        o = ordenes; r = nor_w6f_programar(N_W6F_MOD, 0, d); MIRA(r == -NOR_E_RANGO && ordenes == o, "bloque %u (fuera de la zona): %d, sin órdenes", N_W6F_MOD, r);
        memset(c.mem + ZB(0) * M18_BLOQUE + 0xC00, 0xFF, 0x400); memset(c.mem + ZB(4) * M18_BLOQUE + 0x400, 0xFF, 0x400);   /* regiones EN BLANCO del almacén y de W6U */
        { u32 malas[] = { NOR_ZONA_INI + 0xC00u, NOR_USR_INI + 0x400u, NOR_W6F_INI + 0x200u, NOR_W6F_INI - 0x400u, 0x45E00000u }, k, todas = 1;
          for (k = 0; k < sizeof malas / sizeof malas[0]; k++) { o = ordenes; if (nor_pr_objeto(malas[k], (const u16 *)(const void *)d) != NOR_R_RANGO || ordenes != o) todas = 0; }
          MIRA(todas, "la rutina de RAM del modo objeto se niega ELLA MISMA en el almacén, en W6U, desalineada y fuera: sin órdenes"); }
        {   /* (AUD7 B2) y la del modo CONTROL se niega ella misma en la zona de W6F (una región en blanco, alineada, en el bloque 5) */
            static u16 w[512]; u32 k; long o2 = ordenes;
            for (k = 0; k < 512; k++) w[k] = (k & 8) ? 0xFFFF : (u16)(k * 3);
            MIRA(nor_pr_control(NOR_W6F_INI + 5 * M18_BLOQUE + 0x800u, w) == NOR_R_RANGO && ordenes == o2, "la rutina de RAM del modo CONTROL se niega ELLA MISMA en la zona de W6F: sin órdenes");
        }
        r = nor_w6f_borrar(3); MIRA(r == 0 && c.borrados_hechos == 1 && rst_apuntes == 2 && rst_ultimo == 12 + 1 + 3, "nor_w6f_borrar(3) = %d, apuntado en el registro de reinicio con su código (%u)", r, rst_ultimo);
        for (i = 0; i < M18_BLOQUE && c.mem[3 * M18_BLOQUE + i] == 0xFF; i++) ;
        MIRA(i == M18_BLOQUE, "el bloque 3 de W6F, en blanco");
        r = nor_w6f_programar(3, 7, d); MIRA(r == 0, "tras borrarlo, la región se puede programar otra vez: %d", r);
        MIRA(espejo_igual(1) && espejo_igual(11), "el espejo del almacén y de W6U no se ha movido de sitio (bloques 1 y 11; el 0 y el 4 los tocó la prueba)");
        fin_comun(); printf(mal ? "MAL\n" : "BIEN\n"); return mal;
    }
    if (!strcmp(caso, "programa") || !strcmp(caso, "programa_irq")) {
        static u8 d[700]; const u8 *e;
        int con_irq = !strcmp(caso, "programa_irq");
        u32 ap0;
        for (i = 0; i < sizeof d; i++) d[i] = (u8)(i * 13 + 1);
        r = nor_usr_borrar(2); MIRA(r == 0, "nor_usr_borrar(2) = %d", r);
        if (con_irq) { c.irq_siempre = 1; mirar_al_abrir = 1; }
        ap0 = aperturas;
        r = nor_usr_programar(2, 300, d, sizeof d); MIRA(r == 0, "nor_usr_programar(2, 300, 700 bytes) = %d (%ld palabras)", r, c.palabras);
        MIRA(c.palabras == 350, "cada palabra se programa UNA vez: %ld programas de palabra para 700 bytes (350)", c.palabras);
        if (con_irq) {
            MIRA(nor_cta.cesiones > 0 && aperturas - ap0 >= nor_cta.cesiones, "(2026-10-26) con una IRQ siempre pendiente CEDE entre palabras: %u cesiones (cada una abre las IRQ) para %ld palabras, y ACABA", nor_cta.cesiones, c.palabras);
            MIRA(abiertas_mal == 0, "cada vez que se abren las IRQ el chip está en LECTURA y sin orden a medias (%ld mal)", abiertas_mal);
            c.irq_siempre = 0; mirar_al_abrir = 0;
        } else MIRA(nor_cta.cesiones == 0, "sin IRQ pendientes no cede (%u)", nor_cta.cesiones);
        e = nor_usr(2); MIRA(!memcmp(e + 300, d, sizeof d) && e[299] == 0xFF && e[1000] == 0xFF, "el espejo trae lo programado y nada más");
        MIRA(espejo_igual(6), "y es lo que hay en el chip, en modo control");
        for (i = 0; i < M18_BLOQUE; i++) if ((i & 16) && c.mem[ZB(6) * M18_BLOQUE + i] != 0xFF) break;
        MIRA(i == M18_BLOQUE, "la mitad alta de cada segmento sigue a 0xFF");
        r = nor_usr_programar(2, 300, "\xFF\xFF", 2); MIRA(r == -NOR_E_BITS, "subir un bit: %d (se niega sin tocar el chip)", r);
        fin_comun(); printf(mal ? "MAL\n" : "BIEN\n"); return mal;
    }
    r = nor_usr_borrar(1);             /* bloque absoluto 5 */
    if (!strcmp(caso, "lento")) {
        MIRA(r == -NOR_E_TIEMPO, "nor_usr_borrar = %d (NOR_E_TIEMPO: tardó más que el tope)", r);
        MIRA(c.borrados_hechos == 1, "pero NO se volvió hasta que el chip acabó (borrados acabados: %ld, %ld sondeos)", c.borrados_hechos, c.sondeos);
    } else if (!strcmp(caso, "error")) {
        MIRA(r == -NOR_E_CHIP_SR && nor_cta.ultimo_sr == 0x20, "nor_usr_borrar = %d, SR = %02X (error de borrado)", r, nor_cta.ultimo_sr);
    } else {
        MIRA(r == 0, "nor_usr_borrar(1) = %d", r);
        MIRA(c.borrados_hechos == 1, "el bloque se borró");
    }
    MIRA(espejo_igual(5), "el espejo del bloque es lo que hay en el chip");
    MIRA(rst_apuntes == 2, "el borrado se apuntó en el registro de reinicio (antes y después): %u", rst_apuntes);
    if (!strcmp(caso, "normal")) MIRA(c.suspensiones == 0 && nor_cta.suspensiones == 0, "sin IRQ pendientes no se suspende");
    if (!strcmp(caso, "irq")) MIRA(c.suspensiones > 0 && c.suspensiones < 100 && aperturas >= (u32)c.suspensiones, "con una IRQ siempre pendiente: %ld suspensiones (cada una abre las IRQ) y ACABA", c.suspensiones);
    if (!strcmp(caso, "avance")) {
#if NOR_SONDEOS_SIN_MIRAR >= 1500
        MIRA(c.suspensiones > 0 && c.suspensiones < 100, "un chip que tarda 1500 sondeos en avanzar tras reanudar, con IRQ siempre pendiente: %ld suspensiones y ACABA", c.suspensiones);
#else
        MIRA(c.suspensiones >= 100000 && c.borrados_hechos == 1, "NEGATIVA (sin sondeos de gracia): %ld suspensiones sin avanzar; aun así no se abandona suspendido: se acaba con las IRQ cortadas", c.suspensiones);
#endif
    }
    fin_comun();
    printf(mal ? "MAL\n" : "BIEN\n");
    return mal;
}
