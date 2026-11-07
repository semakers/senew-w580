/* Sistema -> Opciones -> «Banco acel/audio» (2026-10-21): en el aparato, el acelerómetro no se mueve y el altavoz no suena (v3/v4).
 * Esta pantalla enseña EN VIVO lo que separa las hipótesis, sin UART (tmp/acel_re/informe.md §5 E1, tmp/sonido_re/informe.md §6.1):
 *  - el ADC del PMU leído de tres formas: A = 0xA0|canal + pulso (lo de siempre), I = 0x60|canal sin pulso (la forma del 0x6A, que
 *    SÍ funciona), S = 0xA0|canal sin pulso; con la tecla 1 se pone/quita F5000800 bit 14 y con la 2 se alimenta el sensor (P1.1);
 *  - el códec del PMU abierto como «Service tests -> Speaker» (tecla 4) o «-> Earphone» (5), y RELEÍDO: si C0/CD no dicen lo escrito,
 *    el PMU no toma las escrituras; 7 = pops por I²C, 8 = tono del generador F902, 6 = cerrar.
 * La plataforma lo da con hw_banco (débil: solo el W580i). Al salir se cierra el códec y se apaga P1.1.
 * (2026-10-20, tmp/acel_ag) EL ACELERÓMETRO va ahora por hw_acel_diag: el MODO del ADC en uso (lo elige el autodetector de hal/i2c.c
 * entre A P R B I J S, o se fuerza con la tecla 3: A, P, R, B, I, J, S y vuelta a automático), X/Y con ese modo y el RANGO visto (0 lo
 * borra: inclinar el teléfono debe abrirlo), y una fila por modo con lo que dio en la última prueba (canales 1, 8, Y, X; «vN» = cuántas
 * lecturas eran el veneno 6A, o sea, el valor viejo) y su veredicto. 9 repite la prueba ahora. */
#include "shell.h"

typedef struct { Widget base; } BancoHw;
typedef struct { i32 sel; HwBanco b; int hay; HwAcelDiag a; int hay_a; } BancoHwE;
extern const Clase ClsBancoHw;
int  hw_banco(int accion, HwBanco *b) __attribute__((weak));
void hw_banco_salir(void) __attribute__((weak));
int  hw_acel_diag(int accion, HwAcelDiag *d) __attribute__((weak));   /* (acel_ag) el modo del ADC y su autodetector */
#define N_FILAS 21
#define ADC_LETRA(m) ("APRBIJS?"[(m) >= 0 && (m) < HW_ADC_MODOS ? (m) : HW_ADC_MODOS])

static void leer(Ctx *c)
{
    BancoHwE *s = ui_estado(c);
    s->hay = hw_banco && hw_banco(HW_BANCO_LEER, &s->b) == HW_OK;
    s->hay_a = hw_acel_diag && hw_acel_diag(HW_ACEL_DIAG_LEER, &s->a) == HW_OK;
    ui_sucio(c);
}
/* (acel_ag) una fila por modo del ADC: lo que dio en la última prueba (canales 1, 8, Y, X), cuántas lecturas eran el «veneno» y el veredicto */
static const char *const nom_modo[HW_ADC_MODOS] = { "A pulso", "P puerta", "R 72+P", "B b14=0", "I 6n", "J 6n+esp", "S A0 sin" };
static const char *fila_modo(const HwAcelDiag *a, int m)
{
    if (a->v[m][0] < 0 && a->v[m][1] < 0 && a->v[m][2] < 0 && a->v[m][3] < 0) return a->intentos ? "fallo de bus" : "sin probar";
    return ui_fmt("%d %d %d %d v%d %s", a->v[m][0], a->v[m][1], a->v[m][2], a->v[m][3], a->viejos[m],
                  a->vivo[m] == 3 ? "VIVO" : a->vivo[m] ? "a medias" : "muerto");
}
static void iniciar(Ctx *c) { leer(c); ui_cada(c, 400, leer); }
static void desechar(Ctx *c) { (void)c; if (hw_banco_salir) hw_banco_salir(); }
static const char *hx(int v) { return v < 0 ? "--" : ui_fmt("%02X", v); }

static Widget *fila(Ctx *l, int i, int sel, void *dato)
{
    BancoHwE *s = ui_estado(ui_ancestro(l, &ClsBancoHw));
    const HwBanco *b = &s->b;
    const char *n = "", *v = "";
    (void)dato;
    if (!s->hay) return sh_fila(i ? "" : "Banco", i ? "" : "no disponible en esta plataforma", sel);
    switch (i) {
    case 0:  n = "Teclas"; v = "1 b14 2 P1.1 3 modo 9 probar 0 rango"; break;
    case 1:  n = "  más"; v = "4/5 abrir 6 cerrar 7 pops 8 tono"; break;
    case 2:  n = "F5000800 / S16"; v = ui_fmt("%04X / %04X (b14 %s)", b->f5000800 & 0xFFFF, b->syscon16 & 0xFFFF, b->bit14 ? "puesto" : "QUITADO"); break;
    case 3:  n = "P1 / P3 (dir)"; v = ui_fmt("%02X(%02X) %02X(%02X)", b->p1, b->p1dir, b->p3, b->p3dir); break;
    case 4:  n = "Sensor P1.1"; v = b->acel ? "ALIMENTADO" : "por lectura (tecla 2)"; break;
    case 5:  n = "ADC modo";
        if (!s->hay_a) v = "sin diagnóstico";
        else if (s->a.forzado >= 0) v = ui_fmt("%c FORZADO (auto: %c)", ADC_LETRA(s->a.modo), s->a.auto_modo >= 0 ? ADC_LETRA(s->a.auto_modo) : '-');
        else if (!s->a.intentos) v = ui_fmt("%c (sin probar aún)", ADC_LETRA(s->a.modo));
        else v = ui_fmt("%c auto%s, %d prueba%s", ADC_LETRA(s->a.modo), s->a.hallado ? "" : " NINGUNO VIVO", s->a.intentos, s->a.intentos == 1 ? "" : "s");
        break;
    case 6:  n = "Acel X / Y"; v = s->hay_a ? ui_fmt("%d / %d", s->a.x, s->a.y) : "--"; break;
    case 7:  n = "  rango X / Y"; v = s->hay_a ? ui_fmt("%d..%d / %d..%d", s->a.xmin, s->a.xmax, s->a.ymin, s->a.ymax) : "--"; break;
    case 8:  n = "6A / 70 72 73"; v = ui_fmt("%d / %s %s %s", b->l6a, hx(s->a.r70), hx(s->a.r72), hx(s->a.r73)); break;
    case 9:  n = "modo: 1 8 Y X"; v = s->hay_a ? ui_fmt("veneno %d", s->a.veneno[0]) : "--"; break;
    case 10: case 11: case 12: case 13: case 14: case 15: case 16:
        n = nom_modo[i - 10]; v = s->hay_a ? fila_modo(&s->a, i - 10) : "--"; break;
    case 17: n = "Códec"; v = b->codec == 1 ? ui_fmt("ALTAVOZ, %d escr. mal", b->esc_mal) : b->codec == 2 ? ui_fmt("AURICULAR, %d escr. mal", b->esc_mal) : ui_fmt("cerrado (%d escr. mal)", b->esc_mal); break;
    case 18: n = "  C0 C7 C8"; v = ui_fmt("%s %s %s", hx(b->c0), hx(b->c7), hx(b->c8)); break;
    case 19: n = "  CB CC CD"; v = ui_fmt("%s %s %s", hx(b->cb), hx(b->cc), hx(b->cd)); break;
    case 20: n = "Vueltas / lect."; v = ui_fmt("%u / %u (%u mal)", b->vueltas, s->a.lecturas, s->a.fallos); break;
    }
    return sh_fila(n, v, sel);
}
static int tecla(Ctx *t, int k, void *dato)
{
    Ctx *c = ui_ancestro(t, &ClsBancoHw);
    int a = k == K_1 ? HW_BANCO_BIT14 : k == K_2 ? HW_BANCO_ACEL : k == K_4 ? HW_BANCO_CODEC_ABRIR : k == K_5 ? HW_BANCO_AURICULAR :
            k == K_6 ? HW_BANCO_CODEC_CERRAR : k == K_7 ? HW_BANCO_POPS : k == K_8 ? HW_BANCO_TONO : -1;
    if (a >= 0) { if (hw_banco) hw_banco(a, 0); leer(c); return 1; }
    a = k == K_3 ? HW_ACEL_DIAG_SIGUIENTE : k == K_9 ? HW_ACEL_DIAG_PROBAR : k == K_0 ? HW_ACEL_DIAG_RANGO : -1;   /* (acel_ag) */
    if (a >= 0) { if (hw_acel_diag) hw_acel_diag(a, 0); leer(c); return 1; }
    if (k == K_OK || k == K_SEL_IZQ) return 1;
    return sh_teclas(t, k, dato);
}
static Widget *construir(Ctx *c)
{
    BancoHwE *s = ui_estado(c);
    return Teclas(tecla, Pantalla("Banco acel/audio", ListaPerezosa(.n = N_FILAS, .fila = fila, .sel = &s->sel, .alto_fila = dis_tokens()->m.alto_fila), "", "Atrás"));
}
COMPONENTE_CON_ESTADO(BancoHw, BancoHwE, construir, iniciar, .desechar = desechar);
Widget *app_banco_hw(void) { return UI_W(BancoHw); }
