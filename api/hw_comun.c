/* Lo de la API que no depende de la plataforma: calendario y la tabla de celdas del teclado
 * (W580i: notas internas del proyecto, docs/hal.md §5). */
#include <string.h>
#include "hw.h"
#include "placa.h"
#include "ui.h"

int hw_dias_mes(int a, int m)
{
    static const u8 d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 30;
    return (m == 2 && (a % 4 == 0 && (a % 100 != 0 || a % 400 == 0))) ? 29 : d[m - 1];
}
int hw_dia_semana(int a, int m, int d)            /* Zeller a la manera de Sakamoto; 0 = lunes */
{
    static const u8 t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 1 || m > 12) return 0;
    if (m < 3) a--;
    return (a + a / 4 - a / 100 + a / 400 + t[m - 1] + d + 6) % 7;
}

u32 hw_fecha_seg(const HwFecha *f)
{
    u32 dias = 0; int a, m;
    if (!f || f->anio < 2000) return 0;
    for (a = 2000; a < f->anio; a++) dias += (a % 4 == 0 && (a % 100 != 0 || a % 400 == 0)) ? 366 : 365;
    for (m = 1; m < f->mes && m <= 12; m++) dias += (u32)hw_dias_mes(f->anio, m);
    dias += f->dia ? f->dia - 1u : 0;
    return ((dias * 24u + f->hora) * 60u + f->min) * 60u + f->seg;
}
void hw_fecha_sumar(HwFecha *f, int minutos)
{
    int m = f->hora * 60 + f->min + minutos, d = f->dia, me = f->mes, a = f->anio;
    while (m < 0) { m += 1440; if (--d < 1) { if (--me < 1) { me = 12; a--; } d = hw_dias_mes(a, me); } }
    while (m >= 1440) { m -= 1440; if (++d > hw_dias_mes(a, me)) { d = 1; if (++me > 12) { me = 1; a++; } } }
    f->hora = (u8)(m / 60); f->min = (u8)(m % 60); f->dia = (u8)d; f->mes = (u8)me; f->anio = (u16)a;
}

/* (P600, paso 3 de docs/portabilidad.md §3.7) LA TABLA DEL TECLADO ES DE LA PLACA: placas/<modelo>/placa.c (Placa.celdas). Aquí
 * solo se consulta. W580i: celda = columna*5 + fila; W600i: celda = fila*5 + columna; las dos con 30..34 a masa. */
static int celdas_n(void) { const Placa *p = placa(); return p->filas * p->columnas + p->a_masa; }
const char *hw_celda_nombre(int c) { return c >= 0 && c < celdas_n() ? placa()->celdas[c].nombre : ""; }
int hw_celda_tecla(int c) { return c >= 0 && c < celdas_n() ? placa()->celdas[c].tecla : 0; }

/* (CARGA) Donde la plataforma no carga la batería (el PC, el W600i): nada. El W580i las da de verdad en plat/hw_w580.c. */
__attribute__((weak)) int hw_carga_paso(int usb_ma) { (void)usb_ma; return HW_NO_DISPONIBLE; }
__attribute__((weak)) void hw_carga_estado(HwCarga *c) { memset(c, 0, sizeof *c); c->estado = HW_NO_DISPONIBLE; }
