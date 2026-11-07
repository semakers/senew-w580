/* W810i (DB2010). Pista port810 (docs/port_w810.md). El mismo SoC, PMU, RAM y pantalla que el W600i; cambian el teclado (de
 * barra), la NOR (64 MiB, dos chips) y la disposición de la flash (APROBADA por el mantenedor, 2026-10-15).
 * Teclado [observado]: la tabla «exploración -> tecla» del main R4DB005 en 0x4466D938 (35 entradas, celda = fila*5 + columna; en
 * el W600 está en 0x445D2DB0 con el mismo final 01F4 000D FFFF FFFF), con los nombres de tecla del elfpack (W810_R4DB005.h):
 * 0x1C = KEY_TASK [inferido de otros modelos], 0x22 = KEY_CAMERA_FOCUS, 0x23 = KEY_CAMERA_SHOT, 0x1F = KEY_MEDIAPLAYER.
 * La batería: el original del W810 usa en el arranque las MISMAS constantes que el W600 (3400 mV, 0xB22..0x157D, 10,784 y 2785;
 * 0x44BD0348 frente a 0x4499A544) [observado]; la recta de ESTE aparato es [sin saber]: medido_en_aparato = 0. */
#include "placa.h"
#include "ui.h"
static const PlacaCelda celdas[35] = {
    { "3", K_3 },       { "6", K_6 },        { "9", K_9 },            { "#", K_ALM },          { "Actividad", K_WEB },
    { "Sel. izq.", K_SEL_IZQ }, { "Sel. der.", K_SEL_DER }, { "Atrás", K_ATRAS }, { "C", K_C }, { "Play", K_PLAY },
    { "2", K_2 },       { "5", K_5 },        { "8", K_8 },            { "0", K_0 },            { "Vol-", K_VOL_MENOS },
    { "1", K_1 },       { "4", K_4 },        { "7", K_7 },            { "*", K_AST },          { "Vol+", K_VOL_MAS },
    { "Abajo", K_ABAJO }, { "Arriba", K_ARRIBA }, { "Izq.", K_IZQ },   { "OK", K_OK },          { "Der.", K_DER },
    { "", 0 },          { "", 0 },           { "Enfoque", 0 },        { "Cámara", K_CAMARA },  { "Walkman", K_WALKMAN },
    { "Encendido", K_ENCENDIDO }, { "", 0 }, { "", 0 },              { "", 0 },               { "", 0 } };
static const PlacaZona zonas[ZONA_N] = {     /* APROBADA el 2026-10-15 (placas/w810i/nor_placa.h, docs/port_w810.md §4) */
    [ZONA_MAIN]       = { "main",          0x44140000u, 0x006C0000u, 0,        0 },   /* hasta la Memoria: main + W6FS del paquete */
    [ZONA_FS_PROPIO]  = { "W6FS",          0x44400000u, 0,           0,        0 },
    [ZONA_AJUSTES]    = { "ajustes",       0x45780000u, 0x00080000u, 0x20000u, 1 },   /* 4 bloques de 128 KiB */
    [ZONA_USUARIO]    = { "Mis archivos",  0x45800000u, 0x00100000u, 0x20000u, 1 },   /* 8 bloques de 128 KiB */
    [ZONA_FS_FABRICA] = { "FS de fábrica", 0x45900000u, 0x02500000u, 0,        0 },   /* el FS original (37 MiB): no se escribe ni se lee */
    [ZONA_MEMORIA]    = { "Memoria",       0x44800000u, 0x00F00000u, 0x20000u, 1 } }; /* APROBADA: 15 MiB, con escritura (memoria.c) */
static const char *const corto[35] = {
    "3",    "6",    "9",    "#",    "Act.",
    "Sel I", "Sel D", "Atrás", "C",  "Play",
    "2",    "5",    "8",    "0",    "Vol-",
    "1",    "4",    "7",    "*",    "Vol+",
    "Aba.", "Arr.", "Izq.", "OK",   "Der.",
    "",     "",     "Enf.", "Cám.", "Wkm",
    "Enc.", "",     "",     "",     "" };
static const char *const luces[] = { 0 };
static const char *const musica[] = { 0 };
static const char *const imagen[] = { 0 };
static const Placa w810i = {
    .modelo = "W810i", .soc = "DB2010", .api_placa = PLACA_API,
    .capacidades = CAP_PCM | CAP_RTC_ALARMA | CAP_SERIE | CAP_BATERIA_MV | CAP_PERRO,
    .ancho = 176, .alto = 220, .pixel = PIX_RGB565_BE,
    .filas = 6, .columnas = 5, .a_masa = 5, .celdas = celdas, .tecla_modo_seguro = K_C,
    .tapa_tipo = TAPA_NO, .luces_sencillas = 0, .luces_rgb = 0, .luz_nombre = luces,
    .ram_base = 0x4C000000u, .ram_tam = 0x01000000u,          /* «krn/phys_mem/RAM/1=base:0x4c000000 size:16M» del main [observado] */
    .zonas = zonas,
    .bat = { .mv_no_creible = 2850, .mv_arranque = 3400, .mv_aviso = 3500, .mv_corte = 3400,
             .frio = { 2796, 10784 }, .marcha = { 2755, 10843 }, .carga_permitida = 0, .medido_en_aparato = 0 },
    .tic_hz = 250, .cpu_mhz = 104, .rutas_musica = musica, .rutas_imagen = imagen,
    .rotulo = "W810", .flash_esperada = "0089 880D (CFI 64 MiB), bloques de 128 KiB",
    .celda_corto = corto, .celda_columna_mayor = 0 };
const Placa *placa_w810i(void) { return &w810i; }
#ifndef PLACA_SIMULADA
const Placa *placa(void) { return &w810i; }
#endif
