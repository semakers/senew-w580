/* (SER) «Recibir por serie»: el PC manda UN fichero por la UART 0 (herr/enviar_serie.py) y se guarda en la Memoria.
 * Se abre A MANO (Archivos > Opciones > «Recibir por serie»): el sistema NO atiende la serie en ningún otro momento, así que
 * nada que llegue por el cable puede escribir sin que el usuario lo haya pedido. Al abrirse se TIRA lo que hubiera en el
 * anillo de la UART (lo llena la IRQ siempre). Se sondea cada 20 ms desde un temporizador de la UI, con un tope de bytes por
 * vuelta: la interfaz no se congela y el bucle principal sigue dando de comer al perro. Atrás cancela (y se lo dice al PC).
 * El protocolo y sus reglas: api/serie_rx.c; documento: docs/recibir_por_serie.md. */
#include "shell.h"
#include "serie_rx.h"
#include "tarjeta.h"                                 /* (FATW) o a la tarjeta: se elige ANTES de que llegue nada */

typedef struct { Widget base; } Recibir;
typedef struct { u32 version; u8 escribible, avisado, destino; } RecibirE;   /* (FATW) destino: 0 = Memoria, 1 = tarjeta */
extern const Clase ClsRecibir;
static Srx rx;

static int a_info(void *c, const char *r, int *es_dir) { HwW6fInfo i; (void)c; if (hw_w6f_info(r, &i) != HW_OK) return 0; *es_dir = i.es_dir; return 1; }
static uint32_t a_libre(void *c) { (void)c; return hw_w6f_libre(); }
static int a_mkdir(void *c, const char *r) { (void)c; return hw_w6f_mkdir(r) == HW_OK ? 0 : -1; }
static int a_abrir(void *c, const char *r) { (void)c; return hw_w6f_rx_abrir(r) == HW_OK ? 0 : -1; }
static int a_escribir(void *c, const void *d, uint32_t n) { (void)c; return hw_w6f_rx_mas(d, n) == HW_OK ? 0 : -1; }
static int a_cerrar(void *c) { (void)c; return hw_w6f_rx_cerrar() == HW_OK ? 0 : -1; }
static void a_abortar(void *c) { (void)c; hw_w6f_rx_abortar(); }
static int a_renombrar(void *c, const char *de, const char *a, int r) { (void)c; return hw_w6f_renombrar_r(de, a, r) == HW_OK ? 0 : -1; }
static int a_borrar(void *c, const char *r) { (void)c; return hw_w6f_borrar(r) == HW_OK ? 0 : -1; }
static const SrxAlmacen almacen = { 0, a_info, a_libre, a_mkdir, a_abrir, a_escribir, a_cerrar, a_abortar, a_renombrar, a_borrar };
/* (FATW) LA TARJETA: el temporal NO llega a existir. «abrir» empieza la escritura a trozos (clústeres sin dueño), «cerrar» la
 * deja pendiente y «renombrar» le pone el nombre bueno y la compromete de UNA vez (hw_tj_rx_destino + cerrar): en FAT no hay un
 * renombrado atómico encima de otro, y así no hace falta (docs/fat_escritura.md §7). Un corte en medio no deja nada con nombre. */
static u8 t_pend;
static int t_info(void *c, const char *r, int *es_dir) { HwW6fInfo i; (void)c; if (!strcmp(r, SRX_TEMPORAL)) return 0; if (hw_tj_info(r, &i) != HW_OK) return 0; *es_dir = i.es_dir; return 1; }
static uint32_t t_libre(void *c) { (void)c; return hw_tj_libre(); }
static int t_mkdir(void *c, const char *r) { (void)c; return hw_tj_mkdir(r) == HW_OK ? 0 : -1; }
static int t_abrir(void *c, const char *r) { (void)c; t_pend = 0; return hw_tj_rx_abrir_d(r, HW_RX_SER) == HW_OK ? 0 : -1; }
static int t_escribir(void *c, const void *d, uint32_t n) { (void)c; return hw_tj_rx_mas_d(HW_RX_SER, d, n) == HW_OK ? 0 : -1; }
static int t_cerrar(void *c) { (void)c; if (hw_tj_rx_dueno() != HW_RX_SER) return -1; t_pend = 1; return 0; }
static void t_abortar(void *c) { (void)c; t_pend = 0; hw_tj_rx_abortar_d(HW_RX_SER); }
static int t_renombrar(void *c, const char *de, const char *a, int r)
{
    (void)c;
    if (!t_pend || strcmp(de, SRX_TEMPORAL)) return hw_tj_renombrar_r(de, a, r) == HW_OK ? 0 : -1;
    t_pend = 0;
    if (hw_tj_rx_destino(HW_RX_SER, a, r) != HW_OK) { hw_tj_rx_abortar_d(HW_RX_SER); return -1; }
    return hw_tj_rx_cerrar_d(HW_RX_SER) == HW_OK ? 0 : -1;
}
static int t_borrar(void *c, const char *r) { (void)c; if (!strcmp(r, SRX_TEMPORAL)) { t_abortar(c); return 0; } return hw_tj_borrar(r) == HW_OK ? 0 : -1; }
static const SrxAlmacen almacen_tj = { 0, t_info, t_libre, t_mkdir, t_abrir, t_escribir, t_cerrar, t_abortar, t_renombrar, t_borrar };
static int mem_ok(void) { HwW6fEstado w; hw_w6f_estado(&w); return w.escribible; }
static void elegir(RecibirE *s, int destino)
{
    s->destino = (u8)destino;
    s->escribible = (u8)(destino ? hw_tj_escribible() : mem_ok());
    srx_iniciar(&rx, destino ? &almacen_tj : &almacen, hw_ms());
}

static void soltar_salida(void)
{
    if (rx.n_sal) { hw_serie_escribir(rx.sal, (int)rx.n_sal); rx.n_sal = 0; }
}
static void sondeo(Ctx *c)
{
    RecibirE *s = ui_estado(c);
    u8 b[256]; int n, vueltas = 0; u32 antes = rx.hecho, est = rx.estado;
    if (!s->escribible) return;
    while (vueltas++ < 8 && (n = hw_serie_leer(b, sizeof b)) > 0) {   /* tope: 2 KiB por vuelta (unas 4 tramas) */
        srx_meter(&rx, b, (u32)n, hw_ms());
        soltar_salida();
        if (n < (int)sizeof b) break;
    }
    srx_tic(&rx, hw_ms());
    soltar_salida();
    if (rx.estado == SRX_HECHO && est != SRX_HECHO && rx.es_app) sh_apps_pendiente();   /* el menú se rehace al volver al standby */
    if (rx.hecho != antes || rx.estado != est) { s->version++; ui_sucio(c); sh_actividad(); }   /* recibiendo, la pantalla no se duerme: si
                                                       no, el primer Atrás solo la despierta y no cancela [observado en el emulador] */
}
static void iniciar(Ctx *c)
{
    RecibirE *s = ui_estado(c);
    u8 b[64]; int k;
    for (k = 0; k < 64 && hw_serie_leer(b, sizeof b) > 0; k++) ;   /* lo de antes de abrir la pantalla no cuenta */
    elegir(s, !mem_ok() && hw_tj_escribible());          /* (FATW) la Memoria, si se puede; si no, la tarjeta */
    ui_cada(c, 20, sondeo);
}
static void desechar(Ctx *c)
{
    (void)c;
    srx_cancelar(&rx, SRX_E_CANCELADO, 0);       /* la pantalla se va con una recepción a medias: no queda nada y el PC se entera */
    soltar_salida();
}
static int tecla(Ctx *t, int k, void *dato)
{
    Ctx *c = ui_ancestro(t, &ClsRecibir);
    (void)dato;
    if (k == K_SEL_IZQ && rx.estado != SRX_RECIBIENDO && hw_tj_escribible()) {   /* (FATW) cambiar el destino, solo sin nada en marcha */
        RecibirE *s = ui_estado(c);
        elegir(s, !s->destino); s->version++; ui_sucio(c); return 1;
    }
    if (k == K_SEL_DER || k == K_ATRAS) {
        if (rx.estado == SRX_RECIBIENDO) { srx_cancelar(&rx, SRX_E_CANCELADO, 0); soltar_salida(); ((RecibirE *)ui_estado(c))->version++; ui_sucio(c); return 1; }
        ui_sacar(); return 1;
    }
    return 0;
}
static Widget *construir(Ctx *c)
{
    RecibirE *s = ui_estado(c);
    const TemaDatos *t = sh_tema();
    const char *linea, *det;
    if (!s->escribible) { linea = s->destino ? "La tarjeta no se puede escribir ahora." : "La Memoria no se puede escribir ahora."; det = s->destino ? "¿Protegida, o se ha sacado? No se escucha la serie." : "Mira Sistema > Memoria. No se escucha la serie."; }
    else switch (rx.estado) {
    case SRX_RECIBIENDO: linea = ui_fmt("Recibiendo %s", rx.ruta); det = ui_fmt("%u de %u bytes", rx.hecho, rx.total); break;
    case SRX_HECHO: linea = ui_fmt("Guardado: %s", rx.ruta); det = rx.es_app ? "Es una app: sale en el menú al volver." : ui_fmt("%u bytes", rx.total); break;
    case SRX_FALLO: linea = "No se ha guardado nada."; det = rx.texto; break;
    default: linea = "Esperando al PC…"; det = "En el PC: herr/enviar_serie.py FICHERO"; break;
    }
    return Teclas(tecla, Pantalla("Recibir por serie", Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        Caja(.an = INF, .color = t->barra, .hijo = RellenoXY(4, 2, Fila(.hijos = HIJOS(
            Texto(ui_fmt("%s · a %s", hw_serie_conectado() ? "UART 0 · con cable" : "UART 0 · SIN CABLE", s->destino ? "tarjeta" : "Memoria"), .color = hw_serie_conectado() ? t->texto : dis_tokens()->c.error),   /* (MD) lo que falla, en el color de error */
            Hueco(), Texto(ui_fmt("%u tramas", rx.tramas), .color = t->tenue))))),
        Relleno(6, Tarjeta(Columna(.cruzado = P_ESTIRAR, .espacio = 6, .hijos = HIJOS(     /* (MD) el estado, en una tarjeta del diseño */
            Parrafo(linea, .color = rx.estado == SRX_FALLO ? dis_tokens()->c.error : t->texto),
            Progreso(.valor = (i32)(rx.total ? rx.hecho / 256u : 0), .max = (i32)(rx.total ? (rx.total + 255u) / 256u : 1)),
            Parrafo(det, .color = t->tenue))), 0)),
        Hueco(),
        Caja(.an = INF, .color = t->barra, .hijo = Relleno(4, Parrafo(
            "Solo se escucha con esta pantalla abierta. Atrás cancela: lo recibido a medias no se queda.", .color = t->tenue, .marcas = F_ESTILO(DIS_T_PEQUENA)))))),
        rx.estado != SRX_RECIBIENDO && hw_tj_escribible() ? (s->destino ? "A Memoria" : "A tarjeta") : "", rx.estado == SRX_RECIBIENDO ? "Cancelar" : "Atrás"));
}
COMPONENTE_CON_ESTADO(Recibir, RecibirE, construir, iniciar, .desechar = desechar);
Widget *app_recibir(void) { return UI_W(Recibir); }
