/* Iconos de 1 bit dibujados a mano para este sistema (arte en texto; se empaqueta al arrancar). */
#include "shell.h"

static const char *const arte[IC_N][12] = {
  { "............", "............", ".####.......", ".#...######.", ".#........#.", ".#........#.",
    ".#........#.", ".#........#.", ".#........#.", ".##########.", "............", "............" },   /* archivos */
  { "............", ".##########.", ".#........#.", ".#.##.....#.", ".#.##...#.#.", ".#.....###.#",
    ".#..#.#####.", ".#.########.", ".##########.", ".##########.", "............", "............" },   /* imágenes */
  { "............", "....####....", "..##....##..", ".#....#...#.", ".#....#...#.", ".#....#...#.",
    ".#....###.#.", ".#........#.", ".#........#.", "..##....##..", "....####....", "............" },   /* reloj */
  { "............", ".########...", ".#......#...", ".#.####.#.#.", ".#......###.", ".#.####.###.",
    ".#......##..", ".#.###.##...", ".#.....#....", ".#......#...", ".########...", "............" },   /* notas */
  { "............", ".##########.", ".#........#.", ".#.######.#.", ".#........#.", ".#.#.#.#..#.",
    ".#........#.", ".#.#.#.#..#.", ".#........#.", ".#.#.#.##.#.", ".##########.", "............" },   /* calculadora */
  { "............", "............", ".####.......", ".#..#.......", ".####.####..", "....#.#..#..",
    "....###..#..", ".........#..", "..##.....#..", "..##..####..", "............", "............" },   /* juego: la serpiente */
  { "............", ".##########.", ".#........#.", ".#.#......#.", ".#..#.....#.", ".#...#....#.",
    ".#..#.....#.", ".#.#..###.#.", ".#........#.", ".##########.", "............", "............" },   /* consola */
  { "............", ".###.##.###.", ".###.##.###.", "............", ".###.##.###.", ".###.##.###.",
    "............", ".###.##.###.", ".###.##.###.", "............", ".###.##.###.", "............" },   /* teclas */
  { "............", "..#.#.#.#...", ".#########..", ".#.......#.#", ".#.#####.#..", ".#.#####.#.#",
    ".#.#####.#..", ".#.#####.#.#", ".#.......#..", ".#########..", "..#.#.#.#...", "............" },   /* sistema: un chip */
  { "............", ".....##.....", "..#..##..#..", "...######...", "...##..##...", ".###....###.",
    ".###....###.", "...##..##...", "...######...", "..#..##..#..", ".....##.....", "............" },   /* ajustes */
  { "............", ".###........", ".####.......", ".####.......", "..###.......", "..####......",
    "...####.....", "....#####...", ".....######.", "......#####.", ".......###..", "............" },   /* teléfono */
  { "............", ".....######.", ".....######.", ".....#....#.", ".....#....#.", ".....#....#.",
    ".....#....#.", "..###...###.", ".####..####.", ".####..####.", "..##....##..", "............" },   /* música */
  { "........", ".###....", "#######.", "#.....#.", "#.....#.", "#.....#.", "#######.", 0, 0, 0, 0, 0 },      /* carpeta pequeña */
  { "#####...", "#...##..", "#....#..", "#....#..", "#....#..", "#....#..", "######..", 0, 0, 0, 0, 0 },      /* fichero pequeño */
  { ".#...#..", "#.###.#.", ".#.#.#..", ".#.###..", ".#...#..", "..###...", ".#...#..", 0, 0, 0, 0, 0 },      /* alarma pequeña */
  { "..#.#...", "..#.#...", ".#####..", ".#####..", "..###...", "...#....", "...#....", 0, 0, 0, 0, 0 },      /* enchufe (serie) */
  { "..###...", ".#...#..", ".#...#..", "#######.", "#.###.#.", "#..#..#.", "#######.", 0, 0, 0, 0, 0 },      /* candado (bloqueo) */
  { "...##...", "..####..", ".##.....", ".####...", "...###..", ".....##.", "..####..", 0, 0, 0, 0, 0 },      /* rayo (cargando) */
  { "...###..", "...#.##.", "...#..#.", "...#....", ".###....", "####....", ".##.....", 0, 0, 0, 0, 0 },      /* nota pequeña (suena música) */
  { "............", "......##....", "......##....", ".....###....", "....####.#..", "...#.##.#...",
    ".....##.....", ".....###....", "....##.##...", "...##...#...", "..##....##..", "............" },   /* movimiento: alguien andando */
  { ".##.....", "###.....", "###..##.", ".#..###.", "....###.", ".....#..", "........", 0, 0, 0, 0, 0 },      /* pasos: dos huellas (contador del standby) */
  { "............", ".####..####.", ".####..####.", ".####..####.", ".####..####.", "............",
    "............", ".####..####.", ".####..####.", ".####..####.", ".####..####.", "............" },   /* (EST3) apps: cuatro baldosas */
};
static u16 bits[IC_N][12];
Icono sh_iconos[IC_N];

/* El icono 2D de una app EXTERNA (AppExterna.arte, pista M2): 12 cadenas de la misma longitud (1..16), «#» = tinta.
 * Se empaqueta aquí la primera vez; el mismo arte da siempre el mismo icono. Si no vale (falta una fila, las
 * filas no miden lo mismo) o no quedan huecos, NULL: el menú pone el icono genérico que la app pidió. */
#define IC_EXT 2
static u16 bits_ext[IC_EXT][12];
static Icono ic_ext[IC_EXT];
static const char *const *arte_ext[IC_EXT];
const Icono *sh_icono_externo(const char *const *a)
{
    int k, f, c, w;
    if (!a || !a[0]) return NULL;
    for (k = 0; k < IC_EXT && arte_ext[k]; k++) if (arte_ext[k] == a) return &ic_ext[k];
    if (k == IC_EXT) return NULL;
    w = (int)strlen(a[0]);
    if (w < 1 || w > 16) return NULL;
    for (f = 0; f < 12; f++) if (!a[f] || (int)strlen(a[f]) != w) return NULL;
    for (f = 0; f < 12; f++)
        for (c = 0, bits_ext[k][f] = 0; c < w; c++) if (a[f][c] == '#') bits_ext[k][f] |= (u16)(1u << (w - 1 - c));
    ic_ext[k].w = (u8)w; ic_ext[k].h = 12; ic_ext[k].filas = bits_ext[k];
    arte_ext[k] = a;
    return &ic_ext[k];
}

void sh_iconos_init(void)
{
    int i, f, c;
    for (i = 0; i < IC_N; i++) {
        int w = (int)strlen(arte[i][0]), h = 0;
        for (f = 0; f < 12 && arte[i][f]; f++, h++)
            for (c = 0, bits[i][f] = 0; c < w && arte[i][f][c]; c++) if (arte[i][f][c] == '#') bits[i][f] |= (u16)(1u << (w - 1 - c));
        sh_iconos[i].w = (u8)w; sh_iconos[i].h = (u8)h; sh_iconos[i].filas = bits[i];
    }
    /* (MD) qué ES cada icono pequeño: un diseño con símbolos propios los pinta a su manera (ui/diseno.h, DIS_S_*) */
    dis_icono_es(&sh_iconos[IC_ALARMA], DIS_S_ALARMA); dis_icono_es(&sh_iconos[IC_CANDADO], DIS_S_CANDADO);
    dis_icono_es(&sh_iconos[IC_ENCHUFE], DIS_S_ENCHUFE); dis_icono_es(&sh_iconos[IC_NOTA], DIS_S_NOTA);
    dis_icono_es(&sh_iconos[IC_RAYO], DIS_S_RAYO); dis_icono_es(&sh_iconos[IC_CARPETA], DIS_S_CARPETA);
    dis_icono_es(&sh_iconos[IC_FICHERO], DIS_S_FICHERO); dis_icono_es(&sh_iconos[IC_PASOS], DIS_S_PASOS);
    dis_icono_es(&sh_iconos[IC_ARCHIVOS], DIS_S_ARCHIVOS); dis_icono_es(&sh_iconos[IC_IMAGENES], DIS_S_IMAGENES);
    dis_icono_es(&sh_iconos[IC_RELOJ], DIS_S_RELOJ); dis_icono_es(&sh_iconos[IC_NOTAS], DIS_S_NOTAS);
    dis_icono_es(&sh_iconos[IC_CALC], DIS_S_CALCULADORA); dis_icono_es(&sh_iconos[IC_JUEGO], DIS_S_JUEGO);
    dis_icono_es(&sh_iconos[IC_CONSOLA], DIS_S_CONSOLA); dis_icono_es(&sh_iconos[IC_TECLAS], DIS_S_TECLADO);
    dis_icono_es(&sh_iconos[IC_SISTEMA], DIS_S_SISTEMA); dis_icono_es(&sh_iconos[IC_AJUSTES], DIS_S_AJUSTES);
    dis_icono_es(&sh_iconos[IC_TELEFONO], DIS_S_TELEFONO); dis_icono_es(&sh_iconos[IC_MUSICA], DIS_S_MUSICA);
    dis_icono_es(&sh_iconos[IC_MOVIMIENTO], DIS_S_MOVIMIENTO);
}
