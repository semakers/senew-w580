/* NOR del W580i (0x44000000, CS0; Intel M18 «Sibley», 64 MiB, bloques de borrado de 256 KiB, programa con
 * búfer de 1 KiB). docs/hal.md §16. Es hal/nor.c del W600 (Intel L18, bloques de 128 KiB) adaptado al M18.
 *
 * LECTURA: mapeada en memoria, sin órdenes.
 *
 * ESCRITURA: borrar un bloque y programar, SOLO dentro de las zonas nuestras (NOR_ESCR_INI..NOR_ESCR_FIN, hal.h): desde I1,
 * la de W6F (0x44B00000.., MODO OBJETO, sin espejo: regla 9), el almacén y W6U. Reglas, todas dentro de este fichero:
 *
 *  1. DESDE RAM y con las IRQ cortadas. Nuestro código se ejecuta en la propia NOR; mientras el chip está en
 *     modo de órdenes devuelve estado, no código. Todo lo que manda una orden vive en «.ramfn» (el arranque
 *     la copia a RAM con .data), en ARM, sin llamar a nada de la flash (herr/comprobar_ramfn.py lo comprueba
 *     en cada compilación). Es lo que hace el original (rutinas en RAM 0x200016FC.., hal.md §16).
 *  2. LA SECUENCIA DEL ORIGINAL (hal.md §16, traza e14 de la pista E), no «lo que el emulador deja pasar»:
 *     borrar    = 70 · 50 · 60 D0 (desbloquear) · 20 D0 · sondear SR hasta bit 7 · 60 01 (bloquear) · 50 si error · FF
 *     programar = 70 · 50 · 60 D0 · E9 · 0x1FF · 512 palabras · D0 · 70 · sondear SR · 60 01 · 50 si error · FF
 *     SIEMPRE una región entera de 1 KiB en dirección múltiplo de 0x400, como hace el original.
 *  3. MODO CONTROL (hal/m18.h): de cada 32 bytes físicos solo se usan los 16 primeros. Por eso quien usa
 *     esta capa (api/almacen.c, api/usuario.c) ve bloques de 128 KiB, y las lecturas salen de un ESPEJO en
 *     RAM (1,5 MiB) que se carga al arrancar y se pone al día tras cada operación VERIFICADA contra el chip.
 *  4. RANGO: cada rutina de RAM comprueba ELLA MISMA que la dirección cae en la zona; fuera no emite ninguna
 *     orden destructiva. Nunca el FS de fábrica (0x45E00000…), nunca el GDFS (0x47E00000…), nunca el main.
 *  5. Solo se programan bits de 1 a 0: si lo pedido exigiría subir un bit, error (se mira en el espejo).
 *  6. -DNOR_ESCRITURA=0 quita TODA la escritura en compilación (las rutinas ni existen).
 *  7. (R3, P1) UNA SOLA COMPROBACIÓN DEL CHIP, AQUÍ, y dos preguntas distintas:
 *     GEOMETRÍA = lo que la tabla CFI dice (bloques, tamaño de bloque, búfer). Si con ella las zonas existen y el modo
 *       control se puede leer (bloques de 256 KiB que llegan hasta el final de las zonas), HAY ZONAS: se LEEN.
 *     PERMISO = el chip es EXACTAMENTE el de la descripción de la placa (hal/nor_placa.h: id, juego de órdenes, tamaño,
 *       búfer, n.º y tamaño de bloques). Solo entonces se borra y se programa. Un chip inesperado se lee y NO se escribe.
 *     plat/nor_guarda.c (la segunda lectura que S7 tuvo que hacer por fuera) ya no existe: nor_chip() da lo leído.
 *  8. (R3, P2 y AUD3-M2) EL CHIP SE DEJA SIEMPRE EN UN ESTADO CONOCIDO:
 *     - (R3, tras la auditoría AUD6 M3/M4/B1) al iniciar, ANTES de identificar, solo se LEE el estado (70) de cada bloque de
 *       las zonas, con TOPE: si está ocupado se espera; si no acaba, no se le manda nada más y no hay zonas (sin escritura,
 *       dicho). Si hay un BORRADO SUSPENDIDO (sobrevivió a un reinicio sin corte de corriente) NO se toca todavía. Después,
 *       en nor_escr_init (que corre tras la carga a 0 y tras leer el registro de reinicio), y SOLO con permiso de escritura
 *       y si el registro de reinicio (hal/reinicio.c) dice que el borrado a medias era de UN BLOQUE NUESTRO, se reanuda ESE
 *       bloque (la rutina de RAM mira ella misma el rango), con tope, y se deja bloqueado y en lectura (nor_ram_sanear). Un
 *       suspendido que no se puede atribuir a nuestras zonas NO se reanuda: sin escritura, y se dice;
 *     - un borrado NUNCA se abandona con el chip suspendido: si las suspensiones se agotan, se termina con las IRQ cortadas;
 *     - cada reanudación garantiza AVANCE: durante NOR_SONDEOS_SIN_MIRAR sondeos no se atiende a las IRQ;
 *     - el espejo se pone al día en TODOS los caminos de salida que hayan mandado alguna orden al chip;
 *     - si el chip no acaba en el tope de sondeos, NO se vuelve a la flash con él ocupado (se ejecutaría el registro de
 *       estado): se sigue esperando en RAM y, cuando acaba, se dice NOR_R_TIEMPO.
 *  9. (I1; W6F) LA ZONA DE W6F (NOR_W6F_INI..NOR_W6F_FIN, 64 bloques) va en MODO OBJETO: cada región de 1 KiB se programa
 *     ENTERA, UNA vez entre dos borrados, con la orden de búfer (nor_ram_programar_objeto), y solo si está en blanco; no tiene
 *     espejo (se lee derecho del chip, que está mapeado sin caché). Su borrado es el mismo que el de las otras zonas (con
 *     suspensión, con su apunte en el registro de reinicio: códigos 13.., ver rst_dir). El modo objeto se niega fuera de
 *     esa zona. El almacén y W6U siguen en modo control y con espejo, en NOR_ESPEJO_INI..NOR_ESPEJO_FIN.
 *
 * [emulador] el modelo de E acepta estas secuencias; NO modela tiempos, ni la suspensión, ni las restricciones
 * de región del M18 (aceptaría también lo que un chip real rechaza): que funcione ahí no prueba el aparato.
 * [sin saber] si el cargador del aparato deja bloques con bloqueo permanente (60 2F); los tiempos reales.
 * La caché de datos: las secciones de las zonas se mapean SIN caché (hal/reloj.c). */
#include "hal.h"
#include "m18.h"
#include "nor_placa.h"
/* (2026-10-25, música sólida) la VEDA de la flash mientras suena música (plat/hw_w580.c hwo_flash_veda): nor_veda la refleja y nor_veda_ops cuenta
 * cada borrado o programa que, aun así, cae dentro (lo que el usuario o una app guardan a propósito). Debería quedarse en 0 con la música sola. */
volatile u8 nor_veda;
volatile u32 nor_veda_ops;

#define NOR_BASE 0x44000000u
/* (R3) Los accesos al chip van por cuatro macros para que herr/pruebas/prueba_nor.c pueda compilar ESTE fichero en el PC
 * contra un modelo del chip (-DNOR_PRUEBA; make probar-nor). En el teléfono son el acceso directo de siempre. */
#ifdef NOR_PRUEBA
u16 nor_pr_leer(u32 dir); void nor_pr_escribir(u32 dir, u16 v); const u8 *nor_pr_mem(u32 dir); u32 nor_pr_irq(void);
#define RAMFN
#define NL(d)        nor_pr_leer(d)
#define NE(d, v)     nor_pr_escribir((d), (u16)(v))
#define NOR_MEM(d)   nor_pr_mem(d)                 /* la flash mapeada, para leer por puntero */
#define NOR_IRQ()    nor_pr_irq()
#define ORIGEN_EN_FLASH(o) 0
#else
#define RAMFN __attribute__((section(".ramfn"), noinline, target("arm")))
#define NL(d)        R16(d)
#define NE(d, v)     (R16(d) = (u16)(v))
#define NOR_MEM(d)   ((const u8 *)(d))
#if NOR_ESCRITURA
#define NOR_IRQ()    (R32(0xFB020100) & ~R32(0xFB020104))   /* ¿hay una interrupción esperando? (banco raíz: pendientes y no tapadas; hal.md §7) */
#define ORIGEN_EN_FLASH(o) (((u32)(o) >= 0x04000000u && (u32)(o) < 0x08000000u) || ((u32)(o) >= NOR_BASE && (u32)(o) < NOR_BASE + 0x04000000u))
#endif
#endif

/* (I1) El código del registro de reinicio (hal/reinicio.c) para un bloque nuestro: 1..12 = almacén y W6U (lo de siempre,
 * contado desde NOR_ESPEJO_INI: así un registro dejado por S9 significa lo mismo), 13.. = los bloques de W6F. 0 = ninguno. */
#define N_ESPEJO ((NOR_ESPEJO_FIN - NOR_ESPEJO_INI) / 0x40000u)
#define N_W6F    ((NOR_W6F_FIN - NOR_W6F_INI) / 0x40000u)
__attribute__((unused)) static u32 rst_dir(u32 codigo)   /* (sin uso en el binario sin escritura) */                           /* la dirección del bloque, o 0 si el código no es de un bloque nuestro */
{
    if (codigo >= 1 && codigo <= N_ESPEJO) return NOR_ESPEJO_INI + (codigo - 1) * 0x40000u;
    if (codigo > N_ESPEJO && codigo <= N_ESPEJO + N_W6F) return NOR_W6F_INI + (codigo - 1 - N_ESPEJO) * 0x40000u;
    return 0;
}

/* Identificación (orden 90) o tabla CFI (orden 98): copia las palabras 0..n-1. No es destructiva. */
RAMFN static void nor_ram_leer(u32 base, u32 orden, u16 *t, u32 n)
{
    u32 i;
    NE(base, orden);
    for (i = 0; i < n; i++) t[i] = NL(base + 2 * i);
    NE(base, 0xFF);
}
#define SR_LISTO     0x80u
#define SR_BORR_SUSP 0x40u
#define SR_ERROR     0x3Au             /* bit 5 borrado, 4 programa, 3 VPP, 1 bloque bloqueado */
#define TOPE_BORRAR   40000000u        /* vueltas de sondeo: muy por encima del peor borrado */
#define TOPE_PROGRAMA  4000000u
/* (R3; AUD6 M4) EL ESTADO AL ARRANCAR, solo LEYENDO: para cada bloque de [ini, fin) se pide el estado (70) y se espera, CON
 * TOPE, a que el chip esté listo; luego se vuelve a lectura (FF: con el chip suspendido es «leer el resto»; ocupado, no hace
 * nada). Devuelve: bit 0 = alguno seguía OCUPADO en el tope; bit 1 = hay un borrado SUSPENDIDO. 70 y FF no cambian la flash
 * en ningún chip de este juego de órdenes; no se manda nada más. Está también en el binario sin escritura. */
RAMFN static u32 nor_ram_estado(u32 ini, u32 fin, u32 paso)
{
    u32 d, sr = 0, tope, r = 0;
    for (d = ini; d < fin; d += paso) {
        NE(d, 0x70);
        tope = TOPE_BORRAR;
        do sr = NL(d); while (!(sr & SR_LISTO) && --tope);
        if (!(sr & SR_LISTO)) r |= 1;
        else if (sr & SR_BORR_SUSP) r |= 2;
        NE(d, 0xFF);
    }
    return r;
}
#if NOR_ESCRITURA
#define EN_ZONA(d, n) ((d) >= NOR_ESCR_INI && (d) < NOR_ESCR_FIN && (n) <= NOR_ESCR_FIN - (d))
/* (R3, P2; AUD6 M3/M4) SANEAR UN BLOQUE NUESTRO con un borrado SUSPENDIDO de antes del reinicio: comprueba ELLA MISMA que
 * blq es un bloque de nuestras zonas (regla 4); si el chip está listo y suspendido (SR.7 y SR.6, leído dos veces igual),
 * reanuda (D0) y espera, con tope; y en todo caso lo deja bloqueado (60 01), con el estado limpio (50) y en lectura (FF).
 * Devuelve 1 = terminó un borrado, 0 = no había, 0x100 = no acabó en el tope (el bloque no se usará), 0x200 = fuera de rango.
 * Solo la llama nor_escr_init, con PERMISO de escritura y si hal/reinicio.c dice que el borrado a medias era de ese bloque. */
RAMFN static u32 nor_ram_sanear(u32 blq)
{
    u32 sr, sr2, n = 0, tope;
    if (!EN_ZONA(blq, M18_BLOQUE) || (blq & (M18_BLOQUE - 1))) return 0x200;
    NE(blq, 0x70); sr = NL(blq); sr2 = NL(blq);
    if (sr == sr2 && (sr & SR_LISTO) && (sr & SR_BORR_SUSP)) {
        NE(blq, 0xD0); NE(blq, 0x70);
        tope = TOPE_BORRAR;
        do sr = NL(blq); while (!(sr & SR_LISTO) && --tope);
        n = (sr & SR_LISTO) ? 1 : 0x100;
        if (n == 0x100) return n;                       /* ocupado todavía: NO se le manda nada más */
    }
    NE(blq, 0x60); NE(blq, 0x01);
    NE(blq, 0x50); NE(blq, 0xFF);
    return n;
}

#endif

static u16 cfi[0x40], ident[2];
static NorChip chip;
/* LA lectura del chip (una): sanear, identificar (90) y tabla CFI (98); y las dos respuestas: geometría y permiso. */
static void leer_chip(void)
{
    void (*volatile f)(u32, u32, u16 *, u32) = nor_ram_leer;       /* por puntero: la RAM queda lejos para un BL */
    u32 (*volatile e)(u32, u32, u32) = nor_ram_estado;
    u32 cerradas, v = 0, est;
    if (chip.leido) return;
#ifdef SENEW_RAM    /* (27-oct) senew EN RAM no toca el chip: el original puede haberlo dejado a MITAD de una operación (el rastro de la iRAM dio «etapa 13» = aquí,
                     * en nor_id) y no escribe en la NOR (NOR_ESCRITURA=0): sin identificar ni leer el CFI, sin geometría -> «sin almacén», como ya salía */
    chip.leido = 1; chip.que = NOR_CHIP_ID; chip.geometria = 0; chip.ocupado = 0;
    (void)f; (void)e; (void)cerradas; (void)v; (void)est;
    return;
#endif
    cerradas = irq_cerrar();
    est = e(NOR_ESCR_INI, NOR_ESCR_FIN, M18_BLOQUE);               /* (AUD6 M4) antes de cualquier otra orden: solo leer el estado, con tope */
    if (!(est & 1)) {                                              /* ocupado en el tope: ni identificar (no se le manda nada más) */
        f(NOR_BASE, 0x90, ident, 2);
        f(NOR_BASE, 0x98, cfi, 0x40);
    }
    if (!cerradas) irq_abrir();
    chip.leido = 1;
    chip.id = ((u32)ident[0] << 16) | ident[1];
    chip.cfi_tam = (u8)cfi[0x27]; chip.cfi_bufer = (u8)cfi[0x2A];
    chip.cfi_ordenes = (u16)((cfi[0x13] & 0xFF) | ((cfi[0x14] & 0xFF) << 8));
    chip.cfi_bloques = ((cfi[0x2D] & 0xFFu) | ((cfi[0x2E] & 0xFFu) << 8)) + 1;
    chip.cfi_bloque = ((cfi[0x2F] & 0xFFu) | ((cfi[0x30] & 0xFFu) << 8)) << 8;
    chip.ocupado = (u8)(est & 1); chip.suspendido = (u8)((est >> 1) & 1);
    /* GEOMETRÍA (para LEER): hay tabla CFI, una sola región de borrado, y sus bloques son los de 256 KiB sobre los que está
     * hecha la fórmula del modo control (hal/m18.h) y llegan hasta el final de nuestras zonas. No mira quién es el chip. */
    chip.geometria = (u8)((cfi[0x10] & 0xFF) == 'Q' && (cfi[0x11] & 0xFF) == 'R' && (cfi[0x12] & 0xFF) == 'Y' && (cfi[0x2C] & 0xFF) == 1
                          && chip.cfi_bloque == M18_BLOQUE && NOR_BASE + chip.cfi_bloques * chip.cfi_bloque >= NOR_ESCR_FIN);
    /* PERMISO (para ESCRIBIR): exactamente el chip de la descripción de la placa. La primera palabra que no cuadra, en cfi_mal. */
    chip.que = NOR_CHIP_BIEN; chip.cfi_mal = 0;
    if (chip.id != NOR_PLACA_ID) { chip.que = NOR_CHIP_ID; return; }
    if ((cfi[0x10] & 0xFF) != 'Q' || (cfi[0x11] & 0xFF) != 'R' || (cfi[0x12] & 0xFF) != 'Y') v = 0x10;
    else if (chip.cfi_ordenes != NOR_PLACA_ORDENES) v = 0x13;
    else if (chip.cfi_tam != NOR_PLACA_TAM_LOG2) v = 0x27;
    else if (chip.cfi_bufer != NOR_PLACA_BUF_LOG2) v = 0x2A;
    else if ((cfi[0x2C] & 0xFF) != 1) v = 0x2C;
    else if (chip.cfi_bloques != NOR_PLACA_BLOQUES) v = 0x2D;
    else if (chip.cfi_bloque != NOR_PLACA_BLOQUE) v = 0x2F;
    if (v) { chip.que = NOR_CHIP_CFI; chip.cfi_mal = (u8)v; }
}
const NorChip *nor_chip(void) { leer_chip(); return &chip; }
u32 nor_id(void) { leer_chip(); return chip.id; }

/* ---- geometría y espejo -------------------------------------------------------------------------------- */
#define N_ZONA ((NOR_ZONA_FIN - NOR_ZONA_INI) / M18_BLOQUE)        /* 4 bloques: el almacén */
#define N_USR  ((NOR_USR_FIN - NOR_USR_INI) / M18_BLOQUE)          /* 8 bloques: los ficheros de usuario */
#if defined(SENEW_RAM) && !NOR_ESCRITURA && !defined(MANTENER_ESPEJO)   /* (MANTENER_ESPEJO: para aislar un fallo, 28-oct) */
/* (28-oct) senew EN RAM no identifica el chip (nor_leer_chip: sin geometría -> hay_geo = 0): el espejo no se llena nunca y nadie lo lee
 * (las zonas dicen 0 bloques). Eran 1,5 MiB del préstamo de apps: sin ellos caben los juegos en las pruebas en RAM. */
/* ¡OJO! 4096 y no 16 [medido 28-oct, docs/senew_en_ram.md]: con 16 todo lo que va detrás en el .bss se corre 0x17FFF0 (16 mod 32: cambia
 * su alineación respecto a la línea de caché) y las lecturas de la TARJETA salen con bytes cambiados (1-5 por cada 64 KiB, al azar, sin
 * error del controlador); con 4096 (desplazamiento múltiplo de 4 KiB) o con el espejo entero, 0 fallos. Hay una variable de detrás que
 * necesita una alineación que nadie le pide: SIN ENCONTRAR. Este tamaño no lo arregla, lo esquiva. */
#ifndef ESPEJO_RAM_TAM
#define ESPEJO_RAM_TAM 4096
#endif
static u8 espejo[ESPEJO_RAM_TAM];
#else
static u8 espejo[(N_ZONA + N_USR) * M18_LOGICO];                   /* lo que el chip tiene, visto en lógico */
#endif
static u8 hay_geo;                                                 /* 1 = la tabla CFI cuadra: hay zonas (aunque sea solo lectura) */
static u8 motivo;                                                  /* por qué no se puede escribir (NOR_E_*) */

static void cargar_espejo(void)
{
    u32 b, l;
#if defined(SENEW_RAM) && !NOR_ESCRITURA && !defined(MANTENER_ESPEJO)
    return;                                                        /* (28-oct) sin espejo en RAM (arriba) */
#endif
    for (b = 0; b < N_ZONA + N_USR; b++) {
        const u8 *f = NOR_MEM(NOR_ESPEJO_INI + b * M18_BLOQUE);   /* (I1) el tramo CON espejo: W6F no tiene */
        u8 *e = espejo + b * M18_LOGICO;
        for (l = 0; l < M18_LOGICO; l += 16) {
            const u32 *o = (const u32 *)(const void *)(f + 2 * l);   /* alineado: bloque + múltiplo de 32 */ u32 *d = (u32 *)(e + l);
            d[0] = o[0]; d[1] = o[1]; d[2] = o[2]; d[3] = o[3];
        }
    }
}

int nor_escr_init(void)
{
    static u8 hecho;
    if (hecho) return motivo ? -(int)motivo : 0;
    hecho = 1;
    leer_chip();
    motivo = NOR_E_CHIP;
    if (chip.ocupado) return -NOR_E_CHIP;                /* (AUD6 M4) el chip no acabó en el tope: no se le toca (ni zonas) */
    if (!chip.geometria) return -NOR_E_CHIP;             /* ni zonas: con esa tabla no se sabe ni leer el modo control */
    if (chip.suspendido) {                               /* (P2; AUD6 M3/B1) un borrado SUSPENDIDO de antes del reinicio */
#if NOR_ESCRITURA
        u32 b = rst_dir(rst_nor_pendiente()), r = 0x200; /* el bloque que hal/reinicio.c apuntó antes de borrar (0 = ninguno); (I1) también de W6F */
        if (chip.que == NOR_CHIP_BIEN && b) {
            u32 (*volatile sa)(u32) = nor_ram_sanear;
            u32 cerradas = irq_cerrar();
            r = sa(b);
            if (!cerradas) irq_abrir();
        }
        chip.suspendidos = (u8)(r & 0xFF); chip.saneo_lento = (u8)(r == 0x100);
        if (r == 0x100 || r == 0x200) {                  /* sin permiso, sin bloque nuestro apuntado, o no acabó: NO se toca y no se usa */
            chip.suspendido_ajeno = (u8)(r == 0x200);
            return -NOR_E_CHIP;
        }
#else
        chip.suspendido_ajeno = 1;
        return -NOR_E_CHIP;
#endif
    }
    hay_geo = 1;
    cargar_espejo();                                     /* se LEE sea quien sea el chip */
    if (chip.que != NOR_CHIP_BIEN) return -NOR_E_CHIP;   /* (P1) no es el de la placa: motivo = NOR_E_CHIP, nada se borra ni se programa */
#if !NOR_ESCRITURA
    motivo = NOR_E_DESHABILITADA;
    return -NOR_E_DESHABILITADA;
#else
    motivo = 0;
    return 0;
#endif
}
u32 nor_zona_bloque(void) { return hay_geo ? M18_LOGICO : 0; }
u32 nor_zona_bloques(void) { return hay_geo ? N_ZONA : 0; }
int nor_escr_motivo(void) { return motivo; }
const u8 *nor_zona(u32 bloque) { return espejo + bloque * M18_LOGICO; }
u32 nor_usr_bloques(void) { return hay_geo ? N_USR : 0; }
/* (I1; W6F) la zona de W6F se lee derecho del chip (no tiene espejo) */
u32 nor_w6f_bloques(void) { return hay_geo ? N_W6F : 0; }
const u8 *nor_w6f(u32 bloque) { return NOR_MEM(NOR_W6F_INI + bloque * M18_BLOQUE); }
const u8 *nor_usr(u32 bloque) { return espejo + (N_ZONA + bloque) * M18_LOGICO; }
nor_cuentas nor_cta;

#if NOR_ESCRITURA
/* ---- rutinas de RAM ---------------------------------------------------------------------------------- */

static inline __attribute__((always_inline)) void cerrar(u32 blq, u32 sr)
{
    NE(blq, 0x60); NE(blq, 0x01);                        /* volver a bloquear */
    if (sr & SR_ERROR) NE(blq, 0x50);
    NE(blq, 0xFF);
}

/* Borra el bloque de 256 KiB que empieza en «blq». reanudar = 1 si se vuelve tras NOR_R_SUSPENDIDO.
 * Devuelve 0, NOR_R_SUSPENDIDO (hay una IRQ pendiente: abrirlas y volver a llamar con reanudar = 1), los bits
 * de error del SR, o NOR_R_TIEMPO / NOR_R_RANGO. La suspensión (B0 / D0) es la del W600 [inferido que el M18
 * la tenga igual] — y el original del W580 SÍ suspende y reanuda (pista E4): así un borrado de ~1 s no
 * deja al sistema sin tic. */
RAMFN static u32 nor_ram_borrar(u32 blq, u32 reanudar, u32 con_irq)
{
    u32 sr, tope = TOPE_BORRAR, gracia = NOR_SONDEOS_SIN_MIRAR, lento = 0;
    if (!EN_ZONA(blq, M18_BLOQUE) || (blq & (M18_BLOQUE - 1))) return NOR_R_RANGO;
    if (!reanudar) {
        NE(blq, 0x70);
        NE(blq, 0x50);
        NE(blq, 0x60); NE(blq, 0xD0);
        NE(blq, 0x20); NE(blq, 0xD0);
    } else {
        NE(blq, 0x70);
        if (NL(blq) & SR_BORR_SUSP) NE(blq, 0xD0);
    }
    NE(blq, 0x70);
    for (;;) {
        sr = NL(blq);
        if (sr & SR_LISTO) {
            if (!(sr & SR_BORR_SUSP)) break;
            NE(blq, 0xD0); NE(blq, 0x70);                /* estaba suspendido: reanudar */
            gracia = NOR_SONDEOS_SIN_MIRAR;
        } else if (gracia) gracia--;                     /* (R3) recién empezado o reanudado: que avance antes de volver a suspender */
        else if (con_irq && !lento && NOR_IRQ()) {
            u32 t2 = TOPE_PROGRAMA;
            NE(blq, 0xB0); NE(blq, 0x70);
            do sr = NL(blq); while (!(sr & SR_LISTO) && --t2);
            if (sr & SR_BORR_SUSP) { NE(blq, 0xFF); return NOR_R_SUSPENDIDO; }
            if (sr & SR_LISTO) break;                    /* acabó justo antes de suspenderse */
            gracia = NOR_SONDEOS_SIN_MIRAR;              /* ni suspendido ni listo en el tope de la suspensión: se sigue sondeando */
        }
        if (!lento && !--tope) lento = 1;                /* (R3) pasado el tope NO se vuelve a la flash con el chip ocupado: se espera aquí */
    }
    cerrar(blq, sr);
    return lento ? NOR_R_TIEMPO : (sr & SR_ERROR);
}

/* Programa lo que cambia de UNA región de 1 KiB en «dir» (múltiplo de 0x400) EN MODO CONTROL, como hace el original
 * con su GDFS. org = las 512 palabras de la región con 0xFFFF en todo lo que no se toca. El origen, en RAM.
 *
 * CAMBIADO por el aviso del modo estricto de la pista E4 (el único real de su primera pasada: «[nor] programa con
 * búfer sobre región en modo control», 51 veces) y el hallazgo M5 de la pista R. Antes se mandaba la región entera
 * con la orden de BÚFER (0xE9 + 0x1FF + 512 palabras, con 0xFFFF de relleno): eso salió de la traza e14, donde solo
 * se ve escribir el FS de fábrica (modo OBJETO: región en blanco, las dos mitades de cada segmento, una sola vez).
 * [observado por E4 con el original en modo «aprender»] el GDFS —lo único que el original escribe en modo control,
 * y de donde sale nuestra fórmula— se programa PALABRA A PALABRA con la orden 0x41 (~8460 veces por arranque), solo en
 * la mitad baja de cada segmento de 32 bytes, y sí vuelve sobre palabras ya escritas, siempre bajando bits.
 * [observado en el desensamblado del controlador del original en RAM, 0x20001CE8..0x20001D40] para este chip elige
 * 0x41 como orden de palabra y 0xE9 como orden de búfer (0x40 y 0xE8 para los anteriores).
 * Así que: por cada palabra de la mitad baja que no sea 0xFFFF, `dir <- 0x41`, `dir <- dato` y esperar SR bit 7.
 * Una palabra distinta de 0xFFFF en la mitad ALTA de un segmento se rechaza (NOR_R_RANGO): no puede pasar (m18.h).
 * [sin saber] en un M18 de verdad: los tiempos (aquí una región va entera con las IRQ cortadas: hasta 256 palabras).
 * (2026-10-26) MEDIDO: una región entera son ~100-108 ms con las IRQ cortadas (el alimentador del tic del MP3 lo vio: 11-13 huecos
 * de >= 100 ms al guardar 64 KB, y el DSP se vaciaba). Ahora CEDE entre palabra y palabra, como el borrado (y como el original, que
 * suspende también la programación ante una IRQ, docs/re_musica_segundo_plano.md §5): con `con_irq`, si tras programar AL MENOS una
 * palabra hay una IRQ pendiente, la palabra en curso ya ha acabado (SR.7), el chip vuelve a LECTURA (FF) con el bloque aún
 * desbloqueado, *desde apunta a la siguiente y se devuelve NOR_R_SUSPENDIDO; quien llama abre las IRQ (entra el tic, que corre de la
 * flash ya legible) y vuelve a llamar con el mismo *desde. Al acabar, se bloquea como siempre. Ninguna palabra se programa dos veces
 * ni a medias: lo que se corta es ENTRE palabras. *desde = 0 la primera vez (comprobaciones y desbloqueo). */
RAMFN static u32 nor_ram_programar(u32 dir, const u16 *org, u32 *desde, u32 con_irq)
{
    u32 blq = dir & ~(M18_BLOQUE - 1), sr = SR_LISTO, i, tope = TOPE_PROGRAMA, lento = 0, hechas = 0;
    if (!EN_ZONA(dir, M18_REGION) || (dir & (M18_REGION - 1))) return NOR_R_RANGO;
    if (dir < NOR_ESPEJO_INI) return NOR_R_RANGO;                                       /* (AUD7 B2) el modo control, nunca en la zona de W6F (modo objeto) */
    if (ORIGEN_EN_FLASH(org)) return NOR_R_RANGO;                                       /* el origen, nunca en la flash (ni en su alias) */
    if (*desde >= 512) return NOR_R_RANGO;
    if (!*desde) {
        for (i = 0; i < 512; i++) if ((i & 8) && org[i] != 0xFFFF) return NOR_R_RANGO; /* mitad alta de un segmento de 16 palabras */
        NE(blq, 0x70); NE(dir, 0x50);
        NE(blq, 0x60); NE(blq, 0xD0);                    /* desbloquear */
        NE(dir, 0x70);
        /* (R3, AUD3-M2) si el chip no está listo en el tope, se SIGUE esperando: volver a la flash con él ocupado sería ejecutar
         * el registro de estado. Cuando por fin contesta se deja como siempre y se dice NOR_R_TIEMPO. */
        for (;;) { sr = NL(dir); if (sr & SR_LISTO) break; if (!lento && !--tope) lento = 1; }
        NE(dir, 0xFF);
    }
    for (i = *desde; i < 512 && !lento; i++) {
        u32 a = dir + 2 * i;
        if (org[i] == 0xFFFF) continue;
        if (con_irq && hechas && NOR_IRQ()) { NE(dir, 0xFF); *desde = i; return NOR_R_SUSPENDIDO; }   /* (2026-10-26) ceder: el chip, en lectura */
        NE(a, 0x41); NE(a, org[i]);                      /* programa de UNA palabra */
        tope = TOPE_PROGRAMA;
        for (;;) { sr = NL(a); if (sr & SR_LISTO) break; if (!lento && !--tope) lento = 1; }
        hechas++;
        if (sr & SR_ERROR) break;
    }
    cerrar(blq, sr);
    return lento ? NOR_R_TIEMPO : (sr & SR_ERROR);
}

/* (I1; W6F, fs2/INTEGRACION.md §1) Programa UNA región de 1 KiB en MODO OBJETO: la orden de BÚFER del original con su FS
 * (hal.md §16, «Programar una región»; emulador_estricto.md §2.2): `[B]<-70 · [A]<-50 · [B]<-60 D0 · [A]<-70, SR.7 · [A]<-FF ·
 * [A]<-E9 · [A]<-1FF · 512 palabras · [A]<-D0 · [A]<-70, SR.7 · [B]<-60 01 · (50 si error) · FF`. La región entera, alineada a
 * 0x400, las dos mitades de cada segmento, UNA sola vez: por eso ANTES de mandar nada se comprueba aquí mismo que está en
 * blanco (si no, NOR_R_RANGO y no se emite ninguna orden: jamás se reprograma una región en objeto ni se sube un bit). Y que
 * cae en la zona de W6F (el modo objeto, solo ahí: el almacén y W6U son de modo control) y que el origen no está en la flash.
 * Del texto de F2 (fs2/sistema/hal/nor.c) cambia una cosa, por la regla 8 (R3, AUD3-M2): si el chip no está listo en el
 * tope NO se vuelve a la flash con él ocupado; se sigue esperando aquí y se dice NOR_R_TIEMPO.
 * Lo nuestro que el original no hace: toda la región con las IRQ cortadas (él aborta la carga si hay una IRQ pendiente y la
 * repite; aquí son ~2 ms sin interrupciones). [sin saber] los tiempos en un M18 de verdad, y que acepte así el E9. */
RAMFN static u32 nor_ram_programar_objeto(u32 dir, const u16 *org)
{
    u32 blq = dir & ~(M18_BLOQUE - 1), sr, i, tope = TOPE_PROGRAMA, lento = 0;
    if (!EN_ZONA(dir, M18_REGION) || (dir & (M18_REGION - 1))) return NOR_R_RANGO;
    if (dir < NOR_W6F_INI || dir >= NOR_W6F_FIN) return NOR_R_RANGO;                    /* el modo objeto, solo en la zona de W6F */
    if (ORIGEN_EN_FLASH(org)) return NOR_R_RANGO;                                       /* el origen, nunca en la flash (ni en su alias) */
    for (i = 0; i < 512; i++) if (NL(dir + 2 * i) != 0xFFFF) return NOR_R_RANGO;       /* tiene que estar en blanco (el chip está en lectura) */
    NE(blq, 0x70); NE(dir, 0x50);
    NE(blq, 0x60); NE(blq, 0xD0);                        /* desbloquear */
    NE(dir, 0x70);
    for (;;) { sr = NL(dir); if (sr & SR_LISTO) break; if (!lento && !--tope) lento = 1; }
    if (!lento) {
        NE(dir, 0xFF);
        NE(dir, 0xE9); NE(dir, 0x1FF);
        for (i = 0; i < 512; i++) NE(dir + 2 * i, org[i]);
        NE(dir, 0xD0);
        NE(dir, 0x70);
        tope = TOPE_PROGRAMA;
        for (;;) { sr = NL(dir); if (sr & SR_LISTO) break; if (!lento && !--tope) lento = 1; }
    }
    cerrar(blq, sr);
    return lento ? NOR_R_TIEMPO : (sr & SR_ERROR);
}

#ifdef NOR_PRUEBA
u32 nor_pr_sanear(u32 blq);                              /* solo la prueba: las rutinas de RAM tal cual, para darles un rango malo */
u32 nor_pr_sanear(u32 blq) { return nor_ram_sanear(blq); }
u32 nor_pr_objeto(u32 dir, const u16 *org);
u32 nor_pr_objeto(u32 dir, const u16 *org) { return nor_ram_programar_objeto(dir, org); }
u32 nor_pr_control(u32 dir, const u16 *org);
u32 nor_pr_control(u32 dir, const u16 *org) { u32 d = 0; return nor_ram_programar(dir, org, &d, 0); }
#endif
/* ---- la parte que vive en la flash: prepara, llama a RAM con las IRQ cortadas y comprueba ----------
 * NOR_BORRADO_SUSPENDE = 1: si durante un borrado hay una IRQ pendiente, se suspende (B0), se la deja entrar y se
 * reanuda (D0), para que un borrado (~0,2..1 s) no deje al sistema sin tic. ES LO QUE HACE EL ORIGINAL [observado por la
 * pista E4 en cuanto su NOR tuvo tiempos: ~3300 suspensiones por arranque, unas 40 por borrado, sondeando SR.7; pasa
 * el modo estricto]. El hallazgo M10 de la pista R («el original no suspende en ninguna traza») era verdad solo con la
 * NOR sin tiempos, y durante unos minutos esto nació a 0 por él: vuelto a 1. Lo NUESTRO es cómo se decide: leyendo el
 * intcon «a pelo» (FB020100 & ~FB020104); [sin saber] cómo lo decide el original. -DNOR_BORRADO_SUSPENDE=0 = IRQ
 * cortadas todo el borrado. */
#ifndef NOR_BORRADO_SUSPENDE
#define NOR_BORRADO_SUSPENDE 1
#endif
#define NOR_VUELTAS_SUSP 100000u       /* suspensiones de un mismo borrado antes de terminarlo con las IRQ cortadas */
/* (I1) El borrado de UN bloque nuestro, en «dir»: «codigo» es lo que se apunta en el registro de reinicio mientras dura
 * (rst_dir) y «e» el espejo de ese bloque (NULL en W6F, que no tiene). El mismo camino para las tres zonas. */
static int borrar_dir(u32 dir, u8 codigo, u8 *e)
{
    u32 (*volatile f)(u32, u32, u32) = nor_ram_borrar;
    u32 r, i, reanudar = 0, vueltas = 0;
    const u32 *p = (const u32 *)(const void *)NOR_MEM(dir);   /* alineado a bloque */
    if (nor_veda) nor_veda_ops++;
    rst_nor_borrando(codigo);                            /* (AUD6 B1) si nos reinician a medias, el arranque sabrá que el suspendido es NUESTRO */
    for (;;) {
        u32 cerradas = irq_cerrar();
        /* (R3, AUD3-M2) agotadas las vueltas NO se abandona (el chip quedaría suspendido): la última va sin suspender */
        r = f(dir, reanudar, NOR_BORRADO_SUSPENDE && !cerradas && vueltas < NOR_VUELTAS_SUSP);
        if (!cerradas) irq_abrir();                      /* aquí entra la IRQ que hizo suspender */
        if (r != NOR_R_SUSPENDIDO) break;
        reanudar = 1; nor_cta.suspensiones++; vueltas++;
    }
    rst_nor_borrando(0);
    if (r == NOR_R_RANGO) return -NOR_E_RANGO;           /* no se ha mandado NINGUNA orden: ni chip ni espejo han cambiado */
    nor_cta.borrados++;
    if (r) { nor_cta.errores++; nor_cta.ultimo_sr = r; }
    if (e) {    /* (R3) el espejo dice SIEMPRE lo que hay en el chip, haya ido bien o mal: ANTES de cualquier otra salida */
        u32 l; const u8 *o = NOR_MEM(dir);
        for (l = 0; l < M18_LOGICO; l++) e[l] = o[m18_fis(l)];
    }
    if (r == NOR_R_TIEMPO) return -NOR_E_TIEMPO;
    for (i = 0; i < M18_BLOQUE / 4; i++) if (p[i] != 0xFFFFFFFFu) break;
    if (r) return -NOR_E_CHIP_SR;
    if (i < M18_BLOQUE / 4) { nor_cta.errores++; return -NOR_E_VERIFICA; }
    return 0;
}
static int borrar_en(u32 primero, u32 nblq, u32 bloque)
{
    if (motivo || bloque >= nblq) return -NOR_E_RANGO;
    return borrar_dir(NOR_ESPEJO_INI + (primero + bloque) * M18_BLOQUE, (u8)(primero + bloque + 1), espejo + (primero + bloque) * M18_LOGICO);
}

/* El puerto de hal/m18.h: programar una región = la rutina de RAM con las IRQ cortadas. (2026-10-26) CEDIENDO entre palabras si hay una
 * IRQ pendiente (nor_prog_cede = 1, por omisión; 0 = la región entera con las IRQ cortadas, como antes, para comparar por el puente):
 * aquí se abren las IRQ y se sigue desde la palabra siguiente. Con las IRQ ya cortadas al llamar (nadie lo hace) no se cede. */
static u32 ultimo_r;
volatile u32 nor_prog_cede = 1;
static int pto_programar(void *ctx, unsigned off, const unsigned short *buf)
{
    u32 (*volatile f)(u32, const u16 *, u32 *, u32) = nor_ram_programar;
    u32 desde = 0, vueltas = 0;
    if (nor_veda) nor_veda_ops++;
    for (;;) {
        u32 cerradas = irq_cerrar();
        ultimo_r = f((u32)(__SIZE_TYPE__)ctx + off, buf, &desde, nor_prog_cede && !cerradas && vueltas < NOR_VUELTAS_SUSP);
        if (!cerradas) irq_abrir();                      /* aquí entra la IRQ que hizo ceder */
        if (ultimo_r != NOR_R_SUSPENDIDO) break;
        vueltas++; nor_cta.cesiones++;
    }
    return ultimo_r != 0;
}
static const unsigned char *pto_region(void *ctx, unsigned off) { return NOR_MEM((u32)(__SIZE_TYPE__)ctx + off); }

static int programar_en(u32 primero, u32 nblq, u32 bloque, u32 desplaz, const void *datos, u32 n)
{
    static u16 buf[512];                                 /* en RAM (.bss): el origen del programa de búfer */
    M18Puerto p = { 0, pto_programar, pto_region };
    int r;
    if (motivo || bloque >= nblq) return -NOR_E_RANGO;
    p.ctx = (void *)(__SIZE_TYPE__)(NOR_ESPEJO_INI + (primero + bloque) * M18_BLOQUE);
    ultimo_r = 0;
    r = m18_programar(&p, espejo + (primero + bloque) * M18_LOGICO, desplaz, datos, n, buf, &nor_cta.programas);
    if (!r) return 0;
    nor_cta.errores++;
    if (r == -M18_E_RANGO || ultimo_r == NOR_R_RANGO) return -NOR_E_RANGO;
    if (r == -M18_E_BITS) return -NOR_E_BITS;
    if (r == -M18_E_VERIFICA) return -NOR_E_VERIFICA;
    if (ultimo_r == NOR_R_TIEMPO) return -NOR_E_TIEMPO;
    nor_cta.ultimo_sr = ultimo_r;
    return -NOR_E_CHIP_SR;
}
/* Las dos zonas, con las mismas rutinas: el almacén clave -> valor y los ficheros de usuario (W6U). */
int nor_borrar(u32 bloque) { return borrar_en(0, N_ZONA, bloque); }
int nor_programar(u32 bloque, u32 desplaz, const void *d, u32 n) { return programar_en(0, N_ZONA, bloque, desplaz, d, n); }
int nor_usr_borrar(u32 bloque) { return borrar_en(N_ZONA, N_USR, bloque); }
int nor_usr_programar(u32 bloque, u32 desplaz, const void *d, u32 n) { return programar_en(N_ZONA, N_USR, bloque, desplaz, d, n); }

/* ---- (I1; W6F) la zona de W6F: bloques físicos de 256 KiB en modo objeto, sin espejo -------------------------- */
int nor_w6f_borrar(u32 bloque)
{
    if (motivo || bloque >= N_W6F) return -NOR_E_RANGO;
    return borrar_dir(NOR_W6F_INI + bloque * M18_BLOQUE, (u8)(N_ESPEJO + 1 + bloque), 0);
}
/* UNA región entera: se copia a un búfer de RAM (alineado, en .bss), se programa con las IRQ cortadas desde RAM y se RELEE
 * y compara con lo pedido. */
int nor_w6f_programar(u32 bloque, u32 region, const u8 *datos)
{
    static u16 buf[512];                                 /* en RAM (.bss), alineado: el origen del programa de búfer */
    u32 (*volatile f)(u32, const u16 *) = nor_ram_programar_objeto;
    u32 dir, r, i, cerradas;
    const u8 *q;
    if (motivo || bloque >= N_W6F || region >= M18_BLOQUE / M18_REGION) return -NOR_E_RANGO;
    dir = NOR_W6F_INI + bloque * M18_BLOQUE + region * M18_REGION;
    if (nor_veda) nor_veda_ops++;
    for (i = 0; i < 512; i++) buf[i] = (u16)(datos[2 * i] | (datos[2 * i + 1] << 8));
    cerradas = irq_cerrar();
    r = f(dir, buf);
    if (!cerradas) irq_abrir();
    if (r == NOR_R_RANGO) { nor_cta.errores++; return -NOR_E_RANGO; }    /* no se mandó ninguna orden */
    nor_cta.programas++;
    if (r == NOR_R_TIEMPO) { nor_cta.errores++; return -NOR_E_TIEMPO; }
    if (r) { nor_cta.errores++; nor_cta.ultimo_sr = r; return -NOR_E_CHIP_SR; }
    q = NOR_MEM(dir);
    for (i = 0; i < 512; i++) if ((u16)(q[2 * i] | (q[2 * i + 1] << 8)) != buf[i]) { nor_cta.errores++; return -NOR_E_VERIFICA; }
    return 0;
}
#else
int nor_w6f_borrar(u32 bloque) { (void)bloque; return -NOR_E_DESHABILITADA; }
int nor_w6f_programar(u32 bloque, u32 region, const u8 *datos) { (void)bloque; (void)region; (void)datos; return -NOR_E_DESHABILITADA; }
int nor_borrar(u32 bloque) { (void)bloque; return -NOR_E_DESHABILITADA; }
int nor_programar(u32 bloque, u32 desplaz, const void *datos, u32 n) { (void)bloque; (void)desplaz; (void)datos; (void)n; return -NOR_E_DESHABILITADA; }
int nor_usr_borrar(u32 bloque) { (void)bloque; return -NOR_E_DESHABILITADA; }
int nor_usr_programar(u32 bloque, u32 desplaz, const void *datos, u32 n) { (void)bloque; (void)desplaz; (void)datos; (void)n; return -NOR_E_DESHABILITADA; }
#endif
