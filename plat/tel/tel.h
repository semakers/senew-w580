/* plat/tel/tel.h — (P600, paso 6 de docs/portabilidad.md §3.7) LO COMÚN A TODO TELÉFONO, sobre lo que da cada placa.
 * plat/tel/ no incluye ningún hal.h: lo que necesita del hardware lo pide con las funciones tel_* de abajo, que implementa cada
 * placa (W580i: plat/hw_w580.c; W600i: placas/w600i/hw_w600.c). Hoy: el perro de software y la pulsación larga (perro.c). */
#ifndef TEL_H
#define TEL_H
#include "hw.h"
#include "enc.h"

/* ---- lo que da plat/tel/perro.c ------------------------------------------------------------------------------------------ */
extern EncEstado tel_enc;                 /* la tecla de encendido según api/enc.h: la vigía de la IRQ y lo que la placa lee del PMU */
void tel_perro_vigia(void);               /* desde la IRQ del tic, cada 64 tics (256 ms): pulsación larga, apagado forzado y perro */
void tel_perro_prorroga(void);            /* una operación de flash acabada: prórroga (con el tope de PERRO_TOPE_S sin latido del bucle) */
void tel_tras_nor(void);                  /* en el bucle, recién acabada una operación de flash: el apagado forzado que quedó pendiente */
u32  tel_perro_mordidas(void);
/* (y las de api/hw.h: hw_perro_latido, hw_perro_plazo, hwo_progreso, hw_encendido_largo) */

/* ---- lo que da la placa --------------------------------------------------------------------------------------------------- */
extern volatile u32 tics;                 /* el tic de 4 ms */
u32  irq_cerrar(void);                    /* arranque.S de la placa: devuelve el bit I anterior */
void irq_abrir(void);
int  tel_kbd_masa(int *nivel, int *foto); /* la tecla de encendido (celda 30) desde la IRQ: 1 pulsada, 0 suelta, -1 no se pudo; rc -1 = teclado sin iniciar */
int  tel_pmu_tecla(void);                 /* 0x23 bit 0 leído AHORA (I²C tomado desde la IRQ): 1, 0, o -1 */
int  tel_nor_en_curso(void);              /* 1 = una operación de flash a medias (no se muerde ni se apaga a la fuerza) */
int  tel_en_panico(void);                 /* 1 = ya hay pantalla de fallo */
void tel_latido_placa(void);              /* lo que la placa hace en cada latido del bucle (el W580: el espejo de la flash y la tarjeta) */
void tel_sin_bucle(void);                 /* callar y dejar el PMU seguro desde la IRQ: en_panico, gancho del tic, tono, carga a 0, vibrador */
enum { TEL_FALLO_PERRO = 1 };
void tel_fallo(const char *titulo, const char *detalle, int causa, u32 pc, u32 lr, int reiniciar) __attribute__((noreturn));   /* pantalla de fallo */
void tel_apagado_forzado(void) __attribute__((noreturn));   /* tras tel_sin_bucle: luces y panel fuera, apagar (3 intentos) y, si sigue vivo, reiniciar */
int  cg_perro_irq(void);                  /* shell/cargador.c: si quien no vuelve es una app CARGABLE, se la saca a ella (1) */
#endif
