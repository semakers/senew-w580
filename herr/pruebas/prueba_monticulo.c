/* (R2; auditoría del sistema, M2) ui/ui_mem.c de verdad: el montículo ya no se cuelga ni se sale con una cabecera corrupta,
 * y soltar lo que no es un bloque vivo se dice. plat_panico se sustituye por un salto para poder seguir. make probar-monticulo. */
#include <stdio.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include "ui.h"
UiMedidas ui_med;
static jmp_buf salto; static const char *dicho;
void plat_panico(const char *que) { dicho = que; longjmp(salto, 1); }
u32 plat_ms(void) { return 0; }
static void alarma(int s) { (void)s; printf("  MAL   CUELGUE (el bucle del monticulo no avanza)\n"); _exit(1); }
static int mal;
#define PANICO(nombre, codigo, espera) do { dicho = NULL; if (!setjmp(salto)) { codigo; } \
    if (dicho && strstr(dicho, espera)) printf("  bien  %s -> «%s»\n", nombre, dicho); else { printf("  MAL   %s -> %s\n", nombre, dicho ? dicho : "ni panico"); mal = 1; } } while (0)
int main(void)
{
    size_t *a, *b, *c; void *p; const size_t sig = (40 + sizeof(size_t) - 1) / sizeof(size_t);   /* la cabecera del bloque siguiente */
    signal(SIGALRM, alarma); alarm(5);
    a = mo_pedir(40); b = mo_pedir(40); c = mo_pedir(40);
    mo_soltar(b); p = mo_probar(24);
    if (p == (void *)b) printf("  bien  uso normal: pedir, soltar y volver a pedir reutiliza el hueco\n"); else { printf("  MAL   uso normal\n"); mal = 1; }
    mo_soltar(p);
    PANICO("doble mo_soltar", mo_soltar(p), "NO ES UN BLOQUE VIVO");
    PANICO("mo_soltar de un puntero ajeno", { static size_t fuera[4]; mo_soltar(&fuera[2]); }, "NO ES UN BLOQUE VIVO");
    PANICO("mo_soltar de la mitad de un bloque", mo_soltar(a + 3), "NO ES UN BLOQUE VIVO");
    b = mo_pedir(40);
    b[sig] = 0;                                 /* un desborde de b por UNA palabra: la cabecera de c a 0 (antes: bucle infinito) */
    (void)c;
    PANICO("cabecera a 0 (desborde del vecino)", (void)mo_probar(100000), "CORRUPTO");
    b[sig] = 0x7FFFFFF0u;                       /* y con basura (antes: se salía del montículo) */
    PANICO("cabecera con basura", (void)mo_probar(100000), "CORRUPTO");
    printf(mal ? "MONTICULO: HAY FALLOS\n" : "MONTICULO: TODO BIEN\n");
    return mal;
}
