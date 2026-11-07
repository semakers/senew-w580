/* Memoria sin recolector: una arena por cuadro (se vacía entera tras reconciliar) y un montículo
 * propio de primer ajuste con fusión para lo que vive más de un cuadro (elementos, estado). */
#include "ui.h"
#include <stdarg.h>

#ifndef UI_ARENA_TAM
#define UI_ARENA_TAM (32 * 1024)          /* (MD) antes 24: la rejilla de apps + 5 rutas + el aviso del cargador la agotaban (17_energia_s7, clásico) */
#endif
#ifndef UI_MONTICULO_TAM
#define UI_MONTICULO_TAM (512 * 1024)
#endif

typedef size_t pal;                     /* palabra de la máquina: 4 bytes en el teléfono, 8 en el PC */
#define PAL sizeof(pal)
static pal arena_mem[UI_ARENA_TAM / sizeof(pal)];
static u32 arena_pos;

void *ui_arena(size_t n)
{
    void *p;
    n = (n + PAL - 1) & ~(PAL - 1);
    if (arena_pos + n > UI_ARENA_TAM) { arena_pos = 0; plat_panico("ARENA DEL CUADRO AGOTADA"); for (;;) ; }
    p = (u8 *)arena_mem + arena_pos;
    arena_pos += (u32)n;
    if (arena_pos > ui_med.arena_pico) ui_med.arena_pico = arena_pos;
    return p;
}
void ui_arena_vaciar(void) { arena_pos = 0; ui_med.arena_tam = UI_ARENA_TAM; }
u32  ui_arena_libre(void) { return UI_ARENA_TAM - arena_pos; }     /* (C) el cargador de apps no deja que una app la agote */
void ui_arena_tramo(const void **ini, u32 *tam) { *ini = arena_mem; *tam = UI_ARENA_TAM; }   /* (PM) la arena se abre de solo lectura a una app cargable */
int  ui_en_arena(const void *p) { return (const u8 *)p >= (const u8 *)arena_mem && (const u8 *)p < (const u8 *)arena_mem + UI_ARENA_TAM; }

void *ui_nuevo(const void *plantilla, size_t tam) { return memcpy(ui_arena(tam), plantilla, tam); }

Widget **ui_lista(Widget *const *v, size_t n)
{
    Widget **l = ui_arena((n + 1) * sizeof *l);
    size_t i, m = 0;
    for (i = 0; i < n; i++) if (v[i]) l[m++] = v[i];
    l[m] = NULL;
    return l;
}

/* printf mínimo a la arena: %d %u %x %X %s %c %%, con «0» y ancho (%02d). Hexadecimal siempre en mayúsculas. */
char *ui_fmt(const char *f, ...)
{
    char tmp[320]; unsigned n = 0;                        /* (S6) 96 -> 320: los avisos de Mis archivos son frases enteras; el resultado va a la arena con su tamaño justo */
    va_list ap;
    va_start(ap, f);
    for (; *f && n < sizeof tmp - 12; f++) {
        int cero = 0, ancho = 0;
        if (*f != '%') { tmp[n++] = *f; continue; }
        f++;
        if (*f == '0') { cero = 1; f++; }
        while (*f >= '0' && *f <= '9') ancho = ancho * 10 + (*f++ - '0');
        if (!*f) break;                                   /* (R3; calidad) un formato que ACABA en «%» (o «%0», «%12»): antes se leía más allá del cero final */
        if (*f == 's') { const char *s = va_arg(ap, const char *); while (*s && n < sizeof tmp - 12) tmp[n++] = *s++; }
        else if (*f == 'c') tmp[n++] = (char)va_arg(ap, int);
        else if (*f == '%') tmp[n++] = '%';
        else {
            char d[11]; int k = 0, neg = 0; unsigned base = (*f == 'x' || *f == 'X') ? 16 : 10;
            u32 v = va_arg(ap, u32);
            if (*f == 'd' && (i32)v < 0) { neg = 1; v = 0u - v; }   /* (R3; calidad) negar en sin signo: -(i32) del mínimo era indefinido */
            do { d[k++] = "0123456789ABCDEF"[v % base]; v /= base; } while (v);
            if (neg && cero) { tmp[n++] = '-'; ancho--; }  /* (R3; calidad) con relleno de ceros el signo va DELANTE: «-0042», no «00-42» */
            else if (neg) d[k++] = '-';
            for (; ancho > k && n < sizeof tmp - 12; ancho--) tmp[n++] = cero ? '0' : ' ';   /* (R3) y el relleno, acotado: «%400d» no se sale de tmp */
            while (k) tmp[n++] = d[--k];
        }
    }
    va_end(ap);
    tmp[n++] = 0;
    return memcpy(ui_arena(n), tmp, n);
}

/* ---- montículo: bloques contiguos {tamaño en palabras | bit 31 = libre}; primer ajuste ------ */
static pal mo_mem[UI_MONTICULO_TAM / sizeof(pal)];
static u8 mo_listo;
static pal mo_pista;                    /* ningún bloque libre empieza antes de aquí */
#define LIBRE ((pal)0x80000000u)
#define FIN (UI_MONTICULO_TAM / PAL)

void *mo_probar(size_t n)                /* como mo_pedir, pero devuelve NULL si no hay sitio */
{
    pal pide = (n + PAL - 1) / PAL + 1, i;                 /* +1: la cabecera */
    if (!mo_listo) { mo_mem[0] = FIN | LIBRE; mo_listo = 1; ui_med.monticulo_tam = UI_MONTICULO_TAM; }
    for (i = mo_pista; i < FIN; i += mo_mem[i] & ~LIBRE) {
        pal t = mo_mem[i] & ~LIBRE;
        /* (R2; auditoría del sistema, M2) el montículo no se fiaba de nada: una cabecera a 0 (el desborde de un vecino por una
         * palabra) dejaba este bucle sin avanzar —un cuelgue—, y una con basura lo sacaba de mo_mem. Ahora es un fallo DICHO. */
        if (!t || t > FIN - i) { plat_panico("MONTICULO CORRUPTO"); for (;;) ; }
        if (!(mo_mem[i] & LIBRE)) { if (i == mo_pista) mo_pista = i + (mo_mem[i] & ~LIBRE); continue; }
        while (i + t < FIN && (mo_mem[i + t] & LIBRE)) {                                                         /* fusiona */
            pal v = mo_mem[i + t] & ~LIBRE;
            if (!v || v > FIN - (i + t)) { plat_panico("MONTICULO CORRUPTO"); for (;;) ; }
            t += v; mo_mem[i] = t | LIBRE;
        }
        if (t < pide) continue;
        if (t - pide >= 3) { mo_mem[i + pide] = (t - pide) | LIBRE; t = pide; }
        mo_mem[i] = t;
        ui_med.monticulo_usado += (u32)(t * PAL); ui_med.bloques++;
        if (ui_med.monticulo_usado > ui_med.monticulo_pico) ui_med.monticulo_pico = ui_med.monticulo_usado;
        return memset(&mo_mem[i + 1], 0, (t - 1) * PAL);
    }
    return NULL;
}
void *mo_pedir(size_t n)                 /* para lo que no puede faltar (elementos): si no hay, para el sistema */
{
    void *p = mo_probar(n);
    if (p) return p;
    plat_panico("MONTICULO AGOTADO");
    for (;;) ;
}

void mo_soltar(void *p)
{
    pal *c;
    if (!p) return;
    c = (pal *)p - 1;
    /* (R2; M2) soltar lo que no es un bloque vivo del montículo (un puntero ajeno, un doble mo_soltar) descuadraba las cuentas
     * y podía partir un bloque vivo: ahora se para y se dice. */
    if (c < mo_mem || c >= mo_mem + FIN || ((size_t)((u8 *)c - (u8 *)mo_mem) % PAL) || (*c & LIBRE) || !*c || *c > (pal)(mo_mem + FIN - c)) {
        plat_panico("MONTICULO: SE SUELTA LO QUE NO ES UN BLOQUE VIVO"); for (;;) ;
    }
    ui_med.monticulo_usado -= (u32)(*c * PAL); ui_med.bloques--;
    *c |= LIBRE;
    if ((pal)(c - mo_mem) < mo_pista) mo_pista = (pal)(c - mo_mem);
}
