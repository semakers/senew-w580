/* El host simulado que hay detrás de herr/pruebas/usb_dc_falso.c (pista USBD). Las funciones de transferencia llaman a
 * hst_bomba() mientras esperan (ahí va usb_paso() y lo que haga el lado del dispositivo). */
#ifndef USB_HOST_H
#define USB_HOST_H
typedef struct { unsigned controles, stalls_vistos, toggles_mal, excesos, mal_uso, paquetes_in, zlps_in, inicios, paradas, lecturas_vbus, fallar_inicio;
                 unsigned char direccion_puesta; } HostCuentas;
extern HostCuentas hst;
extern int hst_prisa;      /* 1 = tras el estado OUT de una lectura, el host no da tiempo al dispositivo: el SETUP siguiente llega encima */
extern void (*hst_bomba)(void);
int  hst_pullup(void);
unsigned char hst_direccion(void);
void hst_vbus(int si);
void hst_reset(void);
void hst_suspender(int si);
void hst_tog_reset(unsigned char ep);
int  hst_control(const unsigned char s[8], unsigned char *datos, int *n_datos);   /* 0 = bien, -1 STALL, -2 tope, -3 protocolo */
int  hst_bulk_out(unsigned char ep, const unsigned char *p, unsigned n, int zlp);
int  hst_bulk_in(unsigned char ep, unsigned char *p, unsigned max);
int  hst_int_in(unsigned char ep, unsigned char *p);
int  hst_in_lleno(unsigned char ep);
int  hst_control_a_medias(const unsigned char *s);   /* SETUP de lectura, se lleva el primer paquete y lo abandona */
void hst_vbus_mudo(int si);
#endif
