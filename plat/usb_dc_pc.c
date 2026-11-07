/* (USBD) El controlador USB en el sistema del PC: sin host detrás (nunca se configura). VBUS es el bit «USB» del cargador del
 * guion («cargador 2»): lo que se prueba aquí es que el sistema arranca y para el USB con el cable y no se duerme a fondo con él.
 * La enumeración y el tubo se prueban con el host simulado: herr/pruebas/prueba_usb.c (make probar-usb). */
#include "../hal/usb_dc.h"
extern int pc_cargador_bits;
UdcCuentas udc_cta;
static int en_marcha;
int  usbdc_iniciar(void) { en_marcha = 1; return 0; }
void usbdc_conectar(int si) { (void)si; }
int  usbdc_vbus(void) { return (pc_cargador_bits & 2) ? 1 : 0; }
void usbdc_direccion(u8 dir) { (void)dir; }
int  usbdc_ep_configurar(u8 ep, u8 tipo, u16 tam) { (void)ep; (void)tipo; (void)tam; return 0; }
int  usbdc_ep_escribir(u8 ep, const u8 *p, u32 n) { (void)ep; (void)p; (void)n; return -1; }
int  usbdc_ep_leer(u8 ep, u8 *p, u32 max) { (void)ep; (void)p; (void)max; return -1; }
void usbdc_ep_stall(u8 ep, int si) { (void)ep; (void)si; }
int  usbdc_evento(UdcEvento *ev, u8 *ep) { (void)ev; (void)ep; return 0; }
void usbdc_parar(void) { en_marcha = 0; }
