/* camara_sis.c — (1.9, 27-oct) LA CÁMARA para las apps (camara_sis.h). Sin esperas dentro de la app: abrir programa el sensor (~0,75 s, una
 * vez); luego el tic vigila el «cuadro completo» y para el flujo (hal/camara_w580.c, cam_tic), y cada cam_cuadro de la app que encuentra uno
 * listo lo convierte a su búfer (el flujo no se para: el tic lo reanuda en el otro búfer en el acto). Lo de la exposición: los cuadros del primer ~1,2 s salen negros y no se entregan.
 * La conversión YUV 4:2:2 → RGB565 (BT.601, la misma que cam_a_rgb565) va aquí con el GIRO y el byte ALTO primero (lo que pinta li_pixeles).
 * En el teléfono sin el driver (W600, W810) o con el sensor que no contesta: W5_NO_DISPONIBLE / W5_ERROR, nada se cuelga. */
#include "shell.h"
#include "camara_sis.h"
#include "cargable_19.h"
#include "tarjeta.h"

#if defined(TELEFONO) && TELEFONO
/* el driver solo existe en el W580: se enlaza DÉBIL (otro teléfono sin él no tiene cámara) */
int  cam_iniciar(void) __attribute__((weak));
void cam_apagar(void) __attribute__((weak));
void cam_tic(void) __attribute__((weak));
void cam_vigilar(int si) __attribute__((weak));
int  cam_listo(void) __attribute__((weak));
u32  cam_cuadros(void) __attribute__((weak));
int  cam_asentada(void) __attribute__((weak));
const u8 *cam_tomar(void) __attribute__((weak));
void cam_soltar(void) __attribute__((weak));
extern void (*volatile tic_camara)(void) __attribute__((weak));
static int hay_(void) { return cam_iniciar && cam_tic && &tic_camara; }
#else
/* LA CÁMARA DE MENTIRA del banco de PC: barras de colores que se deslizan y un cuadrado que rebota, un cuadro cada 143 ms (7 por segundo) */
static u8 falso[W5_CAM_BYTES];
static u32 f_t0, f_n, f_tomado;
static int hay_(void) { return 1; }
static void falso_pintar(u32 k)
{
    static const u8 barra[8][3] = { { 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 }, { 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 } };   /* Y U V */
    int x, y, cx = (int)(k * 7u % (W5_CAM_ANCHO - 24)), cy = (int)(k * 5u % (W5_CAM_ALTO - 24));
    for (y = 0; y < W5_CAM_ALTO; y++)
        for (x = 0; x < W5_CAM_ANCHO; x += 2) {
            const u8 *b = barra[((x + (int)k * 4) / 22) & 7];
            u8 *p = falso + (y * W5_CAM_ANCHO + x) * 2;
            int dentro = x >= cx && x < cx + 24 && y >= cy && y < cy + 24;
            p[0] = p[2] = dentro ? 128 : b[0]; p[1] = dentro ? 128 : b[1]; p[3] = dentro ? 128 : b[2];
        }
}
#endif

static struct { u8 abierta, fallo; int error; u32 base, entregados, ms_abrir, ms_ultimo; } C;

int sh_cam_hay(void) { return hay_(); }
int sh_cam_activa(void) { return C.abierta; }

int sh_cam_abrir(void)
{
    u32 t0;
    if (!hay_()) return HW_NO_DISPONIBLE;
    if (C.abierta) return HW_OK;
    t0 = hw_ms();
    C.fallo = 0; C.error = 0; C.entregados = 0; C.ms_ultimo = 0;
#if defined(TELEFONO) && TELEFONO
    C.error = cam_iniciar();
    if (C.error) { C.fallo = 1; C.ms_abrir = hw_ms() - t0; hw_log(ui_fmt("camara: no abre (%d)\n", C.error)); return HW_ERROR; }
    C.base = cam_cuadros ? cam_cuadros() : 0;
    cam_vigilar(1);
    tic_camara = cam_tic;
#else
    f_t0 = hw_ms(); f_n = 0; f_tomado = 0;
#endif
    C.abierta = 1;
    C.ms_abrir = hw_ms() - t0;
    hw_log(ui_fmt("camara: abierta en %u ms\n", C.ms_abrir));
    return HW_OK;
}

void sh_cam_cerrar(void)
{
    if (!C.abierta) return;
#if defined(TELEFONO) && TELEFONO
    tic_camara = 0;
    cam_apagar();                                       /* cam_vigilar(0) primero, luego CDI/CDICON y el sensor */
#endif
    C.abierta = 0;
    hw_log(ui_fmt("camara: cerrada (%u cuadros entregados)\n", C.entregados));
}

void sh_cam_soltar_app(void) { if (C.abierta) { hw_log("camara: la app se fue con ella abierta; la apago\n"); sh_cam_cerrar(); } }

static int asentada_(void)
{
#if defined(TELEFONO) && TELEFONO
    return cam_asentada();
#else
    return hw_ms() - f_t0 >= 1200u;
#endif
}

/* YUYV (Y1 U Y2 V) → RGB565 con el byte ALTO primero, girado g × 90° en sentido horario. dst: 176×144 (g par) o 144×176 (g impar) */
static void a_rgb565(const u8 *s, u8 *dst, int g)
{
    const int W = W5_CAM_ANCHO, H = W5_CAM_ALTO;
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x += 2) {
            const u8 *q = s + (y * W + x) * 2;
            int u = q[1] - 128, v = q[3] - 128, rr = (359 * v) >> 8, gg = (88 * u + 183 * v) >> 8, bb = (454 * u) >> 8, k;
            for (k = 0; k < 2; k++) {
                int yy = q[2 * k], r = yy + rr, gr = yy - gg, b = yy + bb, xx = x + k, ox, oy, ow, o;
                u16 c;
                r = r < 0 ? 0 : r > 255 ? 255 : r; gr = gr < 0 ? 0 : gr > 255 ? 255 : gr; b = b < 0 ? 0 : b > 255 ? 255 : b;
                c = (u16)(((r >> 3) << 11) | ((gr >> 2) << 5) | (b >> 3));
                switch (g & 3) {
                case 1:  ox = H - 1 - y; oy = xx;         ow = H; break;   /* 90°: la fila de arriba pasa a la columna de la derecha */
                case 2:  ox = W - 1 - xx; oy = H - 1 - y; ow = W; break;
                case 3:  ox = y;          oy = W - 1 - xx; ow = H; break;
                default: ox = xx;         oy = y;         ow = W; break;
                }
                o = (oy * ow + ox) * 2;
                dst[o] = (u8)(c >> 8); dst[o + 1] = (u8)c;
            }
        }
}

int sh_cam_cuadro(void *dst, int formato)
{
    const u8 *s;
    if (!C.abierta) return -1;
#if defined(TELEFONO) && TELEFONO
    if (!asentada_()) { if (cam_listo()) cam_soltar(); return 0; }   /* (revisión 9) los cuadros oscuros del arranque se tiran, no se guardan */
    if (!cam_listo()) return 0;
    s = cam_tomar();
    if (!s) return 0;
#else
    if (!asentada_()) return 0;
    { u32 n = (hw_ms() - f_t0) / 143u;                  /* la de mentira: un cuadro nuevo cada 143 ms */
      if (n == f_tomado) return 0;
      f_tomado = n; f_n++; falso_pintar(n); s = falso; }
#endif
    if ((formato & 0xF) == W5_CAM_YUYV) memcpy(dst, s, W5_CAM_BYTES);
    else a_rgb565(s, (u8 *)dst, (formato >> 4) & 3);
#if defined(TELEFONO) && TELEFONO
    cam_soltar();
#endif
    C.entregados++; C.ms_ultimo = hw_ms();
    return 1;
}

void sh_cam_info(ShCamInfo *i)
{
    memset(i, 0, sizeof *i);
#if !(defined(TELEFONO) && TELEFONO)
    i->mentira = 1;
#endif
    i->estado = C.fallo ? SH_CAM_FALLO : !C.abierta ? SH_CAM_CERRADA : asentada_() ? SH_CAM_EN_VIVO : SH_CAM_ASENTANDO;
    i->error = C.error;
#if defined(TELEFONO) && TELEFONO
    i->cuadros = C.abierta && cam_cuadros ? cam_cuadros() - C.base : 0;
#else
    i->cuadros = C.abierta ? (hw_ms() - f_t0) / 143u : 0;
#endif
    i->entregados = C.entregados; i->ms_abrir = C.ms_abrir; i->ms_ultimo = C.ms_ultimo;
}

/* LAS FOTOS EN LA TARJETA (1.9): la carpeta del original (W5_CAM_CARPETA) y su numeración DCF: «DSC01404.JPG», «MOV01405.3GP»… comparten el número,
 * así que la siguiente es el MÁXIMO de todos los «XXXnnnnn.YYY» de la carpeta + 1 [observado en la tarjeta del W580, 27-oct]. La app no elige la ruta. */
#define CARPETA_TJ "/DCIM/100MSDCF"                     /* lo mismo, sin «/tarjeta» (hw_tj_*) */
static int es_dcf(const char *n, int solo_foto)        /* «XXXnnnnn.YYY» (o, con solo_foto, exactamente «DSCnnnnn.JPG»): el número, o -1 */
{
    int i, v = 0;
    for (i = 0; i < 3; i++) if (!((n[i] >= 'A' && n[i] <= 'Z') || (n[i] >= 'a' && n[i] <= 'z'))) return -1;   /* (revisión 13) FAT no distingue */
    for (i = 3; i < 8; i++) { if (n[i] < '0' || n[i] > '9') return -1; v = v * 10 + (n[i] - '0'); }
    if (n[8] != '.' || !n[9] || !n[10] || !n[11] || n[12]) return -1;
    if (solo_foto && (memcmp(n, "DSC", 3) || memcmp(n + 8, ".JPG", 4))) return -1;   /* borrar: solo los nuestros, tal cual */
    return v;
}
int sh_cam_foto_guardar(const u8 *d, u32 n, char nombre[16])
{
    static HwEntrada e[16];
    char ruta[40];
    u32 desde = 0;
    int k, i, max = 0, r;
    if (!hw_tj_escribible()) return HW_NO_DISPONIBLE;
    if (n < 4 || n > W5_CAM_FOTO_MAX || d[0] != 0xFF || d[1] != 0xD8 || d[n - 2] != 0xFF || d[n - 1] != 0xD9) return HW_ERROR;   /* un JPEG entero */
    hw_tj_mkdir("/DCIM"); hw_tj_mkdir(CARPETA_TJ);       /* si ya están, no pasa nada */
    for (;;) {                                          /* el número más alto de la carpeta, de 16 en 16 */
        k = (int)hw_fs_listar("/tarjeta" CARPETA_TJ, desde, e, 16);
        for (i = 0; i < k; i++) { int v = es_dcf(e[i].nombre, 0); if (v > max) max = v; }
        if (k < 16) break;
        desde += 16;
    }
    if (max >= 99999) return HW_ERROR;
    max++;
    memcpy(nombre, "DSC00000.JPG", 13);
    for (i = 7; i >= 3; i--, max /= 10) nombre[i] = (char)('0' + max % 10);
    memcpy(ruta, CARPETA_TJ "/", sizeof CARPETA_TJ); memcpy(ruta + sizeof CARPETA_TJ, nombre, 13);
    r = hw_tj_guardar(ruta, d, n);
    if (r == HW_OK) {                                   /* RELEERLA (27-oct: 1 de 40 fotos quedó con el último sector parcial a CEROS en la tarjeta [observado,
                                                         * DSC01442, sin causa]): el principio y el final; si no coinciden, se escribe otra vez UNA vez */
        int k;
        for (k = 0; k < 2; k++) {
            static u8 rb[512];
            char abs_[48];
            u32 m = n < sizeof rb ? n : sizeof rb, ok;
            memcpy(abs_, "/tarjeta", 8); memcpy(abs_ + 8, ruta, sizeof ruta < sizeof abs_ - 8 ? sizeof ruta : sizeof abs_ - 8); abs_[sizeof abs_ - 1] = 0;
            ok = hw_fs_tam(abs_) == n && hw_fs_leer_desde(abs_, n - m, rb, m) == m && !memcmp(rb, d + n - m, m)
                 && hw_fs_leer_desde(abs_, 0, rb, 64) == 64 && !memcmp(rb, d, 64);
            if (ok) break;
            hw_log(ui_fmt("camara: foto %s RELEIDA DISTINTA (intento %d): se escribe otra vez\n", nombre, k + 1));
            if (k == 1 || (r = hw_tj_guardar(ruta, d, n)) != HW_OK) { r = HW_ERROR; break; }
        }
    }
    hw_log(ui_fmt("camara: foto %s (%u B): %d\n", nombre, n, r));
    return r;
}
int sh_cam_foto_borrar(const char *nombre)
{
    char ruta[40];
    if (es_dcf(nombre, 1) < 0) return HW_ERROR;
    if (!hw_tj_escribible()) return HW_NO_DISPONIBLE;
    memcpy(ruta, CARPETA_TJ "/", sizeof CARPETA_TJ); memcpy(ruta + sizeof CARPETA_TJ, nombre, 13);
    return hw_tj_borrar(ruta);
}
