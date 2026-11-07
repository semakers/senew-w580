/* bluetooth.c — (pista BT, 2026-10-16) Ajustes -> «Bluetooth»: la pantalla de DIAGNÓSTICO del chip para la primera prueba en el
 * aparato (docs/sistema.md §30, bt/PASOS_APARATO.md). Enseña paso a paso lo que ha pasado al encender —reset, P2.0, HCI_Reset ->
 * Command Complete, revisión HCI/LMP y fabricante reales, parche sí/no, BD_ADDR, velocidad— y lo mismo sale por la UART 0
 * («[bt] …»). «Opciones»: encender / buscar (8 s: nombre y clase de lo que haya cerca) / apagar, la velocidad final del próximo
 * encendido, la traza HCI por la UART y olvidar los emparejados. Al salir de aquí, si la encendió esta pantalla, se apaga.
 * No escribe en la NOR (lo único que el Bluetooth escribe son las claves de enlace al EMPAREJAR, y «Olvidar», que lo pregunta).
 * Sin radio (W600, W810, el PC) dice «sin Bluetooth» y nada más.
 * (1.6) LA RED: la fila «Puente de red» dice cuál es (almacén NV_RED); OK sobre un aparato encontrado lo elige como puente; OK sobre
 * «Puente de red» PRUEBA la red (radio + SPP + saludo W5RED, con esta pantalla de dueña) y la fila «  red» dice cómo va. */
#include "shell.h"
#include "bt_sis.h"
#include "red_sis.h"

typedef struct { Widget base; } BtPant;
typedef struct { i32 sel; } BtPantE;
extern const Clase ClsBtPant;
enum { F_ESTADO, F_RESET, F_P20, F_HCI, F_VERSION, F_FABR, F_REV, F_PTC, F_SSF, F_ORIGEN, F_ADDR, F_ADDR_LEIDA, F_VEL, F_VEL_FINAL,
       F_UART, F_CLAVES, F_RECORDADO, F_PUENTE, F_RED, F_N };   /* detrás: los aparatos encontrados */

static void tic(Ctx *c) { ui_sucio(c); }
static void iniciar(Ctx *c) { ui_cada(c, 250, tic); }
static void desechar(Ctx *c)                       /* la encendió esta pantalla: se apaga al salir (y la red que probara, antes) */
{
    (void)c;
    sh_red_soltar(SH_BT_SISTEMA);                  /* (1.8) suelta lo suyo: si la música usa la red, sigue (y la radio pasa a ella) */
    sh_bt_soltar(SH_BT_SISTEMA);
}

static const char *addr_txt(const u8 *a) { return ui_fmt("%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]); }
static const char *fabricante(u16 f)
{
    switch (f) { case 0x0000: return "Ericsson"; case 0x0030: return "ST Microelectronics"; case 0x000A: return "CSR";
                 case 0x000F: return "Broadcom"; case 0x001D: return "Qualcomm"; case 0x0025: return "NXP/Philips"; }
    return "otro";
}
static const char *fichero(signed char v) { return v == -2 ? "-" : v > 0 ? "cargado" : v == 0 ? "no había" : "FALLÓ"; }
static const char *ms_o(i32 v, const char *espera) { return v >= 0 ? ui_fmt("a los %d ms", v) : espera; }

/* OJO: sh_fila guarda los PUNTEROS y pinta después: todo texto que viva en `d` (pila) se copia a la arena con ui_fmt */
static Widget *fila(Ctx *l, int i, int sel, void *dato)
{
    ShBtDiag d; const BtDriver *dr = bt_driver(); int on;
    (void)l; (void)dato;
    sh_bt_diag(&d);
    on = d.hay && d.estado != SH_BT_APAGADO;
    if (!d.hay) return i == 0 ? sh_fila("Estado", "sin Bluetooth", sel) : sh_fila("Radio", "no hay", sel);
    switch (i) {
    case F_ESTADO: return sh_fila("Estado", d.estado == SH_BT_FALLO ? ui_fmt("FALLO: %s", sh_bt_fallo_nombre(d.fallo))
                                  : d.fallo == SH_BTF_PARCHE ? ui_fmt("%s (aviso: parche cortado)", sh_bt_estado_nombre(d.estado)) : sh_bt_estado_nombre(d.estado), sel);
    case F_RESET: return sh_fila("1. nRESET (100 ms)", !d.t_encender ? "-" : ms_o(d.ms_reset, on ? "en reset…" : "-"), sel);
    case F_P20: return sh_fila("2. P2.0 (el chip vive)", d.ms_p20 == -2 ? "NO subió en 1 s" : ms_o(d.ms_p20, d.ms_reset >= 0 && on ? "esperando…" : "-"), sel);
    case F_HCI: return sh_fila("3. HCI_Reset", d.hci_reset_estado >= 0 ? ui_fmt("Command Complete %s, %d ms", d.hci_reset_estado ? "CON ERROR" : "(0)", d.ms_hci_reset)
                               : d.ms_p20 >= 0 && on ? "sin respuesta aún" : "-", sel);
    case F_VERSION: return sh_fila("4. Versión HCI / LMP", d.hay_version ? ui_fmt("0x%04X / %d, 0x%04X", d.hci_rev, d.lmp_version, d.lmp_subver) : "-", sel);
    case F_FABR: return sh_fila("  fabricante", d.hay_version ? ui_fmt("0x%04X %s", d.fabricante, fabricante(d.fabricante)) : "-", sel);
    case F_REV: return sh_fila("  revisión (FC0F)", d.revision[0] ? ui_fmt("%s", d.revision) : "-", sel);
    case F_PTC: return sh_fila("5. Parche .ptc", fichero(d.ptc), sel);
    case F_SSF: return sh_fila("  ajustes .ssf", fichero(d.ssf), sel);
    case F_ORIGEN: return sh_fila("  de", d.origen[0] ? ui_fmt("%s", d.origen) : "-", sel);
    case F_ADDR: return sh_fila("6. BD_ADDR", d.t_encender ? ui_fmt("%s %s", addr_txt(d.addr), d.addr_origen == SH_BT_ADDR_GUARDADA ? "(guardada)" : "(aleatoria)") : "-", sel);
    case F_ADDR_LEIDA: return sh_fila("  leída del chip", d.hay_addr_leida ? addr_txt(d.addr_leida) : "-", sel);
    case F_VEL: return sh_fila("7. Velocidad", on && d.baudios ? ui_fmt("%u baudios", d.baudios) : "-", sel);
    case F_VEL_FINAL: return sh_fila("  al final", ui_fmt("%u%s", d.baudios_final, d.baudios_final == 115200 ? " (no se cambia)" : ""), sel);
    case F_UART: return sh_fila("UART 3 enviados / recibidos", ui_fmt("%u / %u B%s", d.tx, d.rx, dr && dr->traza(-1) ? ", traza HCI" : ""), sel);
    case F_CLAVES: return sh_fila("Emparejados guardados", ui_fmt("%d%s", d.claves, d.claves_pendientes ? " (uno PENDIENTE de guardar)" : ""), sel);
    case F_RECORDADO: return sh_fila("  el último", d.hay_recordado ? addr_txt(d.recordado) : "ninguno", sel);
    case F_PUENTE: { u8 a[6]; return sh_fila("Puente de red (OK: probar)", sh_red_puente(a) ? addr_txt(a) : "ninguno (OK en un aparato)", sel); }
    case F_RED: { int e = sh_red_estado();
                  int du = sh_red_duenos();       /* (1.8) quién la usa: la música, una app, esta pantalla */
                  const char *quien = du & (1 << SH_BT_MUSICA) ? (du & (1 << SH_BT_APP) ? " (música y app)" : " (la música)")
                                      : du & (1 << SH_BT_APP) ? " (una app)" : "";
                  return sh_fila("  red", e == SH_RED_FALLO ? ui_fmt("FALLO: %s", sh_red_fallo_nombre(sh_red_fallo()))
                                 : ui_fmt("%s%s", sh_red_estado_nombre(e), quien), sel); }
    }
    {   ShBtDisp x;
        if (dr && on && !dr->encontrado(i - F_N, &x))
            return sh_fila(x.nombre[0] ? ui_fmt("%s", x.nombre) : "(sin nombre)", ui_fmt("%s %06X%s", addr_txt(x.addr), x.clase, dr->emparejado(x.addr) ? " emp." : ""), sel);
    }
    return sh_fila("", "", sel);
}
static int n_filas(void)
{
    ShBtDiag d; const BtDriver *dr = bt_driver();
    sh_bt_diag(&d);
    if (!d.hay) return 2;
    return F_N + (dr && d.estado != SH_BT_APAGADO ? dr->encontrados() : 0);
}

static void olvidar_si(void *x) { const BtDriver *d = bt_driver(); (void)x; if (d) d->olvidar_todo(); }
static u8 candidato[6];
static void puente_si(void *x)
{
    (void)x;
    if (sh_red_poner_puente(candidato) != HW_OK) ui_aviso("Puente de red", "No se pudo guardar (almacén lleno o con fallo).");
}
static const char *const ops_on[] = { "Buscar aparatos (8 s)", "Apagar", "Traza HCI por la UART: sí/no", "Olvidar emparejados" };
static const char *const ops_off[] = { "Encender", "Velocidad final: 115200 / 921600 / 1843200", "Traza HCI por la UART: sí/no", "Olvidar emparejados" };
static void opcion(int i, void *x)
{
    const BtDriver *d = bt_driver(); ShBtDiag g;
    (void)x;
    if (!d) return;
    sh_bt_diag(&g);
    if (i == 2) { d->traza(!d->traza(-1)); return; }
    if (i == 3) { ui_confirmar_no("Olvidar emparejados", "¿Borrar las claves de enlace guardadas? La próxima vez cada robot volverá a pedir el PIN. "
                                  "Es una escritura en el almacén del teléfono.", olvidar_si, 0); return; }
    if (g.estado == SH_BT_APAGADO) {
        if (i == 0) { if (sh_bt_encender(SH_BT_SISTEMA)) ui_aviso("Bluetooth", "No se pudo encender."); }
        else {   /* (velocidad) 115 200 -> 921 600 -> 1 843 200 -> 115 200; las tres medidas en el aparato (docs/bluetooth.md §11) */
            u32 sig = g.baudios_final == 115200 ? 921600 : g.baudios_final == 921600 ? 1843200 : 115200;
            d->velocidad(sig);
            ui_aviso("Velocidad final", ui_fmt("%u baudios entre el teléfono y el chip desde el próximo encendido (0xFCFC tras el parche). "
                                               "Vale solo hasta reiniciar.", sig));
        }
        return;
    }
    if (i == 0) { if (g.estado != SH_BT_LISTO || d->buscar(8)) ui_aviso("Buscar", "Solo cuando el estado es «listo»."); }
    else sh_bt_apagar();
}
/* (1.6) OK sobre una fila (la lista lo manda aquí con su número): «Puente de red» prueba la red; un aparato encontrado, elegirlo */
static void activar(Ctx *l, int i, void *dato)
{
    const BtDriver *d = bt_driver(); ShBtDisp x;
    (void)l; (void)dato;
    if (!d) return;
    if (i == F_PUENTE) {
        if (sh_red_es_dueno(SH_BT_SISTEMA) && sh_red_estado() != SH_RED_FALLO) sh_red_soltar(SH_BT_SISTEMA);   /* (1.8) OK otra vez: suelta lo suyo */
        else if (sh_red_conectar(0, SH_BT_SISTEMA))
            ui_aviso("Puente de red", sh_red_fallo() == SH_REDF_SIN_PUENTE ? "Primero hay que elegir uno: Buscar y OK sobre el aparato."
                                      : ui_fmt("No se pudo: %s.", sh_red_fallo_nombre(sh_red_fallo())));
    } else if (i >= F_N && d->estado() != SH_BT_APAGADO && !d->encontrado(i - F_N, &x)) {
        memcpy(candidato, x.addr, 6);
        ui_confirmar("Puente de red", ui_fmt("¿Usar «%s» (%s) como puente de red? Las apps saldrán a internet por él "
                                             "(el computador con SENUS abierta).", x.nombre[0] ? x.nombre : "sin nombre", addr_txt(x.addr)), puente_si, 0);
    }
}
static int tecla(Ctx *t, int k, void *dato)
{
    ShBtDiag g;
    (void)t; (void)dato;
    if (k == K_SEL_IZQ) {
        sh_bt_diag(&g);
        if (!g.hay) ui_aviso("Bluetooth", "Este teléfono no tiene Bluetooth, o este sistema no trae su controlador (solo el W580i lo trae).");
        else if (g.estado == SH_BT_APAGADO) ui_opciones("Bluetooth", ops_off, 4, opcion, 0);
        else if (sh_bt_dueno() != SH_BT_SISTEMA) ui_aviso("Bluetooth", "Lo tiene encendido una app: se maneja desde ella.");
        else ui_opciones("Bluetooth", ops_on, 4, opcion, 0);
        return 1;
    }
    if (k == K_OK) return 1;
    return sh_teclas(t, k, dato);
}
static Widget *construir(Ctx *c)
{
    BtPantE *s = ui_estado(c);
    return Teclas(tecla, Pantalla("Bluetooth", Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        Expandido(ListaPerezosa(.n = n_filas(), .fila = fila, .al_activar = activar, .sel = &s->sel, .alto_fila = dis_tokens()->m.alto_fila)),
        Caja(.an = INF, .color = sh_tema()->barra, .hijo = Relleno(4, Parrafo(sh_bt_hay() ?
            "Diagnóstico del chip (STLC2500, UART 3). Todo sale también por la UART 0. Al salir de aquí se apaga." :
            "Sin Bluetooth en este teléfono.", .color = sh_tema()->tenue, .marcas = F_ESTILO(DIS_T_PEQUENA)))))),
        "Opciones", "Atrás"));
}
COMPONENTE_CON_ESTADO(BtPant, BtPantE, construir, iniciar, .desechar = desechar);
Widget *app_bluetooth(void) { return UI_W(BtPant); }
