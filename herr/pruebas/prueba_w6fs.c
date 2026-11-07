/* Azar sobre el lector de W6FS: imágenes dañadas (bytes cambiados, truncadas, y la mitad con los CRC de
 * cabecera y tabla recompuestos para que el daño llegue a la validación de rangos) → montar y, si monta,
 * recorrerlo todo. Con ASan/UBSan y la imagen en un bloque del tamaño justo: nada puede leer fuera.
 *   prueba_w6fs imagen.bin [casos] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w6fs.h"
#include "almacen.h"
void hw_log(const char *s) { (void)s; }
static u32 az = 99;
static u32 azar(void) { az ^= az << 13; az ^= az >> 17; az ^= az << 5; return az; }
static long visitas;
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); long n, casos = argc > 2 ? atol(argv[2]) : 20000, c, montadas = 0;
    u8 *bien, *img; W6fs fs;
    if (!f) return 1;
    fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    bien = malloc((size_t)n); if (fread(bien, 1, (size_t)n, f) != (size_t)n) return 1;
    if (!w6fs_montar(&fs, bien, (u32)n)) { printf("la imagen buena no monta\n"); return 1; }
    if (w6fs_buscar(&fs, "/docs/LEEME.txt") < 0 || !w6fs_comprobar(&fs, w6fs_buscar(&fs, "/docs/LEEME.txt"))) { printf("no encuentra /docs/LEEME.txt\n"); return 1; }
    for (c = 0; c < casos; c++) {
        long tam = n, k, m = 1 + azar() % 6; u32 i;
        if (azar() % 4 == 0) tam = (long)(azar() % (u32)n);
        img = malloc((size_t)(tam ? tam : 1)); memcpy(img, bien, (size_t)tam);
        for (k = 0; k < m && tam > 32; k++) {
            u32 donde = azar() % 3 ? 4 + azar() % (32 + 20 * 13 + 40) : azar() % (u32)tam;   /* sobre todo cabecera, tabla y nombres */
            if (donde < (u32)tam) img[donde] = (u8)azar();
        }
        if (tam >= 32 && azar() % 2) {
            u32 ne = img[8] | (img[9] << 8) | (img[10] << 16) | ((u32)img[11] << 24), fin = (u32)tam, crc;
            if (ne && ne < 5000 && 32 + 20 * ne < (u32)tam) {
                for (i = 0; i < ne; i++) { const u8 *e = img + 32 + 20 * i; u32 a = e[4] | (e[5] << 8) | (e[6] << 16) | ((u32)e[7] << 24); if (!(e[16] | e[17] | e[18] | e[19]) && a < fin) fin = a; }
                if (fin > 32) { crc = alm_crc32(0, img + 32, fin - 32); memcpy(img + 24, &crc, 4); }
            }
            crc = alm_crc32(0, img, 28); memcpy(img + 28, &crc, 4);
        }
        if (w6fs_montar(&fs, img, (u32)tam)) {
            u32 e;
            montadas++;
            for (e = 0; e < fs.n; e++) {
                static HwEntrada en[4];
                if (w6fs_es_dir(&fs, (int)e)) { visitas += w6fs_listar(&fs, (int)e, 0, en, 4); w6fs_listar(&fs, (int)e, w6fs_tam(&fs, (int)e) ? w6fs_tam(&fs, (int)e) - 1 : 0, en, 4); }
                else w6fs_comprobar(&fs, (int)e);
            }
            w6fs_buscar(&fs, "/docs/LEEME.txt"); w6fs_buscar(&fs, "/fondos/Plasma.jpg"); w6fs_buscar(&fs, "//imagenes//"); w6fs_buscar(&fs, "/a/b/c/d");
        }
        free(img);
    }
    printf("w6fs: %ld imágenes dañadas, %ld llegaron a montarse y se recorrieron enteras (%ld entradas listadas); sin errores de memoria\n", casos, montadas, visitas);
    return 0;
}
