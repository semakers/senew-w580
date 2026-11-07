/* (USBD) Lo que hal/usb_dc.c ve en el PC cuando se compila con -DUSBDC_MODELO: los accesos a registros van a un MODELO del UDC
 * (udc_modelo.c, el comportamiento de docs/usb.md §9) y las IRQ de la CPU / del intcon son de mentira. */
#ifndef UDC_MODELO_H
#define UDC_MODELO_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
#define SYSCON 0xF9090000u
u16  modelo_lee(u32 a);
void modelo_esc(u32 a, u16 v);
u8   modelo_lee8(u32 a);
void modelo_esc8(u32 a, u8 v);
#define LEE(a)     modelo_lee(a)
#define ESC(a, v)  modelo_esc((a), (u16)(v))
#define LEE8(a)    modelo_lee8(a)
#define ESC8(a, v) modelo_esc8((a), (u8)(v))
typedef void (*irq_fn)(void);
u32  irq_cerrar(void);
void irq_abrir(void);
void intc_habilita_nivel(u32 linea, irq_fn f, u32 prioridad);
void intc_tapar(u32 linea);
void intc_habilita(u32 linea, irq_fn f);
static inline void espera_ms(unsigned ms) { (void)ms; }   /* (2026-10-20) encender_bloque espera al oscilador; en el modelo, nada */
#endif
