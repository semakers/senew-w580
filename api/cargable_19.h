/* cargable_19.h — (1.9, 27-oct) LA CÁMARA PARA APPS, vista desde el SISTEMA (docs/apps_cargables.md §5.11).
 *
 * El lado de las apps es cargables/sdk/w5_camara.h. Este fichero repite, para el sistema, lo MISMO y con los MISMOS números (las funciones en el
 * mismo orden y con la misma firma, las constantes, la estructura y W5_CAM_MENOR); «make probar-cam-abi» lo comprueba.
 *
 * QUÉ TRAE LA 1.9: el visor de la cámara (hal/camara_w580.c) para las apps, sin esperas dentro de ellas: el SISTEMA (shell/camara_sis.c) la
 * enciende, vigila el flujo desde el tic (el «cuadro completo» de CDI+04 solo está bajo 8-18 ms) y para el DMA en cuanto llega un cuadro entero;
 * la app lo toma con cam_cuadro (RGB565 con el byte alto primero, girado si quiere, o el YUYV del sensor) y el flujo sigue. Al descargar la app,
 * se apaga sola. Es OPCIONAL, como la red: sin CG_PIDE; sin cámara, W5_NO_DISPONIBLE (y W5_CAP_CAMARA no está en ap_consulta(W5_Q_CAPS)).
 * En el banco de PC hay una cámara de MENTIRA (barras de colores que se mueven) para hacer las apps sin el teléfono.
 *
 * Dónde va el grupo: DETRÁS de la música por la red (1.8). W5_API (cargable.h) NO cambia. El sistema dice menor 9 (CG_API_MENOR). Las envolturas, en
 * shell/cg19.c. */
#ifndef CARGABLE_19_H
#define CARGABLE_19_H
#include "cargable_18.h"

#define W5_CAM_MENOR 9                         /* la menor de la API que trae este grupo */
#define W5_CAM_ANCHO 176                       /* el cuadro sin girar */
#define W5_CAM_ALTO 144
#define W5_CAM_BYTES (W5_CAM_ANCHO * W5_CAM_ALTO * 2)   /* lo que ocupa un cuadro en cualquiera de los dos formatos (50 688 B) */
#define W5_CAM_CARPETA "/tarjeta/DCIM/100MSDCF"   /* donde guarda las fotos cam_foto_guardar (la del original: DSC01404.JPG, MOV01405.3GP…); se lista con fs_listar */
#define W5_CAM_FOTO_MAX (256 * 1024)            /* lo más que mide una foto que se guarda */
enum { W5_CAP_CAMARA = 1 << 26 };              /* hay cámara (el W580; en el banco de PC, una de mentira con barras de colores) */
/* cam_cuadro(dst, formato): el formato y, para RGB565, el giro (en sentido horario; 1 y 3 dan 144 de ancho × 176 de alto) */
enum { W5_CAM_RGB565 = 0, W5_CAM_YUYV = 1 };
#define W5_CAM_GIRO(g) (((g) & 3) << 4)
/* W5CamInfo.estado */
enum { W5_CAM_CERRADA = 0, W5_CAM_ASENTANDO, W5_CAM_EN_VIVO, W5_CAM_FALLO };
typedef struct {
    unsigned char estado, mentira, rsv[2];     /* W5_CAM_*; 1 = la cámara de mentira del banco de PC */
    short ancho, alto;                         /* del cuadro sin girar: 176 × 144 */
    int error;                                 /* el último fallo del sensor al abrir (< 0; 0 = ninguno) */
    unsigned cuadros, entregados;              /* cuadros enteros que han llegado desde cam_abrir, y los que la app tomó */
    unsigned ms_abrir, ms_ultimo;              /* lo que tardó cam_abrir; ap_ms() del último cuadro entregado */
} W5CamInfo;

/* LAS MISMAS LÍNEAS que W5_API19 de cargables/sdk/w5_camara.h */
#define W5_API19(F) \
    F(int, cam_abrir, (void))                            /* enciende y programa el sensor (~0,75 s). W5_OK (también si ya estaba) / W5_NO_DISPONIBLE / W5_ERROR (cam_info.error) */ \
    F(int, cam_cerrar, (void))                           /* la apaga. W5_OK */ \
    F(int, cam_cuadro, (void *dst, int formato))         /* 1 = un cuadro NUEVO copiado a dst (W5_CAM_BYTES); 0 = aún no hay; W5_ERROR / W5_NO_DISPONIBLE */ \
    F(int, cam_info, (W5CamInfo *i))                     /* la COPIA a i. W5_OK / W5_ERROR */ \
    F(int, cam_foto_guardar, (const void *jpeg, unsigned n, char *nombre))   /* un JPEG (FF D8 … FF D9) a W5_CAM_CARPETA como DSCnnnnn.JPG, con el número SIGUIENTE (como el original); su nombre a nombre[16]. W5_OK / W5_NO_DISPONIBLE (sin tarjeta) / W5_ERROR */ \
    F(int, cam_foto_borrar, (const char *nombre))        /* borra W5_CAM_CARPETA/nombre; SOLO un «DSCnnnnn.JPG». W5_OK / W5_ERROR */

#endif
