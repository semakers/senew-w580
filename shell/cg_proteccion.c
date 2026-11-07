/* cg_proteccion.c — (pista PM) AISLAMIENTO DE MEMORIA de las apps cargables: la app corre en MODO USUARIO con permisos
 * de página, y el sistema le da servicio por una PUERTA (SWI). Documento: docs/proteccion_memoria.md.
 *
 * Las piezas (todo SOLO en el teléfono y solo si la MMU está encendida; en el PC y en los hitos no hace nada):
 *  1. PERMISOS. Mientras hay una app cargada, todas las secciones de 1 MiB pasan a AP=01 (solo sistema) y las 16 de la
 *     SDRAM a tablas de 2.º nivel (páginas de 4 KiB) sacadas DEL PROPIO PRÉSTAMO de la app (no crece el .bss). Solo se
 *     abren: las páginas de la app (lectura y escritura), la PUERTA (solo lectura), la arena del cuadro del sistema
 *     (solo lectura: ahí devuelven ui_fmt, ui_lista y ap_hora_texto) y, en un juego, su marco. Al descargar, todo
 *     vuelve a como lo dejó mmu_cache_init (se guarda y se repone). El sistema (modo privilegiado) lo sigue viendo todo.
 *  2. LA PUERTA. Una página con una COPIA de la tabla (misma cabecera) cuyas funciones son trampolines
 *     «mov r12, #n; svc 0; bx lr». Las seis de memoria y cadenas (memcpy…strcmp) corren en modo usuario, sin SWI: un
 *     puntero ajeno ahí es un aborto de la app. Las .sen de hoy valen sin recompilar.
 *  3. EL VECTOR SWI se redirige EN MARCHA (la palabra 10 de los vectores, en la ITCM, como ya se hacía con la 8 para el
 *     salto nulo): hal/arranque.S NO se toca y el arranque no cambia.
 *  4. ENTRAR Y VOLVER. cg_usr() baja a modo usuario con la pila de la app; la app vuelve a un «svc» de la puerta
 *     (r12 = -1) que deshace el cambio. Anidado (el sistema llama a la app desde dentro de una llamada suya): igual,
 *     con la pila de usuario que tenía la app al entrar en la puerta.
 * [inferido, sin aparato] cachés y TLB: se limpian las tablas antes de invalidar la TLB; el emulador no lo modela. */
#include "cargador.h"
#include "app_externa.h"
#include "placa.h"
#include <stddef.h>

#if TELEFONO
#define PM_L2      (16u * 1024u)          /* 16 tablas de 2.º nivel (una por MiB de SDRAM), 1 KiB cada una */
#define PM_PAG     4096u
#define PM_TRAMP   1536u                 /* (INT2) dónde empiezan los trampolines en la puerta */
#define PM_SDRAM   (placa()->ram_base)   /* (P600) la RAM de la placa: W580i 0x20000000, W600i 0x4C000000 (antes fija la del W580) */
#define PM_SEC0    (PM_SDRAM >> 20)
#define PM_NSEC    16u                    /* 16 secciones de 1 MiB: la SDRAM entera en los dos (en el W580 con la memoria gráfica) */
#define AP_SIS     0x550u                 /* small page, AP0..AP3 = 01: solo sistema */
#define AP_LEER    0xAA0u                 /* 10: el usuario solo lee */
#define AP_TODO    0xFF0u                 /* 11: el usuario lee y escribe */
#define PAG_CB     0x00Eu                 /* small page (10) con C y B */

void cache_limpiar(u32 inicio, u32 fin);  /* hal/reloj.S */
void irq_abrir(void);
u32  irq_cerrar(void);
void ui_arena_tramo(const void **ini, u32 *tam);   /* ui/ui_mem.c */

extern volatile int cg_en_gate;
extern void cg_swi(void);
extern const u32 pm_puerta_cod[], pm_puerta_cod_fin[];   /* el código que se copia a la puerta (abajo) */
u32  cg_svc_sp;                            /* cadena de cg_usr: el sp del sistema al bajar a usuario */
u32  cg_usr_vuelta;                        /* dirección (en la puerta) del «svc» de vuelta */
static u32 usp_actual;                     /* sp de usuario de la app en la llamada a la puerta en curso */
static u32 *l1, *l2;                       /* la tabla de 1.er nivel (la de mmu_cache_init) y las nuestras */
static u32 l1_sdram[PM_NSEC];              /* las 16 entradas originales de la SDRAM, para reponerlas tal cual */
static u8  *puerta;                        /* la página de la puerta */
static W5Api *api_app;                     /* la copia de la tabla, dentro de la puerta */
static u32 vec_swi_antes;
static u8  activa;
static u32 pila_ini, pila_fin;             /* la pila de la app: lo único que la puerta acepta como sp de usuario */
/* (PM) la VENTANA DE RECURSOS: ap_rec_mapa abre en SOLO LECTURA las páginas de la NOR donde vive el fichero de la app (contiguo
 * en nuestro FS). Las tablas de 2.º nivel salen de un hueco reservado delante del bloque (memoria SOLO del sistema: si fueran de
 * la app, podría reescribir los permisos). La tabla es 1:1, así que la app ve el puntero REAL a la NOR (no hay que reubicar).
 * (Es un mecanismo DISTINTO de la ventana W5_REC_VA de la pista M5X, que es para apps con W5_CAP_REC_VENTANA; conviven: una app
 * usa uno u otro según tenga o no la capacidad. Recomendación a futuro: docs/proteccion_memoria.md.) */
static u32 *rec_l2; static u32 rec_l2_bytes, rec_sec0, rec_nsec, rec_antes[64];
static const u8 *rec_fis; static u32 rec_tam;
CgPmMedidas cg_pm_med;

static u32 sctlr(void) { u32 v; __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v)); return v; }
static u32 ttbr0(void) { u32 v; __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(v)); return v; }
static void tlb_todo(void)
{
    cache_limpiar((u32)(size_t)l1, (u32)(size_t)l1 + 16384u);
    if (l2) cache_limpiar((u32)(size_t)l2, (u32)(size_t)l2 + PM_L2);
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c10, 4\n mcr p15, 0, r0, c8, c7, 0\n nop\n nop" : : : "r0", "memory");
}

int cg_pm_posible(void)
{
#ifdef CG_SIN_PROTECCION
    return 0;                              /* la NEGATIVA de las pruebas: el mismo sistema sin aislamiento */
#else
    return (sctlr() & 1u) != 0;            /* sin MMU (los mains de los hitos) no hay nada que proteger con */
#endif
}
/* lo que PM reserva delante del bloque: [tablas SDRAM 16 KiB][tablas de la ventana de recursos][puerta 4 KiB][guarda 4 KiB].
 * La ventana: una tabla de 2.º nivel (1 KiB) por MiB de recursos, más 2 por si el fichero no empieza/acaba en frontera de sección. */
static u32 rec_l2_para(u32 tam_rec) { u32 ns = tam_rec ? (tam_rec >> 20) + 2u : 0u; if (ns > 64u) ns = 64u; return (ns * 1024u + PM_PAG - 1u) & ~(PM_PAG - 1u); }
u32 cg_pm_extra(u32 tam_rec) { return PM_L2 + rec_l2_para(tam_rec) + 2u * PM_PAG; }

/* Cambia los permisos de las páginas que tocan [p, p+n) (solo SDRAM; lo demás se queda como está). */
static void abrir(const void *p, u32 n, u32 ap)
{
    u32 a = (u32)(size_t)p & ~(PM_PAG - 1u), fin = (u32)(size_t)p + n;
    if (!n || !l2) return;
    for (; a < fin; a += PM_PAG) {
        if (a < PM_SDRAM || a >= PM_SDRAM + (PM_NSEC << 20)) continue;
        l2[(a - PM_SDRAM) >> 12] = (a & ~(PM_PAG - 1u)) | ap | PAG_CB;
    }
}
int cg_pm_abrir(const void *p, u32 n, int escribir)
{
    if (!activa) return 0;
    if (((size_t)p & (PM_PAG - 1u)) || (n & (PM_PAG - 1u)) || !n) return 0;   /* (AUD9 C3) solo rangos alineados a página: si no, podría abrir una página con datos de control del sistema */
    abrir(p, n, escribir ? AP_TODO : AP_LEER); tlb_todo(); return 1;
}
void cg_pm_cerrar(const void *p, u32 n) { if (activa) { abrir(p, n, AP_SIS); tlb_todo(); } }   /* (AUD9) retira los permisos (al soltar el panel) */

/* Monta la protección. `base` = principio del préstamo (alineado a 4 KiB), con cg_pm_extra() bytes para nosotros;
 * [app_ini, app_fin) = la memoria de la app (imagen, montón, arena, pila); [pi, pf) = su pila. Devuelve la tabla que ve la app. */
const W5Api *cg_pm_montar(u8 *base, const u8 *app_ini, const u8 *app_fin, const u8 *pi, const u8 *pf, u32 tam_rec, volatile u32 *vec)
{
    u32 i, j, n;
    const void *ar; u32 art;
    u32 *cod;
    if (activa || ((size_t)base & (PM_PAG - 1u))) return NULL;
    l1 = (u32 *)(size_t)(ttbr0() & ~0x3FFFu);
    l2 = (u32 *)base;
    rec_l2_bytes = rec_l2_para(tam_rec);
    rec_l2 = rec_l2_bytes ? (u32 *)(base + PM_L2) : NULL;
    rec_nsec = 0; rec_fis = NULL; rec_tam = 0;
    puerta = base + PM_L2 + rec_l2_bytes;
    /* 1. tablas de 2.º nivel: todo «solo sistema», con caché como estaba */
    for (i = 0; i < PM_NSEC * 256u; i++) l2[i] = (PM_SDRAM + (i << 12)) | AP_SIS | PAG_CB;
    /* 2. la puerta: cabecera de la tabla, punteros a trampolines, el código */
    memset(puerta, 0, PM_PAG);
    n = cg_api.n;
    api_app = (W5Api *)puerta;
    memcpy(api_app, &cg_api, sizeof cg_api);
    cod = (u32 *)(puerta + 1024);
    for (i = 0; pm_puerta_cod + i < pm_puerta_cod_fin; i++) cod[i] = pm_puerta_cod[i];
    /* las seis primeras: en modo usuario (memcpy, memset, memmove, memcmp, strlen, strcmp: palabras 0..5 del código copiado) */
    {
        void **f = (void **)((u8 *)api_app + offsetof(W5Api, memcpy));
        for (j = 0; j < 6; j++) f[j] = (u8 *)cod + cod[j];          /* el código empieza con 6 desplazamientos */
        cg_usr_vuelta = (u32)(size_t)((u8 *)cod + cod[6]);
        /* el resto: un trampolín de 3 palabras por función, detrás del código copiado.
         * (INT2, ronda 2) ANTES empezaban en +3072 y cabían 85 (j < 85): con la API 1.2 (139 funciones) las 85..138 se quedaban
         * con el puntero CRUDO del sistema copiado de cg_api, y la app, en modo usuario, saltaba a la flash del sistema -> aborto:
         * con PM Música, Luces, Notas, Imágenes… se cerraban al llamarlas [observado: proteccion/evidencia/pm_int2/caso_14].
         * Ahora desde +PM_TRAMP (el código copiado ocupa ~250 B desde +1024): caben (4096 - 1536) / 12 = 213. */
        if (n > 6u + (PM_PAG - PM_TRAMP) / 12u || (u32)((u8 *)pm_puerta_cod_fin - (u8 *)pm_puerta_cod) > PM_TRAMP - 1024u) {
            api_app = NULL; puerta = NULL; return NULL;                 /* no caben: no se monta (la app no se carga) */
        }
        {
            u32 *t = (u32 *)(puerta + PM_TRAMP);
            for (j = 6; j < n; j++, t += 3) {
                t[0] = 0xE3A0C000u | j;     /* mov r12, #j */
                t[1] = 0xEF000000u;         /* svc 0 */
                t[2] = 0xE12FFF1Eu;         /* bx lr */
                f[j] = t;
            }
        }
    }
    cache_limpiar((u32)(size_t)puerta, (u32)(size_t)puerta + PM_PAG);
    __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c5, 0" : : : "r0", "memory");   /* caché de instrucciones */
    /* 3. permisos: app, puerta, arena del cuadro */
    abrir(app_ini, (u32)(app_fin - app_ini), AP_TODO);
    abrir(puerta, PM_PAG, AP_LEER);
    ui_arena_tramo(&ar, &art);
    abrir(ar, art, AP_LEER);
    pila_ini = (u32)(size_t)pi; pila_fin = (u32)(size_t)pf;
    /* 4. primer nivel: todo AP=01 (solo sistema); las 16 de la SDRAM, a nuestras tablas */
    {
        u32 cerradas = irq_cerrar();
        for (i = 0; i < PM_NSEC; i++) l1_sdram[i] = l1[PM_SEC0 + i];
        for (i = 0; i < 4096u; i++) if ((l1[i] & 3u) == 2u) l1[i] = (l1[i] & ~0xC00u) | 0x400u;
        for (i = 0; i < PM_NSEC; i++) l1[PM_SEC0 + i] = ((u32)(size_t)l2 + i * 1024u) | 0x11u;   /* tabla gruesa, dominio 0 */
        tlb_todo();
        vec_swi_antes = vec[10];
        vec[10] = (u32)(size_t)cg_swi;
        cache_limpiar(0, 64);
        activa = 1;
        if (!cerradas) irq_abrir();
    }
    cg_svc_sp = 0; usp_actual = 0;
    cg_pm_med.montajes++;
    return api_app;
}
/* (PM) ap_rec_mapa con protección: abre [fis, fis+n) de la NOR en SOLO LECTURA y devuelve el puntero REAL (la tabla es 1:1).
 * La primera llamada arma la ventana con las secciones de la NOR que toca el fichero entero (rec_tam, de cg_pm_rec_fichero);
 * las páginas DENTRO del fichero quedan AP=10 (usuario lee) con caché como la NOR; el resto de las secciones de los bordes
 * sigue AP=01 (el sistema lo ve —ahí vive el resto del FS—, la app no). El relleno de hasta 1 página del borde final es de OTRO
 * fichero del FS, de solo lectura (la app ya puede leer cualquier fichero con fs_leer): aceptable; alinear los ficheros del
 * W6FS a 4 KiB lo cerraría. Devuelve NULL si no hay fichero mapeado o si no cabe la ventana. */
void cg_pm_rec_fichero(const u8 *fis, u32 tam) { rec_fis = fis; rec_tam = tam; }
const void *cg_pm_rec_mapa(u32 desde, u32 n)
{
    u32 s0, s1, i, cerradas;
    if (!activa || !rec_l2 || !rec_fis || !rec_tam) return NULL;
    if (desde > rec_tam || n > rec_tam - desde) return NULL;
    if (!rec_nsec) {                                        /* primera vez: armar la ventana sobre el fichero ENTERO */
        s0 = (u32)(size_t)rec_fis >> 20;
        s1 = ((u32)(size_t)rec_fis + rec_tam - 1u) >> 20;
        if (s1 - s0 + 1u > rec_l2_bytes / 1024u) return NULL;
        rec_sec0 = s0; rec_nsec = s1 - s0 + 1u;
        for (i = 0; i < rec_nsec * 256u; i++) {
            u32 pa = (rec_sec0 << 20) + (i << 12);
            rec_l2[i] = (pa >= ((u32)(size_t)rec_fis & ~(PM_PAG - 1u)) && pa < (u32)(size_t)rec_fis + rec_tam)
                        ? (pa | AP_LEER | 0x8u | 0x2u)         /* dentro del fichero: AP=10 (usuario lee), small page, con C */
                        : (pa | AP_SIS  | 0x8u | 0x2u);        /* el resto de la sección: AP=01, el sistema lo sigue viendo; la app no */
        }
        cerradas = irq_cerrar();
        for (i = 0; i < rec_nsec; i++) { rec_antes[i] = l1[rec_sec0 + i]; l1[rec_sec0 + i] = ((u32)(size_t)rec_l2 + i * 1024u) | 0x11u; }
        cache_limpiar((u32)(size_t)rec_l2, (u32)(size_t)rec_l2 + rec_nsec * 1024u);
        cache_limpiar((u32)(size_t)&l1[rec_sec0], (u32)(size_t)&l1[rec_sec0 + rec_nsec]);
        __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c10, 4\n mcr p15, 0, r0, c8, c7, 0\n nop\n nop" : : : "r0", "memory");
        if (!cerradas) irq_abrir();
        cg_pm_med.ventanas++;
    }
    return rec_fis + desde;
}
static void rec_quitar(void)
{
    u32 i;
    if (!rec_nsec) return;
    for (i = 0; i < rec_nsec; i++) l1[rec_sec0 + i] = rec_antes[i];
    cache_limpiar((u32)(size_t)&l1[rec_sec0], (u32)(size_t)&l1[rec_sec0 + rec_nsec]);
    rec_nsec = 0;
}
void cg_pm_desmontar(volatile u32 *vec)
{
    u32 i, cerradas;
    if (!activa) return;
    cerradas = irq_cerrar();
    rec_quitar();                                          /* (PM) la ventana de recursos, antes que las secciones de la SDRAM */
    for (i = 0; i < 4096u; i++) if ((l1[i] & 3u) == 2u) l1[i] |= 0xC00u;    /* todas eran AP=11 (mmu_cache_init) */
    for (i = 0; i < PM_NSEC; i++) l1[PM_SEC0 + i] = l1_sdram[i];
    l2 = NULL; rec_l2 = NULL; rec_fis = NULL; rec_tam = 0;
    tlb_todo();
    vec[10] = vec_swi_antes;
    cache_limpiar(0, 64);
    activa = 0;
    api_app = NULL; puerta = NULL; cg_svc_sp = 0; usp_actual = 0;
    if (!cerradas) irq_abrir();
}
int cg_pm_activa(void) { return activa; }
const W5Api *cg_pm_api(void) { return activa ? api_app : &cg_api; }
u32 cg_pm_usp(void) { return usp_actual; }
void cg_pm_reiniciar(void) { cg_svc_sp = 0; usp_actual = 0; }   /* tras volver por un fallo: la cadena se abandona */

/* LA PUERTA, del lado del sistema. m: [0]=spsr [1]=(r4) [2..5]=r0..r3 [6]=r12 [7]=lr [8]=sp de usuario [9]=lr de usuario.
 * Corre en modo SVC sobre la pila del sistema. Las IRQ, como las tenía la app. */
typedef W5Pal (*Fn12)(W5Pal, W5Pal, W5Pal, W5Pal, W5Pal, W5Pal, W5Pal, W5Pal, W5Pal, W5Pal, W5Pal, W5Pal);
W5Pal cg_puerta_c(u32 *m)
{
    u32 idx = m[6], usp = m[8], ant = usp_actual, s[8], i;
    W5Pal r;
    const void *const *tab = (const void *const *)((const u8 *)&cg_api_todo + offsetof(W5Api, memcpy));   /* (BT) la tabla ENTERA: n incluye el grupo 1.4 */
    cg_pm_med.llamadas++;
    if (!activa || idx < 6 || idx >= cg_api.n) { cg_pm_fallo("La app llamó a la puerta del sistema con un número que no existe."); }
    if (usp < pila_ini || usp > pila_fin || (usp & 3u)) { cg_pm_fallo("La app llamó al sistema con la pila fuera de su sitio (¿desbordada?)."); }
    for (i = 0; i < 8; i++) s[i] = usp + i * 4u + 4u <= pila_fin ? ((const u32 *)(size_t)usp)[i] : 0;   /* argumentos 5.º.. (variables: ui_fmt) */
    usp_actual = usp;
    cg_en_gate++;                                          /* (AUD9) el perro no saca a la app mientras corre esto */
    if (!(m[0] & 0x80u)) irq_abrir();
    r = ((Fn12)tab[idx])(m[2], m[3], m[4], m[5], s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    irq_cerrar();
    cg_en_gate--;
    usp_actual = ant;
    return r;
}

/* El ensamblador: la entrada del SWI, bajar a usuario y volver, y el código que se COPIA a la puerta (independiente de la
 * posición: los 7 primeros son desplazamientos desde su principio). */
__asm__(".text\n .arm\n .align 2\n"
    " .global cg_swi\n .type cg_swi, %function\n"
    "cg_swi:\n"
    " cmn r12, #1\n beq 1f\n"
    " sub sp, sp, #8\n stmia sp, {sp, lr}^\n nop\n"
    " stmfd sp!, {r0-r3, r12, lr}\n mrs r0, spsr\n stmfd sp!, {r0, r4}\n"
    " mov r0, sp\n ldr r1, =cg_puerta_c\n mov lr, pc\n bx r1\n"
    " ldmfd sp!, {r1, r4}\n msr spsr_cxsf, r1\n add sp, sp, #4\n ldmfd sp!, {r1-r3, r12, lr}\n add sp, sp, #8\n movs pc, lr\n"
    "1:\n"   /* la app VUELVE de una función a la que la llamó cg_usr: r0 = lo que devuelve */
    " ldr r1, =cg_svc_sp\n ldr sp, [r1]\n ldmfd sp!, {r2, r3}\n str r2, [r1]\n msr cpsr_c, r3\n ldmfd sp!, {r4-r12, lr}\n bx lr\n"
    " .ltorg\n"
    /* W5Pal cg_usr(W5Pal *args, u32 usp): llama a args[0](args[1..4]) en MODO USUARIO con la pila usp */
    " .global cg_usr\n .type cg_usr, %function\n"
    "cg_usr:\n"
    " stmfd sp!, {r4-r12, lr}\n mrs r3, cpsr\n ldr r4, =cg_svc_sp\n ldr r2, [r4]\n stmfd sp!, {r2, r3}\n str sp, [r4]\n"
    " ldr r5, =cg_usr_vuelta\n ldr r5, [r5]\n stmfd sp!, {r1, r5}\n ldmfd sp, {sp, lr}^\n nop\n add sp, sp, #8\n"
    " ldr r12, [r0]\n ldr r1, [r0, #8]\n ldr r2, [r0, #12]\n ldr r3, [r0, #16]\n ldr r0, [r0, #4]\n"
    " mrs lr, cpsr\n bic lr, lr, #0x3F\n orr lr, lr, #0x10\n tst r12, #1\n orrne lr, lr, #0x20\n msr spsr_cxsf, lr\n"
    " bic lr, r12, #1\n movs pc, lr\n"
    " .ltorg\n"
    /* ---- lo que va a la puerta ---- */
    " .global pm_puerta_cod, pm_puerta_cod_fin\n"
    "pm_puerta_cod:\n"
    " .word p_memcpy - pm_puerta_cod, p_memset - pm_puerta_cod, p_memmove - pm_puerta_cod, p_memcmp - pm_puerta_cod\n"
    " .word p_strlen - pm_puerta_cod, p_strcmp - pm_puerta_cod, p_vuelta - pm_puerta_cod\n"
    "p_vuelta:\n mvn r12, #0\n svc 0\n"
    "p_memcpy:\n mov r12, r0\n orr r3, r0, r1\n tst r3, #3\n bne 3f\n"
    "2:\n cmp r2, #4\n blo 3f\n ldr r3, [r1], #4\n str r3, [r12], #4\n sub r2, r2, #4\n b 2b\n"
    "3:\n subs r2, r2, #1\n ldrhsb r3, [r1], #1\n strhsb r3, [r12], #1\n bhs 3b\n bx lr\n"
    "p_memset:\n mov r12, r0\n and r1, r1, #255\n"
    "4:\n subs r2, r2, #1\n strhsb r1, [r12], #1\n bhs 4b\n bx lr\n"
    "p_memmove:\n cmp r0, r1\n bls p_memcpy\n add r1, r1, r2\n add r12, r0, r2\n"
    "5:\n subs r2, r2, #1\n ldrhsb r3, [r1, #-1]!\n strhsb r3, [r12, #-1]!\n bhs 5b\n bx lr\n"
    "p_memcmp:\n"
    "6:\n subs r2, r2, #1\n movlo r0, #0\n bxlo lr\n ldrb r3, [r0], #1\n ldrb r12, [r1], #1\n subs r3, r3, r12\n beq 6b\n mov r0, r3\n bx lr\n"
    "p_strlen:\n mov r1, r0\n"
    "7:\n ldrb r2, [r1], #1\n cmp r2, #0\n bne 7b\n sub r0, r1, r0\n sub r0, r0, #1\n bx lr\n"
    "p_strcmp:\n"
    "8:\n ldrb r2, [r0], #1\n ldrb r3, [r1], #1\n cmp r2, #1\n cmphs r2, r3\n beq 8b\n sub r0, r2, r3\n bx lr\n"
    "pm_puerta_cod_fin:\n");
#else
int cg_pm_posible(void) { return 0; }
int cg_pm_activa(void) { return 0; }
const W5Api *cg_pm_api(void) { return &cg_api; }
int cg_pm_abrir(const void *p, u32 n, int escribir) { (void)p; (void)n; (void)escribir; return 0; }
void cg_pm_cerrar(const void *p, u32 n) { (void)p; (void)n; }
void cg_pm_rec_fichero(const u8 *fis, u32 tam) { (void)fis; (void)tam; }
const void *cg_pm_rec_mapa(u32 desde, u32 n) { (void)desde; (void)n; return 0; }
CgPmMedidas cg_pm_med;
#endif
