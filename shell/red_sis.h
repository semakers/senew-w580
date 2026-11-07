/* red_sis.h — (1.6) LA RED de senew por el puente Bluetooth (bt/puente_red/PROTOCOLO.md), vista desde el sistema. La sesión W5RED va
 * sobre el SPP del servicio Bluetooth (shell/bt_sis.h): o red o robot, uno a la vez.
 * (1.8, 26-oct) VARIOS DUEÑOS: SH_BT_SISTEMA (Ajustes, «probar el puente»), SH_BT_APP (la app cargada) y SH_BT_MUSICA (la música por la red,
 * que sigue con la app cerrada). Conectar con la red ya LISTA o de camino hacia el mismo puente SUMA un dueño; soltar uno cierra SUS canales
 * y solo corta la red si no queda nadie. Cada canal es de quien lo abrió (sh_red_canal_poner): la app solo ve y toca los suyos. */
#ifndef RED_SIS_H
#define RED_SIS_H
#include "hw.h"
enum { SH_RED_APAGADA = 0, SH_RED_CONECTANDO, SH_RED_LISTA, SH_RED_FALLO };
enum { SH_REDF_OK = 0, SH_REDF_SIN_RADIO, SH_REDF_SIN_PUENTE, SH_REDF_RADIO, SH_REDF_ENLACE, SH_REDF_SALUDO, SH_REDF_CAIDA, SH_REDF_OCUPADA, SH_REDF_ALMACEN /* no se pudo guardar el puente: solo SEN_BT_EMPAREJAR */ };
int  sh_red_hay(void);                              /* 1 si hay radio (el W580 con su controlador) */
int  sh_red_conectar(const u8 *addr, int quien);    /* addr NULL = el puente guardado; quien = SH_BT_*. 0 = arrancado (o ya estaba: un dueño más) */
void sh_red_soltar(int quien);                      /* (1.8) ese dueño se va: sus canales se cierran; la red se corta si no queda nadie */
void sh_red_desconectar(void);                      /* cortarla para TODOS (lo de antes; mejor sh_red_soltar) */
int  sh_red_estado(void);
int  sh_red_fallo(void);
int  sh_red_es_dueno(int quien);                    /* (1.8) 1 si quien la está usando (la conectó o se sumó) y no está apagada */
int  sh_red_duenos(void);                           /* (1.8) los dueños, como bits 1 << SH_BT_* (0 si está apagada) */
int  sh_red_heredero(int quien);                    /* (1.8) otro dueño que la siga usando (MUSICA, SISTEMA, APP), o SH_BT_NADIE */
void sh_red_canal_poner(int c, int quien);          /* (1.8) el canal c (1..RED_CANALES) es de quien (SH_BT_NADIE: libre) */
int  sh_red_canal_dueno(int c);                     /* (1.8) de quién es el canal c */
void sh_red_paso(void);                             /* desde sh_bt_paso: el bucle y app_latido */
void sh_red_soltar_app(void);                       /* al descargarse una app = sh_red_soltar(SH_BT_APP) */
int  sh_red_puente(u8 a[6]);                        /* el puente elegido (almacén NV_RED): 1 si hay */
int  sh_red_poner_puente(const u8 a[6]);            /* HW_OK o el error del almacén */
/* (0.29.1) EL PUENTE DE SENUS: docs/mtp.md, «SEN_BT_EMPAREJAR». Guarda el puente con «llámalo al arrancar», pone el PIN y llama como SH_BT_SISTEMA. La red queda RETENIDA: sigue
 * sin el cable y sin ningún dueño más, hasta que se cae (entonces se apaga la radio y el fallo queda legible), o se la pide un robot. */
int  sh_red_emparejar(const u8 a[6], const char *pin);   /* 0 = llamando (o ya estaba); -1 = no: sh_red_estado() es FALLO y sh_red_fallo() dice por qué */
int  sh_red_solo_retenida(void);                    /* 1 si la radio solo la tiene encendida esta retención: para una app, como apagada */
void sh_red_ceder(void);                            /* un robot quiere la radio: si la red está solo retenida, se aparta */
void sh_red_arranque_paso(void);                    /* del bucle del sistema: una sola llamada por arranque al puente de SENUS */
const char *sh_red_estado_nombre(int e);
const char *sh_red_fallo_nombre(int f);
#endif
