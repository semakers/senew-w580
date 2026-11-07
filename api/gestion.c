/* gestion.c — el FORMATO del canal de gestión de SENUS (api/gestion.h): cada dataset del contrato el contrato del canal de gestión (docs/ del repositorio de SENUS),
 * su validación y la cola de cambios. Portable: lo enlazan el firmware y el banco de PC (herr/pruebas/mtp/prueba_mtp.c); no ve shell/.
 * Todo es little-endian y se escribe byte a byte: nada de estructuras con huecos en el cable. */
#include "gestion.h"
#include "usb.h"

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);

#define TXT_(x) #x
#define TXT(x) TXT_(x)
static void pon16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void pon32(u8 *p, u32 v) { pon16(p, v); pon16(p + 2, v >> 16); }
static u32 lee16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 lee32(const u8 *p) { return lee16(p) | lee16(p + 2) << 16; }
static u32 largo(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void cad(u8 *d, u32 cap, const char *s) { u32 i; for (i = 0; s[i] && i + 1 < cap; i++) d[i] = (u8)s[i]; }   /* d ya a cero: queda su NUL */

/* ---- la cola: lo que el manejador aceptó y el hilo de la interfaz aún no ha aplicado ------------------------------------------------ */
static struct {
    u32 n; GsCambio e[GS_COLA_MAX];
    u8  hay_txt; char txt[GS_TEXTO_MAX + 1];
    u8  hay_reloj; u32 utc; short zona;
    u8  hay_bt; u8 bt_addr[6]; char bt_pin[GS_PIN_MAX + 1];
} q;
int gs_cola_hay(void) { return q.n || q.hay_txt || q.hay_reloj || q.hay_bt; }
const GsCambio *gs_cola(u32 *n) { *n = q.n; return q.e; }
const char *gs_cola_texto(void) { return q.hay_txt ? q.txt : 0; }
int gs_cola_reloj(u32 *utc, short *zona) { if (q.hay_reloj) { *utc = q.utc; *zona = q.zona; } return q.hay_reloj; }
int gs_cola_bt(u8 addr[6], char pin[GS_PIN_MAX + 1]) { if (q.hay_bt) { memcpy(addr, q.bt_addr, 6); memcpy(pin, q.bt_pin, sizeof q.bt_pin); } return q.hay_bt; }
void gs_cola_vaciar(void) { q.n = 0; q.hay_txt = 0; q.hay_reloj = 0; q.hay_bt = 0; memset(q.bt_pin, 0, sizeof q.bt_pin); }   /* el PIN no se queda en la RAM */
static int cola_poner(u16 clave, short v)                  /* la misma clave dos veces: vale la última */
{
    u32 i;
    for (i = 0; i < q.n; i++) if (q.e[i].clave == clave) { q.e[i].v = v; return 1; }
    if (q.n >= GS_COLA_MAX) return 0;
    q.e[q.n].clave = clave; q.e[q.n].v = v; q.n++;
    return 1;
}

/* ---- la identidad -------------------------------------------------------------------------------------------------------------------- */
static char id_vinc[GS_ID_TAM];
static char serie[GS_SERIE_TAM];
const char *gs_id(void) { return id_vinc; }
/* EL id NO VA EN CLARO (contrato §7): la serie son 24 hexadecimales de tres pasadas de FNV-1a de 32 bits sobre los dígitos, cada una con
 * otra semilla. No es criptografía: un IMEI tiene pocos bits y se saca por fuerza bruta con cualquier hash. Sirve para que la serie no sea el IMEI
 * y para que dos teléfonos no compartan serie. */
static u32 fnv(u32 h, const char *s) { while (*s) { h ^= (u8)*s++; h *= 16777619u; } return h; }
static void serie_hacer(void)
{
    static const char hx[] = "0123456789ABCDEF";
    u32 k, j, l = 0;
    if (!id_vinc[0]) { serie[0] = 0; gs_sis_serie(""); return; }
    for (; USB_TEXTO_MODELO[l]; l++) serie[l] = USB_TEXTO_MODELO[l];
    serie[l++] = 'S'; serie[l++] = '1';
    for (k = 0; k < 3; k++) {
        u32 h = fnv(2166136261u ^ (0x9E3779B1u * (k + 1u)), id_vinc);
        h ^= h >> 15; h *= 0x2C1B3C6Du;  h ^= h >> 13;      /* las dos últimas cifras del IMEI cambian todo el texto, no solo el final */
        for (j = 0; j < 8; j++) serie[l++] = hx[(h >> (28 - 4 * j)) & 15];
    }
    serie[l] = 0;
    gs_sis_serie(serie);
}
const char *gs_serie_mtp(void) { return serie[0] ? serie : USB_TEXTO_MODELO "S10000000000000000000000001"; }   /* sin vincular: la fija de siempre */
/* 14 o 15 dígitos, NUL y ceros hasta el final; lo demás no es un id */
static int id_vale(const u8 *d)
{
    u32 n = 0, i;
    while (n < GS_ID_TAM && d[n] >= '0' && d[n] <= '9') n++;
    if (n != 14 && n != 15) return 0;
    for (i = n; i < GS_ID_TAM; i++) if (d[i]) return 0;
    return 1;
}
void gs_id_cargar(void)
{
    char t[GS_ID_TAM];
    if (gs_sis_id_leer(t) && id_vale((const u8 *)t)) memcpy(id_vinc, t, sizeof id_vinc); else id_vinc[0] = 0;
    serie_hacer();
}

/* ---- SEN_INFO (contrato §2) -------------------------------------------------------------------------------------------------------- */
u32 gs_op_info(u8 *d)
{
    GsInfo i; u32 k; u8 b = 0; char m[8];
    memset(d, 0, GS_INFO_TAM); memset(&i, 0, sizeof i);
    gs_sis_info(&i);
    pon16(d, GS_FORMATO); pon16(d + 2, GS_INFO_TAM); pon32(d + 4, SENEW_BUILD);
    pon16(d + 8, i.api_mayor); pon16(d + 10, i.api_menor); pon16(d + 12, i.bl_version);
    d[14] = i.bateria_pct;
    if (i.tarjeta) b |= 1;
    if (i.cargando) b |= 2;
#ifdef PUENTE
    b |= 4;
#endif
#ifdef ARRANQUE_VIGILADO
    b |= 8;
#endif
    if (id_vinc[0]) b |= 16;
    d[15] = b;
    memcpy(d + 16, i.bd_addr, 6);
    cad(d + 24, 16, SENEW_VERSION);
    for (k = 0; k < sizeof m - 1 && USB_TEXTO_MODELO[k]; k++) m[k] = USB_TEXTO_MODELO[k] >= 'A' && USB_TEXTO_MODELO[k] <= 'Z' ? (char)(USB_TEXTO_MODELO[k] + 32) : USB_TEXTO_MODELO[k];
    m[k] = 0;
    cad(d + 40, 8, m);
    cad(d + 48, 8, i.soc ? i.soc : "");
    cad(d + 56, 24, __DATE__ " " __TIME__);
    cad(d + 80, GS_ID_TAM, id_vinc);
    pon32(d + 96, i.ajustes_version);
    return GS_INFO_TAM;
}

/* ---- ajustes (contrato §3) --------------------------------------------------------------------------------------------------------- */
static u32 largo_de(u8 tipo) { return tipo == GS_T_I16 ? 2u : 1u; }
int gs_op_aj_leer(u8 *d, u32 cap, u32 *n)
{
    u32 nc, i, k = 4, hechas = 0;
    const GsClave *c = gs_sis_claves(&nc);
    if (gs_cola_hay()) return GS_R_OCUPADO;
    for (i = 0; i < nc; i++) {
        short v = 0; const char *t = 0; u32 l;
        if (gs_sis_valor(c[i].clave, &v, &t)) continue;
        l = c[i].tipo == GS_T_TEXTO ? largo(t ? t : "") : largo_de(c[i].tipo);
        if (k + 8 + l > cap) break;                      /* no pasa: 26 claves son ~340 B y `cap` es 1024 */
        pon16(d + k, c[i].clave); d[k + 2] = c[i].tipo; d[k + 3] = (u8)l;
        pon16(d + k + 4, (u16)c[i].min); pon16(d + k + 6, (u16)c[i].max);
        if (c[i].tipo == GS_T_TEXTO) { if (l) memcpy(d + k + 8, t, l); }
        else if (c[i].tipo == GS_T_I16) pon16(d + k + 8, (u16)v);
        else d[k + 8] = (u8)v;
        k += 8 + l; hechas++;
    }
    pon16(d, GS_FORMATO); pon16(d + 2, hechas);
    *n = k;
    return GS_R_OK;
}
static const GsClave *clave_de(u16 clave)
{
    u32 n, i; const GsClave *c = gs_sis_claves(&n);
    for (i = 0; i < n; i++) if (c[i].clave == clave) return &c[i];
    return 0;
}
/* Una entrada ya enmarcada: a la cola si vale. 0 = aplicada, 1 = fuera de rango, 2 = desconocida. */
static int entrada(u16 clave, u8 tipo, u8 l, const u8 *v, char *txt)
{
    const GsClave *c = clave_de(clave);
    int x;
    u32 i;
    if (!c) return 2;
    if (tipo != c->tipo) return 1;
    if (tipo == GS_T_TEXTO) {
        if (l > c->max || l > GS_TEXTO_MAX) return 1;
        for (i = 0; i < l; i++) { if (v[i] < 0x20 || v[i] == 0x7F) return 1; txt[i] = (char)v[i]; }
        txt[l] = 0;
        if (!gs_sis_texto_vale(clave, txt)) return 1;
        memcpy(q.txt, txt, l + 1u); q.hay_txt = 1;
        return 0;
    }
    if (l != largo_de(tipo)) return 1;
    x = tipo == GS_T_I16 ? (int)(short)lee16(v) : (int)v[0];
    if (x < c->min || x > c->max) return 1;
    return cola_poner(clave, (short)x) ? 0 : 1;
}
int gs_op_aj_poner(const u8 *d, u32 n, int rebosa, u32 par[3])
{
    u32 ne, i, k;
    char txt[GS_TEXTO_MAX + 1];
    par[0] = par[1] = par[2] = 0;
    if (rebosa || n < 4 || lee16(d) != GS_FORMATO) return GS_R_PARAM_MAL;
    ne = lee16(d + 2);
    /* PRIMERO el marco entero: «dataset truncado: R_PARAM_MAL y no se aplica nada». Lo que sobre detrás de la última entrada, también. */
    for (i = 0, k = 4; i < ne; i++) {
        if (n - k < 4) return GS_R_PARAM_MAL;
        if (n - k - 4 < d[k + 3]) return GS_R_PARAM_MAL;
        k += 4u + d[k + 3];
    }
    if (k != n) return GS_R_PARAM_MAL;
    if (gs_cola_hay()) return GS_R_OCUPADO;
    for (i = 0, k = 4; i < ne; i++) {
        int r = entrada((u16)lee16(d + k), d[k + 2], d[k + 3], d + k + 4, txt);
        par[r]++;
        k += 4u + d[k + 3];
    }
    return GS_R_OK;
}

/* ---- reloj y vínculo (contrato §4) ------------------------------------------------------------------------------------------------- */
#define UNIX_2000 946684800u
#define UNIX_2100 4102444800u
void gs_fecha_de_unix(u32 utc, HwFecha *f)
{
    u32 dias = utc / 86400u, s = utc % 86400u, z = dias + 719468u, era = z / 146097u, doe = z - era * 146097u;
    u32 yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u, doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    u32 mp = (5u * doy + 2u) / 153u, mes = mp < 10u ? mp + 3u : mp - 9u;
    f->anio = (u16)(yoe + era * 400u + (mes <= 2u));
    f->mes = (u8)mes; f->dia = (u8)(doy - (153u * mp + 2u) / 5u + 1u);
    f->hora = (u8)(s / 3600u); f->min = (u8)(s / 60u % 60u); f->seg = (u8)(s % 60u);
}
int gs_op_reloj(const u8 *d, u32 n, int rebosa)
{
    u32 utc; int z;
    if (rebosa || n != 8) return GS_R_PARAM_MAL;
    utc = lee32(d); z = (int)(short)lee16(d + 4);
    if (utc < UNIX_2000 || utc >= UNIX_2100 || z < -720 || z > 840) return GS_R_PARAM_MAL;
    if (gs_cola_hay()) return GS_R_OCUPADO;
    q.utc = utc; q.zona = (short)z; q.hay_reloj = 1;
    return GS_R_OK;
}
int gs_op_vincular(const u8 *d, u32 n, int rebosa)
{
    if (rebosa || n != GS_ID_TAM || !id_vale(d)) return GS_R_PARAM_MAL;
    if (gs_sis_id_guardar((const char *)d) != HW_OK) return GS_R_GENERAL;
    memcpy(id_vinc, d, sizeof id_vinc);
    serie_hacer();
    return GS_R_OK;
}

/* ---- Bluetooth, BL y apps ---------------------------------------------------------------------------------------------------------- */
u32 gs_op_bt(u8 *d)
{
    GsBt b;
    memset(&b, 0, sizeof b); memset(d, 0, GS_BT_TAM);
    gs_sis_bt(&b);
    /* Aceptado y sin aplicar: lo que hubiera antes ya no vale. SENUS sondea hasta red_lista = 1 o fallo != 0, y un fallo viejo no puede ser la respuesta a esta orden */
    if (q.hay_bt) { b.red_lista = 0; b.fallo = 0; memcpy(b.puente_addr, q.bt_addr, 6); }
    d[0] = b.encendido; d[1] = b.puente_conectado; d[2] = b.red_lista; d[3] = b.fallo;
    memcpy(d + 4, b.puente_addr, 6);
    return GS_BT_TAM;
}
/* SEN_BT_EMPAREJAR (contrato §4): addr[6], pin_n, pin[16], reserva. Solo valida y encola: guardar y llamar es de gs_paso. El PIN no se escribe en ningún registro. */
int gs_op_bt_emparejar(const u8 *d, u32 n, int rebosa)
{
    u32 i, pn, cero = 0, unos = 0;
    int r = gs_sis_bt_puede();
    if (r == GS_R_OP_NO) return r;
    if (rebosa || n != GS_EMP_TAM || d[GS_EMP_TAM - 1]) return GS_R_PARAM_MAL;
    for (i = 0; i < 6; i++) { cero += !d[i]; unos += d[i] == 0xFF; }
    if (cero == 6 || unos == 6) return GS_R_PARAM_MAL;   /* 00:00:… y FF:FF:… no son una dirección */
    pn = d[6];
    if (pn < 1 || pn > GS_PIN_MAX) return GS_R_PARAM_MAL;
    for (i = 0; i < GS_PIN_MAX; i++) {
        u8 c = d[7 + i];
        if (i < pn ? c < 0x20 || c > 0x7E : c != 0) return GS_R_PARAM_MAL;
    }
    if (r != GS_R_OK || gs_cola_hay()) return GS_R_OCUPADO;
    memcpy(q.bt_addr, d, 6); memcpy(q.bt_pin, d + 7, GS_PIN_MAX); q.bt_pin[GS_PIN_MAX] = 0; q.hay_bt = 1;
    return GS_R_OK;
}
int gs_op_entrar_bl(void) { return gs_cola_hay() ? GS_R_OCUPADO : gs_sis_bl(); }   /* lo aceptado se aplica antes de irse */
int gs_op_apps(u32 desde, u8 *d, u32 *n)
{
    u32 total = 0, k = 0;
    const u8 *e = 0;
    if (gs_sis_apps(desde, &total, &e)) return GS_R_OCUPADO;
    while (desde + k < total && k < GS_APPS_PAG) { memcpy(d + GS_APPS_CAB + k * GS_APP_TAM, e + (desde + k) * GS_APP_TAM, GS_APP_TAM); k++; }
    pon16(d, GS_FORMATO); pon16(d + 2, total); pon16(d + 4, k); pon16(d + 6, GS_APP_TAM);
    *n = GS_APPS_CAB + k * GS_APP_TAM;
    return GS_R_OK;
}

/* ---- la vida del canal ------------------------------------------------------------------------------------------------------------- */
void gs_iniciar(void) { gs_sis_sesion_nueva(); }
void gs_parar(void) { gs_sis_parar(); }
