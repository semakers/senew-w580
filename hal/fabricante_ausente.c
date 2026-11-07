/* fabricante_ausente.c — senew PÚBLICO: los datos que en el árbol de desarrollo se sacan del software del fabricante NO vienen en
 * este repositorio (decisión del proyecto: nada de Sony Ericsson salvo lo imprescindible para instalar, que vive en SENUS).
 *
 * Aquí solo hay definiciones DÉBILES y VACÍAS de esos datos, para que senew compile y arranque igual:
 *   - las imágenes del DSP (hal/dsp_w580_img*.c en el árbol de desarrollo): sin ellas el DSP no se arranca y dsp_cargar() devuelve
 *     DSP_E_IMAGEN; se pierden el MP3 por el DSP (hal/mp3_w580.c) y el micrófono (hal/mic_w580.c, que usa el módulo AAC+).
 *   - las plantillas de mensajes ARM->DSP del reproductor MP3 (hal/mp3_w580_dat.c): sacadas de trazas del firmware original; no hay
 *     forma de regenerarlas sin él. Sin las imágenes del DSP no se llegan a usar.
 *   - los ficheros acústicos .zapf de reserva (hal/mp3_w580_apfdat.c): senew los lee del FS de fábrica del propio teléfono
 *     (/ifs/settings/acoustic/); la copia embebida era solo una reserva. Vacía: se usa siempre el FS del teléfono.
 *   - la tabla de arranque del sensor de la cámara (hal/camara_w580_tab.c): se GENERA desde el camdriver0.dat de TU teléfono con
 *     «make camara-tabla CAMDRIVER=…» (herr/camara/gen_tabla_c.py). Sin ella, cam_iniciar() devuelve error (-3).
 * Si generas cualquiera de esos ficheros (o los copias de tu propio árbol), sus definiciones FUERTES sustituyen a estas al enlazar:
 * no hay que tocar nada más. La pantalla (hal/lcd_paneles.h) va por otro camino: hal/lcd_paneles_stub.h. Ver README.md y NOTICE. */
#include "dsp_w580.h"
#include "mp3_w580_i.h"
#include "mp3_w580_dat.h"

#define DEBIL __attribute__((weak))

DEBIL const DspImagen dsp_img_comun    = { "comun", 0u, 0u, 0, 0, 0u };
DEBIL const DspImagen dsp_img_arranque = { "arranque", 0u, 0u, 0, 0, 0u };
DEBIL const DspImagen dsp_img_mp3      = { "mp3", 0u, 0u, 0, 0, 0u };
DEBIL const DspImagen dsp_img_aac_plus = { "aac_plus", 0u, 0u, 0, 0, 0u };

DEBIL const Mp3ApfEmb mp3_apf_emb[1] = { { "", 0, 0u } };
DEBIL const unsigned mp3_apf_emb_n = 0u;

DEBIL const u16 mp3_pool[1939];
DEBIL const Mp3Plan mp3_plan1[MP3_PLAN1_N];
DEBIL const u16 mp3_filtro_pre[11], mp3_drc6_pre[12], mp3_drc9_pre[9];

DEBIL const u8 cam_tabla[3] = { 0, 0, 0 };
DEBIL const u32 cam_tabla_n = 0u;
