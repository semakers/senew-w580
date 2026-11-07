/* Teclado del W580i por sondeo: matriz de 6 COLUMNAS x 5 FILAS + 5 teclas a masa. docs/hal.md §5
 * (HAL del original 0x441ACC64..0x441ACFC8). Cambia respecto al DB2010: dos registros de 32 bits.
 *   +00 control: bits 0..3 antirrebote-1, 4..6 divisor, 7 barrido automático, 8 rearme de la IRQ, 9 IRQ
 *       habilitada, 11; bits 16..21 = columnas a mano (a 0 = excitada); bits 22..26 = filas leídas AHORA
 *       (a 0 = pulsada); bits 27..31 = filas en el momento de la interrupción sin columna excitada.
 *   +04 (solo lectura) barrido automático: bit columna*5 + fila, a 0 = pulsada.
 * Celda = columna*5 + fila (el «scancode» del original); 30..34 = las teclas a masa (work/h2/teclas.tsv).
 * Puerta 21, bloque 17. IRQ 60. */
#include "hal.h"

#define KBD_CTL R32(0xFB010000)
#define KBD_MAT R32(0xFB010004)

void kbd_init(void)
{
    R16(SYSCON + 0x1A) &= (u16)~0x0001u;       /* puerta 21 */
    R16(SYSCON + 0x14) |= 0x0001;              /* bloque 17 */
    KBD_CTL = 0x003F0000 | 0x900 | 2 << 4 | 4; /* 0x924 como el original (antirrebote 5, divisor 2), ninguna columna excitada */
    KBD_CTL |= 0x0080;                         /* barrido automático en marcha entre barridos a mano; sin IRQ todavía (bit 9 lo pone kbd_irq_init) */
    KBD_CTL &= ~0x0200u;
}

/* Interrupción del teclado (línea 60 = banco 2, bit 0): algo ha cambiado. Como FUN_441acfc8, el manejador solo
 * rearma (bit 8 de +00); el barrido se hace fuera. Sirve para salir del reposo profundo al pulsar. */
volatile u32 kbd_irqs;
/* (R3; AUD3-M5) GUARDA CONTRA UNA RÁFAGA: si entre dos barridos del bucle (20 ms) la IRQ 60 salta más de KBD_RAFAGA veces,
 * algo la tiene clavada (el original esquiva con el bit 11 lo que el barrido automático hace con varias teclas: ver kbd_mapa):
 * se TAPA la línea, y el siguiente kbd_mapa la destapa. Sin esto una ráfaga dejaría al sistema sin CPU y a un borrado de
 * flash suspendiéndose sin fin. kbd_rafagas cuenta las veces (debería ser 0). */
#define KBD_RAFAGA 64u
volatile u32 kbd_rafagas;
static volatile u8 irqs_desde_barrido, tapada;
static void kbd_irq(void)
{
    kbd_irqs++; KBD_CTL |= 0x0100;
    if (++irqs_desde_barrido > KBD_RAFAGA && !tapada) { tapada = 1; kbd_rafagas++; intc_tapar(60); }
}

void kbd_irq_init(void)
{
    intc_habilita(60, kbd_irq);
    KBD_CTL |= 0x0200;                         /* interrupción habilitada */
}

/* Barrido a mano (FUN_441ace70): para cada columna, patrón con esa columna a 0, una espera corta y leer las
 * filas en los bits 22..26; con el patrón 0x3F (ninguna) salen las teclas a masa. m[c] bit f = pulsada.
 * Una tecla a masa se ve en todas las columnas: se quita de las seis. Devuelve cuántas hay pulsadas. */
/* (R3; AUD3-M4) EL ASENTAMIENTO: tras cambiar de columna el original espera retardo(0x1D) antes de leer las filas
 * (441ACE7C `movs r0,#0x1d` / 441ACE7E `bl 0x44233818`, en las seis columnas y en la lectura a masa [observado con e_dis.py]).
 * Antes aquí eran 200 vueltas fijas, más o menos la MITAD que el original a 208 MHz: si la fila no se ha asentado, una tecla
 * sale también en la columna siguiente. Ahora es el mismo retardo, que sigue a la frecuencia.
 * (R3; AUD3-M5) CON DOS O MÁS TECLAS el original quita el bit 11 del control (441ACD74: `and` con 0xFFFFF7FF), apaga el
 * barrido automático y sondea A MANO cada 25 ms; cuando no queda ninguna repone el bit 11 (441ACD5C: `orr` 0x800) y el
 * automático [observado: las tres rutinas con e_dis.py; cuándo las llama, en work/export/c/4422.c:14355-14366]. Con UNA tecla
 * no cambia de modo. Aquí igual: `varias` recuerda el modo; en él no se repone el bit 7 (el bucle de la plataforma ya barre a
 * mano cada 20 ms). [sin saber] qué hace mal el barrido automático con varias teclas; se copia la precaución.
 * (AUD6 B6) El original envuelve esos cambios del control entre 0x44233808 / 0x44233810 ([inferido] cortar y abrir las IRQ);
 * aquí la lectura-modificación-escritura va con la IRQ del teclado abierta y su manejador escribe KBD_CTL |= 0x100: la carrera
 * solo puede perder un rearme que esta misma función repone unas líneas más abajo. Inocua; anotada. */
static volatile u8 barriendo;                                    /* (R2) un barrido a mano en curso: la interrupción no toca las columnas */
static u8 varias;                                                /* (R3) 1 = hay (o había hace un momento) dos o más teclas: sin bit 11 ni barrido automático */
int kbd_mapa(u8 m[7])
{
    static const u8 pat[7] = { 0x3E, 0x3D, 0x3B, 0x37, 0x2F, 0x1F, 0x3F };
    int c, f, n = 0;
    barriendo = 1;
    KBD_CTL &= ~0x0080u;                                         /* barrido automático fuera mientras se barre a mano */
    for (c = 0; c < 7; c++) {
        KBD_CTL = (KBD_CTL & 0xFFC0FFFF) | (u32)pat[c] << 16;
        hal_retardo(0x1D);                                       /* como el original */
        m[c] = (u8)(~(KBD_CTL >> 22) & 0x1F);
    }
    /* Las teclas a masa, por los DOS caminos del original: las filas leídas sin ninguna columna excitada (lo de
     * arriba, FUN_441ace70) y la foto de los bits 27..31 que usa su rutina de interrupción (FUN_441acf3c).
     * [emulador] el modelo de la pista E solo las da por los bits 27..31. */
    m[6] |= (u8)(~(KBD_CTL >> 27) & 0x1F);
    for (c = 0; c < 6; c++) m[c] &= (u8)~m[6];
    for (c = 0; c < 7; c++) for (f = 0; f < 5; f++) if (m[c] & (1u << f)) n++;
    if (n >= 2) { varias = 1; KBD_CTL &= ~0x0800u; }             /* (M5) dos o más: fuera el bit 11; el automático se queda apagado */
    else if (!n && varias) { varias = 0; KBD_CTL |= 0x0800; }    /* ninguna: vuelve el bit 11 */
    KBD_CTL |= varias ? 0x0100u : 0x0180u;                       /* rearme; y otra vez automático solo si no se está sondeando a mano */
    barriendo = 0;
    irqs_desde_barrido = 0;
    if (tapada) { tapada = 0; intc_destapar(60); }               /* (M5) la guarda de la ráfaga: se vuelve a escuchar */
    return n;
}
void kbd_destapar(void) { irqs_desde_barrido = 0; if (tapada) { tapada = 0; intc_destapar(60); } }   /* (AUD6 B5) antes de dormir */

/* (R2; auditoría de S7, A3) Las teclas A MASA como NIVEL DE AHORA y como FOTO, para la interrupción del tic (la pulsación larga
 * de la tecla de encendido). El nivel se lee como en el barrido de arriba (ninguna columna excitada, filas en los bits 22..26)
 * y se deja el registro como estaba; si el bucle está a medio barrido NO se tocan las columnas y *nivel = -1 (la vigía
 * siguiente, 256 ms después, lo leerá). La foto (bits 27..31) es una lectura pura. Bit f = fila f pulsada (celda 30 + f).
 * [emulador] el modelo de la pista E solo da las teclas a masa por la foto: el nivel sale siempre 0. */
void kbd_masa(int *nivel, int *foto)
{
    u32 ctl = KBD_CTL;
    *foto = (int)(~(ctl >> 27) & 0x1F);
    if (barriendo) { *nivel = -1; return; }
    KBD_CTL = (ctl & 0xFFC0FF7Fu) | 0x3Fu << 16;                 /* sin barrido automático y sin ninguna columna */
    hal_retardo(0x1D);                                           /* (R3) el mismo asentamiento que el barrido */
    *nivel = (int)(~(KBD_CTL >> 22) & 0x1F);
    KBD_CTL = (ctl & 0xFFFFFEFFu);                               /* como estaba (sin rearmar la IRQ desde aquí) */
}

/* columna*5+fila de la primera tecla pulsada (30..34 = a masa) o -1 */
int kbd_scan(void)
{
    u8 m[7];
    int c, f;
    kbd_mapa(m);
    for (c = 6; c >= 0; c--)
        for (f = 0; f < 5; f++) if (m[c] & (1u << f)) return c * 5 + f;
    return -1;
}
