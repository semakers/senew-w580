/* cg12_d.c — (1.2, pista APX) grupo D: abrir una app de fuera CON algo (un fichero, una orden), y que el sistema abra sus
 * apps de fábrica por la RUTA del .sen (Archivos -> Imágenes/Música/Notas, Ajustes -> Luces/Fondo, el standby -> Reloj, la
 * tecla Walkman -> Música). El argumento lo pone el sistema; la app lo pide COPIADO a su memoria (ap_argumento). */
#include "cargador.h"

static char arg[128];                        /* con qué se abrió la app cargada ahora («» desde el menú) */
static u8 fijado;                           /* lo acaba de poner cg_abrir_con para la próxima carga */
static signed char luz_antes[W5_LUCES + 1];    /* cómo estaba cada luz antes de que la app la tocara (-1 = no la ha tocado) */
static u8 luz_tocada;
void cg12_d_soltar(void)
{
    int n;
    arg[0] = 0;
    if (luz_tocada) for (n = 1; n <= W5_LUCES; n++) if (luz_antes[n] >= 0) { hw_luz(n, luz_antes[n] ? 100 : 0); luz_antes[n] = -1; }
    luz_tocada = 0;
}
/* sin CG_PIDE a propósito: es OPCIONAL (la app pregunta W5_Q_CAPS; sin luces da W5_NO_DISPONIBLE) y no deja nada: al descargar
 * vuelven como estaban. Si la exigiera, una app que solo QUIERE la luz no se cargaría en un modelo sin ellas (el W600) */
int t_ap_luz(int n, int encendida)
{
    int e;
    if (!(HW_CAPS & W5_CAP_LUCES) || n < 1 || n > W5_LUCES) return W5_NO_DISPONIBLE;
    if (!luz_tocada) { memset(luz_antes, -1, sizeof luz_antes); luz_tocada = 1; }
    if (luz_antes[n] < 0) { e = hw_luz_estado(n); if (e < 0) return W5_NO_DISPONIBLE; luz_antes[n] = (signed char)(e > 0); }
    return hw_luz(n, encendida ? 100 : 0) == HW_OK ? W5_OK : W5_ERROR;
}
int t_ap_luz_estado(int n)
{
    int e;
    if (!(HW_CAPS & W5_CAP_LUCES) || n < 1 || n > W5_LUCES) return W5_NO_DISPONIBLE;
    e = hw_luz_estado(n);
    return e < 0 ? W5_NO_DISPONIBLE : e > 0;
}
void cg12_d_al_abrir(void) { if (!fijado) arg[0] = 0; fijado = 0; }   /* cg_abrir_ruta: desde el menú, sin argumento */
const char *cg12_d_argumento(void) { return arg; }   /* (grupo C) ma_escribir: deja escribir el fichero con que se abrió la app */
int t_ap_argumento(char *buf, unsigned max)
{
    size_t n = strlen(arg);
    if (!max || !cg_en_app(buf, max)) return W5_ERROR;
    if (n > max - 1) n = max - 1;
    memcpy(buf, arg, n); buf[n] = 0;
    return (int)n;
}
/* Abre /apps/x.sen (o la ruta que sea) con ese argumento. Si ya hay OTRA app de fuera cargada (el préstamo es uno: hoy pasa
 * con la tecla Walkman desde dentro de una app), se le pide salir como con «Atrás» en su raíz —recibe W5_SALIR y guarda— y la
 * nueva se abre en cuanto la vieja se ha descargado (cg12_d_tras_descargar). Un juego (tiene el bucle) no llega aquí. */
static char pend_ruta[96], pend_arg[128];
static u8 pend;
static void copia(char *d, size_t nd, const char *s) { size_t n = s ? strlen(s) : 0; if (n >= nd) n = nd - 1; if (n) memcpy(d, s, n); d[n] = 0; }
void cg12_d_tras_descargar(void)
{
    if (!pend) return;
    pend = 0;
    sh_abrir_sen(pend_ruta, pend_arg);
}
Widget *cg_abrir_con(const char *ruta, const char *argumento)
{
    size_t n;
    if (cg_estado() == CG_VIVA && cg_perfil() == W5_PERFIL_WIDGETS) {
        copia(pend_ruta, sizeof pend_ruta, ruta); copia(pend_arg, sizeof pend_arg, argumento); pend = 1;
        cg_salir();
        return NULL;
    }
    if (cg_estado() != CG_LIBRE) {
        ui_aviso("Hay otra app abierta", "Ciérrala primero: solo cabe una app de fuera a la vez.");
        return NULL;
    }
    n = argumento ? strlen(argumento) : 0;
    if (n >= sizeof arg) n = sizeof arg - 1;
    if (n) memcpy(arg, argumento, n);
    arg[n] = 0;
    fijado = 1;
    return cg_abrir_ruta(ruta);
}
void sh_abrir_sen(const char *ruta, const char *argumento)
{
    Widget *w = cg_abrir_con(ruta, argumento);
    if (w) ui_empujar(w);
}

/* (1.2, pista JIT: 77) W5_CAP_JIT: la app ha escrito código en [p, p+n) de SU memoria y va a saltar a él. El ARM926EJ-S tiene la caché
 * de datos (16 KiB, líneas de 32 B, write-back) y la de instrucciones separadas: limpiar por línea (c7,c10,1), vaciar el búfer de
 * escritura (c7,c10,4) e invalidar la de instrucciones entera (c7,c5,0). Son órdenes de CP15 de modo privilegiado: con PM la app
 * está en modo usuario y no puede darlas ella. La MISMA semántica que la propuesta de la pista JIT [inferido del manual; el
 * emulador no modela cachés]. */
int t_ap_jit_sincronizar(const void *p, unsigned n)
{
    CG_PIDE(W5_CAP_JIT, W5_ERROR);
    if (!n) return W5_OK;
    if (!cg_en_app(p, n)) return W5_ERROR;
#if defined(TELEFONO) && TELEFONO
    {
        u32 a = (u32)(size_t)p & ~31u, fin = (u32)(size_t)p + n;
        for (; a < fin; a += 32u) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 4\n mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
    }
#endif
    return W5_OK;
}
