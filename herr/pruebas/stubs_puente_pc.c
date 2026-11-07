/* (v16) Lo que api/puente.c necesita fuera del teléfono, para compilar api/usb.c con -DPUENTE y correr contra él las MISMAS pruebas del
 * núcleo USB (prueba_usb): el puente no debe estropear nada del MTP. Las «direcciones» son desplazamientos de una arena. */
#include <stddef.h>
typedef unsigned int u32;
static unsigned char arena[1 << 16];
size_t pu_pc_base = (size_t)arena;
u32 pu_pc_llamar(u32 f, u32 a, u32 b, u32 c, u32 d) { (void)f; return a + b + c + d; }
void pu_pc_reiniciar(void) { }
u32  hw_ms(void) { static u32 t; return t += 4; }
void hw_perro_plazo(int s) { (void)s; }
void hw_perro_latido(void) { }
int  hw_reiniciar(void) { return 0; }
