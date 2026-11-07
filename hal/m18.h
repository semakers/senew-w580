/* m18.h — el «MODO CONTROL» de la NOR Intel M18 («Sibley») del W580i, en funciones PURAS (sin registros), para
 * que las compile también el PC (herr/pruebas/prueba_m18.c).
 *
 * El M18 no es una NOR clásica: se programa por REGIONES de 1 KiB con la orden de búfer (0xE9) y restringe
 * volver a programar dentro de una región ya escrita. Para datos que se van añadiendo poco a poco (que es
 * justo lo que hacen el almacén y los ficheros de usuario) el chip tiene el modo control: de cada segmento de
 * 32 bytes solo se usan los 16 primeros y los otros 16 se dejan a 0xFF; a cambio, la región se puede programar
 * muchas veces. Es lo que hace el firmware original con su GDFS [observado, FUN_44f964dc del R8BE001]:
 *     física = base + ((lógica * 2) & ~0x1F) + (lógica & 0xF)
 * con bloques LÓGICOS de 128 KiB sobre bloques de borrado de 256 KiB (gdfs_set_config(0x47E00000, 0x20000, 8, 7)).
 * CÓMO se programa en modo control [observado por la pista E4 con el original, 2026-10-13]: PALABRA A PALABRA con la
 * orden 0x41, solo en la mitad baja de cada segmento, y volviendo sobre palabras ya escritas (bajando bits). Los
 * programas de BÚFER (0xE9, 512 palabras) de la traza e14 son del FS de fábrica, que va en modo OBJETO: no valen
 * para esto (la primera versión de esta capa, de la pista S, mandaba la región entera por búfer con 0xFFFF de
 * relleno; el modo estricto de E4 lo cazó). Esta capa sigue repartiendo por regiones de 1 KiB y entregando un
 * «búfer» de 512 palabras con 0xFFFF en lo que no se toca: es solo la forma de pasarle el trabajo al puerto, que
 * en el teléfono (hal/nor.c) programa palabra a palabra las que no son 0xFFFF.
 * [sin saber] en un M18 de verdad: los tiempos, y si admite todo lo que aquí se le pide. */
#ifndef M18_H
#define M18_H
#define M18_BLOQUE   0x40000u          /* bloque de borrado físico */
#define M18_REGION   0x400u            /* región de programación = búfer de 512 palabras */
#define M18_LOGICO   (M18_BLOQUE / 2)  /* lo que ve quien usa la capa: 128 KiB por bloque */
#define M18_LOG_REG  (M18_REGION / 2)  /* bytes lógicos por región: 512 */

/* desplazamiento lógico (0..128 KiB) -> físico (0..256 KiB) dentro del bloque: la fórmula del original */
static inline unsigned m18_fis(unsigned logico) { return ((logico * 2) & ~0x1Fu) + (logico & 0xFu); }

/* Arma el búfer de una región (512 palabras, todo 0xFFFF salvo lo nuevo): «n» bytes de «d» que empiezan en el
 * desplazamiento lógico «en» DENTRO de la región (en + n <= 512). */
static inline void m18_armar(unsigned short buf[512], unsigned en, const unsigned char *d, unsigned n)
{
    unsigned i;
    unsigned char *b = (unsigned char *)buf;
    for (i = 0; i < 512; i++) buf[i] = 0xFFFF;
    for (i = 0; i < n; i++) b[m18_fis(en + i)] = d[i];
}

/* PROGRAMAR en lógico: «n» bytes de «d» en el desplazamiento lógico «desplaz» de un bloque, cuyo ESPEJO (lo que
 * el chip tiene, visto en lógico) es «esp». Reparte en regiones, no manda nada si no cambia nada, y deja el
 * espejo con lo que quedó DE VERDAD en el chip (lo relee), haya ido bien o mal. Es el MISMO código en el
 * teléfono (hal/nor.c pone el puerto con las rutinas de RAM) y en el PC (plat/pc_nor.c pone un modelo estricto
 * del modo control con cortes de alimentación: «make probar-m18»). */
typedef struct {
    void *ctx;
    int (*programar_region)(void *ctx, unsigned off_fis, const unsigned short *buf);   /* la región de 1 KiB que empieza en off_fis del bloque; 0 = bien */
    const unsigned char *(*region)(void *ctx, unsigned off_fis);                       /* esa región tal como está en el chip, para releer */
} M18Puerto;
enum { M18_E_RANGO = 1, M18_E_BITS, M18_E_CHIP, M18_E_VERIFICA };
static inline int m18_programar(const M18Puerto *p, unsigned char *esp, unsigned desplaz, const unsigned char *d, unsigned n,
                                unsigned short buf[512], unsigned *programas)
{
    unsigned hecho = 0;
    if ((desplaz & 1) || desplaz >= M18_LOGICO || n > M18_LOGICO - desplaz) return -M18_E_RANGO;
    while (hecho < n) {
        unsigned l = desplaz + hecho, en = l & (M18_LOG_REG - 1), trozo = M18_LOG_REG - en, i, cambia = 0;
        unsigned off = (l / M18_LOG_REG) * M18_REGION;
        if (trozo > n - hecho) trozo = n - hecho;
        for (i = 0; i < trozo; i++) {
            if ((esp[l + i] & d[hecho + i]) != d[hecho + i]) return -M18_E_BITS;       /* habría que subir un bit */
            if (esp[l + i] != d[hecho + i]) cambia = 1;
        }
        if (cambia) {
            const unsigned char *f; int r;
            m18_armar(buf, en, d + hecho, trozo);
            r = p->programar_region(p->ctx, off, buf);
            if (programas) ++*programas;
            f = p->region(p->ctx, off);
            for (i = 0; i < trozo; i++) esp[l + i] = f[m18_fis(en + i)];
            if (r) return -M18_E_CHIP;
            for (i = 0; i < trozo; i++) if (esp[l + i] != d[hecho + i]) return -M18_E_VERIFICA;
        }
        hecho += trozo;
    }
    return 0;
}
#endif
