/* La tarjeta en el PC para api/tarjeta.c: un fichero imagen (la misma que usa el emulador: tools/e9_ms_imagen.py).
 * Pista T; la escritura, pista MSW (docs/tarjeta_escritura.md §5). Se lee y se escribe POR SECTORES con pread/pwrite
 * (nunca entera en memoria; sin la caché de stdio, para que lo que se dio por escrito esté en el fichero).
 *   SIS_TARJETA=fichero.img        hay tarjeta desde el arranque. SOLO LECTURA salvo con SIS_TARJETA_ESCRIBIR=1
 *   SIS_TARJETA_ESCRIBIR=1         (MSW) se abre para leer y escribir; sin él, la tarjeta se declara PROTEGIDA
 *   SIS_TARJETA_PROTEGIDA=1        (MSW) la tarjeta dice que está protegida (aunque el fichero se pueda escribir)
 *   SIS_TARJETA_FALLA=N            a partir del sector leído n.º N, las lecturas fallan (tarjeta que se estropea)
 *   SIS_TARJETA_SALE=N             al llegar al sector leído n.º N, la tarjeta «sale» (deja de haberla)
 *   SIS_TARJETA_FALLA_ESCRITURA=L  (MSW) escribir el sector de LBA L falla (error de E/S; los anteriores de la misma
 *                                  llamada quedan escritos, el L y los siguientes no). Cada vez, no solo la primera
 *   SIS_TARJETA_SALE_ESCRITO=N     (MSW) al ir a escribir el sector escrito n.º N (contando desde 0 todos los de todas
 *                                  las llamadas) la tarjeta «sale»: los anteriores quedan, ese y los siguientes no
 *   SIS_TARJETA_CORTE=N            (MSW) CORTE DE CORRIENTE al escribir el sector escrito n.º N: ese sector queda A
 *                                  MEDIAS (los primeros 256 bytes nuevos, el resto como estaba) y el proceso muere en
 *                                  el acto (_exit(86), sin cerrar nada). Lo de una M2 de verdad: [sin saber] (MS-31). */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "tarjeta.h"

static int fd = -1; static u32 sectores, leidos, falla, sale; static int mirado, fuera, protegida;
static u32 escritos; static long long falla_w = -1, sale_w = -1, corte = -1;
static long long num(const char *v) { const char *x = getenv(v); return x && *x ? atoll(x) : -1; }
static void abrir(void)
{
    const char *r = getenv("SIS_TARJETA"), *x;
    int rw = (x = getenv("SIS_TARJETA_ESCRIBIR")) && *x == '1';
    off_t t;
    mirado = 1;
    if (!r || !*r || (fd = open(r, rw ? O_RDWR : O_RDONLY)) < 0) return;
    t = lseek(fd, 0, SEEK_END); sectores = t > 0 ? (u32)(t / 512) : 0;
    if ((x = getenv("SIS_TARJETA_FALLA"))) falla = (u32)atoi(x) + 1;
    if ((x = getenv("SIS_TARJETA_SALE"))) sale = (u32)atoi(x) + 1;
    protegida = !rw || ((x = getenv("SIS_TARJETA_PROTEGIDA")) && *x == '1');
    falla_w = num("SIS_TARJETA_FALLA_ESCRITURA"); sale_w = num("SIS_TARJETA_SALE_ESCRITO"); corte = num("SIS_TARJETA_CORTE");
}
void hwo_tarjeta_paso(void) { if (!mirado) abrir(); }
u32 hwo_tarjeta_generacion(void) { if (!mirado) abrir(); return fd >= 0 && !fuera && sectores ? 1 : 0; }
u32 hwo_tarjeta_sectores(void) { return sectores; }
int hwo_tarjeta_leer(void *ctx, u32 lba, u32 n, u8 *buf)
{
    (void)ctx;
    if (fd < 0 || fuera || lba >= sectores || n > sectores - lba) return -1;
    for (; n; n--, lba++, buf += 512) {
        leidos++;
        if (sale && leidos >= sale) { fuera = 1; return -1; }
        if (falla && leidos >= falla) return -1;
        if (pread(fd, buf, 512, (off_t)lba * 512) != 512) return -1;
    }
    return 0;
}
/* (MSW) el mismo contrato que el W580 (api/tarjeta.h): sin reintentos; tras un error, lo de [lba, lba+n) es indeterminado */
int hwo_tarjeta_escribir(void *ctx, u32 lba, u32 n, const u8 *buf)
{
    (void)ctx;
    if (!mirado) abrir();
    if (fd < 0 || fuera) return HW_TJ_E_FUERA;
    if (!n) return 0;
    if (lba >= sectores || n > sectores - lba) return HW_TJ_E_RANGO;
    if (protegida) return HW_TJ_E_PROTEGIDA;
    for (; n; n--, lba++, buf += 512, escritos++) {
        if (sale_w >= 0 && escritos >= (u32)sale_w) { fuera = 1; return HW_TJ_E_FUERA; }
        if (falla_w >= 0 && lba == (u32)falla_w) return HW_TJ_E_ES;
        if (corte >= 0 && escritos == (u32)corte) {
            (void)!pwrite(fd, buf, 256, (off_t)lba * 512);
            fprintf(stderr, "tarjeta_pc: CORTE DE CORRIENTE al escribir el sector %u (escrito n.º %u): queda a medias\n", lba, escritos);
            _exit(86);
        }
        if (pwrite(fd, buf, 512, (off_t)lba * 512) != 512) return HW_TJ_E_ES;
    }
    return 0;
}
int hwo_tarjeta_protegida(void) { if (!mirado) abrir(); return fd >= 0 && !fuera && protegida; }
int hwo_tarjeta_vaciar(void *ctx) { (void)ctx; if (fd < 0 || fuera || protegida) return 0; return fdatasync(fd) ? HW_TJ_E_ES : 0; }
int hwo_tarjeta_mala(const char **motivo) { (void)motivo; return 0; }
int hwo_tarjeta_dormida(void) { return 0; }
int hwo_tarjeta_preparando(void) { return 0; }             /* (I1) el fichero no tiene CED */
/* (28-oct) el cierre: un fichero del PC no pierde nada al «apagarse»; se apunta la zona para que las pruebas la vean */
u32 tarjeta_pc_zona_n, tarjeta_pc_zona[8];
u32 hwo_tarjeta_bloque_sectores(void) { return 512; }
void hwo_tarjeta_cierre_zona(const u32 *lba, u32 n, const u8 *sector) { u32 i; (void)sector; for (i = 0; i < n && i < 8; i++) tarjeta_pc_zona[i] = lba[i]; tarjeta_pc_zona_n = n; }
