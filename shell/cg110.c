/* cg110.c — (1.10, 27-oct) las envolturas del grupo MICRÓFONO (api/cargable_110.h; el contrato para las apps está en cargables/sdk/w5_microfono.h).
 * Mismas reglas que cg19.c: OPCIONAL (sin CG_PIDE: sin micrófono, W5_NO_DISPONIBLE), lo que entra de la app se VALIDA (los búferes con cg_en_app y
 * cg_legible_n) y nada acaba en pánico. El micrófono lo lleva el SISTEMA (shell/micro_sis.c): la app es un dueño (SH_MIC_APP) y el dictado otro; al
 * descargarse la app, cg_descargar lo cierra (sh_mic_soltar_app). */
#include "cargador.h"
#include "micro_sis.h"

typedef char w5_mic_estados[(int)W5_MIC_CERRADO == SH_MIC_CERRADO && (int)W5_MIC_PREPARANDO == SH_MIC_PREPARANDO && (int)W5_MIC_GRABANDO == SH_MIC_GRABANDO &&
                            (int)W5_MIC_FALLO == SH_MIC_FALLO ? 1 : -1];

int t_mic_abrir(void)
{
    int r;
    if (!sh_mic_hay()) return W5_NO_DISPONIBLE;
    r = sh_mic_abrir(SH_MIC_APP);
    return r == HW_OK ? W5_OK : r == HW_NO_DISPONIBLE ? W5_NO_DISPONIBLE : W5_ERROR;
}
int t_mic_cerrar(void) { sh_mic_cerrar(SH_MIC_APP); return W5_OK; }
int t_mic_tomar(short *dst, unsigned max)
{
    int r;
    if (!sh_mic_hay()) return W5_NO_DISPONIBLE;
    if (!dst || !max || ((size_t)dst & 1u) || max > 0x100000u || !cg_en_app(dst, (size_t)max * 2u)) return W5_ERROR;
    r = sh_mic_tomar(SH_MIC_APP, dst, max);
    return r < 0 ? W5_ERROR : r;
}
int t_mic_info(W5MicInfo *i)
{
    ShMicInfo s;
    if (!i || !cg_en_app(i, sizeof *i)) return W5_ERROR;
    sh_mic_info(&s);
    memset(i, 0, sizeof *i);
    i->estado = s.dueno == SH_MIC_APP || s.dueno == SH_MIC_NADIE ? s.estado : SH_MIC_CERRADO;   /* el del dictado no es de la app */
    i->mentira = s.mentira; i->musica = s.dueno == SH_MIC_APP ? s.musica : 0; i->ocupado = s.dueno == SH_MIC_DICTADO;
    if (s.dueno == SH_MIC_APP) { i->error = s.error; i->muestras = s.tomadas; i->desbordes = s.desbordes; i->perdidas = s.perdidas; i->pico = s.pico; i->ms_preparar = s.ms_preparar; }
    return W5_OK;
}
int t_mic_wav_guardar(const short *pcm, unsigned n, char *nombre)
{
    char nom[32];
    int r;
    if (!pcm || !n || n > W5_MIC_WAV_MAX || ((size_t)pcm & 1u) || !cg_legible_n(pcm, (size_t)n * 2u)) return W5_ERROR;
    if (!nombre || !cg_en_app(nombre, 32)) return W5_ERROR;
    r = sh_mic_wav_guardar(pcm, n, nom);
    if (r == HW_OK) memcpy(nombre, nom, strlen(nom) + 1);
    return r == HW_OK ? W5_OK : r == HW_NO_DISPONIBLE ? W5_NO_DISPONIBLE : W5_ERROR;
}
unsigned cg_mic_caps(void) { return sh_mic_hay() ? (unsigned)W5_CAP_MICROFONO : 0u; }
