/* dictado.c — (E5, 27-oct) EL DICTADO POR VOZ en cualquier campo de texto: el CampoTexto del sistema, que es también el de las apps .sen (W5C_CAMPO_TEXTO),
 * así que ninguna app tiene que programarlo. Pedido del mantenedor: «que en esa y en todas las apps todos los text inputs puedan usar dictado por voz,
 * conectado al servidor de internet por Bluetooth con whisper» («no estoy acostumbrado al T9»). docs/dictado.md.
 *
 *   pulsación LARGA de «*» en un campo enfocado  ->  el emergente «Dictado» (ui/ui_texto.c llama a ui_dictado_gancho)
 *   «*» u OK: acabar        C / Atrás: cancelar
 *
 * El camino: la RED primero (sh_red_conectar como SH_BT_DICTADO, su dueño propio: si «YT Music» ya la tenía, solo se suma un dueño y no se corta), luego el MICRÓFONO
 * (shell/micro_sis.c, dueño SH_MIC_DICTADO; si sonaba música por el DSP, se para y vuelve al acabar de hablar). Las muestras van a un búfer de 20 s y,
 * en cuanto la red está lista, SALEN YA, mientras se habla: un POST a trozos al servicio de dictado del computador (DICT_URL; el puente junta el cuerpo y lo pasa
 * entero al servicio). Al acabar: el micrófono se cierra (la música vuelve), se termina el cuerpo y se espera el texto (UTF-8, ya en la letra del
 * teléfono), que se inserta en el cursor del campo (ui_campo_insertar).
 * En el banco de PC no hay radio: con SIS_DICTADO_FALSO=«texto» el «servicio» contesta eso al acabar (para probar el gesto, el emergente y la inserción). */
#include "shell.h"
#include "bt_sis.h"
#include "red_sis.h"
#include "micro_sis.h"
#include "../bt/red/red.h"
#include "diseno.h"
#if !(defined(TELEFONO) && TELEFONO)
#include <stdlib.h>
#endif

#define DICT_URL "http://127.0.0.1:8581/dictar?hz=8000&idioma=es&max=500"    /* bt/dictado/servicio.py; el puente con --local 8581 */
#define DICT_MAX (20u * 8000u)                                        /* 20 s de voz: luego se acaba solo */
#define DICT_RED_TOPE_MS 30000u                                       /* conectar la red (con la radio apagada tarda 15-20 s) */
#define DICT_RESP_TOPE_MS 30000u                                      /* desde que se acaba de hablar hasta el texto */
enum { DE_RED, DE_PREPARA, DE_HABLA, DE_ENVIA, DE_ESPERA, DE_FIN };

static short au[DICT_MAX];
static struct {
    u8 fase, red_mia, mic_mio, acabar, cancelar, fallo, vivo;
    int canal;
    Ctx *campo;
    u32 n, enviados, t0, t_fase, t_habla, t_acabo, ms_texto;
    char texto[512], msj[96];
    u32 tam_texto;
} D;

typedef struct { Widget base; } Dictado;
typedef struct { u32 nada; } DictadoE;
extern const Clase ClsDictado;

static int contiene(const char *s, const char *sub) { u32 n = (u32)strlen(sub), m = (u32)strlen(s), i; for (i = 0; i + n <= m; i++) if (!memcmp(s + i, sub, n)) return 1; return 0; }
static void msj(const char *s) { u32 i; for (i = 0; s[i] && i < sizeof D.msj - 1; i++) D.msj[i] = s[i]; D.msj[i] = 0; }
static void fase(u8 f) { D.fase = f; D.t_fase = hw_ms(); }
static void soltar_todo(void)
{
    if (D.canal > 0) { red_cerrar(D.canal); sh_red_canal_poner(D.canal, SH_BT_NADIE); D.canal = 0; }
    if (D.mic_mio) { sh_mic_cerrar(SH_MIC_DICTADO); D.mic_mio = 0; }     /* la música (si se cedió) vuelve y se suma a la red ANTES de soltarla */
    if (D.red_mia) { sh_red_soltar(SH_BT_DICTADO); sh_bt_soltar(SH_BT_DICTADO); D.red_mia = 0; }   /* y la radio: apagada, o al dueño que siga (revisión 27-oct, 1) */
}
static void fallar(const char *s) { msj(s); D.fallo = 1; soltar_todo(); fase(DE_FIN); hw_log("dictado: "); hw_log(s); hw_log("\n"); }

/* empujar lo grabado por la red (hasta 4 KB por llamada; la cola del canal es de 16 KB) */
static void empujar(void)
{
    while (D.canal > 0 && D.enviados < D.n * 2u) {
        u32 q = D.n * 2u - D.enviados;
        int k;
        if (q > 4096u) q = 4096u;
        k = red_enviar(D.canal, (const u8 *)au + D.enviados, q);
        if (k <= 0) break;
        D.enviados += (u32)k;
    }
}
static void abrir_canal(void)
{
    int c;
    if (D.canal > 0 || sh_red_estado() != SH_RED_LISTA) return;
    c = red_http("POST", DICT_URL, "Content-Type: application/octet-stream\r\n", 0, RED_CUERPO_A_TROZOS, 0, 45);
    if (c > 0) { D.canal = c; sh_red_canal_poner(c, SH_BT_DICTADO); }
    else if (c != RED_E_OCUPADO) fallar("No se pudo abrir la petición al dictado.");
}
static void tomar(void)
{
    int k;
    if (!D.mic_mio || D.n >= DICT_MAX) return;
    k = sh_mic_tomar(SH_MIC_DICTADO, au + D.n, DICT_MAX - D.n);
    if (k > 0) D.n += (u32)k;
}

/* una vuelta (desde la tarea del emergente, en cada vuelta del bucle) */
static int paso(void)
{
    ShMicInfo mi;
    u32 t = hw_ms();
    if (D.cancelar && D.fase != DE_FIN) { soltar_todo(); msj("Cancelado."); fase(DE_FIN); }
    switch (D.fase) {
    case DE_RED:                                                       /* la red y el micrófono a la vez: el micrófono tarda ~2-3 s, la radio apagada 15-20 */
    case DE_PREPARA:
        if (D.red_mia && sh_red_estado() == SH_RED_FALLO) {
            fallar(ui_fmt("Sin red con el computador (%s).", sh_red_fallo_nombre(sh_red_fallo()))); break;
        }
        if (D.red_mia && sh_red_estado() != SH_RED_LISTA && t - D.t0 > DICT_RED_TOPE_MS) { fallar("La red con el computador no llega."); break; }
        if (D.red_mia) abrir_canal();
        sh_mic_info(&mi);
        if (mi.estado == SH_MIC_FALLO) { fallar(ui_fmt("El micrófono no arranca (%d).", mi.error)); break; }
        if (mi.estado == SH_MIC_GRABANDO) { D.t_habla = t; fase(DE_HABLA); }
        break;
    case DE_HABLA:
        tomar();
        if (D.red_mia) { abrir_canal(); empujar(); }
        if (D.acabar || D.n >= DICT_MAX) {
            D.acabar = 0;
            sh_mic_cerrar(SH_MIC_DICTADO); D.mic_mio = 0;              /* la música (si se paró) vuelve YA, mientras se reconoce */
            D.t_acabo = t;
            hw_log(ui_fmt("dictado: %u muestras (%u ms de voz)\n", D.n, D.n / 8u));
            if (D.n < 2400u) { soltar_todo(); msj("Demasiado corto: mantén «*» y habla."); D.fallo = 1; fase(DE_FIN); break; }
            fase(DE_ENVIA);
        }
        break;
    case DE_ENVIA:
#if !(defined(TELEFONO) && TELEFONO)
        if (!D.red_mia) {                                              /* el banco de PC: el «servicio» de mentira */
            const char *f = getenv("SIS_DICTADO_FALSO"); u32 i;
            for (i = 0; f && f[i] && i < sizeof D.texto - 1; i++) D.texto[i] = f[i];
            D.texto[i] = 0; D.tam_texto = i; D.ms_texto = t - D.t_acabo; fase(DE_FIN);
            break;
        }
#endif
        abrir_canal(); empujar();
        if (D.canal > 0 && D.enviados >= D.n * 2u) { red_terminar(D.canal); fase(DE_ESPERA); }
        else if (t - D.t_acabo > DICT_RESP_TOPE_MS) fallar("La red no saca el audio.");
        break;
    case DE_ESPERA: {
        int h = red_http_estado(D.canal), k;
        if (h && h != 200 && h != 204) { fallar(ui_fmt("El dictado contesta HTTP %d.", h)); break; }
        while ((k = red_recibir(D.canal, D.texto + D.tam_texto, sizeof D.texto - 1 - D.tam_texto)) > 0) D.tam_texto += (u32)k;
        D.texto[D.tam_texto] = 0;
        if ((red_canal(D.canal) == RED_C_CERRADO && !red_disponibles(D.canal)) || (h && D.tam_texto >= sizeof D.texto - 1)) {
            if (!h) {                                                  /* el puente no pudo hablar con el servicio local */
                const char *x = red_texto(D.canal);
                fallar(contiene(x, "refused") ? "El dictado del computador está parado (¿SENUS abierta, con el dictado puesto?)." : ui_fmt("El dictado no responde (%s).", x));
                break;
            }
            D.ms_texto = t - D.t_acabo;
            hw_log(ui_fmt("dictado: texto en %u ms: «%s»\n", D.ms_texto, D.texto));
            soltar_todo(); fase(DE_FIN);
        } else if (t - D.t_acabo > DICT_RESP_TOPE_MS) fallar("El dictado tarda demasiado.");
        break; }
    case DE_FIN:
        if (!D.fallo && D.tam_texto && D.campo && ui_campo_lineas(D.campo) == 1) {   /* un campo de UNA línea (un buscador, un nombre): sin «¡¿» ni «.!?» */
            char *q = D.texto; u32 n;                                  /* (whisper los pone: «¡Caréfree de Kevin MacLeod!» buscaba peor [observado 28-oct]) */
            while (*q == ' ' || (q[0] == (char)0xC2 && (q[1] == (char)0xA1 || q[1] == (char)0xBF))) q += *q == ' ' ? 1 : 2;   /* ¡ ¿ (UTF-8 C2 A1 / C2 BF) */
            n = (u32)strlen(q);
            while (n && (q[n - 1] == '.' || q[n - 1] == '!' || q[n - 1] == '?' || q[n - 1] == ' ')) n--;
            memmove(D.texto, q, n); D.texto[n] = 0; D.tam_texto = n;
        }
        if (!D.fallo && D.tam_texto) {
            int ok = D.campo ? ui_campo_insertar(D.campo, D.texto) : 0;
            D.tam_texto = 0;
            if (!ok) { msj("El texto no cupo entero en el campo."); D.fallo = 1; D.t_fase = t; break; }
            ui_quitar_ruta(&ClsDictado);
            return 0;
        }
        if (!D.fallo && !D.msj[0]) msj("No se entendió nada.");
        if (!D.fallo) D.fallo = 1;
        if (t - D.t_fase > 2500u) { ui_quitar_ruta(&ClsDictado); return 0; }   /* el porqué, 2,5 s a la vista */
        break;
    }
    return 1;
}

static int d_tarea(Ctx *c, void *dato) { int r = paso(); (void)dato; ui_sucio(c); return r; }
static void d_iniciar(Ctx *c) { D.vivo = 1; ui_tarea(c, d_tarea, NULL); }
/* (revisión 27-oct, 2) si quita la ruta otro (una app con ui_sacar, Walkman, la app que muere), se suelta TODO aquí: micrófono, música, red y radio */
static void d_desechar(Ctx *c) { (void)c; D.vivo = 0; if (D.fase != DE_FIN) { soltar_todo(); fase(DE_FIN); } D.campo = NULL; }
/* el campo donde se dicta se desmonta (su pantalla se fue): el texto ya no tiene dónde ir */
static void campo_muere(Ctx *campo) { if (campo == D.campo) { D.campo = NULL; if (D.fase != DE_FIN) D.cancelar = 1; } }
static int d_tecla(Ctx *t, int k, void *dato)
{
    (void)t; (void)dato;
    int recien = D.fase == DE_FIN && hw_ms() - D.t_fase < 400u;     /* (revisión 8) la C que se repite al mantenerla no cierra y luego borra letras */
    if (k == K_AST || k == K_OK || k == K_SEL_IZQ) { if (D.fase == DE_HABLA) D.acabar = 1; else if (D.fase == DE_FIN && !recien) ui_quitar_ruta(&ClsDictado); }
    else if (k == K_C || k == K_ATRAS || k == K_SEL_DER) { if (D.fase == DE_FIN) { if (!recien) ui_quitar_ruta(&ClsDictado); } else D.cancelar = 1; }
    return 1;                                                         /* modal: todas las teclas son suyas */
}
static Widget *d_construir(Ctx *c)
{
    const TemaDatos *th = sh_tema();
    ShMicInfo mi;
    const char *linea, *izq, *der;
    u32 seg = D.n / 8000u, dec = D.n % 8000u / 800u;
    i32 pico;
    (void)c;
    sh_mic_info(&mi);
    {   u32 v = D.fase == DE_HABLA ? mi.pico : 0, b = 0;              /* el vúmetro en «decibelios» (6 dB por bit): la voz llega a ~-40 dBFS [observado] */
        while (v) { b++; v >>= 1; }
        pico = b <= 5 ? 0 : b >= 15 ? 100 : (i32)((b - 5) * 10); }
    switch (D.fase) {
    case DE_RED: case DE_PREPARA:
        linea = D.red_mia && sh_red_estado() != SH_RED_LISTA && hw_ms() - D.t0 > 3000u ? "Conectando con el computador… (con la radio apagada tarda 15-20 s)"
              : "Un momento: preparando el micrófono…";
        izq = ""; der = "Cancelar"; break;
    case DE_HABLA: linea = ui_fmt("Habla ahora. %u,%u s%s", seg, dec, D.red_mia && D.canal <= 0 ? " (esperando a la red)" : ""); izq = "Listo"; der = "Cancelar"; break;
    case DE_ENVIA: linea = D.canal > 0 ? "Reconociendo…" : "Esperando a la red con el computador…"; izq = ""; der = "Cancelar"; break;
    case DE_ESPERA: linea = "Reconociendo…"; izq = ""; der = "Cancelar"; break;
    default: linea = D.msj[0] ? D.msj : "Listo."; izq = ""; der = "Cerrar"; break;
    }
    return Teclas(d_tecla, Velo(Columna(.cruzado = P_ESTIRAR, .hijos = HIJOS(
        Expandido(Centro(RellenoXY(10, 4, Caja(.an = INF, .color = th->fondo, .borde = th->acento, .hijo = Relleno(1, Columna(.cruzado = P_ESTIRAR, .encoge = 1, .hijos = HIJOS(
            Caja(.an = INF, .color = th->barra, .hijo = RellenoXY(6, 4, Texto(D.fase == DE_HABLA ? "Dictado · escuchando" : "Dictado", .color = th->acento))),
            Relleno(6, Parrafo(linea)),
            RellenoXY(6, 2, Progreso(.valor = pico, .max = 100, .color = pico >= 100 ? 0xE04030 : th->acento, .fondo = th->barra, .al = 6)),
            Relleno(6, Texto(D.fase == DE_HABLA ? "* u OK: ya está · C: cancelar" : "C: cancelar", .color = th->tenue, .marcas = F_ESTILO(DIS_T_PEQUENA)))))))))),
        Caja(.an = INF, .al = dis_tokens()->m.alto_barra_teclas, .color = th->barra, .hijo = RellenoXY(4, 0, Fila(.cruzado = P_CENTRO, .hijos = HIJOS(
            Texto(izq), Hueco(), Texto(der)))))))));
}
COMPONENTE_CON_ESTADO(Dictado, DictadoE, d_construir, d_iniciar, .desechar = d_desechar);

/* el gancho de ui/ui_texto.c: la pulsación larga de «*» en un campo */
static int empezar(Ctx *campo)
{
    if (D.vivo) return 0;                                              /* ya hay uno en marcha (o su emergente aún está) */
    memset(&D, 0, sizeof D);
    D.campo = campo; D.t0 = hw_ms(); fase(DE_RED);
    if (!sh_mic_hay()) { msj("Este teléfono no tiene micrófono."); D.fallo = 1; fase(DE_FIN); }
    else {
        if (sh_red_hay()) { D.red_mia = sh_red_conectar(NULL, SH_BT_DICTADO) == 0; if (!D.red_mia) { msj(ui_fmt("Sin red con el computador (%s).", sh_red_fallo_nombre(sh_red_fallo()))); D.fallo = 1; fase(DE_FIN); } }
#if defined(TELEFONO) && TELEFONO
        else { msj("Sin radio Bluetooth: no hay dictado."); D.fallo = 1; fase(DE_FIN); }
#endif
        if (!D.fallo) {                                                /* la red YA es nuestra (si era de la música, no se corta al pararla) */
            if (sh_mic_abrir(SH_MIC_DICTADO) != HW_OK) { soltar_todo(); msj("El micrófono lo está usando una app."); D.fallo = 1; fase(DE_FIN); }
            else D.mic_mio = 1;
        }
    }
    hw_log("dictado: empieza\n");
    ui_empujar_emergente(UI_W(Dictado));
    return 1;
}
void sh_dictado_init(void)
{
    D.fase = DE_FIN;
    ui_campo_muere_gancho = campo_muere;
    if (sh_mic_hay()) ui_dictado_gancho = empezar;                    /* (revisión 10) sin micrófono (W600, W810) la «*» larga sigue como siempre */
}
int sh_dictado_activo(void) { return D.campo && D.fase != DE_FIN; }
