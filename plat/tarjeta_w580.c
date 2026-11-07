/* La tarjeta del W580i para api/tarjeta.c: la Memory Stick (hal/ms.c sobre hal/ms_db2020.c). Pista T.
 * Aquí solo está la costura: el latido del perro en las esperas largas, que el paso del controlador no se llame
 * dentro de sí mismo, y los avisos por el registro. */
#include "hw.h"
#include "hal.h"
#include "ms_db2020.h"
#include "tarjeta.h"

static u8 dentro, lista_para_usar;

/* Las cuentas del controlador en una línea del registro: es lo primero que hay que mirar en el aparato cuando una
 * tarjeta no monta (docs/tarjeta.md §9: MS-19, MS-23). */
static void n(const char *t, u32 v) { hw_log(t); uart_dec(UART0, v); }
static void cuentas(void)
{
    const MsInfo *i = ms_info();
    n("tarjeta: cuentas: encendidos ", i->encendidos); n(", TPC ", i->tpc); n(" (repetidos ", i->tpc_reintentos); n("), sin respuesta ", i->e_tiempo);
    n(", CRC ", i->e_crc); n(", INT perdidas ", i->e_int); n(", ERR ", i->e_estado); n(", recuperaciones ", i->recuperaciones);
    n("; escritos ", i->escritos); n(" en ", i->ordenes_w); n(" ordenes (falladas ", i->e_escritura); n("), protegida ", i->protegida);
    n("; IRQ 76: ", ms_db2020_cta.irqs); n(" (vacias ", ms_db2020_cta.irqs_vacias); n(", tormentas ", ms_db2020_cta.tormentas);
    n("), esperas cortadas ", ms_db2020_cta.cortes); n(", PMU sin quitar tension ", ms_db2020_cta.pmu_fallos); n(", IRQ del pin ", ms_db2020_cta.det_irqs); n("; identificar ", i->ms_identificar); hw_log(" ms\n");
    /* (M2 ag) el bus: en 4 bits o caído a 1 bit, y por qué */
    hw_log(i->serie ? "tarjeta: bus 1 bit (4 bits fallo en " : "tarjeta: bus 4 bits (ultimo fallo en "); hw_log(ms_par_fase(i->par_fase)); n(" t=", i->par_t);
    n(", fallos ", i->paralelo_fallos); n(", bien ", i->paralelo_bien); n("; espera tras el paso: fin ", i->par_fin); n(" en ", i->par_ms);
    n(" ms; +C ", ms_db2020_cta.sys_antes); n("->", ms_db2020_cta.sys_par); n(" +8 ", ms_db2020_cta.sta_par); n("; PMU 43=", ms_db2020_cta.pmu43); n(" 44=", ms_db2020_cta.pmu44); hw_log(")\n");
}

/* El controlador pide un latido en sus esperas largas (el CED al encender puede tardar segundos; un fichero grande
 * son muchas órdenes). hw_perro_latido() vuelve a llamar al paso de la tarjeta: «dentro» lo corta. */
static void latido(void) { hw_perro_latido(); }

#ifdef MS_EXP
/* (2026-10-26, EXPERIMENTO: ¿por qué falla la identificación de la M2 con el USB en marcha?) Al arrancar, con la tarjeta dentro y ANTES de que
 * nadie toque el USB, UNA identificación entera (desde 4 bits, 1 intento) en cada configuración de los relojes que abre el USB al encenderse
 * (hal/usb_dc.c encender_bloque): bit 0 = SYSCON +02 bit 12 a 0; bit 1 = +0A bits 0..2 a 0; bit 2 = puertas 37/38 (+1A bits 10..11) abiertas.
 * Cada configuración se pone, se identifica y se DESHACE (lo de antes, tal cual). Abrir una puerta no toca el bloque (que sigue sin usar).
 * Resultado en ms_exp[] (se lee por el puente) y en el registro. Luego el arranque sigue como siempre (la tarjeta se ve entrar y se monta). */
int ms_exp_identificar(u32 *ms);
typedef struct { u16 cfg, s02, s0a, s1a; int r; u32 ms; u8 fase, t, serie, pad; u32 tpc, e_tiempo, t_drq, t_rdy, t_toe, t_crc, t_escr; } MsExp;
MsExp ms_exp[16]; u32 ms_exp_n; u16 ms_exp_ini[3];
static void exp_una(u16 cfg)
{
    u16 a02 = R16(SYSCON + 0x02), a0a = R16(SYSCON + 0x0A), a1a = R16(SYSCON + 0x1A);
    const MsInfo *i = ms_info(); MsExp *e = &ms_exp[ms_exp_n % 16]; u32 tpc0 = i->tpc, et0 = i->e_tiempo;
    MsDbCuentas k = ms_db2020_cta;
    if (cfg & 1) R16(SYSCON + 0x02) = (u16)(R16(SYSCON + 0x02) & ~0x1000u);
    if (cfg & 2) R16(SYSCON + 0x0A) = (u16)(R16(SYSCON + 0x0A) & ~0x0007u);
    if (cfg & 4) R16(SYSCON + 0x1A) = (u16)(R16(SYSCON + 0x1A) & ~0x0C00u);
    e->cfg = cfg; e->s02 = R16(SYSCON + 0x02); e->s0a = R16(SYSCON + 0x0A); e->s1a = R16(SYSCON + 0x1A);
    e->r = ms_exp_identificar(&e->ms);
    e->fase = i->par_fase; e->t = i->par_t; e->serie = i->serie;
    e->tpc = i->tpc - tpc0; e->e_tiempo = i->e_tiempo - et0;
    e->t_drq = ms_db2020_cta.t_drq - k.t_drq; e->t_rdy = ms_db2020_cta.t_rdy - k.t_rdy; e->t_toe = ms_db2020_cta.t_toe - k.t_toe;
    e->t_crc = ms_db2020_cta.t_crc - k.t_crc; e->t_escr = ms_db2020_cta.t_escr - k.t_escr;
    R16(SYSCON + 0x1A) = (u16)((R16(SYSCON + 0x1A) & ~0x0C00u) | (a1a & 0x0C00u));     /* deshacer: solo esos bits, como estaban */
    R16(SYSCON + 0x0A) = (u16)((R16(SYSCON + 0x0A) & ~0x0007u) | (a0a & 0x0007u));
    R16(SYSCON + 0x02) = (u16)((R16(SYSCON + 0x02) & ~0x1000u) | (a02 & 0x1000u));
    n("tarjeta EXP: cfg ", cfg); n(" -> ", (u32)(e->r < 0 ? -e->r : e->r)); n(" en ", e->ms); n(" ms, fase ", e->fase); n(" t ", e->t);
    n(", TPC ", e->tpc); n(" sin respuesta ", e->e_tiempo); n(" (DRQ ", e->t_drq); n(", fin ", e->t_rdy); n(", TOE ", e->t_toe); n("; de escribir ", e->t_escr); hw_log(")\n");
    ms_exp_n++;
}
static void experimento(void)
{
    static const u8 orden[] = { 0, 0, 1, 1, 2, 2, 4, 4, 7, 7, 0 };
    unsigned j;
    if (R16(0xF900001Eu) & 0x10u) { hw_log("tarjeta EXP: no hay tarjeta\n"); return; }
    ms_exp_ini[0] = R16(SYSCON + 0x02); ms_exp_ini[1] = R16(SYSCON + 0x0A); ms_exp_ini[2] = R16(SYSCON + 0x1A);
    for (j = 0; j < sizeof orden; j++) exp_una(orden[j]);
}
#endif
void hwo_tarjeta_iniciar(void)
{
    ms_db2020_init(latido); lista_para_usar = 1; hw_log("tarjeta: controlador de Memory Stick listo (lee y escribe sectores, IRQ 76)\n");
#ifdef MS_EXP
    dentro = 1;                                                  /* el latido de las esperas llama a hwo_tarjeta_paso: que no se meta */
    experimento();
    dentro = 0;
#endif
}
void hwo_tarjeta_paso(void)
{
    static u32 reposos, recuperaciones; static u8 estado;
    const MsInfo *i = ms_info();
    if (!lista_para_usar || dentro) return;
    dentro = 1;
    ms_paso();
    dentro = 0;
    if (i->estado != estado) { estado = i->estado; if (estado == MS_MALA || estado == MS_LISTA) cuentas(); if (estado == MS_PREPARANDO) hw_log("tarjeta: metida; su CED tarda: se espera POR PASOS desde el bucle\n"); }
    if (i->reposos != reposos) { reposos = i->reposos; hw_log("tarjeta: 30 s sin uso, apagada\n"); }
    if (i->recuperaciones != recuperaciones) { recuperaciones = i->recuperaciones; hw_log("tarjeta: hubo que apagarla y encenderla tras un fallo\n"); }
}
u32 hwo_tarjeta_generacion(void)
{
    const MsInfo *i = ms_info();
    return (i->estado == MS_LISTA || i->estado == MS_DORMIDA) ? i->generacion : 0;
}
u32 hwo_tarjeta_sectores(void) { return ms_info()->sectores; }
int hwo_tarjeta_leer(void *ctx, u32 lba, u32 n, u8 *buf)
{
    int r, despertar = ms_info()->estado == MS_DORMIDA;
    u8 antes = dentro;
    (void)ctx;
    if (!lista_para_usar) return -1;
    dentro = 1;
    r = ms_leer(lba, n, buf);
    dentro = antes;
    if (despertar && !r) hw_log("tarjeta: despierta\n");
    return r;
}
/* (MSW) La escritura: docs/tarjeta_escritura.md §1. Los MS_E_* se reducen a los HW_TJ_E_* del contrato. */
static int a_contrato(int r)
{
    if (r >= 0) return 0;
    if (r == MS_E_FUERA) return HW_TJ_E_FUERA;
    if (r == MS_E_RANGO) return HW_TJ_E_RANGO;
    if (r == MS_E_PROTEGIDA) return HW_TJ_E_PROTEGIDA;
    return HW_TJ_E_ES;
}
int hwo_tarjeta_escribir(void *ctx, u32 lba, u32 cuenta, const u8 *buf)
{
    int r, despertar = ms_info()->estado == MS_DORMIDA;
    u8 antes = dentro;
    (void)ctx;
    if (!lista_para_usar) return HW_TJ_E_FUERA;
    dentro = 1;
    r = ms_escribir(lba, cuenta, buf);
    dentro = antes;
    if (despertar && !r) hw_log("tarjeta: despierta\n");
    if (r && r != MS_E_RANGO && r != MS_E_PROTEGIDA) { hw_log("tarjeta: ESCRITURA FALLADA: "); hw_log(ms_motivo(r)); n(" (lba ", lba); n(", n ", cuenta); hw_log(")\n"); }
    return a_contrato(r);
}
int hwo_tarjeta_protegida(void)
{
    const MsInfo *i = ms_info();
    return (i->estado == MS_LISTA || i->estado == MS_DORMIDA) && i->protegida;
}
int hwo_tarjeta_vaciar(void *ctx)
{
    int r; u8 antes = dentro;
    (void)ctx;
    if (!lista_para_usar) return 0;
    dentro = 1;
    r = ms_vaciar();
    dentro = antes;
    return a_contrato(r);
}
int hwo_tarjeta_mala(const char **motivo)
{
    const MsInfo *i = ms_info();
    if (i->estado != MS_MALA) return 0;
    if (motivo) *motivo = ms_motivo(i->motivo);
    return 1;
}
int hwo_tarjeta_dormida(void) { return ms_info()->estado == MS_DORMIDA; }
int hwo_tarjeta_preparando(void) { return ms_info()->estado == MS_PREPARANDO; }   /* (I1) */
void hwo_tarjeta_apagar(void) { if (lista_para_usar) { (void)hwo_tarjeta_vaciar(0); ms_apagar_ya(); } }
/* (28-oct) el cierre (hal/ms.c): el bloque de borrado = unidad x bloque de su información del sistema (la M2 del W580: 512 x 512 B) */
u32 hwo_tarjeta_bloque_sectores(void) { const MsInfo *m = ms_info(); return ((u32)m->unidad * m->bloque) >> 9; }
void hwo_tarjeta_cierre_zona(const u32 *lba, u32 n, const u8 *sector) { ms_cierre_zona(lba, n, sector); }
