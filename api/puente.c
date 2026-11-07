/* api/puente.c — EL PUENTE de depuración por USB (senew v16). Ver api/puente.h y docs/puente.md. Todo el fichero existe SOLO con
 * -DPUENTE; sin la bandera queda vacío y el binario sale idéntico. */
#ifdef PUENTE
#include <string.h>
#include "puente.h"
#include "hw.h"

#if defined(TELEFONO) && TELEFONO
#define PU_TEL 1
#else
#define PU_TEL 0
#endif

/* ---- lo que cambia entre el teléfono y el PC de pruebas (herr/pruebas/prueba_puente.c) ----------------------------------
 * En el teléfono una dirección es un puntero y una función es una función. En el PC (-DPU_PC) las «direcciones» son
 * desplazamientos dentro de una arena de la prueba y «llamar» es una función de la prueba. */
#if PU_TEL
#define PU_PTR(a)        ((volatile u8 *)(size_t)(a))
typedef u32 (*PuFn)(u32, u32, u32, u32);
#define PU_LLAMAR(f, a, b, c, d)  (((PuFn)(size_t)(f))((a), (b), (c), (d)))
void cache_limpiar(u32 inicio, u32 fin);                    /* hal/reloj.S */
static void sincronizar(u32 ini, u32 fin)
{
    cache_limpiar(ini, fin);
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c5, 0\n nop\n nop\n nop\n nop" : : : "r0", "memory");   /* invalida la de instrucciones (como shell/cargador.c) */
}
static u32 irq_guardar(void) { u32 f; __asm__ volatile("mrs %0, cpsr\n orr r1, %0, #0x80\n msr cpsr_c, r1" : "=r"(f) : : "r1", "cc", "memory"); return f; }
static void irq_restaurar(u32 f) { __asm__ volatile("msr cpsr_c, %0" : : "r"(f) : "memory"); }
#else
extern size_t pu_pc_base;
extern u32 pu_pc_llamar(u32 f, u32 a, u32 b, u32 c, u32 d);
extern void pu_pc_reiniciar(void);
#define PU_PTR(a)        ((volatile u8 *)(pu_pc_base + (size_t)(a)))
#define PU_LLAMAR(f, a, b, c, d)  pu_pc_llamar((f), (a), (b), (c), (d))
static void sincronizar(u32 ini, u32 fin) { (void)ini; (void)fin; }
static u32 irq_guardar(void) { return 0; }
static void irq_restaurar(u32 f) { (void)f; }
#endif

/* ---- los búferes (globales a propósito: el código que sube el anfitrión los ve por símbolo) ---- */
u8 pu_buf[PU_BUF] __attribute__((aligned(32)));
u8 pu_codigo[PU_CODIGO] __attribute__((aligned(4096)));
/* El identificador de ESTA compilación: el anfitrión lo lee de la imagen (símbolo pu_ident) y lo compara con el del teléfono. */
const char pu_ident[28] __attribute__((used)) = "PUEN1 " __DATE__ " " __TIME__;

static PuCmd cmd, cmd_rx;
static u32 arg[6];                                          /* las palabras del nivel 0 (PU_R_ARG0..5) */
static PuRes res, res_copia;
static PuId id_copia;
static u8 salida[8 + PU_LOG_MAX];
static u8 en_app_ahora;
static u32 reinicio_a;                                      /* hw_ms() + 1: cuándo reiniciar (tras dejar contestada la orden) */

/* ---- el registro (anillo de hw_log) ---------------------------------------------------------------------------------- */
static u8 lg[PU_LOG_TAM];
static volatile u32 lg_total;
void pu_log(const char *s)
{
    u32 f = irq_guardar(), t = lg_total;
    while (*s) lg[t++ & (PU_LOG_TAM - 1)] = (u8)*s++;
    lg_total = t;
    irq_restaurar(f);
}

/* ---- las teclas inyectadas -------------------------------------------------------------------------------------------- */
#define PU_TECLAS 64
static u8 th_modo[PU_TECLAS];                               /* 0 suelta, 1 mantenida, 2 con plazo */
static u32 th_hasta[PU_TECLAS];
static u8 tcola[16], tn;
int pu_tecla_sacar(void)
{
    int k, i;
    if (!tn) return 0;
    k = tcola[0];
    for (i = 1; i < tn; i++) tcola[i - 1] = tcola[i];
    tn--;
    return k;
}
int pu_tecla_abajo(int k)
{
    if (k <= 0 || k >= PU_TECLAS || !th_modo[k]) return 0;
    if (th_modo[k] == 2 && (int)(hw_ms() - th_hasta[k]) >= 0) { th_modo[k] = 0; return 0; }
    return 1;
}
static void tecla_poner(u32 k, u32 modo, u32 ms)
{
    if (k == 0 || k >= PU_TECLAS) return;
    if (modo == 0) { th_modo[k] = 0; return; }
    if (tn < sizeof tcola) tcola[tn++] = (u8)k;
    if (modo == 1) th_modo[k] = 1;
    else { th_modo[k] = 2; th_hasta[k] = hw_ms() + (ms ? ms : 120u); }
}

/* ---- la guardia contra excepciones de la CPU (solo el teléfono) -------------------------------------------------------- */
#if PU_TEL
typedef struct { u32 r[10]; } PuSalto;
__asm__(".text\n .arm\n .align 2\n"
        " .global pu_guardar\n .type pu_guardar, %function\n"
        "pu_guardar:\n stmia r0, {r4-r11}\n str sp, [r0, #32]\n str lr, [r0, #36]\n mov r0, #0\n bx lr\n"
        " .global pu_volver\n .type pu_volver, %function\n"
        "pu_volver:\n msr cpsr_c, #0x13\n ldr sp, [r0, #32]\n ldr lr, [r0, #36]\n ldmia r0, {r4-r11}\n movs r0, r1\n moveq r0, #1\n bx lr\n");
int  pu_guardar(PuSalto *s) __attribute__((returns_twice));
void pu_volver(PuSalto *s, int v) __attribute__((noreturn));
static PuSalto salto;
static volatile u8 en_guardia;
int puente_fallo_cpu(const u32 *ctx)
{
    u32 modo = ctx[16] & 0x1F;
    if (!en_guardia || (modo != 0x13 && modo != 0x1F && modo != 0x10)) return 0;   /* una excepción en una IRQ no es nuestra: se trata como siempre */
    res.falta_tipo = ctx[17]; res.falta_pc = ctx[15]; res.falta_lr = ctx[14]; res.falta_cpsr = ctx[16];
    res.falta_dir = ctx[17] == 4 ? ctx[19] : ctx[15];
    if (ctx[17] == 1) res.falta_pc -= (ctx[16] & 0x20) ? 2u : 4u;   /* una indefinida deja en lr la instrucción SIGUIENTE (arranque.S solo corrige las de aborto) */
    pu_volver(&salto, 1);
}
#else
int puente_fallo_cpu(const u32 *ctx) { (void)ctx; return 0; }
#endif

/* ---- las órdenes ------------------------------------------------------------------------------------------------------ */
#define NOR_INI 0x44000000u
#define NOR_FIN 0x48000000u
static int toca_nor(u32 a, u32 n) { return a < NOR_FIN && a + n > NOR_INI && a + n >= a; }
static int ancho_de(u32 a, u32 n, u32 w)
{
    if (w == 0) w = (!(a & 3) && !(n & 3)) ? 4u : (!(a & 1) && !(n & 1)) ? 2u : 1u;
    if (w != 1 && w != 2 && w != 4) return -1;
    if ((a % w) || (n % w)) return -2;
    return (int)w;
}
static u32 leer_w(u32 a, u32 w)
{
    volatile u8 *p = PU_PTR(a);
    return w == 4 ? *(volatile u32 *)(void *)p : w == 2 ? *(volatile u16 *)(void *)p : *p;
}
static void escribir_w(u32 a, u32 w, u32 v)
{
    volatile u8 *p = PU_PTR(a);
    if (w == 4) *(volatile u32 *)(void *)p = v; else if (w == 2) *(volatile u16 *)(void *)p = (u16)v; else *p = (u8)v;
}
static void despachar(void)
{
    const PuCmd *c = &cmd;
    u32 i, n, a, w;
    int ww;
    switch (c->op) {
    case PU_OP_NADA: res.r0 = c->seq; break;
    case PU_OP_LEER:
        a = c->p[0]; n = c->p[1];
        if (n > PU_BUF) { res.codigo = PU_E_PARAM; break; }
        ww = ancho_de(a, n, c->p[2]);
        if (ww < 0) { res.codigo = ww == -1 ? PU_E_PARAM : PU_E_ALINEA; break; }
        w = (u32)ww;
        for (i = 0; i < n; i += w) {
            u32 v = leer_w(a + i, w);
            if (w == 4) *(u32 *)(void *)(pu_buf + i) = v; else if (w == 2) *(u16 *)(void *)(pu_buf + i) = (u16)v; else pu_buf[i] = (u8)v;
        }
        res.bytes = n;
        break;
    case PU_OP_ESCRIBIR:
        a = c->p[0]; n = c->p[1];
        if (n > PU_BUF) { res.codigo = PU_E_PARAM; break; }
        if (toca_nor(a, n) && !(c->flags & PU_F_PELIGRO)) { res.codigo = PU_E_NOR; break; }
        ww = ancho_de(a, n, c->p[2]);
        if (ww < 0) { res.codigo = ww == -1 ? PU_E_PARAM : PU_E_ALINEA; break; }
        w = (u32)ww;
        for (i = 0; i < n; i += w) {
            u32 v = w == 4 ? *(u32 *)(void *)(pu_buf + i) : w == 2 ? *(u16 *)(void *)(pu_buf + i) : pu_buf[i];
            escribir_w(a + i, w, v);
        }
        if (a >= 0x20000000u && a + n <= 0x21000000u) sincronizar(a, a + n);   /* SDRAM: por si era código, lo escrito sale de la caché de datos y la de instrucciones lo ve */
        res.bytes = n;
        break;
    case PU_OP_LLAMAR: {
        int plazo = c->p[5] ? (int)c->p[5] : 30;
        hw_perro_plazo(plazo);                              /* una función larga no es un cuelgue; sin esto el perro de 10 s mordería */
        hw_perro_latido();
        res.r0 = PU_LLAMAR(c->p[0], c->p[1], c->p[2], c->p[3], c->p[4]);
        hw_perro_latido();
        hw_perro_plazo(en_app_ahora ? 30 : 10);
        break;
    }
    case PU_OP_RMW:
        a = c->p[0]; w = c->p[3];
        if (w != 1 && w != 2 && w != 4) { res.codigo = PU_E_PARAM; break; }
        if (a % w) { res.codigo = PU_E_ALINEA; break; }
        if (toca_nor(a, w) && !(c->flags & PU_F_PELIGRO)) { res.codigo = PU_E_NOR; break; }
        res.r0 = leer_w(a, w);
        res.r1 = (res.r0 & ~c->p[1]) | c->p[2];
        escribir_w(a, w, res.r1);
        break;
    case PU_OP_TECLA: tecla_poner(c->p[0], c->p[1], c->p[2]); break;
    case PU_OP_REINICIAR: reinicio_a = hw_ms() + 400u; if (!reinicio_a) reinicio_a = 1; break;   /* tras contestar */
    case PU_OP_SYNC: sincronizar(c->p[0], c->p[1]); break;
    case PU_OP_DISPERSA:
        n = c->p[0]; w = c->p[1];
        if (n * 4u > PU_BUF) { res.codigo = PU_E_PARAM; break; }
        if (w != 1 && w != 2 && w != 4) { res.codigo = PU_E_PARAM; break; }
        for (i = 0; i < n; i++) {
            u32 dir = ((u32 *)(void *)pu_buf)[i];
            if (dir % w) { res.codigo = PU_E_ALINEA; ((u32 *)(void *)pu_buf)[i] = 0; res.r1 = i + 1; continue; }
            ((u32 *)(void *)pu_buf)[i] = leer_w(dir, w);
        }
        res.bytes = n * 4u;
        break;
    case PU_OP_POKE:
        a = c->p[0]; w = c->p[1] & 0xFF; n = (c->p[1] >> 8) & 0xFF;
        if ((w != 1 && w != 2 && w != 4) || n < 1 || n > 4) { res.codigo = PU_E_PARAM; break; }
        if (a % w) { res.codigo = PU_E_ALINEA; break; }
        if (toca_nor(a, w * n) && !(c->flags & PU_F_PELIGRO)) { res.codigo = PU_E_NOR; break; }
        for (i = 0; i < n; i++) escribir_w(a + w * i, w, c->p[2 + i]);
        if (a >= 0x20000000u && a + w * n <= 0x21000000u) sincronizar(a, a + w * n);
        res.bytes = w * n;
        break;
    default: res.codigo = PU_E_ORDEN; break;
    }
}

void puente_paso(int en_app)
{
    en_app_ahora = (u8)(en_app != 0);
    if (reinicio_a && (int)(hw_ms() - reinicio_a) >= 0) { reinicio_a = 0; hw_reiniciar(); }
    if (res.estado != PU_ST_PENDIENTE) return;
    res.estado = PU_ST_EJECUTANDO;
    res.codigo = PU_OK; res.r0 = res.r1 = res.bytes = 0;
    res.falta_tipo = res.falta_pc = res.falta_dir = res.falta_lr = res.falta_cpsr = 0;
    hw_perro_latido();
#if PU_TEL
    en_guardia = 1;
    if (pu_guardar(&salto) == 0) { despachar(); en_guardia = 0; res.estado = PU_ST_HECHO; }
    else { en_guardia = 0; hw_perro_plazo(en_app_ahora ? 30 : 10); res.estado = PU_ST_FALLO_CPU; res.codigo = PU_E_RAMA; }
#else
    despachar(); res.estado = PU_ST_HECHO;
#endif
    hw_perro_latido();
    res.ms = hw_ms();
    res.ordenes++;
}

/* ---- las peticiones de vendedor (EP0) ------------------------------------------------------------------------------------ */
static int ocupado(void) { return res.estado == PU_ST_PENDIENTE || res.estado == PU_ST_EJECUTANDO; }
int puente_ep0(const u8 s[8], const u8 **p, u32 *n)
{
    u8 tipo = s[0], pet = s[1];
    u32 valor = (u32)(s[2] | s[3] << 8), indice = (u32)(s[4] | s[5] << 8), largo = (u32)(s[6] | s[7] << 8);
    if ((tipo & 0x60) != 0x40 || !((pet >= PU_R_ID && pet <= PU_R_LOG) || (pet >= PU_R_ARG0 && pet <= PU_R_ARG0 + 5) || pet == PU_R_GO)) return PU_USB_NO_ES_MIA;
    if ((tipo & 0x1F) != 0) return PU_USB_STALL;            /* solo al dispositivo */
    if (pet >= PU_R_ARG0 && pet <= PU_R_ARG0 + 5) {         /* nivel 0: una palabra del próximo orden, en el SETUP (sin fase de datos) */
        if (tipo != 0x40 || largo != 0 || ocupado()) return PU_USB_STALL;
        arg[pet - PU_R_ARG0] = valor | indice << 16;
        return PU_USB_LISTO;
    }
    if (pet == PU_R_GO) {                                   /* ... y ejecutarla: wValue = op | seq << 8, wIndex = flags */
        if (tipo != 0x40 || largo != 0 || ocupado() || (valor & 0xFF) >= PU_OP_N) return PU_USB_STALL;
        cmd.op = (u8)valor; cmd.seq = (u8)(valor >> 8); cmd.flags = (u16)indice;
        memcpy(cmd.p, arg, sizeof arg);
        res.seq = cmd.seq; res.op = cmd.op; res.estado = PU_ST_PENDIENTE;
        return PU_USB_LISTO;
    }
    switch (pet) {
    case PU_R_ID:
        if (tipo != 0xC0) return PU_USB_STALL;
        memset(&id_copia, 0, sizeof id_copia);
        memcpy(id_copia.magia, "PUEN", 4);
        id_copia.version = PU_VERSION; id_copia.flags = 0;
        id_copia.buf = (u32)(size_t)pu_buf; id_copia.buf_tam = PU_BUF;
        id_copia.codigo = (u32)(size_t)pu_codigo; id_copia.codigo_tam = PU_CODIGO;
        id_copia.log_total = lg_total; id_copia.ms = hw_ms();
        memcpy(id_copia.build, pu_ident, sizeof id_copia.build);
        *p = (const u8 *)&id_copia; *n = sizeof id_copia;
        return PU_USB_IN;
    case PU_R_PUT:
        if (tipo != 0x40 || valor + largo > PU_BUF || ocupado()) return PU_USB_STALL;
        if (!largo) return PU_USB_LISTO;
        *p = pu_buf + valor; *n = largo;
        return PU_USB_OUT;
    case PU_R_GET:
        if (tipo != 0xC0 || valor + largo > PU_BUF) return PU_USB_STALL;
        *p = pu_buf + valor; *n = largo;
        return PU_USB_IN;
    case PU_R_CMD:
        if (tipo != 0x40 || largo != sizeof cmd_rx || ocupado()) return PU_USB_STALL;
        *p = (const u8 *)&cmd_rx; *n = sizeof cmd_rx;
        return PU_USB_OUT;
    case PU_R_RES:
        if (tipo != 0xC0) return PU_USB_STALL;
        res_copia = res;
        *p = (const u8 *)&res_copia; *n = sizeof res_copia;
        return PU_USB_IN;
    case PU_R_LOG: {
        u32 pos = valor | indice << 16, total = lg_total, ini, k, m;
        if (tipo != 0xC0 || largo < 8) return PU_USB_STALL;
        if (pos > total) pos = total;                       /* el teléfono se reinició desde la última lectura: el anfitrión lo ve por total < pos */
        ini = pos;
        if (total - ini > PU_LOG_TAM) ini = total - PU_LOG_TAM;
        m = total - ini;
        if (m > largo - 8) m = largo - 8;
        if (m > PU_LOG_MAX) m = PU_LOG_MAX;
        for (k = 0; k < m; k++) salida[8 + k] = lg[(ini + k) & (PU_LOG_TAM - 1)];
        memcpy(salida, &ini, 4); memcpy(salida + 4, &total, 4);
        *p = salida; *n = 8 + m;
        return PU_USB_IN;
    }
    default: return PU_USB_STALL;
    }
}
int puente_ep0_fin(const u8 s[8], const u8 *datos, u32 n)
{
    (void)datos;
    if (s[1] != PU_R_CMD) return 0;                          /* PUT, ARG, GO: ya está hecho (los datos de PUT ya están en pu_buf) */
    if (n != sizeof cmd_rx || cmd_rx.op >= PU_OP_N || ocupado()) return -1;
    cmd = cmd_rx;
    res.seq = cmd.seq; res.op = cmd.op;
    res.estado = PU_ST_PENDIENTE;
    return 0;
}
#endif
