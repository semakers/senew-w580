/* El PC como PLACA SIMULADA: no tiene descripción propia; toma la del modelo que diga MODELO (entorno) y simula ESA:
 * pantalla de ese tamaño, ese teclado, esas zonas (pc_nor con esa geometría), esas capacidades. (P600) El modelo lo fija la
 * COMPILACIÓN (make pc MODELO=w600i), no el entorno: el tamaño de pantalla ya va compilado y no pueden discrepar. */
#include <stdlib.h>
#include <string.h>
#include "placa.h"
const Placa *placa_w580i(void); const Placa *placa_w600i(void); const Placa *placa_w810i(void);
static const struct { const char *nombre; const Placa *(*dar)(void); } modelos[] = { { "w580i", placa_w580i }, { "w600i", placa_w600i }, { "w810i", placa_w810i } };
#ifndef MODELO_PC
#define MODELO_PC "w580i"                  /* el Makefile lo da: -DMODELO_PC='"$(MODELO)"' (el tamaño de pantalla ya va compilado) */
#endif
const Placa *placa(void)
{
    const char *m = MODELO_PC; unsigned i;
    for (i = 0; m && i < sizeof modelos / sizeof modelos[0]; i++) if (!strcmp(m, modelos[i].nombre)) return modelos[i].dar();
    return modelos[0].dar();
}
int placa_n_modelos(void) { return (int)(sizeof modelos / sizeof modelos[0]); }
const Placa *placa_modelo(int i) { return modelos[i].dar(); }
