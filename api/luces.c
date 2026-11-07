/* luces.c — (pista S5) el motor de luces portable: intérprete de «.ble», fundidos por pasos y reproductor.
 * Qué es cada cosa y de dónde sale: api/luces.h y docs/luces.md (pista L). Cada función cita la del
 * firmware original que imita; donde NO se imita se dice (siempre para no colgarse o no leer fuera). */
#include "luces.h"

/* ============================== 1. EL INTÉRPRETE (FUN_45391f48) ================================================= */
static const struct { u8 op, n; const char *nombre; } ble_tabla[] = {
    { BLE_NOP, 0, "NOP" }, { BLE_FIN, 0, "FIN" }, { BLE_PONER, 2, "PONER" }, { BLE_ESPERAR, 1, "ESPERAR" },
    { BLE_FUNDIR, 3, "FUNDIR" }, { BLE_SALTAR, 2, "SALTAR" }, { BLE_RGB_FUNDIR, 6, "RGB_FUNDIR" },
    { BLE_RGB_PONER, 5, "RGB_PONER" }, { BLE_RGB_APAGAR, 1, "RGB_APAGAR" }, { BLE_SI_ABIERTO, 1, "SI_ABIERTO" },
    { BLE_FIN_SI, 1, "FIN_SI" }, { BLE_LED_PARPADEAR, 3, "LED_PARPADEAR" }, { BLE_LED_APAGAR, 1, "LED_APAGAR" } };
const char *ble_nombre(u32 op, u32 *n_args)
{
    u32 i;
    for (i = 0; i < sizeof ble_tabla / sizeof ble_tabla[0]; i++)
        if (ble_tabla[i].op == op) { if (n_args) *n_args = ble_tabla[i].n; return ble_tabla[i].nombre; }
    return 0;
}
/* ms pedidos -> código de velocidad (bits 3..5 del registro 0x16 del chip). Es la escalera del original, tal cual. */
u32 ble_velocidad(int ms)
{
    if (ms < 1) return 2;
    if (ms < 0x33) return 6;
    if (ms < 0x60) return 7;
    if (ms < 0x65) return 4;
    if (ms < 0xBF) return 5;
    if (ms < 0x1F5) return 2;
    if (ms < 0x3B7) return 3;
    if (ms < 0x3E9) return 0;
    return 1;
}
/* [inferido] (pista L): la duración nominal de cada código es el extremo de su tramo de la escalera. */
u32 ble_rampa_ms(u32 vel)
{
    static const u16 ms[8] = { 1000, 1900, 500, 950, 100, 190, 50, 95 };
    return ms[vel & 7];
}
void ble_preparar(Ble *b, const u32 *palabras, u32 n, u32 intensidad, const LuOps *ops, void *ctx)
{
    b->p = palabras; b->n = palabras ? n : 0; b->pc = 0; b->fin = BLE_CORRE; b->en_bucle = 0; b->cuenta = 0;
    b->intensidad = (u8)(intensidad > 100 ? 100 : intensidad);
    b->con_rgb = 1; b->con_base = 0; b->base[0] = b->base[1] = b->base[2] = 0;
    b->ops = ops; b->ctx = ctx;
}
void ble_color_base(Ble *b, u32 r, u32 g, u32 bl)
{
    b->base[0] = (u8)(r > 100 ? 100 : r); b->base[1] = (u8)(g > 100 ? 100 : g); b->base[2] = (u8)(bl > 100 ? 100 : bl);
    b->con_base = (u8)(b->base[0] || b->base[1] || b->base[2]);
}
static u32 pct_rgb(u32 v) { int s = (int)v; return s < 0 ? 0u : s > 100 ? 100u : (u32)s; }   /* el original los toma con signo y recorta a 0..100 */
int ble_correr(Ble *b)
{
    int ejecutar = 1;                /* el tramo SI_ABIERTO: como en el original, es LOCAL a cada tanda (tras una espera vuelve a 1) */
    u32 seguidas = 0;
    if (!b || !b->p || !b->ops) return 0;
    while (b->fin == BLE_CORRE) {
        u32 palabra = b->pc, op, na = 0, a[6] = { 0, 0, 0, 0, 0, 0 }, i;
        const LuOps *o = b->ops;
        if (b->pc >= b->n) { b->fin = BLE_ACABADO; break; }        /* se salió por el final: acabado (mt_call no lleva FIN) */
        if (++seguidas > BLE_MAX_SEGUIDAS) { b->fin = BLE_E_SIN_ESPERA; break; }   /* NUESTRO: un bucle sin esperas colgaría el teléfono */
        op = b->p[b->pc++] & 0xFF;                                  /* el código es el byte bajo de la palabra */
        if (!ble_nombre(op, &na)) { b->fin = BLE_E_ORDEN; break; }
        if (na > b->n - b->pc) { b->fin = BLE_E_CORTADA; break; }   /* NUESTRO: el original la ejecutaría con ceros */
        for (i = 0; i < na; i++) a[i] = b->p[b->pc++];
        if (o->traza) o->traza(b->ctx, palabra, op, a, na);
        switch (op) {
        case BLE_NOP: break;
        case BLE_FIN: b->fin = BLE_ACABADO; break;
        case BLE_PONER:                                             /* FUN_4539258c */
            if (ejecutar && o->poner) o->poner(b->ctx, a[0], a[1] && b->intensidad ? ((a[1] > 255 ? 255 : a[1]) * b->intensidad / 100) & 0xFF : 0);
            break;
        case BLE_ESPERAR:
            if ((int)a[0] < 1) { b->fin = BLE_ACABADO; break; }     /* el llamador del original trata «< 1» como fin */
            return (int)a[0];
        case BLE_FUNDIR:                                            /* FUN_453925cc: ritmo = 100000 / ms (16 bits) */
            if (ejecutar && o->fundir && a[2])                      /* (ms = 0: el original divide por cero; aquí no se hace nada) */
                o->fundir(b->ctx, a[0], ((a[1] > 255 ? 255 : a[1]) * b->intensidad / 100) & 0xFF, (100000u / a[2]) & 0xFFFF);
            break;
        case BLE_SALTAR: {                                          /* a[0] = palabra destino, a[1] = veces (0 = para siempre) */
            int c = b->cuenta;
            if (b->en_bucle == 1 && c >= 1) c--;
            else if (b->en_bucle == 1 && c == 0) b->en_bucle = 0;   /* ya dio todas las vueltas: sigue adelante */
            else if (a[1] == 0) { b->en_bucle = 1; c = -1; }
            else { b->en_bucle = 1; c = (int)a[1] - 1; }
            b->cuenta = c;
            if (b->en_bucle) {
                if (a[0] >= b->n) { b->fin = BLE_E_SALTO; break; }
                b->pc = a[0];
            }
            break; }
        case BLE_RGB_FUNDIR:                                        /* rgb, r, g, b, ms, subir */
            if (b->con_rgb && o->rgb_fundir) {
                u32 r = pct_rgb(a[1]), g = pct_rgb(a[2]), bl = pct_rgb(a[3]);
                if (b->con_base) { r = b->base[0]; g = b->base[1]; bl = b->base[2]; }
                o->rgb_fundir(b->ctx, (int)a[0] > 1, r, g, bl, ble_velocidad((int)a[4]), (int)a[5] < 1);
            }
            break;
        case BLE_RGB_PONER:                                         /* rgb, r, g, b, modo */
            if (b->con_rgb && o->rgb_poner) {
                u32 r = pct_rgb(a[1]), g = pct_rgb(a[2]), bl = pct_rgb(a[3]), m = (int)a[4] < 1 ? 0 : a[4] > 3 ? 3 : a[4];
                if (b->con_base) { r = b->base[0]; g = b->base[1]; bl = b->base[2]; }
                o->rgb_poner(b->ctx, (int)a[0] > 1, r, g, bl, m);
            }
            break;
        case BLE_RGB_APAGAR:
            if (b->con_rgb && o->rgb_apagar) o->rgb_apagar(b->ctx, (int)a[0] > 1);
            break;
        case BLE_SI_ABIERTO:
            if (a[0] == 1 && o->cerrado && o->cerrado(b->ctx)) ejecutar = 0;
            break;
        case BLE_FIN_SI:
            if (a[0] == 1) ejecutar = 1;
            break;
        case BLE_LED_PARPADEAR:
            if (o->led_pmu) o->led_pmu(b->ctx, a[0] & 0xFF, a[1], a[2] & 0xFFFF, 0);
            break;
        case BLE_LED_APAGAR:
            if (o->led_pmu) o->led_pmu(b->ctx, a[0] & 0xFF, 0, 0, 1);
            break;
        }
    }
    return 0;
}

/* ============================== 2. EL MOTOR ===================================================================== */
/* Una luz sencilla, con el estado del original (FUN_4529cf78: +0 objetivo, +1 lo escrito, +2 destino del fundido,
 * +3 el paso con signo). Niveles de registro. */
typedef struct { u8 obj, act, dest; signed char paso; } LuLuz;
static LuLuz lu_l[HW_LEDS];
static u8  lu_fundiendo;             /* hay algún fundido en marcha (el temporizador de 50 ms del original, 0x200090) */
static u32 lu_t50;                   /* hw_ms() del próximo paso */
LuCuentas lu_cta;

static u32 lu_nivel(u32 led, u32 pct) { return pct * (u32)hw_luz_salida_max((int)led) / 100; }   /* FUN_4529cffc con mínimo 0 */
void lu_sincronizar(void)
{
    u32 i;
    for (i = 0; i < HW_LEDS; i++) {
        int v = hw_luz_salida_nivel((int)i);
        lu_l[i].obj = lu_l[i].act = lu_l[i].dest = (u8)(v < 0 ? 0 : v);
        lu_l[i].paso = 0;
    }
    lu_fundiendo = 0;
}
int lu_poner(u32 led, u32 pct)
{
    LuLuz *l;
    if (led >= HW_LEDS || pct > 100) return HW_ERROR;
    l = &lu_l[led];
    if (!pct) {                                                     /* FUN_4529c810 */
        int habia = l->obj || l->paso;
        l->paso = 0;
        if (!habia) return HW_OK;
        l->obj = l->act = 0;
        return hw_luz_salida((int)led, 0);
    }
    l->paso = 0;                                                    /* corta el fundido que hubiera (FUN_4529d014) */
    l->obj = l->act = (u8)lu_nivel(led, pct);
    return hw_luz_salida((int)led, l->act);
}
static void lu_un_paso(u32 led)                                     /* FUN_4529cc90 */
{
    LuLuz *l = &lu_l[led];
    int act = l->act, dest = l->dest, nuevo;
    if (l->paso <= 0) {
        int n = -l->paso;
        nuevo = n <= act - dest ? act - n : dest;
        if (nuevo > dest) { l->act = (u8)nuevo; hw_luz_salida((int)led, nuevo); return; }
    } else {
        nuevo = l->paso <= dest - act ? act + l->paso : dest;
        if (nuevo < dest) { l->act = (u8)nuevo; hw_luz_salida((int)led, nuevo); return; }
    }
    l->paso = 0; l->act = (u8)dest;                                 /* llegó */
    if (!dest) l->obj = 0;
    hw_luz_salida((int)led, dest);                                  /* 0 = apagarla del todo (campo, nivel y, si era la última, el elevador) */
}
int lu_fundir(u32 led, u32 pct, u32 ritmo)
{
    LuLuz *l; int paso;
    if (led >= HW_LEDS || pct > 100 || !ritmo) return HW_ERROR;
    l = &lu_l[led];
    l->paso = 0;
    l->obj = l->dest = (u8)lu_nivel(led, pct);
    if (l->dest == l->act) return HW_OK;                            /* NUESTRO: el original, de apagada a 0, enciende y apaga el elevador; aquí, nada */
    paso = (int)(ritmo * (u32)hw_luz_salida_max((int)led) / 100 * 50 / 1000);
    if (paso < 1) paso = 1;
    if (paso > 127) paso = 127;                                     /* NUESTRO: en el original es un byte con signo y con ms < 100 se desborda */
    l->paso = (signed char)(l->dest < l->act ? -paso : paso);
    if (!lu_fundiendo) { lu_fundiendo = 1; lu_t50 = hw_ms() + 50; }
    lu_un_paso(led);                                                /* el primer paso, en el acto */
    return HW_OK;
}
static u32 a_registro(u32 pct) { return (pct > 100 ? 100 : pct) * 255 / 100; }
int lu_rgb_fundir(u32 terna, u32 r, u32 g, u32 b, u32 vel, u32 bajar)
{
    if (terna > 1 || (!r && !g && !b)) return HW_ERROR;             /* el original rechaza el negro (error 2) */
    return hw_luz_rgb_fundir((int)terna, (int)a_registro(r), (int)a_registro(g), (int)a_registro(b), (int)(vel & 7), bajar != 0);
}
int lu_rgb_poner(u32 terna, u32 r, u32 g, u32 b, u32 modo)
{
    if (terna > 1 || (!r && !g && !b) || modo < 1 || modo > 3) return HW_ERROR;
    return hw_luz_rgb_fijo((int)terna, (int)a_registro(r), (int)a_registro(g), (int)a_registro(b), (int)modo);
}
int lu_rgb_apagar(u32 terna) { return terna > 1 ? HW_ERROR : hw_luz_rgb_apagar((int)terna); }
u32 lu_pwm_modelo(u32 vel, int sentido, u32 ms)
{
    u32 d = ble_rampa_ms(vel), v;
    if (sentido != 1 && sentido != 2) return 255;
    v = ms >= d ? 255 : ms * 255 / d;
    return sentido == 1 ? v : 255 - v;
}

static void op_poner(void *c, u32 led, u32 pct) { (void)c; lu_cta.ordenes++; lu_poner(led, pct); }
static void op_fundir(void *c, u32 led, u32 pct, u32 ritmo) { (void)c; lu_cta.ordenes++; lu_fundir(led, pct, ritmo); }
static void op_rgb_fundir(void *c, u32 t, u32 r, u32 g, u32 b, u32 vel, u32 bajar) { (void)c; lu_cta.ordenes++; lu_rgb_fundir(t, r, g, b, vel, bajar); }
static void op_rgb_poner(void *c, u32 t, u32 r, u32 g, u32 b, u32 modo) { (void)c; lu_cta.ordenes++; lu_rgb_poner(t, r, g, b, modo); }
static void op_rgb_apagar(void *c, u32 t) { (void)c; lu_cta.ordenes++; lu_rgb_apagar(t); }
static int  op_cerrado(void *c) { (void)c; return hw_tapa_cerrada() == 1; }
/* Los LED del PMU (órdenes 0F / 10) NO se ejecutan: ningún efecto de fábrica las usa y [sin saber] si están montados. */
const LuOps lu_ops = { op_poner, op_fundir, op_rgb_fundir, op_rgb_poner, op_rgb_apagar, 0, op_cerrado, 0 };

/* ============================== 3. EL REPRODUCTOR =============================================================== */
static Ble ef;
static u8  ef_activo, ef_fin = BLE_ACABADO, ef_rgb = 1;   /* ef_activo: 1 = corriendo, 2 = programa acabado, esperando a que terminen sus fundidos */
static u32 ef_t;                     /* hw_ms() de la orden siguiente */
static u8  ef_base[3];
static u32 ef_buf[BLE_MAX_PALABRAS + 1];    /* + la palabra de versión, que se lee con el fichero */

void lu_color_base(u32 r, u32 g, u32 b) { ef_base[0] = (u8)(r > 100 ? 100 : r); ef_base[1] = (u8)(g > 100 ? 100 : g); ef_base[2] = (u8)(b > 100 ? 100 : b); }
void lu_rgb_permitir(int si) { ef_rgb = (u8)!!si; }
int  lu_efecto_activo(void) { return ef_activo; }
int  lu_efecto_fin(void) { return ef_fin; }
static void ef_soltar(void)
{
    u32 i;
    ef_activo = 0;
    for (i = 0; i < HW_LEDS; i++) lu_l[i].paso = 0;
    lu_fundiendo = 0;
    hw_luz_efecto(0);                                               /* la plataforma vuelve a montar su fase desde lo que haya escrito */
}
void lu_efecto_parar(void)
{
    if (!ef_activo) return;
    if (hw_luz_efecto(-1) > 0) { hw_luz_rgb_apagar(0); hw_luz_rgb_apagar(1); }   /* como «Stop» del original (FUN_453926a8) */
    ef_fin = BLE_ACABADO;
    ef_soltar();
}
int lu_efecto_memoria(const u32 *palabras, u32 n, u32 intensidad, int color)
{
    if (!palabras || !n) return HW_ERROR;
    if (ef_activo) lu_efecto_parar();                               /* «para lo que hubiera» */
    if (hw_luz_efecto(1) <= 0) return HW_NO_DISPONIBLE;             /* la plataforma no cede las luces (pantalla dormida, chip callado) */
    lu_sincronizar();
    ble_preparar(&ef, palabras, n, intensidad, &lu_ops, 0);
    ef.con_rgb = ef_rgb;
    if (color == LU_EF_BASE) ble_color_base(&ef, ef_base[0], ef_base[1], ef_base[2]);
    ef_activo = 1; ef_fin = BLE_CORRE; ef_t = hw_ms();
    lu_cta.efectos++;
    return HW_OK;
}
int lu_efecto_fichero(const char *ruta, u32 intensidad, int color)
{
    u8 *b = (u8 *)ef_buf; u32 tam, n, i, ver;
    if (!ruta || !hw_fs_listo()) return HW_NO_DISPONIBLE;
    tam = hw_fs_tam(ruta);
    if (!tam) return HW_NO_DISPONIBLE;
    if (tam < 8 || tam > sizeof ef_buf) return HW_ERROR;
    if (ef_activo) lu_efecto_parar();                               /* antes de pisar el búfer */
    if (hw_fs_leer(ruta, b, tam) != tam) return HW_ERROR;
    ver = b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
    if (ver > BLE_VERSION_MAX) return HW_ERROR;                     /* el original exige < 6 */
    n = (tam - 4) / 4;
    for (i = 0; i < n; i++) {                                       /* de bytes (little-endian) a palabras, en el sitio y sin la de versión */
        const u8 *q = b + 4 + i * 4;
        u32 v = q[0] | (u32)q[1] << 8 | (u32)q[2] << 16 | (u32)q[3] << 24;
        ef_buf[i] = v;
    }
    return lu_efecto_memoria(ef_buf, n, intensidad, color);
}
u32 lu_paso(void)
{
    u32 t = hw_ms(), falta = 0, i;
    if (ef_activo && hw_luz_efecto(-1) <= 0) {                      /* la plataforma se ha quedado las luces (se atenuó, se durmió): fuera, sin escribir nada */
        ef_activo = 0; ef_fin = BLE_ACABADO;
        for (i = 0; i < HW_LEDS; i++) lu_l[i].paso = 0;
        lu_fundiendo = 0;
    }
    if (lu_fundiendo && (int)(t - lu_t50) >= 0) {
        u32 quedan = 0;
        for (i = 0; i < HW_LEDS; i++) if (lu_l[i].paso) { lu_un_paso(i); lu_cta.pasos_fundido++; if (lu_l[i].paso) quedan++; }
        lu_t50 = (int)(t - lu_t50) > 200 ? t + 50 : lu_t50 + 50;    /* si el bucle se retrasó mucho, no se recupera a ráfagas */
        if (!quedan) lu_fundiendo = 0;
    }
    if (ef_activo == 1 && (int)(t - ef_t) >= 0) {
        int ms = ble_correr(&ef);
        if (ms < 1) { ef_fin = ef.fin; ef_activo = 2; }             /* el programa acabó; los fundidos que dejó en marcha, no */
        else {
            if (ms < BLE_ESPERA_MIN) ms = BLE_ESPERA_MIN;           /* NUESTRO: un fichero dañado no puede machacar el I²C (la fábrica no baja de 25 ms) */
            lu_cta.esperas++; ef_t = (int)(t - ef_t) > 200 ? t + (u32)ms : ef_t + (u32)ms;
        }
    }
    if (ef_activo == 2 && !lu_fundiendo) ef_soltar();               /* como en el original, un fundido sigue hasta el final aunque el efecto haya acabado */
    t = hw_ms();
    if (lu_fundiendo) falta = (int)(lu_t50 - t) > 0 ? lu_t50 - t : 1;
    if (ef_activo == 1) { u32 f = (int)(ef_t - t) > 0 ? ef_t - t : 1; if (!falta || f < falta) falta = f; }
    return falta;
}
