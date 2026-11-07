/* La MEMORIA EXTERNA como la carpeta «/tarjeta» de hw_fs_* (api/hw_fs.c). Pista T. docs/tarjeta.md §4.
 * Portable: la FAT es api/fat.c y el dispositivo lo pone la plataforma (hwo_tarjeta_*): en el W580 la Memory Stick
 * (hal/ms.c + hal/ms_db2020.c, por plat/tarjeta_w580.c); en el PC un fichero imagen (plat/tarjeta_pc.c).
 * (FATW) LECTURA Y ESCRITURA (docs/fat_escritura.md): hw_fs_escribir / hw_fs_borrar funcionan en «/tarjeta/…» como en la
 * Memoria, y hw_tj_* (abajo) da lo mismo que hw_w6f_* (carpetas, renombrar/mover, copiar por pasos, recibir a trozos con
 * dueño). Con la tarjeta PROTEGIDA (o en modo seguro, o si la revisión tras un corte no pudo acabar) es de solo lectura.
 *
 * Qué ve una app, sin cambiar nada en ella: en la raíz «/», la última entrada es la carpeta «tarjeta» cuando hay una
 * tarjeta con FAT; si hay tarjeta pero no sirve, la entrada se llama «tarjeta (motivo)» y está vacía: el mensaje se
 * lee en Archivos. Sin tarjeta no hay entrada. */
#ifndef TARJETA_H
#define TARJETA_H
#include "hw.h"

#define HW_FS_TARJETA "/tarjeta"

enum { HW_TJ_FUERA = 0,                 /* no hay tarjeta */
       HW_TJ_LISTA,                     /* hay, con FAT montada */
       HW_TJ_SIN_FAT,                   /* hay y se lee, pero no tiene una FAT que sepamos montar */
       HW_TJ_MALA,                      /* hay, pero no contesta o no es del tipo que sabemos */
       HW_TJ_PREPARANDO };              /* (I1) recién metida: esperando a que esté lista (hasta 10,5 s), por pasos */
typedef struct {
    u8  estado;                         /* HW_TJ_* */
    u8  fat;                            /* 12, 16 o 32 (0 si no hay FAT) */
    u8  con_mbr;
    u8  dormida;                        /* 1 = apagada por reposo (se despierta sola al leer) */
    u32 sectores, mib;                  /* del dispositivo */
    u32 lecturas, aciertos, errores;    /* sectores pedidos al dispositivo, servidos de la caché, y lecturas falladas */
    u8  escribible, protegida, sucia;   /* (FATW) se puede escribir; la tarjeta dice protegida; marcada sucia ahora mismo */
    u32 escritos, errores_w, arreglos;  /* (FATW) sectores escritos, escrituras falladas, arreglos de fat_revisar al montar */
    u32 capacidad_kib, libre_kib;       /* (FATW) del volumen FAT (libre: 0 si no se sabe aún) */
    char etiqueta[12];
    char texto[56];                     /* el estado en una frase, para enseñarlo */
} HwTarjeta;
void hw_tarjeta_estado(HwTarjeta *t);
int  hw_tarjeta_paso(void);
u32  hw_tarjeta_cambios(void);           /* (I1) sube cada vez que cambia lo que hay (entra, sale, deja de servir): quien enseña la carpeta lo mira */             /* desde el latido del bucle: detecta, monta y avisa por el registro. Devuelve HW_TJ_* */

/* ---- para api/hw_fs.c ---- */
const char *tj_ruta(const char *ruta);  /* la ruta DENTRO de la tarjeta («/…») si «ruta» es de HW_FS_TARJETA; "" si es la carpeta del motivo; NULL si no es de la tarjeta */
int  tj_entrada(char nombre[64]);       /* el nombre de la entrada de la raíz («tarjeta», «tarjeta (motivo)»); 0 = no hay entrada */
int  tj_listar(const char *r, u32 desde, HwEntrada *e, u32 max);
u32  tj_tam(const char *r);
u32  tj_leer_desde(const char *r, u32 desde, u8 *buf, u32 max);
int  tj_escribir(const char *r, const u8 *d, u32 n);   /* (FATW) */
int  tj_borrar(const char *r);

/* ---- (FATW) escribir en la tarjeta: rutas DENTRO de ella («/fotos/a.png»; «» o «/» = la raíz). Como hw_w6f_*:
 * HW_OK; HW_ERROR con el motivo en hw_tj_motivo() (los MISMOS códigos HW_W6F_M_*, para que shell y MTP los digan igual);
 * HW_NO_DISPONIBLE si no hay tarjeta escribible. Nombres: el último trozo de 1..63 bytes (lo que cabe en HwEntrada, como
 * la Memoria) y lo que FAT admite (sin \ / : * ? " < > |, sin «.» ni espacio al final). Cada operación es de las del
 * documento: tras un corte, fat_revisar al montar deja la tarjeta limpia y lo que no se tocaba, igual. */
int  hw_tj_escribible(void);
int  hw_tj_motivo(void);
u32  hw_tj_cambios(void);               /* sube con CADA operación que puede cambiar la tarjeta, y al entrar o salir */
int  hw_tj_info(const char *ruta, HwW6fInfo *i);
int  hw_tj_guardar(const char *ruta, const u8 *d, u32 n);    /* crear o reemplazar entero (el viejo vale hasta el final) */
int  hw_tj_mkdir(const char *ruta);
int  hw_tj_borrar(const char *ruta);                         /* un fichero o una carpeta VACÍA */
int  hw_tj_renombrar_r(const char *vieja, const char *nueva, int reemplazar);   /* renombra y/o mueve dentro de la tarjeta */
int  hw_tj_copiar(const char *destino, const char *ruta_fs, int reemplazar);    /* empieza: destino en la tarjeta, origen cualquier ruta de hw_fs */
int  hw_tj_paso(u32 max, u32 *hecho, u32 *total);           /* 1 = sigue; HW_OK = acabada; HW_ERROR (motivo) */
void hw_tj_cancelar(void);
int  hw_tj_ocupada(void);
int  hw_tj_rx_abrir_d(const char *ruta, int dueno);          /* recibir a trozos (HW_RX_SER / HW_RX_MTP): no existe hasta cerrar */
int  hw_tj_rx_mas_d(int dueno, const void *d, u32 n);
int  hw_tj_rx_destino(int dueno, const char *ruta, int reemplazar);   /* antes de cerrar: con qué nombre se compromete (sin temporal) */
int  hw_tj_rx_cerrar_d(int dueno);
void hw_tj_rx_abortar_d(int dueno);
int  hw_tj_rx_dueno(void);
u32  hw_tj_libre(void);                 /* bytes libres (lo que cabe en ficheros nuevos, por lo alto); 0 si no se escribe */
u32  hw_tj_capacidad_kib(void);

/* ---- lo que pone la plataforma ---- */
void hwo_tarjeta_paso(void);            /* el paso del controlador (detección, reposo) */
u32  hwo_tarjeta_generacion(void);      /* 0 = no hay tarjeta utilizable; si la hay, un número que cambia con cada tarjeta que entra */
u32  hwo_tarjeta_sectores(void);
int  hwo_tarjeta_leer(void *ctx, u32 lba, u32 n, u8 *buf);   /* sectores de 512; 0 = bien */
/* (MSW) ESCRITURA. docs/tarjeta_escritura.md. Mismos sectores de 512 que la lectura.
 *   hwo_tarjeta_escribir: 0 = escritos los n; <0 = error (HW_TJ_E_*), sin reintentos ocultos más allá de los del
 *     controlador (los mismos que en la lectura). Tras un error, lo escrito en [lba, lba+n) es INDETERMINADO: puede
 *     haber quedado una parte (la tarjeta escribe por bloques de 512 y un corte deja los anteriores). Nunca escribe
 *     fuera de [0, hwo_tarjeta_sectores()); n = 0 es 0 sin tocar nada.
 *   hwo_tarjeta_protegida: 1 = la tarjeta dice que está protegida contra escritura (registro de estado, bit WP);
 *     0 = no, o no hay tarjeta. Si es 1, escribir da HW_TJ_E_PROTEGIDA sin mandar nada a la tarjeta.
 *   hwo_tarjeta_vaciar: espera a que la tarjeta termine lo pendiente (antes de soltarla, apagar o dormir); 0 = bien.
 *     En el W580 la escritura ya es síncrona (vuelve cuando la tarjeta dio el fin de la orden), así que solo comprueba
 *     que la tarjeta ya no está ocupada; se llama igual por contrato. */
enum { HW_TJ_E_ES = -1,                 /* fallo de E/S (sin respuesta, CRC, error de la tarjeta) */
       HW_TJ_E_FUERA = -2,              /* no hay tarjeta (o salió a mitad) */
       HW_TJ_E_RANGO = -3,              /* lba/n pasan del final */
       HW_TJ_E_PROTEGIDA = -4 };        /* protegida contra escritura */
int  hwo_tarjeta_escribir(void *ctx, u32 lba, u32 n, const u8 *buf);
int  hwo_tarjeta_protegida(void);
int  hwo_tarjeta_vaciar(void *ctx);
int  hwo_tarjeta_mala(const char **motivo);   /* 1 = hay tarjeta pero el controlador no pudo prepararla (y por qué) */
int  hwo_tarjeta_dormida(void);
int  hwo_tarjeta_preparando(void);
void hwo_tarjeta_iniciar(void);         /* (I1, declaradas aquí) el W580: plat/tarjeta_w580.c; las llama plat/hw_w580.c */
void hwo_tarjeta_apagar(void);      /* (I1) 1 = recién metida, todavía preparándola (el CED, por pasos) */
/* (28-oct) EL CIERRE (docs/tarjeta_escritura.md §10): la M2 del W580 pierde, al quitarle la tensión, los cambios de los 4 últimos bloques de
 * borrado que se tocaron; el controlador los consolida escribiendo 1 sector en cada bloque de una ZONA DE CIERRE antes de cualquier corte.
 * La zona la busca/crea quien monta (api/tarjeta.c, fat_zona_cierre) y se la da aquí: n sectores (en bloques distintos) y su contenido. */
u32  hwo_tarjeta_bloque_sectores(void);  /* sectores del bloque de borrado de la tarjeta (su información del sistema); 0 = no se sabe */
void hwo_tarjeta_cierre_zona(const u32 *lba, u32 n, const u8 *sector);
#endif
