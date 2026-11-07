/* cg111.c — (1.11, 28-oct) las envolturas del grupo DESCARGAS (api/cargable_111.h; el contrato para las apps está en cargables/sdk/w5_descargas.h).
 * Pide W5_CAP_MUSICA, como mu_red_*: la canción sale de la cola de la música por la red (shell/musica_red.c) y la baja el SISTEMA
 * (shell/descarga_red.c), que sigue aunque la app se cierre. Lo que vuelve a la app se COPIA (cg_en_app). */
#include "cargador.h"
#include "musica_red.h"
#include "descarga_red.h"

typedef char w5_desc_fases[(int)W5_DESC_HECHA == DESC_HECHA && (int)W5_DESC_FALLO == DESC_FALLO && W5_DESC_NOMBRE == DESC_NOMBRE ? 1 : -1];

int t_mu_red_descargar(int i)
{
    const char *u, *t, *a;
    u32 d;
    int r;
    CG_PIDE(W5_CAP_MUSICA, W5_ERROR);
    if (!sh_desc_hay() || !sh_mured_hay()) return W5_NO_DISPONIBLE;
    if (sh_mured_pista(i, &u, &t, &a, &d)) return W5_ERROR;
    r = sh_desc_empezar(u, t, a, d);
    return r == HW_OK ? W5_OK : r == HW_NO_DISPONIBLE ? W5_NO_DISPONIBLE : W5_ERROR;
}
int t_mu_red_descarga(W5MuDescarga *d)
{
    DescInfo s;
    CG_PIDE(W5_CAP_MUSICA, W5_ERROR);
    if (!d || !cg_en_app(d, sizeof *d)) return W5_ERROR;
    sh_desc_info(&s);
    memset(d, 0, sizeof *d);
    d->fase = s.fase; d->ya_estaba = s.ya_estaba; d->http = s.http; d->bajados = s.bajados; d->total = s.total; d->esperado = s.esperado;
    memcpy(d->nombre, s.nombre, sizeof d->nombre); d->nombre[sizeof d->nombre - 1] = 0;
    memcpy(d->porque, s.porque, sizeof d->porque); d->porque[sizeof d->porque - 1] = 0;
    return W5_OK;
}
int t_mu_red_descarga_cancelar(void) { CG_PIDE(W5_CAP_MUSICA, W5_ERROR); sh_desc_cancelar(); return W5_OK; }
