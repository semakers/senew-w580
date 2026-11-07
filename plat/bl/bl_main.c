/* bl_main.c — BUCLE DEL BOOTLOADER en modo flasheo. Lo invoca bl_arranque.S con entrada = 1 (bandera) o 2 (main roto). Vive en
 * la ITCM; nada se ejecuta desde la NOR mientras el chip está en modo de órdenes.
 *
 * Bucle:
 *   1. Si no se arrancó el UDC: encender relojes USB (bl_syscon_usb), iniciar UDC (bl_udc_iniciar), pull-up.
 *   2. bl_usb_paso(): sondea el UDC, atiende SETUP/EP0 (fase de datos IN/OUT del protocolo).
 *   3. Si hay orden pendiente: ejecutarla (fuera del manejador USB) y marcar HECHO con su número de secuencia.
 *      El anfitrión sondea BL_R_RES hasta ver HECHO con el seq que mandó.
 *   4. Si la orden es BL_OP_SALIR: contesta, suelta el pull-up, borra la bandera, salta a _bl_reset.
 */
#include <stddef.h>
#include "bl.h"
#include "bl_pantalla.h"

extern void bl_espera_ms(unsigned ms);
extern int  bl_syscon_usb(void);
extern void bl_hw_ms_arranque(void);
extern u32  bl_hw_ms(void);
extern int  bl_udc_iniciar(void);
extern void bl_usb_paso(void);
extern BlRes *bl_res_actual(void);
extern u32  *bl_arg_actual(void);
extern u8   *bl_buf_actual(void);
extern int  bl_nor_borrar(u32 blq, u32 *sr_out);
extern int  bl_nor_en_blanco(u32 dir);
extern int  bl_nor_programar(u32 dir, const u16 *org, u32 *sr_out);
#ifdef BL_INSTALLER
extern int  bl_nor_borrar_propio(u32 blq, u32 *sr_out);
extern int  bl_nor_programar_propio(u32 dir, const u16 *org, u32 *sr_out);
#endif
extern u32  bl_crc32(u32 dir, u32 n);
extern void _bl_reset(void);

int bl_entrada_actual;
/* (installer) estado del progreso que lleva el BL para repintar la pantalla al avanzar WRITE/WRITE_PROPIO. BL_OP_INSTALADOR_INI
 * los pone a (0, total); un total == 0 inhabilita las actualizaciones automáticas (y bl_barra pinta 0 %). */
#ifdef BL_INSTALLER
static u32 inst_hecho, inst_total;
#endif

#define MAIN_BLOQUES ((0x443C0000u - 0x44180000u) / BL_BLOQUE)   /* 9 bloques */

static void op_wipe(BlRes *r)
{
    unsigned i; u32 sr; int e;
    /* El bloque de MAIN_INI PRIMERO (regla: una grabación cortada deja los vectores ausentes → BL se queda) */
    e = bl_nor_borrar(0x44180000u, &sr);
    if (e) { r->codigo = e; r->r0 = 1; r->r1 = sr; return; }
    for (i = 1; i < MAIN_BLOQUES; i++) {
        e = bl_nor_borrar(0x44180000u + i * BL_BLOQUE, &sr);
        if (e) { r->codigo = e; r->r0 = 1 + i; r->r1 = sr; return; }
    }
    r->r0 = MAIN_BLOQUES;
    r->codigo = BL_OK;
}

static void op_write(BlRes *r)
{
    u32 *a = bl_arg_actual();
    u32 dir = a[0], n = a[1];
    u8 *buf = bl_buf_actual();
    u32 off, sr; int e;
    if ((dir & (BL_REGION - 1)) || (n & (BL_REGION - 1)) || !n || n > BL_BUF) { r->codigo = BL_E_PARAM; return; }
    for (off = 0; off < n; off += BL_REGION) {
        e = bl_nor_programar(dir + off, (const u16 *)(buf + off), &sr);
        if (e) { r->codigo = e; r->r0 = dir + off; r->r1 = sr; return; }
    }
    r->r0 = bl_crc32(dir, n);
    r->codigo = BL_OK;
#ifdef BL_INSTALLER
    inst_hecho += n;
    if (inst_total) bl_barra(inst_hecho, inst_total);
#endif
}

#ifdef BL_INSTALLER
/* op_write_propio: como op_write pero el rango permitido se amplía a [BL_INI, MAIN_FIN) por la variante _propio de bl_nor. Si el
 * destino cae dentro del propio bloque del BL (BL_INI..BL_INI+0x40000) la escritura es segura: el BL corre ENTERO desde la ITCM
 * (vectores) y la DTCM (datos y pila); nada se lee de la NOR mientras el programa Intel está en marcha. El bloque del BL tiene
 * que estar borrado antes de llamar (lo hace el anfitrión con un wipe propio: pu_bl_installer.py). */
static void op_write_propio(BlRes *r)
{
    u32 *a = bl_arg_actual();
    u32 dir = a[0], n = a[1];
    u8 *buf = bl_buf_actual();
    u32 off, sr; int e;
    if ((dir & (BL_REGION - 1)) || (n & (BL_REGION - 1)) || !n || n > BL_BUF) { r->codigo = BL_E_PARAM; return; }
    for (off = 0; off < n; off += BL_REGION) {
        e = bl_nor_programar_propio(dir + off, (const u16 *)(buf + off), &sr);
        if (e) { r->codigo = e; r->r0 = dir + off; r->r1 = sr; return; }
    }
    r->r0 = bl_crc32(dir, n);
    r->codigo = BL_OK;
    inst_hecho += n;
    if (inst_total) bl_barra(inst_hecho, inst_total);
}
#endif

static void op_crc(BlRes *r)
{
    u32 *a = bl_arg_actual();
    r->r0 = bl_crc32(a[0], a[1]);
    r->codigo = BL_OK;
}

#ifdef BL_RAM
/* (27-oct) LA RAM: el original ya no corre (se volverá a él reiniciando) y aquí no hay MMU ni caché de datos, así que toda la SDRAM
 * (0x20000000..0x20FFFFFF) es nuestra. RAMW copia bl_buf a la RAM; IR salta a una imagen ya escrita (senew compilado para RAM). */
static void op_ramw(BlRes *r)
{
    u32 *a = bl_arg_actual();
    u32 dir = a[0], n = a[1], i;
    const u8 *org = bl_buf_actual();
    if ((dir & 3) || (n & 3) || !n || n > BL_BUF || dir < 0x20000000u || dir + n > 0x21000000u) { r->codigo = BL_E_RANGO; return; }
    for (i = 0; i < n; i++) ((volatile u8 *)dir)[i] = org[i];
    r->r0 = bl_crc32(dir, n);
    r->codigo = BL_OK;
}
static void op_ir(BlRes *r)
{
    u32 dir = bl_arg_actual()[0];
    if ((dir & 3) || dir < 0x20000000u || dir >= 0x21000000u) { r->codigo = BL_E_RANGO; return; }
    r->codigo = BL_OK; r->r0 = dir; r->ordenes++; r->estado = BL_ST_HECHO;
    {
        u32 t0 = bl_hw_ms();
        while (bl_hw_ms() - t0 < 500) bl_usb_paso();          /* que el anfitrión lea el HECHO */
    }
    *(volatile u16 *)0xFC030018 = *(volatile u16 *)0xFC030018 & ~1u;   /* pull-up fuera */
    bl_espera_ms(100);
    __asm__ volatile("mov r0, #0\n\tmcr p15, 0, r0, c7, c5, 0\n\tmcr p15, 0, r0, c7, c10, 4" : : : "r0", "memory");
    /* EL PERRO: armado (+30 distinto de 0, +2C = 0) muerde a los 4,0 s [medido 27-oct en el aparato] y reinicia al original con su daemon; la
     * llave (AA, 5A, A5) lo desarma. Senew en RAM lo desarma al llegar a su bucle (plat/tel/perro.c): si se cuelga antes, vuelve solo. */
    *(volatile u32 *)0xF9010030 = 0x0F;
    *(volatile u32 *)0xF901002C = 0;
    ((void (*)(void))dir)();
    for (;;) { }
}

/* (27-oct, senew en RAM: docs/senew_en_ram.md) SALIR vuelve al FIRMWARE ORIGINAL, que sigue entero en la NOR: primero el perro, como
 * sys_reset de senew (hal/syscon.c: +30 y +2C a 0 y +14 a 1 sin parar) durante 3 s; si no muerde, el reinicio en caliente de senew
 * (hal/arranque.S, reinicio_caliente): bajar el reloj a 13 MHz (esto corre en la ITCM: no lee la NOR) y saltar al vector de reset del
 * original en 0x44140000. Nada de esto escribe en la NOR. */
static void salir(void)
{
    volatile u16 *s1 = (volatile u16 *)0xFC030018;
    u32 t0;
    *s1 = *s1 & ~1u;                               /* pull-up fuera */
    bl_espera_ms(100);
    *(volatile u32 *)0xF9010030 = 0;
    *(volatile u32 *)0xF901002C = 0;
    t0 = bl_hw_ms();
    while (bl_hw_ms() - t0 < 3000) *(volatile u32 *)0xF9010014 = 1;
    *(volatile u16 *)0xF909001C = *(volatile u16 *)0xF909001C & 0x400u;   /* la CPU deja de ir por el PLL */
    *(volatile u16 *)0xF9090080 = *(volatile u16 *)0xF9090080 & ~0x10u;
    *(volatile u16 *)0xF9090054 = 0;                                      /* bus = CPU (13 MHz) */
    ((void (*)(void))0x44140000u)();                                      /* el reset del original (ARM) */
    for (;;) { }
}
#else
static void salir(void)
{
    /* Suelta el pull-up (que el host vea la desconexión) y borra la bandera. Vuelve a entrar por _bl_reset: la decisión mirará
     * los vectores del main y saltará ahí (si están). */
    volatile u16 *s1 = (volatile u16 *)0xFC030018;
    *s1 = *s1 & ~1u;                               /* pull-up fuera */
    {
        volatile u32 *b = (volatile u32 *)0xF3002F00;
        b[0] = 0; b[1] = 0;
    }
    bl_espera_ms(100);                             /* que el host registre la desconexión */
    /* Salto duro al reset en la NOR. */
    _bl_reset();
    for (;;) { }                                   /* no debería volver */
}
#endif

extern void bl_fase(unsigned fase, u32 valor);

void bl_main(int entrada)
{
    BlRes *r = bl_res_actual();
    bl_entrada_actual = entrada;

    bl_fase(1, entrada);                                      /* fase 1: entró a bl_main */
    bl_hw_ms_arranque();

    bl_fase(2, 0);
    if (bl_syscon_usb() < 0) { bl_fase(0x80, 1); for (;;) { } }   /* 0x80 = falló aquí */

    bl_fase(3, 0);
    if (bl_udc_iniciar() < 0) { bl_fase(0x80, 2); for (;;) { } }  /* 0x80 = falló aquí */

#ifdef BL_INSTALLER
    /* (installer) banner SOLO si llegamos sin bandera: entrada = BL_ENTRADA_SIN_MAIN (main 0xFFFFFFFF o corrupto). En modo
     * clásico (entrada = BL_ENTRADA_BANDERA, 1), no se enciende pantalla: lo del BL clásico es iterar senew por USB. */
    if (entrada == BL_ENTRADA_SIN_MAIN) {
        bl_fase(5, 0);
        bl_pantalla_ini();                                    /* el log del BL avisa si -3: panel no contestó, se sigue sin pantalla */
    }
#endif

    bl_fase(4, 0);                                            /* fase 4: entrando al bucle principal */

    for (;;) {
        bl_usb_paso();
        if (r->estado == BL_ST_PENDIENTE) {
            u8 op = r->op;      /* r->op es u16; los códigos encajan en u8 (0..6 clásicos, 0xD4..0xD6 installer) */
            r->estado = BL_ST_EJECUTANDO;
            switch (op) {
            case BL_OP_NADA:  r->codigo = BL_OK; r->r0 = r->seq; break;
#ifdef BL_RAM                                             /* (27-oct) en RAM, desde el ORIGINAL: la NOR es la del original, NUNCA se escribe */
            case BL_OP_WIPE:
            case BL_OP_WRITE: r->codigo = BL_E_ORDEN; break;
            case BL_OP_RAMW:  op_ramw(r); break;
            case BL_OP_IR:    op_ir(r); break;
#else
            case BL_OP_WIPE:  op_wipe(r); break;
            case BL_OP_WRITE: op_write(r); break;
#endif
            case BL_OP_CRC:   op_crc(r); break;
#ifdef BL_INSTALLER
            case BL_OP_WRITE_PROPIO: op_write_propio(r); break;
            case BL_OP_WIPE_PROPIO: {
                u32 *a = bl_arg_actual(); u32 blq = a[0], sr; int e;
                e = bl_nor_borrar_propio(blq, &sr);
                r->codigo = e; r->r0 = e ? 0 : 1; r->r1 = sr;
                break;
            }
            case BL_OP_INSTALADOR_INI: {
                u32 *a = bl_arg_actual();
                inst_total = a[0];
                inst_hecho = 0;
                if (bl_pantalla_hay()) bl_barra(0, inst_total);
                r->codigo = BL_OK; r->r0 = inst_total;
                break;
            }
            case BL_OP_LISTO:
                bl_listo();
                r->codigo = BL_OK;
                break;
#endif
            case BL_OP_SALIR:
                /* Marca hecho ANTES de salir: el anfitrión sondea RES y luego pierde la conexión. */
                r->codigo = BL_OK;
                r->ordenes++;
                r->estado = BL_ST_HECHO;
                /* Da unas vueltas de USB para contestar el RES pendiente. */
                {
                    u32 t0 = bl_hw_ms();
                    while (bl_hw_ms() - t0 < 500) bl_usb_paso();
                }
                salir();
                break;
            default:          r->codigo = BL_E_ORDEN; break;
            }
            r->ordenes++;
            r->estado = BL_ST_HECHO;
        }
    }
}
