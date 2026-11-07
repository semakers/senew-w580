/* cg_bt.c — (1.4, pista BT) las envolturas del grupo Bluetooth de la tabla `w5` (api/cargable_bt.h; el contrato documentado está
 * en bt/sdk/w5_bt.h). Mismas reglas que cg12_*.c y cg13.c: todo lo que entra de la app se VALIDA (memoria de la app, tamaños con
 * tope) y se COPIA; nada acaba en pánico. Sin CG_PIDE a propósito: el Bluetooth es OPCIONAL para la app (w5_bt.h: una app de robots
 * funciona con y sin radio y NO declara W5_CAP_BT), así que sin radio cada función contesta «no disponible» y la app sigue.
 *
 * La app es la DUEÑA de la radio mientras está cargada (como de la serie): al descargarla, cg_descargar() llama a
 * sh_bt_soltar_app(), que desconecta y apaga (y devuelve el PIN a «1234»). docs/sistema.md §30. */
#include "cargador.h"
#include "cargable_bt.h"
#include "bt_sis.h"
#include "red_sis.h"                                /* (1.6) uno a la vez: con la red viva, no se conecta un robot */

#define COLA_BT 1024u                                 /* lo más que se copia por llamada (la cola del servicio es de 8/16 KB: la app repite) */
static const BtDriver *d_(void) { return cg_estado() == CG_VIVA ? bt_driver() : 0; }

int t_bt_encender(void)
{
    if (!d_()) return W5_NO_DISPONIBLE;
    sh_red_ceder();                                   /* (0.29.1) la red que SENUS deja retenida no le quita la radio a un robot */
    if (sh_bt_activo()) return W5_ERROR;
    return sh_bt_encender(SH_BT_APP) ? W5_ERROR : W5_OK;
}
void t_bt_apagar(void) { if (d_() && sh_bt_dueno() == SH_BT_APP) sh_bt_apagar(); }
int  t_bt_estado(void) { const BtDriver *d = d_(); return d && !sh_red_solo_retenida() ? d->estado() : W5_BT_APAGADO; }   /* (0.29.1) la retenida no es de la app: bt_encender la aparta */
int  t_bt_fallo(void) { const BtDriver *d = d_(); return d ? d->fallo() : W5_BTF_OK; }
int  t_bt_buscar(unsigned s)
{
    const BtDriver *d = d_();
    if (!d) return W5_NO_DISPONIBLE;
    if (d->estado() != W5_BT_LISTO) return W5_ERROR;
    if (s < 1) s = 1;
    if (s > 30) s = 30;
    return d->buscar(s) ? W5_ERROR : W5_OK;
}
int  t_bt_encontrados(void) { const BtDriver *d = d_(); return d && d->estado() != W5_BT_APAGADO ? d->encontrados() : 0; }
int  t_bt_encontrado(int i, W5BtDisp *o)
{
    const BtDriver *d = d_(); ShBtDisp x; W5BtDisp c;
    if (!d) return W5_NO_DISPONIBLE;
    if (!o || !cg_en_app(o, sizeof *o) || d->estado() == W5_BT_APAGADO || d->encontrado(i, &x)) return W5_ERROR;
    memset(&c, 0, sizeof c);
    memcpy(c.addr, x.addr, 6); c.clase = x.clase; memcpy(c.nombre, x.nombre, sizeof c.nombre); c.nombre[sizeof c.nombre - 1] = 0;
    memcpy(o, &c, sizeof c);                          /* entero, de una vez: la app no ve nada a medias */
    return W5_OK;
}
void t_bt_pin(const char *pin)
{
    const BtDriver *d = d_(); char p[17]; unsigned n = 0;
    if (!d) return;
    if (pin) {
        if (!cg_legible(pin)) return;                 /* un puntero que no es suyo: se ignora (el PIN sigue como estaba) */
        while (n < 16 && cg_legible_n(pin + n, 1) && pin[n]) { p[n] = pin[n]; n++; }
    }
    p[n] = 0;
    d->pin(n ? p : 0);                                /* NULL o «» = «1234» */
}
static int addr_de_app(const unsigned char *a, u8 *c) { if (!a || !cg_legible_n(a, 6)) return 0; memcpy(c, a, 6); return 1; }
int  t_bt_emparejado(const unsigned char a[6]) { const BtDriver *d = d_(); u8 c[6]; return d && addr_de_app(a, c) ? (d->emparejado(c) ? 1 : 0) : 0; }
int  t_bt_conectar_spp(const unsigned char a[6])
{
    const BtDriver *d = d_(); u8 c[6];
    if (!d) return W5_NO_DISPONIBLE;
    if (!addr_de_app(a, c) || d->estado() != W5_BT_LISTO) return W5_ERROR;
    if (sh_red_estado() == SH_RED_CONECTANDO || sh_red_estado() == SH_RED_LISTA) return W5_ERROR;
    return d->conectar_spp(c) ? W5_ERROR : W5_OK;
}
int  t_bt_enviar(const void *p, unsigned n)
{
    const BtDriver *d = d_(); u8 b[COLA_BT];
    if (!d || !p || !n) return 0;
    if (n > COLA_BT) n = COLA_BT;
    if (!cg_legible_n(p, n)) return 0;
    memcpy(b, p, n);                                  /* la copia: lo que se mira es lo que se manda */
    return d->enviar(b, n);
}
int  t_bt_pendiente(void) { const BtDriver *d = d_(); return d && d->estado() != W5_BT_APAGADO ? d->pendiente() : 0; }
int  t_bt_recibir(void *p, unsigned max)
{
    const BtDriver *d = d_(); u8 b[COLA_BT]; int k;
    if (!d || !p || !max) return 0;
    if (max > COLA_BT) max = COLA_BT;
    if (!cg_en_app(p, max)) return 0;
    k = d->recibir(b, max);
    if (k > 0) memcpy(p, b, (unsigned)k);
    return k;
}
void t_bt_desconectar(void) { const BtDriver *d = d_(); if (d && sh_bt_dueno() == SH_BT_APP) d->desconectar(); }
int  t_bt_recordado(unsigned char a[6])
{
    const BtDriver *d = d_(); u8 c[6];
    if (!d || !a || !cg_en_app(a, 6) || !d->recordado(c)) return 0;
    memcpy(a, c, 6);
    return 1;
}
/* la capacidad que se suma a CG_CAPS (shell/cargador.h): solo con controlador (el W580) */
unsigned cg_bt_caps(void) { return bt_driver() ? (unsigned)W5_CAP_BT : 0u; }
