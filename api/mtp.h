/* mtp.h — el RESPONDEDOR MTP: la «Memoria» (W6F) vista por la computadora como un dispositivo MTP (pista MTP; contrato:
 * docs/usb_contrato.md §3; lo hecho, las pruebas y lo que falta: docs/mtp.md).
 * Por encima del «tubo» de api/usb.h (usb_tubo_*: contenedores MTP enteros por bulk, eventos por interrupt IN, avisos de
 * cancelar/reset por usb_tubo_aviso) y de hw_w6f_* (api/hw.h). Portable: el mismo código en el teléfono y en el PC.
 * Nada aquí bloquea: mtp_paso() hace como mucho MTP_PASO_BYTES de E/S o MTP_PASO_MS y vuelve. RAM fija (~9 KiB), sin
 * cargar nunca un fichero entero. */
#ifndef MTP_H
#define MTP_H
#include "hw.h"

/* ---- el contrato (§3) ---- */
void mtp_iniciar(void);          /* al configurarse el USB: sin sesión; limpia un temporal que dejara un corte; crea /apps si falta */
void mtp_paso(void);             /* desde el bucle; usa usb_tubo_*; troceado (nunca bloquea al perro) */
void mtp_parar(void);            /* al soltar el cable o parar el USB: lo que se estuviera recibiendo se tira (no queda nada) */

/* ---- (MTP) lo que añade (Propuesta de cambio en el contrato) ---- */
/* Sube cada vez que el respondedor cambia algo en la carpeta /apps de la Memoria (llega, se borra o se renombra un fichero):
 * el shell lo mira y rehace el menú como cuando cambia la tarjeta (sh_apps_init con el standby a la vista). */
u32  mtp_apps_cambios(void);
/* Para la pantalla «Conectado por USB» y la app Sistema. */
typedef struct {
    u8  sesion, ocupado;         /* hay sesión abierta; hay una transacción en curso */
    u16 op;                      /* la operación en curso (o la última) */
    u8  pide_permiso;            /* (AUD9) el host ha intentado abrir sesión sin el «Sí» del usuario */
    u32 transacciones, errores, recibidos, borrados, bytes_rx, bytes_tx, rx_tam, rx_hecho;   /* rx_*: el SendObject en curso */
    u32 ajenos;                  /* (AUD9 A1) veces que la Memoria cambió fuera de MTP con el respondedor en marcha */
    u8  almacenes, tarjeta;      /* (v10) cuántas storages anuncia (1 = Memoria, 2 = + Tarjeta) y el HW_TJ_* con que MTP ve la tarjeta AHORA */
} MtpInfo;
void mtp_info(MtpInfo *i);
/* (AUD9) CONTROL DE ACCESO, como Android: al configurarse el USB el teléfono PREGUNTA «¿Permitir acceso a la Memoria desde el
 * ordenador?» (empezando en «No»). Hasta mtp_permitir(1), GetDeviceInfo contesta pero OpenSession da Device_Busy (0x2019);
 * con mtp_permitir(0), Access_Denied (0x200F) y, si había sesión, se cierra. mtp_iniciar vuelve a «sin preguntar».
 * mtp_permiso(): 0 = sin contestar, 1 = sí, 2 = no. */
void mtp_permitir(int si);
int  mtp_permiso(void);
int  mtp_sesion_abierta(void);   /* el shell pone la pantalla «Conectado por USB» MODAL mientras dure (la Memoria quieta) */

#define MTP_PASO_BYTES 16384u    /* E/S de datos como mucho por paso */
#define MTP_PASO_MS    4u        /* y como mucho este tiempo (hw_ms va a saltos de 4) */
#define MTP_PLAZO_MS   30000u    /* una fase de datos de la computadora que se para más de esto se da por perdida */
#define MTP_STORAGE    0x00010001u
#define MTP_STORAGE_TJ 0x00020001u   /* (FATW) la tarjeta, mientras la haya */
#define MTP_TEMP       ".mtp~rx" /* el temporal de lo que se recibe, en la raíz de la Memoria; los nombres «.mtp~…» no se enseñan */
#define MTP_DIR_APPS   "apps"    /* la carpeta del sistema: las .sen que llegan aquí salen en el menú; no se borra ni se renombra */
#endif
