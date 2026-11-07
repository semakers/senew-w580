/* w6f_imagen.c — (I1) la imagen de la Memoria (W6F) YA FORMATEADA para la flash del EMULADOR (herr/hacer_flash.py --w6f), hecha
 * con el MISMO api/w6f.c y la MISMA configuración (api/w6f_sistema.h) que el sistema, sobre un modelo de la zona en RAM.
 * Es lo que haría el sistema la primera vez con la zona en blanco (hw_w6f_formatear), sin los 13–18 s en cada arranque de las
 * pasadas. Opcional: --con DIR_W6F FICHERO... mete ficheros (para las pruebas del lector).   w6f_imagen SALIDA [N_BLOQUES] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w6f.h"
#include "w6f_sistema.h"

static unsigned char *z; static uint32_t nb;
#define TAMB (W6F_SIS_PAG * W6F_SIS_PPB)
static int leer(void *c, uint32_t b, uint32_t p, uint32_t o, void *buf, uint32_t n)
{ (void)c; if (b >= nb || p >= W6F_SIS_PPB || o > W6F_SIS_PAG || n > W6F_SIS_PAG - o) return -1; memcpy(buf, z + (size_t)b * TAMB + p * W6F_SIS_PAG + o, n); return 0; }
static int prog(void *c, uint32_t b, uint32_t p, const uint8_t *d)
{
    unsigned char *q; uint32_t i; (void)c;
    if (b >= nb || p >= W6F_SIS_PPB) return -1;
    q = z + (size_t)b * TAMB + p * W6F_SIS_PAG;
    for (i = 0; i < W6F_SIS_PAG; i++) if (q[i] != 0xFF) { fprintf(stderr, "w6f_imagen: programar una pagina que no esta en blanco\n"); exit(1); }
    memcpy(q, d, W6F_SIS_PAG); return 0;
}
static int borrar(void *c, uint32_t b) { (void)c; if (b >= nb) return -1; memset(z + (size_t)b * TAMB, 0xFF, TAMB); return 0; }
int main(int argc, char **argv)
{
    static uint32_t mem_fija[W6F_SIS_MEM_U32]; uint32_t *mem = mem_fija, tam_mem = sizeof mem_fija;
    W6fFlash fl = { 0, W6F_FL_NOR_OBJETO, 0, W6F_SIS_PAG, W6F_SIS_PPB, leer, prog, borrar, 0, 0 };
    W6fConf cf = w6f_sis_conf(0);
    W6f fs; FILE *o; int r, i;
    if (argc < 2) { fprintf(stderr, "uso: w6f_imagen SALIDA [N_BLOQUES] [--con RUTA FICHERO]...\n"); return 2; }
    nb = argc > 2 && argv[2][0] != '-' ? (uint32_t)atoi(argv[2]) : 64u;
    fl.n_bloques = nb;
    z = malloc((size_t)nb * TAMB); memset(z, 0xFF, (size_t)nb * TAMB);
    if (w6f_mem_necesaria(&fl, &cf) > tam_mem) { tam_mem = w6f_mem_necesaria(&fl, &cf); mem = malloc(tam_mem); }   /* otra zona (las variantes de prueba) */
    if ((r = w6f_formatear(&fl, &cf, mem, tam_mem, 0, 0))) { fprintf(stderr, "w6f_imagen: formato -%d\n", -r); return 1; }
    for (i = 2; i < argc; i++) if (!strcmp(argv[i], "--con") && i + 2 < argc) {
        FILE *f = fopen(argv[i + 2], "rb"); long n; unsigned char *d; const char *p;
        if (!f) { perror(argv[i + 2]); return 1; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); if (n < 0) return 1;
        d = malloc((size_t)n + 1); if (fread(d, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
        if ((r = w6f_montar(&fs, &fl, &cf, mem, tam_mem))) { fprintf(stderr, "w6f_imagen: montar -%d\n", -r); return 1; }
        for (p = argv[i + 1] + 1; (p = strchr(p, '/')) != 0; p++) {        /* las carpetas de la ruta */
            char dir[1024]; size_t k = (size_t)(p - argv[i + 1]); memcpy(dir, argv[i + 1], k); dir[k] = 0;
            if (w6f_buscar(&fs, dir) < 0 && (r = w6f_mkdir(&fs, dir, 0))) { fprintf(stderr, "w6f_imagen: mkdir %s -%d\n", dir, -r); return 1; }
        }
        if ((r = w6f_guardar(&fs, argv[i + 1], d, (uint32_t)n, 0))) { fprintf(stderr, "w6f_imagen: guardar %s -%d\n", argv[i + 1], -r); return 1; }
        free(d); i += 2;
    }
    if (!(o = fopen(argv[1], "wb")) || fwrite(z, 1, (size_t)nb * TAMB, o) != (size_t)nb * TAMB) { perror(argv[1]); return 1; }
    fclose(o);
    printf("%s: Memoria (W6F) formateada, %u bloques de %u KiB, max_obj %u, reserva %u\n", argv[1], nb, TAMB / 1024, W6F_SIS_MAX_OBJ, W6F_SIS_RESERVA);
    return 0;
}
