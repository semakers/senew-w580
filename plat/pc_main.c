/* El sistema en el PC, movido por un guion (sin emulador):  sistema_pc guion.txt [dir_capturas]
 * Órdenes: app NOMBRE (desde el menú) | tecla N | pulsa N | suelta N | espera MS | salta MS | azar N | captura NOMBRE |
 *          acel X Y (postura del acelerómetro, crudo) | andar DECIHZ AMPL (marcha simulada; 0 0 la para) | acelerometro |
 *          senal FICHERO.csv (señal t_ms,x,y de la pista E2, desde ahora) | pasos (estado del servicio del podómetro) | podometro no|corregido|fiel|sin-ahorro |
 *          serie TEXTO (llega por la UART; \n vale) | restablecer (tema, fondo y hora de fábrica) | medidas TEXTO | # comentario
 *          menu ESTILO EFECTO INTENSIDAD EJES (los ajustes del menú 3D) | inclina TEXTO (el estado del servicio de inclinación)
 *          giro 0|1|2|3 (estética: la orientación a mano) |
 *          usr N TAM (S6: deja N ficheros de TAM bytes en Mis archivos) | llenar (S6: la llena hasta el borde) | usuario TEXTO (S6: el estado y el listado de la zona de usuario)
 * Nombres de tecla: arriba abajo izq der ok sel_izq sel_der atras c 0..9 * # vol+ vol- web play
 *                   juego_a juego_b walkman camara bloqueo encendido
 * Tras CADA cuadro compara el panel (que solo recibe las filas volcadas) con un repintado completo:
 * si el pintado por regiones sucias se deja algo, «DESAJUSTE» y sale con error. */
#include "tarjeta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw.h"
#include "ui.h"
#include "pc.h"
#include "shell.h"

Widget *sistema_raiz(void);
static int desajustes;

static void ppm(const char *ruta, const Color *c)
{
    FILE *f = fopen(ruta, "wb"); int i;
    if (!f) { perror(ruta); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", UI_ANCHO, UI_ALTO);
    for (i = 0; i < UI_ANCHO * UI_ALTO; i++) {
        unsigned v = (unsigned)((c[i] >> 8) | (c[i] << 8)) & 0xFFFF;
        unsigned r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        fputc((int)((r << 3) | (r >> 2)), f); fputc((int)((g << 2) | (g >> 4)), f); fputc((int)((b << 3) | (b >> 2)), f);
    }
    fclose(f);
}

static u32 pc_vueltas_apagada, pc_vueltas_dormibles;
static u32 pc_dano_max;   /* (coordinador, 0.23) el MAYOR daño de un cuadro desde la última «medidas»: el último cuadro no basta
                            * cuando hay animación después (p. ej. el foco de la rejilla de Material) */
static void paso(void)
{
    static Color ref[UI_PIXELES];
    u32 antes = ui_med.cuadros;
    sh_energia_paso();                      /* como en el teléfono: atenuar, apagar, tapa y bloqueo automático */
    sh_sonido_paso();
    sh_musica_paso();
    sh_mic_paso();                          /* (1.10) el micrófono de mentira */
    sh_pasos_paso();
    sh_luces_paso();
    hw_perro_latido();                      /* (FATW) como el bucle del teléfono (plat/main_w580.c): el latido da el paso de la tarjeta (detectar, y dejarla LIMPIA al quedar quieta) */
    sh_usb_paso(0);                         /* (USBD) en el PC, «cargador 2» del guion es el cable USB (sin host detrás: plat/usb_dc_pc.c) */
    pc_sonido_avanzar(pc_reloj_ms);         /* el WAV del zumbador sigue al reloj virtual */
    if (!ui_paso() && sh_inactivo_ms() >= 1500u) hw_w6f_ocioso();   /* (I1) como el teléfono: la recolección de la Memoria en los ratos muertos */
    /* (R2; M10) lo que mide la auditoría: de las vueltas con la pantalla apagada, en cuántas dejaría dormir a fondo el sistema */
    if (sh_pantalla_estado() == EN_APAGADA) { pc_vueltas_apagada++; if (sh_dormir_ms()) pc_vueltas_dormibles++; }
    if (ui_med.cuadros != antes && ui_med.dano_px > pc_dano_max) pc_dano_max = ui_med.dano_px;
    if (ui_med.cuadros != antes && !pc_apagado) {       /* (apagado: la despedida se pinta a mano, sin árbol que comparar) */
        int i, mal = 0, x0 = UI_ANCHO, y0 = UI_ALTO, x1 = -1, y1 = -1;
        ui_repintar_en(ref);
        for (i = 0; i < UI_ANCHO * UI_ALTO; i++) if (ref[i] != pc_panel[i]) {
            const int x = i % UI_ANCHO, y = i / UI_ANCHO;
            mal++;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        if (mal) { desajustes++; if (desajustes < 20) fprintf(stderr, "DESAJUSTE en el cuadro %u: %d píxeles (t=%u ms), entre (%d, %d) y (%d, %d)\n", ui_med.cuadros, mal, pc_reloj_ms, x0, y0, x1, y1);
            if (desajustes <= 3 && getenv("PC_DESAJUSTE_DIR")) {   /* (estética) instrumento: el panel y el repintado completo del desajuste, en PPM */
                char r[512]; int k;
                for (k = 0; k < 2; k++) {
                    const Color *c = k ? ref : pc_panel; FILE *f;
                    snprintf(r, sizeof r, "%s/desajuste_%u_%s.ppm", getenv("PC_DESAJUSTE_DIR"), ui_med.cuadros, k ? "completo" : "panel");
                    if (!(f = fopen(r, "wb"))) continue;
                    fprintf(f, "P6\n%d %d\n255\n", UI_ANCHO, UI_ALTO);
                    for (i = 0; i < UI_ANCHO * UI_ALTO; i++) { u16 v = (u16)((c[i] >> 8) | (c[i] << 8)); fputc((v >> 11) << 3, f); fputc(((v >> 5) & 63) << 2, f); fputc((v & 31) << 3, f); }
                    fclose(f);
                }
            } }
    }
}
static void espera(u32 ms) { u32 fin = pc_reloj_ms + ms; while (pc_reloj_ms < fin && !pc_apagado) { pc_reloj_ms += 4; paso(); } }

static void memoria_arbol(const char *dir, int prof)   /* (I1) la orden «memoria»: cada entrada con su tamaño y una suma de su contenido */
{
    HwEntrada e; u32 i;
    for (i = 0; hw_fs_listar(dir, i, &e, 1) == 1; i++) {
        char r[300]; snprintf(r, sizeof r, "%s/%s", dir, e.nombre);
        if (e.es_dir) { printf(" [%s/", r + sizeof HW_FS_MEMORIA - 1); if (prof < 16) memoria_arbol(r, prof + 1); printf("]"); }
        else {
            static u8 b[4096]; u32 d = 0, n, h = 2166136261u, k;
            while ((n = hw_fs_leer_desde(r, d, b, sizeof b)) > 0) { for (k = 0; k < n; k++) { h ^= b[k]; h *= 16777619u; } d += n; }
            printf(" [%s %u leidos %u fnv %08x]", r + sizeof HW_FS_MEMORIA - 1, e.tam, d, h);
        }
    }
}
static const char *const nombres[K_N] = { "", "arriba", "abajo", "izq", "der", "ok", "sel_izq", "sel_der", "atras", "c",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#", "vol+", "vol-", "web", "play", "juego_a", "juego_b",
    "walkman", "camara", "bloqueo", "encendido" };
static int nombre_tecla(const char *s)
{
    int i;
    for (i = 1; i < K_N; i++) if (!strcmp(s, nombres[i])) return i;
    fprintf(stderr, "tecla desconocida: %s\n", s); exit(1);
}

/* ---- la BOMBA del guion ---------------------------------------------------------------------------
 * Mientras una app EXTERNA tiene el bucle (docs/sistema.md §11), main() ya no lee el guion: lo lee
 * esto, que la plataforma llama desde cada hw_reposo() (el WFI del teléfono). Así el reloj virtual
 * avanza y el guion puede seguir mandando teclas y pidiendo capturas DENTRO de la app. Solo entiende
 * las órdenes que tienen sentido ahí: tecla, pulsa, suelta, espera, captura y medidas. */
static FILE *guion;
static const char *dir_capturas = ".";
static u32 bomba_hasta;
static int en_bomba;

static int ejecutar(char *lin, int dentro);

static void bomba(void)
{
    char lin[512];
    if (en_bomba || !guion) return;
    if ((i32)(pc_reloj_ms - bomba_hasta) < 0) return;
    en_bomba = 1;
    while (fgets(lin, sizeof lin, guion)) if (ejecutar(lin, 1)) break;
    en_bomba = 0;
}

static int ejecutar(char *lin, int dentro)
{
    static u32 pico;
    char a[256], ruta[800];
    {   /* (MTP, ronda 3) las órdenes «usbhost …» de la variante con host USB falso (plat/pc_usb_host.c, make pc-usb) */
        extern int pc_guion_extra(char *l) __attribute__((weak));
        if (pc_guion_extra) { int r = pc_guion_extra(lin); if (r >= 0) { if (!dentro) paso(); return r; } }
    }
    if (sscanf(lin, "tecla %255s", a) == 1) { pc_tecla(nombre_tecla(a)); if (!dentro) { paso(); paso(); } return 1; }
    if (sscanf(lin, "pulsa %255s", a) == 1) { pc_mantener(nombre_tecla(a), 1); if (!dentro) paso(); return 1; }
    if (sscanf(lin, "suelta %255s", a) == 1) { pc_mantener(nombre_tecla(a), 0); if (!dentro) paso(); return 1; }
    if (sscanf(lin, "espera %255s", a) == 1) {
        if (dentro) { bomba_hasta = pc_reloj_ms + (u32)atoi(a); return 1; }
        espera((u32)atoi(a)); return 1;
    }
    if (sscanf(lin, "captura %255s", a) == 1) { snprintf(ruta, sizeof ruta, "%s/%s.ppm", dir_capturas, a); ppm(ruta, pc_panel); return 1; }
    if (!strncmp(lin, "medidas", 7)) {
        lin[strcspn(lin, "\n")] = 0;
        printf("%-26s t=%7u ms cuadros=%5u dano=%5u px/%u rect max=%5u px | elementos=%4u monticulo=%6u (pico %6u) arena pico=%5u rutas=%d\n",
               lin + 7, pc_reloj_ms, ui_med.cuadros, ui_med.dano_px, ui_med.dano_rects, pc_dano_max,
               ui_med.elementos, ui_med.monticulo_usado, ui_med.monticulo_pico, ui_med.arena_pico, ui_rutas());
        pc_dano_max = 0;
        return 0;
    }
    if (!strncmp(lin, "energia", 7)) {                 /* (S7) el estado de la energía, a la salida */
        lin[strcspn(lin, "\n")] = 0;
        printf("energia%-22s t=%7u ms | bateria %d mV %d %% | cargador %d | pantalla %s | apagado en falso %d | hw_apagar fallidos %d | carga fuera %d veces | dormible %u de %u vueltas | rutas %d\n",
               lin + 7, pc_reloj_ms, hw_bateria_mv(), hw_bateria_pct(), pc_cargador_bits,
               sh_pantalla_estado() == EN_VIVA ? "viva" : sh_pantalla_estado() == EN_ATENUADA ? "atenuada" : "apagada", sh_apagado_en_falso(), pc_apagados_fallidos, pc_cargas_fuera, pc_vueltas_dormibles, pc_vueltas_apagada, ui_rutas());
        return 0;
    }
    if (dentro) {                                      /* dentro de una app externa no vale todo */
        if (lin[0] != '#' && lin[0] != '\n') fprintf(stderr, "orden ignorada dentro de una app externa: %s", lin);
        return 0;
    }
    if (!strncmp(lin, "celdas", 6)) {                  /* (LANZ) las celdas del menú: nombre, QUÉ son (id) y su malla */
        int i; lin[strcspn(lin, "\n")] = 0;
        printf("celdas%s: %d\n", lin + 6, sh_n_apps);
        for (i = 0; i < sh_n_apps; i++) printf("  %2d %-14s %-28s malla %d%s\n", i + 1, sh_apps[i].nombre, sh_apps[i].id ? sh_apps[i].id : "(sin id)",
                                               sh_apps[i].malla, sh_apps[i].entrar ? " (juego)" : "");
        return 0;
    }
    if (!strncmp(lin, "abierta", 7)) {                 /* (LANZ) qué hay abierto ahora: la .sen viva (si la hay) y cuántas pantallas */
        extern const char *cg_ruta_viva(void); const char *r = cg_ruta_viva();
        lin[strcspn(lin, "\n")] = 0;
        printf("abierta%s: %s, rutas %d\n", lin + 7, r ? r : "(ninguna .sen)", ui_rutas());
        return 0;
    }
    if (sscanf(lin, "app %255[^\n]", a) == 1) {       /* desde el menú: abre esa app por su atajo (el nombre puede llevar espacios) */
        static const int atajo[12] = { K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_AST, K_0, K_ALM };
        int i;
        for (i = 0; i < sh_n_apps && strcmp(sh_apps[i].nombre, a); i++) ;
        if (i == sh_n_apps) { fprintf(stderr, "app desconocida: %s\n", a); exit(1); }
        if (i < 12) { pc_tecla(atajo[i]); paso(); espera(400); }
        else {                                         /* las celdas 13.. no tienen atajo: con las flechas desde la primera */
            int j;
            for (j = 0; j < i / 3; j++) { pc_tecla(K_ABAJO); paso(); espera(200); }
            for (j = 0; j < i % 3; j++) { pc_tecla(K_DER); paso(); espera(200); }
            pc_tecla(K_OK); paso(); espera(400);
        }
        return 1;
    }
    if (sscanf(lin, "salta %255s", a) == 1) { pc_reloj_ms += (u32)atoi(a); return 1; }
    if (!strncmp(lin, "serie ", 6)) {
        char *o = lin + 6, *d = a;
        for (; *o && *o != '\n'; o++) { if (o[0] == '\\' && o[1] == 'n') { *d++ = '\n'; o++; } else *d++ = *o; }
        *d = 0; pc_serie_rx(a); paso();
        return 1;
    }
    if (sscanf(lin, "azar %255s", a) == 1) {           /* N teclas al azar (todas las del teléfono), con esperas al azar */
        static const int t[] = { K_ARRIBA, K_ABAJO, K_ABAJO, K_IZQ, K_DER, K_OK, K_OK, K_OK, K_SEL_IZQ, K_SEL_IZQ, K_SEL_DER, K_ATRAS, K_C,
                                 K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_AST, K_ALM, K_VOL_MAS, K_VOL_MENOS, K_DER, K_ABAJO, K_OK };
        unsigned sa = 12345; int n = atoi(a);
        extern int pc_azar; pc_azar = 1;
        while (n--) {
            sa = sa * 1103515245u + 12345u; pc_tecla(t[(sa >> 16) % (sizeof t / sizeof t[0])]);
            paso();
            sa = sa * 1103515245u + 12345u; espera(((sa >> 16) % 5) * ((sa >> 20) % 2 ? 4 : 60));
            if (ui_med.monticulo_usado > pico) pico = ui_med.monticulo_usado;
            if (n % 400 == 0) { pc_mantener(K_ATRAS, 1); espera(1300); pc_mantener(K_ATRAS, 0); }   /* de Teclas solo se sale así */
        }
        pc_azar = 0;
        printf("azar: %s teclas, montículo ahora %u, pico %u, rutas %d\n", a, ui_med.monticulo_usado, ui_med.monticulo_pico, ui_rutas());
        return 1;
    }
    if (!strncmp(lin, "sonido", 6)) {                  /* la traza del generador de tonos: qué se pidió y cuándo */
        printf("--- sonido: %u marcos de WAV, %u sonando (%.3f s) ---\n%s",
               pc_tono_marcos, pc_tono_sonando_marcos, pc_tono_sonando_marcos / 32000.0, pc_tono_traza);
        return 0;
    }
    if (sscanf(lin, "bateria %255s", a) == 1) { pc_bateria(atoi(a)); paso(); return 1; }           /* N <= 100: %; si no, mV */
    if (!strncmp(lin, "flash", 5)) {                   /* (S7) cuánto se ha escrito en la flash simulada (para probar «0 escrituras») y cómo está protegida */
        extern u32 pc_usr_escrituras; HwNvEstado nv; HwUsrEstado us;
        hw_nv_estado(&nv); hw_usr_estado(&us);
        lin[strcspn(lin, "\n")] = 0;
        printf("flash%-24s almacen: %u programas, %u borrados, montado %d, escribible %d | usuario: %u escrituras, montado %d, escribible %d | proteccion %d, formato pendiente %d, modo seguro %d | tema %d, fondo [%s], menu %d, pasos %d, luces %d, apagar %d s\n",
               lin + 5, nv.programas, nv.borrados, nv.montado, nv.escribible, pc_usr_escrituras, us.montado, us.escribible,
               hw_nor_proteccion(), hw_formato_pendiente(), hw_modo_seguro(), aj.tema, aj.fondo, aj.menu_estilo, aj.pasos, aj.luces, aj.apagar_s);
        return 0;
    }
    {   /* (R2) nv CLAVE N VALOR: guarda en el almacén esa clave (hex) con N bytes de ese valor; N < 0 la olvida. nv_leer CLAVE: la enseña */
        unsigned c; int n, v;
        if (sscanf(lin, "nv %x %d %d", &c, &n, &v) == 3) {
            u8 b[400]; if (n > 400) n = 400;
            if (n < 0) { printf("nv %04X: hw_olvidar = %d\n", c, hw_olvidar((u16)c)); return 1; }
            memset(b, v, (size_t)n); printf("nv %04X: %d bytes de %d, hw_guardar = %d\n", c, n, v, hw_guardar((u16)c, b, (u32)n)); return 1;
        }
        if (sscanf(lin, "nv_leer %x", &c) == 1) {
            u8 b[401]; int i; n = hw_cargar((u16)c, b, 400);
            printf("nv_leer %04X: %d bytes [", c, n); for (i = 0; i < n && i < 60; i++) putchar(b[i] >= 32 && b[i] < 127 ? b[i] : '.'); printf("]\n"); return 1;
        }
    }
    if (sscanf(lin, "ajustes_danados %255s", a) == 1) {  /* (S7) deja en el almacén un registro de ajustes DAÑADO: cabecera v1 y N bytes en total, sin cero final */
        u8 b[400]; int n = atoi(a), i;                   /* (auditoría C1: con 123 bytes el fondo se salía 15 bytes de `aj`) */
        if (n > (int)sizeof b) n = (int)sizeof b;
        b[0] = 1; b[1] = 2; b[2] = 1; b[3] = 0; b[4] = 7; b[5] = 30; b[6] = 0; b[7] = 0; b[8] = 80;
        for (i = 9; i < n; i++) b[i] = (u8)('A' + i % 26);
        printf("ajustes_danados: registro de %d bytes, hw_guardar = %d\n", n, hw_guardar(NV_AJUSTES, b, (u32)n));
        return 1;
    }
    if (sscanf(lin, "fondo %255s", a) == 1) { sh_poner_fondo(a); paso(); printf("fondo [%s]: %s\n", aj.fondo, sh_fondo() ? "decodificado" : "NO se pudo decodificar (sin fondo)"); return 1; }
    if (sscanf(lin, "descarga %255s", a) == 1) { pc_descarga(atoi(a)); return 1; }                  /* (S7) mV por segundo de reloj virtual */
    if (sscanf(lin, "apagable %255s", a) == 1) { pc_apagable = atoi(a); return 1; }                 /* (S7) 1 = hw_apagar apaga; 2 = no apaga (cargador) */
    if (sscanf(lin, "cargador %255s", a) == 1) { pc_cargador_bits = atoi(a); paso(); return 1; }
    { int x, y;
      if (sscanf(lin, "acel %d %d", &x, &y) == 2) { pc_acel(x, y); return 1; }
      if (sscanf(lin, "andar %d %d", &x, &y) == 2) { pc_andar(x, y); return 1; } }
    if (sscanf(lin, "senal %255s", a) == 1) { extern int pc_acel_senal(const char *); printf("senal %s: %d puntos\n", a, pc_acel_senal(a)); return 1; }
    if (sscanf(lin, "podometro %255s", a) == 1) {       /* podometro no|corregido|fiel|sin-ahorro: como el ajuste, sin pasar por la pantalla */
        sh_pasos_ajuste(strcmp(a, "no") != 0, !strcmp(a, "fiel") ? SH_PASOS_FIEL : !strcmp(a, "sin-ahorro") ? SH_PASOS_SIN_AHORRO : SH_PASOS_CORREGIDO);
        return 1;
    }
    if (!strncmp(lin, "pasos", 5)) {                   /* el estado del servicio del podómetro (shell/pasos.c) */
        const Podometro *p = sh_pasos(); PasosInfo i; sh_pasos_info(&i);
        printf("pasos: %u pasos, %.3f m, %.2f km/h | t=%u ms | %u muestras + %u iteraciones sin sensor, %u tarde, %u fallos | %s%s, ahorro: %s%s\n",
               p->pasos, (double)p->dist, (double)p->kmh, pc_reloj_ms, i.muestras, i.sin_muestra, i.tarde, i.fallos,
               !i.activo ? "PARADO" : i.dormido ? "sensor DORMIDO" : "midiendo", p->vig || p->ventana ? " (vigilando)" : "",
               !p->vigilar_si ? "sin ahorro" : p->modo == POD_FIEL ? "fiel" : "corregido", aj.pasos ? "" : " (ajuste apagado)");
        return 0;
    }
    { int e, f, i, j;                                  /* menu ESTILO EFECTO INTENSIDAD EJES: los ajustes del menú 3D, sin pasar por la pantalla */
      if (sscanf(lin, "menu %d %d %d %d", &e, &f, &i, &j) == 4) {
          aj.menu_estilo = (u8)e; aj.menu_efecto = (u8)f; aj.menu_intensidad = (u8)i; aj.menu_ejes = (u8)j;
          sh_guardar_ajustes(); ui_reconstruir_todo(); paso(); return 1;
      } }
    if (!strncmp(lin, "inclina", 7)) {                 /* el estado del servicio de inclinación (shell/inclina.c) */
        InclinaInfo i; sh_inclina_info(&i);
        lin[strcspn(lin, "\n")] = 0;
        printf("inclina%-22s t=%7u ms cuadros=%5u | crudo %4d %4d | plano %5d %5d filtrado %5d %5d (cuentas x16) | objetivo %4d %4d pintado %4d %4d (1/4096 de vuelta) | %u muestras, sensor %d, retenido %d\n",
               lin + 7, pc_reloj_ms, ui_med.cuadros, i.crudo[0], i.crudo[1], i.plano[0], i.plano[1], i.filtrado[0], i.filtrado[1],
               i.objetivo[0], i.objetivo[1], i.pintado[0], i.pintado[1], i.muestras, i.sensor, i.retenido);
        return 0;
    }
    if (!strncmp(lin, "luces", 5)) {                   /* (S5) las seis luces simuladas, tal como se «verían» ahora (plat/hw_pc.c) */
        HwLuces l; hw_luz_leer(&l);
        lin[strcspn(lin, "\n")] = 0;
        printf("luces%-24s t=%7u ms | fase %d%s | pantalla %02X teclado1 %02X teclado2 %02X deslizador %02X | lateral1 %02X %02X %02X lateral2 %02X %02X %02X (pwm %02X; escrito %02X %02X %02X) | %u ordenes\n",
               lin + 5, pc_reloj_ms, l.fase, l.efecto ? " EFECTO" : "", l.nivel[HW_LED_PANTALLA], l.nivel[HW_LED_TECLADO_1], l.nivel[HW_LED_TECLADO_2], l.nivel[HW_LED_TECLADO_DESL],
               l.rgb[0][0], l.rgb[0][1], l.rgb[0][2], l.rgb[1][0], l.rgb[1][1], l.rgb[1][2], l.pwm, l.rgb_nivel[0][0], l.rgb_nivel[0][1], l.rgb_nivel[0][2], l.escrituras);
        return 0;
    }
    if (!strncmp(lin, "acelerometro", 12)) { printf("acelerómetro: %u lecturas, %u sin alimentar\n", pc_acel_lecturas, pc_acel_sin_alimentar); return 0; }
    { int n, t;                                        /* (S6) usr N TAM: deja N ficheros «relleno TAM-NN.bin» de TAM bytes en Mis archivos, sin pasar por la pantalla */
      if (sscanf(lin, "usr %d %d", &n, &t) == 2) {      /* (para llegar al tope de 64 ficheros y a la zona llena sin miles de teclas) */
          static u8 rel[65536]; int i, bien = 0;
          if (t > (int)sizeof rel) t = (int)sizeof rel;
          for (i = 0; i < t; i++) rel[i] = (u8)(i * 7 + n);
          for (i = 1; i <= n; i++) { char nom[32]; snprintf(nom, sizeof nom, "relleno %d-%02d.bin", t, i); bien += hw_usr_escribir(nom, rel, (u32)t) == HW_OK; }
          printf("usr: %d ficheros de %d bytes pedidos, %d escritos (motivo del ultimo: %d)\n", n, t, bien, hw_usr_motivo());
          paso(); return 1;
      } }
    if (!strncmp(lin, "usr-texto ", 10)) {             /* (AUD B6) usr-texto NOMBRE|bytes con \r \n \0: un fichero «de fuera» en Mis archivos */
        static char b[512]; char *x = lin + 10, *y; int n = 0;
        lin[strcspn(lin, "\n")] = 0; y = strchr(x, '|'); if (!y) return 0; *y++ = 0;
        for (; *y && n < (int)sizeof b; y++) { if (*y == '\\' && y[1]) { y++; b[n++] = *y == 'r' ? '\r' : *y == 'n' ? '\n' : *y == '0' ? 0 : *y; } else b[n++] = *y; }
        printf("usr-texto %s: %s\n", x, hw_usr_escribir(x, b, (u32)n) == HW_OK ? "escrito" : "NO escrito");
        return 0;
    }
    if (!strncmp(lin, "copiar-sin-mirar ", 17)) {     /* (AUD8 A1) la API sin la interfaz: copiar-sin-mirar memoria|usr DESTINO|ORIGEN, con reemplazar */
        char *x = lin + 17, *d, *o; int m, r;
        lin[strcspn(lin, "\n")] = 0; d = strchr(x, ' '); if (!d) return 0; *d++ = 0; o = strchr(d, '|'); if (!o) return 0; *o++ = 0;
        m = !strcmp(x, "memoria") ? 1 : !strcmp(x, "tarjeta") ? 2 : 0;   /* (FATW) y a la tarjeta */
        r = m == 2 ? hw_tj_copiar(d, o, 1) : m ? hw_w6f_copiar(d, o, 1) : hw_usr_copiar(d, o, 1);
        if (r == HW_OK) { u32 h, t; while ((m == 2 ? hw_tj_paso(65536, &h, &t) : m ? hw_w6f_paso(65536, &h, &t) : hw_usr_paso(65536, &h, &t)) == 1) ; r = HW_OK; }
        printf("copiar-sin-mirar %s: %s motivo %d\n", x, r == HW_OK ? "HW_OK" : "HW_ERROR", m == 2 ? hw_tj_motivo() : m ? hw_w6f_motivo() : hw_usr_motivo());
        return 0;
    }
    if (!strncmp(lin, "tarjeta-estado", 14)) {         /* (AUD8 M1) la tarjeta: estado, lecturas y si la raíz enseña su fila */
        HwTarjeta t; HwEntrada e; u32 i; int fila = 0;
        hw_tarjeta_estado(&t);
        for (i = 0; hw_fs_listar("/", i, &e, 1) == 1; i++) if (!strncmp(e.nombre, "tarjeta", 7)) fila = 1;
        printf("tarjeta-estado: estado %d, lecturas %u, fila en la raiz %d; escribible %d protegida %d sucia %d escritos %u errores_w %u arreglos %u\n",
               t.estado, t.lecturas, fila, t.escribible, t.protegida, t.sucia, t.escritos, t.errores_w, t.arreglos);
        return 0;
    }
    if (!strncmp(lin, "fs-escribir ", 12)) {           /* (FATW) hw_fs_escribir RUTA|texto (en la tarjeta, en la Memoria…) */
        char *r = lin + 12, *t; lin[strcspn(lin, "\n")] = 0; t = strchr(r, '|'); if (!t) return 0; *t++ = 0;
        printf("fs-escribir %s: %d\n", r, hw_fs_escribir(r, (const u8 *)t, (u32)strlen(t)));
        return 0;
    }
    if (!strncmp(lin, "fs-borrar ", 10)) { lin[strcspn(lin, "\n")] = 0; printf("fs-borrar %s: %d\n", lin + 10, hw_fs_borrar(lin + 10)); return 0; }
    if (!strncmp(lin, "tj-op ", 6)) {                  /* (FATW) tj-op mkdir|borrar|mover RUTA[|RUTA2] */
        char *o = lin + 6, *a, *b; int r = -99; lin[strcspn(lin, "\n")] = 0; a = strchr(o, ' '); if (!a) return 0; *a++ = 0; b = strchr(a, '|'); if (b) *b++ = 0;
        if (!strcmp(o, "mkdir")) r = hw_tj_mkdir(a); else if (!strcmp(o, "borrar")) r = hw_tj_borrar(a); else if (!strcmp(o, "mover") && b) r = hw_tj_renombrar_r(a, b, 0);
        printf("tj-op %s %s: %d motivo %d\n", o, a, r, hw_tj_motivo());
        return 0;
    }
    if (!strncmp(lin, "usr-borrar ", 11)) { lin[strcspn(lin, "\n")] = 0; printf("usr-borrar: %d\n", hw_usr_borrar(lin + 11)); return 0; }   /* (AUD8 A1) */
    if (!strncmp(lin, "usr-hex ", 8)) {                /* (AUD B6) los bytes de un fichero de Mis archivos, en hexadecimal */
        static u8 b[512]; int n, i;
        lin[strcspn(lin, "\n")] = 0; n = hw_usr_leer(lin + 8, 0, b, sizeof b);
        printf("usr-hex %s:", lin + 8); for (i = 0; i < n; i++) printf(" %02x", b[i]); printf("\n");
        return 0;
    }
    if (!strncmp(lin, "editar ", 7)) {                 /* (AUD B6) el editor de archivos de Notas sobre ese fichero (lo que abre Archivos; APX: Notas es un .sen) */
        lin[strcspn(lin, "\n")] = 0; sh_abrir_sen("/apps/notas.sen", lin + 7); paso(); espera(300); return 1;
    }
    if (!strncmp(lin, "memoria-app ", 12)) {           /* (APX) memoria-app X.sen: una app de la carpeta SEN_PC (las .sen de PC de apps_ext) a la
                                                        * Memoria, /memoria/apps/X.sen, como si llegara por «Recibir por serie»: contenido del usuario,
                                                        * NO del sistema (los juegos no vienen de fábrica). El menú la ve en cuanto vuelve al standby */
        static u8 b[65536]; char ruta[300]; const char *dir = getenv("SEN_PC"); FILE *f; size_t n; int r = HW_ERROR; HwW6fEstado e;
        lin[strcspn(lin, "\n")] = 0;
        snprintf(ruta, sizeof ruta, "%s/%s", dir ? dir : ".", lin + 12);
        hw_w6f_estado(&e);
        if (e.situacion == HW_W6F_VIRGEN) hw_w6f_formatear();
        hw_w6f_mkdir("/apps");
        if ((f = fopen(ruta, "rb"))) {
            snprintf(ruta, sizeof ruta, "/apps/%s", lin + 12);
            hw_w6f_borrar(ruta);
            if (hw_w6f_rx_abrir(ruta) == HW_OK) {
                r = HW_OK;
                while ((n = fread(b, 1, sizeof b, f)) > 0) if (hw_w6f_rx_mas(b, (u32)n) != HW_OK) { r = HW_ERROR; break; }
                r = r == HW_OK ? hw_w6f_rx_cerrar() : (hw_w6f_rx_abortar(), HW_ERROR);
            }
            fclose(f);
        }
        printf("memoria-app %s: %s\n", lin + 12, r == HW_OK ? "HW_OK" : "HW_ERROR");
        sh_apps_pendiente();
        return 0;
    }
    if (!strncmp(lin, "llenar", 6)) {                  /* (S6) llena Mis archivos HASTA EL BORDE: ficheros «r-NN» cada vez más pequeños hasta que no cabe ni uno de 1 byte */
        static u8 rel[65536]; int i, n = 0; u32 t = sizeof rel;
        for (i = 0; i < (int)sizeof rel; i++) rel[i] = (u8)(i * 5 + 1);
        while (t && n < 40) { char nom[16]; snprintf(nom, sizeof nom, "r-%02d", n + 1); if (hw_usr_escribir(nom, rel, t) == HW_OK) n++; else t /= 2; }
        printf("llenar: %d ficheros escritos; el siguiente, de 1 byte, ya no cabe (motivo %d)\n", n, hw_usr_motivo());
        paso(); return 1;
    }
    if (!strncmp(lin, "cargable ", 9)) { extern void cg_prueba(const char *orden); lin[strcspn(lin, "\n")] = 0; cg_prueba(lin + 9); if (!dentro) paso(); return 1; }   /* (C) shell/cg_prueba.c */
    if (!strncmp(lin, "usuario", 7)) {                 /* (S6) el estado de la zona de ficheros de usuario y su listado */
        HwUsrEstado u; HwUsrEntrada e; u32 i; extern u32 pc_usr_violaciones;
        hw_usr_estado(&u);
        lin[strcspn(lin, "\n")] = 0;
        printf("usuario%-18s %u ficheros, %u bytes ocupados, %u libres, %u limpiezas, %u errores, %u violaciones del modelo de flash:", lin + 7, u.ficheros, u.ocupado, u.libre, u.limpiezas, u.errores, pc_usr_violaciones);
        for (i = 0; hw_usr_listar(i, &e, 1) == 1; i++) printf(" [%s %u]", e.nombre, e.tam);
        printf("\n");
        return 0;
    }
    if (!strncmp(lin, "memoria-formatear", 17)) {      /* (AUD7 M1) lo que haría un llamador nuevo: hw_w6f_formatear() sin preguntar */
        int r = hw_w6f_formatear();
        printf("memoria-formatear: %s\n", r == HW_OK ? "HW_OK (FORMATEADA)" : r == HW_NO_DISPONIBLE ? "HW_NO_DISPONIBLE (no se toca)" : "HW_ERROR");
        return 0;
    }
    if (!strncmp(lin, "memoria-borrar ", 15) || !strncmp(lin, "memoria-renombrar ", 18)) {   /* (AUD7 B6) lo que hace Archivos con el nombre LISTADO */
        int b = lin[8] == 'b', r; char *x, *y;
        lin[strcspn(lin, "\n")] = 0; x = lin + (b ? 15 : 18); y = strchr(x, '|');
        if (y) *y++ = 0;
        r = b ? hw_w6f_borrar(x) : hw_w6f_renombrar(x, y ? y : "");
        printf("memoria-%s: %s motivo %d\n", b ? "borrar" : "renombrar", r == HW_OK ? "HW_OK" : r == HW_NO_DISPONIBLE ? "HW_NO_DISPONIBLE" : "HW_ERROR", hw_w6f_motivo());
        return 0;
    }
    if (!strncmp(lin, "memoria", 7)) {                 /* (I1) el estado de la Memoria (W6F) y su árbol entero, por stdout */
        HwW6fEstado w;
        hw_w6f_estado(&w);
        lin[strcspn(lin, "\n")] = 0;
        printf("memoria%-18s situacion %u, escribible %u, sl %u, deficit %u, %u objetos, libre %u de %u, borrados %u..%u, %u programas y %u borrados del modelo, %u violaciones:",
               lin + 7, w.situacion, w.escribible, w.solo_lectura, w.deficit, w.objetos, w.libre, w.capacidad, w.borrados_min, w.borrados_max, pc_w6f_programas, pc_w6f_borrados, pc_w6f_violaciones);
        memoria_arbol(HW_FS_MEMORIA, 0);
        printf("\n");
        return 0;
    }
    if (sscanf(lin, "tapa %255s", a) == 1) { pc_tapa(!strcmp(a, "cerrada")); paso(); return 1; }
    if (sscanf(lin, "giro %255s", a) == 1) {             /* (estética) giro 0|1|2|3: la orientación a mano (0 derecho, 1 arriba a la derecha, 2 boca abajo, 3 a la izquierda) */
        printf("giro %d: %s\n", atoi(a), ui_poner_giro(atoi(a)) ? "puesto" : "esta plataforma no gira"); paso(); paso(); return 1; }
    if (!strncmp(lin, "sen ", 4)) { lin[strcspn(lin, "\n")] = 0; sh_abrir_sen(lin + 4, NULL); paso(); espera(400); return 1; }   /* (1.9) sen RUTA: abrir esa .sen directamente (la sube antes «memoria-app») */
    if (!strncmp(lin, "ejemplo", 7)) { sh_abrir_sen("/memoria/apps/ejemplo.sen", NULL); paso(); espera(400); return 1; }   /* (APX) el ejemplo del SDK (apps_ext/ejemplo): el guion lo sube antes con «memoria-app ejemplo.sen» */
    if (!strncmp(lin, "restablecer", 11)) {            /* ajustes de fábrica: para comparar la memoria antes y después */
        aj.hora12 = 0; aj.alarma_on = 0; sh_poner_fondo("/tpa/user/image/GoLive.gif"); sh_poner_tema(TEMA_FABRICA); espera(100);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char lin[512];
    guion = argc > 1 ? fopen(argv[1], "r") : stdin;
    dir_capturas = argc > 2 ? argv[2] : ".";
    if (!guion) { perror(argv[1]); return 1; }
    pc_bomba(bomba);
    plat_init();
    ui_init(sistema_raiz);
    paso();
    while (!pc_apagado && fgets(lin, sizeof lin, guion)) ejecutar(lin, 0);
    if (pc_apagado) printf("fin: el telefono se ha APAGADO; el guion acaba aqui\n");
    if (*pc_serie_tx()) printf("serie enviada: [%s]\n", pc_serie_tx());
    printf("fin: %u cuadros, %d desajustes entre el panel y un repintado completo\n", ui_med.cuadros, desajustes);
    return desajustes ? 2 : 0;
}
