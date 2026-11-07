/* bt.c — (pista BT) el Bluetooth visto desde el sistema: dueño, paso del bucle y reparto (shell/bt_sis.h, docs/sistema.md §30).
 * Portable: en W600, W810 y el PC no hay controlador (bt_driver() débil = NULL) y todo contesta «sin Bluetooth».
 * DORMIDO POR OMISIÓN: nada de aquí se llama al arrancar; el chip solo se toca tras sh_bt_encender(). */
#include "shell.h"
#include "bt_sis.h"
#include "descarga_red.h"                       /* (1.11) sh_desc_paso */
#include "red_sis.h"                              /* (1.6) sh_red_paso */

__attribute__((weak)) const BtDriver *bt_driver(void) { return 0; }
__attribute__((weak)) u8 bt_puente_red;          /* (0.29.3) la de verdad, en bt/servicio/bt_servicio.c: sin radio no la mira nadie */

static u8 dueno;                                  /* SH_BT_*: quién lo encendió */

int sh_bt_hay(void) { return bt_driver() != 0; }
int sh_bt_dueno(void) { return dueno; }
int sh_bt_activo(void) { const BtDriver *d = bt_driver(); return d && d->estado() != SH_BT_APAGADO; }

int sh_bt_encender(int quien)
{
    const BtDriver *d = bt_driver();
    if (!d || sh_bt_activo()) return -1;
    if (d->encender()) { hw_log("[bt] no se pudo encender\n"); return -1; }
    dueno = (u8)quien;
    hw_log(quien == SH_BT_APP ? "[bt] encendido (lo pide una app)\n" : quien == SH_BT_MUSICA ? "[bt] encendido (la musica por la red)\n"
           : "[bt] encendido (diagnostico)\n");
    return 0;
}
void sh_bt_apagar(void)
{
    const BtDriver *d = bt_driver();
    if (!d) return;
    if (d->estado() != SH_BT_APAGADO) { d->apagar(); hw_log("[bt] apagado: el chip queda en reset\n"); }
    d->guardar_pendiente();                       /* (fuera de una app el almacén deja escribir la clave que quedó pendiente) */
    dueno = SH_BT_NADIE;
}
void sh_bt_paso(void)
{
    const BtDriver *d = bt_driver();
    if (d && dueno != SH_BT_NADIE) d->paso();     /* con la radio apagada no se hace NADA (ni se lee un registro) */
    sh_red_paso();                                /* (1.6) la red por el puente: mueve sus canales (y ve si el SPP se cayó) */
    sh_desc_paso();                               /* (1.11) una canción que se descarga a la tarjeta (shell/descarga_red.c) */
}
void sh_bt_soltar(int quien)
{
    const BtDriver *d = bt_driver();
    int h;
    if (!d || dueno != quien) return;
    h = sh_red_heredero(quien);                   /* (1.8) la red sigue para otro (la música con la app cerrada): la radio es suya ahora */
    if (h) { dueno = (u8)h; hw_log("[bt] la radio pasa al otro duenyo de la red\n"); return; }
    d->desconectar(); sh_bt_apagar();
}
void sh_bt_soltar_app(void)
{
    const BtDriver *d = bt_driver();
    if (!d) return;
    d->pin(0);                                    /* el PIN que pusiera la app no se queda para la siguiente */
    if (dueno == SH_BT_APP) sh_bt_soltar(SH_BT_APP);
    else d->guardar_pendiente();
}
void sh_bt_diag(ShBtDiag *x)
{
    const BtDriver *d = bt_driver();
    memset(x, 0, sizeof *x);
    if (!d) return;
    d->diag(x);
    x->hay = 1; x->dueno = dueno;
}
const char *sh_bt_estado_nombre(int e)
{
    static const char *const n[] = { "apagado", "encendiendo…", "listo", "buscando…", "conectando…", "conectado", "FALLO" };
    return e >= 0 && e <= SH_BT_FALLO ? n[e] : "?";
}
const char *sh_bt_fallo_nombre(int f)
{
    static const char *const n[] = { "ninguno", "P2.0 no subió (sin chip, reloj o pines)", "el chip no contestó a HCI_Reset",
        "el parche se cortó (sigue con su ROM)", "sin respuesta a la nueva velocidad", "el aparato rechazó el PIN",
        "el aparato no tiene puerto serie (o no contesta)", "no se pudo conectar", "el enlace se cayó" };
    return f >= 0 && f <= SH_BTF_CAIDA ? n[f] : "?";
}
