/* red_sis.c — (1.6) la sesión W5RED con el puente de red, sobre el SPP del servicio Bluetooth (red_sis.h; docs/sistema.md §31).
 *
 * Pasos de red_conectar: (0) la radio LISTA (si estaba apagada, se enciende con el mismo dueño); (1) SPP con el puente (SDP + RFCOMM;
 * si hace falta, PIN); (2) HOLA -> HOLA_OK. Después, en cada sh_red_paso (que llama sh_bt_paso: el bucle del sistema y app_latido),
 * red_paso() mueve lo que haya entre el SPP y los canales (bt/red/red.c). Si el SPP se cae, los canales quedan CERRADOS y la red en
 * FALLO (CAIDA); la app puede volver a conectar.
 * UNO A LA VEZ: la red usa el único enlace del servicio. Con un robot conectado, red_conectar da OCUPADA (y cg_bt.c no deja conectar
 * un robot mientras la red está viva). */
#include "shell.h"
#include "bt_sis.h"
#include "red_sis.h"
#include "../bt/red/red.h"

static u8 est, fallo_, sub, duenos, reintentos;      /* duenos: (1.8) bits 1 << SH_BT_* de quienes la usan */
static u8 canal_de[RED_CANALES + 1];                /* (1.8) de quién es cada canal (SH_BT_*; 0 = de nadie) */
#define BIT(q) ((u8)(1u << (q)))
static u8 destino[6];
static u32 t0;
static const BtDriver *dr;
static u8 retenida, pin_puesto, llamada_hecha;      /* (0.29.1) el puente de SENUS: abajo, sh_red_emparejar */
extern u8 bt_puente_red;                            /* (0.29.3) bt/servicio/bt_servicio.c; la débil de shell/bt.c sin radio */
#define NVR_LLAMAR 1u                               /* NV_RED[7], bit 0: al arrancar se llama solo. Sin ese byte (7 B, Ajustes o senew de antes): solo bajo demanda */
#define RED_LLAMAR_MS 8000u                         /* la llamada del arranque, a los 8 s: el sistema ya está en su bucle y el USB contesta */

static int t_enviar(const void *b, unsigned n) { int k = dr ? dr->enviar(b, n) : 0; return k > 0 ? k : 0; }
static int t_recibir(void *b, unsigned m) { int k = dr ? dr->recibir(b, m) : 0; return k > 0 ? k : 0; }
static const red_tubo_t tubo = { t_enviar, t_recibir };

int sh_red_hay(void) { return bt_driver() != 0; }
int sh_red_estado(void) { return est; }
int sh_red_fallo(void) { return fallo_; }
int sh_red_es_dueno(int quien) { return est != SH_RED_APAGADA && quien > 0 && quien < 8 && (duenos & BIT(quien)) != 0; }
int sh_red_duenos(void) { return est == SH_RED_APAGADA ? 0 : duenos; }
int sh_red_heredero(int quien)
{
    static const u8 orden[5] = { SH_BT_MUSICA, SH_BT_SISTEMA, SH_BT_APP, SH_BT_DICTADO, SH_BT_DESCARGA };   /* (E5) el dictado y (1.11) la descarga también heredan la radio */
    int i;
    for (i = 0; i < 5; i++) if (orden[i] != quien && sh_red_es_dueno(orden[i])) return orden[i];
    return SH_BT_NADIE;
}
void sh_red_canal_poner(int c, int quien) { if (c > 0 && c <= RED_CANALES) canal_de[c] = (u8)quien; }
int sh_red_canal_dueno(int c) { return c > 0 && c <= RED_CANALES ? canal_de[c] : SH_BT_NADIE; }

static int hexd(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
int sh_red_puente(u8 a[6])
{
    u8 b[8];
    char t[24];
    int i;
    if (hw_cargar(NV_RED, b, sizeof b) >= 7 && b[0] == 1) { memcpy(a, b + 1, 6); return 1; }
    /* (E5, 27-oct) SIN ALMACÉN (senew en RAM: la NOR no se escribe) el puente puede venir de la tarjeta: «/tarjeta/puente.txt» = «AA:BB:CC:DD:EE:FF» */
    if (hw_fs_leer("/tarjeta/puente.txt", (u8 *)t, sizeof t) < 17) return 0;
    for (i = 0; i < 6; i++) {
        int h = hexd(t[3 * i]), l = hexd(t[3 * i + 1]);
        if (h < 0 || l < 0 || (i < 5 && t[3 * i + 2] != ':')) return 0;
        a[i] = (u8)(h << 4 | l);
    }
    return 1;
}
int sh_red_poner_puente(const u8 a[6])
{
    u8 b[7];
    b[0] = 1; memcpy(b + 1, a, 6);
    return hw_guardar(NV_RED, b, sizeof b);
}
/* NV_RED con la bandera; si ya es ese registro, no gasta flash */
static int poner_de_senus(const u8 a[6])
{
    u8 b[8], v[8];
    b[0] = 1; memcpy(b + 1, a, 6); b[7] = NVR_LLAMAR;
    if (hw_cargar(NV_RED, v, sizeof v) == (int)sizeof v && !memcmp(v, b, sizeof b)) return HW_OK;
    return hw_guardar(NV_RED, b, sizeof b);
}

static void fallar(u8 f)
{
    int be = dr ? dr->estado() : SH_BT_APAGADO;
    red_parar();
    est = SH_RED_FALLO; fallo_ = f;
    if (dr && (be == SH_BT_CONECTADO || be == SH_BT_CONECTANDO || be == SH_BT_FALLO)) dr->desconectar();
    hw_log("red: FALLO: "); hw_log(sh_red_fallo_nombre(f)); hw_log("\n");
}

int sh_red_conectar(const u8 *addr, int quien)
{
    int be;
    dr = bt_driver();
    if (!dr) { est = SH_RED_FALLO; fallo_ = SH_REDF_SIN_RADIO; return -1; }
    if (est == SH_RED_CONECTANDO || est == SH_RED_LISTA) {
        if (addr && memcmp(addr, destino, 6)) return -1;      /* otro puente con esta red en uso: no */
        duenos |= BIT(quien);                                 /* (1.8) ya está (o va de camino): un dueño más */
        return 0;
    }
    if (addr) memcpy(destino, addr, 6);
    else if (!sh_red_puente(destino)) { est = SH_RED_FALLO; fallo_ = SH_REDF_SIN_PUENTE; return -1; }
    be = dr->estado();
    if (sh_bt_activo()) {
        int rb = sh_bt_dueno();                               /* (1.8) la radio la encendió quien pide, u otro dueño de ESTA red (tras un fallo) */
        if ((rb != quien && !(est == SH_RED_FALLO && (duenos & BIT(rb)))) || be == SH_BT_CONECTADO || be == SH_BT_CONECTANDO || be == SH_BT_BUSCANDO) {
            est = SH_RED_FALLO; fallo_ = SH_REDF_OCUPADA; return -1;
        }
        if (be == SH_BT_FALLO) dr->desconectar();             /* de un fallo anterior (un SPP que no se abrió): vuelve a LISTO */
    } else if (sh_bt_encender(quien)) { est = SH_RED_FALLO; fallo_ = SH_REDF_RADIO; return -1; }
    duenos = (u8)((est == SH_RED_FALLO ? duenos : 0) | BIT(quien));   /* (1.8) tras un fallo, los que la usaban siguen siendo dueños */
    est = SH_RED_CONECTANDO; fallo_ = SH_REDF_OK; sub = 0; t0 = hw_ms(); reintentos = 0;
    hw_log("red: conectando con el puente\n");
    return 0;
}

void sh_red_desconectar(void)
{
    int be;
    if (est == SH_RED_APAGADA) return;
    red_parar();
    be = dr ? dr->estado() : SH_BT_APAGADO;
    if (dr && (be == SH_BT_CONECTADO || be == SH_BT_CONECTANDO)) dr->desconectar();
    est = SH_RED_APAGADA; fallo_ = SH_REDF_OK; duenos = 0; retenida = 0;
    memset(canal_de, 0, sizeof canal_de);
    hw_log("red: desconectada\n");
}

void sh_red_soltar(int quien)
{
    int c;
    if (est == SH_RED_APAGADA || quien <= 0 || quien >= 8 || !(duenos & BIT(quien))) return;
    duenos &= (u8)~BIT(quien);
    if (quien == SH_BT_SISTEMA) retenida = 0;           /* Ajustes -> Bluetooth, «Al salir de aquí se apaga»: el puente sigue guardado y la próxima que pida red lo llama */
    for (c = 1; c <= RED_CANALES; c++)
        if (canal_de[c] == quien) { red_cerrar(c); canal_de[c] = 0; }
    if (!duenos) sh_red_desconectar();
    else hw_log("red: un duenyo la suelta; sigue para los otros\n");
}
void sh_red_soltar_app(void) { sh_red_soltar(SH_BT_APP); }

/* (0.29.1) EL PUENTE DE SENUS (SEN_BT_EMPAREJAR; contrato del canal §4, docs/mtp.md).
 * Antes la red solo vivía mientras alguien la usaba. Esta queda RETENIDA por SH_BT_SISTEMA: sobrevive al cable y a que se vaya el último dueño. No es gratis: con la radio encendida
 * no hay reposo profundo (shell/energia.c). Por eso se suelta sola: si se cae (SENUS cerrada, lejos), se apaga la radio salvo que otro dueño la use, y el fallo queda legible.
 * Si un robot pide la radio, cede (sh_red_ceder): uno a la vez, y manda el robot, que es lo que el usuario tiene delante.
 * Un puente en uso que no es este se corta sin preguntar: la orden es explícita y sus dueños siguen siéndolo, como tras una caída. El PIN es de esta orden: en cuanto
 * la llamada acaba vuelve a «1234», para que no se lo lleve el próximo robot. */
static int no_va(u8 f)
{
    if (est == SH_RED_CONECTANDO || est == SH_RED_LISTA) fallar(f);
    else { est = SH_RED_FALLO; fallo_ = f; hw_log("red: FALLO: "); hw_log(sh_red_fallo_nombre(f)); hw_log("\n"); }
    return -1;
}
int sh_red_emparejar(const u8 a[6], const char *pin)
{
    int viva = est == SH_RED_CONECTANDO || est == SH_RED_LISTA, igual = viva && !memcmp(a, destino, 6);
    if (!(dr = bt_driver())) return no_va(SH_REDF_SIN_RADIO);
    if (!viva && sh_bt_activo() && sh_bt_dueno() == SH_BT_APP) return no_va(SH_REDF_OCUPADA);   /* un robot: gs_sis_bt_puede ya contesta R_OCUPADO con la app delante */
    if (poner_de_senus(a) != HW_OK) return no_va(SH_REDF_ALMACEN);
    if (sh_bt_activo() && !igual) {                      /* otro puente, o la radio con otra cosa (Ajustes, una búsqueda): de cero */
        if (viva) fallar(SH_REDF_CAIDA);
        sh_bt_apagar();
    }
    dr->pin(pin); pin_puesto = 1;
    if (sh_red_conectar(a, SH_BT_SISTEMA)) return -1;
    retenida = 1;
    return 0;
}
int sh_red_solo_retenida(void) { return retenida && duenos == BIT(SH_BT_SISTEMA); }
void sh_red_ceder(void)
{
    if (!sh_red_solo_retenida()) return;
    hw_log("red: la red retenida cede la radio\n");
    sh_red_soltar(SH_BT_SISTEMA);
    sh_bt_soltar(SH_BT_SISTEMA);
}
void sh_red_arranque_paso(void)
{
    u8 b[8];
    if (llamada_hecha || hw_ms() < RED_LLAMAR_MS) return;
    llamada_hecha = 1;                                   /* UNA por arranque: si no contesta, la radio se apaga y queda bajo demanda */
    if (hw_modo_seguro() || !bt_driver() || est != SH_RED_APAGADA || sh_bt_activo()) return;
    if (hw_cargar(NV_RED, b, sizeof b) != (int)sizeof b || b[0] != 1 || !(b[7] & NVR_LLAMAR)) return;
    hw_log("red: llamando al puente de SENUS\n");
    if (!sh_red_conectar(b + 1, SH_BT_SISTEMA)) retenida = 1;
}

void sh_red_paso(void)
{
    int be; u32 ahora;
    if (pin_puesto && est != SH_RED_CONECTANDO) { pin_puesto = 0; if (dr) dr->pin(0); }
    if (retenida && est == SH_RED_FALLO) {               /* se cayó: la radio se apaga, o pasa al dueño que quede. El estado y el fallo se quedan para SEN_BT_ESTADO */
        retenida = 0; duenos &= (u8)~BIT(SH_BT_SISTEMA);
        sh_bt_soltar(SH_BT_SISTEMA);
    }
    if (!dr || (est != SH_RED_CONECTANDO && est != SH_RED_LISTA)) return;
    be = dr->estado(); ahora = hw_ms();
    if (est == SH_RED_CONECTANDO) {
        switch (sub) {
        case 0:                                                 /* la radio, LISTA */
            if (be == SH_BT_LISTO) {
                bt_puente_red = 1;                              /* (0.29.3) esta conexión es la del puente: si no se anuncia, que llame él (bt_servicio.c) */
                if (dr->conectar_spp(destino)) { fallar(SH_REDF_ENLACE); return; }
                sub = 1; t0 = ahora;
            } else if (be == SH_BT_FALLO || be == SH_BT_APAGADO || ahora - t0 > 20000) fallar(SH_REDF_RADIO);
            return;
        case 1:                                                 /* el SPP con el puente */
            if (be == SH_BT_CONECTADO) { red_empezar(&tubo, "senew W580"); sub = 2; t0 = ahora; hw_log("red: SPP abierto, saludando\n"); }
            else if (be == SH_BT_FALLO && reintentos < 2) {
                /* [observado en el aparato, 2026-10-25] la llamada (page) al ordenador puente a veces vence (0x04), sobre todo justo después de
                 * una búsqueda; al reintentar entra (en btmon, el ordenador la recibe en ~2 s). Hasta 2 reintentos antes de rendirse. */
                reintentos++; dr->desconectar(); sub = 0; t0 = ahora;
                hw_log("red: el enlace no se abrió: reintento\n");
            }
            else if (be != SH_BT_CONECTANDO || ahora - t0 > 45000) fallar(SH_REDF_ENLACE);
            return;
        default:                                                /* HOLA -> HOLA_OK */
            red_paso();
            if (red_estado() == RED_LISTA) { est = SH_RED_LISTA; hw_log("red: LISTA ("); hw_log(red_nombre_puente()); hw_log(")\n"); }
            else if (be != SH_BT_CONECTADO || red_estado() == RED_CAIDA || ahora - t0 > 8000) fallar(SH_REDF_SALUDO);
            return;
        }
    }
    if (be != SH_BT_CONECTADO) { fallar(SH_REDF_CAIDA); return; }
    red_paso();
    if (red_estado() == RED_CAIDA) fallar(SH_REDF_CAIDA);
}

const char *sh_red_estado_nombre(int e)
{
    static const char *const n[] = { "apagada", "conectando…", "lista", "FALLO" };
    return e >= 0 && e < 4 ? n[e] : "?";
}
const char *sh_red_fallo_nombre(int f)
{
    static const char *const n[] = { "bien", "sin radio", "no hay puente elegido", "la radio no arrancó", "el puente no contesta (o rechazó el PIN)",
                                     "contestó, pero no es un puente W5RED", "se cortó", "el Bluetooth está ocupado (un robot)", "no se pudo guardar el puente" };
    return f >= 0 && f < 9 ? n[f] : "?";
}
