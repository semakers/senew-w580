/* La batería en NÚMEROS (pista S7; docs/bateria.md §2, §4.4, §5.4, §8.A). Funciones PURAS, sin hardware: las usan
 * plat/hw_w580.c, plat/hitos_w580.c y plat/hw_pc.c, así que lo que decide si el teléfono arranca o se apaga es EL
 * MISMO código que se prueba en el PC (make probar-energia).
 * Todo sale del firmware original leído por la pista B; NADA se ha medido en un aparato: la calibración de fábrica
 * de ESE teléfono (unidad 0x0009 de su GDFS) puede mover las rectas unas decenas de mV. */
#ifndef BAT_H
#define BAT_H

/* Las dos rectas del original [observado en su código]:
 *  - EN FRÍO (arranque temprano, 0x449CDB04): lectura con 0x70 <- 0x6A, mV = 2785 + 11 + 10,784 x lectura;
 *  - EN MARCHA (HAL_SYSPWR sobre 0x77): mV = 2755 + 10,843 x cuenta (la de por defecto del GDFS: 108 -> 3926). */
static inline int bat_mv_frio(int lectura)   { return 2796 + (lectura * 10784 + 500) / 1000; }
static inline int bat_mv_marcha(int cuenta)  { return 2755 + (cuenta * 10843 + 500) / 1000; }

/* Umbrales (docs/bateria.md §8.A3, §8.A4). */
#define BAT_MV_NO_CREIBLE 2850               /* por debajo, el original da la lectura por no creíble y sigue arrancando */
#define BAT_MV_ARRANQUE   3400               /* por debajo (sin cargador ni USB) el original se niega a arrancar; nosotros también */
#define BAT_MV_AVISO      3500               /* aviso «batería baja» en marcha */
#define BAT_MV_CORTE      3400               /* dos sondeos seguidos por debajo: guardar y apagar (el original aguanta hasta 3250) */

/* LA DECISIÓN DEL ARRANQUE, como pmu_startup_battery_check del original (§4.4):
 *   estado23 = registro 0x23 del PMU (bit 3 cargador, bit 7 USB), o < 0 si el PMU no contesta;
 *   lectura  = la batería en frío (0x6A), o < 0 si no se pudo leer.
 * El original compara con el mV TRUNCADO de su recta; aquí se compara la lectura: 6..56 es 2850 <= mV < 3400. */
enum { BAT_ARR_SEGUIR = 0,                   /* batería bien, nada enchufado */
       BAT_ARR_APAGAR,                       /* 2850 <= mV < 3400 sin cargador ni USB: señal breve y apagar */
       BAT_ARR_NO_CREIBLE,                   /* lectura < 2850 mV (o sin lectura): seguir, con bandera visible */
       BAT_ARR_CARGADOR };                   /* cargador o USB puestos: arrancar directo al aviso «no carga» */
static inline int bat_arranque_decidir(int estado23, int lectura)
{
    if (estado23 >= 0 && (estado23 & 0x88)) return BAT_ARR_CARGADOR;
    if (estado23 < 0 || lectura < 0) return BAT_ARR_NO_CREIBLE;
    if (lectura < 6) return BAT_ARR_NO_CREIBLE;
    if (lectura <= 56) return BAT_ARR_APAGAR;
    return BAT_ARR_SEGUIR;
}

/* El PORCENTAJE por la tabla de capacidad de fábrica del original (guion del GDFS de la imagen;
 * work/b/gdfs_valores_fabrica.txt), con el 0 % en el corte nuestro (3400 mV). Interpolación lineal. */
static inline int bat_pct(int mv)
{
    static const short t[][2] = { {4155,100},{3980,80},{3869,60},{3810,53},{3770,33},{3740,20},{3685,13},{3645,5},{3510,2},{3400,0} };
    int i;
    if (mv >= t[0][0]) return 100;
    for (i = 1; i < (int)(sizeof t / sizeof t[0]); i++)
        if (mv >= t[i][0]) return t[i][1] + (mv - t[i][0]) * (t[i - 1][1] - t[i][1]) / (t[i - 1][0] - t[i][0]);
    return 0;
}
#endif
