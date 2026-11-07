/* serie_rx.c — (SER) el motor de «Recibir por serie». Protocolo y reglas en serie_rx.h y docs/recibir_por_serie.md.
 * Portable: ni hardware ni libc (memcpy y memset, como el resto de api/). Lo que garantiza:
 *  - nada se escribe sin una cabecera válida, y la cabecera solo se atiende con la pantalla abierta (quien llama a srx_meter);
 *  - el fichero va a un TEMPORAL (SRX_TEMPORAL) que solo existe tras «cerrar» y que se RENOMBRA al nombre bueno, atómicamente,
 *    cuando han llegado exactamente los bytes declarados y su CRC32 coincide. Un corte en cualquier punto deja el nombre bueno
 *    como estaba (o el viejo, si se reemplazaba);
 *  - rutas: solo «/<carpeta>/<nombre>» con carpeta de la lista blanca, nombre de [A-Za-z0-9._-], sin «..» y sin empezar por punto;
 *  - el destino existente se reemplaza solo si la cabecera lo pide (marca 1);
 *  - tamaño: <= SRX_TAM_MAX y <= el espacio libre, comprobado ANTES de escribir nada; y nunca se aceptan más bytes de los declarados. */
#include "serie_rx.h"

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);
void *memmove(void *d, const void *s, __SIZE_TYPE__ n);

static const char *const carpetas[] = { "apps", "musica", "imagenes", "docs", "fondos" };
#define N_CARPETAS ((int)(sizeof carpetas / sizeof carpetas[0]))

uint32_t srx_crc32(uint32_t crc, const void *d, uint32_t n)
{
    const uint8_t *p = d; int k;
    crc = ~crc;
    while (n--) { crc ^= *p++; for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
    return ~crc;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void pon32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t largo(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int igual_n(const char *a, const char *b, uint32_t n) { while (n--) if (*a++ != *b++) return 0; return 1; }

const char *srx_texto_error(int e)
{
    switch (e) {
    case SRX_E_NADA: return "bien";
    case SRX_E_NOMBRE: return "ruta no admitida";
    case SRX_E_EXISTE: return "el destino ya existe (no se pidió reemplazar)";
    case SRX_E_GRANDE: return "demasiado grande";
    case SRX_E_LLENO: return "no cabe en la Memoria";
    case SRX_E_TAMANO: return "el tamaño no cuadra con lo declarado";
    case SRX_E_CRC: return "el CRC del fichero no coincide";
    case SRX_E_ESCRIBIR: return "fallo al escribir en la Memoria";
    case SRX_E_PLAZO: return "sin datos: cable cortado o PC parado";
    case SRX_E_CANCELADO: return "cancelado en el teléfono";
    case SRX_E_ORDEN: return "trama fuera de lugar";
    case SRX_E_ABANDONO: return "el PC abandonó";
    default: return "error";
    }
}

/* «/carpeta/nombre»: carpeta de la lista; nombre 1..63 de [A-Za-z0-9._-], que no empiece por punto (ni «.», ni «..»,
 * ni ocultos como el temporal). Una sola barra en medio: ni subcarpetas ni «//». */
int srx_ruta_valida(const char *r)
{
    uint32_t n = largo(r), i, c, b;
    if (n < 4 || n >= SRX_RUTA_MAX || r[0] != '/') return 0;
    for (b = 1; b < n && r[b] != '/'; b++) ;
    if (b >= n - 1) return 0;
    for (c = 0; c < (uint32_t)N_CARPETAS; c++) if (largo(carpetas[c]) == b - 1 && igual_n(r + 1, carpetas[c], b - 1)) break;
    if (c == (uint32_t)N_CARPETAS) return 0;
    if (n - b - 1 > 63 || r[b + 1] == '.') return 0;
    for (i = b + 1; i < n; i++) {
        char ch = r[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')) return 0;
        if (ch == '.' && r[i + 1] == '.') return 0;
    }
    return 1;
}

static void responder(Srx *s, uint8_t tipo, const uint8_t *carga, uint32_t n)
{
    uint8_t *p;
    if (s->n_sal + 9 + n > sizeof s->sal) return;          /* quien llama no las ha sacado: se pierde una respuesta y el PC repite */
    p = s->sal + s->n_sal;
    p[0] = 'W'; p[1] = '5'; p[2] = tipo; p[3] = (uint8_t)n; p[4] = (uint8_t)(n >> 8);
    if (n) memcpy(p + 5, carga, n);
    pon32(p + 5 + n, srx_crc32(0, p + 2, 3 + n));
    s->n_sal += 9 + n;
}
static void bien(Srx *s, uint8_t tipo) { uint8_t c[4]; pon32(c, s->hecho); responder(s, tipo, c, 4); }
static void negar(Srx *s, int e, const char *extra)
{
    uint8_t c[1 + sizeof s->texto]; uint32_t n = 0, i;
    const char *t = srx_texto_error(e);
    if (s->abierto) { s->al->abortar(s->al->ctx); s->abierto = 0; }
    s->estado = SRX_FALLO; s->error = (uint8_t)e;
    for (i = 0; t[i] && n < sizeof s->texto - 1; i++) s->texto[n++] = t[i];
    if (extra) for (i = 0; extra[i] && n < sizeof s->texto - 1; i++) s->texto[n++] = extra[i];
    s->texto[n] = 0;
    c[0] = (uint8_t)e; memcpy(c + 1, s->texto, n);
    responder(s, 'E', c, 1 + n);
}

void srx_iniciar(Srx *s, const SrxAlmacen *al, uint32_t ahora_ms)
{
    memset(s, 0, sizeof *s);
    s->al = al; s->ultimo_ms = ahora_ms; s->estado = SRX_ESPERA;
}

static void cabecera(Srx *s, const uint8_t *c, uint32_t n)
{
    uint32_t total, crc, libre, nr; int es_dir = 0; char dir[SRX_RUTA_MAX]; uint32_t b;
    if (n < 9 || n - 9 >= SRX_RUTA_MAX) { negar(s, SRX_E_NOMBRE, 0); return; }
    total = le32(c); crc = le32(c + 4); nr = n - 9;
    if (s->estado == SRX_RECIBIENDO) {                     /* el PC repite la cabecera (se perdió nuestro «bien»): si es la MISMA, se re-acusa */
        if (total == s->total && crc == s->crc_decl && nr == largo(s->ruta) && igual_n((const char *)c + 9, s->ruta, nr) && s->hecho == 0) { bien(s, 'A'); return; }
        negar(s, SRX_E_ORDEN, ": otra cabecera a media recepción"); return;
    }
    s->estado = SRX_ESPERA; s->error = 0; s->texto[0] = 0;
    memcpy(s->ruta, c + 9, nr); s->ruta[nr] = 0;
    s->reemplazar = (uint8_t)(c[8] & 1); s->total = total; s->crc_decl = crc; s->hecho = 0; s->crc = 0;
    { uint32_t i; for (i = 0; i < nr; i++) if (!s->ruta[i]) { s->ruta[0] = 0; break; } }   /* un 0 dentro: no vale */
    if (!srx_ruta_valida(s->ruta)) { negar(s, SRX_E_NOMBRE, 0); return; }
    if (total > SRX_TAM_MAX) { negar(s, SRX_E_GRANDE, 0); return; }
    if (s->al->info(s->al->ctx, s->ruta, &es_dir) == 1) {
        if (es_dir) { negar(s, SRX_E_EXISTE, ": es una carpeta"); return; }
        if (!s->reemplazar) { negar(s, SRX_E_EXISTE, 0); return; }
    }
    if (s->al->info(s->al->ctx, SRX_TEMPORAL, &es_dir) == 1) s->al->borrar(s->al->ctx, SRX_TEMPORAL);   /* restos de un corte entre cerrar y renombrar */
    libre = s->al->libre(s->al->ctx);
    if (total > libre) { negar(s, SRX_E_LLENO, 0); return; }   /* reemplazando, el viejo ocupa hasta el final: tiene que caber entero aparte */
    for (b = 1; s->ruta[b] != '/'; b++) dir[b] = s->ruta[b];
    dir[0] = '/'; dir[b] = 0;
    if (s->al->info(s->al->ctx, dir, &es_dir) != 1) { if (s->al->mkdir(s->al->ctx, dir) < 0) { negar(s, SRX_E_ESCRIBIR, ": carpeta"); return; } }
    else if (!es_dir) { negar(s, SRX_E_NOMBRE, ": la carpeta es un fichero"); return; }
    if (s->al->abrir(s->al->ctx, SRX_TEMPORAL) < 0) { negar(s, SRX_E_ESCRIBIR, 0); return; }
    s->abierto = 1; s->estado = SRX_RECIBIENDO;
    s->es_app = (uint8_t)(b == 5 && igual_n(s->ruta, "/apps", 5) && nr > 4 && igual_n(s->ruta + nr - 4, ".sen", 4));
    bien(s, 'A');
}
static void datos(Srx *s, const uint8_t *c, uint32_t n)
{
    uint32_t off, k;
    if (s->estado != SRX_RECIBIENDO) { if (s->estado != SRX_FALLO) negar(s, SRX_E_ORDEN, ": datos sin cabecera"); else negar(s, s->error, 0); return; }
    if (n < 5) { bien(s, 'N'); return; }
    off = le32(c); k = n - 4;
    if (off != s->hecho) { s->repetidas++; bien(s, off < s->hecho ? 'A' : 'N'); return; }   /* repetida (se perdió el acuse): se re-acusa */
    if (k > s->total - s->hecho) { negar(s, SRX_E_TAMANO, ": más datos que los declarados"); return; }
    if (s->al->escribir(s->al->ctx, c + 4, k) < 0) { negar(s, SRX_E_ESCRIBIR, 0); return; }
    s->crc = srx_crc32(s->crc, c + 4, k); s->hecho += k;
    bien(s, 'A');
}
static void fin(Srx *s)
{
    if (s->estado == SRX_HECHO) { bien(s, 'A'); return; }   /* repetido: ya está */
    if (s->estado != SRX_RECIBIENDO) { negar(s, s->estado == SRX_FALLO ? s->error : SRX_E_ORDEN, 0); return; }
    if (s->hecho != s->total) { negar(s, SRX_E_TAMANO, ": faltan bytes"); return; }
    if (s->crc != s->crc_decl) { negar(s, SRX_E_CRC, 0); return; }
    s->abierto = 0;
    if (s->al->cerrar(s->al->ctx) < 0) { s->al->borrar(s->al->ctx, SRX_TEMPORAL); negar(s, SRX_E_ESCRIBIR, ": al cerrar"); return; }
    if (s->al->renombrar(s->al->ctx, SRX_TEMPORAL, s->ruta, s->reemplazar) < 0) { s->al->borrar(s->al->ctx, SRX_TEMPORAL); negar(s, SRX_E_ESCRIBIR, ": al renombrar"); return; }
    s->estado = SRX_HECHO;
    bien(s, 'A');
}
static void trama(Srx *s, uint8_t tipo, const uint8_t *c, uint32_t n)
{
    s->tramas++;
    switch (tipo) {
    case 'H': cabecera(s, c, n); break;
    case 'D': datos(s, c, n); break;
    case 'F': fin(s); break;
    case 'X': if (s->estado == SRX_RECIBIENDO) negar(s, SRX_E_ABANDONO, 0); break;
    default: bien(s, 'N'); break;
    }
}
void srx_meter(Srx *s, const uint8_t *d, uint32_t n, uint32_t ahora_ms)
{
    while (n) {
        uint32_t k = sizeof s->ent - s->n_ent, i, l;
        if (k > n) k = n;
        memcpy(s->ent + s->n_ent, d, k); s->n_ent += k; d += k; n -= k;
        for (;;) {                                         /* buscar 'W' '5' y una trama entera con su CRC */
            for (i = 0; i < s->n_ent && !(s->ent[i] == 'W' && (i + 1 == s->n_ent || s->ent[i + 1] == '5')); i++) ;
            if (i) { memmove(s->ent, s->ent + i, s->n_ent - i); s->n_ent -= i; }
            if (s->n_ent < 5) break;
            l = (uint32_t)s->ent[3] | (uint32_t)s->ent[4] << 8;
            if (l > SRX_CARGA_MAX) { s->rotas++; memmove(s->ent, s->ent + 1, --s->n_ent); continue; }
            if (s->n_ent < 9 + l) break;
            if (srx_crc32(0, s->ent + 2, 3 + l) != le32(s->ent + 5 + l)) {
                s->rotas++; memmove(s->ent, s->ent + 1, --s->n_ent);
                if (s->estado == SRX_RECIBIENDO) bien(s, 'N');   /* que repita ya, sin esperar a su plazo */
                continue;
            }
            s->ultimo_ms = ahora_ms;
            trama(s, s->ent[2], s->ent + 5, l);
            memmove(s->ent, s->ent + 9 + l, s->n_ent - 9 - l); s->n_ent -= 9 + l;
        }
    }
}
void srx_tic(Srx *s, uint32_t ahora_ms)
{
    if (s->estado == SRX_RECIBIENDO && ahora_ms - s->ultimo_ms > SRX_PLAZO_MS) negar(s, SRX_E_PLAZO, 0);
}
void srx_cancelar(Srx *s, int codigo, const char *texto)
{
    if (s->estado == SRX_RECIBIENDO) negar(s, codigo, texto);
}
