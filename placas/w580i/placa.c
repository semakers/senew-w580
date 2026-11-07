/* W580i (DB2020). Valores de work/aud/sistema_s7/hal/hal.h, hal/enlace.ld, api/hw_comun.c, api/bat.h [observado].
 * NADA de esto se ha medido en un aparato (medido_en_aparato = 0). */
#include "placa.h"
#include "ui.h"                         /* los K_* de verdad (paso 3: api/teclas.h) */
static const PlacaCelda celdas[35] = {      /* celda = columna*5 + fila; 30..34 a masa (work/h2/teclas.tsv) */
    { "3", K_3 },       { "6", K_6 },        { "1", K_1 },            { "", 0 },               { "Walkman", K_WALKMAN },
    { "8", K_8 },       { "Sel. der.", K_SEL_DER }, { "Atrás", K_ATRAS }, { "C", K_C },        { "Actividad", K_WEB },
    { "2", K_2 },       { "0", K_0 },        { "Sel. izq.", K_SEL_IZQ }, { "5", K_5 },         { "", 0 },
    { "9", K_9 },       { "4", K_4 },        { "7", K_7 },            { "*", K_AST },          { "#", K_ALM },
    { "", 0 },          { "Vol-", K_VOL_MENOS }, { "", 0 },           { "Operador", 0 },       { "Vol+", K_VOL_MAS },
    { "Abajo", K_ABAJO }, { "Izq.", K_IZQ }, { "Arriba", K_ARRIBA },  { "OK", K_OK },          { "Der.", K_DER },
    { "Encendido", K_ENCENDIDO }, { "Rebobinar", K_JUEGO_A }, { "Avanzar", K_JUEGO_B }, { "Alternar", K_BLOQUEO }, { "Play", K_PLAY } };
static const PlacaZona zonas[ZONA_N] = {
    [ZONA_MAIN]       = { "main",          0x44140000u, 0x003C0000u, 0,        0 },   /* (P600) hasta la Memoria (enlace.ld: ASSERT con NOR_W6F_INI; 2026-10-14: 0x3C0000) */
    [ZONA_FS_PROPIO]  = { "W6FS",          0x44400000u, 0,           0,        0 },   /* tam 0: detrás del main, donde quepa (se busca la firma) */
    [ZONA_AJUSTES]    = { "ajustes",       0x45B00000u, 0x00100000u, 0x20000u, 1 },   /* 4 bloques de 256 KiB = 128 KiB lógicos (M18, modo control) */
    [ZONA_USUARIO]    = { "Mis archivos",  0x45C00000u, 0x00200000u, 0x20000u, 1 },
    [ZONA_FS_FABRICA] = { "FS de fábrica", 0x45E00000u, 0x02000000u, 0,        0 },   /* NUNCA se escribe */
    [ZONA_MEMORIA]    = { "Memoria",       0x44500000u, 0x01600000u, 0x40000u, 1 } }; /* W6F: hal/hal.h NOR_W6F_INI..NOR_ZONA_INI, 88 bloques de 256 KiB (2026-10-14, el mantenedor: 22 MiB) */
static const char *const corto[35] = {     /* la rejilla de la app Teclas (antes en apps/teclas/teclas.c) */
    "3",   "6",   "1",   "",    "Wkm",
    "8",   "Sel D", "Atrás", "C", "Activ",
    "2",   "0",   "Sel I", "5",  "",
    "9",   "4",   "7",   "*",   "#",
    "",    "Vol-", "",   "Oper", "Vol+",
    "Aba.", "Izq.", "Arr.", "OK", "Der.",
    "Enc.", "Rebo", "Avan", "Alt", "Play" };
static const char *const luces[] = { "Teclado del deslizador", "Teclado, zona 2", "Teclado, zona 1" };
static const char *const musica[] = { "/tpa/user/music", "/tpa/user/audio", 0 };
static const char *const imagen[] = { "/tpa/user/picture", "/tpa/user/image", 0 };
static const Placa w580i = {
    .modelo = "W580i", .soc = "DB2020", .api_placa = PLACA_API,
    .capacidades = CAP_ZUMBADOR | CAP_VIBRADOR | CAP_ACELEROMETRO | CAP_TAPA | CAP_LUZ_TECLADO | CAP_LUZ_RGB | CAP_RTC_ALARMA
                 | CAP_SERIE | CAP_FS_FABRICA | CAP_APAGAR | CAP_BATERIA_MV | CAP_PERRO | CAP_PANEL_DETECTADO,
    .ancho = 240, .alto = 320, .pixel = PIX_RGB565_BE,
    .filas = 5, .columnas = 6, .a_masa = 5, .celdas = celdas, .tecla_modo_seguro = K_C,
    .tapa_tipo = TAPA_DESLIZADOR, .luces_sencillas = 3, .luces_rgb = 2, .luz_nombre = luces,
    .ram_base = 0x20000000u, .ram_tam = 0x00E80000u,      /* 16 MiB menos la memoria gráfica del final (enlace.ld) */
    .zonas = zonas,
    .bat = { .mv_no_creible = 2850, .mv_arranque = 3400, .mv_aviso = 3500, .mv_corte = 3400,
             .frio = { 2796, 10784 }, .marcha = { 2755, 10843 }, .carga_permitida = 1, .medido_en_aparato = 0 },   /* (CARGA, 25-oct-2026) carga por USB: api/carga.h, docs/bateria.md §12 */
    .tic_hz = 250, .cpu_mhz = 208, .rutas_musica = musica, .rutas_imagen = imagen,
    .rotulo = "W580", .flash_esperada = "0089 887E, 64 MiB, bloques de 256 KiB",
    .celda_corto = corto, .celda_columna_mayor = 1 };
const Placa *placa_w580i(void) { return &w580i; }
#ifndef PLACA_SIMULADA
const Placa *placa(void) { return &w580i; }
#endif
