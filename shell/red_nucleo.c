/* red_nucleo.c — (1.6) bt/red/red.c, el lado del teléfono de W5RED (bt/puente_red/PROTOCOLO.md), DENTRO del sistema y en todas las
 * plataformas (es C portátil, sin BTstack): se incluye aquí para no tocar las listas de fuentes del Makefile (sus .h y .c están en
 * CAB, así que un cambio allí recompila). Sin radio nadie llama a red_empezar: las apps reciben «no disponible» (shell/cg_red.c). */
#include "../bt/red/red.c"
