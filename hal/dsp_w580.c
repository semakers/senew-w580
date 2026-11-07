/* EL DSP DEL W580i, parte 1: el TRANSPORTE (SYSCON, caché, DMA PL080, eventos y buzón del DSPIF, mensajes cortos y largos) y las
 * funciones de estado para la pantalla de diagnóstico. El arranque está en hal/dsp_w580_arr.c. Qué es cada cosa y de dónde sale:
 * hal/dsp_w580.h y tmp/audio_ag/informe.md. Cada bloque cita la función del ORIGINAL (R8BE001) que copia. Todo con tope: nada se cuelga. */
#include "dsp_w580_i.h"

DspEstado dsp_st;
void hw_perro_latido(void) __attribute__((weak));       /* plat/tel/perro.c: el bucle del sistema vive; aquí, en las esperas largas */

/* ---- tiempo, perro y gancho de la pantalla ---------------------------------------------------------------------------------------- */
static void (*gancho)(void);
static u32 gancho_ultimo;
void dsp_progreso(void (*f)(void)) { gancho = f; }
u32 dspi_ms(void) { return tics * 4u; }
#ifdef MP3_MIGAS_FLASH
extern void mp3_miga(unsigned n);
#define GMIGA(n) mp3_miga(n)
#else
#define GMIGA(n) ((void)0)
#endif
void dspi_progreso_ya(void) { if (gancho) { gancho_ultimo = tics; GMIGA(990); gancho(); GMIGA(991); } }
void dspi_latir(void)
{
    if (hw_perro_latido) hw_perro_latido();
    if (gancho && (u32)(tics - gancho_ultimo) >= 25u) dspi_progreso_ya();    /* como mucho cada 100 ms */
}

/* ---- registro de pasos y de mensajes -------------------------------------------------------------------------------------------- */
static DspPasoReg ring_p[DSP_RING_PASOS]; static u32 ring_p_n;
static DspMsgReg  ring_m[DSP_RING_MSGS];  static u32 ring_m_n;
static u32 t_arr0, t_paso0;

void dspi_arranque_ini(void) { t_arr0 = dspi_ms(); t_paso0 = t_arr0; }
u32 dspi_desde_inicio(void) { return dspi_ms() - t_arr0; }
#ifdef MP3_MIGAS_FLASH
extern void mp3_miga(unsigned n);
#define DMIGA(n) mp3_miga(n)
#else
#define DMIGA(n) ((void)0)
#endif
void dspi_paso_ini(int p) { DMIGA(300u + (unsigned)p); dsp_st.paso = (u8)p; t_paso0 = dspi_ms(); dspi_progreso_ya(); }
int dspi_paso_fin(int p, int res, u32 extra)
{
    DspPasoReg *r = &ring_p[ring_p_n % DSP_RING_PASOS];
    u32 ahora = dspi_ms();
    if (p >= 0 && p < DSP_P_N) dsp_st.ms_paso[p] = ahora - t_paso0;
    r->paso = (u8)p; r->res = (signed char)res; r->sec = (u16)ring_p_n; r->ms = ahora - t_arr0; r->extra = extra;
    ring_p_n++;
    if (res < 0 && !dsp_st.error) dsp_st.error = (signed char)res;
    return res;
}
void (*dsp_gancho_msg)(int rx, int clase, unsigned canal, unsigned n, const u16 *p);   /* (pista MP3) cada mensaje COMPLETO: lo pone hal/mp3_w580.c */
void dspi_msg_apuntar(int rx, int clase, unsigned canal, unsigned n, const u16 *p)
{
    DspMsgReg *r = &ring_m[ring_m_n % DSP_RING_MSGS];
    unsigned i;
#ifdef MP3_MIGAS_FLASH
    {   /* rastro en la iRAM (sobrevive a un reinicio de hardware): t[0] = nº de mensajes; por mensaje 2 palabras: (rx<<31 | clase<<16 | canal), (w0<<16 | w3) */
        volatile u32 *t = (volatile u32 *)0xF3001000u; u32 i = t[0] & 127u;
        t[1 + 2 * i] = ((u32)(rx ? 1u : 0u) << 31) | ((u32)(clase & 0xFF) << 16) | (canal & 0xFFFFu);
        t[2 + 2 * i] = (p && n ? ((u32)p[0] << 16) : 0u) | (p && n > 3 ? p[3] : 0u);
        t[0] = t[0] + 1u;
    }
    if (rx && (canal == 0x16u || canal == 0x27u || canal == 0x18u || canal == 0x26u)) {   /* avisos del DSP (errores, DbgPrint): hasta 16 registros de 32 palabras */
        volatile u32 *c = (volatile u32 *)0xF3001800u;
        if (c[0] < 16u) {
            volatile u16 *q = (volatile u16 *)(0xF3001810u + 64u * c[0]);
            unsigned k;
            q[0] = (u16)canal; q[1] = (u16)n; for (k = 0; k < 30u; k++) q[2 + k] = (p && k < n) ? p[k] : 0;
            c[0] = c[0] + 1u;
        }
    }
#endif
    if (dsp_gancho_msg) { DMIGA(910u + (unsigned)(ring_m_n & 63u)); dsp_gancho_msg(rx, clase, canal, n, p); DMIGA(980u); }
    r->rx = (u8)rx; r->clase = (u8)clase; r->canal = (u16)canal; r->n = (u16)n;
    for (i = 0; i < 4; i++) r->p[i] = p && i < n ? p[i] : 0;
    ring_m_n++;
}
int dsp_ultimos_mensajes(DspMsgReg *dst, int max)
{
    int n = ring_m_n < DSP_RING_MSGS ? (int)ring_m_n : DSP_RING_MSGS, i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) dst[i] = ring_m[(ring_m_n - 1 - (u32)i) % DSP_RING_MSGS];
    return n;
}
int dsp_pasos(DspPasoReg *dst, int max)
{
    int n = ring_p_n < DSP_RING_PASOS ? (int)ring_p_n : DSP_RING_PASOS, i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) dst[i] = ring_p[(ring_p_n - 1 - (u32)i) % DSP_RING_PASOS];
    return n;
}

/* ---- SYSCON: puertas de reloj (+18, bit a 0 = abierta) y bloques (+12, bit a 0 = en reset) --------------------------------------- */
void dspi_sc(u32 off, u16 quitar, u16 poner)
{
    u32 e = irq_cerrar();
    R16(SYSCON + off) = (u16)((R16(SYSCON + off) & (u16)~quitar) | poner);
    if (!e) irq_abrir();
}
/* ids de Do_SYSCON_ClockRequest: 5, 4, 3, 2 = los relojes del DSP (+18 bits 2..5), 26 = el PCM (+18 bit 1) [ejecutado: o1.qemu.log 1749..1779, 132135..132144] */
static u16 masc_puerta(int id) { return id == 5 ? 0x04 : id == 4 ? 0x08 : id == 3 ? 0x10 : id == 2 ? 0x20 : id == 26 ? 0x02 : 0; }
void dspi_puerta(int id, int abrir)
{
    u16 m = masc_puerta(id);
    if (!m) return;
    if (abrir) dspi_sc(0x18, m, 0); else dspi_sc(0x18, 0, m);
}
/* ids de syscon_block_reset: 2 = el núcleo del DSP (+12 bit 3), 3 = el DSPIF (+12 bit 2), 20 = el PCM (+12 bit 1) [ejecutado] */
static u16 masc_bloque(int id) { return id == 2 ? 0x08 : id == 3 ? 0x04 : id == 20 ? 0x02 : 0; }
void dspi_bloque(int id, int reset)
{
    u16 m = masc_bloque(id);
    if (!m) return;
    if (reset) dspi_sc(0x12, m, 0); else dspi_sc(0x12, 0, m);
}
/* FUN_4422C668: lo primero que hace cada envío al buzón [ejecutado: o1.qemu.log 175587..175598] */
void dspi_reloj_dsp(void)
{
    dspi_sc(0x2C, 0, 0x0003);                      /* FUN_44236404(0) */
    dspi_puerta(5, 1); dspi_puerta(4, 1); dspi_puerta(3, 1); dspi_puerta(2, 1);
}
/* FUN_44227CCC(0) 0x44227CCC [observado, desensamblado; ejecutado: o1.qemu.log 132131..132148 y 149190..149207] */
void dspi_ctl0(void)
{
    u32 e; u16 v;
    dspi_sc(0x2C, 0, 0x0003);
    dspi_bloque(2, 1);                             /* el núcleo del DSP a reset */
    dspi_puerta(5, 1); dspi_puerta(4, 1); dspi_puerta(3, 1); dspi_puerta(2, 1);
    e = irq_cerrar();                              /* 0x44227D1E: se lee +12 UNA vez; se escribe v & ~0x10 y luego v | 0x10 */
    v = R16(SYSCON + 0x12); R16(SYSCON + 0x12) = (u16)(v & ~0x10u); R16(SYSCON + 0x12) = (u16)(v | 0x10u);
    if (!e) irq_abrir();
}
/* FUN_44227CCC(1) [ejecutado: o1.qemu.log 173022..173048]: el DSP ARRANCA al soltar los bloques 3 y 2 */
void dspi_ctl1(void)
{
    volatile int i;
    dspi_sc(0x2C, 0, 0x0003);
    dspi_bloque(2, 1); dspi_bloque(3, 1);
    dspi_puerta(5, 1); dspi_puerta(4, 1); dspi_puerta(3, 1); dspi_puerta(2, 1);
    for (i = 0; i < 0x2A; i++) { }                 /* las 42 vueltas vacías del original */
    dspi_bloque(3, 0); dspi_bloque(2, 0);
    D32(0x0C) |= 0x18; D32(0x0C) |= 0x03; D32(0x0C) |= 0x44;   /* FUN_441A7E34: se reponen las máscaras de los tres clientes (el reset del bloque 3 las borra) */
}
/* +10 leído (se borra), +0C &= ~0xDF y los tres clientes del DSPIF: mailbox 0x03 (FUN_4422C610), flujos 0x18 (FUN_4421E028) y 0xC4
 * [ejecutado: o1.qemu.log 1781..1789: +0C = 0, 3, 0x1B, 0xDF] */
void dspi_mascaras_iniciar(void)
{
    (void)D32(0x10);
    D32(0x0C) = D32(0x0C) & 0xFFFFFF20u;
    D32(0x0C) |= 0x03; D32(0x0C) |= 0x18; D32(0x0C) |= 0xC4;
}
/* FUN_441A7ED0 [ejecutado: o1.qemu.log 1749..1757]: puerta 5, pulso del bloque 3, +08 = 1, puerta 5 cerrada */
void dspi_dspif_iniciar(void)
{
    dspi_puerta(5, 1);
    dspi_bloque(3, 1); dspi_bloque(3, 0);
    D32(0x08) = 1;
    dspi_puerta(5, 0);
}
/* FUN_441A7FE8 [ejecutado: o1.qemu.log 1759..1780]: el puerto PCM FA020000. El reloj lo da la puerta 26 + el bloque 20 (el USB NO los usa: tmp/usb_re/) */
void dspi_pcm_configurar(void)
{
    u32 e; u16 v;
    dspi_sc(0x12, 0, 0x0002);                      /* bloque 20 */
    dspi_puerta(26, 1);                            /* puerta 26 */
    e = irq_cerrar();
    v = R16(SYSCON + 0x12); R16(SYSCON + 0x12) = (u16)(v & ~0x0002u); R16(SYSCON + 0x12) = (u16)(v | 0x0002u);   /* pulso al bloque 20 */
    if (!e) irq_abrir();
    R16(0xFA02003E) = 1; R16(0xFA02000E) = 0x3000; R16(0xFA020010) = 0x1010;
    R16(0xFA02001E) = 0x0CB2; R16(0xFA020020) = 0x0CB2; R16(0xFA020022) = 0; R16(0xFA020024) = 0;
    R16(0xFA020026) = 1; R16(0xFA020028) = 0x3C; R16(0xFA02002A) = 0x28;
    R16(0xFA020000) = (u16)(R16(0xFA020000) | 0x0200); R16(0xFA020000) = (u16)(R16(0xFA020000) | 0x0001); R16(0xFA02003E) = 0;
}

/* ---- caché: la SDRAM va con caché de escritura diferida (hal/reloj.c). Búferes alineados a 32 y de tamaño múltiplo de 32 -------------- */
/* ARM926: c7,c14,1 = limpiar e invalidar una línea de la caché de datos por dirección; c7,c10,4 = vaciar el búfer de escritura. Con la caché
 * apagada (los hitos) no hacen nada. */
void dspi_cache_limpiar(const void *p, u32 bytes)
{
    u32 a = (u32)p & ~31u, fin = (u32)p + bytes;
#ifdef __arm__                                     /* (en el PC, para la prueba de tmp/audio_ag/prueba, no hay caché que limpiar) */
    for (; a < fin; a += 32) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
#else
    (void)a; (void)fin;
#endif
}
void dspi_cache_invalidar(const void *p, u32 bytes) { dspi_cache_limpiar(p, bytes); }   /* limpiar+invalidar: sin líneas sucias da lo mismo que invalidar */

/* ---- el DMA PL080, canal 4 (pd_dma 0x441A7B14, DMA_Config 0x441A7304, DMA_Start 0x441A7444; estructuras 0x2019BAF8/BB08 [ejecutado]) ---- */
void dspi_dma_iniciar(void)
{
    u32 c;
    for (c = 0; c < 8; c++) { R32(PL080 + 0x08) = 1u << c; R32(PL080 + 0x10) = 1u << c; }   /* TC y errores a 0 [ejecutado: 341..356] */
    R32(PL080 + 0x30) = 1;                         /* DMACConfiguration: E */
    R32(PL080 + 0x34) = R32(PL080 + 0x34) | 0xFFFF;/* DMACSync: sin sincronizar las 16 líneas */
}
/* hacia_dsp: memoria -> FA040020 (control controlado por el DMA, configuración 0xC8C6); si no, FA040020 -> memoria (lo controla el periférico, 0xF0C6
 * [inferido de DMA_Config]). rafaga: código PL080 (0 = 1, 2 = 8). n32: transferencias de 32 bits (1..0xFFE). El canal 4 va con la línea de petición 3. */
int dspi_dma_lanzar(int hacia_dsp, void *buf, u32 n32, u32 rafaga)
{
    u32 ctrl, cfg, bytes = n32 * 4u;
    if (!n32 || n32 > 0xFFEu || ((u32)buf & 3u)) return -1;
    ctrl = 0x80000000u | (1u << 29) | (2u << 21) | (2u << 18) | (rafaga << 15) | (rafaga << 12) | (hacia_dsp ? (1u << 26) : (1u << 27));
    cfg  = hacia_dsp ? 0xC8C6u : 0xF0C6u;
    /* DMA_Config */
    R32(PL080 + 0x08) = 1u << PL_CH; R32(PL080 + 0x10) = 1u << PL_CH;
    PL_C(0x0C) = ctrl;
    PL_C(0x10) = cfg;
    PL_C(0x08) = 0;
    /* la caché (DMA_Start: 0x34 limpia lo que el DMA va a LEER; 0x3C tira lo que el DMA va a ESCRIBIR) */
    if (hacia_dsp) dspi_cache_limpiar(buf, bytes); else dspi_cache_invalidar(buf, bytes);
    /* DMA_Start */
    PL_C(0x0C) = (PL_C(0x0C) & 0xFFFFF000u) | n32;
    PL_C(0x00) = hacia_dsp ? (u32)buf : DSPIF + 0x20u;
    PL_C(0x04) = hacia_dsp ? DSPIF + 0x20u : (u32)buf;
    PL_C(0x10) = (PL_C(0x10) & 0xFFFBFFFFu) | 1u;
    dsp_st.dma_lanzados++; dsp_st.dma_ctrl = ctrl | n32; dsp_st.dma_cfg = cfg | 1u;
    dsp_st.dma_src = hacia_dsp ? (u32)buf : DSPIF + 0x20u; dsp_st.dma_dst = hacia_dsp ? DSPIF + 0x20u : (u32)buf;
    return 0;
}
void dspi_dma_parar(void)
{
    PL_C(0x10) &= ~1u;
    R32(PL080 + 0x08) = 1u << PL_CH; R32(PL080 + 0x10) = 1u << PL_CH;
}
int dspi_dma_estado(void)                          /* solo se MIRA (diagnóstico): nada de lo que se hace depende de ello */
{
    u32 tc = R32(PL080 + 0x14), er = R32(PL080 + 0x18);
    if (er & (1u << PL_CH)) { dsp_st.dma_err++; R32(PL080 + 0x10) = 1u << PL_CH; return -1; }
    if (tc & (1u << PL_CH)) { dsp_st.dma_tc++;  R32(PL080 + 0x08) = 1u << PL_CH; return 1; }
    return 0;
}

/* ---- LA CAMPANILLA DEL DSP (2026-10-24) -------------------------------------------------------------------------------------------
 * El DSP (TI C55x con OSEck) duerme en reposo profundo (ICR = 0x03FF) cuando no tiene nada que hacer, pero antes PIDE PERMISO al ARM: deja 0xFEED2000
 * en su palabra de datos 0x13F14/0x13F15 (la baja = el comando; su nibble alto menos 1 = índice 1..6 de la tabla de comandos), levanta una línea
 * hacia el ARM (la IRQ 17 «DSPIF_GSM_INT» del original) y ESPERA, con sus interrupciones cortadas, a que esa palabra valga 0. Si el ARM no lo hace el
 * DSP se queda mudo: ni tic ni buzón (lo que se veía como «versión -11» desde v8).
 * El original lo atiende en proc_DSPIF_GSM_INT_IntProcess (0x441A7D84, IRQ 17): [gancho previo 0x4421E249: si había soltado los relojes del DSP, los
 * REABRE (+2C |= 3 y las puertas 5,4,3,2)], espera la ventana libre (FA040024 bit 1), lee el par (+28 = 0x13F14, +24 = 2), lo pone a 0 (+28 = 0x13F14,
 * +2C = 0, +24 = 1; SIN tocar SYSCON +12) y despacha el comando; el 2 lo trata 0x4421E289 (concede el reposo y suelta los relojes). Aquí nunca soltamos los
 * relojes, así que basta CONCEDER: poner la palabra a 0. No usamos la IRQ 17 (en el INTC lleva latida desde el arranque y senew no la gestiona): se
 * SONDEA la palabra (marca 0xFEED y comando != 0) cada vez que se miran los eventos, y dsp_servir() lo hace desde el bucle principal. */
#define DSP_PAL_CAMPANILLA 0x13F14u
/* REGLA (2026-10-24, aprendida con un cuelgue de bus): tras CONCEDER el reposo el DSP ejecuta IDLE con ICR = 0x03FF y su ventana (+24/+28/+2C) deja de
 * contestar: leerla CUELGA el bus del ARM hasta sacar la pila. El original lo evita porque tras conceder suelta los relojes del DSPIF (callback 0x4421E289:
 * puertas 5, 4, 3, 2 y +2C &= ~3) y no toca nada hasta el gancho previo 0x4421E249, que los reabre. Aquí no soltamos los relojes, pero sí aplicamos la misma
 * disciplina: `camp_dormido` = concedido y SIN mensaje nuestro pendiente => no se toca ni la ventana ni el DSPIF hasta el siguiente envío (dspi_buzon_mandar).
 * Si al conceder quedaba un mensaje sin leer (+00 bit 0) el DSP despierta solo para leerlo: no queda dormido, pero la ventana no se sondea hasta
 * CAMP_DESPIERTA_MS más tarde (tiempo de despertar). */
#define CAMP_DESPIERTA_MS dsp_camp_despierta_ms
#define CAMP_ARRANQUE_MS dsp_camp_arranque_ms             /* tras soltar el DSP no se toca su ventana hasta que haya acabado de inicializarse (nunca se había leído tan pronto) */
/* (2026-10-25) Interruptores para la prueba contra el ORIGINAL, que atiende la campanilla en la IRQ 17 en µs y lee la ventana ~10 ms tras soltar el DSP
 * (docs/re_mp3_reloj_arm.md §2.1; medido: 7 campanillas al arrancar la reproducción, 0 sonando). Se cambian por el puente ANTES de reproducir; los
 * valores por omisión son los de siempre. La regla anticuelgue (camp_dormido: no tocar la ventana de un DSP al que se le concedió dormir) NO cambia. */
volatile u32 dsp_camp_despierta_ms = 12u, dsp_camp_arranque_ms = 500u;
static u8 camp_activa, camp_dormido;
static u32 camp_desde;
/* anillo de diagnóstico (solo desarrollo): hora, tipo, valor. tipo 1 = sondeo (valor = palabra 0x13F14 leída, 0xFFFFFFFF = ventana fallida), 2 = concedido (valor = pendiente en +00),
 * 3 = envío, 4 = sondeo saltado (valor: 1 = dormido, 2 = en silencio, 3 = inactivo) */
#ifdef MP3_MIGAS_FLASH
static volatile struct { u32 ms, val; } camp_req[48] __attribute__((used)); static volatile u32 camp_req_n __attribute__((used));   /* TODAS las peticiones FEED vistas (hora, palabra) */
static volatile struct { u32 ms, tipo, val; } camp_ring[96] __attribute__((used)); static volatile u32 camp_ring_n __attribute__((used));
#define CLOG(t, v) do { camp_ring[camp_ring_n % 96].ms = dspi_ms(); camp_ring[camp_ring_n % 96].tipo = (t); camp_ring[camp_ring_n % 96].val = (v); camp_ring_n++; } while (0)
#else
#define CLOG(t, v) ((void)0)
#endif                                    /* ms a partir de los cuales se puede volver a sondear la ventana */
static u32 camp_arranque;                                  /* ms antes de los cuales NO se toca la ventana tras soltar el DSP: ningún envío lo acorta (camp_enviado solo mueve camp_desde) */
void dspi_camp_activar(int on) { camp_activa = (u8)on; camp_dormido = 0; camp_desde = dspi_ms(); camp_arranque = dspi_ms() + (on ? CAMP_ARRANQUE_MS : 0u); }
int dspi_dormido(void) { return camp_dormido; }              /* (27-oct) 1 = concedido el reposo y sin mensaje pendiente: NO tocar la ventana hasta mandarle algo */
static int camp_ventana_ok(void)
{
    return camp_activa && !camp_dormido && (int)(dspi_ms() - camp_desde) >= 0 && (int)(dspi_ms() - camp_arranque) >= 0;
}
static void camp_enviado(void)                             /* tras un envío el DSP despierta; la ventana se sondea cuando ya esté despierto */
{
    CLOG(3, 0);
    camp_dormido = 0; camp_desde = dspi_ms() + CAMP_DESPIERTA_MS;
}
static int vent_libre(void)
{
    u32 t;
    for (t = 0; t < 100000u; t++) if (!(D32(0x24) & 2u)) return 0;
    return -1;
}
static int vent_hecha(void)
{
    u32 t;
    for (t = 0; t < 100000u; t++) if (!D32(0x24)) return 0;
    return -1;
}
volatile u32 dsp_camp_pausa;                               /* (2026-10-25) != 0: no se sondea la campanilla (la pone hw_mp3_prueba SOLO mientras suena, con mp3_sin_sondeo_sonando) */
/* (2026-10-26) LA CAMPANILLA DESDE EL TIC: el alimentador (hal/mp3_w580.c) pone dsp_en_tic mientras trabaja dentro de la interrupción. Con dsp_camp_tic = 1
 * (por omisión) la atiende también allí, con la MISMA regla anticuelgue (camp_ventana_ok), y como mucho UNA lectura de la ventana por tic (los eventos se
 * miran varias veces: recibir, lanzar, mandar); con 0 solo el bucle. El tic y el bucle nunca están a la vez
 * dentro del transporte (el cerrojo dsp_bucle), así que una lectura de la ventana del bucle no se corta a medias por otra del tic. dsp_camp_en_tic cuenta
 * las concedidas desde el tic. */
volatile u32 dsp_camp_tic = 1, dsp_camp_en_tic;
volatile u8 dsp_en_tic;
static void dspi_campanilla(void)
{
    u32 v;
    if (dsp_camp_pausa) return;
    if (dsp_en_tic) { if (!dsp_camp_tic || dsp_en_tic > 1) return; dsp_en_tic = 2; }   /* desde el tic, la ventana UNA vez por tic como mucho */
#ifdef MP3_MIGAS_FLASH
    if (!dsp_st.gate5 || !camp_ventana_ok()) { if (dsp_st.gate5) { static u32 ult4 = 99; u32 q4 = camp_dormido ? 1u : camp_activa ? 2u : 3u; if (q4 != ult4 || camp_ring[(camp_ring_n + 95u) % 96u].tipo != 4u) { CLOG(4, q4); ult4 = q4; } } return; }   /* solo las TRANSICIONES: si no, inunda el anillo */
#else
    if (!dsp_st.gate5 || !camp_ventana_ok()) return;
#endif
    if (vent_libre()) { dsp_st.camp_mal++; CLOG(1, 0xFFFFFFFFu); return; }
    D32(0x28) = DSP_PAL_CAMPANILLA; D32(0x24) = 2;
    if (vent_hecha()) { dsp_st.camp_mal++; CLOG(1, 0xFFFFFFFEu); return; }
    v = D32(0x2C);
    CLOG(1, v);
    if ((v >> 16) != 0xFEEDu || !(v & 0xFFFFu)) return;                    /* ninguna petición pendiente */
#ifdef MP3_MIGAS_FLASH
    camp_req[camp_req_n % 48].ms = dspi_ms(); camp_req[camp_req_n % 48].val = v; camp_req_n++;
#endif
    D32(0x28) = DSP_PAL_CAMPANILLA; D32(0x2C) = 0; D32(0x24) = 1;
    if (vent_hecha()) { dsp_st.camp_mal++; return; }
    dsp_st.campanillas++; dsp_st.camp_marca = (u16)(v >> 16); dsp_st.camp_cmd = (u16)v;
    if (dsp_en_tic) dsp_camp_en_tic++;
    CLOG(2, D32(0x00) & 1u);
    if (D32(0x00) & 1u) camp_desde = dspi_ms() + CAMP_DESPIERTA_MS;      /* queda un mensaje nuestro sin leer: el DSP despierta a por él */
    else { camp_dormido = 1; dsp_st.camp_dormido++; }                      /* sin trabajo: se duerme de verdad */
}

/* ---- eventos de FA040010 (proc_DSPIF_INT_IntProcess 0x441A7E68: se relee hasta que da 0; leerlo borra) ------------------------------- */
static u32 ev_pend;
/* (2026-10-25) cronómetro del transporte (cuentas del temporizador 1, ~30,5 µs): [0] leer FA040010, [1] campanilla (ventana), [2] leer el buzón
 * (32 palabras + acuse), [3] acuses 0x1101, [4] llamadas a dspi_buzon_recibir, [5] mensajes leídos */
volatile u32 dsp_crono[6];
extern volatile u32 tics;
static u32 dcrono(void) { u32 a, c, b; do { a = tics; c = *(volatile u32 *)0xF901001Cu & 0xFFu; b = tics; } while (a != b); return a * 131u + c; }
u32 dspi_eventos(void)
{
    u32 v, c0 = dcrono(), c1; int i, b;
    for (i = 0; i < 8 && ((v = D32(0x10)) & 0xFFFFu); i++) {
        v &= 0xFFFFu; ev_pend |= v; dsp_st.ev_visto |= v;
        for (b = 0; b < 8; b++) if (v & (1u << b)) dsp_st.ev[b]++;
        if (v & 0xFF00u) dsp_st.ev_otros++;
    }
    c1 = dcrono(); dsp_crono[0] += c1 - c0;
    dspi_campanilla();                                       /* ¿pidió el DSP dormir? Él espera con las interrupciones cortadas hasta que le contestemos */
    dsp_crono[1] += dcrono() - c1;
    return ev_pend;
}
void dspi_ev_limpiar(void) { dspi_eventos(); ev_pend &= ~(DSP_EV_FIN_TX | DSP_EV_FIN_RX); }
void dspi_drenar_cortos(void);
static u32 tx_vueltas;
int dspi_esperar_ev(u32 bit, u32 ms)
{
    u32 t0 = dspi_ms(), vueltas = 0;
    for (;;) {
        dspi_eventos();
        if (ev_pend & bit) { ev_pend &= ~bit; return 0; }
        if ((u32)(dspi_ms() - t0) >= ms || ++vueltas > ms * 8u + 64u) return -1;
        dspi_latir();
        dspi_drenar_cortos();                                  /* (2026-10-24) mientras dura un envío largo, los créditos del DSP no pueden esperar */
        tx_vueltas++;
        espera_ms(1);
    }
}

/* ---- el buzón (0x441A7F30 escribir, 0x441A7F80 mandar, 0x441A7F98 leer; 0x4422C668 la cabecera) [ejecutado: o1.qemu.log 175598..175640] ---- */
volatile u32 dsp_buzon_lleno_n, dsp_buzon_lleno_ms, dsp_buzon_lleno_canal;  /* (2026-10-25) veces que el DSP no leyó el buzón a tiempo, hora de la última */
/* (2026-10-25, música sólida) cuántas veces el buzón solo se vació tras CONCEDER la campanilla aquí dentro, cuántas hubo que esperar a que se abriera la
 * ventana de arranque, y la espera más larga (ms) */
volatile u32 dsp_buzon_camp_n, dsp_buzon_arranque_n, dsp_buzon_espera_max;
#define BUZON_PLAZO 25u                                /* ms de espera con la ventana del DSP disponible */
#define BUZON_PLAZO_ARRANQUE 700u                      /* tope dentro de la ventana de arranque (CAMP_ARRANQUE_MS = 500 por omisión) */
static int buzon_libre(void)                       /* +00 bit 0 = hay un mensaje nuestro que el DSP aún no ha leído */
{
    u32 i, t0, t, camp0 = dsp_st.campanillas;
    u8 arranque = 0;
    for (i = 0; i < 4000u; i++) if (!(D32(0x00) & 1u)) return 0;
    /* (2026-10-25) Si el DSP pidió dormir (campanilla, 0xFEED2000) gira con las interrupciones CERRADAS hasta que el ARM se lo conceda, y mientras tanto
     * NO lee el buzón (docs/re_mp3_reloj_dsp.md §4 bis): esperar sin atenderla solo podía acabar en −9 (el −23/−9 de cada arranque del MP3: el 0x1101 y
     * el primer R_n de mx_boot salen dentro de los 500 ms tras soltar el DSP, cuando la regla anticuelgue no deja sondear). Aquí se atiende con la MISMA
     * regla (dspi_campanilla mira camp_ventana_ok) y, si la ventana aún está cerrada por el arranque, se espera a que se abra en vez de rendirse. */
    t0 = dspi_ms();
    for (;;) {
        espera_ms(1);
        if (!(D32(0x00) & 1u)) break;
        dspi_campanilla();
        if (!(D32(0x00) & 1u)) break;
        t = dspi_ms() - t0;
        if (camp_activa && (int)(dspi_ms() - camp_arranque) < 0) { arranque = 1; if (t < BUZON_PLAZO_ARRANQUE) continue; }
        if (t >= BUZON_PLAZO + (arranque ? 30u : 0u)) {
            dsp_buzon_lleno_n++; dsp_buzon_lleno_ms = dspi_ms();
            return -1;
        }
    }
    t = dspi_ms() - t0;
    if (t > dsp_buzon_espera_max) dsp_buzon_espera_max = t;
    if (dsp_st.campanillas != camp0) dsp_buzon_camp_n++;
    if (arranque) dsp_buzon_arranque_n++;
    return 0;
}
int dspi_buzon_mandar(u16 clase, const u16 *cuerpo, unsigned n)
{
    unsigned i;
    if (n < 1 || n > 31) return DSP_E_PARAM;
    dspi_eventos();                                                    /* si el DSP pidió dormir y no lo hemos atendido, no leerá nada: antes de mandar */
    dspi_reloj_dsp();
    if (buzon_libre()) return DSP_E_BUZON;
    D32(0x08) = 1;
    D16(0x40) = (u16)((n << 11) | (clase & 0x7FFu));                 /* la cabecera: (n << 11) | clase */
    if (buzon_libre()) return DSP_E_BUZON;
    D32(0x08) = 1;
    for (i = 0; i < n; i++) D16(0x42 + 2 * i) = cuerpo[i];             /* el cuerpo */
    if (buzon_libre()) return DSP_E_BUZON;
    D32(0x00) = 1;                                                     /* y se manda */
    camp_enviado();
    return 0;
}
/* (2026-10-26) EL MISMO ENVÍO SIN ESPERAR, para el tic: si el DSP aún no ha leído nuestro mensaje anterior (+00 bit 0) no se toca nada y se devuelve
 * DSP_E_BUZON (se reintenta en el tic siguiente). Lo demás, en el orden de dspi_buzon_mandar; las comprobaciones de en medio solo giran un momento. */
static int buzon_ya_libre(void) { u32 i; for (i = 0; i < 4000u; i++) if (!(D32(0x00) & 1u)) return 0; return -1; }
static int buzon_mandar_ya(u16 clase, const u16 *cuerpo, unsigned n)
{
    unsigned i;
    if (n < 1 || n > 31) return DSP_E_PARAM;
    dspi_eventos();
    if (D32(0x00) & 1u) return DSP_E_BUZON;
    dspi_reloj_dsp();
    D32(0x08) = 1;
    D16(0x40) = (u16)((n << 11) | (clase & 0x7FFu));
    if (buzon_ya_libre()) return DSP_E_BUZON;
    D32(0x08) = 1;
    for (i = 0; i < n; i++) D16(0x42 + 2 * i) = cuerpo[i];
    if (buzon_ya_libre()) return DSP_E_BUZON;
    D32(0x00) = 1;
    camp_enviado();
    return 0;
}

/* ---- la cola de mensajes recibidos --------------------------------------------------------------------------------------------- */
#define COLA_N 64
static DspMsg cola[COLA_N]; static u8 cola_n;
void dspi_cola_vaciar(void) { cola_n = 0; ev_pend = 0; }
/* (2026-10-26) EL FILTRO de lo recibido: lo pone hal/mp3_w580.c para contar los CRÉDITOS de su flujo (y el 0x0E04) en cuanto se leen del buzón, los lea
 * el tic o el bucle; lo que se queda (devuelve 1) no entra en la cola. Como la IRQ 9 del original, que suma el crédito al leerlo. */
int (*dsp_filtro_rx)(unsigned canal, const u16 *d, unsigned n);
static void cola_poner(unsigned canal, unsigned clase, const u16 *d, unsigned n, int largo)
{
    DspMsg *m;
    unsigned i;
    if (clase == 0x0A && dsp_filtro_rx && dsp_filtro_rx(canal, d, n)) return;
    if (cola_n >= COLA_N) {                          /* llena: se tira el más viejo */
        for (i = 1; i < COLA_N; i++) cola[i - 1] = cola[i];
        cola_n = COLA_N - 1; dsp_st.rx_perdidos++;
    }
    m = &cola[cola_n++];
    m->canal = (u16)canal; m->clase = (u8)clase; m->largo = (u8)largo; m->recortado = n > DSP_MSG_MAX;
    m->n = (u16)(n > DSP_MSG_MAX ? DSP_MSG_MAX : n);
    for (i = 0; i < m->n; i++) m->d[i] = d[i];
}
static int cola_tomar(DspMsg *dst, unsigned canal, unsigned orden)
{
    unsigned i, j;
    for (i = 0; i < cola_n; i++) {
        if (canal != 0xFFFFu && cola[i].canal != canal) continue;
        if (orden && (!cola[i].n || cola[i].d[0] != orden)) continue;
        *dst = cola[i];
        for (j = i + 1; j < cola_n; j++) cola[j - 1] = cola[j];
        cola_n--;
        return 1;
    }
    return 0;
}

/* la recepción LARGA (FUN_4421DB1C 0x4421DB1C, código 2 de la clase 0x14): el DSP manda {2, canal, n, dir alta, dir baja, anchura}; el ARM prepara
 * el DMA (DSPIF -> memoria, ráfaga 8), pone +18 = dir, +1C = n/2 y +14 |= 1, y espera el evento 4 [observado; sin ejecutar: el emulador no tiene DSP] */
#define RX_LARGO_MAX 1024
static u16 b_rx[RX_LARGO_MAX] __attribute__((aligned(32)));
static int rx_largo(const u16 *c, unsigned nc)
{
    unsigned canal, n, ancho;
    u32 dir;
    if (nc < 6) return DSP_E_RX_LARGO;
    canal = c[1]; n = c[2]; dir = ((u32)c[3] << 16) | c[4]; ancho = c[5] & 0xFFu;
    if (n < 2 || n > RX_LARGO_MAX || (n & 1u) || (ancho != 1 && ancho != 4 && ancho != 8)) return DSP_E_RX_LARGO;
    dspi_ev_limpiar();
    if (ancho == 1) D32(0x14) &= ~0x18u; else if (ancho == 4) D32(0x14) = (D32(0x14) & ~0x18u) | 0x10u; else D32(0x14) |= 0x18u;   /* FUN_441A7CC4 */
    if (dspi_dma_lanzar(0, b_rx, n / 2, 2)) return DSP_E_RX_LARGO;
    D32(0x18) = dir & 0x7FFFFFu; D32(0x1C) = n / 2; D32(0x14) |= 1;     /* FUN_441A7D1C */
    if (dspi_esperar_ev(DSP_EV_FIN_RX, 500)) { dspi_dma_parar(); return DSP_E_RX_LARGO; }
    dspi_dma_estado();
    dspi_cache_invalidar(b_rx, n * 2u);
    dsp_st.rx_largos++;
    dspi_msg_apuntar(1, 0x0A, canal, n - 1, b_rx);
    cola_poner(canal, 0x0A, b_rx, n - 1, 1);               /* la carga es n - 1 palabras: la última del búfer es el canal */
    return 0;
}

/* (2026-10-24) El original contesta 0x1101 por el canal 0x16 a CADA aviso 0x1102 del DSP (callback del canal 0x16 -> 446A.c:626: FUN_446a0348 lo imprime y
 * FUN_446a031c manda el 0x1101 = «recibido, manda el siguiente»); sin ese acuse el DSP los deja en su cola. dsp_acusar_avisos = 1 lo hace (por omisión desde el 2026-10-25).
 * dsp_aviso[k] = {ms, w1..w5} de los últimos 32 avisos recibidos. */
volatile u32 dsp_acusar_avisos = 1, dsp_avisos_n, dsp_acuses_n;   /* (2026-10-25) POR OMISIÓN 1, como el original: sin acuse los avisos se quedan en la cola del DSP (5 de 10) */
volatile u16 dsp_aviso[32][8];
static u8 acuse_pend;
static void apuntar_aviso(const u16 *w, unsigned n)
{
    if (w[n] == 0x16u && n >= 2 && w[1] == 0x1102u) {
        volatile u16 *a = dsp_aviso[dsp_avisos_n++ % 32u]; unsigned k;
        a[0] = (u16)dspi_ms(); a[1] = (u16)(dspi_ms() >> 16); for (k = 0; k < 6; k++) a[2 + k] = (u16)(k + 2 < n ? w[k + 2] : 0);
        acuse_pend++;
    }
}
/* FUN_4422C5A0 (cb 6): vaciar el buzón del DSP. Clase 0x0A = mensaje de un canal; clase 0x14 = control de flujos (aquí, recepción larga) */
void dspi_buzon_recibir(void)
{
    int guarda;
    if (!dsp_st.gate5) return;
    dsp_crono[4]++;
    dspi_eventos();
    for (guarda = 0; guarda < 16 && (D32(0x04) & 0xFFFFu); guarda++) {
        u16 w[32];
        unsigned i, clase, n;
        u32 cb = dcrono();
        D32(0x08) = 1;
        for (i = 0; i < 32; i++) w[i] = D16(0x80 + 2 * i);
        D32(0x04) = 0xFFFFFFFEu;
        dsp_crono[2] += dcrono() - cb; dsp_crono[5]++;
        clase = w[0] & 0x7FFu; n = w[0] >> 11; if (n > 31) n = 31;
        if (clase == 0x0A && n >= 1) {
            dsp_st.rx_cortos++;
            apuntar_aviso(w, n);
            dspi_msg_apuntar(1, 0x0A, w[n], n - 1, w + 1);
            cola_poner(w[n], 0x0A, w + 1, n - 1, 0);
        } else if (clase == 0x14 && n >= 1) {
            dsp_st.rx_ctrl++;
            dspi_msg_apuntar(1, 0x14, w[2], n, w + 1);
            if ((w[1] & 0xFFu) == 2) { if (rx_largo(w + 1, n)) dsp_st.rx_perdidos++; }    /* código 2 = recepción larga: se hace y el mensaje llega por su canal */
            else cola_poner(0xC000u | clase, clase, w + 1, n, 0);                          /* cualquier otro código de control: se entrega tal cual */
        } else {
            dsp_st.rx_perdidos++;
            dspi_msg_apuntar(1, (int)(clase & 0xFF), 0, n, w + 1);
            cola_poner(0xC000u | clase, clase, w + 1, n, 0);                                /* clase desconocida: tampoco se pierde */
        }
    }
    if (acuse_pend) {
        acuse_pend--;
        if (dsp_acusar_avisos) { static const u16 a1101[6] = { 0x1101, 0, 0, 0, 0, 0 }; u32 ca = dcrono(); if (dspi_tx_corto(0x16, a1101, 6) == 0) dsp_acuses_n++; dsp_crono[3] += dcrono() - ca; }
    }
}
/* (2026-10-24) VACIADO RÁPIDO del buzón DSP->ARM. El DSP tiene UN solo mensaje en vuelo hacia el ARM y no manda el siguiente hasta que se acusa; detrás guarda
 * una cola de 13 y, si está llena, TIRA el mensaje (error de SO 0x80000400; la recursión error -> informe -> cola llena acaba matando al DSP). Cada lectura de su
 * anillo de entrada es un crédito, así que con el sondeo lento de senew se perdían créditos (el «70 %») y el anillo se vaciaba. El original lo vacía en la IRQ 9
 * (proc_DSPIF_INT_IntProcess 0x441A7E68). Aquí: leer y acusar en cuanto estén SOLO los mensajes cortos (clase 0x0A); los de control (clase 0x14, recepción larga por
 * DMA) se dejan a dspi_buzon_recibir (no se puede lanzar un DMA de recepción con uno de envío en curso). dsp_drenaje_rapido = 0: como antes. */
volatile u32 dsp_drenaje_rapido, dsp_drenados;
void dspi_drenar_cortos(void)
{
    int guarda;
    u32 v;
    if (!dsp_drenaje_rapido || !dsp_st.gate5) return;
    for (guarda = 0; guarda < 8 && ((v = D32(0x10)) & 0xFFFFu); guarda++) {      /* los eventos, como la IRQ 9 del original (leer borra); SIN el sondeo de la campanilla */
        int b;
        v &= 0xFFFFu; ev_pend |= v; dsp_st.ev_visto |= v;
        for (b = 0; b < 8; b++) if (v & (1u << b)) dsp_st.ev[b]++;
    }
    if (!(ev_pend & DSP_EV_RX)) return;                                           /* sin «mensaje entrante» no se toca el buzón */
    ev_pend &= ~DSP_EV_RX;
    for (guarda = 0; guarda < 16 && (D32(0x04) & 0xFFFFu); guarda++) {
        u16 w[32];
        unsigned i, clase, n;
        D32(0x08) = 1;
        w[0] = D16(0x80); clase = w[0] & 0x7FFu; n = w[0] >> 11; if (n > 31) n = 31;
        if (clase != 0x0A || n < 1) { ev_pend |= DSP_EV_RX; return; }            /* de control: lo recoge dspi_buzon_recibir */
        for (i = 1; i < 32; i++) w[i] = D16(0x80 + 2 * i);
        D32(0x04) = 0xFFFFFFFEu;
        dsp_st.rx_cortos++; dsp_drenados++;
        apuntar_aviso(w, n);
        dspi_msg_apuntar(1, 0x0A, w[n], n - 1, w + 1);
        cola_poner(w[n], 0x0A, w + 1, n - 1, 0);
    }
}
/* (2026-10-26) LA RECEPCIÓN DESDE EL TIC (el alimentador de hal/mp3_w580.c, sin el bucle dentro del transporte): eventos (y la campanilla, con su regla),
 * los mensajes CORTOS del buzón a la cola —los créditos se los queda el filtro— y el acuse 0x1101 pendiente si el buzón está libre. Nada que espere:
 * un mensaje de control (clase 0x14: la recepción larga por DMA, que espera su evento) se deja en el buzón y se devuelve 1 para que lo recoja el bucle.
 * Sin envío largo en vuelo (el que llama lo mira antes). */
static int tx_corto_ya(u16 canal, const u16 *carga, unsigned n);
static u8 tx_vuelo;
volatile u32 dsp_tic_control;                            /* mensajes de control que el tic dejó al bucle */
int dspi_tic_recibir(void)
{
    int guarda;
    if (!dsp_st.gate5) return 0;
    dspi_eventos();
    for (guarda = 0; guarda < 16 && (D32(0x04) & 0xFFFFu); guarda++) {
        u16 w[32];
        unsigned i, clase, n;
        D32(0x08) = 1;
        w[0] = D16(0x80); clase = w[0] & 0x7FFu; n = w[0] >> 11; if (n > 31) n = 31;
        if (clase != 0x0A || n < 1) { dsp_tic_control++; return 1; }
        for (i = 1; i < 32; i++) w[i] = D16(0x80 + 2 * i);
        D32(0x04) = 0xFFFFFFFEu;
        dsp_st.rx_cortos++; dsp_crono[5]++;
        apuntar_aviso(w, n);
        dspi_msg_apuntar(1, 0x0A, w[n], n - 1, w + 1);
        cola_poner(w[n], 0x0A, w + 1, n - 1, 0);
    }
    if (acuse_pend && !tx_vuelo) {
        if (!dsp_acusar_avisos) acuse_pend--;
        else { static const u16 a1101[6] = { 0x1101, 0, 0, 0, 0, 0 }; if (tx_corto_ya(0x16, a1101, 6) == 0) { acuse_pend--; dsp_acuses_n++; } }
    }
    return 0;
}
void dspi_servir(u32 ms)
{
    u32 t0 = dspi_ms();
    while ((u32)(dspi_ms() - t0) < ms) { dspi_buzon_recibir(); dspi_latir(); espera_ms(1); }
}
int dspi_esperar_msg(DspMsg *m, u16 canal, u16 orden, u32 ms)
{
    u32 t0 = dspi_ms(), vueltas = 0;
    for (;;) {
        dspi_buzon_recibir();
        if (cola_tomar(m, canal, orden)) return 0;
        if ((u32)(dspi_ms() - t0) >= ms || ++vueltas > ms * 8u + 64u) return -1;
        dspi_latir();
        espera_ms(1);
    }
}

/* ---- mensajes que salen -------------------------------------------------------------------------------------------------------- */
/* tamaño del cuerpo (con el canal) que el original reserva para `w` palabras (FUN_4421DA9C): por encima de 30 se redondea a múltiplos de 16 palabras
 * porque viaja por DMA [ejecutado: 101 palabras de carga + canal -> 112, el 0x1814 {2, 2, 0x70} de o1.qemu.log] */
static u32 tam_cuerpo(u32 w)
{
    if (w > 30) { w = (w + 1) >> 1; if (w & 7u) w += 8; w &= ~7u; w <<= 1; }
    return w;
}
#define TX_LARGO_MAX 1024
static u16 b_tx[TX_LARGO_MAX] __attribute__((aligned(32)));
int dspi_tx_corto(u16 canal, const u16 *carga, unsigned n)
{
    u16 cuerpo[32];
    unsigned i;
    int r;
    if (n + 1 > 30) return DSP_E_PARAM;
    for (i = 0; i < n; i++) cuerpo[i] = carga[i];
    cuerpo[n] = canal;                                                   /* el canal, en la ÚLTIMA palabra (FUN_4421E1BC) */
    r = dspi_buzon_mandar(0x0A, cuerpo, n + 1);
    if (!r) { dsp_st.tx_cortos++; dspi_msg_apuntar(0, 0x0A, canal, n, carga); }
    return r;
}
/* FUN_4421DCC4 caso 2 [ejecutado: o1.qemu.log 175570..175640]: DMA (memoria -> FA040020, ráfaga 8), +14 |= 0x18 (anchura 8) y el buzón de clase 0x14 {2, canal, n} */
/* (2026-10-24) cuánto espera cada envío largo al fin de flujo del DSP: vueltas de 1 ms de dspi_esperar_ev (0 = el evento ya estaba). dsp_tx_hist[k] = nº de envíos
 * con k vueltas (k >= 15 en la última casilla) */
volatile u32 dsp_tx_hist[16];
int dspi_tx_largo(u16 canal, const u16 *carga, unsigned n)
{
    u32 tam = tam_cuerpo(n + 1u);
    u16 ctl[3];
    unsigned i;
    int r;
    if (tam > TX_LARGO_MAX) return DSP_E_PARAM;
    for (i = 0; i < tam; i++) b_tx[i] = i < n ? carga[i] : 0;
    b_tx[tam - 1] = canal;
    dspi_ev_limpiar();
    if (dspi_dma_lanzar(1, b_tx, tam / 2, 2)) return DSP_E_ENVIO_LARGO;
    D32(0x14) |= 0x18;
    ctl[0] = 2; ctl[1] = canal; ctl[2] = (u16)tam;
    r = dspi_buzon_mandar(0x14, ctl, 3);
    if (r) { dspi_dma_parar(); return r; }
    dsp_st.tx_largos++; dspi_msg_apuntar(0, 0x14, canal, n, carga);
    tx_vueltas = 0;
    if (dspi_esperar_ev(DSP_EV_FIN_TX, 1000)) {
        u32 tc = R32(PL080 + 0x14) & (1u << PL_CH);                                /* ANTES de dspi_dma_estado (que puede limpiarlo) */
        dspi_dma_estado();
        dsp_st.fin_tx_fallos++;
        if (tc) { dspi_dma_parar(); return 0; }                                     /* (2026-10-24) el DMA terminó (TC) aunque no llegó el evento 3 de FA040010: los datos están en el DSP */
        return DSP_E_ENVIO_LARGO;
    }
    dspi_dma_estado();
    dsp_tx_hist[tx_vueltas > 15u ? 15u : tx_vueltas]++;
    return 0;
}

/* ---- (2026-10-26) LOS ENVÍOS DEL TIC: sin esperar nada (docs/mp3_dsp.md §7) ------------------------------------------------------------------
 * El corto, si el buzón está libre. El LARGO en dos tiempos, como la cola de envíos del original (FUN_4421DCC4, que avanzan las interrupciones):
 * dspi_tx_largo_iniciar arma el DMA y manda {2, canal, tam} por el buzón (o DSP_E_BUZON sin tocar nada si el DSP no ha leído el anterior) y
 * dspi_tx_largo_mirar, en los tics siguientes, mira el fin de flujo (evento 3): 1 = hecho, 0 = aún no, < 0 = fallo. Con un envío en vuelo nadie más
 * manda: el bucle, al entrar en el transporte, lo termina antes (dsp_bucle_tomar). */
static u32 tx_vuelo_ms;
static int tx_corto_ya(u16 canal, const u16 *carga, unsigned n)
{
    u16 cuerpo[32];
    unsigned i;
    int r;
    if (n + 1 > 30) return DSP_E_PARAM;
    for (i = 0; i < n; i++) cuerpo[i] = carga[i];
    cuerpo[n] = canal;
    r = buzon_mandar_ya(0x0A, cuerpo, n + 1);
    if (!r) { dsp_st.tx_cortos++; dspi_msg_apuntar(0, 0x0A, canal, n, carga); }
    return r;
}
int dspi_tx_corto_ya(u16 canal, const u16 *carga, unsigned n)
{
    if (!dsp_st.gate5) return DSP_E_NO_ARRANCADO;
    if (dsp_st.ocupado || tx_vuelo) return DSP_E_OCUPADO;
    return tx_corto_ya(canal, carga, n);
}
int dspi_tx_en_vuelo(void) { return tx_vuelo; }
int dspi_tx_largo_iniciar(u16 canal, const u16 *carga, unsigned n)
{
    u32 tam = tam_cuerpo(n + 1u);
    u16 ctl[3];
    unsigned i;
    int r;
    if (!dsp_st.gate5) return DSP_E_NO_ARRANCADO;
    if (dsp_st.ocupado || tx_vuelo) return DSP_E_OCUPADO;
    if (tam > TX_LARGO_MAX) return DSP_E_PARAM;
    dspi_eventos();
    if (D32(0x00) & 1u) return DSP_E_BUZON;                                         /* el DSP no ha leído el anterior: luego */
    for (i = 0; i < tam; i++) b_tx[i] = i < n ? carga[i] : 0;
    b_tx[tam - 1] = canal;
    dspi_ev_limpiar();
    if (dspi_dma_lanzar(1, b_tx, tam / 2, 2)) return DSP_E_ENVIO_LARGO;
    D32(0x14) |= 0x18;
    ctl[0] = 2; ctl[1] = canal; ctl[2] = (u16)tam;
    r = buzon_mandar_ya(0x14, ctl, 3);
    if (r) { dspi_dma_parar(); return r; }
    dsp_st.tx_largos++; dspi_msg_apuntar(0, 0x14, canal, n, carga);
    tx_vuelo = 1; tx_vuelo_ms = dspi_ms();
    return 0;
}
int dspi_tx_largo_mirar(void)
{
    u32 t;
    if (!tx_vuelo) return 1;
    dspi_eventos();
    t = dspi_ms() - tx_vuelo_ms;
    if (ev_pend & DSP_EV_FIN_TX) {
        ev_pend &= ~DSP_EV_FIN_TX;
        dspi_dma_estado();
        tx_vuelo = 0;
        dsp_tx_hist[t > 15u ? 15u : t]++;                                            /* aquí, ms desde que se lanzó (grano de 4 ms) */
        return 1;
    }
    if (t >= 1000u) {                                                                /* como dspi_tx_largo: si el DMA acabó (TC) los datos están en el DSP */
        u32 tc = R32(PL080 + 0x14) & (1u << PL_CH);
        dspi_dma_estado();
        dsp_st.fin_tx_fallos++;
        tx_vuelo = 0;
        if (tc) { dspi_dma_parar(); return 1; }
        return DSP_E_ENVIO_LARGO;
    }
    return 0;
}

/* ---- (2026-10-26) EL CERROJO ENTRE EL BUCLE Y EL TIC ---------------------------------------------------------------------------------------
 * dsp_bucle > 0 = el bucle está dentro del transporte (o se lo ha reservado con dsp_bucle_tomar para una secuencia entera: una RPC, una rampa,
 * el arranque): el tic NO toca el DSP. Cada función pública de aquí y de hal/dsp_w580_arr.c lo toma y lo suelta, así que nada del bucle
 * (la pantalla de diagnóstico, el puente) se cruza con el tic a medio mensaje o a media lectura de la ventana. Al tomarlo (0 -> 1), si el tic
 * dejó un envío largo en vuelo se TERMINA aquí (dspi_tx_largo_mirar cada ms, con tope) y se avisa a quien lo lanzó (dsp_vuelo_fin) para que lo
 * cuente. Se toma de 0 a 1 en una sola escritura: si el tic entra justo antes, deja su envío en vuelo y se termina aquí; si entra después, ve
 * dsp_bucle != 0 y vuelve sin tocar nada. */
volatile u8 dsp_bucle;
void (*dsp_vuelo_fin)(int r);
volatile u32 dsp_vuelos_terminados, dsp_vuelo_espera_max;    /* envíos del tic terminados por el bucle; la espera más larga (ms) */
void dsp_bucle_tomar(void)
{
    if (dsp_bucle++ == 0 && tx_vuelo) {
        u32 t0 = dspi_ms(), t;
        int r;
        u32 vueltas = 0;
        while ((r = dspi_tx_largo_mirar()) == 0) {
            if (++vueltas > 1100u) { dspi_dma_parar(); tx_vuelo = 0; dsp_st.fin_tx_fallos++; r = DSP_E_ENVIO_LARGO; break; }   /* sin tic (IRQ cortadas) dspi_ms no avanza */
            espera_ms(1);
        }
        t = dspi_ms() - t0; if (t > dsp_vuelo_espera_max) dsp_vuelo_espera_max = t;
        dsp_vuelos_terminados++;
        if (dsp_vuelo_fin) dsp_vuelo_fin(r);
    }
}
void dsp_bucle_soltar(void) { if (dsp_bucle) dsp_bucle--; }

static int enviar_i(u16 canal, const u16 *carga, unsigned n)
{
    if (!dsp_st.gate5) return DSP_E_NO_ARRANCADO;
    if (dsp_st.ocupado) return DSP_E_OCUPADO;
    if (n + 1 <= 30) return dspi_tx_corto(canal, carga, n);
    return dspi_tx_largo(canal, carga, n);
}
int dsp_enviar(u16 canal, const u16 *carga, unsigned n) { int r; dsp_bucle_tomar(); r = enviar_i(canal, carga, n); dsp_bucle_soltar(); return r; }
static int enviar_raw_i(u16 clase, const u16 *cuerpo, unsigned n)
{
    int r;
    if (!dsp_st.gate5) return DSP_E_NO_ARRANCADO;
    if (dsp_st.ocupado) return DSP_E_OCUPADO;
    r = dspi_buzon_mandar(clase, cuerpo, n);
    if (!r) { dsp_st.tx_cortos++; dspi_msg_apuntar(0, (int)(clase & 0xFF), 0xC000u | clase, n, cuerpo); }
    return r;
}
int dsp_enviar_raw(u16 clase, const u16 *cuerpo, unsigned n) { int r; dsp_bucle_tomar(); r = enviar_raw_i(clase, cuerpo, n); dsp_bucle_soltar(); return r; }
int dsp_recibir(DspMsg *m, u16 canal)
{
    int r;
    if (!dsp_st.gate5) return DSP_E_NO_ARRANCADO;
    dsp_bucle_tomar();
    dspi_buzon_recibir();
    r = cola_tomar(m, canal, 0) ? 0 : DSP_E_SIN_MENSAJE;
    dsp_bucle_soltar();
    return r;
}
int dsp_esperar(DspMsg *m, u16 canal, u16 orden, unsigned ms)
{
    int r;
    if (!dsp_st.gate5) return DSP_E_NO_ARRANCADO;
    dsp_bucle_tomar();
    r = dspi_esperar_msg(m, canal, orden, ms) ? DSP_E_SIN_MENSAJE : 0;
    dsp_bucle_soltar();
    return r;
}

/* ---- estado, nombres y registros para la pantalla ----------------------------------------------------------------------------- */
const DspEstado *dsp_estado(void) { return &dsp_st; }
int dsp_vivo(void) { return dsp_st.vivo; }
void dsp_servir(void) { if (dsp_st.gate5 && !dsp_st.ocupado) { dsp_bucle_tomar(); dspi_buzon_recibir(); dsp_bucle_soltar(); } }
const char *dsp_version(void) { return dsp_st.version; }
const char *dsp_paso_nombre(int p)
{
    static const char *const n[DSP_P_N] = { "ninguno", "preparar", "DSPIF (puerta 5, bloque 3)", "PCM FA020000 (26/20)", "mascaras +0C",
        "PreBoot (reloj PCM)", "ventana: comun", "ventana: arranque", "soltar el DSP", "espera 0x1800", "XRAM comun", "XRAM arranque",
        "fin XRAM (0x1802)", "version 0x0201", "VIVO", "apagado" };
    return p >= 0 && p < DSP_P_N ? n[p] : "?";
}
const char *dsp_error_nombre(int e)
{
    switch (e) {
    case DSP_E_OK: return "ninguno";
    case DSP_E_IMAGEN: return "imagen mala";
    case DSP_E_VENTANA: return "ventana no se vacia";
    case DSP_E_NO_1800: return "el DSP no pide XRAM (0x1800)";
    case DSP_E_1800_MAL: return "0x1800 imposible";
    case DSP_E_XRAM_RELLENO: return "relleno de XRAM";
    case DSP_E_XRAM_DMA: return "DMA de XRAM no sale";
    case DSP_E_XRAM_FIN: return "sin fin de flujo (ev 3)";
    case DSP_E_NO_1800_MAS: return "no pide mas XRAM";
    case DSP_E_BUZON: return "buzon ocupado";
    case DSP_E_SIN_VERSION: return "sin version";
    case DSP_E_ENVIO_LARGO: return "envio largo falla";
    case DSP_E_PARAM: return "parametro";
    case DSP_E_OCUPADO: return "ocupado";
    case DSP_E_NO_ARRANCADO: return "DSPIF sin reloj";
    case DSP_E_RX_LARGO: return "recepcion larga falla";
    case DSP_E_SIN_MENSAJE: return "sin mensaje";
    }
    return "?";
}
void dsp_regs(DspRegs *r)
{
    r->valido = 0;
    r->s12 = R16(SYSCON + 0x12); r->s18 = R16(SYSCON + 0x18); r->s2c = R16(SYSCON + 0x2C);
    { static const u8 of[10] = { 0x02, 0x04, 0x06, 0x0A, 0x0C, 0x1C, 0x38, 0x54, 0x80, 0x36 }; int k; for (k = 0; k < 10; k++) r->sc[k] = R16(SYSCON + of[k]); }
    r->r00 = r->r04 = r->r10 = r->r14 = r->r18 = r->r1c = 0;
    r->pl_ctrl = r->pl_cfg = r->pl_tc = r->pl_err = 0;
    if (!dsp_st.gate5) return;                             /* sin reloj el DSPIF no se toca (un bloque sin reloj puede colgar el bus: lo midió el USB) */
    dsp_bucle_tomar();                                     /* (2026-10-26) el tic no entra a medias */
    dspi_eventos();                                        /* +10 se borra al leer: lo visto queda acumulado */
    r->r00 = D32(0x00); r->r04 = D32(0x04); r->r10 = dsp_st.ev_visto; r->r14 = D32(0x14); r->r18 = D32(0x18); r->r1c = D32(0x1C);
    r->pl_ctrl = PL_C(0x0C); r->pl_cfg = PL_C(0x10); r->pl_tc = R32(PL080 + 0x14); r->pl_err = R32(PL080 + 0x18);
    r->valido = 1;
    dsp_bucle_soltar();
}
