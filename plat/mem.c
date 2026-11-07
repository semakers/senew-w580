/* Las rutinas de memoria que piden el compilador y la UI (en el PC las pone la libc). Nuestras. */
#include "ui.h"

/* (pista O) memcpy y memset: antes, palabra a palabra SOLO si las dos direcciones eran múltiplo de 4 (una fila de píxeles
 * que empieza en columna impar iba byte a byte). Ahora: si origen y destino tienen la misma alineación, se avanza hasta
 * el múltiplo de 4 y se copia de 32 en 32 bytes; si difieren en 2 (dos filas de píxeles), de 16 en 16 bits. */
void *memcpy(void *d, const void *s, size_t n)
{
    u8 *a = d; const u8 *b = s;
    if (n >= 8) {
        const size_t dif = ((size_t)a ^ (size_t)b) & 3;
        if (dif == 0) {
            u32 *x; const u32 *y;
            while ((size_t)a & 3) { *a++ = *b++; n--; }
            x = (u32 *)(void *)a; y = (const u32 *)(const void *)b;
            for (; n >= 32; n -= 32, x += 8, y += 8) {
                const u32 p = y[0], q = y[1], r = y[2], t = y[3], u = y[4], v = y[5], w = y[6], z = y[7];
                x[0] = p; x[1] = q; x[2] = r; x[3] = t; x[4] = u; x[5] = v; x[6] = w; x[7] = z;
            }
            for (; n >= 4; n -= 4) *x++ = *y++;
            a = (u8 *)x; b = (const u8 *)y;
        } else if (dif == 2) {
            u16 *x; const u16 *y;
            if ((size_t)a & 1) { *a++ = *b++; n--; }
            x = (u16 *)(void *)a; y = (const u16 *)(const void *)b;
            for (; n >= 8; n -= 8, x += 4, y += 4) { const u16 p = y[0], q = y[1], r = y[2], t = y[3]; x[0] = p; x[1] = q; x[2] = r; x[3] = t; }
            a = (u8 *)x; b = (const u8 *)y;
        }
    }
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    u8 *a = d; const u8 *b = s;
    if (a <= b || a >= b + n) return memcpy(d, s, n);
    while (n--) a[n] = b[n];
    return d;
}
void *memset(void *d, int c, size_t n)
{
    u8 *a = d;
    if (n >= 8) {
        u32 *x; const u32 v = (u8)c * 0x01010101u;
        while ((size_t)a & 3) { *a++ = (u8)c; n--; }
        x = (u32 *)(void *)a;
        for (; n >= 32; n -= 32, x += 8) { x[0] = v; x[1] = v; x[2] = v; x[3] = v; x[4] = v; x[5] = v; x[6] = v; x[7] = v; }
        for (; n >= 4; n -= 4) *x++ = v;
        a = (u8 *)x;
    }
    while (n--) *a++ = (u8)c;
    return d;
}
int memcmp(const void *x, const void *y, size_t n)
{
    const u8 *a = x, *b = y;
    for (; n--; a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
