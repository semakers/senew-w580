/* placa_conf.h del W810i — ver placas/w580i/placa_conf.h. (port810, docs/port_w810.md) */
#ifndef PLACA_CONF_H
#define PLACA_CONF_H
#define PLACA_ANCHO 176                 /* [observado: cap_resx=B0, cap_resy=DC del controlador por omisión del main R4DB005] */
#define PLACA_ALTO  220
/* Lo que el W810 ofrece a una .sen: como el W600 (0x3F6) SIN zumbador (no se ruta el pin: [sin saber] en el W810), SIN tapa
 * (es de barra) y sin luces de GPIO (otro cableado). Queda: ficheros, reloj, color, teclado numérico, joystick (+ el bit 1). */
#define HW_CAPS   0x3E2u
#define HW_MODELO 810
#endif
