/* hal/mic_w580.c — EL MICRÓFONO del W580i. 27-oct-2026, sesión «drivers: micrófono y cámara» (docs/reconocimiento_original.md §4).
 * Camino MEDIDO en el aparato (cada dato lleva su marca en el doc):
 *  - códec «Vincenne» del PMU (I²C): ruta C1..D0 y las etapas de C0 (01, 03, 43, 47, 57) de la grabadora del original; el reloj PCM (SYSCON puerta 26 + bloque 20) [observado];
 *  - el DSP (TI C55x + OSEck) con el módulo AAC_PLUS cargado (el que tiene el original en reposo) y el GRAFO MÍNIMO del mezclador de la grabadora: R_0{8 kHz}, R_A→mb1, R_4,
 *    R_A→mb2, R_4, R_B, R_4, R_C{mb1,fuente 0xBD}, 0x0800 (PCM a 8 kHz) por el canal 0x0B, R_5A (activa el PCM), R_D{mb1,mb2}→conexión, R_3. Los identificadores (mb1, mb2,
 *    conexión) los devuelve el DSP en cada respuesta [observado: aparato/buzon_grabando_1.txt y emu/grabadora_setup_msgs.json]. Las palabras sueltas (656E, 6F43, C06C…) son
 *    restos de pila del original: se copian tal cual porque así se probó [validado: el DSP las acepta, código 0];
 *  - las muestras llegan a un ANILLO de 80 palabras (10 ms) en la memoria de datos del DSP: 0x02A2..0x02F1, de 8 en 8 (un trozo por ms); el puntero de escritura es la palabra
 *    0x05D3 (la segunda del par 0x05D2) y vale 0x02F2 al acabar el anillo [observado con 6000 lecturas del puntero y 60 instantáneas, 27-oct]. 0x02F2..0x0341 está a cero.
 *  - se lee por la VENTANA del DSPIF (+24/+28/+2C; orden 2 = leer; devuelve el par primera<<16 | segunda). Reglas: nunca con el DSP dormido (campanilla concedida) y sin
 *    atender la campanilla mientras se captura (dsp_camp_pausa).
 * VALIDADO en el aparato (senew-RAM): tono 440 Hz de la Mac → 440,00 Hz en 10 s sin deriva; 1000 Hz → 1000,02 Hz; barrido 300..2000 Hz → pendiente 142,6 Hz/s (esperado 141,7);
 * silencio σ = 2,5 frente a σ ≈ 250 con sonido; 8002 muestras/s, 0 desbordes.
 * OJO (lo que costó horas): el perro de hardware (F9010000) cuenta de forma ACUMULADA (~2,4 s armado en total desde el arranque; desarmar con la llave NO lo repone) — esta función no lo arma.
 * [sin saber] la ganancia (ruta C1..D0 copiada de la grabadora), la cancelación de eco, el uso con la llamada o el manos libres, cómo se para el grafo con elegancia (aquí se apaga el DSP). */
#ifdef TELEFONO
#include "hal.h"
#include "mic_w580.h"
#include "dsp_w580.h"
#include "dsp_w580_i.h"

extern const DspImagen dsp_img_aac_plus;                  /* hal/dsp_w580_img_aac_plus.c (generado) */
extern volatile u32 dsp_camp_pausa;                       /* hal/dsp_w580.c: != 0 = no se sondea la campanilla */
u32 hw_ms(void);

#define DSPIF   0xFA040000u
#define ANILLO  0x02A2u
#define ANILLO_N 80u
#define PUNTERO 0x05D2u

static u8 mic_on;
static u32 mic_desb;

/* mensajes del mezclador (canal 0x0C), sin la palabra del canal. Posiciones que se parchean: [9] = id del bloque; R_D: [9] y [11]; R_3: [9] y [10] */
static const u16 R_0[11]  = { 0x0900, 0x0004, 0x0000, 0x0000, 0x0001, 0x00D3, 0x0000, 0x4038, 0x4544, 0x0001, 0xFFEE };
static const u16 R_A[13]  = { 0x0900, 0x0006, 0x0000, 0x000A, 0x0001, 0x00D3, 0x656E, 0x4038, 0x4544, 0x0000, 0x0000, 0x0000, 0x0000 };
static const u16 R_4[13]  = { 0x0900, 0x0006, 0x0000, 0x0004, 0x0001, 0x00D3, 0x0000, 0x4038, 0x4544, 0x0000, 0x4000, 0x4000, 0x0000 };
static const u16 R_B[13]  = { 0x0900, 0x0006, 0x0000, 0x000B, 0x0001, 0x00D3, 0x0000, 0x4038, 0x4544, 0x0000, 0x0000, 0x0001, 0x0001 };
static const u16 R_C[13]  = { 0x0900, 0x0006, 0x0000, 0x000C, 0x0001, 0x00D3, 0x6F43, 0x4038, 0x4544, 0x0000, 0x0000, 0x00BD, 0x0001 };
static const u16 R_5A[13] = { 0x0900, 0x0006, 0x0000, 0x005A, 0x0001, 0x00BD, 0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001, 0x0043 };
static const u16 R_D[13]  = { 0x0900, 0x0006, 0x0000, 0x000D, 0x0001, 0x00D3, 0xC06C, 0x4038, 0x4544, 0x0000, 0x0000, 0x0000, 0x0000 };
static const u16 R_3[13]  = { 0x0900, 0x0006, 0x0000, 0x0003, 0x0001, 0x00D3, 0x0000, 0x4038, 0x4544, 0x0000, 0x0000, 0x4000, 0x4000 };
/* 0x0800 por el canal 0x0B: el PCM a 8 kHz (la palabra 24 = 0x1F40) */
static const u16 P_0800[26] = { 0x0800, 0x0000, 0x00BE, 0x0001, 0x0200, 0x0000, 0x0000, 0x0000, 0x0000, 0x3010, 0x1010, 0x0000, 0x0000, 0x0000, 0x0000, 0x0CB2,
                                0x0CB2, 0x0000, 0x0000, 0x0001, 0x001E, 0x0038, 0x0000, 0x0000, 0x1F40, 0xFFFF };

/* manda una petición al mezclador y espera su respuesta (misma R_n en la palabra 3); resp[] = la carga recibida (64 palabras); 0 = el DSP contestó con código 0 */
static int rpc_mix(const u16 *m, unsigned n, u16 *resp)
{
    static DspMsg r;
    u32 t0 = hw_ms();
    unsigned i;
    if (dsp_enviar(0x0C, m, n)) return -1;
    for (;;) {
        if ((u32)(hw_ms() - t0) > 800u) return -2;
        if (dsp_esperar(&r, 0x0C, 0x0900, 200)) continue;
        if (r.n >= 7 && r.d[3] == m[3]) break;
    }
    for (i = 0; i < 64; i++) resp[i] = i < r.n ? r.d[i] : 0;
    return resp[6] ? -3 : 0;
}

int mic_iniciar(void)
{
    static const u8 ruta[][2] = { { 0xC1, 0x08 }, { 0xC2, 0x34 }, { 0xC3, 0xE6 }, { 0xC4, 0xF6 }, { 0xC5, 0x90 }, { 0xC6, 0x00 }, { 0xC7, 0x40 }, { 0xC8, 0x0C }, { 0xC9, 0x10 },
                                  { 0xCA, 0x10 }, { 0xCB, 0x50 }, { 0xCC, 0x50 }, { 0xCD, 0x50 }, { 0xCE, 0x30 }, { 0xCF, 0x20 }, { 0xD0, 0xF5 } };
    static const u8 etapas[] = { 0x01, 0x03, 0x43, 0x47, 0x57 };
    u16 w[13], rx[64], mb1 = 0, mb2 = 0, conn = 0;
    unsigned i, fallos = 0;
    int k;
    if (mic_on) return 0;
    mic_desb = 0;
#ifdef SENEW_RAM
    dspi_ctl0();                                                    /* senew en RAM entra con el DSP del original en marcha y dormido: arranque en frío */
#endif
    if (dsp_arrancar()) return -1;
    espera_ms(1000);                                                /* que el SO del DSP termine de inicializarse */
    if (dsp_cargar(&dsp_img_aac_plus)) return -2;
    espera_ms(500);
    if (dsp_pedir_version()) return -3;
    dspi_puerta(26, 1); dspi_bloque(20, 0);                         /* reloj PCM */
    for (i = 0; i < sizeof ruta / 2; i++) fallos += (unsigned)(pmu_escribir(ruta[i][0], ruta[i][1]) != 0);
    for (i = 0; i < sizeof etapas; i++) { fallos += (unsigned)(pmu_escribir(0xC0, etapas[i]) != 0); espera_ms(5); }
    fallos += (unsigned)(pmu_escribir(0xCD, 0x57) != 0);
    if (fallos) return -4;
    k = 0;
    if (rpc_mix(R_0, 11, rx)) goto fallo;                                                                                         /* 0: 8 kHz */
    k = 1; if (rpc_mix(R_A, 13, rx)) goto fallo;  mb1 = rx[11];                                                                    /* 1: crear bloque 1 */
    k = 2; for (i = 0; i < 13; i++) w[i] = R_4[i];  w[9] = mb1; if (rpc_mix(w, 13, rx)) goto fallo;                                /* 2: R_4{mb1} */
    k = 3; if (rpc_mix(R_A, 13, rx)) goto fallo;  mb2 = rx[11];                                                                    /* 3: crear bloque 2 */
    k = 4; for (i = 0; i < 13; i++) w[i] = R_4[i];  w[9] = mb2; if (rpc_mix(w, 13, rx)) goto fallo;                                /* 4: R_4{mb2} */
    k = 5; for (i = 0; i < 13; i++) w[i] = R_B[i];  w[9] = mb2; if (rpc_mix(w, 13, rx)) goto fallo;                                /* 5: R_B{mb2,0,1,1} */
    k = 6; for (i = 0; i < 13; i++) w[i] = R_4[i];  w[9] = mb2; if (rpc_mix(w, 13, rx)) goto fallo;                                /* 6: R_4{mb2} otra vez */
    k = 7; for (i = 0; i < 13; i++) w[i] = R_C[i];  w[9] = mb1; if (rpc_mix(w, 13, rx)) goto fallo;                                /* 7: R_C{mb1,0,0xBD,1} (fuente) */
    k = 8;                                                                                                                         /* 8: 0x0800 por el canal 0x0B */
    { static DspMsg r; u32 t0 = hw_ms();
      if (dsp_enviar(0x0B, P_0800, 26)) goto fallo;
      for (;;) { if ((u32)(hw_ms() - t0) > 800u) goto fallo; if (!dsp_esperar(&r, 0x0B, 0x0801, 200)) break; } }
    k = 9; if (rpc_mix(R_5A, 13, rx)) goto fallo;                                                                                  /* 9: activar el PCM */
    k = 10; for (i = 0; i < 13; i++) w[i] = R_D[i]; w[9] = mb1; w[11] = mb2; if (rpc_mix(w, 13, rx)) goto fallo;  conn = rx[12];    /* 10: conectar */
    k = 11; for (i = 0; i < 13; i++) w[i] = R_3[i]; w[9] = mb2; w[10] = conn; if (rpc_mix(w, 13, rx)) goto fallo;                  /* 11: R_3{mb2,conexión} */
    dsp_camp_pausa = 1;                                             /* desde aquí NADIE concede el reposo al DSP (la ventana cuelga el bus con el DSP dormido) */
    mic_on = 1;
    return 0;
fallo:
    dsp_apagar();
    return -10 - k;
}

int mic_activo(void) { return mic_on; }
u32 mic_desbordes(void) { return mic_desb; }

/* un par de la memoria de datos del DSP por la ventana: 0 = bien. NO tiene el perro de hardware (su cuenta es acumulada) */
static int vent_par(u32 dir, u32 *v)
{
    u32 t;
    for (t = 0; t < 100000u; t++) if (!(R32(DSPIF + 0x24) & 2u)) break;
    if (t == 100000u) return -1;
    R32(DSPIF + 0x28) = dir & 0x7FFFFEu; R32(DSPIF + 0x24) = 2;
    for (t = 0; t < 100000u; t++) if (!R32(DSPIF + 0x24)) break;
    if (t == 100000u) return -1;
    *v = R32(DSPIF + 0x2C);
    return 0;
}

int mic_leer(short *dst, u32 muestras, u32 plazo_ms)
{
    u32 rec = 0, t0, ult = 0xFFFFFFFFu, tult = 0;
    if (!mic_on) return -1;
    if (dspi_dormido()) {                                           /* concedido el reposo: un mensaje lo despierta */
        dsp_pedir_version(); espera_ms(20);
        if (dspi_dormido()) return -2;
    }
    t0 = hw_ms();
    dsp_bucle_tomar();                                              /* el alimentador del tic no toca el DSP mientras tanto */
    while (rec < muestras && (u32)(hw_ms() - t0) < plazo_ms) {
        u32 v, lim, nuevas, k;
        if (vent_par(PUNTERO, &v)) break;
        v &= 0xFFFFu;
        if (v < ANILLO || v > ANILLO + ANILLO_N) { espera_ms(1); continue; }       /* puntero fuera del anillo: reintentar */
        lim = ((v - ANILLO) & ~7u) % ANILLO_N;                                    /* solo trozos COMPLETOS: el DSP mueve el puntero palabra a palabra dentro de un trozo */
        if (ult == 0xFFFFFFFFu) { ult = lim; tult = hw_ms(); espera_ms(2); continue; }
        nuevas = (lim + ANILLO_N - ult) % ANILLO_N;
        if (!nuevas) { if ((u32)(hw_ms() - tult) > 8u) { mic_desb++; tult = hw_ms(); } espera_ms(1); continue; }
        if ((u32)(hw_ms() - tult) > 8u) mic_desb++;
        for (k = 0; k < nuevas && rec < muestras; k += 2) {
            u32 par;
            if (vent_par(ANILLO + (ult + k) % ANILLO_N, &par)) goto fin;
            dst[rec++] = (short)(par >> 16);
            if (rec < muestras) dst[rec++] = (short)par;
        }
        ult = lim; tult = hw_ms();
        espera_ms(2);
    }
fin:
    dsp_bucle_soltar();
    return (int)rec;
}

/* ---- (1.10, 27-oct) EL MICRÓFONO SIN ESPERAS, para las apps y el dictado (shell/micro_sis.c) ----------------------------------------------------------
 * mic_iniciar BLOQUEA ~3 s y mic_leer bloquea mientras captura: no sirven dentro de una app (cooperativa) ni con la interfaz viva. Aquí:
 *  - mic_empezar + mic_paso: el MISMO arranque, troceado en pasos que llama el bucle del sistema; las dos esperas (1 s tras arrancar el DSP, 0,5 s tras
 *    cargar el módulo) pasan sin bloquear. Quedan dos trozos que bloquean: dsp_arrancar (~0,6 s [observado 27-oct]) y dsp_cargar [sin medir].
 *  - mic_tic (desde el tic de 4 ms, hal/timer.c tic_micro): vacía el anillo del DSP (80 palabras = 10 ms) al ANILLO DEL SISTEMA (MR_N muestras, ~2 s);
 *    cada tic llegan ~32 muestras. Si un tic se retrasa > 8 ms puede perderse audio: cuenta un desborde (como mic_leer).
 *  - mic_sacar: copia lo acumulado, sin esperar. */
#define MR_N 16384u                                               /* el anillo del sistema: 2,05 s a 8 kHz */
static short mr[MR_N];
static volatile u32 mr_esc, mr_lec, mr_perdidas, mr_pico, mr_pico_ant, mr_pico_n;
static u32 mt_ult = 0xFFFFFFFFu, mt_t_ult;
static u8 ini_paso;
static u32 ini_t;
static u16 ini_mb1, ini_mb2, ini_conn;
volatile u32 mic_med[4];                                          /* para el puente: [0] tics con muestras, [1] tics con el DSP dormido, [2] tics saltados (dsp_bucle), [3] lecturas de la ventana fallidas */

static int vent_par_tic(u32 dir, u32 *v)                          /* vent_par con topes cortos: dentro de una interrupción no se espera mucho */
{
    u32 t;
    for (t = 0; t < 4000u; t++) if (!(R32(DSPIF + 0x24) & 2u)) break;
    if (t == 4000u) return -1;
    R32(DSPIF + 0x28) = dir & 0x7FFFFEu; R32(DSPIF + 0x24) = 2;
    for (t = 0; t < 4000u; t++) if (!R32(DSPIF + 0x24)) break;
    if (t == 4000u) return -1;
    *v = R32(DSPIF + 0x2C);
    return 0;
}
static void mr_poner(short x)
{
    u32 a = (u32)(x < 0 ? -x : x);
    if (mr_esc - mr_lec >= MR_N) { mr_perdidas++; return; }       /* nadie lee: se tira lo NUEVO (el lector es dueño de mr_lec) */
    mr[mr_esc % MR_N] = x; mr_esc++;
    if (a > mr_pico) mr_pico = a;
    if (++mr_pico_n >= 800u) { mr_pico_ant = mr_pico; mr_pico = 0; mr_pico_n = 0; }   /* el pico de cada 100 ms */
}
void mic_tic(void)
{
    u32 v, lim, nuevas, k, t;
    if (!mic_on) return;
    if (dsp_bucle) { mic_med[2]++; return; }                      /* el bucle tiene el transporte del DSP: este tic no toca la ventana */
    if (dspi_dormido()) { mic_med[1]++; return; }                 /* con el DSP dormido la ventana CUELGA el bus */
    if (vent_par_tic(PUNTERO, &v)) { mic_med[3]++; return; }
    v &= 0xFFFFu;
    if (v < ANILLO || v > ANILLO + ANILLO_N) return;              /* fuera del anillo: el siguiente tic */
    lim = ((v - ANILLO) & ~7u) % ANILLO_N;                        /* solo trozos COMPLETOS */
    t = hw_ms();
    if (mt_ult == 0xFFFFFFFFu) { mt_ult = lim; mt_t_ult = t; return; }
    nuevas = (lim + ANILLO_N - mt_ult) % ANILLO_N;
    if ((u32)(t - mt_t_ult) > 8u) mic_desb++;                     /* más de 8 ms sin mirar: pudo dar la vuelta */
    mt_t_ult = t;
    if (!nuevas) return;
    for (k = 0; k < nuevas; k += 2) {
        u32 par;
        if (vent_par_tic(ANILLO + (mt_ult + k) % ANILLO_N, &par)) { mic_med[3]++; break; }
        mr_poner((short)(par >> 16));
        mr_poner((short)par);
    }
    mt_ult = lim;
    mic_med[0]++;
}
u32 mic_disponibles(void) { return mr_esc - mr_lec; }
u32 mic_sacar(short *dst, u32 max)
{
    u32 n = mr_esc - mr_lec, i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) dst[i] = mr[(mr_lec + i) % MR_N];
    mr_lec += n;
    return n;
}
u32 mic_perdidas(void) { return mr_perdidas; }
u32 mic_pico(void) { return mr_pico_ant > mr_pico ? mr_pico_ant : mr_pico; }
int mic_preparando(void) { return ini_paso != 0; }

int mic_empezar(void)
{
    if (mic_on || ini_paso) return 0;
    mic_desb = 0; mr_esc = mr_lec = mr_perdidas = mr_pico = mr_pico_ant = mr_pico_n = 0; mt_ult = 0xFFFFFFFFu;
    { u32 k; for (k = 0; k < 4; k++) mic_med[k] = 0; }
    ini_paso = 1;
    return 0;
}
/* un paso del arranque: 0 = sigue (llamar otra vez), 1 = listo (el tic ya recoge muestras), < 0 = falló (el DSP queda apagado; mismos códigos que mic_iniciar) */
int mic_paso(void)
{
    static const u8 ruta[][2] = { { 0xC1, 0x08 }, { 0xC2, 0x34 }, { 0xC3, 0xE6 }, { 0xC4, 0xF6 }, { 0xC5, 0x90 }, { 0xC6, 0x00 }, { 0xC7, 0x40 }, { 0xC8, 0x0C }, { 0xC9, 0x10 },
                                  { 0xCA, 0x10 }, { 0xCB, 0x50 }, { 0xCC, 0x50 }, { 0xCD, 0x50 }, { 0xCE, 0x30 }, { 0xCF, 0x20 }, { 0xD0, 0xF5 } };
    static const u8 etapas[] = { 0x01, 0x03, 0x43, 0x47, 0x57 };
    u16 w[13], rx[64];
    unsigned i, fallos = 0;
    int r = 0, k;
    if (mic_on) return 1;
    switch (ini_paso) {
    case 0: return -1;                                            /* no se empezó */
    case 1:
#ifdef SENEW_RAM
        dspi_ctl0();
#endif
        if (dsp_arrancar()) { r = -1; break; }
        ini_t = hw_ms(); ini_paso = 2; return 0;
    case 2:
        if ((u32)(hw_ms() - ini_t) < 1000u) return 0;             /* que el SO del DSP termine de inicializarse (sin bloquear) */
        if (dsp_cargar(&dsp_img_aac_plus)) { r = -2; break; }
        ini_t = hw_ms(); ini_paso = 3; return 0;
    case 3:
        if ((u32)(hw_ms() - ini_t) < 500u) return 0;
        if (dsp_pedir_version()) { r = -3; break; }
        dspi_puerta(26, 1); dspi_bloque(20, 0);                   /* reloj PCM */
        for (i = 0; i < sizeof ruta / 2; i++) fallos += (unsigned)(pmu_escribir(ruta[i][0], ruta[i][1]) != 0);
        for (i = 0; i < sizeof etapas; i++) { fallos += (unsigned)(pmu_escribir(0xC0, etapas[i]) != 0); espera_ms(5); }
        fallos += (unsigned)(pmu_escribir(0xCD, 0x57) != 0);
        if (fallos) { r = -4; break; }
        ini_paso = 4; return 0;
    default:                                                      /* 4..15: el grafo del mezclador, una petición por paso (como mic_iniciar) */
        k = ini_paso - 4;
        switch (k) {
        case 0: r = rpc_mix(R_0, 11, rx); break;
        case 1: r = rpc_mix(R_A, 13, rx); ini_mb1 = rx[11]; break;
        case 2: for (i = 0; i < 13; i++) w[i] = R_4[i]; w[9] = ini_mb1; r = rpc_mix(w, 13, rx); break;
        case 3: r = rpc_mix(R_A, 13, rx); ini_mb2 = rx[11]; break;
        case 4: for (i = 0; i < 13; i++) w[i] = R_4[i]; w[9] = ini_mb2; r = rpc_mix(w, 13, rx); break;
        case 5: for (i = 0; i < 13; i++) w[i] = R_B[i]; w[9] = ini_mb2; r = rpc_mix(w, 13, rx); break;
        case 6: for (i = 0; i < 13; i++) w[i] = R_4[i]; w[9] = ini_mb2; r = rpc_mix(w, 13, rx); break;
        case 7: for (i = 0; i < 13; i++) w[i] = R_C[i]; w[9] = ini_mb1; r = rpc_mix(w, 13, rx); break;
        case 8: { static DspMsg m; u32 t0 = hw_ms();
                  if (dsp_enviar(0x0B, P_0800, 26)) { r = -1; break; }
                  for (;;) { if ((u32)(hw_ms() - t0) > 800u) { r = -1; break; } if (!dsp_esperar(&m, 0x0B, 0x0801, 200)) break; } } break;
        case 9: r = rpc_mix(R_5A, 13, rx); break;
        case 10: for (i = 0; i < 13; i++) w[i] = R_D[i]; w[9] = ini_mb1; w[11] = ini_mb2; r = rpc_mix(w, 13, rx); ini_conn = rx[12]; break;
        case 11: for (i = 0; i < 13; i++) w[i] = R_3[i]; w[9] = ini_mb2; w[10] = ini_conn; r = rpc_mix(w, 13, rx); break;
        }
        if (r) { r = -10 - k; break; }
        if (k < 11) { ini_paso++; return 0; }
        dsp_camp_pausa = 1;                                       /* desde aquí NADIE concede el reposo al DSP (la ventana cuelga el bus con el DSP dormido) */
        if (dspi_dormido()) dsp_pedir_version();                  /* (revisión 11) si se le concedió dormir en la última petición, un mensaje lo despierta */
        ini_paso = 0; mt_ult = 0xFFFFFFFFu;
        mic_on = 1;                                               /* lo último: el tic empieza a recoger */
        return 1;
    }
    ini_paso = 0;
    dsp_apagar();
    return r;
}

void mic_parar(void)
{
    if (ini_paso) { ini_paso = 0; dsp_apagar(); }                 /* (1.10) a medio arrancar */
    if (!mic_on) return;
    dsp_camp_pausa = 0;
    mic_on = 0;                                                     /* (1.10) primero: el tic deja de leer la ventana */
    pmu_escribir(0xC0, 0x00);                                       /* el códec sin etapas */
    dsp_apagar();                                                   /* núcleo y DSPIF a reset, puertas cerradas, reloj PCM cerrado */
    mic_on = 0;
}
#endif
