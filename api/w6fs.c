/* Lector de W6FS (formato en w6fs.h). Portable: el teléfono lo usa sobre la NOR y el PC sobre un fichero. */
#include "w6fs.h"
#include "almacen.h"                               /* alm_crc32 */

#define ENT 20u
static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static const u8 *ent(const W6fs *fs, u32 i) { return fs->base + fs->tabla + i * ENT; }

int w6fs_montar(W6fs *fs, const u8 *b, u32 max)
{
    u32 i, n, tam, tabla, nombres;
    fs->base = 0; fs->n = 0;
    if (max < 32 || b[0] != 'W' || b[1] != '6' || b[2] != 'F' || b[3] != 'S' || le32(b + 4) != 1) return 0;
    if (le32(b + 28) != alm_crc32(0, b, 28)) return 0;
    n = le32(b + 8); tam = le32(b + 12); tabla = le32(b + 16); nombres = le32(b + 20);
    if (tam > max || tabla != 32 || n < 1 || n > 65535 || nombres != tabla + n * ENT || nombres >= tam) return 0;
    {   /* la zona de nombres acaba donde empieza el primer dato (o en el fin de la imagen) */
        u32 fin = tam;
        for (i = 0; i < n; i++) { const u8 *e = b + tabla + i * ENT; if (le32(e + 16) == 0 && le32(e + 4) < fin) fin = le32(e + 4); }
        if (fin <= nombres) return 0;
        if (le32(b + 24) != alm_crc32(0, b + tabla, fin - tabla)) return 0;
        fs->fin_nombres = fin;
    }
    for (i = 0; i < n; i++) {                       /* todos los rangos, una vez */
        const u8 *e = b + tabla + i * ENT; u32 nom = le32(e), a = le32(e + 4), c = le32(e + 8), tipo = le32(e + 16), k;
        if (nom >= fs->fin_nombres - nombres) return 0;
        for (k = nombres + nom; k < fs->fin_nombres && b[k]; k++) ;
        if (k >= fs->fin_nombres) return 0;         /* nombre sin terminar */
        if (tipo == 1) { if (a > n || c > n - a || (c && a <= i)) return 0; }      /* los hijos van después del padre: no hay ciclos */
        else if (tipo == 0) { if (a > tam || c > tam - a) return 0; }
        else return 0;
    }
    if (le32(b + tabla + 16) != 1) return 0;        /* la raíz es un directorio */
    fs->base = b; fs->n = n; fs->tam = tam; fs->tabla = tabla; fs->nombres = nombres;
    return 1;
}
static const char *nombre(const W6fs *fs, u32 i) { return (const char *)fs->base + fs->nombres + le32(ent(fs, i)); }
int w6fs_es_dir(const W6fs *fs, int i) { return i >= 0 && (u32)i < fs->n && le32(ent(fs, (u32)i) + 16) == 1; }
u32 w6fs_tam(const W6fs *fs, int i) { return i >= 0 && (u32)i < fs->n ? le32(ent(fs, (u32)i) + 8) : 0; }
const u8 *w6fs_datos(const W6fs *fs, int i) { return i >= 0 && (u32)i < fs->n && !w6fs_es_dir(fs, i) ? fs->base + le32(ent(fs, (u32)i) + 4) : 0; }
int w6fs_comprobar(const W6fs *fs, int i)
{
    const u8 *d = w6fs_datos(fs, i);
    return d && alm_crc32(0, d, w6fs_tam(fs, i)) == le32(ent(fs, (u32)i) + 12);
}
int w6fs_buscar(const W6fs *fs, const char *ruta)
{
    u32 dir = 0;
    if (!fs->base || !ruta || ruta[0] != '/') return -1;
    for (;;) {
        u32 k, largo = 0, a, c, hallado = 0;
        while (*ruta == '/') ruta++;
        if (!*ruta) return (int)dir;
        while (ruta[largo] && ruta[largo] != '/') largo++;
        if (le32(ent(fs, dir) + 16) != 1) return -1;
        a = le32(ent(fs, dir) + 4); c = le32(ent(fs, dir) + 8);
        for (k = a; k < a + c; k++) {
            const char *nm = nombre(fs, k); u32 j = 0;
            while (j < largo && nm[j] && nm[j] == ruta[j]) j++;
            if (j == largo && !nm[j]) { dir = k; hallado = 1; break; }
        }
        if (!hallado) return -1;
        ruta += largo;
    }
}
int w6fs_listar(const W6fs *fs, int dir, u32 desde, HwEntrada *e, u32 max)
{
    u32 a, c, k, n = 0;
    if (!w6fs_es_dir(fs, dir)) return HW_ERROR;
    a = le32(ent(fs, (u32)dir) + 4); c = le32(ent(fs, (u32)dir) + 8);
    if (!e) return (int)c;
    for (k = desde; k < c && n < max; k++, n++) {
        const char *nm = nombre(fs, a + k); u32 j = 0;
        while (nm[j] && j < sizeof e[n].nombre - 1) { e[n].nombre[j] = nm[j]; j++; }
        while (j && nm[j] && ((u8)nm[j] & 0xC0) == 0x80) j--;                 /* no cortar un carácter UTF-8 */
        e[n].nombre[j] = 0;
        e[n].es_dir = (u8)w6fs_es_dir(fs, (int)(a + k)); e[n].tam = e[n].es_dir ? 0 : w6fs_tam(fs, (int)(a + k));
    }
    return (int)n;
}
