/* podometro.c — contador de pasos de senew (versión propia, simple; ver podometro.h). Software puro. */
#include "podometro.h"

#define CERO       125            /* HW_ACEL_CERO: lo que lee el ADC sin aceleración */
#define UMBRAL16   (3 * 16)       /* el umbral, en cuentas ×16: hay que pasar de -UMBRAL a +UMBRAL para dar un cruce */
#define MIN_ENTRE  7              /* iteraciones: 0,28 s */
#define MAX_ENTRE  50             /* 2 s */
#define RACHA      4
#define PARADO_IT  75             /* 3 s sin cruces: ya no se anda */

static int abs_i(int v) { return v < 0 ? -v : v; }

void pod_iniciar(Podometro *p, int modo)
{
    unsigned char *b = (unsigned char *)p; unsigned n;
    for (n = 0; n < sizeof *p; n++) b[n] = 0;
    p->hz = 25; p->modo = (unsigned char)modo; p->primera = 1;
    pod_perfil(p, 1.70f, 70.0f);
}
void pod_perfil(Podometro *p, float estatura_m, float peso_kg) { p->estatura = estatura_m; p->peso = peso_kg; }
void pod_despertar(Podometro *p) { p->primera = 1; p->armado = 0; }
int  pod_sin_muestra(Podometro *p) { (void)p; return 0; }

int pod_muestra(Podometro *p, int x, int y)
{
    int contado = 0, dx16, dy16, u16, gx, gy;
    p->it++;
    if (p->primera) { p->bx16 = x << 4; p->by16 = y << 4; p->ex = p->ey = 0; p->primera = 0; p->armado = 0; p->ult_it = p->it; }
    dx16 = (x << 4) - p->bx16; dy16 = (y << 4) - p->by16;
    p->bx16 += dx16 >> 5; p->by16 += dy16 >> 5;                       /* media lenta: ~1,3 s (la gravedad) */
    p->ex += (abs_i(dx16) - p->ex) >> 4; p->ey += (abs_i(dy16) - p->ey) >> 4;   /* cuánto se mueve cada eje */
    u16 = p->ex >= p->ey ? dx16 : dy16;                               /* la señal: el eje más activo, sin gravedad */
    gx = (p->bx16 >> 4) - CERO; gy = (p->by16 >> 4) - CERO;
    if (gx > 20) gx = 20;
    if (gx < -20) gx = -20;
    if (gy > 20) gy = 20;
    if (gy < -20) gy = -20;
    p->g[0] = (float)gx; p->g[1] = (float)gy;
    p->eje = abs_i(gx) >= 8 && abs_i(gx) >= abs_i(gy) ? 1 : abs_i(gy) >= 8 ? 2 : 0;
    if (p->idle < 0xFFFFu) p->idle++;
    p->inactivo = p->idle >= p->hz;
    if (u16 < -UMBRAL16) p->armado = 1;
    if (p->armado && u16 > UMBRAL16) {                                 /* un cruce hacia arriba */
        unsigned entre = p->it - p->ult_it;
        p->armado = 0; p->ult_it = p->it; p->idle = 0; p->inactivo = 0; p->c++;
        if (entre >= MIN_ENTRE && entre <= MAX_ENTRE) { if (p->racha < 255) p->racha++; } else p->racha = 1;
        if (p->racha >= RACHA) {
            unsigned n = p->andando ? 1u : p->racha;       /* al validar la racha, se cuentan sus pasos atrasados */
            float zancada = 0.415f * p->estatura;
            p->andando = 1; p->pasos += n; p->dist += zancada * (float)n; contado = 1;
            { float v = zancada * (float)p->hz / (float)(entre ? entre : 1) * 3.6f; p->kmh = p->kmh > 0 ? 0.7f * p->kmh + 0.3f * v : v; }
        }
    }
    if (p->idle > PARADO_IT) { p->andando = 0; p->racha = 0; p->kmh = 0; }
    return contado;
}

int pod_gesto(const unsigned char m[10][2])
{
    int i, minx = 255, maxx = 0, miny = 255, maxy = 0, mx = 0, my = 0, rx, ry, dx, dy;
    for (i = 0; i < 10; i++) {
        if (m[i][0] < minx) minx = m[i][0];
        if (m[i][0] > maxx) maxx = m[i][0];
        if (m[i][1] < miny) miny = m[i][1];
        if (m[i][1] > maxy) maxy = m[i][1];
        mx += m[i][0]; my += m[i][1];
    }
    mx /= 10; my /= 10; rx = maxx - minx; ry = maxy - miny;
    if (rx > 40 && ry > 40) return 6;                       /* sacudida: se mueven los dos ejes a fondo */
    if (rx < 8 && ry < 8) return 7;                         /* quieto */
    if (rx >= 20 && rx >= ry) { dx = abs_i(maxx - mx) >= abs_i(minx - mx) ? 1 : 0; return dx; }
    if (ry >= 20) { dy = abs_i(maxy - my) >= abs_i(miny - my) ? 2 : 3; return dy; }
    return 7;
}
