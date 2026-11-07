/* EL REPRODUCTOR DE MP3 DEL W580i POR EL DSP: la máquina de estados y la API hw_mp3_* (pista MP3, 2026-10-21). Ver hal/mp3_w580.h (qué hace) y
 * tmp/mp3/informe.md (qué está verificado). El protocolo ARM<->DSP sale de tmp/dsp_emu/protocolo_mp3.md; el transporte (buzón, DMA, arranque) es de
 * hal/dsp_w580.c y dsp_w580_arr.c; el lector de la cabecera, el APF y el códec, de hal/mp3_w580_cab.c y mp3_w580_apf.c; las plantillas de mensajes,
 * GENERADAS del original, de hal/mp3_w580_dat.c.
 * Reglas: nada se cuelga (tope de tiempo y de vueltas en cada espera, perro de software), ante cualquier fallo se apaga ordenadamente y queda el motivo,
 * y el DSP no se arranca hasta hw_mp3_play(). */
#include "hw.h"
#include "dsp_w580_i.h"
#include "mp3_w580.h"
#include "mp3_w580_i.h"
#include "mp3_w580_dat.h"

void hw_perro_latido(void) __attribute__((weak));       /* plat/tel/perro.c */
u32  hw_fs_leer_desde(const char *ruta, u32 desde, u8 *buf, u32 max);
extern void (*volatile tic_audio)(void);               /* hal/timer.c (2026-10-26): el tercer dueño del tic, el alimentador */

/* ---- estado -------------------------------------------------------------------------------------------------------------------------------- */
static Mp3Diag D;                                       /* lo que enseña la pantalla; es también el estado público */
static struct {
    u8  abierto;                                        /* hay un fichero abierto con cabecera válida */
    u8  dsp_ok;                                         /* el DSP está vivo CON el módulo MP3 (válido solo si M.intentos == dsp_estado()->intentos) */
    u8  intentos;
    u8  proc;                                           /* hay un proceso decodificador creado */
    u8  idx;                                            /* índice de proceso pedido al gestor (0) */
    u8  hw_cod;                                         /* el códec tiene la sombra iniciada */
    u8  ramp;                                           /* una rampa de volumen en curso (no se usa: las rampas son bloqueantes y cortas) */
    u16 flujo, canal;
    int espacio;                                        /* palabras libres del anillo (cuenta del ARM: FUN_441a51f8) */
    int tiene_cap;
    u32 pos_arch;                                       /* siguiente byte del fichero a mandar */
    u32 pos_base_ms, t_ref;                             /* tiempo transcurrido = pos_base_ms + (ahora - t_ref) mientras suena */
    u32 t_play, t_fase, t_cierre, t_cable, t_sonar;
    u32 hay_cierre;
    u8  mx_limpio;                                      /* el mezclador acaba de reiniciarse (R_14…R_17) y nadie lo ha vuelto a configurar */
    u8  ini;                                            /* ya se inició el módulo (volumen por defecto, gancho) */
    u32 pos_ini, ini_ms, t_credito;                     /* byte donde empieza a sonar la pista, su ms, hora del último crédito */
    Mp3Cab cab;
    u8  vaciando;                                       /* (2026-10-26) parar/buscar esperan el 0x02xx del vaciado: el filtro no se lo queda */
    u16 gain;                                           /* ganancia R_4 del volumen actual */
    u8  vol_pend, vol_fallos;                           /* (2026-10-25) un volumen pedido y aún no mandado; fallos seguidos al mandarlo */
    u8  api;                                            /* (2026-10-25) funciones públicas del motor en curso (hw_mp3_respirar no entra) */
    u32 t_ritmo, cred_ritmo;                            /* (2026-10-26) ventana del vigilante de ritmo (0 = empezar otra) */
    u32 t_lento, cred_lento, secos_lento;              /* (2026-10-26) y la de su lado bajo (12 s) */
    u32 t_vol;                                          /* cuándo se mandó (o se intentó) el último */
    void (*progreso)(void);
    u32 progreso_t;
} M;
static Mp3LogMsg lg[MP3_LOG_N]; static u32 lg_n;
static Mp3Rn rnv[MP3_RN_N];
static u32 cred_logs;

/* los 16 niveles de volumen: 3 dB por paso (ganancia de la salida, R_4) [observado, tmp/dsp_emu/protocolo_mp3.md §5] */
static const u16 niveles_gain[16] = { 0x0000, 0x0040, 0x005A, 0x0080, 0x00B5, 0x0100, 0x016A, 0x0200, 0x02D4, 0x0400, 0x05A8, 0x0800, 0x0B50, 0x1000, 0x16A0, 0x2000 };
static const u16 rampa_baja[10] = { 0x0196, 0x0142, 0x0100, 0x00CB, 0x00A1, 0x0080, 0x0065, 0x0050, 0x0040, 0x0000 };            /* sobre 0x0200 */
static const u16 rampa_sube[11] = { 0x0000, 0x0040, 0x0050, 0x0065, 0x0080, 0x00A1, 0x00CB, 0x0100, 0x0142, 0x0196, 0x0200 };
static const u16 rampa_ini[5]   = { 0x0050, 0x0080, 0x00CB, 0x0142, 0x0200 };

/* ---- tiempo, perro, progreso ------------------------------------------------------------------------------------------------------------------ */
static u32 ahora(void) { return dspi_ms(); }
static void latir(void)
{
    if (hw_perro_latido) hw_perro_latido();
    if (M.progreso && (u32)(tics - M.progreso_t) >= 25u) { M.progreso_t = tics; M.progreso(); }
}
static void espera(u32 ms) { while (ms) { u32 t = ms > 10 ? 10 : ms; espera_ms(t); ms -= t; latir(); } }
void mp3_progreso(void (*f)(void)) { M.progreso = f; }
#ifdef MP3_MIGAS_FLASH
void mp3_miga(unsigned n);
#define PMIGA(n) mp3_miga(n)
#else
#define PMIGA(n) ((void)0)
#endif
static void pinta(void) { if (M.progreso) { M.progreso_t = tics; PMIGA(992); M.progreso(); PMIGA(993); } }

/* ---- fases y errores ------------------------------------------------------------------------------------------------------------------------ */
#ifdef MP3_MIGAS_FLASH            /* SOLO build de desarrollo: una miga de pan en la flash (zona de usuario) ANTES de cada paso; sobrevive a sacar la pila */
void mp3_miga(unsigned n)
{
    static unsigned v;
    v = n;
    hw_usr_escribir("miga.bin", &v, 4);
}
#define MIGA(n) mp3_miga(n)
#else
#define MIGA(n) ((void)0)
#endif
static void fase_ini(int f)
{
    MIGA(100u + (unsigned)f);
    D.fase = (u8)f; M.t_fase = ahora(); if (M.t_play) D.ms_total = ahora() - M.t_play;
    pinta();
    MIGA(150u + (unsigned)f);
}
static int fase_fin(int f, int r)
{
    if (f >= 0 && f < MP3_F_N) D.ms_fase[f] = ahora() - M.t_fase;
    if (M.t_play) D.ms_total = ahora() - M.t_play;
    return r;
}
volatile u32 mp3_err_ms, mp3_err_fase, mp3_err_n, mp3_err_t0;   /* (2026-10-25) hora (desde el PLAY) y fase del PRIMER error, y cuántos */
static int fallo(int code, int sub)
{
    mp3_err_n++;
    if (!D.error) { D.error = (signed char)code; D.sub = (signed char)sub; mp3_err_ms = ahora() - mp3_err_t0; mp3_err_fase = D.fase; }
    return code;
}
const char *mp3_fase_nombre(int f)
{
    static const char *const n[MP3_F_N] = { "ninguna", "abrir fichero", "arranque DSP", "cargar APF", "codec (53 campos)", "linea F1 (mezclador)", "PreBoot", "cambio de modulo",
        "1101 + version", "PostBoot (mezclador)", "version 2", "proceso 0x1208", "arrancar 0x120A", "INIT 0x0E06", "R_2", "CONFIG 0x0E00", "umbral 0x0404", "datos al anillo",
        "PLAY 0x0E02", "SONANDO", "PAUSA", "BUSQUEDA", "FIN/parada", "CIERRE de la linea", "APAGADO" };
    return f >= 0 && f < MP3_F_N ? n[f] : "?";
}
const char *mp3_error_nombre(int e)
{
    switch (e) {
    case MP3_E_OK: return "ninguno";
    case MP3_E_PARAM: return "parametro malo";
    case MP3_E_FICHERO: return "no se puede leer el fichero";
    case MP3_E_NO_MP3: return "no es un MP3 (capa III)";
    case MP3_E_USB: return "cable USB puesto";
    case MP3_E_APF: return "no hay APF";
    case MP3_E_DSP: return "el DSP no arranca/carga";
    case MP3_E_MEZCLADOR: return "el mezclador no contesta";
    case MP3_E_SALIDA: return "sin 0x0801 (salida)";
    case MP3_E_VERSION: return "el DSP no da su version";
    case MP3_E_PROCESO: return "sin 0x1209 (proceso)";
    case MP3_E_ARRANCA: return "sin 0x120B (arrancar)";
    case MP3_E_INIT: return "sin 0x0E07 (INIT)";
    case MP3_E_CONFIG: return "sin 0x0E01 (CONFIG)";
    case MP3_E_ACCION: return "sin 0x0E03 (accion)";
    case MP3_E_ANILLO: return "sin anillo de datos (0x0304)";
    case MP3_E_ATASCO: return "sin creditos del DSP";
    case MP3_E_DSP_PERDIDO: return "el DSP se reinicio";
    case MP3_E_LECTURA: return "fallo de lectura del fichero";
    case MP3_E_NO_ABIERTO: return "no hay fichero abierto";
    case MP3_E_OCUPADO: return "ocupado";
    case MP3_E_FORMATO: return "formato no soportado";
    case MP3_E_CABLE: return "cable USB: DSP parado";
    case MP3_E_ENVIO: return "fallo al mandar al DSP";
    case MP3_E_FALTAN: return "faltan datos en el colchon";
    case MP3_E_RITMO: return "el DSP pide al doble (vaciado)";
    }
    return "?";
}
const char *hw_mp3_motivo_txt(int m)
{
    switch (m) {
    case MP3_E_OK: return "sin fallo";
    case MP3_E_PARAM: case MP3_E_NO_ABIERTO: return "no hay MP3 abierto";
    case MP3_E_FICHERO: case MP3_E_LECTURA: return "no se puede leer el fichero";
    case MP3_E_NO_MP3: case MP3_E_FORMATO: return "no es un MP3 que se pueda tocar";
    case MP3_E_USB: case MP3_E_CABLE: return "quita el cable USB";
    case MP3_E_APF: return "faltan los ajustes de audio (APF)";
    case MP3_E_DSP: return "el DSP no arranca";
    case MP3_E_MEZCLADOR: case MP3_E_SALIDA: return "el DSP no contesta (mezclador)";
    case MP3_E_VERSION: case MP3_E_PROCESO: case MP3_E_ARRANCA: case MP3_E_INIT: case MP3_E_CONFIG: case MP3_E_ACCION: return "el DSP no contesta (decodificador)";
    case MP3_E_ANILLO: case MP3_E_ATASCO: return "el DSP no pide datos";
    case MP3_E_DSP_PERDIDO: return "el DSP se reinicio";
    case MP3_E_OCUPADO: return "ocupado";
    case MP3_E_ENVIO: return "fallo al hablar con el DSP";
    case MP3_E_FALTAN: return "aun no han llegado bastantes datos";
    case MP3_E_RITMO: return "el DSP se desajusto";
    }
    return "fallo";
}

/* ---- el gancho de mensajes completos (diagnóstico) ------------------------------------------------------------------------------------------- */
static void gancho(int rx, int clase, unsigned canal, unsigned n, const u16 *p)
{
    Mp3LogMsg *e;
    unsigned i;
    if (rx) D.n_msgs_rx++; else D.n_msgs_tx++;
    if (p && n >= 2 && canal == 0x14) {                                        /* datos y créditos: contados, no anotados (inundarían el anillo) */
        unsigned code = p[0] >> 8;
        if (!rx && code == 8) return;
        if (rx && (code == 5 || code == 2)) { if (cred_logs >= 6) return; cred_logs++; }
    }
    if (rx && p && n >= 7 && canal == 0x0C && p[0] == 0x0900) {                   /* la PRIMERA respuesta de cada R_n */
        unsigned rn = p[3], k = rn <= 17 ? rn : rn == 91 ? 18 : 19;
        if (!rnv[k].hay) { rnv[k].hay = 1; rnv[k].rn = (u8)rn; rnv[k].n = (u16)n; for (i = 0; i < n && i < MP3_LOG_PAL; i++) rnv[k].w[i] = p[i]; }
        rnv[k].n_resp++;
    }
    e = &lg[lg_n++ % MP3_LOG_N];
    e->rx = (u8)rx; e->clase = (u8)clase; e->canal = (u16)canal; e->n = (u16)n; e->ms = ahora();
    for (i = 0; i < MP3_LOG_PAL; i++) e->w[i] = p && i < n ? p[i] : 0;
}
int mp3_diag_log(Mp3LogMsg *dst, int max)
{
    int n = lg_n < MP3_LOG_N ? (int)lg_n : MP3_LOG_N, i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) dst[i] = lg[(lg_n - 1 - (u32)i) % MP3_LOG_N];
    return n;
}
int mp3_diag_rn(Mp3Rn *dst, int max)
{
    int i, n = 0;
    for (i = 0; i < MP3_RN_N && n < max; i++) if (rnv[i].hay) dst[n++] = rnv[i];
    return n;
}

/* ---- mensajes al DSP ------------------------------------------------------------------------------------------------------------------------ */
static int enviar(u16 canal, const u16 *w, unsigned n)
{
    int r = dsp_enviar(canal, w, n);
    latir();
    if (r) { fallo(MP3_E_ENVIO, r); return MP3_E_ENVIO; }
    return 0;
}
/* espera un mensaje del canal con esa orden; 0 = lo hay */
static int esperar(DspMsg *m, u16 canal, u16 orden, unsigned ms)
{
    int r = dsp_esperar(m, canal, orden, ms);
    return r ? -1 : 0;
}
/* IDS DEL MEZCLADOR (2026-10-24): las trazas del original se hicieron con un DSP FALSO que contestaba ceros; el DSP real numera sus bloques (R_10 -> [11]) y sus conexiones
 * (R_13 -> [12]) y las órdenes siguientes los llevan. A = el bloque del arranque (salida), B = el de la línea (fuente); se rellenan con las respuestas. */
static u16 rx_mx[48];
static unsigned rx_mx_n;
static u16 mb_a, mb_b, mb_conn;
static u32 h_fil;                                                              /* manejador del efecto «filtro» (R_6): todos los R_9 y el R_7 del PreBoot lo llevan; 0 = no hay */
enum { ID_A = 1, ID_B, ID_CONN, ID_HFH, ID_HFL };
static const struct { u8 idx, pal, rol; } mp3_ids[] = {
    { MP3_P_F1_R4_UNIDAD, 9, ID_B }, { MP3_P_F1_R12, 9, ID_B }, { MP3_P_F1_R4_CERO, 9, ID_B }, { MP3_P_F1_R11, 9, ID_A },
    { MP3_P_F1_R13, 9, ID_B }, { MP3_P_F1_R13, 11, ID_A }, { MP3_P_F1_R3, 9, ID_A }, { MP3_P_F1_R3, 10, ID_CONN },
    { MP3_P_RAMPA_PASO, 9, ID_B },
    { MP3_P_PRE_R7A, 9, ID_HFH }, { MP3_P_PRE_R7A, 10, ID_HFL },                                  /* PreBoot y mx_reinicio: el DSP real desmonta con los ids que dio */
    { MP3_P_PRE_R14, 9, ID_B }, { MP3_P_PRE_R14, 10, ID_A }, { MP3_P_PRE_R14, 11, ID_CONN },
    { MP3_P_PRE_R16, 9, ID_A }, { MP3_P_PRE_R17A, 9, ID_A }, { MP3_P_PRE_R15, 9, ID_B }, { MP3_P_PRE_R17B, 9, ID_B },
    { MP3_P_F3_R4A, 9, ID_A }, { MP3_P_F3_R4B, 9, ID_B }, { MP3_P_F3_R12, 9, ID_B }, { MP3_P_F3_R11, 9, ID_A },
    { MP3_P_F3_R13, 9, ID_B }, { MP3_P_F3_R13, 11, ID_A }, { MP3_P_F3_R3, 9, ID_A }, { MP3_P_F3_R3, 10, ID_CONN },
};
#define RPC_PLAZO 500u                                  /* el plazo del original («[Mixer] WARNING: Response from DSP timed out») */
volatile u32 mp3_rpc_max_ms, mp3_rpc_max_n;            /* (2026-10-25) la respuesta más lenta del mezclador (ms) y su R_n: banco de la música */
/* manda un mensaje completo del mezclador (cabecera + cuerpo, nw = 7 + w[1] palabras) y espera su respuesta (misma cabecera) */
volatile u32 mp3_rpc_tardias;                          /* (2026-10-25) respuestas del mezclador que llegaron tarde (tras rendirse rpc) y se tiraron */
static int rpc(const u16 *w, unsigned nw)
{
    DspMsg m;
    u32 t0 = ahora();
    unsigned n = w[3];
    int r;
    while (dsp_recibir(&m, 0x0C) == 0) mp3_rpc_tardias++;       /* nadie espera nada del mezclador: lo que haya en la cola es de una RPC que ya se rindió */
    r = enviar(0x0C, w, nw);
    if (r) return r;
    for (;;) {
        u32 gastado = ahora() - t0;
        if (gastado >= RPC_PLAZO || esperar(&m, 0x0C, 0x0900, RPC_PLAZO - gastado)) {
            D.rpc_mal++; if (D.rpc_seguidos_mal < 255) D.rpc_seguidos_mal++;
            D.rpc_mal_n = (u8)n;                                                      /* no es fatal por sí solo: lo anota quien decide abortar (dos seguidos) */
            return MP3_E_MEZCLADOR;
        }
        if (m.n >= 7 && m.d[3] == n) break;
    }
    { unsigned k; rx_mx_n = m.n < 48 ? m.n : 48; for (k = 0; k < rx_mx_n; k++) rx_mx[k] = m.d[k]; }
    if (ahora() - t0 > mp3_rpc_max_ms) { mp3_rpc_max_ms = ahora() - t0; mp3_rpc_max_n = n; }
    D.rpc_ok++; D.rpc_seguidos_mal = 0;
    if (m.d[6] != 0) D.rpc_res_no0++;
    return 0;
}
/* Una plantilla del plan 1 (mensaje del mezclador): hasta 2 palabras parcheadas (posición, valor). */
static u16 buf_mx[320];
/* SUSTITUCIONES (2026-10-24): el cuerpo de varios mensajes se copió de trazas del DSP FALSO, cuyas respuestas eran ceros; con el DSP REAL algunas palabras son manejadores
 * que él devuelve y que luego hay que usar (R_13 con el cuerpo en ceros lo deja muerto). Tabla rellenable desde el puente: {índice del plan, palabra, valor}; 0xFFFF = libre. */
u16 mp3_sust[8][3] = { { 0xFFFF, 0, 0 }, { 0xFFFF, 0, 0 }, { 0xFFFF, 0, 0 }, { 0xFFFF, 0, 0 }, { 0xFFFF, 0, 0 }, { 0xFFFF, 0, 0 }, { 0xFFFF, 0, 0 }, { 0xFFFF, 0, 0 } };
#ifdef MP3_TONO
volatile u16 mp3_tono_gainA;
volatile u16 mp3_tono_gain[3];
volatile u16 mp3_r2_rep[16];                               /* la respuesta del DSP al R_2 de F4 */
volatile u16 mp3_tono_l12 = 0xFFFF, mp3_tono_l11 = 0xFFFF;   /* 0xFFFF = la plantilla tal cual */                  /* ids de la línea en R_12 (fuente: 0..5 válidos en el DSP) y R_11 (destino) */
#endif
static unsigned plan_copiar(int i, u16 *b)
{
    const Mp3Plan *p = &mp3_plan1[i];
    unsigned k;
    for (k = 0; k < p->nw; k++) b[k] = mp3_pool[p->off + k];
    for (k = 0; k < sizeof mp3_ids / sizeof mp3_ids[0]; k++)
        if (mp3_ids[k].idx == i && mp3_ids[k].pal < p->nw) {
            u8 r = mp3_ids[k].rol;
            b[mp3_ids[k].pal] = r == ID_A ? mb_a : r == ID_B ? mb_b : r == ID_CONN ? mb_conn : r == ID_HFH ? (u16)(h_fil >> 16) : (u16)h_fil;
        }
    if (b[3] == 9 && p->nw > 12 && b[9] == 1) { b[11] = (u16)(h_fil >> 16); b[12] = (u16)h_fil; }   /* R_9 con una entrada: su clave es el manejador del filtro (los demás dan 0x14) */
#ifdef MP3_TONO
    if (i == MP3_P_F3_R4A && mp3_tono_gainA) { b[10] = mp3_tono_gainA; b[11] = mp3_tono_gainA; }
    if ((i == MP3_P_F1_R12 || i == MP3_P_F3_R12) && p->nw > 10 && mp3_tono_l12 != 0xFFFFu) b[10] = mp3_tono_l12;
    if ((i == MP3_P_F1_R11 || i == MP3_P_F3_R11) && p->nw > 11 && mp3_tono_l11 != 0xFFFFu) b[11] = mp3_tono_l11;
#endif
    for (k = 0; k < 8; k++) if (mp3_sust[k][0] == (u16)i && mp3_sust[k][1] < p->nw) b[mp3_sust[k][1]] = mp3_sust[k][2];
    return p->nw;
}
static int mx(int i)
{
    unsigned nw = plan_copiar(i, buf_mx);
    int r = rpc(buf_mx, nw);
    if (!r && rx_mx_n > 12) {                                                        /* lo que el DSP devuelve y las órdenes siguientes necesitan */
        if (i == MP3_P_F3_R10A) mb_a = rx_mx[11];
        else if (i == MP3_P_F1_R10 || i == MP3_P_F3_R10B) mb_b = rx_mx[11];
        else if (i == MP3_P_F1_R13 || i == MP3_P_F3_R13) mb_conn = rx_mx[12];
    }
    return r;
}
static int mx_rango(int a, int b)                       /* a..b incluidos; para en el primer fallo grave (dos seguidos) */
{
    int i, r = 0;
    for (i = a; i <= b; i++) {
        r = mx(i);
        if (r && D.rpc_seguidos_mal >= 2) return r;
    }
    return r;
}
/* Los 5 R_9 que el ORIGINAL manda delante de cada paso del volumen son conjuntos de biquads de ecualizador/DRC (tmp/dsp_emu/protocolo_mp3.md §4.3 y §5) para los
 * efectos DRC y banco, que senew NO crea (el DSP real rechaza sus R_6 con 0x10: ver f1_linea); plan_copiar les pone la clave del FILTRO (h_fil) y el mezclador del
 * DSP se mete en un bucle de errores de bus (0x40005512 extra 0x8302E598 = PC 0x2E598, 0x40005515) y deja de contestar: era el «un cambio de volumen mata la
 * canción» (2026-10-25, medido: 60 R_4 solos sin un fallo; el primer grupo de R_9 solo ya lo rompe). Por omisión NO se mandan; mp3_grupo_r9 = 1 (puente) los
 * vuelve a mandar, para repetir la prueba. El volumen y las rampas son solo R_4 (protocolo §8.3). */
volatile u32 mp3_grupo_r9;
static int mx_grupo(void) { return mp3_grupo_r9 ? mx_rango(MP3_P_GRUPO_0, MP3_P_GRUPO_4) : 0; }
static int mx_ganancia(u16 g, u16 u)                    /* R_4 {0, g, g, u} */
{
    unsigned nw = plan_copiar(MP3_P_RAMPA_PASO, buf_mx);
    buf_mx[10] = g; buf_mx[11] = g; buf_mx[12] = u;
    return rpc(buf_mx, nw);
}
/* los constructores del APF: el cuerpo del R_6/R_9 sale del APF cargado; la cabecera, de la plantilla */
static int mx_apf_fin(int i, unsigned nw)                 /* manda el R_6 ya armado y guarda el manejador que devuelve el DSP (reply [7..8]; código [6] = 0 si lo aceptó) */
{
    int r = rpc(buf_mx, nw);
    if (!r && rx_mx_n > 8 && rx_mx[6] == 0 && (i == MP3_P_F1_R6_FILTRO || i == MP3_P_F3_R6_FILTRO)) h_fil = ((u32)rx_mx[7] << 16) | rx_mx[8];
    return r;
}
static int mx_apf(int i, int (*construir)(u16 *, u32))
{
    unsigned nw = plan_copiar(i, buf_mx);
    u16 cuerpo[300];
    int n = construir(cuerpo, 300);
    unsigned k;
    if (n < 0 || (unsigned)n + 7 > nw) return mx_apf_fin(i, nw);                          /* sin APF: la plantilla tal cual (la del original con farfield_closed_play) */
    for (k = 0; k < (unsigned)n; k++) buf_mx[7 + k] = cuerpo[k];
    return mx_apf_fin(i, nw);
}
#ifdef MP3_TONO
static u16 vol_gain(void) { return mp3_tono_gain[0] ? mp3_tono_gain[0] : niveles_gain[D.volumen > 15 ? 15 : D.volumen]; }   /* desarrollo: ganancia en bruto del bloque B */
#else
static u16 vol_gain(void) { return niveles_gain[D.volumen > 15 ? 15 : D.volumen]; }
#endif
static u16 escala(u16 f, u16 g) { return (u16)(((u32)f * g) / 0x200u); }                  /* un paso de las rampas del original (hechas sobre 0x0200) escalado al volumen */

/* ---- hardware: PMU, GPIO, USB, ruta de salida -------------------------------------------------------------------------------------------------- */
static void pmu_w(u8 reg, u8 v) { i2c_revivir(0); pmu_escribir(reg, v); }
static int ruta_sel;                                    /* MP3_RUTA_* pedida */
void mp3_ruta(int r) { ruta_sel = r >= 0 && r < MP3_RUTAS ? r : 0; }
int mp3_ruta_actual(void) { return ruta_sel; }
const char *mp3_ruta_apf(int r)
{
    switch (r) {
    case MP3_RUTA_ALTAVOZ_CERRADO: return "farfield_closed_play";
    case MP3_RUTA_ALTAVOZ_ABIERTO: return "farfield_play";
    case MP3_RUTA_MANOS_LIBRES: return "PHF_play";
    case MP3_RUTA_LINEOUT: return "LO_play";
    case MP3_RUTA_BT_ESTEREO: return "BTL_play";
    }
    return gpio_deslizador() == DESLIZADOR_CERRADO_ES ? "farfield_closed_play" : "farfield_play";      /* el altavoz del teléfono según la corredera */
}
static int es_altavoz(void)
{
    const char *n = mp3_ruta_apf(ruta_sel);
    return n[0] == 'f';                                                           /* farfield_*: el altavoz del teléfono (elevador P3.7 + AMP P1.5) */
}
/* ac_AudioExtHWDriver_FUN_44fd1cac({amp, stepUp}): P1.5 «Standalone AMP» y P3.7 «Audio Stepup», en ese orden [ejecutado: s7.linea.txt 54.17, 54.49, 51.60, 51.80] */
static void gp(int amp, int stepup)
{
    gpio_dir(1, 0x20, 1); gpio_dir(3, 0x80, 1);
    gpio_set(1, 0x20, amp ? 1 : 0); gpio_set(3, 0x80, stepup ? 1 : 0);
    D.p15 = (u8)gpio_get(1, 0x20); D.p37 = (u8)gpio_get(3, 0x80);
}
#ifdef MP3_IGNORA_CABLE            /* SOLO build de desarrollo (puente por USB): el original se niega a sonar con cable, pero arrancar el DSP con el USB en marcha no colgó nada (2026-10-24) */
static int cable_usb(void) { return 0; }
#else
static int cable_usb(void) { int c = hw_cargador(); return c > 0 && (c & 2); }
#endif

/* ---- el fichero ------------------------------------------------------------------------------------------------------------------------------- */
static int rd_cab(void *ctx, u32 off, u8 *buf, u32 n)
{
    u32 k;
    (void)ctx;
    if (D.tam && off >= D.tam) return 0;
    if (D.tam && n > D.tam - off) n = D.tam - off;          /* la Memoria (W6F) da error en una lectura que pasa del final: se pide solo lo que hay */
    k = hw_fs_leer_desde(D.ruta_fichero, off, buf, n);
    return (int)k;
}
/* (2026-10-25) pruebas contra el original: MP3 precargado en RAM (mp3_ram_dir/tam != 0) y cronómetro de hw_mp3_paso por partes (cuentas del
 * temporizador 1, ~30,5 µs): [0] rx_bombear, [1] lectura del fichero, [2] envíos al DSP, [3] resto, [4] vueltas */
volatile u32 mp3_ram_dir, mp3_ram_tam;
volatile u32 mp3_crono[5];
extern volatile u32 tics;
static u32 crono_c(void) { u32 a, c, b; do { a = tics; c = *(volatile u32 *)0xF901001Cu & 0xFFu; b = tics; } while (a != b); return a * 131u + c; }
/* ---- (2026-10-26) EL COLCHÓN delante del DSP (docs/mp3_dsp.md §7) ------------------------------------------------------------------------------
 * Un anillo de bytes con dos contadores ABSOLUTOS, que son posiciones en el fichero (o en el flujo): `escrito` = hasta dónde hay datos y `leido` = hasta
 * dónde ya no hacen falta (lo mandado al DSP, o la etiqueta ID3 saltada). M.pos_arch, el siguiente byte a mandar, está siempre entre los dos. Lo
 * llena UNO (el bucle: relleno() leyendo el fichero, o la app por hw_mp3_flujo_escribir) y lo vacía OTRO (el alimentador del tic, o el bucle con el
 * cerrojo puesto): quien llena escribe los bytes y DESPUÉS sube `escrito`; quien vacía copia y DESPUÉS sube `leido`. Con un solo núcleo basta la barrera
 * del compilador. Para un FICHERO el anillo es del sistema (colchon_sis, MP3_COLCHON_MAX; se usan mp3_colchon_kb) y el bucle lo rellena a trozos de
 * 8 KB como el reproductor del original (anillo de 192 KB, lecturas de 8 KB); para el FLUJO es el de la app (hw_mp3_flujo_abrir). */
#define BARRERA() __asm__ volatile("" ::: "memory")
static struct {
    u8 *buf; u32 tam;
    volatile u32 escrito, leido;
    volatile u8 fin;                                                                 /* no llegará más: el fichero está entero en el anillo / hw_mp3_flujo_fin */
    u8 fichero;                                                                      /* lo llena relleno() desde D.ruta_fichero */
} R;
#define MP3_COLCHON_MAX (192u * 1024u)
static u8 colchon_sis[MP3_COLCHON_MAX] __attribute__((aligned(32)));
volatile u32 mp3_colchon_kb = 192;                                                 /* (puente) KB del colchón que se usan para un fichero, 16..192, al empezar cada pista */
#define RELLENO_TROZO 8192u                                                          /* bytes por lectura del fichero (los del original) */
volatile u32 mp3_relleno_lecturas, mp3_relleno_max_ms, mp3_relleno_ms, mp3_relleno_cortas, mp3_colchon_min;   /* lecturas, la más lenta y el total (ms), las que dieron 0 antes del final; el nivel más bajo sonando (bytes) */
static u32 ring_copiar(u32 desde, u8 *dst, u32 n)                                  /* copia [desde, desde+n) del anillo; devuelve lo que había */
{
    u32 hay, i, p;
    if (!R.buf || desde < R.leido || desde >= R.escrito) return 0;
    hay = R.escrito - desde; if (n > hay) n = hay;
    BARRERA();
    p = desde % R.tam;
    for (i = 0; i < n; i++) { dst[i] = R.buf[p]; if (++p == R.tam) p = 0; }
    return n;
}
/* un FICHERO al colchón del sistema, empezando en el byte `desde` (al empezar a sonar y en cada búsqueda) */
static void colchon_fichero(u32 desde)
{
    u32 kb = mp3_colchon_kb < 16u ? 16u : mp3_colchon_kb > MP3_COLCHON_MAX / 1024u ? MP3_COLCHON_MAX / 1024u : mp3_colchon_kb;
    R.buf = colchon_sis; R.tam = kb * 1024u; R.fichero = 1;
    R.escrito = R.leido = desde; R.fin = D.tam && desde >= D.tam;
    mp3_colchon_min = 0xFFFFFFFFu;
}
static u32 leer_fichero(u32 off, u8 *dst, u32 n)
{
    if (mp3_ram_dir && mp3_ram_tam) {                                                  /* (2026-10-25, prueba) el MP3 precargado en RAM: sin leer la NOR */
        u32 j, w2 = off >= mp3_ram_tam ? 0 : (n > mp3_ram_tam - off ? mp3_ram_tam - off : n);
        for (j = 0; j < w2; j++) dst[j] = ((const u8 *)mp3_ram_dir)[off + j];
        return w2;
    }
    return hw_fs_leer_desde(D.ruta_fichero, off, dst, n);
}
/* EL RELLENO, desde el bucle (el sistema de ficheros no es reentrante: nunca desde el tic): como mucho `max` lecturas de <= 8 KB, cada una si cabe
 * entera; la última del fichero, lo que quede. Sin cerrojo: el tic sigue mandando del colchón mientras tanto. */
static void relleno(int max)
{
    while (max-- > 0 && R.fichero && R.buf && !R.fin) {
        u32 queda = D.tam > R.escrito ? D.tam - R.escrito : 0, n, p, k, t0, dt;
        if (!queda) { R.fin = 1; break; }
        n = queda < RELLENO_TROZO ? queda : RELLENO_TROZO;
        if (R.tam - (R.escrito - R.leido) < n) break;                                  /* no cabe: en otra vuelta */
        p = R.escrito % R.tam;
        if (n > R.tam - p) n = R.tam - p;                                             /* hasta el final del anillo; lo demás, en la lectura siguiente */
        t0 = ahora(); k = leer_fichero(R.escrito, R.buf + p, n); dt = ahora() - t0;
        mp3_relleno_lecturas++; mp3_relleno_ms += dt; if (dt > mp3_relleno_max_ms) mp3_relleno_max_ms = dt;
        if (!k) { R.fin = 1; mp3_relleno_cortas++; break; }                           /* el fichero no da más: que acabe ahí */
        BARRERA();
        R.escrito += k;
        if (R.escrito >= D.tam) R.fin = 1;
    }
}

/* ---- (2026-10-25) LA FUENTE EN RAM («flujo», api/hw.h; docs/mp3_dsp.md §6) ------------------------------------------------------------------------
 * (aún no las llama nadie del sistema —la API 1.7 está por hacer—; las prueba el puente, herr/puente/dsp/exp_flujo.py: mp3_flujo_api las conserva)
 * El colchón (R) es el de la app; F guarda lo propio del flujo: el total, los umbrales de la pausa limpia y sus cuentas. */
static void ini(void);
static struct {
    u8  activo, esperando;
    u32 total, bajo, alto;
    u32 esperas, ms_esperando, t_espera;
} F;
#define FLUJO_BAJO_MS 1000u                                                          /* pausa limpia si quedan menos de ~1 s de audio sin mandar */
#define FLUJO_ALTO_MS 3000u                                                          /* y se sigue al haber ~3 s */
static u32 leer_fs_apf(const char *ruta, u8 *buf, u32 max) { return hw_fs_leer(ruta, buf, max); }

static int rd_flujo(void *ctx, u32 off, u8 *buf, u32 n) { (void)ctx; return (int)ring_copiar(off, buf, n); }
static u32 flujo_bytes_de_ms(u32 ms) { u32 k = M.cab.kbps ? M.cab.kbps : 128u; return k * ms / 8u; }   /* kbps * ms / 8 = bytes */
int hw_mp3_flujo_abrir(void *colchon, u32 tam, u32 total)
{
    ini();
    hw_mp3_parar();                                                                   /* lo que hubiera (fichero u otro flujo) se para */
    D.error = 0; D.sub = 0; M.abierto = 0;
    if (!colchon) {                                                                   /* (1.8, 26-oct) el colchón del SISTEMA (el de los ficheros): la música por la
                                                                                       * red (shell/musica_red.c) sigue con la app cerrada; no hay fichero a la vez */
        u32 kb = mp3_colchon_kb < 16u ? 16u : mp3_colchon_kb > MP3_COLCHON_MAX / 1024u ? MP3_COLCHON_MAX / 1024u : mp3_colchon_kb;
        colchon = colchon_sis; tam = kb * 1024u;
    }
    if (tam < 4096u) { fallo(MP3_E_PARAM, 0); return HW_ERROR; }
    F.activo = 1; F.esperando = 0; F.total = total;
    R.fichero = 0; R.fin = 0; R.buf = (u8 *)colchon; R.tam = tam; R.escrito = R.leido = 0;
    F.esperas = 0; F.ms_esperando = 0;
    D.ruta_fichero[0] = 0; D.tam = total; D.dur_ms = 0; D.pos_ms = 0;
    D.estado = HW_MP3_ABIERTO;
    return HW_OK;
}
u32 hw_mp3_flujo_libre(void) { return F.activo && !R.fin ? R.tam - (R.escrito - R.leido) : 0; }
u32 hw_mp3_flujo_escribir(const void *d, u32 n)
{
    const u8 *s = (const u8 *)d;
    u32 libre = hw_mp3_flujo_libre(), i, p;
    if (!s || !n || !libre) return 0;
    if (n > libre) n = libre;
    p = R.escrito % R.tam;
    for (i = 0; i < n; i++) { R.buf[p] = s[i]; if (++p == R.tam) p = 0; }
    BARRERA();                                                                        /* los bytes antes que el contador: el tic puede estar leyendo */
    R.escrito += n;
    return n;
}
void hw_mp3_flujo_fin(void) { if (F.activo) R.fin = 1; }
void hw_mp3_flujo_info(HwMp3Flujo *i)
{
    i->activo = F.activo; i->fin = R.fin; i->esperando = F.esperando; i->tam = R.tam; i->total = F.total;
    i->nivel = F.activo ? R.escrito - (M.pos_arch > R.leido ? M.pos_arch : R.leido) : 0;
    i->escritos = R.escrito; i->mandados = M.pos_arch; i->bajo = F.bajo; i->alto = F.alto; i->esperas = F.esperas;
    i->ms_esperando = F.ms_esperando + (F.esperando ? ahora() - F.t_espera : 0);
}
/* la cabecera del flujo, del propio colchón, al dar PLAY: como hw_mp3_abrir pero sin fichero */
static int flujo_cabecera(void)
{
    Mp3Cab c;
    int r = mp3_cab_leer(rd_flujo, 0, F.total ? F.total : 0x7FFFFFFFu, &c);
    if (r < 0) {
        if (!R.fin && R.escrito < R.tam) { fallo(MP3_E_FALTAN, r); return HW_ERROR; }   /* aún llegará más: que se reintente */
        fallo(r == -3 ? MP3_E_NO_MP3 : MP3_E_FICHERO, r); return HW_ERROR;
    }
    if (!mp3_cab_codigo_fs(c.fs)) { fallo(MP3_E_FORMATO, 0); return HW_ERROR; }
    if (!F.total) c.dur_ms = 0;                                                       /* sin el tamaño no hay duración (se enseña solo el tiempo sonado) */
    M.cab = c; M.abierto = 1; M.pos_ini = c.inicio; M.pos_arch = c.inicio; M.pos_base_ms = 0;
    R.leido = c.inicio;                                                               /* la etiqueta ID3 ya no hace falta */
    F.bajo = flujo_bytes_de_ms(FLUJO_BAJO_MS); F.alto = flujo_bytes_de_ms(FLUJO_ALTO_MS);
    if (F.alto > R.tam / 2) F.alto = R.tam / 2;
    if (F.bajo > F.alto / 2) F.bajo = F.alto / 2;
    D.fs = c.fs; D.kbps = c.kbps; D.dur_ms = c.dur_ms; D.tam = F.total; D.canales = c.canales; D.vbr = c.vbr; D.pos_ms = 0;
    return HW_OK;
}

/* ---- el estado del DSP: ¿sigue siendo MI sesión? ------------------------------------------------------------------------------------------------ */
static int dsp_mio(void)
{
    const DspEstado *e = dsp_estado();
    return M.dsp_ok && e->vivo && e->gate5 && e->intentos == M.intentos;
}
static void olvidar_dsp(void) { M.dsp_ok = 0; M.proc = 0; M.tiene_cap = 0; D.dsp_modulo = 0; M.mx_limpio = 0; }

/* ---- 0x0800: la salida de audio (canal 0x0B) -> 0x0801 --------------------------------------------------------------------------------------- */
static int salida_0800(int i)
{
    DspMsg m;
    unsigned nw = plan_copiar(i, buf_mx);
    int r;
    R16(0xFA02003E) = 0;                                                              /* FUN_441A70A4: el puerto PCM, antes de la orden */
    r = enviar(0x0B, buf_mx, nw);
    if (r) return r;
    if (esperar(&m, 0x0B, 0x0801, 500)) return fallo(MP3_E_SALIDA, 0);
    return 0;
}
/* la salida a 44,1 kHz va siempre igual (P20 = 0xAC44); el decodificador convierte la frecuencia del MP3 (CONFIG) */

/* ---- el arranque del DSP (con el módulo de arranque) ------------------------------------------------------------------------------------------------- */
static int env_1101(void)                                                             /* 0x1101 {0, tam_hi, tam_lo, dir_hi, dir_lo} por el canal 0x16 */
{
    const DspEstado *e = dsp_estado();
    u16 w[6];
    w[0] = 0x1101; w[1] = 0; w[2] = (u16)(e->xram_cap >> 16); w[3] = (u16)e->xram_cap; w[4] = (u16)(e->xram_dir >> 16); w[5] = (u16)e->xram_dir;
    return enviar(0x16, w, 6);
}
/* El reloj del bloque PCM del SoC (puerta 26 + bloque 20): el ORIGINAL lo CIERRA al acabar el arranque del DSP (FUN_4459F428 estado 2, 5.07 s de s7.dbg.txt) y solo lo ABRE
 * en la fase de línea, JUSTO DESPUÉS de que el códec dé su reloj PCM (pmu C9 |= 0x80 a 54.3296 s; luego ClockRequest(26, modo 4) y block_reset(20, 0)). Dejarlo abierto desde el
 * arranque hace que el R_91 (que pone al DSP a usar el PCM) lo deje parado: sin un solo tic entre dos volcados de su memoria (2026-10-24). */
static void pcm_reloj(int on)
{
    if (on) { dspi_puerta(26, 1); dspi_bloque(20, 0); }
    else    { dspi_bloque(20, 1); dspi_puerta(26, 0); }
}
static int dsp_levantar(void)
{
    int r;
    fase_ini(MP3_F_DSP);
    if (!dsp_estado()->vivo || !dsp_estado()->gate5) {
        MIGA(10);
        r = dsp_arrancar();
        MIGA(11);
        if (r) { fase_fin(MP3_F_DSP, 0); return fallo(MP3_E_DSP, r); }
        r = env_1101();
        MIGA(12);                                                              /* igual que el original al arrancar el teléfono: versión y luego 0x1101 */
        if (r) { fase_fin(MP3_F_DSP, 0); return r; }
        pcm_reloj(0);                                                         /* el original cierra el reloj PCM al acabar el arranque (estado 2) */
    }
    M.intentos = dsp_estado()->intentos; D.dsp_modulo = 1;
    return fase_fin(MP3_F_DSP, 0);
}

/* El mezclador tras ARRANCAR el DSP: el original lo hace al encender el teléfono, justo después de 0x0201/0x1101, y todos sus registros lo traen delante de la primera
 * reproducción (s7.dsp.log 5.10 y 8.60 s): SetSampleRate(32 kHz), R_10, R_4 0x2000, R_4 0x4000, SetSampleRate(44,1 kHz). Palabras tal cual se vieron (hw6 y los dos
 * últimos de R_10 eran basura de pila en el original: aquí, ceros). Como el DSP lo arrancamos nosotros al sonar, esta inicialización también es nuestra. */
static const u16 mx_boot_r0a[11]  = { 0x0900, 0x0004, 0, 0x0000, 1, 0x00D3, 0, 0x4038, 0x4544, 0x0007, 0x0000 };
static const u16 mx_boot_r10[13]  = { 0x0900, 0x0006, 0, 0x000A, 1, 0x00D3, 0, 0x4038, 0x4544, 0x0000, 0x0000, 0x0000, 0x0000 };
static const u16 mx_boot_r4a[13]  = { 0x0900, 0x0006, 0, 0x0004, 1, 0x00D3, 0, 0x4038, 0x4544, 0x0000, 0x2000, 0x2000, 0x0000 };
static const u16 mx_boot_r4b[13]  = { 0x0900, 0x0006, 0, 0x0004, 1, 0x00D3, 0, 0x4038, 0x4544, 0x0000, 0x4000, 0x4000, 0x0000 };
static const u16 mx_boot_r0b[11]  = { 0x0900, 0x0004, 0, 0x0000, 1, 0x00D3, 0, 0x4038, 0x4544, 0x0008, 0x0000 };
static int mx_boot(void)
{
    static const struct { const u16 *w; unsigned n; } m[5] = { { mx_boot_r0a, 11 }, { mx_boot_r10, 13 }, { mx_boot_r4a, 13 }, { mx_boot_r4b, 13 }, { mx_boot_r0b, 11 } };
    unsigned i, k;
    D.rpc_seguidos_mal = 0;
    for (i = 0; i < 5; i++) {
        for (k = 0; k < m[i].n; k++) buf_mx[k] = m[i].w[k];
        if (i == 0) { mb_a = 0; mb_b = 0; mb_conn = 0; h_fil = 0; }                   /* DSP nuevo: ningún bloque ni efecto */
        if (i == 2 || i == 3) buf_mx[9] = mb_a;                                       /* los R_4 son del bloque que acaba de crear el R_10 */
        MIGA(20u + i);
        rpc(buf_mx, m[i].n);
        if (i == 1 && rx_mx_n > 11) mb_a = rx_mx[11];
        MIGA(30u + i);
        if (D.rpc_seguidos_mal >= 2) return fallo(MP3_E_MEZCLADOR, D.rpc_mal_n);
        espera(5);
    }
    return 0;
}

/* ---- F1: abrir la línea de audio: APF -> códec -> mezclador (R_n) con GPIO y escalones intercalados, como el original (tmp/mp3/ref_hw.py + s7.linea.txt) ------------- */
static int apf_elegir(void)
{
    const char *n = mp3_ruta_apf(ruta_sel);
    int r, o = 0, k = 0;
    while (n[k] && k < (int)sizeof D.apf_nombre - 1) { D.apf_nombre[k] = n[k]; k++; }
    D.apf_nombre[k] = 0;
    r = mp3_apf_cargar(n, leer_fs_apf, &o);
    D.apf_origen = (u8)o;
    return r;
}
static int mx_r4(int plan_i, u16 g, u16 u)                                            /* un R_4 {g, g, u} con la cabecera de esa plantilla */
{
    unsigned nw = plan_copiar(plan_i, buf_mx);
    buf_mx[10] = g; buf_mx[11] = g; buf_mx[12] = u;
    return rpc(buf_mx, nw);
}
#define MAL() (D.rpc_seguidos_mal >= 2)
#define SIGUE(e) do { r = (e); if (MAL()) goto mal; } while (0)
/* con_apf: manda también los bloques del APF al mezclador (R_6 filtro, R_9, R_6 DRC, R_9, R_9 DRC) y toca el códec y los GPIO (la línea estaba baja);
 * sin él (otra pista con la línea ya arriba) solo el mezclador: R_10 … rampa de volumen (el F6 del original) */
static int f1_linea(int con_apf)
{
    u16 g[5], v = vol_gain();
    int r = 0, i;
    if (con_apf) {
        fase_ini(MP3_F_APF);
        MIGA(50);
        if (apf_elegir()) { fase_fin(MP3_F_APF, 0); return fallo(MP3_E_APF, 0); }
        MIGA(51);
        fase_fin(MP3_F_APF, 0);
        fase_ini(MP3_F_CODEC);
        if (D.linea == 0) {                                                           /* chip apagado: la sombra parte de lo que deja el inicio del original */
            Mp3CodHw hw; hw.pmu = pmu_w;
            mp3_cod_iniciar(&hw); M.hw_cod = 1;
        }
        MIGA(52);
        mp3_cod_aplicar_apf();                                                        /* los 53 campos, en orden */
        MIGA(53);
        mp3_cod_escalon(0, 0);                                                        /* todo apagado antes de encender (54.05) */
        D.linea = 1;
        fase_fin(MP3_F_CODEC, 0);
    }
    fase_ini(MP3_F_LINEA);
    D.rpc_seguidos_mal = 0; M.mx_limpio = 0;
    if (con_apf) {
        SIGUE(mx_apf(MP3_P_F1_R6_FILTRO, mp3_apf_filtro));
        SIGUE(mx(MP3_P_F1_R9_A));                                                     /* clave = manejador del filtro. El DRC y el banco (R_6 tipos 6 y 5) el DSP real los rechaza con código 0x10: se omiten */
        mp3_cod_subir();                                                              /* vincenne(0x66, 1): C0|=1, C0|=2, escalón arriba (54.11) */
        if (es_altavoz()) { gp(0, 0); gp(0, 1); } else gp(0, 0);                      /* AudioExtHW {amp=0, stepUp=0} y {0, 1} (54.17) */
        mp3_cod_escalon(1, 1); mp3_cod_escalon(1, 1);                                 /* CreateChannel / OpenLine repiten el escalón (54.18, 54.21) */
        D.linea = 2;
    }
    SIGUE(mx_rango(MP3_P_F1_R10, MP3_P_F1_R11));                                      /* R_10, R_4 0x4000, R_12, R_4 0, R_11 */
    if (con_apf) { mp3_cod_rmw(0xC9, 0xC0, 0x80); pcm_reloj(1); mp3_cod_escalon(1, 1); }   /* PCM_clk(1): C9 |= 0x80 (54.33) y, JUSTO DESPUÉS, puerta 26 modo 4 + bloque 20 fuera de reset (54.3371/54.3375) */
    r = salida_0800(MP3_P_F1_0800);                                                   /* 0x0800 -> 0x0801 (la salida a 44,1 kHz) */
    if (r) return fase_fin(MP3_F_LINEA, r);
    SIGUE(mx_rango(MP3_P_F1_R91, MP3_P_F1_R3));                                       /* R_91, R_13, R_3 (sin el banco ni su R_9) */
    SIGUE(mx_grupo());                                                                /* el primer grupo de 5 R_9 */
    if (con_apf && es_altavoz()) gp(1, 1);                                            /* AudioExtHW {amp=1, stepUp=1} (54.49) */
    if (con_apf) { mp3_cod_escalon(1, 1); mp3_cod_escalon(1, 1); }                    /* 54.50, 54.64 */
    for (i = 0; i < 5; i++) g[i] = i == 4 ? v : (rampa_ini[i] < v ? rampa_ini[i] : v);
    for (i = 0; i < 5; i++) {                                                         /* la rampa de entrada: 5 R_9 y un R_4 (u = 5) por paso */
        SIGUE(mx_grupo());
        SIGUE(mx_ganancia(g[i], 5));
    }
    return fase_fin(MP3_F_LINEA, 0);
mal:
    return fase_fin(MP3_F_LINEA, fallo(MP3_E_MEZCLADOR, D.rpc_mal_n));
}

/* (2026-10-25) La línea tras una PARADA con el DSP vivo y el módulo MP3 (secuencia(), camino corto): el hardware de f1_linea en su orden (APF, códec, escalones,
 * elevador; C9 |= 0x80 del reloj PCM del códec) SIN los mensajes del mezclador, que pone f3_postboot. El amplificador (P1.5) se deja para linea_hw_amp(), con el
 * mezclador ya configurado, como en f1_linea. */
static int linea_hw_subir(void)
{
    fase_ini(MP3_F_APF);
    if (apf_elegir()) { fase_fin(MP3_F_APF, 0); return fallo(MP3_E_APF, 0); }
    fase_fin(MP3_F_APF, 0);
    fase_ini(MP3_F_CODEC);
    if (D.linea == 0) { Mp3CodHw hw; hw.pmu = pmu_w; mp3_cod_iniciar(&hw); M.hw_cod = 1; }
    mp3_cod_aplicar_apf();
    mp3_cod_escalon(0, 0);
    D.linea = 1;
    mp3_cod_subir();
    if (es_altavoz()) { gp(0, 0); gp(0, 1); } else gp(0, 0);
    mp3_cod_escalon(1, 1); mp3_cod_escalon(1, 1);
    D.linea = 2;
    mp3_cod_rmw(0xC9, 0xC0, 0x80); mp3_cod_escalon(1, 1);                           /* el reloj PCM del códec; la puerta 26 y el bloque 20 los abre f3_postboot */
    return fase_fin(MP3_F_CODEC, 0);
}
static void linea_hw_amp(void)
{
    if (es_altavoz()) gp(1, 1);
    mp3_cod_escalon(1, 1); mp3_cod_escalon(1, 1);
}

/* ---- F2: PreBoot, cambio de módulo, 0x1101 y versión ---------------------------------------------------------------------------------------- */
/* (2026-10-25) prueba: con 1 NO se apaga el reloj del PCM durante la recarga. Hipótesis: el módulo MP3 configura sus puertos del PCM (0x40xx) mientras
 * senew aún espera (dspi_servir 700 ms) con el PCM sin reloj -> errores de bus 0x40005512/0x40005514 y PCM mal configurado; el original lo reenciende ~10 ms
 * después de soltar el DSP (PostBoot). Por omisión 1 desde el 2026-10-25 (banco de la música). */
volatile u32 mp3_pcm_en_recarga = 1;                  /* (2026-10-25) POR OMISIÓN 1: medido en el banco de la música, sin él salen los errores de bus 0x40005514/12 al arrancar; con él, ninguno */
static int f2_modulo(void)
{
    int r;
    fase_ini(MP3_F_PREBOOT);
    D.rpc_seguidos_mal = 0;
    if (h_fil) { r = mx(MP3_P_PRE_R7A); if (MAL()) return fase_fin(MP3_F_PREBOOT, fallo(MP3_E_MEZCLADOR, D.rpc_mal_n)); }   /* R_7 del filtro: con un manejador que no existe el DSP no contesta */
    r = mx(MP3_P_PRE_R14);  if (!MAL()) r = mx(MP3_P_PRE_R91);  if (!MAL()) r = mx(MP3_P_PRE_R16);  if (!MAL()) r = mx(MP3_P_PRE_R17A);
    if (!MAL()) r = mx(MP3_P_PRE_R15);  if (!MAL()) r = mx(MP3_P_PRE_R17B);            /* R_14, R_91, R_16, R_17, R_15, R_17: el mezclador a cero */
    if (MAL()) return fase_fin(MP3_F_PREBOOT, fallo(MP3_E_MEZCLADOR, D.rpc_mal_n));
    if (!mp3_pcm_en_recarga) pcm_reloj(0);                                            /* PCM_clk(estado 2) justo antes de DSP_RM_LoadAllMemories (s7.linea.txt 56.9116): bloque 20 a reset y puerta 26 cerrada DURANTE la recarga */
    fase_fin(MP3_F_PREBOOT, 0);
    fase_ini(MP3_F_MODULO);
    r = dsp_cargar(&dsp_img_mp3);                                                     /* DSP_RM_LoadAllMemories: la común + el módulo MP3 (ventana + XRAM) */
    if (r) { olvidar_dsp(); fase_fin(MP3_F_MODULO, 0); return fallo(MP3_E_DSP, r); }
    D.dsp_modulo = 2; mb_a = 0; mb_b = 0; mb_conn = 0; h_fil = 0;                      /* el DSP se reinició: sus bloques y efectos ya no existen */
    fase_fin(MP3_F_MODULO, 0);
    fase_ini(MP3_F_VERSION);
    r = env_1101();
    if (r) return fase_fin(MP3_F_VERSION, r);
    r = dsp_pedir_version();                                                          /* 0x0201 -> 0x0202 + la cadena */
    if (r) return fase_fin(MP3_F_VERSION, fallo(MP3_E_VERSION, r));
    { const char *v = dsp_version(); int k = 0; while (v[k] && k < (int)sizeof D.version - 1) { D.version[k] = v[k]; k++; } D.version[k] = 0; }
    D.version_ok = 1;
    return fase_fin(MP3_F_VERSION, 0);
}

/* ---- F3: PostBoot: el mezclador otra vez (el reset del DSP lo borró) ------------------------------------------------------------------------- */
static int f3_postboot(void)
{
    int r = 0;
    fase_ini(MP3_F_POSTBOOT);
    D.rpc_seguidos_mal = 0; M.mx_limpio = 0;
    SIGUE(mx_rango(MP3_P_F3_R0, MP3_P_F3_R4A));                                       /* R_0 (SetSampleRate 44,1), R_10, R_4 0x4000 */
    SIGUE(mx(MP3_P_F3_R10B));
    SIGUE(mx_r4(MP3_P_F3_R4B, vol_gain(), 0));                                        /* R_4 con el volumen */
    SIGUE(mx_rango(MP3_P_F3_R12, MP3_P_F3_R11));                                      /* R_12, R_11 */
    pcm_reloj(1);                                                                     /* PCM_clk(1) antes del 0x0800, como en la línea (F1) */
    r = salida_0800(MP3_P_F3_0800);
    if (r) return fase_fin(MP3_F_POSTBOOT, r);
    SIGUE(mx_rango(MP3_P_F3_R91, MP3_P_F3_R3));                                       /* R_91, R_13, R_3 */
    SIGUE(mx_apf(MP3_P_F3_R6_FILTRO, mp3_apf_filtro));
    SIGUE(mx(MP3_P_F3_R9_A));                                                         /* (sin banco ni DRC: ver f1_linea) */
    fase_fin(MP3_F_POSTBOOT, 0);
    fase_ini(MP3_F_VERSION2);
    r = dsp_pedir_version();                                                          /* la segunda 0x0201 (57.184) */
    return fase_fin(MP3_F_VERSION2, r ? fallo(MP3_E_VERSION, r) : 0);
mal:
    return fase_fin(MP3_F_POSTBOOT, fallo(MP3_E_MEZCLADOR, D.rpc_mal_n));
}

/* ---- lo que el DSP manda por su cuenta: créditos del flujo y eventos del decodificador ---------------------------------------------------------- */
#ifdef MP3_TONO
/* avisos del MEZCLADOR del DSP (2026-10-24): 0x0901 por el canal 0x0C (4417.c FUN_4417c68c → 445A.c FUN_445a040c «The Audio Mixer sent a warning!»): código en
 * [7]:[8], extras [9]:[10] y [11]:[12] (0x41 datos perdidos, 0x42 desbordamiento de entrada, 0x43 de salida, 0x44/45 fallo de entrada/salida, 0x46-49 RSRC...).
 * Antes rx_bombear los tiraba. mp3_mxav[k] = {ms, n, palabras 0..13} */
volatile u32 mp3_mxav_n; volatile u16 mp3_mxav[32][16];
volatile u32 mp3_otros_n; volatile u16 mp3_otros[32][8];   /* cualquier otro mensaje que rx_bombear no sabía qué hacer: {ms, canal, n, d0..d4} */
#endif
#ifdef MP3_TONO
volatile u32 mp3_cred_n; volatile u32 mp3_cred[2048];   /* desarrollo: cada crédito del flujo {hora ms << 12 | palabras} para ver el ritmo real de consumo del DSP */
#endif
/* (2026-10-26) EL FILTRO del transporte (dsp_filtro_rx): los CRÉDITOS de nuestro flujo (0x05id / 0x02id {n}: n palabras libres en el anillo del DSP) y
 * el 0x0E04 del decodificador se cuentan EN CUANTO SE LEEN del buzón, los lea el tic o el bucle (antes los contaba rx_bombear desde la cola). Corre
 * con el transporte tomado por uno de los dos (el cerrojo dsp_bucle): M.espacio no se pisa. Mientras parar/buscar esperan el 0x02xx del vaciado
 * (M.vaciando) ese se deja pasar a la cola. */
static int filtro_rx(unsigned canal, const u16 *d, unsigned n)
{
    if (!M.proc || n < 1) return 0;
    if (canal == 0x14) {
        unsigned code = d[0] >> 8, id = d[0] & 0xFFu;
        if (id != M.flujo || n < 2 || !(code == 5 || (code == 2 && !M.vaciando))) return 0;
        M.espacio += d[1]; D.espacio = M.espacio;
        D.cred_pal += d[1]; D.cred_msgs++; D.t_ult_credito = ahora(); M.t_credito = ahora();
#ifdef MP3_TONO
        mp3_cred[mp3_cred_n++ % 2048u] = (ahora() << 12) | (d[1] & 0xFFFu);
#endif
        return 1;
    }
    if (canal == M.canal && d[0] == 0x0E04) {                                         /* 0x0E04 {motivo}: 1 = sin datos, 2 = fin de flujo */
        if (n >= 2 && d[1] == 2) D.eos_recibido = 1;
        else D.nodata++;
        return 1;
    }
    return 0;
}
static void rx_bombear(void)                            /* NO usar mientras se espera la respuesta de otra cosa: vacía la cola entera (los créditos ya no llegan a ella) */
{
    DspMsg m;
    unsigned guarda = 0;
    while (guarda++ < 64 && dsp_recibir(&m, 0xFFFF) == 0) {
#ifdef MP3_TONO
        if (m.canal == 0x0C && m.n >= 1 && m.d[0] == 0x0901) {
            volatile u16 *a = mp3_mxav[mp3_mxav_n++ % 32u]; unsigned k;
            a[0] = (u16)ahora(); a[1] = (u16)m.n; for (k = 0; k < 14; k++) a[2 + k] = k < m.n ? m.d[k] : 0;
        } else {
            volatile u16 *a = mp3_otros[mp3_otros_n++ % 32u]; unsigned k;
            a[0] = (u16)ahora(); a[1] = m.canal; a[2] = (u16)m.n; for (k = 0; k < 5; k++) a[3 + k] = k < m.n ? m.d[k] : 0;
        }
#endif
    }
}

/* ---- una orden al decodificador y su confirmación ------------------------------------------------------------------------------------------ */
static int accion(unsigned a, unsigned ms)              /* 0x0E02 {a} -> 0x0E03 {0}: 0 PLAY, 1 STOP, 2 PAUSA, 3 LIBERAR */
{
    DspMsg m;
    u16 w[2];
    w[0] = 0x0E02; w[1] = (u16)a;
    if (enviar(M.canal, w, 2)) return MP3_E_ENVIO;
    if (esperar(&m, M.canal, 0x0E03, ms)) return fallo(MP3_E_ACCION, (int)a);
    return 0;
}
static int cfg_y_umbral(void)                           /* CONFIG (frecuencia y canales de ESTE mp3) y umbral del flujo */
{
    DspMsg m;
    u16 w[16];
    unsigned nw;
    nw = plan_copiar(MP3_P_M0E00, w);
    w[1] = (u16)mp3_cab_codigo_fs(M.cab.fs); w[2] = M.cab.canales == 1 ? 1 : 2; w[nw - 1] = 0;
    if (enviar(M.canal, w, nw)) return MP3_E_ENVIO;
    if (esperar(&m, M.canal, 0x0E01, 2000)) return fallo(MP3_E_CONFIG, 0);
    if (m.n >= 2 && m.d[1] != 0) return fallo(MP3_E_CONFIG, (int)(signed char)m.d[1]);
    nw = plan_copiar(MP3_P_M0404, w);
    w[0] = (u16)(0x0400u | M.flujo);                                                  /* el umbral: 0x309 palabras para el MP3 */
    if (enviar(0x14, w, nw)) return MP3_E_ENVIO;
    return 0;
}

/* ---- el colchón al anillo del DSP: trozos de 500 bytes = 250 palabras (registro de 6 + 250 = 256; el ARM comprueba espacio - 1 >= 256, FUN_441A51F8) ---- */
static u16 buf_dat[8 + 256];
/* (2026-10-25, prueba contra el original) bytes por registro de datos: 500 = como el original (mensaje LARGO por DMA); con <= 42 el registro
 * (8 + 21 palabras) viaja como mensaje CORTO por el buzón, sin DMA: si así el reloj del DSP vuelve al 100 %, la pérdida está en el camino largo. */
volatile u32 mp3_trozo_bytes = 500u;
static u32 trozo(void) { return (mp3_trozo_bytes >= 2u && mp3_trozo_bytes <= 500u) ? mp3_trozo_bytes : 500u; }
/* (2026-10-26) el registro siguiente, del colchón a buf_dat: {0x08id, 6+nw, 1, 0, 1, 0, 0xFFFF, nw, datos big-endian}; devuelve los bytes (k) y
 * deja en *nw las palabras de datos. Lo usan el bucle (alimentar) y el tic (alim_mandar): nunca a la vez (el cerrojo). */
static u32 armar_registro(u32 tb, u32 *pnw)
{
    u32 k, nw, i, p;
    if (!R.buf) return 0;
    k = R.escrito - M.pos_arch; if (k > tb) k = tb;
    BARRERA();
    nw = (k + 1) / 2; p = M.pos_arch % R.tam;
    for (i = 0; i < nw; i++) {                                                       /* big-endian: el 1.º byte en la parte alta */
        u16 hi = R.buf[p], lo = 0;
        if (++p == R.tam) p = 0;
        if (2 * i + 1 < k) { lo = R.buf[p]; if (++p == R.tam) p = 0; }
        buf_dat[8 + i] = (u16)((hi << 8) | lo);
    }
    buf_dat[0] = (u16)(0x0800u | M.flujo); buf_dat[1] = (u16)(6 + nw);
    buf_dat[2] = 1; buf_dat[3] = 0; buf_dat[4] = 1; buf_dat[5] = 0; buf_dat[6] = 0xFFFF; buf_dat[7] = (u16)nw;
    *pnw = nw;
    return k;
}
static void armar_fin(void)                                                          /* el registro de fin de pista (kind 2, sin carga) */
{
    buf_dat[0] = (u16)(0x0800u | M.flujo); buf_dat[1] = 6; buf_dat[2] = 2; buf_dat[3] = 0; buf_dat[4] = 1; buf_dat[5] = 0; buf_dat[6] = 0xFFFF; buf_dat[7] = 0;
}
static void registro_hecho(u32 k, u32 nw)                                            /* el DSP ya lo tiene: se cuenta y su sitio en el colchón queda libre */
{
    M.espacio -= (int)(6 + nw); D.espacio = M.espacio; M.pos_arch += k;
    BARRERA();
    R.leido = M.pos_arch;
    D.bytes_enviados += k; D.pal_enviadas += 8 + nw; D.trozos++;
}
static void fin_hecho(void) { M.espacio -= 6; D.espacio = M.espacio; D.eos_enviado = 1; D.pal_enviadas += 8; }
/* desde el BUCLE, con el transporte tomado (el prellenado, la búsqueda, las rampas y el bucle sin alimentador): registros mientras quepan en el DSP */
static int alimentar(int max)
{
    int n = 0;
    while (n < max && !D.eos_enviado && M.espacio - 1 >= 256) {
        u32 tb = trozo(), hay = R.escrito - M.pos_arch, k, nw, c1;
        int r;
        if (hay < tb && !R.fin) break;                                               /* falta un registro entero en el colchón: se espera */
        if (!hay) {                                                                   /* se acabó: el registro de fin de pista */
            armar_fin();
            r = enviar(0x14, buf_dat, 8);
            if (r) return r;
            fin_hecho();
            break;
        }
        k = armar_registro(tb, &nw);
        c1 = crono_c();
        r = enviar(0x14, buf_dat, 8 + nw);
        mp3_crono[2] += crono_c() - c1;
        if (r) return r;
        registro_hecho(k, nw); n++;
        latir();
    }
    if (M.espacio - 1 < 256 && !D.eos_enviado) D.cred_pedidos++;
    return n;
}

/* ---- (2026-10-26) EL ALIMENTADOR DEL TIC (docs/mp3_dsp.md §7; el papel de MMAD y de la IRQ 9 del original, docs/re_musica_segundo_plano.md) ----------
 * Cada 4 ms, desde la interrupción del tic (tic_audio, hal/timer.c), SONANDO y con el transporte libre (dsp_bucle == 0): termina el registro que dejó en
 * vuelo, lee el buzón (los créditos los cuenta el filtro) y, si el DSP tiene sitio y el colchón un registro entero, lanza el siguiente (DMA + {2, canal,
 * tam}); el fin de ese envío lo mira el tic siguiente. Como mucho UN registro por tic (500 B / 4 ms = 125 KB/s; el MP3 pide 16 KB/s a 128 kbps). Nada
 * espera, nada lee el sistema de ficheros, nada pinta: si algo falla deja el motivo en A.fallo y el bucle aborta (hw_mp3_paso). El bucle se queda
 * con las RPC, las rampas, el arranque, el cambio de pista y el RELLENO del colchón. mp3_alim = 0 lo apaga (el bucle alimenta como antes). */
volatile u32 mp3_alim = 1;
static struct {
    volatile int fallo, sub;                             /* lo que falló dentro del tic (MP3_E_*): lo aborta el bucle */
    u32 k, nw;                                           /* el registro en vuelo */
    u8  fin;                                             /* el registro en vuelo es el de fin de pista */
    volatile u8 rx_bucle;                                /* un mensaje de control (recepción larga) espera al bucle */
    u32 t_ult;                                           /* tic de la última pasada sonando (0 = no sonaba) */
    u32 t_limpieza;
} A;
/* contadores (solo RAM): pasadas que trabajaron, registros lanzados, tics saltados por el cerrojo, el hueco más largo entre dos pasadas sonando (ms)
 * y cuándo, tics sin datos en el colchón con sitio en el DSP, envíos aplazados por el buzón, tiempo dentro (cuentas de 30,5 µs: el máximo y el total),
 * registros terminados por el bucle y pasadas con el DSP lleno */
volatile u32 mp3_alim_pasadas, mp3_alim_registros, mp3_alim_saltos, mp3_alim_hueco_max, mp3_alim_hueco_cuando, mp3_alim_huecos_100;
volatile u32 mp3_alim_secos, mp3_alim_buzon, mp3_alim_irq_max, mp3_alim_irq_total, mp3_alim_llenos, mp3_alim_fallos;
static int alim_toca(void)
{
    return mp3_alim && D.estado == HW_MP3_SONANDO && M.proc && M.tiene_cap && !A.fallo && dsp_mio() && !dsp_estado()->ocupado;
}
static void alim_fallo(int e, int sub) { if (!A.fallo) { A.sub = sub; A.fallo = e; mp3_alim_fallos++; } }
static void vuelo_fin(int r)                             /* un registro del tic llegó (r > 0) o no (r < 0); lo llama el tic o el bucle al tomar el transporte */
{
    if (r > 0) { if (A.fin) fin_hecho(); else registro_hecho(A.k, A.nw); }
    else alim_fallo(MP3_E_ENVIO, r);
    A.fin = 0;
}
static void alim_mandar(void)
{
    u32 hay, tb = trozo(), k, nw;
    int r;
    if (D.eos_enviado) return;
    if (M.espacio - 1 < 256) { mp3_alim_llenos++; return; }
    hay = R.escrito - M.pos_arch;
    if (hay < tb && !R.fin) { mp3_alim_secos++; return; }
    if (!hay) {                                                                      /* el fichero (o el flujo) se acabó: el registro de fin, corto */
        armar_fin();
        r = dspi_tx_corto_ya(0x14, buf_dat, 8);
        if (!r) { fin_hecho(); return; }
    } else {
        k = armar_registro(tb, &nw);
        r = dspi_tx_largo_iniciar(0x14, buf_dat, 8 + nw);
        if (!r) { A.k = k; A.nw = nw; A.fin = 0; mp3_alim_registros++; return; }
    }
    if (r == DSP_E_BUZON || r == DSP_E_OCUPADO) mp3_alim_buzon++;                   /* el DSP aún no leyó lo anterior: en el tic siguiente */
    else alim_fallo(MP3_E_ENVIO, r);
}
/* el reloj del tiempo dentro de la IRQ (cuentas del temporizador 1, 30,5 µs; dentro de la IRQ `tics` no cambia): el contador va en otro reloj
 * (32 kHz) y una lectura justo en el cambio puede salir mal (medido: restas de -1), así que se lee hasta que dos lecturas seguidas coinciden */
static u32 crono_irq(void) { u32 c, c2; do { c = *(volatile u32 *)0xF901001Cu & 0xFFu; c2 = *(volatile u32 *)0xF901001Cu & 0xFFu; } while (c != c2); return tics * 131u + c; }
volatile u32 mp3_alim_crono_raros;
static void alim_tic(void)
{
    u32 c0, t, c;
    if (!alim_toca()) { A.t_ult = 0; return; }
    if (dsp_bucle) { mp3_alim_saltos++; return; }                                   /* el bucle está en el transporte: ni tocarlo */
    c0 = crono_irq(); t = tics;
    if (A.t_ult) {
        u32 h = (t - A.t_ult) * 4u;
        if (h > mp3_alim_hueco_max) { mp3_alim_hueco_max = h; mp3_alim_hueco_cuando = t * 4u; }
        if (h >= 100u) mp3_alim_huecos_100++;
    }
    A.t_ult = t;
    mp3_alim_pasadas++;
    dsp_en_tic = 1;
    if (dspi_tx_en_vuelo()) {
        int r = dspi_tx_largo_mirar();
        if (r == 0) goto fin;                                                        /* aún en vuelo */
        vuelo_fin(r);
        if (r < 0) goto fin;
    }
    if (dspi_tic_recibir()) A.rx_bucle = 1;
    if (!A.fallo) alim_mandar();
    if (R.fichero) { u32 nivel = R.escrito - M.pos_arch; if (nivel < mp3_colchon_min) mp3_colchon_min = nivel; }
fin:
    dsp_en_tic = 0;
    c = crono_irq() - c0;
    if (c > 0x80000000u) { mp3_alim_crono_raros++; c = 0; }
    mp3_alim_irq_total += c; if (c > mp3_alim_irq_max) mp3_alim_irq_max = c;
}
/* (2026-10-26) EL CERROJO del motor: dsp_bucle_tomar/soltar (hal/dsp_w580.c) alrededor de cada secuencia que habla con el DSP desde el bucle (las
 * funciones públicas enteras; en hw_mp3_paso solo lo que lo necesita). Se mide cuánto lo tiene el bucle con la música sonando. */
volatile u32 mp3_cerrojo_max_ms, mp3_cerrojo_max_cuando, mp3_cerrojo_100;
static u8 cerr_n; static u32 cerr_t0; static u8 cerr_sonaba;
static void tomar(void)
{
    if (cerr_n++ == 0) { cerr_t0 = ahora(); cerr_sonaba = D.estado == HW_MP3_SONANDO; }
    dsp_bucle_tomar();
}
static void soltar(void)
{
    dsp_bucle_soltar();
    if (cerr_n && --cerr_n == 0 && cerr_sonaba) {
        u32 t = ahora() - cerr_t0;
        if (t > mp3_cerrojo_max_ms) { mp3_cerrojo_max_ms = t; mp3_cerrojo_max_cuando = ahora(); }
        if (t >= 100u) mp3_cerrojo_100++;
    }
}

/* ---- F4: el proceso decodificador (gestor de recursos, INIT, R_2, CONFIG, umbral, datos y PLAY) ------------------------------------------------- */
static int f4_proceso(u32 desde_byte)
{
    DspMsg m;
    u16 w[16];
    unsigned nw;
    int r;
    M.proc = 0; D.eos_enviado = D.eos_recibido = 0; D.bytes_enviados = 0; M.espacio = 0; M.tiene_cap = 0; cred_logs = 0;
    fase_ini(MP3_F_PROCESO);
    nw = plan_copiar(MP3_P_M1208, w);
    if (enviar(0x26, w, nw)) return fase_fin(MP3_F_PROCESO, MP3_E_ENVIO);
    if (esperar(&m, 0x26, 0x1209, 1000)) return fase_fin(MP3_F_PROCESO, fallo(MP3_E_PROCESO, 0));
    if (m.n < 5 || m.d[2] != 0) return fase_fin(MP3_F_PROCESO, fallo(MP3_E_PROCESO, m.n > 2 ? (int)(signed char)m.d[2] : 1));
    M.idx = (u8)m.d[1]; M.canal = m.d[3]; M.flujo = m.d[4];
    D.proceso_canal = M.canal; D.flujo = M.flujo; M.proc = 1;
    fase_fin(MP3_F_PROCESO, 0);                                                       /* (el 0x03ff {capacidad} del anillo NO llega aquí con el DSP real: el original manda 0x120A a los 2 ms del 0x1208 sin esperarlo; se recoge después) */
    fase_ini(MP3_F_ARRANCA);
    nw = plan_copiar(MP3_P_M120A, w); w[1] = M.idx;
    if (enviar(0x26, w, nw)) return fase_fin(MP3_F_ARRANCA, MP3_E_ENVIO);
    if (esperar(&m, 0x26, 0x120B, 1000)) return fase_fin(MP3_F_ARRANCA, fallo(MP3_E_ARRANCA, 0));
    fase_fin(MP3_F_ARRANCA, 0);
    fase_ini(MP3_F_INIT);
    nw = plan_copiar(MP3_P_M0E06, w);
    if (enviar(M.canal, w, nw)) return fase_fin(MP3_F_INIT, MP3_E_ENVIO);
    if (esperar(&m, M.canal, 0x0E07, 2000)) return fase_fin(MP3_F_INIT, fallo(MP3_E_INIT, 0));
    fase_fin(MP3_F_INIT, 0);
    fase_ini(MP3_F_R2);
    D.rpc_seguidos_mal = 0;
    nw = plan_copiar(MP3_P_F4_R2, buf_mx);
    { unsigned i; for (i = 7; i < 12; i++) buf_mx[i] = 0; buf_mx[12] = 0x0020; }      /* la 1.ª reproducción tras el arranque: manejador 0 */
    rpc(buf_mx, nw);
#ifdef MP3_TONO
    { unsigned k; for (k = 0; k < 16; k++) mp3_r2_rep[k] = k < rx_mx_n ? rx_mx[k] : 0; }
#endif
    if (MAL()) return fase_fin(MP3_F_R2, fallo(MP3_E_MEZCLADOR, 2));
    fase_fin(MP3_F_R2, 0);
    fase_ini(MP3_F_CONFIG);
    r = cfg_y_umbral();
    fase_fin(MP3_F_CONFIG, 0);
    if (r) return r;
    if (!esperar(&m, 0x14, (u16)(0x0300u | M.flujo), 800) && m.n >= 2) D.cap = m.d[1];   /* 0x03ff {capacidad}: el anillo del DSP (el DSP real no la manda hasta aquí: si no llega se supone la del DSP falso, 0x800) */
    else D.cap = 0x0800;
    M.espacio = D.cap; D.espacio = M.espacio; M.tiene_cap = 1;
    fase_ini(MP3_F_DATOS);
    M.pos_arch = desde_byte;
    if (!F.activo) { colchon_fichero(desde_byte); relleno(4); }                      /* (2026-10-26) el fichero al colchón: 32 KB antes de empezar */
    r = alimentar(99);                                                               /* prellenar hasta que no quepa otro trozo */
    if (r < 0) return fase_fin(MP3_F_DATOS, r);
    if (r == 0 && !D.eos_enviado) return fase_fin(MP3_F_DATOS, fallo(MP3_E_ANILLO, 1));
    fase_fin(MP3_F_DATOS, 0);
    fase_ini(MP3_F_PLAY);
    r = accion(0, 2000);
    fase_fin(MP3_F_PLAY, 0);
    return r;
}

/* ---- parar el decodificador: STOP, vaciar x2, parar el escritor, LIBERAR, matar el proceso (§8.7 del protocolo) ----------------------------------- */
static void parar_decodificador(void)
{
    DspMsg m;
    u16 w[4];
    int i;
    if (!M.proc) return;
    fase_ini(MP3_F_FIN);
    if (dsp_mio()) {
        accion(1, 500);
        M.vaciando = 1;
        for (i = 0; i < 2; i++) {
            w[0] = (u16)(0x0700u | M.flujo);
            if (enviar(0x14, w, 1)) break;
            esperar(&m, 0x14, (u16)(0x0200u | M.flujo), 300);                        /* el DSP devuelve lo no consumido: se ignora, se reinicia la cuenta */
        }
        M.vaciando = 0;
        w[0] = (u16)(0x0100u | M.flujo); enviar(0x14, w, 1);                         /* parar el escritor: sin respuesta */
        accion(3, 500);
        w[0] = 0x120C; w[1] = M.idx;
        if (!enviar(0x26, w, 2)) esperar(&m, 0x26, 0x120D, 1000);
        while (dsp_recibir(&m, 0xFFFF) == 0) ;                                       /* nada viejo en la cola */
    }
    M.proc = 0; M.espacio = 0; M.tiene_cap = 0; D.espacio = 0;
    fase_fin(MP3_F_FIN, 0);
}
/* el mezclador a cero (R_14, R_91, R_16, R_7, R_15, R_17): lo que el original hace 0,6 s después de parar */
static void mx_reinicio(void)
{
    static const u8 orden[6] = { MP3_P_PRE_R14, MP3_P_PRE_R91, MP3_P_PRE_R16, MP3_P_PRE_R17A, MP3_P_PRE_R15, MP3_P_PRE_R17B };
    int i;
    if (!dsp_mio() || M.mx_limpio) return;
    D.rpc_seguidos_mal = 0;
    if (h_fil) mx(MP3_P_PRE_R7A);
    for (i = 0; i < 6 && !MAL(); i++) mx(orden[i]);
    M.mx_limpio = 1;
}

/* ---- el cierre de la línea y el apagado ---------------------------------------------------------------------------------------------------- */
static void bajar_linea(void)                           /* la parada: silenciar, amplificador y elevador abajo (P1.5 = 0 y luego P3.7 = 0) */
{
    if (D.linea == 2) {
        fase_ini(MP3_F_CIERRE);
        mp3_cod_bajar_rx();
        if (es_altavoz()) { gp(0, 1); gp(0, 0); } else gp(0, 0);
        D.linea = 1;
        fase_fin(MP3_F_CIERRE, 0);
    }
}
static void cerrar_todo(void)                           /* el «noveno segundo» del original: códec apagado, reloj del PCM y DSP en reset */
{
    fase_ini(MP3_F_APAGADO);
    if (M.hw_cod) { if (D.linea == 2) bajar_linea(); mp3_cod_apagar(); M.hw_cod = 0; }
    D.linea = 0;
    if (dsp_estado()->gate5 || dsp_estado()->vivo) dsp_apagar();
    olvidar_dsp();
    M.hay_cierre = 0;
    fase_fin(MP3_F_APAGADO, 0);
}
static void abortar(void)                               /* ante cualquier fallo: todo ordenado hacia abajo y queda el motivo */
{
    /* (2026-10-25) la posición donde se quedó: lo que el DSP RECIBIÓ (tras un vaciado lo consumió todo), no el reloj, que siguió corriendo
     * los segundos del atasco; así el servicio puede volver a tocar desde ahí (shell/musica.c). Con VBR es aproximado. */
    if (D.estado == HW_MP3_SONANDO && M.cab.kbps && M.pos_arch > M.cab.inicio) M.pos_base_ms = (M.pos_arch - M.cab.inicio) * 8u / M.cab.kbps;
#ifdef MP3_NO_ABORT                                     /* SOLO desarrollo: tras un fallo el DSP queda tal cual para poder mirarlo */
    D.estado = HW_MP3_ERROR;
    return;
#endif
    if (M.proc) parar_decodificador();
    if (dsp_mio()) mx_reinicio();
    cerrar_todo();
    M.proc = 0; D.estado = HW_MP3_ERROR;
    pinta();
}

#ifdef MP3_TONO
volatile u16 mp3_tono_dbg[8];
volatile u16 mp3_tono_msg[8] = { 0x0600, 0x0003, 0x0000, 0x0048, 0x0010, 0x7FFF, 0x0000, 0x0009 };   /* a1.dsp.log 65.623 */
volatile u8 mp3_tono_niv[3] = { 12, 14, 15 };
volatile u16 mp3_tono_lista[96];                          /* si [0] != 0: nº de mensajes; por mensaje {canal, n, palabras}: se mandan en vez de mp3_tono_msg */
volatile u32 mp3_tono_val1;                               /* si != 0: dirección (par) de la palabra del DSP donde el gancho pi_val deja la última muestra del PCM */
volatile u16 mp3_tono_orac[4];                            /* oráculo: lecturas, no nulas, máx |valor|, última */
volatile u8 mp3_tono_cod[40];
volatile u16 mp3_tono_pre = 700;                           /* ms entre abrir la línea (amplificador encendido) y el primer tono */
volatile u16 mp3_tono_pcm[32 * 60];
/* PRUEBA DE SONIDO (2026-10-24, SOLO desarrollo, -DMP3_TONO): lo mínimo del original para oír algo, sin MP3 ni efectos (R_6/R_9): DSP -> mezclador de arranque -> códec/amplificador/reloj PCM ->
 * bloque B + conexiones (con los ids que devuelve el DSP) -> R_2 -> tono por el canal 0x09 (a1.dsp.log 65.6 s: 0x0600 {3, 0, 0x48, 0x10, 0x7FFF, 0, 9}). */
static int tono_linea(void)
{
    int r = 0;
    fase_ini(MP3_F_APF);
    if (apf_elegir()) { fase_fin(MP3_F_APF, 0); return fallo(MP3_E_APF, 0); }
    fase_fin(MP3_F_APF, 0);
    fase_ini(MP3_F_CODEC);
    if (D.linea == 0) { Mp3CodHw hw; hw.pmu = pmu_w; mp3_cod_iniciar(&hw); M.hw_cod = 1; }
    mp3_cod_aplicar_apf();
    mp3_cod_escalon(0, 0);
    D.linea = 1;
    fase_fin(MP3_F_CODEC, 0);
    fase_ini(MP3_F_LINEA);
    D.rpc_seguidos_mal = 0; M.mx_limpio = 0;
    mp3_cod_subir();
    if (es_altavoz()) { gp(0, 0); gp(0, 1); } else gp(0, 0);
    mp3_cod_escalon(1, 1); mp3_cod_escalon(1, 1);
    D.linea = 2;
    SIGUE(mx_rango(MP3_P_F1_R10, MP3_P_F1_R11));
    SIGUE(mx_r4(MP3_P_F1_R4_CERO, vol_gain(), 0));                                   /* el bloque B parte en 0 en el F1: aquí sube al volumen para oír el tono */
    mp3_cod_rmw(0xC9, 0xC0, 0x80); pcm_reloj(1); mp3_cod_escalon(1, 1);
    r = salida_0800(MP3_P_F1_0800);
    if (r) return fase_fin(MP3_F_LINEA, r);
    SIGUE(mx_rango(MP3_P_F1_R91, MP3_P_F1_R3));
    if (es_altavoz()) gp(1, 1);
    mp3_cod_escalon(1, 1); mp3_cod_escalon(1, 1);
    SIGUE(mx(MP3_P_F4_R2));
    return fase_fin(MP3_F_LINEA, 0);
mal:
    return fase_fin(MP3_F_LINEA, fallo(MP3_E_MEZCLADOR, D.rpc_mal_n));
}
/* (2026-10-24) el reloj del SO del DSP con el módulo de ARRANQUE (sin MP3): ticks dbl(0x736) al empezar y al acabar los pitidos, con la línea PCM abierta (DSP
 * despierto). mp3_tick_lec = {ms0, tick0, ms1, tick1} */
volatile u32 mp3_tick_lec[4];
volatile u32 mp3_tick_dir = 0x94Cu;                       /* dbl del contador de ticks del núcleo: 0x94C en la imagen de ARRANQUE, 0x736 en la del MP3 */
static u32 dsp_par(u32 dir)
{
    u32 t;
    D32(0x28) = dir & 0x7FFFFEu; D32(0x24) = 2;
    for (t = 0; t < 100000u; t++) if (!D32(0x24)) break;
    return t == 100000u ? 0xFFFFFFFFu : D32(0x2C);
}
int hw_mp3_tono(int ms)
{
    int r;
    u32 t0;
    ini();
    D.error = 0; D.sub = 0; D.cable = 0; M.t_play = ahora();
    if (M.hay_cierre || dsp_estado()->gate5 || D.linea) cerrar_todo();
    r = dsp_levantar();
    if (!r) r = mx_boot();
    if (!r) r = tono_linea();
    if (!r) {                                                                         /* tres pitidos de menos a más fuerte (niveles 7, 9 y 11 del volumen) para saber cuál se oye */
        unsigned ns = 0, q, v;
        if (mp3_tono_gainA) { r = mx_r4(MP3_P_F3_R4A, mp3_tono_gainA, 0); }                       /* ganancia en bruto del bloque A (salida) */
        t0 = ahora();
        while (ahora() - t0 < mp3_tono_pre) { rx_bombear(); espera(10); }
        for (q = 0; q < 17; q++) {                                                    /* el códec REAL (PMU C0..D0) frente a la sombra, y los GPIO del amplificador */
            u8 x = 0xEE;
            if (pmu_leer((u8)(0xC0 + q), &x)) x = 0xEF;
            mp3_tono_cod[q] = x; mp3_tono_cod[17 + q] = mp3_cod_sombra((u8)(0xC0 + q));
        }
        mp3_tick_lec[0] = ahora(); mp3_tick_lec[1] = dsp_par(mp3_tick_dir); mp3_tick_lec[2] = mp3_tick_lec[3] = 0;
        mp3_tono_cod[34] = (u8)gpio_get(1, 0x20); mp3_tono_cod[35] = (u8)gpio_get(3, 0x80); mp3_tono_cod[36] = D.apf_origen; mp3_tono_cod[37] = (u8)D.linea;             /* el elevador/amplificador necesita su tiempo antes del primer sonido (en el original pasan ~0,3 s entre P1.5 y la rampa) */
        for (v = 0; v < 3 && !r; v++) {
            u16 tono[8];
            for (q = 0; q < 8; q++) tono[q] = mp3_tono_msg[q];                        /* el mensaje del tono lo pone el anfitrión (word 0 = 0: sin tono) */
            D.volumen = mp3_tono_niv[v];
            r = mx_r4(MP3_P_F1_R4_CERO, mp3_tono_gain[v] ? mp3_tono_gain[v] : vol_gain(), 0);        /* mp3_tono_gain[v] != 0: ganancia en bruto del bloque B (0x4000 = 1,0) */
            if (!r && mp3_tono_lista[0]) {                                           /* lista de mensajes del anfitrión */
                unsigned i, k, p = 1, cnt = mp3_tono_lista[0];
                for (i = 0; i < cnt && !r; i++) {
                    u16 w[16]; unsigned canal = mp3_tono_lista[p], nw = mp3_tono_lista[p + 1];
                    for (k = 0; k < nw && k < 16; k++) w[k] = mp3_tono_lista[p + 2 + k];
                    p += 2 + nw;
                    if (canal == 0xFFFFu) { u32 t1 = ahora(), q1 = nw ? w[0] : 0; while (ahora() - t1 < q1) { rx_bombear(); espera(5); } }   /* canal 0xFFFF = esperar w[0] ms */
                    else { r = enviar((u16)canal, w, nw); espera(3); }
                }
            } else
            if (!r && tono[0]) r = enviar(0x09, tono, 7);
            t0 = ahora();
            while (!r && ahora() - t0 < (u32)ms) {
                rx_bombear();
                if (mp3_tono_val1) {                                                  /* oráculo: la última muestra que pcm_int escribió al PCM (gancho pi_val) */
                    u32 tt, vv;
                    D32(0x28) = mp3_tono_val1 & 0x7FFFFFu; D32(0x24) = 2;
                    for (tt = 0; tt < 100000u; tt++) if (!D32(0x24)) break;
                    vv = (u16)(D32(0x2C) >> 16);
                    mp3_tono_orac[0]++; if (vv) mp3_tono_orac[1]++;
                    { u16 ab = (u16)(vv > 32767u ? 65536u - vv : vv); if (ab > mp3_tono_orac[2]) mp3_tono_orac[2] = ab; }
                    mp3_tono_orac[3] = (u16)vv;
                }
                if (ns < 60u) {                                                       /* el puerto PCM durante el tono: FA02003E = 1 para leer; los 32 registros cada 5 ms */
                    R16(0xFA02003E) = 1;
                    for (q = 0; q < 32; q++) mp3_tono_pcm[32 * ns + q] = R16(0xFA020000u + 2 * q);
                    R16(0xFA02003E) = 0;
                    ns++;
                }
                espera(5);
            }
        }
    }
    if (!r && mp3_tick_lec[1] != 0xFFFFFFFFu) { mp3_tick_lec[2] = ahora(); mp3_tick_lec[3] = dsp_par(mp3_tick_dir); }
    mp3_tono_dbg[0] = (u16)r; mp3_tono_dbg[1] = mb_a; mp3_tono_dbg[2] = mb_b; mp3_tono_dbg[3] = mb_conn;
    mp3_tono_dbg[4] = (u16)D.error; mp3_tono_dbg[5] = (u16)D.sub; mp3_tono_dbg[6] = (u16)D.rpc_ok; mp3_tono_dbg[7] = (u16)D.rpc_mal;
    bajar_linea();
    cerrar_todo();
    D.estado = HW_MP3_PARADO;
    return r;
}
#endif
/* ---- el tiempo de reproducción: lo lleva el ARM (el DSP no manda la posición) ------------------------------------------------------------------- */
static u32 pos_actual(void)
{
    u32 p = M.pos_base_ms;
    if (D.estado == HW_MP3_SONANDO) p += ahora() - M.t_ref;
    if (D.dur_ms && p > D.dur_ms) p = D.dur_ms;
    return p;
}

/* ---- rampas de volumen (cada paso va precedido por los 5 R_9 que no dependen del nivel) -------------------------------------------------------- */
/* Entre paso y paso de la rampa el original SIGUE mandando datos al DSP según llegan los créditos (s8.dsp.log: «R_4 … datos*4 … R_4»): una rampa dura ~0,6 s y el anillo
 * guarda solo ~0,2 s de audio, así que sin esto se quedaría vacío a mitad del fundido. */
static int servir_flujo(void)
{
    int r;
    if (!M.proc || !M.tiene_cap) return 0;
    rx_bombear();
    if (D.eos_enviado) return 0;
    r = alimentar(4);
    return r < 0 ? r : 0;
}
static int rampa(const u16 *niv, int n)
{
    int i;
    D.rpc_seguidos_mal = 0;
    for (i = 0; i < n; i++) {
        mx_grupo();
        if (MAL()) return fallo(MP3_E_MEZCLADOR, D.rpc_mal_n);
        mx_ganancia(escala(niv[i], vol_gain()), 5);
        if (MAL()) return fallo(MP3_E_MEZCLADOR, D.rpc_mal_n);
        if (servir_flujo() < 0) return MP3_E_ENVIO;
        espera(15);
    }
    return 0;
}

/* ---- API ---------------------------------------------------------------------------------------------------------------------------------- */
static void ini(void)
{
    if (M.ini) return;
    M.ini = 1; D.volumen = 7;                           /* 7 = el de fábrica del original (ganancia 0x0200) */
    dsp_gancho_msg = gancho;
    dsp_filtro_rx = filtro_rx;                          /* (2026-10-26) los créditos, al recibirlos */
    dsp_vuelo_fin = vuelo_fin;
    tic_audio = alim_tic;                               /* (2026-10-26) el alimentador: no hace nada hasta que algo SUENE */
}
static void soltar_pista(void)                          /* cambio de pista: se suelta el decodificador, la línea SIGUE arriba (§8.5) */
{
    if (M.proc) { parar_decodificador(); mx_reinicio(); }
    D.estado = M.abierto ? HW_MP3_ABIERTO : HW_MP3_PARADO;
}
static int hw_mp3_abrir_i(const char *ruta)
{
    Mp3Cab c;
    u32 tam, k = 0;
    int r;
    ini();
    soltar_pista();
    F.activo = 0; F.esperando = 0;                                                    /* (2026-10-25) un fichero: el flujo que hubiera, fuera */
    D.error = 0; D.sub = 0; M.abierto = 0;
    if (!ruta || !ruta[0]) { fallo(MP3_E_PARAM, 0); return HW_ERROR; }
    tam = hw_fs_tam(ruta);
    if (!tam) { fallo(MP3_E_FICHERO, 0); return HW_ERROR; }
    while (ruta[k] && k < sizeof D.ruta_fichero - 1) { D.ruta_fichero[k] = ruta[k]; k++; }
    D.ruta_fichero[k] = 0;
    D.tam = tam;
    r = mp3_cab_leer(rd_cab, 0, tam, &c);
    if (r < 0) { fallo(r == -3 ? MP3_E_NO_MP3 : MP3_E_FICHERO, r); return HW_ERROR; }
    if (!mp3_cab_codigo_fs(c.fs)) { fallo(MP3_E_FORMATO, 0); return HW_ERROR; }
    M.cab = c; M.abierto = 1; M.pos_ini = c.inicio; M.pos_arch = c.inicio; M.pos_base_ms = 0;
    D.fs = c.fs; D.kbps = c.kbps; D.dur_ms = c.dur_ms; D.tam = c.tam; D.canales = c.canales; D.vbr = c.vbr; D.pos_ms = 0;
    D.estado = HW_MP3_ABIERTO;
    return HW_OK;
}

static int reanudar(void)
{
    int r = accion(0, 2000);
    A.fallo = 0; A.rx_bucle = 0;
    M.vol_pend = 0; M.t_ritmo = 0; M.t_lento = 0;                                     /* la rampa de subida llega al nivel apuntado; el DSP rellena su anillo: el ritmo, de nuevo */
    if (!r) r = rampa(rampa_sube, 11);
    if (r) { abortar(); return HW_ERROR; }
    M.t_ref = ahora(); D.estado = HW_MP3_SONANDO; D.fase = MP3_F_SONANDO;
    return HW_OK;
}
/* la secuencia completa de una primera reproducción, o la corta si el DSP sigue arriba con la línea puesta (cambio de pista) o recién parada (< 9 s) */
static int secuencia(void)
{
    int r;
    D.rpc_seguidos_mal = 0;
    if (dsp_mio() && D.dsp_modulo == 2 && M.hw_cod) {
        int bajada = D.linea != 2;
        M.hay_cierre = 0;
        /* (2026-10-25) Recién parada (< 9 s, la línea abajo): antes se repetía f1_linea ENTERA, pero su mezclador es el del módulo de ARRANQUE (un solo bloque,
         * el R_6 del filtro que el módulo MP3 rechaza con 0x0017) y mx_reinicio ya había deshecho los dos bloques: el R_13 salía con todo a cero y el DSP moría
         * (−7/3, medido: parar y volver a tocar a los 2 s). Ahora: solo el HARDWARE de la línea, el mezclador como en «pista siguiente» (f3, el del módulo
         * MP3: crea los bloques A y B) y el amplificador al final, antes del PLAY. */
        if (bajada) { r = linea_hw_subir(); if (r) return r; }
        mx_reinicio();                                                               /* «pista siguiente»: reinicio del mezclador y su configuración */
        r = f3_postboot();
        if (r) return r;
        if (bajada) linea_hw_amp();
        return f4_proceso(M.pos_ini);
    }
    if (M.hay_cierre || dsp_estado()->gate5 || D.linea) cerrar_todo();               /* restos de otra sesión: a cero antes de empezar */
    r = dsp_levantar();
    if (r) return r;
    r = mx_boot();                                                                   /* lo que el original hace al encender: el mezclador a 44,1 kHz */
    if (r) return r;
    r = f1_linea(1);
    if (r) return r;
    r = f2_modulo();
    if (r) return r;
    M.dsp_ok = 1; M.intentos = dsp_estado()->intentos;
    r = f3_postboot();
    if (r) return r;
    return f4_proceso(M.pos_ini);
}
static int hw_mp3_play_i(void)
{
    int r;
    ini();
    if (D.estado == HW_MP3_SONANDO) return HW_OK;
    if (D.estado == HW_MP3_PAUSA && M.proc) { F.esperando = 0; return reanudar(); }
    if (F.activo && !M.abierto) { D.error = 0; D.sub = 0; if (flujo_cabecera() != HW_OK) return HW_ERROR; }      /* (flujo) la cabecera, del colchón */
    if (F.activo && !R.fin && R.escrito - M.pos_arch < F.bajo) { D.error = 0; fallo(MP3_E_FALTAN, 0); return HW_ERROR; }   /* que haya al menos ~1 s */
    if (!M.abierto) { fallo(MP3_E_NO_ABIERTO, 0); return HW_ERROR; }
    D.error = 0; D.sub = 0; D.cable = (u8)cable_usb();
    if (D.cable) { fallo(MP3_E_CABLE, 0); D.estado = HW_MP3_ABIERTO; return HW_ERROR; }
    M.vol_pend = 0; M.vol_fallos = 0; M.t_ritmo = 0; M.t_lento = 0;                   /* F3 pone la ganancia del nivel apuntado; el ritmo se mide de nuevo */
    A.fallo = 0; A.sub = 0; A.rx_bucle = 0;
    M.t_play = ahora(); M.t_credito = M.t_play; mp3_err_t0 = M.t_play; mp3_err_n = 0; D.nodata = 0; D.cred_pal = 0; D.cred_msgs = 0; D.cred_pedidos = 0; D.trozos = 0; D.pal_enviadas = 0;
    r = secuencia();
    if (r) { abortar(); return HW_ERROR; }
    D.ms_total = ahora() - M.t_play; M.t_play = 0;
    M.t_ref = ahora(); M.t_cable = M.t_ref; D.estado = HW_MP3_SONANDO; D.fase = MP3_F_SONANDO;
    M.pos_base_ms = M.ini_ms; M.ini_ms = 0;
    pinta();
    return HW_OK;
}
static int hw_mp3_pausa_i(void)
{
    int r;
    ini();
    F.esperando = 0;                                                                  /* (flujo) una pausa pedida manda: no se sigue sola */
    if (D.estado == HW_MP3_PAUSA) return HW_OK;
    if (D.estado != HW_MP3_SONANDO || !M.proc) return HW_ERROR;
    fase_ini(MP3_F_PAUSA);
    r = rampa(rampa_baja, 10);
    if (!r) r = accion(2, 2000);
    fase_fin(MP3_F_PAUSA, 0);
    if (r) { abortar(); return HW_ERROR; }
    M.pos_base_ms = pos_actual(); D.estado = HW_MP3_PAUSA;
    return HW_OK;
}
static int hw_mp3_parar_i(void)
{
    ini();
    if (M.proc) parar_decodificador();
    if (dsp_mio()) { espera(100); mx_reinicio(); }
    bajar_linea();
    if (M.hw_cod || dsp_estado()->vivo) { M.hay_cierre = 1; M.t_cierre = ahora() + 9000u; }            /* ~9 s después se apaga el códec y el DSP */
    if (F.activo) { F.activo = 0; F.esperando = 0; M.abierto = 0; }                  /* (flujo) lo que pasó ya no está: no se puede volver a tocar */
    D.estado = M.abierto ? HW_MP3_ABIERTO : HW_MP3_PARADO;
    M.pos_base_ms = 0; M.ini_ms = 0; M.pos_ini = M.cab.inicio; D.pos_ms = 0;
    pinta();
    return HW_OK;
}
/* (2026-10-25, música sólida) EL VOLUMEN: hw_mp3_volumen solo APUNTA el nivel pedido; lo manda vol_aplicar (desde hw_mp3_paso, o ya si hace rato del
 * último): un R_4 {0, g, g, 0} con el ÚLTIMO nivel, como mucho uno cada MP3_VOL_CADA ms, así una ráfaga de teclas son unos pocos mensajes y no uno por
 * pulsación. Si el mezclador no contesta NO se aborta la canción (antes: fallo + abortar = un cambio de volumen la mataba): se reintenta, con una espera
 * que crece con los fallos seguidos (un DSP muerto de verdad lo cazan el atasco o dsp_mio). En PAUSA solo se apunta: la rampa de subida usa el nivel nuevo. */
#define MP3_VOL_CADA 100u
volatile u32 mp3_vol_enviados, mp3_vol_fallos, mp3_vol_pedidos;
static void vol_aplicar(void)
{
    u32 espera_vol = MP3_VOL_CADA << (M.vol_fallos > 4 ? 4 : M.vol_fallos);     /* 0,1 s; tras fallos 0,2 / 0,4 … 1,6 s */
    if (!M.vol_pend || D.estado != HW_MP3_SONANDO || !M.proc) return;
    if ((u32)(ahora() - M.t_vol) < espera_vol) return;
    M.t_vol = ahora();
    D.rpc_seguidos_mal = 0;
    if (mx_ganancia(vol_gain(), 0) == 0) { M.vol_pend = 0; M.vol_fallos = 0; mp3_vol_enviados++; }
    else { if (M.vol_fallos < 255) M.vol_fallos++; mp3_vol_fallos++; }
}
static int hw_mp3_volumen_i(int nivel)
{
    ini();
    if (nivel < 0) nivel = 0;
    if (nivel > 15) nivel = 15;
    mp3_vol_pedidos++;
    if (nivel == D.volumen && !M.vol_pend) return HW_OK;
    D.volumen = (u8)nivel;
    if (D.estado == HW_MP3_SONANDO && M.proc) { M.vol_pend = 1; vol_aplicar(); }
    return HW_OK;
}
static int hw_mp3_buscar_i(u32 ms)
{
    DspMsg m;
    u16 w[2];
    u32 pos;
    int r, pausado, i;
    ini();
    if (!M.abierto) { fallo(MP3_E_NO_ABIERTO, 0); return HW_ERROR; }
    if (F.activo) return HW_NO_DISPONIBLE;                                            /* (flujo) no se puede saltar: que lo pida quien manda los datos (HTTP Range) */
    if (D.dur_ms && ms >= D.dur_ms) ms = D.dur_ms > 1000 ? D.dur_ms - 1000 : 0;
    pos = mp3_cab_alinear(rd_cab, 0, &M.cab, mp3_cab_pos_bytes(&M.cab, ms));
    if (!M.proc) { M.pos_ini = pos; M.ini_ms = ms; M.pos_base_ms = ms; D.pos_ms = ms; return HW_OK; }
    fase_ini(MP3_F_BUSCA);
    pausado = D.estado == HW_MP3_PAUSA;
    if (!pausado) accion(2, 1000);
    accion(1, 1000);
    M.vaciando = 1;
    for (i = 0; i < 2; i++) {
        w[0] = (u16)(0x0700u | M.flujo);
        if (enviar(0x14, w, 1)) break;
        esperar(&m, 0x14, (u16)(0x0200u | M.flujo), 300);
    }
    M.vaciando = 0;
    w[0] = (u16)(0x0100u | M.flujo); enviar(0x14, w, 1);
    while (dsp_recibir(&m, 0xFFFF) == 0) ;
    A.fallo = 0; A.rx_bucle = 0;
    D.eos_enviado = 0; D.eos_recibido = 0; M.espacio = D.cap; D.espacio = M.espacio; M.pos_arch = pos; colchon_fichero(pos); relleno(4);
    r = cfg_y_umbral();
    if (!r) { r = alimentar(99); if (r > 0) r = 0; }
    if (!r && !pausado) { r = accion(0, 2000); M.vol_pend = 0; if (!r) r = rampa(rampa_sube, 11); }      /* s8: PLAY y la rampa de subida de 11 pasos (al nivel apuntado) */
    fase_fin(MP3_F_BUSCA, 0);
    if (r) { abortar(); return HW_ERROR; }
    M.pos_base_ms = ms; M.t_ref = ahora(); M.t_credito = M.t_ref; M.t_ritmo = 0; M.t_lento = 0;
    if (!pausado) D.fase = MP3_F_SONANDO;
    return HW_OK;
}
#ifdef MP3_TONO
volatile int mp3_tono_ref;                              /* siempre 0: solo existe para que el enlazador conserve hw_mp3_tono (se llama desde el puente) */
volatile u16 mp3_vig[8][6]; volatile u32 mp3_vig_n;
extern volatile u32 dsp_drenaje_rapido;
volatile u32 mp3_espera_modo;                            /* experimento (2026-10-24): 0 = espera(2) (sondea el temporizador F9010020 sin parar), 1 = WFI, 2 = bucle en registros */
void dspi_drenar_cortos(void);
/* FOTOS de la memoria del DSP (desarrollo, 2026-10-24): si mp3_foto_t != 0, a los mp3_foto_t ms de sonar se leen por la ventana las palabras
 * [mp3_foto_dir, +MP3_FOTO_N) dos veces, separadas mp3_foto_sep ms (sin dejar de alimentar entre medias). Comparadas con dos fotos tomadas en un
 * atasco dicen qué se congela dentro del DSP. mp3_foto_ms[k] = lo que tardó cada foto; mp3_foto_ok = fotos hechas (0..2, o 0xFF si la ventana falló). */
#define MP3_FOTO_N 0x14000u
volatile u32 mp3_foto_t, mp3_foto_dir, mp3_foto_sep = 20, mp3_foto_ok, mp3_foto_ms[4], mp3_foto_trozos[4], mp3_foto_cuando[4];
volatile u32 mp3_foto_tcred, mp3_foto_t0;
volatile u32 mp3_foto_atasco = 800;                      /* fotos 2 y 3: en cuanto pasan tantos ms sin créditos del DSP (sonando), ANTES de que salte el atasco y se apague nada */
u16 mp3_foto[4][MP3_FOTO_N] __attribute__((aligned(32)));
static int foto_tomar(int k)
{
    u32 i, t, v, t0 = ahora();
    for (i = 0; i < MP3_FOTO_N; i += 2) {
        D32(0x28) = (mp3_foto_dir + i) & 0x7FFFFFu; D32(0x24) = 2;
        for (t = 0; t < 100000u; t++) if (!D32(0x24)) break;
        if (t == 100000u) { mp3_foto_ok = 0xFF; return -1; }
        v = D32(0x2C); mp3_foto[k][i] = (u16)(v >> 16); mp3_foto[k][i + 1] = (u16)v;
    }
    mp3_foto_ms[k] = ahora() - t0; mp3_foto_trozos[k] = D.trozos; mp3_foto_cuando[k] = t0; mp3_foto_ok = (u32)k + 1;
    return 0;
}
/* experimento (2026-10-24): si mp3_5d7 != 0xFFFFFFFF, al empezar a sonar se escribe ese valor en la palabra 0x5D7 del DSP (reposo del proceso Idle: 0 = no duerme,
 * gira). Sirve para ver si el temporizador del DSP se para mientras duerme (su reloj va al ~70 %). mp3_5d7_antes = lo que había (0x10000 | valor). */
volatile u32 mp3_5d7 = 0xFFFFFFFFu, mp3_5d7_antes;
static void dsp_pal_poner(u32 dir, u16 v)               /* una palabra del DSP por la ventana, conservando la otra mitad del par (el DSP tiene que estar despierto) */
{
    u32 par = dir & ~1u, x, t;
    D32(0x28) = par; D32(0x24) = 2; for (t = 0; t < 100000u; t++) if (!D32(0x24)) break;
    if (t == 100000u) return;
    x = D32(0x2C); mp3_5d7_antes = 0x10000u | ((dir & 1u) ? (x & 0xFFFFu) : (x >> 16));
    x = (dir & 1u) ? ((x & 0xFFFF0000u) | v) : ((x & 0xFFFFu) | ((u32)v << 16));
    D32(0x28) = par; D32(0x2C) = x; D32(0x24) = 1; for (t = 0; t < 100000u; t++) if (!D32(0x24)) break;
}
/* (2026-10-25) PERFIL del DSP sonando, lo MISMO que se midió en el ORIGINAL por el puente (docs/reconocimiento_original.md §1:
 * Idle 79 %, DSPIF_INT 0 %, 0xAB52 = 0xAB53 = 0 siempre, tic al 100 %). Si mp3_perfil_max != 0, en cada vuelta de hw_mp3_prueba
 * (~2 ms) se leen por la ventana el PCB actual (par 0x72E: 0xABC6 Idle, 0xB67E Mixer, 0xAFAA pcmif_int, 0xAF76 pcmif_dl, 0xAF0E
 * DSPIF_INT, 0xC530 DynProc), el par 0xAB52 (mensaje al ARM en vuelo | estado del mensaje largo 0xAB53) y el tic 0x736, con la hora.
 * Lo lee herr/puente/dsp/errores_dsp.py --perfil N. Por omisión 0: no cambia nada. */
volatile u32 mp3_sin_sondeo_sonando;                      /* (2026-10-25) 1 = no sondear la campanilla (ventana del DSP) mientras suena, como el original */
extern volatile u32 dsp_camp_pausa;
#define MP3_PERFIL_N 512
volatile u32 mp3_perfil_max, mp3_perfil_n;
volatile u32 mp3_perfil[MP3_PERFIL_N][4];
int hw_mp3_prueba(int ms)                                /* desarrollo: play + el bucle principal sin el USB entre medias (el anillo del DSP guarda solo ~0,3 s) */
{
    int r = hw_mp3_play();
    u32 t0 = ahora(), tf = 0;
    if (!r && mp3_5d7 != 0xFFFFFFFFu) dsp_pal_poner(0x5D7u, (u16)mp3_5d7);
    mp3_foto_ok = 0; mp3_foto_t0 = t0; mp3_cred_n = 0; mp3_mxav_n = 0; mp3_otros_n = 0; mp3_perfil_n = 0;
    u16 v18 = R16(0xF9090018u), v12 = R16(0xF9090012u), v1c = R16(0xF909001Cu), v2c = R16(0xF909002Cu);
    mp3_vig_n = 0;
    while (!r && ahora() - t0 < (u32)ms) {
        u16 n18 = R16(0xF9090018u), n12 = R16(0xF9090012u), n1c = R16(0xF909001Cu), n2c = R16(0xF909002Cu);
        if ((n18 != v18 || n12 != v12 || n1c != v1c || n2c != v2c) && mp3_vig_n < 8) {              /* vigilante: cada cambio de los relojes/puertas del SoC durante la reproducción */
            u32 k = mp3_vig_n++;
            mp3_vig[k][0] = (u16)(ahora() - t0); mp3_vig[k][1] = n12; mp3_vig[k][2] = n18; mp3_vig[k][3] = n1c; mp3_vig[k][4] = n2c; mp3_vig[k][5] = (u16)D.trozos;
            v18 = n18; v12 = n12; v1c = n1c; v2c = n2c;
        }
        dsp_camp_pausa = mp3_sin_sondeo_sonando && D.estado == HW_MP3_SONANDO;          /* el original no toca la ventana del DSP mientras suena (0 campanillas medidas) */
        hw_mp3_paso();
        if (dsp_drenaje_rapido) { unsigned k; latir(); for (k = 0; k < 20; k++) { dspi_drenar_cortos(); hal_retardo(200); } }   /* ~2 ms vaciando el buzón del DSP cada ~100 µs en vez de dormir */
        else if (mp3_espera_modo == 1) { latir(); __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" :: "r"(0) : "memory"); }      /* WFI: el ARM duerme hasta la siguiente IRQ (tic de 4 ms) sin tocar el bus */
        else if (mp3_espera_modo == 2) { u32 k = 104000u; latir(); __asm__ volatile("1: subs %0, %0, #1\n bne 1b" : "+r"(k) :: "cc"); }   /* ~2 ms girando en registros */
        else espera(2);
        if (mp3_foto_t && mp3_foto_ok < 4 && D.estado == HW_MP3_SONANDO) {
            if (mp3_foto_ok == 0 && ahora() - t0 >= mp3_foto_t) { if (foto_tomar(0) == 0) tf = ahora(); }
            else if (mp3_foto_ok == 1 && ahora() - tf >= mp3_foto_sep) foto_tomar(1);
            else if (mp3_foto_ok == 2 && ahora() - M.t_credito >= mp3_foto_atasco) { mp3_foto_tcred = M.t_credito; if (foto_tomar(2) == 0) tf = ahora(); }
            else if (mp3_foto_ok == 3 && ahora() - tf >= mp3_foto_sep) foto_tomar(3);
        }
        if (mp3_perfil_n < mp3_perfil_max && mp3_perfil_n < MP3_PERFIL_N && D.estado == HW_MP3_SONANDO) {
            u32 k = mp3_perfil_n++;
            mp3_perfil[k][0] = ahora() - t0; mp3_perfil[k][1] = dsp_par(0x72Eu); mp3_perfil[k][2] = dsp_par(0xAB52u); mp3_perfil[k][3] = dsp_par(0x736u);
        }
    }
    dsp_camp_pausa = 0;
    return r;
}
u32 reloj_ciclo_13_208(void);                            /* hal/reloj.c (experimento): se llama desde el puente */
int hw_mp3_estado(void) { if (mp3_tono_ref) return hw_mp3_tono(mp3_tono_ref) + hw_mp3_prueba(mp3_tono_ref) + (int)reloj_ciclo_13_208(); return D.estado; }
#else
int hw_mp3_estado(void) { return D.estado; }
#endif
int hw_mp3_activo(void) { return M.ini && (M.proc || M.hay_cierre || D.linea); }                  /* el DSP o la línea de audio están encendidos */
u32 hw_mp3_tiempo(u32 *dur_ms)
{
    if (dur_ms) *dur_ms = D.dur_ms;
    D.pos_ms = pos_actual();
    return D.pos_ms;
}
int hw_mp3_motivo(void) { return D.error; }
int hw_mp3_info(HwMp3Info *i)
{
    if (!i) return HW_ERROR;
    i->estado = D.estado; i->canales = D.canales; i->vbr = D.vbr; i->volumen = D.volumen;
    i->fs = D.fs; i->kbps = D.kbps; i->dur_ms = D.dur_ms; i->pos_ms = pos_actual(); i->tam = D.tam; i->motivo = D.error;
    return HW_OK;
}

/* una vez por vuelta del bucle: créditos, datos, fin de pista, cable, cierre diferido. No bloquea más de unos trozos */
volatile u32 mp3_ritmo_fallos;                          /* (2026-10-26) veces que saltó el vigilante de ritmo */
volatile u32 mp3_ritmo_bajo = 97, mp3_ritmo_lentos;     /* (2026-10-26) su lado bajo: % de lo que toca en 12 s (0 = apagado); veces que saltó */
/* que el enlazador conserve la API del flujo mientras nadie del sistema la llama (el puente sí: herr/puente/dsp/exp_flujo.py) */
void *const volatile mp3_flujo_api[5] = { (void *)hw_mp3_flujo_abrir, (void *)hw_mp3_flujo_escribir, (void *)hw_mp3_flujo_libre, (void *)hw_mp3_flujo_fin, (void *)hw_mp3_flujo_info };
static void paso_i(void);
/* (2026-10-26) EL HUECO DEL BUCLE: el tiempo más largo entre dos hw_mp3_paso con la música SONANDO, y cuántos pasaron de 150 ms (solo RAM). Un bucle
 * parado ~0,3 s o más puede vaciar el DSP y dejar su salida estropeada; esto dice qué trabajos del sistema lo hacen en uso normal. */
volatile u32 mp3_hueco_max, mp3_hueco_max_cuando, mp3_huecos_150;
static u32 t_paso_ant;
void hw_mp3_paso(void)
{
    u32 t = ahora();
    if (D.estado == HW_MP3_SONANDO && t_paso_ant) {
        u32 h = t - t_paso_ant;
        if (h > mp3_hueco_max) { mp3_hueco_max = h; mp3_hueco_max_cuando = t; }
        if (h > 150u) mp3_huecos_150++;
    }
    t_paso_ant = D.estado == HW_MP3_SONANDO ? t : 0;
    M.api++; paso_i(); M.api--;
}
static void abortar_c(void) { tomar(); abortar(); soltar(); }
volatile u32 mp3_respirar = 1;                          /* (2026-10-26, puente) 0 = las esperas del panel ya no llaman al motor (hw_mp3_respirar), para medir si sobra */
static void paso_i(void)
{
    (void)mp3_flujo_api[0];
    if (!M.ini) return;
    if (M.hay_cierre && !M.proc) {                                                     /* el cierre diferido: a los ~9 s, o ya si llega el cable USB */
        int cable = 0;
        if ((u32)(ahora() - M.t_cable) >= 500u) { M.t_cable = ahora(); cable = cable_usb(); }
        if ((int)(ahora() - M.t_cierre) >= 0 || cable) { tomar(); cerrar_todo(); soltar(); }
    }
    if (!M.proc || (D.estado != HW_MP3_SONANDO && D.estado != HW_MP3_PAUSA)) return;
    if (!dsp_mio()) { fallo(MP3_E_DSP_PERDIDO, 0); abortar_c(); return; }
    if ((u32)(ahora() - M.t_cable) >= 500u) {                                         /* el cable USB y el DSP no se llevan: se corta */
        M.t_cable = ahora();
        if (cable_usb()) { D.cable = 1; fallo(MP3_E_CABLE, 0); abortar_c(); return; }
    }
    if (A.fallo) { fallo(A.fallo, A.sub); abortar_c(); return; }                      /* (2026-10-26) algo falló dentro del tic */
    relleno(1);                                                                       /* (2026-10-26) el fichero al colchón: una lectura de 8 KB si cabe; sin cerrojo */
    if (D.estado == HW_MP3_SONANDO && mp3_alim) {
        /* (2026-10-26) EL ALIMENTADOR DEL TIC manda los datos: aquí solo lo que espera al bucle (un mensaje de control, cada ~250 ms vaciar la cola
         * de lo que nadie espera, el volumen apuntado) */
        if (A.rx_bucle || (u32)(tics - A.t_limpieza) >= 63u) {
            A.rx_bucle = 0; A.t_limpieza = tics;
            tomar(); { u32 c0 = crono_c(); rx_bombear(); mp3_crono[0] += crono_c() - c0; mp3_crono[4]++; } soltar();
        }
        if (M.vol_pend) { tomar(); vol_aplicar(); soltar(); }
    } else {                                                                          /* en pausa, o sin alimentador (mp3_alim = 0): como antes */
        int r = 0;
        tomar();
        { u32 c0 = crono_c(); rx_bombear(); mp3_crono[0] += crono_c() - c0; mp3_crono[4]++; }
        vol_aplicar();                                                                /* (2026-10-25) el volumen apuntado, si toca */
        if (!D.eos_enviado) r = alimentar(4);
        soltar();
        if (r < 0) { abortar_c(); return; }
    }
    if (!D.eos_enviado) {
        /* (2026-10-25) 2,5 s (antes 6): sonando, los créditos llegan cada ~30 ms; si el DSP se vació (el bucle parado > ~0,6 s, medido) su decodificador
         * se para y NO vuelve a pedir: cuanto antes se diga, antes lo recupera el servicio (shell/musica.c: vuelve a tocar desde donde iba) */
        if (D.estado == HW_MP3_SONANDO && !D.eos_enviado && (u32)(ahora() - M.t_credito) >= 2500u) {   /* sonando y sin créditos: el DSP no consume */
            fallo(MP3_E_ATASCO, 0); abortar_c(); return;
        }
    }
    /* (2026-10-26) EL VIGILANTE DE RITMO. Sonando, el DSP pide datos al ritmo del MP3 (créditos ≈ 1,02 × kbps: lo de más son las cabeceras). Tras un
     * VACIADO (el bucle parado ~0,3 s o más) se midió que puede seguir pidiendo al DOBLE y sonar ACELERADO, sin NO_DATA ni error (micrófono de la Mac:
     * r 0,54 a 2× frente a 0,39 a 1×, en el segundo de canción que daban los bytes): el banco lo daba por bueno. Si en 4 s pide más de lo que
     * toca (+ el anillo del DSP entero, 2,7 KB, por si se rellena) —1,1× con tasa constante, 1,6× con tasa variable—, es un fallo: abortar y el
     * servicio vuelve a tocar desde donde iba. */
    if (D.estado == HW_MP3_SONANDO && M.cab.kbps && !D.eos_enviado) {
        u32 t = ahora();
        if (!M.t_ritmo) { M.t_ritmo = t ? t : 1; M.cred_ritmo = D.cred_pal; }
        else if ((u32)(t - M.t_ritmo) >= 4000u) {
            u32 bytes = (D.cred_pal - M.cred_ritmo) * 2u, toca = M.cab.kbps * 125u * ((t - M.t_ritmo) / 100u) / 10u;
            M.t_ritmo = t; M.cred_ritmo = D.cred_pal;
            /* (medido, 26-oct: tras un vaciado el DSP sigue a 4×, 2×, ~1,4×, ~1,2× o se para) tasa constante: > 1,1× (lo normal oscila ±3 % por
             * segundo alrededor de 1,02×); variable (kbps es su media): > 1,6× */
            if (bytes > toca + (M.cab.vbr ? toca * 3u / 5u : toca / 10u) + 2742u) { mp3_ritmo_fallos++; fallo(MP3_E_RITMO, (int)(bytes * 10u / (toca ? toca : 1))); abortar_c(); return; }
        }
    } else M.t_ritmo = 0;
    /* (2026-10-26) EL LADO BAJO del vigilante: tras un vaciado el DSP también puede quedarse pidiendo al ~0,96× (medido 4 veces: el bucle parado 300 ms
     * con mus10, y guardar 64 KB con la NOR sin ceder; sin error, sin NO_DATA y sin que el lado alto lo vea). Normal: 1,015-1,035× en ventanas de
     * 4 s (141 medidas, banco «todo» ×3). Con tasa CONSTANTE, si en 12 s el DSP pide menos de mp3_ritmo_bajo % (97) de lo que toca y el colchón NO
     * se quedó sin datos (eso es la fuente, no el DSP), es un fallo: volver a tocar desde donde iba. 0 = apagado. */
    if (D.estado == HW_MP3_SONANDO && M.cab.kbps && !M.cab.vbr && !D.eos_enviado && mp3_ritmo_bajo) {
        u32 t = ahora();
        if (!M.t_lento) { M.t_lento = t ? t : 1; M.cred_lento = D.cred_pal; M.secos_lento = mp3_alim_secos; }
        else if ((u32)(t - M.t_lento) >= 12000u) {
            u32 bytes = (D.cred_pal - M.cred_lento) * 2u, toca = M.cab.kbps * 125u * ((t - M.t_lento) / 100u) / 10u, secos = mp3_alim_secos - M.secos_lento;
            M.t_lento = t; M.cred_lento = D.cred_pal; M.secos_lento = mp3_alim_secos;
            if (!secos && (unsigned long long)bytes * 100u < (unsigned long long)toca * mp3_ritmo_bajo) { mp3_ritmo_lentos++; fallo(MP3_E_RITMO, -(int)((unsigned long long)bytes * 100u / (toca ? toca : 1))); abortar_c(); return; }
        }
    } else M.t_lento = 0;
    if (F.activo) {                                                                   /* (flujo) el colchón se vacía: PAUSA LIMPIA; vuelve a tener: sigue solo */
        u32 hay = R.escrito - M.pos_arch;
        if (D.estado == HW_MP3_SONANDO && !R.fin && hay < F.bajo) {
            if (hw_mp3_pausa() == HW_OK) { F.esperando = 1; F.esperas++; F.t_espera = ahora(); }
            return;
        }
        if (F.esperando && D.estado == HW_MP3_PAUSA && (hay >= F.alto || R.fin)) {
            F.ms_esperando += ahora() - F.t_espera; F.esperando = 0;
            tomar(); reanudar(); soltar();
            return;
        }
    }
    if (D.estado == HW_MP3_SONANDO && (D.eos_recibido ||
            (D.eos_enviado && D.dur_ms && M.pos_base_ms + (ahora() - M.t_ref) > D.dur_ms + 4000u))) {      /* el DSP avisó (0x0E04 {2}) o pasó la duración + 4 s */
        D.pos_ms = D.dur_ms;                                                          /* fin de pista: el DSP lo avisó (0x0E04 {2}) */
        tomar();
        parar_decodificador();                                                        /* se libera el decodificador; la línea sigue arriba para la pista que venga */
        D.estado = HW_MP3_FIN; D.fase = MP3_F_FIN;
        soltar();
        M.pos_base_ms = D.dur_ms;
        pinta();
    }
}

/* ---- diagnóstico ----------------------------------------------------------------------------------------------------------------------------- */
void mp3_diag(Mp3Diag *d)
{
    DspRegs rg;
    int k;
    D.pos_ms = pos_actual();
    D.cable = (u8)cable_usb(); D.p15 = (u8)gpio_get(1, 0x20); D.p37 = (u8)gpio_get(3, 0x80);
    D.espacio = M.espacio; D.canales = M.abierto ? M.cab.canales : 0;
    if (dsp_estado()->gate5) { dsp_regs(&rg); D.dsp_regs_s12 = (u16)rg.s12; D.dsp_regs_s18 = (u16)rg.s18; }
    if (M.hw_cod && D.linea) { for (k = 0; k < 16; k++) D.c1_5[k] = mp3_cod_sombra((u8)(0xC1 + k)); D.c1_5_ok = 1; }
    *d = D;
}

/* (2026-10-26) cada función pública con el CERROJO puesto de principio a fin: mientras el bucle hace una RPC, una rampa, una pausa, una búsqueda, una
 * parada o el arranque, el tic no toca el DSP (las rampas alimentan desde el bucle, servir_flujo) */
int hw_mp3_abrir(const char *ruta) { int r; M.api++; tomar(); r = hw_mp3_abrir_i(ruta); soltar(); M.api--; return r; }
int hw_mp3_play(void) { int r; M.api++; tomar(); r = hw_mp3_play_i(); soltar(); M.api--; return r; }
int hw_mp3_pausa(void) { int r; M.api++; tomar(); r = hw_mp3_pausa_i(); soltar(); M.api--; return r; }
int hw_mp3_parar(void) { int r; M.api++; tomar(); r = hw_mp3_parar_i(); soltar(); M.api--; return r; }
int hw_mp3_volumen(int nivel) { int r; M.api++; tomar(); r = hw_mp3_volumen_i(nivel); soltar(); M.api--; return r; }
int hw_mp3_buscar(u32 ms) { int r; M.api++; tomar(); r = hw_mp3_buscar_i(ms); soltar(); M.api--; return r; }
/* (1.10, 27-oct) para y APAGA YA el códec y el DSP (sin el cierre diferido de ~9 s): el micrófono los necesita (shell/musica.c, sh_musica_ceder_dsp) */
int hw_mp3_cerrar_ya(void)
{
    M.api++; tomar();
    if (M.proc) hw_mp3_parar_i();
    if (M.ini && (M.hay_cierre || M.hw_cod || D.linea || dsp_estado()->gate5 || dsp_estado()->vivo)) cerrar_todo();
    soltar(); M.api--;
    return HW_OK;
}
/* (2026-10-25) hw_respirar (plat/hw_w580.c): un paso del motor desde un trabajo largo del bucle, solo si el motor no está ya dentro de sí mismo */
void hw_mp3_respirar(void) { if (mp3_respirar && !M.api && M.ini) hw_mp3_paso(); }
