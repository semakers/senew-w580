/* (USBD) El USB en el sistema: lo mínimo (docs/usb_driver.md §4). Con VBUS arranca el núcleo USB (api/usb.c), que se presenta
 * como dispositivo MTP; al configurarlo el host avisa «Conectado por USB» y le da el bucle al respondedor MTP (api/mtp.c, de
 * la pista MTP: si no está enlazado, el USB enumera igual y el respondedor no hace nada); al quitar el cable lo para todo.
 * Nada de aquí toca la carga: la lleva hw_carga_paso (api/carga.h, docs/bateria.md §12) con lo que concede el PC (usb_carga_ma); VBUS solo se LEE. */
#include "shell.h"
#include "usb.h"
#include "gestion.h"                            /* (Fase 3b) el canal de gestión de SENUS: gs_paso, en el hilo de la interfaz */
#ifdef PUENTE
#include "puente.h"                             /* (v16) el puente de depuración por USB (docs/puente.md) */
#endif

/* el respondedor MTP, si está enlazado (contrato §3); débiles para que el sistema se enlace sin él */
void mtp_iniciar(void) __attribute__((weak));
void mtp_paso(void) __attribute__((weak));
void mtp_parar(void) __attribute__((weak));
void mtp_permitir(int si) __attribute__((weak));
int  mtp_permiso(void) __attribute__((weak));
int  mtp_sesion_abierta(void) __attribute__((weak));

/* (MTP; AUD9) EL PERMISO Y LA PANTALLA MODAL. Al configurarse el USB el teléfono PREGUNTA, con el foco en «No» (como Android):
 * hasta el «Sí», MTP no abre sesión (OpenSession = Device_Busy). Con la sesión abierta, «Conectado por USB» es una pantalla
 * MODAL: se traga las teclas (salvo encendido y bloqueo), así que no se abren apps ni Archivos y la Memoria queda quieta
 * mientras la computadora la usa (A1/M3 de AUD9; y, por si algo escribe igual, MTP invalida sus handles: api/mtp.c). Se quita
 * sola al cerrar la sesión o soltar el cable. «No» o salir del diálogo deja el acceso denegado hasta volver a enchufar. */
typedef struct { Widget base; } UsbModal;
extern const Clase ClsUsbModal;
static int modal_tecla(Ctx *c, int k, void *dato) { (void)c; (void)dato; return k == K_ENCENDIDO || k == K_BLOQUEO ? 0 : 1; }
static Widget *modal_construir(Ctx *c)
{
    (void)c;
    /* (MD) una tarjeta del diseño: el símbolo, el título y la explicación */
    return Teclas(modal_tecla, Pantalla("Conectado por USB", Relleno(8, Tarjeta(Columna(.espacio = 8, .cruzado = P_ESTIRAR, .hijos = HIJOS(
        Icon(&sh_iconos[IC_ENCHUFE], .escala = 2, .color = dis_tokens()->c.primario),
        Texto("La computadora está usando la Memoria", .color = dis_tokens()->c.sobre_superficie, .marcas = F_ESTILO(DIS_T_TITULO)),
        Parrafo("Mientras tanto no se puede usar el teléfono. Quita el cable (o expulsa el teléfono en la computadora) para seguir. "
                "La batería se sigue cargando del ordenador.", .color = dis_tokens()->c.sobre_superficie_variante))), 0)), "", ""));
}
const Clase ClsUsbModal = { .nombre = "UsbModal", .tipo = T_CON_ESTADO, .tam = sizeof(UsbModal), .tam_estado = 4, .construir = modal_construir };
static u8 preguntado, modal;
static void permitir_si(void *d) { (void)d; if (mtp_permitir) mtp_permitir(1); }
static void modal_fuera(void) { if (modal) { ui_quitar_ruta(&ClsUsbModal); modal = 0; hw_log("usb: fuera la pantalla modal\n"); } }

#ifndef USB_ESPERA_CABLE_PMU
#ifdef TELEFONO                                   /* (usb_ag, 2026-10-20) solo el teléfono: en el PC el «cable» del host falso (pc-usb, */
#define USB_ESPERA_CABLE_PMU 1                    /* plat/pc_usb_host.c) no pone el bit del cargador y probar-mtp (permiso_si/no) no enumeraba */
#else
#define USB_ESPERA_CABLE_PMU 0
#endif
#endif
static u8 en_marcha, configurado;
static u32 t_marcha;                              /* hw_ms() al arrancar el USB */
void sh_usb_paso(int en_app)
{
    int act, c;
#ifdef SIN_USB
    (void)en_app; return;                         /* (DIAG 2026-10-20) el USB fuera: en el aparato, leer el UDC colgaba el arranque */
#endif
    if (hw_modo_seguro()) return;                 /* en modo seguro, nada de USB (la Memoria es de solo lectura y no se arranca nada nuevo) */
    { static u8 id_leido; if (!id_leido) { id_leido = 1; gs_id_cargar(); } }   /* (Fase 3b) antes de la primera enumeración: la serie sale del id vinculado */
#if USB_ESPERA_CABLE_PMU
    /* (2026-10-20) Mientras el USB está parado, el bloque NO se toca hasta que el PMU (0x23 bit 7) vea el cable: un UDC sin sus
     * relojes cuelga el bus en el aparato (hal/usb_dc.c, encender_bloque). Así un fallo del USB nunca impide arrancar sin cable.
     * El emulador no pone ese bit al enchufar: sus pruebas de USB compilan con -DUSB_ESPERA_CABLE_PMU=0. */
    if (!en_marcha) { int cg = hw_cargador(); if (cg < 0 || !(cg & 2)) return; }
#endif
    act = usb_vigilar(hw_ms());                   /* lee VBUS como mucho cada 300 ms y atiende como mucho 32 eventos: no bloquea */
    if (act != en_marcha) hw_log(act ? "usb: VBUS, en marcha\n" : "usb: sin VBUS, parado\n");
    if (act && !en_marcha) t_marcha = hw_ms();
    en_marcha = (u8)act;
#ifdef PUENTE
    if (act) puente_paso(en_app);                 /* (v16) la orden pendiente del puente: aquí, con las IRQ abiertas y el perro al día, no dentro del manejador del USB */
#endif
    c = usb_conectado();
    if (c && !configurado) {
        hw_log("usb: configurado por el host (MTP)\n");
        if (mtp_iniciar) mtp_iniciar();
        preguntado = 0;
    } else if (!c && configurado) {
        hw_log("usb: desconfigurado\n");
        if (mtp_parar) mtp_parar();
        modal_fuera();
    }
    configurado = (u8)c;
    /* la pregunta, en cuanto se pueda (no encima de una app ni de otro diálogo: el de energía, si está, va antes) */
    if (c && mtp_permiso && !preguntado && !en_app && !ui_cima_emergente()) {
        preguntado = 1; sh_actividad();
        ui_confirmar_no("Conectado por USB", "¿Permitir acceso a la Memoria desde el ordenador? Podrá leer, copiar y borrar ficheros y poner apps. "
                                             "La batería se carga igual, permitas o no.", permitir_si, NULL);
    }
    /* la pantalla modal mientras haya sesión */
    if (c && mtp_sesion_abierta && mtp_sesion_abierta() && !modal && !en_app && !ui_cima_emergente()) { ui_empujar(UI_W(UsbModal)); modal = 1; hw_log("usb: sesión MTP abierta: pantalla modal\n"); }
    if (modal && !(c && mtp_sesion_abierta && mtp_sesion_abierta())) modal_fuera();
    if (c && !en_app && mtp_paso) {               /* con una app externa en pantalla el USB sigue vivo, pero el MTP espera */
        mtp_paso();
        gs_paso();                                /* (Fase 3b) lo que el manejador aceptó se aplica AQUÍ, fuera de MTP y en la misma vuelta (api/gestion.h) */
    }
}
/* ¿Impide el USB el reposo profundo? Sí mientras un host lo use, y los 10 primeros segundos tras ver VBUS (un host enumera en
 * décimas de segundo). Si en 10 s nadie lo ha configurado es un CARGADOR DE PARED (VBUS sin datos): sin configurar no se carga (CARGA: como el original),
 * quedarse despierto solo gastaría batería; el USB sigue en marcha y el reposo profundo vuelve. [sin saber] si una IRQ del USB
 * despierta del reposo profundo (cierra la puerta del intcon): por eso, con un host, no se duerme a fondo. */
int sh_usb_activo(void) { return usb_activo() && (usb_conectado() || hw_ms() - t_marcha < 10000u); }
