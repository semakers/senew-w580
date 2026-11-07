/* FAT12 / FAT16 / FAT32 con nombres largos (VFAT). Pista T (docs/tarjeta.md §3): el lector. Pista FATW (docs/fat_escritura.md):
 * la ESCRITURA, que solo existe si se monta con fat_montar_rw y una FatEscribir; montado con fat_montar sigue sin escribir nada.
 *
 * Portable: no sabe de qué dispositivo lee. Recibe una función que entrega sectores de 512 bytes y nada más; con ella
 * se monta la tarjeta (Memory Stick: hal/ms.c) y también los volúmenes del FS de fábrica de la NOR (api/fsfab.c, que
 * antes traía su propio lector FAT12/16: era este mismo código, ligado a punteros a la flash).
 *
 *  - Sin malloc: todo el estado vive en el FatVol que pone quien lo usa (~5 KiB con la caché de 8 sectores).
 *  - Montado con fat_montar NO ESCRIBE (no tiene por dónde: FatLeer solo lee). Con fat_montar_rw escribe con un orden fijo
 *    pensado para los cortes (docs/fat_escritura.md §3) y la caché pasa a ser de escritura inmediata salvo la FAT, que se
 *    junta y se vuelca (las DOS copias, primero la 1.ª) antes de cualquier dato o entrada de directorio que dependa de ella.
 *  - Todo lo leído es dato SIN CONFIANZA: cada índice se acota y cada cadena tiene tope; una FAT dañada (cadenas en
 *    bucle, clústeres fuera de rango, directorios sin fin, geometría imposible) da «no existe» o una lectura corta,
 *    nunca un cuelgue ni un acceso fuera de sitio (herr/pruebas/prueba_fat.c: fuzz con ASan/UBSan).
 *  - MBR (solo la primera entrada, como el original: docs/memory_stick.md §3.2) o volumen sin tabla de particiones.
 *  - Sectores de 512 bytes solamente (lo único que da una Memory Stick PRO); otro tamaño: FAT_E_SECTOR.
 *  - Los nombres se comparan sin distinguir mayúsculas SOLO en ASCII (como api/fsfab.c): «Ñ» y «ñ» son distintas. */
#ifndef FAT_H
#define FAT_H
#include "hw.h"

#ifndef FAT_CACHE
#define FAT_CACHE 8u                    /* sectores de la caché (LRU): 4 KiB. Acotada: es una constante */
#endif
#define FAT_NOMBRE 256u                 /* bytes UTF-8 del nombre más largo que se sabe comparar (lo demás: por el corto 8.3) */

/* El dispositivo: «n» sectores de 512 bytes desde «lba» a «buf». 0 = bien; otra cosa = no se pudo (ni un byte vale). */
typedef int (*FatLeer)(void *ctx, u32 lba, u32 n, u8 *buf);
typedef int (*FatEscribir)(void *ctx, u32 lba, u32 n, const u8 *buf);   /* (FATW) lo mismo, al revés. 0 = bien */
typedef u32 (*FatFecha)(void);          /* (FATW) fecha y hora DOS: (fecha << 16) | hora. NULL = 2000-01-01 00:00 */

enum { FAT_E_ES = -1,                   /* el dispositivo no entregó un sector */
       FAT_E_NO_FAT = -2,               /* ni sector de arranque FAT ni MBR con una primera partición FAT */
       FAT_E_MBR = -3,                  /* MBR con la primera entrada vacía o fuera del dispositivo */
       FAT_E_SECTOR = -4,               /* FAT con sectores que no son de 512 bytes */
       FAT_E_GEOMETRIA = -5,            /* sector de arranque con números imposibles */
       /* (FATW) de las operaciones de escritura */
       FAT_E_SOLO_LECTURA = -6,         /* montado sin escritura (tarjeta protegida, o fat_montar) */
       FAT_E_NO_HAY = -7,               /* la ruta (o su carpeta) no existe */
       FAT_E_EXISTE = -8,               /* ya hay algo con ese nombre */
       FAT_E_LLENO = -9,                /* no quedan clústeres (o la raíz fija de FAT12/16 no tiene más entradas) */
       FAT_E_NOMBRE = -10,              /* nombre vacío, demasiado largo, «.»/«..», o con caracteres que FAT no admite */
       FAT_E_NO_VACIA = -11,            /* borrar una carpeta con cosas */
       FAT_E_ES_DIR = -12, FAT_E_NO_DIR = -13,
       FAT_E_DENTRO = -14,              /* mover una carpeta dentro de sí misma */
       FAT_E_ROTO = -15,                /* una escritura falló antes: el volumen no se toca más hasta volver a montarlo */
       FAT_E_OCUPADA = -16 };           /* hay una escritura a trozos abierta (FatEsc) y esta operación la estropearía */

typedef struct { u32 cluster, i, vistos, marca, paso, hasta, s_lba; } FatDir;   /* s_lba: el sector de la última entrada entregada (FATW) */   /* cluster 0 = la raíz fija de FAT12/16; marca/paso/hasta: detector de bucles */

typedef struct {
    FatLeer leer; void *ctx;
    u32 sectores;                       /* del dispositivo (0 = no se sabe: se cree al sector de arranque) */
    u32 part, total;                    /* primer sector del volumen y cuántos tiene */
    u32 s_fat, sfat, s_raiz, s_datos, nraiz, spc, nclusters, raiz_cl;
    u8  tipo;                           /* 12, 16 o 32 */
    u8  montado, con_mbr;
    u8  fallo_es;                       /* 1 = desde la última vez que se puso a 0, el dispositivo falló una lectura */
    char etiqueta[12];                  /* la del sector de arranque (no la de la raíz), sin espacios finales */
    /* cuentas (para la pantalla de Sistema y las pruebas) */
    u32 lecturas, aciertos, errores_es, topes;
    /* la caché de sectores */
    u32 c_lba[FAT_CACHE], c_uso[FAT_CACHE], c_reloj;
    u8  c_hay[FAT_CACHE];
    u8  c_dat[FAT_CACHE][512];
    /* apuntes para no repetir trabajo (se olvidan al montar): el último directorio resuelto, por dónde iba su
     * listado, y por dónde iba la lectura del último fichero */
    char m_ruta[FAT_NOMBRE]; u32 m_cluster, m_tam; u8 m_hay;
    FatDir l_dir; u32 l_cluster, l_idx; u8 l_hay;
    u32 f_ini, f_k, f_c; u8 f_hay;
    u16 largo[20 * 13 + 1];             /* el nombre largo que se está juntando */
    /* (FATW) la escritura */
    FatEscribir escribir; FatFecha fecha;
    int (*vaciar)(void *ctx);
    void (*latido)(void);               /* (FATW) en lo largo (revisar, contar libres): cada 64 sectores leídos. NULL = nada */           /* barrera: lo escrito, escrito de verdad (hwo_tarjeta_vaciar). NULL = no hace falta */
    u8  rw;                             /* montado para escribir */
    u8  sucio_al_montar;                /* el volumen venía marcado sucio (o con las FAT distintas): pide fat_revisar */
    u8  sucio;                          /* lo hemos marcado sucio en el disco (fat_limpio lo quita) */
    u8  roto;                           /* falló una escritura: nada más hasta volver a montar */
    u8  abiertos;                       /* escrituras a trozos (FatEsc) abiertas */
    u8  c_pend[FAT_CACHE];              /* sector de la 1.ª FAT modificado en la caché y aún sin escribir */
    u32 nfat, s_info, s_arranque;       /* n.º de copias de la FAT; sector del FSInfo (0 = no hay); sector de arranque */
    u32 libres, pista; u8 libres_ok;    /* clústeres libres (si libres_ok) y por dónde buscar el siguiente */
    u32 escrituras, errores_w;          /* sectores escritos, escrituras fallidas */
} FatVol;

int  fat_montar(FatVol *v, FatLeer leer, void *ctx, u32 sectores);       /* 0 = montado; < 0 = FAT_E_* */
const char *fat_motivo(int e);                                           /* el FAT_E_* en una frase */
/* Rutas RELATIVAS AL VOLUMEN, con «/» («/», «/music/a.mid»). Lo mismo que piden hw_fs_* (api/hw.h): */
int  fat_listar(FatVol *v, const char *dir, u32 desde, HwEntrada *e, u32 max);   /* e == NULL: cuenta. HW_ERROR si no existe o no es directorio */
int  fat_info(FatVol *v, const char *ruta, u32 *tam, int *es_dir);       /* 1 = existe */
u32  fat_tam(FatVol *v, const char *ruta);                               /* 0 si no existe o es un directorio */
u32  fat_leer_desde(FatVol *v, const char *ruta, u32 desde, u8 *buf, u32 max);   /* los bytes [desde, desde+max): devuelve los leídos */

/* ---- (FATW) ESCRITURA. docs/fat_escritura.md. Todas devuelven 0 o un FAT_E_* (< 0). Un fallo del dispositivo deja el
 * volumen «roto» (FAT_E_ROTO a todo lo que escribe) hasta volver a montarlo: lo que quede a medias es de lo que dice el §4
 * del documento y lo arregla fat_revisar. Nombres: UTF-8, 1..255 caracteres UTF-16, sin \ / : * ? " < > | ni controles,
 * sin «.»/espacio al final; si no cabe en 8.3 lleva nombre largo y un corto «NOMBRE~N». */
int  fat_montar_rw(FatVol *v, FatLeer leer, FatEscribir escribir, void *ctx, u32 sectores, FatFecha fecha);   /* escribir NULL = solo lectura */
typedef struct {
    u8  modo, raiz;                     /* FAT_ESC_*; la carpeta es la raíz */
    u32 dir_cl;                         /* clúster de la carpeta */
    u32 primero, ultimo, tam;           /* la cadena NUEVA (o, al añadir, la del fichero) y los bytes que lleva el fichero */
    u32 viejo_ult, nuevo_pri;           /* al añadir: el último clúster que tenía y el primero de los nuevos (se enlazan al cerrar) */
    char nombre[FAT_NOMBRE];
} FatEsc;
enum { FAT_ESC_NUEVO = 1,               /* no puede existir (FAT_E_EXISTE) */
       FAT_ESC_REEMPLAZAR,              /* exista o no: al cerrar, la entrada pasa de la cadena vieja a la nueva de UNA escritura */
       FAT_ESC_ANADIR };                /* al final de lo que hay (si no existe, lo crea) */
int  fat_esc_abrir(FatVol *v, FatEsc *f, const char *ruta, int modo);
int  fat_esc_mas(FatVol *v, FatEsc *f, const u8 *d, u32 n);
int  fat_esc_cerrar(FatVol *v, FatEsc *f);      /* el fichero existe (o cambia) AQUÍ, no antes */
int  fat_esc_destino(FatVol *v, FatEsc *f, const char *ruta, int modo);   /* antes de cerrar: cambia el nombre y el modo (NUEVO /
                                                    REEMPLAZAR) con que se comprometerá. Es el «temporal que se renombra» sin temporal */
void fat_esc_abortar(FatVol *v, FatEsc *f);     /* devuelve los clústeres: el fichero queda como estaba */
int  fat_guardar(FatVol *v, const char *ruta, const u8 *d, u32 n);          /* crear o reemplazar entero (abrir + mas + cerrar) */
int  fat_truncar(FatVol *v, const char *ruta, u32 tam);                     /* solo acorta */
int  fat_borrar(FatVol *v, const char *ruta);                               /* un fichero o una carpeta VACÍA */
int  fat_mkdir(FatVol *v, const char *ruta);
int  fat_renombrar(FatVol *v, const char *vieja, const char *nueva);         /* renombra y/o mueve dentro del volumen; nunca encima de otro */
int  fat_limpio(FatVol *v);             /* sin escrituras a medias: FSInfo al día y quita la marca de sucio. Llamarlo al quedar quieto */
#define FAT_CIERRE_FIRMA "SENEW-CIERRE-M2"
int  fat_zona_cierre(FatVol *v, u32 *lba, u32 n, u32 sect_blq);   /* (28-oct) n clústeres MALOS con firma, en bloques de borrado distintos
                                           (los busca; si faltan y se puede, los crea al final): cuántos hay y su primer sector. docs/tarjeta_escritura.md §10 */
u32  fat_libres(FatVol *v);             /* clústeres libres (los cuenta la primera vez: lee la FAT entera) */
u32  fat_bytes_cluster(const FatVol *v);
/* Revisión tras un corte (un chkdsk mínimo): iguala las copias de la FAT a la 1.ª, ajusta cadenas y tamaños que no cuadran,
 * quita la entrada repetida de un renombrado a medias y los trozos de nombre largo huérfanos, devuelve los clústeres sin
 * dueño y deja el FSInfo y la marca de limpio. «mapa»: memoria para el mapa de clústeres (bits); con poca, da más vueltas.
 * Devuelve el número de arreglos hechos (0 = estaba bien) o FAT_E_*. «informe»: si no es NULL, recibe una línea por arreglo. */
typedef struct { u32 fat_copias, cadenas_largas, tamanos, repetidas, lfn_huerfanos, perdidos, bucles, cruzados, puntos; } FatRevision;
int  fat_revisar(FatVol *v, u8 *mapa, u32 bytes_mapa, FatRevision *r);
#endif
