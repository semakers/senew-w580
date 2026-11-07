/* El framework de UI encima de la API de hardware: lo que ui/ pide a «la plataforma» (plat_*) se
 * contesta aquí con hw_*. Es el único punto de contacto entre ui/ y el teléfono. */
#include "hw.h"
#include "ui.h"

void plat_init(void) { hw_iniciar(); }
u32  plat_ms(void) { return hw_ms(); }
int  plat_tecla(void) { return hw_tecla_evento(); }
int  plat_abajo(int k) { return hw_tecla_abajo(k); }
void plat_volcar(const Color *c, int y0, int filas) { hw_pantalla_volcar(c, y0, filas); }
int  plat_girar(int g) { return hw_pantalla_girar(g) == HW_OK ? 0 : -1; }
void plat_reposo(void) { hw_reposo(); }
void plat_log(const char *s) { hw_log(s); }
void plat_panico(const char *que) { hw_panico(que, "El sistema se ha quedado sin memoria. Esto es un fallo nuestro, no tuyo."); }
u32  plat_cuenta(void) { return hw_cuenta(); }
u32  plat_fichero(const char *ruta, u8 *buf, u32 max) { return buf ? hw_fs_leer(ruta, buf, max) : hw_fs_tam(ruta); }
