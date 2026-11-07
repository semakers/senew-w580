/* LA CARGA DE LA BATERÍA POR USB, en NÚMEROS y DECISIONES (pista CARGA, 25-oct-2026). Funciones PURAS, sin hardware: la
 * plataforma (plat/hw_w580.c) lee el PMU, llama a carga_paso() una vez por segundo y escribe lo que diga; el PC la prueba entera
 * (herr/pruebas/prueba_carga.c, `make probar-carga`). Docs: docs/bateria.md §12, work/bitacora_CARGA.md, work/b/usb_carga_RE.md.
 *
 * LO QUE HACE EL ORIGINAL (R8BE001, BATTMGR con ChargerType = 2) [observado + ejecutado en el emulador, c1/c2]:
 *  - carga SOLO cuando el PC ha CONFIGURADO el teléfono (EVENT_USB_CONFIGURED con 500 mA, los del GDFS 0x05E6) y no está suspendido;
 *  - escribe en el PMU únicamente 0x63: tensión (bits 0..1) = 3 y corriente (bits 4..5) = 3, LEYENDO EL CHIP antes de cada campo;
 *    lo reescribe cada 10 s; 0x60/0x61 no se tocan para el USB;
 *  - cuando el CHIP llega al techo levanta el suceso 0x21 bit 7 («USB voltage reached») y, en el aparato, PONE 0x63 A 00 ÉL SOLO
 *    [medido 25-oct, E4: con los bornes en 4,09-4,10 V]; el original baja entonces la corriente pedida un escalón
 *    (5000 -> 4000 -> 3000 -> 2000 -> 1500 -> 1000, en 0,1 mA: códigos 3, 3, 3, 2, 1, 1) y vuelve a pedir; con 1000 o menos y
 *    otro suceso, CARGA COMPLETA (0x63 bits 4..5 = 0); vuelve a cargar si la batería baja de 4020 mV;
 *  - para la carga USB no mira el temporizador de 4 h ni la corriente de la batería; sigue cargando hasta 60 °C.
 * LO QUE AÑADE ESTE SISTEMA (más prudente que el original; nada de esto quita lo suyo):
 *  - solo con la batería IDENTIFICADA (BDATA 8,2..180 kΩ: TYPE 1 o 2) y entre 5 y 45 °C medidos (el original, hasta 60);
 *  - temporizador de seguridad de 6 h por enchufada (el original: ninguno con USB; 4 h con el de pared) y corte con > 4200 mV en bornes;
 *  - el corte del chip se detecta por el SUCESO y, además, por 0x63 releído sin corriente (la IRQ del suceso puede no llegar:
 *    0x26 se relee E8 en el aparato); en el aparato es el mismo corte.
 * LA PROTECCIÓN QUE NO DEPENDE DE ESTE CÓDIGO: el chip corta solo en el techo del código 3 (4,1 V en bornes, por debajo de los 4,2
 * de la celda) [medido]. NO hay perro: el bit 7 de 0x60 no para la carga USB [medido, E3]. Si el sistema se cuelga, además, el perro
 * de software y la pantalla de fallo ponen 0x63 a 0 (tel_sin_bucle, tel_fallo). */
#ifndef CARGA_H
#define CARGA_H

/* Códigos de 0x63 [medido 25-oct]: tensión 1 = 3,9 V, 2 = 4,0 V, 3 = 4,1 V (en bornes, cargando); corriente 1 ≈ 75 mA, 3 ≈ 275 mA
 * (el firmware los cree 75/175/275; en el aparato, el código 1 sube los bornes ~22 mV y el 3, ~80 mV: r ≈ 0,3 Ω). */
#define CARGA_VCOD          3          /* el del original */
#define CARGA_MV_RECARGA    4020       /* COMPLETA -> CARGANDO por debajo (0xFB4 del original) */
#define CARGA_MV_TOPE       4200       /* por encima (bornes, cargando): DETENIDA hasta desenchufar [nuestro] */
#define CARGA_MS_TOPE       (6u * 3600u * 1000u)   /* 6 h cargando por enchufada: DETENIDA [nuestro; el original no tiene tope con USB y el de su
                                                    * cargador de pared es de 4 h, pero por USB el final va a ~75 mA y dura horas: con 4 h una
                                                    * carga desde abajo acabaría detenida antes de completar. Es la TERCERA red: antes, el corte
                                                    * del chip a 4,1 V y el nuestro a 4200 mV] */
#define CARGA_DC_MIN        50         /* 5,0 °C */
#define CARGA_DC_MAX        450        /* 45,0 °C */
#define CARGA_DC_HIST       10         /* 1 °C para volver de la pausa por temperatura */
#define CARGA_MS_REESCRIBIR 10000u     /* reescribir 0x63 aunque no cambie nada (el original: cada 10 llamadas de 1 s) */
#define CARGA_SIN_TEMP      (-32768)
#define CARGA_SIN_MV        (-1)

enum { CARGA_SIN_FUENTE = 0,   /* ni USB ni cargador */
       CARGA_USB_SIN_PERMISO,  /* USB sin configurar o suspendido: el PC no ha concedido corriente */
       CARGA_MIDIENDO,         /* esperando la primera medida de la batería (tipo y temperatura) */
       CARGA_CARGANDO,
       CARGA_COMPLETA,         /* el chip llegó al techo con la corriente mínima; recarga bajo CARGA_MV_RECARGA */
       CARGA_PAUSA_TEMP,       /* fuera de 5..45 °C */
       CARGA_DETENIDA,         /* un tope de seguridad: no vuelve a cargar hasta quitar y poner el USB (motivo) */
       CARGA_CARGADOR,         /* el cargador de pared (0x23 bit 3): este sistema NO lo usa (no medido en el aparato) */
       CARGA_PROHIBIDA,        /* la placa no carga, modo seguro, o el PMU no contesta */
       CARGA_N_ESTADOS };
enum { CARGA_M_NADA = 0, CARGA_M_TIEMPO, CARGA_M_TENSION, CARGA_M_QUITADA, CARGA_M_DESCONOCIDA, CARGA_M_PMU, CARGA_M_MEDIDA };

typedef struct {
    unsigned ms;                       /* hw_ms() */
    unsigned char pmu_ok;              /* se pudo leer 0x23 */
    unsigned char usb, cargador, quitada;   /* 0x23 bits 7, 3 y 6 */
    unsigned short usb_ma;             /* lo concedido por el PC: 500 configurado y despierto; 0 sin configurar o suspendido */
    short mv;                          /* bornes (mV, recta de la placa) o CARGA_SIN_MV */
    short temp_dc;                     /* temperatura de la batería (décimas de °C) o CARGA_SIN_TEMP */
    unsigned char tipo;                /* 1 o 2; 0 = desconocida (R de BDATA fuera de rango); 0xFF = aún sin medir */
    short r63;                         /* 0x63 releído del chip (antes de escribir nada en este paso), o -1 */
    unsigned char suceso_techo;        /* llegó 0x21 bit 7 desde el paso anterior */
    unsigned char prohibida;           /* la placa no carga / modo seguro */
    unsigned char fallo_escritura;     /* la escritura de 0x63 del paso anterior NO salió (el 00 releído no es un corte del chip) */
} CargaEntrada;

typedef struct {
    unsigned char estado, motivo;
    unsigned short pedida;             /* 0,1 mA: la escalera del original */
    unsigned char icod;                /* el código de corriente que se escribió por última vez (0 = parada) */
    unsigned char sin_mv;              /* pasos seguidos cargando sin lectura de bornes */
    unsigned char sesion;              /* 1 = hay USB desde el último «sin fuente» (la sesión de los 4 h) */
    unsigned ms_cargando;              /* acumulado en esta sesión */
    unsigned ms_ult, ms_escrito;       /* el paso anterior; la última escritura de 0x63 */
    unsigned short techos, escalones;  /* cortes del chip y escalones bajados en esta sesión (para Sistema) */
} Carga;

typedef struct {
    unsigned char escribir;            /* 0 = nada; 1 = programar (vcod, icod); 2 = parar (icod = 0, vcod se deja) */
    unsigned char vcod, icod;
} CargaSalida;

static inline unsigned char carga_icod(unsigned pedida)   /* op 8 del original (tabla 0x4427FB58): el primero con pedida >= valor */
{
    return (unsigned char)(pedida >= 2750 ? 3 : pedida >= 1750 ? 2 : pedida >= 750 ? 1 : 0);
}
static inline unsigned short carga_escalon(unsigned short i)   /* FUN_449c8d70 del original; 0 = se acabó */
{
    if (i < 1001) return 0;
    i -= 500;
    if (i >= 2001) i -= 500;
    return i;
}
static inline void carga_iniciar(Carga *c) { *c = (Carga){ 0 }; c->estado = CARGA_SIN_FUENTE; c->pedida = 5000; }

/* Un paso. Devuelve lo que hay que escribir en 0x63. */
static inline CargaSalida carga_paso(Carga *c, const CargaEntrada *e)
{
    CargaSalida s = { 0, CARGA_VCOD, 0 };
    unsigned dt = c->ms_ult ? e->ms - c->ms_ult : 0;
    int cargaba = c->estado == CARGA_CARGANDO && c->icod;
    c->ms_ult = e->ms ? e->ms : 1;
    if (cargaba) c->ms_cargando += dt;
    if (e->fallo_escritura && c->estado == CARGA_CARGANDO) { c->estado = CARGA_MIDIENDO; c->motivo = CARGA_M_PMU; c->icod = 0; cargaba = 0; }
#define CARGA_PARAR(est, mot) do { c->estado = (est); c->motivo = (mot); if (c->icod || (e->r63 >= 0 && (e->r63 & 0x30))) { s.escribir = 2; c->icod = 0; c->ms_escrito = e->ms; } return s; } while (0)
    if (e->prohibida) CARGA_PARAR(CARGA_PROHIBIDA, CARGA_M_NADA);
    if (!e->pmu_ok) CARGA_PARAR(CARGA_PROHIBIDA, CARGA_M_PMU);
    if (!e->usb && !e->cargador) { c->sesion = 0; c->ms_cargando = 0; c->techos = c->escalones = 0; c->pedida = 5000; c->motivo = 0; CARGA_PARAR(CARGA_SIN_FUENTE, CARGA_M_NADA); }
    if (e->cargador) CARGA_PARAR(CARGA_CARGADOR, CARGA_M_NADA);
    c->sesion = 1;
    if (c->estado == CARGA_DETENIDA) CARGA_PARAR(CARGA_DETENIDA, c->motivo);        /* hasta desenchufar */
    if (e->quitada) CARGA_PARAR(CARGA_DETENIDA, CARGA_M_QUITADA);
    if (!e->usb_ma) CARGA_PARAR(CARGA_USB_SIN_PERMISO, CARGA_M_NADA);
    if (e->tipo == 0xFF || e->temp_dc == CARGA_SIN_TEMP) CARGA_PARAR(CARGA_MIDIENDO, CARGA_M_NADA);
    if (e->tipo == 0) CARGA_PARAR(CARGA_DETENIDA, CARGA_M_DESCONOCIDA);
    if (cargaba && e->mv != CARGA_SIN_MV && e->mv > CARGA_MV_TOPE) CARGA_PARAR(CARGA_DETENIDA, CARGA_M_TENSION);
    if (c->ms_cargando >= CARGA_MS_TOPE) CARGA_PARAR(CARGA_DETENIDA, CARGA_M_TIEMPO);
    if (c->estado == CARGA_PAUSA_TEMP ? (e->temp_dc < CARGA_DC_MIN + CARGA_DC_HIST || e->temp_dc > CARGA_DC_MAX - CARGA_DC_HIST)
                                      : (e->temp_dc < CARGA_DC_MIN || e->temp_dc > CARGA_DC_MAX)) CARGA_PARAR(CARGA_PAUSA_TEMP, CARGA_M_NADA);
    if (e->mv == CARGA_SIN_MV) {                                  /* sin lectura de bornes: no se empieza; cargando, se aguanta 5 pasos */
        if (!cargaba) CARGA_PARAR(c->estado == CARGA_COMPLETA ? CARGA_COMPLETA : CARGA_MIDIENDO, CARGA_M_MEDIDA);
        if (++c->sin_mv > 5) CARGA_PARAR(CARGA_MIDIENDO, CARGA_M_MEDIDA);
    } else c->sin_mv = 0;
    if (c->estado == CARGA_COMPLETA) {
        if (e->mv == CARGA_SIN_MV || e->mv >= CARGA_MV_RECARGA) { if (e->r63 >= 0 && (e->r63 & 0x30)) { s.escribir = 2; c->icod = 0; } return s; }
        c->pedida = 5000;                                         /* COMPL -> CHARGE: la corriente máxima otra vez (como el original) */
    }
    if (c->estado != CARGA_CARGANDO) {                            /* (re)empezar */
        if (c->estado != CARGA_COMPLETA && e->mv > 4100) { c->estado = CARGA_COMPLETA; c->motivo = CARGA_M_NADA; return s; }   /* START del original: Vbat > 4100 -> COMPL */
        c->estado = CARGA_CARGANDO; c->motivo = CARGA_M_NADA;
        if (!c->pedida) c->pedida = 5000;
        c->icod = carga_icod(c->pedida);
        s.escribir = 1; s.icod = c->icod; c->ms_escrito = e->ms;
        return s;
    }
    /* CARGANDO: ¿ha cortado el chip? (el suceso, o 0x63 releído sin corriente cuando se escribió con ella) */
    if (c->icod && (e->suceso_techo || (e->r63 >= 0 && !(e->r63 & 0x30)))) {
        c->techos++;
        c->pedida = carga_escalon(c->pedida);
        if (!c->pedida) { c->icod = 0; c->estado = CARGA_COMPLETA; s.escribir = 2; c->ms_escrito = e->ms; return s; }
        c->escalones++;
        c->icod = carga_icod(c->pedida);
        s.escribir = 1; s.icod = c->icod; c->ms_escrito = e->ms;
        return s;
    }
    if (e->ms - c->ms_escrito >= CARGA_MS_REESCRIBIR || (e->r63 >= 0 && ((e->r63 >> 4) & 3) != c->icod)) {
        s.escribir = 1; s.icod = c->icod; c->ms_escrito = e->ms;
    }
    return s;
#undef CARGA_PARAR
}

/* La temperatura de la batería por BDATA (unidad 0x057B del GDFS del W580 bueno, TYPE 1; la del TYPE 2 es otra y no se ha visto
 * en este aparato: fisico/respaldo_bueno_20261019) y su identificación (FUN_44a140e0: R = mV x 1000 / 15). La recta del ADC 11 es
 * la de la unidad 0x000C: mV = 10,827 x cuenta - 11. */
static inline int carga_bdata_mv(int cuenta) { return (cuenta * 10827 + 500) / 1000 - 11; }
static inline unsigned char carga_tipo(int mv)                 /* 1, 2 o 0 = desconocida */
{
    long r = (long)mv * 1000 / 15;
    return (unsigned char)(r >= 8200 && r <= 84000 ? 1 : r > 84000 && r <= 180000 ? 2 : 0);
}
static inline int carga_temp_dc(int mv)                        /* décimas de °C, o CARGA_SIN_TEMP fuera de la tabla */
{
    static const short t[][2] = { {135,100},{173,70},{185,65},{198,60},{214,55},{234,50},{257,45},{285,40},{319,35},{358,30},{404,25},
                                  {456,20},{515,15},{580,10},{651,5},{723,0},{992,-20} };
    int i;
    if (mv < t[0][0] || mv > t[16][0]) return CARGA_SIN_TEMP;
    for (i = 1; i < 17; i++)
        if (mv <= t[i][0]) return t[i - 1][1] * 10 + (mv - t[i - 1][0]) * (t[i][1] - t[i - 1][1]) * 10 / (t[i][0] - t[i - 1][0]);
    return CARGA_SIN_TEMP;
}
#endif
