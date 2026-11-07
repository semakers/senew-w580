/* cg15.c — (1.5) las envolturas del grupo de SONIDO Y VIBRADOR de la tabla (api/cargable_15.h; el contrato para las apps está
 * en cargables/sdk/w5_sonido.h). Mismas reglas que cg12_*.c, cg13.c y cg_bt.c: lo que entra de la app se VALIDA (la ruta, byte
 * a byte con cg_copiar_cad, a un búfer del sistema con tope) y nada acaba en pánico. Los servicios no cambian: aquí solo se
 * traduce. Las dos claves de volumen (W5_AJ_VOLUMEN, W5_AJ_VOLUMEN_MUSICA) van con los ajustes, en cg12_a.c.
 * Lo que la app puede dejar a medias: una vibración (la para sh_app_externa_recoger al descargarla, como la de ap_vibrar). La
 * MÚSICA que puso NO se para al descargarla: es lo que hace el servicio (decisión del mantenedor, 2026-10-25). */
#include "cargador.h"

typedef char w5_son_ruta[W5_MU_RUTA == MU_RUTA ? 1 : -1];

int t_mu_tocar_ruta(const char *ruta)
{
    char r[W5_MU_RUTA];
    CG_PIDE(W5_CAP_MUSICA, W5_ERROR);
    if (!cg_copiar_cad(r, sizeof r, ruta) || !r[0]) return W5_ERROR;      /* 0 = no es de la app o no cabe (> 111 bytes) */
    return sh_musica_tocar_ruta(r) == HW_OK ? W5_OK : W5_NO_DISPONIBLE;
}

int t_ap_vibrar_patron(int hz, int pct, int ms)
{
    if (!(HW_CAPS & W5_CAP_VIBRADOR)) return W5_NO_DISPONIBLE;
    if (ms <= 0) return hw_vibrar(0) == HW_OK ? W5_OK : W5_NO_DISPONIBLE;
    if (hz < 1 || hz > 8 || pct < 1 || pct > 100) return W5_ERROR;
    return hw_vibrar_patron(hz, pct, ms > W5_VIBRAR_MAX_MS ? W5_VIBRAR_MAX_MS : ms) == HW_OK ? W5_OK : W5_NO_DISPONIBLE;
}
