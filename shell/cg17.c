/* cg17.c — (1.7) las envolturas del grupo PANTALLA (api/cargable_17.h): medir el texto con la letra en uso y el tamaño de la
 * pantalla lógica. Una app de la 1.7 coloca sus cosas con esto en vez de con números fijos (docs/estetica.md §6). */
#include "cargador.h"

int t_ui_paso_linea(int marcas) { return dis_paso_linea(marcas); }
int t_ui_alto_texto(int escala, int marcas) { return dis_alto_texto(escala < 1 ? 1 : escala > 4 ? 4 : escala, marcas, (const char *)0); }
int t_pantalla_tam(void) { return (UI_ANCHO << 16) | UI_ALTO; }
int t_pantalla_giro(void) { return ui_giro(); }
