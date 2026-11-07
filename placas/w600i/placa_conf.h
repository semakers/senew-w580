/* placa_conf.h del W600i — ver placas/w580i/placa_conf.h. */
#ifndef PLACA_CONF_H
#define PLACA_CONF_H
#define PLACA_ANCHO 176                 /* [observado: w600/sistema/ui/ui.h, hal/lcd.c] */
#define PLACA_ALTO  220
/* El cargador de apps (shell/cargador.h): lo que este modelo ofrece a una .sen. Sin acelerómetro ni vibrador
 * (herr/probar_cargables_pc.sh ya lo probaba así con -DHW_CAPS=0x3F6 -DHW_MODELO=600). */
#define HW_CAPS   0x3F6u
#define HW_MODELO 600
#endif
