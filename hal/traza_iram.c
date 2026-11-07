/* hal/traza_iram.c — EL RASTRO EN LA iRAM de senew EN RAM (27-oct, docs/senew_en_ram.md). Solo con -DSENEW_RAM.
 * Si senew se cuelga al arrancar desde la RAM, el PERRO (el bootloader lo arma, 4 s) reinicia el teléfono y vuelve el original con su daemon;
 * la iRAM SOBREVIVE a ese reinicio (medido 27-oct) y el original puede leerla (fisico/elf/puente_orig/herr/iram.py). Aquí queda:
 *   0xF3003000: magia 'TZRM', tamaño del anillo, posición (total escrito), etapa (la última que marcó arranque.S o tz_etapa)
 *   0xF3003010: el anillo de 2 KiB con TODO lo que pasó por hw_log (las líneas del arranque), lo más reciente al final.
 * NO se borra al arrancar el original: arranque.S lo reinicia al principio de CADA arranque de senew. */
#include "hal.h"
#ifdef SENEW_RAM
#define TZ_CAB 0xF3003000u
#define TZ_ANILLO (TZ_CAB + 16u)
#define TZ_TAM 2048u
typedef struct { volatile u32 magia, tam, pos, etapa; } TzCab;
void tz_iniciar(void) { TzCab *c = (TzCab *)TZ_CAB; c->magia = 0x4D525A54u; c->tam = TZ_TAM; c->pos = 0; c->etapa = 1; }
void tz_etapa(u32 n) { ((TzCab *)TZ_CAB)->etapa = n; }
void tz_log(const char *s)
{
    TzCab *c = (TzCab *)TZ_CAB; volatile u8 *r = (volatile u8 *)TZ_ANILLO; u32 p = c->pos;
    while (*s) r[p++ & (TZ_TAM - 1u)] = (u8)*s++;
    c->pos = p;
}
#else
typedef int tz_vacio_;      /* sin SENEW_RAM este fichero no aporta nada */
#endif
