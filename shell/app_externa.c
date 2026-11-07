/* La parte del sistema que sostiene a una APP EXTERNA a pantalla completa (docs/sistema.md §11).
 * Pista S3. Lo que hay aquí es lo que el shell presta y lo que recupera; el contrato, en
 * api/app_externa.h.
 *
 * Portable: va por hw_* y por el núcleo de la UI, así que una app externa se puede probar también en
 * el PC con un guion (el marco acaba en el panel simulado igual que cualquier otra pantalla). */
#include "shell.h"
#include "app_externa.h"

/* ---- la memoria prestada -------------------------------------------------------------------------
 * Desde el final de los estáticos del sistema (_fin, que pone el enlazador) hasta el final de la RAM.
 * Hoy son ~15 MiB que no usa nadie. Se reparte por avance y se devuelve de golpe: no hay fragmentación
 * posible y el sistema recupera el bloque entero en cuanto la app sale.
 * En el PC no hay un «_fin» que valga, así que el bloque es un arreglo estático de 8 MiB: suficiente
 * para probar la mecánica y el reparto, y no es la RAM de verdad (eso se prueba en el emulador). */
#if TELEFONO
extern char _fin[], _ram_fin[];           /* hal/enlace.ld: el final de lo que usa el sistema y el final de la RAM */
#define MEM_BASE  ((u8 *)(((size_t)_fin + 4095) & ~(size_t)4095))
#define MEM_TOPE  ((u8 *)_ram_fin)
#else
static u8 mem_pc[8u << 20];
#define MEM_BASE  (mem_pc)
#define MEM_TOPE  (mem_pc + sizeof mem_pc)
#endif

static u8 *mem_pos;                      /* NULL = el bloque no está tomado */
static u32 mem_pico;

void *app_mem_tomar(u32 *bytes)
{
    if (mem_pos) return NULL;            /* una sola app a la vez */
    mem_pos = MEM_BASE;
    if (bytes) *bytes = (u32)(MEM_TOPE - MEM_BASE);
    return MEM_BASE;
}
u32 app_mem_libre(void) { return mem_pos ? (u32)(MEM_TOPE - mem_pos) : (u32)(MEM_TOPE - MEM_BASE); }
void *app_mem_reservar(u32 n)
{
    u8 *p;
    if (!mem_pos && !app_mem_tomar(NULL)) return NULL;
    n = (n + 7u) & ~7u;
    if (n > (u32)(MEM_TOPE - mem_pos)) return NULL;
    p = mem_pos;
    mem_pos += n;
    if ((u32)(mem_pos - MEM_BASE) > mem_pico) mem_pico = (u32)(mem_pos - MEM_BASE);
    memset(p, 0, n);                     /* como el .bss que sustituye: a cero */
    return p;
}
void app_mem_soltar(void) { mem_pos = NULL; }
u32 app_mem_pico(void) { return mem_pico; }

static u32 ext_tecla_ms; static u8 ext_dormida;   /* (R2) la última tecla dentro de la app; panel y luz apagados por inactividad */
/* ---- la pantalla completa ------------------------------------------------------------------------- */
static u8 tomada;
static u16 *girado;                      /* en APP_GIRADA: el búfer de 220x176 donde dibuja la app */
static int modo_actual;
/* W580 (240x320): los dos marcos de app salen del tamaño del panel, no de números del W600.
 * Girada = el panel de lado (320x240); bandas = 4:3 a lo ancho (240x180), bandas arriba y abajo. */
#define APP_GIR_AN UI_ALTO
#define APP_GIR_AL UI_ANCHO
#define APP_BAN_AL (UI_ANCHO * 3 / 4)

int app_pantalla_tomar(AppMarco *m, int modo)
{
    if (tomada || !m) return HW_ERROR;
    ui_poner_giro(UI_GIRO_0);            /* (estética, giro) el marco de la app es el del panel de pie (la girada se gira aquí aparte) */
    if (modo == APP_GIRADA) {
        /* (M5X/AUD9) en PÁGINAS ENTERAS PROPIAS (alineado a 4 KiB y redondeado): PM lo abre a la app por páginas y no
         * puede compartir página con nada del sistema (las tablas de la ventana de recursos, p. ej.) */
        {   const u32 n = ((u32)APP_GIR_AN * APP_GIR_AL * sizeof(u16) + 4095u) & ~4095u;
            u8 *g = app_mem_reservar(n + 4096u);
            girado = g ? (u16 *)(((u32)(size_t)g + 4095u) & ~(size_t)4095u) : NULL; }
        if (!girado) return HW_ERROR;    /* sin memoria prestada no hay girado: que lo diga la app */
        m->pix = girado; m->an = APP_GIR_AN; m->al = APP_GIR_AL; m->paso = APP_GIR_AN;
    } else {
        modo = APP_BANDAS;
        m->pix = ui_pantalla + ((UI_ALTO - APP_BAN_AL) / 2) * UI_ANCHO;   /* bandas arriba y abajo */
        m->an = UI_ANCHO; m->al = APP_BAN_AL; m->paso = UI_ANCHO;
    }
    m->modo = modo_actual = modo;
    tomada = 1;
    sh_callar();                         /* el clic de la tecla que abrió la app (o lo que sonara): quien lo iba */
    hw_tono_fin();                       /* a apagar era el bucle del shell, y ese ya no corre. Lo encontró I1 */
    sh_musica_app();                     /* (28-oct) el MP3 sigue sonando (lo rellena app_latido); la del zumbador o el PCM se queda
                                          * en pausa (S7): el zumbador, mientras manda la app, es para los avisos. Se sigue con Play al salir */
    sh_luces_asentar();                  /* (S5) las luces, quietas y en su color: el bucle que acabaría un latido o un efecto ya no corre */
    while (hw_tecla_evento()) ;          /* la cola de sucesos del shell no es de la app: se vacía */
    sh_energia_suspender(1);             /* ni atenuar, ni apagar, ni bloquear, ni la tapa */
    hwo_claves_app(1);                   /* (R2) del almacén, solo sus claves (0x6000..0x6FFF) */
    ext_tecla_ms = hw_ms(); ext_dormida = 0;
    hw_perro_plazo(30);                  /* (S7) una app puede tardar en cargar: el perro le da 30 s sin latido (al sistema, 10) */
    hw_carga_fuera();                    /* (S7) el bucle del sistema deja de correr: la corriente de carga del PMU, a 0 otra vez (docs/bateria.md §8.A1) */
    hw_pantalla_dormir(0);
    hw_luz(0, aj.brillo < 10 ? 10 : aj.brillo);
    hw_luz_fase(HW_LUZ_VIVA);
    ui_pantalla_activa(0);               /* el shell deja de pintar: el marco es de la app */
    memset(ui_pantalla, 0, sizeof(Color) * UI_ANCHO * UI_ALTO);
    hw_pantalla_volcar(ui_pantalla, 0, UI_ALTO);
    return HW_OK;
}

/* El girado: la app dibuja el panel de lado (320x240 en el W580) y aquí se traspone al
 * marco del panel, que es 240x320. Son 76 800 píxeles movidos por cuadro; el coste está [sin medir].
 * Se recorre el DESTINO en orden (escrituras seguidas, que es lo que le importa a la caché de escritura)
 * y se salta por el origen. La app gira en el sentido en que se sujeta el teléfono con el teclado a la
 * derecha: la columna 0 del panel es la fila 0 de la app leída de abajo arriba. */
static void girar(void)
{
    const u16 *o = girado;
    u16 *d = ui_pantalla;
    int x, y;
    for (y = 0; y < UI_ALTO; y++)                    /* y del panel = x de la app */
        for (x = 0; x < UI_ANCHO; x++)               /* x del panel = 175 - y de la app */
            *d++ = o[(UI_ANCHO - 1 - x) * APP_GIR_AN + y];
}
/* ---- la melodía de la app por el generador de tonos (api/app_externa.h §2c, pista M2) --------------------
 * Una lista de { hz, ms } que avanza por hw_ms() desde el TIC (hw_tic_gancho; en el PC, desde app_latido()).
 * El zumbador se escribe con hw_tono_mantener (no reinicia la onda si ya da esa nota) y solo cuando NO suena
 * un sonido del sistema: el protocolo es el de la música (shell/musica.c): shell/sonido.c pone `s.n` antes de
 * tocar su primera nota y calla ANTES de soltarlo, así que mirar sh_sonando() desde la interrupción basta.
 * La exclusión con el bucle (app_melodia cambia la lista) es un cerrojo de un byte, como en la música.
 * El gancho del tic es UNO y lo tiene la música del sistema (que está en pausa mientras manda una app): se lo
 * pedimos al empezar y se lo devolvemos al callar (sh_musica_gancho). */
static struct {
    volatile u8 cerrojo, viva;
    u8  gancho;
    const AppNota *n;
    u32 cuantas, i;
    int repetir;
    u32 hasta;                           /* hw_ms() en que acaba la nota en curso */
} mel;

static void mel_tic(void)
{
    u32 ahora, hz;
    int k, ancho;
    if (mel.cerrojo || !mel.viva) return;
    ahora = hw_ms();
    if ((i32)(ahora - mel.hasta) > 500) mel.hasta = ahora;    /* el reloj dio un salto (la NOR, un reposo): la melodía no corre a alcanzarlo */
    for (k = 0; k < 8 && (i32)(ahora - mel.hasta) >= 0; k++) {   /* la nota en curso ha acabado: la siguiente empieza donde acababa esa */
        if (++mel.i >= mel.cuantas) {
            if (mel.repetir < 0 || (u32)mel.repetir >= mel.cuantas) { mel.viva = 0; break; }
            mel.i = (u32)mel.repetir;
        }
        mel.hasta += mel.n[mel.i].ms;
    }
    if (sh_sonando()) return;                                 /* un aviso del sistema: el zumbador es suyo */
    hz = mel.viva ? mel.n[mel.i].hz : 0;
    ancho = sh_sonido_ancho();                                /* el volumen del sistema; 0 = silencio */
    if (hz < 397 || hz > 20000 || !ancho) hz = 0;
    hw_tono_mantener(hz, ancho);
}

int app_melodia_suena(void) { return mel.viva; }
int app_melodia(const AppNota *notas, u32 cuantas, int repetir_desde)
{
    const int era = mel.viva || mel.gancho;
    mel.cerrojo = 1;
    mel.viva = 0;
    if (era && !sh_sonando()) hw_tono_mantener(0, 0);         /* lo que sonara de la melodía anterior, fuera */
    if (!notas || !cuantas || !tomada) {
        if (mel.gancho) { hwo_tic_sistema(NULL); mel.gancho = 0; sh_musica_gancho(); }   /* el tic vuelve a su dueña */
        mel.n = NULL; mel.cuantas = 0;
        mel.cerrojo = 0;
        return notas && cuantas ? HW_ERROR : HW_OK;
    }
    if (!sh_sonando()) hw_tono_mantener(0, 0);                /* desde el BUCLE la primera vez: abre los relojes y la ruta del pin */
    mel.n = notas; mel.cuantas = cuantas; mel.repetir = repetir_desde; mel.i = 0;
    mel.hasta = hw_ms() + notas[0].ms;
    mel.viva = 1;
    if (!mel.gancho) mel.gancho = hwo_tic_sistema(mel_tic) == HW_OK;    /* (R2) la ranura del SISTEMA: la melodía la toca el sistema; la de la app (hw_tic_gancho) es otra */
    mel.cerrojo = 0;
    return HW_OK;
}

/* El latido del sistema mientras manda la app (api/app_externa.h §2b): la melodía de un aviso avanza, y una
 * vez por segundo se miran la alarma, la cuenta atrás y la batería. Nada de esto pinta ni toca una tecla. */
/* (R2; auditoría del sistema, A4) UNA APP EXTERNA OLVIDADA NO DEJA LA PANTALLA ENCENDIDA PARA SIEMPRE. Con la energía
 * suspendida no había tope: un juego en pausa en un bolsillo era la luz al brillo de uso hasta agotar la batería. Ahora, a los
 * EXT_SIN_TECLAS_MS (10 min) sin ninguna tecla, la luz y el panel se apagan (la app sigue corriendo: es dueña del bucle; sus
 * volcados no se mandan); cualquier tecla los vuelve a encender. La app no se entera. */
#define EXT_SIN_TECLAS_MS 600000u
int app_pantalla_dormida(void) { return ext_dormida; }
static void ext_energia(u32 t)
{
    u8 col[7];
    if (hw_teclas_mapa(col) > 0) {
        ext_tecla_ms = t;
        if (ext_dormida) { ext_dormida = 0; hw_log("app externa: tecla; la pantalla vuelve\n"); hw_pantalla_dormir(0); hw_luz_fase(HW_LUZ_VIVA); hw_pantalla_volcar(ui_pantalla, 0, UI_ALTO); }
    } else if (!ext_dormida && t - ext_tecla_ms >= EXT_SIN_TECLAS_MS) {
        ext_dormida = 1;
        hw_log("app externa: 10 min sin teclas; luz y panel fuera (la app sigue)\n");
        hw_luz_fase(HW_LUZ_APAGADA); hw_pantalla_dormir(1);
    }
}
void app_latido(void)
{
    static u32 mirado;
    u32 t;
    if (!tomada) return;
    hw_perro_latido();               /* (S7) la app da señales: el perro de software (30 s sin esto con una app en pantalla = cuelgue) */
    sh_encendido_largo();            /* (S7) la tecla de encendido mantenida 8 s apaga también dentro de una app */
    sh_sonido_paso();
    sh_musica_paso();                /* (28-oct) el MP3 que sigue sonando: el colchón, el cambio de pista, la red; y en pausa, el cierre diferido del DSP */
    if (mel.viva && !mel.gancho) mel_tic();   /* sin gancho del tic (el PC): la melodía de la app se sondea aquí */
    sh_pasos_paso();                 /* el podómetro sigue contando con la app en pantalla */
    sh_usb_paso(1);                  /* (USBD) el USB sigue vivo (sin avisos ni MTP) con la app en pantalla */
    sh_bt_paso();                    /* (BT) y el Bluetooth, si la app lo encendió (un juego no vuelve al bucle del sistema) */
    sh_mic_paso();                   /* (1.10) y el arranque del micrófono */
    t = hw_ms();
    if (mirado && t - mirado < 1000) return;
    mirado = t ? t : 1;
    ext_energia(t);
    sh_vigilar_avisos(1);
    sh_cargador_vigilar(1);          /* (S7) el cargador y la batería se vigilan igual con la app en pantalla */
    sh_bateria_vigilar(1);
}
int app_avisos(void) { return sh_avisos_pendientes(); }

void app_pantalla_volcar(void)
{
    if (!tomada) return;
    app_latido();
    if (ext_dormida) return;                 /* (R2) panel dormido por inactividad: no se le manda nada */
    if (modo_actual == APP_GIRADA) { girar(); hw_pantalla_volcar(ui_pantalla, 0, UI_ALTO); }
    else hw_pantalla_volcar(ui_pantalla, (UI_ALTO - APP_BAN_AL) / 2, APP_BAN_AL);   /* solo las filas de la app: las bandas no cambian */
}
/* (1.1, M5X) W5_JG_PANEL de una app cargable: tomada en bandas, pero la app escribe el panel ENTERO (240x320) */
void app_pantalla_volcar_entera(void)
{
    if (!tomada) return;
    app_latido();
    if (ext_dormida) return;
    hw_pantalla_volcar(ui_pantalla, 0, UI_ALTO);
}
/* (R2; auditoría del sistema, A1/A2 y §5) Lo que una app puede haberse dejado puesto y soltar NO recogía: su gancho del tic
 * (seguiría latiendo sobre memoria devuelta), un tono sin plazo (hw_tono: sonaría para siempre), el vibrador y la restricción
 * de claves. Se recoge aquí, y aquí llegan TANTO app_pantalla_soltar COMO la vuelta de `entrar` (sh_app_externa_recoger). */
static void recoger_restos(void)
{
    if (hwo_tic_app_fuera()) hw_log("app externa: dejo puesto su gancho del tic; quitado\n");
    if (!sh_sonando()) hw_tono_fin();    /* (un aviso del sistema que esté sonando ahora no se corta) */
    hw_vibrar(0);
    hwo_claves_app(0);
}
void sh_app_externa_recoger(void)
{
    if (tomada) { hw_log("app externa: ha vuelto SIN soltar la pantalla; el sistema la recoge\n"); app_pantalla_soltar(); }
    else { recoger_restos(); app_mem_soltar(); }
}
void app_pantalla_soltar(void)
{
    if (!tomada) return;
    app_melodia(NULL, 0, 0);             /* (M2) la melodía de la app calla con ella, y el tic vuelve a la música */
    recoger_restos();
    if (ext_dormida) { ext_dormida = 0; hw_pantalla_dormir(0); }
    tomada = 0; girado = NULL;
    while (hw_tecla_evento()) ;          /* y lo que se pulsó DENTRO de la app no puede caerle al shell */
    app_mem_soltar();                    /* la memoria vuelve al sistema: eso era el encargo */
    ui_pantalla_activa(1);               /* repinta todo el shell */
    ui_forzar_todo();
    sh_energia_suspender(0);
    hw_perro_plazo(10);
    sh_actividad();                      /* la cuenta de inactividad empieza de cero al volver */
    sh_avisos_mostrar();                 /* lo que saltó jugando (alarma, cuenta atrás, batería): ahora sí, en pantalla */
}

/* ---- los estáticos de la app, como en un arranque en frío (api/app_externa.h §3b) --------------------- */
#if TELEFONO
extern char _data[], _edata[], _data_flash[], _bss[], _ebss[];
int app_estaticos_reiniciar(void *data_ini, void *data_fin, void *bss_ini, void *bss_fin)
{
    char *a = data_ini, *b = data_fin, *c = bss_ini, *d = bss_fin;
    if (a > b || c > d || a < _data || b > _edata || c < _bss || d > _ebss) return HW_ERROR;   /* el enlace no salió como se esperaba */
    memcpy(a, _data_flash + (a - _data), (size_t)(b - a));
    memset(c, 0, (size_t)(d - c));
    return HW_OK;
}
#else
/* PC: no hay imagen del .data en ninguna flash. Se guarda una foto la PRIMERA vez (que es antes de que la app
 * haya tocado nada, si cumple el contrato) y se repone las siguientes. A mano y sin sanitizador: entre dos
 * globales ASan pone zonas rojas, y un memcpy de una punta a otra las pisaría. */
#include <stdlib.h>
__attribute__((no_sanitize("address"), noinline))
int app_estaticos_reiniciar(void *data_ini, void *data_fin, void *bss_ini, void *bss_fin)
{
    static struct { char *a, *foto; size_t n; } f[4];
    volatile char *a = data_ini, *c = bss_ini;
    char *b = data_fin, *d = bss_fin;
    size_t i, n, k;
    if ((char *)a > b || (char *)c > d) return HW_ERROR;
    n = (size_t)(b - (char *)a);
    for (k = 0; k < 4 && f[k].a && f[k].a != (char *)a; k++) ;
    if (k == 4) return HW_ERROR;
    if (!f[k].a) {
        if (!(f[k].foto = malloc(n ? n : 1))) return HW_ERROR;
        f[k].a = (char *)a; f[k].n = n;
        for (i = 0; i < n; i++) ((volatile char *)f[k].foto)[i] = a[i];
    } else for (i = 0; i < n && i < f[k].n; i++) a[i] = ((volatile char *)f[k].foto)[i];
    for (i = 0; i < (size_t)(d - (char *)c); i++) c[i] = 0;
    return HW_OK;
}
#endif
int app_pantalla_tomada(void) { return tomada; }

/* ---- el registro: las apps externas que el enlazador haya metido -----------------------------------
 * Cada una deja un `const AppExterna` en la sección .apps_ext; hal/enlace.ld la delimita. Sin ninguna,
 * los dos símbolos valen lo mismo y aquí no se hace nada: el sistema es el de siempre. */
#if TELEFONO
extern const AppExterna _apps_ext[], _apps_ext_fin[];
#define EXT_INI _apps_ext
#define EXT_FIN _apps_ext_fin
#else
/* En el PC no hay guion de enlace nuestro: la sección existe igual (los descriptores llevan
 * `section(".apps_ext")`), pero los símbolos que la delimitan los pone el enlazador de GNU con el
 * nombre `__start_<sección>` / `__stop_<sección>`, y son débiles: sin ninguna app externa son NULL. */
extern const AppExterna __start_apps_ext[] __attribute__((weak));
extern const AppExterna __stop_apps_ext[] __attribute__((weak));
#define EXT_INI __start_apps_ext
#define EXT_FIN __stop_apps_ext
#endif

int sh_apps_externas(const AppExterna **v, int max)
{
    int n = 0;
    const AppExterna *p, *fin = EXT_FIN;
    if (hw_modo_seguro()) return 0;            /* (S7) en modo seguro NO hay apps externas */
    if (!(p = EXT_INI) || !fin) return 0;      /* en el PC los dos son débiles: sin apps externas, NULL */
    for (; p < fin && n < max; p++)
        if (p->nombre && p->entrar) v[n++] = p;
    return n;
}
