/* cargable.h — EL CONTRATO de una APP CARGABLE («.sen»): el formato del binario y la tabla de funciones (API v1).
 *
 * Es el ÚNICO fichero que comparten el sistema y el SDK de apps (cargables/sdk/ lo incluye tal cual). No incluye
 * nada del sistema ni de ningún modelo de teléfono: ni el tamaño de la pantalla, ni las teclas que hay, ni los
 * sensores. Todo eso se PREGUNTA con ap_consulta(). Documento: docs/apps_cargables.md.
 *
 * ABI (lo que no puede cambiar dentro de una versión MAYOR):
 *   - máquina 1 (teléfono): ARMv5TE, little-endian, AAPCS (r0-r3 argumentos, r0 resultado, r4-r11 se conservan,
 *     pila alineada a 8 en cada llamada), coma flotante por software (no cruza la tabla), ARM o Thumb con
 *     interworking (toda llamada indirecta es BX/BLX; el bit 0 de un puntero a función dice Thumb);
 *   - máquina 2 (banco de PC): AArch64 LP64, AAPCS64. Sirve para probar el cargador y las apps; nada más;
 *   - enteros: char 8, short 16, int 32, puntero y size_t = la palabra de la máquina; alineación natural;
 *   - NINGUNA estructura cruza la tabla por valor, y las pocas que cruzan por puntero (W5Rect, W5Fecha, W5Icono)
 *     están fijadas aquí campo a campo; los widgets, el contexto, el lienzo y la entrada de texto son MANEJADORES
 *     OPACOS: la app no sabe qué hay dentro ni cuánto miden;
 *   - el sistema llama a la app con 4 argumentos como mucho (todos de una palabra);
 *   - los colores son 0x00RRGGBB (W5_TRANSPARENTE = «el del tema / ninguno»); el formato del panel no se ve;
 *   - las teclas son los códigos abstractos W5K_*; cuáles existen en este aparato lo dice ap_consulta().
 * Política de compatibilidad: versión MAYOR.MENOR. Dentro de una mayor la tabla SOLO CRECE POR EL FINAL (cada
 * función añadida sube la menor) y ninguna función, atributo de widget o consulta cambia de número ni de
 * significado. Una app declara la mayor y la menor con que se compiló: se carga si mayor == la del sistema y
 * menor <= la del sistema; si no, se rechaza con un mensaje que lo dice. */
#ifndef CARGABLE_H
#define CARGABLE_H
#include <stddef.h>

#define W5_API_MAYOR 1
#define W5_API_MENOR 3                  /* (MD) 1.3: el sistema de diseño (grupo 13, al final de la tabla) */
/* Historia de la menor (lo que cada una AÑADE; nada cambia de número ni de significado):
 *   1.0  la tabla de 77 funciones.
 *   1.1  (pista M5X, juegos grandes) la VENTANA DE RECURSOS (W5_CAP_REC_VENTANA, W5_REC_VA), el modo de pantalla
 *        W5_JG_PANEL y el tope de imagen W5_IMG_MAX de 8 MiB. La tabla NO cambia: sigue con 77 funciones.
 *   1.2  (pista APX, las apps de casa sacadas del firmware) los SERVICIOS del sistema que una app de utilidad necesita:
 *        ajustes con lista blanca, hora y temporizador, retenciones, serie, teclas crudas, podómetro, luces, música,
 *        Mis archivos, listar carpetas, imágenes; los iconos W5_IC_* de todos los de casa. Y (pista JIT, 77 y 78) W5_CAP_JIT con
 *        ap_jit_sincronizar, y fs_listar. Funciones nuevas desde la 77; la tabla entera, en docs/apps_externas.md §3. */

/* ---- 1. EL FORMATO DEL FICHERO (.sen), little-endian --------------------------------------------------------
 *   [cabecera: 128 bytes][imagen: tam_cod + tam_dat bytes][n_reub desplazamientos de 32 bits][recursos: tam_rec bytes]
 * El cargador lee el fichero SIEMPRE por la API de ficheros, a trozos y con desplazamiento: no supone que esté
 * mapeado en memoria ni dice nada de dónde vive (la procedencia es una ruta: /apps, /usuario, mañana /tarjeta).
 * Los RECURSOS (datos grandes de solo lectura: niveles, texturas, sonido) no se copian a RAM: la app los lee a
 * trozos (ap_rec_leer) o, si el fichero resulta estar contiguo en una flash mapeada, los usa en el sitio
 * (ap_rec_mapa devuelve un puntero; si devuelve NULL, a leer).
 * La imagen está enlazada en la dirección 0 (código, constantes y datos seguidos; el .bss va detrás, a cero).
 * Cada desplazamiento señala una PALABRA de la imagen (4 bytes en la máquina 1, 8 en la 2) que contiene una
 * dirección absoluta relativa a 0: el cargador le suma la dirección donde ha puesto la imagen. Nada más: lo
 * relativo al contador de programa no cambia al mover la imagen entera. La lista va en orden creciente. */
#define W5X_MAGIA "W5X\1"
#define W5X_TAM_CAB 128
#define SEN_EXT ".sen"
enum { W5_MAQ_ARMV5TE = 1, W5_MAQ_AARCH64 = 2 };
enum { W5_PERFIL_WIDGETS = 1,          /* la app son pantallas de widgets; el bucle es del sistema; recibe sucesos */
       W5_PERFIL_JUEGO = 2 };          /* la app se queda con la pantalla y con el bucle hasta que sale (jg_*) */
typedef struct W5xCab {
    unsigned char  magia[4];           /* "W5X\1" */
    unsigned short tam_cab;            /* 128 */
    unsigned char  maquina;            /* W5_MAQ_* */
    unsigned char  perfil;             /* W5_PERFIL_*: de widgets (por sucesos) o juego (pantalla completa, bucle propio) */
    unsigned short api_mayor, api_menor;   /* la API con que se compiló */
    unsigned int   caps;               /* W5_CAP_* que la app NECESITA: si falta una, no se carga */
    unsigned short an_min, al_min;     /* pantalla mínima que necesita, en píxeles (0 = cualquiera) */
    unsigned int   tam_fich;           /* el fichero entero */
    unsigned int   tam_cod;            /* código + constantes */
    unsigned int   tam_dat;            /* datos con valor inicial */
    unsigned int   tam_bss;            /* datos a cero */
    unsigned int   pila;               /* bytes de pila que pide */
    unsigned int   monton;             /* bytes de montón que pide (ap_mem_pedir) */
    unsigned int   entrada;            /* desplazamiento del punto de entrada dentro del código (bit 0 = Thumb) */
    unsigned int   n_reub;             /* cuántas reubicaciones */
    unsigned int   ver_app;            /* la versión de la app (libre) */
    unsigned char  icono_gen;          /* icono genérico del sistema si no trae el suyo (W5_IC_*) */
    unsigned char  icono_an;           /* ancho de su icono de 1 bit: 1..16; 0 = no trae */
    unsigned short icono[12];          /* 12 filas; el bit (icono_an - 1) es el píxel de la izquierda */
    char           nombre[30];         /* UTF-8, acabado en 0: el que sale en el menú */
    unsigned int   tam_rec;            /* RECURSOS: bytes de solo lectura que van al final del fichero y NO se cargan en RAM */
    unsigned int   crc_rec;            /* su CRC-32 (lo comprueba quien empaqueta y ap_rec_comprobar(); el cargador no, para no leer MiB) */
    unsigned int   crc_cuerpo;         /* CRC-32 (el de zlib) de todo lo que va detrás de la cabecera */
    unsigned int   crc_cab;            /* CRC-32 de los 124 bytes anteriores */
} W5xCab;
typedef char w5x_cab_mide_128[sizeof(W5xCab) == W5X_TAM_CAB ? 1 : -1];
enum { W5_IC_JUEGO = 5, W5_IC_IMAGENES = 1, W5_IC_MUSICA = 11, W5_IC_CONSOLA = 6, W5_IC_SISTEMA = 8, W5_IC_FICHERO = 13 };
/* (1.2) los demás iconos genéricos del sistema, con el MISMO número que en el menú de casa (el 2D y la malla 3D salen de aquí) */
enum { W5_IC_ARCHIVOS = 0, W5_IC_RELOJ = 2, W5_IC_NOTAS = 3, W5_IC_CALC = 4, W5_IC_TECLAS = 7, W5_IC_AJUSTES = 9,
       W5_IC_TELEFONO = 10, W5_IC_CARPETA = 12, W5_IC_ALARMA = 14, W5_IC_ENCHUFE = 15, W5_IC_CANDADO = 16, W5_IC_RAYO = 17,
       W5_IC_NOTA = 18, W5_IC_MOVIMIENTO = 19, W5_IC_PASOS = 20 };

/* ---- (ICO) LA SECCIÓN DE ICONO: la malla 3D del menú dentro del .sen (docs/menu3d.md §9) ----------------------------------
 * El icono PLANO es el de la cabecera (icono_an, icono[12]: 1 bit, hasta 16x12): lo pintan los dos diseños (material3 lo tiñe,
 * clásico también) y lo ve hasta un sistema viejo. La MALLA es opcional y va al FINAL DE LOS RECURSOS (cuenta en tam_rec y en
 * crc_rec): [recursos de la app][W5xIcoMalla][pos 3*npos][nor 3*nnor][tri 6*Σnt][W5xIcoPie]. Para un sistema que no la conoce
 * son bytes de recursos y no cambia nada; una .sen sin ella sale con la malla genérica. El formato del .sen no sube de versión.
 * Es el MISMO formato que AppMalla (api/app_externa.h). El sistema la lee y la VALIDA al descubrir (dato de fuera: no se fía de
 * nada): pie con magia y CRC-32, tamaño exacto, cada índice de triángulo dentro de rango y todo acotado por lo de abajo. */
#define W5X_ICO_MAGIA   "W5I1"
#define W5X_ICO_MAX     3072u          /* bytes de la sección SIN el pie */
#define W5X_ICO_TRI_MAX 256u           /* triángulos (el icono más cargado de casa lleva 242) */
typedef struct W5xIcoMalla {
    unsigned char  version;            /* 1 */
    unsigned char  npos, nnor;         /* 3..255 posiciones (s8 x,y,z; 1.0 = 100), 1..255 normales (s8 x,y,z; x127) */
    unsigned char  _r;                 /* 0 */
    unsigned short nt[5];              /* triángulos de cuerpo, secundario, claro, oscuro, acento (agrupados en ese orden) */
    unsigned char  natural[3][3];      /* RGB de cuerpo, secundario y acento (el estilo «Cristal») */
    unsigned char  _r2[3];             /* 0 */
} W5xIcoMalla;                         /* 26 bytes; detrás pos, nor y tri (3 índices de posición y 3 de normal por triángulo) */
typedef struct W5xIcoPie { unsigned int tam, crc; char magia[4]; } W5xIcoPie;   /* los ÚLTIMOS 12 bytes del fichero; tam = la sección sin el pie */
typedef char w5x_ico_mide[sizeof(W5xIcoMalla) == 26 && sizeof(W5xIcoPie) == 12 ? 1 : -1];

/* Límites que el cargador impone (por encima, se rechaza) */
#define W5_PILA_MIN   (4u << 10)
#define W5_PILA_MAX   (256u << 10)
#define W5_IMG_MAX    (8u << 20)       /* código + datos + bss (1.0: 4 MiB; 1.1: 8 MiB) */
/* LA VENTANA DE RECURSOS (1.1). Una app que declara W5_CAP_REC_VENTANA se enlaza con sus recursos EN W5_REC_VA
 * (sdk/w5_juego.ld): todo puntero a un recurso —desde el código, desde sus datos o desde otro recurso— es ya la
 * dirección de la ventana, y no hay nada que reubicar. Al cargarla, el sistema pone con la MMU, en páginas de 4 KiB,
 * los recursos del fichero en [W5_REC_VA, W5_REC_VA + tam_rec), de solo lectura para la app; al descargarla, la quita.
 * De dónde salen las páginas no lo ve la app: EN EL SITIO si el fichero está contiguo en una flash mapeada y sus recursos
 * empiezan en frontera de 4 KiB (el W6FS; herr/hacer_fs.py los alinea así), o COPIADOS A RAM (páginas del préstamo fuera
 * del bloque de la app) si viene de la Memoria (W6F), de Mis archivos o de una tarjeta. Si no caben, no se carga. Para esa app, ap_rec_mapa(desde, n) devuelve W5_REC_VA + desde (el mapa ES la ventana).
 * Sin esa capacidad, nada cambia: los recursos se leen con ap_rec_leer / ap_rec_mapa como en 1.0. */
#define W5_REC_VA     0x70000000u
#define W5_REC_VENTANA_MAX (16u << 20)
#define W5_REC_ALINEA 4096u
#define W5_MONTON_MAX (14u << 20)
#define W5_REUB_MAX   65536u

/* ---- 2. CAPACIDADES Y CONSULTAS ------------------------------------------------------------------------------ */
enum { W5_CAP_ACELEROMETRO = 1 << 0, W5_CAP_VIBRADOR = 1 << 1, W5_CAP_ZUMBADOR = 1 << 2, W5_CAP_LUCES = 1 << 3,
       W5_CAP_TAPA = 1 << 4,           /* tapa o deslizador */
       W5_CAP_FICHEROS = 1 << 5,       /* la app puede GUARDAR ficheros (ap_fich_escribir) */
       W5_CAP_RELOJ = 1 << 6,          /* hay calendario (ap_fecha) */
       W5_CAP_COLOR = 1 << 7,          /* pantalla en color */
       W5_CAP_TECLADO_NUM = 1 << 8,    /* 0-9 * # */
       W5_CAP_JOYSTICK = 1 << 9,       /* arriba, abajo, izquierda, derecha y OK */
       W5_CAP_REC_VENTANA = 1 << 10 }; /* (1.1) la app necesita sus recursos en la ventana W5_REC_VA (ver arriba) */
                                       /* (1.2) W5_CAP_JIT = 1 << 11, de la pista JIT: en el grupo «0» de la tabla (más abajo) */
enum { W5_Q_API = 0,                   /* (mayor << 16) | menor del sistema */
       W5_Q_ANCHO, W5_Q_ALTO,          /* la pantalla, en píxeles */
       W5_Q_CAPS,                      /* bits W5_CAP_* de ESTE aparato */
       W5_Q_TECLAS_A, W5_Q_TECLAS_B,   /* qué W5K_* existen: bit k de A (k < 32) o bit k-32 de B */
       W5_Q_ALTO_LINEA,                /* alto de una línea de texto a escala 1 */
       W5_Q_MODELO,                    /* 580, 600… (para diagnósticos; no para decidir nada) */
       W5_Q_PRIMER_PLANO,              /* 1 = la app se ve y recibe teclas y tics */
       W5_Q_PILA_LIBRE,                /* bytes de pila que nunca se han tocado */
       W5_Q_ACEL_1G,                   /* cuentas de ap_acel_leer que valen 1 g (0 = no se sabe) */
       W5_Q_PIXEL,                     /* formato de los píxeles del marco de un JUEGO: W5_PIX_* */
       W5_Q_AVISOS,                    /* perfil juego: bits W5_AV_* de lo que ha saltado mientras la app tenía la pantalla */
       W5_Q_N };

/* ---- 3. TECLAS (códigos abstractos; los mismos en todos los modelos) ----------------------------------------- */
enum { W5K_NADA, W5K_ARRIBA, W5K_ABAJO, W5K_IZQ, W5K_DER, W5K_OK, W5K_SEL_IZQ, W5K_SEL_DER, W5K_ATRAS, W5K_C,
       W5K_0, W5K_1, W5K_2, W5K_3, W5K_4, W5K_5, W5K_6, W5K_7, W5K_8, W5K_9, W5K_AST, W5K_ALM,
       W5K_VOL_MAS, W5K_VOL_MENOS, W5K_WEB, W5K_PLAY, W5K_JUEGO_A, W5K_JUEGO_B, W5K_WALKMAN, W5K_CAMARA,
       W5K_BLOQUEO, W5K_ENCENDIDO, W5K_N };

/* ---- 4. MANEJADORES OPACOS Y LOS POCOS REGISTROS FIJOS -------------------------------------------------------- */
typedef struct W5W_ *W5W;              /* un widget (dura un cuadro: se construye en construir() y se devuelve) */
typedef struct W5Ctx_ *W5Ctx;          /* el contexto de una pantalla o de un widget con estado */
typedef struct W5Lienzo_ *W5Lienzo;    /* donde se pinta, dentro de una retrollamada de pintar */
typedef struct W5Et_ *W5Et;            /* una entrada de texto (el «controlador» de un CampoTexto) */
typedef size_t W5Pal;                  /* una palabra de la máquina */
typedef struct { int x, y, w, h; } W5Rect;                                  /* 16 bytes */
typedef struct { unsigned short anio; unsigned char mes, dia, hora, min, seg, dia_semana; } W5Fecha;   /* 8 bytes; lunes = 0 */
typedef struct { unsigned char w, h; const unsigned short *filas; } W5Icono;   /* 1 bit por píxel; bit (w-1) = izquierda */
#define W5_RGB(r, g, b) ((unsigned)(((r) & 255) << 16 | ((g) & 255) << 8 | ((b) & 255)))
#define W5_TRANSPARENTE 0xFF000000u
enum { W5_COL_FONDO, W5_COL_TEXTO, W5_COL_TENUE, W5_COL_ACENTO, W5_COL_SOBRE_ACENTO, W5_COL_BARRA, W5_COL_N };   /* ui_color() */
enum { W5_F_MONO = 1, W5_F_BASTA = 2 };                                     /* marcas de texto */
enum { W5_SN_TECLA = 1, W5_SN_ERROR = 4, W5_SN_AVISO = 7 };                 /* ap_sonar() */
enum { W5_PIX_RGB565_BE = 1 };        /* 16 bits, R5 G6 B5, el byte alto primero en memoria */
enum { W5_AV_ALARMA = 1, W5_AV_TEMPORIZADOR = 2, W5_AV_BATERIA = 4 };
enum { W5_JG_BANDAS = 0,               /* el marco a lo ancho de la pantalla, 4:3, sin copia */
       W5_JG_GIRADA = 1,               /* el aparato de lado: el marco es alto x ancho y el sistema lo gira al volcar */
       W5_JG_PANEL = 2 };              /* (1.1) el panel ENTERO en su orientación (ancho x alto), sin copia: la app gira si quiere */
typedef struct { unsigned short hz, ms; } W5Nota;                           /* 4 bytes; hz 0 = silencio */
#define W5_OK 0
#define W5_NO_DISPONIBLE (-1)
#define W5_ERROR (-2)

/* ---- 5. LO QUE EL SISTEMA LE PIDE A LA APP ------------------------------------------------------------------
 * El punto de entrada es UNA función: W5Pal entrada(const W5Api *api, int suceso, W5Pal a, W5Pal b).
 * (El crt0 del SDK la pone, guarda `api` en `w5` y llama a app_suceso(suceso, a, b).) */
enum { W5_ABRIR = 1,                   /* recién cargada: devuelve su pantalla raíz (ui_pantalla(...)) o 0 si no puede */
       W5_PAUSA,                       /* deja de verse (un aviso del sistema encima, pantalla apagada, bloqueo): sin tics hasta W5_SIGUE */
       W5_SIGUE,
       W5_SALIR };                     /* se va a descargar: última ocasión de guardar */
/* Retrollamadas de la app (el sistema las llama SIEMPRE en la pila de la app y de una en una): */
typedef W5W  (*W5Construir)(W5Ctx c, void *dato);
typedef void (*W5Vida)(W5Ctx c, void *dato);                 /* iniciar / desechar de una pantalla */
typedef void (*W5Retro)(W5Ctx c);                            /* un temporizador */
typedef void (*W5Pintar)(W5Lienzo l, const W5Rect *r, void *dato);
typedef int  (*W5Tecla)(W5Ctx c, int tecla, void *dato);     /* 1 = me la quedo */
typedef W5W  (*W5Fila)(W5Ctx c, int i, int sel, void *dato);
typedef void (*W5FilaAcc)(W5Ctx c, int i, void *dato);
typedef void (*W5Accion)(W5Ctx c, void *dato);
typedef W5W  (*W5Enfocado)(W5Ctx c, int enfocado, int t, void *dato);   /* t: 0..256, animado */
typedef void (*W5Si)(void *dato);
typedef void (*W5Elegir)(int i, void *dato);

/* ---- 6. LOS WIDGETS: ui_w(clase, pares, n) --------------------------------------------------------------------
 * `pares` son n parejas de palabras { atributo, valor }. Lo que no se dice toma su valor por omisión. Un atributo
 * que esa clase no tiene, o un valor que no vale, es un error de la app (se la cierra con un mensaje).
 * Los NÚMEROS de clase y de atributo son parte de la ABI: solo se añaden por el final. */
enum { W5C_FLEX = 1, W5C_EXPANDIDO, W5C_RELLENO, W5C_ALINEAR, W5C_CAJA, W5C_PILA, W5C_REJILLA, W5C_LISTA,
       W5C_TEXTO, W5C_PARRAFO, W5C_VELO, W5C_BARRA_DESP, W5C_PROGRESO, W5C_PINTURA, W5C_LISTA_PEREZOSA,
       W5C_SELECTOR, W5C_CAMPO_TEXTO, W5C_ICONO, W5C_ENFOCABLE, W5C_TECLAS, W5C_N };
enum { W5A_CLAVE = 1,                  /* identidad entre hermanos (todas las clases) */
       W5A_HIJO, W5A_HIJOS,            /* un W5W / una lista hecha con ui_lista() */
       W5A_AN, W5A_AL,                 /* tamaño fijo; W5_INF = todo lo que haya; 0 = el del contenido */
       W5A_COLOR, W5A_BORDE, W5A_FONDO, W5A_SOMBRA, W5A_BARRA,   /* colores 0x00RRGGBB */
       W5A_VERTICAL, W5A_PRINCIPAL, W5A_CRUZADO, W5A_ENCOGE, W5A_ESPACIO,   /* Flex */
       W5A_FLEX,                       /* Expandido */
       W5A_IZQ, W5A_ARR, W5A_DER, W5A_ABA,                       /* Relleno */
       W5A_AX, W5A_AY,                 /* Alinear: -1, 0, 1 */
       W5A_COLUMNAS, W5A_ALTO_CELDA,   /* Rejilla */
       W5A_TEXTO, W5A_ESCALA, W5A_MARCAS, W5A_MAX_LINEAS, W5A_CENTRADO,
       W5A_TOTAL, W5A_VISIBLES, W5A_POS,                         /* BarraDesp */
       W5A_VALOR, W5A_MAX, W5A_MIN,    /* Progreso: valor; Selector: puntero a un int de la app */
       W5A_PINTAR, W5A_DATO, W5A_VERSION,                        /* Pintura */
       W5A_N, W5A_FILA, W5A_AL_ACTIVAR, W5A_AL_MOVER, W5A_SEL, W5A_ALTO_FILA, W5A_AUTOFOCO,   /* ListaPerezosa */
       W5A_AL_CAMBIAR, W5A_DIGITOS,    /* Selector, CampoTexto */
       W5A_ENTRADA, W5A_LINEAS,        /* CampoTexto: un W5Et */
       W5A_ICONO,                      /* Icono: un const W5Icono * */
       W5A_CONSTRUCTOR, W5A_AL_ENFOCAR, W5A_ANIM_MS,             /* Enfocable */
       W5A_AL_TECLA,                   /* Teclas */
       W5A_NUM };
#define W5_INF 0x3FFF
enum { W5P_INICIO, W5P_CENTRO, W5P_FIN, W5P_ENTRE, W5P_ESTIRAR };

/* ---- 7. LA TABLA ------------------------------------------------------------------------------------------------
 * El orden ES el número de cada función. SOLO SE AÑADE POR EL FINAL (herr/api_congelada.py lo comprueba contra
 * api/cargable_v1.lst). F(tipo de retorno, nombre, argumentos). */
/* cargable12_0.h — (1.2) el grupo de la pista JIT, el PRIMERO de la 1.2 (77 y 78): así los números que ya usan las apps no se
 * mueven. Al entregar, herr/juntar_cargable.py lo mete dentro de cargable.h. */
#ifndef CARGABLE12_0
#define CARGABLE12_0
enum { W5_CAP_JIT = 1 << 11 };          /* (1.2, pista JIT) la app GENERA CÓDIGO en su memoria y lo ejecuta: antes de saltar a él llama a
                                         * ap_jit_sincronizar(ini, n). Solo en el teléfono (en el banco de PC no hace falta ni se ofrece) */
/* una entrada de carpeta (fs_listar; también ma_listar del grupo C): 72 bytes, campo a campo */
typedef struct {
    char nombre[64];                    /* acabado en 0 (lo más largo, recortado) */
    unsigned int tam;                   /* bytes (0 en una carpeta) */
    unsigned char es_dir;               /* 1 = carpeta */
    unsigned char res[3];               /* a cero */
} W5Ent;
typedef char w5_ent_mide_72[sizeof(W5Ent) == 72 ? 1 : -1];
typedef W5Ent W5FsEntrada;              /* el nombre que usa la propuesta de la pista JIT: el mismo registro */
#define W5_API12_0(F) \
    F(int, ap_jit_sincronizar, (const void *ini, unsigned n))   /* W5_CAP_JIT: el código recién escrito en [ini, ini+n) de SU memoria ya se puede ejecutar */ \
    F(int, fs_listar, (const char *dir, unsigned desde, W5Ent *e, unsigned max))   /* CUALQUIER carpeta, solo lectura: hasta max entradas desde la n.º desde, COPIADAS a e; cuántas */
#if defined(TELEFONO) && TELEFONO
#define CG12_CAPS_0 ((unsigned)W5_CAP_JIT)
#else
#define CG12_CAPS_0 0u
#endif
#endif
/* cargable12_a.h — (1.2, pista APX) grupo A de la tabla. En desarrollo va aparte para que cada grupo se escriba sin pisar a
 * otro; al entregar, herr/juntar_cargable.py lo mete dentro de cargable.h (que vuelve a ser UN fichero).
 * Reglas: solo F(...) en W5_API12_A, una por línea, con su comentario; capacidades nuevas SOLO en los bits del grupo
 * (A: 11..15, B: 16..19, C: 20..23); constantes con prefijo W5_. Nada de punteros internos del sistema.
 *
 * GRUPO A: los servicios del sistema que usan Reloj, Movimiento, Teclas y Consola.
 *   - AJUSTES con lista blanca (W5_AJ_*): leer y poner, con rango validado; nunca el bloque de ajustes entero.
 *   - LA HORA: poner el reloj en hora local, la zona horaria en texto.
 *   - EL TEMPORIZADOR del sistema (la cuenta atrás que vigila el standby y avisa con la app cerrada): ponerlo y preguntar.
 *     La ALARMA diaria también es del sistema: se programa con los ajustes W5_AJ_ALARMA_*.
 *   - RETENCIONES (W5_RET_*): la pantalla encendida, las teclas de música para la app. Todas se sueltan al descargar.
 *   - EL PODÓMETRO (W5_CAP_PASOS): una foto del servicio copiada a un registro fijo (W5Pasos), y ponerlo a cero.
 *     El sensor EN VIVO se pide con ap_acel(1) (1.0), que también se suelta al descargar.
 *   - LA SERIE (W5_CAP_SERIE): la UART del aparato, por sondeo. La app que la usa es su dueña mientras está cargada.
 *   - LAS TECLAS CRUDAS (W5_CAP_TECLAS_CRUDAS): la matriz del teclado tal cual y el nombre de cada celda. */
#ifndef CARGABLE12_A
#define CARGABLE12_A
/* (bit 11 = W5_CAP_JIT, de la pista JIT: api/cargable12_0.h) */
enum { W5_CAP_PASOS = 1 << 12,         /* (1.2) hay servicio de podómetro (ap_pasos); si además hay sensor lo dice W5Pasos.hay_sensor */
       W5_CAP_SERIE = 1 << 13,         /* (1.2) hay un puerto serie para las apps (ap_serie_*) */
       W5_CAP_TECLAS_CRUDAS = 1 << 14 };   /* (1.2) se puede leer la matriz del teclado (ap_teclas_mapa, ap_celda_nombre) */
/* Las claves de ap_ajuste / ap_ajuste_poner. Solo estas; cualquier otra da W5_ERROR. */
enum { W5_AJ_HORA12 = 1,               /* 0/1: la hora en formato de 12 h (solo leer) */
       W5_AJ_ALARMA_ON,                /* 0/1: la alarma diaria está puesta (la vigila el sistema) */
       W5_AJ_ALARMA_H,                 /* 0..23 */
       W5_AJ_ALARMA_M };               /* 0..59 */
enum { W5_RET_ENERGIA = 1,             /* la pantalla no se atenúa ni se apaga (con tope: a los 10 min sin teclas vuelve a mandar la energía) */
       W5_RET_TECLAS_MUSICA = 2 };     /* el sistema no atiende Walkman, Play ni el volumen: llegan a la app */
/* La foto del podómetro (ap_pasos). 52 bytes, sin punteros. */
typedef struct {
    unsigned pasos, muestras, sacudidas, fallos;      /* pasos contados · muestras del sensor · sacudidas · lecturas fallidas */
    unsigned dist_dm, kmh10, idle, version;           /* distancia en decímetros · km/h x 10 · muestras desde el último candidato · sube con cada cambio */
    short x, y;                                       /* la última muestra respecto al cero (cuentas con signo) */
    short gx, gy;                                     /* la gravedad estimada, en cuentas */
    unsigned char crudo_x, crudo_y, eje, hz;          /* la muestra cruda (0..255) · eje de trabajo 1 = X, 2 = Y, 0 = ninguno · muestras por segundo */
    unsigned char andando, inactivo, hay_sensor, gesto;   /* cerrojo «andando» · 1 = ≥ 1 s sin candidatos · el sensor contesta · el gesto (0..7) */
    unsigned char candidatos, activo, dormido, res;   /* pasos regulares candidatos (0..8) · muestreando · sensor dormido por ahorro · 0 */
} W5Pasos;
typedef char w5_pasos_mide_52[sizeof(W5Pasos) == 52 ? 1 : -1];
#define W5_API12_A(F) \
    F(int, ap_ajuste, (int clave))                       /* el valor de un ajuste W5_AJ_*; W5_ERROR si la clave no está en la lista */ \
    F(int, ap_ajuste_poner, (int clave, int valor, int guardar))   /* lo cambia (rango validado; W5_AJ_HORA12 no); guardar = 1 lo lleva ya a la flash */ \
    F(int, ap_hora_poner, (const W5Fecha *f))            /* pone el reloj en hora LOCAL (anio, mes, dia, hora, min, seg); W5_ERROR si la fecha no existe */ \
    F(char *, ap_zona_texto, (void))                     /* la zona horaria del usuario, «UTC-4:00»; dura un cuadro */ \
    F(int, ap_temporizador_poner, (unsigned ms))         /* la cuenta atrás del SISTEMA vence dentro de ms (avisa aunque la app se cierre); 0 = pararla */ \
    F(int, ap_temporizador_queda, (void))                /* ms que le quedan, o W5_NO_DISPONIBLE si no hay cuenta atrás en marcha */ \
    F(int, ap_retener, (int que, int si))                /* W5_RET_*: pide (1) o suelta (0); al descargar se suelta todo */ \
    F(int, ap_pasos, (W5Pasos *p))                       /* COPIA la foto del podómetro; W5_NO_DISPONIBLE sin W5_CAP_PASOS */ \
    F(int, ap_pasos_a_cero, (void))                      /* el contador de pasos del sistema (el del standby) a cero */ \
    F(int, ap_serie_leer, (void *buf, unsigned max))     /* no bloquea: bytes recibidos (0 si nada); W5_NO_DISPONIBLE sin W5_CAP_SERIE */ \
    F(int, ap_serie_escribir, (const void *d, unsigned n))   /* bytes aceptados (0 sin cable: no cuelga) */ \
    F(int, ap_serie_conectado, (void))                   /* 1 si hay algo al otro lado del cable */ \
    F(int, ap_teclas_mapa, (unsigned char *col))         /* COPIA 7 bytes: col[f] bit k = celda f*5+k pulsada (f = 6: teclas a masa); devuelve cuántas */ \
    F(int, ap_celda_nombre, (int celda, char *buf, unsigned max))   /* COPIA el nombre real de la celda 0..34 («Vol+»); «» si no hay tecla; su longitud */
/* lo que ESTE sistema ofrece del grupo (solo lo mira el sistema; en el SDK no estorba) */
#define CG12_CAPS_A ((unsigned)(W5_CAP_PASOS | W5_CAP_SERIE | W5_CAP_TECLAS_CRUDAS))
#endif
/* cargable12_b.h — (1.2, pista APX) grupo B de la tabla: los SERVICIOS de MÚSICA y de LUCES, para las apps que son su
 * interfaz (apps_ext/musica: el Walkman; apps_ext/luces: el apartado de Ajustes). Los servicios se quedan en el sistema
 * (shell/musica.c: la música sigue sonando con la app cerrada, el filtro de Walkman/Play/volumen, guardar el punto;
 * shell/luces.c y api/luces.c: el motor, el latido, los efectos .ble): la app solo pregunta y ordena.
 * En desarrollo va aparte para que cada grupo se escriba sin pisar a otro; al entregar, herr/juntar_cargable.py lo mete
 * dentro de cargable.h (que vuelve a ser UN fichero).
 * Reglas: solo F(...) en W5_API12_B, una por línea, con su comentario; capacidades nuevas SOLO en los bits del grupo
 * (A: 11..15, B: 16..19, C: 20..23); constantes con prefijo W5_. Nada de punteros internos del sistema: todo lo que
 * sale se COPIA a un registro fijo (W5MuInfo, W5Luces) o a un búfer de la app, acabado en 0 y con tope. */
#ifndef CARGABLE12_B
#define CARGABLE12_B

/* ---- capacidades del grupo B (bits 16..19) ---- */
enum { W5_CAP_MUSICA = 1 << 16,        /* el servicio de música del sistema (mu_*) */
       W5_CAP_LUCES_EF = 1 << 17 };    /* el servicio de luces del sistema: ajustes de las laterales y efectos (lu_*) */

/* ---- MÚSICA ---- */
enum { W5_MU_PARADA, W5_MU_SONANDO, W5_MU_PAUSA };                     /* W5MuInfo.estado */
enum { W5_MU_SAL_NADA, W5_MU_SAL_PCM, W5_MU_SAL_ZUMBADOR, W5_MU_SAL_MP3 };            /* .salida: por dónde suena la pista en curso */
enum { W5_MU_MODO_AUTO, W5_MU_MODO_ZUMBADOR, W5_MU_MODOS };            /* .modo: PCM si hay / solo zumbador */
enum { W5_MU_REP_NO, W5_MU_REP_UNA, W5_MU_REP_TODAS, W5_MU_REPS };     /* .repetir */
enum { W5_MU_T_MIDI = 1, W5_MU_T_DSP, W5_MU_T_MP3 };                                /* mu_pista(): se puede tocar / lo decodifica el DSP (MP3, AAC, AMR…) */
#define W5_MU_VOL_MAX 5
#define W5_MU_RUTA 112                 /* lo más largo que mide una ruta de pista, con su 0 */
typedef struct {                       /* 56 bytes; lo llena mu_info() */
    unsigned char  estado, salida, volumen, modo;      /* W5_MU_*; volumen 0..W5_MU_VOL_MAX */
    unsigned char  repetir, pcm_probado, pcm_hay, cedida;   /* cedida: en pausa porque suena un aviso del sistema */
    unsigned char  guardado, reserva0, reserva1, reserva2;  /* guardado: 0 nada que decir, 1 guardado, 2 no hay zona de usuario, 3 falló */
    short          pista, n;                           /* la pista cargada (-1 = ninguna) y cuántas hay en la lista */
    unsigned int   pos_ms, dur_ms;                     /* posición y duración de la cargada */
    unsigned int   notas, percusion, perdidas, pistas_smf;   /* del MIDI cargado: notas, de percusión sin tocar, que no cupieron, pistas SMF */
    unsigned int   tam, pcm_marcos;                    /* bytes del fichero cargado · marcos PCM dados */
    int            pcm_motivo;                         /* por qué no hay PCM (el paso en que el DSP no contestó), o 0 */
    unsigned short hz, reserva3;                       /* la nota del zumbador ahora mismo */
} W5MuInfo;
typedef char w5_mu_info_mide_56[sizeof(W5MuInfo) == 56 ? 1 : -1];

/* ---- LUCES ---- */
enum { W5_LED_PANTALLA, W5_LED_TECLADO_DESL, W5_LED_TECLADO_2, W5_LED_TECLADO_1, W5_LEDS };   /* W5Luces.nivel[] */
typedef struct {                       /* 24 bytes; lo llena lu_leer() */
    unsigned char presente;            /* 1 = el chip de LED contesta */
    unsigned char efecto;              /* 1 = hay un efecto en marcha (lo que dice lu_sonando) */
    unsigned char luces, color, latido;/* los tres AJUSTES: laterales sí/no, color 0..n_colores-1 (0 = automático), latido sí/no */
    unsigned char n_colores, n_casa;   /* cuántos colores tiene la paleta (lu_color_nombre) y cuántos efectos de casa (lu_casa) */
    unsigned char reserva0;
    unsigned char nivel[W5_LEDS];      /* lo escrito en cada luz sencilla: pantalla 0..0x84, deslizador 0..0xFF, zonas del teclado 0..0x10 */
    unsigned char rgb[2][3];           /* cada lateral TAL COMO SE VERÍA ahora (0..255) */
    unsigned char reserva1[6];
} W5Luces;
typedef char w5_luces_mide_24[sizeof(W5Luces) == 24 ? 1 : -1];

#define W5_API12_B(F) \
    F(int, mu_abrir, (void))                             /* arranca el servicio si no lo estaba (busca las pistas, recupera lo guardado); da cuántas pistas hay */ \
    F(int, mu_buscar, (void))                            /* rehace la lista de pistas; da cuántas hay */ \
    F(int, mu_info, (W5MuInfo *m))                       /* COPIA el estado del servicio; W5_ERROR si m no es de la app */ \
    F(int, mu_pista, (int i, char *ruta, unsigned max))  /* COPIA la ruta de la pista i (ruta NULL: solo el tipo); da W5_MU_T_*, o W5_ERROR */ \
    F(int, mu_nombre, (int i, char *buf, unsigned max))  /* COPIA su nombre sin carpeta ni extensión; da la longitud, o W5_ERROR */ \
    F(int, mu_tocar, (int i))                            /* toca la pista i desde el principio: W5_OK, o W5_ERROR si no se deja leer como MIDI */ \
    F(void, mu_alternar, (void))                         /* reproducir / pausa (parada: la cargada, o la primera) */ \
    F(void, mu_parar, (void)) \
    F(void, mu_saltar, (int d))                          /* +1 siguiente, -1 anterior (a más de 3 s de pista, vuelve a empezarla) */ \
    F(int, mu_volumen, (int v))                          /* 0..W5_MU_VOL_MAX; W5_ERROR fuera de rango */ \
    F(int, mu_modo, (int m))                             /* W5_MU_MODO_*; W5_ERROR fuera de rango */ \
    F(int, mu_repetir, (int r))                          /* W5_MU_REP_*; W5_ERROR fuera de rango */ \
    F(void, mu_pantalla, (int abierta))                  /* RETENCIÓN: 1 = la pantalla del Walkman está montada (la tecla Walkman alterna en vez de abrir; el volumen es de la música); se suelta sola al descargar la app */ \
    F(int, lu_leer, (W5Luces *l))                        /* COPIA las luces de ahora y los ajustes; W5_ERROR si l no es de la app */ \
    F(int, lu_ajuste, (int luces, int color, int latido))   /* los TRES ajustes de luces, con rango (luces y latido 0/1, color 0..n_colores-1); los guarda; W5_ERROR si no valen */ \
    F(int, lu_color_nombre, (int i, char *buf, unsigned max))   /* COPIA el nombre del color i de la paleta («automático», «blanco»…); da la longitud, o W5_ERROR */ \
    F(int, lu_casa, (int i))                             /* reproduce el efecto de casa i (el que hubiera se para): W5_OK, W5_NO_DISPONIBLE (el chip no contesta, pantalla apagada), W5_ERROR */ \
    F(int, lu_casa_nombre, (int i, char *buf, unsigned max))   /* COPIA el nombre del efecto de casa i; da la longitud, o W5_ERROR */ \
    F(int, lu_fabricas, (char *buf, unsigned max))       /* COPIA los efectos de fábrica, «contacteffects/X.ble» y «systemeffects/Y.ble», uno tras otro, cada uno acabado en 0; da cuántos (los que quepan), o W5_NO_DISPONIBLE si no está el FS original */ \
    F(int, lu_fabrica, (const char *relativo))           /* reproduce uno de ellos (por su nombre relativo): W5_OK, W5_NO_DISPONIBLE (no está, o el chip no contesta), W5_ERROR (no es un efecto válido) */ \
    F(void, lu_parar, (void))                            /* para el efecto en marcha */ \
    F(int, lu_sonando, (char *buf, unsigned max))        /* COPIA el nombre del efecto en marcha («» = ninguno); da la longitud */ \
    F(int, lu_mantener, (void))                          /* mientras suena un efecto, que la pantalla no se atenúe (cuenta como actividad); sin efecto no hace nada; da 1 si lo había */

/* lo que ESTE sistema ofrece del grupo (solo lo mira el sistema; en el SDK no estorba) */
#define CG12_CAPS_B (W5_CAP_MUSICA | W5_CAP_LUCES_EF)
#endif
/* cargable12_c.h — (1.2, pista APX) grupo C de la tabla: CARPETAS, MIS ARCHIVOS (texto), IMÁGENES y lo que Notas e Imágenes
 * necesitan del sistema. Al entregar, herr/juntar_cargable.py lo mete dentro de cargable.h (que vuelve a ser UN fichero).
 * Reglas: solo F(...) en W5_API12_C, una por línea, con su comentario; capacidades nuevas SOLO en los bits del grupo
 * (A: 11..15, B: 16..19, C: 20..23); constantes con prefijo W5_. Nada de punteros internos del sistema.
 *
 * - fs_contar / fs_listar: CUALQUIER carpeta del teléfono, solo lectura (como fs_tam / fs_leer de la 1.0). Las entradas se
 *   COPIAN a un registro fijo W5Ent de la app.
 * - ma_*: Mis archivos, la carpeta del USUARIO (no la de la app: lo de la app sigue en ap_fich_*). Leer y listar: todo (ya se
 *   leía con fs_leer en /usuario). ESCRIBIR, solo: un nombre acabado en «.txt», o el fichero exacto con que el sistema abrió
 *   la app (Archivos -> «Editar el texto»: ap_argumento). Cada escritura cuenta en el tope de la app (20 por minuto, el mismo
 *   de ap_fich_escribir) y mide como mucho W5_MA_MAX bytes. Es atómica (tras un corte vale lo de antes o lo nuevo, entero).
 *   Nunca borra ni renombra: eso es de Archivos.
 * - im_*: una imagen (GIF, también animado; PNG; JPEG base) que DECODIFICA EL SISTEMA. La app tiene un MANEJADOR (un número),
 *   no un puntero: la pinta en su Pintura con im_pintar, la anima con im_siguiente y la cierra con im_cerrar; las que no
 *   cierre las suelta el sistema al descargarla. Dos a la vez como mucho.
 * - nt_*: las notas viven donde siempre, en el ALMACÉN del sistema (las claves de la Notas de casa: el orden y 12 huecos de
 *   W5_NT_TAM bytes): no se ven en Mis archivos ni gastan sus 64 ficheros, y las de antes de sacar Notas del firmware siguen
 *   ahí. Solo esas claves; cada escritura o borrado cuenta en el tope de la app (20 por minuto).
 * - ap_poner_fondo: el fondo del standby (el único ajuste del sistema que toca este grupo, y solo con una imagen que existe). */
#ifndef CARGABLE12_C
#define CARGABLE12_C

enum { W5_CAP_MIS_TEXTOS = 1 << 20,     /* (1.2) Mis archivos: ma_* */
       W5_CAP_IMAGENES = 1 << 21,       /* (1.2) el decodificador de imágenes del sistema (im_*) y el fondo (ap_poner_fondo) */
       W5_CAP_NOTAS = 1 << 22 };        /* (1.2) el sitio de las notas en el almacén del sistema (nt_*) */

/* W5Ent (la entrada de carpeta de fs_listar y ma_listar) está en api/cargable12_0.h: fs_listar es la 78, de la pista JIT */
/* el estado de Mis archivos (ma_estado): 16 bytes */
typedef struct {
    unsigned int montado;               /* 1 = este teléfono tiene Mis archivos */
    unsigned int escribible;            /* 1 = se puede guardar */
    unsigned int libre;                 /* bytes que caben todavía en UN fichero nuevo (por lo bajo) */
    unsigned int ficheros;              /* cuántos hay (el máximo son 64) */
} W5MaEstado;
typedef char w5_ma_estado_mide_16[sizeof(W5MaEstado) == 16 ? 1 : -1];
/* lo que se sabe de una imagen abierta (im_info): 12 bytes */
typedef struct {
    unsigned short w, h;                /* lo que mide YA DECODIFICADA (lo que pinta im_pintar) */
    unsigned short w_orig, h_orig;      /* lo que mide el fichero */
    unsigned char formato;              /* W5_IM_GIF, W5_IM_PNG, W5_IM_JPEG */
    unsigned char reduccion;            /* 1, 2, 4 u 8 (decodificada a 1/n para que quepa) */
    unsigned char parcial;              /* 1 = el fichero está dañado y se ve a medias */
    unsigned char animado;              /* 1 = im_siguiente ya ha dado algún cuadro nuevo (es un GIF animado) */
} W5ImInfo;
typedef char w5_im_info_mide_12[sizeof(W5ImInfo) == 12 ? 1 : -1];
enum { W5_IM_GIF, W5_IM_PNG, W5_IM_JPEG };
/* im_abrir: un manejador (> 0) o uno de estos (o W5_ERROR si la ruta no es una cadena de la app) */
enum { W5_IM_E_ROTO = -10,              /* no es un GIF, PNG o JPEG válido (o no existe) */
       W5_IM_E_VARIANTE = -11,          /* variante que no se decodifica (JPEG progresivo, aritmético, de 12 bits, CMYK…) */
       W5_IM_E_MEMORIA = -12,           /* el fichero pasa de 300 KB o la imagen es demasiado grande */
       W5_IM_E_TIEMPO = -13,            /* tardaba demasiado en decodificarse */
       W5_IM_E_MUCHAS = -14 };          /* la app ya tiene dos abiertas */
/* ma_motivo: por qué falló la última ma_escribir (los mismos números que HW_USR_M_* del sistema, y dos más) */
enum { W5_MA_M_NADA, W5_MA_M_LLENO, W5_MA_M_NOMBRE, W5_MA_M_MUCHOS, W5_MA_M_FLASH, W5_MA_M_EXISTE, W5_MA_M_NO_HAY,
       W5_MA_M_OCUPADA, W5_MA_M_FUENTE, W5_MA_M_MISMO, W5_MA_M_SIN_ZONA, W5_MA_M_SOLO_LECTURA, W5_MA_M_CANCELADA,
       W5_MA_M_TOPE = 100,              /* demasiadas escrituras seguidas (20 por minuto) */
       W5_MA_M_PROHIBIDO = 101 };       /* ese nombre no es un .txt ni el fichero con que se abrió la app (o mide más de W5_MA_MAX) */
#define W5_MA_MAX 16384u                /* bytes como mucho por ma_escribir */
#define W5_MA_MAX_NOMBRE 47             /* bytes de un nombre de Mis archivos */
enum { W5_NT_ORDEN = -1, W5_NT_N = 12, W5_NT_TAM = 400 };   /* nt_*: el orden (1 + W5_NT_N bytes como mucho) y los huecos */
enum { W5_ET_ABC, W5_ET_ABC_INICIAL, W5_ET_MAYUS, W5_ET_123 };   /* et_poner_modo: «abc», «Abc», «ABC», «123» */
typedef void (*W5Nombre)(const char *nombre, void *dato);       /* ui_pedir_nombre: el nombre (dura un cuadro) */

#define W5_API12_C(F) \
    F(int, fs_contar, (const char *dir))                 /* entradas de esa carpeta (cualquiera, solo lectura); W5_NO_DISPONIBLE si no existe */ \
    F(int, ma_estado, (W5MaEstado *e))                   /* Mis archivos: montado, escribible, libre, ficheros; W5_OK */ \
    F(int, ma_listar, (unsigned desde, W5Ent *e, unsigned max))   /* los ficheros de Mis archivos por orden de nombre, COPIADOS; cuántos */ \
    F(int, ma_tam, (const char *nombre))                 /* bytes de ese fichero de Mis archivos, o W5_NO_DISPONIBLE */ \
    F(int, ma_leer, (const char *nombre, unsigned desde, void *buf, unsigned max))   /* bytes leídos; W5_NO_DISPONIBLE; W5_ERROR (dañado) */ \
    F(int, ma_escribir, (const char *nombre, const void *d, unsigned n))   /* entero y atómico; solo .txt o el fichero del argumento; W5_OK o W5_ERROR (ma_motivo) */ \
    F(int, ma_motivo, (void))                            /* W5_MA_M_*: por qué falló la última ma_escribir */ \
    F(const char *, ma_motivo_texto, (void))             /* ese motivo dicho para una persona (las palabras del sistema); dura un cuadro */ \
    F(void, ui_pedir_nombre, (const char *titulo, const char *inicial, W5Nombre hecho, void *dato))   /* la pantalla del sistema que pide un nombre de fichero */ \
    F(int, et_poner_modo, (W5Et e, int modo))            /* W5_ET_*: el modo de escritura de esa entrada; W5_OK */ \
    F(int, nt_leer, (int hueco, void *buf, unsigned max))   /* hueco W5_NT_ORDEN = el orden, 0..W5_NT_N-1 = una nota: COPIA y da los bytes que mide; W5_NO_DISPONIBLE si no existe */ \
    F(int, nt_guardar, (int hueco, const void *d, unsigned n))   /* W5_OK; W5_ERROR (no cabe, fallo de flash, tope); W5_NO_DISPONIBLE (sin almacén o sin escritura) */ \
    F(int, nt_olvidar, (int hueco))                      /* W5_OK; W5_NO_DISPONIBLE (no existía); W5_ERROR */ \
    F(int, nt_escribible, (void))                        /* 1 = el almacén guarda (sobrevive a apagar); 0 = no */ \
    F(int, im_abrir, (const char *ruta))                 /* decodifica una imagen: manejador > 0, o W5_IM_E_* */ \
    F(int, im_info, (int im, W5ImInfo *i))               /* la COPIA a i; W5_OK o W5_ERROR (no es un manejador vivo) */ \
    F(int, im_siguiente, (int im))                       /* avanza un cuadro de un GIF animado: sus ms; 0 = no está animada */ \
    F(void, im_pintar, (W5Lienzo l, int im, int x, int y))   /* dentro de una retrollamada de pintar: la imagen con su esquina en (x, y) */ \
    F(void, im_cerrar, (int im))                         /* la suelta (las que no, se sueltan al descargar la app) */ \
    F(int, ap_poner_fondo, (const char *ruta))           /* el fondo del standby («» lo quita): W5_OK; W5_ERROR (no se pudo cargar); W5_NO_DISPONIBLE (no cabía: ya lo ha dicho el sistema) */
/* lo que ESTE sistema ofrece del grupo (solo lo mira el sistema; en el SDK no estorba) */
#define CG12_CAPS_C (W5_CAP_MIS_TEXTOS | W5_CAP_IMAGENES | W5_CAP_NOTAS)
#endif
/* cargable12_d.h — (1.2, pista APX) grupo D: abrir una app CON algo (la abre el sistema desde Archivos, Ajustes, el standby…).
 * Al entregar, herr/juntar_cargable.py lo mete dentro de cargable.h. */
#ifndef CARGABLE12_D
#define CARGABLE12_D
#define W5_API12_D(F) \
    F(int, ap_argumento, (char *buf, unsigned max))   /* con qué la abrió el sistema («» desde el menú): lo COPIA (acabado en 0) y da su longitud; W5_ERROR si buf no es suyo */ \
    F(int, ap_luz, (int n, int encendida))           /* W5_CAP_LUCES: la luz n (1..W5_LUCES) encendida o apagada; al descargar la app vuelven como estaban */ \
    F(int, ap_luz_estado, (int n))                   /* 1 encendida, 0 apagada, W5_NO_DISPONIBLE si no hay o no contesta */
#define W5_LUCES 3                     /* las luces que maneja ap_luz (las del teclado/laterales: no el brillo de la pantalla) */
#define CG12_CAPS_D 0u
#endif
/* ---- (1.3, pista MD) EL SISTEMA DE DISEÑO: componentes semánticos y tokens. docs/material.md §1 y §8 ----------------------
 * Los nombres dicen QUÉ es cada cosa, no cómo se ve: el diseño en uso (de serie «material3»; también «clasico») decide el aspecto.
 * Una app escrita contra esto se ve bien con cualquier diseño, ahora y con los que vengan. Todo lo que entra se VALIDA y se COPIA
 * (cadenas, el registro W5FilaDatos, el icono): docs/proteccion_memoria.md. */
#ifndef CARGABLE13
#define CARGABLE13
enum { W5_ACC_NADA, W5_ACC_VALOR, W5_ACC_INTERRUPTOR, W5_ACC_CASILLA, W5_ACC_OPCION, W5_ACC_FLECHA, W5_ACC_N };   /* lo de la derecha de una fila */
enum { W5_PRIO_PRINCIPAL, W5_PRIO_SECUNDARIO, W5_PRIO_TERCIARIO };                  /* la prioridad de un botón */
enum { W5_CTL_INTERRUPTOR, W5_CTL_CASILLA, W5_CTL_OPCION };                         /* ui_control */
enum { W5_TECLA_NUMERO, W5_TECLA_OPERACION, W5_TECLA_ACCION };                     /* ui_tecla_boton: el papel de la tecla */
/* tipografía: en W5A_MARCAS de un Texto o un Párrafo, junto a W5_F_MONO/W5_F_BASTA. Sin estilo manda la escala. */
enum { W5_T_ESCALA, W5_T_CUERPO, W5_T_ETIQUETA, W5_T_TITULO, W5_T_TITULAR, W5_T_DISPLAY, W5_T_DISPLAY_G,
       W5_T_PEQUENA /* (1.7) la talla pequeña: segundas líneas, ayudas, la hora de la barra */ };
#define W5_F_ESTILO(t) ((t) << 4)
/* tokens (ui_token): los colores dan 0x00RRGGBB; las medidas, píxeles o ms. El ORDEN es ABI: solo se añade al final. */
enum { W5_TK_PRIMARIO, W5_TK_SOBRE_PRIMARIO, W5_TK_CONTENEDOR_PRIMARIO, W5_TK_SOBRE_CONTENEDOR_PRIMARIO,
       W5_TK_SECUNDARIO, W5_TK_CONTENEDOR_SECUNDARIO, W5_TK_SOBRE_CONTENEDOR_SECUNDARIO,
       W5_TK_TERCIARIO, W5_TK_CONTENEDOR_TERCIARIO, W5_TK_SOBRE_CONTENEDOR_TERCIARIO,
       W5_TK_ERROR, W5_TK_SOBRE_ERROR, W5_TK_CONTENEDOR_ERROR, W5_TK_SOBRE_CONTENEDOR_ERROR,
       W5_TK_SUPERFICIE, W5_TK_SOBRE_SUPERFICIE, W5_TK_SOBRE_SUPERFICIE_VARIANTE,
       W5_TK_SUPERFICIE_CONTENEDOR_MINIMO, W5_TK_SUPERFICIE_CONTENEDOR_BAJO, W5_TK_SUPERFICIE_CONTENEDOR,
       W5_TK_SUPERFICIE_CONTENEDOR_ALTO, W5_TK_SUPERFICIE_CONTENEDOR_MAXIMO,
       W5_TK_CONTORNO, W5_TK_CONTORNO_VARIANTE, W5_TK_SUPERFICIE_INVERSA, W5_TK_SOBRE_SUPERFICIE_INVERSA, W5_TK_PRIMARIO_INVERSO,
       W5_TK_VELO,
       W5_TK_RADIO_PEQ = 64, W5_TK_RADIO_MED, W5_TK_RADIO_GRA, W5_TK_REJILLA, W5_TK_OSCURO, W5_TK_ANIM_CORTA, W5_TK_ANIM_MEDIA,
       W5_TK_ALTO_FILA, W5_TK_ALTO_FILA_DOBLE,
       W5_TK_ALTO_BARRA_SUPERIOR, W5_TK_ALTO_BARRA_TECLAS, W5_TK_ALTO_LINEA /* (1.7) las medidas de la letra en uso */ };
typedef struct {                        /* una fila de lista: se COPIA */
    const char *titulo, *subtitulo, *valor;    /* subtítulo y valor pueden ser NULL */
    const W5Icono *icono;                      /* NULL = sin icono */
    unsigned char accesorio, activo, res[2];   /* W5_ACC_*; activo = 1 marcado; res a 0 */
} W5FilaDatos;
#define W5_API13(F) \
    F(W5W, ui_fila_de, (const W5FilaDatos *f, int sel))      /* una fila de lista con icono, título, subtítulo y accesorio */ \
    F(W5W, ui_boton, (const char *texto, int prioridad, int enfocado))   /* W5_PRIO_*: para el constructor de un Enfocable */ \
    F(W5W, ui_tarjeta, (W5W hijo, int enfocada))         /* un bloque de contenido agrupado */ \
    F(W5W, ui_chip, (const char *texto, int elegido, int enfocado))   /* una opción compacta (filtro) */ \
    F(W5W, ui_control, (int tipo, int activo, int enfocado))          /* W5_CTL_*: interruptor, casilla u opción */ \
    F(W5W, ui_deslizador, (int valor, int max, int enfocado))         /* un valor en un rango (lo mueve la app con izq./der.) */ \
    F(void, ui_aviso_breve, (const char *texto))         /* una tira abajo que se va sola a los 3 s (no quita el foco) */ \
    F(unsigned, ui_token, (int clave))                   /* W5_TK_*: el color (0x00RRGGBB) o la medida del diseño en uso; 0 si no existe */ \
    F(int, ui_diseno, (char *buf, unsigned max))         /* COPIA el nombre del diseño en uso («material3», «clasico»); su longitud */ \
    F(int, ui_diseno_pedir, (const char *nombre))        /* pide un diseño concreto mientras la app esté cargada: W5_OK, o W5_NO_DISPONIBLE (sigue el de serie); NULL = soltar */ \
    F(W5W, ui_tecla_boton, (const char *texto, int rol, int elegida))   /* una tecla de un teclado en pantalla que llena su caja: W5_TECLA_* */
#endif
#define W5_API(F) \
    /* 0: base */ \
    F(void *, memcpy, (void *d, const void *s, size_t n)) \
    F(void *, memset, (void *d, int c, size_t n)) \
    F(void *, memmove, (void *d, const void *s, size_t n)) \
    F(int, memcmp, (const void *a, const void *b, size_t n)) \
    F(size_t, strlen, (const char *s)) \
    F(int, strcmp, (const char *a, const char *b)) \
    /* 6: la app y el aparato */ \
    F(int, ap_consulta, (int que))                       /* W5_Q_*; -1 si esa consulta no existe */ \
    F(void, ap_salir, (void))                            /* cierra la app (como «Atrás» en su pantalla raíz) */ \
    F(void, ap_latido, (void))                           /* «sigo viva» dentro de un cálculo largo (como mucho 60 s por llamada) */ \
    F(void *, ap_mem_pedir, (unsigned n))                /* de SU montón; a cero; NULL si no cabe */ \
    F(void, ap_mem_soltar, (void *p)) \
    F(unsigned, ap_mem_libre, (void)) \
    F(void, ap_log, (const char *s))                     /* traza de depuración */ \
    F(unsigned, ap_ms, (void))                           /* milisegundos desde el arranque */ \
    F(int, ap_fecha, (W5Fecha *f))                       /* hora local; W5_ERROR si el reloj no está en hora */ \
    F(char *, ap_hora_texto, (int con_segundos))         /* en el formato del usuario; dura un cuadro */ \
    /* 16: ficheros. Los de la app llevan SU prefijo, que pone el sistema: no ve los de nadie más */ \
    F(int, ap_fich_tam, (const char *nombre))            /* bytes, o W5_NO_DISPONIBLE */ \
    F(int, ap_fich_leer, (const char *nombre, unsigned desde, void *buf, unsigned max)) \
    F(int, ap_fich_escribir, (const char *nombre, const void *d, unsigned n))   /* entero y atómico */ \
    F(int, ap_fich_borrar, (const char *nombre)) \
    F(int, fs_tam, (const char *ruta))                   /* cualquier fichero del teléfono, SOLO LECTURA */ \
    F(int, fs_leer, (const char *ruta, unsigned desde, void *buf, unsigned max)) \
    /* 22: teclas, sensores, sonido (todo opcional: mira W5_Q_CAPS) */ \
    F(int, ap_tecla_abajo, (int k))                      /* 1 si esa tecla está pulsada ahora */ \
    F(int, ap_acel, (int encendido))                     /* pide o suelta el acelerómetro */ \
    F(int, ap_acel_leer, (int *x, int *y))               /* cuentas con signo, 0 = plano */ \
    F(int, ap_vibrar, (int ms)) \
    F(int, ap_tono, (unsigned hz, unsigned ms))          /* se apaga solo */ \
    F(int, ap_sonar, (int cual))                         /* W5_SN_*: con el volumen del usuario */ \
    F(int, ap_bateria, (void))                           /* 0..100, o W5_NO_DISPONIBLE */ \
    /* 29: construir la interfaz */ \
    F(W5W, ui_w, (unsigned clase, const W5Pal *pares, unsigned n)) \
    F(W5W *, ui_lista, (const W5W *v, unsigned n))       /* copia; quita los nulos; acaba en nulo */ \
    F(char *, ui_fmt, (const char *f, ...))              /* %d %u %x %s %c, con 0 y ancho; dura un cuadro */ \
    F(void *, ui_arena, (unsigned n))                    /* memoria que dura un cuadro */ \
    F(W5W, ui_pantalla, (W5Construir construir, W5Vida iniciar, W5Vida desechar, void *dato)) \
    F(W5W, ui_andamio, (const char *titulo, W5W cuerpo, const char *izq, const char *der))   /* barra de título + cuerpo + teclas de selección */ \
    F(W5W, ui_fila, (const char *texto, const char *detalle, int sel)) \
    F(W5W, ui_dato, (const char *nombre, const char *valor)) \
    F(unsigned, ui_color, (int rol))                     /* W5_COL_*: la paleta del tema en uso */ \
    F(void, ui_sucio, (W5Ctx c))                         /* «he cambiado: reconstrúyeme» */ \
    F(W5Ctx, ui_mi_pantalla, (W5Ctx c))                  /* la pantalla de la app que contiene a c */ \
    F(int, ui_cada, (W5Ctx c, unsigned ms, W5Retro f))   /* mueren con c; no laten en pausa */ \
    F(int, ui_tras, (W5Ctx c, unsigned ms, W5Retro f)) \
    F(void, ui_cancelar, (W5Ctx c, W5Retro f)) \
    F(void, ui_empujar, (W5W pantalla))                  /* otra pantalla de la app, encima */ \
    F(void, ui_sacar, (void)) \
    F(void, ui_tecla, (int k))                           /* entrega una tecla como si se pulsara */ \
    F(int, ui_teclas_defecto, (W5Ctx c, int k))          /* sel. izq. = OK, sel. der. / atrás = salir */ \
    F(void, ui_aviso, (const char *titulo, const char *texto)) \
    F(void, ui_confirmar, (const char *titulo, const char *texto, W5Si si, void *dato)) \
    F(void, ui_opciones, (const char *titulo, const char *const *ops, int n, W5Elegir elegir, void *dato)) \
    /* 50: pintar */ \
    F(void, li_rect, (W5Lienzo l, int x, int y, int w, int h, unsigned color)) \
    F(void, li_marco, (W5Lienzo l, int x, int y, int w, int h, unsigned color)) \
    F(void, li_linea_h, (W5Lienzo l, int x, int y, int w, unsigned color)) \
    F(void, li_punto, (W5Lienzo l, int x, int y, unsigned color)) \
    F(void, li_texto, (W5Lienzo l, int x, int y, const char *utf8, unsigned color, int escala, int marcas)) \
    F(void, li_texto_n, (W5Lienzo l, int x, int y, const char *utf8, int bytes, unsigned color, int escala, int marcas)) \
    F(int, li_ancho_texto, (const char *utf8, int escala, int marcas)) \
    F(int, li_partir, (const char *utf8, int ancho, int escala, int marcas)) \
    F(void, li_velo, (W5Lienzo l, int x, int y, int w, int h)) \
    F(void, li_icono, (W5Lienzo l, int x, int y, const W5Icono *i, unsigned color, int escala)) \
    /* 60: entrada de texto */ \
    F(W5Et, et_nueva, (unsigned cap))                    /* del montón de la app; NULL si no cabe */ \
    F(void, et_poner, (W5Et e, const char *s)) \
    F(const char *, et_texto, (W5Et e)) \
    F(unsigned, et_len, (W5Et e)) \
    F(int, et_insertar, (W5Et e, const char *utf8)) \
    F(int, et_borrar, (W5Et e)) \
    F(const char *, et_modo, (W5Et e))                   /* «abc», «Abc», «ABC», «123» */ \
    /* 67: recursos grandes de solo lectura (van al final del .sen; no ocupan RAM) */ \
    F(unsigned, ap_rec_tam, (void)) \
    F(int, ap_rec_leer, (unsigned desde, void *buf, unsigned max))   /* bytes leídos */ \
    F(const void *, ap_rec_mapa, (unsigned desde, unsigned n))   /* puntero EN EL SITIO si el fichero está en flash mapeada y contiguo; si no, NULL */ \
    F(int, ap_rec_comprobar, (void))                     /* recalcula el CRC de los recursos: W5_OK o W5_ERROR (lento) */ \
    /* 71: PERFIL JUEGO: pantalla completa y bucle propio (solo apps con perfil W5_PERFIL_JUEGO) */ \
    F(void *, jg_tomar, (int modo, int *an, int *al, int *paso))   /* la pantalla: puntero al marco (W5_Q_PIXEL) o NULL */ \
    F(void, jg_volcar, (void))                           /* el marco al panel; de paso, late */ \
    F(void, jg_soltar, (void))                           /* devuelve la pantalla; después solo queda volver de W5_ABRIR */ \
    F(void, jg_espera, (void))                           /* duerme hasta la siguiente interrupción (como mucho un tic) */ \
    F(int, jg_teclas, (unsigned *a, unsigned *b))        /* TODAS las teclas pulsadas ahora: bit k de a (k < 32) o de b; devuelve cuántas */ \
    F(int, jg_melodia, (const W5Nota *notas, unsigned n, int repetir_desde))   /* por el zumbador, la toca el sistema; NULL la calla */ \
    /* 77: (1.2) servicios del sistema para las apps de utilidad */ \
    W5_API12_0(F) W5_API12_A(F) W5_API12_B(F) W5_API12_C(F) W5_API12_D(F) \
    W5_API13(F)

#define W5_API_MAGIA 0x49503557u       /* «W5PI» */
typedef struct W5Api {
    unsigned magia;                    /* W5_API_MAGIA */
    unsigned short mayor, menor;       /* la versión de ESTE sistema */
    unsigned n;                        /* cuántas funciones tiene la tabla de ESTE sistema */
    unsigned tam_puntero;              /* 4 u 8 */
#define W5_F(r, nombre, args) r (*nombre) args;
    W5_API(W5_F)
#undef W5_F
} W5Api;
typedef W5Pal (*W5Entrada)(const W5Api *api, int suceso, W5Pal a, W5Pal b);
#endif
