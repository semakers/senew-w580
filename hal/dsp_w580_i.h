/* Lo que comparten hal/dsp_w580.c (el transporte: SYSCON, caché, DMA, buzón, mensajes) y hal/dsp_w580_arr.c (el arranque: ventana, XRAM,
 * versión, apagado). No es API: nadie más lo incluye. */
#ifndef DSP_W580_I_H
#define DSP_W580_I_H
#include "dsp_w580.h"
#include "hal.h"

#define DSPIF   0xFA040000u
#define D32(o)  R32(DSPIF + (o))
#define D16(o)  R16(DSPIF + (o))
#define PL080   0xF2000000u                      /* «ARM_PL080» (pd_dma.c) */
#define PL_CH   4u                               /* el canal que pide el DSPIF: DMA_Alloc(4, …) en 0x44227BC4 [observado, ejecutado] */
#define PL_C(o) R32(PL080 + 0x100u + PL_CH * 0x20u + (o))

/* bits de FA040010 (proc_DSPIF_INT_IntProcess 0x441A7E68 [observado]) */
#define DSP_EV_TX_LEIDO  0x01u                   /* el DSP leyó nuestro mensaje del buzón (cb 7) */
#define DSP_EV_RX        0x02u                   /* mensaje entrante (cb 6) */
#define DSP_EV_ABORTO    0x04u                   /* ARM_MPORTIF_ABORTBIT: error de acceso a la API del DSP (cb 8) */
#define DSP_EV_FIN_TX    0x08u                   /* fin del flujo ARM->DSP (cb 5) */
#define DSP_EV_FIN_RX    0x10u                   /* fin del flujo DSP->ARM (cb 4) */
#define DSP_EV_DMA_ERR   0x40u                   /* ARM_DMA_ERROR (cb 3) */

extern DspEstado dsp_st;

/* tiempo y perro */
u32  dspi_ms(void);                              /* ms a partir de los tics (4 ms de resolución) */
void dspi_latir(void);                           /* perro de software + gancho de la pantalla (como mucho cada 100 ms) */
void dspi_progreso_ya(void);                     /* el gancho ahora */
/* registro de pasos y de mensajes */
void dspi_arranque_ini(void);                    /* pone a 0 el reloj de los pasos */
u32  dspi_desde_inicio(void);                    /* ms desde dspi_arranque_ini() */
void dspi_paso_ini(int p);
int  dspi_paso_fin(int p, int res, u32 extra);   /* anota y devuelve res */
void dspi_msg_apuntar(int rx, int clase, unsigned canal, unsigned n, const u16 *p);
/* SYSCON */
void dspi_sc(u32 off, u16 quitar, u16 poner);    /* leer-modificar-escribir (SIEMPRE escribe, como el original) con las IRQ cerradas */
void dspi_puerta(int id, int abrir);             /* Do_SYSCON_ClockRequest(1, id, modo) o (…,0): ids 2..5 y 26 */
void dspi_bloque(int id, int reset);             /* syscon_block_reset(id, reset): ids 2, 3 y 20 */
void dspi_reloj_dsp(void);                       /* el prólogo de cada mensaje (FUN_4422C668): +2C |= 3 y las puertas 5, 4, 3, 2 */
void dspi_ctl0(void);                            /* FUN_44227CCC(0): antes de cada imagen por la ventana */
void dspi_ctl1(void);                            /* FUN_44227CCC(1): suelta el DSP */
void dspi_mascaras_iniciar(void);                /* +10 leído, +0C &= ~0xDF y los tres clientes */
void dspi_dspif_iniciar(void);                   /* FUN_441A7ED0 */
void dspi_pcm_configurar(void);                  /* FUN_441A7FE8 */
/* caché y DMA */
void dspi_cache_limpiar(const void *p, u32 bytes);   /* a la SDRAM las líneas sucias (y se invalidan): antes de que el DMA LEA */
void dspi_cache_invalidar(const void *p, u32 bytes); /* se tiran las líneas: antes y después de que el DMA ESCRIBA */
void dspi_dma_iniciar(void);                     /* pd_dma al arrancar el teléfono: canales a 0, DMACConfiguration = 1, DMACSync */
int  dspi_dma_lanzar(int hacia_dsp, void *buf, u32 n32, u32 rafaga);   /* 0 = lanzado */
void dspi_dma_parar(void);
int  dspi_dma_estado(void);                      /* 1 = el PL080 dio fin de transferencia del canal 4 (solo se mira, nunca manda) */
/* eventos y buzón */
u32  dspi_eventos(void);                         /* relee +10 (los borra) y devuelve los pendientes acumulados */
int  dspi_esperar_ev(u32 bit, u32 ms);           /* 0 = llegó (y se consume); -1 = plazo */
void dspi_ev_limpiar(void);
int  dspi_buzon_mandar(u16 clase, const u16 *cuerpo, unsigned n);   /* n palabras de cuerpo (con el canal); 0 = bien */
void dspi_buzon_recibir(void);                   /* vacía el buzón del DSP a la cola de mensajes */
int  dspi_esperar_msg(DspMsg *m, u16 canal, u16 orden, u32 ms);    /* 0 = lo hay */
int  dspi_dormido(void);                                       /* 1 = el DSP tiene concedido el reposo (la ventana CUELGA el bus) */
void dspi_servir(u32 ms);                                      /* atiende el buzón y la campanilla del DSP durante ms (sin esperar nada concreto) */
void dspi_cola_vaciar(void);
int  dspi_tx_corto(u16 canal, const u16 *carga, unsigned n);
int  dspi_tx_largo(u16 canal, const u16 *carga, unsigned n);
void dspi_camp_activar(int on);                  /* (2026-10-24) sondear la campanilla de reposo del DSP: sólo con el SO del DSP ya arrancado (tras el último 0x1802) */
/* arranque.c */
void dspi_estado_reiniciar(void);
#endif
