/* micro_sis.c — (1.10, 27-oct) EL MICRÓFONO para las apps y el dictado (micro_sis.h). Sin esperas para quien lo usa: sh_mic_abrir solo EMPIEZA; el arranque
 * del DSP avanza por pasos en sh_mic_paso (el bucle del sistema y el latido de las apps) y, ya grabando, el tic de 4 ms recoge las muestras (hal/mic_w580.c,
 * mic_tic) en un anillo de ~2 s del que se TOMA sin esperar. La música por el DSP se para antes (sh_musica_ceder_dsp) y vuelve al cerrar. */
#include "shell.h"
#include "micro_sis.h"
#include "cargable_110.h"
#include "tarjeta.h"

#if defined(TELEFONO) && TELEFONO
int  mic_empezar(void) __attribute__((weak));
int  mic_paso(void) __attribute__((weak));
void mic_parar(void) __attribute__((weak));
void mic_tic(void) __attribute__((weak));
u32  mic_sacar(short *dst, u32 max) __attribute__((weak));
u32  mic_desbordes(void) __attribute__((weak));
u32  mic_perdidas(void) __attribute__((weak));
u32  mic_pico(void) __attribute__((weak));
extern void (*volatile tic_micro)(void) __attribute__((weak));
static int hay_(void) { return mic_empezar && mic_paso && mic_tic && &tic_micro; }
#else
/* EL MICRÓFONO DE MENTIRA del banco de PC: lo que «oye» es un tono de 440 Hz (a -12 dBFS, con algo de ruido) o, con SIS_MIC_WAV=fichero.wav (8 kHz mono
 * 16 bits), ese fichero en bucle; a 8000 muestras por segundo del reloj de hw_ms(). Tarda 300 ms en «prepararse», como un DSP muy rápido. */
#include <stdio.h>
#include <stdlib.h>
static short *f_wav; static u32 f_n, f_pos, f_t0, f_dadas, f_pico;
static int hay_(void) { return 1; }
static void falso_cargar(void)
{
    const char *r = getenv("SIS_MIC_WAV"); FILE *f; long t;
    if (f_wav || !r || !(f = fopen(r, "rb"))) return;
    fseek(f, 0, SEEK_END); t = ftell(f); fseek(f, 44, SEEK_SET);          /* la cabecera de un WAV PCM sencillo */
    if (t > 44 && (f_wav = malloc((size_t)(t - 44)))) f_n = (u32)fread(f_wav, 2, (size_t)(t - 44) / 2, f);
    fclose(f);
}
static short falso_muestra(u32 k)
{
    static const short seno[20] = { 0, 2532, 4816, 6629, 7792, 8193, 7792, 6629, 4816, 2532, 0, -2532, -4816, -6629, -7792, -8193, -7792, -6629, -4816, -2532 };   /* 400 Hz exactos a 8 kHz */
    if (f_wav && f_n) return f_wav[(f_pos + k) % f_n];
    return (short)(seno[k % 20] + (int)((k * 1103515245u + 12345u) >> 24) - 128);
}
#endif

static struct { u8 dueno, fallo, musica; int error; u32 tomadas, t0, ms_preparar; u8 listo; } M;

int sh_mic_hay(void) { return hay_(); }
int sh_mic_dueno(void) { return M.dueno; }
int sh_mic_activo(void) { return M.dueno != SH_MIC_NADIE && !M.fallo; }

int sh_mic_abrir(int dueno)
{
    if (!hay_()) return HW_NO_DISPONIBLE;
    if (M.dueno == dueno && !M.fallo) return HW_OK;
    if (M.dueno != SH_MIC_NADIE && !M.fallo) return HW_ERROR;            /* lo tiene otro */
    if (M.dueno != SH_MIC_NADIE) sh_mic_cerrar(M.dueno);                  /* uno que falló: limpiarlo */
    M.dueno = (u8)dueno; M.fallo = 0; M.error = 0; M.tomadas = 0; M.listo = 0; M.ms_preparar = 0; M.t0 = hw_ms();
    M.musica = (u8)sh_musica_ceder_dsp();                                 /* el DSP es del micrófono: la música se para (y vuelve al cerrar) */
#if defined(TELEFONO) && TELEFONO
    mic_empezar();
#else
    falso_cargar(); f_dadas = 0; f_pico = 0;
#endif
    hw_log(ui_fmt("micro: abierto por %s%s\n", dueno == SH_MIC_DICTADO ? "el dictado" : "la app", M.musica ? " (para la musica)" : ""));
    return HW_OK;
}

void sh_mic_cerrar(int dueno)
{
    if (M.dueno == SH_MIC_NADIE || M.dueno != dueno) return;
#if defined(TELEFONO) && TELEFONO
    tic_micro = 0;
    if (mic_parar) mic_parar();                                           /* apaga el códec y el DSP (también a medio arrancar) */
#endif
    hw_log(ui_fmt("micro: cerrado (%u muestras tomadas)\n", M.tomadas));
    M.dueno = SH_MIC_NADIE; M.listo = 0;
    if (M.musica) { M.musica = 0; sh_musica_recuperar_dsp(); }
}

void sh_mic_soltar_app(void) { if (M.dueno == SH_MIC_APP) { hw_log("micro: la app se fue con el microfono abierto; lo cierro\n"); sh_mic_cerrar(SH_MIC_APP); } }

void sh_mic_paso(void)
{
    int r;
    if (M.dueno == SH_MIC_NADIE || M.fallo || M.listo) return;
#if defined(TELEFONO) && TELEFONO
    r = mic_paso();
    if (r == 1) tic_micro = mic_tic;
#else
    r = hw_ms() - M.t0 >= 300u ? 1 : 0;
    if (r == 1) { f_t0 = hw_ms(); f_pos = 0; }
#endif
    if (r == 1) { M.listo = 1; M.ms_preparar = hw_ms() - M.t0; hw_log(ui_fmt("micro: grabando (listo en %u ms)\n", M.ms_preparar)); }
    else if (r < 0) {
        M.fallo = 1; M.error = r;
        hw_log(ui_fmt("micro: no arranca (%d)\n", r));
        if (M.musica) { M.musica = 0; sh_musica_recuperar_dsp(); }
    }
}

int sh_mic_tomar(int dueno, short *dst, u32 max)
{
    u32 n;
    if (M.dueno != dueno || M.fallo) return -1;
    if (!M.listo) return 0;
#if defined(TELEFONO) && TELEFONO
    n = mic_sacar(dst, max);
#else
    {   u32 hay = (hw_ms() - f_t0) * 8u - f_dadas, i;                   /* las que «han llegado» desde que está listo */
        n = hay < max ? hay : max;
        u32 p = 0;
        for (i = 0; i < n; i++) { short x = falso_muestra(f_dadas + i); u32 a = (u32)(x < 0 ? -x : x); dst[i] = x; if (a > p) p = a; }
        if (n) f_pico = p;                                                /* el pico de lo último tomado (como el de ~100 ms del de verdad) */
        f_dadas += n;
    }
#endif
    M.tomadas += n;
    return (int)n;
}

void sh_mic_info(ShMicInfo *i)
{
    memset(i, 0, sizeof *i);
#if !(defined(TELEFONO) && TELEFONO)
    i->mentira = 1; i->pico = f_pico;
#else
    if (M.listo && mic_pico) i->pico = mic_pico();
    if (mic_desbordes) i->desbordes = M.listo ? mic_desbordes() : 0;
    if (mic_perdidas) i->perdidas = M.listo ? mic_perdidas() : 0;
#endif
    i->dueno = M.dueno; i->musica = M.musica; i->error = M.error; i->tomadas = M.tomadas; i->ms_preparar = M.ms_preparar;
    i->estado = M.fallo ? SH_MIC_FALLO : M.dueno == SH_MIC_NADIE ? SH_MIC_CERRADO : M.listo ? SH_MIC_GRABANDO : SH_MIC_PREPARANDO;
}

/* GUARDAR UN WAV en /tarjeta/music, donde el original deja «Grabación.amr», «Grabación (1).amr»…: el siguiente número de todos los «Grabación (N).*» */
static void le32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
int sh_mic_wav_guardar(const short *pcm, u32 n, char nombre[32])
{
    static HwEntrada e[16];
    static const char base[] = "Grabaci\xC3\xB3n (";                     /* «Grabación (» en UTF-8 */
    static u8 cab[44];
    char ruta[64];
    u32 desde = 0, i, l;
    int k, max = 0, r;
    if (!hw_tj_escribible()) return HW_NO_DISPONIBLE;
    if (!n || n > W5_MIC_WAV_MAX) return HW_ERROR;
    hw_tj_mkdir("/music");
    for (;;) {
        k = (int)hw_fs_listar("/tarjeta/music", desde, e, 16);
        for (i = 0; i < (u32)k; i++)
            if (!memcmp(e[i].nombre, base, sizeof base - 1)) {
                const char *q = e[i].nombre + sizeof base - 1; int v = 0;
                while (*q >= '0' && *q <= '9' && v < 1000000) v = v * 10 + (*q++ - '0');
                if (*q == ')' && v > max && v <= 99999) max = v;          /* (revisión 6) números absurdos: no cuentan */
            }
        if (k < 16) break;
        desde += 16;
    }
    max++;
    memcpy(nombre, base, sizeof base - 1); l = sizeof base - 1;
    { char t[12]; int m = 0, v = max; do t[m++] = (char)('0' + v % 10); while (v /= 10); while (m) nombre[l++] = t[--m]; }
    memcpy(nombre + l, ").wav", 6);
    memcpy(cab, "RIFF", 4); le32(cab + 4, 36 + n * 2); memcpy(cab + 8, "WAVEfmt ", 8); le32(cab + 16, 16);
    cab[20] = 1; cab[21] = 0; cab[22] = 1; cab[23] = 0; le32(cab + 24, W5_MIC_HZ); le32(cab + 28, W5_MIC_HZ * 2); cab[32] = 2; cab[33] = 0; cab[34] = 16; cab[35] = 0;
    memcpy(cab + 36, "data", 4); le32(cab + 40, n * 2);
    memcpy(ruta, "/music/", 7); memcpy(ruta + 7, nombre, strlen(nombre) + 1);
    /* la cabecera y las muestras en DOS trozos: por la recepción a trozos de la tarjeta (el dueño de «serie»: si en ese momento entra algo por la serie,
     * esto da HW_ERROR y no se pisa nada); el fichero no existe hasta cerrar */
    r = hw_tj_rx_abrir_d(ruta, HW_RX_SER);
    if (r == HW_OK) {                                                     /* (revisión 7) abortar SOLO lo nuestro: si no abrió, la ranura es de otro */
        if ((r = hw_tj_rx_mas_d(HW_RX_SER, cab, sizeof cab)) == HW_OK && (r = hw_tj_rx_mas_d(HW_RX_SER, pcm, n * 2)) == HW_OK) r = hw_tj_rx_cerrar_d(HW_RX_SER);
        else hw_tj_rx_abortar_d(HW_RX_SER);
    }
    hw_log(ui_fmt("micro: %s (%u muestras): %d\n", nombre, n, r));
    return r;
}
