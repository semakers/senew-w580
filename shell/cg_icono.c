/* cg_icono.c — (ICO) la MALLA 3D del menú que trae una .sen en su sección de icono (api/cargable.h, W5xIcoMalla).
 * Es DATO DE FUERA (W6FS, Memoria, Mis archivos, tarjeta): no se fía de nada. Se lee el pie (12 B al final del fichero), se
 * acota, se lee la sección ENTERA a un búfer del sistema (como mucho W5X_ICO_MAX), se comprueba su CRC y se valida campo a
 * campo: tamaño exacto, contadores en rango, cada índice de triángulo menor que su tabla. Solo entonces se da una AppMalla
 * que apunta DENTRO de ese búfer (el pintor de shell/m3d_dibujo.c confía en los índices: por eso se miran todos aquí).
 * La protección de memoria no interviene: es el sistema el que lee el fichero. Fuzz: herr/fuzz_icono.c (PC, ASan). */
#include "cargador.h"
#include "app_externa.h"
#include <string.h>

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* La sección ya en memoria (sin el pie): 0 y *m apuntando dentro de `s` si vale; -1 si no (y *m a cero). */
int cg_ico_validar(const u8 *s, u32 tam, AppMalla *m)
{
    u32 i, ntri = 0, esperado;
    u8 npos, nnor;
    const u8 *t;
    memset(m, 0, sizeof *m);
    if (!s || tam < sizeof(W5xIcoMalla) || tam > W5X_ICO_MAX) return -1;
    if (s[0] != 1 || s[3] != 0 || s[23] || s[24] || s[25]) return -1;            /* versión 1 y lo reservado a cero */
    npos = s[1]; nnor = s[2];
    if (npos < 3 || nnor < 1) return -1;
    for (i = 0; i < 5; i++) ntri += le16(s + 4 + 2 * i);                          /* cada uno <= 65535: la suma no desborda */
    if (!ntri || ntri > W5X_ICO_TRI_MAX) return -1;
    esperado = (u32)sizeof(W5xIcoMalla) + 3u * npos + 3u * nnor + 6u * ntri;      /* <= 26 + 765 + 765 + 1536 */
    if (esperado != tam) return -1;
    t = s + sizeof(W5xIcoMalla) + 3u * npos + 3u * nnor;
    for (i = 0; i < ntri; i++, t += 6)
        if (t[0] >= npos || t[1] >= npos || t[2] >= npos || t[3] >= nnor || t[4] >= nnor || t[5] >= nnor) return -1;
    m->pos = (const signed char *)(s + sizeof(W5xIcoMalla));
    m->nor = (const signed char *)(s + sizeof(W5xIcoMalla) + 3u * npos);
    m->tri = s + sizeof(W5xIcoMalla) + 3u * npos + 3u * nnor;
    m->npos = npos; m->nnor = nnor;
    for (i = 0; i < 5; i++) m->nt[i] = le16(s + 4 + 2 * i);
    memcpy(m->natural, s + 14, 9);
    return 0;
}

/* El pie y la sección de un fichero cuya cabecera YA validó cg_cab_vale (tam_fich, tam_rec cuadran con el fichero).
 * `buf` tiene W5X_ICO_MAX bytes. 1 = trae malla y vale (*m dentro de buf); 0 = no trae; -1 = trae una que no vale. */
int cg_ico_leer(const char *ruta, const W5xCab *c, u8 *buf, AppMalla *m)
{
    u8 p[sizeof(W5xIcoPie)];
    u32 tam;
    memset(m, 0, sizeof *m);
    if (!ruta || !c || c->tam_rec < sizeof p || c->tam_fich < W5X_TAM_CAB + sizeof p) return 0;
    if (hw_fs_leer_desde(ruta, c->tam_fich - (u32)sizeof p, p, sizeof p) != sizeof p) return 0;
    if (memcmp(p + 8, W5X_ICO_MAGIA, 4)) return 0;                               /* sin pie: no trae (lo normal) */
    tam = le32(p);
    if (tam < sizeof(W5xIcoMalla) || tam > W5X_ICO_MAX || tam > c->tam_rec - (u32)sizeof p) return -1;   /* dentro de los recursos */
    if (hw_fs_leer_desde(ruta, c->tam_fich - (u32)sizeof p - tam, buf, tam) != tam) return -1;
    if (cg_crc(0, buf, tam) != le32(p + 4)) return -1;
    return cg_ico_validar(buf, tam, m) ? -1 : 1;
}
