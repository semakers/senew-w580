/* red.c — el lado del teléfono de W5RED v1 (red.h, bt/puente_red/PROTOCOLO.md). Sin malloc; toda la memoria es estática:
 * RED_CANALES × (RED_RX + RED_TX + RED_CAB) + las colas de entrada y salida (~143 KB con 4 canales de 16+16 KB). */
#include "red.h"
#include <string.h>

enum { T_HOLA = 1, T_HOLA_OK, T_PING, T_PONG, T_ADIOS,
       T_ABRE_HTTP = 0x10, T_ABRE_TCP, T_LISTO, T_RESPUESTA, T_DATOS, T_VENTANA, T_FIN, T_CIERRA };
#define SAL        8192u                              /* bytes ya en tramas, esperando sitio en el tubo */
#define RESERVA    512u                               /* lo que se deja libre en la salida para las tramas de control */

typedef struct {
    uint8_t  estado, codigo, es_http, fin_rx, fin_pedido, fin_mandado, cierra_pend;
    uint16_t http;
    uint32_t vent_tx;                                 /* lo que el puente nos deja mandar aún */
    uint32_t vent_dada;                               /* ventana dada al puente y aún sin gastar */
    uint8_t  rx[RED_RX]; uint16_t rx_ini, rx_n;
    uint8_t  tx[RED_TX]; uint16_t tx_ini, tx_n;
    char     cab[RED_CAB]; uint16_t cab_n;
    char     texto[64];
} canal_t;

static const red_tubo_t *tubo;
static int estado;
static uint16_t carga = RED_CARGA;
static uint8_t ncanales = RED_CANALES;
static char puente[48];
static canal_t can[RED_CANALES];                      /* el canal c (1..RED_CANALES) es can[c - 1] */
static uint8_t sal[SAL]; static uint32_t sal_ini, sal_n;
static uint8_t ent_cab[4]; static uint8_t ent[RED_CARGA > 4096 ? RED_CARGA : 4096]; static uint32_t ent_n, ent_largo; static uint8_t ent_en_carga;
static uint8_t rr;                                    /* el turno del reparto de DATOS entre canales */
static red_cuentas_t cuentas;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void pon16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void pon32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static unsigned largo_c(const char *s, unsigned max) { unsigned n = 0; while (s && n < max && s[n]) n++; return n; }
static canal_t *k_(int c) { return c >= 1 && c <= RED_CANALES ? &can[c - 1] : 0; }

/* ---- salida: una trama entera o nada ---- */
static uint32_t sal_libre(void) { return SAL - sal_n; }
static void sal_poner(const void *d, uint32_t n)
{
    const uint8_t *b = d; uint32_t i;
    for (i = 0; i < n; i++) sal[(sal_ini + sal_n + i) % SAL] = b[i];
    sal_n += n;
}
static int trama(uint8_t tipo, uint8_t canal, const void *c1, uint32_t n1, const void *c2, uint32_t n2)
{
    uint8_t h[4];
    if (n1 + n2 > carga || 4 + n1 + n2 > sal_libre()) return 0;
    h[0] = tipo; h[1] = canal; pon16(h + 2, n1 + n2);
    sal_poner(h, 4); if (n1) sal_poner(c1, n1); if (n2) sal_poner(c2, n2);
    cuentas.tramas_tx++;
    return 1;
}
static void sal_vaciar(void)
{
    while (sal_n && tubo) {
        uint32_t seguido = SAL - sal_ini < sal_n ? SAL - sal_ini : sal_n;
        int k = tubo->enviar(sal + sal_ini, seguido);
        if (k <= 0) break;
        sal_ini = (sal_ini + (uint32_t)k) % SAL; sal_n -= (uint32_t)k; cuentas.bytes_tx += (uint32_t)k;
    }
}

/* ---- la sesión ---- */
void red_empezar(const red_tubo_t *t, const char *cliente)
{
    uint8_t h[9 + 24]; unsigned n = largo_c(cliente, 24);
    memset(can, 0, sizeof can); memset(&cuentas, 0, sizeof cuentas);
    tubo = t; sal_ini = sal_n = 0; ent_n = 0; ent_en_carga = 0; rr = 0; puente[0] = 0;
    carga = RED_CARGA; ncanales = RED_CANALES;
    memcpy(h, "W5RED", 5); h[5] = 1; pon16(h + 6, RED_CARGA); h[8] = RED_CANALES;
    if (n) memcpy(h + 9, cliente, n);
    estado = trama(T_HOLA, 0, h, 9 + n, 0, 0) ? RED_SALUDANDO : RED_CAIDA;
    sal_vaciar();
}
void red_parar(void)
{
    int i;
    for (i = 1; i <= RED_CANALES; i++) {
        canal_t *k = &can[i - 1];
        if (k->estado == RED_C_ABRIENDO || k->estado == RED_C_ABIERTO) { k->estado = RED_C_CERRADO; k->codigo = RED_SIN_SESION; memcpy(k->texto, "sin sesion", 11); }
        else if (k->estado == RED_C_CERRANDO) k->estado = RED_C_LIBRE;
    }
    tubo = 0; sal_n = 0; estado = RED_APAGADA;
}
int red_estado(void) { return estado; }
const char *red_nombre_puente(void) { return puente; }
const red_cuentas_t *red_cuentas(void) { return &cuentas; }

/* ---- lo que llega ---- */
static void cerrado_por_puente(canal_t *k, const uint8_t *p, uint32_t n)
{
    if (k->estado == RED_C_CERRANDO) { k->estado = RED_C_LIBRE; return; }   /* la respuesta a nuestro CIERRA: el número queda libre */
    k->estado = RED_C_CERRADO; k->codigo = n ? p[0] : RED_INTERNO;
    { unsigned i = 0; while (i + 1 < n && i < sizeof k->texto - 1 && p[1 + i]) { k->texto[i] = (char)p[1 + i]; i++; } k->texto[i] = 0; }
}
static void tramitar(uint8_t tipo, uint8_t c, const uint8_t *p, uint32_t n)
{
    canal_t *k;
    cuentas.tramas_rx++;
    if (c == 0) {
        if (tipo == T_HOLA_OK && n >= 9 && !memcmp(p, "W5RED", 5) && estado == RED_SALUDANDO) {
            uint16_t cm = le16(p + 6); unsigned i;
            if (cm < carga) carga = cm < 64 ? 64 : cm;
            if (p[8] < ncanales) ncanales = p[8];
            for (i = 0; i + 9 < n && i < sizeof puente - 1; i++) puente[i] = (char)p[9 + i];
            puente[i] = 0;
            estado = RED_LISTA;
        } else if (tipo == T_PING) {
            trama(T_PONG, 0, p, n > 64 ? 64 : n, 0, 0);
        } else if (tipo == T_ADIOS) {
            red_parar(); estado = RED_CAIDA;
        }
        return;
    }
    k = k_(c);
    if (!k || k->estado == RED_C_LIBRE) { cuentas.errores++; return; }
    if (k->estado == RED_C_CERRANDO && tipo != T_CIERRA) return;   /* hasta su CIERRA, lo que llegue de este canal se ignora */
    switch (tipo) {
    case T_LISTO:
        if (n < 4) break;
        k->vent_tx += le32(p);
        if (k->estado == RED_C_ABRIENDO) k->estado = RED_C_ABIERTO;
        if (!k->es_http) { uint32_t i = 0; while (i + 4 < n && i < RED_CAB - 1 && p[4 + i]) { k->cab[i] = (char)p[4 + i]; i++; } k->cab_n = (uint16_t)i; }
        break;
    case T_RESPUESTA: {
        uint32_t i = 2;
        if (n < 3) break;
        k->http = le16(p);
        while (i < n && p[i]) i++;                     /* la url final: se salta */
        i++;
        { uint32_t j = 0; while (i < n && p[i] && j < RED_CAB - 1) k->cab[j++] = (char)p[i++]; k->cab_n = (uint16_t)j; }
        break;
    }
    case T_DATOS: {
        uint32_t i;
        if (n > (uint32_t)(RED_RX - k->rx_n)) { cuentas.ventana_violada++; n = (uint32_t)(RED_RX - k->rx_n); }
        for (i = 0; i < n; i++) k->rx[(k->rx_ini + k->rx_n + i) % RED_RX] = p[i];
        k->rx_n = (uint16_t)(k->rx_n + n);
        k->vent_dada = k->vent_dada > n ? k->vent_dada - n : 0;
        break;
    }
    case T_VENTANA: if (n >= 4) k->vent_tx += le32(p); break;
    case T_FIN: k->fin_rx = 1; break;
    case T_CIERRA: cerrado_por_puente(k, p, n); break;
    default: cuentas.errores++; break;
    }
}
static void entrada(void)
{
    uint8_t b[512]; int n, i, vueltas = 0;
    while (tubo && vueltas++ < 64 && (n = tubo->recibir(b, sizeof b)) > 0) {
        cuentas.bytes_rx += (uint32_t)n;
        for (i = 0; i < n; i++) {
            if (!ent_en_carga) {
                ent_cab[ent_n++] = b[i];
                if (ent_n == 4) {
                    ent_largo = le16(ent_cab + 2); ent_n = 0;
                    if (ent_largo > sizeof ent) { cuentas.errores++; red_parar(); estado = RED_CAIDA; return; }   /* trama imposible: se corta */
                    if (ent_largo) ent_en_carga = 1; else tramitar(ent_cab[0], ent_cab[1], ent, 0);
                }
            } else {
                uint32_t k = (uint32_t)(n - i), falta = ent_largo - ent_n;
                if (k > falta) k = falta;
                memcpy(ent + ent_n, b + i, k); ent_n += k; i += (int)k - 1;
                if (ent_n == ent_largo) { ent_en_carga = 0; ent_n = 0; tramitar(ent_cab[0], ent_cab[1], ent, ent_largo); }
            }
        }
    }
}

/* ---- lo que sale: ventanas, FIN, CIERRA y DATOS repartidos por turno ---- */
static void salida(void)
{
    int i, j;
    for (i = 1; i <= RED_CANALES; i++) {
        canal_t *k = &can[i - 1]; uint8_t b4[4];
        if (k->cierra_pend) { uint8_t cc[2] = { RED_ABORTADA, 0 }; if (trama(T_CIERRA, (uint8_t)i, cc, 2, 0, 0)) k->cierra_pend = 0; continue; }
        if (k->estado != RED_C_ABIERTO && k->estado != RED_C_ABRIENDO) continue;
        {   /* ventana: lo libre de la cola menos lo ya dado; se da a trozos de 1/4 de cola como poco (cada VENTANA es una trama) */
            uint32_t libre = RED_RX - k->rx_n;
            if (!k->fin_rx && libre > k->vent_dada && libre - k->vent_dada >= RED_RX / 4) {
                pon32(b4, libre - k->vent_dada);
                if (trama(T_VENTANA, (uint8_t)i, b4, 4, 0, 0)) k->vent_dada = libre;
            }
        }
    }
    for (j = 0; j < RED_CANALES; j++) {               /* DATOS: una trama por canal y vuelta, mientras quepa */
        int c = 1 + (rr + j) % RED_CANALES; canal_t *k = &can[c - 1]; uint32_t m, seguido;
        if (k->estado != RED_C_ABIERTO) continue;
        while (k->tx_n && k->vent_tx && sal_libre() >= 4 + 64 + RESERVA) {
            m = k->tx_n; if (m > k->vent_tx) m = k->vent_tx; if (m > carga) m = carga;
            if (4 + m + RESERVA > sal_libre()) m = sal_libre() - 4 - RESERVA;
            seguido = RED_TX - k->tx_ini; if (seguido > m) seguido = m;
            if (!trama(T_DATOS, (uint8_t)c, k->tx + k->tx_ini, seguido, k->tx, m - seguido)) break;
            k->tx_ini = (uint16_t)((k->tx_ini + m) % RED_TX); k->tx_n = (uint16_t)(k->tx_n - m); k->vent_tx -= m;
        }
        if (k->fin_pedido && !k->fin_mandado && !k->tx_n && trama(T_FIN, (uint8_t)c, 0, 0, 0, 0)) k->fin_mandado = 1;
    }
    rr = (uint8_t)((rr + 1) % RED_CANALES);
}

void red_paso(void)
{
    if (!tubo || estado == RED_APAGADA) return;
    entrada();
    if (tubo && estado == RED_LISTA) salida();
    sal_vaciar();
}

/* ---- abrir ---- */
static int libre_(void)
{
    int i;
    for (i = 1; i <= ncanales && i <= RED_CANALES; i++) if (can[i - 1].estado == RED_C_LIBRE) return i;
    return RED_E_SIN_CANAL;
}
static void preparar(canal_t *k, int es_http)
{
    memset(k, 0, sizeof *k);
    k->estado = RED_C_ABRIENDO; k->es_http = (uint8_t)es_http; k->vent_dada = RED_RX;
}
int red_http(const char *metodo, const char *url, const char *cabeceras, const void *cuerpo, unsigned n, unsigned banderas, unsigned plazo_s)
{
    uint8_t cab[6]; char m[16]; unsigned lm, lu, lc, i; int c; canal_t *k; int con_cuerpo = n != 0;
    if (estado != RED_LISTA) return RED_E_SIN_SESION;
    lm = largo_c(metodo, sizeof m - 1); lu = largo_c(url, 4096); lc = largo_c(cabeceras, 4096);
    if (!lu || (n != RED_CUERPO_A_TROZOS && n > RED_TX)) return RED_E_ARG;
    for (i = 0; i < lm; i++) m[i] = metodo[i];
    if (!lm) { memcpy(m, "GET", 3); lm = 3; }
    m[lm] = 0;
    if (6 + lm + 1 + lu + 1 + lc + 1 > carga) return RED_E_ARG;              /* url + cabeceras no caben en una trama */
    if ((c = libre_()) < 0) return c;
    if (sal_libre() < 4 + 6 + lm + lu + lc + 3 + RESERVA) return RED_E_OCUPADO;
    k = &can[c - 1]; preparar(k, 1);
    pon32(cab, RED_RX); cab[4] = (uint8_t)((banderas & (RED_B_SIN_REDIR | RED_B_SIN_VERIFICAR)) | (con_cuerpo ? 1 : 0)); cab[5] = (uint8_t)(plazo_s > 255 ? 255 : plazo_s);
    {   /* la trama a mano: cabecera fija y las tres cadenas */
        uint8_t h[4]; uint8_t cero = 0;
        h[0] = T_ABRE_HTTP; h[1] = (uint8_t)c; pon16(h + 2, 6 + lm + 1 + lu + 1 + lc + 1);
        sal_poner(h, 4); sal_poner(cab, 6); sal_poner(m, lm); sal_poner(&cero, 1); sal_poner(url, lu); sal_poner(&cero, 1);
        if (lc) sal_poner(cabeceras, lc);
        sal_poner(&cero, 1); cuentas.tramas_tx++;
    }
    if (con_cuerpo && n != RED_CUERPO_A_TROZOS) {     /* entero: a la cola, y el FIN detrás */
        if (n) { memcpy(k->tx, cuerpo, n); k->tx_n = (uint16_t)n; }
        k->fin_pedido = 1;
    }
    sal_vaciar();
    return c;
}
int red_tcp(const char *host, unsigned puerto, unsigned banderas, unsigned plazo_s)
{
    uint8_t cab[8], h[4], cero = 0; unsigned lh; int c; canal_t *k;
    if (estado != RED_LISTA) return RED_E_SIN_SESION;
    lh = largo_c(host, 255);
    if (!lh || !puerto || puerto > 65535 || 8 + lh + 1 > carga) return RED_E_ARG;
    if ((c = libre_()) < 0) return c;
    if (sal_libre() < 4 + 8 + lh + 1 + RESERVA) return RED_E_OCUPADO;
    k = &can[c - 1]; preparar(k, 0);
    pon32(cab, RED_RX); cab[4] = (uint8_t)(banderas & (RED_B_TLS | RED_B_SIN_VERIFICAR)); cab[5] = (uint8_t)(plazo_s > 255 ? 255 : plazo_s); pon16(cab + 6, puerto);
    h[0] = T_ABRE_TCP; h[1] = (uint8_t)c; pon16(h + 2, 8 + lh + 1);
    sal_poner(h, 4); sal_poner(cab, 8); sal_poner(host, lh); sal_poner(&cero, 1); cuentas.tramas_tx++;
    sal_vaciar();
    return c;
}

/* ---- el canal visto por la app ---- */
int red_canal(int c) { canal_t *k = k_(c); return k ? k->estado : RED_C_LIBRE; }
int red_codigo(int c) { canal_t *k = k_(c); return k ? k->codigo : RED_INTERNO; }
const char *red_texto(int c) { canal_t *k = k_(c); return k ? k->texto : ""; }
int red_http_estado(int c) { canal_t *k = k_(c); return k ? k->http : 0; }
int red_cabeceras(int c, char *d, unsigned max)
{
    canal_t *k = k_(c); unsigned n;
    if (!k || !d || !max) return 0;
    n = k->cab_n < max - 1 ? k->cab_n : max - 1;
    memcpy(d, k->cab, n); d[n] = 0;
    return (int)n;
}
int red_fin_recibido(int c) { canal_t *k = k_(c); return k ? (k->fin_rx || k->estado == RED_C_CERRADO) : 1; }
int red_disponibles(int c) { canal_t *k = k_(c); return k ? k->rx_n : 0; }
int red_recibir(int c, void *d, unsigned max)
{
    canal_t *k = k_(c); uint8_t *b = d; unsigned i;
    if (!k || !b || k->estado == RED_C_LIBRE || k->estado == RED_C_CERRANDO) return 0;
    for (i = 0; i < max && k->rx_n; i++) { b[i] = k->rx[k->rx_ini]; k->rx_ini = (uint16_t)((k->rx_ini + 1) % RED_RX); k->rx_n--; }
    return (int)i;
}
int red_enviar(int c, const void *d, unsigned n)
{
    canal_t *k = k_(c); const uint8_t *b = d; unsigned i;
    if (!k || !b || (k->estado != RED_C_ABIERTO && k->estado != RED_C_ABRIENDO) || k->fin_pedido) return 0;
    for (i = 0; i < n && k->tx_n < RED_TX; i++) { k->tx[(k->tx_ini + k->tx_n) % RED_TX] = b[i]; k->tx_n++; }
    return (int)i;
}
int red_pendiente(int c) { canal_t *k = k_(c); return k ? k->tx_n : 0; }
int red_terminar(int c)
{
    canal_t *k = k_(c);
    if (!k || (k->estado != RED_C_ABIERTO && k->estado != RED_C_ABRIENDO)) return -1;
    k->fin_pedido = 1;
    return 0;
}
void red_cerrar(int c)
{
    canal_t *k = k_(c);
    if (!k) return;
    if (k->estado == RED_C_CERRADO) { k->estado = RED_C_LIBRE; return; }
    if (k->estado == RED_C_ABRIENDO || k->estado == RED_C_ABIERTO) { k->estado = RED_C_CERRANDO; k->cierra_pend = 1; k->rx_n = k->tx_n = 0; }
}
