/* bt_indices.c — lee BT_PatchInfo.txt / BT_FactorysettingsInfo.txt de /ifs/corrections (docs/bluetooth.md §1, §4
 * paso 7–8) y dice qué fichero toca a (HCI_Revision, LMP_Subversion). Sin libc: lo usa igual el teléfono (con el
 * texto leído por fsfab) que la prueba del PC.
 *
 * Formato [observado]: líneas «<CHIP_ID> <HCI_rev> <LMP_sub> <fichero>» en hexadecimal «0x…», separadas por
 * espacios o tabuladores; «#» = comentario. CHIP_ID es 0 para ST y no se mira. */
#include <stdint.h>

static const char *salta_blancos(const char *p, const char *fin)
{
    while (p < fin && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static const char *hex(const char *p, const char *fin, uint32_t *v)
{
    uint32_t x = 0; int dig = 0;
    p = salta_blancos(p, fin);
    if (p + 1 < fin && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    for (; p < fin; p++, dig++) {
        char c = *p; unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        x = x * 16 + d;
    }
    *v = x;
    return dig ? p : 0;
}

/* Devuelve 1 y copia el nombre (hasta max-1) si hay línea para (rev, sub). */
int bt_indice_buscar(const char *txt, unsigned n, uint16_t rev, uint16_t sub, char *nombre, unsigned max)
{
    const char *p = txt, *fin = txt + n;
    while (p < fin) {
        const char *eol = p, *q;
        uint32_t chip, r, s;
        while (eol < fin && *eol != '\n' && *eol != '\r') eol++;
        q = salta_blancos(p, eol);
        if (q < eol && *q != '#' && (q = hex(q, eol, &chip)) && (q = hex(q, eol, &r)) && (q = hex(q, eol, &s))
            && r == rev && s == sub) {
            unsigned k = 0;
            q = salta_blancos(q, eol);
            while (q < eol && *q != ' ' && *q != '\t' && k + 1 < max) nombre[k++] = *q++;
            nombre[k] = 0;
            return k > 0;
        }
        p = eol + 1;
    }
    return 0;
}
