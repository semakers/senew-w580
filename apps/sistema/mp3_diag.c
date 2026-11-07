/* Sistema -> Opciones -> «Diagnóstico MP3» (2026-10-21): el reproductor de MP3 por el DSP del W580i (hal/mp3_w580.c) A PETICIÓN, con una pantalla que dice en qué
 * FASE está o se quedó, el primer ERROR, cuánto tardó cada fase, qué contestó el DSP a cada R_n del mezclador, los créditos del anillo de datos, los 16 registros del
 * códec y los últimos mensajes ARM<->DSP. Una foto de esta pantalla (arriba y abajo) basta para saber si el DSP real habla el protocolo que el original y el emulador
 * enseñaron. NO se arranca nada hasta pulsar 1; con cable USB puesto se NIEGA (la 5 no fuerza: aquí no hay experimento del USB).
 * Teclas: 1 tocar (cada pulsación pasa al siguiente MP3 de la lista de Música) · 2 pausa / seguir · 3 parar · 4 / 5 volumen − / + · 6 / 7 adelantar / atrasar 10 s.
 * Al SALIR se para la música y el DSP se apaga. Todo débil: en las placas sin hal/mp3_w580.c dice «no disponible». */
#include "shell.h"
#include "../../hal/mp3_w580.h"

#pragma weak mp3_diag
#pragma weak mp3_diag_log
#pragma weak mp3_diag_rn
#pragma weak mp3_fase_nombre
#pragma weak mp3_error_nombre
#pragma weak mp3_progreso
#pragma weak hw_mp3_abrir
#pragma weak hw_mp3_play
#pragma weak hw_mp3_pausa
#pragma weak hw_mp3_parar
#pragma weak hw_mp3_volumen
#pragma weak hw_mp3_buscar
#pragma weak hw_mp3_estado
#pragma weak hw_mp3_tiempo
#pragma weak hw_mp3_motivo_txt

typedef struct { Widget base; } Mp3Pant;
typedef struct {
    i32 sel;
    int hay, cable, cual, n_mp3;
    Mp3Diag d;
    Mp3LogMsg lg[6]; int n_lg;
    Mp3Rn rn[8]; int n_rn;
    char nombre[40];
} Mp3PantE;
extern const Clase ClsMp3Pant;
#define N_FILAS 37

static Ctx *cur;                                    /* la pantalla abierta: el gancho del reproductor la repinta mientras bloquea */

static const char *estado_txt(int e) { static const char *const t[] = { "parado", "abierto", "SONANDO", "pausa", "fin de pista", "ERROR" }; return e >= 0 && e < 6 ? t[e] : "?"; }
static int nth_mp3(int k, char *ruta, unsigned max)         /* la ruta del k-ésimo MP3 de la lista de Música; da cuántos hay */
{
    int i, n = 0, tot = sh_musica_n();
    for (i = 0; i < tot; i++) {
        const MuPista *p = sh_musica_pista(i);
        if (p && p->tipo == MU_T_MP3) {
            if (n == k && ruta) { unsigned j = 0; while (p->ruta[j] && j + 1 < max) { ruta[j] = p->ruta[j]; j++; } ruta[j] = 0; }
            n++;
        }
    }
    return n;
}
static void leer(Ctx *c)
{
    Mp3PantE *s = ui_estado(c);
    int cg;
    s->hay = mp3_diag != 0 && hw_mp3_play != 0;
    if (s->hay) {
        mp3_diag(&s->d);
        s->n_lg = mp3_diag_log(s->lg, 6);
        s->n_rn = mp3_diag_rn(s->rn, 8);
    }
    cg = hw_cargador();
#ifdef MP3_IGNORA_CABLE                            /* SOLO build de desarrollo con el puente por USB */
    s->cable = 0;
#else
    s->cable = cg > 0 && (cg & 2);
#endif
    ui_sucio(c);
}
static void gancho(void) { if (cur) { leer(cur); ui_cuadro(); } }   /* desde dentro de un play largo: pinta la fase en que va */
static void iniciar(Ctx *c)
{
    Mp3PantE *s = ui_estado(c);
    cur = c;
    sh_musica_abrir();                              /* el servicio de música tiene que estar iniciado: su paso (cada vuelta del bucle) alimenta al DSP */
    sh_musica_parar();                              /* nada del motor de música suena a la vez */
    sh_musica_buscar();
    s->n_mp3 = nth_mp3(-1, 0, 0);
    if (mp3_progreso) mp3_progreso(gancho);
    leer(c);
    ui_cada(c, 500, leer);
}
static void desechar(Ctx *c)
{
    (void)c;
    if (mp3_progreso) mp3_progreso(0);
    cur = 0;
    if (hw_mp3_parar) hw_mp3_parar();
}

static const char *hx(u32 v) { return ui_fmt("%04X", v & 0xFFFF); }

static Widget *fila(Ctx *l, int i, int sel, void *dato)
{
    Mp3PantE *s = ui_estado(ui_ancestro(l, &ClsMp3Pant));
    const Mp3Diag *d = &s->d;
    const char *n = "", *v = "";
    (void)dato;
    if (!s->hay) return sh_fila(i ? "" : "MP3", i ? "" : "no disponible en esta plataforma", sel);
    switch (i) {
    case 0:  n = "Teclas"; v = "1 tocar 2 pausa 3 parar"; break;
    case 1:  n = ""; v = "4/5 volumen 6/7 +10s/-10s"; break;
    case 2:  n = "AVISO"; v = "sin cable USB al tocar"; break;
    case 3:  n = "Cable USB"; v = s->cable ? "PUESTO: se niega" : "sin cable: bien"; break;
    case 4:  n = "Pista"; v = s->n_mp3 ? ui_fmt("%d de %d  %s", s->cual + 1, s->n_mp3, s->nombre) : "no hay MP3 en la lista de Música"; break;
    case 5:  n = "MP3"; v = ui_fmt("%u Hz %u kbps %u ca %s", d->fs, d->kbps, d->canales, d->vbr ? "VBR" : "CBR"); break;
    case 6:  n = "Estado"; v = ui_fmt("%s", estado_txt(d->estado)); break;
    case 7:  n = "Fase"; v = ui_fmt("%u %s", d->fase, mp3_fase_nombre(d->fase)); break;
    case 8:  n = "Error"; v = d->error ? ui_fmt("%d %s", (i32)d->error, mp3_error_nombre(d->error)) : "ninguno"; break;
    case 9:  n = "  causa A"; v = d->sub ? ui_fmt("dsp_* %d", (i32)d->sub) : "-"; break;
    case 10: n = "Tiempo"; v = ui_fmt("%u / %u s  vol %u/15", d->pos_ms / 1000, d->dur_ms / 1000, d->volumen); break;
    case 11: n = "ms fases"; v = ui_fmt("dsp %u apf %u cod %u lin %u", d->ms_fase[MP3_F_DSP], d->ms_fase[MP3_F_APF], d->ms_fase[MP3_F_CODEC], d->ms_fase[MP3_F_LINEA]); break;
    case 12: n = "  (cont.)"; v = ui_fmt("pre %u mod %u ver %u post %u", d->ms_fase[MP3_F_PREBOOT], d->ms_fase[MP3_F_MODULO], d->ms_fase[MP3_F_VERSION], d->ms_fase[MP3_F_POSTBOOT]); break;
    case 13: n = "  (cont.)"; v = ui_fmt("proc %u arr %u ini %u r2 %u", d->ms_fase[MP3_F_PROCESO], d->ms_fase[MP3_F_ARRANCA], d->ms_fase[MP3_F_INIT], d->ms_fase[MP3_F_R2]); break;
    case 14: n = "  (cont.)"; v = ui_fmt("cfg %u dat %u play %u TOTAL %u", d->ms_fase[MP3_F_CONFIG], d->ms_fase[MP3_F_DATOS], d->ms_fase[MP3_F_PLAY], d->ms_total); break;
    case 15: n = "DSP"; v = ui_fmt("mod %u linea %u apf %u", d->dsp_modulo, d->linea, d->apf_origen); break;
    case 16: n = "APF"; v = ui_fmt("%s", d->apf_nombre[0] ? d->apf_nombre : "-"); break;
    case 17: n = "Version"; v = ui_fmt("%s", d->version[0] ? d->version : "(sin respuesta)"); break;
    case 18: n = "Mezclador"; v = ui_fmt("ok %u mal %u res!=0 %u (R_%u)", d->rpc_ok, d->rpc_mal, d->rpc_res_no0, d->rpc_mal_n); break;
    case 19: n = "Anillo"; v = ui_fmt("canal %02X flujo %u cap %u esp %d", d->proceso_canal, d->flujo, d->cap, d->espacio); break;
    case 20: n = "Datos"; v = ui_fmt("troz %u bytes %u pal %u", d->trozos, d->bytes_enviados, d->pal_enviadas); break;
    case 21: n = "Creditos"; v = ui_fmt("pal %u msg %u ped %u", d->cred_pal, d->cred_msgs, d->cred_pedidos); break;
    case 22: n = "Sin datos/fin"; v = ui_fmt("nodata %u  fin tx %u rx %u", d->nodata, d->eos_enviado, d->eos_recibido); break;
    case 23: n = "Mensajes"; v = ui_fmt("tx %u rx %u", d->n_msgs_tx, d->n_msgs_rx); break;
    case 24: n = "Codec C1-C8"; v = !d->c1_5_ok ? "-" : ui_fmt("%02X %02X %02X %02X %02X %02X %02X %02X", d->c1_5[0], d->c1_5[1], d->c1_5[2], d->c1_5[3], d->c1_5[4], d->c1_5[5], d->c1_5[6], d->c1_5[7]); break;
    case 25: n = "Codec C9-D0"; v = !d->c1_5_ok ? "-" : ui_fmt("%02X %02X %02X %02X %02X %02X %02X %02X", d->c1_5[8], d->c1_5[9], d->c1_5[10], d->c1_5[11], d->c1_5[12], d->c1_5[13], d->c1_5[14], d->c1_5[15]); break;
    case 26: n = "GPIO/SYSCON"; v = ui_fmt("P1.5 %u P3.7 %u 12:%03X 18:%03X", d->p15, d->p37, d->dsp_regs_s12 & 0xFFFF, d->dsp_regs_s18 & 0xFFFF); break;
    case 27: case 28: case 29: case 30: case 31: case 32: {                      /* los últimos mensajes, el más reciente primero */
        int k = i - 27;
        if (k >= s->n_lg) { n = "Mensaje -"; v = ""; break; }
        n = ui_fmt("%s c%02X n%u t%u", s->lg[k].rx ? "RX" : "TX", s->lg[k].canal & 0xFF, s->lg[k].n, s->lg[k].ms % 100000);
        v = ui_fmt("%s %s %s %s", hx(s->lg[k].w[0]), hx(s->lg[k].w[1]), hx(s->lg[k].w[2]), hx(s->lg[k].w[3]));
        break; }
    default: {                                                                    /* 33..36: la PRIMERA respuesta del DSP a cada R_n visto: hw6 (resultado) y el cuerpo */
        int k = i - 33;
        if (k >= s->n_rn) { n = "R_n -"; v = ""; break; }
        n = ui_fmt("R_%u x%u n%u", s->rn[k].rn, s->rn[k].n_resp, s->rn[k].n);
        v = ui_fmt("res %s  %s %s %s", hx(s->rn[k].w[6]), hx(s->rn[k].w[7]), hx(s->rn[k].w[8]), hx(s->rn[k].w[9]));
        break; }
    }
    return sh_fila(n, v, sel);
}

static void registrar(const char *que)             /* una línea por la UART 0 con lo esencial */
{
    Mp3Diag d;
    if (!mp3_diag) return;
    mp3_diag(&d);
    hw_log(ui_fmt("mp3: %s -> estado %u fase %u (%s) error %d (%s) sub %d rpc %u/%u cred %u/%u troz %u nodata %u\n", que, d.estado, d.fase, mp3_fase_nombre(d.fase),
                  (i32)d.error, mp3_error_nombre(d.error), (i32)d.sub, d.rpc_ok, d.rpc_mal, d.cred_msgs, d.cred_pal, d.trozos, d.nodata));
}
static void tocar(Ctx *c)
{
    Mp3PantE *s = ui_estado(c);
    char ruta[MU_RUTA];
    int n;
    leer(c);
    if (!s->hay) { ui_aviso("MP3", "No disponible en esta plataforma."); return; }
    if (s->cable) { ui_aviso("MP3", "Hay un cable USB puesto. Desenchufalo y vuelve a pulsar 1."); return; }
    s->n_mp3 = nth_mp3(-1, 0, 0);
    if (!s->n_mp3) { ui_aviso("MP3", "No hay ningún MP3 en la lista de Música (carpeta music de la tarjeta o /tpa/user/music)."); return; }
    if (hw_mp3_estado() == HW_MP3_SONANDO || hw_mp3_estado() == HW_MP3_PAUSA) s->cual = (s->cual + 1) % s->n_mp3;     /* ya hay uno: el siguiente */
    if (s->cual >= s->n_mp3) s->cual = 0;
    n = nth_mp3(s->cual, ruta, sizeof ruta);
    (void)n;
    { const char *b = ruta, *p; unsigned k = 0; for (p = ruta; *p; p++) if (*p == '/') b = p + 1; while (b[k] && k + 1 < sizeof s->nombre) { s->nombre[k] = b[k]; k++; } s->nombre[k] = 0; }
    sh_actividad();
    if (hw_mp3_abrir(ruta) == HW_OK) { registrar("abierto"); hw_mp3_play(); }
    registrar("tocar");
    leer(c);
}
static int tecla(Ctx *t, int k, void *dato)
{
    Ctx *c = ui_ancestro(t, &ClsMp3Pant);
    Mp3PantE *s = ui_estado(c);
    if (k == K_1) { tocar(c); return 1; }
    if (k == K_2 && s->hay) {
        sh_actividad();
        if (hw_mp3_estado() == HW_MP3_PAUSA) hw_mp3_play(); else hw_mp3_pausa();
        registrar("pausa/sigue"); leer(c); return 1; }
    if (k == K_3 && s->hay) { hw_mp3_parar(); registrar("parar"); leer(c); return 1; }
    if ((k == K_4 || k == K_5) && s->hay) {
        int v = (int)s->d.volumen + (k == K_5 ? 1 : -1);
        hw_mp3_volumen(v < 0 ? 0 : v > 15 ? 15 : v); registrar("volumen"); leer(c); return 1; }
    if ((k == K_6 || k == K_7) && s->hay) {
        u32 dur, pos = hw_mp3_tiempo(&dur);
        hw_mp3_buscar(k == K_6 ? pos + 10000u : pos > 10000u ? pos - 10000u : 0); registrar("buscar"); leer(c); return 1; }
    if (k == K_OK || k == K_SEL_IZQ) return 1;
    return sh_teclas(t, k, dato);
}
static Widget *construir(Ctx *c)
{
    Mp3PantE *s = ui_estado(c);
    return Teclas(tecla, Pantalla("Diagnóstico MP3", ListaPerezosa(.n = N_FILAS, .fila = fila, .sel = &s->sel, .alto_fila = dis_tokens()->m.alto_fila), "", "Atrás"));
}
COMPONENTE_CON_ESTADO(Mp3Pant, Mp3PantE, construir, iniciar, .desechar = desechar);
Widget *app_mp3_diag(void) { return UI_W(Mp3Pant); }
