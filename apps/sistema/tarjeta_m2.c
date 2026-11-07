/* Sistema -> OK en las filas «Tarjeta (M2)» / «bus» / «cuentas» -> «Tarjeta (M2)» a fondo (M2 ag, 2026-10-21). En el aparato (v3) la
 * Memory Stick dio «la tarjeta no pasa a 4 bits»; desde la v4 cae sola a 1 bit. UNA FOTO de esta pantalla dice qué pasó:
 *  - el bus en que quedó y, si cayó a 1 bit, en QUÉ TRAMO del paso a 4 bits (hal/ms.h MS_PAR_*) y con qué código;
 *  - cómo acabó la espera tras pasar a paralelo (INT de la tarjeta / CED visto en +8 / tope del original) y en cuántos ms;
 *  - los registros de la tarjeta leídos en serie y YA EN 4 BITS (si difieren, el bus de 4 bits trae datos rotos);
 *  - el anfitrión: +C en serie, antes y después del paso (el original: 0x4015, 0x6015 -> 0x200D), +8 tras el paso y el último leído;
 *    el PMU releído (el original: 43 = 07, 44 = 0E) y SYSCON +40 +1A +14 +1C con la tarjeta encendida (el original, +1C = 0401);
 *  - las cuentas (TPC, tiempos, CRC, INT, recuperaciones, lecturas y escrituras).
 * Tecla 1: PRUEBA DE PUNTA A PUNTA sobre la FAT: escribe /SENEW_M2.BIN (64 KiB con patrón), lo relee, lo compara y lo borra; dice
 * las velocidades. Todo débil: en las placas sin hal/ms.c (o sin hal/ms_db2020.c) dice «-». */
#include "shell.h"
#include "tarjeta.h"
#include "../../hal/ms.h"
#include "../../hal/ms_db2020.h"
#pragma weak ms_info
#pragma weak ms_motivo
#pragma weak ms_par_fase
#pragma weak ms_int_reintentos
extern MsDbCuentas ms_db2020_cta __attribute__((weak));

typedef struct { Widget base; } TarjetaM2;
typedef struct { i32 sel; } TarjetaM2E;
extern const Clase ClsTarjetaM2;
#define N_FILAS 19
#define PR_RUTA "/SENEW_M2.BIN"
#define PR_TAM  65536u

static char pr_txt[48] = "pulsa 1 (escribe y borra 64 KiB)";
static u32 pr_kbs_w, pr_kbs_r;

static void leer(Ctx *c) { ui_sucio(c); }
static void iniciar(Ctx *c) { ui_cada(c, 500, leer); }
static const char *sino(u8 v) { return v == MS_INT_SERIE_SI ? "si" : v == MS_INT_SERIE_NO ? "NO" : "?"; }
static u32 kbs(u32 bytes, u32 ms) { return ms ? bytes / ms * 1000u / 1024u : 0; }
static void copia(char *d, u32 n, const char *o) { u32 i; for (i = 0; i + 1 < n && o[i]; i++) d[i] = o[i]; d[i] = 0; }

static void prueba(void)
{
    u8 *a = mo_probar(PR_TAM), *b = mo_probar(PR_TAM);
    u32 i, t0, t1, t2, n; int r;
    pr_kbs_w = pr_kbs_r = 0;
    if (!a || !b) { if (a) mo_soltar(a); if (b) mo_soltar(b); copia(pr_txt, sizeof pr_txt, "sin memoria"); return; }
    if (!hw_tj_escribible()) { mo_soltar(a); mo_soltar(b); copia(pr_txt, sizeof pr_txt, "NO SE ESCRIBE (no montada / prot.)"); return; }
    for (i = 0; i < PR_TAM; i++) a[i] = (u8)(i * 7u + (i >> 9) + 0x5Au);
    t0 = hw_ms();
    r = hw_tj_guardar(PR_RUTA, a, PR_TAM);
    t1 = hw_ms();
    if (r != HW_OK) { mo_soltar(a); mo_soltar(b); copia(pr_txt, sizeof pr_txt, ui_fmt("ESCRIBIR FALLO (%d, motivo %d)", r, hw_tj_motivo())); return; }
    for (i = 0; i < PR_TAM; i++) b[i] = (u8)~a[i];
    n = tj_leer_desde(PR_RUTA, 0, b, PR_TAM);
    t2 = hw_ms();
    for (i = 0; i < PR_TAM && a[i] == b[i]; i++) { }
    r = hw_tj_borrar(PR_RUTA);
    pr_kbs_w = kbs(PR_TAM, t1 - t0); pr_kbs_r = kbs(n, t2 - t1);
    if (n != PR_TAM) copia(pr_txt, sizeof pr_txt, ui_fmt("LEER CORTO: %u de %u", n, PR_TAM));
    else if (i != PR_TAM) copia(pr_txt, sizeof pr_txt, ui_fmt("DISTINTO en el byte %u", i));
    else copia(pr_txt, sizeof pr_txt, r == HW_OK ? "BIEN: escrito, igual, borrado" : "BIEN, pero no se pudo borrar");
    mo_soltar(a); mo_soltar(b);
}

static Widget *fila(Ctx *l, int i, int sel, void *dato)
{
    static const char *const est[] = { "fuera", "lista", "dormida", "MALA", "preparando" };
    const MsInfo *m = ms_info ? ms_info() : 0;
    const MsDbCuentas *d = &ms_db2020_cta ? &ms_db2020_cta : 0;
    const char *n = "", *v = "-";
    (void)l; (void)dato;
    if (!m && i) return sh_fila("", "", sel);
    switch (i) {
    case 0:  n = "Tarjeta (M2)"; if (m) v = m->estado == MS_FUERA ? "no hay" : ui_fmt("%s, %u MB%s%s", est[m->estado < 5 ? m->estado : 3], m->sectores / 2048, m->motivo ? ": " : "", m->motivo && ms_motivo ? ms_motivo(m->motivo) : ""); break;
    case 1:  n = "Bus"; v = ui_fmt("%s (4b: %u mal, %u bien)", m->serie ? "1 BIT" : "4 bits", m->paralelo_fallos, m->paralelo_bien); break;
    case 2:  n = "Fallo 4 bits"; v = !m->par_fase ? "ninguno" : ui_fmt("%s, t=%u", ms_par_fase ? ms_par_fase(m->par_fase) : "?", m->par_t); break;
    case 3:  n = "Espera tras paso"; v = !m->par_fin ? "-" : ui_fmt("%s %u ms, INT %s", m->par_fin == MS_PAR_FIN_INT ? "INT" : m->par_fin == MS_PAR_FIN_CED ? "CED+8" : "TOPE", m->par_ms, sino(m->int_paso)); break;
    case 4:  n = "Regs serie"; v = ui_fmt("S0 %02X tipo %u cat %u cl %u", m->reg[0], m->reg[2], m->reg[4], m->reg[5]); break;
    case 5:  n = "Regs en 4 bits"; v = !m->par_reg[2] && !m->paralelo_bien && m->par_fase < MS_PAR_REG_DISTINTOS ? "-" : ui_fmt("S0 %02X t %u c %u cl %u r10 %02X", m->par_reg[0], m->par_reg[2], m->par_reg[4], m->par_reg[5], m->par_reg[6]); break;
    case 6:  n = "+C ser / par"; if (d) v = ui_fmt("%04X / %04X>%04X", d->sys_serie & 0xFFFF, d->sys_antes & 0xFFFF, d->sys_par & 0xFFFF); break;
    case 7:  n = "+8 paso / ult"; if (d) v = ui_fmt("%04X / %04X (%u pasos)", d->sta_par & 0xFFFF, d->sta_ult & 0xFFFF, d->pasos_par); break;
    case 8:  n = "PMU 43 44"; if (d) v = ui_fmt("%02X %02X (orig 07 0E)%s", d->pmu43, d->pmu44, d->pmu_tension_mal ? ui_fmt(" MAL %u", d->pmu_tension_mal) : ""); break;
    case 9:  n = "SC 40 1A 14 1C"; if (d) v = ui_fmt("%04X %04X %04X %04X", d->s40, d->s1a, d->s14, d->s1c); break;
    case 10: n = "IRQ 76"; if (d) v = ui_fmt("%u vac %u torm %u cort %u", d->irqs, d->irqs_vacias, d->tormentas, d->cortes); break;
    case 11: n = "INT en serie"; v = ui_fmt("%s, sinINT %u, rep %u", sino(m->int_serie), m->e_int, ms_int_reintentos ? ms_int_reintentos() : 0); break;
    case 12: n = "TPC"; v = ui_fmt("%u rep %u t/o %u crc %u err %u", m->tpc, m->tpc_reintentos, m->e_tiempo, m->e_crc, m->e_estado); break;
    case 13: n = "Encendidos"; v = ui_fmt("%u, reposos %u, recup %u", m->encendidos, m->reposos, m->recuperaciones); break;
    case 14: n = "Metidas/sacadas"; v = ui_fmt("%u / %u, ident %u ms", m->entradas, m->salidas, m->ms_identificar); break;
    case 15: n = "Leidos"; v = ui_fmt("%u sect. en %u ord.", m->leidos, m->ordenes); break;
    case 16: n = "Escritos"; v = ui_fmt("%u sect. en %u ord. (mal %u)", m->escritos, m->ordenes_w, m->e_escritura); break;
    case 17: n = "Prueba (1)"; v = pr_txt; break;
    case 18: n = "  velocidad"; v = pr_kbs_w || pr_kbs_r ? ui_fmt("esc %u KB/s, lee %u KB/s", pr_kbs_w, pr_kbs_r) : "-"; break;
    default: break;
    }
    return sh_fila(n, v, sel);
}
static int tecla(Ctx *t, int k, void *dato)
{
    Ctx *c = ui_ancestro(t, &ClsTarjetaM2);
    if (k == K_1) { copia(pr_txt, sizeof pr_txt, "probando..."); prueba(); ui_forzar_todo(); leer(c); return 1; }
    if (k == K_OK || k == K_SEL_IZQ) return 1;
    return sh_teclas(t, k, dato);
}
static Widget *construir(Ctx *c)
{
    TarjetaM2E *s = ui_estado(c);
    return Teclas(tecla, Pantalla("Tarjeta (M2)", ListaPerezosa(.n = N_FILAS, .fila = fila, .sel = &s->sel, .alto_fila = dis_tokens()->m.alto_fila), "", "Atrás"));
}
COMPONENTE_CON_ESTADO(TarjetaM2, TarjetaM2E, construir, iniciar);
Widget *app_tarjeta_m2(void) { return UI_W(TarjetaM2); }
