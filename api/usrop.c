/* usrop.c — copiar por trozos y renombrar en la zona de usuario, SOLO con la API de usuario.h. Interfaz,
 * garantías y por qué está aparte: usrop.h. Pista S6 del W580. Portable: ni UI ni hardware. */
#include "usrop.h"

void *memset(void *d, int c, __SIZE_TYPE__ n);

#define USRO_TROZO 4096u               /* lo que se pide a la fuente de cada vez (= el trozo mayor de W6U) */
static u8 tr[USRO_TROZO];
static UsroOp *en_marcha;              /* la operación que tiene abierta la escritura a trozos de usuario.c */

static int copiar_nombre(char *d, const char *s)   /* 1 si cabe (1..USR_MAX_NOMBRE bytes, sin «/») */
{
    u32 n = 0;
    if (!s) return 0;
    for (; s[n]; n++) { if (n >= USR_MAX_NOMBRE || s[n] == '/') return 0; d[n] = s[n]; }
    d[n] = 0;
    return n > 0;
}
static int iguales(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

int usro_copiar(UsroOp *o, const char *destino, u32 tam, u32 fecha, int reemplazar, UsroFuente f, void *dato)
{
    int e;
    if (!o || !f) return USR_E_ARG;
    if (en_marcha) return USRO_E_OCUPADA;
    memset(o, 0, sizeof *o);
    if (!copiar_nombre(o->destino, destino)) return USR_E_ARG;
    e = usr_info(o->destino, 0);
    if (e == USR_OK && !reemplazar) return USRO_E_EXISTE;
    if (e != USR_OK && e != USR_E_NO_HAY) return e;         /* sin montar */
    if ((e = usr_empezar(o->destino, tam, fecha))) return e;   /* LLENO, MUCHOS, solo lectura: aquí, sin haber escrito nada */
    o->tam = tam; o->fuente = f; o->dato = dato; o->activa = 1;
    en_marcha = o;
    return USR_OK;
}

static int del_viejo(void *dato, u32 desde, u8 *buf, u32 n) { return usr_leer(((UsroOp *)dato)->origen, desde, buf, n); }
int usro_renombrar(UsroOp *o, const char *viejo, const char *nuevo)
{
    char v[USR_MAX_NOMBRE + 1];
    UsrEntrada en;
    int e;
    if (!o) return USR_E_ARG;
    if (en_marcha) return USRO_E_OCUPADA;
    if (!copiar_nombre(v, viejo)) return USR_E_ARG;
    if ((e = usr_info(v, &en)) != USR_OK) return e;
    if (nuevo && iguales(v, nuevo)) return USRO_E_MISMO;
    if ((e = usro_copiar(o, nuevo, en.tam, en.fecha, 0, del_viejo, o))) return e;   /* nunca encima de otro: USRO_E_EXISTE */
    copiar_nombre(o->origen, v);
    o->renombra = 1;
    return USR_OK;
}

static int fin(UsroOp *o, int e)
{
    if (o->activa && e && en_marcha == o) usr_cancelar();   /* (si el error vino de usuario.c ya lo canceló él: no estorba) */
    o->activa = 0;
    if (en_marcha == o) en_marcha = 0;
    return e;
}
void usro_cancelar(UsroOp *o) { if (o && o->activa) fin(o, USR_E_ARG); }

int usro_paso(UsroOp *o, u32 max)
{
    u32 hecho_aqui = 0;
    int e;
    if (!o || !o->activa || en_marcha != o) return USR_E_ARG;
    while (o->hecho < o->tam && (hecho_aqui < max || !hecho_aqui)) {
        u32 n = o->tam - o->hecho < USRO_TROZO ? o->tam - o->hecho : USRO_TROZO;
        if (o->fuente(o->dato, o->hecho, tr, n) != (int)n) return fin(o, USRO_E_FUENTE);
        if ((e = usr_mas(tr, n))) return fin(o, e);
        o->hecho += n; hecho_aqui += n;
    }
    if (o->hecho < o->tam) return USRO_SIGUE;
    /* EL COMPROMISO de la copia: hasta que usr_acabar vuelve bien, el destino vale lo de antes */
    if ((e = usr_acabar())) return fin(o, e);
    /* …y SOLO AHORA, con la copia ya comprometida, se borra el viejo. Un corte entre los dos deja los dos. */
    if (o->renombra && (e = usr_borrar(o->origen))) { o->activa = 0; en_marcha = 0; return e; }
    return fin(o, USR_OK);
}
