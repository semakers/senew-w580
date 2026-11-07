/* Memory Stick PRO: el protocolo y la política. Pista T; la escritura de sectores, pista MSW (docs/tarjeta_escritura.md).
 * docs/tarjeta.md §2.
 * Portable: ni un registro; todo lo de la placa entra por MsAnf (hal/ms.h). El plano es docs/memory_stick.md §4, con
 * los tres retoques de su §10.3, y cada paso cita de dónde sale del firmware original ([observado] en su código y
 * [ejecutado] por la pista E9 en el emulador). Lo que es NUESTRO va marcado «NUESTRO:».
 *
 * orden() solo deja pasar 0x20 (leer sectores), 0x24 (leer atributos) y, SOLO desde ms_escribir (puerta_w), 0x21 (escribir
 * sectores). Nunca 0x10 (formato).
 *
 * Reglas de la casa: todos los bucles tienen tope; ante CUALQUIER fallo a media orden se hace lo único que el
 * original hace y está visto —apagar, esperar 250 ms, encender, identificar— hasta 3 veces; que la tarjeta salga en
 * cualquier momento es un resultado más (MS_E_FUERA), nunca un cuelgue. */
#include "ms.h"

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);

static const MsAnf *anf;
static MsInfo inf;
static u8 dentro;                       /* lo que dice el pin TRAS el antirrebote */
static u8 encendida;
static u8 hubo_apagado;
static u32 t_uso, t_apagado;
static u8 puerta_w;                     /* (MSW) 1 solo mientras ms_escribir manda su orden 0x21 */
static u32 cierre_lba[MS_CIERRE_MAX];   /* (28-oct) la zona de cierre (ver «el cierre», más abajo) */
static u8 cierre_n, cierre_i, cerrando, cierre_seguidos;
static u8 cierre_sector[512];
static u8 abierta;                      /* (28-oct) se mandó una orden (0x20/0x21/0x24) y aún no se vio su CED: no se puede mandar otra */
/* (M2, 2026-10-20) EL BUS EN SERIE (1 bit). Con MS_SOLO_SERIE (el W810: su anfitrión no tiene 4 bits) siempre. Si no, empieza a 0
 * y se pone a 1 la PRIMERA vez que el paso a 4 bits falla (en el W580 real: «la tarjeta no pasa a 4 bits»): se apaga, se reintenta
 * y desde ahí se habla en serie hasta el próximo ms_init (el arranque). Es del ANFITRIÓN, no de la tarjeta: no se borra al
 * sacarla. NUESTRO: el original del W580 no tiene vuelta atrás (sin 4 bits no monta). */
#ifdef MS_SOLO_SERIE
#define en_serie 1
#else
static u8 en_serie;
#endif

const MsInfo *ms_info(void) { return &inf; }
const char *ms_motivo(int e)
{
    switch (e) {
    case 0:                return "bien";
    case MS_E_FUERA:       return "no hay tarjeta (o se saco a media orden)";
    case MS_E_ANFITRION:   return "el controlador no sale del reset";
    case MS_E_PMU:         return "el PMU no contesta: no se le da tension";
    case MS_E_RELOJ:       return "la CPU no esta a 208 MHz";
    case MS_E_MUDA:        return "la tarjeta no contesta";
    case MS_E_NO_PRO:      return "no es una Memory Stick PRO";
    case MS_E_CED:         return "la tarjeta no llega a estar lista";
    case MS_E_ERR:         return "la tarjeta da error al encender";
    case MS_E_PARALELO:    return "la tarjeta no pasa a 4 bits";
    case MS_E_ATRIBUTOS:   return "no se pueden leer los atributos";
    case MS_E_SIN_INFO:    return "atributos sin informacion del sistema";
    case MS_E_INFO_LEJOS:  return "informacion del sistema fuera del primer sector";
    case MS_E_CAPACIDAD:   return "capacidad imposible";
    case MS_E_RANGO:       return "sector fuera de la tarjeta";
    case MS_E_LECTURA:     return "error de lectura";
    case MS_E_OTRA:        return "la tarjeta cambio mientras dormia";
    case MS_E_TORMENTA:    return "la interrupcion del controlador no se deja acusar";
    case MS_E_ESCRITURA:   return "error de escritura";
    case MS_E_PROTEGIDA:   return "la tarjeta esta protegida contra escritura";
    default:               break;                      /* (I1) -Wswitch-default: lo de abajo */
    }
    return "error";
}
const char *ms_par_fase(int f)
{
    switch (f) {
    case MS_PAR_NADA:          return "-";
    case MS_PAR_RW_ADRS:       return "RW_ADRS";               /* SET_RW_REG_ADRS(0,0x1F,0x10,1), aún en serie */
    case MS_PAR_WRITE_REG:     return "WRITE_REG";             /* reg 0x10 <- 0, aún en serie: la tarjeta no acepta el cambio */
    case MS_PAR_LEER_REG:      return "READ_REG 4b";           /* ya en 4 bits, el primer TPC no sale (tiempo o CRC): el bus de 4 bits no anda */
    case MS_PAR_REG_DISTINTOS: return "regs 4b distintos";     /* sale, pero los registros no son los leídos en serie: datos rotos en 4 bits */
    case MS_PAR_ATRIBUTOS:     return "atrib 4b";              /* los registros sí, pero los atributos (orden + INT + 512 bytes) no */
    case MS_PAR_SIN_INT:       return "sin INT en 4b";         /* los atributos salen, pero la INT de la tarjeta no llega en paralelo */
    default:                   break;
    }
    return "?";
}

/* ---- transacciones ------------------------------------------------------------------------------------------- */

static int tpc(int t, u8 *b, u32 n, int a_la_tarjeta)
{
    int r = anf->tpc(t, b, n, a_la_tarjeta);
    inf.tpc++;
    if (r == MS_T_TIEMPO) inf.e_tiempo++; else if (r == MS_T_CRC) inf.e_crc++;
    return r;
}
/* 3 intentos, como el original en ReadReg / WriteReg / SetRWRegAdrs (docs/memory_stick.md §1.3 punto 4) */
static int tpc3(int t, u8 *b, u32 n, int a_la_tarjeta)
{
    int r = 0, k;
    for (k = 0; k < 3; k++) {
        if (k) inf.tpc_reintentos++;
        r = tpc(t, b, n, a_la_tarjeta);
        if (r != MS_T_TIEMPO && r != MS_T_CRC) break;      /* bien, o algo que repetir no arregla (fuera, negado) */
    }
    return r;
}
static int rw_adrs(u8 dl, u8 nl, u8 de, u8 ne) { u8 b[4]; b[0] = dl; b[1] = nl; b[2] = de; b[3] = ne; return tpc3(MS_TPC_SET_RW_REG_ADRS, b, 4, 1); }
/* EX_SET_CMD: 7 bytes «orden, cuenta (BE16), dirección (BE32)» [0x4422CB72..CB9C].
 * LA ÚNICA PUERTA DE LAS ÓRDENES: 0x20 = leer sectores, 0x24 = leer atributos, 0x21 = escribir sectores (MSW: solo con
 * puerta_w, que pone escribir_orden). Cualquier otra (0x10 formatear, 0x25…) se niega aquí. NUESTRO: sin reintento (el original repite hasta 3 veces el TPC; repetir una orden
 * que quizá llegó deja a la tarjeta con dos: preferimos el apagado). */
static int orden(u8 c, u32 n, u32 a)
{
    u8 b[7];
    if (c != 0x20 && c != 0x24 && !(c == 0x21 && puerta_w)) return MS_T_NEGADO;
    b[0] = c; b[1] = (u8)(n >> 8); b[2] = (u8)n; b[3] = (u8)(a >> 24); b[4] = (u8)(a >> 16); b[5] = (u8)(a >> 8); b[6] = (u8)a;
    abierta = 1;                                            /* (28-oct) aunque el TPC falle: no se sabe si la tomó */
    return tpc(MS_TPC_EX_SET_CMD, b, 7, 1);
}
/* (ms810) EN SERIE (el W810 siempre: su anfitrión no tiene modo de 4 bits, docs/ms_w810.md; el W580 si el paso a 4 bits falló): el
 * registro INT se lee con el TPC GET_INT, como MemStickPD_GetInt del W810 (0x44CB1594). 0..255 = el registro; MS_E_FUERA;
 * INT_NO_SE = el TPC falló «veces» veces. */
#define INT_NO_SE (-0x100)
static u32 int_reintentos;
static u8 int_fallos;                                       /* (M2) respuestas halladas sin INT mientras inf.int_serie está sin saber */
u32 ms_int_reintentos(void) { return int_reintentos; }
static int int_serie(int veces)                             /* NUESTRO: hasta «veces» (leer un registro se puede repetir sin daño; el original, 1) */
{
    u8 g; int t, k;
    for (k = 0; k < veces; k++) {
        if (k) int_reintentos++;
        g = 0; t = tpc(MS_TPC_GET_INT, &g, 1, 0);
        if (t == MS_T_FUERA) return MS_E_FUERA;
        if (!t) return g;
        if (t != MS_T_TIEMPO && t != MS_T_CRC) break;
    }
    return INT_NO_SE;
}
/* Espera a que la tarjeta DIGA algo (BREQ, CED, ERR o CMDNK) tras una INT. Devuelve el registro INT o un MS_E_*.
 * El original espera la señal de la interrupción (<= 1500 ms) y lee el estado una vez [4477.c:11185]. NUESTRO: si la
 * INT llega pero el estado aún no dice nada (una INT de más, p. ej. la del paso a paralelo: MS-12), se espera otra,
 * hasta 4; y si la INT no llega pero el estado SÍ dice algo, se acepta (y se cuenta en e_int). */
/* (M2) EN SERIE: ¿HAY MSINT? Que el anfitrión del DB2020 levante MSINT en serie es [sin saber] (el original del W580 solo habla en
 * serie para identificar, y ahí pregunta el CED con GET_INT sin esperar INT). Así que se APRENDE (inf.int_serie):
 *  - sin saber o «sí»: se espera la INT a tramos de MS_TRAMO_SERIE_MS (vuelve en cuanto llega) y tras cada tramo se pregunta con
 *    GET_INT (lo del W810: INT y GET_INT). Si la INT llega: «sí». Si MS_INT_FALLOS_SERIE veces la tarjeta ya estaba lista y la
 *    INT no llegó: «no».
 *  - «no»: GET_INT cada ~1 ms (espera_ms, por el temporizador de 32 kHz). NO seguidos: en el modelo del emulador (su H4) cada TPC
 *    aplaza la INT de la tarjeta, y 256 GET_INT seguidos bajaron la lectura en serie de 270 a 130 KB/s.
 * Tope, los mismos 1500 ms del original. e_int cuenta las respuestas halladas sin su INT. */
static int espera_serie(void)
{
    u32 t0 = anf->ms(); int r, v;
    for (;;) {
        if (inf.int_serie == MS_INT_SERIE_NO) { r = MS_T_TIEMPO; anf->espera_ms(1); }
        else r = anf->espera_int(MS_TRAMO_SERIE_MS);
        if (r == MS_T_FUERA) return MS_E_FUERA;
        if (r == MS_T_BIEN) inf.int_serie = MS_INT_SERIE_SI;
        v = int_serie(r == MS_T_BIEN ? 3 : 1);              /* tras la INT, el LD del W810 pregunta con GET_INT [0x449E8A3C: ReadAtrbSectors] */
        if (v == MS_E_FUERA) return MS_E_FUERA;
        if (v != INT_NO_SE && (v & (MS_INT_BREQ | MS_INT_CED | MS_INT_ERR | MS_INT_CMDNK))) {
            if (r != MS_T_BIEN) {
                inf.e_int++;
                if (inf.int_serie == MS_INT_SERIE_SIN_SABER && ++int_fallos >= MS_INT_FALLOS_SERIE) inf.int_serie = MS_INT_SERIE_NO;
            }
            return v;
        }
        if ((u32)(anf->ms() - t0) >= 1500) { inf.e_int++; return MS_E_LECTURA; }
        if (!anf->hay()) return MS_E_FUERA;
    }
}
static int espera_tarjeta(void)
{
    int k, r, v;
    if (en_serie) return espera_serie();
    for (k = 0; k < 4; k++) {
        r = anf->espera_int(1500);
        if (r == MS_T_FUERA) return MS_E_FUERA;
        v = anf->int_paralelo();
        if (r != MS_T_BIEN) inf.e_int++;
        if (v & (MS_INT_BREQ | MS_INT_CED | MS_INT_ERR | MS_INT_CMDNK)) return v;
        if (r != MS_T_BIEN) break;
    }
    return MS_E_LECTURA;
}
static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 be32(const u8 *p) { return be16(p) << 16 | be16(p + 2); }

/* ---- encendido, identificación y atributos ----------------------------------------------------------------- */

static void cerrar_entero(void);
static u8 pendiente_w;                  /* (28-oct) hay escrituras sin CERRAR: se perderían al quitarle la tensión (ver «el cierre», abajo) */
static void apagar(void)
{
    if (pendiente_w && encendida && !abierta) cerrar_entero();   /* (28-oct) antes de quitarle la tensión, lo escrito, consolidado */
    if (pendiente_w) { inf.cortes_sin_cierre++; pendiente_w = 0; }
    abierta = 0;
    anf->apagar();
    encendida = 0; hubo_apagado = 1; t_apagado = anf->ms();
}
/* Los atributos [4477.c:11595 y 11491]: una orden 0x24 de UN sector en la dirección 0 (lo único que hace el original
 * y lo único calibrado: la unidad de la dirección —bytes o sectores— es [sin saber]). De ahí, la información del
 * sistema (id 0x10, 0x60 bytes) y la capacidad. */
static int atributos(void)
{
    static u8 at[512];
    const u8 *e, *s; u32 i, dir, tam, a; int v, t;
    anf->olvida_int();
    if ((t = orden(0x24, 1, 0)) != 0) return t == MS_T_FUERA ? MS_E_FUERA : MS_E_ATRIBUTOS;
    if ((v = espera_tarjeta()) < 0) return v == MS_E_FUERA ? v : MS_E_ATRIBUTOS;
    if ((v & (MS_INT_ERR | MS_INT_CMDNK)) || !(v & MS_INT_BREQ)) { inf.e_estado++; return MS_E_ATRIBUTOS; }
    if ((t = tpc(MS_TPC_READ_LONG_DATA, at, 512, 0)) != 0) return t == MS_T_FUERA ? MS_E_FUERA : MS_E_ATRIBUTOS;   /* NUESTRO: sí miramos el CRC */
    if ((v = espera_tarjeta()) < 0) return v == MS_E_FUERA ? v : MS_E_ATRIBUTOS;
    if (!(v & MS_INT_CED)) { inf.e_estado++; return MS_E_ATRIBUTOS; }
    abierta = 0;
    if (at[0] != 0xA5 || at[1] != 0xC3) return MS_E_ATRIBUTOS;
    for (i = 0, e = at + 0x10; ; i++, e += 12) {            /* entradas de 12 bytes: dirección, tamaño, id */
        if (i >= at[4] || e + 12 > at + 512) return MS_E_SIN_INFO;       /* tope: las que caben en el sector */
        if (e[8] == 0x10) break;
    }
    dir = be32(e); tam = be32(e + 4);
    if (tam != 0x60 || dir < 0x1A0 || dir > 0x8000 - 0x60) return MS_E_SIN_INFO;
    if (dir + tam > 512) return MS_E_INFO_LEJOS;            /* el original leería otro sector de atributos [4477.c:11563]: sin calibrar */
    s = at + dir;
    if (s[0] != 2 || s[0x38] >= 4) return MS_E_SIN_INFO;
    if (s[0x38]) inf.protegida = 1;
    inf.bloque = (u16)be16(s + 2); inf.bloques = (u16)be16(s + 4); inf.usuario = (u16)be16(s + 6); inf.unidad = (u16)be16(s + 0x2C);
    a = ((u32)inf.unidad * inf.bloque) >> 9;                /* [4477.c:11461]: ((unidad x bloque) >> 9) x bloques de usuario */
    if (!a || !inf.usuario || a > 0xFFFFFFFFu / inf.usuario) return MS_E_CAPACIDAD;
    inf.sectores = a * inf.usuario;
    return 0;
}
/* Tras dar tensión: identificar, esperar CED, pasar a 4 bits y leer los atributos
 * [MemStickLD_BuildStickInfo 44A1.c:6904 + MemStickProLD_SetupStick 4477.c:10973].
 * (I1) PARTIDO EN TRES para que la espera del CED (hasta 101 x 104 ms = 10,5 s) pueda ir POR PASOS desde el bucle al meter
 * una tarjeta (ms_paso, estado MS_PREPARANDO): preparar_ini (identificar), ced_mira (UNA pregunta GET_INT) y preparar_fin (lo
 * de después del CED). levantar() los encadena igual que antes, con la espera dentro: es lo que usa la lectura al despertar
 * (una lectura no puede volver sin datos). */
static int preparar_ini(void)
{
    u8 r[6]; int t;
    anf->espera_ms(14);                                     /* [44A1.c:6503] */
    if ((t = rw_adrs(2, 6, 0x10, 1)) != 0) return t == MS_T_FUERA ? MS_E_FUERA : MS_E_MUDA;
    if ((t = tpc3(MS_TPC_READ_REG, r, 6, 0)) != 0) return t == MS_T_FUERA ? MS_E_FUERA : MS_E_MUDA;   /* registros 0x02..0x07 */
    memcpy(inf.reg, r, 6);
    if (r[2] != 1 || r[4] != 0 || r[5] >= 4) return MS_E_NO_PRO;         /* tipo 1, categoría 0, clase < 4 [4477.c:10995] */
    inf.protegida = (u8)(r[5] != 0 || (r[0] & 1));
    return 0;
}
static int ced_mira(void)                                   /* 1 = CED; 0 = todavía no; MS_E_FUERA */
{
    u8 g = 0; int t = tpc(MS_TPC_GET_INT, &g, 1, 0);       /* CED por GET_INT (en serie, un TPC): 101 veces cada 104 ms [4477.c:11000] */
    if (t == MS_T_FUERA) return MS_E_FUERA;
    return t == 0 && (g & MS_INT_CED) ? 1 : 0;
}
#ifndef MS_SOLO_SERIE
/* (M2) El paso a 4 bits falló: desde ahora, en serie. MS_E_PARALELO es reintentable: quien llama apaga (la tarjeta vuelve a serie al
 * quitarle la tensión, y el anfitrión, al encender) y el intento siguiente ya no pide 4 bits. */
static int a_serie(int fase, int t)
{
    if (!en_serie) { en_serie = 1; inf.serie = 1; }
    inf.paralelo_fallos++;
    inf.par_fase = (u8)fase; inf.par_t = (u8)(t < 0 ? -t : t);
    return MS_E_PARALELO;
}
/* (M2 ag, 2026-10-21) TRAS PASAR A PARALELO, LO QUE HACE EL ORIGINAL: esperar la INT hasta 1500 ms y, llegue o no, seguir
 * [MemStickProLD_SetupStick 0x44778C44: receive_w_tmo(0x5dc, 0x5278); si llega, GetInt y el valor se tira; luego los atributos].
 * NUNCA mira el CED en +8 sin una INT de por medio.
 * LA CAUSA [inferida] DE «la tarjeta no pasa a 4 bits» EN EL APARATO (v3): aquí se exigía ver CED en los bits 3..0 de +8 nada más pasar
 * a paralelo, sin ningún TPC ni INT. El modelo del emulador los enseña «vivos» (emu/maquina/w580-ms.inc: «en paralelo: la INT de la
 * tarjeta, viva»: una suposición del modelo, sin hipótesis numerada); en un anfitrión de esta familia (el MSHC de Sony; en Linux,
 * tifm_ms.c lee int_reg de STATUS & 0xFF al COMPLETAR un TPC que espera la INT) esos bits se cargan con la INT de la tarjeta: sin INT
 * nueva tras el cambio quedan a 0, el bucle agotaba sus 1500 ms y daba MS_E_PARALELO tres veces.
 * Aquí: se espera la INT (como el original) o, si el anfitrión sí los enseña vivos, el CED en +8 (lo que pase antes); si no hay
 * ninguno de los dos en el tope, se sigue (no es error). Se APRENDE (inf.int_paso) si esa INT llega: si no, las veces siguientes
 * (despertar del reposo) el tope es MS_PASO_CORTO_MS y no 1,5 s. Lo que de verdad decide si el bus de 4 bits anda es el READ_REG
 * de después (preparar_fin). */
static u8 atrib_par_fallos;                                 /* atributos fallados en 4 bits antes del primer paso entero */
static int espera_paso(void)
{
    u32 t0 = anf->ms(), tope = inf.int_paso == MS_INT_SERIE_NO ? MS_PASO_CORTO_MS : MS_PASO_INT_MS, d; int r;
    for (;;) {
        r = anf->espera_int(4);
        d = anf->ms() - t0;
        inf.par_ms = (u16)(d > 0xFFFFu ? 0xFFFFu : d);
        if (r == MS_T_FUERA) return MS_E_FUERA;
        if (r == MS_T_BIEN) { inf.par_fin = MS_PAR_FIN_INT; inf.int_paso = MS_INT_SERIE_SI; return 0; }
        if (anf->int_paralelo() & MS_INT_CED) { inf.par_fin = MS_PAR_FIN_CED; return 0; }
        if (d >= tope) { inf.par_fin = MS_PAR_FIN_TOPE; return 0; }
        if (!anf->hay()) return MS_E_FUERA;
    }
}
#endif
static int preparar_fin(void)
{
    u8 g = 0, z = 0, rr[0x20]; int t, r;
    if ((t = tpc(MS_TPC_GET_INT, &g, 1, 0)) != 0) return t == MS_T_FUERA ? MS_E_FUERA : MS_E_MUDA;
    if (g & MS_INT_ERR) { if (!(g & MS_INT_CMDNK)) return MS_E_ERR; inf.protegida = 1; }
    /* (ms810) El SetupStick PRO del W810 (0x449E8260) NO escribe el registro 0x10 ni pasa a paralelo: tras el CED va a los
     * atributos. Antes espera una INT hasta 1500 ms («que no llegue no es error», 0x449E82F4); en serie, sin paso a paralelo,
     * no hay nada que la provoque. NUESTRO: no se espera (1,5 s menos en cada montaje). Lo mismo el W580 caído a 1 bit. */
    if (en_serie) return atributos();
#ifdef MS_SOLO_SERIE
    (void)z; (void)t; (void)rr; (void)r;
    return MS_E_PARALELO;                                   /* (no se llega: en_serie es 1) */
#else
    if ((t = rw_adrs(0, 0x1F, 0x10, 1)) != 0) return t == MS_T_FUERA ? MS_E_FUERA : a_serie(MS_PAR_RW_ADRS, t);
    if ((t = tpc3(MS_TPC_WRITE_REG, &z, 1, 1)) != 0) return t == MS_T_FUERA ? MS_E_FUERA : a_serie(MS_PAR_WRITE_REG, t);   /* parámetro de sistema (reg. 0x10) <- 0: interfaz paralela */
    anf->paralelo();
    if ((r = espera_paso()) != 0) return r;                 /* (M2 ag) la INT como el original, sin exigir CED en +8 */
    /* (M2 ag) NUESTRO: ¿ANDA EL BUS DE 4 BITS? Un READ_REG de los registros 0x00..0x1E (el tamaño de lectura que dejó el SET_RW_REG_ADRS
     * de arriba: no hace falta otro TPC) ya en paralelo, como la prueba de Linux tras su cambio de interfaz (mspro_block: un GET_INT en
     * PAR4; aquí GET_INT por TPC en paralelo el original no lo manda nunca, READ_REG sí lo admite su ReadReg). Tipo, categoría y clase
     * tienen que ser los de la identificación en serie y el 0x10, el 0 recién escrito. Si no: datos rotos en 4 bits -> serie. */
    memset(rr, 0, sizeof rr);
    if ((t = tpc3(MS_TPC_READ_REG, rr, 0x1F, 0)) != 0) return t == MS_T_FUERA ? MS_E_FUERA : a_serie(MS_PAR_LEER_REG, t);
    memcpy(inf.par_reg, rr + 2, 6); inf.par_reg[6] = rr[0x10];
    if (rr[4] != inf.reg[2] || rr[6] != inf.reg[4] || rr[7] != inf.reg[5] || rr[0x10] != 0) return a_serie(MS_PAR_REG_DISTINTOS, 0);
    t = (int)inf.e_int;
    r = atributos();
    /* (M2 ag) SIN INT EN PARALELO: si los atributos han salido pero alguna respuesta llegó SIN su INT (por el tope de 1500 ms: e_int sube),
     * el anfitrión no da la INT de la tarjeta en 4 bits; leer así costaría 1,5 s por sector (y mirar +8 sin INT no es seguro: el estado
     * puede ser el de la respuesta anterior). La primera vez, a serie, donde la INT se aprende y se puede preguntar con GET_INT. */
    if (!r && !inf.paralelo_bien && inf.e_int != (u32)t) return a_serie(MS_PAR_SIN_INT, 0);
    /* (M2 ag) los atributos son la primera ORDEN en 4 bits (INT de la tarjeta en paralelo + 512 bytes): si fallan DOS veces seguidas sin
     * que el paso a 4 bits haya salido nunca entero en este arranque, también a serie (la primera vez es un tropiezo más: apagar y otra
     * vez, en 4 bits; levantar() tiene 3 intentos, así que el tercero ya va en serie). Si ya salió alguna vez, es un fallo normal. */
    if (r == MS_E_ATRIBUTOS && !inf.paralelo_bien && ++atrib_par_fallos >= 2) return a_serie(MS_PAR_ATRIBUTOS, r);
    if (!r) { inf.paralelo_bien++; atrib_par_fallos = 0; if (inf.par_fin == MS_PAR_FIN_TOPE && inf.int_paso == MS_INT_SERIE_SIN_SABER) inf.int_paso = MS_INT_SERIE_NO; }
    return r;
#endif
}
static int preparar(void)                                   /* las tres, con la espera del CED dentro (como antes de I1) */
{
    int i, c, r = preparar_ini();
    if (r) return r;
    for (i = 0; ; i++) {
        if ((c = ced_mira()) < 0) return c;
        if (c) break;
        if (i == 100) return MS_E_CED;                      /* tope: ~10,5 s */
        if (!anf->hay()) return MS_E_FUERA;
        anf->latido(); anf->espera_ms(104);
    }
    return preparar_fin();
}
static int reintentable(int r) { return r == MS_E_MUDA || r == MS_E_PARALELO || r == MS_E_ATRIBUTOS; }
#ifdef MS_EXP
static int encender_ya(void);
volatile u32 ms_exp_espera[3];                              /* la última: ms desde el apagado, lo que tardó encender_ya (con su espera), esperas cortas dentro */
/* (2026-10-26, EXPERIMENTO: ¿por qué falla la identificación con el USB en marcha?) UNA identificación entera, desde 4 bits, con UN solo
 * intento (sin los 3 de levantar), y apagar. Devuelve el motivo (0 = bien) y deja los ms en *ms. Solo desde plat, al arrancar. */
int ms_exp_identificar(u32 *ms)
{
    int r; u32 t0 = anf->ms();
    if (!anf->hay()) { *ms = 0; return MS_E_FUERA; }
    if (encendida) apagar();
#ifndef MS_SOLO_SERIE
    en_serie = 0; inf.serie = 0; atrib_par_fallos = 0; inf.paralelo_bien = 0; inf.par_fase = 0; inf.par_t = 0;
#endif
    { extern volatile u32 tics, espera_cortas; u32 k0 = tics, c0 = espera_cortas; ms_exp_espera[0] = hubo_apagado ? anf->ms() - t_apagado : 0xFFFFu;
      r = encender_ya(); ms_exp_espera[1] = (tics - k0) * 4u; ms_exp_espera[2] = espera_cortas - c0; }
    if (!r) r = preparar();
    apagar();
#ifndef MS_SOLO_SERIE
    en_serie = 0; inf.serie = 0; atrib_par_fallos = 0;
#endif
    *ms = anf->ms() - t0;
    return r;
}
#endif
static int encender_ya(void)                                /* 250 ms desde el último apagado, tensión; 0 = encendida */
{
    int r;
    if (hubo_apagado && (u32)(anf->ms() - t_apagado) < 250) anf->espera_ms(250 - (u32)(anf->ms() - t_apagado));   /* [44A1.c: temporizador de 250 ms antes de Init] */
    r = anf->encender();
    if (r) { apagar(); return r; }                          /* el anfitrión, el PMU o el reloj: repetir no lo arregla */
    encendida = 1; inf.encendidos++;
    return 0;
}
/* Encender y preparar. Una tarjeta que no contesta o tropieza: apagar, 250 ms, encender; 3 veces [44A1.c:6914]. */
static int levantar(void)
{
    int i, r = MS_E_FUERA; u32 t0 = anf->ms();
    for (i = 0; i < 3; i++) {
        if (!anf->hay()) { r = MS_E_FUERA; break; }
        if ((r = encender_ya()) != 0) break;
        r = preparar();
        if (!r) break;
        apagar();
        if (!reintentable(r)) break;
        inf.recuperaciones++;
        anf->latido();
    }
    inf.ms_identificar = anf->ms() - t0;
    return r;
}
/* (I1) LO MISMO, POR PASOS, para una tarjeta recién metida (ms_paso): cada vuelta del bucle hace como mucho UNA pregunta del CED
 * (cada 104 ms) o un intento entero hasta ella; la interfaz no se queda quieta los 10,5 s del peor caso (docs/tarjeta.md §2.8). */
static void fuera(void);
static struct { u8 intento; u16 i; u32 t, t0; } pr;
static void pr_acaba(int r)                                 /* lista, mala o fuera: como al final de levantar() en ms_paso */
{
    inf.ms_identificar = anf->ms() - pr.t0;
    if (r == MS_E_FUERA) { fuera(); return; }
    if (r) { inf.estado = MS_MALA; inf.motivo = (signed char)r; inf.sectores = 0; return; }
    inf.estado = MS_LISTA; inf.generacion++; t_uso = anf->ms();
}
static void pr_intento(void)                                /* encender, identificar y la PRIMERA pregunta del CED (lo normal: ya está) */
{
    int r, c;
    for (;;) {
        if (!anf->hay()) { pr_acaba(MS_E_FUERA); return; }
        if ((r = encender_ya()) != 0) { pr_acaba(r); return; }
        r = preparar_ini();
        if (!r) {
            if ((c = ced_mira()) == 0) { inf.estado = MS_PREPARANDO; pr.i = 0; pr.t = anf->ms(); return; }   /* a esperar, por pasos */
            r = c < 0 ? c : preparar_fin();
        }
        if (!r) { pr_acaba(0); return; }
        apagar();
        if (!reintentable(r) || ++pr.intento >= 3) { pr_acaba(r); return; }
        inf.recuperaciones++;
        anf->latido();
    }
}
static void pr_paso(void)                                   /* desde ms_paso, con la tarjeta en MS_PREPARANDO */
{
    int c, r;
    if ((u32)(anf->ms() - pr.t) < 104) return;
    pr.t = anf->ms();
    if ((c = ced_mira()) == 0) {
        if (++pr.i < 101) return;                           /* tope: 101 preguntas, ~10,5 s, como el original */
        r = MS_E_CED;
    } else r = c < 0 ? c : preparar_fin();
    if (!r) { pr_acaba(0); return; }
    apagar();
    if (r == MS_E_FUERA) { pr_acaba(r); return; }
    if (!reintentable(r) || ++pr.intento >= 3) { pr_acaba(r); return; }
    inf.recuperaciones++;
    pr_intento();
}
static void fuera(void)
{
    if (encendida) apagar();
    cierre_n = 0; cierre_i = 0; pendiente_w = 0;            /* (28-oct) la zona de cierre era de ESA tarjeta */
    if (inf.estado != MS_FUERA) inf.salidas++;
    inf.estado = MS_FUERA; inf.motivo = 0; inf.sectores = 0; dentro = 0;
}

/* ---- lectura ------------------------------------------------------------------------------------------------- */

/* Una orden 0x20 de k sectores [MemStickProLD_Read 4477.c:11182..11214]: por cada sector, INT -> estado -> con BREQ,
 * READ_LONG_DATA de 512; al final, INT hasta CED. NUESTRO: el original ignora el resultado de READ_LONG_DATA (CRC o
 * tiempo) y ante un sector malo «sigue con el siguiente»; nosotros lo miramos y cortamos al primer fallo. */
/* (28-oct) Tras un fallo a media lectura, CERRAR la orden (como sigue el original: lee lo que quede, aquí se tira) hasta su CED, para que la
 * tarjeta quede lista para otra orden (el cierre de lo escrito, antes de apagarla, la necesita). v: lo último que dijo, o SIN_ESTADO_L. */
#define SIN_ESTADO_L (-0x201)
static u8 tira[512];
static void cerrar_lectura(int v, u32 quedan)
{
    u32 n;
    for (n = 0; n < quedan + 4u; n++) {
        if (v == MS_E_FUERA) return;
        if (v >= 0) {
            if (v & MS_INT_CED) { abierta = 0; return; }
            if ((v & MS_INT_BREQ) && !(v & (MS_INT_CMDNK | MS_INT_ERR)) && tpc(MS_TPC_READ_LONG_DATA, tira, 512, 0) == MS_T_FUERA) return;
        } else if (v != SIN_ESTADO_L) return;               /* muda (1,5 s sin nada): en lectura no se insiste */
        v = espera_tarjeta();
    }
}
static int leer_orden(u32 lba, u32 k, u8 *buf)
{
    u32 i; int v = SIN_ESTADO_L, t;
    anf->olvida_int();
    if ((t = orden(0x20, k, lba)) != 0) { if (t == MS_T_FUERA) return MS_E_FUERA; i = 0; goto fallo; }
    inf.ordenes++;
    for (i = 0; i < k; i++, buf += 512) {
        if ((v = espera_tarjeta()) < 0) { if (v == MS_E_FUERA) return v; goto fallo; }
        if ((v & (MS_INT_CMDNK | MS_INT_ERR)) || !(v & MS_INT_BREQ)) { inf.e_estado++; goto fallo; }
        if ((t = tpc(MS_TPC_READ_LONG_DATA, buf, 512, 0)) != 0) { if (t == MS_T_FUERA) return MS_E_FUERA; v = SIN_ESTADO_L; i++; goto fallo; }
        inf.leidos++;
    }
    for (i = 0; i < 3; i++) {                               /* cierre: hasta CED (tope: 3 INT) */
        if ((v = espera_tarjeta()) < 0) { if (v == MS_E_FUERA) return v; i = k; goto fallo; }
        if (v & (MS_INT_CMDNK | MS_INT_ERR)) { inf.e_estado++; i = k; goto fallo; }
        if (v & MS_INT_CED) { abierta = 0; return 0; }
    }
    i = k;
fallo:
    cerrar_lectura(v, k > i ? k - i : 0);
    return MS_E_LECTURA;
}
/* Despertar una tarjeta apagada (reposo, o tras un fallo): encender e identificar. 0 = lista; si no, queda FUERA o MALA. */
static int despertar(void)
{
    u32 antes = inf.sectores;
    int r = levantar();
    if (!r && inf.sectores != antes) { r = MS_E_OTRA; apagar(); }   /* NUESTRO: el original no relee los atributos al despertar */
    if (r == MS_E_FUERA) { fuera(); return r; }
    if (r) { inf.estado = MS_MALA; inf.motivo = (signed char)r; inf.sectores = 0; return r; }
    inf.estado = MS_LISTA;
    return 0;
}
int ms_leer(u32 lba, u32 n, u8 *buf)
{
    int intento, r = MS_E_LECTURA;
    if (!anf || !dentro || inf.estado == MS_FUERA) return MS_E_FUERA;
    if (inf.estado == MS_MALA) return inf.motivo;
    if (inf.estado == MS_PREPARANDO) return MS_E_CED;       /* (I1) todavía esperando su CED: no se lee (api/tarjeta.c ni lo intenta) */
    if (lba >= inf.sectores || n > inf.sectores - lba) return MS_E_RANGO;
    if (!n) return 0;
    for (intento = 0; intento < 3; intento++) {             /* tope: 3 intentos, cada uno con su apagado y encendido */
        u32 l = lba, q = n; u8 *b = buf;
        if (!encendida && (r = despertar()) != 0) return r; /* dormía (reposo) o viene de un fallo */
        for (r = 0; q && !r; ) {
            u32 k = q < inf.por_orden ? q : inf.por_orden;
            r = leer_orden(l, k, b);
            l += k; q -= k; b += k * 512;
            anf->latido();
        }
        t_uso = anf->ms();
        if (!r) return 0;
        if (r == MS_E_FUERA || !anf->hay()) { fuera(); return MS_E_FUERA; }
        apagar(); inf.recuperaciones++;                     /* sin orden de parada: apagar, 250 ms (en levantar), encender */
        if (inf.por_orden > 4) inf.por_orden = 4;           /* y, desde ahora, como mucho 4 sectores por orden: lo que pide el original */
    }
    inf.estado = MS_DORMIDA; inf.motivo = MS_E_LECTURA;     /* apagada; otra lectura lo volverá a intentar desde cero */
    return MS_E_LECTURA;
}

/* ---- escritura (pista MSW, docs/tarjeta_escritura.md) ---------------------------------------------------------- */

/* Una orden 0x21 de k sectores, como el volcado del original [FUN_44779528, 4477.c:11651..11707; MemStickProLD_Write]:
 * EX_SET_CMD(0x21, k, lba); por cada sector: INT (<= 1500 ms) -> estado -> sin CMDNK ni ERR y con BREQ, WRITE_LONG_DATA
 * de 512; al final, INT hasta CED. Diferencias, NUESTRAS:
 *  - el original repite el TPC WRITE_LONG_DATA hasta 3 veces [FUN_4422c9d8] y ante un fallo SIGUE con el sector siguiente
 *    (los errores solo se imprimen: el FS no se entera). Aquí NO se repite nada y se corta al primer fallo: repetir el
 *    dato de un sector que la tarjeta quizá ya tomó lo escribiría en el SIGUIENTE (la tarjeta avanza sola). Repetir la
 *    escritura ENTERA sí es seguro, y eso lo decide quien llama.
 *  - un CED a media orden («Got CED in write», que el original solo imprime) es aquí un error. */
/* (28-oct) CERRAR LA ORDEN TRAS UN FALLO, ANTES DE PENSAR EN APAGAR. Lo que hace el original [FUN_44779528]: ante un fallo NO quita la
 * tensión; sigue con los sectores que quedan y espera el CED (la tensión solo se va a los 30 s sin uso, y antes espera al volcado en curso:
 * FUN_44a1564c antes de Shutdown, 44A1.c:7239). Antes, aquí se apagaba EN EL ACTO con la orden 0x21 abierta: la tarjeta tenía a medias un
 * bloque de 256 KiB (su unidad de borrado: 512 sectores, lo dice su información del sistema) y un corte así es lo que rompe la tabla interna
 * de una flash. [observado 28-oct en la M2 del W580: bloques enteros de 256 KiB a 0xFF o con contenido viejo, y dos sectores de la FAT2 con
 * el de otros, tras una escritura fallada a las 03:53 (ram_j, «motivo FLASH») cortada así.]
 * Cerrar: mientras la tarjeta pida sectores (BREQ) se le dan, SIEMPRE dentro del tramo de la orden (la propia tarjeta no toma más de los k
 * que se le pidieron): el que falló otra vez (con CRC o tiempo agotado no lo tomó, o no se sabe) y luego los siguientes; el último se repite
 * si hace falta. Lo escrito en el tramo queda INDETERMINADO y la escritura se da por FALLADA igual (quien llama la repite entera). 1 = la
 * tarjeta llegó a CED (orden cerrada: sigue encendida y lista); 0 = muda (no queda otra que apagar); MS_E_FUERA. */
static u8 w_cerrada;
#define SIN_ESTADO (-0x200)                                 /* (no es un MS_E_*: MS_E_FUERA vale -1) el fallo fue un TPC: aún no se sabe qué dice */
static int cerrar_escritura(int v, const u8 *dato, const u8 *ultimo)
{
    u32 n, mudas = 0; int t;
    inf.cierres_w++;
    for (n = 0; n < MS_ESCRIBE_POR_ORDEN + 4u; n++) {      /* tope: los sectores de una orden y unos pocos más */
        if (v == MS_E_FUERA) return MS_E_FUERA;
        if (v >= 0) {
            mudas = 0;
            if (v & MS_INT_CED) { inf.cerradas_w++; abierta = 0; return 1; }
            if ((v & MS_INT_BREQ) && !(v & (MS_INT_CMDNK | MS_INT_ERR))) {
                if ((t = tpc(MS_TPC_WRITE_LONG_DATA, (u8 *)(__UINTPTR_TYPE__)dato, 512, 1)) == MS_T_FUERA) return MS_E_FUERA;
                if (!t && dato < ultimo) dato += 512;     /* tomado: el siguiente. Si no, el mismo otra vez */
                inf.relleno_w++;
                anf->latido();
            }
        } else if (v != SIN_ESTADO && ++mudas >= 2) return 0;   /* dos esperas enteras (1,5 s cada una) sin INT ni estado */
        v = espera_tarjeta();
    }
    return 0;
}
static int escribir_orden(u32 lba, u32 k, const u8 *buf)
{
    u32 i; int v = SIN_ESTADO, t, r = MS_E_ESCRITURA;
    const u8 *ultimo = buf + (k - 1) * 512;
    w_cerrada = 0;
    anf->olvida_int();
    puerta_w = 1; t = orden(0x21, k, lba); puerta_w = 0;
    if (t == MS_T_FUERA) return MS_E_FUERA;
    if (t) goto fallo;                                      /* ¿tomó la orden? Si la tomó, pedirá el primer sector: cerrar también */
    inf.ordenes_w++;
    for (i = 0; i < k; i++, buf += 512) {
        if ((v = espera_tarjeta()) < 0) { if (v == MS_E_FUERA) return v; goto fallo; }
        if ((v & (MS_INT_CMDNK | MS_INT_ERR)) || !(v & MS_INT_BREQ)) { inf.e_estado++; goto fallo; }
        if ((t = tpc(MS_TPC_WRITE_LONG_DATA, (u8 *)(__UINTPTR_TYPE__)buf, 512, 1)) != 0) { if (t == MS_T_FUERA) return MS_E_FUERA; v = SIN_ESTADO; goto fallo; }
        inf.escritos++;
        anf->latido();                                      /* por SECTOR: una tarjeta que tarde ~1,5 s en cada uno no deja 48 s sin latido */
    }
    for (i = 0; i < 3; i++) {                               /* cierre: hasta CED (tope: 3 INT de 1500 ms; el original, hasta CED o una INT que no llega) */
        if ((v = espera_tarjeta()) < 0) { if (v == MS_E_FUERA) return v; goto fallo; }
        if (v & (MS_INT_CMDNK | MS_INT_ERR)) { inf.e_estado++; goto fallo; }
        if (v & MS_INT_CED) { abierta = 0; return 0; }
    }
fallo:
    if (buf > ultimo) buf = ultimo;
    if ((t = cerrar_escritura(v, buf, ultimo)) == MS_E_FUERA) return MS_E_FUERA;
    w_cerrada = (u8)t;
    return r;
}
static u32 t_ult_w;
static int escribir_tramo(u32 lba, u32 n, const u8 *buf);
int ms_escribir(u32 lba, u32 n, const u8 *buf)
{
    u32 antes = inf.ordenes_w + inf.cierres_w;
    int r = escribir_tramo(lba, n, buf);
    if (inf.ordenes_w + inf.cierres_w != antes) {           /* (28-oct) salió alguna orden 0x21: a cerrar */
        if (encendida) { pendiente_w = 1; t_ult_w = anf->ms(); }
        else inf.cortes_sin_cierre++;                       /* un fallo ya la apagó: lo de esta llamada no se pudo cerrar */
    }
    return r;
}
static int escribir_tramo(u32 lba, u32 n, const u8 *buf)
{
    int r = 0; u32 l = lba, q = n;
    if (!anf || !dentro || inf.estado == MS_FUERA) return MS_E_FUERA;
    if (inf.estado == MS_MALA) return inf.motivo;
    if (inf.estado == MS_PREPARANDO) return MS_E_CED;
    if (!n) return 0;
    if (lba >= inf.sectores || n > inf.sectores - lba) return MS_E_RANGO;
    if (inf.protegida) return MS_E_PROTEGIDA;               /* ni una orden: el original tampoco manda 0x21 a una protegida */
    if (!encendida && (r = despertar()) != 0) return r;
    if (inf.protegida) return MS_E_PROTEGIDA;               /* (al despertar puede declararse protegida: se relee el registro) */
    while (q && !r) {
        u32 k = q < MS_ESCRIBE_POR_ORDEN ? q : MS_ESCRIBE_POR_ORDEN;
        if (!anf->hay()) { r = MS_E_FUERA; break; }
        r = escribir_orden(l, k, buf);
        l += k; q -= k; buf += k * 512;
        anf->latido();
    }
    t_uso = anf->ms();
    if (!r) return 0;
    inf.e_escritura++;
    if (r == MS_E_FUERA || !anf->hay()) { fuera(); return MS_E_FUERA; }
    if (w_cerrada) { inf.motivo = (signed char)r; return r; }   /* (28-oct) la orden se cerró (CED): sigue encendida y LISTA, como el original */
    inf.cortes_w++;                                         /* (28-oct) muda con la orden abierta: el único corte que queda */
    apagar(); inf.recuperaciones++;                         /* SIN orden de parada (el original no tiene): apagada; la siguiente operación la levanta */
    inf.estado = MS_DORMIDA; inf.motivo = (signed char)r;
    return r;
}
/* La escritura es síncrona (vuelve tras el CED de su última orden): no queda nada en vuelo. Aun así se comprueba con el
 * estado que enseña el anfitrión en paralelo: si la tarjeta no está en CED, se espera hasta 1500 ms. */
int ms_vaciar(void)
{
    u32 i;
    if (!anf || !encendida || inf.estado != MS_LISTA) return 0;
    if (en_serie) for (i = 0; ; i++) {                         /* (ms810) en serie, el CED se pregunta con GET_INT; tope ~1,5 s */
        int v = int_serie(1);
        if (v == MS_E_FUERA || !anf->hay()) { fuera(); return MS_E_FUERA; }
        if (v >= 0 && (v & MS_INT_CED)) return 0;
        if (i >= 375) return MS_E_ESCRITURA;
        if (!(i & 15)) anf->latido();
        anf->espera_ms(4);
    }
    for (i = 0; !(anf->int_paralelo() & MS_INT_CED); i++) {
        if (!anf->hay()) { fuera(); return MS_E_FUERA; }
        if (i >= 1500) return MS_E_ESCRITURA;               /* tope: 1500 ms, lo que el original espera una INT */
        if (!(i & 63)) anf->latido();
        anf->espera_ms(1);
    }
    return 0;
}

/* ---- (28-oct) EL CIERRE: que lo escrito SOBREVIVA a quitarle la tensión --------------------------------------------
 * [observado 28-oct en la M2 de 2 GB del W580 bueno, herr/puente/tj_lote.py; docs/tarjeta_escritura.md §10]: la tarjeta guarda los cambios de
 * los 4 ÚLTIMOS bloques de borrado distintos que se tocaron (256 KiB, 512 sectores) SOLO en su memoria: al quitarle la tensión —aunque pasen
 * 30 s, aunque se le mande SLEEP (0x11), aunque se relea— esos bloques vuelven a su versión anterior. Cada bloque NUEVO que se toca consolida el
 * más antiguo: tras reescribir 4 bloques y escribir 1 sector en K bloques de descarte distintos se perdieron 2-4 (K=0), 1-2 (K=2), 1 (K=3) y
 * NINGUNO (K=4, 7 de 7). El original no hace nada de esto (solo 0x20/0x21/0x24; corta a los 30 s).
 * CERRAR = 1 sector en cada bloque de la ZONA DE CIERRE (la registra quien sabe dónde hay sitio: api/tarjeta.c, con fat_zona_cierre: clústeres
 * marcados malos). Cada sector cuesta ~140 ms (lo que tarda la tarjeta en consolidar un bloque): POR PASOS, uno por vuelta del bucle, a partir
 * de MS_CIERRE_MS sin escribir; y ENTERO antes de quitarle la tensión (apagar(): reposo, fallos, ms_apagar_ya). Lo escrito entre una
 * escritura y su cierre sigue expuesto a la pila o a un cuelgue. Sin zona no hay cierre (inf.cortes_sin_cierre). */
void ms_cierre_zona(const u32 *lba, u32 n, const u8 *sector)
{
    u32 i;
    if (n > MS_CIERRE_MAX) n = MS_CIERRE_MAX;
    for (i = 0; i < n; i++) cierre_lba[i] = lba[i];
    if (sector) memcpy(cierre_sector, sector, 512); else memset(cierre_sector, 0, 512);
    cierre_n = (u8)n; cierre_i = 0;
}
u32 ms_cierre_pendiente(void) { return pendiente_w; }
/* Un sector más del cierre. 0 = escrito (o nada que hacer); < 0 = falló (la orden se cerró o la tarjeta quedó muda: quien llama decide). */
static int cierre_paso(void)
{
    int r; u32 k;
    if (!pendiente_w || !cierre_n || !encendida || inf.estado != MS_LISTA || !dentro || abierta) return 0;
    if (inf.protegida) { pendiente_w = 0; return 0; }
    k = ++inf.cierre_sectores;                              /* el sector lleva un contador (offset 508): cada cierre, datos distintos */
    cierre_sector[508] = (u8)k; cierre_sector[509] = (u8)(k >> 8); cierre_sector[510] = (u8)(k >> 16); cierre_sector[511] = (u8)(k >> 24);
    r = escribir_orden(cierre_lba[cierre_i], 1, cierre_sector);
    t_uso = anf->ms();
    if (r) {
        inf.cierre_fallos++;
        if (++cierre_seguidos >= 3) { cierre_seguidos = 0; cierre_i = 0; pendiente_w = 0; inf.cortes_sin_cierre++; }   /* tope: no insistir más */
        return r;
    }
    cierre_seguidos = 0;
    if (++cierre_i >= cierre_n) { cierre_i = 0; pendiente_w = 0; inf.cierres++; }
    return 0;
}
static void cerrar_entero(void)                             /* desde apagar(): sin recuperar nada (ya se está apagando) */
{
    u32 tope = MS_CIERRE_MAX + 1;
    if (cerrando) return;
    cerrando = 1;
    while (pendiente_w && cierre_n && tope--) if (cierre_paso() < 0) break;
    cerrando = 0;
}

/* ---- el paso del bucle: detección, tarjeta nueva, reposo ----------------------------------------------------- */

void ms_init(const MsAnf *a)
{
    memset(&inf, 0, sizeof inf);
    anf = a; dentro = 0; encendida = 0; hubo_apagado = 0;
#ifndef MS_SOLO_SERIE
    en_serie = 0;
#endif
    int_fallos = 0;
#ifndef MS_SOLO_SERIE
    atrib_par_fallos = 0;
#endif
    inf.serie = en_serie;
    inf.por_orden = MS_POR_ORDEN > 255u ? 255 : (u8)MS_POR_ORDEN;
}
void ms_paso(void)
{
    int hay;
    if (!anf) return;
    hay = anf->hay() != 0;
    if (hay != dentro) {
        anf->espera_ms(20);                                 /* antirrebote: releer a los 20 ms [44A1.c:5381] */
        if ((anf->hay() != 0) != hay) return;               /* rebote: se verá en la vuelta siguiente */
        if (!hay) { fuera(); return; }
        dentro = 1; inf.entradas++; inf.motivo = 0;
        cierre_n = 0; cierre_i = 0; pendiente_w = 0;        /* (28-oct) tarjeta nueva: sin zona de cierre hasta que la registre quien la monta */
        inf.por_orden = MS_POR_ORDEN > 255u ? 255 : (u8)MS_POR_ORDEN;
        pr.intento = 0; pr.t0 = anf->ms();
        pr_intento();                                       /* (I1) por pasos: si el CED tarda, sigue en las vueltas siguientes */
        return;
    }
    if (inf.estado == MS_PREPARANDO) { pr_paso(); return; }
    if (pendiente_w && cierre_n && inf.estado == MS_LISTA && (u32)(anf->ms() - t_ult_w) >= MS_CIERRE_MS) {   /* (28-oct) el cierre, por vueltas */
        int r = cierre_paso();
        if (r == MS_E_FUERA || !anf->hay()) { fuera(); return; }
        if (r && !w_cerrada) { apagar(); inf.estado = MS_DORMIDA; inf.recuperaciones++; }   /* muda: como un fallo de escritura */
        if (r) t_ult_w = anf->ms();                         /* si la orden se cerró, otro intento dentro de MS_CIERRE_MS */
        return;
    }
    if (inf.estado == MS_LISTA && (u32)(anf->ms() - t_uso) >= MS_REPOSO_MS) {   /* 30 s sin uso: apagada [44A1.c:7239] */
        apagar(); inf.estado = MS_DORMIDA; inf.reposos++;
    }
}
void ms_apagar_ya(void)
{
    if (!anf) return;
    if (encendida) apagar();
    if (inf.estado == MS_LISTA) inf.estado = MS_DORMIDA;
    if (inf.estado == MS_PREPARANDO) { inf.estado = MS_FUERA; dentro = 0; }   /* (I1) a medio preparar: que se vuelva a ver entrar */
}
