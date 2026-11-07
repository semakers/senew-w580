/* giro.c — (estética, 26-oct) la pantalla GIRA SOLA con el acelerómetro (docs/estetica.md §8). Pedido del mantenedor: las cuatro
 * orientaciones, el giro automático encendido de fábrica (aj.giro) y las softkeys junto a las teclas físicas (ui/diseno.h).
 *
 * LO MEDIDO EN EL APARATO (26-oct, build/est1, herr/estetica/acel_postura.c; cero ~125, 1 g ~13-14 cuentas):
 *     de pie            x 126  y 139   -> giro 0
 *     teclas a la der.  x 112  y 125   -> giro 1 (UI_GIRO_DER: el arriba del cuadro en el borde derecho del panel, PDICON código 5)
 *     teclas a la izq.  x 137  y 124   -> giro 3 (UI_GIRO_IZQ, código 6)
 *     boca abajo        x 124  y 111   -> giro 2 (UI_GIRO_180)
 * La regla: cada orientación tiene su «fuerza» (la gravedad a lo largo de su eje: gy, -gx, -gy, gx); manda la más fuerte si pasa de
 * GIRO_MIN y le gana a la de ahora por GIRO_HOLGURA (histéresis: inclinado a 45° no baila) y si se mantiene GIRO_QUIETO ms. Tumbado en
 * la mesa ninguna llega: se queda como estaba.
 *
 * CUÁNDO MIRA EL SENSOR: solo con el giro automático puesto, la pantalla encendida, ninguna app a pantalla completa y, si hay una app
 * cargada, que diga que sabe girar (W5_CAP_GIRA). Si no, el sensor se suelta (su servicio lo apaga entre muestras: shell/pasos.c) y la
 * pantalla vuelve DE PIE (salvo con la pantalla apagada: entonces se queda como estaba, para no girar a oscuras). */
#include "shell.h"
#include "app_externa.h"
#include "cargador.h"

#define GIRO_MIN 8          /* cuentas: ~35° desde la mesa */
#define GIRO_HOLGURA 4
#define GIRO_QUIETO 500     /* ms */

static u8 candidato = 0xFF;
static u32 cand_ms, vistas;

static int fuerza(int g, int gx, int gy) { return g == UI_GIRO_0 ? gy : g == UI_GIRO_DER ? -gx : g == UI_GIRO_180 ? -gy : gx; }
int sh_giro_de(int x, int y, int ahora)        /* la orientación que pide la muestra (x, y) estando en «ahora», o -1 = quedarse */
{
    const int gx = x - 125, gy = y - 125;
    int g, mejor = -1, fm = -1000;
    for (g = 0; g < 4; g++) { int f = fuerza(g, gx, gy); if (f > fm) { fm = f; mejor = g; } }
    if (mejor == ahora || fm < GIRO_MIN || fm < fuerza(ahora, gx, gy) + GIRO_HOLGURA) return -1;
    return mejor;
}
static int puede_girar(void)                    /* 0 = de pie por fuerza */
{
    return aj.giro && !app_pantalla_tomada() && cg_app_sabe_girar() && !sh_escritorio_visible();   /* (6-nov) el escritorio va de pie: al
                                                                                                  * inclinarlo para mover el cursor, giraría */
}
void sh_giro_paso(void)
{
    PasosInfo p;
    int q = puede_girar() && !sh_pantalla_apagada(), g;
    sh_pasos_retener_giro(q);
    if (!q) {
        candidato = 0xFF;
        if (!puede_girar() && ui_giro() != UI_GIRO_0 && ui_poner_giro(UI_GIRO_0)) cg_avisar_giro(UI_GIRO_0);
        return;
    }
    sh_pasos_info(&p);
    if (!p.hay_sensor || p.muestras == vistas) return;
    vistas = p.muestras;
    g = sh_giro_de(p.x, p.y, ui_giro());
    if (g < 0) { candidato = 0xFF; return; }
    if (g != candidato) { candidato = (u8)g; cand_ms = hw_ms(); return; }
    if (hw_ms() - cand_ms >= GIRO_QUIETO) { candidato = 0xFF; if (ui_poner_giro(g)) { cg_avisar_giro(g); sh_luces_latido(); } }
}
