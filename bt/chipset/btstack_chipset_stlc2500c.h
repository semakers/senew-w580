/* btstack_chipset_stlc2500c.h — «chipset» de BTstack para el STLC2500C/C4/R5 del W580i (bt/DISENO.md §3).
 *
 * Amplía el btstack_chipset_stlc2500d de BTstack (que solo sabe 0xFCFC y 0xFC22) con lo que hace el original y
 * BlueZ (stlc2500_init, hciattach_st.c): tras el primer HCI_Reset,
 *     0xFC0F Ericsson_Read_Revision_Information
 *     0xFC2E <seq> <≤254 B>   × N   el .ptc (parche de ROM)       — seq desde 0
 *     0xFC2E <seq> <≤254 B>   × N   el .ssf (ajustes estáticos)   — seq vuelve a 0
 *     HCI_Reset                       aplica parche y ajustes
 *     0xFC22 FE 06 <BD_ADDR>          si se ha dado una (btstack_chipset_stlc2500c_set_bd_addr)
 *     HCI_Reset                       la aplica
 * (No se usa hci_set_bd_addr de BTstack: su reset posterior solo lo hace si el fabricante leído es ST, 0x0030, y el
 * del STLC2500C es [sin saber].) El cambio de velocidad (0xFCFC) lo hace el servicio al final, como el original
 * (DISENO §3.3); set_baudrate_command queda para quien prefiera el orden de BTstack.
 *
 * El chipset no ve los eventos: quien registra el manejador de HCI (el servicio) le pasa la versión local y cada
 * Command Complete de 0xFC2E, para comprobar estado y número de secuencia como BlueZ. */
#ifndef BTSTACK_CHIPSET_STLC2500C_H
#define BTSTACK_CHIPSET_STLC2500C_H
#include <stdint.h>
#include "btstack_chipset.h"

/* Lee el fichero «cual» (0 = .ptc, 1 = .ssf) elegido para (hci_rev, lmp_subver) desde «off»: devuelve los bytes
 * copiados (0 = fin) o -1 si no hay fichero para esa versión (entonces se salta, como BlueZ: «No ROM patch file»). */
typedef int (*stlc2500c_leer_t)(int cual, uint16_t hci_rev, uint16_t lmp_subver, uint32_t off, uint8_t *buf, uint16_t max);

const btstack_chipset_t *btstack_chipset_stlc2500c_instance(void);
void btstack_chipset_stlc2500c_set_lector(stlc2500c_leer_t f);
void btstack_chipset_stlc2500c_set_bd_addr(const bd_addr_t a);
/* del manejador de eventos HCI del servicio: */
void btstack_chipset_stlc2500c_version(uint16_t hci_rev, uint16_t lmp_subver);
void btstack_chipset_stlc2500c_evento(const uint8_t *ev, uint16_t n);   /* todos los eventos; mira los de 0xFC0F y 0xFC2E */

typedef struct {
    uint16_t hci_rev, lmp_subver;
    char     revision[64];          /* la cadena de 0xFC0F («STLC2500 R4.2 …») */
    uint16_t trozos[2];             /* trozos de .ptc y .ssf mandados y confirmados */
    uint32_t bytes[2];
    int8_t   fichero[2];            /* 1 = cargado, 0 = no había, -1 = falló (estado o secuencia) */
    uint8_t  error;                 /* 1 = la descarga se cortó: el chip sigue con su ROM */
} stlc2500c_informe_t;
const stlc2500c_informe_t *btstack_chipset_stlc2500c_informe(void);

#endif
