/* descarga_red.c — (1.11, 28-oct) una canción de la red a la TARJETA (descarga_red.h). La URL es la de la cola de la música por la red (shell/musica_red.c:
 * en «YT Music», un MP3 de 128 kbps del servicio de música del computador). Se baja por SU canal de la red (un dueño propio, SH_BT_DESCARGA: si la música se para, la
 * descarga sigue) mientras la canción suena por otro, y se escribe a trozos con la recepción de la tarjeta (dueño HW_RX_RED): el fichero NO existe hasta
 * cerrarlo, así que una descarga cortada no deja nada a medias. Al acabar, la Música rehace su lista (la carpeta /tarjeta/music ya la mira). */
#include "shell.h"
#include "bt_sis.h"
#include "red_sis.h"
#include "descarga_red.h"
#include "tarjeta.h"
#include "../bt/red/red.h"

#define D_URL 200
#define D_TOPE_RED_MS 40000u                    /* conectar la red (con la radio apagada, 15-20 s) */
#define D_POR_PASO 16384u                       /* lo más que se escribe en la tarjeta por vuelta del bucle (que la interfaz no se trabe) */
static struct { u8 fase, ya, rx, red; int canal, http; u32 bajados, total, esperado, t0; char url[D_URL]; char ruta[8 + DESC_NOMBRE]; char nombre[DESC_NOMBRE]; char porque[48]; } D;
static u8 tmp[4096];

int sh_desc_hay(void) { return sh_red_hay(); }
static void copiar(char *d, const char *s, u32 cap) { u32 i; for (i = 0; s && s[i] && i + 1 < cap; i++) d[i] = s[i]; d[i] = 0; }
static void soltar(void)
{
    if (D.canal > 0) { red_cerrar(D.canal); sh_red_canal_poner(D.canal, SH_BT_NADIE); D.canal = 0; }
    if (D.rx) { hw_tj_rx_abortar_d(HW_RX_RED); D.rx = 0; }
    if (D.red) { sh_red_soltar(SH_BT_DESCARGA); sh_bt_soltar(SH_BT_DESCARGA); D.red = 0; }
}
static void fallar(const char *p, const char *q)
{
    copiar(D.porque, p, sizeof D.porque);
    if (q) { u32 n = (u32)strlen(D.porque); copiar(D.porque + n, q, sizeof D.porque - n); }
    soltar(); D.fase = DESC_FALLO;
    hw_log("descarga: FALLO: "); hw_log(D.porque); hw_log("\n");
}
/* «Artista - Título.mp3»: sin los caracteres que FAT no admite, recortado a lo que cabe sin partir un carácter UTF-8 */
static void nombre_de(const char *titulo, const char *artista)
{
    char b[DESC_NOMBRE + 8];
    u32 n = 0, i, max = DESC_NOMBRE - 5;                 /* deja sitio a «.mp3» y al 0 */
    const char *partes[3]; int k;
    partes[0] = artista && artista[0] ? artista : 0; partes[1] = partes[0] ? " - " : 0; partes[2] = titulo && titulo[0] ? titulo : "cancion";
    for (k = 0; k < 3; k++)
        for (i = 0; partes[k] && partes[k][i] && n < max; i++) {
            char c = partes[k][i];
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (u8)c < 32) c = '_';
            b[n++] = c;
        }
    while (n && ((u8)b[n - 1] & 0xC0) == 0xC0) n--;      /* el último carácter, si quedó partido */
    if (n && ((u8)b[n - 1] & 0x80)) { u32 j = n; while (j && ((u8)b[j - 1] & 0xC0) == 0x80) j--; if (j && ((u8)b[j - 1] & 0xC0) == 0xC0) {
        u32 lon = ((u8)b[j - 1] & 0xE0) == 0xC0 ? 2u : ((u8)b[j - 1] & 0xF0) == 0xE0 ? 3u : 4u; if (n - (j - 1) < lon) n = j - 1; } }
    while (n && (b[n - 1] == ' ' || b[n - 1] == '.')) n--;   /* FAT: sin espacio ni punto al final */
    memcpy(b + n, ".mp3", 5);
    copiar(D.nombre, b, sizeof D.nombre);
    copiar(D.ruta, "/music/", sizeof D.ruta); copiar(D.ruta + 7, D.nombre, sizeof D.ruta - 7);
}

int sh_desc_empezar(const char *url, const char *titulo, const char *artista, u32 dur_ms)
{
    char abs_[8 + 8 + DESC_NOMBRE];
    if (D.fase != DESC_QUIETA && D.fase != DESC_HECHA && D.fase != DESC_FALLO) return HW_ERROR;   /* una a la vez */
    memset(&D, 0, sizeof D);
    if (!url || !url[0] || strlen(url) >= D_URL) return HW_ERROR;
    if (!hw_tj_escribible()) { copiar(D.porque, "no hay tarjeta donde guardarla", sizeof D.porque); D.fase = DESC_FALLO; return HW_NO_DISPONIBLE; }
    copiar(D.url, url, sizeof D.url);
    nombre_de(titulo, artista);
    D.esperado = dur_ms / 1000u * 16000u;               /* 128 kbps: lo que medirá si el servidor no da Content-Length */
    copiar(abs_, "/tarjeta", sizeof abs_); copiar(abs_ + 8, D.ruta, sizeof abs_ - 8);
    if (hw_fs_tam(abs_)) { D.fase = DESC_HECHA; D.ya = 1; D.bajados = D.total = hw_fs_tam(abs_); hw_log("descarga: ya estaba\n"); return HW_OK; }
    hw_tj_mkdir("/music");
    if (sh_red_conectar(0, SH_BT_DESCARGA)) { fallar("sin red con el computador: ", sh_red_fallo_nombre(sh_red_fallo())); return HW_ERROR; }
    D.red = 1; D.fase = DESC_CONECTANDO; D.t0 = hw_ms();
    hw_log("descarga: empieza "); hw_log(D.nombre); hw_log("\n");
    return HW_OK;
}
void sh_desc_cancelar(void) { if (D.fase != DESC_QUIETA && D.fase != DESC_HECHA && D.fase != DESC_FALLO) { fallar("cancelada", 0); } }
void sh_desc_info(DescInfo *i)
{
    memset(i, 0, sizeof *i);
    i->fase = D.fase; i->ya_estaba = D.ya; i->http = D.http; i->bajados = D.bajados; i->total = D.total; i->esperado = D.esperado;
    copiar(i->nombre, D.nombre, sizeof i->nombre); copiar(i->porque, D.porque, sizeof i->porque);
}

void sh_desc_paso(void)
{
    u32 escrito = 0;
    switch (D.fase) {
    case DESC_CONECTANDO:
        if (sh_red_estado() == SH_RED_LISTA) D.fase = DESC_PIDIENDO;
        else if (sh_red_estado() == SH_RED_FALLO) fallar("sin red con el computador: ", sh_red_fallo_nombre(sh_red_fallo()));
        else if (hw_ms() - D.t0 > D_TOPE_RED_MS) fallar("la red con el computador no llega", 0);
        break;
    case DESC_PIDIENDO: {
        int c = red_http("GET", D.url, 0, 0, 0, 0, 30);
        if (c > 0) {
            D.canal = c; sh_red_canal_poner(c, SH_BT_DESCARGA);
            if (hw_tj_rx_abrir_d(D.ruta, HW_RX_RED) != HW_OK) { fallar("la tarjeta está ocupada o llena", 0); break; }
            D.rx = 1; D.fase = DESC_RESPUESTA;
        } else if (c == RED_E_SIN_SESION) fallar("la red se cayó", 0);
        break; }                                        /* OCUPADO / SIN_CANAL: en la vuelta siguiente */
    case DESC_RESPUESTA: {
        static char cab[RED_CAB];
        const char *p;
        D.http = red_http_estado(D.canal);
        if (!D.http) { if (red_canal(D.canal) == RED_C_CERRADO) fallar("sin respuesta: ", red_texto(D.canal)); break; }
        if (D.http != 200) { char t[8]; t[0] = (char)('0' + D.http / 100 % 10); t[1] = (char)('0' + D.http / 10 % 10); t[2] = (char)('0' + D.http % 10); t[3] = 0; fallar("el servidor dice HTTP ", t); break; }
        red_cabeceras(D.canal, cab, sizeof cab);
        for (p = cab; *p; ) {                           /* Content-Length (puede no venir: w580-musica lo manda por una tubería) */
            if ((p[0] | 32) == 'c' && !memcmp(p + 1, "ontent-", 7) && ((p[8] | 32) == 'l') && !memcmp(p + 9, "ength:", 6)) {
                const char *q = p + 15; u32 v = 0; while (*q == ' ') q++; while (*q >= '0' && *q <= '9') v = v * 10u + (u32)(*q++ - '0'); D.total = v; break; }
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
        D.fase = DESC_BAJANDO;
        break; }
    case DESC_BAJANDO:
        while (escrito < D_POR_PASO) {
            int k = red_recibir(D.canal, tmp, sizeof tmp);
            if (k <= 0) break;
            if (hw_tj_rx_mas_d(HW_RX_RED, tmp, (u32)k) != HW_OK) { fallar("no se pudo escribir en la tarjeta (¿llena?)", 0); return; }
            D.bajados += (u32)k; escrito += (u32)k;
        }
        if (red_canal(D.canal) == RED_C_CERRADO && !red_disponibles(D.canal)) {
            if (red_codigo(D.canal) == RED_BIEN && D.bajados > 0 && (!D.total || D.bajados >= D.total)) {
                int r = hw_tj_rx_cerrar_d(HW_RX_RED);
                D.rx = 0;
                if (r != HW_OK) { fallar("no se pudo cerrar el fichero en la tarjeta", 0); return; }
                soltar();
                D.fase = DESC_HECHA;
                hw_log(ui_fmt("descarga: HECHA %s (%u B en %u ms)\n", D.nombre, D.bajados, hw_ms() - D.t0));
                sh_musica_refrescar();                  /* que la Música la vea ya */
            } else fallar("se cortó: ", red_texto(D.canal));
        } else if (sh_red_estado() != SH_RED_LISTA) fallar("la red se cayó", 0);
        break;
    default: break;
    }
}
