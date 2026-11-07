/* Prueba de api/carga.h (pista CARGA, 25-oct-2026): la máquina de la carga por USB contra una batería y un PMU SIMULADOS con lo
 * medido en el aparato (work/bitacora_CARGA.md): el código de tensión 3 corta a 4,1 V en bornes, el 2 a 4,0 y el 1 a 3,9; la corriente
 * de los códigos 75/175/275 mA; r = 0,3 Ω; al cortar, el chip pone 0x63 = 00 y el suceso 0x21 bit 7. Escenarios con su veredicto;
 * sale con 1 si alguno falla.  make probar-carga */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "carga.h"

static int fallos, comprobados, icod_prohibido;
#define OK(c, ...) do { comprobados++; if (!(c)) { fallos++; printf("  MAL (%s:%d): ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- el mundo simulado */
typedef struct {
    double soc_mah, cap_mah, r, consumo;   /* mAh, mAh, ohmios, mA */
    int usb, cargador, quitada, usb_ma, temp_dc, tipo;
    int r63;                               /* el registro del chip */
    int ev21;                              /* suceso 0x21 bit 7 pendiente */
    int chip_corta;                        /* 1 = el chip corta en el techo (lo medido); 0 = un chip roto que nunca corta */
    int mv_falla;                          /* pasos en que la lectura de bornes falla */
    int escritura_falla;                   /* la próxima escritura de 0x63 no sale */
    int pmu_ok, prohibida;
    unsigned ms;
    int escrituras, max_icod_prohibido;    /* contadores para las comprobaciones */
} Mundo;
static const int techo_mv[4] = { 3800, 3900, 4000, 4100 };
static const int icod_ma[4] = { 0, 75, 175, 275 };
static double ocv(const Mundo *m)          /* tabla TYPE 1 del aparato (0x057D) + lleno en 4195 */
{
    static const double t[][2] = { {0,3250},{2,3510},{5,3645},{13,3685},{20,3740},{33,3770},{53,3810},{60,3869},{80,3980},{95,4155},{100,4195} };
    double p = 100.0 * m->soc_mah / m->cap_mah; int i;
    if (p <= 0) return 3250;
    for (i = 1; i < 11; i++) if (p <= t[i][0]) return t[i - 1][1] + (p - t[i - 1][0]) * (t[i][1] - t[i - 1][1]) / (t[i][0] - t[i - 1][0]);
    return 4195 + (p - 100) * 8;
}
static double i_bat(const Mundo *m) { int ic = m->usb && !m->quitada ? icod_ma[(m->r63 >> 4) & 3] : 0; return ic - m->consumo; }
static double bornes(const Mundo *m) { return ocv(m) + i_bat(m) * m->r; }   /* mA x Ω = mV */
static void avanza(Mundo *m, unsigned dms)
{
    unsigned k;
    for (k = 0; k < dms; k += 100) {           /* pasos de 100 ms */
        m->soc_mah += i_bat(m) * 0.1 / 3600.0;
        if (m->soc_mah < 0) m->soc_mah = 0;
        if (m->chip_corta && m->usb && (m->r63 & 0x30) && ocv(m) + i_bat(m) * m->r >= techo_mv[m->r63 & 3]) { m->r63 = 0; m->ev21 = 1; }
        m->ms += 100;
    }
}
static CargaEntrada entrada(Mundo *m, int *fallo_previo)
{
    CargaEntrada e;
    memset(&e, 0, sizeof e);
    e.ms = m->ms; e.pmu_ok = (unsigned char)m->pmu_ok; e.usb = (unsigned char)m->usb; e.cargador = (unsigned char)m->cargador; e.quitada = (unsigned char)m->quitada;
    e.usb_ma = (unsigned short)(m->usb ? m->usb_ma : 0);
    e.mv = m->mv_falla > 0 ? (m->mv_falla--, CARGA_SIN_MV) : (short)(bornes(m) + 0.5);
    e.temp_dc = (short)m->temp_dc; e.tipo = (unsigned char)m->tipo;
    e.r63 = (short)m->r63; e.suceso_techo = (unsigned char)m->ev21; m->ev21 = 0;
    e.prohibida = (unsigned char)m->prohibida; e.fallo_escritura = (unsigned char)*fallo_previo; *fallo_previo = 0;
    return e;
}
/* Un segundo: el sistema lee, decide y escribe como plat/hw_w580.c (dos escrituras, leyendo el chip antes de cada campo). */
static CargaSalida segundo(Mundo *m, Carga *c, int *fallo)
{
    CargaEntrada e = entrada(m, fallo);
    CargaSalida s = carga_paso(c, &e);
    int permitido = m->usb && !m->cargador && !m->quitada && m->usb_ma && m->pmu_ok && !m->prohibida && m->tipo >= 1 && m->tipo <= 2 &&
                    m->temp_dc >= CARGA_DC_MIN && m->temp_dc <= CARGA_DC_MAX;
    if (s.escribir) {
        m->escrituras++;
        if (m->escritura_falla) { m->escritura_falla = 0; *fallo = 1; }
        else {
            m->r63 = (m->r63 & 0xFC) | s.vcod;
            m->r63 = (m->r63 & 0xCF) | ((s.escribir == 1 ? s.icod : 0) << 4);
        }
    }
    if (!permitido && (m->r63 & 0x30)) { m->max_icod_prohibido = (m->r63 >> 4) & 3; icod_prohibido++; }
    avanza(m, 1000);
    return s;
}
static void mundo(Mundo *m, double mv_ini)
{
    memset(m, 0, sizeof *m);
    m->cap_mah = 930; m->r = 0.3; m->consumo = 30; m->usb = 1; m->usb_ma = 500; m->temp_dc = 220; m->tipo = 1; m->chip_corta = 1; m->pmu_ok = 1;
    m->ms = 1000;
    { double lo = 0, hi = m->cap_mah * 1.05; int i; for (i = 0; i < 60; i++) { m->soc_mah = (lo + hi) / 2; if (ocv(m) < mv_ini) lo = m->soc_mah; else hi = m->soc_mah; } }
}
static const char *nombre(int e)
{
    static const char *n[] = { "SIN_FUENTE", "USB_SIN_PERMISO", "MIDIENDO", "CARGANDO", "COMPLETA", "PAUSA_TEMP", "DETENIDA", "CARGADOR", "PROHIBIDA" };
    return e >= 0 && e < CARGA_N_ESTADOS ? n[e] : "?";
}

int main(void)
{
    Mundo m; Carga c; int f = 0, i, n;
    /* 1. Ciclo completo: 3,8 V -> carga con 33 -> el chip corta en 4,1 V -> escalera 3,3,3,2,1,1 -> COMPLETA; luego descarga y recarga */
    printf("1. ciclo completo con la escalera del original\n");
    mundo(&m, 3800); carga_iniciar(&c);
    {
        int icods[16], ni = 0, ult = -1, t_completa = -1, techos_vistos;
        for (i = 0; i < 6 * 3600; i++) {
            CargaSalida s = segundo(&m, &c, &f);
            if (s.escribir == 1 && s.icod != ult) { if (ni < 16) icods[ni++] = s.icod; ult = s.icod; }
            if (s.escribir == 1) OK(s.vcod == 3, "vcod %d", s.vcod);
            if (c.estado == CARGA_COMPLETA) { t_completa = i; break; }
        }
        printf("   completa a los %d s; %.0f mV en vacío; códigos escritos:", t_completa, ocv(&m));
        for (n = 0; n < ni; n++) printf(" %d", icods[n]);
        printf("; cortes del chip %u, escalones %u\n", c.techos, c.escalones);
        OK(t_completa > 0, "no llegó a COMPLETA");
        OK(ni >= 3 && icods[0] == 3 && icods[ni - 1] == 1, "códigos %d..%d", icods[0], icods[ni - 1]);
        OK(c.techos == 6, "cortes del chip %u (el original: 6 sucesos hasta COMPL)", c.techos);
        OK(ocv(&m) > 4040 && ocv(&m) < 4110, "vacío al acabar %.0f mV", ocv(&m));
        OK((m.r63 & 0x30) == 0, "0x63 con corriente en COMPLETA: %02X", m.r63);
        techos_vistos = c.techos;
        /* descarga con 60 mA hasta que recargue */
        m.consumo = 60;
        for (i = 0; i < 3 * 3600 && c.estado == CARGA_COMPLETA; i++) segundo(&m, &c, &f);
        printf("   recarga a los %d s de completa, con %.0f mV en bornes\n", i, bornes(&m));
        OK(c.estado == CARGA_CARGANDO && c.icod == 3, "no recargó (%s, icod %d)", nombre(c.estado), c.icod);
        OK(bornes(&m) > 3900, "recargó tarde");
        (void)techos_vistos;
    }
    /* 2. USB sin configurar / suspendido */
    printf("2. USB sin configurar y suspendido\n");
    mundo(&m, 3800); m.usb_ma = 0; carga_iniciar(&c);
    for (i = 0; i < 20; i++) segundo(&m, &c, &f);
    OK(c.estado == CARGA_USB_SIN_PERMISO && !(m.r63 & 0x30), "sin configurar: %s 63=%02X", nombre(c.estado), m.r63);
    m.usb_ma = 500; for (i = 0; i < 3; i++) segundo(&m, &c, &f);
    OK(c.estado == CARGA_CARGANDO && (m.r63 & 0x30) == 0x30, "configurado: %s 63=%02X", nombre(c.estado), m.r63);
    m.usb_ma = 0; segundo(&m, &c, &f);
    OK(c.estado == CARGA_USB_SIN_PERMISO && !(m.r63 & 0x30), "suspendido: %s 63=%02X", nombre(c.estado), m.r63);
    /* 3. Temperatura con histéresis */
    printf("3. temperatura\n");
    mundo(&m, 3800); carga_iniciar(&c);
    for (i = 0; i < 5; i++) segundo(&m, &c, &f);
    m.temp_dc = 455; segundo(&m, &c, &f);
    OK(c.estado == CARGA_PAUSA_TEMP && !(m.r63 & 0x30), "a 45,5 °C: %s 63=%02X", nombre(c.estado), m.r63);
    m.temp_dc = 445; for (i = 0; i < 3; i++) segundo(&m, &c, &f);
    OK(c.estado == CARGA_PAUSA_TEMP, "a 44,5 °C sigue en pausa (histéresis): %s", nombre(c.estado));
    m.temp_dc = 435; segundo(&m, &c, &f);
    OK(c.estado == CARGA_CARGANDO && (m.r63 & 0x30), "a 43,5 °C vuelve: %s", nombre(c.estado));
    m.temp_dc = 40; segundo(&m, &c, &f);
    OK(c.estado == CARGA_PAUSA_TEMP && !(m.r63 & 0x30), "a 4 °C: %s", nombre(c.estado));
    m.temp_dc = CARGA_SIN_TEMP; segundo(&m, &c, &f);
    OK(c.estado == CARGA_MIDIENDO && !(m.r63 & 0x30), "sin temperatura: %s", nombre(c.estado));
    /* 4. Batería desconocida, sin medir y quitada */
    printf("4. batería desconocida, sin medir, quitada\n");
    mundo(&m, 3800); m.tipo = 0xFF; carga_iniciar(&c);
    for (i = 0; i < 3; i++) segundo(&m, &c, &f);
    OK(c.estado == CARGA_MIDIENDO && !(m.r63 & 0x30), "sin medir: %s", nombre(c.estado));
    m.tipo = 0; segundo(&m, &c, &f);
    OK(c.estado == CARGA_DETENIDA && c.motivo == CARGA_M_DESCONOCIDA, "desconocida: %s", nombre(c.estado));
    m.tipo = 1; for (i = 0; i < 3; i++) segundo(&m, &c, &f);
    OK(c.estado == CARGA_DETENIDA && !(m.r63 & 0x30), "detenida hasta desenchufar: %s", nombre(c.estado));
    m.usb = 0; segundo(&m, &c, &f); m.usb = 1; for (i = 0; i < 2; i++) segundo(&m, &c, &f);
    OK(c.estado == CARGA_CARGANDO, "tras desenchufar y enchufar: %s", nombre(c.estado));
    m.quitada = 1; segundo(&m, &c, &f);
    OK(c.estado == CARGA_DETENIDA && c.motivo == CARGA_M_QUITADA && !(m.r63 & 0x30), "quitada: %s 63=%02X", nombre(c.estado), m.r63);
    /* 5. Un chip que NO corta: el tope de 4200 mV (bornes) y el de 4 h */
    printf("5. chip que no corta: topes de tensión y de tiempo\n");
    mundo(&m, 3900); m.chip_corta = 0; carga_iniciar(&c);
    for (i = 0; i < 8 * 3600 && c.estado != CARGA_DETENIDA; i++) segundo(&m, &c, &f);
    printf("   detenida a los %d s, motivo %d, %.0f mV en bornes antes de parar\n", i, c.motivo, ocv(&m) + 275 * 0.3 - 30 * 0.3);
    OK(c.estado == CARGA_DETENIDA && (c.motivo == CARGA_M_TENSION || c.motivo == CARGA_M_TIEMPO), "%s motivo %d", nombre(c.estado), c.motivo);
    OK(!(m.r63 & 0x30), "63=%02X", m.r63);
    mundo(&m, 3300); m.chip_corta = 0; m.soc_mah *= 8000.0 / 930; m.cap_mah = 8000; carga_iniciar(&c);   /* una batería enorme: no llega a 4200 en 6 h */
    for (i = 0; i < 8 * 3600 && c.estado != CARGA_DETENIDA; i++) segundo(&m, &c, &f);
    printf("   batería grande: detenida a los %d s, motivo %d (6 h = 21600)\n", i, c.motivo);
    OK(c.estado == CARGA_DETENIDA && c.motivo == CARGA_M_TIEMPO && i >= 21599 && i <= 21602, "tope de 6 h: %s motivo %d a %d s", nombre(c.estado), c.motivo, i);
    /* 6. Lecturas de bornes perdidas */
    printf("6. lecturas perdidas\n");
    mundo(&m, 3800); carga_iniciar(&c);
    for (i = 0; i < 3; i++) segundo(&m, &c, &f);
    m.mv_falla = 5; for (i = 0; i < 5; i++) segundo(&m, &c, &f);
    OK(c.estado == CARGA_CARGANDO && (m.r63 & 0x30), "5 perdidas se aguantan: %s", nombre(c.estado));
    m.mv_falla = 6; for (i = 0; i < 6; i++) segundo(&m, &c, &f);
    OK(c.estado == CARGA_MIDIENDO && !(m.r63 & 0x30), "6 perdidas paran: %s 63=%02X", nombre(c.estado), m.r63);
    /* 7. Una escritura de 0x63 que no sale no es un corte del chip */
    printf("7. fallo de escritura\n");
    mundo(&m, 3800); carga_iniciar(&c);
    for (i = 0; i < 3; i++) segundo(&m, &c, &f);
    m.r63 = 0x03; m.escritura_falla = 1;        /* algo lo borró y la reescritura falla */
    { unsigned esc = c.escalones; for (i = 0; i < 15; i++) segundo(&m, &c, &f);
      OK(c.estado == CARGA_CARGANDO && (m.r63 & 0x30) == 0x30, "se recupera: %s 63=%02X", nombre(c.estado), m.r63);
      printf("   escalones tras el borrado ajeno: %u (antes %u)\n", c.escalones, esc); }
    /* 8. Cargador de pared, PMU mudo, prohibida */
    printf("8. cargador, PMU mudo, prohibida\n");
    mundo(&m, 3800); carga_iniciar(&c);
    for (i = 0; i < 3; i++) segundo(&m, &c, &f);
    m.cargador = 1; segundo(&m, &c, &f);
    OK(c.estado == CARGA_CARGADOR && !(m.r63 & 0x30), "cargador: %s 63=%02X", nombre(c.estado), m.r63);
    m.cargador = 0; for (i = 0; i < 2; i++) segundo(&m, &c, &f);
    m.pmu_ok = 0; segundo(&m, &c, &f); m.pmu_ok = 1;
    OK(c.estado == CARGA_PROHIBIDA && c.motivo == CARGA_M_PMU, "PMU mudo: %s", nombre(c.estado));
    m.prohibida = 1; for (i = 0; i < 3; i++) segundo(&m, &c, &f);
    OK(c.estado == CARGA_PROHIBIDA && !(m.r63 & 0x30), "prohibida: %s 63=%02X", nombre(c.estado), m.r63);
    /* 9. Batería ya llena al enchufar: directo a COMPLETA, sin pulsos */
    printf("9. llena al enchufar\n");
    mundo(&m, 4150); carga_iniciar(&c); m.escrituras = 0;
    for (i = 0; i < 10; i++) segundo(&m, &c, &f);
    OK(c.estado == CARGA_COMPLETA && m.escrituras == 0, "%s, %d escrituras", nombre(c.estado), m.escrituras);
    /* 10. Solo por el releído (sin suceso) y solo por el suceso */
    printf("10. corte del chip visto solo por 0x63 releído\n");
    mundo(&m, 4000); carga_iniciar(&c);
    for (i = 0; i < 2 * 3600 && c.estado != CARGA_COMPLETA; i++) { m.ev21 = 0; segundo(&m, &c, &f); m.ev21 = 0; }
    OK(c.estado == CARGA_COMPLETA && c.techos == 6, "sin suceso: %s, %u cortes", nombre(c.estado), c.techos);
    /* 11. Reescritura periódica: cada 10 s aunque no cambie nada */
    printf("11. reescritura cada 10 s\n");
    mundo(&m, 3600); carga_iniciar(&c); m.escrituras = 0;
    for (i = 0; i < 61; i++) segundo(&m, &c, &f);
    OK(m.escrituras >= 6 && m.escrituras <= 8, "%d escrituras en 61 s", m.escrituras);
    /* 12. Nunca hubo corriente cuando no tocaba */
    OK(icod_prohibido == 0, "hubo corriente programada cuando no tocaba (%d segundos)", icod_prohibido);
    printf("\nprueba_carga: %d comprobaciones, %d fallos\n", comprobados, fallos);
    return fallos ? 1 : 0;
}
