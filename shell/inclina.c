/* inclina.c — del acelerómetro al vector con que se inclina el menú 3D (pista V, docs/menu3d.md §2).
 *
 * El sensor del W580i es analógico, de dos ejes y 8 bits, con el cero hacia 125; lo muestrea el servicio del
 * podómetro (shell/pasos.c) una vez cada 40 ms. Por los umbrales del podómetro original 1 g son 15–25 cuentas
 * [inferido], o sea que inclinar el teléfono 20° son 5–8 CUENTAS: la señal es gruesa y lo que se ve lo decide
 * el filtro. La cadena:
 *
 *   muestra (25 Hz) -> ejes (signo y cruce: AJUSTE) -> paso bajo (tau ~ 110 ms) -> menos «plano» -> zona muerta
 *     -> x ganancia (AJUSTE, 5 niveles) -> tope ±18° -> OBJETIVO -> seguimiento por cuadro (tau ~ 80 ms) -> PINTADO
 *
 *  - «plano» es la postura al abrir el menú (la media de las 4 primeras muestras) y después sigue a la postura
 *    MUY despacio (tau ~ 12 s): quien se queda con el teléfono inclinado ve los iconos volver a su sitio.
 *    sh_inclina_centrar() lo recalibra en el acto.
 *  - la zona muerta (0,75 cuentas, con codo: lo que pasa de ahí empieza en cero) evita que tiemble en la mesa,
 *    y una HOLGURA de ±0,31 cuentas evita que tiemble inclinado: el ruido de ±1 cuenta de un ADC de 8 bits,
 *    filtrado, no llega a salir de ninguna de las dos.
 *  - el objetivo cambia a saltos, 25 veces por segundo; lo pintado lo persigue en CADA cuadro: eso es la
 *    interpolación entre muestras.
 *
 * Todo en enteros. Lo que NO se puede saber aquí: cuántas cuentas da de verdad 1 g, qué eje del sensor es cuál
 * de la carcasa y con qué signo, y si el retardo (~0,2 s entre mover la mano y verlo quieto) molesta. Por eso
 * la ganancia y los ejes son ajustes. */
#include "shell.h"
#include "r3d.h"

static struct {
    i32 lp[2], ref[2];          /* cuentas x256: filtrado y «plano» */
    i32 hol[2];                 /* la holgura: la desviación de «plano» que se da por buena */
    i32 suma[2];
    i32 obj[2], pint[2];        /* unidades de ángulo x16 */
    i16 sal[2];                 /* lo último entregado */
    i16 crudo[2];
    u32 vistas;                 /* n.º de muestras del servicio ya consumidas */
    u32 fase;                   /* ms, para el vaivén */
    u16 n;                      /* muestras desde que se abrió o se recentró */
    u8  abierto, retenido, sensor, ejes, prueba;
} in;

/* grados por cuenta x 4096/360, por nivel de intensidad: 1,0° · 1,5° · 2,2° · 3,2° · 4,5° por cuenta */
static const u8 ganancia[5] = { 11, 17, 25, 36, 51 };
#define ZONA_MUERTA 192         /* 0,75 cuentas */
#define HOLGURA 80              /* 0,31 cuentas */

static void muestra(int x, int y)
{
    i32 v[2], d, a; int k;
    x -= HW_ACEL_CERO; y -= HW_ACEL_CERO;
    if (aj.menu_ejes & 4) { int t = x; x = y; y = t; }
    if (aj.menu_ejes & 1) x = -x;
    if (aj.menu_ejes & 2) y = -y;
    v[0] = x * 256; v[1] = y * 256;
    in.crudo[0] = (i16)x; in.crudo[1] = (i16)y;
    if (in.n < 4) {                                   /* calibrando «plano»: todavía no se mueve nada */
        in.suma[0] += v[0]; in.suma[1] += v[1];
        if (++in.n == 4) for (k = 0; k < 2; k++) { in.ref[k] = in.lp[k] = in.suma[k] / 4; in.suma[k] = 0; }
        return;
    }
    if (in.n < 0xFFFF) in.n++;
    for (k = 0; k < 2; k++) {
        const int g = ganancia[(aj.menu_intensidad >= 1 && aj.menu_intensidad <= 5 ? aj.menu_intensidad : 3) - 1];
        in.lp[k] += ((v[k] - in.lp[k]) * 5) / 16;     /* paso bajo: 5/16 por muestra */
        in.ref[k] += (in.lp[k] - in.ref[k]) / 300;    /* «plano» sigue a la postura, despacísimo */
        d = in.lp[k] - in.ref[k];
        if (d > in.hol[k] + HOLGURA) in.hol[k] = d - HOLGURA;      /* holgura: un vaivén de menos de ±0,3 cuentas no mueve nada, */
        else if (d < in.hol[k] - HOLGURA) in.hol[k] = d + HOLGURA; /* esté el teléfono plano o inclinado (el ruido del ADC) */
        d = in.hol[k];
        d = d > ZONA_MUERTA ? d - ZONA_MUERTA : d < -ZONA_MUERTA ? d + ZONA_MUERTA : 0;
        a = (d * g) / 16;                             /* cuentas x256 -> unidades de ángulo x16 */
        if (a > SH_INCLINA_MAX * 16) a = SH_INCLINA_MAX * 16;
        if (a < -SH_INCLINA_MAX * 16) a = -SH_INCLINA_MAX * 16;
        in.obj[k] = a;
    }
}

static void recalibrar(void) { in.n = 0; in.suma[0] = in.suma[1] = 0; in.obj[0] = in.obj[1] = 0; in.hol[0] = in.hol[1] = 0; }
void sh_inclina_centrar(void) { recalibrar(); }

void sh_inclina_abrir(void)
{
    PasosInfo i;
    in.abierto = 1;
    in.retenido = (u8)(aj.menu_efecto == SH_EFECTO_INCLINA);
    if (in.retenido) sh_pasos_retener_menu(1);
    sh_pasos_info(&i);
    in.vistas = i.muestras;
    recalibrar();
}
void sh_inclina_cerrar(void)
{
    if (in.retenido) sh_pasos_retener_menu(0);
    in.abierto = in.retenido = 0;
}

int sh_inclina_paso(int dt, i16 *x, i16 *y)
{
    int k, cambio = 0, paso;
    const int q = aj.menu_efecto == SH_EFECTO_VAIVEN ? 12 : 6;
    if (dt < 1) dt = 1;
    if (dt > 250) dt = 250;
    if (in.ejes != aj.menu_ejes) { in.ejes = aj.menu_ejes; recalibrar(); }   /* otros ejes: el «plano» de antes ya no vale */
    if (in.prueba) { /* el objetivo lo ha puesto sh_inclina_prueba */ }
    else if (aj.menu_efecto == SH_EFECTO_INCLINA && in.abierto) {
        PasosInfo i;
        sh_pasos_info(&i);
        in.sensor = i.hay_sensor;
        if (i.muestras != in.vistas) { in.vistas = i.muestras; if (i.hay_sensor) muestra(i.x, i.y); }
        if (!i.hay_sensor) in.obj[0] = in.obj[1] = 0;
    } else if (aj.menu_efecto == SH_EFECTO_VAIVEN) {
        /* un balanceo propio, lento y pequeño: dos senos de periodos que no casan (9,1 s y 12,3 s), ±6° y ±4,5°.
         * Lento A PROPÓSITO: cada salto de 1° es un cuadro entero fino (~4 M de instrucciones); así salen ~4 por segundo */
        in.fase = (in.fase + (u32)dt) % 1119300u;          /* (9100 x 123: las dos fases vuelven a cero a la vez) */
        in.obj[0] = (r3d_sen(in.fase % 9100u * 4096u / 9100u) * 68 * 16) >> 14;
        in.obj[1] = (r3d_sen(in.fase % 12300u * 4096u / 12300u + 700u) * 51 * 16) >> 14;
    } else in.obj[0] = in.obj[1] = 0;
    paso = dt * 256 / (dt + 80);                       /* lo pintado persigue al objetivo: tau ~ 80 ms */
    for (k = 0; k < 2; k++) {
        i32 d = in.obj[k] - in.pint[k], m = (d * paso) / 256;
        if (!m && d) m = d > 0 ? 1 : -1;               /* que acabe de llegar */
        in.pint[k] += m;
        d = in.pint[k] / 16;
        /* lo que se entrega va a SALTOS de `paso_sal` (0,5°; el vaivén, 1°), con holgura: medio grado en un icono
         * de 37 píxeles es un cuarto de píxel, no se ve, y así una deriva lenta no repinta el menú 30 veces por segundo */
        if (d > in.sal[k] + q || d < in.sal[k] - q || (!in.obj[k] && d > -q && d < q && in.sal[k])) {
            in.sal[k] = (i16)(!in.obj[k] && d > -q && d < q ? 0 : (d >= 0 ? d + q / 2 : d - q / 2) / q * q); cambio = 1;
        }
    }
    *x = in.sal[0]; *y = in.sal[1];
    return cambio;
}

/* Para las MEDIDAS (plat/main_w580.c, -DMEDIR_M3): fija el objetivo como si lo diera el sensor (en unidades de
 * ángulo) y deja de mirar el sensor; si = 0 lo devuelve. El emulador de medidas no mueve el acelerómetro. */
void sh_inclina_prueba(int si, int x, int y) { in.prueba = (u8)!!si; in.obj[0] = x * 16; in.obj[1] = y * 16; }

void sh_inclina_info(InclinaInfo *i)
{
    int k;
    for (k = 0; k < 2; k++) {
        i->crudo[k] = in.crudo[k]; i->plano[k] = (i16)(in.ref[k] / 16); i->filtrado[k] = (i16)(in.lp[k] / 16);
        i->objetivo[k] = (i16)(in.obj[k] / 16); i->pintado[k] = in.sal[k];
    }
    i->muestras = in.vistas; i->sensor = in.sensor; i->retenido = in.retenido;
}
