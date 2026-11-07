/* W600i (DB2010). Valores de w600/sistema/hal/hal.h, hal/enlace.ld, api/hw_comun.c [observado].
 * La batería (pista B600, w600/docs/bateria_w600.md §2.2, §5.4, §11): el original del W600 usa las MISMAS rectas y umbrales que
 * el W580 [observado en su firmware]. La recta de ESTE aparato (unidad 0x0009 de su GDFS) es [sin saber]: medido_en_aparato = 0. */
#include "placa.h"
#include "ui.h"                         /* los K_* de verdad (paso 3: api/teclas.h) */
static const PlacaCelda celdas[35] = {      /* celda = fila*5 + columna; 30..34 a masa (work/emu/teclado_w600.tsv) */
    { "", 0 },          { "3", K_3 },        { "4", K_4 },            { "*", K_AST },          { "1", K_1 },
    { "5", K_5 },       { "7", K_7 },        { "6", K_6 },            { "2", K_2 },            { "9", K_9 },
    { "0", K_0 },       { "", 0 },           { "#", K_ALM },          { "8", K_8 },            { "", 0 },
    { "Vol+", K_VOL_MAS }, { "Internet", K_WEB }, { "C", K_C },       { "Vol-", K_VOL_MENOS }, { "Atrás", K_ATRAS },
    { "Juego A", K_JUEGO_A }, { "Juego B", K_JUEGO_B }, { "Sel. izq.", K_SEL_IZQ }, { "Sel. der.", K_SEL_DER }, { "Play", K_PLAY },
    { "OK", K_OK },     { "Arriba", K_ARRIBA }, { "Abajo", K_ABAJO }, { "Izq.", K_IZQ },       { "Der.", K_DER },
    { "Encendido", K_ENCENDIDO }, { "Walkman", K_WALKMAN }, { "Cámara", K_CAMARA }, { "Bloqueo", K_BLOQUEO }, { "", 0 } };
static const PlacaZona zonas[ZONA_N] = {
    [ZONA_MAIN]       = { "main",          0x440A0000u, 0x00560000u, 0,        0 },   /* (P600) hasta la Memoria: main + W6FS del paquete (enlace.ld) */
    [ZONA_FS_PROPIO]  = { "W6FS",          0x44400000u, 0,           0,        0 },
    [ZONA_AJUSTES]    = { "ajustes",       0x45680000u, 0x00080000u, 0x20000u, 1 },   /* 4 bloques de 128 KiB */
    [ZONA_USUARIO]    = { "Mis archivos",  0x45700000u, 0x00100000u, 0x20000u, 1 },   /* 8 bloques de 128 KiB */
    [ZONA_FS_FABRICA] = { "FS de fábrica", 0x45800000u, 0x00700000u, 0,        0 },   /* + la NAND: lector propio (hal/fs.c, hal/nand.c) */
    [ZONA_MEMORIA]    = { "Memoria",       0x44600000u, 0x01000000u, 0x20000u, 1 } }; /* DECIDIDA por el mantenedor (2026-10-14): 16 MiB */
static const char *const corto[35] = {     /* de w600/sistema/apps/teclas/teclas.c */
    "",    "3",   "4",   "*",   "1",
    "5",   "7",   "6",   "2",   "9",
    "0",   "",    "#",   "8",   "",
    "Vol+", "Web", "C",  "Vol-", "Atrás",
    "Jue A", "Jue B", "Sel I", "Sel D", "Play",
    "OK",  "Arr.", "Aba.", "Izq.", "Der.",
    "Enc.", "Wkm", "Cám.", "Bloq", "" };
static const char *const luces[] = { "Luz 1 (P0.2)", "Luz 2 (P3.6)", "Luz 3 (P3.4)" };
static const char *const musica[] = { "/tpa/user/audio", 0 };
static const char *const imagen[] = { "/tpa/user/image", 0 };
static const Placa w600i = {
    .modelo = "W600i", .soc = "DB2010", .api_placa = PLACA_API,
    .capacidades = CAP_ZUMBADOR | CAP_PCM | CAP_TAPA | CAP_LUZ_TECLADO | CAP_RTC_ALARMA | CAP_SERIE | CAP_FS_FABRICA | CAP_BATERIA_MV | CAP_PERRO,
    .ancho = 176, .alto = 220, .pixel = PIX_RGB565_BE,
    .filas = 6, .columnas = 5, .a_masa = 5, .celdas = celdas, .tecla_modo_seguro = K_C,
    .tapa_tipo = TAPA_GIRO, .luces_sencillas = 3, .luces_rgb = 0, .luz_nombre = luces,
    .ram_base = 0x4C000000u, .ram_tam = 0x01000000u,
    .zonas = zonas,
    .bat = { .mv_no_creible = 2850, .mv_arranque = 3400, .mv_aviso = 3500, .mv_corte = 3400,      /* los del W580 S7 (el original: aviso 3400, corte 3250 x4) */
             .frio = { 2796, 10784 }, .marcha = { 2755, 10843 }, .carga_permitida = 0, .medido_en_aparato = 0 },
    .tic_hz = 250, .cpu_mhz = 104, .rutas_musica = musica, .rutas_imagen = imagen,
    .rotulo = "W600", .flash_esperada = "0089 880D, 32 MiB, bloques de 128 KiB",
    .celda_corto = corto, .celda_columna_mayor = 0 };
const Placa *placa_w600i(void) { return &w600i; }
#ifndef PLACA_SIMULADA
const Placa *placa(void) { return &w600i; }
#endif
