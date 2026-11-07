/* El main del W580i: arranca el hardware por la API, monta el shell y da vueltas al bucle de la UI.
 * Con -DHITO=n se compila en su lugar uno de los mains mínimos de plat/hitos_w580.c (docs/sistema.md §3). */
#include "hw.h"
#include "ui.h"
#include "shell.h"
#include "red_sis.h"                                /* (0.29.1) sh_red_arranque_paso */

Widget *sistema_raiz(void);

#if defined(HITO)
/* el main es el del hito: plat/hitos_w580.c */
#elif !defined(MEDIR)
int main(void)
{
#ifdef ARRANQUE_VIGILADO
    void hw_vigilado_armar(void), hw_vigilado_quitar(void);   /* (Fase 3b) plat/hw_w580.c: apagado por omisión; NUNCA en un binario para el teléfono hasta medirlo en silicio */
    hw_vigilado_armar();
#endif
    plat_init();
    hw_log("sistema " SISTEMA_VERSION "\n");
#ifdef VIGILADO_CUELGUE_PRUEBA
    hw_log("vigilado: cuelgue de prueba\n");            /* (Fase 3b) solo emulador: la prueba negativa de herr/bl/medir_vigilado.sh */
    for (;;) ;
#endif
    sh_pantalla_arranque("montando los ficheros…");   /* algo que mirar: el FS y el fondo tardan */
    MIGA("main: ui_init");
    ui_init(sistema_raiz);
int hw_flash_vedada(void);                         /* plat/hw_w580.c: la veda de la flash mientras suena música */
#ifdef MIGAS
    static int vuelta;                             /* (DIAG) migas solo en las dos primeras vueltas */
#define MIGA1(p) do { if (vuelta < 2) MIGA(p); } while (0)
#else
#define MIGA1(p) ((void)0)
#endif
#ifdef ARRANQUE_VIGILADO
    hw_vigilado_quitar();                          /* (Fase 3b) ha llegado al bucle: el arranque vale */
#endif
    for (;;) {
        MIGA1("bucle: latido");
        hw_perro_latido();                         /* (S7) el bucle da señales: el perro de software (plat/hw_w580.c) */
        plat_bucle();                              /* (R3, P3) a los 5 s de bucle vivo, este arranque cuenta como bueno */
        MIGA1("bucle: energia");
        sh_energia_paso();                         /* atenuar, apagar, tapa, bloqueo automático */
        sh_sonido_paso();                          /* la nota siguiente de la melodía que esté sonando */
        sh_musica_paso();                          /* la música: el PCM que toca, el cambio de pista, lo que hay que guardar */
        MIGA1("bucle: podometro");
        sh_pasos_paso();                           /* el podómetro: la fase que toque del sensor y una iteración del detector */
        MIGA1("bucle: luces");
        sh_luces_paso();                           /* (S5) las luces: el color, el latido y la orden siguiente del efecto que haya */
        MIGA1("bucle: usb");
        sh_usb_paso(0);                            /* (USBD) el cable USB, el núcleo USB y el respondedor MTP */
        sh_bt_paso();                              /* (BT) el Bluetooth, SOLO si alguien lo encendió (dormido por omisión: no toca nada) */
        sh_red_arranque_paso();                    /* (0.29.1) una sola llamada por arranque, y solo si SENUS emparejó este teléfono (bandera en NV_RED) */
        sh_mic_paso();                             /* (1.10) el arranque del micrófono, por pasos (nada si nadie lo abrió) */
        MIGA1("bucle: ui_paso (primer cuadro)");
#ifdef MIGAS
        vuelta++;
#endif
        if (!ui_paso()) {
            u32 d;
            /* (I1) un paso de recolección de la Memoria, si queda (W6F: condición 3 de la auditoría), pero SOLO con el usuario quieto
             * 1,5 s: un paso puede ser un borrado (0,2 s en el modelo; ~1 s en un M18 [sin saber]) y mientras tanto el bucle no
             * contesta; justo tras una operación es cuando más se va a pulsar. */
            if (sh_inactivo_ms() >= 1500u && !hw_flash_vedada() && hw_w6f_ocioso()) continue;   /* (2026-10-25) no con la música sonando (veda de la flash) */
            d = sh_dormir_ms();
            if (d) hw_dormir(d);                   /* pantalla apagada: reposo PROFUNDO hasta el próximo aviso */
            else plat_reposo();                    /* si no, WFI normal: despierta el tic de 4 ms */
        }
    }
}
#else
/* ---- guion de medidas (make medidas): cada línea «M nombre total construir disponer pintar volcar |
 * rectángulos píxeles | elementos montículo pico arena», en cuentas del contador de 32 768 Hz del
 * emulador. Con «-icount shift=10» una cuenta = ~29,8 instrucciones (se calibra al empezar). */
extern u32 medir_ms;
static void num(u32 v) { char t[12]; int n = 11; t[11] = 0; do { t[--n] = (char)('0' + v % 10); v /= 10; } while (v); hw_log(t + n); hw_log(" "); }
/* (pista O) la HUELLA de lo pintado tras cada cuadro medido: el lienzo (ui_pantalla) y lo que se mandó al panel (el búfer
 * de la plataforma, que empieza en _ram_fin). Va en una línea «H» aparte y FUERA de la medida: dos binarios que pintan lo
 * mismo dan las mismas líneas H. */
extern u16 _ram_fin[];
static u32 fnv(const u16 *p, u32 n) { u32 h = 2166136261u; while (n--) { h ^= *p++; h *= 16777619u; } return h; }
static void huella(void)
{
    static const char hx[] = "0123456789abcdef"; char t[20]; u32 a = fnv(ui_pantalla, UI_ANCHO * UI_ALTO), b = fnv(_ram_fin, UI_ANCHO * UI_ALTO); int i;
    for (i = 0; i < 8; i++) { t[i] = hx[(a >> (28 - 4 * i)) & 15]; t[9 + i] = hx[(b >> (28 - 4 * i)) & 15]; }
    t[8] = ' '; t[17] = '\n'; t[18] = 0; hw_log("H "); hw_log(t);
}
static int tecla_medida;                                     /* si no es 0, la tecla se entrega DENTRO de la medida (así cuenta decodificar una imagen) */
static void cuadro(const char *nombre)
{
    u32 c0 = hw_cuenta(), total = 0;
    if (tecla_medida) { ui_tecla(tecla_medida); tecla_medida = 0; }
    ui_paso();
    total = hw_cuenta() - c0;
    hw_log("M "); hw_log(nombre); hw_log(" ");
    num(total); num(ui_med.t_construir); num(ui_med.t_disponer); num(ui_med.t_pintar); num(ui_med.t_volcar);
    hw_log("| "); num(ui_med.dano_rects); num(ui_med.dano_px);
    hw_log("| "); num(ui_med.elementos); num(ui_med.monticulo_usado); num(ui_med.monticulo_pico); num(ui_med.arena_pico);
    hw_log("\n"); huella();
}
static void cuadro_real(const char *nombre)                  /* avanza de 20 en 20 ms hasta que de verdad sale un cuadro, y mide ese */
{
    u32 antes = ui_med.cuadros, c0, total; int i;
    for (i = 0; i < 100; i++) { medir_ms += 20; c0 = hw_cuenta(); ui_paso(); total = hw_cuenta() - c0; if (ui_med.cuadros != antes) break; }
    hw_log("M "); hw_log(nombre); hw_log(" ");
    num(total); num(ui_med.t_construir); num(ui_med.t_disponer); num(ui_med.t_pintar); num(ui_med.t_volcar);
    hw_log("| "); num(ui_med.dano_rects); num(ui_med.dano_px);
    hw_log("| "); num(ui_med.elementos); num(ui_med.monticulo_usado); num(ui_med.monticulo_pico); num(ui_med.arena_pico);
    hw_log("\n"); huella();
}
static void asentar(void) { int i; for (i = 0; i < 12; i++) { medir_ms += 40; ui_paso(); } }   /* acaba transiciones y animaciones */
static void pulsa(int k, const char *nombre) { tecla_medida = k; cuadro(nombre); }
__attribute__((target("arm"), noinline)) static void bucle(u32 n) { __asm__ volatile("1: subs %0, %0, #1\n bne 1b" : "+r"(n) : : "cc"); }

/* ---- EL MENÚ 3D (pista V; docs/menu3d.md §6): make w580 DEFS="-DMEDIR -DMEDIR_M3" B=build/medir_m3 -----------
 * Por cada estilo: abrir el menú (el primer cuadro hace la imagen del fondo), la transición, el cambio de foco,
 * el reposo (solo se mece la celda del foco), el cuadro ENTERO que se pinta cuando la inclinación cambia deprisa
 * (el objetivo se pone a mano con sh_inclina_prueba: el emulador de medidas no mueve el sensor), el cuadro fino que
 * sale al parar, el «vaivén» y el efecto «quieto».
 * El tiempo lo pone el guion (medir_ms): cada cuadro son 33 ms de reloj. */
#ifdef MEDIR_M3
#include "m3d.h"
static void varios(const char *nombre, int n) { int i; for (i = 0; i < n; i++) { medir_ms += 33; cuadro(nombre); } }
static void medir_m3(void)
{
    static const char *const est[M3_ESTILOS] = { "clasico", "walkman", "cristal", "neon", "medios", "escritorio" };
    int e;
    for (e = 0; e < M3_ESTILOS; e++) {
        hw_log("ESTILO "); hw_log(est[e]); hw_log("\n");
        aj.menu_estilo = (u8)e; aj.menu_efecto = SH_EFECTO_QUIETO;
        pulsa(K_SEL_IZQ, "abrir:_montar_el_menu_(y_hacer_el_fondo)");
        varios("abrir:_transicion", 7);
        asentar();
        medir_ms += 33; cuadro("quieto:_un_tic_sin_nada_que_hacer");
        pulsa(K_DER, "foco:_tecla");
        varios("foco:_animacion_(dos_celdas_y_el_titulo)", 6);
        asentar();
        if (e != M3_CLASICO) {
            aj.menu_efecto = SH_EFECTO_INCLINA;            /* el sensor del emulador no se mueve: solo se mece el foco */
            pulsa(K_ABAJO, "foco:_tecla_(despierta_el_tic)");
            asentar();
            varios("reposo:_solo_la_celda_del_foco", 8);
            /* inclinar DEPRISA: el objetivo salta de un tope al otro y lo pintado lo persigue cambiando en cada cuadro
             * (los iconos sin foco van bastos); al parar, a los 160 ms sale el cuadro entero fino */
            sh_inclina_prueba(1, 205, -205); varios("inclinar_deprisa:_cuadro_entero_(basto)", 6);
            sh_inclina_prueba(1, -205, 205); varios("inclinar_deprisa:_cuadro_entero_(basto)", 8);
            sh_inclina_prueba(1, 0, 0);      varios("inclinar_deprisa:_cuadro_entero_(basto)", 4);
            varios("al_parar:_reposo_y_el_cuadro_entero_fino", 14);
            sh_inclina_prueba(0, 0, 0);
            aj.menu_efecto = SH_EFECTO_VAIVEN;
            varios("vaiven:_cuadro_entero_fino_o_solo_el_foco", 30);
            aj.menu_efecto = SH_EFECTO_QUIETO;
            asentar();
            pulsa(K_OK, "abrir_app:_tecla");
            varios("abrir_app:_animacion_y_transicion", 8);
            asentar();
            pulsa(K_ATRAS, "volver_al_menu");
            varios("volver:_animacion", 6);
            asentar();
        }
        pulsa(K_ATRAS, "salir_al_standby");
        asentar();
    }
}
#endif

int main(void)
{
    u32 c0; int i;
    plat_init();
    hw_log("sistema W580: medidas\n");
    c0 = hw_cuenta(); bucle(1000000); c0 = hw_cuenta() - c0;
    hw_log("CAL 2000000 instrucciones = "); num(c0); hw_log("cuentas\n");
    c0 = hw_cuenta(); ui_init(sistema_raiz); c0 = hw_cuenta() - c0;
    hw_log("M montar_fs+decodificar_fondo "); num(c0); hw_log("0 0 0 0 | 0 0 | 0 0 0 0\n");
#ifdef MEDIR_M3
    cuadro("standby_primer_cuadro");
    medir_m3();
    hw_log("FIN\n");
    for (;;) ;
#endif
    cuadro("standby_primer_cuadro");
    medir_ms += 1000; cuadro("standby_pasa_un_segundo");
    pulsa(K_SEL_IZQ, "menu_montar+transicion_0");
    medir_ms += 40; cuadro("menu_transicion_cuadro");
    asentar();
    pulsa(K_DER, "menu_cambio_de_foco");
    medir_ms += 40; cuadro("menu_animacion_de_foco");
    asentar();
    pulsa(K_1, "archivos_montar_(lista_la_raiz)");
    asentar();
    pulsa(K_ABAJO, "archivos_bajar_una_fila");
    pulsa(K_OK, "archivos_entrar_en_carpeta");
    pulsa(K_SEL_IZQ, "emergente_abrir_(opciones)");
    pulsa(K_ABAJO, "emergente_mover_foco");
    pulsa(K_SEL_DER, "emergente_cerrar");
    pulsa(K_ATRAS, "archivos_subir"); pulsa(K_ATRAS, "archivos_salir_al_menu");
    pulsa(K_2, "imagenes_abrir+JPEG_176x220_(VultureSkate.jpg,_28_KB)");
    asentar();
    pulsa(K_DER, "imagenes_JPEG_176x220_(Tattoo.jpg,_18_KB)");
    pulsa(K_DER, "imagenes_JPEG_176x220_(Sun.jpg,_32_KB)");
    for (i = 0; i < 4; i++) pulsa(K_DER, "imagenes_siguiente_(GIF)");
    pulsa(K_DER, "imagenes_PNG_176x220_(Orange_Circles.png,_34_KB)");
    pulsa(K_ATRAS, "imagenes_salir");
    pulsa(K_4, "notas_abrir"); asentar();
    pulsa(K_OK, "notas_editor_abrir"); asentar();
    pulsa(K_4, "editor_una_letra");
    medir_ms += 1000; cuadro("editor_confirmar_letra");
    for (i = 0; i < 30; i++) { ui_tecla(K_2 + i % 8); medir_ms += 1000; ui_paso(); }
    pulsa(K_6, "editor_letra_con_3_lineas");
    pulsa(K_SEL_DER, "editor_salir"); pulsa(K_ATRAS, "notas_salir");
    pulsa(K_6, "serpiente_abrir"); asentar();
    cuadro_real("serpiente_un_paso");
    pulsa(K_ATRAS, "serpiente_salir");
    pulsa(K_9, "sistema_abrir"); asentar();
    medir_ms += 1000; cuadro("sistema_refresco_1s");
    pulsa(K_ATRAS, "sistema_salir");
    pulsa(K_AST, "ajustes_abrir"); asentar();
    pulsa(K_OK, "ajustes_cambiar_tema_(todo)");
    hw_log("FIN\n");
    for (;;) ;
}
#endif
