/* El anfitrión de Memory Stick del DB2020 (hal/ms_db2020.c): lo que ve la plataforma. Pista T. */
#ifndef MS_DB2020_H
#define MS_DB2020_H
#include "ms.h"
typedef struct { u32 irqs, irqs_vacias, det_irqs, cortes, tormentas, pmu_fallos;
                 /* (M2 ag, 2026-10-21) la FOTO del anfitrión para el diagnóstico en el aparato (apps/sistema/tarjeta_m2.c) */
                 u32 sys_serie;          /* +C tras encender (serie, antes de INTEN): el original deja 0x4015 */
                 u32 sys_antes, sys_par; /* +C justo antes y justo después de pasar a paralelo (el original: 0x6015 -> 0x200D) */
                 u32 sta_par, sta_ult;   /* +8 justo después de pasar a paralelo, y el último leído por int_paralelo */
                 u32 pasos_par;          /* veces que se pasó el anfitrión a paralelo */
                 u32 pmu_tension_mal;    /* veces que el PMU no dejó la tensión puesta (0x43 releído sin bits 0..1 = 3) */
                 u8  pmu43, pmu44;       /* el PMU releído al dar tensión (el original: 0x07 y 0x0E) */
                 u16 s40, s1a, s14, s1c; /* SYSCON +40 +1A +14 +1C con la tarjeta recién encendida (el original, en sus TPC: +1C = 0x0401) */
                 u16 s02, s0a;           /* (2026-10-26) y +02 y +0A (los relojes que también abre el USB) */
                 u32 t_drq, t_rdy, t_toe, t_crc, t_escr;   /* (2026-10-26) CÓMO falla un TPC: sin DRQ del FIFO, sin «terminado» en 500 ms, el anfitrión
                                                              da tiempo agotado (TOE), CRC; y de esos, cuántos eran de escribir a la tarjeta */
               } MsDbCuentas;
extern MsDbCuentas ms_db2020_cta;       /* IRQ 76 servidas / sin nada que atender; IRQ del pin; esperas cortadas por el tope de vueltas;
                                           veces que hubo que tapar la IRQ; veces que el PMU no dejó quitar la tensión */
extern u8 ms_db2020_sondeo;             /* RESERVA DIAGNÓSTICA (-DMS_SONDEO=1): 1 = sin IRQ, sondeando +8. No se usa en el sistema */
void ms_db2020_init(void (*latido)(void));   /* una vez, con el tic en marcha; «latido» = lo que hay que llamar en las esperas largas */
#endif
