/* Memory Stick PRO (la M2 lo es). Pista T; la ESCRITURA de sectores, pista MSW (docs/tarjeta_escritura.md). docs/tarjeta.md §2; el plano: docs/memory_stick.md.
 *
 * DOS MITADES:
 *  - hal/ms.c        el PROTOCOLO Memory Stick PRO y la política (antirrebote, identificación, atributos, lectura de
 *                    sectores, reintentos por apagado, reposo a los 30 s). C portable: no toca ni un registro. Habla
 *                    con el anfitrión por la tabla MsAnf de abajo. Se prueba en el PC contra un anfitrión y una
 *                    tarjeta simulados con fallos inyectables (herr/pruebas/prueba_ms.c).
 *  - hal/ms_db2020.c el ANFITRIÓN del DB2020 (bloque 0xFC010000, IRQ 76, puertas 22/23, bloque de reset 18, regulador 6
 *                    del PMU, detección por P3.4). Para otro modelo se escribe otro fichero como ese.
 *
 * (MSW) ESCRITURA: ms.c manda las órdenes 0x20 (leer sectores), 0x24 (leer atributos) y 0x21 (escribir sectores, solo
 * desde ms_escribir) y el TPC WRITE_LONG_DATA (0xD) de 512. NUNCA 0x10 (formato), ni WRITE_SHORT_DATA, ni SET_CMD:
 * no existen en el binario y el anfitrión rechaza cualquier TPC u orden fuera de su lista (ms_db2020.c). */
#ifndef MS_H
#define MS_H
#ifndef HAL_H
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
#endif

/* Resultado de un TPC (los dos primeros números son los del original: 2 = tiempo, 5 = CRC) */
enum { MS_T_BIEN = 0, MS_T_TIEMPO = 2, MS_T_CRC = 5, MS_T_FUERA = 7, MS_T_NEGADO = 8, MS_T_ANFITRION = 9 };
/* Los TPC que usa el protocolo de lectura (nibble alto del TPC del estándar; docs/memory_stick.md §1.2) */
enum { MS_TPC_READ_LONG_DATA = 2, MS_TPC_READ_REG = 4, MS_TPC_GET_INT = 7, MS_TPC_SET_RW_REG_ADRS = 8,
       MS_TPC_EX_SET_CMD = 9, MS_TPC_WRITE_REG = 0xB,
       MS_TPC_WRITE_LONG_DATA = 0xD };                /* (MSW) el dato de UN sector a la tarjeta (WriteLongData 0x4422C9D8) */
/* El registro INT de la tarjeta */
enum { MS_INT_CED = 0x80, MS_INT_ERR = 0x40, MS_INT_BREQ = 0x20, MS_INT_CMDNK = 0x01 };

/* Lo que pone la placa. Todas las esperas tienen tope DENTRO del anfitrión. */
typedef struct {
    int  (*hay)(void);                              /* 1 = hay tarjeta en la ranura (crudo, sin antirrebote) */
    int  (*encender)(void);                         /* relojes, reset, tensión, modo serie, interrupción. 0 = bien; < 0 = motivo (MS_E_*) */
    void (*apagar)(void);                           /* tensión fuera, reset, relojes cerrados. Vale llamarla dos veces */
    void (*paralelo)(void);                         /* el anfitrión pasa a 4 bits */
    int  (*tpc)(int tpc, u8 *b, u32 n, int a_la_tarjeta);   /* una transacción: MS_T_* */
    int  (*espera_int)(u32 ms);                     /* espera la INT de la tarjeta y la consume: MS_T_BIEN, MS_T_TIEMPO o MS_T_FUERA */
    void (*olvida_int)(void);                       /* tira una INT que hubiera quedado apuntada */
    int  (*int_paralelo)(void);                     /* el registro INT de la tarjeta tal como lo enseña el anfitrión en paralelo (MS_INT_*) */
    void (*espera_ms)(u32 ms);
    u32  (*ms)(void);                               /* reloj en milisegundos (no tiene que ser exacto: plazos de 250 ms y 30 s) */
    void (*latido)(void);                           /* se llama en las esperas largas (el CED al encender puede tardar segundos): el perro */
} MsAnf;

enum { MS_FUERA = 0,                                /* no hay tarjeta */
       MS_LISTA,                                    /* identificada y encendida */
       MS_DORMIDA,                                  /* identificada, apagada por reposo: la siguiente lectura la despierta */
       MS_MALA,                                     /* hay tarjeta pero no se pudo preparar (motivo): hasta que se saque y se meta */
       MS_PREPARANDO };                             /* (I1) recién metida, esperando su CED (hasta 10,5 s) POR PASOS desde el bucle */

/* Motivos (negativos). Los textos, en ms_motivo(). */
enum { MS_E_FUERA = -1,                             /* la tarjeta no está (o se sacó a media orden) */
       MS_E_ANFITRION = -2,                         /* el bloque no sale del reset */
       MS_E_PMU = -3,                               /* el PMU no contestó: no se da tensión a ciegas */
       MS_E_RELOJ = -4,                             /* la CPU no está a 208 MHz: el original nunca habla con la tarjeta a otra velocidad */
       MS_E_MUDA = -5,                              /* no contesta a ningún TPC (tras 3 apagados y encendidos) */
       MS_E_NO_PRO = -6,                            /* no es una Memory Stick PRO (tipo, categoría o clase) */
       MS_E_CED = -7,                               /* no llegó a estar lista */
       MS_E_ERR = -8,                               /* la tarjeta dice ERR */
       MS_E_PARALELO = -9,                          /* no pasó a 4 bits */
       MS_E_ATRIBUTOS = -10,                        /* no se pudieron leer, o no llevan la firma A5C3 */
       MS_E_SIN_INFO = -11,                         /* no traen la «información del sistema» (o no es la que se espera) */
       MS_E_INFO_LEJOS = -12,                       /* la información del sistema no está en el primer sector de atributos (no implementado) */
       MS_E_CAPACIDAD = -13,                        /* capacidad 0 o imposible */
       MS_E_RANGO = -14,                            /* se pidió un sector que la tarjeta no tiene */
       MS_E_LECTURA = -15,                          /* la lectura falló las 3 veces */
       MS_E_OTRA = -16,                             /* al despertar había OTRA tarjeta (otra capacidad) */
       MS_E_TORMENTA = -17,                         /* la interrupción no se dejaba acusar: se tapó */
       MS_E_ESCRITURA = -18,                        /* (MSW) la escritura falló (sin reintentos: lo decide quien llama) */
       MS_E_PROTEGIDA = -19 };                      /* (MSW) la tarjeta está protegida contra escritura */

enum { MS_INT_SERIE_SIN_SABER = 0, MS_INT_SERIE_SI = 1, MS_INT_SERIE_NO = 2 };
/* (M2 ag, 2026-10-21) EN QUÉ TRAMO FALLÓ EL PASO A 4 BITS (MsInfo.par_fase; el texto, ms_par_fase()). El paso es: SET_RW_REG_ADRS(0,0x1F,0x10,1)
 * -> WRITE_REG reg 0x10 <- 0 (en serie) -> anfitrión a paralelo -> esperar la INT como el original (no es error que no llegue) ->
 * NUESTRO: READ_REG de los registros 0x00..0x1E YA EN 4 BITS (tipo/categoría/clase iguales a los de la identificación en serie y el
 * 0x10 a 0) -> atributos en 4 bits. Lo que falle desde el SET_RW_REG_ADRS hasta los atributos (la primera vez) deja el bus en serie. */
enum { MS_PAR_NADA = 0, MS_PAR_RW_ADRS = 1, MS_PAR_WRITE_REG = 2, MS_PAR_LEER_REG = 3, MS_PAR_REG_DISTINTOS = 4, MS_PAR_ATRIBUTOS = 5,
       MS_PAR_SIN_INT = 6 };     /* los atributos salen, pero SIN la INT de la tarjeta en paralelo (por el tope): en 4 bits cada espera costaría 1,5 s */
/* cómo acabó la espera tras pasar a paralelo (MsInfo.par_fin) */
enum { MS_PAR_FIN_NADA = 0, MS_PAR_FIN_INT = 1, MS_PAR_FIN_CED = 2, MS_PAR_FIN_TOPE = 3 };
typedef struct {
    u8  estado;                                     /* MS_FUERA … MS_MALA */
    u8  protegida;                                  /* la tarjeta se declara de solo lectura: STATUS0 bit 0, clase, tipo de dispositivo (MSW: escribir se niega) */
    u8  por_orden;                                  /* sectores por orden en uso (MS_POR_ORDEN, o 4 tras un fallo: lo calibrado con el original) */
    signed char motivo;                             /* el último MS_E_* (0 = ninguno) */
    u8  reg[6];                                     /* registros 0x02..0x07 de la tarjeta leídos al identificar */
    u16 bloque, bloques, usuario, unidad;           /* de la información del sistema */
    u32 sectores;                                   /* de 512 bytes */
    u32 generacion;                                 /* sube cada vez que una tarjeta queda LISTA tras entrar (no al despertar) */
    /* cuentas */
    u32 entradas, salidas;                          /* tarjetas metidas / sacadas (tras el antirrebote) */
    u32 encendidos, reposos;                        /* veces que se le ha dado tensión / que se apagó por 30 s sin uso */
    u32 ordenes, leidos;                            /* órdenes 0x20 mandadas y sectores entregados */
    u32 ordenes_w, escritos, e_escritura;           /* (MSW) órdenes 0x21, sectores aceptados por la tarjeta, escrituras falladas */
    u32 tpc, tpc_reintentos;                        /* transacciones y las repetidas */
    u32 e_tiempo, e_crc, e_int, e_estado;           /* fallos: TPC sin respuesta, CRC, INT que no llega, la tarjeta dice ERR/CMDNK */
    u32 recuperaciones;                             /* apagar-esperar-encender tras un fallo */
    u32 ms_identificar;                             /* lo que tardó la última identificación */
    u8  serie;                                      /* (M2) 1 = el bus en serie, 1 bit (W810 siempre; W580 si el paso a 4 bits falló) */
    u32 paralelo_fallos;                            /* (M2) veces que el paso a 4 bits falló */
    u8  int_serie;                                  /* (M2) en serie, ¿el anfitrión da la INT de la tarjeta? MS_INT_SERIE_* (lo aprende hal/ms.c) */
    /* (M2 ag) EL DIAGNÓSTICO DEL PASO A 4 BITS, para una foto en el aparato */
    u8  par_fase;                                   /* el tramo del ÚLTIMO fallo del paso a 4 bits (MS_PAR_*; 0 = nunca falló) */
    u8  par_t;                                      /* su código: el MS_T_* del TPC que falló, o -motivo (MS_E_*) de los atributos */
    u8  par_fin;                                    /* cómo acabó la última espera tras pasar a paralelo: MS_PAR_FIN_* */
    u8  int_paso;                                   /* ¿llega una INT tras pasar a paralelo? MS_INT_SERIE_* (aprendido: si no llega, la espera se acorta) */
    u16 par_ms;                                     /* lo que duró esa espera (ms) */
    u8  par_reg[7];                                 /* los registros 0x02..0x07 y 0x10 leídos YA EN 4 BITS (la comprobación del bus) */
    u32 paralelo_bien;                              /* veces que el paso a 4 bits salió entero (hasta los atributos) */
    /* (28-oct) tras una escritura fallada, CERRAR la orden en vez de quitar la tensión (hal/ms.c cerrar_escritura) */
    u32 cierres_w;                                  /* órdenes 0x21 falladas que se intentaron cerrar */
    u32 cerradas_w;                                 /* … y llegaron a CED (la tarjeta sigue encendida y lista) */
    u32 relleno_w;                                  /* sectores dados para cerrarlas (dentro del tramo de la orden) */
    u32 cortes_w;                                   /* tensión quitada con una orden 0x21 abierta (la tarjeta, muda): lo que rompe una flash */
    u32 cierres;                                    /* (28-oct) cierres completos: 1 sector en cada bloque de la zona (hal/ms.c, «el cierre») */
    u32 cierre_sectores, cierre_fallos;             /* … sectores escritos en la zona y los que fallaron */
    u32 cortes_sin_cierre;                          /* tensión quitada con escrituras SIN cerrar (sin zona, o falló el cierre): pueden perderse */
} MsInfo;

#ifndef MS_POR_ORDEN
#define MS_POR_ORDEN 64u                            /* sectores por orden 0x20. El original no pide nunca más de 4: ver docs/tarjeta.md §2.6 */
#endif
#ifndef MS_ESCRIBE_POR_ORDEN
#define MS_ESCRIBE_POR_ORDEN 32u                    /* (MSW) sectores por orden 0x21. El original vuelca rachas de su caché de 35 (vistas hasta 28) */
#endif
#ifndef MS_TRAMO_SERIE_MS
#define MS_TRAMO_SERIE_MS 8u                        /* (M2) en serie, cada cuánto se pregunta con GET_INT mientras se espera la INT (hal/ms.c espera_serie) */
#endif
#define MS_INT_FALLOS_SERIE 3u                      /* (M2) respuestas sin su INT para concluir que en serie no hay MSINT */
#define MS_PASO_INT_MS 1500u                        /* (M2 ag) tras pasar a paralelo, lo que espera el original la INT [4477.c:11017: receive_w_tmo(0x5dc)] */
#ifndef MS_PASO_CORTO_MS
#define MS_PASO_CORTO_MS 20u                        /* (M2 ag) … y lo que se espera una vez aprendido que esa INT no llega (MsInfo.int_paso = NO) */
#endif
#define MS_REPOSO_MS 30000u                         /* como el original (44A1.c:7239) */
#define MS_CIERRE_MAX 8u                            /* (28-oct) bloques de la zona de cierre, como mucho (hal/ms.c, «el cierre») */
#ifndef MS_CIERRE_MS
#define MS_CIERRE_MS 1000u                          /* (28-oct) sin escribir este rato: se empieza a cerrar, un sector por vuelta (tras el «limpio» de FATW, 800 ms) */
#endif

void ms_init(const MsAnf *a);
void ms_paso(void);                                 /* desde el bucle: entra/sale (antirrebote de 20 ms), prepara la tarjeta nueva, reposo a los 30 s */
int  ms_leer(u32 lba, u32 n, u8 *buf);              /* 0 = bien; < 0 = MS_E_*. Despierta la tarjeta si dormía */
int  ms_escribir(u32 lba, u32 n, const u8 *buf);    /* (MSW) 0 = bien; < 0 = MS_E_* (FUERA, RANGO, PROTEGIDA, ESCRITURA…). SIN reintentos */
int  ms_vaciar(void);                               /* (MSW) 0 = no queda nada a medias en la tarjeta */
const MsInfo *ms_info(void);
const char *ms_motivo(int e);
const char *ms_par_fase(int f);                     /* (M2 ag) MS_PAR_* en palabras cortas, para la pantalla */
void ms_apagar_ya(void);                            /* antes de apagar el teléfono o de bajar el reloj (cierra lo escrito antes) */
void ms_cierre_zona(const u32 *lba, u32 n, const u8 *sector);   /* (28-oct) la zona de cierre: n (<= MS_CIERRE_MAX) sectores, cada uno en un
                                                       bloque de borrado distinto, y lo que se escribe en ellos (512 B; los 4 últimos, un contador) */
u32  ms_cierre_pendiente(void);                     /* (28-oct) 1 = hay escrituras sin cerrar */
u32  ms_int_reintentos(void);                       /* (ms810) GET_INT repetidos tras un fallo (en serie) */
#endif
