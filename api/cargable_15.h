/* cargable_15.h — (1.5) EL GRUPO DE SONIDO Y VIBRADOR de la tabla `w5`, visto desde el SISTEMA (docs/apps_cargables.md §5.8).
 *
 * El lado de las apps es cargables/sdk/w5_sonido.h (se incluye detrás de w5.h). Este fichero repite, para el sistema, lo MISMO y
 * con los MISMOS números: las funciones en el mismo orden y con la misma firma, W5_SON_MENOR y las claves de ajuste nuevas. Que
 * los dos digan lo mismo lo comprueba «make probar-son-abi» (herr/comprobar_son_abi.py).
 *
 * Dónde va el grupo: DETRÁS del grupo Bluetooth (1.4), que va detrás de la 1.3. Como con la 1.4, W5_API (cargable.h) NO cambia:
 * ni la tabla congelada, ni W5Api, ni w5_nombres.h, ni ninguna app ya compilada. El sistema arma W5Api + 1.4 + 1.5 en una sola
 * estructura (CgApi, shell/cargador.h) y dice n = 1.3 + 15 + este grupo, y menor 5 (CG_API_MENOR).
 *
 * Lo que trae (decisiones del mantenedor, 2026-10-25):
 *   - mu_tocar_ruta: tocar CUALQUIER MIDI o MP3 del teléfono por su ruta, por el servicio de música del sistema. Como lo que pone
 *     el Walkman, SIGUE SONANDO con la app cerrada (se para o se pausa desde la Música o con las teclas). Pide W5_CAP_MUSICA,
 *     como el resto de mu_*.
 *   - ap_vibrar_patron: el vibrador continuo o A GOLPES (1/2/4/8 Hz con el 25/50/75/100 % del periodo; medido en el aparato:
 *     docs/reconocimiento_original.md §2), hasta 10 s por llamada; se para solo al cerrarse la app.
 *   - LOS DOS VOLÚMENES con los ajustes de la 1.2 (ap_ajuste / ap_ajuste_poner): W5_AJ_VOLUMEN (tonos y avisos del sistema, la
 *     fila «Volumen» de Ajustes) y W5_AJ_VOLUMEN_MUSICA (el de la música, el de las teclas Vol+/Vol− y de mu_volumen). 0..5.
 *     No piden W5_CAP_RELOJ (la alarma sí, como siempre). */
#ifndef CARGABLE_15_H
#define CARGABLE_15_H
#include "cargable_bt.h"

#define W5_SON_MENOR 5                         /* la menor de la API que trae este grupo */
enum { W5_AJ_VOLUMEN = 5,                      /* 0..5: tonos y avisos del sistema (0 = silencio) */
       W5_AJ_VOLUMEN_MUSICA };                 /* 0..5: la música (el servicio de mu_*) */
#define W5_VOL_MAX 5
#define W5_VIBRAR_MAX_MS 10000                 /* lo más que dura una vibración pedida con ap_vibrar_patron */

/* LAS MISMAS LÍNEAS que W5_API15 de cargables/sdk/w5_sonido.h (sin los comentarios largos: el contrato documentado está allí) */
#define W5_API15(F) \
    F(int, mu_tocar_ruta, (const char *ruta)) \
    F(int, ap_vibrar_patron, (int hz, int pct, int ms))
#endif
