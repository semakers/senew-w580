/* reinicio.c — (R3; propuesta P3 de la pista S7) EL REGISTRO DE «CAUSA DEL ÚLTIMO REINICIO»: unos bytes que SOBREVIVEN a un
 * reinicio en caliente (el del perro de software, una excepción, el apagado forzado que no apaga, sys_reset) y que el
 * arranque siguiente lee ANTES de hacer nada. Sirve para dos cosas: decir «el sistema se reinició por…» en vez de volver al
 * standby como si nada, y entrar SOLO en modo seguro tras 3 arranques seguidos que no llegan al bucle (un ajuste o un
 * fichero que cuelga el arranque ya no deja el teléfono dando vueltas hasta que alguien sepa lo de la tecla C).
 *
 * DÓNDE, y por qué ahí: en la iRAM del SoC (0xF3000000, 16 KiB), en las dos direcciones que fija hal/enlace.ld.
 *  - La SDRAM no vale: hal/arranque.S la reinicia en cada arranque (apagado ordenado + reprogramación del EMC, como el crt0
 *    del original): su contenido no se puede dar por conservado.
 *  - Un registro libre del RTC sobreviviría incluso sin batería principal, pero [sin saber] cuáles están libres en el DB2020
 *    y escribir en uno equivocado estropea la hora o la alarma. No se toca.
 *  - La iRAM es RAM estática interna: mientras el núcleo tenga corriente conserva lo escrito, y nuestro arranque solo usa
 *    sus primeros 0x210 bytes (pilas provisionales). No hace falta cambiar arranque.S.
 * [sin saber] en el aparato: si el cargador de arranque de Sony Ericsson (que corre antes que nosotros tras un reset de
 * verdad) pisa esas direcciones, y si un reset por el perro de hardware conserva la iRAM. Por eso: FIRMA + CRC-32, DOS
 * copias en sitios separados, y si ninguna vale la causa es «desconocida» y la cuenta empieza en 1: se degrada, no se inventa.
 * [emulador] la iRAM se conserva en reinicio_caliente y con reset-real (probado: herr/pasada_r3_w580.sh).
 *
 * LA CUENTA: cada arranque suma 1 a `sin_bucle`; plat/ la pone a 0 cuando el bucle principal lleva 5 s vivo, y también la
 * ponen a 0 un apagado ordenado y un reinicio pedido (un arranque que se apaga por batería baja no es un cuelgue). */
#include "hal.h"

extern char _reg_reinicio_a[], _reg_reinicio_b[];        /* hal/enlace.ld */
#define FIRMA 0x52335253u                                /* «SR3R» */
typedef struct { u32 firma; u8 causa, sin_bucle, version, nor_blq; u32 pc, lr, arranques, crc; } Reg;   /* 24 bytes; nor_blq: (AUD6 B1) 1 + bloque que hal/nor.c estaba borrando, o 0 */

static u32 crc_de(const Reg *r)
{
    const u8 *p = (const u8 *)r; u32 c = 0xFFFFFFFFu, i; int k;
    for (i = 0; i < sizeof *r - 4; i++) { c ^= p[i]; for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static int leer_copia(const char *donde, Reg *r)
{
    const volatile u8 *o = (const volatile u8 *)donde; u8 *d = (u8 *)r; u32 i;    /* byte a byte: sin trampas de alias */
    for (i = 0; i < sizeof *r; i++) d[i] = o[i];
    return r->firma == FIRMA && r->version == 1 && r->crc == crc_de(r);
}
/* (AUD6 B3) si valen las dos, la MÁS NUEVA (más arranques): un reinicio entre grabar la B y la A deja la A vieja */
static int leer(Reg *r)
{
    Reg b;
    int va = leer_copia(_reg_reinicio_a, r), vb = leer_copia(_reg_reinicio_b, &b);
    if (vb && (!va || b.arranques > r->arranques)) *r = b;
    return va || vb;
}
static void grabar(Reg *r)
{
    volatile u8 *a = (volatile u8 *)_reg_reinicio_a, *b = (volatile u8 *)_reg_reinicio_b; const u8 *o = (const u8 *)r; u32 i;
    r->firma = FIRMA; r->version = 1; r->crc = crc_de(r);
    for (i = 0; i < sizeof *r; i++) b[i] = o[i];         /* primero la B: si el reinicio corta esto a medias, la A sigue entera */
    for (i = 0; i < sizeof *r; i++) a[i] = o[i];
}
static void nuevo(Reg *r) { r->causa = RST_SIN_APUNTAR; r->sin_bucle = 0; r->pc = r->lr = 0; r->arranques = 0; r->nor_blq = 0; }

static u32 nor_pendiente;
/* UNA vez, lo primero del arranque: qué dejó apuntado el arranque anterior, y se apunta este (uno más sin llegar al bucle). */
void rst_arranque(RstInfo *o)
{
    Reg r;
    o->valido = (u8)leer(&r);
    if (!o->valido) nuevo(&r);
    o->causa = o->valido ? r.causa : RST_DESCONOCIDA; o->pc = r.pc; o->lr = r.lr;
    o->sin_bucle = r.sin_bucle; o->arranques = r.arranques;
    nor_pendiente = o->valido ? r.nor_blq : 0; r.nor_blq = 0;
    r.causa = RST_SIN_APUNTAR; r.pc = r.lr = 0;
    if (r.sin_bucle < 255) r.sin_bucle++;
    r.arranques++;
    grabar(&r);
}
/* Antes de reiniciar o de apagar: por qué. Un apagado ordenado o un reinicio pedido ponen la cuenta a 0. No falla nunca. */
void rst_apuntar(u8 causa, u32 pc, u32 lr)
{
    Reg r;
    if (!leer(&r)) nuevo(&r);
    r.causa = causa; r.pc = pc; r.lr = lr;
    if (causa == RST_APAGADO || causa == RST_PEDIDO) r.sin_bucle = 0;
    grabar(&r);
}
/* (AUD6 B1) hal/nor.c apunta qué bloque va a borrar (y 0 al acabar): así, tras un reinicio con un borrado SUSPENDIDO, el
 * arranque sabe si es nuestro. Si este registro no sobrevive, el suspendido no se atribuye y no se toca (sin escritura). */
u32 rst_nor_pendiente(void) { return nor_pendiente; }
void rst_nor_borrando(u8 bloque)
{
    Reg r;
    if (!leer(&r)) nuevo(&r);
    r.nor_blq = bloque;
    grabar(&r);
}
void rst_bucle_vivo(void)                                /* el arranque llegó al bucle y se sostiene: la cuenta, a 0 */
{
    Reg r;
    if (!leer(&r)) nuevo(&r);
    r.sin_bucle = 0;
    grabar(&r);
}
