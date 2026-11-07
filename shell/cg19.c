/* cg19.c — (1.9, 27-oct) las envolturas del grupo CÁMARA (api/cargable_19.h; el contrato para las apps está en cargables/sdk/w5_camara.h).
 * Mismas reglas que cg_red.c: OPCIONAL (sin CG_PIDE: sin cámara, W5_NO_DISPONIBLE), lo que entra de la app se VALIDA (el búfer del cuadro con
 * cg_en_app: tiene que ser memoria de la app, entera y escribible) y nada acaba en pánico. La cámara la lleva el SISTEMA (shell/camara_sis.c):
 * al descargarse la app, cg_descargar la apaga (sh_cam_soltar_app). */
#include "cargador.h"
#include "camara_sis.h"

typedef char w5_cam_estados[(int)W5_CAM_CERRADA == SH_CAM_CERRADA && (int)W5_CAM_ASENTANDO == SH_CAM_ASENTANDO && (int)W5_CAM_EN_VIVO == SH_CAM_EN_VIVO &&
                            (int)W5_CAM_FALLO == SH_CAM_FALLO ? 1 : -1];

int t_cam_abrir(void)
{
    int r;
    if (!sh_cam_hay()) return W5_NO_DISPONIBLE;
    r = sh_cam_abrir();
    return r == HW_OK ? W5_OK : r == HW_NO_DISPONIBLE ? W5_NO_DISPONIBLE : W5_ERROR;
}
int t_cam_cerrar(void) { sh_cam_cerrar(); return W5_OK; }
int t_cam_cuadro(void *dst, int formato)
{
    int r;
    if (!sh_cam_hay()) return W5_NO_DISPONIBLE;
    if ((formato & 0xF) > W5_CAM_YUYV || (formato & ~0x3F)) return W5_ERROR;
    if (!dst || ((size_t)dst & 1u) || !cg_en_app(dst, W5_CAM_BYTES)) return W5_ERROR;   /* el cuadro va ENTERO a memoria de la app */
    r = sh_cam_cuadro(dst, formato);
    return r < 0 ? W5_ERROR : r;
}
int t_cam_info(W5CamInfo *i)
{
    ShCamInfo s;
    if (!i || !cg_en_app(i, sizeof *i)) return W5_ERROR;
    sh_cam_info(&s);
    memset(i, 0, sizeof *i);
    i->estado = s.estado; i->mentira = s.mentira; i->ancho = W5_CAM_ANCHO; i->alto = W5_CAM_ALTO; i->error = s.error;
    i->cuadros = s.cuadros; i->entregados = s.entregados; i->ms_abrir = s.ms_abrir; i->ms_ultimo = s.ms_ultimo;
    return W5_OK;
}
unsigned cg_cam_caps(void) { return sh_cam_hay() ? (unsigned)W5_CAP_CAMARA : 0u; }
int t_cam_foto_guardar(const void *jpeg, unsigned n, char *nombre)
{
    char nom[16];
    int r;
    if (!jpeg || !n || n > W5_CAM_FOTO_MAX || !cg_legible_n(jpeg, n)) return W5_ERROR;
    if (!nombre || !cg_en_app(nombre, 16)) return W5_ERROR;
    r = sh_cam_foto_guardar((const u8 *)jpeg, n, nom);
    if (r == HW_OK) memcpy(nombre, nom, 13);
    return r == HW_OK ? W5_OK : r == HW_NO_DISPONIBLE ? W5_NO_DISPONIBLE : W5_ERROR;
}
int t_cam_foto_borrar(const char *nombre)
{
    char nom[16];
    int r;
    if (!nombre || !cg_copiar_cad(nom, sizeof nom, nombre)) return W5_ERROR;
    r = sh_cam_foto_borrar(nom);
    return r == HW_OK ? W5_OK : r == HW_NO_DISPONIBLE ? W5_NO_DISPONIBLE : W5_ERROR;
}
