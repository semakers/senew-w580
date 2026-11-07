/* gestion.h — EL CANAL DE GESTIÓN de SENUS: las operaciones MTP de vendedor 0x9400..0x9408.
 * El contrato, byte a byte: el contrato del canal de gestión (docs/ del repositorio de SENUS). Lo hecho y lo probado: docs/mtp.md §Canal de gestión.
 *
 * TRES PIEZAS, porque api/mtp.c se compila en DOS sitios (el firmware y el banco de PC herr/pruebas/mtp/prueba_mtp.c, sin shell):
 *   api/mtp.c            la transacción MTP: recibe la orden y los datos, llama a gs_op_* y contesta.
 *   api/gestion.c        el FORMATO y la VALIDACIÓN del contrato, y la cola de cambios. Portable: no ve nada de shell/.
 *   shell/gestion_sis.c  lo que depende del sistema (gs_sis_*): las claves con sus rangos, los valores de `aj`, aplicar la cola en el hilo
 *                        de la interfaz, el recorrido de las apps y el «¿Actualizar el sistema?». En el banco: un doble, en prueba_mtp.c.
 *
 * LA REGLA: el manejador MTP solo valida y encola. Nada de interfaz ni de `aj` dentro de mtp_paso. Lo aplica gs_paso(), desde sh_usb_paso,
 * que corre en el bucle principal entre dos cuadros. */
#ifndef GESTION_H
#define GESTION_H
#include "hw.h"

enum { OP_SEN_INFO = 0x9400, OP_SEN_AJ_LEER = 0x9401, OP_SEN_AJ_PONER = 0x9402, OP_SEN_RELOJ = 0x9403, OP_SEN_VINCULAR = 0x9404,
       OP_SEN_ENTRAR_BL = 0x9405, OP_SEN_BT_ESTADO = 0x9406, OP_SEN_BT_EMPAREJAR = 0x9407, OP_SEN_APPS = 0x9408 };
/* las respuestas (los mismos códigos de api/mtp.c, MTP 1.1 Apéndice F) */
enum { GS_R_OK = 0x2001, GS_R_GENERAL = 0x2002, GS_R_OP_NO = 0x2005, GS_R_DENEGADO = 0x200F, GS_R_OCUPADO = 0x2019, GS_R_PARAM_MAL = 0x201D };

enum { GS_T_BOOL = 1, GS_T_U8 = 2, GS_T_I16 = 3, GS_T_TEXTO = 4 };
enum { GS_FORMATO = 1, GS_INFO_TAM = 128, GS_BT_TAM = 12, GS_ID_TAM = 16, GS_APP_TAM = 128, GS_APPS_PAG = 7, GS_APPS_CAB = 8, GS_EMP_TAM = 24, GS_PIN_MAX = 16 };
#define GS_COLA_MAX  32u                 /* claves distintas en un SEN_AJ_PONER (hoy 26); más: «fuera de rango» */
#define GS_TEXTO_MAX 99u                 /* bytes de un valor de tipo 4, sin el NUL: aj.fondo */
#define GS_SERIE_TAM 40u                 /* el texto de la serie, con su NUL */

/* una clave de ajustes: estable para siempre (contrato §3). Para el tipo 4: min = 0, max = capacidad en bytes sin el NUL */
typedef struct { u16 clave; u8 tipo; short min, max; } GsClave;

/* lo que el sistema dice para SEN_INFO; lo demás (versión, build, compilación, banderas de compilación) lo pone api/gestion.c */
typedef struct {
    u16 api_mayor, api_menor, bl_version;
    u8  bateria_pct;                     /* 0..100; 0xFF = no se sabe */
    u8  tarjeta, cargando;               /* 0/1 */
    u8  bd_addr[6];                      /* todo 0 = no se sabe */
    u32 ajustes_version;
    const char *soc;
} GsInfo;
/* SEN_BT_ESTADO, tal cual lo pide el contrato §4 */
typedef struct { u8 encendido, puente_conectado, red_lista, fallo, puente_addr[6]; } GsBt;
/* lo que hay en la cola tras un SEN_AJ_PONER o un SEN_RELOJ */
typedef struct { u16 clave; short v; } GsCambio;

/* ---- lo que usa api/mtp.c ---- */
void gs_iniciar(void);                   /* mtp_iniciar: el estado de la sesión anterior fuera (la cola NO: lo aceptado se aplica) */
void gs_parar(void);                     /* mtp_parar: cable fuera; lo que esperaba al host se olvida */
u32  gs_op_info(u8 *d);                  /* 128 bytes */
int  gs_op_aj_leer(u8 *d, u32 cap, u32 *n);                        /* GS_R_OK con *n bytes, o GS_R_OCUPADO */
int  gs_op_aj_poner(const u8 *d, u32 n, int rebosa, u32 par[3]);   /* par: aplicadas, fuera de rango, desconocidas */
int  gs_op_reloj(const u8 *d, u32 n, int rebosa);
int  gs_op_vincular(const u8 *d, u32 n, int rebosa);
int  gs_op_entrar_bl(void);              /* GS_R_OK, GS_R_OCUPADO, GS_R_DENEGADO o GS_R_OP_NO */
u32  gs_op_bt(u8 *d);                    /* 12 bytes */
int  gs_op_bt_emparejar(const u8 *d, u32 n, int rebosa);            /* R_OK (encolado), R_PARAM_MAL, R_OCUPADO o R_OP_NO */
int  gs_op_apps(u32 desde, u8 *d, u32 *n);                         /* GS_R_OK con *n bytes, o GS_R_OCUPADO */
const char *gs_serie_mtp(void);          /* el SerialNumber de DeviceInfo */

/* ---- lo que usa el sistema (shell/gestion_sis.c, o el doble del banco) ---- */
int  gs_cola_hay(void);                  /* hay cambios sin aplicar */
const GsCambio *gs_cola(u32 *n);         /* los cambios de ajustes numéricos */
const char *gs_cola_texto(void);         /* el de tipo 4 si lo hay; NULL si no */
int  gs_cola_reloj(u32 *utc, short *zona); /* 1 si hay un SEN_RELOJ en la cola */
int  gs_cola_bt(u8 addr[6], char pin[GS_PIN_MAX + 1]);             /* 1 si hay un SEN_BT_EMPAREJAR en la cola */
void gs_cola_vaciar(void);
void gs_id_cargar(void);                 /* lee el id vinculado (NV_APARATO) y pone la serie en el USB: UNA vez, antes de la primera enumeración */
const char *gs_id(void);                 /* el id vinculado, o «» */
void gs_fecha_de_unix(u32 utc, HwFecha *f);                        /* segundos desde 1970 a fecha (UTC) */

/* ---- lo que pone el sistema: shell/gestion_sis.c en el firmware, el doble en el banco ---- */
const GsClave *gs_sis_claves(u32 *n);
int  gs_sis_valor(u16 clave, short *v, const char **txt);            /* 0 = bien: el valor en *v, o el texto en *txt para el tipo 4 */
int  gs_sis_texto_vale(u16 clave, const char *s);                  /* 1 = ese texto se puede poner */
void gs_sis_info(GsInfo *i);
void gs_sis_bt(GsBt *b);
int  gs_sis_bt_puede(void);                                         /* GS_R_OK, GS_R_OP_NO (sin radio) o GS_R_OCUPADO (una app en pantalla) */
int  gs_sis_id_leer(char id[GS_ID_TAM]);                           /* 1 si hay uno guardado */
int  gs_sis_id_guardar(const char id[GS_ID_TAM]);                  /* HW_OK o el error del almacén */
void gs_sis_serie(const char *s);                                  /* la serie para iSerialNumber (usb_poner_serie) */
int  gs_sis_bl(void);                                              /* como gs_op_entrar_bl */
int  gs_sis_apps(u32 desde, u32 *total, const u8 **e);             /* 1 = aún se recorre; 0 = *total entradas en *e, de GS_APP_TAM bytes */
void gs_sis_parar(void);                                           /* el cable se fue */
void gs_sis_sesion_nueva(void);                                    /* el USB se acaba de configurar */

/* ---- el hilo de la interfaz (shell/usb.c, tras mtp_paso): aplica, recorre, pregunta ---- */
void gs_paso(void);
#endif
