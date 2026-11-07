/* (R2; auditoría de S7, A3) LA TECLA DE ENCENDIDO MANTENIDA: en qué señal se cree, en números y sin hardware, para que la
 * MISMA decisión corra en el teléfono (plat/hw_w580.c) y en la prueba del PC (herr/pruebas/prueba_seguridad_plat.c).
 *
 * El problema: la pulsación larga APAGA (8 s con orden; 11 s a la fuerza, desde la interrupción del tic), y la señal que se
 * miraba desde la interrupción es el bit 27 de FB010000, que según hal/kbd.c es «la fila EN EL MOMENTO DE LA INTERRUPCIÓN del
 * teclado»: una FOTO. [sin saber] cuándo la refresca el aparato. Si se quedara en «pulsada» (p. ej. tras la pulsación con la
 * que se enciende) el teléfono se apagaría a los 8/11 s de cada arranque.
 *
 * Las reglas (cada vía por separado: FOTO = bits 27..31; NIVEL = filas leídas ahora sin columna excitada, bits 22..26, como
 * el barrido de hal/kbd.c; PMU = registro 0x23 bit 0, que solo se puede leer desde el bucle):
 *  1. una vía NO CUENTA hasta que se la ha visto SUELTA al menos una vez desde el arranque (una señal clavada en «pulsada»
 *     desde el principio no arma nada, nunca);
 *  2. «pulsada por el teclado» = alguna vía de teclado ARMADA la da pulsada;
 *  3. si el PMU ha demostrado que por él llega la tecla en ESTE aparato (se le ha visto suelta Y pulsada) y AHORA dice
 *     «suelta», al teclado no se le cree: es la foto, que se ha quedado. Si el PMU dice «pulsada» (y está armado), vale él;
 *  4. el apagado FORZADO (interrupción) exige además que el bucle haya dejado de latir, que no sea el modo seguro, y —si el
 *     PMU es vía probada— que una lectura del PMU hecha en ese momento no diga «suelta» (lo hace plat/). */
#ifndef ENC_H
#define ENC_H
#define ENC_LARGO_VIGIAS 31                        /* 31 x 256 ms = 7,9 s: el shell apaga con orden */
#define ENC_FORZADO_VIGIAS 43                      /* 11 s: apagado forzado desde la interrupción */
#define ENC_SIN_LATIR_TICS 750u                    /* 3 s sin latido del bucle = «la interfaz no responde» */
typedef struct {
    unsigned char foto_suelta, nivel_suelta, pmu_suelta;   /* vista SUELTA alguna vez por esa vía */
    unsigned char pmu_pulsada;                     /* el PMU la ha dado pulsada alguna vez, DESPUÉS de haberla dado suelta */
    unsigned char vigias;                          /* vigías seguidas pulsada por el teclado (vías armadas) */
    unsigned char kbd;                             /* lo que dijo la última vigía: 1 = pulsada (vías armadas) */
    unsigned char descartes;                       /* veces que el PMU desmintió al teclado en un apagado forzado */
} EncEstado;
/* Una vigía (cada 256 ms, desde la interrupción). foto, nivel: 1 = pulsada, 0 = suelta, -1 = no se pudo leer ahora. */
static inline int enc_vigia(EncEstado *e, int foto, int nivel)
{
    int p = 0;
    if (foto == 0) e->foto_suelta = 1;
    if (nivel == 0) e->nivel_suelta = 1;
    if (foto == 1 && e->foto_suelta) p = 1;
    if (nivel == 1 && e->nivel_suelta) p = 1;
    if (p) { if (e->vigias < 255) e->vigias++; } else e->vigias = 0;
    e->kbd = (unsigned char)p;
    return p;
}
/* Lo que dice el PMU (desde el bucle, cada vez que se lee 0x23): pulsada = BIT 0 A 0 (W580 real; el emulador modelaba al revés hasta 2026-10-20). El invocador debe pasar `!(reg & 1)`. */
static inline void enc_pmu(EncEstado *e, int pulsada)
{
    if (!pulsada) e->pmu_suelta = 1;
    else if (e->pmu_suelta) e->pmu_pulsada = 1;
}
static inline int enc_pmu_probado(const EncEstado *e) { return e->pmu_suelta && e->pmu_pulsada; }
/* ¿Está MANTENIDA ahora, a efectos de apagar? pmu_ahora: 1 pulsada, 0 suelta, -1 no se pudo leer. */
static inline int enc_mantenida(const EncEstado *e, int pmu_ahora)
{
    if (pmu_ahora == 1 && e->pmu_suelta) return 1;
    if (pmu_ahora == 0 && enc_pmu_probado(e)) return 0;
    return e->kbd;
}
/* ¿Toca el apagado FORZADO desde la interrupción? sin_latir = tics desde el último latido del bucle. */
static inline int enc_forzar(const EncEstado *e, unsigned sin_latir, int modo_seguro)
{
    return e->vigias >= ENC_FORZADO_VIGIAS && sin_latir >= ENC_SIN_LATIR_TICS && !modo_seguro;
}
#endif
