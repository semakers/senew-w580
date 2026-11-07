/* La API de hardware (api/hw.h) SIMULADA en el PC: tiempo virtual, teclas y serie inyectadas por el
 * guion (plat/pc_main.c), RTC de mentira que avanza con el tiempo virtual, FS = el árbol extraído
 * del teléfono (W600_FS o work/fs/AMERICA_1/arbol), «panel» que solo recibe las filas volcadas. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <math.h>
#include "hw.h"
#include "ui.h"
#include "pc.h"
#include "pc_nor.h"
#include "bat.h"
#include "enc.h"
#include "w6f.h"
#include "w6f_sistema.h"

u32 pc_reloj_ms;
u16 pc_panel[PLACA_ANCHO * PLACA_ALTO];
u32 pc_volcados, pc_filas_volcadas;
static int cola[32], n_cola;
static u8 abajo[K_N];
static u32 abajo_desde[K_N];
static u8 rx[4096]; static int n_rx;
static char tx[8192]; static int n_tx;
static signed char luz_puesta[HW_LUCES] = { 50, 0, 0, 0 };
static int luz_fase;
static u8 gpio_dat[5], gpio_dir[5];

void pc_tecla(int k) { if (n_cola < 32) cola[n_cola++] = k; }
void pc_mantener(int k, int si) { abajo[k] = (u8)si; abajo_desde[k] = pc_reloj_ms; if (si) pc_tecla(k); }
void pc_serie_rx(const char *s) { while (*s && n_rx < (int)sizeof rx) rx[n_rx++] = (u8)*s++; }
const char *pc_serie_tx(void) { tx[n_tx] = 0; return tx; }

/* almacén persistente: el modelo estricto de NOR (pc_nor.c), 4 bloques de 128 KiB como la zona del teléfono.
 * Con W600_NV=fichero se carga al arrancar y se guarda tras cada escritura (así dos ejecuciones seguidas
 * prueban «apagar y encender»); sin él, cada ejecución empieza con el almacén vacío. */
static int nv_hay;
static void nv_volcar(void)
{
    const char *f = getenv("W600_NV"); FILE *o;
    if (f && (o = fopen(f, "wb"))) { fwrite(pc_nor.mem, 1, (size_t)pc_nor.bloques * pc_nor.tam, o); fclose(o); }
}
/* (S7) La comprobación temprana de la batería, con LA MISMA decisión que el teléfono (api/bat.h). El «arranque» del PC la toma
 * del entorno: PC_ARR_LECTURA = la lectura en frío (0x6A; 56 = 3400 mV, 47 = 3303 mV) y PC_ARR_ESTADO23 = el registro 0x23
 * (8 = cargador, 128 = USB). Sin PC_ARR_LECTURA no se mira nada (como siempre). herr/probar_energia.sh. */
static int pc_seguro, pc_prot, pc_pend, pc_propia;          /* (S7) PC_MODO_SEGURO, PC_NOR_PROT (bits HW_PROT_*), y la zona sin formato (PC_NV_SUCIA) */
u32 pc_usr_escrituras;                             /* (S7) operaciones de escritura sobre la zona de usuario (para probar «0 escrituras») */
static HwArranque pc_arr = { HW_ARR_BIEN, 108, 3961, 0 };
int pc_apagable, pc_apagado, pc_apagados_fallidos, pc_cargas_fuera;
static void pc_bateria_al_arrancar(void)
{
    const char *l = getenv("PC_ARR_LECTURA"), *e = getenv("PC_ARR_ESTADO23");
    int que;
    if (!l) return;
    pc_arr.lectura = atoi(l); pc_arr.estado23 = e ? atoi(e) : 0; pc_arr.mv = pc_arr.lectura < 0 ? -1 : bat_mv_frio(pc_arr.lectura);
    que = bat_arranque_decidir(pc_arr.estado23, pc_arr.lectura);
    pc_arr.como = que == BAT_ARR_APAGAR ? HW_ARR_BIEN : que;
    printf("arranque: lectura %d = %d mV, estado 23 = %02X: %s\n", pc_arr.lectura, pc_arr.mv, pc_arr.estado23 & 0xFF,
           que == BAT_ARR_APAGAR ? "BATERIA BAJA sin cargador: NO se arranca (tres toques de vibrador y apagar)" :
           que == BAT_ARR_CARGADOR ? "hay cargador o USB: se arranca directo al aviso" :
           que == BAT_ARR_NO_CREIBLE ? "lectura NO CREIBLE: se sigue, con bandera" : "bien");
    if (que == BAT_ARR_APAGAR) { printf("fin: el telefono NO ha arrancado\n"); exit(0); }
    if (que == BAT_ARR_CARGADOR) pc_cargador_bits = (pc_arr.estado23 & 8 ? 1 : 0) | (pc_arr.estado23 & 0x80 ? 2 : 0);
}
void hw_iniciar(void)
{
    const char *f = getenv("W600_NV"); FILE *o;
    int sucia = getenv("PC_NV_SUCIA") != NULL;
    pc_bateria_al_arrancar();
    pc_seguro = getenv("PC_MODO_SEGURO") != NULL;
    pc_prot = (getenv("PC_NOR_PROT") ? atoi(getenv("PC_NOR_PROT")) : 0) | (pc_seguro ? HW_PROT_SEGURO : 0);
    if (pc_seguro) fprintf(stderr, "*** MODO SEGURO (PC_MODO_SEGURO): sin escritura, sin leer ajustes, sin apps externas\n");
    if (pc_prot & ~HW_PROT_SEGURO) fprintf(stderr, "nor: *** MODO SIN ESCRITURA *** (PC_NOR_PROT=%d)\n", pc_prot);
    pc_nor_crear(4, 131072);
    if (f && (o = fopen(f, "rb"))) {
        size_t n = fread(pc_nor.mem, 1, (size_t)pc_nor.bloques * pc_nor.tam, o), i;
        fclose(o);
        for (i = 0; i + 1 < n; i += 2) pc_nor.prog[i / 2] = pc_nor.mem[i] != 0xFF || pc_nor.mem[i + 1] != 0xFF;
    }
    /* (S7) como en el teléfono: primero SIN escribir; en modo seguro el almacén no se lee; protegida, solo lectura; una zona que
     * no está en blanco ni es nuestra (PC_NV_SUCIA la llena de «restos») NO se formatea: queda pendiente de que el usuario lo diga */
    if (sucia && !f) { size_t i; for (i = 0; i < (size_t)pc_nor.bloques * pc_nor.tam; i++) { pc_nor.mem[i] = (u8)(i * 31 + 7); } for (i = 0; i < (size_t)pc_nor.bloques * pc_nor.tam / 2; i++) pc_nor.prog[i] = 1; }
    if (pc_seguro) nv_hay = 0;
    else {
        nv_hay = alm_montar(pc_nor_puerto(), 0) == ALM_OK;
        if (nv_hay) { if (!pc_prot) nv_hay = alm_montar(pc_nor_puerto(), 1) == ALM_OK; }
        else if (!pc_prot) {
            if (sucia) { pc_pend |= HW_ZONA_AJUSTES; fprintf(stderr, "nv: la zona NO esta en blanco ni es nuestra: NO se formatea sin permiso\n"); }
            else if (alm_firma(pc_nor_puerto())) {   /* (AUD2 B4) como el teléfono: un almacén NUESTRO que no monta no está en blanco: se pregunta */
                pc_pend |= HW_ZONA_AJUSTES; pc_propia |= HW_ZONA_AJUSTES;
                fprintf(stderr, "nv: la zona lleva NUESTRA marca pero NO monta (danada o a medio formatear): NO se formatea sin permiso\n");
            }
            else nv_hay = alm_montar(pc_nor_puerto(), 1) == ALM_OK;
        }
    }
    if (!pc_prot) nv_volcar();
    hw_usr_iniciar();
    hw_w6f_iniciar();                       /* (I1) la Memoria (W6F) */
}
int hw_modo_seguro(void) { return pc_seguro; }
/* (R3, P3) PC_REINICIO=causa[,pc_hex[,auto]]: la causa del «arranque anterior» (un HW_RST_*), para probar lo que dice el shell. */
void hw_reinicio(HwReinicio *r)
{
    const char *e = getenv("PC_REINICIO"); unsigned c = 0, pc = 0, au = 0;
    memset(r, 0, sizeof *r);
    if (e && sscanf(e, "%u,%x,%u", &c, &pc, &au) >= 1) { r->causa = (unsigned char)c; r->pc = pc; r->seguro_auto = (unsigned char)(au && pc_seguro); r->sin_bucle = au ? 3 : 0; }
}
int hw_nor_proteccion(void) { return pc_prot; }
int hw_w6f_formatear_zona(int ajena); int hw_w6f_ajena(void);
int hw_formato_pendiente(void) { return pc_pend | (hw_w6f_ajena() ? HW_ZONA_MEMORIA : 0); }
int hw_formato_propio(void) { return pc_propia & pc_pend; }   /* (AUD2 B4) */
int  hw_tarjeta_paso(void);                                /* (pista T) api/tarjeta.c */
void hw_perro_latido(void) { hw_tarjeta_paso(); }
void hwo_progreso(void) { }
void hw_perro_plazo(int s) { (void)s; }
int hw_encendido_largo(void) { return 0; }
int hw_formatear(int zonas)
{
    int b;
    if (pc_prot) return HW_NO_DISPONIBLE;
    if (zonas & pc_pend & HW_ZONA_AJUSTES) {
        fprintf(stderr, "nv: formato de la zona de ajustes, CONFIRMADO por el usuario\n");
        for (b = 0; b < (int)pc_nor.bloques; b++) pc_nor_puerto()->borrar((u32)b);
        pc_pend &= ~HW_ZONA_AJUSTES;
        nv_hay = alm_montar(pc_nor_puerto(), 1) == ALM_OK; nv_volcar();
    }
    if ((zonas & HW_ZONA_MEMORIA) && hw_w6f_ajena() && hw_w6f_formatear_zona(1) != HW_OK) return HW_ERROR;   /* (I1) */
    return nv_hay || !(zonas & HW_ZONA_AJUSTES) ? HW_OK : HW_ERROR;
}
int hw_borrar_ajustes(void)
{
    int b;
    if (!pc_seguro || (pc_prot & ~HW_PROT_SEGURO)) return HW_NO_DISPONIBLE;   /* (R2) como en el teléfono: solo en modo seguro */
    fprintf(stderr, "nv: BORRADO del almacen de ajustes pedido por el usuario\n");
    for (b = 0; b < (int)pc_nor.bloques; b++) pc_nor_puerto()->borrar((u32)b);
    nv_hay = 0; nv_volcar();
    return HW_OK;
}
int hw_cargar(u16 clave, void *buf, u32 max) { int r = nv_hay ? alm_cargar(clave, buf, max) : -1; return r < 0 ? HW_NO_DISPONIBLE : r; }
/* (R2) como el teléfono: con una app externa en pantalla, solo las claves de las externas se pueden guardar u olvidar */
static u8 claves_de_app;
void hwo_claves_app(int si) { claves_de_app = (u8)(si != 0); }
static int clave_vedada(u16 clave) { return claves_de_app && (clave < HW_NV_EXT_INI || clave > HW_NV_EXT_FIN); }
int pc_gancho_app;                                 /* veces que una app puso el gancho del tic (el PC no tiene tic) */
int hwo_tic_sistema(void (*f)(void)) { (void)f; return HW_NO_DISPONIBLE; }
int hwo_tic_app_fuera(void) { int h = pc_gancho_app != 0; pc_gancho_app = 0; return h; }
int hw_guardar(u16 clave, const void *d, u32 n) { int r; if (!nv_hay || pc_prot) return HW_NO_DISPONIBLE; if (clave_vedada(clave)) return HW_ERROR; if (getenv("PC_NV_FALLA")) return HW_ERROR; r = alm_guardar(clave, d, n); nv_volcar(); return r == ALM_OK ? HW_OK : HW_ERROR; }
int hw_olvidar(u16 clave) { int r; if (!nv_hay || pc_prot) return HW_NO_DISPONIBLE; if (clave_vedada(clave)) return HW_ERROR; r = alm_borrar(clave); nv_volcar(); return r == ALM_OK ? HW_OK : HW_ERROR; }
void hw_nv_estado(HwNvEstado *e)
{
    AlmEstado a;
    alm_estado(&a);
    *e = (HwNvEstado){ .montado = (u8)nv_hay, .escribible = (u8)(nv_hay && !pc_prot), .bloques = a.bloques, .tam_bloque = a.tam_bloque, .usado = a.usado, .vivos = a.vivos,
        .claves = a.claves, .generacion = a.generacion, .borrados = (u32)pc_nor.n_borr, .programas = (u32)pc_nor.n_prog, .errores = a.errores + (u32)pc_nor.violaciones };
}
/* ---- la zona de ficheros de usuario (W6U): 8 bloques de 128 KiB en RAM, como la del teléfono ----------------
 * pc_nor.c es de una sola instancia (y es del almacén), así que esta zona tiene su propio modelo, más simple:
 * programar solo baja bits (y si se pide subir uno, falla y lo cuenta) y borrar deja el bloque a 0xFF. La
 * prueba de verdad del formato —cortes en cada operación sobre el modelo estricto— es herr/pruebas/
 * prueba_usuario.c, no esto. Con W600_USR=fichero se carga al arrancar y se guarda tras cada escritura. */
#define USR_BLQ 8u
#define USR_TAM 131072u
static u8 usr_mem[USR_BLQ * USR_TAM];
u32 pc_usr_violaciones;
static u8 pc_usr_vaciada;
static const u8 *usr_leer_blq(u32 b) { return usr_mem + (size_t)b * USR_TAM; }
static int usr_borrar_blq(u32 b) { pc_usr_escrituras++; if (b >= USR_BLQ) return -1; memset(usr_mem + (size_t)b * USR_TAM, 0xFF, USR_TAM); return 0; }
static int usr_programar(u32 b, u32 desplaz, const void *datos, u32 n)
{
    const u8 *d = datos; u8 *p = usr_mem + (size_t)b * USR_TAM + desplaz; u32 i;
    pc_usr_escrituras++;
    if (b >= USR_BLQ || (desplaz & 1) || desplaz >= USR_TAM || n > USR_TAM - desplaz) { pc_usr_violaciones++; return -1; }
    for (i = 0; i < n; i++) if ((p[i] & d[i]) != d[i]) { pc_usr_violaciones++; return -1; }
    memcpy(p, d, n);
    return 0;
}
const AlmPuerto *hwo_usr_puerto(int *puede_escribir)
{
    static const AlmPuerto pto = { USR_BLQ, USR_TAM, usr_leer_blq, usr_programar, usr_borrar_blq };
    const char *f = getenv("W600_USR"); FILE *o;
    if (pc_usr_vaciada) { pc_usr_vaciada = 0; *puede_escribir = !pc_prot; return &pto; }   /* (R2) recién vaciada: no se recarga del fichero */
    memset(usr_mem, 0xFF, sizeof usr_mem);
    if (f && (o = fopen(f, "rb"))) { if (fread(usr_mem, 1, sizeof usr_mem, o) != sizeof usr_mem) fprintf(stderr, "W600_USR: fichero corto\n"); fclose(o); }
    *puede_escribir = !pc_prot;                    /* (S7) flash protegida o modo seguro: solo lectura */
    if (pc_seguro) { fprintf(stderr, "usr: MODO SEGURO: Mis archivos NO se monta (ni se lee)\n"); return 0; }   /* (R2; M7 de la auditoría de S7), como el teléfono */
    return &pto;
}
void hwo_usr_tras_escribir(void); void hw_usr_cancelar(void);
int hw_borrar_usuario(void)                        /* (R2) el segundo rescate del modo seguro */
{
    u32 b;
    if ((pc_prot & ~HW_PROT_SEGURO) || (pc_prot && !pc_seguro)) return HW_NO_DISPONIBLE;   /* como el teléfono: también en modo normal y montado */
    fprintf(stderr, pc_seguro ? "usr: BORRADO de Mis archivos pedido por el usuario (modo seguro)\n" : "usr: VACIADO de Mis archivos pedido por el usuario\n");
    hw_usr_cancelar();
    for (b = 0; b < USR_BLQ; b++) usr_borrar_blq(b);
    hwo_usr_tras_escribir();
    if (!pc_seguro) { pc_usr_vaciada = 1; return hw_usr_iniciar() == HW_OK ? HW_OK : HW_ERROR; }
    return HW_OK;
}
void hwo_usr_tras_escribir(void)
{
    const char *f = getenv("W600_USR"); FILE *o;
    if (f && (o = fopen(f, "wb"))) { fwrite(usr_mem, 1, sizeof usr_mem, o); fclose(o); }
}

/* ---- (I1) LA MEMORIA (W6F): 88 bloques (antes 64) de 256 KiB en RAM, como la zona del teléfono (modo objeto: 256 páginas de 1 KiB) --------
 * El modelo hace cumplir lo que W6F le pide a la flash: programar una página SOLO si está en blanco y entera (si no, falla y lo
 * cuenta en pc_w6f_violaciones), borrar deja el bloque a 0xFF. La prueba de verdad (cortes, bits marginales, fallos) es
 * make probar-w6f (las pruebas de fs2/ contra su modelo), no esto. Como la imagen de la flash del emulador (herr/w6f_imagen.c),
 * la zona EMPIEZA FORMATEADA ya; con PC_W6F_VIRGEN=1, en blanco (el shell da formato), con PC_W6F_AJENO=1 llena de «restos»
 * (el shell pregunta). Con W600_W6F=fichero se carga al arrancar y se guarda tras cada escritura. */
#define W6F_BLQ 88u   /* (INT2) como el teléfono desde 2026-10-14: 88 bloques (hal.h, 0x44500000..0x45AFFFFF); antes 64. Con 64 el w6f_vacio.bin de 88 era «otra configuración» y probar-memoria fallaba */
#define W6F_TAM (W6F_SIS_PAG * W6F_SIS_PPB)
static u8 *w6f_mem;
u32 pc_w6f_violaciones, pc_w6f_programas, pc_w6f_borrados;
static int pw_leer(void *c, uint32_t b, uint32_t pg, uint32_t off, void *buf, uint32_t n)
{
    (void)c;
    if (b >= W6F_BLQ || pg >= W6F_SIS_PPB || off > W6F_SIS_PAG || n > W6F_SIS_PAG - off) { pc_w6f_violaciones++; return -1; }
    memcpy(buf, w6f_mem + (size_t)b * W6F_TAM + pg * W6F_SIS_PAG + off, n);
    return 0;
}
static int pw_prog(void *c, uint32_t b, uint32_t pg, const uint8_t *d)
{
    u8 *p; u32 i;
    (void)c;
    if (pc_prot) return -1;
    if (b >= W6F_BLQ || pg >= W6F_SIS_PPB) { pc_w6f_violaciones++; return -1; }
    p = w6f_mem + (size_t)b * W6F_TAM + pg * W6F_SIS_PAG;
    for (i = 0; i < W6F_SIS_PAG; i++) if (p[i] != 0xFF) { pc_w6f_violaciones++; fprintf(stderr, "w6f: VIOLACION: programar la pagina %u del bloque %u, que no esta en blanco\n", pg, b); return -1; }
    memcpy(p, d, W6F_SIS_PAG); pc_w6f_programas++;
    return 0;
}
static int pw_borrar(void *c, uint32_t b)
{
    (void)c;
    if (pc_prot) return -1;
    if (b >= W6F_BLQ) { pc_w6f_violaciones++; return -1; }
    memset(w6f_mem + (size_t)b * W6F_TAM, 0xFF, W6F_TAM); pc_w6f_borrados++;
    return 0;
}
static W6fFlash w6f_fl = { 0, W6F_FL_NOR_OBJETO, W6F_BLQ, W6F_SIS_PAG, W6F_SIS_PPB, pw_leer, pw_prog, pw_borrar, 0, 0 };
const W6fFlash *hwo_w6f_flash(int *puede_escribir)
{
    const char *f = getenv("W600_W6F"); FILE *o;
    if (!w6f_mem && !(w6f_mem = malloc((size_t)W6F_BLQ * W6F_TAM))) { fprintf(stderr, "w6f: sin memoria\n"); return 0; }
    memset(w6f_mem, 0xFF, (size_t)W6F_BLQ * W6F_TAM);
    if (f && (o = fopen(f, "rb"))) { if (fread(w6f_mem, 1, (size_t)W6F_BLQ * W6F_TAM, o) != (size_t)W6F_BLQ * W6F_TAM) fprintf(stderr, "W600_W6F: fichero corto\n"); fclose(o); }
    else if (getenv("PC_W6F_AJENO")) { size_t i; for (i = 0; i < (size_t)W6F_BLQ * W6F_TAM; i++) w6f_mem[i] = (u8)(i * 13 + 5); }
    else if (!getenv("PC_W6F_VIRGEN")) {           /* la imagen «de fábrica» ya formateada, como la de la flash del emulador */
        static uint32_t tmp[W6F_SIS_MEM_U32]; W6fConf cf = w6f_sis_conf(0);
        if (w6f_formatear(&w6f_fl, &cf, tmp, sizeof tmp, 0, 0) != W6F_OK) fprintf(stderr, "w6f: la imagen de fabrica no se pudo formatear\n");
        pc_w6f_programas = pc_w6f_borrados = 0;
    }
    *puede_escribir = !pc_prot;
    if (pc_seguro) { fprintf(stderr, "w6f: MODO SEGURO: la Memoria NO se monta (ni se lee)\n"); return 0; }
    return &w6f_fl;
}
int hwo_w6f_fallo_de_prueba(void) { return getenv("PC_W6F_FALLO") != NULL; }   /* (AUD7 M1) */
void hwo_w6f_tras_escribir(void)
{
    const char *f = getenv("W600_W6F"); FILE *o;
    if (f && w6f_mem && (o = fopen(f, "wb"))) { fwrite(w6f_mem, 1, (size_t)W6F_BLQ * W6F_TAM, o); fclose(o); }
}

/* El WFI del teléfono: la CPU se para hasta el tic siguiente, que llega como tarde en 4 ms. Aquí es lo
 * que hace avanzar el reloj virtual cuando NO manda el bucle del guion: dentro de una app externa a
 * pantalla completa (docs/sistema.md §11) o en una espera del propio sistema. La «bomba» la pone
 * pc_main.c: es lo que sigue leyendo el guion mientras la app tiene el bucle. */
static void (*bomba)(void);
void pc_bomba(void (*f)(void)) { bomba = f; }
void hw_reposo(void) { pc_reloj_ms += 4; if (bomba) bomba(); }
/* En el PC no hay nada que dormir: se adelanta el reloj virtual lo pedido y se cuenta. El bucle de
 * pc_main.c NO llama aquí (mueve él el reloj de 4 en 4 ms para que las capturas sean deterministas):
 * lo que los guiones prueban de verdad es la MÁQUINA DE ESTADOS de energía (shell/energia.c), que va
 * por hw_ms(); el reposo profundo de verdad solo se prueba en el emulador. */
u32 pc_dormidas, pc_ms_dormido;
u32 hw_dormir(u32 max_ms)
{
    if (max_ms > 1900) max_ms = 1900;
    if (max_ms < 20) max_ms = 20;
    pc_reloj_ms += max_ms;
    pc_dormidas++; pc_ms_dormido += max_ms;
    return max_ms;
}
/* En el PC un pánico tiene que HACER FALLAR la prueba: se imprime y se aborta (ASan deja la traza). */
int pc_azar;                                       /* pc_main.c: se están dando teclas al azar */
void hw_panico(const char *titulo, const char *detalle)
{
    /* La pantalla de fallo DE PRUEBA (Sistema -> Opciones) pedida por el mono de las teclas al azar no es un fallo:
     * no puede tumbar la prueba. Salió en la pista S7: al cambiar el menú, la secuencia al azar llegó hasta ahí y
     * «make probar» abortaba. Un pánico de verdad (montículo agotado…) sigue abortando. */
    if (pc_azar && titulo && !strncmp(titulo, "PRUEBA", 6)) { fprintf(stderr, "(azar: pantalla de fallo de prueba pedida; se sigue)\n"); return; }
    fprintf(stderr, "\n*** PANICO: %s / %s\n", titulo ? titulo : "", detalle ? detalle : "");
    fflush(stderr);
    abort();
}
void hw_salud(HwSalud *s)
{
    memset(s, 0, sizeof *s);                       /* las pilas no se vigilan en el PC: las pone el sistema operativo */
    s->dormidas = pc_dormidas; s->ms_dormido = pc_ms_dormido;
}
int  hw_reiniciar(void) { return HW_NO_DISPONIBLE; }
void hw_entrar_bl(void) { hw_log("[pc] hw_entrar_bl: en el teléfono, carga fuera, bandera BLOA y reinicio_caliente\n"); printf("hw_entrar_bl\n"); }   /* (Fase 3b) en el PC no hay a dónde ir */
/* (S7) «apagable 1» en el guion: hw_apagar apaga (el guion se acaba ahí); «apagable 2»: 0x41 <- 0 NO apaga (como puede pasar con el
 * cargador puesto) y vuelve con HW_ERROR, con el panel dormido; sin nada, HW_NO_DISPONIBLE como siempre. */
static int pc_mv(void);
int  hw_apagar(void)
{
    if (pc_apagable == 1) { pc_apagado = 1; printf("hw_apagar: el telefono SE APAGA (t=%u ms, bateria %d mV, cargador %d)\n", pc_reloj_ms, pc_mv(), pc_cargador_bits); return HW_OK; }
    if (pc_apagable == 2) { pc_apagados_fallidos++; printf("hw_apagar: 41 <- 0 y el telefono SIGUE VIVO (t=%u ms, cargador %d)\n", pc_reloj_ms, pc_cargador_bits); return HW_ERROR; }
    return HW_NO_DISPONIBLE;
}
void hw_log(const char *s) { fputs(s, stderr); }
u32  hw_cuenta(void) { return 0; }
void hw_info(HwInfo *i)
{
    memset(i, 0, sizeof *i);
    i->version = HW_VERSION_SISTEMA " (PC)"; i->plataforma = "PC (simulado)"; i->panel = 0; i->panel_r28 = -1; i->cpu_mhz = 0; i->ram_total = 16u << 20;
}
/* (estética, giro) en el PC el «panel» guarda el cuadro LÓGICO tal cual (paso = su ancho): es lo que compara el detector de desajustes
 * y lo que sale en las capturas, derecho, como lo ve quien sujeta el teléfono. El giro de verdad lo hace el PDICON del teléfono. */
int pc_giro;
int hw_pantalla_girar(int g) { pc_giro = g & 3; return HW_OK; }
void hw_pantalla_volcar(const u16 *c, int y0, int filas)
{
    const int an = pc_giro & 1 ? PLACA_ALTO : PLACA_ANCHO;
    memcpy(pc_panel + y0 * an, c + y0 * an, (size_t)filas * an * sizeof(u16));
    pc_volcados++; pc_filas_volcadas += (u32)filas;
}
/* ---- LAS LUCES, SIMULADAS (pista S5) -----------------------------------------------------------------------
 * Lo que el teléfono tendría escrito en el chip de LED y en los PWM, llevado a mano: las seis luces de
 * docs/luces.md con su nivel de registro, las dos ternas con sus campos y el PWM del chip con su rampa (el mismo
 * modelo [inferido] que usa el emulador). Las FASES (hw_luz_fase) dejan aquí el estado FINAL de cada una, sin
 * rampas (las rampas de verdad son de plat/hw_w580.c y se miran en el emulador); los EFECTOS sí van orden a orden,
 * porque el motor (api/luces.c) es el mismo código que en el teléfono. El guion lo vuelca con «luces». */
static struct {
    u8 nivel[HW_LEDS];
    u8 rgb_obj[2][3], rgb_act[2][3], rgb_modo[2], rgb_sent[2];
    u8 pwm_sentido, pwm_vel;             /* 0 = fijo en FF, 1 = subiendo desde 0, 2 = bajando desde FF */
    u32 pwm_t0;
    u8 efecto, color[3];
    u32 escrituras;
} sl = { .color = { 255, 255, 255 } };
static int sl_nivel_brillo(int pct) { return pct <= 50 ? 0x06 + (0x2F - 0x06) * pct / 50 : 0x2F + (0x84 - 0x2F) * (pct - 50) / 50; }   /* como plat/hw_w580.c */
static void sl_terna(int t, int r, int g, int b, int modo)
{
    sl.rgb_obj[t][0] = sl.rgb_act[t][0] = (u8)r; sl.rgb_obj[t][1] = sl.rgb_act[t][1] = (u8)g; sl.rgb_obj[t][2] = sl.rgb_act[t][2] = (u8)b;
    sl.rgb_modo[t] = (u8)modo; sl.rgb_sent[t] = 0;
}
static void sl_montar_fase(void)
{
    int viva = luz_fase == HW_LUZ_VIVA, t;
    sl.nivel[HW_LED_PANTALLA] = (u8)(luz_fase == HW_LUZ_LOGO ? 0x84 : viva ? sl_nivel_brillo(luz_puesta[0]) : luz_fase == HW_LUZ_APAGADA ? 0 : 0x06);
    sl.nivel[HW_LED_TECLADO_1] = sl.nivel[HW_LED_TECLADO_2] = (u8)(viva ? 5 : 0);
    sl.nivel[HW_LED_TECLADO_DESL] = (u8)(viva && !pc_tapa_cerrada ? 0xFF * 36 / 100 : 0);
    for (t = 0; t < 2; t++) {
        if (viva && (sl.color[0] || sl.color[1] || sl.color[2])) sl_terna(t, sl.color[0], sl.color[1], sl.color[2], 2);
        else sl_terna(t, 0, 0, 0, 0);
    }
    sl.pwm_sentido = 0;
}
int hw_luz(int n, int pct)
{
    if (n < 0 || n >= HW_LUCES || pct < 0 || pct > 100) return HW_ERROR;
    luz_puesta[n] = (signed char)pct;
    if (n == 0) { if (!pct) luz_fase = HW_LUZ_APAGADA; else if (luz_fase == HW_LUZ_APAGADA) luz_fase = HW_LUZ_VIVA; if (!sl.efecto) sl_montar_fase(); }
    return HW_OK;
}
int hw_luz_estado(int n) { return n < 0 || n >= HW_LUCES ? HW_ERROR : luz_puesta[n]; }
/* (S4) la luz por fases: en el PC solo se apunta; (S5) y se deja el estado final de la fase en las luces simuladas */
void hw_luz_fase(int fase)
{
    if (fase < HW_LUZ_APAGADA || fase > HW_LUZ_DORMIDA || fase == luz_fase) return;
    luz_fase = fase;
    if (fase != HW_LUZ_VIVA && fase != HW_LUZ_LOGO) sl.efecto = 0;      /* el efecto pierde las luces */
    if (!sl.efecto) sl_montar_fase();
}
int hw_luz_fase_actual(void) { return luz_fase; }
int hw_luz_salida_max(int led) { static const u8 m[HW_LEDS] = { 0x84, 0xFF, 0x10, 0x10 }; return led < 0 || led >= HW_LEDS ? 0 : m[led]; }
int hw_luz_salida(int led, int nivel)
{
    if (led < 0 || led >= HW_LEDS || nivel < 0) return HW_ERROR;
    if (!sl.efecto) return HW_NO_DISPONIBLE;
    if (nivel > hw_luz_salida_max(led)) nivel = hw_luz_salida_max(led);
    if (led == HW_LED_TECLADO_DESL && pc_tapa_cerrada) nivel = 0;       /* solo con el deslizador abierto */
    if (sl.nivel[led] != nivel) sl.escrituras++;
    sl.nivel[led] = (u8)nivel;
    return HW_OK;
}
int hw_luz_salida_nivel(int led) { return led < 0 || led >= HW_LEDS ? 0 : sl.nivel[led]; }
static int sl_rgb_vale(void) { return sl.efecto || luz_fase == HW_LUZ_VIVA; }
int hw_luz_rgb_fundir(int t, int r, int g, int b, int vel, int bajar)       /* las mismas reglas que hal/i2c.c: ledc_grupo_fundir */
{
    int cero, igual;
    if (t < 0 || t > 1 || (!r && !g && !b)) return HW_ERROR;
    if (!sl_rgb_vale()) return HW_NO_DISPONIBLE;
    cero = !sl.rgb_act[t][0] && !sl.rgb_act[t][1] && !sl.rgb_act[t][2];
    igual = sl.rgb_act[t][0] == r && sl.rgb_act[t][1] == g && sl.rgb_act[t][2] == b;
    sl.rgb_obj[t][0] = (u8)r; sl.rgb_obj[t][1] = (u8)g; sl.rgb_obj[t][2] = (u8)b;
    bajar = bajar != 0;
    if ((igual && sl.rgb_sent[t] == bajar) || (cero && bajar)) { sl.rgb_sent[t] = (u8)bajar; return HW_OK; }
    sl.rgb_act[t][0] = (u8)r; sl.rgb_act[t][1] = (u8)g; sl.rgb_act[t][2] = (u8)b;
    sl.rgb_sent[t] = (u8)bajar; sl.rgb_modo[t] = 2;
    sl.pwm_sentido = (u8)(bajar ? 2 : 1); sl.pwm_vel = (u8)(vel & 7); sl.pwm_t0 = pc_reloj_ms;
    sl.escrituras += 4;
    return HW_OK;
}
int hw_luz_rgb_fijo(int t, int r, int g, int b, int modo)
{
    if (t < 0 || t > 1 || (!r && !g && !b) || modo < 1 || modo > 3) return HW_ERROR;
    if (!sl_rgb_vale()) return HW_NO_DISPONIBLE;
    sl_terna(t, r, g, b, modo);
    sl.pwm_sentido = 0;
    sl.escrituras += 2;
    return HW_OK;
}
int hw_luz_rgb_apagar(int t)
{
    if (t < 0 || t > 1) return HW_ERROR;
    if (sl.rgb_obj[t][0] || sl.rgb_obj[t][1] || sl.rgb_obj[t][2]) sl.escrituras += 2;
    sl_terna(t, 0, 0, 0, 0);
    return HW_OK;
}
int hw_luz_efecto(int si)
{
    if (si == 1) sl.efecto = (u8)(luz_fase == HW_LUZ_VIVA || luz_fase == HW_LUZ_LOGO);
    else if (si == 0 && sl.efecto) { sl.efecto = 0; sl_montar_fase(); }
    return sl.efecto;
}
void hw_luz_color(int r, int g, int b)
{
    sl.color[0] = (u8)r; sl.color[1] = (u8)g; sl.color[2] = (u8)b;
    if (!sl.efecto && luz_fase == HW_LUZ_VIVA) {
        int t;
        for (t = 0; t < 2; t++) { if (r || g || b) hw_luz_rgb_fundir(t, r, g, b, 2, 0); else hw_luz_rgb_apagar(t); }
    }
}
void hw_luz_leer(HwLuces *e)
{
    extern u32 lu_pwm_modelo(u32 vel, int sentido, u32 ms);
    int t, c;
    memset(e, 0, sizeof *e);
    e->presente = 1; e->fase = (u8)luz_fase; e->efecto = sl.efecto;
    memcpy(e->nivel, sl.nivel, sizeof e->nivel);
    e->pwm = (u8)lu_pwm_modelo(sl.pwm_vel, sl.pwm_sentido, pc_reloj_ms - sl.pwm_t0);
    for (t = 0; t < 2; t++) for (c = 0; c < 3; c++) {
        e->rgb_nivel[t][c] = sl.rgb_act[t][c];
        e->rgb[t][c] = (u8)(sl.rgb_modo[t] == 2 ? sl.rgb_act[t][c] * e->pwm / 255 : sl.rgb_modo[t] ? sl.rgb_act[t][c] : 0);
    }
    e->escrituras = sl.escrituras;
}

int hw_tecla_evento(void)
{
    int k, i;
    for (i = 1; i < K_N && !n_cola; i++)                      /* repetición de flechas y C mantenidas */
        if (abajo[i] && ((i >= K_ARRIBA && i <= K_DER) || i == K_C) && pc_reloj_ms - abajo_desde[i] >= 400) { abajo_desde[i] = pc_reloj_ms - 280; pc_tecla(i); }
    if (!n_cola) return 0;
    k = cola[0]; memmove(cola, cola + 1, (size_t)--n_cola * sizeof cola[0]);
    return k;
}
int hw_tecla_abajo(int k) { return k > 0 && k < K_N && abajo[k]; }
int hw_teclas_mapa(u8 col[7])
{
    int c, n = 0;
    memset(col, 0, 7);
    for (c = 0; c < 35; c++) if (hw_celda_tecla(c) && abajo[hw_celda_tecla(c)]) { col[c / 5] |= (u8)(1u << (c % 5)); n++; }
    return n;
}

u32 hw_ms(void) { return pc_reloj_ms; }
/* RTC simulado: arranca el lunes 12 de octubre de 2026 a las 12:00:00 y avanza con el tiempo virtual */
static HwFecha base = { 2026, 10, 12, 12, 0, 0 }; static u32 base_ms;
int hw_rtc_leer(HwFecha *f)
{
    u32 s = (pc_reloj_ms - base_ms) / 1000 + base.seg + base.min * 60u + base.hora * 3600u, dias = s / 86400;
    *f = base;
    s %= 86400; f->hora = (u8)(s / 3600); f->min = (u8)(s / 60 % 60); f->seg = (u8)(s % 60);
    while (dias--) if (++f->dia > hw_dias_mes(f->anio, f->mes)) { f->dia = 1; if (++f->mes > 12) { f->mes = 1; f->anio++; } }
    return HW_OK;
}
int hw_rtc_alarma(int hora, int min) { (void)hora; (void)min; return HW_OK; }
int hw_rtc_alarma_salto(void) { return 0; }
int hw_rtc_poner(const HwFecha *f)
{
    if (f->anio < 1970 || f->anio > 2099 || f->mes < 1 || f->mes > 12 || f->dia < 1 || f->dia > hw_dias_mes(f->anio, f->mes) ||
        f->hora > 23 || f->min > 59 || f->seg > 59) return HW_ERROR;
    base = *f; base_ms = pc_reloj_ms;
    return HW_OK;
}

/* El FS «original» en el PC, dos maneras:
 *  - W580_FSFAB=fichero : la región cruda del FS de fábrica del W580 (work/fs/…/crudo/region_45e00000.bin, o
 *    cualquier volcado de 0x45E00000..) leída con EL MISMO lector que el teléfono (api/fsfab.c);
 *  - si no, W600_FS=directorio : un árbol ya extraído (como en el W600). Sin ninguna de las dos, no hay. */
#include "fsfab.h"
static int fab_listo(void)
{
    static int mirado; static u8 *img;
    const char *f = getenv("W580_FSFAB"); FILE *o; long n;
    if (!mirado) {
        mirado = 1;
        if (f && (o = fopen(f, "rb"))) {
            fseek(o, 0, SEEK_END); n = ftell(o); rewind(o);
            img = n > 0 ? malloc((size_t)n + 1) : NULL;   /* (R3; calidad) ftell puede dar -1 y malloc NULL: antes no se miraba */
            if (img && fread(img, 1, (size_t)n, o) == (size_t)n) ff_montar(img, (u32)n);
            fclose(o);
        }
    }
    return ff_listo();
}
static const char *real(const char *ruta)
{
    static char r[700];
    const char *raiz = getenv("W600_FS");
    snprintf(r, sizeof r, "%s%s", raiz ? raiz : "../work/fs/AMERICA_1/arbol", ruta);
    return r;
}
static int dir_listo(void) { struct stat st; return stat(real("/"), &st) == 0; }
static int cmp(const void *a, const void *b) { return strcmp(((const HwEntrada *)a)->nombre, ((const HwEntrada *)b)->nombre); }
static int dir_listar(const char *dir, u32 desde, HwEntrada *e, u32 max)
{
    static HwEntrada todo[2048]; static char de[600];             /* caché del último directorio */
    static int n = -1;
    u32 i, k = 0;
    if (n < 0 || strcmp(de, dir)) {
        DIR *d = opendir(real(dir)); struct dirent *x;
        if (!d) return HW_ERROR;
        n = 0; snprintf(de, sizeof de, "%s", dir);
        while ((x = readdir(d)) && n < 2048) {
            char r[1100]; struct stat st;
            if (x->d_name[0] == '.' && (!x->d_name[1] || (x->d_name[1] == '.' && !x->d_name[2]))) continue;
            snprintf(r, sizeof r, "%s/%s", real(dir), x->d_name);
            if (stat(r, &st)) continue;
            snprintf(todo[n].nombre, sizeof todo[n].nombre, "%.63s", x->d_name);
            todo[n].tam = (u32)st.st_size; todo[n].es_dir = S_ISDIR(st.st_mode);
            if (todo[n].es_dir) todo[n].tam = 0;
            n++;
        }
        closedir(d);
        qsort(todo, (size_t)n, sizeof todo[0], cmp);
    }
    if (!e) return n;
    for (i = desde; i < (u32)n && k < max; i++) e[k++] = todo[i];
    return (int)k;
}
static u32 dir_tam(const char *ruta)
{
    struct stat st;
    return (stat(real(ruta), &st) || S_ISDIR(st.st_mode)) ? 0 : (u32)st.st_size;
}
static u32 dir_leer_desde(const char *ruta, u32 desde, u8 *buf, u32 max)
{
    FILE *f; u32 n;
    if (!dir_tam(ruta) || !(f = fopen(real(ruta), "rb"))) return 0;
    n = fseek(f, (long)desde, SEEK_SET) ? 0 : (u32)fread(buf, 1, max, f);
    fclose(f);
    return n;
}
static u32 dir_leer(const char *ruta, u8 *buf, u32 max) { return dir_leer_desde(ruta, 0, buf, max); }

int hwo_fs_listo(void) { return fab_listo() || (!getenv("W580_FSFAB") && dir_listo()); }
int hwo_fs_listar(const char *dir, u32 desde, HwEntrada *e, u32 max) { return fab_listo() ? ff_listar(dir, desde, e, max) : dir_listar(dir, desde, e, max); }
int hwo_fs_contar(const char *dir) { return hwo_fs_listar(dir, 0, 0, 0); }
u32 hwo_fs_tam(const char *ruta) { return fab_listo() ? ff_tam(ruta) : dir_tam(ruta); }
u32 hwo_fs_leer(const char *ruta, u8 *buf, u32 max) { return fab_listo() ? ff_leer(ruta, buf, max) : dir_leer(ruta, buf, max); }
u32 hwo_fs_leer_desde(const char *ruta, u32 desde, u8 *buf, u32 max) { return fab_listo() ? ff_leer_desde(ruta, desde, buf, max) : dir_leer_desde(ruta, desde, buf, max); }
void hwo_fs_estado(HwFsEstado *e)
{
    memset(e, 0, sizeof *e); e->montado = (u8)hwo_fs_listo();
    if (fab_listo()) { const FfEstado *f = ff_estado(); e->bloques = f->validos; e->cabeceras = f->cabeceras; e->lecturas_nor = f->lecturas; e->errores = f->cabeceras_malas + f->desconocidos + f->fuera; }
}

const u8 *hwo_w6fs(int i, u32 *max)                 /* W600_W6FS = imagen hecha con herr/hacer_fs.py */
{
    static u8 *img; static u32 n; static int mirado;
    const char *f = getenv("W600_W6FS"); FILE *o;
    if (i) return NULL;
    if (!mirado) {
        mirado = 1;
        if (f && (o = fopen(f, "rb"))) { fseek(o, 0, SEEK_END); n = (u32)ftell(o); rewind(o); img = malloc(n ? n : 1); if (fread(img, 1, n, o) != n) n = 0; fclose(o); }
    }
    *max = n;
    return img;
}
/* Energía simulada: el guion la mueve con «bateria N» (N <= 100: un porcentaje; si no, MILIVOLTIOS), «descarga MV_POR_S» (baja sola
 * con el reloj virtual), «cargador 0|1|2», «apagable 0|1|2» y «tapa abierta|cerrada», así que la barra de estado, el aviso de batería
 * baja, el apagado y la tapa se pueden probar con capturas en el PC. (S7) La batería va en mV y el % sale de la MISMA tabla que en el
 * teléfono (api/bat.h). */
/* (R2) INYECCIÓN DE FALLOS para las pruebas negativas de la auditoría de S7 (herr/probar_seguridad_s8.sh), por el entorno:
 *   PC_BAT_FALLA=1 la lectura de la batería falla SIEMPRE (el PMU no contesta) · =2 falla un periodo de 10 s de cada dos
 *   PC_BAT_INICIAL=mV lo que marca la batería desde el primer sondeo   · PC_CARGA_FALLA=1 0x61 no se relee como 10 (queda 13)
 *   PC_ENC_ATASCADA=1 la señal de la tecla de encendido está «pulsada» desde el arranque y no se suelta nunca */
int pc_bateria_mv = 3925, pc_cargador_bits, pc_tapa_cerrada, pc_descarga_mv_s;
static int pc_env(const char *n) { const char *v = getenv(n); return v ? atoi(v) : 0; }
static int pc_bat_falla(void) { int f = pc_env("PC_BAT_FALLA"); return f == 1 || (f == 2 && (pc_reloj_ms / 10000u) % 2u); }
static u32 pc_descarga_desde;
u32 pc_tapa_cambios;
void pc_bateria(int n)
{
    if (n > 100) { pc_bateria_mv = n; return; }
    for (pc_bateria_mv = 3400; pc_bateria_mv < 4155 && bat_pct(pc_bateria_mv) < n; pc_bateria_mv++) ;
}
void pc_descarga(int mv_por_s) { pc_bateria_mv = pc_mv(); pc_descarga_mv_s = mv_por_s; pc_descarga_desde = pc_reloj_ms; }
static int pc_mv(void)
{
    static int puesta; int v;
    if (!puesta) { puesta = 1; if (pc_env("PC_BAT_INICIAL")) pc_bateria_mv = pc_env("PC_BAT_INICIAL"); }
    v = pc_bateria_mv - (int)((pc_reloj_ms - pc_descarga_desde) / 1000u) * pc_descarga_mv_s; return v < 2800 ? 2800 : v;
}
int hw_bateria_mv(void) { return pc_bat_falla() ? HW_NO_DISPONIBLE : pc_mv(); }
int hw_bateria_pct(void) { int mv = hw_bateria_mv(); return mv < 0 ? mv : bat_pct(mv); }
int hw_bateria_fuente(void) { return 0; }
int hw_bateria_presente(void) { return 1; }
int hw_cargador(void) { return pc_env("PC_BAT_FALLA") == 1 ? HW_NO_DISPONIBLE : pc_cargador_bits; }   /* sin PMU tampoco se ve el cargador */
int pc_cargas_vigiladas;
int hw_carga_vigilar(void) { pc_cargas_vigiladas++; return pc_env("PC_CARGA_FALLA") ? HW_ERROR : HW_OK; }
void hw_pmu_salud(HwPmuSalud *p)
{
    memset(p, 0, sizeof *p);
    p->carga_confirmada = !pc_env("PC_CARGA_FALLA"); p->r61 = p->carga_confirmada ? 0x10 : 0x13; p->r63 = 0;
    p->enc_foto_armada = 1;
}
/* La pulsación larga, con LAS MISMAS reglas que el teléfono (api/enc.h): la «foto» es la tecla del guion (o clavada, con
 * PC_ENC_ATASCADA); no hay nivel ni PMU. */
int hw_encendido_mantenida(void)
{
    static EncEstado e;
    enc_vigia(&e, abajo[K_ENCENDIDO] || pc_env("PC_ENC_ATASCADA"), -1);
    return enc_mantenida(&e, -1);
}
void hw_arranque(HwArranque *a) { *a = pc_arr; }
int hw_carga_fuera(void) { pc_cargas_fuera++; return HW_OK; }
static int pc_cuenta(void) { return (pc_mv() - 2755) * 1000 / 10843; }
int hw_pmu_diag(HwPmuDiag *d)                      /* lo que diría un PMU con la carga a 0: 60 = 88, 61 = 10, 62 = 63 = 00 */
{
    int c = pc_cuenta();
    *d = (HwPmuDiag){ .r23 = (short)((pc_cargador_bits & 1 ? 0x08 : 0) | (pc_cargador_bits & 2 ? 0x80 : 0)), .r60 = 0x88, .r61 = 0x10, .r62 = 0, .r63 = 0,
                      .r75 = (short)c, .r76 = (short)c, .r77 = (short)c, .r7a = 0, .l6a = (short)((pc_mv() - 2796) * 1000 / 10784),
                      .adc8 = (short)(pc_cargador_bits & 1 ? 200 : 0), .adc9 = 0, .adc1 = 60 };
    return HW_OK;
}
int hw_adc(int canal) { return canal == 10 && !pc_bat_falla() ? pc_cuenta() : HW_NO_DISPONIBLE; }
int hw_vibrar(int ms) { (void)ms; return HW_NO_DISPONIBLE; }
int hw_vibrar_patron(int hz, int pct, int ms) { (void)hz; (void)pct; (void)ms; return HW_NO_DISPONIBLE; }
/* ACELERÓMETRO simulado (W580i): el guion lo mueve con «acel X Y» (postura fija, en cuentas crudas) y «andar
 * DECIHZ AMPLITUD» (una marcha: seno de esa amplitud a DECIHZ/10 pasos por segundo sobre el eje Y, que lleva la
 * gravedad a +18 cuentas, y un balanceo pequeño en X), en función del reloj virtual. «andar 0 0» la para.
 * Como el de verdad, solo da lecturas buenas si está alimentado (y lo apunta, para que el guion lo pueda mirar). */
static int acel_x = 125, acel_y = 125 + 18, acel_dhz, acel_amp, acel_alim;
static u32 acel_desde;
u32 pc_acel_lecturas, pc_acel_sin_alimentar;
/* «senal FICHERO.csv»: una señal «t_ms,x,y» (las de la pista E2: notas internas del proyecto) reproducida en el
 * reloj virtual desde el instante de la orden, como hace la máquina de E2: interpolación lineal, redondeo, y al
 * acabar se queda en el último punto. */
static struct { float t, x, y; } *senal;
static int senal_n;
int pc_acel_senal(const char *ruta)
{
    FILE *f = fopen(ruta, "r"); char l[160]; int cap = 0; float t, x, y;
    free(senal); senal = NULL; senal_n = 0;
    if (!f) return -1;
    while (fgets(l, sizeof l, f)) {
        if (sscanf(l, "%f,%f,%f", &t, &x, &y) != 3) continue;
        if (senal_n == cap) { cap = cap ? cap * 2 : 4096; senal = realloc(senal, (size_t)cap * sizeof *senal); if (!senal) abort(); }
        senal[senal_n].t = t; senal[senal_n].x = x; senal[senal_n].y = y; senal_n++;
    }
    fclose(f);
    acel_desde = pc_reloj_ms; acel_dhz = acel_amp = 0;
    return senal_n;
}
static void senal_ahora(int *x, int *y)
{
    static int j;
    float t = (float)(pc_reloj_ms - acel_desde), fx, fy;
    if (j >= senal_n || senal[j].t > t) j = 0;
    while (j + 1 < senal_n && senal[j + 1].t <= t) j++;
    if (j + 1 >= senal_n || t <= senal[j].t) { fx = senal[j].x; fy = senal[j].y; }
    else { float a = (t - senal[j].t) / (senal[j + 1].t - senal[j].t); fx = senal[j].x + a * (senal[j + 1].x - senal[j].x); fy = senal[j].y + a * (senal[j + 1].y - senal[j].y); }
    *x = (int)floorf(fx + 0.5f); *y = (int)floorf(fy + 0.5f);
}
void pc_acel(int x, int y) { acel_x = x; acel_y = y; acel_dhz = acel_amp = 0; senal_n = 0; }
void pc_andar(int decihz, int amplitud) { acel_dhz = decihz; acel_amp = amplitud; acel_desde = pc_reloj_ms; if (decihz) { acel_x = 125; acel_y = 125 + 18; } }
int hw_acel_alimentar(int si) { acel_alim = si; return HW_OK; }
int hw_acel_leer(int *x, int *y)
{
    double t = (pc_reloj_ms - acel_desde) / 1000.0, f = acel_dhz / 10.0;
    pc_acel_lecturas++;
    if (!acel_alim) { pc_acel_sin_alimentar++; *x = *y = 0; return HW_OK; }       /* sin alimentar el ADC lee el suelo */
    if (senal_n) senal_ahora(x, y);
    else {
        *x = acel_x + (acel_dhz ? (int)lround(1.5 * sin(M_PI * f * t)) : 0);
        *y = acel_y + (acel_dhz ? (int)lround(acel_amp * sin(2 * M_PI * f * t)) : 0);
    }
    *x = *x < 0 ? 0 : *x > 255 ? 255 : *x;
    *y = *y < 0 ? 0 : *y > 255 ? 255 : *y;
    return HW_OK;
}
int hw_tecla_encendido(void) { return abajo[K_ENCENDIDO] || pc_env("PC_ENC_ATASCADA"); }
static int pantalla_dormida;
/* El panel dormido no refresca: para que una captura durante ese rato NO mienta (enseñando lo último
 * que se volcó), el panel simulado se pone a negro, como se ve un panel apagado. */
int hw_pantalla_dormir(int si)
{
    if (si && !pantalla_dormida) memset(pc_panel, 0, sizeof pc_panel);
    pantalla_dormida = si;
    return HW_OK;
}
int pc_pantalla_dormida(void) { return pantalla_dormida; }
void pc_tapa(int cerrada) { if (pc_tapa_cerrada != cerrada) pc_tapa_cambios++; pc_tapa_cerrada = cerrada; if (!sl.efecto) sl_montar_fase(); else if (cerrada) sl.nivel[HW_LED_TECLADO_DESL] = 0; }
#define PIN_MAL(p, n) ((p) < 0 || (p) > 4 || (n) < 0 || (n) > 7)
int hw_gpio_modo(int p, int pin, int salida) { (void)p; (void)pin; (void)salida; return HW_ERROR; }   /* (R2) como el teléfono: la lista blanca de pines de usuario está vacía */
int hw_gpio_poner(int p, int pin, int v) { (void)p; (void)pin; (void)v; return HW_ERROR; }
int hw_gpio_leer(int p, int pin) { if (PIN_MAL(p, pin)) return HW_ERROR; return (gpio_dat[p] >> pin) & 1; }
int hw_tapa(void) { return !pc_tapa_cerrada; }
int hw_tapa_cerrada(void) { return pc_tapa_cerrada; }
u32 hw_tapa_cambios(void) { return pc_tapa_cambios; }
int hw_i2c_escribir(int dir7, const u8 *d, int n) { (void)dir7; (void)d; (void)n; return HW_NO_DISPONIBLE; }
int hw_i2c_leer(int dir7, int reg, u8 *d, int n) { (void)dir7; (void)reg; (void)d; (void)n; return HW_NO_DISPONIBLE; }
int hw_serie_conectado(void) { return 1; }
int hw_serie_escribir(const u8 *d, int n) { int i; for (i = 0; i < n && n_tx < (int)sizeof tx - 1; i++) tx[n_tx++] = (char)d[i]; return n; }
int hw_serie_leer(u8 *d, int max)
{
    int n = n_rx < max ? n_rx : max;
    memcpy(d, rx, (size_t)n); memmove(rx, rx + n, (size_t)(n_rx - n)); n_rx -= n;
    return n;
}
/* ---- SONIDO simulado (pista S5) --------------------------------------------------------------------------
 * El PC no tiene zumbador ni DSP, pero el servicio de sonido del shell (shell/sonido.c) es portable, así
 * que aquí se le da un hardware de mentira que (a) apunta cada cambio del generador en una traza que el
 * guion puede imprimir y (b) escribe los mismos dos WAV que el emulador de la pista S4, con el mismo
 * modelo: onda cuadrada de amplitud fija a 32 kHz para el zumbador, y el PCM tal cual se entrega.
 *
 * AVISO: el WAV del PC NO prueba que el teléfono suene — lo que prueba es la LÓGICA (qué notas, en qué
 * orden, cuánto duran, con qué ancho de pulso). La prueba de que el camino de registros produce sonido es
 * el WAV del EMULADOR (audio/evidencia/ y sistema/evidencia/w600_sonido/), donde la onda sale de las
 * escrituras de verdad a 0xF9020000 y de la ruta de P0.7. */
#define PC_TONO_RATE 32000u
#define PC_TONO_AMPL 16384
static FILE *wav_tono, *wav_pcm;
static u32 wav_tono_marcos, wav_pcm_marcos, wav_pcm_rate, wav_pcm_canales;
static u32 tono_hz, tono_ciclo, tono_desde_ms;     /* lo que suena AHORA y desde cuándo */
static u32 tono_fase_ciclos;                       /* ciclos de 13 MHz ya volcados (la fase no se pierde) */
char pc_tono_traza[4096];
static int n_traza;
u32 pc_tono_marcos, pc_tono_sonando_marcos;

static void wav_cabecera(FILE *f, u32 marcos, u32 rate, u32 canales)
{
    unsigned char h[44] = {0};
    u32 bytes = marcos * canales * 2, bps = rate * canales * 2;
    long pos = ftell(f);
    memcpy(h, "RIFF", 4); h[4] = (unsigned char)(36 + bytes); h[5] = (unsigned char)((36 + bytes) >> 8);
    h[6] = (unsigned char)((36 + bytes) >> 16); h[7] = (unsigned char)((36 + bytes) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8); h[16] = 16;
    h[20] = 1; h[22] = (unsigned char)canales;
    h[24] = (unsigned char)rate; h[25] = (unsigned char)(rate >> 8); h[26] = (unsigned char)(rate >> 16);
    h[28] = (unsigned char)bps; h[29] = (unsigned char)(bps >> 8); h[30] = (unsigned char)(bps >> 16);
    h[32] = (unsigned char)(canales * 2); h[34] = 16;
    memcpy(h + 36, "data", 4);
    h[40] = (unsigned char)bytes; h[41] = (unsigned char)(bytes >> 8);
    h[42] = (unsigned char)(bytes >> 16); h[43] = (unsigned char)(bytes >> 24);
    if (fseek(f, 0, SEEK_SET) == 0) fwrite(h, 1, sizeof h, f);
    if (pos > 0) fseek(f, pos, SEEK_SET);
    fflush(f);
}
static FILE *wav_abrir(const char *var, u32 rate, u32 canales)
{
    const char *r = getenv(var); FILE *f;
    unsigned char ceros[44] = {0};
    if (!r || !*r || !(f = fopen(r, "w+b"))) return NULL;
    fwrite(ceros, 1, sizeof ceros, f);
    wav_cabecera(f, 0, rate, canales);
    return f;
}
/* Vuelca al WAV del zumbador lo que ha sonado (o callado) hasta `hasta_ms` del reloj virtual. */
static u32 tono_hasta_ms;                          /* 0, o cuándo se apaga solo el tono (hw_tono_ms) */
static void traza(const char *q, u32 hz, u32 ciclo);
void pc_sonido_avanzar(u32 hasta_ms)
{
    u32 marcos, k;
    if (tono_hz && tono_hasta_ms && (i32)(hasta_ms - tono_hasta_ms) >= 0) {   /* el plazo venció por el camino */
        u32 fin = tono_hasta_ms, ahora = pc_reloj_ms;
        tono_hasta_ms = 0;
        pc_sonido_avanzar(fin);                    /* suena hasta el plazo… */
        pc_reloj_ms = fin; traza("off", 0, 0); pc_reloj_ms = ahora;   /* (la traza lleva la hora del plazo, no la de ahora) */
        tono_hz = 0;                               /* …y de ahí en adelante, silencio */
    }
    if (!wav_tono) {
        if (!(wav_tono = wav_abrir("W600_WAV_TONO", PC_TONO_RATE, 1))) { tono_desde_ms = hasta_ms; return; }
    }
    if (hasta_ms <= tono_desde_ms) return;
    marcos = (hasta_ms - tono_desde_ms) * PC_TONO_RATE / 1000u;
    if (marcos > (1u << 20)) marcos = 1u << 20;
    for (k = 0; k < marcos; k++) {
        /* mismo modelo que emu/maquina-s4: cuadrada de amplitud fija, arriba mientras
         * (ciclos % periodo) < ancho, con el ancho = periodo * ciclo / 200 */
        short v = 0;
        if (tono_hz) {
            u32 periodo = 13000000u / tono_hz, ancho = periodo * tono_ciclo / 200u;
            u32 c = tono_fase_ciclos + (u32)((unsigned long long)k * 13000000ull / PC_TONO_RATE);
            v = (periodo && (c % periodo) < ancho) ? (short)PC_TONO_AMPL : (short)-PC_TONO_AMPL;
        }
        fputc((int)((unsigned short)v & 0xFF), wav_tono);
        fputc((int)(((unsigned short)v >> 8) & 0xFF), wav_tono);
    }
    tono_fase_ciclos += (u32)((unsigned long long)marcos * 13000000ull / PC_TONO_RATE);
    wav_tono_marcos += marcos;
    pc_tono_marcos += marcos;
    if (tono_hz) pc_tono_sonando_marcos += marcos;
    wav_cabecera(wav_tono, wav_tono_marcos, PC_TONO_RATE, 1);
    tono_desde_ms = hasta_ms;
}
static void traza(const char *q, u32 hz, u32 ciclo)
{   /* OJO: con hw_tono_ms el «off» sale cuando alguien vuelve a mirar el reloj, pero con la hora del plazo */
    if (n_traza > (int)sizeof pc_tono_traza - 64) return;
    n_traza += snprintf(pc_tono_traza + n_traza, sizeof pc_tono_traza - (size_t)n_traza,
                        "%7u ms %-4s %5u Hz ciclo %3u%%\n", pc_reloj_ms, q, hz, ciclo);
}
int hw_tono(u32 hz, int ciclo)
{
    if (hz < 397 || hz > 20000 || ciclo < 1 || ciclo > 100) return HW_ERROR;
    pc_sonido_avanzar(pc_reloj_ms);
    if (hz != tono_hz || (u32)ciclo != tono_ciclo) traza("on", hz, (u32)ciclo);
    tono_hz = hz; tono_ciclo = (u32)ciclo; tono_hasta_ms = 0;
    return HW_OK;
}
int hw_tono_ms(u32 hz, int ciclo, u32 ms)
{
    if (hw_tono(hz, ciclo) != HW_OK) return HW_ERROR;
    tono_hasta_ms = pc_reloj_ms + ((ms + 3) / 4 ? (ms + 3) / 4 * 4 : 4);   /* el grano del tic del teléfono: 4 ms */
    if (!tono_hasta_ms) tono_hasta_ms = 1;
    return HW_OK;
}
int hw_tono_fin(void)
{
    pc_sonido_avanzar(pc_reloj_ms);
    if (tono_hz) traza("off", 0, 0);
    tono_hz = 0; tono_hasta_ms = 0;
    return HW_OK;
}

int hw_tono_mantener(u32 hz, int ciclo)
{
    pc_sonido_avanzar(pc_reloj_ms);                /* si había un plazo y ha vencido, aquí se apaga */
    if (!hz) { if (!tono_hz) return 0; hw_tono_fin(); return 1; }
    if (hz < 397 || hz > 20000 || ciclo < 1 || ciclo > 100) return HW_ERROR;
    if (tono_hz == hz && tono_ciclo == (u32)ciclo && !tono_hasta_ms) return 0;
    hw_tono(hz, ciclo);
    return 1;
}
extern int pc_gancho_app;
int hw_tic_gancho(void (*f)(void)) { pc_gancho_app = f != 0; return HW_NO_DISPONIBLE; }   /* en el PC no hay tic: se sondea desde el bucle */

static int pcm_vivo, pcm_motivo;
static u32 pcm_marcos;
int hw_pcm_init(u32 rate, int canales)
{
    if (!rate || canales < 1 || canales > 2) return HW_ERROR;
    /* En el PC no hay DSP. Por omisión se contesta lo mismo que contestaría el teléfono con el DSP
     * dormido (HW_NO_DISPONIBLE, motivo -3: la configuración de salida sin respuesta), para que la app
     * de Música se pruebe en su camino honesto. Con W600_PCM=1 se finge que el DSP contesta. */
    const char *q = getenv("W600_PCM");
    if (!q || !*q || *q == '0') { pcm_motivo = -3; return HW_NO_DISPONIBLE; }
    pcm_motivo = 0; pcm_vivo = 1; pcm_marcos = 0;
    wav_pcm_rate = rate; wav_pcm_canales = (u32)canales;
    if (!wav_pcm) wav_pcm = wav_abrir("W600_WAV_PCM", rate, (u32)canales);
    return HW_OK;
}
int hw_pcm_motivo(void) { return pcm_motivo; }
int hw_pcm_escribir(const short *pcm, u32 marcos)
{
    u32 i;
    if (!pcm_vivo || !pcm || !marcos) return 0;
    if (wav_pcm) {
        for (i = 0; i < marcos * wav_pcm_canales; i++) {
            fputc((int)((unsigned short)pcm[i] & 0xFF), wav_pcm);
            fputc((int)(((unsigned short)pcm[i] >> 8) & 0xFF), wav_pcm);
        }
        wav_pcm_marcos += marcos;
        wav_cabecera(wav_pcm, wav_pcm_marcos, wav_pcm_rate, wav_pcm_canales);
    }
    pcm_marcos += marcos;
    return (int)marcos;
}
void hw_pcm_cerrar(void) { pcm_vivo = 0; }
u32  hw_pcm_marcos(void) { return pcm_marcos; }
int  hw_sonido_hay(void) { return HW_SND_ZUMBADOR | (pcm_vivo ? HW_SND_PCM : 0); }
