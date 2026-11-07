/* El ANFITRIÓN de Memory Stick del DB2020 (bloque 0xFC010000) para hal/ms.c. Pista T. docs/tarjeta.md §2.
 * Todo lo que es de ESTE chip y de ESTE teléfono: los cuatro registros, la IRQ 76, las puertas de reloj 22 y 23, el
 * bloque de reset 18, el regulador 6 del PMU y la detección por P3.4. Para otro modelo: otro fichero como este.
 *
 * De dónde sale: docs/memory_stick.md §1 y §2.1 (lectura del firmware original R8BE001), calibrado por la pista E9
 * con el original en el emulador (su §10). NADA se ha ejecutado en un teléfono.
 *
 * POR INTERRUPCIÓN, COMO EL ORIGINAL. El original no sondea nunca: todos sus TPC van con INTEN a 1 y es su proceso de
 * interrupción (0x4422CFC0, work/ms/dis_4422cfc0_int_process.txt) quien lee +8, reparte «INT de la tarjeta» / «TPC
 * terminado» (+ CRC / tiempo) y acusa con +C |= 0x0800. Aquí igual: ms_irq(). Que los bits de +8 se puedan leer con
 * INTEN a 0 es [sin saber] (MS-1); el camino por sondeo queda SOLO como reserva diagnóstica (ms_db2020_sondeo, o
 * compilar con -DMS_SONDEO=1) y no se usa en el sistema.
 *
 * Diferencias con la rutina del original, las dos a favor:
 *  - el original mira MSINT y, SI NO, RDY: si coinciden pierde el «terminado» y cae por su tope de 500 ms
 *    (memory_stick.md §1.3). Aquí se apuntan los dos.
 *  - una interrupción que no se deja acusar colgaría el teléfono dentro de la IRQ: si entra más de MS_IRQ_TOPE veces
 *    sin que se arranque un TPC, se quita INTEN, se tapa la línea y el controlador da MS_E_TORMENTA. */
#include "hal.h"
#include "ms_db2020.h"

#define MS_CMD  R32(0xFC010000u)        /* +0 orden: TPC << 12 | tamaño. Escribirlo arranca la transacción */
#define MS_DAT  R32(0xFC010004u)        /* +4 FIFO, de dos en dos palabras (8 bytes) */
#define MS_STA  R32(0xFC010008u)        /* +8 estado */
#define MS_SYS  R32(0xFC01000Cu)        /* +C sistema */
#define S_DRQ   0x4000u
#define S_MSINT 0x2000u
#define S_RDY   0x1000u
#define S_CRC   0x0200u
#define S_TOE   0x0100u
#define C_RST   0x8000u
#define C_INTEN 0x2000u
#define C_INTCLR 0x0800u
#define P3_DAT   R16(0xF900001Eu)       /* GPIO puerto 3: datos, máscara de IRQ y nivel que interrumpe (hal/gpio.c) */
#define P3_MASC  R16(0xF9000022u)
#define P3_NIVEL R16(0xF9000024u)
#define PIN_MS   0x10u                  /* P3.4: 0 = hay tarjeta [44A1.c:5538] */
#define MS_IRQ_TOPE 64u

#ifndef MS_SONDEO
#define MS_SONDEO 0
#endif
u8 ms_db2020_sondeo = MS_SONDEO;        /* RESERVA DIAGNÓSTICA: 1 = sin INTEN, sondeando +8 (hipótesis H1: [sin saber] en el aparato) */

MsDbCuentas ms_db2020_cta;

static volatile u8 f_rdy, f_res, f_int, tormenta;
static volatile u32 irq_racha;
static u8 con_tension, en_paralelo, det_armada, iniciado;

/* ---- la interrupción (línea 76, por nivel) ------------------------------------------------------------------- */
static void ms_irq(void)
{
    u32 s = MS_STA;
    ms_db2020_cta.irqs++;
    if (s & S_MSINT) f_int = 1;
    if (s & S_RDY) { f_res = (u8)((s & S_CRC) ? MS_T_CRC : (s & S_TOE) ? MS_T_TIEMPO : MS_T_BIEN); f_rdy = 1; }
    if (!(s & (S_MSINT | S_RDY))) ms_db2020_cta.irqs_vacias++;
    MS_SYS |= C_INTCLR;                                    /* el acuse, lo último [0x4422D002..D00C] */
    if (++irq_racha > MS_IRQ_TOPE) {                       /* tope: no se deja acusar */
        MS_SYS &= ~C_INTEN; intc_tapar(76);
        tormenta = 1; ms_db2020_cta.tormentas++;
    }
}
/* Lo que haya llegado. Por interrupción son los apuntes de ms_irq; por sondeo (reserva) se lee +8 y se acusa aquí. */
static void sondear(void)
{
    u32 s;
    if (!ms_db2020_sondeo) return;
    s = MS_STA;
    if (s & (S_MSINT | S_RDY)) {
        if (s & S_MSINT) f_int = 1;
        if (s & S_RDY) { f_res = (u8)((s & S_CRC) ? MS_T_CRC : (s & S_TOE) ? MS_T_TIEMPO : MS_T_BIEN); f_rdy = 1; }
        MS_SYS |= C_INTCLR;
    }
}

/* ---- detección: P3.4, con la IRQ de GPIO del puerto 3 (línea 68) en los dos sentidos, como el original --------
 * [FUN_44a155dc(3, 0x10, 2, 100)]. La IRQ es por NIVEL: se arma esperando el nivel CONTRARIO al que hay; la rutina
 * solo cuenta y desarma (su efecto es sacar al sistema del reposo); hay() vuelve a armar. El antirrebote de 20 ms lo
 * hace hal/ms.c. Mismo esquema que el deslizador (hal/gpio.c). */
static void det_armar(void)
{
    u32 e = irq_cerrar();
    u16 nivel = P3_NIVEL & 0xFF, pin = P3_DAT & PIN_MS;
    P3_NIVEL = pin ? (u16)(nivel & ~PIN_MS) : (u16)(nivel | PIN_MS);
    P3_MASC = (u16)((P3_MASC | PIN_MS) & 0xFF);
    if (!e) irq_abrir();
}
static void det_irq(void)
{
    P3_MASC = (u16)(P3_MASC & ~PIN_MS & 0xFF);
    ms_db2020_cta.det_irqs++;
}
static int hay(void)
{
    int v = (P3_DAT & PIN_MS) == 0;
    if (det_armada && !(P3_MASC & PIN_MS)) det_armar();
    return v;
}

/* (28-oct) RESCATE: con interrupción, si el apunte no llega pero el estado +8 YA tiene el bit, se coge de ahí y se acusa, como en ms_irq.
 * [observado 28-oct en el aparato, senew-RAM: +8 = 0x3028 (MSINT y RDY puestos), +C con INTEN, la línea 76 sin tapar y NADA pendiente en
 * el INTC (ITR = 0): la tarjeta decía «hecho» y la línea no subía; cada espera agotaba su tope de 1,5 s (MsInfo.e_int: 2192 en una noche;
 * Doom cargaba en 132 s) y al apagar y encender la tarjeta volvía a ir.] Hipótesis [sin probar]: la salida de interrupción del anfitrión
 * va por flanco y, perdido un acuse, el estado se queda arriba sin más flancos; acusarlo aquí lo baja. SOLO con el bloque encendido
 * (con_tension): leer +8 con los relojes cerrados cuelga el bus [observado 28-oct, 05:53]. `rescates` va en .data: el .bss no se mueve
 * (con el espejo de 16 B, mover el .bss estropeó las lecturas: docs/senew_en_ram.md). */
static u32 rescates __attribute__((section(".data")));
static void rescatar(void)
{
    u32 s, e;
    if (ms_db2020_sondeo || !con_tension) return;
    e = irq_cerrar();                                       /* que ms_irq no se cruce con esto */
    s = MS_STA;
    if (s & (S_MSINT | S_RDY)) {
        if (s & S_MSINT) f_int = 1;
        if (s & S_RDY) { f_res = (u8)((s & S_CRC) ? MS_T_CRC : (s & S_TOE) ? MS_T_TIEMPO : MS_T_BIEN); f_rdy = 1; }
        MS_SYS |= C_INTCLR;
        rescates++;
    }
    if (!e) irq_abrir();
}

/* ---- esperas con tope ---------------------------------------------------------------------------------------- */
/* Espera a que *f se ponga, como mucho «ms» (por el tic de 4 ms). Dos topes más: de VUELTAS, por si el tic no latiera
 * (IRQ cortadas): ~40 000 vueltas por ms son varias veces lo que da la CPU a 208 MHz, así que solo estira la espera;
 * y la tarjeta, que puede salir en cualquier momento. MS_T_BIEN, MS_T_TIEMPO, MS_T_FUERA o MS_T_ANFITRION. */
static int esperar(volatile u8 *f, u32 ms)
{
    u32 t0 = tics, plazo = ms / 4 + 2, vueltas = 0, tope = ms * 40000u + 200000u;
    for (;;) {
        sondear();
        if (*f) return MS_T_BIEN;
        if (!(vueltas & 0xFFFu) && (u32)(tics - t0) >= 2) { rescatar(); if (*f) return MS_T_BIEN; }   /* (28-oct) tras 4-8 ms sin el apunte */
        if (tormenta) return MS_T_ANFITRION;
        if ((u32)(tics - t0) > plazo) return MS_T_TIEMPO;
        if (++vueltas > tope) { ms_db2020_cta.cortes++; return MS_T_TIEMPO; }
        if (!(vueltas & 0xFF) && (P3_DAT & PIN_MS)) return MS_T_FUERA;
    }
}
/* DRQ del FIFO: 0x4000 sondeos y, si no llega, hasta ~1 s más mirando cada milisegundo [4422.c:16156: 10 x 104 ms].
 * DRQ se sondea también en el original (no hay interrupción de DRQ). Si entre tanto el TPC «termina» (sin tarjeta
 * el anfitrión da tiempo agotado enseguida), se sale con su resultado. */
static int drq(void)
{
    u32 i;
    for (i = 0; i < 0x4000u; i++) {
        if (MS_STA & S_DRQ) return MS_T_BIEN;
        sondear();
        if (f_rdy) return f_res ? f_res : MS_T_TIEMPO;
    }
    for (i = 0; i < 1040u; i++) {
        if (MS_STA & S_DRQ) return MS_T_BIEN;
        sondear();
        if (f_rdy) return f_res ? f_res : MS_T_TIEMPO;
        if (tormenta) return MS_T_ANFITRION;
        if (P3_DAT & PIN_MS) return MS_T_FUERA;
        espera_ms(1);
    }
    return MS_T_TIEMPO;
}

/* ---- una transacción [StartTPC 4422.c:16377, FifoRead :16206, FifoWrite :16294, WaitDone :16178] --------------
 * Los TPC de datos de 512 (READ_LONG_DATA y, MSW, WRITE_LONG_DATA [WriteLongData 0x4422C9D8: 0xD con 0x200]) y los de
 * control, cada uno con el tamaño que le pone el original. WRITE_SHORT_DATA (0xC) y SET_CMD (0xE) no pasan: MS_T_NEGADO.
 * Órdenes por EX_SET_CMD: 0x20, 0x21 y 0x24; nunca 0x10 (formato). */
static int tpc(int t, u8 *b, u32 n, int a_la_tarjeta)
{
    u32 i, w[2]; int r;
    switch (t) {
    case MS_TPC_READ_LONG_DATA:   if (n != 0x200 || a_la_tarjeta) return MS_T_NEGADO; break;
    case MS_TPC_READ_REG:         if (!n || n > 0x3FF || a_la_tarjeta) return MS_T_NEGADO; break;
    case MS_TPC_GET_INT:          if (n != 1 || a_la_tarjeta || en_paralelo) return MS_T_NEGADO; break;   /* en paralelo no hay GET_INT por TPC [0x4422C984] */
    case MS_TPC_SET_RW_REG_ADRS:  if (n != 4 || !a_la_tarjeta) return MS_T_NEGADO; break;
    case MS_TPC_WRITE_LONG_DATA:  if (n != 0x200 || !a_la_tarjeta) return MS_T_NEGADO; break;   /* (MSW) */
    case MS_TPC_EX_SET_CMD:       if (n != 7 || !a_la_tarjeta || (b[0] != 0x20 && b[0] != 0x21 && b[0] != 0x24)) return MS_T_NEGADO; break;
    case MS_TPC_WRITE_REG:        if (n != 1 || !a_la_tarjeta) return MS_T_NEGADO; break;   /* solo el parámetro de sistema (1 byte) */
    default: return MS_T_NEGADO;
    }
    if (!con_tension) return MS_T_ANFITRION;
    if (tormenta) return MS_T_ANFITRION;
    if (P3_DAT & PIN_MS) return MS_T_FUERA;
    f_rdy = 0; irq_racha = 0;
    MS_CMD = (u32)t << 12 | n;
    for (i = 0; i < n; i += 8) {                            /* de 8 en 8 bytes; el resto, a ceros / se tira */
        u32 m = n - i < 8 ? n - i : 8;
        if ((r = drq()) != 0) { if (r == MS_T_TIEMPO) { ms_db2020_cta.t_drq++; if (a_la_tarjeta) ms_db2020_cta.t_escr++; } return r; }
        if (a_la_tarjeta) { w[0] = w[1] = 0; memcpy(w, b + i, m); MS_DAT = w[0]; MS_DAT = w[1]; }
        else              { w[0] = MS_DAT; w[1] = MS_DAT; memcpy(b + i, w, m); }
    }
    r = esperar(&f_rdy, 500);                               /* [4422.c:16185: 500 ms] */
    if (r) { if (r == MS_T_TIEMPO) { ms_db2020_cta.t_rdy++; if (a_la_tarjeta) ms_db2020_cta.t_escr++; } return r; }
    if (f_res == MS_T_TIEMPO) { ms_db2020_cta.t_toe++; if (a_la_tarjeta) ms_db2020_cta.t_escr++; }     /* (2026-10-26) cómo falla: ver MsDbCuentas */
    else if (f_res == MS_T_CRC) ms_db2020_cta.t_crc++;
    return f_res;
}
static int espera_int(u32 ms)
{
    int r;
    if (!con_tension) return MS_T_ANFITRION;
    irq_racha = 0;
    r = esperar(&f_int, ms);
    if (r == MS_T_BIEN) f_int = 0;                          /* consumida */
    return r;
}
static void olvida_int(void)
{
    u32 e = irq_cerrar();
    f_int = 0;
    if (!e) irq_abrir();
    if (ms_db2020_sondeo && con_tension && (MS_STA & S_MSINT)) MS_SYS |= C_INTCLR;
}
static int int_paralelo(void)                               /* [4422.c:15925]: CED ERR BREQ . . . . CMDNK */
{
    u32 s;
    if (!con_tension) return 0;
    s = MS_STA;
    ms_db2020_cta.sta_ult = s;
    return (int)((s & 0xE) << 4 | (s & 1));
}
/* (M2 ag, 2026-10-21) SetIfMode COMO EL ORIGINAL, PASO A PASO. El pseudo-C (4422.c:16139/16146) lo enseña como UNA escritura
 * («+C = (+C & 0xFFFFBF2F) | 8»), pero el desensamblado de FUN_4422cc04 son CINCO lecturas-escrituras seguidas de +C, en este orden
 * [observado, tools/e_dis.py 4422CC05]:
 *   paralelo (0x4422CC54..CC78): &= ~0x4000 (SRAC) ; &= ~0x10 (REI) ; |= 0x08 (REO) ; &= ~0x80 ; &= ~0x40
 *   serie    (0x4422CC24..CC4A): |=  0x4000 (SRAC) ; |=  0x10 (REI) ; &= ~0x08 (REO) ; &= ~0x80 ; &= ~0x40
 * (el emulador lo confirma: tras encender, «anfitrion en serie (+C = 0x4005)» y luego 0x4015; al pasar a paralelo, 0x2015 antes que
 * 0x200D). Que el bloque necesite los valores intermedios es [sin saber]; se copia tal cual porque cuesta cuatro accesos. */
static void if_modo(int par)
{
    if (par) { MS_SYS &= ~0x4000u; MS_SYS &= ~0x0010u; MS_SYS |= 0x0008u; }
    else     { MS_SYS |= 0x4000u;  MS_SYS |= 0x0010u;  MS_SYS &= ~0x0008u; }
    MS_SYS &= ~0x0080u; MS_SYS &= ~0x0040u;
}
static void paralelo(void)                                  /* SetIfMode(1) [FUN_4422cc04, 0x4422CC54] */
{
    ms_db2020_cta.sys_antes = MS_SYS;
    if_modo(1);
    en_paralelo = 1;
    ms_db2020_cta.sys_par = MS_SYS; ms_db2020_cta.sta_par = MS_STA; ms_db2020_cta.pasos_par++;
}

/* ---- encender y apagar: el orden EXACTO del original --------------------------------------------------------- */
static int rst(void)                                        /* +C bit 15: se pone y se quita solo. NUESTRO: tope de 100 ms (el original, ninguno) */
{
    u32 n;
    MS_SYS |= C_RST;
    for (n = 0; (MS_SYS & C_RST) && n < 100; n++) espera_ms(1);
    return (MS_SYS & C_RST) ? -1 : 0;
}
static void cerrar_bloque(void)
{
    R16(SYSCON + 0x14) &= (u16)~0x0080u;                    /* bloque 18 a reset */
    R16(SYSCON + 0x1A) |= 0x0080;                           /* puerta 23 cerrada */
    R16(SYSCON + 0x1A) |= 0x0100;                           /* puerta 22 cerrada */
}
static void apagar(void)                                    /* MemStickPD_Shutdown, 0x4422C818 */
{
    u8 v; int k, fuera = 0;
    if (!iniciado) return;
    if (!(R16(SYSCON + 0x1A) & 0x0180) && (R16(SYSCON + 0x14) & 0x0080)) MS_SYS &= ~C_INTEN;   /* (solo si el bloque está vivo) */
    intc_tapar(76);
    for (k = 0; k < 3 && !fuera; k++)                       /* SetMode(6, 0): la tensión, lo primero. Releída: tiene que quedar a 0 */
        if (pmu_leer(0x43, &v) == 0 && ((v & 3) == 0 || (pmu_escribir(0x43, (u8)(v & 0xFC)) == 0 && pmu_leer(0x43, &v) == 0 && (v & 3) == 0))) fuera = 1;
    con_tension = 0; en_paralelo = 0;
    f_rdy = f_int = 0;
    if (!fuera) { ms_db2020_cta.pmu_fallos++; return; }     /* NUESTRO: si el PMU no deja quitar la tensión, los relojes NO se cierran
                                                               (cerrarlos con la tarjeta en tensión es lo que el original nunca hace) */
    if (!(R16(SYSCON + 0x1A) & 0x0180) && (R16(SYSCON + 0x14) & 0x0080)) {
        rst(); rst();
        MS_SYS = (MS_SYS & ~7u) | 5;
    }
    cerrar_bloque();
}
static int encender(void)                                   /* MemStickPD_Init, 0x4422C6F8 (docs/memory_stick.md §2.1, pasos 1..11) */
{
    u8 v;
    if (reloj_mhz != 208) return MS_E_RELOJ;                /* el original pide 208 MHz mientras la tarjeta está encendida y TODOS sus TPC van a 208 */
    f_rdy = f_int = 0; tormenta = 0; irq_racha = 0;
    R16(SYSCON + 0x40) |= 0x0002;                           /* 1. [0x4422C712] qué es: [sin saber] */
    R16(SYSCON + 0x1A) &= (u16)~0x0100u;                    /* 2. puerta 22 */
    R16(SYSCON + 0x1A) &= (u16)~0x0080u;                    /* 3. puerta 23 */
    R16(SYSCON + 0x14) |= 0x0080;                           /* 4. bloque 18 vivo */
    if (rst() || rst()) { cerrar_bloque(); return MS_E_ANFITRION; }   /* 5. dos resets */
    MS_SYS = (MS_SYS & ~7u) | 5;                            /* 6. BSY = 5 */
    /* 7 y 8. La tensión: SOLO los valores vistos en el original (otro código puede ser otra tensión) y solo si el PMU
     * contesta y se relee lo escrito: SetVoltage(6, 4) = 0x44 bits 0..1 <- 2; SetMode(6, 3) = 0x43 bits 0..1 <- 3. */
    if (pmu_leer(0x44, &v) || pmu_escribir(0x44, (u8)((v & 0xFC) | 2)) || pmu_leer(0x44, &v) || (v & 3) != 2) { cerrar_bloque(); return MS_E_PMU; }
    ms_db2020_cta.pmu44 = v;
    if (pmu_leer(0x43, &v) || pmu_escribir(0x43, (u8)((v & 0xFC) | 3))) { cerrar_bloque(); return MS_E_PMU; }
    con_tension = 1; en_paralelo = 0;
    /* (M2 ag) releída, como la 0x44 (antes no se miraba): si el PMU no tomó la tensión, la tarjeta «no contesta» por eso. La tensión ya
     * está pedida: con_tension = 1 para que apagar() la quite. */
    if (pmu_leer(0x43, &v) || (v & 3) != 3) { ms_db2020_cta.pmu43 = v; ms_db2020_cta.pmu_tension_mal++; return MS_E_PMU; }
    ms_db2020_cta.pmu43 = v;
    if_modo(0);                                             /* 9. serie [FUN_4422cc04 paso a paso, 0x4422CC24] y acuse */
    MS_SYS |= C_INTCLR;
    ms_db2020_cta.sys_serie = MS_SYS;
    ms_db2020_cta.s40 = R16(SYSCON + 0x40); ms_db2020_cta.s1a = R16(SYSCON + 0x1A); ms_db2020_cta.s14 = R16(SYSCON + 0x14); ms_db2020_cta.s1c = R16(SYSCON + 0x1C);
    ms_db2020_cta.s02 = R16(SYSCON + 0x02); ms_db2020_cta.s0a = R16(SYSCON + 0x0A);
    if (!ms_db2020_sondeo) {
        intc_habilita_nivel(76, ms_irq, 31);                /* 10. la IRQ 76 por nivel, prioridad 31 (ILR = 0x7E, como el original) */
        MS_SYS |= C_INTEN;                                  /* 11. */
    }
    return 0;
}

static u32 reloj_ms(void) { return tics * 4; }
static void sin_latido(void) { }
static MsAnf anfitrion = { hay, encender, apagar, paralelo, tpc, espera_int, olvida_int, int_paralelo, espera_ms, reloj_ms, sin_latido };

/* Lo llama la plataforma una vez, con el tic ya en marcha. «latido» (puede ser 0): lo que hay que llamar en las
 * esperas largas para que el perro de la plataforma no muerda. */
void ms_db2020_init(void (*latido)(void))
{
    if (latido) anfitrion.latido = latido;
    iniciado = 1;
    intc_habilita(68, det_irq);                             /* GPIO puerto 3 */
    det_armada = 1; det_armar();
    ms_init(&anfitrion);
}
