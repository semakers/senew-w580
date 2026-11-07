/* Temporizador 1 como tic de 4 ms (línea 53, «OSTICK»). docs/hal.md §6. */
#include "hal.h"

volatile u32 tics;

/* Los temporizadores del DB2020 son de RECARGA AUTOMÁTICA [observado por la pista E]: cuentan 0..comparador a
 * 32 768 Hz y vuelven a 0 dando la interrupción; no hay nada que reconocer (basta el intcon) y el comparador
 * NO se reescribe en cada tic. 0x82 = 131 cuentas = 3,998 ms; como el original, se corrige la deriva: un tic
 * de cada 125 dura 140 cuentas (124 x 131 + 140 = 16 384 = medio segundo justo). */
static volatile u8 largo;                        /* reposo profundo: el comparador está puesto a un plazo largo, no al tic */
volatile u8 tic_vencio;
void tic_vigia(void);                            /* plat: testigos de las pilas */
/* El TONO CON PLAZO: si no es 0, el tic en que el zumbador se apaga SOLO. Lo apaga la propia
 * interrupción (una escritura de 16 bits, hal_audio.c), así que una nota dura lo que se pidió con el grano
 * del tic (4 ms) aunque el bucle del sistema esté ocupado pintando una pantalla entera. Medido antes de
 * esto: un clic de 12 ms sonaba entre 9 y 61 ms según lo que tardara el cuadro. */
volatile u32 tic_tono_hasta;
void tono_fin(void);
/* EL GANCHO DEL TIC (pista S7, hw_tic_gancho): una función de la aplicación que late cada 4 ms desde aquí. Lo
 * usa la música por el zumbador: una nota que cuelga del bucle baila lo que tarde en pintarse la pantalla. */
void (*volatile tic_gancho)(void);
/* (2026-10-26) EL TERCER DUEÑO: EL AUDIO. hal/mp3_w580.c lo pone para alimentar al DSP desde aquí (el alimentador del tic: créditos del DSP y
 * registros del colchón en RAM), como los procesos de prioridad 15 del original (docs/re_musica_segundo_plano.md §7). Va el PRIMERO: el anillo del
 * DSP guarda ~0,17 s y lo demás puede esperar. Lo quitan el pánico y el perro, como tic_gancho. */
void (*volatile tic_audio)(void);
/* (1.9, 27-oct) LA CÁMARA: shell/camara_sis.c pone aquí cam_tic (hal/camara_w580.c) mientras una app tiene la cámara abierta: mira CDI+04 una vez
 * por tic y para el flujo en el flanco de «cuadro completo» (8-18 ms en bajo: desde el bucle se perdería). Lo quitan el pánico y el perro. */
void (*volatile tic_camara)(void);
/* (1.10, 27-oct) EL MICRÓFONO: shell/micro_sis.c pone aquí mic_tic (hal/mic_w580.c) mientras alguien graba: vacía el anillo de 10 ms del DSP al del sistema.
 * Lo quitan el pánico y el perro. */
void (*volatile tic_micro)(void);
static void tic(void)
{
    static u32 fase;
    if (largo) { tic_vencio = 1; return; }                             /* el que duerme repone el tic */
    tics++;
    if (tic_tono_hasta && (int)(tics - tic_tono_hasta) >= 0) { tic_tono_hasta = 0; tono_fin(); }
    { void (*a)(void) = tic_audio; if (a) a(); }
    { void (*k)(void) = tic_camara; if (k) k(); }
    { void (*m)(void) = tic_micro; if (m) m(); }
    { void (*g)(void) = tic_gancho; if (g) g(); }
    if (!(tics & 63)) tic_vigia();
    if (++fase == 125) fase = 0;
    if (fase == 124) R32(0xF9010004) = 0x8B;                           /* el periodo que empieza ahora dura 140 */
    else if (fase == 0) R32(0xF9010004) = 0x82;
}

void tic_init(void)
{
    R16(SYSCON + 0x16) &= (u16)~0x0002u;                               /* puerta 30 (no tiene bloque de reset) */
    R32(0xF9010000) &= ~1u;
    intc_init();
    intc_habilita(53, tic);
    R32(0xFB020280) = 0x70;                                            /* ILR 53 como el original: flanco, prioridad 28 [emulador e14] */
    /* (2026-10-19) EN MARCHA PRIMERO Y EL COMPARADOR DESPUÉS, como 0x442368f8 (0x44236940 |= 1, 0x4423694a <- 0x82) [observado en el
     * desensamblado]. Antes iba al revés y en el aparato el tic no entraba [sospecha: el comparador escrito con el temporizador
     * parado no se toma]. En el emulador da igual. */
    R32(0xF9010000) = (R32(0xF9010000) & ~1u) | 1;
    R32(0xF9010004) = 0x82;
    irq_abrir();
    tic_en_marcha = 1;
}

/* Reposo profundo: el temporizador 1 deja de ser el tic y despierta una sola vez dentro de «cuentas» de
 * 32 768 Hz (16 bits: hasta 2 s). Se para y se vuelve a arrancar para que la cuenta empiece de 0
 * [emulador: el modelo de E pone la cuenta a 0 al arrancar; en el aparato, sin saber]. Con las IRQ cortadas. */
void tic_largo(u32 cuentas)
{
    largo = 1; tic_vencio = 0;
    R32(0xF9010000) &= ~1u;
    R32(0xF9010000) |= 1;                                              /* marcha y luego comparador, como tic_init */
    R32(0xF9010004) = cuentas > 0xFFFF ? 0xFFFF : cuentas;
}
void tic_normal(void)
{
    R32(0xF9010000) &= ~1u;
    R32(0xF9010000) |= 1;
    R32(0xF9010004) = 0x82;
    largo = 0;
}
u8 tic_en_marcha;
