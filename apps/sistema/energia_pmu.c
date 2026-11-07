/* Sistema -> «Energía (PMU)» (pista S7; docs/bateria.md §8.A7 y §9; CARGA: §12, las filas 0 y 19..24): SOLO LECTURA, refresco de 1 s. Es la herramienta de
 * las pruebas T1–T4 con el aparato: ver si el bit 7 de 0x60 se baja solo y cuándo, y que con el cargador puesto 0x7A, el
 * canal 9 y 0x77 no se mueven. Todo se RELEE DEL CHIP en cada refresco (hw_pmu_diag) y SOLO mientras esta pantalla está
 * abierta: fuera de aquí el sistema no lee 0x60..0x63, 0x7A ni convierte los canales 8, 9 y 1.
 * No escribe nada en el PMU (más que el selector del ADC, 0x70, como cualquier medida). */
#include "shell.h"
#include "bat.h"
#include "carga.h"                    /* (CARGA) los estados de la carga por USB */

typedef struct { Widget base; } EnergiaPmu;
typedef struct { i32 sel; HwPmuDiag d; int hay; u32 t0, lecturas; } EnergiaPmuE;
extern const Clase ClsEnergiaPmu;
#define N_FILAS 25

static int mv_de_codigo(int c)                 /* tabla de códigos de tensión del cargador (0x44288F28 del original; docs/bateria.md §1) */
{
    static const short alto[7] = { 4350, 4400, 4500, 4600, 4800, 5000, 5200 };
    c &= 0x1F;
    return c <= 4 ? 3300 + 100 * c : c <= 7 ? 3750 + 50 * (c - 5) : c <= 0x18 ? 3900 + 25 * (c - 8) : alto[c - 0x19];
}
static void leer(Ctx *c)
{
    EnergiaPmuE *s = ui_estado(c);
    s->hay = hw_pmu_diag(&s->d) == HW_OK;
    s->lecturas++;
    ui_sucio(c);
}
static void iniciar(Ctx *c) { EnergiaPmuE *s = ui_estado(c); s->t0 = hw_ms(); leer(c); ui_cada(c, 1000, leer); }
static const char *hex(int v) { return v < 0 ? "--" : ui_fmt("%02X", v); }

static Widget *fila(Ctx *l, int i, int sel, void *dato)
{
    EnergiaPmuE *s = ui_estado(ui_ancestro(l, &ClsEnergiaPmu));
    const HwPmuDiag *d = &s->d;
    HwArranque a;
    const char *n = "", *v = "";
    u32 seg = (hw_ms() - s->t0) / 1000;
    (void)dato;
    switch (i) {
    case 0: {                                  /* (CARGA, 25-oct-2026) la carga por USB (api/carga.h) */
        static const char *const nom[CARGA_N_ESTADOS] = { "sin USB", "USB sin configurar", "midiendo", "CARGANDO", "completa", "pausa (temperatura)",
                                                          "DETENIDA", "cargador de pared: no", "prohibida" };
        HwCarga k; hw_carga_estado(&k);
        n = "Carga USB"; v = k.estado < 0 ? "no en este aparato" : k.estado < CARGA_N_ESTADOS ? nom[k.estado] : "?"; break; }
    case 19: { HwCarga k; hw_carga_estado(&k); n = "  corriente";
        v = k.estado < 0 ? "--" : ui_fmt("código %d (%d mA pedidos), %d cortes", k.icod, k.pedida / 10, k.techos); break; }
    case 20: { HwCarga k; hw_carga_estado(&k); n = "  bornes / tope";
        v = k.mv < 0 ? "--" : ui_fmt("%d mV (recarga < %d; tope %d)", k.mv, CARGA_MV_RECARGA, CARGA_MV_TOPE); break; }
    case 21: { HwCarga k; hw_carga_estado(&k); n = "  batería (BDATA)";
        v = k.tipo == 255 || k.temp_dc == CARGA_SIN_TEMP ? (k.bdata < 0 ? "sin medir" : ui_fmt("ADC11 %d: fuera de tabla", k.bdata))
          : ui_fmt("%d,%d °C, tipo %d (ADC11 %d)", k.temp_dc / 10, (k.temp_dc < 0 ? -k.temp_dc : k.temp_dc) % 10, k.tipo, k.bdata); break; }
    case 22: { HwCarga k; hw_carga_estado(&k); n = "  tiempo cargando"; v = ui_fmt("%u:%02u de 6:00", k.ms_cargando / 3600000u, k.ms_cargando / 60000u % 60u); break; }
    case 23: { HwCarga k; hw_carga_estado(&k); n = "  medidor (sigma)"; v = !k.fg_ok ? "--" : ui_fmt("%ld (≈ %ld mA%s)", k.fg, k.fg * 10 / 94, k.fg > 0 ? ", entra" : ""); break; }
    case 24: { HwCarga k; hw_carga_estado(&k); n = "  USB concedido"; v = ui_fmt("%d mA; 63 = %s", k.usb_ma, hex(k.r63)); break; }
    case 1:  n = "Abierta hace"; v = ui_fmt("%u:%02u (%u lect.)", seg / 60, seg % 60, s->lecturas); break;
    case 2:  n = "23 estado"; v = d->r23 < 0 ? "--" : ui_fmt("%02X%s%s%s", d->r23, d->r23 & 0x08 ? " CARGADOR" : "", d->r23 & 0x80 ? " USB" : "", d->r23 & 0x40 ? " SIN BAT." : ""); break;
    case 3:  n = "60 tensión carga"; v = d->r60 < 0 ? "--" : ui_fmt("%02X: %d mV, bit7=%d", d->r60, mv_de_codigo(d->r60), (d->r60 >> 7) & 1); break;
    case 4:  n = "61 corriente carga"; v = d->r61 < 0 ? "--" : ui_fmt("%02X: %d mA%s", d->r61, (d->r61 & 15) * 100, (d->r61 & 15) ? " ¡NO ES 0!" : ""); break;
    case 5:  n = "62"; v = hex(d->r62); break;
    case 6:  n = "63 carga por USB"; v = d->r63 < 0 ? "--" : ui_fmt("%02X (V%d, I%d)", d->r63, d->r63 & 3, (d->r63 >> 4) & 3); break;
    case 7:  n = "77 VBAT (vigilada)"; v = d->r77 < 0 ? "--" : ui_fmt("%d = %d mV", d->r77, bat_mv_marcha(d->r77)); break;
    case 8:  n = "75 VBAT"; v = d->r75 < 0 ? "--" : ui_fmt("%d = %d mV", d->r75, bat_mv_marcha(d->r75)); break;
    case 9:  n = "76 VBAT (TX)"; v = d->r76 < 0 ? "--" : ui_fmt("%d = %d mV", d->r76, bat_mv_marcha(d->r76)); break;
    case 10: n = "6A VBAT en frío"; v = d->l6a < 0 ? "--" : ui_fmt("%d = %d mV", d->l6a, bat_mv_frio(d->l6a)); break;
    case 11: n = "7A corr. cargador"; v = d->r7a < 0 ? "--" : ui_fmt("%d (%02X)", d->r7a, d->r7a); break;
    case 12: n = "ADC 8 V cargador"; v = d->adc8 < 0 ? "--" : ui_fmt("%d = %d mV", d->adc8, d->adc8 * 25); break;
    case 13: n = "ADC 9 (transistor)"; v = d->adc9 < 0 ? "--" : ui_fmt("%d", d->adc9); break;
    case 14: n = "ADC 1 temp. placa"; v = d->adc1 < 0 ? "--" : ui_fmt("%d = %d mV", d->adc1, d->adc1 * 1078 / 100); break;
    case 15: n = "Vigilancia"; v = hw_bateria_mv() < 0 ? "sin PMU" : ui_fmt("%d mV, %d %% (%s)", hw_bateria_mv(), hw_bateria_pct(), hw_bateria_fuente() ? "por 6A" : "por 77"); break;
    case 16: n = "  aviso / apagado"; v = ui_fmt("< %d / < %d mV", HW_BAT_MV_AVISO, HW_BAT_MV_CORTE); break;
    case 17: hw_arranque(&a); n = "Al arrancar"; v = a.lectura < 0 ? "sin lectura" : ui_fmt("%d = %d mV, 23=%02X", a.lectura, a.mv, a.estado23 & 0xFF); break;
    case 18: hw_arranque(&a); n = "  veredicto"; v = a.como == HW_ARR_CON_CARGADOR ? "con cargador o USB" : a.como == HW_ARR_BAT_NO_CREIBLE ? "LECTURA NO CREIBLE" : "batería bien"; break;
    }
    return sh_fila(n, v, sel);
}
static int tecla(Ctx *t, int k, void *dato)
{
    if (k == K_OK || k == K_SEL_IZQ) return 1;
    return sh_teclas(t, k, dato);
}
static Widget *construir(Ctx *c)
{
    EnergiaPmuE *s = ui_estado(c);
    return Teclas(tecla, Pantalla("Energía (PMU)", ListaPerezosa(.n = N_FILAS, .fila = fila, .sel = &s->sel, .alto_fila = dis_tokens()->m.alto_fila), "", "Atrás"));
}
COMPONENTE_CON_ESTADO(EnergiaPmu, EnergiaPmuE, construir, iniciar);
Widget *app_energia_pmu(void) { return UI_W(EnergiaPmu); }
