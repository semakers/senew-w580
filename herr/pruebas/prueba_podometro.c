/* prueba_podometro.c -- el contador de pasos (shell/podometro.c) con señales sintéticas: quieto, andar lento y rápido, correr, balanceo suave
 * y golpes aislados, más la clasificación de gestos. `make probar-podometro`. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "podometro.h"
static int correr(const char *n, double amp, double hz, double ruido, int seg, int esperado_min, int esperado_max)
{
    Podometro p; int i, tot = seg * 25; pod_iniciar(&p, POD_CORREGIDO); srand(7);
    for (i = 0; i < tot; i++) {
        double t = i / 25.0;
        int x = (int)(125 + 2 + amp * sin(2 * 3.14159265 * hz * t) + ruido * ((rand() % 2001) / 1000.0 - 1));
        int y = (int)(143 + 0.5 * amp * sin(2 * 3.14159265 * hz * t + 1) + ruido * ((rand() % 2001) / 1000.0 - 1));
        pod_muestra(&p, x, y);
    }
    printf("%-34s pasos=%3u dist=%.1f m kmh=%.1f andando=%d (esperado %d..%d) %s\n", n, p.pasos, p.dist, p.kmh, p.andando, esperado_min, esperado_max,
           (int)p.pasos >= esperado_min && (int)p.pasos <= esperado_max ? "OK" : "FALLA");
    return !((int)p.pasos >= esperado_min && (int)p.pasos <= esperado_max);
}
int main(void)
{
    int f = 0; unsigned char q[10][2], s[10][2], g[10][2]; int i;
    f += correr("quieto con ruido", 0, 1, 1.0, 20, 0, 0);
    f += correr("camina 1,8 pasos/s, 20 s", 12, 1.8, 1.0, 20, 30, 40);
    f += correr("camina lento 1,2 pasos/s, 20 s", 12, 1.2, 1.0, 20, 18, 27);
    f += correr("corre 2,8 pasos/s, 10 s", 25, 2.8, 1.0, 10, 20, 30);
    f += correr("balanceo suave (amp 2)", 2, 1.5, 1.0, 20, 0, 2);
    f += correr("golpes aislados", 20, 0.2, 1.0, 8, 0, 3);
    for (i = 0; i < 10; i++) { q[i][0] = 125; q[i][1] = 143; s[i][0] = i % 2 ? 190 : 60; s[i][1] = i % 2 ? 60 : 200; g[i][0] = i == 5 ? 90 : 125; g[i][1] = 143; }
    printf("gestos: quieto=%d (7) sacudida=%d (6) golpe en X=%d (0)\n", pod_gesto(q), pod_gesto(s), pod_gesto(g));
    if (pod_gesto(q) != 7 || pod_gesto(s) != 6) f++;
    return f;
}
