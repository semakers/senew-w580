/* lcd_paneles_stub.h — senew PÚBLICO: la tabla de la pantalla SIN datos del fabricante.
 *
 * En el árbol de desarrollo, hal/lcd_paneles.h se GENERA con herr/hacer_paneles.py desde el fichero de configuración de pantalla
 * del propio teléfono (/ifs/settings/display/dispdriver0.dat, en su FS de fábrica): el microcódigo del PDI y los guiones de
 * inicio de los cuatro paneles posibles. Esos datos son del fabricante y NO se publican. Genera los tuyos desde TU teléfono:
 *
 *     make paneles DISPDRIVER=/ruta/a/dispdriver0.dat        (escribe hal/lcd_paneles.h, que git ignora)
 *
 * Con este stub senew compila y ARRANCA EN EL EMULADOR (que, con sus opciones por omisión, no ejecuta el microcódigo del PDI),
 * pero en un TELÉFONO DE VERDAD LA PANTALLA NO SE INICIALIZA: no grabes en un aparato un senew compilado con este stub.
 * Lo que queda aquí es solo la FORMA de la tabla (los mismos nombres de C) y los identificadores de variante (el valor del
 * registro 0x0028 del panel, como en el modelo público del emulador). Formato de un guion: [n, rs (4 bytes LE), n bytes] por
 * transferencia; [0xFE, ms] espera; [0xFF] fin (rs = el valor de PDI+11C: sus dos bits altos marcan los dos bytes del índice).
 * Un programa del PDI vacío es solo su instrucción de fin (0x08). */
#define LCD_PANELES_STUB 1
static const u8 pgm_write[1] = { 0x08 };
static const u8 pgm_gc[1] = { 0x08 };
static const u8 pgm_readhigh[1] = { 0x08 };
static const u8 pgm_read[1] = { 0x08 };
#define PGM_WRITE_EN    0x00
#define PGM_GC_EN       0x1C
#define PGM_READHIGH_EN 0x70
#define PGM_READ_EN     0x9C
#define PDI_VEL_GC    0x1
#define PDI_VEL_WRITE 0x1
#define PDI_VEL_READ  0x15
#define PANEL_REG_SONDA 0x00A0
#define PANEL_REG_ID    0x0028
#define PANEL_REG_MASC  0x000F
static const u8 g_swreset[1] = { 0xFF };
static const u8 g_init_vacio[1] = { 0xFF };
/* «encender»: UNA escritura, R07 (Display Control 1 del controlador) <- 0x0003 (D1:D0 = 11, la imagen se ve; hoja de datos del
 * controlador, no del fabricante del teléfono). Es lo único que necesita el emulador para dar la pantalla por encendida. */
static const u8 g_init2[10] = { 0x04, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x07, 0x00, 0x03, 0xFF };
static const u8 g_standby_from_normal[1] = { 0xFF };
static const u8 g_normal_from_standby[1] = { 0xFF };
static const u8 g_shutdown[1] = { 0xFF };
static const u8 g_pre_update[1] = { 0xFF };
typedef struct { const char *nombre; short id; const u8 *init; } lcd_panel;   /* id < 0: el de «si ninguno casa» */
static const lcd_panel lcd_paneles[4] = {
    { "panel A (sin tabla: stub)", 0, g_init_vacio },
    { "panel B (sin tabla: stub)", 10, g_init_vacio },
    { "panel C (sin tabla: stub)", 14, g_init_vacio },
    { "por omision (sin tabla: stub)", -1, g_init_vacio },
};
