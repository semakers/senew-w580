/* App EXTERNA de ejemplo y de PRUEBA del mecanismo de docs/sistema.md §11. No vive en sistema/apps/:
 * se enlaza desde fuera, con `make w600 APPS_EXT=herr/app_ext_ejemplo`, y sin esa variable no está.
 * Hace lo justo para ejercitar las cuatro cosas del contrato: pantalla completa girada, memoria
 * prestada, teclas en crudo (varias a la vez) y salida limpia manteniendo Atrás. */
#include "app_externa.h"
#include "ui.h"                                 /* solo por los códigos K_* */

#define AN 220
#define AL 176
static u8 *tablero;                             /* memoria PRESTADA: nada de arreglos estáticos grandes */

/* Dos estáticos que la app ENSUCIA mientras corre, para probar APP_ESTATICOS_REINICIAR: uno con valor
 * inicial (va a .data) y otro sin él (va a .bss). Al entrar se dicen por la UART antes y después del
 * reinicio: después tienen que valer SIEMPRE 100 y 0, se haya entrado las veces que se haya entrado. */
static int est_data = 100;
static int est_bss;
static void log_n(const char *t, int v)
{
    char b[12]; int n = 11; unsigned u = (unsigned)(v < 0 ? -v : v);
    b[11] = 0; do { b[--n] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[--n] = '-';
    hw_log(t); hw_log(b + n);
}

static void dibujar(const AppMarco *m, u32 t, const u8 col[7])
{
    int x, y, f, c;
    for (y = 0; y < m->al; y++) {
        u16 *fila = m->pix + y * m->paso;
        for (x = 0; x < m->an; x++) {
            int v = (x + (int)t) & 255, w = (y * 2 + (int)t / 2) & 255;
            fila[x] = (u16)RGB(v, w, 128 + (tablero[(y / 16) * 16 + x / 16] & 63));
        }
    }
    for (f = 0; f < 7; f++)                     /* las teclas pulsadas, en crudo, como cuadritos */
        for (c = 0; c < 5; c++)
            if (col[f] & (1u << c)) {
                for (y = 0; y < 10; y++) for (x = 0; x < 10; x++)
                    m->pix[(4 + f * 12 + y) * m->paso + 4 + c * 12 + x] = (u16)RGB(255, 255, 0);
            }
}

/* La «partida» del ejemplo, en la zona de ficheros de usuario (hw_usr_*, api/app_externa.h §6): cuántas veces
 * se ha entrado, y detrás 3000 bytes que dependen de esa cuenta (para que haya algo que comprobar al leer).
 * Se lee al entrar y se reescribe ENTERA al salir; sobrevive a un reinicio y a apagar y encender. */
#define PARTIDA "ejemplo.dat"
#define PARTIDA_TAM 3004u
static u32 partida_leer(void)
{
    u8 *b = app_mem_reservar(PARTIDA_TAM);
    u32 veces = 0, i, mal = 0;
    int n = hw_usr_tam(PARTIDA), r;
    log_n("ejemplo_ext: hw_usr_tam(" PARTIDA ")=", n);
    if (n == (int)PARTIDA_TAM && b && (r = hw_usr_leer(PARTIDA, 0, b, PARTIDA_TAM)) == (int)PARTIDA_TAM) {
        veces = b[0] | (b[1] << 8) | (b[2] << 16) | ((u32)b[3] << 24);
        for (i = 4; i < PARTIDA_TAM; i++) if (b[i] != (u8)(i * 7 + veces)) mal++;
        log_n(" -> entradas anteriores=", (int)veces); log_n(", bytes que no cuadran=", (int)mal);
    } else hw_log(" -> no hay partida (o no hay zona de usuario)");
    hw_log("\n");
    return veces;
}
static void partida_escribir(u32 veces)
{
    u8 *b = app_mem_reservar(PARTIDA_TAM);
    u32 i;
    int r;
    if (!b) return;
    b[0] = (u8)veces; b[1] = (u8)(veces >> 8); b[2] = (u8)(veces >> 16); b[3] = (u8)(veces >> 24);
    for (i = 4; i < PARTIDA_TAM; i++) b[i] = (u8)(i * 7 + veces);
    r = hw_usr_escribir(PARTIDA, b, PARTIDA_TAM);
    log_n("ejemplo_ext: hw_usr_escribir(" PARTIDA ", entradas=", (int)veces); log_n(") = ", r); hw_log(r == HW_OK ? " (HW_OK)\n" : "\n");
}

/* (pista M2) Lo que una app externa puede traer además: su ICONO —una malla para el menú 3D y un dibujo de 1 bit
 * para el «Clásico»— y una MELODÍA que el sistema toca por el generador de tonos (api/app_externa.h §1 y §2c).
 * Aquí, lo mínimo que ejercita las tres cosas: una pirámide de dos caras, un triángulo y tres notas en bucle. */
static const signed char ej_pos[] = { 0, 80, 0,   -80, -60, 30,   0, -60, 90,   80, -60, 30 };
static const signed char ej_nor[] = { -68, 58, 90,   68, 58, 90 };
static const u8 ej_tri[] = { 0, 1, 2, 0, 0, 0,   0, 2, 3, 1, 1, 1 };       /* antihorario visto de frente; un papel cada cara */
static const AppMalla ej_malla = { ej_pos, ej_nor, ej_tri, 4, 2, { 1, 1, 0, 0, 0 }, { { 80, 200, 120 }, { 40, 120, 220 }, { 255, 255, 255 } } };
static const char *const ej_arte[12] = {
    "............", ".....##.....", ".....##.....", "....#..#....", "....#..#....", "...#....#...",
    "...#....#...", "..#......#..", "..#......#..", ".#........#.", ".##########.", "............" };
static const AppNota ej_melodia[] = { { 523, 400 }, { 0, 100 }, { 659, 400 }, { 0, 100 }, { 784, 400 }, { 0, 600 } };

#ifdef EJ_MALA
volatile u32 ej_mala_tics;                      /* si el gancho sigue latiendo tras salir, esto sigue subiendo */
static void ej_mala_tic(void) { ej_mala_tics++; }
#endif
static void entrar(void)
{
    AppMarco m;
    u32 siguiente, t = 0, atras_desde = 0;
    int i, r, avisos_vistos = 0;
    u32 veces;
    log_n("ejemplo_ext: al entrar est_data=", est_data); log_n(" est_bss=", est_bss);
    r = APP_ESTATICOS_REINICIAR(ejemplo);       /* lo PRIMERO: los estáticos, como en un arranque en frío */
    log_n("; tras reiniciar (r=", r); log_n(") est_data=", est_data); log_n(" est_bss=", est_bss); hw_log("\n");
    if (app_pantalla_tomar(&m, APP_GIRADA) != HW_OK) return;
    tablero = app_mem_reservar(16 * 16);        /* a cero, como un .bss */
    if (!tablero) { app_pantalla_soltar(); return; }
    veces = partida_leer();
    for (i = 0; i < 256; i++) tablero[i] = (u8)(i * 7);
    r = app_melodia(ej_melodia, sizeof ej_melodia / sizeof ej_melodia[0], 0);   /* en bucle; calla sola al soltar la pantalla */
    log_n("ejemplo_ext: app_melodia = ", r); log_n(", suena = ", app_melodia_suena()); hw_log("\n");
    siguiente = hw_ms();
    for (;;) {
        u8 col[7];
        u32 ahora = hw_ms();
        hw_teclas_mapa(col);
        if (hw_tecla_abajo(K_ATRAS)) { if (!atras_desde) atras_desde = ahora; else if (ahora - atras_desde > 1000) break; }
        else atras_desde = 0;
        t += 4;
        est_data += 7; est_bss++;               /* se ensucian a propósito */
        if (app_avisos() != avisos_vistos) {    /* algo ha saltado mientras jugamos (alarma, cuenta atrás, batería) */
            avisos_vistos = app_avisos();
            log_n("ejemplo_ext: el sistema avisa, app_avisos()=", avisos_vistos); hw_log("\n");
        }
#ifdef EJ_CUELGUE                               /* (S7) PRUEBA: a los ~2 s la app SE CUELGA de verdad —bucle infinito sin app_latido, sin
                                                 * hw_tecla_evento, sin soltar la pantalla— con el vibrador y un tono puestos. De ahí solo
                                                 * sacan el perro de software (30 s) o la pulsación larga de la tecla de encendido (11 s). */
        if (t > 240) {
            hw_vibrar(2000); hw_tono(440, 50);
            hw_log("ejemplo_ext: ME CUELGO a proposito (EJ_CUELGUE), con el vibrador y un tono puestos\n");
            for (;;) { }
        }
#endif
#ifdef EJ_MALA                                  /* (R2; auditoría del sistema, A1, A2 y §5) PRUEBA: una app con un fallo de los que la API deja
                                                 * cometer. A los ~2 s pone SU gancho del tic, un tono sin plazo y el vibrador, intenta pisar la
                                                 * clave de los AJUSTES del sistema y mover un pin, y VUELVE SIN SOLTAR LA PANTALLA. El sistema
                                                 * tiene que quedar visible y limpio: gancho fuera, tono y vibrador callados, ajustes intactos. */
        if (t > 240) {
            static const u8 basura[123] = { 1 };    /* un registro de ajustes «v1» de 123 bytes: el que desbordaba cargar_ajustes (M1) */
            log_n("ejemplo_ext: EJ_MALA: hw_tic_gancho=", hw_tic_gancho(ej_mala_tic));
            hw_tono(440, 50); hw_vibrar(2000);
            log_n(" hw_guardar(0x0100 AJUSTES)=", hw_guardar(0x0100, basura, sizeof basura));
            log_n(" hw_olvidar(0x0200 NOTAS)=", hw_olvidar(0x0200));
            log_n(" hw_guardar(0x6100 suya)=", hw_guardar(0x6100, basura, 4));
            log_n(" hw_gpio_modo(P1.1)=", hw_gpio_modo(1, 1, 0)); log_n(" hw_gpio_poner(P1.1)=", hw_gpio_poner(1, 1, 0));
            hw_log("\nejemplo_ext: EJ_MALA: VUELVO SIN SOLTAR LA PANTALLA\n");
            return;
        }
#endif
        siguiente += 33;                        /* ~30 Hz lógicos */
        if ((i32)(hw_ms() - siguiente) < 0) { dibujar(&m, t, col); app_pantalla_volcar(); }
        while ((i32)(hw_ms() - siguiente) < 0) hw_reposo();
        if ((i32)(hw_ms() - siguiente) > 200) siguiente = hw_ms();
    }
    log_n("ejemplo_ext: sale con est_data=", est_data); log_n(" est_bss=", est_bss); hw_log("\n");
    partida_escribir(veces + 1);
    app_pantalla_soltar();
}
APP_EXTERNA(ejemplo, "Ejemplo ext.", APP_IC_JUEGO, entrar, .arte = ej_arte, .malla = &ej_malla);
