/* Sistema -> Opciones -> «Diagnóstico DSP» (2026-10-21): el ARRANQUE del DSP del W580i (hal/dsp_w580.c) A PETICIÓN, con una pantalla que dice hasta qué
 * paso llegó. NO se arranca solo nunca. UNA FOTO de esta pantalla (arriba y, si hace falta, abajo) basta:
 *  - el paso en que está/quedó (la pantalla se REPINTA al empezar cada paso: si el teléfono se cuelga, la foto dice cuál fue el último),
 *    el error (si lo hubo), cuánto tardó cada paso, y la versión que contestó el DSP (la cadena «## DSP VersionIdentity»);
 *  - los últimos pasos con su resultado, los últimos mensajes (canal, clase, primeras palabras) y los contadores de eventos de +10 y de DMA;
 *  - los registros del DSPIF releídos (+00 +04 +10 +14 +18 +1C), los de SYSCON (+12 bloques, +18 puertas, +2C) y el canal 4 del DMA PL080.
 * Teclas: 1 arrancar el DSP · 2 pedir la versión · 3 apagar el DSP · 5 arrancar AUNQUE haya cable USB (riesgo).
 * AVISO: «desenchufa el USB antes de arrancar el DSP». La puerta 26 y el bloque 20 (el reloj del PCM) se sospecharon causa de colgar el bus USB
 * (tmp/usb_ag/informe.md); el original los abre al arrancar el DSP con el USB sin reloj. Sin cable el USB no se toca nunca (shell/usb.c lo espera con
 * el PMU), así que arrancar sin cable es la prueba segura. Con cable la tecla 1 se NIEGA; la 5 arranca igual.
 * Al SALIR de la pantalla el DSP se apaga (dsp_apagar: bloques 2 y 3 a reset, puertas 2..5 y 26 cerradas, +2C como estaba).
 * Todo débil: en las placas sin hal/dsp_w580.c (el PC, el W600, el W810) dice «no disponible». */
#include "shell.h"
#include "../../hal/dsp_w580.h"

#pragma weak dsp_arrancar
#pragma weak dsp_pedir_version
#pragma weak dsp_apagar
#pragma weak dsp_estado
#pragma weak dsp_paso_nombre
#pragma weak dsp_error_nombre
#pragma weak dsp_version
#pragma weak dsp_ultimos_mensajes
#pragma weak dsp_pasos
#pragma weak dsp_regs
#pragma weak dsp_progreso
#pragma weak dsp_exp

typedef struct { Widget base; } DspDiag;
typedef struct {
    i32 sel;
    int hay, cable;
    DspEstado e;
    DspRegs r;
    DspMsgReg m[3]; int n_m;
    DspPasoReg p[6]; int n_p;
} DspDiagE;
extern const Clase ClsDspDiag;
#define N_FILAS 36

static Ctx *cur;                                   /* la pantalla abierta: el gancho del arranque la repinta */

static void leer(Ctx *c)
{
    DspDiagE *s = ui_estado(c);
    int cg;
    s->hay = dsp_estado != 0 && dsp_arrancar != 0 && dsp_regs != 0;
    if (s->hay) {
        s->e = *dsp_estado();
        dsp_regs(&s->r);
        s->n_m = dsp_ultimos_mensajes(s->m, 3);
        s->n_p = dsp_pasos(s->p, 6);
    }
    cg = hw_cargador();
    s->cable = cg > 0 && (cg & 2);
    ui_sucio(c);
}
static void gancho(void) { if (cur) { leer(cur); ui_cuadro(); } }   /* desde dentro del arranque: pinta el paso en que va */
static void iniciar(Ctx *c) { cur = c; if (dsp_progreso) dsp_progreso(gancho); leer(c); ui_cada(c, 500, leer); }
static void desechar(Ctx *c)
{
    (void)c;
    if (dsp_progreso) dsp_progreso(0);
    cur = 0;
    if (dsp_apagar && dsp_estado && dsp_estado()->gate5) dsp_apagar();
}

static const char *const corto[DSP_P_N] = { "-", "prepara", "DSPIF", "PCM 26/20", "mascaras", "PreBoot", "vent comun", "vent arranque", "suelta DSP",
    "espera 1800", "XRAM comun", "XRAM arranq.", "fin XRAM", "version", "VIVO", "apagado" };
static const char *nom(int p) { return p >= 0 && p < DSP_P_N ? corto[p] : "?"; }
static const char *hx(u32 v) { return ui_fmt("%08X", v); }

static Widget *fila(Ctx *l, int i, int sel, void *dato)
{
    DspDiagE *s = ui_estado(ui_ancestro(l, &ClsDspDiag));
    const DspEstado *e = &s->e;
    const DspRegs *r = &s->r;
    const char *n = "", *v = "";
    (void)dato;
    if (!s->hay) return sh_fila(i ? "" : "DSP", i ? "" : "no disponible en esta plataforma", sel);
    switch (i) {
    case 0:  n = "Teclas"; v = "1 arrancar 2 versión 3 apagar"; break;
    case 1:  n = "Exper. 4 6 7 8 9"; v = ui_fmt("p%u s%s r%u 1802 %s 2C %s", dsp_exp.pausa_ms, dsp_exp.sondeo ? "SI" : "no", dsp_exp.reintentos, dsp_exp.re1802 ? "x2" : "x1", dsp_exp.r2c ? "sin 8,9" : "tal cual"); break;
    case 2:  n = "Sondeo corto"; v = !e->sondeo_hecho ? "-" : e->sondeo_ms < 0 ? "NO lo leyo (500 ms)" : ui_fmt("leido en %d ms (vers x%u)", e->sondeo_ms, e->version_intentos); break;
    case 3:  n = "Cable USB"; v = s->cable ? "PUESTO: 1 se niega, 5 fuerza" : "sin cable: bien"; break;
    case 4:  n = "Paso"; v = ui_fmt("%u: %s%s", e->paso, dsp_paso_nombre(e->paso), e->ocupado ? " ..." : ""); break;
    case 5:  n = "Error"; v = e->error ? ui_fmt("%d %s", (i32)e->error, dsp_error_nombre(e->error)) : e->paso ? "ninguno" : "-"; break;
    case 6:  n = "Arranque"; v = ui_fmt("n %u, %u ms, %s%s", e->intentos, e->ms_total, e->vivo ? "VIVO" : "no vivo", e->nucleo ? ", DSP suelto" : ""); break;
    case 7:  n = "Version"; v = ui_fmt("%s", e->version[0] ? e->version : "(sin respuesta)"); break;
    case 8:  n = "  (sigue)"; v = e->version[0] && e->version[28] ? e->version + 28 : ""; break;
    case 9: case 10: case 11: case 12: case 13: case 14: {                       /* los últimos pasos, el más reciente primero */
        int k = i - 9;
        if (k >= s->n_p) { n = "Paso -"; v = ""; break; }
        n = ui_fmt("#%u %s", s->p[k].sec, nom(s->p[k].paso));
        v = s->p[k].res ? ui_fmt("ERR %d  t%u  x%X", (i32)s->p[k].res, s->p[k].ms, s->p[k].extra)
                        : ui_fmt("ok  t%u  x%X", s->p[k].ms, s->p[k].extra);
        break; }
    case 15: case 16: case 17: {                                                // los últimos mensajes
        int k = i - 15;
        if (k >= s->n_m) { n = "Mensaje -"; v = ""; break; }
        n = ui_fmt("%s c%02X %02X n%u", s->m[k].rx ? "RX" : "TX", s->m[k].canal & 0xFFFF, s->m[k].clase, s->m[k].n);
        v = ui_fmt("%04X %04X %04X %04X", s->m[k].p[0], s->m[k].p[1], s->m[k].p[2], s->m[k].p[3]);
        break; }
    case 18: n = "Mensajes"; v = ui_fmt("tx %u+%u rx %u+%u c%u p%u", e->tx_cortos, e->tx_largos, e->rx_cortos, e->rx_largos, e->rx_ctrl, e->rx_perdidos); break;
    case 19: n = "Eventos +10"; v = ui_fmt("0:%u 1:%u 2:%u 3:%u 4:%u 6:%u", e->ev[0], e->ev[1], e->ev[2], e->ev[3], e->ev[4], e->ev[6]); break;
    case 20: n = "DMA"; v = ui_fmt("lanz %u tc %u err %u", e->dma_lanzados, e->dma_tc, e->dma_err); break;
    case 21: n = "  canal 4"; v = !r->valido ? "-" : ui_fmt("ct %s cf %04X tc %02X", hx(r->pl_ctrl), r->pl_cfg & 0xFFFF, r->pl_tc & 0xFF); break;
    case 22: n = "XRAM"; v = ui_fmt("pet %u troz %u pal %u", e->xram_pet, e->xram_trozos, e->xram_pal); break;
    case 23: n = "  pedida"; v = ui_fmt("dir %X cap %u", e->xram_dir, e->xram_cap); break;
    case 24: n = "Ventana"; v = ui_fmt("pal %u esc %u", e->ventana_pal, e->ventana_esc); break;
    case 25: n = "DSPIF +00 +04"; v = !r->valido ? "sin reloj: no se lee" : ui_fmt("%s %s", hx(r->r00), hx(r->r04)); break;
    case 26: n = "  +10* +14"; v = !r->valido ? "-" : ui_fmt("%04X %s", r->r10 & 0xFFFF, hx(r->r14)); break;
    case 27: n = "  +18 +1C"; v = !r->valido ? "-" : ui_fmt("%s %s", hx(r->r18), hx(r->r1c)); break;
    case 28: n = "SYSCON"; v = ui_fmt("12:%03X 18:%03X 2C:%04X", r->s12 & 0xFFFF, r->s18 & 0xFFFF, r->s2c & 0xFFFF); break;
    case 29: n = "ms/paso"; v = ui_fmt("dspif %u pcm %u pre %u vent %u+%u", e->ms_paso[2], e->ms_paso[3], e->ms_paso[5], e->ms_paso[6], e->ms_paso[7]); break;
    case 32: n = "SC ini 02 04 06 0A 0C"; v = ui_fmt("%04X %04X %04X %04X %04X", e->sc_ini[0], e->sc_ini[1], e->sc_ini[2], e->sc_ini[3], e->sc_ini[4]); break;
    case 33: n = "SC ini 1C 38 54 80 36"; v = ui_fmt("%04X %04X %04X %04X %04X", e->sc_ini[5], e->sc_ini[6], e->sc_ini[7], e->sc_ini[8], e->sc_ini[9]); break;
    case 34: n = "SC ahora 02..0C"; v = ui_fmt("%04X %04X %04X %04X %04X", r->sc[0], r->sc[1], r->sc[2], r->sc[3], r->sc[4]); break;
    case 35: n = "SC ahora 1C..36"; v = ui_fmt("%04X %04X %04X %04X %04X", r->sc[5], r->sc[6], r->sc[7], r->sc[8], r->sc[9]); break;
    case 31: n = "SYSCON +2C"; v = ui_fmt("inicial %04X, puesto %04X, ahora %04X", e->s2c_ini, e->s2c_puesto, r->s2c & 0xFFFF); break;
    case 30: n = "  (cont.)"; v = ui_fmt("suelta %u 1800 %u xram %u+%u ver %u", e->ms_paso[8], e->ms_paso[9], e->ms_paso[10], e->ms_paso[11], e->ms_paso[13]); break;
    }
    return sh_fila(n, v, sel);
}

static void registrar(const char *que)             /* una línea por la UART 0 (si hay cable de serie) con lo esencial: el mismo resumen que la pantalla */
{
    const DspEstado *e = dsp_estado();
    hw_log(ui_fmt("dsp: %s -> paso %u (%s) error %d (%s) vivo %u ms %u tx %u+%u rx %u+%u ctl %u ventana %u/%u xram %u/%u/%u ev %X dma %u/%u/%u ver \"%s\"\n",
                  que, e->paso, dsp_paso_nombre(e->paso), (i32)e->error, dsp_error_nombre(e->error), e->vivo, e->ms_total, e->tx_cortos, e->tx_largos,
                  e->rx_cortos, e->rx_largos, e->rx_ctrl, e->ventana_pal, e->ventana_esc, e->xram_pet, e->xram_trozos, e->xram_pal, e->ev_visto,
                  e->dma_lanzados, e->dma_tc, e->dma_err, e->version));
}
static void arrancar(Ctx *c, int forzar)
{
    DspDiagE *s = ui_estado(c);
    leer(c);
    if (!s->hay) { ui_aviso("DSP", "No disponible en esta plataforma."); return; }
    if (s->cable && !forzar) { ui_aviso("DSP", "Hay un cable USB puesto. Desenchufalo y vuelve a pulsar 1 (la tecla 5 arranca igualmente, con riesgo de colgar el USB)."); return; }
    sh_actividad();
    dsp_arrancar();
    registrar("arrancar");
    leer(c);
}
static int tecla(Ctx *t, int k, void *dato)
{
    Ctx *c = ui_ancestro(t, &ClsDspDiag);
    DspDiagE *s = ui_estado(c);
    if (k == K_1 || k == K_5) { arrancar(c, k == K_5); return 1; }
    if (k == K_2) {
        if (s->hay && !s->e.gate5) ui_aviso_breve("Primero arranca el DSP (tecla 1)");
        else if (s->hay) { sh_actividad(); dsp_pedir_version(); registrar("version"); }
        leer(c); return 1; }
    if (k == K_3) { if (s->hay) { dsp_apagar(); registrar("apagar"); } leer(c); return 1; }
    if (s->hay && &dsp_exp) {                                               /* (v11) los experimentos: pausa 0/50/250/1000/3000 ms, sondeo corto sí/no, reintentos 1/3/6 */
        static const u16 pausas[5] = { 0, 50, 250, 1000, 3000 };
        int i;
        if (k == K_4) { for (i = 0; i < 5 && pausas[i] != dsp_exp.pausa_ms; i++) ; dsp_exp.pausa_ms = pausas[(i + 1) % 5]; leer(c); return 1; }
        if (k == K_9) { dsp_exp.r2c = (u8)!dsp_exp.r2c; leer(c); return 1; }
        if (k == K_8) { dsp_exp.re1802 = (u8)!dsp_exp.re1802; leer(c); return 1; }
        if (k == K_6) { dsp_exp.sondeo = (u8)!dsp_exp.sondeo; leer(c); return 1; }
        if (k == K_7) { dsp_exp.reintentos = dsp_exp.reintentos == 1 ? 3 : dsp_exp.reintentos == 3 ? 6 : 1; leer(c); return 1; }
    }
    if (k == K_OK || k == K_SEL_IZQ) return 1;
    return sh_teclas(t, k, dato);
}
static Widget *construir(Ctx *c)
{
    DspDiagE *s = ui_estado(c);
    return Teclas(tecla, Pantalla("Diagnóstico DSP", ListaPerezosa(.n = N_FILAS, .fila = fila, .sel = &s->sel, .alto_fila = dis_tokens()->m.alto_fila), "", "Atrás"));
}
COMPONENTE_CON_ESTADO(DspDiag, DspDiagE, construir, iniciar, .desechar = desechar);
Widget *app_dsp_diag(void) { return UI_W(DspDiag); }
