/* cargador.c — EL CARGADOR DE APPS («.sen»): una app es un fichero aparte que se lee, se valida, se reubica en memoria
 * PRESTADA, se ejecuta contra la tabla de funciones (shell/cg_tabla.c) y se devuelve entera al salir.
 * Contrato y formato: api/cargable.h. Documento: docs/apps_cargables.md.
 *
 * Las cuatro ideas:
 *  1. VALIDAR TODO ANTES DE TOCAR NADA: cabecera (con su CRC), tamaños (sin desbordar la suma), cada reubicación
 *     (alineada, creciente, dentro de la imagen, y el valor que reubica dentro de lo de la app), el CRC del cuerpo.
 *     Solo entonces se aplica. El fichero se lee siempre a trozos con desplazamiento (no se supone mapeado).
 *  2. UN SOLO SITIO DE ENTRADA: cg_llamar(). Pasa a la PILA DE LA APP, deja puesto el punto de vuelta ante un fallo y
 *     el plazo del perro, y al volver mira los centinelas (pila, zonas rojas, vectores). Toda retrollamada de un
 *     widget pasa por ahí porque el sistema nunca guarda una función de la app: guarda un trampolín suyo.
 *  3. SI LA APP FALLA, SE VA ELLA: excepción de la CPU o cuelgue dentro de cg_llamar -> se vuelve al punto de vuelta,
 *     la app queda MUERTA (no se la llama más), se quitan sus pantallas, se devuelve su memoria y se dice por qué.
 *  4. LA MEMORIA: [zona roja][imagen: código+datos+bss][montón][centinela][pila][zona roja], un bloque del préstamo
 *     de shell/app_externa.c (en el PC, un mmap ejecutable). La pila va ARRIBA: si desborda, pisa SU montón.
 * Lo que NO se puede impedir sin MMU está dicho en el documento (§ límites): aquí no se finge. */
#include "cargador.h"
#include "app_externa.h"
#include "red_sis.h"                                         /* (1.6) sh_red_soltar_app */
#include "camara_sis.h"                                      /* (1.9) sh_cam_soltar_app */
#include "micro_sis.h"                                       /* (1.10) sh_mic_soltar_app */
#include "m3d.h"                                            /* (ICO) m3_malla_sen */
#if !TELEFONO
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/time.h>
#endif

#if TELEFONO
#define CG_MAQUINA W5_MAQ_ARMV5TE
#elif defined(__aarch64__)
#define CG_MAQUINA W5_MAQ_AARCH64
#else
#error "el banco de PC de las apps cargables solo está hecho para AArch64"
#endif
#define PAL ((u32)sizeof(W5Pal))
#define ZONA 256u                       /* zona roja a cada lado del bloque */
#define CENT 64u                        /* centinela entre el montón y la pila */
#define PATRON 0xC5A5C0DEu
#define R16(n) (((n) + 15u) & ~15u)
#define CG_PLAZO_S 30                   /* el perro, con una retrollamada de la app en curso */
#define CG_ESCRITURAS_MIN 20            /* escrituras de fichero por minuto, como mucho */
#define CG_ARENA_APP 8192u              /* (PM) la arena de un cuadro dentro de la app */

static struct {
    u8  estado, perfil, pausada, cerrando, en_juego, acel;
    u8  prof;                           /* > 0: hay código de la app en curso (anidado: prof > 1) */
    u8 *bloque; u32 tam_bloque;
    u8 *img; u32 tam_cod, tam_img;      /* imagen = código + datos + bss */
    u8 *monton; u32 tam_monton;
    u8 *pila; u32 tam_pila;             /* la pila va de pila a pila + tam_pila; crece hacia abajo */
    W5Entrada entrada;
    W5xCab cab;
    char ruta[96], prefijo[20];
    CgRet ret[CG_RETS]; u32 gen;
    void *retro[CG_RETROS];
    int  n_pant, rutas; Ctx *raiz;
    char motivo[120];
    u8  *pm_base; u8 *arena; u32 tam_arena, arena_pos, arena_visto;   /* (PM) el préstamo entero y la arena de la app */
    u32  t_entrada, esc_t0, esc_n;
    u32  falta_pc, falta_dir, falta_tipo, falta_modo, falta_exc;
} cg;
CgMedidas cg_med;
volatile int cg_en_gate;                                      /* (AUD9) >0: la app está DENTRO de una función del sistema (la puerta): el perro no la saca */
static CgRet ret_vacia;

int cg_estado(void) { return cg.estado; }
const char *cg_ruta_viva(void) { return cg.estado == CG_VIVA ? cg.ruta : NULL; }   /* (LANZ) la .sen abierta ahora (los guiones del PC) */
int cg_perfil(void) { return cg.perfil; }
int cg_api_menor(void) { return cg.cab.api_menor; }
int cg_app_sabe_girar(void) { return cg.estado == CG_LIBRE || (cg.cab.caps & W5_CAP_GIRA) != 0; }
void cg_avisar_giro(int g)                                  /* (1.7) a la app que sabe girar, el suceso W5_GIRO (si no está en pausa) */
{
    if (cg.estado == CG_VIVA && !cg.pausada && cg.entrada && (cg.cab.caps & W5_CAP_GIRA))
        cg_llamar((void *)cg.entrada, (W5Pal)(size_t)cg_pm_api(), W5_GIRO, (W5Pal)g, 0);
}

/* ---- CRC-32 (el de zlib), por medios bytes ---------------------------------------------------------------------- */
u32 cg_crc(u32 crc, const void *d, u32 n)
{
    static const u32 t[16] = { 0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
                               0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C };
    const u8 *p = d;
    crc = ~crc;
    while (n--) { crc ^= *p++; crc = t[crc & 15] ^ (crc >> 4); crc = t[crc & 15] ^ (crc >> 4); }
    return ~crc;
}

/* ---- motivos ------------------------------------------------------------------------------------------------------- */
static void poner(char *d, u32 nd, const char *s) { u32 i = 0; if (!d || !nd) return; for (; s[i] && i + 1 < nd; i++) d[i] = s[i]; d[i] = 0; }
static int no(char *motivo, u32 nm, const char *s) { poner(motivo, nm, s); return -1; }

/* ---- 1. LA CABECERA ------------------------------------------------------------------------------------------------ */
int cg_cab_vale(const W5xCab *c, u32 tam_real, char *motivo, u32 nm)
{
    u32 i, imagen, pal;
    if (memcmp(c->magia, W5X_MAGIA, 4)) return no(motivo, nm, "No es una app (.sen).");
    if (c->tam_cab != W5X_TAM_CAB) return no(motivo, nm, "Cabecera de un formato que no conozco.");
    if (cg_crc(0, c, W5X_TAM_CAB - 4) != c->crc_cab) return no(motivo, nm, "El fichero está dañado (cabecera).");
    if (c->maquina != CG_MAQUINA) return no(motivo, nm, "Esta app es para otro procesador.");
    if (c->api_mayor != W5_API_MAYOR || c->api_menor > CG_API_MENOR) {   /* (BT) 1.4: el sistema trae el grupo Bluetooth (api/cargable_bt.h) */
        poner(motivo, nm, ui_fmt("Pide la API %u.%u y este sistema tiene la %u.%u: hace falta un sistema más nuevo.",
                                 c->api_mayor, c->api_menor, W5_API_MAYOR, CG_API_MENOR));
        if (c->api_mayor < W5_API_MAYOR) poner(motivo, nm, ui_fmt("Es de la API %u, que este sistema (API %u) ya no lleva.", c->api_mayor, W5_API_MAYOR));
        return -1;
    }
    if (c->perfil != W5_PERFIL_WIDGETS && c->perfil != W5_PERFIL_JUEGO) return no(motivo, nm, "Perfil de app desconocido.");
    if (c->caps & ~(u32)CG_CAPS) return no(motivo, nm, "Necesita algo que este teléfono no tiene (sensor, vibrador, teclas…).");
    if (c->an_min > UI_ANCHO || c->al_min > UI_ALTO) {
        poner(motivo, nm, ui_fmt("Necesita una pantalla de %ux%u y esta es de %ux%u.", c->an_min, c->al_min, UI_ANCHO, UI_ALTO));
        return -1;
    }
    pal = PAL;
    if (c->tam_cod < 4 || c->tam_cod > W5_IMG_MAX || c->tam_dat > W5_IMG_MAX || c->tam_bss > W5_IMG_MAX) return no(motivo, nm, "Tamaños imposibles.");
    imagen = c->tam_cod + c->tam_dat;                       /* no desborda: cada uno <= W5_IMG_MAX (8 MiB) */
    if (imagen + c->tam_bss > W5_IMG_MAX) return no(motivo, nm, "La app es demasiado grande.");
    if ((c->tam_cod | c->tam_dat | c->tam_bss) & (pal - 1)) return no(motivo, nm, "Tamaños sin alinear.");
    if (c->pila < W5_PILA_MIN || c->pila > W5_PILA_MAX || (c->pila & 15)) return no(motivo, nm, "Pide una pila que no vale.");
    if (c->monton > W5_MONTON_MAX || (c->monton & 15)) return no(motivo, nm, "Pide un montón que no vale.");
    if (c->n_reub > W5_REUB_MAX || c->n_reub > imagen / pal) return no(motivo, nm, "Tabla de reubicaciones imposible.");
    if ((c->entrada & ~1u) >= c->tam_cod || (CG_MAQUINA != W5_MAQ_ARMV5TE && (c->entrada & 3))) return no(motivo, nm, "Punto de entrada fuera del código.");
    if (c->tam_rec > 0xF0000000u - W5X_TAM_CAB - imagen - c->n_reub * 4u) return no(motivo, nm, "Recursos de tamaño imposible.");
    if (c->tam_fich != W5X_TAM_CAB + imagen + c->n_reub * 4u + c->tam_rec) return no(motivo, nm, "El tamaño no cuadra con la cabecera.");
    if ((c->caps & W5_CAP_JIT) && c->api_menor < 2) return no(motivo, nm, "Pide generar código sin declarar la API 1.2.");   /* (1.2, pista JIT) */
    if ((c->caps & W5_CAP_REC_VENTANA) && (c->api_menor < 1 || !c->tam_rec || c->tam_rec > W5_REC_VENTANA_MAX))
        return no(motivo, nm, "La ventana de recursos que pide no es posible.");
    if (tam_real != c->tam_fich) return no(motivo, nm, "El fichero está cortado o le sobra algo.");
    if (c->icono_an > 16) return no(motivo, nm, "Icono que no vale.");
    for (i = 0; i < sizeof c->nombre && c->nombre[i]; i++) if ((u8)c->nombre[i] < 0x20) return no(motivo, nm, "Nombre que no vale.");
    if (!i || i == sizeof c->nombre) return no(motivo, nm, "Nombre que no vale.");
    return 0;
}

/* ---- la memoria de la app ------------------------------------------------------------------------------------------ */
int cg_en_app(const void *p, size_t n)
{
    const u8 *q = p, *fin;
    if (cg.estado == CG_LIBRE) return 0;
    fin = cg.pila + cg.tam_pila;                             /* (AUD9 A3) sin desbordar: nada de q + n (daba la vuelta a 0: los vectores) */
    return q >= cg.img && q <= fin && n <= (size_t)(fin - q);
}
#if TELEFONO
extern char _data[], _ebss[], _fin_flash[];
extern void _inicio(void);
static int en_sistema(const void *p) { const char *q = p; return (q >= (const char *)MAIN_INI && q < _fin_flash) || (q >= _data && q < _ebss); }
#else
extern char __executable_start[], end[];
static int en_sistema(const void *p) { const char *q = p; return q >= __executable_start && q < end; }
#endif
int cg_legible(const void *p) { return p && (cg_en_app(p, 1) || ui_en_arena(p) || (!cg_pm_activa() && en_sistema(p))); }   /* (AUD9) con PM, nada del sistema: ni su flash ni su RAM */
int cg_legible_n(const void *p, size_t n) { const u8 *q = p; return p && n && (cg_en_app(q, n) || (ui_en_arena(q) && ui_en_arena(q + n - 1)) || (!cg_pm_activa() && en_sistema(q) && en_sistema(q + n - 1))); }   /* (AUD9) rango entero, no solo el primer byte */
int cg_sis_leible(const void *p, size_t n) { const u8 *q = p; return p && n && ((ui_en_arena(q) && ui_en_arena(q + n - 1)) || (en_sistema(q) && en_sistema(q + n - 1))); }   /* (AUD9) un puntero que el SISTEMA entregó (arena o su RAM/flash), no de la app ni un registro: para validar contextos */
int cg_cadena(const char *s) { return !s || cg_legible(s); }

static void zonas_poner(void)
{
    u32 i, *z;
    for (z = (u32 *)cg.bloque, i = 0; i < ZONA / 4; i++) z[i] = PATRON;
    for (z = (u32 *)(cg.bloque + cg.tam_bloque - ZONA), i = 0; i < ZONA / 4; i++) z[i] = PATRON;
    for (z = (u32 *)(cg.pila - CENT), i = 0; i < CENT / 4; i++) z[i] = PATRON;
    for (z = (u32 *)cg.pila, i = 0; i < cg.tam_pila / 4; i++) z[i] = PATRON;      /* para medir cuánta pila se usa */
}
static const char *zonas_mirar(void)
{
    const u32 *z; u32 i;
    for (z = (const u32 *)(cg.pila - CENT), i = 0; i < CENT / 4; i++) if (z[i] != PATRON) return "La app desbordó su pila.";
    for (z = (const u32 *)cg.bloque, i = 0; i < ZONA / 4; i++) if (z[i] != PATRON) return "La app escribió por debajo de su memoria.";
    for (z = (const u32 *)(cg.bloque + cg.tam_bloque - ZONA), i = 0; i < ZONA / 4; i++) if (z[i] != PATRON) return "La app escribió por encima de su memoria.";
    return NULL;
}
u32 cg_pila_libre(void)
{
    const u32 *z = (const u32 *)cg.pila; u32 i = 0;
    if (cg.estado == CG_LIBRE) return 0;
    while (i < cg.tam_pila / 4 && z[i] == PATRON) i++;
    return i * 4;
}

/* El montón de la app: primer ajuste con fusión, DENTRO de su bloque. Cabecera de 8 bytes: tamaño (con la cabecera,
 * múltiplo de 8) y marca. Lo lleva el sistema (y no la app) para que la prueba con ASan lo cubra y para poder decir
 * «montón corrupto» en vez de colgarse recorriéndolo. */
typedef struct { u32 tam, marca; } Mon;
#define MON_LIBRE 0x4C494252u
#define MON_USADO 0x55534144u
static void monton_iniciar(void) { if (cg.tam_monton >= 16) { Mon *m = (Mon *)cg.monton; m->tam = cg.tam_monton; m->marca = MON_LIBRE; } }
static Mon *monton_sig(Mon *m)
{
    u8 *fin = cg.monton + cg.tam_monton;
    if ((m->marca != MON_LIBRE && m->marca != MON_USADO) || m->tam < 8 || (m->tam & 7) || m->tam > (u32)(fin - (u8 *)m)) { cg_matar("El montón de la app está corrupto."); return NULL; }
    return (Mon *)((u8 *)m + m->tam);
}
void *cg_monton_pedir(u32 n)
{
    Mon *m = (Mon *)cg.monton, *s, *fin = (Mon *)(cg.monton + cg.tam_monton);
    if (cg.estado != CG_VIVA || cg.tam_monton < 16 || !n || n > cg.tam_monton) return NULL;
    n = ((n + 7u) & ~7u) + 8;
    while (m < fin) {
        if (!(s = monton_sig(m))) return NULL;
        if (m->marca == MON_LIBRE) {
            while (s < fin && s->marca == MON_LIBRE) { Mon *t = monton_sig(s); if (!t) return NULL; m->tam += s->tam; s = t; }   /* fusiona los libres que siguen */
            if (m->tam >= n) {
                if (m->tam - n >= 16) { Mon *r = (Mon *)((u8 *)m + n); r->tam = m->tam - n; r->marca = MON_LIBRE; m->tam = n; }
                m->marca = MON_USADO;
                memset(m + 1, 0, m->tam - 8);
                return m + 1;
            }
        }
        m = s;
    }
    return NULL;
}
void cg_monton_soltar(void *p)
{
    Mon *m;
    if (!p || cg.estado != CG_VIVA) return;
    m = (Mon *)p - 1;                        /* (INT2; AUD10 BAJO) la resta DESPUÉS de mirar el nulo (era UB con p = NULL) */
    if ((u8 *)m < cg.monton || (u8 *)p >= cg.monton + cg.tam_monton || ((size_t)p & 7) || m->marca != MON_USADO) { cg_matar("La app soltó memoria que no era suya (o dos veces)."); return; }
    m->marca = MON_LIBRE;
}
u32 cg_monton_libre(void)
{
    Mon *m = (Mon *)cg.monton, *fin = (Mon *)(cg.monton + cg.tam_monton); u32 mayor = 0;
    if (cg.estado != CG_VIVA || cg.tam_monton < 16) return 0;
    while (m < fin) { Mon *s = monton_sig(m); if (!s) return 0; if (m->marca == MON_LIBRE && m->tam - 8 > mayor) mayor = m->tam - 8; m = s; }
    return mayor;
}

/* ---- la plataforma: de dónde sale el bloque, las cachés, los vectores ----------------------------------------------- */
#if TELEFONO
static u8 *bloque_tomar(u32 n) { u32 hay; if (app_pantalla_tomada() || !app_mem_tomar(&hay)) return NULL; if (n > hay) { app_mem_soltar(); return NULL; } return app_mem_reservar(n); }
static void bloque_soltar(void) { app_mem_soltar(); }
/* (R3; auditoría AUD6, A1 — ANTES DECÍA, Y ERA FALSO: «la [caché] de datos no [está encendida]») LAS DOS CACHÉS ESTÁN
 * ENCENDIDAS en el sistema: la de instrucciones y la de DATOS, con la SDRAM en escritura diferida (C y B: hal/reloj.c,
 * mmu_cache_init; hal/reloj.S, mmu_activar). Tras copiar y reubicar el código de la app, lo escrito puede estar todavía en
 * líneas SUCIAS de la caché de datos y no en la SDRAM, que es de donde la caché de instrucciones lee: hay que LIMPIAR primero
 * (cache_limpiar: c7,c10,1 línea a línea y vaciar el búfer de escritura) el tramo del código y DESPUÉS invalidar la de
 * instrucciones. Sin lo primero, el procesador puede ejecutar lo que hubiera antes en esa RAM. [inferido: el emulador no
 * modela la caché de instrucciones; lo vio la lectura del código, no una prueba]. */
void cache_limpiar(u32 inicio, u32 fin);                    /* hal/reloj.S */
static void cache_codigo_tramo(const void *ini, const void *fin)
{
    cache_limpiar((u32)(size_t)ini, (u32)(size_t)fin);
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c5, 0\n nop\n nop\n nop\n nop" : : : "r0", "memory");   /* invalida la de instrucciones */
}
static void cache_codigo(void) { cache_codigo_tramo(cg.img, cg.img + cg.tam_img); }
/* LOS VECTORES. Sin MMU la dirección 0 es RAM (la ITCM) y ahí están los 8 saltos y las 8 direcciones de las
 * excepciones: un puntero nulo de la app NO da fallo, los pisa. Se les hace una foto al cargar; tras cada llamada a
 * la app se comparan, y si han cambiado se reponen y la app se va. */
static volatile size_t cero;
static u32 vec_foto[16];
static void vec_fotografiar(void) { const volatile u32 *v = (const volatile u32 *)cero; int i; for (i = 0; i < 16; i++) vec_foto[i] = v[i]; }
/* Y LLAMAR a un puntero nulo ejecuta el vector de reset: el sistema se reiniciaría a medias (con la SDRAM y el PLL en
 * marcha: lo vio el modo estricto). Mientras hay código de la app en curso, la DIRECCIÓN del vector de reset (la palabra
 * 8, en 0x20) apunta a una trampa del cargador: saltar a 0 se convierte en un fallo más de la app. Nadie más usa ese
 * vector en marcha: el reinicio por software va por reinicio_caliente() a _inicio, y un reset de verdad no pasa por la RAM. */
static void vec_trampa(void (*f)(void)) { volatile u32 *v = (volatile u32 *)cero; v[8] = f ? (u32)(size_t)f : vec_foto[8]; }
static int vec_reponer(void) { volatile u32 *v = (volatile u32 *)cero; int i, mal = 0; for (i = 0; i < 16; i++) if (v[i] != vec_foto[i]) { v[i] = vec_foto[i]; mal = 1; } if (mal) cache_codigo_tramo((const void *)cero, (const void *)(cero + sizeof vec_foto)); return mal; }   /* (AUD6 A1) los vectores (ITCM): limpiar no estorba */
#else
static u8 *bloque_tomar(u32 n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static void bloque_soltar(void) { munmap(cg.bloque, cg.tam_bloque); }
static void cache_codigo(void) { __builtin___clear_cache((char *)cg.img, (char *)cg.img + cg.tam_cod); }
static void vec_fotografiar(void) { }
static int vec_reponer(void) { return 0; }
#define vec_trampa(f) ((void)0)
#endif

/* ---- (1.1, pista M5X) LA VENTANA DE RECURSOS -------------------------------------------------------------------------
 * Una app con W5_CAP_REC_VENTANA se enlazó con sus recursos en W5_REC_VA. Aquí se ponen, con la MMU, las páginas de 4 KiB
 * de la flash donde están sus recursos (el fichero, contiguo en NUESTRO FS: hw_fs_mapa) en esas direcciones: tablas de
 * SEGUNDO NIVEL («coarse», 256 entradas de 4 KiB) sacadas del préstamo, y en la tabla de primer nivel (la de TTBR0; la
 * pone hal/reloj.c: 1:1, secciones de 1 MiB) las secciones de la ventana apuntan a ellas mientras la app está cargada.
 * Al descargar se reponen las secciones como estaban. Las cachés son VIVT (ARM926): la de datos se limpia e invalida
 * entera (mrc c7,c14,3 en bucle) al poner y al quitar, para que no quede ninguna línea de la ventana de la app anterior;
 * y las tablas se limpian de la caché antes de invalidar la TLB (el recorrido de tablas lee la SDRAM, no la caché).
 * [inferido] cómo se porta en el aparato: el emulador no modela las cachés. */
#if TELEFONO
#define VEN_SEC (W5_REC_VENTANA_MAX >> 20)
static u32 *ven_l1, ven_antes[VEN_SEC], ven_n;
void cache_limpiar(u32 inicio, u32 fin);                    /* hal/reloj.S */
static void dcache_vaciar(void)
{
    __asm__ volatile("1: mrc p15, 0, r15, c7, c14, 3\n bne 1b\n mov r0, #0\n mcr p15, 0, r0, c7, c10, 4" : : : "r0", "cc", "memory");
}
static void tlb_invalidar(void) { __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c8, c7, 0" : : : "r0", "memory"); }
static int ventana_poner(const u8 *fis, u32 n)
{
    u32 ttb, nsec = (n + 0xFFFFFu) >> 20, npag = (n + 4095u) >> 12, i, *l2;
    u8 *m;
    if (((u32)(size_t)fis & (W5_REC_ALINEA - 1)) || !nsec || nsec > VEN_SEC) return -1;
    /* (AUD9 C2) las tablas en PÁGINAS ENTERAS PROPIAS: si compartieran página de 4 KiB con lo que se reserve después (el
     * búfer girado de jg_tomar, que PM abre a la app redondeando a páginas), la app podría reescribir su propio mapa.
     * [m, m + T + 4096) con l2 = m redondeado a 4 KiB: [l2, l2 + T) queda dentro, y la primera página de lo siguiente
     * empieza en m + T + 4096 redondeado hacia abajo, que es >= l2 + T. Lo anterior acaba en m <= l2. */
    const u32 T = (nsec * 1024u + 4095u) & ~4095u;
    if (!(m = app_mem_reservar(T + 4096u))) return -2;                            /* del préstamo: vuelve con él */
    l2 = (u32 *)(((u32)(size_t)m + 4095u) & ~4095u);
    for (i = 0; i < nsec * 256u; i++) l2[i] = i < npag ? (((u32)(size_t)fis + (i << 12)) | 0xAA0u | 0x8u | 0x2u) : 0;   /* página pequeña, AP=10 (sistema RW, USUARIO SOLO LECTURA: lo que pide la protección de PM), C (como la NOR) */
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttb));
    ven_l1 = (u32 *)(size_t)(ttb & ~0x3FFFu);
    dcache_vaciar();
    for (i = 0; i < nsec; i++) { ven_antes[i] = ven_l1[(W5_REC_VA >> 20) + i]; ven_l1[(W5_REC_VA >> 20) + i] = (u32)(size_t)(l2 + i * 256u) | 0x11u; }
    ven_n = nsec;
    cache_limpiar((u32)(size_t)l2, (u32)(size_t)(l2 + nsec * 256u));
    cache_limpiar((u32)(size_t)&ven_l1[W5_REC_VA >> 20], (u32)(size_t)&ven_l1[(W5_REC_VA >> 20) + nsec]);
    tlb_invalidar();
    return 0;
}
static void ventana_quitar(void)
{
    u32 i;
    if (!ven_n) return;
    dcache_vaciar();                                        /* (VIVT) ninguna línea de la ventana sobrevive a la app */
    for (i = 0; i < ven_n; i++) ven_l1[(W5_REC_VA >> 20) + i] = ven_antes[i];
    cache_limpiar((u32)(size_t)&ven_l1[W5_REC_VA >> 20], (u32)(size_t)&ven_l1[(W5_REC_VA >> 20) + ven_n]);
    tlb_invalidar();
    ven_n = 0;
}
#else
static u32 ven_n;
static int ventana_poner(const u8 *fis, u32 n) { (void)fis; (void)n; return -1; }   /* sin MMU en el banco de PC (y CG_CAPS no la ofrece) */
static void ventana_quitar(void) { }
#endif

#if TELEFONO
#define PM_DESHACER() cg_pm_desmontar((volatile u32 *)cero)   /* (M5X sobre PM) si la ventana falla, la protección ya estaba puesta */
#else
#define PM_DESHACER() ((void)0)
#endif
/* ---- 2. CARGAR ------------------------------------------------------------------------------------------------------ */
static u32 leer_todo(CgLeer leer, void *quien, u32 desde, void *buf, u32 n)   /* a trozos de 4 KiB, latiendo */
{
    u32 hecho = 0;
    while (hecho < n) {
        u32 t = n - hecho > 4096 ? 4096 : n - hecho, r = leer(quien, desde + hecho, (u8 *)buf + hecho, t);
        if (r != t) return hecho + (r < t ? r : 0);
        hecho += t;
        hw_perro_latido();
    }
    return hecho;
}
int cg_cargar_de(CgLeer leer, void *quien, u32 tam, const char *ruta, char *motivo, u32 nm)
{
    W5xCab c;
    u32 imagen, total, bloque, t0 = hw_ms(), crc, crc2 = 0, pasada, i, k, ant = 0, des_reub, arena, extra;
    int pm = 0;
    u32 tabla[64];
    u8 *b;
    const char *base, *p;
    if (cg.estado != CG_LIBRE) return no(motivo, nm, "Ya hay otra app cargada.");
    if (hw_modo_seguro()) return no(motivo, nm, "En modo seguro no se cargan apps.");
    if (tam < W5X_TAM_CAB || leer(quien, 0, &c, W5X_TAM_CAB) != W5X_TAM_CAB) return no(motivo, nm, "No se puede leer el fichero.");
    if (cg_cab_vale(&c, tam, motivo, nm)) return -1;
    imagen = c.tam_cod + c.tam_dat; total = imagen + c.tam_bss;
#if TELEFONO
    pm = cg_pm_posible();
#endif
    arena = pm ? CG_ARENA_APP : 0u;                          /* (PM) con protección, ui_arena da memoria DE LA APP */
    bloque = ZONA + R16(total) + R16(c.monton) + arena + CENT + c.pila + ZONA;
    if (pm) bloque = (bloque + 4095u) & ~4095u;              /* (PM) en páginas enteras: lo que siga en el préstamo empieza en página nueva */
    extra = 0;
#if TELEFONO
    if (pm) extra = cg_pm_extra((c.caps & W5_CAP_REC_VENTANA) ? 0u : c.tam_rec);                           /* (PM) [tablas de 2.º nivel][puerta][guarda] delante del bloque */
#endif
    if (!(b = bloque_tomar(extra + bloque))) return no(motivo, nm, ui_fmt("No hay memoria para esta app (pide %u KB).", (extra + bloque + 1023) >> 10));
    memset(&cg.cab, 0, sizeof cg.cab);
    cg.pm_base = b; b += extra;
    cg.bloque = b; cg.tam_bloque = bloque;
    cg.img = b + ZONA; cg.tam_cod = c.tam_cod; cg.tam_img = total;
    cg.monton = cg.img + R16(total); cg.tam_monton = R16(c.monton);
    cg.arena = cg.monton + cg.tam_monton; cg.tam_arena = arena; cg.arena_pos = 0; cg.arena_visto = 0;
    cg.pila = cg.arena + arena + CENT; cg.tam_pila = c.pila;
    if (leer_todo(leer, quien, W5X_TAM_CAB, cg.img, imagen) != imagen) { bloque_soltar(); return no(motivo, nm, "No se puede leer el fichero."); }
    memset(cg.img + imagen, 0, total - imagen);
    crc = cg_crc(0, cg.img, imagen);
    des_reub = W5X_TAM_CAB + imagen;
    /* Dos pasadas por la tabla: la primera SOLO comprueba (y acaba el CRC); la segunda aplica. Nada se reubica hasta
     * saber que todas las entradas y el CRC valen. */
    for (pasada = 0; pasada < 2; pasada++) {
        for (i = 0; i < c.n_reub; i += k) {
            k = c.n_reub - i > 64 ? 64 : c.n_reub - i;
            if (leer(quien, des_reub + i * 4, tabla, k * 4) != k * 4) { bloque_soltar(); return no(motivo, nm, "No se puede leer el fichero."); }
            if (!pasada) crc = cg_crc(crc, tabla, k * 4);
            else crc2 = cg_crc(crc2, tabla, k * 4);     /* (AUD7 B4) lo que se aplica tiene que ser lo que se comprobó: se vuelve a sumar */
            for (u32 j = 0; j < k; j++) {
                u32 d = tabla[j];
                W5Pal v;
                if ((d & (PAL - 1)) || d > imagen - PAL || (i + j && d <= ant)) { bloque_soltar(); return no(motivo, nm, "El fichero está dañado (reubicación fuera de la app)."); }
                memcpy(&v, cg.img + d, PAL);
                /* (AUD7 B4) la segunda pasada RELEE la tabla (y la tarjeta puede cambiar entre dos lecturas): cada valor se vuelve a validar */
                if (v > total) { bloque_soltar(); return no(motivo, nm, "El fichero está dañado (dirección fuera de la app)."); }
                if (pasada) { v += (W5Pal)(size_t)cg.img; memcpy(cg.img + d, &v, PAL); }
                ant = d;
            }
            hw_perro_latido();
        }
        if (!pasada && crc != c.crc_cuerpo) { bloque_soltar(); return no(motivo, nm, "El fichero está dañado (no cuadra su suma)."); }
        if (!pasada) crc2 = cg_crc(0, cg.img, imagen);      /* la imagen tal como está ANTES de aplicar: la segunda pasada suma encima la tabla releída */
        ant = 0;
    }
    if (crc2 != c.crc_cuerpo) { bloque_soltar(); return no(motivo, nm, "El fichero ha cambiado mientras se cargaba (la tabla leída dos veces no es la misma)."); }   /* (AUD7 B4) */
    cg.cab = c;
    cg.entrada = (W5Entrada)(size_t)((W5Pal)(size_t)cg.img + c.entrada);
    cg.perfil = c.perfil;
    poner(cg.ruta, sizeof cg.ruta, ruta ? ruta : "");
    /* el prefijo de sus ficheros: el nombre del .sen sin carpeta ni extensión, y un punto */
    for (base = p = cg.ruta; *p; p++) if (*p == '/') base = p + 1;
    for (i = 0; base[i] && base[i] != '.' && i < sizeof cg.prefijo - 2; i++) cg.prefijo[i] = base[i];
    if (!i) cg.prefijo[i++] = 'a';
    cg.prefijo[i++] = '.'; cg.prefijo[i] = 0;
    zonas_poner(); monton_iniciar(); cache_codigo(); vec_fotografiar();
#if TELEFONO
    if (pm) {   /* (PM) la app, a modo usuario: permisos de página y la puerta. Sin esto (sin MMU), como antes */
        u32 tf = 0;
        const u8 *fm = (c.tam_rec && ruta) ? hw_fs_mapa(ruta, &tf) : NULL;   /* el fichero, contiguo en la NOR (nuestro FS) */
        if (!cg_pm_montar(cg.pm_base, cg.bloque, cg.bloque + cg.tam_bloque, cg.pila, cg.pila + cg.tam_pila, (c.caps & W5_CAP_REC_VENTANA) ? 0u : c.tam_rec, (volatile u32 *)cero)) { bloque_soltar(); return no(motivo, nm, "No se pudo proteger la memoria de la app."); }
        if (fm && tf == c.tam_fich && !(c.caps & W5_CAP_REC_VENTANA)) cg_pm_rec_fichero(fm + W5X_TAM_CAB + imagen + c.n_reub * 4u, c.tam_rec);   /* para ap_rec_mapa */
        vec_fotografiar();                                   /* la palabra 10 (SWI) ya apunta a la puerta */
    }
#endif
    if (c.caps & W5_CAP_REC_VENTANA) {                     /* (1.1) los recursos, en la ventana W5_REC_VA. DESPUÉS de proteger (PM) */
        /* Dos orígenes, y la app no se entera de cuál: (a) EN EL SITIO, si el fichero está contiguo en una flash mapeada y sus
         * recursos caen en frontera de 4 KiB (el W6FS: hw_fs_mapa); (b) COPIADOS A RAM, en cualquier otro caso (la Memoria W6F,
         * Mis archivos, la tarjeta): páginas enteras del préstamo, FUERA del bloque de la app (con PM quedan solo-sistema en su
         * dirección 1:1; la app solo las ve, de solo lectura, por la ventana). Se comprueba su CRC (crc_rec) tras copiar. */
        u32 tf = 0, ri = W5X_TAM_CAB + imagen + c.n_reub * 4u, t1 = hw_ms();
        const u8 *f = ruta ? hw_fs_mapa(ruta, &tf) : NULL, *fis;
        int r, copia = 0;
        if (f && tf == c.tam_fich && !((u32)(size_t)(f + ri) & (W5_REC_ALINEA - 1))) fis = f + ri;
        else {
            const u32 T = (c.tam_rec + 4095u) & ~4095u;
            u8 *q = app_mem_reservar(T + 4096u);              /* (AUD9 C2) páginas enteras propias: ver ventana_poner */
            if (!q) { PM_DESHACER(); bloque_soltar(); return no(motivo, nm, ui_fmt("No hay memoria para copiar sus recursos a RAM (%u KB).", (T + 4096u) >> 10)); }
            q = (u8 *)(((u32)(size_t)q + 4095u) & ~4095u);
            if (leer_todo(leer, quien, ri, q, c.tam_rec) != c.tam_rec) { PM_DESHACER(); bloque_soltar(); return no(motivo, nm, "No se pueden leer sus recursos."); }
            if (cg_crc(0, q, c.tam_rec) != c.crc_rec) { PM_DESHACER(); bloque_soltar(); return no(motivo, nm, "El fichero está dañado (recursos)."); }
            fis = q; copia = 1;
        }
        if ((r = ventana_poner(fis, c.tam_rec)) != 0) { PM_DESHACER(); bloque_soltar(); return no(motivo, nm, r == -2 ? "No hay memoria para la ventana de recursos." : "La ventana de recursos no se puede poner."); }
        hw_log(ui_fmt("[cargador] ventana de recursos: %u B %s %08X en %08X (%u ms)\n", c.tam_rec, copia ? "COPIADOS a RAM en" : "en el sitio, flash", (u32)(size_t)fis, W5_REC_VA, hw_ms() - t1));
    }
    cg.gen++; memset(cg.ret, 0, sizeof cg.ret); memset(cg.retro, 0, sizeof cg.retro);
    cg.n_pant = cg.rutas = 0; cg.raiz = NULL; cg.pausada = cg.cerrando = cg.en_juego = cg.acel = 0; cg.prof = 0;
    cg.motivo[0] = 0; cg.esc_n = 0; cg.esc_t0 = hw_ms();
    cg.estado = CG_VIVA;
    /* (1.7, estética) una app de antes de la letra nueva corre con la letra y las medidas de antes (MODO V1): las colocó contando con ellas */
    if ((c.api_menor < W5_PANT_MENOR) != dis_es_v1()) { dis_v1(c.api_menor < W5_PANT_MENOR); ui_tema_global(dis_tema()); ui_redisponer_todo(); }
    cg_med.cargas++; cg_med.t_carga_ms = hw_ms() - t0; cg_med.tam_bloque = bloque;
    return 0;
}
void cg_descargar(void)
{
    if (cg.estado == CG_LIBRE) return;
    cg_med.pila_usada = cg.tam_pila - cg_pila_libre();
    if (cg.ruta[0]) hw_log(ui_fmt("[cargador] «%s» descargada: pila usada %u de %u B\n", cg.cab.nombre, cg_med.pila_usada, cg.tam_pila));
    if (cg.acel) { sh_pasos_retener(0); cg.acel = 0; }
    cg12_a_soltar(); cg12_b_soltar(); cg12_c_soltar(); cg12_d_soltar(); cg13_soltar();   /* (1.2; MD 1.3: el diseño pedido) lo que la app retuviera de los servicios (INT2: antes de quitar la ventana y desmontar la PM) */
    if (dis_es_v1()) { dis_v1(0); ui_tema_global(dis_tema()); ui_redisponer_todo(); }      /* (1.7) fuera el modo v1: el sistema, con la letra nueva */
    ventana_quitar();                    /* (1.1) ANTES de cg_pm_desmontar (que solo repone secciones: dejaría la 0x700 colgada) */
#if TELEFONO
    cg_pm_desmontar((volatile u32 *)cero);                   /* (PM) antes de recoger: la app ya no corre */
#endif
    cg_et_reset();                                           /* (PM) las entradas de texto del depósito del sistema, libres */
    sh_app_externa_recoger();            /* (R3) lo mismo que tras `entrar` de una app enlazada: pantalla, gancho del tic, tono, vibrador, claves, memoria */
    sh_cam_soltar_app();                 /* (1.9) si la app dejó la cámara abierta: apagada */
    sh_mic_soltar_app();                 /* (1.10) y el micrófono (la música vuelve si la paró) */
    sh_red_soltar_app();                 /* (1.6) si la app conectó la red: cerrar sus canales y cortar el SPP (antes que la radio) */
    sh_bt_soltar_app();                  /* (BT) si la app encendió el Bluetooth: desconectar y apagar (DESPUÉS de soltar sus claves del almacén:
                                          * una clave de enlace que quedó pendiente con la app delante se escribe aquí) */
    cg.estado = CG_LIBRE; cg.gen++;
    memset(cg.ret, 0, sizeof cg.ret); memset(cg.retro, 0, sizeof cg.retro);
    bloque_soltar();
    cg.bloque = cg.img = cg.monton = cg.pila = NULL; cg.tam_bloque = cg.tam_img = cg.tam_monton = cg.tam_pila = 0;
    cg.raiz = NULL; cg.n_pant = cg.rutas = 0; cg.entrada = NULL;
    cg_med.descargas++;
}

/* ---- 3. ENTRAR EN LA APP: cg_llamar ----------------------------------------------------------------------------------
 * cg_saltar(args, sp): pone la pila en `sp`, llama a args[0](args[1], args[2], args[3], args[4]) y repone la pila. */
W5Pal cg_saltar(W5Pal *args, void *sp);
#if TELEFONO
__asm__(".text\n .arm\n .align 2\n .global cg_saltar\n .type cg_saltar, %function\n"
        "cg_saltar:\n push {r4, r5, r6, lr}\n mov r4, sp\n mov sp, r1\n mov r12, r0\n"
        " ldr r0, [r12, #4]\n ldr r1, [r12, #8]\n ldr r2, [r12, #12]\n ldr r3, [r12, #16]\n ldr r12, [r12]\n"
        " mov lr, pc\n bx r12\n mov sp, r4\n pop {r4, r5, r6, lr}\n bx lr\n"
        /* cg_guardar(s): como setjmp (r4-r11, sp, lr). cg_volver(s, v): vuelve allí con v; abre las IRQ en modo SVC */
        " .global cg_guardar\n .type cg_guardar, %function\n"
        "cg_guardar:\n stmia r0, {r4-r11}\n str sp, [r0, #32]\n str lr, [r0, #36]\n mov r0, #0\n bx lr\n"
        " .global cg_volver\n .type cg_volver, %function\n"
        "cg_volver:\n msr cpsr_c, #0x13\n ldr sp, [r0, #32]\n ldr lr, [r0, #36]\n ldmia r0, {r4-r11}\n movs r0, r1\n moveq r0, #1\n bx lr\n");
typedef struct { u32 r[10]; } CgSalto;
int  cg_guardar(CgSalto *s) __attribute__((returns_twice));
void cg_volver(CgSalto *s, int v) __attribute__((noreturn));
static CgSalto salto;
#define GUARDAR() cg_guardar(&salto)
enum { F_EXCEPCION = 1, F_PERRO = 2, F_SALTO_NULO = 3, F_PM = 4, F_NULO_DATOS = 5 };
static void salto_nulo(void);
#else
__asm__(".text\n .align 2\n .global cg_saltar\n .type cg_saltar, %function\n"
        "cg_saltar:\n stp x29, x30, [sp, #-32]!\n stp x19, x20, [sp, #16]\n mov x29, sp\n mov x19, sp\n mov sp, x1\n mov x9, x0\n"
        " ldr x0, [x9, #8]\n ldr x1, [x9, #16]\n ldr x2, [x9, #24]\n ldr x3, [x9, #32]\n ldr x9, [x9]\n blr x9\n"
        " mov sp, x19\n ldp x19, x20, [sp, #16]\n ldp x29, x30, [sp], #32\n ret\n");
static sigjmp_buf salto;
#define GUARDAR() sigsetjmp(salto, 1)
enum { F_EXCEPCION = 1, F_PERRO = 2 };
/* ASan tiene que saber que cambiamos de pila (si no, da falsos positivos al volver de un fallo). */
#if defined(__SANITIZE_ADDRESS__)
void __sanitizer_start_switch_fiber(void **fake, const void *fondo, size_t tam);
void __sanitizer_finish_switch_fiber(void *fake, const void **fondo_ant, size_t *tam_ant);
void __asan_unpoison_memory_region(void const volatile *p, size_t n);
#define FIBRA_IR(f, fondo, tam) __sanitizer_start_switch_fiber(f, fondo, tam)
#define FIBRA_LLEGAR(f, fondo, tam) __sanitizer_finish_switch_fiber(f, fondo, tam)
#define DESENVENENAR(p, n) __asan_unpoison_memory_region(p, n)
#else
#define FIBRA_IR(f, fondo, tam) ((void)0)
#define FIBRA_LLEGAR(f, fondo, tam) ((void)0)
#define DESENVENENAR(p, n) ((void)0)
#endif
static const void *pc_fondo; static size_t pc_tam;         /* la pila del sistema, vista por ASan */
static void *pc_fibra;
#define PC_SEN 5
static struct sigaction pc_antes[PC_SEN];
static const int pc_sen[PC_SEN] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP };
static u32 pc_plazo_ms = CG_PLAZO_S * 1000;
static W5Pal pc_en_pila(W5Pal real, W5Pal b, W5Pal c, W5Pal d)   /* ya en la pila de la app */
{
    W5Pal *a = (W5Pal *)real, r;
    (void)b; (void)c; (void)d;
    FIBRA_LLEGAR(NULL, &pc_fondo, &pc_tam);
    r = ((W5Pal (*)(W5Pal, W5Pal, W5Pal, W5Pal))a[0])(a[1], a[2], a[3], a[4]);
    FIBRA_IR(NULL, pc_fondo, pc_tam);
    return r;
}
static void pc_senal(int s, siginfo_t *inf, void *u)
{
    int i;
    (void)u;
    if (cg.prof) {                                          /* la culpa es de la app: de vuelta a cg_llamar */
        cg.falta_tipo = s == SIGALRM ? F_PERRO : F_EXCEPCION;
        cg.falta_dir = inf ? (u32)(size_t)inf->si_addr : 0; cg.falta_pc = (u32)s;
        FIBRA_IR(NULL, pc_fondo, pc_tam);
        siglongjmp(salto, 1);
    }
    if (s == SIGALRM) return;
    for (i = 0; i < PC_SEN; i++) if (pc_sen[i] == s) sigaction(s, &pc_antes[i], NULL);   /* no es nuestra: que la vea quien estaba (ASan) */
}
static void pc_senales(void)
{
    static int puestas;
    struct sigaction sa;
    int i;
    if (puestas) return;
    puestas = 1;
    { const char *e = getenv("W5_PLAZO_MS"); if (e) pc_plazo_ms = (u32)atoi(e); }   /* (R3; calidad) una lectura, no dos */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = pc_senal; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < PC_SEN; i++) sigaction(pc_sen[i], &sa, &pc_antes[i]);
    sigaction(SIGALRM, &sa, NULL);
}
#include <time.h>
static u32 pc_entrada;
static u32 pc_real_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u32)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static void pc_alarma(u32 ms) { struct itimerval t; memset(&t, 0, sizeof t); t.it_value.tv_sec = ms / 1000; t.it_value.tv_usec = (ms % 1000) * 1000; setitimer(ITIMER_REAL, &t, NULL); }
#endif

static void vigia_muerte(Ctx *c);
void cg_matar(const char *motivo)
{
    if (cg.estado != CG_VIVA) return;
    cg.estado = CG_MUERTA;
    poner(cg.motivo, sizeof cg.motivo, motivo);
    cg_med.muertes++;
    hw_log("[cargador] app cerrada: "); hw_log(cg.cab.nombre); hw_log(": "); hw_log(cg.motivo); hw_log("\n");
    if (cg.raiz) ui_tras(cg.raiz, 1, vigia_muerte);         /* sus pantallas se quitan en el cuadro siguiente, no aquí dentro */
}
void cg_latido(void)                                         /* ap_latido(): prórroga, con tope */
{
    if (cg.en_juego) { app_latido(); return; }
#if TELEFONO
    if (hw_ms() - cg.t_entrada < CG_PLAZO_S * 1000u) hw_perro_latido();
#else
    if (pc_real_ms() - pc_entrada < pc_plazo_ms) pc_alarma(pc_plazo_ms);   /* (el reloj del guion no corre dentro de la app: el plazo va por el reloj real) */
#endif
}
static void revisar(void)
{
    const char *m = zonas_mirar();
    if (vec_reponer() && !m) m = "La app escribió en la dirección 0 (los vectores del procesador): repuestos.";
    if (m) cg_matar(m);
}
W5Pal cg_llamar(void *fn, W5Pal a, W5Pal b, W5Pal c, W5Pal d)
{
    volatile W5Pal r = 0;
    W5Pal args[5];
    if (cg.estado != CG_VIVA || !fn) return 0;
    if (cg.prof) {                                           /* la app llamó al sistema y el sistema la llama a ella: misma pila, mismo punto de vuelta */
        if (cg.prof > 12) { cg_matar("La app se llama a sí misma sin fin a través del sistema."); return 0; }
        cg.prof++;
#if TELEFONO
        if (cg_pm_activa()) {                                /* (PM) desde dentro de la puerta: otra vez a modo usuario, bajo su sp */
            u32 usp = cg_pm_usp() & ~7u;
            if (!usp) cg_pm_fallo("Llamada anidada a la app sin pila de usuario.");
            args[0] = (W5Pal)(size_t)fn; args[1] = a; args[2] = b; args[3] = c; args[4] = d;
            r = cg_usr(args, usp);
        } else
#endif
        r = ((W5Pal (*)(W5Pal, W5Pal, W5Pal, W5Pal))fn)(a, b, c, d);
        cg.prof--;
        return r;
    }
    cg_med.llamadas++;
    cg.t_entrada = hw_ms();
    hw_perro_plazo(CG_PLAZO_S);
#if TELEFONO
    args[0] = (W5Pal)(size_t)fn; args[1] = a; args[2] = b; args[3] = c; args[4] = d;
    if (GUARDAR() == 0) {
        cg.prof = 1;
        vec_trampa(salto_nulo);
        if (cg_pm_activa()) r = cg_usr(args, (u32)(size_t)(cg.pila + cg.tam_pila));   /* (PM) en MODO USUARIO */
        else r = cg_saltar(args, cg.pila + cg.tam_pila);
        vec_trampa(NULL);
        cg.prof = 0;
    } else {                                                 /* se volvió por un fallo: la pila de la app se abandona tal cual */
        vec_trampa(NULL);
        cg.prof = 0;
        cg_en_gate = 0;                                       /* (AUD9) se salió por un fallo DENTRO de la puerta: la cuenta se abandona */
        cg_pm_reiniciar();
        if (cg.falta_tipo == F_PERRO) cg_matar("La app no responde (30 s sin volver).");
        else if (cg.falta_tipo == F_PM) ;                    /* (PM) ya la mató la puerta, con su motivo */
        else if (cg.falta_tipo == F_SALTO_NULO) cg_matar("La app saltó a la dirección 0 (llamó a un puntero nulo).");
        else if (cg.falta_tipo == F_NULO_DATOS) cg_matar(ui_fmt("La app tocó la dirección %x (un puntero nulo): la MMU la ha parado.", cg.falta_dir));
        else if (cg_pm_activa() && cg.falta_tipo == F_EXCEPCION && (cg.falta_modo & 0x1F) == 0x10 && (cg.falta_exc == 3 || cg.falta_exc == 4))
            cg_matar(ui_fmt("La app tocó memoria que no es suya en %x (dirección %x): la MMU la ha parado.", cg.falta_pc, cg.falta_dir));
        else cg_matar(ui_fmt("Fallo de la app en %x (dirección %x).", cg.falta_pc, cg.falta_dir));
        r = 0;
    }
    hw_perro_plazo(10);
#else
    {
        W5Pal real[5];
        pc_senales();
        real[0] = (W5Pal)(size_t)fn; real[1] = a; real[2] = b; real[3] = c; real[4] = d;
        args[0] = (W5Pal)(size_t)pc_en_pila; args[1] = (W5Pal)(size_t)real; args[2] = args[3] = args[4] = 0;
        if (GUARDAR() == 0) {
            pc_entrada = pc_real_ms();
            if (!cg.en_juego) pc_alarma(pc_plazo_ms);
            cg.prof = 1;
            FIBRA_IR(&pc_fibra, cg.pila, cg.tam_pila);
            r = cg_saltar(args, cg.pila + cg.tam_pila);
            FIBRA_LLEGAR(pc_fibra, NULL, NULL);
            cg.prof = 0;
            pc_alarma(0);
        } else {
            FIBRA_LLEGAR(pc_fibra, NULL, NULL);
            cg.prof = 0;
            pc_alarma(0);
            DESENVENENAR(cg.pila, cg.tam_pila);
            if (cg.falta_tipo == F_PERRO) cg_matar("La app no responde (30 s sin volver).");
            else cg_matar(ui_fmt("Fallo de la app (señal %u, dirección %x).", cg.falta_pc, cg.falta_dir));
            r = 0;
        }
    }
#endif
    if (cg.estado == CG_VIVA) revisar();
    return r;
}

#if TELEFONO
/* plat/hw_w580.c, excepcion_c(): una excepción de la CPU con código de la app en curso. No vuelve: salta al punto
 * de vuelta de cg_llamar (que está en la pila del sistema, intacta) con las IRQ abiertas otra vez. */
void cg_fallo_cpu(const u32 *ctx)
{
    if (!cg.prof) return;                                    /* (aunque ya esté MUERTA: puede seguir dando vueltas dentro de su retrollamada) */
    cg.falta_tipo = F_EXCEPCION; cg.falta_pc = ctx[15]; cg.falta_dir = ctx[17] == 4 ? ctx[19] : ctx[15]; cg.falta_modo = ctx[16]; cg.falta_exc = ctx[17];
    if ((ctx[16] & 0x1F) == 0x10) {                         /* (PM) desde modo usuario: la MMU paró a la app */
        if (ctx[17] == 3 && ctx[15] < 0x20) cg.falta_tipo = F_SALTO_NULO;
        else if (ctx[17] == 4 && ctx[19] < 0x1000) cg.falta_tipo = F_NULO_DATOS;
    }
    vec_reponer();
    cg_volver(&salto, 1);
}
/* plat/hw_w580.c, perro_vigia(), DESDE LA IRQ DEL TIC: el bucle no late y hay código de la app en curso. Se cambia la
 * dirección de vuelta de la interrupción (está en la pila de la IRQ: arranque.S guarda {r0-r3, r12, lr}) por
 * perro_fuera(), en modo ARM: al acabar la IRQ el procesador no vuelve a la app sino ahí, y de ahí a cg_llamar.
 * LO QUE NO PUEDE: sacar a una app que haya cortado las interrupciones (no hay tic). */
extern char _pila_irq[];
static void salto_nulo(void) { cg.falta_tipo = F_SALTO_NULO; cg_volver(&salto, 1); }
static void perro_fuera(void) { cg.falta_tipo = F_PERRO; cg_volver(&salto, 1); }
void cg_pm_fallo(const char *motivo) { cg_matar(motivo); cg.falta_tipo = F_PM; cg_volver(&salto, 1); }
int cg_perro_irq(void)
{
    u32 *lr = (u32 *)((size_t)_pila_irq - 4), v;
    if (!cg.prof) return 0;
    if (cg_en_gate) { hw_perro_latido(); return 1; }         /* (AUD9) no sacar a la app a mitad de una llamada del sistema: se esperaría a que termine (una escritura de FS/Memoria no queda a medias). Las funciones del sistema terminan; las largas laten */
    *lr = (u32)(size_t)perro_fuera;
    __asm__ volatile("mrs %0, spsr\n bic %0, %0, #0xBF\n orr %0, %0, #0x13\n msr spsr_cxsf, %0" : "=&r"(v));   /* sin Thumb, con las IRQ abiertas y (PM) en modo SVC al volver: la app pudo estar en modo usuario */
    hw_perro_latido();
    return 1;
}
#else
void cg_fallo_cpu(const u32 *ctx) { (void)ctx; }
int  cg_perro_irq(void) { return 0; }
void cg_pm_fallo(const char *motivo) { cg_matar(motivo); cg.falta_tipo = F_EXCEPCION; FIBRA_IR(NULL, pc_fondo, pc_tam); siglongjmp(salto, 1); }
#endif

/* (PM) ui_arena con protección: la memoria de un cuadro sale de una arena DENTRO de la app (la del sistema, la app solo la
 * lee). Se vacía cuando se vacía la del sistema (su espacio libre sube: ha empezado otro cuadro) o cuando está vacía. */
void *cg_arena_app(u32 n)
{
    const void *ini; u32 tam, libre = ui_arena_libre();
    void ui_arena_tramo(const void **ini, u32 *tam);
    void *p;
    if (cg.estado != CG_VIVA || !cg.tam_arena) return NULL;
    ui_arena_tramo(&ini, &tam);
    if (libre > cg.arena_visto || libre == tam) cg.arena_pos = 0;
    cg.arena_visto = libre;
    n = (n + 7u) & ~7u;
    if (!n || n > cg.tam_arena - cg.arena_pos) return NULL;   /* (AUD9 A4) sin pánico ni cierre: NULL, y los que la usan tratan NULL como «sin texto» */
    p = cg.arena + cg.arena_pos; cg.arena_pos += n;
    memset(p, 0, n);
    return p;
}
void cg_arena_reset(void) { cg.arena_pos = 0; }                 /* (AUD9 A4) cada fotograma de un juego (t_jg_volcar): la arena del cuadro de la app no se acumula */

/* ---- las retrollamadas internadas ------------------------------------------------------------------------------------- */
static int fn_vale(void *f)
{
    size_t d = (size_t)f - (size_t)cg.img;
    if (!f) return 1;
    if (CG_MAQUINA != W5_MAQ_ARMV5TE && (d & 3)) return 0;
    return (size_t)f >= (size_t)cg.img && (d & ~(size_t)1) < cg.tam_cod;
}
CgRet *cg_ret(void *f0, void *f1, void *f2, void *dato)
{
    int i, libre = -1;
    if (cg.estado != CG_VIVA) return &ret_vacia;
    if (!fn_vale(f0) || !fn_vale(f1) || !fn_vale(f2)) { cg_matar("La app dio una retrollamada que no está en su código."); return &ret_vacia; }
    for (i = 0; i < CG_RETS; i++) {
        CgRet *t = &cg.ret[i];
        if (t->gen != cg.gen) { if (libre < 0) libre = i; continue; }
        if (t->fn[0] == f0 && t->fn[1] == f1 && t->fn[2] == f2 && t->dato == dato) return t;
    }
    if (libre < 0) { cg_matar("La app usa demasiadas retrollamadas distintas (128)."); return &ret_vacia; }
    cg.ret[libre] = (CgRet){ { f0, f1, f2 }, dato, cg.gen };
    if ((u32)libre + 1 > cg_med.rets) cg_med.rets = (u32)libre + 1;
    return &cg.ret[libre];
}
CgRet *cg_ret_de(void *dato)
{
    CgRet *t = dato;
    if (cg.estado != CG_VIVA || t < cg.ret || t >= cg.ret + CG_RETS || t->gen != cg.gen) return NULL;
    return t;
}

/* ---- la pausa ------------------------------------------------------------------------------------------------------- */
int cg_primer_plano(void)
{
    Ctx *r = ui_raiz();
    if (cg.en_juego) return 1;
    return cg.estado == CG_VIVA && r && r->cls == &ClsWCgPant && !ui_cima_emergente() && sh_pantalla_estado() != EN_APAGADA && !sh_bloqueado();
}
static void pausa_mirar(void)
{
    int pp = cg_primer_plano();
    if (cg.estado != CG_VIVA || cg.cerrando || !cg.n_pant) return;
    if (!pp && !cg.pausada) { cg.pausada = 1; cg_llamar((void *)cg.entrada, (W5Pal)(size_t)cg_pm_api(), W5_PAUSA, 0, 0); }
    else if (pp && cg.pausada) { cg.pausada = 0; cg_llamar((void *)cg.entrada, (W5Pal)(size_t)cg_pm_api(), W5_SIGUE, 0, 0); }
}
static void retro_n(Ctx *c, int i)
{
    pausa_mirar();
    if (cg.pausada) return;                                  /* en pausa los temporizadores de la app no laten */
    cg_llamar(cg.retro[i], (W5Pal)(size_t)c, 0, 0, 0);
}
#define RETRO(i) static void retro_##i(Ctx *c) { retro_n(c, i); }
RETRO(0) RETRO(1) RETRO(2) RETRO(3) RETRO(4) RETRO(5) RETRO(6) RETRO(7)
static const Retro retros[CG_RETROS] = { retro_0, retro_1, retro_2, retro_3, retro_4, retro_5, retro_6, retro_7 };
Retro cg_retro(W5Retro f, int crear)
{
    int i, libre = -1;
    if (cg.estado != CG_VIVA || !f) return NULL;
    for (i = 0; i < CG_RETROS; i++) { if (cg.retro[i] == (void *)f) return retros[i]; if (!cg.retro[i] && libre < 0) libre = i; }
    if (!crear) return NULL;
    if (!fn_vale((void *)f)) { cg_matar("La app dio un temporizador que no está en su código."); return NULL; }
    if (libre < 0) { cg_matar("La app usa demasiados temporizadores distintos (8)."); return NULL; }
    cg.retro[libre] = (void *)f;
    return retros[libre];
}

/* ---- 4. LA PANTALLA DE LA APP: una clase del SISTEMA que aloja (construir, iniciar, desechar, dato) de la app ------------- */
static Widget *pant_error(const char *titulo, const char *texto);
static void descargar_y_decir(void)
{
    static char m[120];                                     /* estático: ui_aviso copia el texto al MONTAR el diálogo, no ahora (lo cazó ASan) */
    int muerta = cg.estado == CG_MUERTA;
    poner(m, sizeof m, cg.motivo);
    cg_descargar();
    if (muerta) ui_aviso("App cerrada", m);
    cg12_d_tras_descargar();                                /* (APX) la que esperaba (Walkman desde dentro de otra app) */
}
static void vigia_muerte(Ctx *c)
{
    if (cg.estado != CG_MUERTA) return;
    ui_quitar_ruta(&ClsWCgPant);                             /* de arriba abajo, una por cuadro; la raíz es la última y con ella se va este temporizador */
    if (cg.rutas > 1) ui_tras(c, 1, vigia_muerte);
}
static void vigia(Ctx *c) { (void)c; pausa_mirar(); if (cg.estado == CG_VIVA) revisar(); }
static Widget *pant_construir(Ctx *c)
{
    const WCgPant *w = ui_widget(c);
    CgRet *t = cg_ret_de(w->r);
    Widget *h = t ? cg_widget((void *)cg_llamar(t->fn[0], (W5Pal)(size_t)c, (W5Pal)(size_t)t->dato, 0, 0)) : NULL;
    if (!h && cg.estado == CG_VIVA && t) cg_matar("La app no devolvió una pantalla válida.");
    return h ? h : Caja(.an = INF, .al = INF, .color = sh_tema()->fondo);
}
static void pant_iniciar(Ctx *c)
{
    const WCgPant *w = ui_widget(c);
    CgRet *t = cg_ret_de(w->r);
    if (!t) return;
    cg.n_pant++;
    if (!c->padre) cg.rutas++;
    if (!cg.raiz) { cg.raiz = c; ui_cada(c, 1000, vigia); }
    *(u32 *)ui_estado(c) = cg.gen;
    cg_llamar(t->fn[1], (W5Pal)(size_t)c, (W5Pal)(size_t)t->dato, 0, 0);
}
static void pant_desechar(Ctx *c)
{
    const WCgPant *w = ui_widget(c);
    CgRet *t;
    if (cg.estado == CG_LIBRE || *(u32 *)ui_estado(c) != cg.gen) return;   /* de una app que ya no está */
    if ((t = cg_ret_de(w->r)) != NULL) cg_llamar(t->fn[2], (W5Pal)(size_t)c, (W5Pal)(size_t)t->dato, 0, 0);
    if (!c->padre && cg.rutas) cg.rutas--;
    if (cg.raiz == c) cg.raiz = NULL;
    if (cg.n_pant && --cg.n_pant == 0) {                     /* la última: aquí ya no queda en el árbol nada de la app */
        cg.cerrando = 1;
        cg_llamar((void *)cg.entrada, (W5Pal)(size_t)cg_pm_api(), W5_SALIR, 0, 0);
        descargar_y_decir();
    }
}
const Clase ClsWCgPant = { .nombre = "AppCargable", .tipo = T_CON_ESTADO, .tam = sizeof(WCgPant), .tam_estado = sizeof(u32),
                           .construir = pant_construir, .iniciar = pant_iniciar, .desechar = pant_desechar };
Widget *cg_pantalla(void *construir, void *iniciar, void *desechar, void *dato)
{
    CgRet *t;
    if (!construir) { cg_matar("ui_pantalla sin función de construir."); return NULL; }
    t = cg_ret(construir, iniciar, desechar, dato);
    if (t == &ret_vacia) return NULL;
    return ui_nuevo(&(WCgPant){ .base = { &ClsWCgPant, 0 }, .r = t }, sizeof(WCgPant));
}
Ctx *cg_mi_pantalla(Ctx *c) { return !c ? NULL : c->cls == &ClsWCgPant ? c : ui_ancestro(c, &ClsWCgPant); }
void cg_salir(void)
{
    int i;
    if (cg.estado != CG_VIVA || cg.en_juego) return;
    for (i = 0; i < cg.rutas; i++) ui_quitar_ruta(&ClsWCgPant);
}

/* ---- la pantalla que dice por qué no ------------------------------------------------------------------------------------ */
static char err_txt[2][160];
static int err_tecla(Ctx *c, int k, void *dato) { if (k == K_SEL_DER || k == K_ATRAS || k == K_OK || k == K_SEL_IZQ) { ui_sacar(); return 1; } (void)c; (void)dato; return 0; }
/* (APX) La pantalla es un COMPONENTE (se construye cada vez de err_txt), no un árbol suelto de la arena: si la ruta se
 * reconstruía en otro cuadro (p. ej. el «App cerrada» de una app que se abrió encima), los hijos del árbol viejo ya no
 * existían (UBSan: ui_nucleo.c:222, Clase nula). Salía con el standby -> sel. der. -> Reloj ausente -> abrir otra app y que muera. */
typedef struct { Widget base; } CgError;
static Widget *err_construir(Ctx *c)
{
    const DisTokens *k = dis_tokens();
    (void)c;
    /* (MD) el error, en una tarjeta con los colores de error del diseño */
    return Teclas(err_tecla, Pantalla(err_txt[0], Relleno(8, Tarjeta(Columna(.espacio = 8, .cruzado = P_ESTIRAR, .hijos = HIJOS(
        Texto("No se puede abrir", .color = k->c.error, .marcas = F_ESTILO(DIS_T_TITULO)),
        Parrafo(err_txt[1], .color = k->c.sobre_superficie_variante))), 0)), "", "Atrás"));
}
COMPONENTE(CgError, err_construir);
static Widget *pant_error(const char *titulo, const char *texto)
{
    const TemaDatos *t = sh_tema();
    poner(err_txt[0], sizeof err_txt[0], titulo); poner(err_txt[1], sizeof err_txt[1], texto);
    hw_log("[cargador] no se abre «"); hw_log(err_txt[0]); hw_log("»: "); hw_log(err_txt[1]); hw_log("\n");
    (void)t;
    return UI_W(CgError);
}

/* ---- abrir un fichero ------------------------------------------------------------------------------------------------- */
static u32 leer_fs(void *quien, u32 desde, void *buf, u32 max) { return hw_fs_leer_desde(quien, desde, buf, max); }
/* (EST3, 26-oct) LA COPIA NUEVA MANDA. Las .sen de fábrica viven en /apps (el W6FS), que ni el BL ni senew reescriben: en un aparato
 * ya grabado siguen las de antes (API < 1.7: letra pequeña, modo v1) hasta pasar por Far. Las nuevas se suben a /memoria/apps con
 * el MISMO nombre; si esa copia está y su cabecera vale, se abre ELLA: desde su celda, desde Ajustes (Luces), desde Archivos o desde
 * «Apps» (donde la vieja ya no sale: descubrir_en). Si no vale, la de /apps, como siempre. */
static int empieza(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
const char *cg_ruta_nueva(const char *ruta)
{
    static char r[96];
    W5xCab c;
    char m[8];
    u32 tam;
    if (!ruta || !empieza(ruta, "/apps/") || hw_modo_seguro() || strlen(ruta) + sizeof HW_FS_MEMORIA > sizeof r) return ruta;
    poner(r, sizeof r, HW_FS_MEMORIA); poner(r + sizeof HW_FS_MEMORIA - 1, sizeof r - (sizeof HW_FS_MEMORIA - 1), ruta);
    tam = hw_fs_tam(r);
    if (!tam || hw_fs_leer_desde(r, 0, (u8 *)&c, W5X_TAM_CAB) != W5X_TAM_CAB || cg_cab_vale(&c, tam, m, sizeof m)) return ruta;
    return r;
}
static void juego_correr(void)                               /* perfil JUEGO: la app se queda con el bucle hasta que vuelve */
{
    cg.en_juego = 1;
    cg_llamar((void *)cg.entrada, (W5Pal)(size_t)cg_pm_api(), W5_ABRIR, 0, 0);
    cg.en_juego = 0;
    sh_app_externa_recoger();            /* (R3) UN solo mecanismo, el de R2 (shell/app_externa.c): si salió —o se la sacó— sin soltar la
                                          * pantalla, la recoge el sistema; y con ella su gancho del tic, su tono, el vibrador y las claves */
    descargar_y_decir();
    ui_forzar_todo();
}
Widget *cg_abrir_ruta(const char *ruta)
{
    char m[160];
    static char r[96];
    Widget *w;
    u32 tam;
    const char *nom = ruta;
    cg12_d_al_abrir();                                      /* (1.2) sin cg_abrir_con delante, la app se abre sin argumento */
    poner(r, sizeof r, cg_ruta_nueva(ruta));                /* la ruta puede venir de la arena. (EST3) la copia nueva, si la hay */
    for (const char *p = ruta; *p; p++) if (*p == '/') nom = p + 1;
    tam = hw_fs_tam(r);
    if (!tam) return pant_error(nom, "No se puede leer el fichero.");
    if (cg_cargar_de(leer_fs, r, tam, r, m, sizeof m)) return pant_error(nom, m);
    hw_log(ui_fmt("[cargador] «%s» cargada de %s: %u B de fichero, %u B de RAM, %u reubicaciones, %u ms\n", cg.cab.nombre, r, tam, cg.tam_bloque, cg.cab.n_reub, cg_med.t_carga_ms));
    if (cg.perfil == W5_PERFIL_JUEGO) {
        juego_correr();
        return NULL;                                         /* el juego ya ha corrido y ha vuelto: no hay pantalla que empujar */
    }
    w = (Widget *)cg_llamar((void *)cg.entrada, (W5Pal)(size_t)cg_pm_api(), W5_ABRIR, 0, 0);
    if (cg.estado != CG_VIVA || !w || !ui_en_arena(w) || w->cls != &ClsWCgPant) {
        poner(m, sizeof m, cg.estado == CG_MUERTA ? cg.motivo : "La app no dio su pantalla al abrirse.");
        cg_descargar();
        return pant_error(nom, m);
    }
    return w;
}

/* ---- 5. DESCUBRIR: los .sen de /apps (nuestro FS) y de Mis archivos, y al menú -------------------------------------------- */
/* CG_MAX y CG_CELDAS: cargador.h (LANZ) */
#define CG_AUTO 4                                           /* (LANZ) lo más que el orden AUTOMÁTICO (§28) pone solo en el menú: antes era CG_MENU */
#define CG_MENU M3_SEN_MAX                                  /* (ICO) huecos de MALLA 3D para las .sen del menú (3 KiB + 640 B de rampas cada uno). (LANZ) ya no
                                                             * es «una por celda»: el hueco se lo lleva, por orden de celda, la que TRAE una malla que vale.
                                                             * (28-oct) 7: las de fábrica con malla propia (Cámara, YT Music, Grabadora) + las CG_AUTO */
typedef struct { char ruta[96]; char nombre[30]; u8 perfil, icono_gen; Icono ic; u16 bits[12]; u8 malla; } CgApp;   /* (ICO) malla: 1 = trae una y vale */
static CgApp apps[CG_MAX];
/* Las del MENÚ, COPIADAS: `apps` se vuelve a llenar al abrir la lista «Apps» (con las de fábrica, en otro orden), y las celdas
 * del menú apuntaban a apps[i]: tras abrir la lista, el icono de un juego abría la Calculadora (el mantenedor, 2026-10-15). */
static CgApp menu_a[CG_CELDAS];                            /* (LANZ) una por celda que puede ocupar una .sen (eran CG_MENU = 4) */
/* (ICO) las mallas 3D de las .sen que van al menú (3 KiB cada una como mucho). Se leen y se validan al armar el menú
 * (shell/cg_icono.c); el menú 3D guarda el puntero a ico_m[h], que apunta dentro de ico_buf[h]. */
static u8 ico_buf[CG_MENU][W5X_ICO_MAX];
static AppMalla ico_m[CG_MENU];
static int huecos;                                          /* (LANZ) huecos de malla ya dados en este armado del menú */
static int n_apps;
static int termina_sen(const char *s)                       /* (I1) sin mirar mayúsculas: en la tarjeta (FAT) es «HOLA.SEN» */
{
    size_t n = strlen(s), i;
    if (n <= 4) return 0;
    for (i = 0; i < 4; i++) { char a = s[n - 4 + i]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32); if (a != SEN_EXT[i]) return 0; }
    return 1;
}
static u8 en_menu;                                          /* (APX) descubriendo para el menú (no para la lista «Apps», que las enseña todas) */
enum { F_TODO, F_SIN_JUEGOS, F_SOLO_JUEGOS };
static u8 filtro;                                           /* (28-oct) los juegos van en «Juegos»: ni en las celdas automáticas ni en «Apps» */
static int mismo_nombre(const char *a, const char *b, u32 n)   /* (28-oct) los dos nombres visibles iguales en sus n bytes como mucho */
{
    u32 i;
    for (i = 0; i < n; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; }
    return 1;
}
/* (28-oct) ¿`nombre` en `dir` (la tarjeta o Mis archivos) es una copia de una .sen de fábrica (/apps/nombre, no retirada)? Las copias de
 * desarrollo en /tarjeta/apps (ytmusic.sen cuando aún se llamaba «YT Music») salían como OTRA app. Para poner una versión nueva de una
 * de fábrica está la Memoria (/memoria/apps: cg_ruta_nueva), que no pasa por aquí */
static int copia_de_fabrica(const char *dir, const char *nombre)
{
    static char r[96];
    if (!strcmp(dir, "/apps") || empieza(dir, HW_FS_MEMORIA) || strlen(nombre) + 7 > sizeof r) return 0;
    poner(r, sizeof r, "/apps/"); poner(r + 6, sizeof r - 6, nombre);
    { char *p; for (p = r + 6; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32); }   /* en la tarjeta (FAT) es «YTMUSIC.SEN» */
    return !sh_ruta_retirada(r) && hw_fs_tam(r) > 0;
}
int cg_copia_de_fabrica(const char *dir, const char *nombre) { return copia_de_fabrica(dir, nombre); }   /* (Fase 3b) SEN_APPS: la que no sale, «tapada» */
static void descubrir_en(const char *dir)
{
    HwEntrada e[8];
    u32 desde = 0;
    int k, i;
    for (;;) {
        k = hw_fs_listar(dir, desde, e, 8);
        if (k <= 0) return;
        for (i = 0; i < k && n_apps < CG_MAX; i++) {
            CgApp *a = &apps[n_apps];
            W5xCab c;
            char m[8];
            if (e[i].es_dir || !termina_sen(e[i].nombre) || strlen(dir) + strlen(e[i].nombre) + 2 > sizeof a->ruta) continue;
            poner(a->ruta, sizeof a->ruta, ui_fmt("%s/%s", dir, e[i].nombre));
            if (en_menu && sh_ruta_de_fabrica(a->ruta)) continue;   /* (APX) ya tiene su celda en la disposición de fábrica */
            if (sh_ruta_retirada(a->ruta) || cg_ruta_nueva(a->ruta) != a->ruta) continue;   /* (EST3) retirada, o la vieja de una que tiene copia nueva */
            if (copia_de_fabrica(dir, e[i].nombre)) {        /* (28-oct) una copia VIEJA de una de fábrica en la tarjeta o en Mis archivos */
                hw_log(ui_fmt("[cargador] %s: copia de /apps/%s; no sale (la que vale es la de fábrica o su copia en la Memoria)\n", a->ruta, e[i].nombre));
                continue;
            }
            if (hw_fs_leer_desde(a->ruta, 0, (u8 *)&c, W5X_TAM_CAB) != W5X_TAM_CAB || cg_cab_vale(&c, e[i].tam, m, sizeof m)) continue;   /* la que no vale no sale en el menú (sí en Archivos, que dice por qué) */
            if ((filtro == F_SIN_JUEGOS && c.perfil == W5_PERFIL_JUEGO) || (filtro == F_SOLO_JUEGOS && c.perfil != W5_PERFIL_JUEGO)) continue;   /* (28-oct) */
            {   /* (28-oct) SIN REPETIDAS (el mantenedor: «no deben quedar apps repetidas en el menú»): una app con el MISMO nombre visible que otra ya
                 * descubierta (/apps va primero) o que una celda de la disposición de fábrica no sale otra vez; p. ej. la copia de una de fábrica en
                 * /tarjeta/apps (la de /memoria/apps ya la quitaba la regla de arriba: es la copia NUEVA y abre en su lugar) */
                int j, rep = 0;
                for (j = 0; j < n_apps && !rep; j++) rep = mismo_nombre(apps[j].nombre, c.nombre, sizeof a->nombre);
                for (j = 0; en_menu && j < sh_n_apps && !rep; j++)   /* contra las celdas de CASA (o de fábrica) solamente: desde Ajustes el menú ya
                                                                      * lleva las descubiertas y cada una se quitaba a sí misma [observado, 19_lanzador] */
                    rep = sh_apps[j].nombre && sh_apps[j].id && (sh_apps[j].id[0] == '@' || sh_ruta_de_fabrica(sh_apps[j].id)) &&
                          mismo_nombre(sh_apps[j].nombre, c.nombre, sizeof a->nombre);
                if (rep) { char nm[sizeof a->nombre + 1]; memcpy(nm, c.nombre, sizeof a->nombre); nm[sizeof a->nombre] = 0;
                           hw_log(ui_fmt("[cargador] %s: «%s» ya está en el menú; no sale otra vez\n", a->ruta, nm)); continue; }
            }
            memcpy(a->nombre, c.nombre, sizeof a->nombre);
            a->perfil = c.perfil; a->icono_gen = c.icono_gen < IC_N ? c.icono_gen : IC_FICHERO;
            memcpy(a->bits, c.icono, sizeof a->bits);
            a->ic = (Icono){ c.icono_an, 12, a->bits };
            a->malla = 0;                                     /* (ORDEN) la malla se lee en cg_al_menu, YA ordenadas: va con la de la celda */
            n_apps++;
        }
        desde += (u32)k;
        if (k < 8) return;
    }
}
/* (ORDEN, 2026-10-16) Un orden FIJO, no el de llegada de los ficheros: primero las apps (perfil que no es juego), después los
 * juegos; dentro de cada grupo por nombre visible sin mirar mayúsculas (A-Z ASCII; el resto de bytes tal cual) y, a igual nombre,
 * por ruta: orden total, el mismo en cada arranque. Inserción en el sitio, sin memoria dinámica (CG_MAX = 32). */
static int cg_antes(const CgApp *a, const CgApp *b)        /* <0: a va antes que b */
{
    const u8 *x = (const u8 *)a->nombre, *y = (const u8 *)b->nombre;
    int ja = a->perfil == W5_PERFIL_JUEGO, jb = b->perfil == W5_PERFIL_JUEGO, i;
    if (ja != jb) return ja - jb;
    for (i = 0; i < (int)sizeof a->nombre; i++) {
        int p = x[i], q = y[i];
        if (p >= 'A' && p <= 'Z') p += 32;
        if (q >= 'A' && q <= 'Z') q += 32;
        if (p != q) return p - q;
        if (!p) break;
    }
    return strcmp(a->ruta, b->ruta);
}
static void ordenar_apps(void)
{
    static CgApp t;
    int i, j;
    for (i = 1; i < n_apps; i++) {
        if (cg_antes(&apps[i - 1], &apps[i]) <= 0) continue;
        t = apps[i];
        for (j = i; j > 0 && cg_antes(&apps[j - 1], &t) > 0; j--) apps[j] = apps[j - 1];
        apps[j] = t;
    }
    for (i = 0; i < n_apps; i++) apps[i].ic.filas = apps[i].bits;   /* el arte apunta a SUS bits: al mover la ficha, apuntaba a la del hueco viejo */
}
static Widget *abrir_i(int i) { Widget *w = cg_abrir_ruta(menu_a[i].ruta); return w ? w : pant_error(menu_a[i].nombre, "La app no dio su pantalla."); }
static void entrar_i(int i) { Widget *w = cg_abrir_ruta(menu_a[i].ruta); if (w) ui_empujar(w); }   /* un juego: corre dentro y vuelve; w solo si no se pudo abrir */
#define MENU(i) static Widget *abrir_##i(void) { return abrir_i(i); } static void entrar_##i(void) { entrar_i(i); }
MENU(0) MENU(1) MENU(2) MENU(3) MENU(4) MENU(5) MENU(6) MENU(7) MENU(8) MENU(9) MENU(10) MENU(11) MENU(12)
static Widget *(*const abrir_v[CG_CELDAS])(void) = { abrir_0, abrir_1, abrir_2, abrir_3, abrir_4, abrir_5, abrir_6, abrir_7, abrir_8, abrir_9, abrir_10, abrir_11, abrir_12 };
static void (*const entrar_v[CG_CELDAS])(void) = { entrar_0, entrar_1, entrar_2, entrar_3, entrar_4, entrar_5, entrar_6, entrar_7, entrar_8, entrar_9, entrar_10, entrar_11, entrar_12 };
typedef char cg_celdas_cuadran[(int)(sizeof abrir_v / sizeof abrir_v[0]) == CG_CELDAS ? 1 : -1];   /* (LANZ) un trampolín por celda */

/* La pantalla «Apps»: todas las descubiertas (vuelve a mirar al abrirse). Sale en el menú cuando no caben todas. */
typedef struct { Widget base; } CgLista;
extern const Clase ClsCgLista;
static Widget *lista_fila(Ctx *c, int i, int sel, void *dato)
{
    const TemaDatos *t = sh_tema();
    (void)c; (void)dato;
    DisFila f = { .titulo = apps[i].nombre, .icono = apps[i].ic.w ? &apps[i].ic : &sh_iconos[apps[i].icono_gen],   /* (MD) fila del diseño */
                  .accesorio = DIS_ACC_VALOR, .valor = apps[i].perfil == W5_PERFIL_JUEGO ? "juego" : NULL };
    (void)t;
    return dis_actual()->fila(&f, sel);
}
static void lista_activar(Ctx *c, int i, void *dato)
{
    Widget *w;
    (void)c; (void)dato;
    if (i < 0 || i >= n_apps) return;
    w = cg_abrir_ruta(apps[i].ruta);
    if (w) ui_empujar(w);
}
static void lista_iniciar(Ctx *c) { (void)c; n_apps = 0; filtro = F_SIN_JUEGOS; if (hw_fs_listo()) { descubrir_en("/apps"); descubrir_en(HW_FS_USUARIO); } descubrir_en("/tarjeta/apps"); descubrir_en(HW_FS_MEMORIA "/apps"); filtro = F_TODO; ordenar_apps(); }   /* (28-oct) sin los juegos: están en «Juegos» */   /* (ORDEN) en el mismo orden que el menú. (I1) y las de la tarjeta; (SER) y las de la Memoria */   /* (I1) y las de la tarjeta */
static Widget *lista_construir(Ctx *c)
{
    (void)c;
    return Teclas(sh_teclas, Pantalla("Apps", n_apps ? ListaPerezosa(.n = n_apps, .fila = lista_fila, .al_activar = lista_activar)
        : Relleno(8, Parrafo("No hay ninguna app (.sen) en /apps, en Mis archivos ni en /tarjeta/apps.")), n_apps ? "Abrir" : "", "Atrás"));
}
const Clase ClsCgLista = { .nombre = "CgLista", .tipo = T_CON_ESTADO, .tam = sizeof(CgLista), .tam_estado = 4, .construir = lista_construir, .iniciar = lista_iniciar };
static Widget *abrir_lista(void) { return UI_W(CgLista); }

/* ---- (28-oct) «JUEGOS»: la celda del sistema con los juegos de la TARJETA --------------------------------------------------
 * el mantenedor: «los juegos deben poderse encontrar desde un nuevo acceso en el menú
 * llamado Juegos, con un icono de un mando; todos los juegos almacenados y abiertos desde la
 * memoria externa, nunca desde la NOR». Lista TODAS las .sen de /tarjeta/juegos y las de perfil JUEGO de /tarjeta/apps (de NINGÚN
 * otro sitio). Ya no ocupan las celdas del orden automático (§28) ni salen en «Apps» (filtro F_SIN_JUEGOS); el que el usuario ponga a
 * mano en Ajustes -> «Menú principal» sí sale. Sus recursos se copian a RAM al abrirlo (la ventana de M5X, arriba): la NOR no se
 * toca. El icono (una malla y su arte) está en shell/icono_juegos.h (generado). */
#include "icono_juegos.h"
typedef struct { Widget base; } CgJuegos;
extern const Clase ClsCgJuegos;
static Widget *juegos_fila(Ctx *c, int i, int sel, void *dato)
{
    (void)c; (void)dato;
    DisFila f = { .titulo = apps[i].nombre, .icono = apps[i].ic.w ? &apps[i].ic : &sh_iconos[apps[i].icono_gen] };
    return dis_actual()->fila(&f, sel);
}
static void juegos_iniciar(Ctx *c)
{
    (void)c; n_apps = 0;
    if (hw_modo_seguro()) return;                            /* (S7) en modo seguro no se carga nada de fuera */
    descubrir_en("/tarjeta/juegos");                         /* lo de la carpeta «juegos», TODO (la Serpiente es de widgets, no de perfil JUEGO) */
    filtro = F_SOLO_JUEGOS; descubrir_en("/tarjeta/apps"); filtro = F_TODO;   /* y de «apps», los de perfil JUEGO (los que ya no salen en el menú) */
    ordenar_apps();
}
static Widget *juegos_construir(Ctx *c)
{
    (void)c;
    return Teclas(sh_teclas, Pantalla("Juegos", n_apps ? ListaPerezosa(.n = n_apps, .fila = juegos_fila, .al_activar = lista_activar)
        : Relleno(8, Parrafo("No hay juegos en la tarjeta de memoria. Copia sus ficheros .sen a la carpeta «juegos» de la tarjeta.")),
        n_apps ? "Jugar" : "", "Atrás"));
}
const Clase ClsCgJuegos = { .nombre = "CgJuegos", .tipo = T_CON_ESTADO, .tam = sizeof(CgJuegos), .tam_estado = 4, .construir = juegos_construir, .iniciar = juegos_iniciar };
Widget *cg_abrir_juegos(void) { return UI_W(CgJuegos); }
/* (28-oct, Medios) los MISMOS juegos para la categoría «Juegos» del menú de medios (shell/menu_medios.c), con su malla 3D si la traen
 * (a un hueco libre, detrás de los del menú: valen hasta el próximo armado del menú). Llamarla UNA vez por armado (sh_apps_gen). */
int cg_juegos_descubrir(int *mallas, int max)
{
    int i;
    juegos_iniciar(NULL);
    for (i = 0; i < n_apps && i < max; i++) {
        W5xCab c;
        int r = 0;
        mallas[i] = 0;
        if (huecos < CG_MENU && hw_fs_leer_desde(apps[i].ruta, 0, (u8 *)&c, W5X_TAM_CAB) == W5X_TAM_CAB) r = cg_ico_leer(apps[i].ruta, &c, ico_buf[huecos], &ico_m[huecos]);
        if (r == 1) { mallas[i] = m3_malla_sen(huecos, &ico_m[huecos]) + 1; huecos++; }
    }
    return n_apps;
}
static const Icono juegos_ic = { JUEGOS_ARTE_AN, 12, juegos_arte };
const Icono *cg_juegos_arte(void) { return &juegos_ic; }
int cg_juegos_malla(void)                                    /* para App.malla: su índice + 1 (0 = la de IC_JUEGO si no vale o no cabe) */
{
    static AppMalla m;
    static int ind = -2;
    if (ind == -2) ind = cg_ico_validar(juegos_malla, sizeof juegos_malla, &m) ? -1 : m3_malla_externa(&m);
    return ind + 1;
}

/* (APX) Una celda de la disposición de fábrica (shell/apps.c): el .sen tiene que estar y valer. Da su nombre y su icono
 * genérico (el mismo número que el de casa: el menú pinta el mismo icono y la misma malla). 0 = vale. */
u32 cg_caps_app(void) { return cg.estado == CG_LIBRE ? 0 : cg.cab.caps; }
int cg_de_fabrica(const char *ruta, char *nombre, u32 max, int *icono)
{
    W5xCab c;
    char m[8];
    u32 tam;
    if (hw_modo_seguro() || !hw_fs_listo() || max < 2) return -1;     /* (S7) en modo seguro no se carga nada de fuera */
    ruta = cg_ruta_nueva(ruta);                                        /* (EST3) el nombre y el icono, de la que se va a abrir */
    tam = hw_fs_tam(ruta);
    if (!tam || hw_fs_leer_desde(ruta, 0, (u8 *)&c, W5X_TAM_CAB) != W5X_TAM_CAB || cg_cab_vale(&c, tam, m, sizeof m)) return -1;
    poner(nombre, max, c.nombre);
    *icono = c.icono_gen < IC_N ? c.icono_gen : IC_FICHERO;
    return 0;
}

/* (LANZ) LA CELDA k DEL MENÚ para la .sen `src`: COPIADA a menu_a[k] con su ruta (la celda guarda lo que abre; nunca un índice de
 * `apps`, que se vuelve a llenar al abrir la lista «Apps»: la regresión del 2026-10-15, el icono de un juego abría la Calculadora) y
 * su malla 3D, si la trae y vale, al siguiente hueco libre. Sin malla o sin hueco: la genérica de su icono (malla = 0), NUNCA la
 * de otra app (los huecos que sobran se vacían en menu_fin). */
static App celda(int k, const CgApp *src)
{
    CgApp *a = &menu_a[k];
    int m = 0;
    *a = *src; a->ic.filas = a->bits; a->malla = 0;          /* (la copia apunta a SUS bits) */
    if (huecos < CG_MENU) {                                  /* (ICO)(ORDEN) su malla 3D, si la trae: leída ENTERA y validada */
        W5xCab c;
        int r = hw_fs_leer_desde(a->ruta, 0, (u8 *)&c, W5X_TAM_CAB) == W5X_TAM_CAB ? cg_ico_leer(a->ruta, &c, ico_buf[huecos], &ico_m[huecos]) : 0;
        if (r < 0) hw_log(ui_fmt("[cargador] %s trae una malla de icono que no vale: la genérica\n", a->ruta));
        if (r == 1) { a->malla = 1; m = m3_malla_sen(huecos, &ico_m[huecos]) + 1; huecos++; }   /* (ICO) -1 + 1 = 0: la de `icono` */
    }
    return (App){ a->nombre, a->icono_gen, a->perfil == W5_PERFIL_JUEGO ? 0 : abrir_v[k],
                  a->perfil == W5_PERFIL_JUEGO ? entrar_v[k] : 0, .arte = a->ic.w ? &a->ic : NULL, .malla = m, .id = a->ruta };
}
static void menu_fin(void) { int j; for (j = huecos; j < CG_MENU; j++) m3_malla_sen(j, NULL); }   /* (ICO) los huecos que sobran, vacíos */
static void descubrir_menu(void)                            /* las .sen que NO son de la disposición de fábrica, en el orden de §28 */
{
    n_apps = 0;
    en_menu = 1;
    if (hw_fs_listo()) { descubrir_en("/apps"); descubrir_en(HW_FS_USUARIO); }
    descubrir_en("/tarjeta/apps");                           /* (I1) las de la tarjeta de memoria (detrás: no mueven las de casa) */
    descubrir_en(HW_FS_MEMORIA "/apps");                     /* (SER) las de la Memoria (lo que llega por «Recibir por serie»): las últimas */
    en_menu = 0;
    ordenar_apps();                                          /* (ORDEN) apps antes que juegos, por nombre: no el que llegó primero */
}

/* El menú POR OMISIÓN (el usuario no ha elegido nada en Ajustes -> «Menú principal»): tras la disposición de fábrica, las
 * descubiertas en el orden de §28 mientras quepan (CG_AUTO como mucho) y, si no caben todas, la celda «Apps». */
int cg_al_menu(App *v, int n, int max)
{
    int i, caben;
    if (hw_modo_seguro()) return n;                          /* (S7) en modo seguro no hay apps de fuera */
    filtro = F_SIN_JUEGOS; descubrir_menu(); filtro = F_TODO;   /* (28-oct) los juegos, en «Juegos» */
    caben = max - n;
    if (caben > CG_AUTO) caben = CG_AUTO;
    if (n_apps > caben) caben--;                             /* no caben todas: el último hueco es la lista «Apps» */
                                                             /* (28-oct) los huecos de malla NO se vacían aquí: las de fábrica ya han
                                                              * cogido los suyos (cg_icono_fabrica); se empieza en cg_menu_empezar */
    for (i = 0; i < n_apps && i < caben; i++) v[n++] = celda(i, &apps[i]);
    menu_fin();                                              /* (juegos600) con OTRA variable que i: con 3 o 4 apps de las que solo cabían
                                                              * 2, la celda «Apps» no salía y las demás quedaban sin entrada */
    if (n_apps > i && n < max) v[n++] = cg_celda_apps();
    return n;
}
/* (LANZ) El menú ELEGIDO por el usuario (shell/apps.c): se empieza, se pide cada .sen por su RUTA en la celda k (0..CG_CELDAS-1) y
 * se termina. Una ruta que ya no está o que no vale da -1 y no ocupa nada (el usuario la borró: se ignora sin romper nada). */
void cg_menu_empezar(void) { huecos = 0; }
int cg_menu_sen(App *v, int k, const char *ruta)
{
    static CgApp t;
    W5xCab c;
    char m[8];
    u32 tam;
    if (k < 0 || k >= CG_CELDAS || !ruta || hw_modo_seguro() || !termina_sen(ruta) || strlen(ruta) >= sizeof t.ruta) return -1;
    tam = hw_fs_tam(ruta);
    if (!tam || hw_fs_leer_desde(ruta, 0, (u8 *)&c, W5X_TAM_CAB) != W5X_TAM_CAB || cg_cab_vale(&c, tam, m, sizeof m)) return -1;
    memset(&t, 0, sizeof t);
    poner(t.ruta, sizeof t.ruta, ruta);
    memcpy(t.nombre, c.nombre, sizeof t.nombre); t.nombre[sizeof t.nombre - 1] = 0;
    t.perfil = c.perfil; t.icono_gen = c.icono_gen < IC_N ? c.icono_gen : IC_FICHERO;
    memcpy(t.bits, c.icono, sizeof t.bits);
    t.ic = (Icono){ c.icono_an, 12, t.bits };
    *v = celda(k, &t);
    return 0;
}
void cg_menu_terminar(void) { menu_fin(); }
/* (28-oct) El icono PROPIO de una .sen de la disposición de fábrica (shell/apps.c), si lo trae (como lo traen las descubiertas, celda):
 * su arte de 1 bit en `arte`/`bits` (arte->w = 0 si no trae) y, con `con_malla`, su malla 3D al siguiente hueco. Devuelve la malla
 * para App.malla (0 = la genérica de su icono). Las de fábrica de antes no traen nada: salen como siempre. */
int cg_icono_fabrica(const char *ruta, Icono *arte, u16 *bits, int con_malla)
{
    W5xCab c;
    char m[8];
    u32 tam;
    int r;
    arte->w = 0;
    if (hw_modo_seguro() || !hw_fs_listo()) return 0;
    ruta = cg_ruta_nueva(ruta);                              /* (EST3) la de la copia nueva, si la hay: la que se abre */
    tam = hw_fs_tam(ruta);
    if (!tam || hw_fs_leer_desde(ruta, 0, (u8 *)&c, W5X_TAM_CAB) != W5X_TAM_CAB || cg_cab_vale(&c, tam, m, sizeof m)) return 0;
    if (c.icono_an >= 1 && c.icono_an <= 16) { memcpy(bits, c.icono, 12 * sizeof bits[0]); *arte = (Icono){ c.icono_an, 12, bits }; }
    if (!con_malla || huecos >= CG_MENU) return 0;
    r = cg_ico_leer(ruta, &c, ico_buf[huecos], &ico_m[huecos]);
    if (r < 0) hw_log(ui_fmt("[cargador] %s trae una malla de icono que no vale: la genérica\n", ruta));
    if (r != 1) return 0;
    r = m3_malla_sen(huecos, &ico_m[huecos]) + 1;
    huecos++;
    return r;
}
App cg_celda_apps(void) { return (App){ "Apps", IC_APPS, abrir_lista, 0, .id = "@apps" }; }   /* (EST3) su icono: antes el de Archivos (dos iguales) */
/* (LANZ) Para la pantalla de Ajustes -> «Menú principal»: las .sen que no son de fábrica, en el orden de §28 (las mismas que mira
 * el menú). Lo que da cg_app_info apunta a `apps`: vale hasta que alguien vuelva a descubrir (el menú o la lista «Apps»). */
int cg_descubrir(void) { if (hw_modo_seguro()) return n_apps = 0; descubrir_menu(); return n_apps; }
int cg_app_info(int i, CgInfo *o)
{
    if (i < 0 || i >= n_apps) return -1;
    *o = (CgInfo){ apps[i].ruta, apps[i].nombre, apps[i].ic.w ? &apps[i].ic : &sh_iconos[apps[i].icono_gen], apps[i].perfil == W5_PERFIL_JUEGO };
    return 0;
}

/* ---- nombres de fichero de la app, tope de escrituras, recursos -------------------------------------------------------- */
const char *cg_nombre_fich(const char *nombre)
{
    static char n[HW_USR_MAX_NOMBRE + 1];
    size_t a = strlen(cg.prefijo), b, i;
    if (cg.estado != CG_VIVA || !nombre || !cg_legible(nombre)) return NULL;
    b = strlen(nombre);
    if (!b || a + b > HW_USR_MAX_NOMBRE) return NULL;
    for (i = 0; i < b; i++) if (nombre[i] == '/' || (u8)nombre[i] < 0x20) return NULL;
    memcpy(n, cg.prefijo, a); memcpy(n + a, nombre, b + 1);
    return n;
}
/* (28-oct) Los ficheros PROPIOS (ap_fich_*) de una app que vino de la TARJETA van a la tarjeta, junto a su .sen (con el mismo prefijo
 * que en Mis archivos: «doom.default.cfg»), no a Mis archivos (la NOR). el mantenedor: los juegos, «desde la memoria externa, nunca desde
 * la NOR»: también sus partidas guardadas. NULL = la app no es de la tarjeta (sus ficheros siguen en Mis archivos) o el nombre no vale */
const char *cg_fich_tarjeta(const char *nombre)
{
    static char r[96 + HW_USR_MAX_NOMBRE + 2];
    const char *n, *barra = NULL, *p;
    if (cg.estado != CG_VIVA || !empieza(cg.ruta, "/tarjeta/") || !(n = cg_nombre_fich(nombre))) return NULL;
    for (p = cg.ruta; *p; p++) if (*p == '/') barra = p;
    if (!barra || (size_t)(barra - cg.ruta) + 1 + strlen(n) + 1 > sizeof r) return NULL;
    memcpy(r, cg.ruta, (size_t)(barra - cg.ruta) + 1); poner(r + (barra - cg.ruta) + 1, sizeof r - (size_t)(barra - cg.ruta) - 1, n);
    return r;
}
int cg_escritura_permitida(void)
{
    u32 t = hw_ms();
    if (t - cg.esc_t0 >= 60000u) { cg.esc_t0 = t; cg.esc_n = 0; }
    return cg.esc_n++ < CG_ESCRITURAS_MIN;
}
static u32 rec_ini(void) { return W5X_TAM_CAB + cg.cab.tam_cod + cg.cab.tam_dat + cg.cab.n_reub * 4u; }
u32 cg_rec_tam(void) { return cg.estado == CG_LIBRE ? 0 : cg.cab.tam_rec; }
int cg_rec_leer(u32 desde, void *buf, u32 max)
{
    if (cg.estado != CG_VIVA || desde >= cg.cab.tam_rec) return 0;
    if (max > cg.cab.tam_rec - desde) max = cg.cab.tam_rec - desde;
    if (!cg_en_app(buf, max)) return W5_ERROR;
    return (int)hw_fs_leer_desde(cg.ruta, rec_ini() + desde, buf, max);
}
/* EN EL SITIO: solo si el fichero vive contiguo en una flash mapeada (hoy: nuestro FS, que lo da con hw_fs_mapa).
 * Desde Mis archivos (W6U, troceado) o desde un medio no mapeado (una tarjeta) devuelve NULL y la app lee a trozos. */
const void *cg_rec_mapa(u32 desde, u32 n)
{
    u32 tam = 0;
    const u8 *p;
    if (cg.estado != CG_VIVA || desde > cg.cab.tam_rec || n > cg.cab.tam_rec - desde) return NULL;
    if (cg.cab.caps & W5_CAP_REC_VENTANA) return ven_n ? (const void *)(size_t)(W5_REC_VA + desde) : NULL;   /* (1.1) M5X: el mapa ES la ventana */
    if (cg_pm_activa()) return cg_pm_rec_mapa(desde, n);     /* (PM) abre esas páginas de la NOR en SOLO LECTURA y da el puntero real (1:1). La ventana W5_REC_VA de M5X es otro mecanismo (apps con W5_CAP_REC_VENTANA) */
    p = hw_fs_mapa(cg.ruta, &tam);
    return p && tam == cg.cab.tam_fich ? p + rec_ini() + desde : NULL;
}
int cg_rec_comprobar(void)
{
    u8 t[256];
    u32 crc = 0, d = 0, k;
    if (cg.estado != CG_VIVA) return W5_ERROR;
    for (; d < cg.cab.tam_rec; d += k) {
        k = cg.cab.tam_rec - d > sizeof t ? (u32)sizeof t : cg.cab.tam_rec - d;
        if (hw_fs_leer_desde(cg.ruta, rec_ini() + d, t, k) != k) return W5_ERROR;
        crc = cg_crc(crc, t, k);
        if (!(d & 0xFFFF)) cg_latido();
    }
    return crc == cg.cab.crc_rec ? W5_OK : W5_ERROR;
}
int cg_acel(int si)                                          /* ap_acel(): el sensor, con dueño (se suelta al descargar) */
{
    if (cg.estado != CG_VIVA) return W5_ERROR;
    if (!!si != cg.acel) { cg.acel = (u8)!!si; sh_pasos_retener(si); }
    return W5_OK;
}

/* (INT2, ronda 2) una cadena de la app COPIADA a un búfer del sistema, comprobando CADA byte (cg_legible del primero no basta:
 * una cadena pegada al final de la memoria de la app seguiría leyéndose en la del sistema). La usan las envolturas de la 1.2
 * (shell/cg12_*.c). 1 si cabe entera con su 0; 0 si no es legible o no cabe (sin cerrar la app: lo decide quien llama). */
int cg_copiar_cad(char *d, size_t cap, const char *s)
{
    size_t n;
    if (!d || !cap || !s) return 0;
    for (n = 0; n < cap; n++) {
        if (!cg_legible(s + n)) return 0;
        if (!(d[n] = s[n])) return 1;
    }
    d[cap - 1] = 0;
    return 0;
}
