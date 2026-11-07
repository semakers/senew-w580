/* w6f.h — «W6F»: sistema de ficheros ESCRIBIBLE de registro, para flash con páginas de escritura única.
 *
 * C99 portable, sin malloc, sin dependencias: la memoria se la da quien lo monta y la flash se ve por una
 * interfaz pequeña (W6fFlash) con la GEOMETRÍA COMO DATO. Perfiles probados (fs2/pruebas): NOR Intel M18 en
 * modo objeto (W580i: bloques de 256 KiB, páginas de 1 KiB), NAND de página grande (2048+64, 64 páginas por
 * bloque, bloques malos de fábrica) y NOR clásica (bloques de 128 KiB, páginas lógicas de 512).
 *
 * LO QUE EL FS LE PIDE A LA FLASH (y nada más):
 *   - bloques de borrado de «ppb» páginas de «pag» bytes;
 *   - una página se programa ENTERA, UNA sola vez entre dos borrados, y solo si está en blanco (todo 0xFF);
 *   - dentro de un bloque las páginas se programan en orden creciente (lo exige la NAND; a la NOR le da igual);
 *   - nunca se reprograma, nunca se pide subir un bit, no hay actualización en el sitio de ninguna clase.
 *
 * GARANTÍAS (fs2/pruebas las demuestran con corte en cada orden de flash):
 *   - cada operación (guardar/añadir/truncar/renombrar/mover/borrar/crear directorio) es ATÓMICA: tras un corte
 *     vale entero lo de antes o entero lo de después; los demás ficheros no cambian;
 *   - el montaje tolera cualquier contenido y está acotado: O(páginas · log páginas), sin bucles sin tope;
 *   - el espacio libre se da EXACTO en páginas.
 *
 * El formato en disco está en docs/fs_propio.md §2 y lo lee, sin este código, fs2/herr/w6f_extraer.py. */
#ifndef W6F_H
#define W6F_H
#include <stdint.h>

#define W6F_VERSION      2             /* 2 = con el bit «dañada» en el lote y max_obj en la cabecera de bloque. Un volumen de
                                          CUALQUIER otra versión (anterior o posterior) no se monta ni se toca: W6F_E_VERSION */
#define W6F_NOMBRE_MAX   255           /* bytes UTF-8 de un nombre (un componente de la ruta) */
#define W6F_RUTA_MAX     1023
#define W6F_PROF_MAX     16            /* niveles de directorio */
#define W6F_MAX_ESCR     2             /* escrituras a trozos abiertas a la vez */
#define W6F_CAB          28            /* cabecera de página */

#ifdef W6F_PAG32                       /* más de 65 527 páginas (p. ej. una NAND de 256 MiB entera): 4 bytes por página */
typedef uint32_t w6f_pag;
#define W6F_P_MAX 0xFFFFFFFFu
#else
typedef uint16_t w6f_pag;
#define W6F_P_MAX 0xFFFFu
#endif

enum { W6F_OK = 0, W6F_E_NO_HAY = -1, W6F_E_ARG = -2, W6F_E_LLENO = -3, W6F_E_FLASH = -4, W6F_E_SIN_MONTAR = -5,
       W6F_E_MUCHOS = -6, W6F_E_EXISTE = -7, W6F_E_OCUPADO = -8, W6F_E_NO_VACIO = -9, W6F_E_NO_DIR = -10,
       W6F_E_ES_DIR = -11, W6F_E_CRC = -12, W6F_E_DANADO = -13, W6F_E_VIRGEN = -14, W6F_E_AJENO = -15,
       W6F_E_FORMATO = -16, W6F_E_SOLO_LECTURA = -17, W6F_E_GEOMETRIA = -18, W6F_E_MEMORIA = -19,
       W6F_E_NOMBRE = -20, W6F_E_PROFUNDO = -21,
       W6F_E_VERSION = -22 /* el volumen es de OTRA versión del formato (anterior o posterior): NO es «ajeno», no se le da formato sin más */ };

/* Por qué un volumen montado no admite escrituras (W6fEstado.solo_lectura). */
enum { W6F_SL_NO = 0, W6F_SL_PEDIDO = 1,        /* conf.solo_lectura */
       W6F_SL_SECUENCIA = 2,                    /* la secuencia global está al borde de agotarse: lo que se escribiera no se podría ordenar */
       W6F_SL_SIN_RESERVA = 3,                  /* no queda ni un bloque libre ni una página en blanco ni nada que borrar (bloques que han muerto) */
       W6F_SL_FLASH = 4 };                      /* un punto de control imprescindible no se pudo escribir */
/* Los motivos 3 y 4 NO son para siempre: se revalúan al entrar en cada operación, en cada w6f_gc_paso y al montar; si la
 * causa ha desaparecido (la flash vuelve a dejarse, hay algo que borrar) el volumen vuelve a admitir escrituras. */

enum { W6F_FL_NOR_OBJETO = 1,          /* NOR de regiones de escritura única (Intel M18 en modo objeto) */
       W6F_FL_NAND = 2,                /* NAND: página + OOB; el puerto pone la ECC y la marca de bloque malo */
       W6F_FL_NOR_CLASICA = 3 };       /* NOR reprogramable palabra a palabra (no se aprovecha: se usa igual) */

/* LA FLASH. «blq» es el índice del bloque DENTRO de la lista que se le da al FS (el puerto traduce a direcciones:
 * así la zona puede ser una lista de tramos). Todas devuelven 0 = bien, < 0 = mal. */
typedef struct W6fFlash {
    void    *ctx;
    uint32_t clase;                    /* W6F_FL_*: se apunta en la cabecera de cada bloque; el FS no cambia de conducta por ella */
    uint32_t n_bloques;                /* bloques de la zona (2 .. ) */
    uint32_t pag;                      /* bytes de una página = unidad de programa (512 .. 4096, múltiplo de 4) */
    uint32_t ppb;                      /* páginas por bloque de borrado (4 .. 4096) */
    int  (*leer)(void *ctx, uint32_t blq, uint32_t pagina, uint32_t off, void *buf, uint32_t n);
    int  (*programar)(void *ctx, uint32_t blq, uint32_t pagina, const uint8_t *buf);   /* la página entera; «buf» está en RAM */
    int  (*borrar)(void *ctx, uint32_t blq);
    int  (*es_malo)(void *ctx, uint32_t blq);          /* opcional (NAND): 1 = bloque malo, no se toca jamás */
    int  (*marcar_malo)(void *ctx, uint32_t blq);      /* opcional (NAND): apuntarlo en el OOB */
} W6fFlash;

typedef struct {
    uint32_t max_obj;                  /* ficheros + directorios, contando la raíz (2 .. 65535) */
    uint32_t reserva_bloques;          /* bloques que no se dan al usuario (>= 3): DOS están siempre libres —el destino de la
                                          recolección y uno de repuesto, para que un bloque que muera al borrarlo no deje al
                                          volumen sin destino; el repuesto se REPONE solo— y el resto es holgura para que
                                          recolectar cueste menos (recomendado: 4) */
    uint32_t umbral_desgaste;          /* diferencia de borrados que dispara el nivelado estático (0 = 8) */
    uint32_t solo_lectura;             /* 1 = no se emite ninguna orden de escritura */
    uint32_t tope_paso;                /* programas de página por paso de recolección DENTRO de una escritura (0 = 32) */
    void   (*respiro)(void *ctx);      /* opcional: se llama entre dos pasos de esa recolección (pintar, dar de comer al perro) */
    void    *respiro_ctx;              /* OJO: desde «respiro» NO se puede llamar a nada que cambie el volumen (da W6F_E_OCUPADO); leer sí */
    uint32_t relecturas;               /* veces que se RELEE una página que sale mal al recolectarla antes de darla por dañada
                                          (0 = 2). Súbelo en un puerto con lecturas pasajeramente malas (NAND) */
    uint32_t reusar_blanco;            /* 0 (lo seguro, por omisión): NUNCA se programa una página en blanco heredada de antes del
                                          montaje sin borrar su bloque —un corte pudo dejarla a medio programar aunque se lea en
                                          blanco—: los bloques a medias se cierran al montar y un bloque libre heredado se borra
                                          otra vez al abrirlo (un borrado por bloque que se abre en cada sesión).
                                          1: se sigue escribiendo donde se quedó, comprobando solo que se lea en blanco */
} W6fConf;

typedef struct { uint32_t seq; w6f_pag nodo, primera; uint16_t padre; uint8_t hash, marcas; } W6fObj;
typedef struct { uint32_t borrados; uint16_t vivas; uint8_t estado, fallos; } W6fBlq;
typedef struct {
    uint8_t  activo, modo, nl, escrito0;
    uint16_t id, padre;
    uint32_t gen, tam, fecha, n_buf, npag, pend, base;
    w6f_pag  primera, ultima, cola0, ant;
    uint8_t  tenia_cola0;
    char     nombre[W6F_NOMBRE_MAX + 1];
    uint8_t *buf;
} W6fEscr;

typedef struct {
    uint32_t montajes_lecturas;        /* lecturas de flash del último montaje */
    uint32_t programas, borrados, copias_gc, fallos_programa, fallos_borrado, incoherencias;
    uint32_t danados;                  /* objetos dañados o huérfanos vistos al montar */
    uint32_t peor_paso_programas;      /* el paso de recolección más largo, en programas de página */
    uint32_t lotes_abortados, ciclos_gc, emergencias;
    uint32_t copias_danadas;           /* páginas que la recolección encontró rotas y copió marcadas como tales */
    uint32_t perdidos;                 /* objetos cuyo nodo se pudrió sin remedio */
    uint32_t lotes_parciales, reposiciones;
    uint32_t peor_op_programas, peor_op_borrados;   /* la operación de usuario más cara (las pone a cero quien mide) */
} W6fCuentas;

typedef struct W6f {
    const W6fFlash *fl;
    W6fConf  conf;
    uint8_t  montado, solo_lectura;
    uint32_t n_pag, carga, cola_max, por_rango, n_rangos;
    uint32_t seq;                      /* la próxima secuencia */
    w6f_pag *sig;                      /* por página: la siguiente del mismo fichero, o un estado */
    W6fObj  *obj;
    W6fBlq  *blq;
    w6f_pag *cp;                       /* por rango de ids: su punto de control vivo */
    uint8_t *pg;                       /* una página: lo que se va a programar */
    uint8_t *pgb;                      /* otra: la cabecera de un bloque que se borra MIENTRAS hay una página armada en pg */
    W6fEscr  escr[W6F_MAX_ESCR];
    uint32_t cabeza, llenado, parcial, parcial_llenado, libres, utiles, fallos_seguidos;
    uint32_t cap, usadas, pend, reservadas, n_obj, ids_reservados;
    uint32_t victima, gc_pos, destino, dest_llenado, gc_ncop, gc_comprometido, gc_emergencia, libres_min;
    uint32_t repo_intentos;            /* lotes gastados en reponer la reserva desde la última vez que se logró (o que se borró algo) */
    uint32_t gc_compromiso;            /* la página que compromete el lote en curso */
    uint8_t  dentro, urgente, motivo_sl;
    uint32_t pend_cp;                  /* rangos (bit r; el 31 = «del 31 en adelante») cuyo punto de control hay que reescribir */
    w6f_pag  nodo_ok;                  /* último nodo cuyo CRC se comprobó entero */
    uint32_t cambios;                  /* sube cuando una página viva cambia de sitio: invalida el cursor */
    uint32_t cur_cambios, cur_idx; uint16_t cur_id; w6f_pag cur_pag;
    w6f_pag  crc_ok;                   /* última página de datos verificada al leer */
    W6fCuentas cta;
} W6f;

typedef struct {
    uint16_t id, padre;
    uint8_t  es_dir, danado, nombre_len;
    uint32_t tam, fecha;
    char     nombre[W6F_NOMBRE_MAX + 1];
} W6fInfo;

typedef struct {
    uint32_t paginas_cap, paginas_usadas, paginas_libres;
    uint32_t bytes_libres;             /* el fichero NUEVO más grande que cabe ahora mismo (exacto) */
    uint32_t objetos, max_obj;
    uint32_t bloques, bloques_malos, bloques_libres;
    uint32_t borrados_min, borrados_max, borrados_suma;
    uint32_t paginas_muertas, paginas_blancas;
    uint32_t solo_lectura;             /* W6F_SL_*: 0 = se puede escribir */
    uint32_t deficit;                  /* 1 = falta un bloque libre de la reserva (murió uno): no se admite escribir hasta reponerlo; borrar sí */
} W6fEstado;

/* ---- montar y dar formato ---------------------------------------------------------------------------- */
uint32_t w6f_mem_necesaria(const W6fFlash *fl, const W6fConf *conf);   /* bytes de RAM que hay que darle (0 = geometría inválida) */
/* W6F_OK; W6F_E_VIRGEN (zona toda en blanco: dar formato no destruye nada); W6F_E_AJENO (hay algo que no es
 * nuestro: preguntar antes de dar formato); W6F_E_FORMATO (un formato se quedó a medias: hay que repetirlo). */
int  w6f_montar(W6f *fs, const W6fFlash *fl, const W6fConf *conf, void *mem, uint32_t mem_bytes);
/* Da formato (no hace falta estar montado; usa «mem» de trabajo, la misma de montar). «paso(ctx, hechos, total)»
 * opcional para pintar el progreso. BORRA TODO lo que haya en la zona. */
int  w6f_formatear(const W6fFlash *fl, const W6fConf *conf, void *mem, uint32_t mem_bytes,
                   void (*paso)(void *ctx, uint32_t hechos, uint32_t total), void *ctx);

/* ---- consultar ------------------------------------------------------------------------------------ */
int  w6f_buscar(W6f *fs, const char *ruta);                       /* id (0 = la raíz) o W6F_E_* */
int  w6f_info(W6f *fs, uint16_t id, W6fInfo *inf);
int  w6f_listar(W6f *fs, uint16_t dir, uint32_t *cursor, W6fInfo *inf);   /* *cursor = 0 al empezar; 1 = hay una entrada, 0 = fin */
/* Bytes leídos (0 al final) o W6F_E_*. OJO: si una página del fichero está rota o falta, devuelve LO LEÍDO HASTA ELLA
 * (menos de lo pedido, SIN error); el error (W6F_E_CRC / W6F_E_DANADO) lo da la llamada que EMPIEZA en esa página. Quien
 * lea de un tirón tiene que comparar con el tamaño del fichero (o usar w6f_leer_todo): «menos bytes» no es «fin». */
int  w6f_leer(W6f *fs, uint16_t id, uint32_t desde, void *buf, uint32_t max);
/* El fichero ENTERO o nada: devuelve su tamaño, o W6F_E_ARG si no cabe en «max», o el error de la primera página que no
 * se pudo leer bien (W6F_E_CRC, W6F_E_DANADO…). Nunca una lectura corta sin error. */
int  w6f_leer_todo(W6f *fs, uint16_t id, void *buf, uint32_t max);
void w6f_estado(W6f *fs, W6fEstado *e);
uint32_t w6f_paginas_para(const W6f *fs, uint32_t tam);          /* páginas que ocupa un fichero de «tam» bytes */

/* ---- cambiar (cada una es atómica) ----------------------------------------------------------------- */
enum { W6F_CREAR = 1,                  /* fichero nuevo, o REEMPLAZO entero del que haya (el viejo vale hasta w6f_cerrar) */
       W6F_ANADIR = 2 };               /* añadir al final del que haya (lo añadido vale desde w6f_cerrar) */
int  w6f_abrir(W6f *fs, const char *ruta, int modo, uint32_t fecha);       /* manejador (>= 0) o W6F_E_* */
int  w6f_escribir(W6f *fs, int h, const void *d, uint32_t n);
int  w6f_cerrar(W6f *fs, int h);                                 /* EL COMPROMISO */
void w6f_abortar(W6f *fs, int h);                                /* (llamada desde «respiro» NO hace nada: el manejador sigue abierto) */
int  w6f_guardar(W6f *fs, const char *ruta, const void *d, uint32_t n, uint32_t fecha);   /* abrir + escribir + cerrar */
int  w6f_truncar(W6f *fs, const char *ruta, uint32_t tam);       /* solo a menos */
int  w6f_mkdir(W6f *fs, const char *ruta, uint32_t fecha);
int  w6f_borrar(W6f *fs, const char *ruta);                      /* fichero o directorio vacío */
int  w6f_renombrar(W6f *fs, const char *vieja, const char *nueva, int reemplazar);   /* renombra y/o mueve */

/* ---- mantenimiento -------------------------------------------------------------------------------- */
/* Un paso ACOTADO de recolección / nivelado / reposición de la reserva: como mucho «tope» programas de página, o un
 * borrado (más su cabecera). 1 = queda trabajo útil; 0 = nada que hacer; < 0 = error. HAY QUE llamarla en los ratos
 * muertos: es la única que hace el nivelado estático, y mantiene un colchón de dos bloques de páginas en blanco para que
 * una escritura normal no tenga que esperar a la recolección. Con el volumen casi lleno y reescrituras muy repartidas
 * NO lo evita del todo (docs/fs_propio.md §12, M4): por eso las escrituras recolectan por pasos y llaman a «respiro». */
int  w6f_gc_paso(W6f *fs, uint32_t tope);
int  w6f_verificar(W6f *fs, uint16_t id);                        /* relee y comprueba el CRC de todo el fichero */
/* Vacía el volumen MONTADO (aunque esté lleno): da formato y vuelve a montar. Atómico en el sentido de que tras un
 * corte w6f_montar devuelve W6F_E_FORMATO (repetir w6f_formatear) o el volumen ya vacío. */
int  w6f_vaciar(W6f *fs, void *mem, uint32_t mem_bytes);
#endif
