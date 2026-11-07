/* placa_conf.h del W580i — lo ÚNICO de la placa que el núcleo necesita en tiempo de COMPILACIÓN: lo que da el tamaño de
 * un arreglo estático (docs/portabilidad.md §3.1, regla 4). Todo lo demás es dato de placa() (api/placa.h).
 * Lo encuentra el -Iplacas/$(MODELO) del Makefile; ui/ui.h y api/hw.h lo incluyen. */
#ifndef PLACA_CONF_H
#define PLACA_CONF_H
#define PLACA_ANCHO 240                 /* QVGA vertical [observado: hal/lcd.c, docs/hal.md] */
#define PLACA_ALTO  320
/* El cargador de apps (.sen) toma HW_CAPS, HW_MODELO… de shell/cargador.h, cuyos valores por omisión son los del W580i. */
#endif
